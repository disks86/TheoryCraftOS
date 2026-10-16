/* x86_64 CPU instructions wrapped for X86_64Cpu (Microsoft x64 ABI). */
.intel_syntax noprefix
.text

.global tcos_halt
tcos_halt:
    hlt
    ret

.global tcos_pause
tcos_pause:
    pause
    ret

.global tcos_breakpoint
tcos_breakpoint:
    int3
    ret
