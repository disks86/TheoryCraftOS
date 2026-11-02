# Privileged-architecture test payload, part 3: Svpbmt, Svnapot and Svadu. See common.inc.
.include "common.inc"

.equ PTE_V, 1
.equ PTE_R, 2
.equ PTE_W, 4
.equ PTE_X, 8
.equ PTE_U, 16
.equ PTE_A, 0x40
.equ PTE_D, 0x80
.macro PTE dst, addr, flags
  la \dst, \addr
  srli \dst, \dst, 12
  slli \dst, \dst, 10
  ori \dst, \dst, \flags
.endm

run_tests:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'N'
  la t0, strap
  csrw stvec, t0
  la t0, strap_stack_top
  csrw sscratch, t0
  li t0, (1 << 2) | (1 << 12) | (1 << 13) | (1 << 15)
  csrw medeleg, t0
  call build_tables
  la t0, root
  srli t0, t0, 12
  li t1, 8
  slli t1, t1, 60
  or t0, t0, t1
  la t1, satp_value
  sd t0, 0(t1)
  # the type bits and the N bit with Svpbmt on (the reset state)
  csrr t1, 0x30A
  PRINT t1
  ENTER s_types, 1
  # Svpbmt off: any non-zero type is reserved
  li t0, 1 << 62
  csrc 0x30A, t0
  ENTER s_types_off, 1
  # Svadu: the hart sets A and D
  li t0, 1 << 61
  csrs 0x30A, t0
  csrr t1, 0x30A
  PRINT t1
  ENTER s_adu, 1
  la t1, l0
  ld t2, 24(t1)
  PRINT t2
  ld t2, 32(t1)
  PRINT t2
  ld t2, 40(t1)
  PRINT t2
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

build_tables:
  la t1, root
  li t0, 0xC7
  sd t0, 0(t1)
  PTE t0, l1, PTE_V
  sd t0, 8(t1)
  li t0, 0x200000CF
  sd t0, 16(t1)
  PTE t0, l1b, PTE_V
  sd t0, 24(t1)
  la t1, l1
  PTE t0, l0, PTE_V
  sd t0, 0(t1)
  la t1, l1b
  PTE t0, l0, PTE_V
  li t2, 1
  slli t2, t2, 61
  or t0, t0, t2                   # a pointer with a type bit
  sd t0, 0(t1)
  la t1, l0
  PTE t0, data_area, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D
  li t2, 1
  slli t2, t2, 61
  or t3, t0, t2
  sd t3, 0(t1)                    # 0: PBMT = 1 (NC)
  li t2, 3
  slli t2, t2, 61
  or t3, t0, t2
  sd t3, 8(t1)                    # 1: PBMT = 3 (reserved)
  li t2, 1
  slli t2, t2, 54
  or t3, t0, t2
  sd t3, 16(t1)                   # 2: a reserved bit
  PTE t0, data_area, PTE_V | PTE_R | PTE_W
  sd t0, 24(t1)                   # 3: A and D clear
  PTE t0, data_area, PTE_V | PTE_R | PTE_W | PTE_A
  sd t0, 32(t1)                   # 4: D clear
  PTE t0, data_area, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D
  sd t0, 40(t1)                   # 5: complete (control)
  # 16..31: a 64 KiB NAPOT region (the PPN ends in 0b1000), 32..: one with the wrong low PPN bits
  la t2, napot_area
  srli t2, t2, 12
  addi t2, t2, 8
  slli t2, t2, 10
  ori t2, t2, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D
  li t3, 1
  slli t3, t3, 63
  or t2, t2, t3
  li t4, 16
1:
  slli t5, t4, 3
  add t5, t5, t1
  sd t2, 0(t5)
  addi t4, t4, 1
  li t5, 32
  bltu t4, t5, 1b
  la t2, napot_area
  srli t2, t2, 12
  slli t2, t2, 10
  ori t2, t2, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D
  or t2, t2, t3
  sd t2, 256(t1)                  # 32: N set, PPN low bits 0
  ret

s_types:
  la t1, satp_value
  ld t0, 0(t1)
  csrw satp, t0
  sfence.vma
  li t5, 0x40000000
  ld t2, 0(t5)                    # PBMT = 1: fine
  sd t2, 0(t5)
  li t5, 0x40001000
  ld t2, 0(t5)                    # PBMT = 3: page fault
  li t5, 0x40002000
  ld t2, 0(t5)                    # reserved bit: page fault
  li t5, 0x40005000
  ld t2, 0(t5)                    # control: fine
  li t5, 0xC0000000
  ld t2, 0(t5)                    # a pointer with a type bit: page fault
  # NAPOT: the 16 pages map one 64 KiB region; a store through one page is visible through the others
  li t5, 0x40010000
  li t2, 0x1234
  sd t2, 0(t5)
  li t5, 0x4001F000
  ld t3, 0(t5)                    # a different page of the region: another word
  li t5, 0x4001F008
  sd t2, 0(t5)
  li t5, 0x40010000
  ld t3, 0(t5)
  li t5, 0x40020000
  ld t2, 0(t5)                    # N with the wrong PPN bits: page fault
  ecall

s_types_off:
  li t5, 0x40000000
  ld t2, 0(t5)                    # PBMT non-zero with Svpbmt off: page fault
  li t5, 0x40005000
  ld t2, 0(t5)
  ecall

s_adu:
  li t5, 0x40003000
  ld t2, 0(t5)                    # A clear: the hart sets it
  sd t2, 0(t5)                    # and D
  li t5, 0x40004000
  ld t2, 0(t5)                    # D clear, load: nothing to set
  sd t2, 0(t5)                    # the store sets D
  ecall

.section .data
.balign 4096
root: .space 4096
l1: .space 4096
l1b: .space 4096
l0: .space 4096
data_area: .space 4096
.balign 65536
napot_area: .space 65536
