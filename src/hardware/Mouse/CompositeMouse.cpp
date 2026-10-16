#include "hardware/Mouse/CompositeMouse.hpp"

namespace hardware {

bool CompositeMouse::add(IMouse* mouse) {
  if (!mouse || count_ == kMaxMice) return false;
  mice_[count_++] = mouse;
  return true;
}

bool CompositeMouse::poll(MouseEvent* out) {
  for (uint32_t i = 0; i < count_; ++i) {
    IMouse* m = mice_[(next_ + i) % count_];
    if (m->poll(out)) {
      next_ = (next_ + i + 1) % count_;
      return true;
    }
  }
  return false;
}

}  // namespace hardware
