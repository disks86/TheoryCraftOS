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

.global tcos_disable_interrupts
tcos_disable_interrupts:
    cli
    ret

.global tcos_enable_interrupts
tcos_enable_interrupts:
    sti
    ret

.global tcos_rdtsc
tcos_rdtsc:
    rdtsc
    shl     rdx, 32
    or      rax, rdx
    ret
