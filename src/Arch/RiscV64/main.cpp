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

// UEFI entry point of the RISC-V image (BOOTRISCV64.EFI). The portable layers (console, RAM, allocator, framebuffer,
// ACPI tables) come up here, then trap handling, the SBI timer tick, the preemptive thread scheduler and the other
// harts; the drivers are not part of this image yet.
#include "Allocators/Allocator.hpp"
#include "Allocators/ThreadSafeAllocator.hpp"
#include "Arch/RiscV64/Cpu.hpp"
#include "Arch/RiscV64/Sbi.hpp"
#include "Arch/RiscV64/Timer.hpp"
#include "BootLog.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/DebugConsole.hpp"
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"
#include "Hardware/SystemHardware.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/ByteStream.hpp"
#include "console.hpp"

using bootlog::Entry;
using bootlog::Level;

namespace {
// The firmware's RISC-V boot protocol (RISCV_EFI_BOOT_PROTOCOL): which hart the firmware booted on.
constexpr efi::Guid kRiscvBootGuid = {0xccd15fec, 0x6f73, 0x4eec, {0x83, 0x95, 0x3e, 0x69, 0xe4, 0xb9, 0x40, 0xbf}};
struct RiscvBootProtocol {
  uint64_t revision;
  efi::Status (*get_boot_hartid)(RiscvBootProtocol* self, uint64_t* hart);
};

bool find_boot_hart(efi::SystemTable* st, uint64_t* hart) {
  RiscvBootProtocol* protocol = nullptr;
  if (st->boot_services->locate_protocol(&kRiscvBootGuid, nullptr, reinterpret_cast<void**>(&protocol)) != efi::kSuccess || !protocol) return false;
  return protocol->get_boot_hartid(protocol, hart) == efi::kSuccess;
}

// The timebase frequency from the ACPI RHCT table (offset 36: flags, 40: time base frequency), or 0.
uint64_t rhct_timebase(const hardware::AcpiTables& acpi) {
  const ConstByteView rhct = acpi.find("RHCT");
  if (rhct.size() < 48) return 0;
  ByteReader r(rhct.subview(40, 8));
  const uint64_t hz = r.le64();
  return r.ok() ? hz : 0;
}

const char* implementation_name(uint64_t id) {
  switch (id) {
    case 0: return "Berkeley Boot Loader";
    case 1: return "OpenSBI";
    case 2: return "Xvisor";
    case 3: return "KVM";
    case 4: return "RustSBI";
    case 5: return "Diosix";
    case 6: return "Coffer";
    case 7: return "Xen";
    case 8: return "PolarFire HSS";
    case 9: return "coreboot";
    case 10: return "oreboot";
    default: return "unknown SBI";
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

void scheduler_check(allocators::IAllocator& allocator, hardware::RiscV64Cpu& cpu) {
  if (!kernel::start(cpu, allocator, 1000)) {
    Entry(Level::Fault, "Kernel").text("could not start threads");
    return;
  }
  Entry(Level::Ok, "Kernel").text("threads running, tick ").dec(kernel::tick_rate()).text(" Hz");
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
}  // namespace

extern "C" efi::Status efi_main(efi::Handle image, efi::SystemTable* st) {
  efi::LoadedImage* li = nullptr;
  st->boot_services->handle_protocol(image, &efi::kLoadedImageGuid, reinterpret_cast<void**>(&li));
  console::init(st);
  hardware::delay_init(st);
  st->boot_services->set_watchdog_timer(0, 0, 0, nullptr);

  console::puts("TheoryCraftOS (RISC-V): initializing hardware\nTCOS_IMAGE_BASE=");
  console::put_hex(li ? reinterpret_cast<uint64_t>(li->image_base) : 0);
  console::puts("\n");
  Entry(Level::Info, "Firmware").text("UEFI ").dec(st->hdr.revision >> 16).text(".").dec((st->hdr.revision & 0xFFFF) / 10);

  namespace sbi = arch_riscv64::sbi;
  const uint64_t spec = sbi::spec_version();
  Entry(Level::Ok, "SBI").text(implementation_name(sbi::implementation_id())).text(" ").hex(sbi::implementation_version())
      .text(", spec ").dec((spec >> 24) & 0x7F).text(".").dec(spec & 0xFFFFFF).text(", time ").text(sbi::probe(sbi::kExtTime) ? "yes" : "no")
      .text(", IPI ").text(sbi::probe(sbi::kExtIpi) ? "yes" : "no").text(", HSM ").text(sbi::probe(sbi::kExtHsm) ? "yes" : "no");
  uint64_t boot_hart = 0;
  const bool boot_hart_known = find_boot_hart(st, &boot_hart);
  arch_riscv64::set_boot_hart_id(boot_hart);
  if (boot_hart_known) Entry(Level::Ok, "CPU").text("RISC-V hart ").dec(boot_hart).text(", vendor ").hex(sbi::machine_vendor_id())
      .text(" arch ").hex(sbi::machine_arch_id()).text(" impl ").hex(sbi::machine_impl_id());
  else Entry(Level::Warning, "CPU").text("the firmware has no boot hart protocol; running on one hart");

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

  hardware::AcpiTables acpi(st);
  const bool acpi_ok = acpi.initialize();
  if (acpi_ok) {
    Entry(Level::Ok, "ACPI").text("tables found");
    const uint64_t hz = rhct_timebase(acpi);
    if (hz) kernel::stimer::set_frequency(hz);
    Entry(Level::Info, "Timer").text("time CSR ").dec(kernel::stimer::frequency() / 1000000).text(" MHz")
        .text(hz ? " (RHCT)" : " (measured)");
  } else {
    Entry(Level::Warning, "ACPI").text("no tables (a devicetree-only platform is not supported yet)");
  }

  // Trap handling, the tick and the thread scheduler.
  hardware::RiscV64Cpu cpu;
  allocators::ThreadSafeAllocator shared_allocator(allocator, cpu);
  if (acpi_ok) cpu.discover(acpi, shared_allocator);

  Entry(Level::Info, "CPU").dec(cpu.core_count()).text(" hart(s)");
  if (kernel::stimer::available()) {
    scheduler_check(shared_allocator, cpu);
  } else {
    Entry(Level::Warning, "Timer").text("no SBI timer; running without interrupts");
  }

  hardware::GpuContext context{st, nullptr, {}, &allocator};
  hardware::UefiGopGpu gpu(context);
  if (gpu.initialize()) {
    console::use_gpu(&gpu);
    console::puts("TheoryCraftOS (RISC-V): initializing hardware\n");
    bootlog::replay();
    Entry(Level::Ok, "GPU").text(gpu.name()).text(", ").dec(gpu.width()).text("x").dec(gpu.height());
  } else {
    Entry(Level::Warning, "GPU").text("no UEFI framebuffer; staying on the text console");
  }

  Entry(Level::Ok, "Boot").text("RISC-V platform is up (").dec(bootlog::warnings()).text(" warnings)");
  for (;;) __asm__ volatile("wfi");
}
