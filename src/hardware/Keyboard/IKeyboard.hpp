#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

struct KeyEvent {
  uint8_t usage;  // USB HID keyboard usage (see KeyCodes.hpp)
  bool pressed;   // false = released
};

// A keyboard delivering key press/release events. Polled: nothing blocks.
// Implementations live in subfolders by transport (Ps2/, Usb/, Virtio/).
class IKeyboard : public IHardware {
 public:
  // Returns the next event, or false when there is none right now.
  virtual bool poll(KeyEvent* out) = 0;
  // Modifier keys currently held (modifier:: bits).
  virtual uint8_t modifiers() const = 0;

 protected:
  constexpr IKeyboard() = default;
  ~IKeyboard() = default;
};

}  // namespace hardware
