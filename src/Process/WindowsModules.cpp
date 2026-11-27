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
#include "Process/WindowsModules.hpp"
#include "Process/WindowsObjects.hpp"
#include "Process/ModuleLoader.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsProcess.hpp"
#include "Process/WindowsDebug.hpp"
#include "Process/ImageFileCache.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Utf16.hpp"

namespace process::windowsabi {

namespace {
constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidParameter = 0xC000000Du, kNotImplemented = 0xC0000002u, kDllNotFound = 0xC0000135u;
// A record (ntdll's LoadRecord): base, size, entry (8 bytes each), the canonical name (40), flags (4: 1 = a private layer), 4 reserved, the Windows folder of the
// file as UTF-16 (260 units, NUL-terminated; empty: System32).
constexpr uint32_t kRecordBytes = 592, kMaxRecords = 256;      // (the records are on the heap: kernel stacks are small)
constexpr uint32_t kRecordFlags = 64, kRecordDirectory = 72, kDirectoryUnits = 260;

// Writes `m` as a record into `out` (kRecordBytes, zeroed).
[[nodiscard]] bool write_record(ByteView out, const LoadedModule& m, allocators::IAllocator& allocator) {
  if (out.size() < kRecordBytes) return false;
  const uint64_t values[3] = {m.base, m.size, m.entry};
  for (uint32_t v = 0; v < 3; ++v) for (uint32_t b = 0; b < 8; ++b) out[v * 8 + b] = uint8_t(values[v] >> (8 * b));
  for (uint32_t k = 0; k < sizeof(m.name); ++k) out[24 + k] = uint8_t(m.name[k]);
  out[kRecordFlags] = m.private_layer ? 1 : 0;
  ByteBuffer directory(allocator);
  uint64_t length = 0;
  while (length < sizeof(m.directory) && m.directory[length]) ++length;
  if (!util::append_utf16le(&directory, StringView(m.directory, length)) || directory.size() >= 2u * kDirectoryUnits) return true;      // (System32 is said for one that cannot be)
  return out.subview(kRecordDirectory, directory.size()).copy_from(directory.view());
}
}  // namespace

namespace {
// The bytes of the DLL file at `path` (a Win32 path): the ones kept for that file (ImageFileCache: a big DLL that every process of a program loads is read once
// and its block taken once), or read now. False with an NTSTATUS in *status as read_program_file gives one.
bool library_file(Process& p, StringView path, SharedPointer<ByteBuffer>* out, uint32_t* status) {
  allocators::IAllocator& allocator = p.files().allocator();
  ImageFileCache* cache = image_file_cache();
  ImageFileCache::Key key;
  String virtual_path(allocator);
  if (cache) {
    filesystem::DirectoryEntry entry;
    if (p.files().resolve(path, &virtual_path) == PathResult::Ok && p.files().stat(virtual_path.view(), &entry) == filesystem::Status::Ok && entry.type == filesystem::NodeType::File) {
      key.size = entry.size;
      key.modified = entry.modified;
      key.exact_modified = entry.exact_modified;
      key.file_id = entry.file_id;
      SharedPointer<ByteBuffer> kept = cache->find(virtual_path.view(), key);
#ifdef TCOS_TRACE_SERVICE_FAILURES
      process_log(kept ? "library file kept: " : "library file read: ");
      process_log(virtual_path.view());
#endif
      if (kept) { *out = kept; return true; }
    } else {
      cache = nullptr;      // (read_program_file says why it is not there)
    }
  }
  SharedPointer<ByteBuffer> bytes = SharedPointer<ByteBuffer>::make(allocator, allocator);
  if (!bytes) { *status = 0xC0000017u; return false; }
  if (!read_program_file(p, path, bytes.get(), status)) return false;
  if (cache && bytes->size() == key.size) *out = cache->put(virtual_path.view(), key, bytes);      // (a file that changed while it was read is not kept)
  else *out = bytes;
  return true;
}
}  // namespace

uint32_t nt_load_module(Process& p, const SyscallRequest& r) {
  uint64_t count_at = 0, flags = 0;
  if (!argument(p, r, 4, &count_at) || !argument(p, r, 5, &flags)) return kAccessViolation;
  IModuleHost* host = p.module_host();
  if (!host || !p.files().attached()) return kNotImplemented;
  const uint32_t capacity = uint32_t(r.args[3]);
  if (capacity == 0 || capacity > kMaxRecords || r.args[1] == 0 || r.args[2] == 0) return kInvalidParameter;
  const uint8_t none[4] = {};
  if ((flags & 8) != 0) {      // (ours) one more use of the library whose image is at the address in *args[1] (LdrAddRefDll, GetModuleHandleEx): whatever its name or file
    uint64_t at = 0;
    if (!p.read_user_u64(r.args[1], &at)) return kAccessViolation;
    if (!host->reference_module_base(at)) return kDllNotFound;
    return count_at == 0 || p.write_user(count_at, ConstByteView(none, 4)) ? kSuccess : kAccessViolation;
  }
  String path(p.files().allocator());
  uint32_t status = read_unicode_string_argument(p, r.args[0], &path);
  if (status != kSuccess) return status;
  // A library named by its file name alone is the first loaded library of that name, if there is one (Windows matches loaded libraries by base name before it
  // searches); one named by a path is the library loaded from that file, if there is one, else that file is loaded as a library of its own - even when another
  // library of the same name is loaded (a program's own d3d9.dll that loads the one of System32 by its path gets that one, as on Windows).
  bool bare = true;
  for (char c : path.view()) if (c == '\\' || c == '/' || c == ':') bare = false;
  const uint64_t slash = path.view().rfind('\\');
  String canonical(p.files().allocator());
  if (!canonical_library_name(slash == StringView::npos ? path.view() : path.view().without_prefix(slash + 1), &canonical)) return kInvalidParameter;
  if (canonical.view().equals("msvcrt.dll")) {      // (a C runtime the program brought is that file, not msvcrt.dll: see ModuleSet::library_name())
    String plain(p.files().allocator());
    if (!plain_library_name(slash == StringView::npos ? path.view() : path.view().without_prefix(slash + 1), &plain)) return kInvalidParameter;
    if (host->owns_runtime(plain.view()) && !canonical.assign(plain.view())) return kInvalidParameter;
  }
  if ((flags & 4) != 0) {      // a library of an activation context (an assembly's own copy): known by its file as well as its name, so two files of one name are two libraries
    uint32_t hash = 2166136261u;
    for (char c : path.view()) hash = (hash ^ uint8_t(c >= 'A' && c <= 'Z' ? c + 32 : c)) * 16777619u;
    if (canonical.size() > 24 && !canonical.assign(canonical.view().first(24))) return kInvalidParameter;
    if (!canonical.push_back('#')) return kInvalidParameter;
    for (int shift = 28; shift >= 0; shift -= 4) if (!canonical.push_back("0123456789abcdef"[(hash >> shift) & 15])) return kInvalidParameter;
  }
  uint64_t existing = 0;
  auto answer_existing = [&]() {      // loaded already: one more use
    return p.write_user_u64(r.args[1], existing) && (count_at == 0 || p.write_user(count_at, ConstByteView(none, 4))) ? kSuccess : kAccessViolation;
  };
  if ((bare || (flags & 4) != 0) && host->reference_module(canonical.view(), &existing)) return answer_existing();      // (an activation context's copy: its name says its file)
  // A name that stands for another library (an API set such as api-ms-win-core-winrt-l1-1-0.dll, kernelbase.dll, a C runtime) is that library, from System32, wherever
  // the caller looked for it: no file of the alias's name exists.
  String alias_path(p.files().allocator());
  {
    String plain(p.files().allocator());
    const StringView file_name = slash == StringView::npos ? path.view() : path.view().without_prefix(slash + 1);
    for (char ch : file_name) if (!plain.push_back(ch >= 'A' && ch <= 'Z' ? char(ch + 32) : ch)) return kInvalidParameter;
    if (plain.view().rfind('.') == StringView::npos && !plain.append(".dll")) return kInvalidParameter;
    if (!(flags & 4) && !plain.view().equals(canonical.view()) && (!alias_path.assign("C:\\Windows\\System32\\") || !alias_path.append(canonical.view()))) return kInvalidParameter;
  }
  // A 32-bit program's system libraries are the ones of SysWOW64: a library it asks for in System32 (by GetSystemDirectory's path, or an alias) is the one of that
  // name there, and "not found" if there is none - the file in System32 is a 64-bit library, which it cannot load.
  String redirected(p.files().allocator());
  StringView from = alias_path.empty() ? path.view() : alias_path.view();
  constexpr StringView kSystem32("C:\\Windows\\System32\\");
  if (p.arch() == UserArch::X86_32 && from.size() > kSystem32.size()) {
    bool under = true;
    for (uint64_t i = 0; under && i < kSystem32.size(); ++i) {
      const char a = from[i] == '/' ? '\\' : from[i], b = kSystem32[i];
      under = (a >= 'A' && a <= 'Z' ? char(a + 32) : a) == (b >= 'A' && b <= 'Z' ? char(b + 32) : b);
    }
    if (under) {
      if (!redirected.assign("C:\\Windows\\SysWOW64\\") || !redirected.append(from.without_prefix(kSystem32.size()))) return kInvalidParameter;
      from = redirected.view();
    }
  }
  // The file in the OS file system ("/Windows/Windows/System32/d3d9.dll", as the library provider names the files it reads): what the library is known by.
  String os_path(p.files().allocator());
  {
    String virtual_path(p.files().allocator());
    const bool known = p.files().resolve(from, &virtual_path) == PathResult::Ok && p.files().host_path_of(virtual_path.view(), &os_path);
    if (!known && !os_path.assign(from)) return 0xC0000017u;      // (a name the process cannot resolve: the read below says why it fails)
  }
  if (host->reference_module_file(os_path.view(), &existing)) return answer_existing();
  SharedPointer<ByteBuffer> file;
  if (!library_file(p, from, &file, &status)) {
#ifdef TCOS_TRACE_SERVICE_FAILURES
    // (with the trace of failing services: which library it was - the service's arguments are pointers, and a program that gives up after this says nothing)
    process_log("library not found: ");
    process_log(from);
#endif
    return status == 0xC0000034u || status == 0xC000003Au ? kDllNotFound : status;
  }
  {
    exe::PeFile checked(p.files().allocator());
    if (const uint32_t header = image_section_status(file->view(), checked)) return header;
  }
  List<LoadedModule> loaded_list(p.files().allocator());
  ByteBuffer record_buffer(p.files().allocator());
  if (!loaded_list.reserve(capacity) || !record_buffer.put_zeros(uint64_t{capacity} * kRecordBytes)) return 0xC0000017u;
  for (uint32_t i = 0; i < capacity; ++i) loaded_list.push_back(LoadedModule());
  LoadedModule* loaded = &loaded_list[0];
  uint32_t count = 0;
  status = host->load_module(file->view(), canonical.view(), os_path.view(), loaded, capacity, &count, (flags & 1) == 0);
  if (status == 0xC000007Bu) status = 0xC00000BBu;      // a good image that this process cannot run (a 32-bit library): the caller may try the next file of its path
  if (status != kSuccess) return status;
  ByteView records = record_buffer.writable_view();
  uint64_t base = 0;
  bool written = true;
  for (uint32_t i = 0; i < count; ++i) {
    written = write_record(records.subview(uint64_t{i} * kRecordBytes, kRecordBytes), loaded[i], p.files().allocator()) && written;
    if (StringView(loaded[i].name).equals(canonical.view()) && !loaded[i].private_layer && base == 0) base = loaded[i].base;
  }
  uint8_t count_bytes[4] = {uint8_t(count), uint8_t(count >> 8), uint8_t(count >> 16), uint8_t(count >> 24)};
  if (!written || !p.write_user_u64(r.args[1], base) || !p.write_user(r.args[2], records.first(uint64_t{count} * kRecordBytes)) || (count_at != 0 && !p.write_user(count_at, ConstByteView(count_bytes, 4)))) {
    for (uint32_t i = count; i > 0; --i) host->unload_module(loaded[i - 1].base);
    return kAccessViolation;
  }
  for (uint32_t i = 0; i < count; ++i) debug_module_loaded(p, loaded[i].base);
  return kSuccess;
}

uint32_t nt_release_module(Process& p, const SyscallRequest& r) {
  const uint64_t count_at = r.args[3];
  IModuleHost* host = p.module_host();
  if (!host) return kNotImplemented;
  const uint32_t capacity = uint32_t(r.args[2]);
  if (capacity == 0 || capacity > kMaxRecords || r.args[1] == 0) return kInvalidParameter;
  List<LoadedModule> doomed_list(p.files().allocator());
  ByteBuffer record_buffer(p.files().allocator());
  if (!doomed_list.reserve(capacity) || !record_buffer.put_zeros(uint64_t{capacity} * kRecordBytes)) return 0xC0000017u;
  for (uint32_t i = 0; i < capacity; ++i) doomed_list.push_back(LoadedModule());
  LoadedModule* doomed = &doomed_list[0];
  uint32_t count = 0;
  const uint32_t status = host->release_module(r.args[0], doomed, capacity, &count);
  if (status != kSuccess) return status;
  ByteView records = record_buffer.writable_view();
  bool written = true;
  for (uint32_t i = 0; i < count; ++i) written = write_record(records.subview(uint64_t{i} * kRecordBytes, kRecordBytes), doomed[i], p.files().allocator()) && written;
  const uint8_t count_bytes[4] = {uint8_t(count), uint8_t(count >> 8), uint8_t(count >> 16), uint8_t(count >> 24)};
  if (!written || (count != 0 && !p.write_user(r.args[1], records.first(uint64_t{count} * kRecordBytes))) || (count_at != 0 && !p.write_user(count_at, ConstByteView(count_bytes, 4)))) return kAccessViolation;
  return kSuccess;
}

uint32_t nt_link_modules(Process& p, const SyscallRequest& r) {
  IModuleHost* host = p.module_host();
  if (!host) return kNotImplemented;
  return host->link_modules(r.args[0], r.args[1]);
}

uint32_t nt_unload_module(Process& p, const SyscallRequest& r) {
  IModuleHost* host = p.module_host();
  if (!host) return kNotImplemented;
  const uint32_t status = host->unload_module(r.args[0]);
  if (status == kSuccess) debug_module_unloaded(p, r.args[0]);
  return status;
}

}  // namespace process::windowsabi
