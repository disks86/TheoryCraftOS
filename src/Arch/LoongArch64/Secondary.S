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

/* Where a parked core lands after the boot core wrote its mailbox and sent it an IPI: PLV0, direct address
 * translation, interrupts off. Mailbox 1 holds the SecondaryContext the boot core filled in (see Cpu.hpp; offsets are
 * used here). The core takes the boot core's CRMD (the memory access types), switches to its own stack and calls
 * tcos_secondary_main(context). The IPI that woke it was already acknowledged by the parking code. */
.equ CTX_CRMD,  0
.equ CTX_STACK, 8

.text
.balign 4
.global tcos_secondary_entry
tcos_secondary_entry:
    li.w    $t0, 0x1028           /* IOCSR mailbox 1 */
    iocsrrd.d $a0, $t0
    ld.d    $t0, $a0, CTX_CRMD
    csrwr   $t0, 0x0              /* CRMD */
    ld.d    $sp, $a0, CTX_STACK
    move    $fp, $zero
    bl      tcos_secondary_main
1:  idle    0
    b       1b
