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
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"
#include "Hardware/Mmio.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_gt {

// Execlist submission of a Gen8 to Gen12 engine. Gen8 to Gen10:  the engine is given the descriptor of a context through the ELSP register and reports what it did through the
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

  // Gen11 and later (Gen12 here): the submit queue (ELSQ) takes two descriptors, written as two dwords each and started by a write to the control register; the CSB has
  // twelve entries, its write pointer is at dword 0x2f of the status page, and an entry is in the Gen12 format (the context IDs of the contexts switched to and from).
  enum class Mode : uint8_t { Elsp, Elsq };
  static constexpr uint32_t kCsb12Entries = 12;
  static constexpr uint32_t kCsb12WriteDword = 0x2f;     // ICL_HWS_CSB_WRITE_INDEX
  static constexpr uint32_t kRingExeclistSqContents = 0x510;
  static constexpr uint32_t kRingExeclistControl = 0x550;
  static constexpr uint32_t kControlLoad = 1u << 0;      // EL_CTRL_LOAD
  static constexpr uint32_t kGen11DisableLegacyMode = 1u << 3;
  static constexpr uint32_t kIdleContextId = 0x7FF;

  // Bits of the status dword of a CSB entry.
  static constexpr uint32_t kStatusIdleToActive = 1u << 0;
  static constexpr uint32_t kStatusPreempted = 1u << 1;
  static constexpr uint32_t kStatusElementSwitch = 1u << 2;
  static constexpr uint32_t kStatusActiveToIdle = 1u << 3;
  static constexpr uint32_t kStatusComplete = 1u << 4;

  // `status_page` is the CPU's view of the engine's hardware status page, whose graphics address is `status_page_ggtt_address`.
  ExeclistPort() = default;
  ExeclistPort(IRegisterBank& registers, uint32_t engine_base, ArrayView<uint32_t> status_page, uint32_t status_page_ggtt_address, Mode mode = Mode::Elsp) {
    configure(registers, engine_base, status_page, status_page_ggtt_address, mode);
  }
  void configure(IRegisterBank& registers, uint32_t engine_base, ArrayView<uint32_t> status_page, uint32_t status_page_ggtt_address, Mode mode = Mode::Elsp) {
    registers_ = &registers;
    base_ = engine_base;
    status_ = status_page;
    status_ggtt_ = status_page_ggtt_address;
    mode_ = mode;
    csb_head_ = entries() - 1;
  }

  // Brings the engine into execlist mode: status page address, run-list enable, ring running, interrupts masked (the driver polls), and the CSB pointers back at
  // their start. The caller holds the engine's power well awake.
  bool enable();
  // Hands the engine one context. Element 1 goes in first (empty), then element 0 starts the context.
  void submit(uint64_t descriptor);
  uint32_t entries() const { return mode_ == Mode::Elsq ? kCsb12Entries : kCsbEntries; }
  uint32_t write_dword() const { return mode_ == Mode::Elsq ? kCsb12WriteDword : kCsbWriteDword; }

  // Reads the new CSB entries. `started` counts entries that say a context began running, `completed` those that say one finished.
  struct Events {
    uint32_t started = 0;
    uint32_t completed = 0;
    uint32_t idle = 0;      // entries that say the engine went idle
    uint32_t status_or = 0; // every status dword, OR-ed
  };
  Events poll();
  // Stops the ring (STOP_RING) and waits for the engine to say it is idle. False if it never does: the engine may still be reading memory.
  bool stop(uint32_t attempts);

 private:
  static constexpr uint32_t masked_enable(uint32_t bits) { return (bits << 16) | bits; }
  static constexpr uint32_t masked_disable(uint32_t bits) { return bits << 16; }

  IRegisterBank* registers_ = nullptr;
  uint32_t base_ = 0;
  ArrayView<uint32_t> status_;
  uint32_t status_ggtt_ = 0;
  uint32_t csb_head_ = kCsbEntries - 1;
  Mode mode_ = Mode::Elsp;
};

}  // namespace intel_gt
}  // namespace hardware
