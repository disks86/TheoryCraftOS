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
#include "Hardware/Bus/Usb/UsbPortWatcher.hpp"
#include "Hardware/Camera/ICamera.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A change to the set of cameras (see SystemCameras::poll).
struct CameraEvent {
  enum class Kind : uint8_t { Added, Removed };
  Kind kind = Kind::Added;
  uint32_t id = 0;                      // the camera's id, which stays the same for as long as it is there
  SharedPointer<ICamera> camera;        // a removed camera is shut down and present() is false; it lives while anyone holds this
  bool was_streaming = false;           // Removed: it was streaming when it left
};

// Finds every camera the machine has: USB video class devices on host controllers that do isochronous transfers (xHCI), at start-up and as
// they are plugged in and pulled out later. Cameras are only found, not started: a program picks one, calls start() on it and reads pictures.
// The cameras have to be serviced while they stream (ICamera::pump), which start_service() does from a thread of its own, together with the
// check for cameras that came or went.
//
// The list changes while the system runs, so a program that uses a camera holds share() (not the pointer at() hands out, which is only good
// while the camera stays) and checks present() - a camera that is pulled out stops streaming and read_frame() returns false.
class SystemCameras {
 public:
  static constexpr uint64_t kPumpIntervalUs = 4000;
  static constexpr uint64_t kHotplugIntervalUs = 250000;
  using Notifier = void (*)(void* context, StringView text);

  SystemCameras(IUsbBus& usb, allocators::IAllocator& allocator) : usb_(usb), allocator_(allocator), cameras_(allocator), pending_(allocator), watcher_(&usb) {}
  ~SystemCameras() { shutdown(); }

  void initialize();
  // Stops the service and every camera and frees them.
  void shutdown();

  uint32_t count() const;
  ICamera* at(uint32_t index) const;
  uint32_t id(uint32_t index) const;
  SharedPointer<ICamera> share(uint32_t index) const;

  // Hot-plug: one change per call (false when there is none); call it every few hundred milliseconds from one thread. A new camera is already
  // initialized and in the list (at the end); a removed one has been taken out and shut down, and is freed once everyone has let go of the
  // event's pointer. Only controllers that report hot-plug (xHCI) are watched.
  bool poll(CameraEvent* event);

  // Runs pump() on every camera every few milliseconds, and poll() every 250 ms, from a thread ("camera"). Each change is passed to `notifier`
  // ("USB camera: Foo: added, 12 modes", "...: removed"). The calls are safe to repeat.
  bool start_service(Notifier notifier = nullptr, void* context = nullptr);
  void stop_service();
  // One round of the pumping, for callers that service the cameras themselves.
  void pump();

 private:
  struct Entry {
    uint32_t id = 0;
    SharedPointer<ICamera> camera;
  };

  void adopt(SharedPointer<ICamera> camera, bool report);
  void sweep_removed();
  void handle_changes();
  static void service_main(void* self);

  IUsbBus& usb_;
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;   // the list
  List<Entry> cameras_;
  List<CameraEvent> pending_;
  uint32_t next_id_ = 1;
  UsbPortWatcher watcher_;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
  Notifier notifier_ = nullptr;
  void* notifier_context_ = nullptr;
};

}  // namespace hardware
