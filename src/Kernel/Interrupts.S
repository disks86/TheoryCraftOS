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

/* GS-relative offsets in the core's page: the UserEntry structure sits 64 bytes in (see Kernel/UserMode.hpp). */
.set UE_KERNEL_CR3, 64
.set UE_USER_CR3, 72
.set UE_KERNEL_STACK, 80
.set UE_ENTRY_STACK_TOP, 96
.set UE_TRAMP_EXIT, 104

.global tcos_isr_common
tcos_isr_common:
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
isr_resume:
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
    test byte ptr [rsp + 8], 3      /* returning to ring 3? */
    jnz tcos_to_user
    iretq

/* Returns to user mode: the user page tables are not in force yet, and the kernel stack is not mapped on them, so the iret frame moves to the core's
 * entry stack (mapped on both), and the final steps run from the trampoline (UserEntry.S). rax, rcx and rdx are borrowed and put back there. */
tcos_to_user:
    push rax
    push rcx
    push rdx
    mov rcx, qword ptr gs:[UE_USER_CR3]
    test rcx, rcx
    jz 1f
    mov rax, qword ptr gs:[UE_ENTRY_STACK_TOP]
    sub rax, 40
    mov rdx, [rsp + 24]
    mov [rax], rdx
    mov rdx, [rsp + 32]
    mov [rax + 8], rdx
    mov rdx, [rsp + 40]
    mov [rax + 16], rdx
    mov rdx, [rsp + 48]
    mov [rax + 24], rdx
    mov rdx, [rsp + 56]
    mov [rax + 32], rdx
    mov rdx, [rsp + 16]
    mov [rax - 8], rdx
    mov rdx, [rsp + 8]
    mov [rax - 16], rdx
    mov rdx, [rsp]
    mov [rax - 24], rdx
    lea rsp, [rax - 24]
    jmp qword ptr gs:[UE_TRAMP_EXIT]
1:  ud2                             /* a user frame on a thread without user page tables */

/* void tcos_resume_frame(Frame*): continues as if the frame had been saved by an interrupt (the way a new user thread starts). */
.global tcos_resume_frame
tcos_resume_frame:
    mov rsp, rcx
    jmp isr_resume

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
