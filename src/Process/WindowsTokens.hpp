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
#include "Process/AccessCheck.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Utility/ByteBuffer.hpp"

namespace process::windowsabi {

// Access tokens and the security descriptors of objects. A token is an object of the object manager that holds one blob (src/Windows/tcos_token.h); every process has a token
// (the first use makes it, from the default token of the system; a new process starts with a copy of its parent's) and a thread can have an impersonation token. ntdll.dll does
// the work of the token services over NtTcosToken(operation, ...): this file keeps the blobs, the handles and the descriptors that objects carry, and decides access.
uint32_t nt_tcos_token(Process& p, const SyscallRequest& r);
// NtTcosAccessCheck(PVOID arguments): the access check of NtAccessCheck over a descriptor in the program's memory and a token handle.
uint32_t nt_tcos_access_check(Process& p, const SyscallRequest& r);

// The blob of the token that decides the access of the calling thread (its impersonation token, else the process token) into `out`. False if there is none.
bool caller_token_blob(Process& p, ByteBuffer& out);
// The token of the process `pid` into `out` (the default token if it has none yet).
bool process_token_blob(uint64_t pid, ByteBuffer& out);
// A new process (the child) starts with a copy of the token of its parent.
void token_inherit(uint64_t parent_pid, uint64_t child_pid);
// The process has ended.
void token_process_exited(uint64_t pid);

// May the calling thread have the rights `desired` to something that carries the self-relative descriptor `descriptor` (empty: it carries none and everybody may)? `mapping` maps
// the generic rights. On success *granted gets the rights. False with the NTSTATUS in *status if not.
bool caller_may(Process& p, ConstByteView descriptor, uint32_t desired, const security::GenericMapping& mapping, uint32_t* granted, uint32_t* status, bool backup_intent = false);

// The rights the calling thread gets when it opens the object with the descriptor kept under (tag, id) ('p' a process by its id, 'h' a thread by its id): the rights asked for (generic
// ones mapped, MAXIMUM_ALLOWED means all that the descriptor allows). STATUS_ACCESS_DENIED etc. in the return; 0 and *granted otherwise. An object with no descriptor allows all.
uint32_t open_with_descriptor(Process& p, char tag, uint64_t id, uint32_t desired, const security::GenericMapping& mapping, uint32_t* granted);
inline const security::GenericMapping kProcessMapping = {0x20410, 0x20BEA, 0x121001, 0x1FFFFF};
inline const security::GenericMapping kThreadMapping = {0x20048, 0x20037, 0x120000, 0x1FFFFF};

// Is the privilege (the low part of its LUID) held and enabled in the token of the calling thread?
bool caller_privilege_enabled(Process& p, uint32_t low);

// Does the calling thread impersonate a token?
bool caller_is_impersonating(Process& p);

// The generic mapping of files and directories.
inline const security::GenericMapping kFileMapping = {0x120089, 0x120116, 0x1200A0, 0x1F01FF};

}  // namespace process::windowsabi
