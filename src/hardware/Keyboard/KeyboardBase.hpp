#pragma once
#include "hardware/Keyboard/IKeyboard.hpp"
#include "hardware/Keyboard/KeyCodes.hpp"

namespace hardware {

// Event queue and modifier tracking shared by keyboards. A subclass implements
// fetch(), which reads whatever the device has and calls push() for each event.
class KeyboardBase : public IKeyboard {
 public:
  bool poll(KeyEvent* out) override;
  uint8_t modifiers() const override { return modifiers_; }

 protected:
  constexpr KeyboardBase() = default;
  ~KeyboardBase() = default;

  virtual void fetch() = 0;
  void push(uint8_t usage, bool pressed);

 private:
  static constexpr uint32_t kCapacity = 64;
  KeyEvent queue_[kCapacity] = {};
  uint32_t head_ = 0, count_ = 0;
  uint8_t modifiers_ = 0;
};

}  // namespace hardware
