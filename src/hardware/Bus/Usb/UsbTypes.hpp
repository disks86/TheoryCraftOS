#pragma once
#include <stdint.h>

namespace hardware {

enum class UsbControllerKind : uint8_t { Uhci, Ohci, Ehci, Xhci, Unknown };
enum class UsbSpeed : uint8_t { Unknown, Low, Full, High, Super, SuperPlus };

struct UsbPortInfo {
  bool connected;  // a device is attached
  UsbSpeed speed;  // meaningful only when connected
};

// USB 2.0 standard device descriptor (section 9.6.1), as sent on the wire.
struct UsbDeviceDescriptor {
  uint8_t length;
  uint8_t descriptor_type;  // 1
  uint16_t usb_version;     // BCD, e.g. 0x0200
  uint8_t device_class;
  uint8_t device_subclass;
  uint8_t device_protocol;
  uint8_t max_packet_size0;  // for SuperSpeed this is an exponent: size = 2^value
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t device_version;
  uint8_t manufacturer_string;
  uint8_t product_string;
  uint8_t serial_string;
  uint8_t configuration_count;
} __attribute__((packed));
static_assert(sizeof(UsbDeviceDescriptor) == 18);

}  // namespace hardware
