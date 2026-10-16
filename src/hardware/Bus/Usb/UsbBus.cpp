#include "hardware/Bus/Usb/UsbBus.hpp"
#include "hardware/Bus/Usb/Ehci/EhciController.hpp"
#include "hardware/Bus/Usb/Ohci/OhciController.hpp"
#include "hardware/Bus/Usb/Uhci/UhciController.hpp"
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
  controllers_.clear();
  // Two passes: the USB 2.0 and 3.x controllers first. An EHCI controller claims
  // every port of the chip (routing slower devices to its companion UHCI/OHCI
  // controllers), so the companions must look at their ports only afterwards.
  for (int pass = 0; pass < 2; ++pass) {
    PciDeviceInfo d;
    for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
      if (d.class_code != kPciClassSerialBus || d.subclass != kPciSubclassUsb) continue;
      const UsbControllerKind kind = kind_from_prog_if(d.prog_if);
      if (kind == UsbControllerKind::Unknown) continue;
      const bool companion = kind == UsbControllerKind::Uhci || kind == UsbControllerKind::Ohci;
      if (companion != (pass == 1)) continue;

      Controller controller{kind, UniquePointer<IUsbHostController>()};
      // A controller that fails to initialize has already released its own
      // memory; the pointer frees the object itself.
      if (kind == UsbControllerKind::Xhci) {
        auto host = UniquePointer<XhciController>::make(allocator_, pci_, d, allocator_, st_);
        if (host && host->initialize()) controller.host = util::move(host);
      } else if (kind == UsbControllerKind::Ehci) {
        auto host = UniquePointer<EhciController>::make(allocator_, pci_, d, allocator_, st_);
        if (host && host->initialize()) controller.host = util::move(host);
      } else if (kind == UsbControllerKind::Uhci) {
        auto host = UniquePointer<UhciController>::make(allocator_, pci_, d, allocator_, st_);
        if (host && host->initialize()) controller.host = util::move(host);
      } else if (kind == UsbControllerKind::Ohci) {
        auto host = UniquePointer<OhciController>::make(allocator_, pci_, d, allocator_, st_);
        if (host && host->initialize()) controller.host = util::move(host);
      }
      controllers_.push_back(util::move(controller));
    }
  }
  return true;
}

void UsbBus::shutdown() {
  for (Controller& c : controllers_)
    if (c.host) c.host->shutdown();
  controllers_.clear();  // frees the drivers
}

uint32_t UsbBus::device_count() const {
  uint32_t count = 0;
  UsbDeviceDescriptor descriptor;
  for (uint32_t c = 0; c < controller_count(); ++c)
    for (uint32_t p = 0; p < port_count(c); ++p)
      if (device_descriptor(c, p, &descriptor)) ++count;
  return count;
}

UsbControllerKind UsbBus::controller_kind(uint32_t controller) const {
  return controller < controllers_.count() ? controllers_[controller].kind : UsbControllerKind::Unknown;
}

uint32_t UsbBus::port_count(uint32_t controller) const {
  if (controller >= controllers_.count() || !controllers_[controller].host) return 0;
  return controllers_[controller].host->port_count();
}

bool UsbBus::port_info(uint32_t controller, uint32_t port, UsbPortInfo* out) const {
  if (controller >= controllers_.count() || !controllers_[controller].host) return false;
  return controllers_[controller].host->port_info(port, out);
}

bool UsbBus::device_descriptor(uint32_t controller, uint32_t port, UsbDeviceDescriptor* out) const {
  if (controller >= controllers_.count() || !controllers_[controller].host) return false;
  return controllers_[controller].host->device_descriptor(port, out);
}

}  // namespace hardware
