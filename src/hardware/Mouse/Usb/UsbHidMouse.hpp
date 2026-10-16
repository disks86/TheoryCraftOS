#pragma once
#include "hardware/Bus/Usb/Hid/HidReportParser.hpp"
#include "hardware/Bus/Usb/Hid/UsbHidInterface.hpp"
#include "hardware/Bus/Usb/IUsbHostController.hpp"
#include "hardware/Mouse/MouseBase.hpp"

namespace hardware {

// A USB mouse or tablet. The device's HID report descriptor is parsed to find
// the button, X, Y and wheel fields, so relative mice and absolute tablets (such
// as QEMU's usb-tablet) both work. A boot-protocol mouse whose descriptor cannot
// be parsed falls back to the fixed 3-byte boot layout.
class UsbHidMouse final : public MouseBase {
 public:
  UsbHidMouse(IUsbHostController& host, uint32_t port, const UsbHidInterface& interface)
      : host_(host), port_(port), interface_(interface) {}

  const char* name() const override { return layout_.x.relative ? "USB HID mouse" : "USB HID tablet"; }
  bool initialize() override;
  void shutdown() override {}

  bool is_absolute() const override { return !layout_.x.relative; }
  uint32_t max_x() const override { return is_absolute() ? layout_.x.logical_max : 0; }
  uint32_t max_y() const override { return is_absolute() ? layout_.y.logical_max : 0; }

 private:
  void fetch() override;

  IUsbHostController& host_;
  uint32_t port_;
  UsbHidInterface interface_;
  HidPointerLayout layout_;
};

}  // namespace hardware
