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
#include "Process/Syscall.hpp"

namespace process::windowsabi {

// NtLockFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, PLARGE_INTEGER ByteOffset, PLARGE_INTEGER Length, ULONG Key, BOOLEAN FailImmediately, BOOLEAN ExclusiveLock)
// and NtUnlockFile(FileHandle, IoStatusBlock, ByteOffset, Length, Key): advisory locks on byte ranges of a file, kept for the whole system, so that programs agree on who may
// use a part of a file (LockFileEx). A lock that cannot be had waits, or fails with STATUS_LOCK_NOT_GRANTED; a lock goes with the open file it was made on.
uint32_t nt_lock_file(Process& p, const SyscallRequest& r);
uint32_t nt_unlock_file(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
