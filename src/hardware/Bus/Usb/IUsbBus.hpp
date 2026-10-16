#pragma once
#include <stdint.h>
#include "hardware/Bus/IBus.hpp"
#include "hardware/Bus/Usb/IUsbHostController.hpp"
#include "hardware/Bus/Usb/UsbTypes.hpp"

namespace hardware {

// All USB host controllers in the system, seen as one bus. Controllers are
// numbered 0..controller_count()-1; device_count() is the number of devices that
// were enumerated (addressed and described) across all controllers.
class IUsbBus : public IBus {
 public:
  virtual uint32_t controller_count() const = 0;
  virtual UsbControllerKind controller_kind(uint32_t controller) const = 0;
  // Root ports of a controller; 0 when this controller type cannot be read yet.
  virtual uint32_t port_count(uint32_t controller) const = 0;
  virtual bool port_info(uint32_t controller, uint32_t port, UsbPortInfo* out) const = 0;
  // Device descriptor of the device enumerated on a root port; false if none.
  virtual bool device_descriptor(uint32_t controller, uint32_t port, UsbDeviceDescriptor* out) const = 0;
  // The driver for a controller (to talk to its devices), or nullptr if it has none.
  virtual IUsbHostController* host_controller(uint32_t controller) = 0;

 protected:
  constexpr IUsbBus() = default;
  ~IUsbBus() = default;
};

}  // namespace hardware
