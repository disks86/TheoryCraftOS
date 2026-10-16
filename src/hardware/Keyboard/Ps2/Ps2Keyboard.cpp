#include "hardware/Keyboard/Ps2/Ps2Keyboard.hpp"

namespace hardware {

bool Ps2Keyboard::initialize() {
  extended_ = false;
  skip_ = 0;
  return controller_.send_keyboard(0xF4);  // enable scanning
}

void Ps2Keyboard::fetch() {
  uint8_t byte;
  while (controller_.read_keyboard(&byte)) {
    if (skip_) { --skip_; continue; }
    if (byte == 0xE0) { extended_ = true; continue; }
    if (byte == 0xE1) { skip_ = 5; continue; }              // Pause/Break: ignore the sequence
    if (byte == 0xFA || byte == 0xFE || byte == 0x00 || byte == 0xFF) continue;

    const bool released = byte & 0x80;
    const uint8_t code = byte & 0x7F;
    const uint8_t usage = usage_from_scan_code_set1(code, extended_);
    extended_ = false;
    if (usage) push(usage, !released);
  }
}

}  // namespace hardware
