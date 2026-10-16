#pragma once
#include <stdint.h>

namespace hardware {

// A HID interface found in a configuration descriptor, with its first
// interrupt-IN endpoint.
struct UsbHidInterface {
  uint8_t configuration_value;
  uint8_t number;
  uint8_t subclass;  // 1 = boot interface
  uint8_t protocol;  // with subclass 1: 1 = keyboard, 2 = mouse
  uint8_t endpoint_address;
  uint16_t max_packet;
  uint8_t interval;
  uint16_t report_descriptor_length;

  bool is_boot_keyboard() const { return subclass == 1 && protocol == 1; }
  bool is_boot_mouse() const { return subclass == 1 && protocol == 2; }
};

// Lists the HID interfaces (class 3) that have an interrupt-IN endpoint.
// Returns how many were found (up to `capacity` are stored).
uint32_t find_hid_interfaces(const uint8_t* configuration, uint16_t length, UsbHidInterface* out,
                             uint32_t capacity);

}  // namespace hardware
