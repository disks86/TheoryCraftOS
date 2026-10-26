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
  if (!utf16(input.image_path, &image_path) || !utf16(input.command_line, &command_line) || !utf16("C:", &directory)) return LoadStatus::Malformed;
  {
    // C:\dir\ : the folder with backslashes and a trailing one.
    if (input.current_directory.size() > 1) {
      for (char c : input.current_directory) {
        const char converted = c == '/' ? '\\' : c;
        if (!utf16(StringView(&converted, 1), &directory)) return LoadStatus::Malformed;
      }
    }
    if (!utf16("\\", &directory)) return LoadStatus::Malformed;
  }
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
  const uint64_t strings_bytes = path_bytes + command_bytes + directory_bytes + environment_bytes + 16;      // (the three strings are followed by a NUL each: 6 of the 16)
  uint64_t parameters_bytes = 0, tls_bytes = 0, total = 0;
  if (strings_bytes > kMaxStartupBytes || !AddressSpace::round_up(strings_at + strings_bytes, &parameters_bytes)) return LoadStatus::Malformed;
  if (!AddressSpace::round_up(tls_size ? 16 + tls_size : 0, &tls_bytes)) return LoadStatus::Malformed;
  // The loader lists: PEB_LDR_DATA, and per module a LDR_DATA_TABLE_ENTRY with its two names.
  constexpr uint64_t kLdrHeader = 0x60, kEntrySize = 0x140, kPerModule = 1024;
  if (input.modules.size() > 64) return LoadStatus::Malformed;
  uint64_t ldr_bytes = 0;
  if (!input.modules.empty() && !AddressSpace::round_up(kLdrHeader + input.modules.size() * kPerModule, &ldr_bytes)) return LoadStatus::Malformed;
  total = kPebSize + kTebSize + parameters_bytes + tls_bytes + ldr_bytes;
  uint64_t base = 0;
  if (!space.find_free(total, kPlacementHint, &base) || !space.map(base, total, kProtRead | kProtWrite)) return LoadStatus::OutOfMemory;
  const uint64_t peb = base, teb = base + kPebSize, parameters = teb + kTebSize, tls_area = parameters + parameters_bytes, ldr = tls_area + tls_bytes;
  const uint64_t tls_block = tls_size ? tls_area + 16 : 0;

  Writer w(space);
  // --- the parameters (RTL_USER_PROCESS_PARAMETERS, pointers absolute: flag NORMALIZED)
  uint64_t cursor = parameters + strings_at;
  const uint64_t path_at = cursor, command_at = path_at + path_bytes + 2, directory_at = command_at + command_bytes + 2, environment_at = directory_at + directory_bytes + 2;      // (zeroed memory: each string ends in a NUL, as Windows has it)
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
  w.unicode_string(parameters + 0x38, directory_at, directory_bytes, directory_bytes + 2);      // CurrentDirectory.DosPath
  w.unicode_string(parameters + 0x60, path_at, path_bytes, path_bytes + 2);                    // ImagePathName
  w.unicode_string(parameters + 0x70, command_at, command_bytes, command_bytes + 2);            // CommandLine
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
  // --- the loader lists
  if (!input.modules.empty()) {
    const uint64_t count = input.modules.size();
    // LIST_ENTRY heads at ldr + 0x10 (load order), 0x20 (memory order, the same here), 0x30 (initialization order).
    w.u32(ldr + 0x00, 0x58);                           // Length
    w.u8(ldr + 0x04, 1);                               // Initialized
    auto entry_at = [&](uint64_t i) { return ldr + kLdrHeader + i * kPerModule; };
    // The three chains: a list of entry addresses through the link at `link_offset` of each, closed on the head.
    auto chain = [&](uint64_t head, uint64_t link_offset, const uint64_t* members, uint64_t n) {
      uint64_t previous = head;
      for (uint64_t k = 0; k < n; ++k) {
        const uint64_t link = entry_at(members[k]) + link_offset;
        w.u64(previous, link);                         // previous.Flink = this
        w.u64(link + 8, previous);                     // this.Blink = previous
        previous = link;
      }
      w.u64(previous, head);                           // last.Flink = head
      w.u64(head + 8, previous);                       // head.Blink = last
    };
    uint64_t all[64], init[64], init_count = 0;
    for (uint64_t i = 0; i < count; ++i) all[i] = i;
    for (uint32_t index : input.init_order) {
      if (index >= count || init_count >= 64) return LoadStatus::Malformed;
      init[init_count++] = index;
    }
    chain(ldr + 0x10, 0x00, all, count);
    chain(ldr + 0x20, 0x10, all, count);
    chain(ldr + 0x30, 0x20, init, init_count);
    for (uint64_t i = 0; i < count; ++i) {
      const WindowsModule& m = input.modules[i];
      if (m.name.size() > 100) return LoadStatus::Malformed;
      const uint64_t entry = entry_at(i);
      ByteBuffer base_name(allocator), full_name(allocator);
      if (!utf16(m.name, &base_name)) return LoadStatus::Malformed;
      if (m.main) {
        if (!utf16(input.image_path, &full_name)) return LoadStatus::Malformed;
      } else if (!utf16("C:\\Windows\\System32\\", &full_name) || !utf16(m.name, &full_name)) {
        return LoadStatus::Malformed;
      }
      if (base_name.size() + full_name.size() + 8 > kPerModule - kEntrySize) return LoadStatus::Malformed;
      const uint64_t strings = entry + kEntrySize;
      w.bytes(strings, base_name.view());
      w.bytes(strings + base_name.size() + 2, full_name.view());
      w.u64(entry + 0x30, m.base);                     // DllBase
      w.u64(entry + 0x38, m.entry);                    // EntryPoint
      w.u32(entry + 0x40, uint32_t(m.size));           // SizeOfImage
      w.unicode_string(entry + 0x48, strings + base_name.size() + 2, full_name.size(), full_name.size() + 2);      // FullDllName
      w.unicode_string(entry + 0x58, strings, base_name.size(), base_name.size() + 2);                               // BaseDllName
      w.u32(entry + 0x68, m.main ? 0x20 : 0x4);        // Flags: LDRP_IMAGE_DLL
      w.u16(entry + 0x6C, 0xFFFF);                     // LoadCount: static
    }
    w.u64(peb + 0x18, ldr);                            // PEB.Ldr
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

  out->tls_template = tls_start;
  out->tls_template_bytes = tls_end - tls_start;
  out->tls_zero_fill = tls_zero;
  out->peb = peb;
  out->teb = teb;
  out->parameters = parameters;
  out->tls_block = tls_block;
  out->shared_user_data = kSharedUserDataAddress;
  out->ldr = input.modules.empty() ? 0 : ldr;
  return LoadStatus::Ok;
}

LoadStatus build_windows_thread(AddressSpace& space, const WindowsThreadInput& input, WindowsThread* out) {
  constexpr uint64_t kMinStack = 64 * 1024, kMaxStack = 256ull << 20, kPlacement = 0x00007FE000000000ull;
  uint64_t stack = 0, tls_bytes = 0, total = 0, tls_size = input.tls_template_bytes + input.tls_zero_fill;
  if (input.tls_template_bytes > kMaxStartupBytes || input.tls_zero_fill > kMaxStartupBytes) return LoadStatus::Malformed;
  if (!AddressSpace::round_up(input.stack_bytes < kMinStack ? kMinStack : input.stack_bytes, &stack) || stack > kMaxStack) return LoadStatus::Malformed;
  if (!AddressSpace::round_up(tls_size ? 16 + tls_size : 0, &tls_bytes)) return LoadStatus::Malformed;
  total = stack + kTebSize + tls_bytes;
  uint64_t base = 0;
  if (!space.find_free(total, kPlacement, &base) || !space.map(base, total, kProtRead | kProtWrite)) return LoadStatus::OutOfMemory;
  const uint64_t stack_top = base + stack, teb = stack_top, tls_area = teb + kTebSize, tls_block = tls_size ? tls_area + 16 : 0;
  Writer w(space);
  w.u64(teb + 0x00, ~uint64_t{0});
  w.u64(teb + 0x08, stack_top);                     // StackBase
  w.u64(teb + 0x10, base);                          // StackLimit
  w.u64(teb + 0x30, teb);                           // Self
  w.u64(teb + 0x40, input.process_id);              // ClientId
  w.u64(teb + 0x48, input.thread_id);
  w.u64(teb + 0x60, input.peb);
  w.u64(teb + 0x1478, base);                        // DeallocationStack
  if (tls_size) {
    w.u64(tls_area, tls_block);
    w.u64(teb + 0x58, tls_area);                    // ThreadLocalStoragePointer
    uint8_t chunk[256];
    for (uint64_t done = 0; done < input.tls_template_bytes;) {
      const uint64_t n = input.tls_template_bytes - done < sizeof(chunk) ? input.tls_template_bytes - done : sizeof(chunk);
      if (!space.read_privileged(input.tls_template + done, ByteView(chunk, n))) { space.unmap(base, total); return LoadStatus::Malformed; }
      w.bytes(tls_block + done, ConstByteView(chunk, n));
      done += n;
    }
  }
  if (!w.ok()) { space.unmap(base, total); return LoadStatus::Malformed; }
  out->teb = teb;
  out->stack_top = stack_top;
  out->stack_pointer = windows_initial_stack_pointer(stack_top);
  out->region_base = base;
  out->region_size = total;
  return LoadStatus::Ok;
}

}  // namespace process
