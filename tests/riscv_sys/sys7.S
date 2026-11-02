# Privileged-architecture test payload, part 7: Sv48x4 / Sv57x4, fetches and atomics in the guest, hardware A/D in the guest, the guest's clock and timer, TVM and the hfence
# instructions. See common.inc.
.include "common.inc"
.include "htables.inc"

run_tests:
  addi sp, sp, -16
  sd ra, 0(sp)
  call build_tables
  call build_wide_tables
  call test_wide
  call test_fetch_atomic
  call test_guest_adu
  call test_guest_clock
  call test_tvm
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

# deeper tables: Sv48x4 and Sv57x4 over the same mappings (the Sv39x4 root becomes a lower level), and Sv48 / Sv57 for the guest's own tables
build_wide_tables:
  la t1, g3
  li t0, 0xD7
  sd t0, 0(t1)
  SETPTE g3, 1, gl1, PTE_V
  li t0, 0x200000DF
  sd t0, 16(t1)
  SETPTE groot48, 0, g3, PTE_V
  SETPTE groot57, 0, g4, PTE_V
  SETPTE g4, 0, g3, PTE_V
  SETPTE vroot48, 0, vroot, PTE_V
  SETPTE vroot57, 0, vroot48, PTE_V
  ret

.macro GUEST_MODES vs_mode, g_root, g_mode
  la t0, vroot
  .if \vs_mode == 9
  la t0, vroot48
  .endif
  .if \vs_mode == 10
  la t0, vroot57
  .endif
  srli t0, t0, 12
  li t1, \vs_mode
  slli t1, t1, 60
  or t0, t0, t1
  csrw 0x280, t0
  la t0, \g_root
  srli t0, t0, 12
  li t1, \g_mode
  slli t1, t1, 60
  or t0, t0, t1
  csrw 0x680, t0
.endm

test_wide:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'K'
  SETUP_GUEST
  li t0, (1 << 12) | (1 << 13) | (1 << 15) | (1 << 20) | (1 << 21) | (1 << 23) | (1 << 2)
  csrw medeleg, t0
  li t0, (1 << 12) | (1 << 13) | (1 << 15) | (1 << 2)
  csrw 0x602, t0
  GUEST_MODES 8, groot48, 9
  ENTERG vs_wide, 1
  GUEST_MODES 9, groot, 8
  ENTERG vs_wide, 1
  GUEST_MODES 9, groot48, 9
  ENTERG vs_wide, 1
  GUEST_MODES 10, groot57, 10
  ENTERG vs_wide, 1
  GUEST_MODES 10, groot, 8
  ENTERG vs_wide, 1
  # an unsupported mode is not written
  li t0, 11
  slli t0, t0, 60
  csrw 0x680, t0
  csrr t1, 0x680
  PRINT t1
  csrw 0x280, t0
  csrr t1, 0x280
  PRINT t1
  li t0, -1
  csrw 0x680, zero
  csrr t1, 0x680
  PRINT t1
  la t0, groot
  srli t0, t0, 12
  addi t0, t0, 3                 # hgatp.PPN is 16 KiB aligned: the low two bits are zero
  li t1, 8
  slli t1, t1, 60
  or t0, t0, t1
  csrw 0x680, t0
  csrr t1, 0x680
  srli t1, t1, 44
  PRINT t1
  csrw medeleg, zero
  csrw 0x602, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

vs_wide:
  li t5, 0x40000000
  ld t2, 0(t5)
  sd t2, 0(t5)
  li t5, 0x40001000
  ld t2, 0(t5)                   # VS page fault
  li t5, 0x40002000
  ld t2, 0(t5)                   # guest page fault
  li t5, 0x40003000
  sd t2, 0(t5)                   # guest page fault 23
  ecall

test_fetch_atomic:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'Z'
  SETUP_GUEST
  li t0, (1 << 12) | (1 << 13) | (1 << 15) | (1 << 20) | (1 << 21) | (1 << 23) | (1 << 2) | (1 << 4) | (1 << 6)
  csrw medeleg, t0
  li t0, (1 << 12) | (1 << 13) | (1 << 15) | (1 << 2) | (1 << 4) | (1 << 6)
  csrw 0x602, t0
  ENTERG vs_fetch, 1
  csrw medeleg, zero
  csrw 0x602, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

vs_fetch:
  li t5, 0x40001000
  SJUMP t5                       # VS page fault on the fetch: 12
  li t5, 0x40002000
  SJUMP t5                       # guest page fault on the fetch: 20, htval
  li t5, 0x40005000
  SJUMP t5                       # VS execute-only: but the G-stage page has no X: guest page fault
  li t5, 0x40000000
  SJUMP t5                       # no X in the VS-stage: page fault
  li t5, 0x40003000
  amoadd.d t2, t2, (t5)          # G-stage read-only: store/AMO guest page fault
  lr.d t2, (t5)
  sc.d t2, t2, (t5)
  li t5, 0x40000001
  lr.w t2, (t5)                  # misaligned: cause 4 to the guest
  sc.w t2, t2, (t5)
  amoswap.w t2, t2, (t5)
  ld t2, 0(t5)                   # a misaligned load is fine
  li t5, 0x40001000
  amoadd.d t2, t2, (t5)          # VS invalid: AMO page fault 15
  ecall

test_guest_adu:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'D'
  SETUP_GUEST
  # without henvcfg.ADUE the A and D bits fault (Svade); with menvcfg.ADUE and henvcfg.ADUE the hart sets them in both stages
  li t0, (1 << 12) | (1 << 13) | (1 << 15) | (1 << 20) | (1 << 21) | (1 << 23)
  csrw medeleg, t0
  li t0, (1 << 12) | (1 << 13) | (1 << 15)
  csrw 0x602, t0
  ENTERG vs_adu, 1
  li t0, 1 << 61
  csrs 0x30A, t0
  csrs 0x60A, t0
  csrr t1, 0x60A
  PRINT t1
  ENTERG vs_adu, 1
  la t1, vl0
  ld t2, 48(t1)
  PRINT t2
  la t1, gl0
  ld t2, 48(t1)
  PRINT t2
  csrw 0x60A, zero
  csrc 0x30A, t0
  csrw medeleg, zero
  csrw 0x602, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

vs_adu:
  li t5, 0x40006000
  ld t2, 0(t5)                   # A clear in both stages
  sd t2, 0(t5)
  ecall

test_guest_clock:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'T'
  SETUP_GUEST
  # the guest's time is the machine's plus htimedelta
  li t0, 5
  slli t0, t0, 32
  csrw 0x605, t0
  li t0, 7
  csrw mcounteren, t0
  csrw scounteren, t0
  csrw 0x606, t0
  rdtime s2
  la t0, g_time
  sd s2, 0(t0)
  ENTERG vs_time, 1
  la t0, g_time
  ld s3, 0(t0)
  la t0, g_time2
  ld s4, 0(t0)
  sub s4, s4, s3
  srli s4, s4, 32
  PRINT s4
  # the guest's timer: vstimecmp with Sstc, delta added; htimedelta puts the time past the compare at once
  li t0, 1 << 63
  csrs 0x30A, t0
  csrs 0x60A, t0
  csrw 0x24D, zero               # vstimecmp = 0: pending
  li t0, 0x444
  csrw 0x603, t0
  li t0, 0x1444
  csrw mie, t0
  la t0, vstrap
  csrw 0x205, t0
  li t0, 2
  csrs 0x200, t0                 # vsstatus.SIE
  la t0, irq_count
  sd zero, 0(t0)
  ENTERG vs_wait2, 1
  li t0, -1
  csrw 0x24D, t0
  csrw mie, zero
  csrw 0x603, zero
  csrw 0x605, zero
  csrw mcounteren, zero
  csrw scounteren, zero
  csrw 0x606, zero
  li t0, 1 << 63
  csrc 0x30A, t0
  csrw 0x60A, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

vs_time:
  rdtime t2
  la t0, g_time2
  sd t2, 0(t0)
  ecall
vs_wait2:
  la t0, irq_count
1:
  ld t1, 0(t0)
  beqz t1, 1b
  ecall

test_tvm:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'V'
  li t0, 1 << 20
  csrs mstatus, t0               # TVM: HS cannot touch hgatp / satp or use hfence.gvma, hfence.vvma is still fine
  ENTER hs_tvm, 1
  li t0, 1 << 20
  csrc mstatus, t0
  ENTER hs_tvm, 1
  ENTER u_tvm, 0
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

hs_tvm:
  csrr t0, 0x680
  csrr t0, satp
  csrr t0, 0x280                 # vsatp is fine
  hfence.gvma
  hfence.vvma
  sfence.vma
  ecall
u_tvm:
  hfence.gvma
  hfence.vvma
  csrr t0, 0x600
  ecall

.section .data
.balign 16384
groot48: .space 16384
groot57: .space 16384
.balign 4096
g3: .space 4096
g4: .space 4096
vroot48: .space 4096
vroot57: .space 4096
g_time: .dword 0
g_time2: .dword 0
