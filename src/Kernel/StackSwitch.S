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

/* uint64_t tcos_call_on_stack(uint64_t (*function)(void*, void*), void* a, void* b, uint64_t stack_top)
 *
 * Calls function(a, b) with the stack pointer at `stack_top` (16-byte aligned, the end of memory the caller owns) and returns what it returns, back
 * on the caller's stack. Microsoft x64 ABI: rcx = function, rdx = a, r8 = b, r9 = stack_top. The entry point uses it to leave the firmware's small
 * boot stack (see efi_main). */
.intel_syntax noprefix
.text

.global tcos_call_on_stack
.balign 16
tcos_call_on_stack:
    push rbp
    mov rbp, rsp                     /* the caller's stack, kept in a register the callee saves */
    mov rsp, r9
    mov rax, rcx
    mov rcx, rdx
    mov rdx, r8
    sub rsp, 32                      /* the callee's shadow space; rsp stays 16-byte aligned at the call */
    call rax
    mov rsp, rbp
    pop rbp
    ret
