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
  if (status_.size() != kStatusPageDwords || (status_ggtt_ & 4095)) return false;
  // The CSB pointers first (reset_csb_pointers): the hardware starts writing at entry 0, so our head sits one entry before it, and the write pointer we
  // compare against is faked to the last entry until the hardware writes its own.
  constexpr uint32_t kReset = kCsbEntries - 1;
  registers_.write32(base_ + kRingContextStatusPtr, (0xFFFFu << 16) | (kReset << 8) | kReset);
  registers_.read32(base_ + kRingContextStatusPtr);
  csb_head_ = kReset;
  store(status_[kCsbWriteDword], kReset);
  for (uint32_t i = 0; i < kCsbEntries * 2; ++i) store(status_[kCsbFirstDword + i], 0xFFFFFFFFu);  // to see that the hardware really writes them
  registers_.write32(base_ + kRingContextStatusPtr, (0xFFFFu << 16) | (kReset << 8) | kReset);
  registers_.read32(base_ + kRingContextStatusPtr);

  // enable_execlists()
  registers_.write32(base_ + kRingHwstam, ~0u);
  registers_.write32(base_ + kRingModeGen7, masked_enable(1u << 15));  // GFX_RUN_LIST_ENABLE
  registers_.write32(base_ + kRingMiMode, masked_disable(1u << 8));    // not STOP_RING
  registers_.write32(base_ + kRingHwsPga, status_ggtt_);
  registers_.read32(base_ + kRingHwsPga);
  return true;
}

void ExeclistPort::submit(uint64_t descriptor) {
  // Each element is written as its upper half, then its lower half; the second element (0) starts the load.
  registers_.write32(base_ + kRingElsp, 0);
  registers_.write32(base_ + kRingElsp, 0);
  registers_.write32(base_ + kRingElsp, static_cast<uint32_t>(descriptor >> 32));
  registers_.write32(base_ + kRingElsp, static_cast<uint32_t>(descriptor));
}

ExeclistPort::Events ExeclistPort::poll() {
  Events e;
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
