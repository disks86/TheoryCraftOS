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

// The NT services behind LoadLibrary and FreeLibrary (WindowsSyscalls.cpp maps service numbers to these). Each returns an NTSTATUS.
namespace process::windowsabi {

// NtLoadModule(PUNICODE_STRING Path, PVOID* Base, PVOID Records, ULONG Capacity, PULONG Count): maps the DLL file at the Win32 path and the libraries it imports that are not
// loaded yet, and binds their imports. `Base` gets where the DLL is; `Records` (Capacity entries of 64 bytes: base, size and entry point as 64-bit numbers, then a 40-byte
// NUL-terminated canonical name) get the libraries that are new, in the order their entry points have to run; `Count` how many. The caller (ntdll) lists them in the PEB, runs
// their entry points and, on failure, calls NtUnloadModule for each.
uint32_t nt_load_module(Process& p, const SyscallRequest& r);
// NtReleaseModule(PVOID Base, PVOID Records, ULONG Capacity, PULONG Count): FreeLibrary's first half. Counts one use less; `Records` get the libraries that nobody uses any more,
// in the order their entry points have to be called with DLL_PROCESS_DETACH (they stay mapped). Succeeds with Count 0 for a library that is in use otherwise or loaded at
// start-up.
uint32_t nt_release_module(Process& p, const SyscallRequest& r);
// NtUnloadModule(PVOID Base): takes the library at Base out of the address space (its entry point has been called already).
uint32_t nt_unload_module(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
