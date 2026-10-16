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

.global tcos_outw
tcos_outw:
    mov     ax, dx
    mov     dx, cx
    out     dx, ax
    ret

.global tcos_inw
tcos_inw:
    mov     dx, cx
    xor     eax, eax
    in      ax, dx
    ret

.global tcos_outl
tcos_outl:
    mov     eax, edx
    mov     dx, cx
    out     dx, eax
    ret

.global tcos_inl
tcos_inl:
    mov     dx, cx
    in      eax, dx
    ret
