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
  // The full configuration descriptor (configuration, interfaces, endpoints, class
  // descriptors) of the device on a root port, cached during enumeration.
  virtual bool configuration_descriptor(uint32_t port, uint8_t* buffer, uint16_t capacity,
                                        uint16_t* length) const = 0;

  // --- Device control (endpoint 0). `port` identifies an enumerated device.
  // Selects a configuration; repeating the call for the same value is a no-op.
  virtual bool set_configuration(uint32_t port, uint8_t configuration) = 0;
  // Control transfer with no data stage.
  virtual bool control_out(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                           uint16_t index) = 0;
  // Control transfer reading `length` bytes (at most 4096) into `buffer`.
  virtual bool control_in(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value,
                          uint16_t index, void* buffer, uint16_t length) = 0;

  // --- Interrupt IN endpoints (HID reports and the like).
  // Configures the endpoint and starts receiving packets of up to max_packet
  // bytes. `interval` is the descriptor's bInterval.
  virtual bool start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet,
                                  uint8_t interval) = 0;
  // If a packet has arrived, copies it out (at most `capacity` bytes), re-arms
  // the endpoint and returns true. Never blocks.
  virtual bool poll_interrupt_in(uint32_t port, uint8_t endpoint_address, void* buffer,
                                 uint16_t capacity, uint16_t* length) = 0;

 protected:
  constexpr IUsbHostController() = default;
  ~IUsbHostController() = default;
};

}  // namespace hardware
