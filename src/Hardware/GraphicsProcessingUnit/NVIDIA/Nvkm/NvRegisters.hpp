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

// The shared foundation of the native NVIDIA driver (the counterpart of nouveau's nvkm core): how its parts reach the GPU's registers, which chip
// it is, and what the video BIOS says about the board. Every subsystem (Nvkm/Bios, Display, I2c, Therm, Fb, Mmu, Fifo, Ce) takes these
// interfaces, so each can be tested against a register model without the others.
//
// Register offsets are BAR0 offsets (the 16 MiB "MMIO" BAR; nouveau's nvkm_rd32/nvkm_wr32). A register the model does not know reads 0.
#pragma once
#include <stdint.h>
#include "Hardware/Mmio.hpp"

namespace hardware {
namespace nv {

class INvRegisters {
 public:
  virtual uint32_t read32(uint32_t offset) = 0;
  virtual void write32(uint32_t offset, uint32_t value) = 0;
  // nvkm_mask: the bits of `clear` are taken out, `set` put in; returns the value before.
  uint32_t mask(uint32_t offset, uint32_t clear, uint32_t set) {
    const uint32_t old = read32(offset);
    write32(offset, (old & ~clear) | set);
    return old;
  }
  // Reads until (value & mask) == expected, at most `attempts` times (nvkm_msec loops: the caller picks the count); true if it happened.
  bool wait(uint32_t offset, uint32_t mask, uint32_t expected, uint32_t attempts) {
    for (uint32_t i = 0; i < attempts; ++i)
      if ((read32(offset) & mask) == expected) return true;
    return false;
  }
  // A short pause between polls (microseconds); the kernel's delay, a no-op in tests.
  virtual void delay_us(uint32_t us) { (void)us; }

 protected:
  INvRegisters() = default;
  ~INvRegisters() = default;
};

// BAR0 as mapped (MmioRegion checks every access against the window).
class MmioNvRegisters final : public INvRegisters {
 public:
  static constexpr uint64_t kBar0Bytes = 16u << 20;
  explicit MmioNvRegisters(const MmioRegion& bar0) : bar0_(bar0) {}
  uint32_t read32(uint32_t offset) override { return offset + 4ull <= bar0_.length() ? bar0_.read32(offset) : 0xFFFFFFFFu; }
  void write32(uint32_t offset, uint32_t value) override { if (offset + 4ull <= bar0_.length()) bar0_.write32(offset, value); }
  void delay_us(uint32_t us) override;

 private:
  MmioRegion bar0_;
};

}  // namespace nv
}  // namespace hardware
