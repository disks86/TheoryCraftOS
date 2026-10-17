/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "Hardware/Bus/Usb/UsbBus.hpp"
#include "Hardware/Bus/Usb/Ehci/EhciController.hpp"
#include "Hardware/Bus/Usb/Ohci/OhciController.hpp"
#include "Hardware/Bus/Usb/Uhci/UhciController.hpp"
#include "Hardware/Bus/Usb/Xhci/XhciController.hpp"

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

bool UsbBus::poll_hotplug(UsbHotplugEvent* event) {
  for (uint32_t i = 0; i < controllers_.count(); ++i) {
    // Start after the controller that reported last, so a busy one cannot starve the others.
    const uint32_t c = (next_poll_ + i) % static_cast<uint32_t>(controllers_.count());
    IUsbHostController* host = controllers_[c].host.get();
    if (!host || !host->supports_hotplug() || !host->poll_hotplug(event)) continue;
    event->controller = c;
    next_poll_ = c;
    return true;
  }
  return false;
}

}  // namespace hardware
