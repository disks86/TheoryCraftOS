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

#include "hardware/SystemInput.hpp"
#include <new>
#include "hardware/Bus/Usb/Hid/UsbHidInterface.hpp"
#include "hardware/Bus/Virtio/VirtioPciDevice.hpp"
#include "hardware/Keyboard/Usb/UsbHidKeyboard.hpp"
#include "hardware/Keyboard/Virtio/VirtioKeyboard.hpp"
#include "hardware/Mouse/Usb/UsbHidMouse.hpp"
#include "hardware/Mouse/Virtio/VirtioMouse.hpp"

namespace hardware {

template <class T, class... Args>
T* SystemInput::make(Args&&... args) {
  UniquePointer<T> device = UniquePointer<T>::make(allocator_, static_cast<Args&&>(args)...);
  if (!device) return nullptr;
  T* raw = device.get();
  if (!owned_.push_back(UniquePointer<IHardware>(util::move(device)))) return nullptr;  // freed on the way out
  return raw;
}

// Stops and destroys the device most recently made by make().
void SystemInput::release_last() {
  owned_[owned_.count() - 1]->shutdown();
  owned_.remove_at(owned_.count() - 1);
}

void SystemInput::find_ps2() {
  if (!i8042_.initialize()) return;
  if (ps2_keyboard_.initialize()) keyboards_.add(&ps2_keyboard_);
  if (ps2_mouse_.initialize()) mice_.add(&ps2_mouse_);
}

void SystemInput::find_usb() {
  for (uint32_t c = 0; c < usb_.controller_count(); ++c) {
    IUsbHostController* host = usb_.host_controller(c);
    if (!host) continue;
    for (uint32_t port = 0; port < host->port_count(); ++port) {
      uint8_t config[256];
      uint16_t length = 0;
      if (!host->configuration_descriptor(port, config, sizeof(config), &length)) continue;

      UsbHidInterface interfaces[4];
      uint32_t count = find_hid_interfaces(config, length, interfaces, 4);
      if (count > 4) count = 4;
      for (uint32_t i = 0; i < count; ++i) {
        const UsbHidInterface& iface = interfaces[i];
        if (iface.is_boot_keyboard()) {
          if (auto* k = make<UsbHidKeyboard>(*host, port, iface)) {
            if (k->initialize()) keyboards_.add(k);
            else release_last();
          }
        } else if (auto* m = make<UsbHidMouse>(*host, port, iface)) {
          // Anything else HID: it is used if its report descriptor describes a pointer.
          if (m->initialize()) mice_.add(m);
          else release_last();
        }
      }
    }
  }
}

void SystemInput::find_virtio() {
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.vendor_id != kVirtioPciVendor || d.device_id != kVirtioInputDevice) continue;
    // The device announces what it is; try each class until one accepts it.
    if (auto* k = make<VirtioKeyboard>(pci_, d, allocator_, st_)) {
      if (k->initialize()) { keyboards_.add(k); continue; }
      release_last();
    }
    if (auto* m = make<VirtioMouse>(pci_, d, allocator_, st_)) {
      if (m->initialize()) mice_.add(m);
      else release_last();
    }
  }
}

void SystemInput::initialize() {
  find_ps2();
  find_usb();
  find_virtio();
}

void SystemInput::shutdown() {
  for (uint64_t i = owned_.count(); i > 0; --i) owned_[i - 1]->shutdown();  // newest first
  owned_.clear();  // frees the devices
  ps2_keyboard_.shutdown();
  ps2_mouse_.shutdown();
}

}  // namespace hardware
