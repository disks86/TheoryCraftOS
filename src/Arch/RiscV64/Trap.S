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

/* The supervisor trap entry: every interrupt and exception taken in supervisor mode lands here (stvec, direct mode).
 * It saves the integer registers in a kernel::Frame on the interrupted stack (see Kernel/Interrupts.hpp: x1..x31 at
 * 0..240, sepc 248, sstatus 256, scause 264, stval 272; 288 bytes), calls tcos_trap_dispatch(frame), and resumes the
 * frame it returns - the same one, or another thread's. */
.option norelax
.text
.balign 4
.global tcos_trap_entry
tcos_trap_entry:
    addi sp, sp, -288
    sd   x1,    0(sp)
    sd   x3,   16(sp)
    sd   x4,   24(sp)
    sd   x5,   32(sp)
    sd   x6,   40(sp)
    sd   x7,   48(sp)
    sd   x8,   56(sp)
    sd   x9,   64(sp)
    sd   x10,  72(sp)
    sd   x11,  80(sp)
    sd   x12,  88(sp)
    sd   x13,  96(sp)
    sd   x14, 104(sp)
    sd   x15, 112(sp)
    sd   x16, 120(sp)
    sd   x17, 128(sp)
    sd   x18, 136(sp)
    sd   x19, 144(sp)
    sd   x20, 152(sp)
    sd   x21, 160(sp)
    sd   x22, 168(sp)
    sd   x23, 176(sp)
    sd   x24, 184(sp)
    sd   x25, 192(sp)
    sd   x26, 200(sp)
    sd   x27, 208(sp)
    sd   x28, 216(sp)
    sd   x29, 224(sp)
    sd   x30, 232(sp)
    sd   x31, 240(sp)
    addi t0, sp, 288
    sd   t0, 8(sp)
    csrr t0, sepc
    sd   t0, 248(sp)
    csrr t0, sstatus
    sd   t0, 256(sp)
    csrr t0, scause
    sd   t0, 264(sp)
    csrr t0, stval
    sd   t0, 272(sp)
    mv   a0, sp
    call tcos_trap_dispatch
    mv   sp, a0
    ld   t0, 248(sp)
    csrw sepc, t0
    ld   t0, 256(sp)
    csrw sstatus, t0
    ld   x1,    0(sp)
    ld   x3,   16(sp)
    ld   x4,   24(sp)
    ld   x5,   32(sp)
    ld   x6,   40(sp)
    ld   x7,   48(sp)
    ld   x8,   56(sp)
    ld   x9,   64(sp)
    ld   x10,  72(sp)
    ld   x11,  80(sp)
    ld   x12,  88(sp)
    ld   x13,  96(sp)
    ld   x14, 104(sp)
    ld   x15, 112(sp)
    ld   x16, 120(sp)
    ld   x17, 128(sp)
    ld   x18, 136(sp)
    ld   x19, 144(sp)
    ld   x20, 152(sp)
    ld   x21, 160(sp)
    ld   x22, 168(sp)
    ld   x23, 176(sp)
    ld   x24, 184(sp)
    ld   x25, 192(sp)
    ld   x26, 200(sp)
    ld   x27, 208(sp)
    ld   x28, 216(sp)
    ld   x29, 224(sp)
    ld   x30, 232(sp)
    ld   x31, 240(sp)
    addi sp, sp, 288
    sret
