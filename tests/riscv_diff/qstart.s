.text
.globl _start
_start:
  li s0, 0
  call c_main
1: j 1b
.globl call_word
call_word:
  addi sp, sp, -112
  sd ra, 0(sp)
  sd s0, 8(sp)
  sd s1, 16(sp)
  sd s2, 24(sp)
  sd s3, 32(sp)
  sd s4, 40(sp)
  sd s5, 48(sp)
  sd s6, 56(sp)
  sd s7, 64(sp)
  sd s8, 72(sp)
  sd s9, 80(sp)
  sd s10, 88(sp)
  sd s11, 96(sp)
  la t0, saved_sp
  sd sp, 0(t0)
  jr a0
.globl cont
cont:
  la t0, saved_sp
  ld sp, 0(t0)
  ld ra, 0(sp)
  ld s0, 8(sp)
  ld s1, 16(sp)
  ld s2, 24(sp)
  ld s3, 32(sp)
  ld s4, 40(sp)
  ld s5, 48(sp)
  ld s6, 56(sp)
  ld s7, 64(sp)
  ld s8, 72(sp)
  ld s9, 80(sp)
  ld s10, 88(sp)
  ld s11, 96(sp)
  addi sp, sp, 112
  ret
.data
saved_sp: .quad 0
