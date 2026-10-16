#pragma once
#include "hardware/Bus/Usb/UsbTypes.hpp"
#include "hardware/IHardware.hpp"

namespace hardware {

// One USB host controller (UHCI, OHCI, EHCI, xHCI, ...). initialize() brings
// the controller up and enumerates what is plugged into its root ports.
class IUsbHostController : public IHardware {
 public:
  virtual UsbControllerKind kind() const = 0;

  virtual uint32_t port_count() const = 0;  // root ports
  virtual bool port_info(uint32_t port, UsbPortInfo* out) const = 0;

  // Descriptor of the device enumerated on a root port during initialize().
  // False if nothing was enumerated there.
  virtual bool device_descriptor(uint32_t port, UsbDeviceDescriptor* out) const = 0;

 protected:
  constexpr IUsbHostController() = default;
  ~IUsbHostController() = default;
};

}  // namespace hardware
