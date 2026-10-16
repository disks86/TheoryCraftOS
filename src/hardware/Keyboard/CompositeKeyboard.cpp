#include "hardware/Keyboard/CompositeKeyboard.hpp"

namespace hardware {

bool CompositeKeyboard::add(IKeyboard* keyboard) {
  if (!keyboard || count_ == kMaxKeyboards) return false;
  keyboards_[count_++] = keyboard;
  return true;
}

bool CompositeKeyboard::poll(KeyEvent* out) {
  for (uint32_t i = 0; i < count_; ++i) {
    IKeyboard* k = keyboards_[(next_ + i) % count_];
    if (k->poll(out)) {
      next_ = (next_ + i + 1) % count_;
      return true;
    }
  }
  return false;
}

uint8_t CompositeKeyboard::modifiers() const {
  uint8_t all = 0;
  for (uint32_t i = 0; i < count_; ++i) all |= keyboards_[i]->modifiers();
  return all;
}

}  // namespace hardware
