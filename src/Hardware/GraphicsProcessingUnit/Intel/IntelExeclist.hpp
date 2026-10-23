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
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_gt {

// Register access as the engine code sees it. The real one is a window onto the GPU's registers; a test supplies a pretend engine.
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

// Execlist submission of a Gen8 to Gen10 engine: the engine is given the descriptor of a context through the ELSP register and reports what it did through the
// context status buffer (CSB), six 64-bit entries the GPU writes into the hardware status page. Sequence and constants follow the i915 driver
// (gt/intel_execlists_submission.c, gt/intel_engine_regs.h).
class ExeclistPort {
 public:
  static constexpr uint32_t kCsbEntries = 6;
  static constexpr uint32_t kCsbFirstDword = 0x10;   // I915_HWS_CSB_BUF0_INDEX
  static constexpr uint32_t kCsbWriteDword = 0x1f;   // I915_HWS_CSB_WRITE_INDEX
  static constexpr uint32_t kStatusPageDwords = 1024;

  // Engine register offsets (added to the engine's base).
  static constexpr uint32_t kRingHwsPga = 0x80;
  static constexpr uint32_t kRingHwstam = 0x98;
  static constexpr uint32_t kRingMiMode = 0x9c;
  static constexpr uint32_t kRingElsp = 0x230;
  static constexpr uint32_t kRingModeGen7 = 0x29c;
  static constexpr uint32_t kRingContextStatusPtr = 0x3a0;

  // Bits of the status dword of a CSB entry.
  static constexpr uint32_t kStatusIdleToActive = 1u << 0;
  static constexpr uint32_t kStatusPreempted = 1u << 1;
  static constexpr uint32_t kStatusElementSwitch = 1u << 2;
  static constexpr uint32_t kStatusActiveToIdle = 1u << 3;
  static constexpr uint32_t kStatusComplete = 1u << 4;

  // `status_page` is the CPU's view of the engine's hardware status page, whose graphics address is `status_page_ggtt_address`.
  ExeclistPort(IRegisterBank& registers, uint32_t engine_base, ArrayView<uint32_t> status_page, uint32_t status_page_ggtt_address)
      : registers_(registers), base_(engine_base), status_(status_page), status_ggtt_(status_page_ggtt_address) {}

  // Brings the engine into execlist mode: status page address, run-list enable, ring running, interrupts masked (the driver polls), and the CSB pointers back at
  // their start. The caller holds the engine's power well awake.
  bool enable();
  // Hands the engine one context. Element 1 goes in first (empty), then element 0 starts the context.
  void submit(uint64_t descriptor);

  // Reads the new CSB entries. `started` counts entries that say a context began running, `completed` those that say one finished.
  struct Events {
    uint32_t started = 0;
    uint32_t completed = 0;
    uint32_t idle = 0;      // entries that say the engine went idle
    uint32_t status_or = 0; // every status dword, OR-ed
  };
  Events poll();

 private:
  static constexpr uint32_t masked_enable(uint32_t bits) { return (bits << 16) | bits; }
  static constexpr uint32_t masked_disable(uint32_t bits) { return bits << 16; }

  IRegisterBank& registers_;
  uint32_t base_;
  ArrayView<uint32_t> status_;
  uint32_t status_ggtt_;
  uint32_t csb_head_ = kCsbEntries - 1;
};

}  // namespace intel_gt
}  // namespace hardware
