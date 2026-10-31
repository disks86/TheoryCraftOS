# Shared by the privileged-architecture payloads: UART output, trap handlers, mode switching macros. Each payload includes this and defines run_tests.
# A payload runs in machine mode from RAM at 0x80000000 (qemu `virt`: -bios none -kernel x.elf), prints what it observes through the 16550 UART and ends through the test
# finisher. The same file runs on qemu-system-riscv64 and on RiscV64Machine and the two outputs must be identical.
.equ UART, 0x10000000
.equ FINISHER, 0x100000
.equ CLINT, 0x2000000
.equ MTIMECMP, 0x2004000
.equ MTIME, 0x200BFF8
.equ MSIP, 0x2000000

.section .text.init, "ax"
.globl _start
_start:
  la sp, stack_top
  la t0, mtrap
  csrw mtvec, t0
  la t0, mtrap_stack_top
  csrw mscratch, t0
  li t0, 0x2000
  csrs mstatus, t0                # FS = initial
  li t0, -1
  csrw pmpaddr15, t0
  li t0, 0x1F
  slli t0, t0, 56
  csrw pmpcfg2, t0                # entry 15: everything, so that S and U mode can run at all
  li t0, -1
  li t1, MTIMECMP
  sd t0, 0(t1)
  call run_tests
  li t0, FINISHER
  li t1, 0x5555
  sw t1, 0(t0)
1: j 1b

# ---- output ---------------------------------------------------------------------------------------------------------------------------------
puthex:                           # a0: value; clobbers t0-t3
  li t0, UART
  li t1, 60
1:
  srl t2, a0, t1
  andi t2, t2, 15
  li t3, 10
  blt t2, t3, 2f
  addi t2, t2, 'a' - 10
  j 3f
2:
  addi t2, t2, '0'
3:
  sb t2, 0(t0)
  addi t1, t1, -4
  bgez t1, 1b
  li t2, 10
  sb t2, 0(t0)
  ret

.macro PRINT reg
  mv a0, \reg
  call puthex
.endm
.macro TAG ch
  li a0, \ch
  li t0, UART
  sb a0, 0(t0)
.endm

# ---- the machine-mode trap handler: log, then resume ------------------------------------------------------------------------------------------
.balign 4
mtrap:
  csrrw sp, mscratch, sp
  addi sp, sp, -96
  sd ra, 0(sp)
  sd a0, 8(sp)
  sd a1, 16(sp)
  sd t0, 24(sp)
  sd t1, 32(sp)
  sd t2, 40(sp)
  sd t3, 48(sp)
  TAG 'M'
  csrr a0, mcause
  call puthex
  csrr a0, mtval
  call puthex
  csrr a0, mepc
  la t0, _start
  sub a0, a0, t0
  csrr a1, mcause
  bgez a1, 1f
  li a0, 0                         # an interrupt arrives at an arbitrary instruction: not logged
1:
  call puthex
  csrr a0, mstatus
  call puthex
  csrr a1, mcause
  bltz a1, 20f                     # an interrupt: silence the source, do not skip anything
  li t0, 8
  beq a1, t0, 5f
  li t0, 9
  bne a1, t0, 6f
5:
  la t0, resume_hook               # an ecall from a lower mode returns to the machine-mode code that entered it
  ld t1, 0(t0)
  beqz t1, 10f
  sd zero, 0(t0)
  j 8f
6:
  la t0, fault_hook                # an exception that cannot be skipped (a fetch fault) returns to a known place
  ld t1, 0(t0)
  beqz t1, 10f
  sd zero, 0(t0)
8:
  csrw mepc, t1
  li t0, 0x1800
  csrs mstatus, t0                 # return to machine mode
  j 30f
10:
  csrr t0, mepc
  lhu t1, 0(t0)
  andi t1, t1, 3
  li t2, 3
  addi t0, t0, 2
  bne t1, t2, 11f
  addi t0, t0, 2
11:
  csrw mepc, t0
  j 30f
20:
  la t0, irq_count
  ld t1, 0(t0)
  addi t1, t1, 1
  sd t1, 0(t0)
  slli a1, a1, 1
  srli a1, a1, 1
  li t0, 7
  bne a1, t0, 21f
  li t0, MTIMECMP
  li t1, -1
  sd t1, 0(t0)
  j 30f
21:
  li t0, 3
  bne a1, t0, 22f
  li t0, MSIP
  sw zero, 0(t0)
  j 30f
22:
  li t0, 1
  sll t0, t0, a1
  csrc mip, t0
30:
  ld ra, 0(sp)
  ld a0, 8(sp)
  ld a1, 16(sp)
  ld t0, 24(sp)
  ld t1, 32(sp)
  ld t2, 40(sp)
  ld t3, 48(sp)
  addi sp, sp, 96
  csrrw sp, mscratch, sp
  mret

# vectored table for the interrupt test: entry 0 must never run
.balign 256
mvec:
  j bad_vector
  .rept 15
  j mtrap
  .endr
bad_vector:
  TAG 'X'
1: j 1b

# ---- the supervisor-mode trap handler (traps delegated to it) -----------------------------------------------------------------------------------
.balign 4
strap:
  csrrw sp, sscratch, sp
  addi sp, sp, -96
  sd ra, 0(sp)
  sd a0, 8(sp)
  sd a1, 16(sp)
  sd t0, 24(sp)
  sd t1, 32(sp)
  sd t2, 40(sp)
  sd t3, 48(sp)
  TAG 'S'
  csrr a0, scause
  call puthex
  csrr a0, stval
  call puthex
  csrr a0, sepc
  la t0, _start
  sub a0, a0, t0
  csrr a1, scause
  bgez a1, 1f
  li a0, 0
1:
  call puthex
  csrr a0, sstatus
  call puthex
  csrr a1, scause
  bltz a1, 20f
  li t0, 8
  bne a1, t0, 9f
  la t0, s_final
  ld t1, 0(t0)
  beqz t1, 9f
  sd zero, 0(t0)
  ld ra, 0(sp)                    # an ecall from U that is delegated: leave through the machine with an ecall of our own
  ld a0, 8(sp)
  ld a1, 16(sp)
  ld t0, 24(sp)
  ld t1, 32(sp)
  ld t2, 40(sp)
  ld t3, 48(sp)
  addi sp, sp, 96
  csrrw sp, sscratch, sp
  ecall
9:
  la t0, s_hook
  ld t1, 0(t0)
  beqz t1, 10f
  sd zero, 0(t0)
  csrw sepc, t1
  j 30f
10:
  csrr t0, sepc
  lhu t1, 0(t0)
  andi t1, t1, 3
  li t2, 3
  addi t0, t0, 2
  bne t1, t2, 11f
  addi t0, t0, 2
11:
  csrw sepc, t0
  j 30f
20:
  la t0, irq_count
  ld t1, 0(t0)
  addi t1, t1, 1
  sd t1, 0(t0)
  slli a1, a1, 1
  srli a1, a1, 1
  li t0, 5
  bne a1, t0, 21f
  li t1, -1
  csrw 0x14D, t1                  # stimecmp
  li t0, 0x20
  csrc sie, t0                    # and no more supervisor timer interrupts
  j 30f
21:
  li t0, 1
  sll t0, t0, a1
  csrc sip, t0
30:
  ld ra, 0(sp)
  ld a0, 8(sp)
  ld a1, 16(sp)
  ld t0, 24(sp)
  ld t1, 32(sp)
  ld t2, 40(sp)
  ld t3, 48(sp)
  addi sp, sp, 96
  csrrw sp, sscratch, sp
  sret

# ---- leaving machine mode ------------------------------------------------------------------------------------------------------------------------
.macro ENTER target, mode
  la t0, 1f
  la t1, resume_hook
  sd t0, 0(t1)
  la t0, \target
  csrw mepc, t0
  li t0, 0x1800
  csrc mstatus, t0
  .if \mode == 1
  li t0, 0x800
  csrs mstatus, t0
  .endif
  mret
1:
.endm

.macro MJUMP reg                  # a jump whose fault cannot be skipped: it returns to the next instruction
  la t0, 1f
  la t1, fault_hook
  sd t0, 0(t1)
  jalr \reg
1:
.endm
.macro SJUMP reg
  la t0, 1f
  la t1, s_hook
  sd t0, 0(t1)
  jalr \reg
1:
.endm
.macro WAIT_IRQ n
  la t0, irq_count
1:
  ld t1, 0(t0)
  li t2, \n
  bltu t1, t2, 1b
.endm

.macro ENTERV target, mode          # like ENTER, for a target given as a number (a virtual address)
  la t0, 1f
  la t1, resume_hook
  sd t0, 0(t1)
  li t0, \target
  csrw mepc, t0
  li t0, 0x1800
  csrc mstatus, t0
  .if \mode == 1
  li t0, 0x800
  csrs mstatus, t0
  .endif
  mret
1:
.endm

.macro ENTERU label                 # enter user mode at `label`, a label in the page that is mapped at UVA (the address is computed from the offset into ucode)
  la t0, 1f
  la t1, resume_hook
  sd t0, 0(t1)
  la t0, \label
  la t1, ucode
  sub t0, t0, t1
  li t1, UVA
  add t0, t0, t1
  csrw mepc, t0
  li t0, 0x1800
  csrc mstatus, t0
  mret
1:
.endm

.macro CSRPROBE name, mask=-1
  li t4, 0
  li t5, 0
  li t6, 0
  TAG 'C'
  csrr t4, \name
  li t0, \mask
  csrrw t1, \name, t0
  csrr t5, \name
  csrrw x0, \name, x0
  csrr t6, \name
  csrw \name, t4
  PRINT t4
  PRINT t5
  PRINT t6
.endm


.section .data
.balign 4096
ctl_page:
resume_hook: .dword 0
fault_hook: .dword 0
s_hook: .dword 0
irq_count: .dword 0
s_final: .dword 0
satp_value: .dword 0
.balign 16
.space 4096
stack_top:
.space 4096
mtrap_stack_top:
.space 4096
strap_stack_top:
.space 16
