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

/* The user-mode entry trampoline. While the CPU runs a user program it is on that program's page tables, which map almost nothing of the kernel. These
 * stubs are what an interrupt or SYSCALL from ring 3 reaches first: they live in memory that UserMode.cpp copies to a page-aligned place and maps on
 * every user page table (and the IDT points into that copy), so everything here is position independent, refers to the core's page through GS (the
 * UserEntry structure 64 bytes in), and keeps its few constants at the end of the block.
 *
 * Entry from ring 3 (the CPU switched to the entry stack in the TSS, or the SYSCALL stub did):
 *   swapgs; switch to the kernel page tables; move the 7-quad interrupt frame to the thread's kernel stack; continue in the kernel's isr_common.
 * Entry from ring 0 changes nothing: it is the old vector stub, followed by a jump to isr_common.
 * The way back is tcos_to_user (Interrupts.S) and tramp_exit below. */
.intel_syntax noprefix
.altmacro
.text

.set UE_KERNEL_CR3, 64
.set UE_KERNEL_STACK, 80
.set UE_SCRATCH, 88
.set UE_ENTRY_STACK_TOP, 96

.global tcos_tramp_begin
.balign 16
tcos_tramp_begin:

.macro TRAMP_STUB n
.balign 16
tramp_stub_\n:
.if (\n == 8) || (\n == 10) || (\n == 11) || (\n == 12) || (\n == 13) || (\n == 14) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
    /* the CPU pushed an error code */
.else
    push 0
.endif
    push \n
    jmp tramp_common
.endm

.set i, 0
.rept 256
    TRAMP_STUB %i
    .set i, i + 1
.endr

.balign 16
tramp_common:
    test byte ptr [rsp + 24], 3      /* the saved CS: from ring 3? */
    jz 1f
    swapgs
tramp_user:
    push rax
    push rcx
    push rdx
    mov rax, qword ptr gs:[UE_KERNEL_CR3]
    mov cr3, rax
    mov rax, qword ptr gs:[UE_KERNEL_STACK]
    sub rax, 56
    lea rcx, [rsp + 24]
    mov rdx, [rcx]
    mov [rax], rdx
    mov rdx, [rcx + 8]
    mov [rax + 8], rdx
    mov rdx, [rcx + 16]
    mov [rax + 16], rdx
    mov rdx, [rcx + 24]
    mov [rax + 24], rdx
    mov rdx, [rcx + 32]
    mov [rax + 32], rdx
    mov rdx, [rcx + 40]
    mov [rax + 40], rdx
    mov rdx, [rcx + 48]
    mov [rax + 48], rdx
    mov rdx, [rsp + 16]
    mov [rax - 8], rdx
    mov rdx, [rsp + 8]
    mov [rax - 16], rdx
    mov rdx, [rsp]
    mov [rax - 24], rdx
    lea rsp, [rax - 24]
    pop rdx
    pop rcx
    pop rax
1:  jmp qword ptr [rip + tcos_tramp_isr_slot]

/* SYSCALL: rcx = user rip, r11 = user rflags, rsp = user stack, interrupts off (SFMASK), still on the user page tables. Builds the same frame an
 * interrupt from ring 3 would have and joins the path above. */
.global tcos_tramp_syscall
.balign 16
tcos_tramp_syscall:
    swapgs
    mov qword ptr gs:[UE_SCRATCH], rsp
    mov rsp, qword ptr gs:[UE_ENTRY_STACK_TOP]
    push 0x1B                        /* ss: user data */
    push qword ptr gs:[UE_SCRATCH]   /* rsp */
    push r11                         /* rflags */
    push 0x23                        /* cs: user code */
    push rcx                         /* rip */
    push 0                           /* error code */
    push 0x80                        /* vector: kVectorSyscall */
    jmp tramp_user

/* The last step to user mode (jumped to from tcos_to_user): rcx = the user page tables, the stack holds rdx, rcx, rax and the iret frame. */
.global tcos_tramp_exit
.balign 16
tcos_tramp_exit:
    mov cr3, rcx
    pop rdx
    pop rcx
    pop rax
    swapgs
    iretq

.global tcos_tramp_isr_slot
.balign 8
tcos_tramp_isr_slot:
    .quad 0                          /* filled in with the address of isr_common */
.global tcos_tramp_end
tcos_tramp_end:
