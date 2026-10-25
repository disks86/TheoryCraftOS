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
#include "Executable/ExecutableKind.hpp"
#include "Executable/ElfFile.hpp"
#include "Executable/PeFile.hpp"

namespace exe {

ExecutableKind identify_executable(ConstByteView head) {
  if (ElfFile::looks_like_elf(head)) return ExecutableKind::Elf;
  if (PeFile::looks_like_pe(head)) return ExecutableKind::Pe;
  return ExecutableKind::None;
}

ExecutableKind identify_executable_file(filesystem::IFileSystem& fs, StringView path) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok || !file) return ExecutableKind::None;
  uint8_t buffer[kIdentifyBytes];
  uint64_t got = 0;
  if (file->read(0, ByteView(buffer, kIdentifyBytes), &got) != filesystem::Status::Ok || got > kIdentifyBytes) return ExecutableKind::None;
  return identify_executable(ConstByteView(buffer, got));
}

StringView executable_kind_name(ExecutableKind kind) {
  switch (kind) {
    case ExecutableKind::Elf: return "ELF";
    case ExecutableKind::Pe: return "PE";
    default: return "";
  }
}

bool unsupported_executable_message(StringView path, ExecutableKind kind, String* out) {
  const uint64_t slash = path.rfind('/');
  const StringView name = slash == StringView::npos ? path : path.substr(slash + 1);
  return out->assign("Unsupported executable: ") && out->append(name) && out->append("\n\nThis is an ") && out->append(executable_kind_name(kind)) &&
         out->append(" program, and running programs is not supported yet.");
}

}  // namespace exe
