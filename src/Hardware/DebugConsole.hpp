/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
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

#pragma once
#include <stdint.h>
#include "Hardware/IHardware.hpp"
#if defined(__x86_64__)
#include "Hardware/IoPort.hpp"
#endif

namespace hardware {

#if defined(__x86_64__)
// QEMU's isa-debugcon device: bytes written to port 0x402 reach the host.
// Harmless on real hardware (the write is ignored).
class DebugConsole final : public IHardware {
 public:
  constexpr DebugConsole() = default;

  const char* name() const override { return "QEMU debugcon"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  void put(char c) const { port_.write8(static_cast<uint8_t>(c)); }

 private:
  static constexpr uint16_t kPort = 0x402;
  IoPort port_{kPort};
};
#elif defined(__aarch64__)
// The firmware-described debug UART: a PL011 whose base address comes from the ACPI SPCR table
// (set_base(), see Arch/AArch64/Spcr.hpp). Until a base is known the output is dropped, so nothing
// ever touches an address the platform did not name.
class DebugConsole final : public IHardware {
 public:
  constexpr DebugConsole() = default;

  const char* name() const override { return "PL011 debug UART"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  static void set_base(uint64_t mmio_base) { base_ = mmio_base; }
  void put(char c) const {
    if (!base_) return;
    auto* dr = reinterpret_cast<volatile uint32_t*>(base_);          // UARTDR
    auto* fr = reinterpret_cast<volatile uint32_t*>(base_ + 0x18);   // UARTFR
    constexpr uint32_t kTxFull = 1u << 5;
    for (uint32_t spin = 0; (*fr & kTxFull) && spin < 100000; ++spin) {}
    *dr = static_cast<uint8_t>(c);
  }

 private:
  static inline uint64_t base_ = 0;
};
#elif defined(__riscv) && __riscv_xlen == 64
// The SBI debug console (every RISC-V UEFI platform runs on SBI firmware): bytes go wherever the firmware's
// console is, with no UART address to find. That is usually the same serial port the UEFI text console already
// writes to, so the mirror is off until something needs it (a kernel panic, when firmware services are unusable).
// Defined in Arch/RiscV64/Sbi.cpp.
class DebugConsole final : public IHardware {
 public:
  constexpr DebugConsole() = default;

  const char* name() const override { return "SBI debug console"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  static void set_enabled(bool enabled) { enabled_ = enabled; }
  void put(char c) const;

 private:
  static inline bool enabled_ = false;
};
#else
#error "No debug console for this architecture"
#endif

}  // namespace hardware
