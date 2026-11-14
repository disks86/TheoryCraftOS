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
#include "FileSystem/IFileSystem.hpp"
#include "ObjectContainer/List.hpp"
#include "Process/Process.hpp"
#include "Utility/String.hpp"

namespace process::linuxabi {

// execve and execveat: the program becomes another one.
//
// Linux replaces the process's image in place. Here the new program is started as a child process (IProcessRuntime::spawn_process) with exactly the
// arguments and environment given, the current folder, the same file tree, and every descriptor that is not close-on-exec at the same number; the old
// program then closes its descriptors, stops its other threads and waits for the new one, and ends with its exit code. Seen from outside, the program
// runs on under a new process id; seen from the new program, it was execve'd (its /proc/self/exe is the new file).
//
// A script that starts with "#!" runs its interpreter (with the optional argument of that line, then the script's path, then the arguments after argv[0]),
// four levels deep at most. Errors come back before anything has changed, as on Linux: ENOENT, EACCES (a folder), ENOEXEC (not a Linux program for this
// machine), E2BIG, ENOSYS where the runtime cannot start processes.
uint64_t sys_execve(Process& p, uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags);

// --- The built-in shell. The Linux tree's /bin/sh and /bin/bash (made by prepare_program_folders when nothing is there) are not programs but a
// marker: a file whose first line is kBuiltinShellLine. execve of one (directly, or as the "#!" interpreter of a script) runs the script with the
// OS's own shell (src/Program/AppRunScript.hpp: enough sh and bash for launcher scripts, no fork) and then execs what the script finally starts.
// "sh [options] script [arguments]" is understood; options are ignored, "-c" and a shell without a script are not (ENOEXEC).
// A real shell (a program named sh, bash, dash or ash, such as the dash sharun AppImages bundle) asked to run a script file is given the same treatment:
// without fork it could not get past its first $( ). It still runs as itself for -c and interactive use.
constexpr const char* kBuiltinShellLine = "#!tcos-builtin-shell";

// Runs `script` (a path in the Linux world, whose files are `files` below `world_root`) with `arguments` after $0 and `environment`. 1: it starts
// *program (a path in the Linux world) with *program_arguments (after argv[0]) and *program_environment; 0: it ended without starting a program;
// -1: out of memory.
using BuiltinShell = int (*)(allocators::IAllocator& allocator, filesystem::IFileSystem& files, StringView world_root, StringView script,
                             ArrayView<const StringView> arguments, ArrayView<const StringView> environment, String* program, List<String>* program_arguments,
                             List<String>* program_environment);
// Set once at boot (src/Program installs the AppRun interpreter); without one, the marker files are refused with ENOEXEC.
void install_builtin_shell(BuiltinShell shell);

}  // namespace process::linuxabi
