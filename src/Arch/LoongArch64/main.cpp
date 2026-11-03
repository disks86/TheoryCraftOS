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
// UEFI entry point of the LoongArch image (BOOTLOONGARCH64.EFI). The portable layers (console, RAM, allocator, framebuffer,
// ACPI tables) come up here, then exception handling, the per-core timer tick, the preemptive thread scheduler and the
// other cores; the drivers are not part of this image yet.
#include "Allocators/Allocator.hpp"
#include "Allocators/ThreadSafeAllocator.hpp"
#include "Arch/LoongArch64/Cpu.hpp"
#include "Arch/LoongArch64/Csr.hpp"
#include "Arch/LoongArch64/PageProtector.hpp"
#include "Arch/LoongArch64/Timer.hpp"
#include "Arch/LoongArch64/Uart.hpp"
#include "BootLog.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/DebugConsole.hpp"
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"
#include "Hardware/SystemHardware.hpp"
#include "Jit/CodePageSource.hpp"
#include "Jit/CrossCoreInstructionSync.hpp"
#include "Jit/JitSelfTest.hpp"
#include "Jit/KernelJit.hpp"
#include "Jit/ProtectedCodeMemory.hpp"
#include "Jit/UefiPageProtector.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/ByteStream.hpp"
#include "console.hpp"

using bootlog::Entry;
using bootlog::Level;

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

// The JIT's code memory on this image: pages from a code pool, protections through the firmware's memory attribute protocol, then a compile-and-run check.
// The objects live in the caller's frame (main never returns) and are registered for the compilers that will want them.
struct JitHandles {
  jit::CodePool pool;
  jit::CrossCoreInstructionSync sync;
  arch_loongarch64::PageProtector protector;   // (UEFI runs with direct address translation: nothing to protect)
  jit::ProtectedCodeMemory memory;
  JitHandles(allocators::IAllocator& allocator, efi::SystemTable*) : pool(allocator, 8), protector(), memory(pool, protector, &sync) {}
};

void jit_check(JitHandles& jit_handles, allocators::IAllocator& allocator) {
  jit::set_kernel_code_memory(&jit_handles.memory);
  uint64_t bytes = 0;
  const jit::SelfTestResult result = jit::jit_self_test(allocator, jit_handles.memory, &bytes);
  Entry e(result == jit::SelfTestResult::Passed ? Level::Ok : Level::Fault, "JIT");
  e.text("self test ").text(jit::self_test_result_name(result));
  if (result == jit::SelfTestResult::Passed) e.text(": ").dec(bytes).text(" bytes of code run, memory protections ").text(jit_handles.memory.protections_enforced() ? "enforced (W^X)" : "not available");
}

void scheduler_check(allocators::IAllocator& allocator, hardware::LoongArch64Cpu& cpu) {
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

  console::puts("TheoryCraftOS (LoongArch): initializing hardware\nTCOS_IMAGE_BASE=");
  console::put_hex(li ? reinterpret_cast<uint64_t>(li->image_base) : 0);
  console::puts("\n");
  Entry(Level::Info, "Firmware").text("UEFI ").dec(st->hdr.revision >> 16).text(".").dec((st->hdr.revision & 0xFFFF) / 10);

  using namespace arch_loongarch64;
  const uint64_t crmd = csr_read<kCsrCrmd>();
  const uint32_t prid = __builtin_loongarch_cpucfg(0);
  Entry(Level::Ok, "CPU").text("LoongArch core ").dec(this_core_id()).text(", PRID ").hex(prid).text(", CRMD ").hex(crmd)
      .text(crmd & kCrmdPg ? " (paging)" : (crmd & kCrmdDa) ? " (direct address translation)" : "");

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
    if (const uint64_t uart = spcr_uart_base(acpi)) {
      hardware::DebugConsole::set_base(uart);
      Entry(Level::Info, "Serial").text("16550 debug UART at ").hex(uart).text(" (SPCR)");
    }
  } else {
    Entry(Level::Warning, "ACPI").text("no tables (a devicetree-only platform is not supported)");
  }
  Entry(Level::Info, "Timer").text("stable counter ").dec(kernel::stimer::frequency() / 1000000).text(" MHz");

  // Exception handling, the tick and the thread scheduler.
  hardware::LoongArch64Cpu cpu;
  allocators::ThreadSafeAllocator shared_allocator(allocator, cpu);
  if (acpi_ok) cpu.discover(acpi, shared_allocator);

  Entry(Level::Info, "CPU").dec(cpu.core_count()).text(" core(s)");
  JitHandles jit_handles(shared_allocator, st);   // (lives as long as main, which registers it)
  scheduler_check(shared_allocator, cpu);
  jit_check(jit_handles, shared_allocator);

  hardware::GpuContext context{st, nullptr, {}, &allocator};
  hardware::UefiGopGpu gpu(context);
  if (gpu.initialize()) {
    console::use_gpu(&gpu);
    console::puts("TheoryCraftOS (LoongArch): initializing hardware\n");
    bootlog::replay();
    Entry(Level::Ok, "GPU").text(gpu.name()).text(", ").dec(gpu.width()).text("x").dec(gpu.height());
  } else {
    Entry(Level::Warning, "GPU").text("no UEFI framebuffer; staying on the text console");
  }

  Entry(Level::Ok, "Boot").text("LoongArch platform is up (").dec(bootlog::warnings()).text(" warnings)");
  for (;;) __asm__ volatile("idle 0");
}
