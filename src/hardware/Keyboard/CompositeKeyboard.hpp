#pragma once
#include "hardware/Keyboard/IKeyboard.hpp"

namespace hardware {

// Presents several keyboards as one: poll() returns events from whichever has
// them, and modifiers() is the union. Children are not owned.
class CompositeKeyboard final : public IKeyboard {
 public:
  static constexpr uint32_t kMaxKeyboards = 8;

  constexpr CompositeKeyboard() = default;

  const char* name() const override { return "keyboards"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  bool add(IKeyboard* keyboard);
  uint32_t count() const { return count_; }
  IKeyboard* at(uint32_t index) const { return index < count_ ? keyboards_[index] : nullptr; }

  bool poll(KeyEvent* out) override;
  uint8_t modifiers() const override;

 private:
  IKeyboard* keyboards_[kMaxKeyboards] = {};
  uint32_t count_ = 0;
  uint32_t next_ = 0;  // round-robin start, so one chatty device cannot starve the rest
};

}  // namespace hardware
