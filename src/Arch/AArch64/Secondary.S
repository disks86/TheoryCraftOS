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

/* Where a secondary core lands after PSCI CPU_ON: MMU and caches off, interrupts masked, x0 = the
 * SecondaryContext the boot core filled in (see Cpu.hpp for the layout; offsets are used here).
 * The core copies the boot core's translation setup, turns the MMU on (memory is mapped 1:1, so the
 * program counter stays valid), switches to its own stack and calls tcos_secondary_main(context). */
.equ CTX_TTBR0,  0
.equ CTX_TCR,    8
.equ CTX_MAIR,   16
.equ CTX_SCTLR,  24
.equ CTX_HCR,    32
.equ CTX_STACK,  40

.text
.balign 16
.global tcos_secondary_entry
tcos_secondary_entry:
    mov  x19, x0
    msr  daifset, #0xF
    mrs  x1, CurrentEL
    lsr  x1, x1, #2
    cmp  x1, #2
    b.eq 2f

1:  /* EL1 */
    ldr  x1, [x19, #CTX_TTBR0]
    msr  ttbr0_el1, x1
    ldr  x1, [x19, #CTX_TCR]
    msr  tcr_el1, x1
    ldr  x1, [x19, #CTX_MAIR]
    msr  mair_el1, x1
    isb
    ic   iallu
    tlbi vmalle1
    dsb  nsh
    isb
    ldr  x1, [x19, #CTX_SCTLR]
    msr  sctlr_el1, x1
    isb
    b    3f

2:  /* EL2 */
    ldr  x1, [x19, #CTX_HCR]
    msr  hcr_el2, x1
    ldr  x1, [x19, #CTX_TTBR0]
    msr  ttbr0_el2, x1
    ldr  x1, [x19, #CTX_TCR]
    msr  tcr_el2, x1
    ldr  x1, [x19, #CTX_MAIR]
    msr  mair_el2, x1
    isb
    ic   iallu
    tlbi alle2
    dsb  nsh
    isb
    ldr  x1, [x19, #CTX_SCTLR]
    msr  sctlr_el2, x1
    isb

3:  ldr  x1, [x19, #CTX_STACK]
    mov  sp, x1
    mov  x29, xzr
    mov  x0, x19
    bl   tcos_secondary_main
4:  wfi
    b    4b
