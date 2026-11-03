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

#include "Process/WindowsDialogs.hpp"
#include "Process/CommonDialog.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Utf16.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidParameter = 0xC000000Du, kNoMemory = 0xC0000017u, kNotSupported = 0xC00000BBu,
                   kBufferTooSmall = 0xC0000023u, kObjectPathNotFound = 0xC000003Au;
constexpr uint32_t kRequestSize = 72;
uint32_t swap_red_blue(uint32_t color) { return (color & 0xFF00u) | ((color & 0xFFu) << 16) | ((color >> 16) & 0xFFu); }      // COLORREF 0x00BBGGRR <-> 0x00RRGGBB
constexpr uint64_t kMaxUnits = 1 << 15;      // the longest text read from the program: a path, a title, the whole filter list

// The program's NUL-terminated wide string at `address` (up to kMaxUnits characters) as UTF-8 into `out`; `*units` is how many UTF-16 units it took.
uint32_t read_wide_string(Process& p, uint64_t address, String* out, uint64_t* units = nullptr) {
  out->clear();
  if (units) *units = 0;
  if (address == 0) return kSuccess;
  ByteBuffer raw(out->allocator());
  uint64_t count = 0;
  for (;;) {
    uint8_t pair[2];
    if (count >= kMaxUnits) return kInvalidParameter;
    if (!p.read_user(address + 2 * count, ByteView(pair, 2))) return kAccessViolation;
    if (pair[0] == 0 && pair[1] == 0) break;
    if (!raw.put8(pair[0]) || !raw.put8(pair[1])) return kNoMemory;
    ++count;
  }
  if (units) *units = count;
  return util::append_utf8_from_utf16le(out, raw.view()) ? kSuccess : kNoMemory;
}

// The filter list: wide strings in pairs, "name" NUL "extensions" NUL, ended by an empty name. The pairs are read straight from the program's memory so that a NUL inside the
// list works (read_wide_string stops at one).
uint32_t read_filters(Process& p, uint64_t address, CommonDialogRequest* request) {
  if (address == 0) return kSuccess;
  uint64_t at = address;
  uint64_t used = 0;
  for (uint32_t i = 0; i < CommonDialogRequest::kMaxFilters; ++i) {
    String name(request->start_path.allocator()), extensions(request->start_path.allocator());
    uint64_t units = 0;
    uint32_t status = read_wide_string(p, at, &name, &units);
    if (status != kSuccess) return status;
    if (units == 0) return kSuccess;
    at += 2 * (units + 1);
    used += units + 1;
    status = read_wide_string(p, at, &extensions, &units);
    if (status != kSuccess) return status;
    at += 2 * (units + 1);
    used += units + 1;
    if (used > kMaxUnits) return kInvalidParameter;
    CommonDialogRequest::Filter& f = request->filters[request->filter_count++];
    f.name.assign(name.view().size() <= f.name.capacity() ? name.view() : name.view().first(f.name.capacity()));
    if (!f.extensions.assign(extensions.view())) f.extensions.assign("*");
  }
  return kSuccess;
}

// "C:\Dir\file.txt" (or a relative path) to the path of the OS file system.
bool host_path_of_dos(Process& p, StringView dos, String* host) {
  if (dos.empty()) return host->assign(StringView());
  String virtual_path(host->allocator());
  if (p.files().resolve(dos, &virtual_path) != PathResult::Ok) return false;
  return p.files().host_path_of(virtual_path.view(), host);
}

// The path of the OS file system as a DOS path of the program's C: drive; false for one outside it.
bool dos_path_of_host(Process& p, StringView host, String* dos) {
  String virtual_path(dos->allocator());
  if (!p.files().virtual_path_of_host(host, &virtual_path)) return false;
  if (!dos->assign("C:")) return false;
  if (virtual_path.view().equals("/")) return dos->append("\\");
  for (char c : virtual_path.view()) if (!dos->push_back(c == '/' ? '\\' : c)) return false;
  return true;
}

}  // namespace

uint32_t nt_tcos_common_dialog(Process& p, const SyscallRequest& r) {
  ICommonDialogs* screen = p.host().common_dialogs();
  uint8_t raw[kRequestSize];
  if (!p.read_user(r.args[0], ByteView(raw, kRequestSize))) return kAccessViolation;
  auto u32 = [&](uint32_t at) { return uint32_t(raw[at]) | uint32_t(raw[at + 1]) << 8 | uint32_t(raw[at + 2]) << 16 | uint32_t(raw[at + 3]) << 24; };
  auto u64 = [&](uint32_t at) { return uint64_t(u32(at)) | uint64_t(u32(at + 4)) << 32; };
  if (u32(0) != kRequestSize) return kInvalidParameter;
  const uint32_t kind = u32(4);
  if (kind < 1 || kind > 3) return kInvalidParameter;
  if (!screen) return kNotSupported;

  allocators::IAllocator& allocator = p.files().allocator();
  CommonDialogRequest request(allocator);
  request.kind = kind == 1 ? CommonDialogRequest::Kind::OpenFile : kind == 2 ? CommonDialogRequest::Kind::SaveFile : CommonDialogRequest::Kind::Color;
  request.filter_index = u32(12);
  request.color = swap_red_blue(u32(16));
  String text(allocator);
  uint32_t status = read_wide_string(p, u64(32), &text);
  if (status != kSuccess) return status;
  request.title.assign(text.view().size() <= request.title.capacity() ? text.view() : text.view().first(request.title.capacity()));
  status = read_wide_string(p, u64(40), &text);
  if (status != kSuccess) return status;
  if (!host_path_of_dos(p, text.view(), &request.start_path)) request.start_path.clear();
  status = read_filters(p, u64(48), &request);
  if (status != kSuccess) return status;
  if (request.filter_index >= request.filter_count) request.filter_index = 0;

  CommonDialogReply reply(allocator);
  if (!screen->run(request, &reply)) return kNotSupported;

  uint32_t result_status = kSuccess;
  uint32_t length = 0;
  if (reply.accepted && kind != 3) {
    String dos(allocator);
    if (!dos_path_of_host(p, reply.path.view(), &dos)) { reply.accepted = false; result_status = kObjectPathNotFound; }
    else {
      ByteBuffer units(allocator);
      if (!util::append_utf16le(&units, dos.view())) return kNoMemory;
      length = uint32_t(units.size() / 2);
      const uint32_t capacity = u32(64);
      const uint64_t needed = uint64_t{length} + 1;
      if (needed > capacity) result_status = kBufferTooSmall;
      else {
        if (!units.put8(0) || !units.put8(0)) return kNoMemory;
        if (!p.write_user(u64(56), units.view())) return kAccessViolation;
      }
    }
  }
  const uint64_t base = r.args[0];
  if (!p.write_user_u32(base + 24, reply.accepted ? 1 : 0) || !p.write_user_u32(base + 20, swap_red_blue(reply.color)) || !p.write_user_u32(base + 12, reply.filter_index) ||
      !p.write_user_u32(base + 68, length))
    return kAccessViolation;
  return result_status;
}

}  // namespace process::windowsabi
