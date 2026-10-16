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

/* x86 port I/O for IoPort. Microsoft x64 ABI: args in rcx, rdx; result in rax. */
.intel_syntax noprefix
.text

.global tcos_outb
tcos_outb:
    mov     al, dl
    mov     dx, cx
    out     dx, al
    ret

.global tcos_inb
tcos_inb:
    mov     dx, cx
    xor     eax, eax
    in      al, dx
    ret

.global tcos_outw
tcos_outw:
    mov     ax, dx
    mov     dx, cx
    out     dx, ax
    ret

.global tcos_inw
tcos_inw:
    mov     dx, cx
    xor     eax, eax
    in      ax, dx
    ret

.global tcos_outl
tcos_outl:
    mov     eax, edx
    mov     dx, cx
    out     dx, eax
    ret

.global tcos_inl
tcos_inl:
    mov     dx, cx
    in      eax, dx
    ret
