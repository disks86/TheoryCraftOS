/* x86 port I/O for IoPort. Microsoft x64 ABI: args in rcx, rdx; result in rax. */
.intel_syntax noprefix
.text

.global tcos_outb
tcos_outb:
    mov     al, dl
    mov     dx, cx
    out     dx, al
    ret

.global tcos_inb
tcos_inb:
    mov     dx, cx
    xor     eax, eax
    in      al, dx
    ret
