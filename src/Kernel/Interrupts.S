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

/* Interrupt entry stubs: one per vector, all funnelling into isr_common, which
 * saves the full register state (including x87/SSE) as a kernel::Frame, calls
 * tcos_interrupt_dispatch(Frame*) and resumes whichever Frame it returns.
 * Microsoft x64 ABI for the call into C++. */
.intel_syntax noprefix
.altmacro
.text

.macro ISR_STUB n
.align 16
isr_stub_\n:
.if (\n == 8) || (\n == 10) || (\n == 11) || (\n == 12) || (\n == 13) || (\n == 14) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
    /* the CPU pushed an error code */
.else
    push 0
.endif
    push \n
    jmp isr_common
.endm

.set i, 0
.rept 256
    ISR_STUB %i
    .set i, i + 1
.endr

isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    sub rsp, 512
    fxsave [rsp]
    mov rcx, rsp
    sub rsp, 32
    cld
    call tcos_interrupt_dispatch
    mov rsp, rax
    fxrstor [rsp]
    add rsp, 512
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

.macro ISR_ADDRESS n
    .quad isr_stub_\n
.endm

.section .rdata,"dr"
.align 8
.global tcos_isr_table
tcos_isr_table:
.set i, 0
.rept 256
    ISR_ADDRESS %i
    .set i, i + 1
.endr
