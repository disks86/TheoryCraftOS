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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuIrq.hpp"

namespace hardware {

namespace {
constexpr uint64_t icb_both_mask() { return (uint64_t{IntelNpuIrq::kIcb1Mask} << 32) | IntelNpuIrq::kIcb0Mask; }
constexpr uint32_t kBtrsDisableAll = 0xFFFFFFFFu;
}  // namespace

uint32_t IntelNpuIrq::buttress_mask() const {
  if (lnl()) return kLnlAtsErr | kLnlCfi0Err | kLnlCfi1Err | kLnlImr0Err | kLnlImr1Err | kLnlSurvErr | (watch_frequency_ ? kLnlFreqChange : 0);
  return kMtlAtsErr | kMtlUfiErr | (watch_frequency_ ? kMtlFreqChange : 0);
}

void IntelNpuIrq::enable(bool watch_frequency_changes) {
  if (!platform_.valid()) return;
  watch_frequency_ = watch_frequency_changes;
  ip_.write32(kFwSocIrqEnable, kFirewallSources);
  ip_.write64(kIcbEnable0, icb_both_mask());
  // Interrupts whose bit is set in the local mask are blocked; the global mask 0 lets the enabled ones through.
  btrs_.write32(lnl() ? kLnlLocalMask : kMtlLocalMask, ~buttress_mask());
  btrs_.write32(lnl() ? kLnlGlobalMask : kMtlGlobalMask, 0);
}

void IntelNpuIrq::disable() {
  if (!platform_.valid()) return;
  btrs_.write32(lnl() ? kLnlGlobalMask : kMtlGlobalMask, 1);
  btrs_.write32(lnl() ? kLnlLocalMask : kMtlLocalMask, kBtrsDisableAll);
  ip_.write64(kIcbEnable0, 0);
  ip_.write32(kFwSocIrqEnable, 0);
}

void IntelNpuIrq::clear() { ip_.write64(kIcbClear0, icb_both_mask()); }

bool IntelNpuIrq::handle_buttress(IntelNpuIrqResult& r) {
  const uint64_t status_reg = lnl() ? kLnlStatus : kMtlStatus;
  const uint32_t status = btrs_.read32(status_reg) & buttress_mask();
  if (status == 0) return false;
  if (lnl()) {
    if (status & kLnlSurvErr) r.survivability = true;
    if (status & kLnlFreqChange) r.frequency_change = true;
    // Each error is cleared at its source before the status bit.
    if (status & kLnlAtsErr) { btrs_.write32(kLnlAtsErrClear, 1); r.buttress_error = true; }
    if (status & kLnlCfi0Err) { btrs_.write32(kLnlCfi0ErrClear, 1); r.buttress_error = true; }
    if (status & kLnlCfi1Err) { btrs_.write32(kLnlCfi1ErrClear, 1); r.buttress_error = true; }
    if (status & kLnlImr0Err) { btrs_.write32(kLnlImr0ErrClear, 1); r.buttress_error = true; }
    if (status & kLnlImr1Err) { btrs_.write32(kLnlImr1ErrClear, 1); r.buttress_error = true; }
  } else {
    if (status & kMtlFreqChange) r.frequency_change = true;
    if (status & kMtlAtsErr) { btrs_.write32(kMtlAtsErrClear, 1); r.buttress_error = true; }
    if (status & kMtlUfiErr) { btrs_.write32(kMtlUfiErrClear, 1); r.buttress_error = true; }
  }
  btrs_.write32(status_reg, status);  // after the sources: clearing the status bit first would let the line fire again
  if (r.buttress_error) r.recovery_needed = true;
  return true;
}

bool IntelNpuIrq::handle_ip(IntelNpuIrqResult& r, IntelNpuIpc* ipc, IntelNpuMmu* mmu) {
  const uint32_t status = ip_.read32(kIcbStatus0) & kIcb0Mask;
  if (status == 0) return false;
  ip_.write32(kIcbClear0, status);
  if (status & kIcbMmu0) r.mmu_events = true;
  if (status & kIcbIpcFifo) {
    r.ipc = true;
    if (ipc) ipc->irq_handler();
  }
  if (status & kIcbMmu1) r.mmu_sync = true;
  if (status & kIcbMmu2) {
    r.mmu_errors = true;
    if (mmu) mmu->acknowledge_global_errors();
  }
  if (status & kIcbRedirect0) { r.watchdog_mss = true; r.recovery_needed = true; }
  if (status & kIcbRedirect1) { r.watchdog_nce = true; r.recovery_needed = true; }
  if (status & kIcbFirewall) { r.firewall = true; ++firewall_count_; }
  return true;
}

IntelNpuIrqResult IntelNpuIrq::handle(IntelNpuIpc* ipc, IntelNpuMmu* mmu) {
  IntelNpuIrqResult r;
  if (!platform_.valid()) return r;
  const uint64_t global = lnl() ? kLnlGlobalMask : kMtlGlobalMask;
  btrs_.write32(global, 1);
  const bool btrs_handled = handle_buttress(r);
  const bool ip_handled = handle_ip(r, ipc, mmu);
  // Unmasking again re-triggers the line if something arrived meanwhile.
  btrs_.write32(global, 0);
  r.handled = btrs_handled || ip_handled;
  return r;
}

}  // namespace hardware
