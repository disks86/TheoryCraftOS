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

#include "Hardware/WirelessInterface/WifiFirmwareSource.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

bool wifi_firmware_name_valid(StringView name) {
  if (name.size() == 0 || name.size() > 200 || name[0] == '/') return false;
  uint64_t start = 0;
  for (uint64_t i = 0; i <= name.size(); ++i) {
    if (i < name.size()) {
      const char c = name[i];
      if (c < 0x21 || c > 0x7E || c == '\\') return false;
      if (c != '/') continue;
    }
    const uint64_t length = i - start;
    if (length == 0) return false;
    if (length == 1 && name[start] == '.') return false;
    if (length == 2 && name[start] == '.' && name[start + 1] == '.') return false;
    start = i + 1;
  }
  return true;
}

bool FileSystemFirmwareSource::read(const char* name, ByteBuffer& out) {
  if (!name || !wifi_firmware_name_valid(StringView(name))) return false;
  String path(allocator_);
  if (!path.assign("/firmware/") || !path.append(StringView(name))) return false;
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(path.c_str()), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok || !file) return false;
  const uint64_t size = file->size();
  if (size == 0 || size > kMaxBytes) return false;
  out.clear();
  if (!out.put_zeros(size)) return false;
  uint64_t got = 0;
  if (file->read(0, out.writable_view(), &got) != filesystem::Status::Ok || got != size) {
    out.clear();
    return false;
  }
  return true;
}

}  // namespace hardware
