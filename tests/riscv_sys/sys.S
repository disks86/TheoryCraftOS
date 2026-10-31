# Privileged-architecture test payload for the RISC-V machine: CSRs, modes, delegation, traps, interrupts, paging and PMP. See common.inc.
.include "common.inc"

# ---- the tests -----------------------------------------------------------------------------------------------------------------------------------
run_tests:
  addi sp, sp, -16
  sd ra, 0(sp)
  call test_csrs
  call test_modes
  call test_delegation
  call test_misc_traps
  call test_fp_vector
  call test_interrupts
  call test_paging
  call test_pmp
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

test_csrs:
  addi sp, sp, -16
  sd ra, 0(sp)
  CSRPROBE misa
  CSRPROBE mstatus
  CSRPROBE medeleg, 0xFBFF
  CSRPROBE mideleg, 0x222
  CSRPROBE mie, 0xFFF
  CSRPROBE mtvec
  CSRPROBE mcounteren
  CSRPROBE menvcfg
  CSRPROBE mcountinhibit
  CSRPROBE mepc
  CSRPROBE mcause
  CSRPROBE mtval
  CSRPROBE mip, 0x222
  CSRPROBE sstatus
  CSRPROBE sie
  CSRPROBE stvec
  CSRPROBE scounteren
  CSRPROBE senvcfg
  CSRPROBE sscratch
  CSRPROBE sepc
  CSRPROBE scause
  CSRPROBE stval
  CSRPROBE sip
  CSRPROBE satp
  CSRPROBE pmpcfg0
  CSRPROBE pmpcfg2
  CSRPROBE pmpaddr0
  CSRPROBE pmpaddr15
  CSRPROBE mvendorid
  CSRPROBE mimpid
  CSRPROBE mhartid
  CSRPROBE 0x747
  CSRPROBE 0x14D
  CSRPROBE 0x3A1
  CSRPROBE 0x7C0
  CSRPROBE 0xB03
  CSRPROBE 0xC03
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

test_modes:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'A'
  ecall
  ebreak
  ENTER s_code1, 1
  ENTER u_code1, 0
  li t0, 7
  csrw mcounteren, t0
  ENTER u_code1, 0
  csrw scounteren, t0
  ENTER u_code1, 0
  csrw mcounteren, zero
  csrw scounteren, zero
  ENTER s_code2, 1
  ENTER u_code2, 0
  # TSR / TW / TVM
  li t0, (1 << 22) | (1 << 21) | (1 << 20)
  csrs mstatus, t0
  ENTER s_code3, 1
  li t0, (1 << 22) | (1 << 21) | (1 << 20)
  csrc mstatus, t0
  ENTER s_code3b, 1
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

s_code1:
  csrr t0, mstatus
  csrr t0, sstatus
  csrr t0, cycle
  rdtime t0
  mret
  ecall
u_code1:
  csrr t0, cycle
  rdtime t0
  csrr t0, instret
  csrr t0, sstatus
  sret
  wfi
  ecall
s_code2:
  sfence.vma
  csrr t0, satp
  sret_test:
  ecall
u_code2:
  sfence.vma
  fence
  fence.i
  ecall
s_code3:
  wfi
  sfence.vma
  csrr t0, satp
  la t0, 1f
  csrw sepc, t0
  csrr t1, sstatus
  sret
1:
  ecall

s_code3b:
  sfence.vma
  csrr t0, satp
  la t0, 1f
  csrw sepc, t0
  csrr t1, sstatus
  sret
1:
  ecall

test_delegation:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'D'
  la t0, strap
  csrw stvec, t0
  la t0, strap_stack_top
  csrw sscratch, t0
  li t0, (1 << 8) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 4) | (1 << 6) | (1 << 0)
  csrw medeleg, t0
  csrr t0, medeleg
  PRINT t0
  la t0, s_final
  li t1, 1
  sd t1, 0(t0)
  ENTER u_code3, 0
  li t0, 0
  csrw medeleg, t0
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

u_code3:
  .word 0xFFFFFFFF                # illegal: delegated to S
  ebreak                          # delegated
  ecall                           # delegated; the S handler leaves through the machine
  ecall

test_misc_traps:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'T'
  la t5, data_area
  addi t5, t5, 2
  lr.w t0, (t5)                   # misaligned lr: load misaligned
  sc.w t0, t0, (t5)
  amoadd.w t0, t0, (t5)
  addi t5, t5, 2
  lr.d t0, (t5)
  li t5, 0x8000000
  ld t0, 0(t5)                    # load access fault
  sd t0, 0(t5)                    # store access fault
  amoadd.d t0, t0, (t5)
  li t5, 0x9000000
  MJUMP t5                        # instruction access fault
  li t5, 0
  .word 0x00000000                # illegal (all-zero)
  .word 0x0000100F                # fence.i is fine
  c.ebreak
  li t0, 0x30200073
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

.set fscase_n, 0
.macro FSCASE insn
  .set fscase_n, fscase_n + 1
  li t0, 0x6000
  csrc mstatus, t0
  li t0, 0x2000
  csrs mstatus, t0
  \insn
  li t1, 0xF500 + fscase_n
  PRINT t1
  csrr t1, mstatus
  PRINT t1
.endm
test_fp_vector:
  addi sp, sp, -16
  sd ra, 0(sp)
  la t6, data_area
  TAG 'F'
  li t0, 0x6000
  csrc mstatus, t0                # FS off
  csrr t1, mstatus
  PRINT t1
  fadd.d f0, f0, f0               # illegal
  fld f0, 0(t6)
  fsd f0, 0(t6)
  csrr t1, fcsr
  csrw frm, zero
  fmv.d.x f1, t0
  fmv.x.d t1, f1
  FSCASE nop
  FSCASE "fsd f0, 0(t6)"
  FSCASE "fld f0, 0(t6)"
  FSCASE "fmv.x.d t1, f0"
  FSCASE "fmv.d.x f0, t0"
  FSCASE "fclass.d t1, f0"
  FSCASE "feq.d t1, f0, f0"
  FSCASE "flt.d t1, f0, f1"
  FSCASE "fcvt.w.d t1, f0"
  FSCASE "fcvt.d.w f0, t0"
  FSCASE "fadd.d f0, f0, f0"
  FSCASE "csrr t1, fflags"
  FSCASE "csrw fflags, zero"
  FSCASE "csrr t1, frm"
  FSCASE "csrs fcsr, zero"
  FSCASE "csrs fcsr, t0"
  FSCASE "fsw f0, 0(t6)"
  FSCASE "flw f0, 0(t6)"
  FSCASE "fmv.x.w t1, f0"
  csrr t1, sstatus
  PRINT t1
  li t0, 0x600
  csrc mstatus, t0                # VS off
  csrr t1, vl
  li t0, 4
  vsetvli t1, t0, e32, m1, ta, ma
  vadd.vv v1, v1, v1
  li t0, 0x200
  csrs mstatus, t0
  csrr t1, mstatus
  PRINT t1
  vsetvli t1, t0, e32, m1, ta, ma
  PRINT t1
  csrr t1, mstatus
  PRINT t1
  li t0, 0x600
  csrc mstatus, t0
  li t0, 0x200
  csrs mstatus, t0
  vle32.v v0, (t6)
  csrr t1, mstatus
  PRINT t1
  li t0, 0x600
  csrc mstatus, t0
  li t0, 0x200
  csrs mstatus, t0
  vse32.v v0, (t6)
  csrr t1, mstatus
  PRINT t1
  csrr t1, vlenb
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

test_interrupts:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'I'
  la t0, irq_count
  sd zero, 0(t0)
  li t0, MTIME
  ld t1, 0(t0)
  addi t1, t1, 20
  li t0, MTIMECMP
  sd t1, 0(t0)
  li t0, 1 << 7
  csrs mie, t0
  li t0, 8
  csrs mstatus, t0                # MIE
  WAIT_IRQ 1
  li t0, 8
  csrc mstatus, t0
  # software interrupt and timer pending together: the software one goes first
  la t0, irq_count
  sd zero, 0(t0)
  li t0, 1 << 3
  csrs mie, t0
  li t0, MSIP
  li t1, 1
  sw t1, 0(t0)
  li t0, MTIMECMP
  sd zero, 0(t0)
  csrr t1, mip
  PRINT t1
  li t0, 8
  csrs mstatus, t0
  WAIT_IRQ 2
  li t0, 8
  csrc mstatus, t0
  # vectored mtvec
  la t0, irq_count
  sd zero, 0(t0)
  la t0, mvec
  ori t0, t0, 1
  csrw mtvec, t0
  li t0, MSIP
  li t1, 1
  sw t1, 0(t0)
  li t0, 8
  csrs mstatus, t0
  WAIT_IRQ 1
  li t0, 8
  csrc mstatus, t0
  la t0, mtrap
  csrw mtvec, t0
  # wfi with a timer that has to fire: MTIE set, MIE clear, so wfi returns and the interrupt stays pending
  li t0, MTIME
  ld t1, 0(t0)
  addi t1, t1, 500
  li t0, MTIMECMP
  sd t1, 0(t0)
  wfi
  csrr t1, mip
  andi t1, t1, 0x80
  PRINT t1
  li t0, MTIMECMP
  li t1, -1
  sd t1, 0(t0)
  li t0, (1 << 7) | (1 << 3)
  csrc mie, t0
  # delegated supervisor interrupts: SSIP set by machine mode, taken in S with SIE
  la t0, strap
  csrw stvec, t0
  la t0, strap_stack_top
  csrw sscratch, t0
  li t0, 0x222
  csrw mideleg, t0
  csrw mie, t0
  csrr t1, mideleg
  PRINT t1
  li t0, 2
  csrs mip, t0
  li t0, 2
  csrs mstatus, t0                # SIE (only matters in S)
  ENTER s_code4, 1
  # Sstc: stimecmp raises the supervisor timer
  li t0, 1 << 63
  csrs 0x30A, t0
  li t0, 2
  csrw mcounteren, t0
  la t0, irq_count
  sd zero, 0(t0)
  ENTER s_code5, 1
  csrw mcounteren, zero
  li t0, 1 << 63
  csrc 0x30A, t0
  csrw mideleg, zero
  csrw mie, zero
  li t0, 2
  csrc mstatus, t0
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

s_code4:
  nop
  nop
  ecall

s_code5:
  rdtime t1
  addi t1, t1, 30
  csrw 0x14D, t1
  li t0, 2
  csrs sstatus, t0
  la t0, irq_count
1:
  ld t1, 0(t0)
  beqz t1, 1b
  ecall

# ---- paging --------------------------------------------------------------------------------------------------------------------------------------
.equ PTE_V, 1
.equ PTE_R, 2
.equ PTE_W, 4
.equ PTE_X, 8
.equ PTE_U, 16
.equ PTE_A, 0x40
.equ PTE_D, 0x80
.macro PTE dst, addr, flags      # dst = (addr >> 12 << 10) | flags
  la \dst, \addr
  srli \dst, \dst, 12
  slli \dst, \dst, 10
  ori \dst, \dst, \flags
.endm

test_paging:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'P'
  la t0, strap
  csrw stvec, t0
  la t0, strap_stack_top
  csrw sscratch, t0
  li t0, (1 << 12) | (1 << 13) | (1 << 15) | (1 << 2) | (1 << 8)
  csrw medeleg, t0
  # root table: [0, 1G) and [2G, 3G) identity (gigapages), entry 1 -> l1
  la t1, root
  li t0, 0xC7                     # V R W A D
  sd t0, 0(t1)
  li t0, 0x200000CF               # PPN of 0x80000000, V R W X A D
  sd t0, 16(t1)
  PTE t0, l1, PTE_V
  sd t0, 8(t1)
  # l1: entry 0 -> l0; entry 1: misaligned superpage (ppn low bits set)
  la t1, l1
  PTE t0, l0, PTE_V
  sd t0, 0(t1)
  li t0, 0x20000000
  addi t0, t0, 1
  slli t0, t0, 10
  ori t0, t0, 0xC7
  sd t0, 8(t1)
  # l0 pages: 0 data R only; 1 data U RW; 2 code-ish X only; 3 A clear; 4 D clear; 5 invalid; 6 W only (reserved)
  la t1, l0
  PTE t0, data_area, PTE_V | PTE_R | PTE_A | PTE_D
  sd t0, 0(t1)
  PTE t0, data_area, PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D
  sd t0, 8(t1)
  PTE t0, xpage, PTE_V | PTE_X | PTE_A
  sd t0, 16(t1)
  PTE t0, data_area, PTE_V | PTE_R | PTE_W | PTE_D
  sd t0, 24(t1)
  PTE t0, data_area, PTE_V | PTE_R | PTE_W | PTE_A
  sd t0, 32(t1)
  PTE t0, data_area, PTE_R | PTE_W | PTE_A | PTE_D
  sd t0, 40(t1)
  PTE t0, data_area, PTE_V | PTE_W | PTE_A | PTE_D
  sd t0, 48(t1)
  PTE t0, uxpage, PTE_V | PTE_X | PTE_U | PTE_A
  sd t0, 56(t1)
  # satp: Sv39 (8)
  la t0, root
  srli t0, t0, 12
  li t1, 8
  slli t1, t1, 60
  or t0, t0, t1
  la t1, satp_value
  sd t0, 0(t1)
  la t0, s_final
  li t1, 1
  sd t1, 0(t0)
  ENTER s_paging, 1
  # MPRV: a machine-mode load goes through the page tables
  la t1, satp_value
  ld t0, 0(t1)
  csrw satp, t0
  li t0, 0x20000                  # MPRV
  li t1, 0x1800
  csrc mstatus, t1
  li t1, 0x800
  csrs mstatus, t1                # MPP = S
  csrs mstatus, t0
  li t5, 0x40005000
  ld t2, 0(t5)                    # page fault
  li t5, 0x40000000
  ld t2, 0(t5)
  sd t2, 0(t5)                    # R only: store page fault
  csrc mstatus, t0
  csrw satp, zero
  # Sv48 and Sv57 over the same tables: the root's entry 0 points at the Sv39 root
  la t1, root48
  PTE t0, root, PTE_V
  sd t0, 0(t1)
  la t1, root57
  PTE t0, root48, PTE_V
  sd t0, 0(t1)
  la t0, root48
  srli t0, t0, 12
  li t1, 9
  slli t1, t1, 60
  or t0, t0, t1
  la t1, satp_value
  sd t0, 0(t1)
  la t0, s_final
  li t1, 1
  sd t1, 0(t0)
  ENTER s_paging48, 1
  la t0, root57
  srli t0, t0, 12
  li t1, 10
  slli t1, t1, 60
  or t0, t0, t1
  la t1, satp_value
  sd t0, 0(t1)
  la t0, s_final
  li t1, 1
  sd t1, 0(t0)
  ENTER s_paging48, 1
  li t0, 0
  csrw medeleg, t0
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

s_paging:
  la t1, satp_value
  ld t0, 0(t1)
  csrw satp, t0
  sfence.vma
  csrr t2, satp
  li t5, 0x40000000
  ld t2, 0(t5)                    # R page: fine
  sd t2, 0(t5)                    # store: page fault 15
  li t5, 0x40001000
  ld t2, 0(t5)                    # U page from S without SUM: 13
  li t0, 1 << 18
  csrs sstatus, t0
  ld t2, 0(t5)                    # SUM: fine
  sd t2, 0(t5)
  csrc sstatus, t0
  li t5, 0x40002000
  ld t2, 0(t5)                    # X only: 13
  li t0, 1 << 19
  csrs sstatus, t0
  ld t2, 0(t5)                    # MXR: fine
  csrc sstatus, t0
  li t5, 0x40003000
  ld t2, 0(t5)                    # A clear: 13
  li t5, 0x40004000
  ld t2, 0(t5)                    # D clear, read: fine
  sd t2, 0(t5)                    # store: 15
  li t5, 0x40005000
  ld t2, 0(t5)                    # invalid: 13
  li t5, 0x40006000
  ld t2, 0(t5)                    # W only: 13
  li t5, 0x40200000
  ld t2, 0(t5)                    # misaligned superpage: 13
  li t5, 1
  slli t5, t5, 40
  ld t2, 0(t5)                    # not canonical: 13
  li t5, 0x40000000
  amoadd.d t2, t2, (t5)          # AMO on a read-only page: store fault 15
  lr.d t2, (t5)
  li t5, 0x40005000
  lr.d t2, (t5)                  # 13
  li t5, 0x40000000
  SJUMP t5                        # fetch from a page without X: 12
  li t5, 0x40007000
  SJUMP t5                        # fetch from a U page in S: 12
  li t5, 0x40002000
  jalr t5                         # fetch from the X page: runs (xpage returns)
  .hword 0
  # a store that straddles a page: the first part is fine, the second page is read-only
  li t5, 0x40001FFC
  li t0, 1 << 18
  csrs sstatus, t0
  sd t2, 0(t5)                    # crosses from the writable U page into the invalid page 0x40002000: 15 with stval of the second page
  csrc sstatus, t0
  ecall

s_paging48:
  la t1, satp_value
  ld t0, 0(t1)
  csrw satp, t0
  sfence.vma
  li t5, 0x40000000
  ld t2, 0(t5)
  sd t2, 0(t5)
  li t5, 1
  slli t5, t5, 47
  ld t2, 0(t5)
  li t5, 1
  slli t5, t5, 38
  ld t2, 0(t5)
  ecall

# ---- PMP -----------------------------------------------------------------------------------------------------------------------------------------
test_pmp:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'R'
  la t0, strap
  csrw stvec, t0
  la t0, strap_stack_top
  csrw sscratch, t0
  li t0, (1 << 1) | (1 << 5) | (1 << 7) | (1 << 2)
  csrw medeleg, t0
  # entry 0: the 4 KiB at data_area is off limits (NAPOT, no permissions); entry 1: everything else is open
  la t0, data_area
  srli t0, t0, 2
  ori t0, t0, 0x1FF               # 4 KiB NAPOT
  li t1, 0x1FF
  li t1, 0x0
  csrw pmpaddr0, t0
  li t0, -1
  csrw pmpaddr1, t0
  li t0, 0x1F18                   # cfg1 = NAPOT RWX, cfg0 = NAPOT with no permissions
  csrw pmpcfg0, t0
  csrr t1, pmpcfg0
  PRINT t1
  la t0, s_final
  li t1, 1
  sd t1, 0(t0)
  ENTER s_pmp, 1
  ld t5, data_area                # machine mode is not bound by an unlocked entry
  # a locked entry binds machine mode too
  li t0, 0x1F98
  csrw pmpcfg0, t0
  csrr t1, pmpcfg0
  PRINT t1
  ld t5, data_area
  sd t5, data_area, t6
  li t0, 0
  csrw pmpcfg0, t0                # locked: the write has no effect
  csrr t1, pmpcfg0
  PRINT t1
  la t0, strap_stack_top
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

s_pmp:
  la t5, data_area
  ld t2, 0(t5)                    # PMP load access fault 5
  sd t2, 0(t5)                    # 7
  SJUMP t5                        # fetch access fault 1
  la t5, strap_stack_top
  addi t5, t5, -8
  ld t2, 0(t5)                    # open
  ecall

.section .data
.balign 4096
xpage:
  ret
.balign 4096
uxpage:
  ret
.balign 4096
root: .space 4096
l1: .space 4096
l0: .space 4096
root48: .space 4096
root57: .space 4096
data_area: .space 4096
