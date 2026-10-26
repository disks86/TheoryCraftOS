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

// How many users hold each power well (ForcewakeDomain, four of them) awake: the first one to ask wakes it, the last one to let go lets it sleep (see Forcewake). The lock
// is a spin lock; the section it guards waits for the hardware for up to a few milliseconds.
struct ForcewakeState {
  uint32_t lock = 0;
  uint32_t counts[4] = {};
};

// Register access as the engine code sees it. The real one is a window onto the GPU's registers; a test supplies a pretend GPU.
class IRegisterBank {
 public:
  virtual uint32_t read32(uint64_t offset) = 0;
  virtual void write32(uint64_t offset, uint32_t value) = 0;
  // The power well counts of the GPU behind this bank. Banks that are windows onto the same GPU must answer with the same object (the render engine driver and the
  // video engine driver each have one), or one of them would let a well sleep under the other.
  virtual ForcewakeState& forcewake_state() { return forcewake_; }

 protected:
  constexpr IRegisterBank() = default;
  ~IRegisterBank() = default;

 private:
  ForcewakeState forcewake_;
};

// The counts of the GPUs whose registers are reached through MmioRegisterBank, found by the address of the register window (up to four GPUs).
inline ForcewakeState& shared_forcewake_state(uint64_t window_address) {
  static uint64_t addresses[4] = {};
  static ForcewakeState states[4] = {};
  static uint32_t used = 0;
  static uint32_t lock = 0;
  while (__atomic_exchange_n(&lock, 1u, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
  uint32_t found = used;
  for (uint32_t i = 0; i < used; ++i)
    if (addresses[i] == window_address) found = i;
  if (found == used && used < 4) addresses[used++] = window_address;
  if (found >= 4) found = 3;   // (more GPUs than that share the last slot: counts stay correct, only the wells are held together)
  __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE);
  return states[found];
}

class MmioRegisterBank final : public IRegisterBank {
 public:
  explicit MmioRegisterBank(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint64_t offset) override { return region_.read32(offset); }
  void write32(uint64_t offset, uint32_t value) override { region_.write32(offset, value); }
  ForcewakeState& forcewake_state() override { return shared_forcewake_state(region_.address()); }

 private:
  MmioRegion region_;
};

}  // namespace intel_gt
}  // namespace hardware
