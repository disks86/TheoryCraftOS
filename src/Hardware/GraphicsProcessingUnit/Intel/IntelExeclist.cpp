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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelExeclist.hpp"

namespace hardware {
namespace intel_gt {

namespace {
// The status page is written by the GPU behind the compiler's back: every access goes to memory, in order.
uint32_t load(const uint32_t& word) {
  const volatile uint32_t& v = word;
  const uint32_t value = v;
  __asm__ volatile("" ::: "memory");
  return value;
}
void store(uint32_t& word, uint32_t value) {
  volatile uint32_t& v = word;
  __asm__ volatile("" ::: "memory");
  v = value;
}
}  // namespace

bool ExeclistPort::enable() {
  if (!registers_ || status_.size() != kStatusPageDwords || (status_ggtt_ & 4095)) return false;
  // The CSB pointers first (reset_csb_pointers): the hardware starts writing at entry 0, so our head sits one entry before it, and the write pointer we
  // compare against is faked to the last entry until the hardware writes its own.
  const uint32_t kReset = entries() - 1;
  registers_->write32(base_ + kRingContextStatusPtr, (0xFFFFu << 16) | (kReset << 8) | kReset);
  registers_->read32(base_ + kRingContextStatusPtr);
  csb_head_ = kReset;
  store(status_[write_dword()], kReset);
  for (uint32_t i = 0; i < entries() * 2; ++i) store(status_[kCsbFirstDword + i], 0xFFFFFFFFu);  // to see that the hardware really writes them
  registers_->write32(base_ + kRingContextStatusPtr, (0xFFFFu << 16) | (kReset << 8) | kReset);
  registers_->read32(base_ + kRingContextStatusPtr);

  // enable_execlists()
  registers_->write32(base_ + kRingHwstam, ~0u);
  // GFX_RUN_LIST_ENABLE; from Gen11 on the legacy ring mode is disabled instead
  registers_->write32(base_ + kRingModeGen7, masked_enable(mode_ == Mode::Elsq ? kGen11DisableLegacyMode : 1u << 15));
  registers_->write32(base_ + kRingMiMode, masked_disable(1u << 8));    // not STOP_RING
  registers_->write32(base_ + kRingHwsPga, status_ggtt_);
  registers_->read32(base_ + kRingHwsPga);
  return true;
}

void ExeclistPort::submit(uint64_t descriptor) {
  if (!registers_) return;
  if (mode_ == Mode::Elsq) {
    // Port 1 first (empty), then port 0 with the context: the low dword, then the high one; the queue is started by a write to the control register.
    registers_->write32(base_ + kRingExeclistSqContents + 8, 0);
    registers_->write32(base_ + kRingExeclistSqContents + 12, 0);
    registers_->write32(base_ + kRingExeclistSqContents + 0, static_cast<uint32_t>(descriptor));
    registers_->write32(base_ + kRingExeclistSqContents + 4, static_cast<uint32_t>(descriptor >> 32));
    registers_->write32(base_ + kRingExeclistControl, kControlLoad);
    return;
  }
  // Each element is written as its upper half, then its lower half; the second element (0) starts the load.
  registers_->write32(base_ + kRingElsp, 0);
  registers_->write32(base_ + kRingElsp, 0);
  registers_->write32(base_ + kRingElsp, static_cast<uint32_t>(descriptor >> 32));
  registers_->write32(base_ + kRingElsp, static_cast<uint32_t>(descriptor));
}

bool ExeclistPort::stop(uint32_t attempts) {
  if (!registers_) return false;
  constexpr uint32_t kStopRing = 1u << 8, kModeIdle = 1u << 9;
  registers_->write32(base_ + kRingMiMode, masked_enable(kStopRing));
  for (uint32_t i = 0; i < attempts; ++i)
    if (registers_->read32(base_ + kRingMiMode) & kModeIdle) return true;
  return false;
}

ExeclistPort::Events ExeclistPort::poll() {
  Events e;
  if (status_.size() != kStatusPageDwords) return e;
  if (mode_ == Mode::Elsq) {
    const uint32_t tail = load(status_[kCsb12WriteDword]) & 0xF;
    if (tail >= kCsb12Entries) return e;
    uint32_t head = csb_head_;
    while (head != tail) {
      const uint32_t next = head + 1 == kCsb12Entries ? 0 : head + 1;
      const uint32_t low = load(status_[kCsbFirstDword + next * 2]), high = load(status_[kCsbFirstDword + next * 2 + 1]);
      if (low == 0xFFFFFFFFu && high == 0xFFFFFFFFu) break;   // the pointer is ahead of the entry (the hardware does not always order the two): look again later
      head = next;
      e.status_or |= low | high;
      const bool to_valid = ((low >> 15) & 0x7FF) != kIdleContextId, away_valid = ((high >> 15) & 0x7FF) != kIdleContextId, new_queue = (low & 1) != 0;
      if (!away_valid || new_queue) ++e.started;
      else ++e.completed;
      if (!to_valid) ++e.idle;
    }
    csb_head_ = head;
    return e;
  }
  uint32_t tail = load(status_[kCsbWriteDword]) & 0x7;
  if (tail >= kCsbEntries || csb_head_ == tail) return e;  // 6 and 7 are not valid pointers
  uint32_t head = csb_head_;
  csb_head_ = tail;
  do {
    if (++head == kCsbEntries) head = 0;
    const uint32_t status = load(status_[kCsbFirstDword + head * 2]);
    e.status_or |= status;
    if (status & (kStatusIdleToActive | kStatusPreempted)) ++e.started;
    else ++e.completed;
    if (status & kStatusActiveToIdle) ++e.idle;
  } while (head != tail);
  return e;
}

}  // namespace intel_gt
}  // namespace hardware
