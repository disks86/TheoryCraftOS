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
#include "Hardware/Mmio.hpp"

namespace hardware {
namespace intel_gt {

// Register access as the engine code sees it. The real one is a window onto the GPU's registers; a test supplies a pretend GPU.
class IRegisterBank {
 public:
  virtual uint32_t read32(uint64_t offset) = 0;
  virtual void write32(uint64_t offset, uint32_t value) = 0;

 protected:
  constexpr IRegisterBank() = default;
  ~IRegisterBank() = default;
};

class MmioRegisterBank final : public IRegisterBank {
 public:
  explicit MmioRegisterBank(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint64_t offset) override { return region_.read32(offset); }
  void write32(uint64_t offset, uint32_t value) override { region_.write32(offset, value); }

 private:
  MmioRegion region_;
};

}  // namespace intel_gt
}  // namespace hardware
