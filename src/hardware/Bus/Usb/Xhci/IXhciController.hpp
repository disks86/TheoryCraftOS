#pragma once
#include "hardware/Bus/Usb/IUsbHostController.hpp"

namespace hardware {

// An xHCI (USB 3.x) host controller: the generic controller interface plus the
// capabilities xHCI reports about itself.
class IXhciController : public IUsbHostController {
 public:
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }

  virtual uint16_t interface_version() const = 0;  // BCD, e.g. 0x0100 = xHCI 1.0
  virtual uint32_t max_device_slots() const = 0;
  virtual bool uses_64_byte_contexts() const = 0;

 protected:
  constexpr IXhciController() = default;
  ~IXhciController() = default;
};

}  // namespace hardware
