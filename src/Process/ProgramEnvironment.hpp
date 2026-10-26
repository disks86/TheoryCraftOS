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
#include <stdint.h>
#include "Process/Syscall.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace process {

// How a program started from the shell sees the places the shell knows by their paths in the OS file system. "/Linux" holds the world of Linux programs
// (their "/"), "/Windows" the world of Windows programs (their "C:\").
constexpr StringView kLinuxRoot = "/Linux";
constexpr StringView kWindowsRoot = "/Windows";

// The folder that is the root of the world for this kind of program.
StringView world_root(SyscallAbi abi);

// The name the program knows `shell_path` by: its path inside the world if it lies in it ("/Linux/bin/tool" is "/bin/tool"; "/Windows/tools/app.exe" is
// "C:\tools\app.exe"), else only its file name at the root of the world ("/tool", "C:\app.exe"): the program cannot see where it really is.
bool program_name(SyscallAbi abi, StringView shell_path, String* out);

// The program's current folder for a shell in `shell_cwd`: the same place seen from inside the world ("/" or "/work" for Linux; Windows keeps its folders
// in the same form, "/work", which the file services understand), or the root of the world if the shell is somewhere else.
bool world_directory(SyscallAbi abi, StringView shell_cwd, String* out);

// The environment a program starts with, as "NAME=value" strings (static text).
ArrayView<const StringView> default_environment(SyscallAbi abi);

}  // namespace process
