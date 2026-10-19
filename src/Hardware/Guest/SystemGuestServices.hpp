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
#include "Hardware/Guest/PvPanic.hpp"
#include "Hardware/Guest/VirtioBalloon.hpp"
#include "Hardware/Guest/VirtioRng.hpp"
#include "Hardware/VirtualBox/VmmDev.hpp"
#include "Utility/UniquePointer.hpp"

namespace kernel {
struct Thread;
}

namespace hardware {

// What a hypervisor gives a guest besides disks, network and screen. QEMU: the host's entropy pool (virtio-rng), the memory
// balloon (virtio-balloon) and the crash notification (pvpanic). VirtualBox: its guest device (VmmDev: pointer integration, display
// size requests, balloon, heartbeat). Whichever of them the machine has is brought
// up by initialize(); the others are simply absent.
class SystemGuestServices {
 public:
  SystemGuestServices(IPciBus& pci, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), allocator_(allocator), st_(st) {}
  ~SystemGuestServices() { shutdown(); }
  SystemGuestServices(const SystemGuestServices&) = delete;
  SystemGuestServices& operator=(const SystemGuestServices&) = delete;

  void initialize();
  void shutdown();

  // The host's randomness, or null without a virtio-rng device.
  crypto::IEntropySource* entropy() { return rng_.get(); }
  VirtioBalloon* balloon() { return balloon_.get(); }
  bool pvpanic() const { return PvPanic::present(); }
  // The VirtualBox guest device, or null elsewhere.
  VmmDev* virtualbox() { return vmm_.get(); }

  // Told (from the service thread) when VirtualBox's window is resized and the host asks the guest to follow.
  using DisplayRequest = void (*)(void* context, uint32_t width, uint32_t height);
  void set_display_request_handler(DisplayRequest handler, void* context) { display_handler_ = handler; display_context_ = context; }

  // Follows the host in a background thread: the balloon target, and VirtualBox's pointer, display and heartbeat.
  bool start_service();
  void stop_service();

 private:
  static void service_main(void* self);

  IPciBus& pci_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  UniquePointer<VirtioRng> rng_;
  UniquePointer<VirtioBalloon> balloon_;
  UniquePointer<VmmDev> vmm_;
  DisplayRequest display_handler_ = nullptr;
  void* display_context_ = nullptr;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace hardware
