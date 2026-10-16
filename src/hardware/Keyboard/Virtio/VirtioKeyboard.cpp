#include "hardware/Keyboard/Virtio/VirtioKeyboard.hpp"

namespace hardware {

void VirtioKeyboard::fetch() {
  VirtioInputEvent e;
  while (device_.next_event(&e)) {
    if (e.type != kEvKey || e.value == 2) continue;  // ignore sync markers and auto-repeat
    const uint8_t usage = usage_from_linux_key(e.code);
    if (usage) push(usage, e.value == 1);
  }
}

}  // namespace hardware
