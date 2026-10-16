#include "hardware/Bus/Usb/UsbBus.hpp"
#include <new>
#include "hardware/Bus/Usb/Xhci/XhciController.hpp"

namespace hardware {

namespace {
// PCI programming interface values for class 0x0C, subclass 0x03.
UsbControllerKind kind_from_prog_if(uint8_t prog_if) {
  switch (prog_if) {
    case 0x00: return UsbControllerKind::Uhci;
    case 0x10: return UsbControllerKind::Ohci;
    case 0x20: return UsbControllerKind::Ehci;
    case 0x30: return UsbControllerKind::Xhci;
    default:   return UsbControllerKind::Unknown;
  }
}
}  // namespace

bool UsbBus::initialize() {
  controller_count_ = 0;
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.class_code != kPciClassSerialBus || d.subclass != kPciSubclassUsb) continue;
    const UsbControllerKind kind = kind_from_prog_if(d.prog_if);
    if (kind == UsbControllerKind::Unknown || controller_count_ == kMaxControllers) continue;

    Controller& c = controllers_[controller_count_++];
    c = {kind, nullptr};
    if (kind != UsbControllerKind::Xhci) continue;

    void* memory = allocator_.allocate(sizeof(XhciController));
    if (!memory) continue;
    auto* xhci = new (memory) XhciController(pci_, d, allocator_, st_);
    if (xhci->initialize()) {
      c.host = xhci;
    } else {
      allocator_.deallocate(memory);  // initialize() already released its own memory
    }
  }
  return true;
}

void UsbBus::shutdown() {
  for (uint32_t i = 0; i < controller_count_; ++i) {
    if (!controllers_[i].host) continue;
    controllers_[i].host->shutdown();
    allocator_.deallocate(controllers_[i].host);
    controllers_[i].host = nullptr;
  }
  controller_count_ = 0;
}

uint32_t UsbBus::device_count() const {
  uint32_t count = 0;
  UsbDeviceDescriptor descriptor;
  for (uint32_t c = 0; c < controller_count_; ++c)
    for (uint32_t p = 0; p < port_count(c); ++p)
      if (device_descriptor(c, p, &descriptor)) ++count;
  return count;
}

UsbControllerKind UsbBus::controller_kind(uint32_t controller) const {
  return controller < controller_count_ ? controllers_[controller].kind : UsbControllerKind::Unknown;
}

uint32_t UsbBus::port_count(uint32_t controller) const {
  if (controller >= controller_count_ || !controllers_[controller].host) return 0;
  return controllers_[controller].host->port_count();
}

bool UsbBus::port_info(uint32_t controller, uint32_t port, UsbPortInfo* out) const {
  if (controller >= controller_count_ || !controllers_[controller].host) return false;
  return controllers_[controller].host->port_info(port, out);
}

bool UsbBus::device_descriptor(uint32_t controller, uint32_t port, UsbDeviceDescriptor* out) const {
  if (controller >= controller_count_ || !controllers_[controller].host) return false;
  return controllers_[controller].host->device_descriptor(port, out);
}

}  // namespace hardware
