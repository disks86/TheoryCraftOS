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

#include "Hardware/Guest/SystemGuestServices.hpp"
#include "Kernel/Thread.hpp"

namespace hardware {

void SystemGuestServices::initialize() {
  PvPanic::probe();
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.vendor_id != kVirtioPciVendor) continue;
    if (!rng_ && (d.device_id == kVirtioRngDevice || d.device_id == kVirtioRngTransitionalDevice)) {
      UniquePointer<VirtioRng> rng = UniquePointer<VirtioRng>::make(allocator_, pci_, d, allocator_, st_);
      if (rng && rng->initialize()) rng_ = util::move(rng);
      else if (rng) rng->shutdown();  // hands the device back to the firmware
    } else if (!balloon_ && (d.device_id == kVirtioBalloonDevice || d.device_id == kVirtioBalloonTransitionalDevice)) {
      UniquePointer<VirtioBalloon> balloon = UniquePointer<VirtioBalloon>::make(allocator_, pci_, d, allocator_, st_);
      if (balloon && balloon->initialize()) balloon_ = util::move(balloon);
      else if (balloon) balloon->shutdown();
    }
  }
}

bool SystemGuestServices::start_service() {
  if (service_) return true;
  if (!balloon_) return false;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "balloon");
  return service_ != nullptr;
}

void SystemGuestServices::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void SystemGuestServices::service_main(void* self) {
  SystemGuestServices* services = static_cast<SystemGuestServices*>(self);
  while (!__atomic_load_n(&services->stop_, __ATOMIC_ACQUIRE)) {
    services->balloon_->service();
    kernel::sleep_us(500000);
  }
}

void SystemGuestServices::shutdown() {
  stop_service();
  if (balloon_) balloon_->shutdown();
  balloon_ = UniquePointer<VirtioBalloon>();
  if (rng_) rng_->shutdown();
  rng_ = UniquePointer<VirtioRng>();
}

}  // namespace hardware
