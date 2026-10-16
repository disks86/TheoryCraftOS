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
#include "hardware/IHardware.hpp"
#include "hardware/IoPort.hpp"

namespace hardware {

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

}  // namespace hardware
