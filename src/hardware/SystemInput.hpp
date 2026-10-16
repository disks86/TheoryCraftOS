#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/Bus/Ps2/I8042Controller.hpp"
#include "hardware/Bus/Usb/IUsbBus.hpp"
#include "hardware/Keyboard/CompositeKeyboard.hpp"
#include "hardware/Keyboard/Ps2/Ps2Keyboard.hpp"
#include "hardware/Mouse/CompositeMouse.hpp"
#include "hardware/Mouse/Ps2/Ps2Mouse.hpp"
#include "uefi.hpp"

namespace hardware {

// Finds every keyboard and mouse the machine has - PS/2, USB HID (behind xHCI
// root ports) and virtio-input - brings them up and merges them into one
// keyboard and one mouse. Devices that need memory get it from the allocator.
//
// The firmware also polls PS/2 devices from its timer interrupt; once
// initialize() has run, call ICentralProcessingUnit::disable_interrupts() so the
// firmware does not steal bytes from the PS/2 controller.
class SystemInput {
 public:
  static constexpr uint32_t kMaxDevices = 16;

  SystemInput(IPciBus& pci, IUsbBus& usb, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), usb_(usb), allocator_(allocator), st_(st) {}

  void initialize();
  // Stops every device and frees what initialize() allocated.
  void shutdown();

  IKeyboard& keyboard() { return keyboards_; }
  IMouse& mouse() { return mice_; }
  CompositeKeyboard& keyboards() { return keyboards_; }  // to list the individual devices
  CompositeMouse& mice() { return mice_; }

 private:
  template <class T, class... Args>
  T* make(Args&&... args);  // allocates and constructs a device; null if out of memory/slots
  void release(IHardware* device);

  void find_ps2();
  void find_usb();
  void find_virtio();

  IPciBus& pci_;
  IUsbBus& usb_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  I8042Controller i8042_;
  Ps2Keyboard ps2_keyboard_{i8042_};
  Ps2Mouse ps2_mouse_{i8042_};

  CompositeKeyboard keyboards_;
  CompositeMouse mice_;
  IHardware* owned_[kMaxDevices] = {};  // allocator-backed devices
  uint32_t owned_count_ = 0;
};

}  // namespace hardware
