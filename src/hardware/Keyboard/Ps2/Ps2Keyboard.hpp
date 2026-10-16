#pragma once
#include "hardware/Bus/Ps2/I8042Controller.hpp"
#include "hardware/Keyboard/KeyboardBase.hpp"

namespace hardware {

// A PS/2 keyboard on the i8042. The controller translates the keyboard's scan
// code set 2 into set 1, which is decoded here (including the 0xE0 extended
// keys) into HID usages.
class Ps2Keyboard final : public KeyboardBase {
 public:
  explicit constexpr Ps2Keyboard(I8042Controller& controller) : controller_(controller) {}

  const char* name() const override { return "PS/2 keyboard"; }
  bool initialize() override;  // false if no keyboard answers
  void shutdown() override {}

 private:
  void fetch() override;

  I8042Controller& controller_;
  bool extended_ = false;   // saw 0xE0
  uint8_t skip_ = 0;        // bytes of the Pause sequence still to ignore
};

}  // namespace hardware
