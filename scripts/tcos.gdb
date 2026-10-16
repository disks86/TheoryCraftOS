# Usage (after `cmake --build build --target debug` in another terminal,
# with the project configured using -DTCOS_DEBUG_WAIT=ON):
#   gdb -x scripts/tcos.gdb
#
# OVMF relocates the image, so symbols are loaded at the base the OS prints
# ("TCOS_IMAGE_BASE=0x...") to build/debugcon.log.
set pagination off
set confirm off
set disassembly-flavor intel
set architecture i386:x86-64

define tcos-symbols
  shell sed -n 's/.*TCOS_IMAGE_BASE=\(0x[0-9a-f]*\).*/set $tcos_base = \1/p' build/debugcon.log | tail -1 > build/.tcos_base.gdb
  source build/.tcos_base.gdb
  # .text is the first section, one 0x1000 page after the PE headers.
  add-symbol-file build/BOOTX64.efi $tcos_base+0x1000
  printf "symbols loaded for image base %#lx\n", $tcos_base
end
document tcos-symbols
Load BOOTX64.efi symbols at the base address the OS logged.
end

target remote :1234
tcos-symbols
set var g_debugger_attached = 1
break kernel_main
continue
