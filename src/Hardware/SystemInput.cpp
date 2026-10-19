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

#include "Hardware/SystemInput.hpp"
#include <new>
#include "Utility/String.hpp"
#include "Hardware/Bus/Usb/Hid/UsbHidInterface.hpp"
#include "Hardware/Bus/Virtio/VirtioPciDevice.hpp"
#include "Hardware/Keyboard/Usb/UsbHidKeyboard.hpp"
#include "Hardware/Keyboard/Virtio/VirtioKeyboard.hpp"
#include "Hardware/Mouse/Usb/UsbHidMouse.hpp"
#include "Hardware/Mouse/VirtualBox/VBoxMouse.hpp"
#include "Hardware/Mouse/VMware/VmwareMouse.hpp"
#include "Hardware/VMware/Backdoor.hpp"
#include "Hardware/Mouse/Virtio/VirtioMouse.hpp"

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

// Brings up the keyboards and mice among the HID interfaces of the device on a root port. Returns how many were taken up.
uint32_t SystemInput::take_up(IUsbHostController& host_controller, uint32_t port) {
  IUsbHostController* host = &host_controller;
  uint8_t config[256];
  uint16_t length = 0;
  if (!host->configuration_descriptor(port, ByteView(config), &length)) return 0;  // length is the full size

  uint32_t taken = 0;
  UsbHidInterface interfaces[4];
  uint32_t count = find_hid_interfaces(ConstByteView(config).at_most(length), ArrayView<UsbHidInterface>(interfaces));
  if (count > 4) count = 4;
  for (uint32_t i = 0; i < count; ++i) {
    const UsbHidInterface& iface = interfaces[i];
    if (iface.is_boot_keyboard()) {
      if (auto* k = make<UsbHidKeyboard>(*host, port, iface)) {
        if (k->initialize() && keyboards_.add(k)) {
          for (UsbHidKeyboard*& slot : usb_keyboards_)
            if (!slot) { slot = k; break; }
          ++taken;
          notify(k->name(), "added");
        } else {
          release_last();
        }
      }
    } else if (auto* m = make<UsbHidMouse>(*host, port, iface)) {
      // Anything else HID: it is used if its report descriptor describes a pointer.
      if (m->initialize() && mice_.add(m)) {
        for (UsbHidMouse*& slot : usb_mice_)
          if (!slot) { slot = m; break; }
        ++taken;
        notify(m->name(), "added");
      } else {
        release_last();
      }
    }
  }
  return taken;
}

void SystemInput::find_usb() {
  for (uint32_t c = 0; c < usb_.controller_count(); ++c) {
    IUsbHostController* host = usb_.host_controller(c);
    if (!host) continue;
    for (uint32_t port = 0; port < host->port_count(); ++port) take_up(*host, port);
  }
}

void SystemInput::notify(const char* name, const char* what) {
  if (!notifier_) return;
  String text(allocator_);
  text.assign(name);
  text.append(": ");
  text.append(what);
  notifier_(notifier_context_, text.view());
}

// Lets go of the keyboards and mice whose device is no longer there: their held keys and buttons are released, they leave the merged
// devices (which hand over what they still had), and they are shut down and freed.
uint32_t SystemInput::drop_removed() {
  uint32_t dropped = 0;
  auto destroy = [&](IHardware* device) {
    for (uint64_t i = 0; i < owned_.count(); ++i) {
      if (owned_[i].get() != device) continue;
      owned_[i]->shutdown();
      owned_.remove_at(i);
      return;
    }
  };
  for (UsbHidKeyboard*& k : usb_keyboards_) {
    if (!k || k->present()) continue;
    UsbHidKeyboard* gone = k;
    k = nullptr;
    gone->release_all();
    keyboards_.remove(gone);
    notify(gone->name(), "removed");
    destroy(gone);
    ++dropped;
  }
  for (UsbHidMouse*& m : usb_mice_) {
    if (!m || m->present()) continue;
    UsbHidMouse* gone = m;
    m = nullptr;
    gone->release_all();
    mice_.remove(gone);
    notify(gone->name(), "removed");
    destroy(gone);
    ++dropped;
  }
  return dropped;
}

uint32_t SystemInput::poll() {
  uint32_t changes = drop_removed();
  watcher_.scan([&](uint32_t, IUsbHostController& host, uint32_t port) {
    changes += drop_removed();   // (before the replacement on the same port is taken up)
    changes += take_up(host, port);
  });
  changes += drop_removed();
  return changes;
}

bool SystemInput::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "input-hotplug");
  return service_ != nullptr;
}

void SystemInput::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void SystemInput::service_main(void* self) {
  SystemInput* input = static_cast<SystemInput*>(self);
  while (!__atomic_load_n(&input->stop_, __ATOMIC_ACQUIRE)) {
    input->poll();
    kernel::sleep_us(250000);
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

// With a VirtualBox guest device the PS/2 mouse is wrapped: positions come from the host, buttons from the PS/2 reports.
void SystemInput::find_virtualbox() {
  if (!vmm_ || !mice_.at(0)) return;
  if (mice_.at(0) != &ps2_mouse_) return;
  if (auto* m = make<VBoxMouse>(*vmm_, ps2_mouse_)) {
    mice_.remove(&ps2_mouse_);
    mice_.add(m);
  }
}

// On VMware (or QEMU's vmport) the host hands out the pointer through the backdoor; the PS/2 mouse is then only drained.
void SystemInput::find_vmware() {
  if (vmm_ || !Backdoor::probe() || mice_.at(0) != &ps2_mouse_) return;
  if (auto* m = make<VmwareMouse>(ps2_mouse_)) {
    if (m->initialize()) {
      mice_.remove(&ps2_mouse_);
      mice_.add(m);
    } else {
      release_last();
    }
  }
}

void SystemInput::initialize() {
  find_ps2();
  find_virtualbox();
  find_vmware();
  find_usb();
  find_virtio();
  watcher_.snapshot();   // what was plugged in at start-up has been taken over; later ports are new
}

void SystemInput::shutdown() {
  stop_service();
  for (uint64_t i = owned_.count(); i > 0; --i) owned_[i - 1]->shutdown();  // newest first
  owned_.clear();  // frees the devices
  ps2_keyboard_.shutdown();
  ps2_mouse_.shutdown();
}

}  // namespace hardware
