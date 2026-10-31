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

namespace process::windowsabi {

namespace {
constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidParameter = 0xC000000Du, kNotImplemented = 0xC0000002u, kDllNotFound = 0xC0000135u;
constexpr uint32_t kRecordBytes = 64, kMaxRecords = 32;
}  // namespace

uint32_t nt_load_module(Process& p, const SyscallRequest& r) {
  uint64_t count_at = 0, flags = 0;
  if (!argument(p, r, 4, &count_at) || !argument(p, r, 5, &flags)) return kAccessViolation;
  IModuleHost* host = p.module_host();
  if (!host || !p.files().attached()) return kNotImplemented;
  const uint32_t capacity = uint32_t(r.args[3]);
  if (capacity == 0 || capacity > kMaxRecords || r.args[1] == 0 || r.args[2] == 0) return kInvalidParameter;
  String path(p.files().allocator());
  uint32_t status = read_unicode_string_argument(p, r.args[0], &path);
  if (status != kSuccess) return status;
  const uint64_t slash = path.view().rfind('\\');
  String canonical(p.files().allocator());
  if (!canonical_library_name(slash == StringView::npos ? path.view() : path.view().without_prefix(slash + 1), &canonical)) return kInvalidParameter;
  if ((flags & 4) != 0) {      // a library of an activation context (an assembly's own copy): known by its file as well as its name, so two files of one name are two libraries
    uint32_t hash = 2166136261u;
    for (char c : path.view()) hash = (hash ^ uint8_t(c >= 'A' && c <= 'Z' ? c + 32 : c)) * 16777619u;
    if (canonical.size() > 24 && !canonical.assign(canonical.view().first(24))) return kInvalidParameter;
    if (!canonical.push_back('#')) return kInvalidParameter;
    for (int shift = 28; shift >= 0; shift -= 4) if (!canonical.push_back("0123456789abcdef"[(hash >> shift) & 15])) return kInvalidParameter;
  }
  uint64_t existing = 0;
  if (host->reference_module(canonical.view(), &existing)) {      // loaded already: one more use
    const uint8_t none[4] = {};
    return p.write_user_u64(r.args[1], existing) && (count_at == 0 || p.write_user(count_at, ConstByteView(none, 4))) ? kSuccess : kAccessViolation;
  }
  ByteBuffer file(p.files().allocator());
  if (!read_program_file(p, path.view(), &file, &status)) return status == 0xC0000034u || status == 0xC000003Au ? kDllNotFound : status;
  {
    exe::PeFile checked(p.files().allocator());
    if (const uint32_t header = image_section_status(file.view(), checked)) return header;
  }
  LoadedModule loaded[kMaxRecords];
  uint32_t count = 0;
  status = host->load_module(file.view(), canonical.view(), loaded, capacity, &count, (flags & 1) == 0);
  if (status == 0xC000007Bu) status = 0xC00000BBu;      // a good image that this process cannot run (a 32-bit library): the caller may try the next file of its path
  if (status != kSuccess) return status;
  uint8_t records[kMaxRecords * kRecordBytes] = {};
  uint64_t base = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* out = records + i * kRecordBytes;
    const uint64_t values[3] = {loaded[i].base, loaded[i].size, loaded[i].entry};
    for (uint32_t v = 0; v < 3; ++v) for (uint32_t b = 0; b < 8; ++b) out[v * 8 + b] = uint8_t(values[v] >> (8 * b));
    for (uint32_t k = 0; k < sizeof(loaded[i].name); ++k) out[24 + k] = uint8_t(loaded[i].name[k]);
    if (StringView(loaded[i].name).equals(canonical.view())) base = loaded[i].base;
  }
  uint8_t count_bytes[4] = {uint8_t(count), uint8_t(count >> 8), uint8_t(count >> 16), uint8_t(count >> 24)};
  if (!p.write_user_u64(r.args[1], base) || !p.write_user(r.args[2], ConstByteView(records, count * kRecordBytes)) || (count_at != 0 && !p.write_user(count_at, ConstByteView(count_bytes, 4)))) {
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
  LoadedModule doomed[kMaxRecords];
  uint32_t count = 0;
  const uint32_t status = host->release_module(r.args[0], doomed, capacity, &count);
  if (status != kSuccess) return status;
  uint8_t records[kMaxRecords * kRecordBytes] = {};
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* out = records + i * kRecordBytes;
    const uint64_t values[3] = {doomed[i].base, doomed[i].size, doomed[i].entry};
    for (uint32_t v = 0; v < 3; ++v) for (uint32_t b = 0; b < 8; ++b) out[v * 8 + b] = uint8_t(values[v] >> (8 * b));
    for (uint32_t k = 0; k < sizeof(doomed[i].name); ++k) out[24 + k] = uint8_t(doomed[i].name[k]);
  }
  const uint8_t count_bytes[4] = {uint8_t(count), uint8_t(count >> 8), uint8_t(count >> 16), uint8_t(count >> 24)};
  if ((count != 0 && !p.write_user(r.args[1], ConstByteView(records, count * kRecordBytes))) || (count_at != 0 && !p.write_user(count_at, ConstByteView(count_bytes, 4)))) return kAccessViolation;
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
