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

/* Where a hart lands after SBI hart_start: supervisor mode, interrupts off, a0 = the hart id, a1 = the
 * SecondaryContext the boot hart filled in (see Cpu.hpp for the layout; offsets are used here).
 * The hart keeps its id in tp, copies the boot hart's address translation (satp; memory is mapped 1:1, so the program
 * counter stays valid), switches to its own stack and calls tcos_secondary_main(context). */
.equ CTX_SATP,  0
.equ CTX_STACK, 8

.option norelax
.text
.balign 4
.global tcos_secondary_entry
tcos_secondary_entry:
    csrw sie, zero
    mv   tp, a0
    ld   t0, CTX_SATP(a1)
    beqz t0, 1f
    csrw satp, t0
    sfence.vma
1:  ld   sp, CTX_STACK(a1)
    mv   fp, zero
    mv   a0, a1
    call tcos_secondary_main
2:  wfi
    j    2b
