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

/* x86_64 CPU instructions wrapped for X86_64Cpu (Microsoft x64 ABI). */
.intel_syntax noprefix
.text

.global tcos_halt
tcos_halt:
    hlt
    ret

.global tcos_pause
tcos_pause:
    pause
    ret

.global tcos_breakpoint
tcos_breakpoint:
    int3
    ret

.global tcos_disable_interrupts
tcos_disable_interrupts:
    cli
    ret

.global tcos_enable_interrupts
tcos_enable_interrupts:
    sti
    ret

.global tcos_rdtsc
tcos_rdtsc:
    rdtsc
    shl     rdx, 32
    or      rax, rdx
    ret
