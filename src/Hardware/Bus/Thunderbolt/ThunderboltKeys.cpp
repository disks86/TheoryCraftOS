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

#include "Hardware/Bus/Thunderbolt/ThunderboltKeys.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

bool FileTbKeyStore::path_of(const uint8_t uuid[16], String& path) {
  static const char hex[] = "0123456789abcdef";
  if (!path.assign(filesystem::layout::kThunderboltKeys) || !path.append(StringView("/"))) return false;
  for (uint32_t i = 0; i < 16; ++i) {
    const char pair[3] = {hex[uuid[i] >> 4], hex[uuid[i] & 15], 0};
    if (!path.append(StringView(pair))) return false;
  }
  return true;
}

bool FileTbKeyStore::load(const uint8_t uuid[16], uint8_t key[kTbKeyBytes]) {
  String path(allocator_);
  if (!path_of(uuid, path)) return false;
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(path.c_str()), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok || !file || file->size() != kTbKeyBytes) return false;
  uint64_t got = 0;
  return file->read(0, ByteView(key, kTbKeyBytes), &got) == filesystem::Status::Ok && got == kTbKeyBytes;
}

bool FileTbKeyStore::store(const uint8_t uuid[16], const uint8_t key[kTbKeyBytes]) {
  String path(allocator_);
  if (!path_of(uuid, path)) return false;
  // the folders on the way (they may exist already)
  const char* folders[] = {"/AppStore", "/AppStore/Thunderbolt", filesystem::layout::kThunderboltKeys};
  for (const char* folder : folders) {
    const filesystem::Status s = file_system_.create_directory(StringView(folder));
    if (s != filesystem::Status::Ok && s != filesystem::Status::AlreadyExists) return false;
  }
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(path.c_str()), filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok || !file) return false;
  uint64_t put = 0;
  if (file->write(0, ConstByteView(key, kTbKeyBytes), &put) != filesystem::Status::Ok || put != kTbKeyBytes || file->flush() != filesystem::Status::Ok) return false;
  return file_system_.flush() == filesystem::Status::Ok;
}

void FileTbKeyStore::remove(const uint8_t uuid[16]) {
  String path(allocator_);
  if (path_of(uuid, path)) file_system_.remove(StringView(path.c_str()));
}

}  // namespace hardware
