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

// The Supervisor Binary Interface: how supervisor-mode code (this kernel, running under the UEFI firmware) asks the
// machine-mode firmware (OpenSBI and the like) for the timer, inter-processor interrupts, starting other harts and
// the console. Every RISC-V UEFI platform provides it.
#pragma once
#include <stdint.h>

namespace arch_riscv64 {

struct SbiResult {
  int64_t error;   // 0 = success, negative SBI error codes otherwise
  int64_t value;
};

namespace sbi {

constexpr int64_t kErrNotSupported = -2;

// Extension ids (the ASCII of their names) and the legacy single-call ones.
constexpr uint64_t kExtBase = 0x10;
constexpr uint64_t kExtTime = 0x54494D45;
constexpr uint64_t kExtIpi = 0x735049;
constexpr uint64_t kExtHsm = 0x48534D;
constexpr uint64_t kExtDebugConsole = 0x4442434E;
constexpr uint64_t kLegacySetTimer = 0;
constexpr uint64_t kLegacyConsolePutchar = 1;

// One ecall: extension id, function id and up to four arguments.
SbiResult call(uint64_t extension, uint64_t function, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0);

// Base extension: the implemented specification version, implementation id and version, and the machine's ids.
uint64_t spec_version();
uint64_t implementation_id();
uint64_t implementation_version();
uint64_t machine_vendor_id();
uint64_t machine_arch_id();
uint64_t machine_impl_id();
bool probe(uint64_t extension);

// Timer: the next supervisor timer interrupt for this hart at `time` (the time CSR); ~0 never.
bool set_timer(uint64_t time);
// Inter-processor interrupt: a supervisor software interrupt on the given hart.
bool send_ipi(uint64_t hart);
// Starts a stopped hart in supervisor mode at the physical `entry` with a0 = hart id and a1 = `opaque`.
bool hart_start(uint64_t hart, uint64_t entry, uint64_t opaque);
// One byte to the firmware console (debug console extension, or the legacy call on older firmware).
void console_put(char c);

}  // namespace sbi

// The id of the hart the firmware booted this image on (from the firmware's boot protocol), 0 when unknown.
uint64_t boot_hart_id();
void set_boot_hart_id(uint64_t hart);
// A hart started by SBI `hart_start` finds its own id in tp (Secondary.S puts it there).
inline uint64_t this_hart_id() {
  uint64_t id;
  __asm__ volatile("mv %0, tp" : "=r"(id));
  return id;
}

}  // namespace arch_riscv64
