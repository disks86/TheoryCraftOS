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
#include "Hardware/Keyboard/CompositeKeyboard.hpp"
#include "Hardware/Keyboard/Ps2/Ps2Keyboard.hpp"
#include "Hardware/Mouse/CompositeMouse.hpp"
#include "Hardware/Mouse/Ps2/Ps2Mouse.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"
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
  SystemInput(IPciBus& pci, IUsbBus& usb, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), usb_(usb), allocator_(allocator), st_(st), owned_(allocator) {}

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
  void release_last();

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
  List<UniquePointer<IHardware>> owned_;  // the allocator-backed devices, freed with the list
};

}  // namespace hardware
