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

#include "DBus/MachineId.hpp"
#include "FileSystem/SystemLayout.hpp"

namespace dbus {

bool is_hex_id(StringView text) {
  if (text.size() != 32) return false;
  for (char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

bool random_hex_id(crypto::IEntropySource& entropy, ArrayView<char> out) {
  if (out.size() < 33) return false;
  uint8_t bytes[16] = {};
  if (!entropy.read(ByteView(bytes, 16))) return false;
  static const char kHex[] = "0123456789abcdef";
  for (uint32_t i = 0; i < 16; ++i) {
    out[2 * i] = kHex[bytes[i] >> 4];
    out[2 * i + 1] = kHex[bytes[i] & 15];
  }
  out[32] = '\0';
  return true;
}

bool load_machine_id(filesystem::IFileSystem* files, crypto::IEntropySource& entropy, ArrayView<char> out) {
  if (out.size() < 33) return false;
  UniquePointer<filesystem::IFile> file;
  if (files && files->open_file(filesystem::layout::kLinuxMachineId, filesystem::OpenMode::Read, &file) == filesystem::Status::Ok) {
    uint8_t text[33] = {};
    uint64_t got = 0;
    if (file->read(0, ByteView(text, 33), &got) == filesystem::Status::Ok && got >= 32 && is_hex_id(StringView(ConstByteView(text, 32))) &&
        (got == 32 || text[32] == '\n')) {
      StringView(ConstByteView(text, 32)).copy_to(out);
      return true;
    }
    file.reset();
  }
  if (!random_hex_id(entropy, out)) return false;
  if (files && files->open_file(filesystem::layout::kLinuxMachineId, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok) {
    uint8_t line[33];
    ByteView(line, 33).first(32).copy_from(StringView(out.data(), 32).bytes());
    line[32] = '\n';
    uint64_t written = 0;
    file->write(0, ConstByteView(line, 33), &written);
    file->flush();
  }
  return true;
}

}  // namespace dbus
