/* Link layout of the RISC-V UEFI image. The ELF is linked as a position-independent executable at address 0 and
 * scripts/elf2efi.py turns it into a PE32+ file: one PE section per output section below, each starting on its
 * own 4 KiB page (the PE SectionAlignment), page 0 left for the PE headers. */
ENTRY(efi_main)

SECTIONS
{
  . = 0x1000;
  .text : ALIGN(4096) { *(.text .text.*) }
  .rodata : ALIGN(4096) { *(.rodata .rodata.* .srodata .srodata.*) }
  .data : ALIGN(4096) {
    *(.data .data.* .sdata .sdata.*)
    *(.got .got.*)
    *(.init_array .init_array.* .fini_array .fini_array.*)
  }
  .bss : ALIGN(4096) { *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) }

  /* The dynamic-linking tables only carry the relative relocations to elf2efi.py; they are not loaded. */
  .dynamic : { *(.dynamic) }
  .dynsym : { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  .hash : { *(.hash) }
  .gnu.hash : { *(.gnu.hash) }
  .rela.dyn : { *(.rela.dyn .rela.*) }

  /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.riscv.attributes) }
}
