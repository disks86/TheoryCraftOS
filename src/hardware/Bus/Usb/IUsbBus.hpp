#pragma once
#include <stdint.h>
#include "hardware/Bus/IBus.hpp"

namespace hardware {

enum class UsbControllerKind : uint8_t { Uhci, Ohci, Ehci, Xhci, Unknown };
enum class UsbSpeed : uint8_t { Unknown, Low, Full, High, Super, SuperPlus };

struct UsbPortInfo {
  bool connected;  // a device is attached
  UsbSpeed speed;  // meaningful only when connected
};

// All USB host controllers in the system, seen as one bus. Controllers are
// numbered 0..controller_count()-1; device_count() is the number of attached
// devices across every readable port.
class IUsbBus : public IBus {
 public:
  virtual uint32_t controller_count() const = 0;
  virtual UsbControllerKind controller_kind(uint32_t controller) const = 0;
  // Root ports of a controller; 0 when this controller type cannot be read yet.
  virtual uint32_t port_count(uint32_t controller) const = 0;
  virtual bool port_info(uint32_t controller, uint32_t port, UsbPortInfo* out) const = 0;

 protected:
  constexpr IUsbBus() = default;
  ~IUsbBus() = default;
};

}  // namespace hardware
