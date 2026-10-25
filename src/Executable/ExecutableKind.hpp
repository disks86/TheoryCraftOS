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
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace exe {

enum class ExecutableKind : uint8_t { None, Elf, Pe };

// How many bytes from the start of a file identify it (the PE signature sits where the DOS header says, normally in the first few hundred bytes).
constexpr uint64_t kIdentifyBytes = 4096;

// Is this the start of an ELF or PE file? Looks at the magic numbers only (a PE needs its "PE\0\0" signature inside `head`), so it is cheap and
// does not accept every file that happens to start with "MZ".
ExecutableKind identify_executable(ConstByteView head);
// Reads the start of the file at `path` and identifies it; None if it cannot be read.
ExecutableKind identify_executable_file(filesystem::IFileSystem& fs, StringView path);
StringView executable_kind_name(ExecutableKind kind);      // "ELF", "PE", ""

// The text of the "this cannot be run" notice for the file at `path` (only its name is shown). False if memory ran out.
bool unsupported_executable_message(StringView path, ExecutableKind kind, String* out);

}  // namespace exe
