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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

// USB host controllers discovered on a PCI bus. every controller type gets a
// driver (XhciController, EhciController, UhciController, OhciController) that
// enumerates the devices on its root ports. Companion UHCI/OHCI controllers are
// started after the EHCI controller whose ports they share.
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
  bool poll_hotplug(UsbHotplugEvent* event) override;
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
  uint32_t next_poll_ = 0;
};

}  // namespace hardware
