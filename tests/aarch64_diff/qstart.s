.text
.globl _start
_start:
  mov x29, #0
  bl c_main
1: b 1b
.globl call_word
call_word:
  stp x29, x30, [sp, #-96]!
  stp x19, x20, [sp, #16]
  stp x21, x22, [sp, #32]
  stp x23, x24, [sp, #48]
  stp x25, x26, [sp, #64]
  stp x27, x28, [sp, #80]
  mov x9, sp
  adrp x10, saved_sp
  str x9, [x10, :lo12:saved_sp]
  br x0
.globl cont
cont:
  adrp x10, saved_sp
  ldr x9, [x10, :lo12:saved_sp]
  mov sp, x9
  ldp x19, x20, [sp, #16]
  ldp x21, x22, [sp, #32]
  ldp x23, x24, [sp, #48]
  ldp x25, x26, [sp, #64]
  ldp x27, x28, [sp, #80]
  ldp x29, x30, [sp], #96
  ret
.bss
saved_sp: .skip 8
