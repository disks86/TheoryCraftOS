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
#include "Utility/UniquePointer.hpp"

namespace kernel {
struct Thread;
}

namespace hardware {

// What QEMU gives a guest besides disks, network and screen: the host's entropy pool (virtio-rng), the memory
// balloon (virtio-balloon) and the crash notification (pvpanic). Whichever of them the machine has is brought
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

  // Follows the host's balloon target in a background thread (only with a balloon device).
  bool start_service();
  void stop_service();

 private:
  static void service_main(void* self);

  IPciBus& pci_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  UniquePointer<VirtioRng> rng_;
  UniquePointer<VirtioBalloon> balloon_;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace hardware
