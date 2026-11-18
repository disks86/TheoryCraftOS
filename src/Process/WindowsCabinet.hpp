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
// NtTcosCabinet(PULONG64 arguments): this OS's own service (511), behind cabinet.dll's FDI (FDICopy, FDIIsCabinet). The DLL reads a cabinet into the
// program's memory through its caller's file functions; the kernel decodes it with the cabinet reader of src/Media (stored, MSZIP and LZX folders), so
// there is one decompressor in the system. The work is media::windows_cabinet_service (src/Media/CabinetService.hpp), installed at start-up with
// set_cabinet_handler so that the process layer does not depend on it. The arguments are 8 ULONG64 in the program's memory - one layout for 32- and
// 64-bit programs:
//
//   [0] operation  [1] the cabinet (all of it)  [2] its length (at most 512 MiB)
//
//   1 DESCRIBE  [3] a buffer for the records, [4] its size in bytes; [5] = the number of files, [6] = the size of a record (280). One record a file,
//               in the order the cabinet lists them:
//                 +0 ULONG64 size  +8 ULONG name length  +12 USHORT date  +14 USHORT time (MS-DOS)  +16 USHORT attributes  +18 USHORT folder
//                 +20 ULONG flags (1: the name is UTF-8, 2: the name is longer than what is here)  +24 the name as stored, at most 256 bytes
//               STATUS_BUFFER_TOO_SMALL (with [5] and [6] written) when the buffer cannot hold them all.
//   2 EXTRACT   [3] which file (its record's index), [4] a buffer, [5] its size; [6] = the file's size, and its bytes are in the buffer.
//               STATUS_BUFFER_TOO_SMALL (with [6] written) when the buffer is smaller than the file.
// STATUS_INVALID_PARAMETER for a length, index or operation that cannot be, STATUS_ACCESS_VIOLATION for memory the program does not have,
// STATUS_NO_MEMORY, STATUS_FILE_CORRUPT_ERROR for what is not a cabinet this OS reads (damaged, cut short, one of a set, Quantum) or a file in it that
// does not decode, STATUS_NOT_SUPPORTED without the handler.
#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

namespace process::windowsabi {

using CabinetHandler = uint32_t (*)(Process& p, const SyscallRequest& r);
void set_cabinet_handler(CabinetHandler handler);
uint32_t nt_tcos_cabinet(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
