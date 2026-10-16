#include "Allocators/Allocator.hpp"
#include "CPUScheduler/Scheduler.hpp"
#include "console.hpp"
#include "hardware/Bus/SystemPciBus.hpp"
#include "hardware/Bus/Usb/UsbBus.hpp"
#include "hardware/GraphicsProcessingUnit/SystemGpu.hpp"
#include "hardware/SystemHardware.hpp"
#include "hardware/SystemInput.hpp"
#include "hardware/Keyboard/KeyCodes.hpp"
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

// Scheduler demo. Tasks may run on other cores, where firmware (and so the
// console) is off limits, so they only touch memory; the boot core reports.
struct DemoState {
  hardware::ICentralProcessingUnit* cpu;
  uint32_t done;
  uint64_t cores_used;  // bit n set => core n ran a task
};

static void demo_task(void* context) {
  auto* state = static_cast<DemoState*>(context);
  for (volatile int i = 0; i < 2000000; i = i + 1) {}  // long enough for tasks to overlap
  __atomic_fetch_or(&state->cores_used, 1ull << state->cpu->current_core(), __ATOMIC_SEQ_CST);
  __atomic_fetch_add(&state->done, 1, __ATOMIC_SEQ_CST);
}

static void print_key(const hardware::KeyEvent& key, uint8_t modifiers) {
  console::puts("KEY ");
  console::put_hex(key.usage);
  console::puts(key.pressed ? " down" : " up");
  if (char c = hardware::key_to_ascii(key.usage, modifiers)) {
    const char text[2] = {c, 0};
    console::puts(" '");
    console::puts(c == '\n' ? "\\n" : c == '\b' ? "\\b" : c == '\t' ? "\\t" : c == 0x1b ? "esc" : text);
    console::puts("'");
  }
  console::puts("\n");
}

static void print_mouse(const hardware::MouseEvent& m) {
  if (m.absolute) {
    console::puts("TABLET x=");
    console::put_dec(m.x);
    console::puts("/");
    console::put_dec(m.max_x);
    console::puts(" y=");
    console::put_dec(m.y);
    console::puts("/");
    console::put_dec(m.max_y);
  } else {
    console::puts("MOUSE dx=");
    console::put_dec(m.dx);
    console::puts(" dy=");
    console::put_dec(m.dy);
  }
  console::puts(" wheel=");
  console::put_dec(m.wheel);
  console::puts(" buttons=");
  console::put_dec(m.buttons);
  console::puts("\n");
}

// Shows keyboard and mouse events until 'q' is pressed. With no keyboard at all
// it falls back to the firmware's console input.
static void kernel_main(efi::SystemTable* st, hardware::SystemInput& input) {
  console::puts("Hello from TheoryCraftOS!\n");
  console::puts("Firmware revision: ");
  console::put_hex(st->hdr.revision);
  console::puts("\n");

  if (input.keyboards().count() == 0) {
    console::puts("No keyboard found; press any key (firmware input) to shut down.\n");
    efi::InputKey key;
    while (st->con_in->read_key_stroke(st->con_in, &key) != efi::kSuccess)
      st->boot_services->stall(10000);
    return;
  }

  console::puts("INPUT READY: type, move the mouse; press q to shut down.\n");
  for (;;) {
    hardware::KeyEvent key;
    while (input.keyboard().poll(&key)) {
      print_key(key, input.keyboard().modifiers());
      if (key.pressed && key.usage == hardware::key::kA + ('q' - 'a')) return;
    }
    hardware::MouseEvent mouse;
    while (input.mouse().poll(&mouse)) print_mouse(mouse);
    st->boot_services->stall(5000);
  }
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

  // Display first, so everything after it appears on screen. Falls back to the
  // firmware text console if no GPU class can drive a framebuffer.
  hardware::SystemPciBus system_pci(st);
  hardware::IPciBus& pci = system_pci.pci();
  hardware::SystemGpu system_gpu(st, pci);
  hardware::IGraphicsProcessingUnit* gpu = system_gpu.gpu();
  if (gpu) {
    // Mode setting demo: switch to 1024x768 if the GPU offers it. The screen is
    // cleared by a mode change, so do it before anything is printed.
    for (uint32_t i = 0; i < gpu->mode_count(); ++i) {
      hardware::DisplayMode m;
      if (gpu->mode_info(i, &m) && m.width == 1024 && m.height == 768 && gpu->set_mode(i)) break;
    }
    console::use_gpu(gpu);
    console::puts("GPU: ");
    console::puts(gpu->name());
    console::puts("\n");
    console::puts("Display: ");
    console::put_hex(gpu->width());
    console::puts(" x ");
    console::put_hex(gpu->height());
    console::puts(" (");
    console::put_hex(gpu->mode_count());
    console::puts(" modes)\n");
  }

  console::puts(pci.name());
  console::puts(": ");
  console::put_hex(pci.device_count());
  console::puts(" devices\n");

  // Bring-up order: CPU -> RAM -> allocator -> scheduler. Objects live on this
  // stack for the life of the OS; nothing needs static constructors.
  hardware::SystemCpu system_cpu(st);
  hardware::ICentralProcessingUnit& cpu = system_cpu.cpu();
  if (!cpu.initialize()) {
    console::puts("CPU initialization failed\n");
    return efi::kBufferTooSmall;
  }
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

  // USB comes after the allocator: the xHCI driver needs DMA-able memory.
  hardware::UsbBus usb(pci, allocator, st);
  usb.initialize();
  console::puts("USB: ");
  console::put_hex(usb.controller_count());
  console::puts(" controllers, ");
  console::put_hex(usb.device_count());
  console::puts(" devices\n");
  for (uint32_t c = 0; c < usb.controller_count(); ++c) {
    for (uint32_t p = 0; p < usb.port_count(c); ++p) {
      hardware::UsbDeviceDescriptor d;
      if (!usb.device_descriptor(c, p, &d)) continue;
      console::puts("  port ");
      console::put_hex(p);
      console::puts(": device ");
      console::put_hex(d.vendor_id);
      console::puts(":");
      console::put_hex(d.product_id);
      console::puts(" class ");
      console::put_hex(d.device_class);
      console::puts("\n");
    }
  }

  console::puts(cpu.name());
  console::puts(": ");
  console::puts(cpu.model());
  console::puts("\n");
  console::puts("Cores: ");
  console::put_hex(cpu.core_count());
  console::puts(" (");
  uint32_t efficiency = 0;
  for (uint32_t i = 0; i < cpu.core_count(); ++i)
    efficiency += cpu.core_kind(i) == hardware::CoreKind::Efficiency;
  console::put_hex(efficiency);
  console::puts(" efficiency)\n");
  log_size("RAM total:  ", ram.total_bytes());
  log_size("RAM pool:   ", ram.usable_bytes());
  log_size("Allocator free: ", allocator.free_bytes());

  DemoState demo{&cpu, 0, 0};
  for (int i = 0; i < 8; ++i) scheduler.submit(demo_task, &demo);
  scheduler.run();
  console::puts("Scheduler finished tasks: ");
  console::put_hex(demo.done);
  console::puts(", core mask: ");
  console::put_hex(demo.cores_used);
  console::puts("\n");

  // Input devices: PS/2, USB HID and virtio-input, merged into one keyboard and mouse.
  hardware::SystemInput input(pci, usb, allocator, st);
  input.initialize();
  console::puts("Input: ");
  console::put_dec(input.keyboards().count());
  console::puts(" keyboards, ");
  console::put_dec(input.mice().count());
  console::puts(" mice\n");
  for (uint32_t i = 0; i < input.keyboards().count(); ++i) {
    console::puts("  ");
    console::puts(input.keyboards().at(i)->name());
    console::puts("\n");
  }
  for (uint32_t i = 0; i < input.mice().count(); ++i) {
    console::puts("  ");
    console::puts(input.mice().at(i)->name());
    console::puts("\n");
  }

  // The firmware polls PS/2 from its timer interrupt and would steal our bytes.
  cpu.disable_interrupts();
  kernel_main(st, input);
  cpu.enable_interrupts();
  input.shutdown();
  usb.shutdown();  // resets the USB controllers and returns them to the firmware

  allocator.deallocate(allocator.allocate(1));  // exercise the pool once
  ram.shutdown();
  cpu.shutdown();

  st->runtime_services->reset_system(efi::ResetType::Shutdown, efi::kSuccess, 0, nullptr);
  for (;;) cpu.halt();
}
