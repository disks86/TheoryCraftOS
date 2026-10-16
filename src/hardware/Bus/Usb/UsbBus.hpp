#pragma once
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/Bus/Usb/IUsbBus.hpp"

namespace hardware {

// USB host controllers discovered on a PCI bus. For xHCI controllers the root
// ports are read straight from the controller's registers (read-only: the
// firmware keeps ownership, so nothing is reset). UHCI, OHCI and EHCI
// controllers are identified but their ports are not read yet. Attached
// devices are detected, not enumerated: no descriptors are fetched.
class UsbBus final : public IUsbBus {
 public:
  static constexpr uint32_t kMaxControllers = 8;

  explicit constexpr UsbBus(const IPciBus& pci) : pci_(pci) {}

  const char* name() const override { return "USB"; }
  bool initialize() override;
  void shutdown() override { controller_count_ = 0; }

  uint32_t device_count() const override { return connected_; }
  uint32_t controller_count() const override { return controller_count_; }
  UsbControllerKind controller_kind(uint32_t controller) const override;
  uint32_t port_count(uint32_t controller) const override;
  bool port_info(uint32_t controller, uint32_t port, UsbPortInfo* out) const override;

 private:
  struct Controller {
    UsbControllerKind kind;
    uint32_t ports;
    volatile uint32_t* port_registers;  // xHCI PORTSC array, else null
  };

  const IPciBus& pci_;
  Controller controllers_[kMaxControllers] = {};
  uint32_t controller_count_ = 0;
  uint32_t connected_ = 0;
};

}  // namespace hardware
