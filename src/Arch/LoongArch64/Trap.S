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

/* The exception entry: every interrupt and exception taken at PLV0 lands here (CSR.EENTRY, vector spacing 0, so the
 * address must be page aligned). It saves the integer registers in a kernel::Frame on the interrupted stack (see
 * Kernel/Interrupts.hpp: r1..r31 at 0..240, era 248, prmd 256, estat 264, badv 272; 288 bytes), calls
 * tcos_trap_dispatch(frame), and resumes the frame it returns - the same one, or another thread's. The hardware has
 * already saved the interrupted mode in PRMD and the program counter in ERA and has masked interrupts. */
.text
.balign 4096
.global tcos_trap_entry
tcos_trap_entry:
    addi.d  $sp, $sp, -288
    st.d    $r1, $sp, 0
    st.d    $r2, $sp, 8
    st.d    $r4, $sp, 24
    st.d    $r5, $sp, 32
    st.d    $r6, $sp, 40
    st.d    $r7, $sp, 48
    st.d    $r8, $sp, 56
    st.d    $r9, $sp, 64
    st.d    $r10, $sp, 72
    st.d    $r11, $sp, 80
    st.d    $r12, $sp, 88
    st.d    $r13, $sp, 96
    st.d    $r14, $sp, 104
    st.d    $r15, $sp, 112
    st.d    $r16, $sp, 120
    st.d    $r17, $sp, 128
    st.d    $r18, $sp, 136
    st.d    $r19, $sp, 144
    st.d    $r20, $sp, 152
    st.d    $r21, $sp, 160
    st.d    $r22, $sp, 168
    st.d    $r23, $sp, 176
    st.d    $r24, $sp, 184
    st.d    $r25, $sp, 192
    st.d    $r26, $sp, 200
    st.d    $r27, $sp, 208
    st.d    $r28, $sp, 216
    st.d    $r29, $sp, 224
    st.d    $r30, $sp, 232
    st.d    $r31, $sp, 240
    addi.d  $t0, $sp, 288
    st.d    $t0, $sp, 16
    csrrd   $t0, 0x6               /* ERA */
    st.d    $t0, $sp, 248
    csrrd   $t0, 0x1               /* PRMD */
    st.d    $t0, $sp, 256
    csrrd   $t0, 0x5               /* ESTAT */
    st.d    $t0, $sp, 264
    csrrd   $t0, 0x7               /* BADV */
    st.d    $t0, $sp, 272
    move    $a0, $sp
    bl      tcos_trap_dispatch
    move    $sp, $a0
    bl      tcos_finish_switch      /* the old thread is left: it becomes runnable for other cores only now (finish_switch in ThreadScheduler.cpp); the call runs below the frame of the thread that now runs */
    ld.d    $t0, $sp, 248
    csrwr   $t0, 0x6
    ld.d    $t0, $sp, 256
    csrwr   $t0, 0x1
    ld.d    $r1, $sp, 0
    ld.d    $r2, $sp, 8
    ld.d    $r4, $sp, 24
    ld.d    $r5, $sp, 32
    ld.d    $r6, $sp, 40
    ld.d    $r7, $sp, 48
    ld.d    $r8, $sp, 56
    ld.d    $r9, $sp, 64
    ld.d    $r10, $sp, 72
    ld.d    $r11, $sp, 80
    ld.d    $r12, $sp, 88
    ld.d    $r13, $sp, 96
    ld.d    $r14, $sp, 104
    ld.d    $r15, $sp, 112
    ld.d    $r16, $sp, 120
    ld.d    $r17, $sp, 128
    ld.d    $r18, $sp, 136
    ld.d    $r19, $sp, 144
    ld.d    $r20, $sp, 152
    ld.d    $r21, $sp, 160
    ld.d    $r22, $sp, 168
    ld.d    $r23, $sp, 176
    ld.d    $r24, $sp, 184
    ld.d    $r25, $sp, 192
    ld.d    $r26, $sp, 200
    ld.d    $r27, $sp, 208
    ld.d    $r28, $sp, 216
    ld.d    $r29, $sp, 224
    ld.d    $r30, $sp, 232
    ld.d    $r31, $sp, 240
    addi.d  $sp, $sp, 288
    ertn
