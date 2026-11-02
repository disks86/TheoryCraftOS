# Shared by the hypervisor payloads: PTE macros, the G-stage and VS-stage tables that sys6.S and sys7.S use, and the macro that points vsatp / hgatp at them.
.equ PTE_V, 1
.equ PTE_R, 2
.equ PTE_W, 4
.equ PTE_X, 8
.equ PTE_U, 16
.equ PTE_A, 0x40
.equ PTE_D, 0x80
.equ ALL, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D
.macro PTE dst, addr, flags
  la \dst, \addr
  srli \dst, \dst, 12
  slli \dst, \dst, 10
  ori \dst, \dst, \flags
.endm
.macro SETPTE table, index, addr, flags
  PTE t0, \addr, \flags
  la t1, \table
  sd t0, \index * 8(t1)
.endm

build_tables:
  # G-stage: GPA [0,1G) and [2G,3G) identity (user pages), [1G,2G) through gl1 -> gl0
  la t1, groot
  li t0, 0xD7
  sd t0, 0(t1)
  SETPTE groot, 1, gl1, PTE_V
  li t0, 0x200000DF
  sd t0, 16(t1)
  SETPTE gl1, 0, gl0, PTE_V
  SETPTE gl0, 0, data0, ALL | PTE_U
  SETPTE gl0, 1, data0, ALL | PTE_U
  # 2: invalid
  SETPTE gl0, 3, data0, PTE_V | PTE_R | PTE_A | PTE_D | PTE_U
  SETPTE gl0, 4, data0, ALL                               # no U bit
  SETPTE gl0, 5, data0, ALL | PTE_U
  SETPTE gl0, 6, data0, PTE_V | PTE_R | PTE_W | PTE_D | PTE_U
  SETPTE gl0, 7, data0, PTE_V | PTE_X | PTE_A | PTE_U
  SETPTE gl0, 8, data0, ALL | PTE_U
  SETPTE gl0, 9, data0, ALL | PTE_U
  # VS-stage: VA [0,1G) and [2G,3G) identity supervisor pages, [1G,2G) through vl1 -> vl0, [3G,4G) a pointer into guest-physical space that has no mapping
  la t1, vroot
  li t0, 0xC7
  sd t0, 0(t1)
  SETPTE vroot, 1, vl1, PTE_V
  li t0, 0x200000CF
  sd t0, 16(t1)
  li t0, 0x100000
  slli t0, t0, 10
  ori t0, t0, PTE_V
  sd t0, 24(t1)
  SETPTE vl1, 0, vl0, PTE_V
  SETPTE vl0, 0, data0, ALL
  # 1: invalid
  SETPTE vl0, 2, data0, ALL
  SETPTE vl0, 3, data0, ALL
  SETPTE vl0, 4, data0, ALL
  SETPTE vl0, 5, data0, PTE_V | PTE_X | PTE_A
  SETPTE vl0, 6, data0, PTE_V | PTE_R | PTE_W | PTE_D
  SETPTE vl0, 7, data0, ALL
  SETPTE vl0, 8, data0, ALL | PTE_U
  SETPTE vl0, 9, data0, ALL
  ret

.macro SETUP_GUEST
  la t0, vroot
  srli t0, t0, 12
  li t1, 8
  slli t1, t1, 60
  or t0, t0, t1
  csrw 0x280, t0                 # vsatp
  la t0, groot
  srli t0, t0, 12
  li t1, 8
  slli t1, t1, 60
  or t0, t0, t1
  csrw 0x680, t0                 # hgatp
  la t0, strap
  csrw stvec, t0
  la t0, strap_stack_top
  csrw sscratch, t0
  la t0, vstrap
  csrw 0x205, t0
  la t0, vstrap_stack_top
  csrw 0x240, t0
.endm

.section .data
.balign 4096
data0: .space 4096
.balign 4096
vroot: .space 4096
vl1: .space 4096
vl0: .space 4096
.balign 16384
groot: .space 16384
gl1: .space 4096
gl0: .space 4096
.balign 16
.space 4096
vstrap_stack_top:
.space 16
