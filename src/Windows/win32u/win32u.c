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

/* win32u.dll: the system-call stubs of win32k (the NtUser* and NtGdi* services that user32.dll and gdi32.dll call), numbered from NT_SERVICE_WIN32K_BASE (see
 * src/Windows/tcos_syscalls.h). The kernel has no code for any of them yet, so each ends in STATUS_NOT_IMPLEMENTED. The stubs are generated: win32u/services.inc. */
#include "../tcos_syscalls.h"

#define NT_SERVICE(name)                                                                                                                                      \
  __asm__(".text\n.globl " #name "\n" #name ":\n mov %rcx, %r10\n mov $" _STR(NT_SERVICE_##name) ", %eax\n syscall\n ret\n.section .drectve\n.ascii \" -export:" #name "\"\n.text\n")
#define _STR(x) _STR2(x)
#define _STR2(x) #x

#include "services.inc"

int DllMain(void* module, unsigned reason, void* reserved) {
  (void)module;
  (void)reason;
  (void)reserved;
  return 1;
}
