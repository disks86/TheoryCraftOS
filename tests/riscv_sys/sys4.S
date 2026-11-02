# Privileged-architecture test payload, part 4: the PLIC and the UART's interrupt, taken by machine mode and by supervisor mode. See common.inc.
.include "common.inc"

.equ PLIC, 0xC000000
.equ PRIORITY, 0xC000000
.equ PENDING, 0xC001000
.equ ENABLE0, 0xC002000
.equ ENABLE1, 0xC002080
.equ THRESHOLD0, 0xC200000
.equ CLAIM0, 0xC200004
.equ THRESHOLD1, 0xC201000
.equ CLAIM1, 0xC201004
.equ UART_IER, 0x10000001
.equ UART_IIR, 0x10000002

.macro PLIC_READ reg, addr
  li t0, \addr
  lw \reg, 0(t0)
.endm
.macro PLIC_WRITE addr, value
  li t0, \addr
  li t1, \value
  sw t1, 0(t0)
.endm

run_tests:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'L'
  # register behaviour
  PLIC_WRITE PRIORITY + 4, -1
  PLIC_READ t2, PRIORITY + 4
  PRINT t2
  PLIC_WRITE PRIORITY + 40, 5
  PLIC_READ t2, PRIORITY + 40
  PRINT t2
  PLIC_WRITE PRIORITY, 7
  PLIC_READ t2, PRIORITY
  PRINT t2
  PLIC_WRITE ENABLE0, -1
  PLIC_READ t2, ENABLE0
  PRINT t2
  PLIC_WRITE ENABLE0 + 8, -1
  PLIC_READ t2, ENABLE0 + 8
  PRINT t2
  PLIC_WRITE ENABLE1, -1
  PLIC_READ t2, ENABLE1
  PRINT t2
  PLIC_WRITE THRESHOLD0, -1
  PLIC_READ t2, THRESHOLD0
  PRINT t2
  PLIC_WRITE THRESHOLD1, 3
  PLIC_READ t2, THRESHOLD1
  PRINT t2
  PLIC_READ t2, CLAIM0                 # nothing pending: 0
  PRINT t2
  PLIC_READ t2, PENDING
  PRINT t2
  PLIC_WRITE PRIORITY + 4, 0
  PLIC_WRITE PRIORITY + 40, 0
  PLIC_WRITE PRIORITY, 0
  PLIC_WRITE ENABLE0, 0
  PLIC_WRITE ENABLE0 + 8, 0
  PLIC_WRITE ENABLE1, 0
  PLIC_WRITE THRESHOLD0, 0
  PLIC_WRITE THRESHOLD1, 0
  # the UART's transmit-empty interrupt, source 10
  PLIC_WRITE PRIORITY + 40, 1
  PLIC_WRITE ENABLE0, 0x400
  li t0, UART_IER
  li t1, 2
  sb t1, 0(t0)
  PLIC_READ t2, PENDING
  PRINT t2
  csrr t2, mip
  srli t2, t2, 11
  andi t2, t2, 1
  PRINT t2
  li t0, UART_IIR
  lbu t2, 0(t0)
  PRINT t2
  li t0, UART_IIR
  lbu t2, 0(t0)
  PRINT t2
  li t0, UART_IER
  sb zero, 0(t0)
  PLIC_READ t2, PENDING
  PRINT t2
  csrr t2, mip
  srli t2, t2, 11
  andi t2, t2, 1
  PRINT t2
  # machine external interrupt through the PLIC: threshold above the priority blocks it, equal priority does not pass either
  la t0, irq_count
  sd zero, 0(t0)
  PLIC_WRITE THRESHOLD0, 1
  li t0, UART_IER
  li t1, 2
  sb t1, 0(t0)
  csrr t2, mip
  srli t2, t2, 11
  andi t2, t2, 1
  PRINT t2
  PLIC_WRITE THRESHOLD0, 0
  csrr t2, mip
  srli t2, t2, 11
  andi t2, t2, 1
  PRINT t2
  li t0, 0x800
  csrs mie, t0
  li t0, 8
  csrs mstatus, t0
  WAIT_IRQ 1
  li t0, 8
  csrc mstatus, t0
  PLIC_READ t2, PENDING
  PRINT t2
  # the same source for supervisor mode: context 1, delegated nothing (SEI is taken in S through mideleg)
  PLIC_WRITE ENABLE0, 0
  PLIC_WRITE ENABLE1, 0x400
  la t0, strap
  csrw stvec, t0
  la t0, strap_stack_top
  csrw sscratch, t0
  li t0, 0x222
  csrw mideleg, t0
  csrw mie, t0
  la t0, irq_count
  sd zero, 0(t0)
  li t0, UART_IER
  li t1, 2
  sb t1, 0(t0)
  csrr t2, mip
  andi t2, t2, 0x200
  PRINT t2
  li t0, 2
  csrs mstatus, t0
  ENTER s_wait, 1
  csrw mideleg, zero
  csrw mie, zero
  li t0, 2
  csrc mstatus, t0
  PLIC_WRITE PRIORITY + 40, 0
  PLIC_WRITE ENABLE1, 0
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

s_wait:
  la t0, irq_count
1:
  ld t1, 0(t0)
  beqz t1, 1b
  ecall
