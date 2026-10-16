#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/Bus/Usb/IUsbBus.hpp"
#include "hardware/Bus/Usb/IUsbHostController.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

// USB host controllers discovered on a PCI bus. xHCI controllers get a driver
// (XhciController) that enumerates the devices on their root ports. UHCI, OHCI
// and EHCI controllers are identified but have no driver yet, so they report no
// ports and no devices.
class UsbBus final : public IUsbBus {
 public:
  UsbBus(const IPciBus& pci, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), allocator_(allocator), st_(st), controllers_(allocator) {}

  const char* name() const override { return "USB"; }
  bool initialize() override;
  void shutdown() override;

  uint32_t device_count() const override;
  uint32_t controller_count() const override { return static_cast<uint32_t>(controllers_.count()); }
  UsbControllerKind controller_kind(uint32_t controller) const override;
  uint32_t port_count(uint32_t controller) const override;
  bool port_info(uint32_t controller, uint32_t port, UsbPortInfo* out) const override;
  bool device_descriptor(uint32_t controller, uint32_t port, UsbDeviceDescriptor* out) const override;
  IUsbHostController* host_controller(uint32_t controller) override {
    return controller < controllers_.count() ? controllers_[controller].host.get() : nullptr;
  }

 private:
  struct Controller {
    UsbControllerKind kind;
    UniquePointer<IUsbHostController> host;  // null when there is no driver (or it failed)
  };

  const IPciBus& pci_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  List<Controller> controllers_;
};

}  // namespace hardware
