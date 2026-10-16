#include "Allocators/Allocator.hpp"
#include "CPUScheduler/Scheduler.hpp"
#include "console.hpp"
#include "hardware/SystemHardware.hpp"
#include "uefi.hpp"

// gdb: `set var g_debugger_attached = 1` releases the boot-time spin loop.
extern "C" {
volatile int g_debugger_attached = 0;
// Exported so gdb/scripts can locate the image without symbols.
uint64_t g_image_base = 0;
}

static void log_size(const char* label, uint64_t bytes) {
  console::puts(label);
  console::put_hex(bytes);
  console::puts(" bytes\n");
}

static void hello_task(void*) { console::puts("Scheduler ran a task.\n"); }

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

  // Bring-up order: CPU -> RAM -> allocator -> scheduler. Objects live on this
  // stack for the life of the OS; nothing needs static constructors.
  hardware::SystemCpu cpu;
  cpu.initialize();
  hardware::SystemRam ram(st);
  if (!ram.initialize()) {
    console::puts("RAM initialization failed\n");
    return efi::kBufferTooSmall;
  }
  allocators::Allocator allocator(ram);
  if (!allocator.initialize()) {
    console::puts("Allocator initialization failed\n");
    return efi::kBufferTooSmall;
  }
  scheduling::Scheduler scheduler(cpu, allocator);

  console::puts(cpu.name());
  console::puts(" ready\n");
  log_size("RAM total:  ", ram.total_bytes());
  log_size("RAM pool:   ", ram.usable_bytes());
  log_size("Allocator free: ", allocator.free_bytes());

  scheduler.submit(hello_task, nullptr);
  scheduler.run();

  kernel_main(st);

  allocator.deallocate(allocator.allocate(1));  // exercise the pool once
  ram.shutdown();

  st->runtime_services->reset_system(efi::ResetType::Shutdown, efi::kSuccess, 0, nullptr);
  for (;;) cpu.halt();
}
