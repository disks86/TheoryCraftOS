#include "hardware/Mouse/MouseBase.hpp"

namespace hardware {

void MouseBase::push(const MouseEvent& event) {
  if (count_ == kCapacity) return;  // drop on overflow
  MouseEvent e = event;
  if (e.absolute) {  // let consumers of a merged stream scale positions
    e.max_x = max_x();
    e.max_y = max_y();
  }
  queue_[(head_ + count_) % kCapacity] = e;
  ++count_;
}

bool MouseBase::poll(MouseEvent* out) {
  if (count_ == 0) fetch();
  if (count_ == 0 || !out) return false;
  *out = queue_[head_];
  head_ = (head_ + 1) % kCapacity;
  --count_;
  return true;
}

}  // namespace hardware
