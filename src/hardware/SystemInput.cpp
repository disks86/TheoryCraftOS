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
  if (owned_count_ == kMaxDevices) return nullptr;
  void* memory = allocator_.allocate(sizeof(T));
  if (!memory) return nullptr;
  T* device = new (memory) T(static_cast<Args&&>(args)...);
  owned_[owned_count_++] = device;
  return device;
}

// Destroys a device made by make(); only the most recent one may be released.
void SystemInput::release(IHardware* device) {
  device->shutdown();
  allocator_.deallocate(device);
  --owned_count_;
  owned_[owned_count_] = nullptr;
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
            else release(k);
          }
        } else if (auto* m = make<UsbHidMouse>(*host, port, iface)) {
          // Anything else HID: it is used if its report descriptor describes a pointer.
          if (m->initialize()) mice_.add(m);
          else release(m);
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
      release(k);
    }
    if (auto* m = make<VirtioMouse>(pci_, d, allocator_, st_)) {
      if (m->initialize()) mice_.add(m);
      else release(m);
    }
  }
}

void SystemInput::initialize() {
  find_ps2();
  find_usb();
  find_virtio();
}

void SystemInput::shutdown() {
  while (owned_count_) release(owned_[owned_count_ - 1]);
  ps2_keyboard_.shutdown();
  ps2_mouse_.shutdown();
}

}  // namespace hardware
