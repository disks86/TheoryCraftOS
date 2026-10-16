#include "hardware/Bus/Usb/Xhci/XhciRing.hpp"

namespace hardware::xhci {

bool ProducerRing::init(allocators::IAllocator& allocator) {
  trbs_ = static_cast<Trb*>(allocator.allocate_aligned(kRingTrbs * sizeof(Trb), 4096));
  if (!trbs_) return false;
  __builtin_memset(trbs_, 0, kRingTrbs * sizeof(Trb));
  index_ = 0;
  cycle_ = 1;
  return true;
}

void ProducerRing::release(allocators::IAllocator& allocator) {
  allocator.deallocate(trbs_);
  trbs_ = nullptr;
}

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
  trbs_ = static_cast<Trb*>(allocator.allocate_aligned(kRingTrbs * sizeof(Trb), 4096));
  table_ = static_cast<SegmentEntry*>(allocator.allocate_aligned(sizeof(SegmentEntry), 64));
  if (!trbs_ || !table_) {
    release(allocator);
    return false;
  }
  __builtin_memset(trbs_, 0, kRingTrbs * sizeof(Trb));
  table_->base = address();
  table_->size = kRingTrbs;
  table_->reserved = 0;
  index_ = 0;
  cycle_ = 1;
  return true;
}

void EventRing::release(allocators::IAllocator& allocator) {
  allocator.deallocate(trbs_);
  allocator.deallocate(table_);
  trbs_ = nullptr;
  table_ = nullptr;
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
