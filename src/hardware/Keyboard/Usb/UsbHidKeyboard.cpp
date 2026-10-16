#include "hardware/Keyboard/Usb/UsbHidKeyboard.hpp"

namespace hardware {

namespace {
constexpr uint8_t kRequestSetIdle = 0x0A;
constexpr uint8_t kRequestSetProtocol = 0x0B;
constexpr uint8_t kClassInterfaceOut = 0x21;  // host-to-device, class, interface

bool contains(const uint8_t* keys, uint8_t usage) {
  for (int i = 2; i < 8; ++i)
    if (keys[i] == usage) return true;
  return false;
}
}  // namespace

bool UsbHidKeyboard::initialize() {
  if (!host_.set_configuration(port_, interface_.configuration_value)) return false;
  // Boot protocol (a fixed report layout) and "report only on change".
  if (!host_.control_out(port_, kClassInterfaceOut, kRequestSetProtocol, 0, interface_.number)) return false;
  host_.control_out(port_, kClassInterfaceOut, kRequestSetIdle, 0, interface_.number);  // optional
  return host_.start_interrupt_in(port_, interface_.endpoint_address, interface_.max_packet,
                                  interface_.interval);
}

void UsbHidKeyboard::fetch() {
  uint8_t report[8];
  uint16_t length = 0;
  while (host_.poll_interrupt_in(port_, interface_.endpoint_address, report, sizeof(report), &length)) {
    if (length < 8) continue;

    for (int bit = 0; bit < 8; ++bit) {  // modifier byte: one bit per modifier key
      const bool now = report[0] & (1u << bit), before = previous_[0] & (1u << bit);
      if (now != before) push(key::kLeftControl + bit, now);
    }
    for (int i = 2; i < 8; ++i) {
      if (report[i] > 3 && !contains(previous_, report[i])) push(report[i], true);   // 1-3 = rollover errors
      if (previous_[i] > 3 && !contains(report, previous_[i])) push(previous_[i], false);
    }
    __builtin_memcpy(previous_, report, sizeof(previous_));
  }
}

}  // namespace hardware
