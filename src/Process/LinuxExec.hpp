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
#include "Process/Process.hpp"

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

}  // namespace process::linuxabi
