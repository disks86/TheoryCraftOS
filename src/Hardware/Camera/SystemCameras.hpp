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
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Camera/ICamera.hpp"
#include "Kernel/Thread.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Finds every camera the machine has: USB video class devices on host controllers that do isochronous transfers (xHCI). Cameras are only
// found, not started: a program picks one, calls start() on it and reads pictures. The cameras have to be serviced while they stream
// (ICamera::pump), which start_service() does from a thread of its own.
class SystemCameras {
 public:
  static constexpr uint64_t kPumpIntervalUs = 4000;

  SystemCameras(IUsbBus& usb, allocators::IAllocator& allocator) : usb_(usb), allocator_(allocator), cameras_(allocator) {}
  ~SystemCameras() { shutdown(); }

  void initialize();
  // Stops the service and every camera and frees them.
  void shutdown();

  uint32_t count() const { return static_cast<uint32_t>(cameras_.count()); }
  ICamera* at(uint32_t index) const { return index < cameras_.count() ? cameras_[index].get() : nullptr; }

  // Runs pump() on every camera every few milliseconds from a thread ("camera").
  bool start_service();
  void stop_service();
  // One round of the service, for callers that service the cameras themselves.
  void pump();

 private:
  static void service_main(void* self);

  IUsbBus& usb_;
  allocators::IAllocator& allocator_;
  List<UniquePointer<ICamera>> cameras_;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace hardware
