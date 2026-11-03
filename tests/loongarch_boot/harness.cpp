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

// A bare-metal check of the LoongArch exception, timer, IPI and second-core code on QEMU's "virt" machine without any
// firmware (-kernel loads the ELF at its physical address and starts it in direct address translation, as UEFI does):
// it links the real Trap.S, Interrupts.cpp, Timer.cpp, Cpu.cpp and Secondary.S of the image and drives them.
// tests/loongarch_boot.sh builds and runs it; the serial output must end with "ALL PASSED".
#include "Arch/LoongArch64/Cpu.hpp"
#include "Arch/LoongArch64/Csr.hpp"
#include "Arch/LoongArch64/Timer.hpp"
#include "Hardware/DebugConsole.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Interrupts.hpp"
#include "console.hpp"

using namespace arch_loongarch64;

namespace {
constexpr uint64_t kUart = 0x1fe001e0;   // QEMU virt's ns16550a

void uart_put(char c) {
  auto* thr = reinterpret_cast<volatile uint8_t*>(kUart);
  auto* lsr = reinterpret_cast<volatile uint8_t*>(kUart + 5);
  while (!(*lsr & 0x20)) {}
  *thr = static_cast<uint8_t>(c);
}

int g_failures = 0;
void check(bool ok, const char* what) {
  console::puts(ok ? "PASS " : "FAIL ");
  console::puts(what);
  console::puts("\n");
  if (!ok) ++g_failures;
}

volatile uint32_t g_ticks = 0, g_yields = 0, g_wakeups = 0, g_secondary_core = ~0u, g_secondary_ticks = 0;
volatile uint32_t g_secondary_done = 0;

kernel::Frame* on_timer(kernel::Frame* frame, void*) {
  kernel::stimer::rearm();
  if (this_core_id() == 0) g_ticks = g_ticks + 1;
  else g_secondary_ticks = g_secondary_ticks + 1;
  return frame;
}
kernel::Frame* on_yield(kernel::Frame* frame, void*) {
  g_yields = g_yields + 1;
  return frame;
}
kernel::Frame* on_wakeup(kernel::Frame* frame, void*) {
  ack_ipi();
  g_wakeups = g_wakeups + 1;
  return frame;
}

struct Bump final : allocators::IAllocator {
  alignas(4096) uint8_t memory[1 << 20];
  uint64_t used = 0;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t start = (used + alignment - 1) & ~(alignment - 1);
    if (!size || start + size > sizeof(memory)) return nullptr;
    used = start + size;
    return memory + start;
  }
  void deallocate(void*) override {}
};
Bump g_bump;

void secondary(void*) {
  kernel::interrupts_load(false);
  g_secondary_core = this_core_id();
  kernel::stimer::start(1000);
  kernel::irq_enable();
  const uint64_t begin = read_counter();
  while (g_secondary_ticks < 3 && read_counter() - begin < 2000000000ull) {}
  kernel::irq_disable();
  kernel::stimer::stop();
  send_ipi(0, 1);   // tell the boot core
  __atomic_store_n(&g_secondary_done, 1, __ATOMIC_RELEASE);
  for (;;) __asm__ volatile("idle 0");
}

// A MADT with the two CORE_PIC entries (type 0x11, length 15: version, ACPI id, core id, flags) of a 2-core machine.
void build_madt(uint8_t* out, uint32_t* size) {
  for (uint32_t i = 0; i < 44 + 2 * 15; ++i) out[i] = 0;
  for (uint32_t core = 0; core < 2; ++core) {
    uint8_t* e = out + 44 + core * 15;
    e[0] = 0x11; e[1] = 15; e[2] = 1;
    e[3] = static_cast<uint8_t>(core);   // ACPI id
    e[7] = static_cast<uint8_t>(core);   // core id
    e[11] = 1;                           // enabled
  }
  *size = 44 + 2 * 15;
}
}  // namespace

namespace console {
void puts(StringView text) { for (char c : text) uart_put(c); }
void break_lock() {}
void put_hex(uint64_t v) {
  puts("0x");
  for (int shift = 60; shift >= 0; shift -= 4) uart_put("0123456789abcdef"[(v >> shift) & 15]);
}
void put_dec(int64_t v) {
  if (v < 0) { uart_put('-'); v = -v; }
  char digits[24];
  int n = 0;
  do { digits[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
  while (n) uart_put(digits[--n]);
}
}  // namespace console

namespace hardware {
uint64_t tsc_ticks_per_us() { return 100; }   // (only reached when CPUCFG gives no frequency)
void DebugConsole::put(char c) const { uart_put(c); }
}  // namespace hardware

extern "C" void harness_main() {
  console::puts("LoongArch bare-metal check\n");
  const uint64_t crmd = csr_read<kCsrCrmd>();
  check((crmd & (kCrmdDa | kCrmdPg)) == kCrmdDa, "starts in direct address translation");
  console::puts("core ");
  console::put_dec(this_core_id());
  console::puts(" CRMD ");
  console::put_hex(crmd);
  console::puts(" timer frequency ");
  console::put_dec(static_cast<int64_t>(kernel::stimer::frequency()));
  console::puts(" Hz\n");
  check(this_core_id() == 0, "boot core id is 0");
  check(kernel::stimer::frequency() >= 1000000, "counter frequency known");

  kernel::set_interrupt_handler(kernel::kVectorTimer, on_timer, nullptr);
  kernel::set_interrupt_handler(kernel::kVectorYield, on_yield, nullptr);
  kernel::set_interrupt_handler(kernel::kVectorWakeup, on_wakeup, nullptr);
  kernel::interrupts_init();
  kernel::interrupts_load(true);

  const uint64_t flags = kernel::irq_save();
  check(!kernel::irq_enabled(), "irq_save masks interrupts");
  kernel::irq_restore(flags);
  check(!kernel::irq_enabled(), "irq_restore keeps them masked when they were");
  kernel::irq_enable();
  check(kernel::irq_enabled(), "irq_enable");

  // BREAK: the yield, resumed after the instruction.
  __asm__ volatile("break 0" ::: "memory");
  __asm__ volatile("break 0" ::: "memory");
  check(g_yields == 2, "BREAK reaches the yield handler and execution continues");
  check(kernel::irq_enabled(), "interrupts are enabled again after the exception");

  // Timer: 1000 Hz for 10 ticks, measured by the stable counter.
  const uint64_t hz = kernel::stimer::frequency();
  const uint64_t begin = read_counter();
  kernel::stimer::start(1000);
  while (g_ticks < 10 && read_counter() - begin < 2000000000ull) {}
  const uint64_t elapsed = read_counter() - begin;
  kernel::stimer::stop();
  check(g_ticks >= 10, "timer ticks arrive");
  const uint64_t ten_ms = hz / 100;
  check(elapsed > ten_ms * 8 / 10 && elapsed < ten_ms * 3, "ten ticks take about ten milliseconds");
  const uint32_t after_stop = g_ticks;
  const uint64_t wait = read_counter();
  while (read_counter() - wait < hz / 100) {}
  check(g_ticks == after_stop, "no ticks after stop");

  // IPI to ourselves.
  send_ipi(0, 1);
  const uint64_t ipi_begin = read_counter();
  while (g_wakeups < 1 && read_counter() - ipi_begin < hz) {}
  check(g_wakeups == 1, "an IPI to this core is delivered as the wake-up interrupt");

  // The second core: started through the mailbox and an IPI, with its own stack, takes timer ticks, and sends an IPI back.
  uint8_t madt[44 + 30];
  uint32_t madt_size = 0;
  build_madt(madt, &madt_size);
  hardware::LoongArch64Cpu cpu;
  check(cpu.parse_madt(ConstByteView(madt, madt_size), g_bump), "MADT with two CORE_PIC entries parses");
  check(cpu.core_count() == 2, "two cores found");
  const bool started = cpu.start_on_core(1, secondary, nullptr);
  check(started, "start_on_core accepted");
  const uint64_t second_begin = read_counter();
  while (!__atomic_load_n(&g_secondary_done, __ATOMIC_ACQUIRE) && read_counter() - second_begin < 3 * hz) {}
  check(g_secondary_done == 1, "the second core ran the function");
  check(g_secondary_core == 1, "...and reports core id 1");
  check(g_secondary_ticks >= 3, "...and takes its own timer ticks");
  const uint64_t back = read_counter();
  while (g_wakeups < 2 && read_counter() - back < hz) {}
  check(g_wakeups == 2, "an IPI from the second core arrives here");

  console::puts(g_failures ? "SOME FAILED\n" : "ALL PASSED\n");
  for (;;) __asm__ volatile("idle 0");
}

