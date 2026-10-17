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
#include "Hardware/Bus/Ps2/I8042Controller.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Bus/Usb/UsbPortWatcher.hpp"
#include "Hardware/Keyboard/CompositeKeyboard.hpp"
#include "Hardware/Keyboard/Ps2/Ps2Keyboard.hpp"
#include "Hardware/Keyboard/Usb/UsbHidKeyboard.hpp"
#include "Hardware/Mouse/CompositeMouse.hpp"
#include "Hardware/Mouse/Ps2/Ps2Mouse.hpp"
#include "Hardware/Mouse/Usb/UsbHidMouse.hpp"
#include "Kernel/Thread.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

// Finds every keyboard and mouse the machine has - PS/2, USB HID (behind xHCI
// root ports) and virtio-input - brings them up and merges them into one
// keyboard and one mouse. Devices that need memory get it from the allocator.
//
// USB keyboards and mice come and go while the machine runs: poll() (or the
// service thread, start_service()) takes up those plugged in after start-up and
// lets go of those pulled out - their keys and buttons are released first - on
// controllers that report hot-plug (xHCI). The merged keyboard() and mouse()
// stay valid throughout and may be polled from other threads meanwhile.
//
// The firmware also polls PS/2 devices from its timer interrupt; once
// initialize() has run, call ICentralProcessingUnit::disable_interrupts() so the
// firmware does not steal bytes from the PS/2 controller.
class SystemInput {
 public:
  SystemInput(IPciBus& pci, IUsbBus& usb, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), usb_(usb), allocator_(allocator), st_(st), owned_(allocator), watcher_(&usb) {}
  ~SystemInput() { stop_service(); }
  SystemInput(const SystemInput&) = delete;
  SystemInput& operator=(const SystemInput&) = delete;

  // Told when a USB keyboard or mouse was added or removed ("USB HID keyboard: added"), from the thread that polls. May be null.
  using Notifier = void (*)(void* context, StringView text);
  void set_notifier(Notifier notifier, void* context) { notifier_ = notifier; notifier_context_ = context; }

  void initialize();
  // Stops every device and frees what initialize() allocated.
  void shutdown();

  IKeyboard& keyboard() { return keyboards_; }
  IMouse& mouse() { return mice_; }

  // Hot-plug: takes up the USB keyboards and mice plugged in since the last call and drops those that were pulled out; returns how many
  // came or went. Call from one thread.
  uint32_t poll();
  // Runs poll() every 250 ms on a thread of its own ("input-hotplug").
  bool start_service();
  void stop_service();
  CompositeKeyboard& keyboards() { return keyboards_; }  // to list the individual devices
  CompositeMouse& mice() { return mice_; }

 private:
  template <class T, class... Args>
  T* make(Args&&... args);  // allocates and constructs a device; null if out of memory/slots
  void release_last();

  void find_ps2();
  void find_usb();
  uint32_t take_up(IUsbHostController& host, uint32_t port);   // the HID interfaces of the device on a port
  uint32_t drop_removed();                                      // devices whose port moved on
  void notify(const char* name, const char* what);
  static void service_main(void* self);
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
  List<UniquePointer<IHardware>> owned_;  // the allocator-backed devices, freed with the list
  UsbPortWatcher watcher_;
  UsbHidKeyboard* usb_keyboards_[CompositeKeyboard::kMaxKeyboards] = {};   // the USB devices among them, to find those that left
  UsbHidMouse* usb_mice_[CompositeMouse::kMaxMice] = {};
  Notifier notifier_ = nullptr;
  void* notifier_context_ = nullptr;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace hardware
