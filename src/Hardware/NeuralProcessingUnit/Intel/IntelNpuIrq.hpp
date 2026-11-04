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
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuIpc.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuMmu.hpp"

namespace hardware {

// What one interrupt turned out to be about.
struct IntelNpuIrqResult {
  bool handled = false;         // the NPU raised it (false: not ours, or spurious)
  bool ipc = false;             // the receive FIFO had messages (drained into the IPC channel if one was given)
  bool mmu_events = false;      // the MMU's event queue has faults to read (IntelNpuMmu::next_event)
  bool mmu_sync = false;        // an MMU command-sync completed (informational)
  bool mmu_errors = false;      // a global error of the MMU (acknowledged if an MMU was given)
  bool watchdog_mss = false;    // the management subsystem's watchdog fired: recover the NPU
  bool watchdog_nce = false;    // the neural compute engine's watchdog fired: recover the NPU
  bool firewall = false;        // a NoC firewall violation (counted)
  bool buttress_error = false;  // an address-translation, fabric or IMR error reported by the buttress
  bool survivability = false;   // the survivability interrupt (Lunar Lake and later)
  bool frequency_change = false;
  bool recovery_needed = false; // the owner should reset and reboot the NPU
};

// The interrupt side of the NPU (ivpu_hw.c ivpu_hw_irq_*, ivpu_hw_btrs.c, ivpu_hw_ip.c): two sources, the IP block's interrupt controller (ICB: IPC FIFO, MMU,
// watchdogs, firewall) and the buttress's own error interrupts. Enabling programs the masks; handle() reads and clears what is pending, with the buttress's
// global mask held while it works so a new interrupt re-triggers the line afterwards. Not thread safe: the owner runs one handler at a time.
class IntelNpuIrq {
 public:
  // IP registers.
  static constexpr uint64_t kFwSocIrqEnable = 0x00000170, kIcbStatus0 = 0x00010210, kIcbClear0 = 0x00010220, kIcbEnable0 = 0x00010240;
  static constexpr uint32_t kIcbIpcFifo = 1u << 4, kIcbMmu0 = 1u << 5, kIcbMmu1 = 1u << 6, kIcbMmu2 = 1u << 7, kIcbFirewall = 1u << 8, kIcbRedirect0 = 1u << 30,
                            kIcbRedirect1 = 1u << 31;
  static constexpr uint32_t kIcb0Mask = kIcbIpcFifo | kIcbMmu0 | kIcbMmu1 | kIcbMmu2 | kIcbFirewall | kIcbRedirect0 | kIcbRedirect1;
  static constexpr uint32_t kIcb1Mask = 0x7;  // redirect 2-4, the high word of the 64-bit enable and clear registers
  static constexpr uint32_t kFirewallSources = 0x7F;  // CSS ROM/CMX/debug/control, DEC400, MSS NCE, MBI, MBI CMX
  // Buttress registers, Meteor Lake generation.
  static constexpr uint64_t kMtlStatus = 0x04, kMtlLocalMask = 0x30, kMtlGlobalMask = 0x34, kMtlAtsErrClear = 0xA8, kMtlUfiErrClear = 0xB4;
  static constexpr uint32_t kMtlFreqChange = 1u << 0, kMtlAtsErr = 1u << 1, kMtlUfiErr = 1u << 2;
  // Buttress registers, Lunar Lake generation and later.
  static constexpr uint64_t kLnlStatus = 0x00, kLnlLocalMask = 0x04, kLnlGlobalMask = 0x08, kLnlAtsErrClear = 0x18, kLnlCfi0ErrClear = 0x20,
                            kLnlCfi1ErrClear = 0x44, kLnlImr0ErrClear = 0x50, kLnlImr1ErrClear = 0x60;
  static constexpr uint32_t kLnlFreqChange = 1u << 0, kLnlAtsErr = 1u << 1, kLnlCfi0Err = 1u << 2, kLnlCfi1Err = 1u << 3, kLnlImr0Err = 1u << 4,
                            kLnlImr1Err = 1u << 5, kLnlSurvErr = 1u << 6;

  IntelNpuIrq(INpuRegisters& ip, INpuRegisters& buttress, const IntelNpuPlatform& platform) : ip_(ip), btrs_(buttress), platform_(platform) {}

  // ivpu_hw_irq_enable()/disable(). `watch_frequency_changes` also unmasks the (noisy) frequency-change interrupt, a test mode of ivpu.
  void enable(bool watch_frequency_changes = false);
  void disable();
  // Clears everything pending in the IP block (before enabling, after the firmware booted).
  void clear();
  // ivpu_hw_irq_handler(). `ipc` and `mmu` are optional; given, the IPC FIFO is drained and the MMU's global errors acknowledged.
  IntelNpuIrqResult handle(IntelNpuIpc* ipc = nullptr, IntelNpuMmu* mmu = nullptr);

  uint32_t firewall_count() const { return firewall_count_; }

 private:
  bool lnl() const { return platform_.buttress == IntelNpuButtress::Lnl; }
  bool handle_buttress(IntelNpuIrqResult& result);
  bool handle_ip(IntelNpuIrqResult& result, IntelNpuIpc* ipc, IntelNpuMmu* mmu);
  uint32_t buttress_mask() const;

  INpuRegisters& ip_;
  INpuRegisters& btrs_;
  IntelNpuPlatform platform_;
  bool watch_frequency_ = false;
  uint32_t firewall_count_ = 0;
};

}  // namespace hardware
