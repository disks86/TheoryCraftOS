/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "Process/WindowsStartup.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Utf16.hpp"

namespace process {

namespace {

constexpr uint64_t kPage = AddressSpace::kPageSize;
constexpr uint64_t kPebSize = kPage, kTebSize = 2 * kPage, kParametersHeader = 0x440;
constexpr uint64_t kMaxStartupBytes = 1ull << 20;
constexpr uint64_t kPlacementHint = 0x00007FF000000000ull;
constexpr uint32_t kTlsDirectory = 9;      // IMAGE_DIRECTORY_ENTRY_TLS

// Writes numbers into the new region through the privileged path (little endian).
class Writer {
 public:
  explicit Writer(AddressSpace& space) : space_(space) {}
  bool ok() const { return ok_; }
  void u8(uint64_t at, uint8_t v) { put(at, v, 1); }
  void u16(uint64_t at, uint16_t v) { put(at, v, 2); }
  void u32(uint64_t at, uint32_t v) { put(at, v, 4); }
  void u64(uint64_t at, uint64_t v) { put(at, v, 8); }
  void bytes(uint64_t at, ConstByteView data) { ok_ = ok_ && space_.write_privileged(at, data); }
  // UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } at `at`.
  void unicode_string(uint64_t at, uint64_t buffer, uint64_t length_bytes, uint64_t capacity_bytes) {
    u16(at, uint16_t(length_bytes));
    u16(at + 2, uint16_t(capacity_bytes));
    u64(at + 8, buffer);
  }

 private:
  void put(uint64_t at, uint64_t v, uint32_t n) {
    uint8_t raw[8];
    for (uint32_t i = 0; i < n; ++i) raw[i] = uint8_t(v >> (8 * i));
    ok_ = ok_ && space_.write_privileged(at, ConstByteView(raw, n));
  }
  AddressSpace& space_;
  bool ok_ = true;
};

bool utf16(StringView text, ByteBuffer* out) { return util::append_utf16le(out, text); }

}  // namespace

LoadStatus build_windows_startup(AddressSpace& space, const exe::PeFile& pe, const LoadedImage& image, const WindowsStartupInput& input, WindowsStartup* out) {
  if (!input.allocator || input.stack_top < input.stack_size) return LoadStatus::Malformed;
  allocators::IAllocator& allocator = *input.allocator;

  // --- the text, as UTF-16, and its sizes
  ByteBuffer image_path(allocator), command_line(allocator), environment(allocator), directory(allocator);
  if (!utf16(input.image_path, &image_path) || !utf16(input.command_line, &command_line) || !utf16("C:\\", &directory)) return LoadStatus::Malformed;
  for (const StringView& entry : input.environment) {
    if (!utf16(entry, &environment) || !environment.put16(0)) return LoadStatus::Malformed;
  }
  if (!environment.put16(0) || (input.environment.empty() && !environment.put16(0))) return LoadStatus::OutOfMemory;      // ends with an empty string (two NULs if empty)
  const uint64_t path_bytes = image_path.size(), command_bytes = command_line.size(), directory_bytes = directory.size(), environment_bytes = environment.size();

  // --- the image's static TLS
  uint64_t tls_start = 0, tls_end = 0, tls_index_at = 0, tls_zero = 0, tls_size = 0;
  uint32_t tls_rva = 0, tls_dir_size = 0;
  if (pe.directory(kTlsDirectory, &tls_rva, &tls_dir_size) && tls_rva != 0 && tls_dir_size >= 0x28) {
    uint8_t raw[0x28];
    if (!space.read_privileged(image.base + tls_rva, ByteView(raw, sizeof(raw)))) return LoadStatus::Malformed;
    auto word = [&](uint32_t at) { uint64_t v = 0; for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{raw[at + i]} << (8 * i); return v; };
    tls_start = word(0);
    tls_end = word(8);
    tls_index_at = word(0x10);
    tls_zero = raw[0x20] | (uint32_t{raw[0x21]} << 8) | (uint32_t{raw[0x22]} << 16) | (uint32_t{raw[0x23]} << 24);
    if (tls_end < tls_start || tls_end - tls_start > kMaxStartupBytes || tls_zero > kMaxStartupBytes) return LoadStatus::Malformed;
    tls_size = tls_end - tls_start + tls_zero;
  }

  // --- one region: PEB | TEB | parameters and strings | TLS array and block
  uint64_t strings_at = kParametersHeader;
  const uint64_t strings_bytes = path_bytes + command_bytes + directory_bytes + environment_bytes + 16;
  uint64_t parameters_bytes = 0, tls_bytes = 0, total = 0;
  if (strings_bytes > kMaxStartupBytes || !AddressSpace::round_up(strings_at + strings_bytes, &parameters_bytes)) return LoadStatus::Malformed;
  if (!AddressSpace::round_up(tls_size ? 16 + tls_size : 0, &tls_bytes)) return LoadStatus::Malformed;
  total = kPebSize + kTebSize + parameters_bytes + tls_bytes;
  uint64_t base = 0;
  if (!space.find_free(total, kPlacementHint, &base) || !space.map(base, total, kProtRead | kProtWrite)) return LoadStatus::OutOfMemory;
  const uint64_t peb = base, teb = base + kPebSize, parameters = teb + kTebSize, tls_area = parameters + parameters_bytes;
  const uint64_t tls_block = tls_size ? tls_area + 16 : 0;

  Writer w(space);
  // --- the parameters (RTL_USER_PROCESS_PARAMETERS, pointers absolute: flag NORMALIZED)
  uint64_t cursor = parameters + strings_at;
  const uint64_t path_at = cursor, command_at = path_at + path_bytes, directory_at = command_at + command_bytes, environment_at = directory_at + directory_bytes;
  w.bytes(path_at, image_path.view());
  w.bytes(command_at, command_line.view());
  w.bytes(directory_at, directory.view());
  w.bytes(environment_at, environment.view());
  w.u32(parameters + 0x00, uint32_t(parameters_bytes));      // MaximumLength
  w.u32(parameters + 0x04, uint32_t(parameters_bytes));      // Length
  w.u32(parameters + 0x08, 1);                               // Flags: RTL_USER_PROC_PARAMS_NORMALIZED
  w.u64(parameters + 0x20, kStdInputHandle);
  w.u64(parameters + 0x28, kStdOutputHandle);
  w.u64(parameters + 0x30, kStdErrorHandle);
  w.unicode_string(parameters + 0x38, directory_at, directory_bytes, directory_bytes);      // CurrentDirectory.DosPath
  w.unicode_string(parameters + 0x60, path_at, path_bytes, path_bytes);                     // ImagePathName
  w.unicode_string(parameters + 0x70, command_at, command_bytes, command_bytes);            // CommandLine
  w.u64(parameters + 0x80, environment_at);                                                 // Environment
  w.u64(parameters + 0x3F0, environment_bytes);                                             // EnvironmentSize

  // --- the PEB
  w.u64(peb + 0x08, ~uint64_t{0});                  // Mutant
  w.u64(peb + 0x10, image.base);                    // ImageBaseAddress
  w.u64(peb + 0x20, parameters);                    // ProcessParameters
  w.u32(peb + 0xB8, input.processor_count);         // NumberOfProcessors
  w.u32(peb + 0x118, 10);                           // OSMajorVersion
  w.u32(peb + 0x11C, 0);                            // OSMinorVersion
  w.u16(peb + 0x120, 19045);                        // OSBuildNumber
  w.u32(peb + 0x124, 2);                            // OSPlatformId: VER_PLATFORM_WIN32_NT
  w.u32(peb + 0x2C0, 1);                            // SessionId

  // --- the TEB (NT_TIB first)
  w.u64(teb + 0x00, ~uint64_t{0});                  // ExceptionList (unused on x64)
  w.u64(teb + 0x08, input.stack_top);               // StackBase
  w.u64(teb + 0x10, input.stack_top - input.stack_size);      // StackLimit
  w.u64(teb + 0x30, teb);                           // Self
  w.u64(teb + 0x40, input.process_id);              // ClientId.UniqueProcess
  w.u64(teb + 0x48, input.thread_id);               // ClientId.UniqueThread
  w.u64(teb + 0x60, peb);                           // ProcessEnvironmentBlock
  w.u64(teb + 0x1478, input.stack_top - input.stack_size);    // DeallocationStack
  if (tls_size) {
    // The static TLS: an array of one block pointer (module index 0), the block = template + zero fill.
    w.u64(tls_area, tls_block);
    w.u64(teb + 0x58, tls_area);                    // ThreadLocalStoragePointer
    if (tls_end > tls_start) {
      uint8_t chunk[256];
      for (uint64_t done = 0; done < tls_end - tls_start;) {
        const uint64_t n = tls_end - tls_start - done < sizeof(chunk) ? tls_end - tls_start - done : sizeof(chunk);
        if (!space.read_privileged(tls_start + done, ByteView(chunk, n))) return LoadStatus::Malformed;      // the template lies outside the image
        w.bytes(tls_block + done, ConstByteView(chunk, n));
        done += n;
      }
    }
    if (tls_index_at != 0) w.u32(tls_index_at, 0);      // _tls_index = 0 (the memory is the program's own writable data)
  }
  if (!w.ok()) return LoadStatus::Malformed;

  // --- KUSER_SHARED_DATA
  if (!space.map(kSharedUserDataAddress, kPage, kProtRead)) return LoadStatus::OutOfMemory;
  Writer shared(space);
  shared.u32(kSharedUserDataAddress + 0x04, 0x0FA00000);          // TickCountMultiplier
  shared.u16(kSharedUserDataAddress + 0x26, 9);                   // NativeProcessorArchitecture: PROCESSOR_ARCHITECTURE_AMD64
  shared.u32(kSharedUserDataAddress + 0x260, 19045);              // NtBuildNumber
  shared.u32(kSharedUserDataAddress + 0x264, 1);                  // NtProductType: workstation
  shared.u8(kSharedUserDataAddress + 0x268, 1);                   // ProductTypeIsValid
  shared.u32(kSharedUserDataAddress + 0x26C, 10);                 // NtMajorVersion
  shared.u32(kSharedUserDataAddress + 0x270, 0);                  // NtMinorVersion
  shared.u32(kSharedUserDataAddress + 0x3C0, input.processor_count);      // ActiveProcessorCount
  if (!shared.ok()) return LoadStatus::Malformed;

  out->peb = peb;
  out->teb = teb;
  out->parameters = parameters;
  out->tls_block = tls_block;
  out->shared_user_data = kSharedUserDataAddress;
  return LoadStatus::Ok;
}

}  // namespace process
