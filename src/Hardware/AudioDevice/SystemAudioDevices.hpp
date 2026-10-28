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
#include "Hardware/AudioDevice/IAudioDevice.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Bus/Usb/UsbPortWatcher.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

// Finds every audio output the machine has and brings them up: Intel High
// Definition Audio controllers (any vendor), Intel AC'97 and virtio-sound, each
// through a native driver. A machine with none gets a SoftwareAudioDevice that
// discards what it is given, so there is always something to play to.
// A change to the set of audio devices (see SystemAudioDevices::poll).
struct AudioEvent {
  enum class Kind : uint8_t { Added, Removed };
  Kind kind = Kind::Added;
  uint32_t id = 0;                         // the device's id, which stays the same for as long as it is there
  bool hardware = true;                    // false for the software fallback
  SharedPointer<IAudioDevice> device;      // a removed device is shut down and present() is false; it lives while anyone holds this
};

class SystemAudioDevices {
 public:
  SystemAudioDevices(IPciBus& pci, const IClock& clock, allocators::IAllocator& allocator, efi::SystemTable* st, IUsbBus* usb = nullptr)
      : pci_(pci), usb_(usb), clock_(clock), allocator_(allocator), st_(st), devices_(allocator), pending_(allocator), watcher_(usb) {}

  void initialize();
  // Stops every device and frees them.
  void shutdown();

  // The devices there are now. at() hands out a plain pointer, which is only good while the device stays plugged in: a program that keeps
  // one while hot-plug is running holds share() instead (and checks present()).
  uint32_t count() const;
  IAudioDevice* at(uint32_t index) const;
  uint32_t id(uint32_t index) const;
  SharedPointer<IAudioDevice> share(uint32_t index) const;
  // Every device that can send its output elsewhere chooses again (IAudioDevice::retarget): a graphics card's HDMI / DisplayPort audio follows the displays that come and go. The number
  // of devices that did.
  uint32_t retarget();
  // True for a device that is real hardware, false for the software fallback.
  bool is_hardware(uint32_t index) const;

  // Hot-plug: USB audio devices plugged in or pulled out since start-up. One change per call (false when there is none); call it every few
  // hundred milliseconds from one thread. A new device is already initialized and in the list (at the end); a removed one has been taken out
  // of the list and shut down, and is freed once everyone has let go of the event's pointer. When the last device that plays sound goes, the
  // software fallback is brought up (and reported as added), so there is always something to play to.
  bool poll(AudioEvent* event);

 private:
  struct Entry {
    uint32_t id = 0;
    bool hardware = true;
    SharedPointer<IAudioDevice> device;
  };

  void adopt(SharedPointer<IAudioDevice> device, bool hardware, bool report);
  bool has_output() const;       // (locked by the caller)
  void add_fallback(bool report);
  void sweep_removed();

  IPciBus& pci_;
  IUsbBus* usb_;
  const IClock& clock_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  mutable kernel::Mutex mutex_;   // the list of devices
  List<Entry> devices_;
  List<AudioEvent> pending_;      // changes found but not yet handed out
  uint32_t next_id_ = 1;
  UsbPortWatcher watcher_;        // the USB ports as last looked at
};

}  // namespace hardware
