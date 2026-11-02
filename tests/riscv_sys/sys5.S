# Privileged-architecture test payload, part 5: the hypervisor extension (CSRs, virtualization mode, traps, virtual-instruction exceptions, interrupts). See common.inc.
.include "common.inc"

run_tests:
  addi sp, sp, -16
  sd ra, 0(sp)
  call test_h_csrs
  call test_h_traps
  call test_h_virtual_instructions
  call test_h_interrupts
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

test_h_csrs:
  addi sp, sp, -16
  sd ra, 0(sp)
  CSRPROBE 0x600, 0x7003C0       # hstatus
  CSRPROBE 0x602, 0xFFFF         # hedeleg
  CSRPROBE 0x603, 0x444          # hideleg
  CSRPROBE 0x605                 # htimedelta
  CSRPROBE 0x606                 # hcounteren
  CSRPROBE 0x607                 # hgeie
  CSRPROBE 0x60A                 # henvcfg
  CSRPROBE 0x643                 # htval
  CSRPROBE 0x644, 0x1444         # hip
  CSRPROBE 0x645                 # hvip
  CSRPROBE 0x64A                 # htinst
  CSRPROBE 0x680                 # hgatp
  CSRPROBE 0xE12                 # hgeip
  CSRPROBE 0x34A                 # mtinst
  CSRPROBE 0x34B                 # mtval2
  CSRPROBE 0x200, 0xC6722        # vsstatus
  CSRPROBE 0x204                 # vsie
  CSRPROBE 0x205                 # vstvec
  CSRPROBE 0x240                 # vsscratch
  CSRPROBE 0x241, 0xFFFFFFFFFFFFFFFE      # vsepc
  CSRPROBE 0x242                 # vscause
  CSRPROBE 0x243                 # vstval
  CSRPROBE 0x244                 # vsip
  CSRPROBE 0x24D                 # vstimecmp
  CSRPROBE 0x280                 # vsatp
  CSRPROBE mideleg, 0x1FFF
  CSRPROBE mie, 0x1FFF
  CSRPROBE mip, 0x1FFF
  CSRPROBE medeleg, 0xFBFFFF
  # hideleg gates vsie and vsip
  li t0, 0x444
  csrw 0x603, t0
  li t0, 0x1444
  csrw mie, t0
  csrr t1, 0x204
  PRINT t1
  csrr t1, mie
  PRINT t1
  li t0, 0x222
  csrw 0x204, t0
  csrr t1, mie
  PRINT t1
  csrw 0x603, zero
  csrw mie, zero
  # hvip shows in hip, vsip and mip
  li t0, 0x444
  csrw 0x645, t0
  csrr t1, 0x644
  PRINT t1
  csrr t1, mip
  PRINT t1
  li t0, 0x444
  csrw 0x603, t0
  csrr t1, 0x244
  PRINT t1
  csrw 0x603, zero
  csrw 0x645, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

test_h_traps:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'H'
  la t0, strap
  csrw stvec, t0
  la t0, strap_stack_top
  csrw sscratch, t0
  la t0, vstrap
  csrw 0x205, t0                 # vstvec
  la t0, vstrap_stack_top
  csrw 0x240, t0                 # vsscratch
  # the guest: VS mode traps to machine mode (nothing delegated)
  ENTERG vs_code1, 1
  ENTERG vu_code1, 0
  # delegate to HS: illegal instruction, ecall from VS, breakpoint
  li t0, (1 << 2) | (1 << 3) | (1 << 10) | (1 << 8)
  csrw medeleg, t0
  la t0, s_final
  li t1, 1
  sd t1, 0(t0)
  ENTERG vs_code2, 1
  ENTERG vu_code2, 0
  # on to the guest's own handler: hedeleg
  li t0, (1 << 2) | (1 << 3) | (1 << 8)
  csrw 0x602, t0
  ENTERG vs_code3, 1
  ENTERG vu_code3, 0
  csrw 0x602, zero
  csrw medeleg, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

vs_code1:
  .word 0xFFFFFFFF               # illegal: to machine mode, MPV set
  ebreak
  csrr t0, mstatus               # machine CSR: illegal, not virtual
  csrr t0, 0x600                 # hypervisor CSR: virtual instruction
  mret                           # illegal
  ecall
vu_code1:
  .word 0xFFFFFFFF
  csrr t0, sstatus               # a supervisor CSR from VU: virtual instruction
  csrr t0, cycle                 # mcounteren is 0: illegal
  ecall

vs_code2:
  .word 0xFFFFFFFF               # to HS: hstatus.SPV, SPVP
  ebreak
  ecall
vu_code2:
  .word 0xFFFFFFFF
  ecall

vs_code3:
  .word 0xFFFFFFFF               # to VS itself (vstvec = vstrap)
  ebreak
  csrr t0, sstatus               # the guest's sstatus (vsstatus)
  csrr t0, sepc
  csrr t0, scause
  ecall
vu_code3:
  .word 0xFFFFFFFF
  ebreak
  ecall

test_h_virtual_instructions:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'W'
  # no delegation: everything lands in machine mode, whose log tells illegal (2) from virtual (22)
  li t0, 0x700000
  csrc 0x600, t0                 # hstatus.VTVM, VTW, VTSR clear
  ENTERG vs_code4, 1
  csrs 0x600, t0
  ENTERG vs_code4, 1
  li t0, 0x200000
  csrc mstatus, t0               # mstatus.TW = 0
  li t0, 0x200000
  csrs mstatus, t0
  ENTERG vs_code5, 1
  li t0, 0x200000
  csrc mstatus, t0
  li t0, 7
  csrw mcounteren, t0
  csrw scounteren, t0
  csrw 0x606, zero
  ENTERG vs_code6, 1
  ENTERG vu_code6, 0
  li t0, 7
  csrw 0x606, t0
  ENTERG vs_code6, 1
  csrw mcounteren, zero
  csrw scounteren, zero
  csrw 0x606, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

vs_code4:                        # wfi / sret / sfence.vma / satp, with and without the VT* bits
  sfence.vma
  csrr t0, satp
  sret
  ecall
vs_code5:                        # mstatus.TW set: wfi is illegal in the guest too (it would be virtual otherwise)
  wfi
  ecall
vs_code6:                        # the counters
  csrr t0, cycle
  csrr t0, time
  csrr t0, instret
  ecall
vu_code6:
  csrr t0, cycle
  csrr t0, time
  ecall

test_h_interrupts:
  addi sp, sp, -16
  sd ra, 0(sp)
  TAG 'J'
  la t0, vstrap
  csrw 0x205, t0
  la t0, strap
  csrw stvec, t0
  # a guest software interrupt: hvip.VSSIP with hideleg, vsie.SSIE and vsstatus.SIE set, taken in VS
  li t0, 0x444
  csrw 0x603, t0
  csrw 0x645, t0
  li t0, 0x1444
  csrw mie, t0
  li t0, 2
  csrs 0x200, t0                 # vsstatus.SIE
  la t0, irq_count
  sd zero, 0(t0)
  ENTERG vs_wait, 1
  # the same with vsstatus.SIE clear: not taken in VS, but VU takes it whatever SIE says
  li t0, 2
  csrc 0x200, t0
  la t0, irq_count
  sd zero, 0(t0)
  ENTERG vu_wait, 0
  # hideleg clear: the VS interrupts are HS-level ones now and are taken in HS (by S mode, as the hypervisor)
  csrw 0x603, zero
  la t0, irq_count
  sd zero, 0(t0)
  li t0, 0x222
  csrw mideleg, t0
  ENTERG vs_wait, 1
  csrw mideleg, zero
  csrw 0x645, zero
  csrw mie, zero
  csrw 0x603, zero
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

vs_wait:
  la t0, irq_count
1:
  ld t1, 0(t0)
  beqz t1, 1b
  ecall
vu_wait:
  la t0, irq_count
1:
  ld t1, 0(t0)
  beqz t1, 1b
  ecall

.section .data
.balign 16
.space 4096
vstrap_stack_top:
.space 16
