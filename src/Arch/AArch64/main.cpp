/*
 * TheoryCraftOS - a minimal UEFI operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// UEFI entry point of the AArch64 image (BOOTAA64.EFI). The portable layers (console, RAM, allocator,
// framebuffer, ACPI tables) come up here; the x86-only subsystems (PCI port access, interrupts, APIC,
// threads, drivers) are not part of this image yet.
#include "Allocators/Allocator.hpp"
#include "Allocators/ThreadSafeAllocator.hpp"
#include "Arch/AArch64/Cpu.hpp"
#include "Arch/AArch64/Psci.hpp"
#include "Arch/AArch64/Gic.hpp"
#include "Arch/AArch64/Spcr.hpp"
#include "BootLog.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/DebugConsole.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpuMemory.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dSelfTest.hpp"
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"
#include "Hardware/Platform/RaspberryPi/IdentityRegisterMapper.hpp"
#include "Hardware/Platform/RaspberryPi/RaspberryPiDetect.hpp"
#include "Hardware/Platform/RaspberryPi/RaspberryPiPlatform.hpp"
#include "Hardware/VideoCodec/Broadcom/RpiHevcBlock.hpp"
#include "Hardware/SystemHardware.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "console.hpp"

using bootlog::Entry;
using bootlog::Level;

namespace {
uint64_t read_sysreg_midr() {
  uint64_t v;
  __asm__ volatile("mrs %0, midr_el1" : "=r"(v));
  return v;
}
uint64_t read_current_el() {
  uint64_t v;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
  return (v >> 2) & 3;
}
uint64_t read_cntfrq() {
  uint64_t v;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
  return v;
}

const char* implementer(uint32_t id) {
  switch (id) {
    case 0x41: return "ARM";
    case 0x42: return "Broadcom";
    case 0x43: return "Cavium";
    case 0x46: return "Fujitsu";
    case 0x51: return "Qualcomm";
    case 0x61: return "Apple";
    case 0x69: return "Intel";
    case 0xC0: return "Ampere";
    default: return "unknown";
  }
}
}  // namespace

namespace {
// Boot-time check of the scheduler: two threads count while the main thread sleeps, then a mutex-guarded
// counter shows both ran in parallel with preemption (neither yields by itself).
struct Worker {
  volatile uint64_t spins = 0;
  volatile bool stop = false;
};
void spin_worker(void* argument) {
  Worker* w = static_cast<Worker*>(argument);
  while (!w->stop) w->spins = w->spins + 1;
}
struct Shared {
  kernel::Mutex mutex;
  uint64_t counter = 0;
};
void mutex_worker(void* argument) {
  Shared* shared = static_cast<Shared*>(argument);
  for (uint32_t i = 0; i < 1000; ++i) {
    kernel::ScopedMutex guard(shared->mutex);
    shared->counter = shared->counter + 1;
    if (i % 100 == 0) kernel::sleep_us(200);
  }
}

// One thread pinned to each core reports the core it really runs on.
struct Pinned {
  volatile uint32_t ran_on = ~0u;
  volatile uint64_t spins = 0;
};
void pinned_worker(void* argument) {
  Pinned* p = static_cast<Pinned*>(argument);
  p->ran_on = kernel::current_core();
  for (uint32_t i = 0; i < 100000; ++i) p->spins = p->spins + 1;
}

void smp_check() {
  const uint32_t cores = kernel::core_count();
  uint32_t online = 0;
  for (uint32_t i = 0; i < cores; ++i) {
    kernel::CoreInfo info;
    if (kernel::core_info(i, &info) && info.online) ++online;
  }
  constexpr uint32_t kMax = 128;
  static Pinned pinned[kMax];
  static kernel::Thread* threads[kMax];
  uint32_t n = cores < kMax ? cores : kMax;
  for (uint32_t i = 0; i < n; ++i) threads[i] = kernel::spawn(pinned_worker, &pinned[i], "pinned", static_cast<int32_t>(i));
  uint32_t right = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (threads[i] && kernel::join(threads[i], 2000000) && pinned[i].ran_on == i && pinned[i].spins == 100000) ++right;
  }
  Entry(online == cores && right == n ? Level::Ok : Level::Fault, "SMP").dec(online).text(" of ").dec(cores).text(" cores online, ")
      .dec(right).text(" pinned threads ran on their own core");
}

void scheduler_check(allocators::IAllocator& allocator, hardware::AArch64Cpu& cpu) {
  if (!kernel::start(cpu, allocator, 1000)) {
    Entry(Level::Fault, "Kernel").text("could not start threads");
    return;
  }
  Entry(Level::Ok, "Kernel").text("threads running, tick ").dec(kernel::tick_rate()).text(" Hz, GICv").dec(kernel::gic::version());
  Worker a, b;
  kernel::Thread* ta = kernel::spawn(spin_worker, &a, "spin-a");
  kernel::Thread* tb = kernel::spawn(spin_worker, &b, "spin-b");
  const uint64_t begin = hardware::uptime_us();
  kernel::sleep_ms(50);   // the workers run (and are preempted) while this thread sleeps
  const uint64_t slept = hardware::uptime_us() - begin;
  a.stop = true;
  b.stop = true;
  const bool joined = kernel::join(ta, 1000000) && kernel::join(tb, 1000000);
  const bool preempted = a.spins > 0 && b.spins > 0;
  Entry(preempted && joined ? Level::Ok : Level::Fault, "Kernel").text("preemption: slept ").dec(slept / 1000).text(" ms, workers ran ")
      .text(preempted ? "both" : "not both").text(joined ? ", joined" : ", join timed out");

  Shared shared;
  kernel::Thread* m1 = kernel::spawn(mutex_worker, &shared, "mutex-1");
  kernel::Thread* m2 = kernel::spawn(mutex_worker, &shared, "mutex-2");
  const bool done = kernel::join(m1, 2000000) && kernel::join(m2, 2000000);
  Entry(done && shared.counter == 2000 ? Level::Ok : Level::Fault, "Kernel").text("mutex: counter ").dec(shared.counter).text(" of 2000");
  smp_check();
}

// The Raspberry Pi 4 / Pi 400: the VideoCore mailbox tells the board, the V3D 3D core and the HEVC decoder block are brought up. Only on a machine whose ACPI tables say it
// is one (the BCM2711 registers fault anywhere else).
struct PiHandles {
  UniquePointer<hardware::IdentityRegisterMapper> mapper;
  UniquePointer<hardware::RaspberryPiPlatform> platform;
  UniquePointer<hardware::RpiHevcBlock> hevc;
  UniquePointer<hardware::V3dSystemMemory> gpu_memory;
};

void raspberry_pi_bringup(PiHandles& pi, allocators::IAllocator& allocator, const hardware::IRandomAccessMemory& ram, hardware::IGraphicsProcessingUnit* display) {
  pi.mapper = UniquePointer<hardware::IdentityRegisterMapper>::make(allocator, ram);
  if (!pi.mapper) return;
  pi.platform = UniquePointer<hardware::RaspberryPiPlatform>::make(allocator, allocator, *pi.mapper);
  if (!pi.platform) return;
  hardware::RaspberryPiPlatform& platform = *pi.platform;
  if (!platform.initialize()) {
    Entry(Level::Warning, "Pi").text("the VideoCore firmware did not answer as a BCM2711 board");
    return;
  }
  const hardware::PiBoard& board = platform.board();
  Entry(Level::Ok, "Pi").text(board.model).text(" (").text(board.processor_name).text("), ").dec(board.memory_mib).text(" MiB, revision ").hex(board.revision_code);
  if (platform.start_v3d()) {
    const hardware::V3dIdentity& id = platform.v3d()->identity();
    Entry(Level::Ok, "V3D").text("version ").dec(id.version() / 10).text(".").dec(id.version() % 10).text(", ").dec(id.qpus()).text(" QPUs, ")
        .dec(id.texture_units).text(" TMUs per slice");
    // The clear-and-draw check: a frame rendered on the GPU through the built-in QPU programs, read back and counted, and shown top left if there is a screen.
    pi.gpu_memory = UniquePointer<hardware::V3dSystemMemory>::make(allocator, allocator);
    if (pi.gpu_memory) {
      constexpr uint32_t kPollAttempts = 2000000;
      const hardware::V3dSelfTestResult r = hardware::V3dSelfTest::run(*platform.v3d(), *pi.gpu_memory, kPollAttempts, display);
      if (!r.memory_ok) {
        Entry(Level::Warning, "V3D").text("self test skipped: no GPU memory in the first 1 GiB");
      } else {
        const char* outcome = r.job == hardware::V3dJobResult::Done ? "finished" : r.job == hardware::V3dJobResult::TimedOut ? "timed out" : r.job == hardware::V3dJobResult::OutOfMemory ? "ran out of tile memory" : "failed";
        Entry(r.passed() ? Level::Ok : Level::Warning, "V3D").text("self test: frame ").text(outcome).text(", ").dec(r.clear_pixels).text(" clear pixels, ")
            .dec(r.drawn_pixels).text(" drawn pixels").text(r.passed() ? "" : " (expected a cleared image with a triangle on it)");
      }
    }
  } else {
    Entry(Level::Warning, "V3D").text("3D core did not start");
  }
  hardware::MmioRegion hevc_registers, intc_registers;
  if (platform.map_hevc(&hevc_registers, &intc_registers)) {
    pi.hevc = UniquePointer<hardware::RpiHevcBlock>::make(allocator, hevc_registers, intc_registers, platform.firmware());
    if (pi.hevc && pi.hevc->start() == hardware::RpiHevcBlock::Result::Ready) {
      Entry(Level::Ok, "Video").text("HEVC decoder block ready, version ").hex(pi.hevc->version());
    } else {
      Entry(Level::Warning, "Video").text("HEVC decoder block did not start");
    }
  }
}
}  // namespace

extern "C" efi::Status efi_main(efi::Handle image, efi::SystemTable* st) {
  efi::LoadedImage* li = nullptr;
  st->boot_services->handle_protocol(image, &efi::kLoadedImageGuid, reinterpret_cast<void**>(&li));
  console::init(st);
  hardware::delay_init(st);
  st->boot_services->set_watchdog_timer(0, 0, 0, nullptr);

  console::puts("TheoryCraftOS (AArch64): initializing hardware\nTCOS_IMAGE_BASE=");
  console::put_hex(li ? reinterpret_cast<uint64_t>(li->image_base) : 0);
  console::puts("\n");
  Entry(Level::Info, "Firmware").text("UEFI ").dec(st->hdr.revision >> 16).text(".").dec((st->hdr.revision & 0xFFFF) / 10);

  const uint64_t midr = read_sysreg_midr();
  Entry(Level::Ok, "CPU").text(implementer(static_cast<uint32_t>(midr >> 24) & 0xFF)).text(" part ").hex((midr >> 4) & 0xFFF)
      .text(", EL").dec(read_current_el()).text(", timer ").dec(read_cntfrq() / 1000000).text(" MHz");

  hardware::SystemRam ram(st);
  if (!ram.initialize()) {
    Entry(Level::Fault, "RAM").text("initialization failed");
    return efi::kBufferTooSmall;
  }
  allocators::Allocator allocator(ram);
  if (!allocator.initialize()) {
    Entry(Level::Fault, "Allocator").text("initialization failed");
    return efi::kBufferTooSmall;
  }
  Entry(Level::Ok, "RAM").size(ram.total_bytes()).text(" total, ").size(ram.usable_bytes()).text(" in the allocator pool");

  // The debug UART the firmware describes. Boot services keep the firmware's mapping of it alive.
  hardware::AcpiTables acpi(st);
  const bool acpi_ok = acpi.initialize();
  if (acpi_ok) {
    Entry(Level::Ok, "ACPI").text("tables found");
    const uint64_t uart = arch_aarch64::spcr_uart_base(acpi);
    if (uart) {
      hardware::DebugConsole::set_base(uart);
      Entry(Level::Ok, "UART").text("PL011 at ").hex(uart);
    } else {
      Entry(Level::Info, "UART").text("no SPCR serial port");
    }
  } else {
    Entry(Level::Warning, "ACPI").text("no tables (a devicetree-only platform is not supported yet)");
  }

  // Interrupts: the GIC from the MADT, then exception vectors, the timer tick and the thread scheduler.
  hardware::AArch64Cpu cpu;
  allocators::ThreadSafeAllocator shared_allocator(allocator, cpu);
  if (acpi_ok) cpu.discover(acpi, shared_allocator);
  {
    const auto conduit = arch_aarch64::psci_conduit(acpi);
    Entry(Level::Info, "CPU").dec(cpu.core_count()).text(" core(s), PSCI via ")
        .text(conduit == arch_aarch64::PsciConduit::Hvc ? "HVC" : conduit == arch_aarch64::PsciConduit::Smc ? "SMC" : "nothing");
  }
  if (acpi_ok && kernel::gic::init_boot_core(acpi)) {
    Entry(Level::Ok, "GIC").text("version ").dec(kernel::gic::version());
    scheduler_check(shared_allocator, cpu);
  } else {
    Entry(Level::Warning, "GIC").text("not found; running without interrupts");
  }

  hardware::GpuContext context{st, nullptr, {}, &allocator};
  hardware::UefiGopGpu gpu(context);
  hardware::IGraphicsProcessingUnit* display = nullptr;
  if (gpu.initialize()) {
    display = &gpu;
    console::use_gpu(&gpu);
    console::puts("TheoryCraftOS (AArch64): initializing hardware\n");
    bootlog::replay();
    Entry(Level::Ok, "GPU").text(gpu.name()).text(", ").dec(gpu.width()).text("x").dec(gpu.height());
  } else {
    Entry(Level::Warning, "GPU").text("no UEFI framebuffer; staying on the text console");
  }

  PiHandles pi;  // lives as long as efi_main, which never returns
  if (acpi_ok && hardware::acpi_says_raspberry_pi(acpi)) raspberry_pi_bringup(pi, allocator, ram, display);

  Entry(Level::Ok, "Boot").text("AArch64 platform is up (").dec(bootlog::warnings()).text(" warnings)");
  for (;;) __asm__ volatile("wfi");
}
