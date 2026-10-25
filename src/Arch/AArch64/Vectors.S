/*
 * TheoryCraftOS - a minimal UEFI operating system.
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

/* Exception vector tables for AArch64. Two copies of the same code: one for firmware that runs us at EL1
 * (VBAR_EL1, ELR_EL1, ...) and one for EL2 (many real machines). Every entry saves the general registers as a
 * kernel::Frame (see Kernel/Interrupts.hpp for the layout and offsets), calls
 * tcos_exception_dispatch(Frame*) and resumes whichever Frame it returns. */

.macro SAVE_AND_CALL el, idx
    sub  sp, sp, #304
    stp  x0, x1, [sp, #0]
    mov  x0, #\idx
    b    tcos_exception_common_el\el
.endm

.macro VECTOR_ENTRY el, idx
    .balign 128
    SAVE_AND_CALL \el, \idx
.endm

.macro COMMON el
tcos_exception_common_el\el:
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]
    str  x30, [sp, #240]
    add  x2, sp, #304
    str  x2, [sp, #248]
    mrs  x2, elr_el\el
    mrs  x3, spsr_el\el
    stp  x2, x3, [sp, #256]
    str  x0, [sp, #272]
    mrs  x2, esr_el\el
    mrs  x3, far_el\el
    stp  x2, x3, [sp, #280]
    mov  x0, sp
    bl   tcos_exception_dispatch
    mov  sp, x0
    ldp  x2, x3, [sp, #256]
    msr  elr_el\el, x2
    msr  spsr_el\el, x3
    ldp  x2,  x3,  [sp, #16]
    ldp  x4,  x5,  [sp, #32]
    ldp  x6,  x7,  [sp, #48]
    ldp  x8,  x9,  [sp, #64]
    ldp  x10, x11, [sp, #80]
    ldp  x12, x13, [sp, #96]
    ldp  x14, x15, [sp, #112]
    ldp  x16, x17, [sp, #128]
    ldp  x18, x19, [sp, #144]
    ldp  x20, x21, [sp, #160]
    ldp  x22, x23, [sp, #176]
    ldp  x24, x25, [sp, #192]
    ldp  x26, x27, [sp, #208]
    ldp  x28, x29, [sp, #224]
    ldr  x30, [sp, #240]
    ldp  x0,  x1,  [sp, #0]
    add  sp, sp, #304
    eret
.endm

.macro TABLE el
    .balign 2048
    .global tcos_vectors_el\el
tcos_vectors_el\el:
    VECTOR_ENTRY \el, 0     /* current EL, SP0: sync / irq / fiq / serror */
    VECTOR_ENTRY \el, 1
    VECTOR_ENTRY \el, 2
    VECTOR_ENTRY \el, 3
    VECTOR_ENTRY \el, 4     /* current EL, SPx */
    VECTOR_ENTRY \el, 5
    VECTOR_ENTRY \el, 6
    VECTOR_ENTRY \el, 7
    VECTOR_ENTRY \el, 8     /* lower EL, AArch64 */
    VECTOR_ENTRY \el, 9
    VECTOR_ENTRY \el, 10
    VECTOR_ENTRY \el, 11
    VECTOR_ENTRY \el, 12    /* lower EL, AArch32 */
    VECTOR_ENTRY \el, 13
    VECTOR_ENTRY \el, 14
    VECTOR_ENTRY \el, 15
    COMMON \el
.endm

.text
TABLE 1
TABLE 2
