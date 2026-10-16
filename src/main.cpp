#include "arch/x86_64/cpu.hpp"
#include "console.hpp"
#include "uefi.hpp"

// gdb: `set var g_debugger_attached = 1` releases the boot-time spin loop.
extern "C" {
volatile int g_debugger_attached = 0;
// Exported so gdb/scripts can locate the image without symbols.
uint64_t g_image_base = 0;
}

static void kernel_main(efi::SystemTable* st) {
  console::puts("Hello from TheoryCraftOS!\n");
  console::puts("Firmware revision: ");
  console::put_hex(st->hdr.revision);
  console::puts("\n");

  console::puts("Press any key to shut down.\n");
  efi::InputKey key;
  while (st->con_in->read_key_stroke(st->con_in, &key) != efi::kSuccess)
    st->boot_services->stall(10000);
}

extern "C" efi::Status efi_main(efi::Handle image, efi::SystemTable* st) {
  console::init(st);
  st->boot_services->set_watchdog_timer(0, 0, 0, nullptr);  // no 5-minute reset

  efi::LoadedImage* li = nullptr;
  st->boot_services->handle_protocol(image, &efi::kLoadedImageGuid,
                                     reinterpret_cast<void**>(&li));
  if (li) g_image_base = reinterpret_cast<uint64_t>(li->image_base);

  // Line parsed by scripts/tcos.gdb to relocate symbols.
  console::puts("TCOS_IMAGE_BASE=");
  console::put_hex(g_image_base);
  console::puts("\n");

#ifdef TCOS_DEBUG_WAIT
  // Gives a debugger time to attach (IDE) or lets you flip the flag by hand.
  console::puts("Waiting for debugger...\n");
  for (int i = 0; i < TCOS_DEBUG_WAIT * 10 && !g_debugger_attached; ++i)
    st->boot_services->stall(100000);
#endif

  kernel_main(st);

  st->runtime_services->reset_system(efi::ResetType::Shutdown, efi::kSuccess, 0, nullptr);
  for (;;) tcos_halt();
}
