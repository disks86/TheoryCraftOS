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

#include "Hardware/Camera/SystemCameras.hpp"
#include "Hardware/Camera/Usb/UvcCamera.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

void SystemCameras::initialize() {
  List<UniquePointer<UvcCamera>> usb_cameras(allocator_);
  UvcCamera::discover(usb_, allocator_, usb_cameras);
  for (uint64_t i = 0; i < usb_cameras.count(); ++i) cameras_.push_back(UniquePointer<ICamera>(util::move(usb_cameras[i])));
}

void SystemCameras::shutdown() {
  stop_service();
  for (uint64_t i = cameras_.count(); i > 0; --i) cameras_[i - 1]->shutdown();   // newest first
  cameras_.clear();
}

void SystemCameras::pump() {
  for (uint64_t i = 0; i < cameras_.count(); ++i) cameras_[i]->pump();
}

bool SystemCameras::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "camera");
  return service_ != nullptr;
}

void SystemCameras::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void SystemCameras::service_main(void* self) {
  SystemCameras* cameras = static_cast<SystemCameras*>(self);
  while (!__atomic_load_n(&cameras->stop_, __ATOMIC_ACQUIRE)) {
    cameras->pump();
    kernel::sleep_us(kPumpIntervalUs);
  }
}

}  // namespace hardware
