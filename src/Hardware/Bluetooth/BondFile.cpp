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

#include "Hardware/Bluetooth/BondFile.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace {
constexpr uint64_t kMaxBondFile = 64 * 1024;
}

bool FileBluetoothBonds::load(ByteBuffer& out) {
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(filesystem::layout::kBluetoothBonds), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok || !file) return false;
  const uint64_t size = file->size();
  if (size == 0 || size > kMaxBondFile) return false;
  out.clear();
  if (!out.put_zeros(size)) return false;
  uint64_t got = 0;
  if (file->read(0, out.writable_view(), &got) != filesystem::Status::Ok || got != size) {
    out.clear();
    return false;
  }
  return true;
}

bool FileBluetoothBonds::save(ConstByteView bytes) {
  const char* folders[] = {"/AppStore", "/AppStore/Bluetooth"};
  for (const char* folder : folders) {
    const filesystem::Status s = file_system_.create_directory(StringView(folder));
    if (s != filesystem::Status::Ok && s != filesystem::Status::AlreadyExists) return false;
  }
  UniquePointer<filesystem::IFile> file;
  if (file_system_.open_file(StringView(filesystem::layout::kBluetoothBonds), filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok || !file) return false;
  uint64_t put = 0;
  if (file->write(0, bytes, &put) != filesystem::Status::Ok || put != bytes.size() || file->flush() != filesystem::Status::Ok) return false;
  return file_system_.flush() == filesystem::Status::Ok;
}

}  // namespace hardware
