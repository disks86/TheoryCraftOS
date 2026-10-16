#pragma once
#include "hardware/Bus/Usb/Hid/UsbHidInterface.hpp"
#include "hardware/Bus/Usb/IUsbHostController.hpp"
#include "hardware/Keyboard/KeyboardBase.hpp"

namespace hardware {

// A USB keyboard speaking the HID boot protocol: 8-byte reports with a modifier
// byte and up to six pressed keys, which are diffed into press/release events.
class UsbHidKeyboard final : public KeyboardBase {
 public:
  UsbHidKeyboard(IUsbHostController& host, uint32_t port, const UsbHidInterface& interface)
      : host_(host), port_(port), interface_(interface) {}

  const char* name() const override { return "USB HID keyboard"; }
  bool initialize() override;
  void shutdown() override {}

 private:
  void fetch() override;

  IUsbHostController& host_;
  uint32_t port_;
  UsbHidInterface interface_;
  uint8_t previous_[8] = {};
};

}  // namespace hardware
