#include "hardware/Keyboard/KeyboardBase.hpp"

namespace hardware {

void KeyboardBase::push(uint8_t usage, bool pressed) {
  if (usage >= key::kLeftControl && usage <= key::kRightGui) {
    const uint8_t bit = 1u << (usage - key::kLeftControl);
    modifiers_ = pressed ? (modifiers_ | bit) : (modifiers_ & ~bit);
  }
  if (count_ == kCapacity) return;  // drop on overflow
  queue_[(head_ + count_) % kCapacity] = {usage, pressed};
  ++count_;
}

bool KeyboardBase::poll(KeyEvent* out) {
  if (count_ == 0) fetch();
  if (count_ == 0 || !out) return false;
  *out = queue_[head_];
  head_ = (head_ + 1) % kCapacity;
  --count_;
  return true;
}

}  // namespace hardware
