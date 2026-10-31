# Privileged-architecture test payload, part 2: user mode under paging, user-visible faults, interrupts taken in user mode. See common.inc.
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
.equ UVA, 0x40000000

run_tests:
  addi sp, sp, -16
  sd ra, 0(sp)
  call test_user_paging
  call test_user_interrupts
  call test_xret
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

test_user_paging:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'U'
  la t0, strap
  csrw stvec, t0
  la t0, strap_stack_top
  csrw sscratch, t0
  li t0, (1 << 2) | (1 << 12) | (1 << 13) | (1 << 15)
  csrw medeleg, t0
  # root: [0,1G) supervisor RW, [1G,2G) -> l1, [2G,3G) supervisor RWX
  la t1, root_u
  li t0, 0xC7
  sd t0, 0(t1)
  PTE t0, l1u, PTE_V
  sd t0, 8(t1)
  li t0, 0x200000CF
  sd t0, 16(t1)
  la t1, l1u
  PTE t0, l0u, PTE_V
  sd t0, 0(t1)
  la t1, l0u
  PTE t0, ucode, PTE_V | PTE_R | PTE_X | PTE_U | PTE_A
  sd t0, 0(t1)
  PTE t0, ctl_page, PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D
  sd t0, 8(t1)
  PTE t0, spage, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D
  sd t0, 16(t1)
  PTE t0, xupage, PTE_V | PTE_X | PTE_U | PTE_A
  sd t0, 24(t1)
  PTE t0, rupage, PTE_V | PTE_R | PTE_U | PTE_A | PTE_D
  sd t0, 32(t1)
  PTE t0, spage, PTE_V | PTE_R | PTE_W | PTE_U | PTE_D
  sd t0, 40(t1)
  la t0, root_u
  srli t0, t0, 12
  li t1, 8
  slli t1, t1, 60
  or t0, t0, t1
  csrw satp, t0
  li t0, 1 << 18                  # SUM, for the handler's look at the user's instruction
  csrs mstatus, t0
  ENTERU ucode
  li t0, 1 << 19                  # MXR
  csrs mstatus, t0
  ENTERU u_mxr
  li t0, (1 << 19) | (1 << 18)
  csrc mstatus, t0
  csrw satp, zero
  csrw medeleg, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

test_user_interrupts:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'V'
  li t0, 8
  csrc mstatus, t0                # MIE clear: the timer can only be taken once we are in user mode
  la t0, root_u
  srli t0, t0, 12
  li t1, 8
  slli t1, t1, 60
  or t0, t0, t1
  csrw satp, t0
  la t0, strap
  csrw stvec, t0
  # a delegated supervisor software interrupt is taken in user mode whatever sstatus.SIE says
  li t0, 0x222
  csrw mideleg, t0
  csrw mie, t0
  la t0, irq_count
  sd zero, 0(t0)
  li t0, 2
  csrs mip, t0
  ENTERU u_wait
  # a machine timer interrupt in user mode
  la t0, irq_count
  sd zero, 0(t0)
  csrw mideleg, zero
  li t0, 8
  csrc mstatus, t0
  li t0, MTIME
  ld t1, 0(t0)
  addi t1, t1, 20
  li t0, MTIMECMP
  sd t1, 0(t0)
  li t0, 1 << 7
  csrw mie, t0
  ENTERU u_wait
  csrw mie, zero
  csrw satp, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

test_xret:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'X'
  # mret to U with MPRV set clears MPRV; sret clears it too only when going below M
  li t0, 0x20000
  csrs mstatus, t0
  csrr t1, mstatus
  PRINT t1
  ENTER x_code, 0
  csrr t1, mstatus
  PRINT t1
  # mepc low bit, sepc low bit
  li t0, 0x80000003
  csrw mepc, t0
  csrr t1, mepc
  PRINT t1
  csrw sepc, t0
  csrr t1, sepc
  PRINT t1
  # mstatus.MPP written with the reserved value 2 keeps its old value
  li t0, 0x1800
  csrc mstatus, t0
  li t0, 0x800
  csrs mstatus, t0
  li t0, 0x1000
  csrs mstatus, t0
  csrr t1, mstatus
  PRINT t1
  li t0, 0x1800
  csrs mstatus, t0
  # SPP and the sstatus view
  li t0, 0x100
  csrs sstatus, t0
  csrr t1, mstatus
  PRINT t1
  li t0, 0x100
  csrc sstatus, t0
  # sstatus cannot touch machine fields
  li t0, -1
  csrw sstatus, t0
  csrr t1, mstatus
  PRINT t1
  csrr t1, sstatus
  PRINT t1
  li t0, 0x6000
  csrw mstatus, t0
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

x_code:
  ecall

# ---- the user program (mapped at UVA by 4 KiB pages) ---------------------------------------------------------------------------------------------
.section .data
.balign 4096
ucode:
  li t6, UVA + 0x1000 + 16        # s_hook
  li t5, UVA + 0x1000
  ld t0, 8(t5)                    # fine
  sd t0, 8(t5)                    # fine
  li t5, UVA + 0x2000
  ld t0, 0(t5)                    # supervisor page: load page fault
  sd t0, 0(t5)                    # store page fault
  li t5, UVA + 0x3000
  ld t0, 0(t5)                    # execute-only: load page fault (MXR clear)
  li t5, UVA + 0x4000
  ld t0, 0(t5)
  sd t0, 0(t5)                    # read-only: store page fault
  li t5, UVA + 0x5000
  ld t0, 0(t5)                    # accessed bit clear
  csrr t0, sstatus                # illegal
  li t5, UVA + 0x2000
  lla t0, 1f
  sd t0, 0(t6)
  jalr t5                         # fetch from a supervisor page: instruction page fault, resumed at 1:
1:
  li t5, UVA + 0x1FFC
  sd t0, 0(t5)                    # crosses into the supervisor page: store fault, stval = the second page
  ld t0, 0(t5)
  lla t0, 2f
  sd t0, 0(t6)
  j cross
2:
  ecall
u_mxr:
  li t5, UVA + 0x3000
  ld t0, 0(t5)                    # MXR: fine
  lbu t0, 0(t5)
  sd t0, 0(t5)                    # still not writable
  ecall
u_wait:
  li t5, UVA + 0x1000
1:
  ld t0, 0x18(t5)                 # irq_count (ctl_page + 0x18)
  beqz t0, 1b
  ecall
.org ucode + 0xFFE
cross:
  .word 0x00128293                # addi t0, t0, 1 : its second half is in the next page, which is not executable
.balign 4096
xupage:
  .word 0
.balign 4096
rupage:
  .word 0
.balign 4096
spage:
  .space 4096
.balign 4096
root_u: .space 4096
l1u: .space 4096
l0u: .space 4096
