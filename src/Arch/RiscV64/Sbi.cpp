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

#include "Arch/RiscV64/Sbi.hpp"
#include "Hardware/DebugConsole.hpp"

namespace arch_riscv64 {

namespace {
uint64_t g_boot_hart = 0;
}  // namespace

uint64_t boot_hart_id() { return g_boot_hart; }
void set_boot_hart_id(uint64_t hart) { g_boot_hart = hart; }

namespace sbi {

SbiResult call(uint64_t extension, uint64_t function, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  register uint64_t r0 __asm__("a0") = a0;
  register uint64_t r1 __asm__("a1") = a1;
  register uint64_t r2 __asm__("a2") = a2;
  register uint64_t r3 __asm__("a3") = a3;
  register uint64_t r6 __asm__("a6") = function;
  register uint64_t r7 __asm__("a7") = extension;
  __asm__ volatile("ecall" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3), "r"(r6), "r"(r7) : "memory");
  return {static_cast<int64_t>(r0), static_cast<int64_t>(r1)};
}

namespace {
uint64_t base(uint64_t function, uint64_t a0 = 0) { return static_cast<uint64_t>(call(kExtBase, function, a0).value); }

// 0 = not probed yet, 1 = debug console extension, 2 = legacy putchar
volatile int g_console_mode = 0;
}  // namespace

uint64_t spec_version() { return base(0); }
uint64_t implementation_id() { return base(1); }
uint64_t implementation_version() { return base(2); }
bool probe(uint64_t extension) { return base(3, extension) != 0; }
uint64_t machine_vendor_id() { return base(4); }
uint64_t machine_arch_id() { return base(5); }
uint64_t machine_impl_id() { return base(6); }

bool set_timer(uint64_t time) {
  if (call(kExtTime, 0, time).error == 0) return true;
  return call(kLegacySetTimer, 0, time).error == 0;
}

bool send_ipi(uint64_t hart) {
  // hart_mask bit i addresses hart (hart_mask_base + i): base = the target, mask = bit 0.
  return call(kExtIpi, 0, 1, hart).error == 0;
}

bool hart_start(uint64_t hart, uint64_t entry, uint64_t opaque) { return call(kExtHsm, 0, hart, entry, opaque).error == 0; }

void console_put(char c) {
  if (g_console_mode == 0) g_console_mode = probe(kExtDebugConsole) ? 1 : 2;
  if (g_console_mode == 1) {
    // sbi_debug_console_write_byte; fall back to the legacy call if the extension refuses.
    if (call(kExtDebugConsole, 2, static_cast<uint8_t>(c)).error == 0) return;
    g_console_mode = 2;
  }
  call(kLegacyConsolePutchar, 0, static_cast<uint8_t>(c));
}

}  // namespace sbi

}  // namespace arch_riscv64

namespace hardware {
void DebugConsole::put(char c) const {
  if (enabled_) arch_riscv64::sbi::console_put(c);
}
}  // namespace hardware
