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

#include "Hardware/HyperV/SystemHyperV.hpp"
#include "Kernel/Thread.hpp"

namespace hardware {

bool SystemHyperV::initialize() {
  if (!HyperV::detected()) return false;
  UniquePointer<HyperV> hv = UniquePointer<HyperV>::make(allocator_);
  if (!hv) return false;
  if (!hv->initialize(allocator_)) {
    hv->shutdown();
    return false;
  }
  hv_ = util::move(hv);

  // VMBus is optional: a host may run the guest without it. A silent host costs a few seconds here, once.
  UniquePointer<VmBus> bus = UniquePointer<VmBus>::make(allocator_, *hv_, allocator_);
  if (bus && bus->connect()) {
    bus_ = util::move(bus);
    UniquePointer<HyperVIntegration> integration = UniquePointer<HyperVIntegration>::make(allocator_, *bus_);
    if (integration) {
      integration->start();
      integration_ = util::move(integration);
    }
  }
  return true;
}

bool SystemHyperV::start_service(int32_t core) {
  if (service_) return true;
  if (!integration_) return false;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "hyperv", core);
  return service_ != nullptr;
}

void SystemHyperV::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void SystemHyperV::service_main(void* self) {
  SystemHyperV* hyperv = static_cast<SystemHyperV*>(self);
  while (!__atomic_load_n(&hyperv->stop_, __ATOMIC_ACQUIRE)) {
    hyperv->bus_->pump();
    hyperv->integration_->service();
    kernel::sleep_us(20000);
  }
}

void SystemHyperV::shutdown() {
  stop_service();
  integration_ = UniquePointer<HyperVIntegration>();   // closes its channels
  bus_ = UniquePointer<VmBus>();                       // unloads the connection
  if (hv_) hv_->shutdown();
  hv_ = UniquePointer<HyperV>();
}

}  // namespace hardware
