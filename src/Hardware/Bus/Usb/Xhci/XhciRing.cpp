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

#include "Hardware/Bus/Usb/Xhci/XhciRing.hpp"

namespace hardware::xhci {

bool ProducerRing::init(allocators::IAllocator& allocator) {
  trbs_ = allocate_dma_view<Trb>(allocator, kRingTrbs, 4096);
  if (trbs_.empty()) return false;
  __builtin_memset(trbs_.data(), 0, trbs_.size_bytes());
  index_ = 0;
  cycle_ = 1;
  return true;
}

void ProducerRing::release(allocators::IAllocator& allocator) { release_view(allocator, trbs_); }

uint64_t ProducerRing::next_address() {
  wrap_if_needed();
  return address() + uint64_t{index_} * sizeof(Trb);
}

uint64_t ProducerRing::push(uint64_t parameter, uint32_t status, uint32_t control) {
  wrap_if_needed();
  Trb* trb = &trbs_[index_++];
  trb->parameter = parameter;
  trb->status = status;
  __atomic_thread_fence(__ATOMIC_RELEASE);  // the cycle bit makes the TRB visible: write it last
  trb->control = (control & ~kTrbCycle) | cycle_;
  return reinterpret_cast<uint64_t>(trb);
}

void ProducerRing::wrap_if_needed() {
  if (index_ == kRingTrbs - 1) {  // reached the Link TRB: hand it over and wrap
    Trb* link = &trbs_[index_];
    link->parameter = address();
    link->status = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    link->control = (kTrbLink << 10) | kTrbToggleCycle | cycle_;
    cycle_ ^= 1;
    index_ = 0;
  }
}

bool EventRing::init(allocators::IAllocator& allocator) {
  trbs_ = allocate_dma_view<Trb>(allocator, kRingTrbs, 4096);
  table_ = allocate_dma_view<SegmentEntry>(allocator, 1, 64);
  if (trbs_.empty() || table_.empty()) {
    release(allocator);
    return false;
  }
  __builtin_memset(trbs_.data(), 0, trbs_.size_bytes());
  table_[0].base = address();
  table_[0].size = kRingTrbs;
  table_[0].reserved = 0;
  index_ = 0;
  cycle_ = 1;
  return true;
}

void EventRing::release(allocators::IAllocator& allocator) {
  release_view(allocator, trbs_);
  release_view(allocator, table_);
}

bool EventRing::pop(Trb* out) {
  const Trb* trb = &trbs_[index_];
  const uint32_t control = reinterpret_cast<const volatile Trb*>(trb)->control;
  if ((control & kTrbCycle) != cycle_) return false;  // nothing new
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  *out = *trb;
  if (++index_ == kRingTrbs) {
    index_ = 0;
    cycle_ ^= 1;
  }
  return true;
}

}  // namespace hardware::xhci
