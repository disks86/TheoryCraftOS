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
#include "Utility/FixedString.hpp"

namespace hardware {

void SystemCameras::adopt(SharedPointer<ICamera> camera, bool report) {
  if (!camera) return;
  Entry entry;
  entry.id = next_id_++;
  entry.camera = camera;
  CameraEvent event;
  event.kind = CameraEvent::Kind::Added;
  event.id = entry.id;
  event.camera = util::move(camera);
  kernel::ScopedMutex guard(mutex_);
  cameras_.push_back(util::move(entry));
  if (report) pending_.push_back(util::move(event));
}

void SystemCameras::initialize() {
  List<UniquePointer<UvcCamera>> found(allocator_);
  UvcCamera::discover(usb_, allocator_, found);
  for (uint64_t i = 0; i < found.count(); ++i) adopt(SharedPointer<ICamera>::from(util::move(found[i])), false);
  watcher_.snapshot();   // what was plugged in at start-up has been taken over; later ports are new
}

void SystemCameras::shutdown() {
  stop_service();
  kernel::ScopedMutex guard(mutex_);
  pending_.clear();
  for (uint64_t i = cameras_.count(); i > 0; --i) cameras_[i - 1].camera->shutdown();   // newest first
  cameras_.clear();
}

uint32_t SystemCameras::count() const {
  kernel::ScopedMutex guard(mutex_);
  return static_cast<uint32_t>(cameras_.count());
}

ICamera* SystemCameras::at(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < cameras_.count() ? cameras_[index].camera.get() : nullptr;
}

uint32_t SystemCameras::id(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < cameras_.count() ? cameras_[index].id : 0;
}

SharedPointer<ICamera> SystemCameras::share(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < cameras_.count() ? cameras_[index].camera : SharedPointer<ICamera>();
}

void SystemCameras::pump() {
  List<SharedPointer<ICamera>> snapshot(allocator_);   // (outside the lock: a pump decodes nothing but copies a few packets)
  {
    kernel::ScopedMutex guard(mutex_);
    for (const Entry& e : cameras_.view()) snapshot.push_back(e.camera);
  }
  for (SharedPointer<ICamera>& c : snapshot.view()) c->pump();
}

// Takes every camera that is no longer present out of the list: shut down first (which waits for a read_frame() running on another thread), then reported.
void SystemCameras::sweep_removed() {
  for (uint64_t i = 0;;) {
    SharedPointer<ICamera> gone;
    uint32_t id = 0;
    {
      kernel::ScopedMutex guard(mutex_);
      while (i < cameras_.count() && cameras_[i].camera->present()) ++i;
      if (i >= cameras_.count()) return;
      gone = cameras_[i].camera;
      id = cameras_[i].id;
      cameras_.remove_at(i);
    }
    CameraEvent event;
    event.kind = CameraEvent::Kind::Removed;
    event.id = id;
    event.was_streaming = gone->streaming();
    gone->shutdown();
    event.camera = util::move(gone);
    kernel::ScopedMutex guard(mutex_);
    pending_.push_back(util::move(event));
  }
}

bool SystemCameras::poll(CameraEvent* event) {
  for (int rounds = 0; rounds < 64; ++rounds) {
    {
      kernel::ScopedMutex guard(mutex_);
      if (pending_.count() > 0) {
        *event = util::move(pending_[0]);
        pending_.remove_at(0);
        return true;
      }
    }
    watcher_.scan([&](uint32_t, IUsbHostController& host, uint32_t port) {
      sweep_removed();
      List<UniquePointer<UvcCamera>> found(allocator_);
      UvcCamera::discover_port(host, port, allocator_, found);
      for (uint64_t i = 0; i < found.count(); ++i) adopt(SharedPointer<ICamera>::from(util::move(found[i])), true);
    });
    sweep_removed();   // (a camera that was pulled out and nothing came: no port reports it, but the camera knows)
    kernel::ScopedMutex guard(mutex_);
    if (pending_.count() == 0) return false;
  }
  return false;
}

void SystemCameras::handle_changes() {
  CameraEvent event;
  while (poll(&event)) {
    if (!notifier_) continue;
    FixedString<192> text;
    text.assign(event.camera->name());
    if (event.kind == CameraEvent::Kind::Added) {
      text.append(": added, ");
      text.append_uint(event.camera->mode_count());
      text.append(" modes");
    } else {
      text.append(": removed");
      if (event.was_streaming) text.append(" while streaming");
    }
    notifier_(notifier_context_, text.view());
  }
}

bool SystemCameras::start_service(Notifier notifier, void* context) {
  if (service_) return true;
  notifier_ = notifier;
  notifier_context_ = context;
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
  constexpr uint32_t kTicksPerHotplug = kHotplugIntervalUs / kPumpIntervalUs;
  for (uint32_t tick = 0; !__atomic_load_n(&cameras->stop_, __ATOMIC_ACQUIRE); ++tick) {
    if (tick % kTicksPerHotplug == 0) cameras->handle_changes();
    cameras->pump();
    kernel::sleep_us(kPumpIntervalUs);
  }
}

}  // namespace hardware
