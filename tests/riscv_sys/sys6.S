# Privileged-architecture test payload, part 6: two-stage address translation, guest page faults and the hlv / hsv instructions. See common.inc.
.include "common.inc"
.include "htables.inc"

run_tests:
  addi sp, sp, -16
  sd ra, 0(sp)
  call build_tables
  call test_two_stage
  call test_hlv
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

test_two_stage:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'G'
  SETUP_GUEST
  # VS-stage page faults go to the guest; guest page faults to HS
  li t0, (1 << 12) | (1 << 13) | (1 << 15) | (1 << 20) | (1 << 21) | (1 << 23) | (1 << 2)
  csrw medeleg, t0
  li t0, (1 << 12) | (1 << 13) | (1 << 15) | (1 << 2)
  csrw 0x602, t0
  ENTERG vs_translate, 1
  li t0, (1 << 18)
  csrs 0x200, t0                 # vsstatus.SUM (bit 18)
  li t0, (1 << 19)
  csrs 0x200, t0                 # vsstatus.MXR
  ENTERG vs_translate2, 1
  li t0, (1 << 19) | (1 << 18)
  csrc 0x200, t0
  # mstatus.MXR for the execute-only G-stage page
  li t0, (1 << 19)
  csrs mstatus, t0
  ENTERG vs_translate3, 1
  csrc mstatus, t0
  # hgatp bare: the guest-physical address is the host-physical one; vsatp bare: guest-virtual is guest-physical
  csrw 0x680, zero
  ENTERG vs_bare_g, 1
  csrw 0x280, zero
  ENTERG vs_bare_g, 1
  # Svade in the guest: a clear A bit is a fault unless henvcfg.ADUE
  csrw medeleg, zero
  csrw 0x602, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

vs_translate:
  li t5, 0x40000000
  ld t2, 0(t5)                   # fine through both stages
  sd t2, 0(t5)
  li t5, 0x40001000
  ld t2, 0(t5)                   # VS invalid: page fault to the guest
  li t5, 0x40002000
  ld t2, 0(t5)                   # G invalid: guest page fault (htval = the guest-physical address >> 2)
  li t5, 0x40003000
  ld t2, 0(t5)                   # G read-only: load fine
  sd t2, 0(t5)                   # store: guest page fault 23
  li t5, 0x40004000
  ld t2, 0(t5)                   # G without U: guest page fault
  li t5, 0x40005000
  ld t2, 0(t5)                   # VS execute-only: page fault (MXR clear)
  li t5, 0x40006000
  ld t2, 0(t5)                   # VS A clear: page fault
  li t5, 0x40008000
  ld t2, 0(t5)                   # VS user page from VS without SUM: page fault
  li t5, 0xC0000000
  ld t2, 0(t5)                   # the walk itself needs an unmapped guest-physical page: guest page fault with htval of the PTE
  sd t2, 0(t5)
  li t5, 0x2000000000
  ld t2, 0(t5)                   # beyond the VS-stage's reach: page fault
  li t5, 0x40007000
  ld t2, 0(t5)                   # G execute-only: guest page fault (MXR clear)
  li t5, 0x40000FFC
  ld t2, 0(t5)                   # a load that crosses into the invalid page
  ecall

vs_translate2:                    # SUM and MXR in vsstatus
  li t5, 0x40008000
  ld t2, 0(t5)                   # user page, SUM: fine
  sd t2, 0(t5)
  li t5, 0x40005000
  ld t2, 0(t5)                   # VS execute-only, MXR: VS-stage fine, G-stage RW fine
  li t5, 0x40007000
  ld t2, 0(t5)                   # G execute-only: vsstatus.MXR does not help the G-stage
  ecall

vs_translate3:                    # mstatus.MXR makes the G-stage page readable
  li t5, 0x40007000
  ld t2, 0(t5)
  ecall

vs_bare_g:
  li t5, 0x80000000
  ld t2, 0(t5)
  ecall

test_hlv:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'Y'
  SETUP_GUEST
  li t0, (1 << 12) | (1 << 13) | (1 << 15) | (1 << 20) | (1 << 21) | (1 << 23) | (1 << 2)
  csrw medeleg, t0
  li t0, 0x100
  csrs 0x600, t0                 # hstatus.SPVP: the accesses are made as VS
  ENTER hs_hlv, 1
  li t0, 0x100
  csrc 0x600, t0                 # as VU: pages without U are faults
  ENTER hs_hlv_user, 1
  li t0, 0x200
  csrs 0x600, t0                 # hstatus.HU: U mode may use hlv / hsv
  ENTER u_hlv, 0
  li t0, 0x200
  csrc 0x600, t0
  ENTER u_hlv, 0
  ENTERG vs_hlv, 1
  csrw medeleg, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

hs_hlv:
  li t5, 0x40000000
  li t6, 0x1122334455667788
  hsv.d t6, (t5)
  hlv.b t0, (t5)
  hlv.bu t1, (t5)
  hlv.h t2, (t5)
  hlv.hu t3, (t5)
  hlv.w t4, (t5)
  hlv.wu t6, (t5)
  hlv.d s2, (t5)
  PRINT t0
  PRINT t1
  PRINT t2
  PRINT t3
  PRINT t4
  PRINT t6
  PRINT s2
  hsv.b t0, (t5)
  hsv.h t0, (t5)
  hsv.w t0, (t5)
  li t5, 0x40007000
  hlvx.hu t0, (t5)               # execute permission is what it needs
  hlvx.wu t0, (t5)
  hlv.w t0, (t5)                 # no read permission in the G-stage: guest page fault
  li t5, 0x40001000
  hlv.w t0, (t5)                 # VS invalid: page fault (to HS, V = 0)
  li t5, 0x40002000
  hlv.w t0, (t5)                 # G invalid
  li t5, 0x40003000
  hsv.w t0, (t5)                 # G read-only
  li t5, 0x40008000
  hlv.w t0, (t5)                 # a user page as VS: no SUM
  li t0, 1 << 18
  csrs sstatus, t0
  hlv.w t0, (t5)                 # HS's sstatus.SUM does not apply: the guest's does
  csrc sstatus, t0
  li t5, 0x40000001
  hlv.w t0, (t5)                 # misaligned
  li t5, 0x40005000
  hlv.w t0, (t5)
  li t0, 1 << 19
  csrs sstatus, t0
  hlv.w t0, (t5)                 # sstatus.MXR applies to both stages
  csrc sstatus, t0
  ecall

hs_hlv_user:
  li t5, 0x40000000
  hlv.w t0, (t5)                 # fine
  li t5, 0x40008000
  hlv.w t0, (t5)                 # a user page as VU: fine
  li t5, 0x40002000
  hlv.w t0, (t5)
  ecall

u_hlv:
  li t5, 0x40000000
  hlv.w t0, (t5)                 # U mode: illegal unless hstatus.HU
  hsv.w t0, (t5)
  ecall

vs_hlv:
  li t5, 0x40000000
  hlv.w t0, (t5)                 # virtual instruction
  hsv.w t0, (t5)
  hfence.vvma
  hfence.gvma
  ecall

