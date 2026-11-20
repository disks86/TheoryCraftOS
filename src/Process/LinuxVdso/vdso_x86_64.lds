/* The layout of the x86-64 vDSO: one read+execute segment from address 0 (file offset = address, so the kernel maps the file as it is), the data page
   (tcos_vvar) in the page below it, and the symbol version LINUX_2.6 glibc and musl ask for. Modelled on Linux's vdso-layout.lds. */
SECTIONS {
  tcos_vvar = . - 4096;
  . = SIZEOF_HEADERS;
  .hash : { *(.hash) } :text
  .gnu.hash : { *(.gnu.hash) }
  .dynsym : { *(.dynsym) }
  .dynstr : { *(.dynstr) }
  .gnu.version : { *(.gnu.version) }
  .gnu.version_d : { *(.gnu.version_d) }
  .gnu.version_r : { *(.gnu.version_r) }
  .dynamic : { *(.dynamic) } :text :dynamic
  .rodata : { *(.rodata*) } :text
  .note : { *(.note.*) } :text :note
  .eh_frame_hdr : { *(.eh_frame_hdr) } :text :eh_frame_hdr
  .eh_frame : { KEEP (*(.eh_frame)) } :text
  .text : { *(.text*) } :text =0xCCCCCCCC
  .got : { *(.got) *(.got.plt) } :text      /* (empty: GNU ld makes it whether or not anything uses it) */
  /DISCARD/ : { *(.data*) *(.bss*) *(.plt*) *(.rela*) *(.comment) *(.note.GNU-stack) *(.note.gnu.property) }
}
PHDRS {
  text PT_LOAD FLAGS(5) FILEHDR PHDRS;
  dynamic PT_DYNAMIC FLAGS(4);
  note PT_NOTE FLAGS(4);
  eh_frame_hdr PT_GNU_EH_FRAME;
}
VERSION {
  LINUX_2.6 {
    global:
      clock_gettime; __vdso_clock_gettime;
      gettimeofday; __vdso_gettimeofday;
      time; __vdso_time;
      clock_getres; __vdso_clock_getres;
    local: *;
  };
}
