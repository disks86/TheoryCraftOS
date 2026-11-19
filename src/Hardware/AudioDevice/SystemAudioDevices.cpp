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

#include "Hardware/AudioDevice/SystemAudioDevices.hpp"
#include "Hardware/AudioDevice/Intel/Ac97AudioDevice.hpp"
#include "Hardware/AudioDevice/Intel/IntelHdaAudioDevice.hpp"
#include "Hardware/AudioDevice/Software/SoftwareAudioDevice.hpp"
#include "Hardware/AudioDevice/Usb/UsbAudioDevice.hpp"
#include "Hardware/AudioDevice/Virtio/VirtioSoundDevice.hpp"

namespace hardware {

namespace {
constexpr uint8_t kPciClassMultimedia = 0x04;
constexpr uint8_t kPciSubclassAudio = 0x01, kPciSubclassHda = 0x03;

// Builds a T, initializes it, and keeps it in `list` only if that all works.
// Returns the device (still owned by the list) or null.
template <class T, class... Args>
T* try_add(List<UniquePointer<IAudioDevice>>& list, allocators::IAllocator& allocator, Args&&... args) {
  UniquePointer<T> device = UniquePointer<T>::make(allocator, static_cast<Args&&>(args)...);
  if (!device) return nullptr;
  if (!device->initialize()) {
    device->shutdown();  // hands the device back to the firmware
    return nullptr;
  }
  T* raw = device.get();
  list.push_back(UniquePointer<IAudioDevice>(util::move(device)));
  return raw;
}
}  // namespace

void SystemAudioDevices::adopt(SharedPointer<IAudioDevice> device, bool hardware, bool report, IntelHdaAudioDevice* hda) {
  if (!device) return;
  Entry entry;
  entry.id = next_id_++;
  entry.hardware = hardware;
  entry.hda = hda;
  entry.device = device;
  AudioEvent event;
  event.kind = AudioEvent::Kind::Added;
  event.id = entry.id;
  event.hardware = hardware;
  event.device = util::move(device);
  kernel::ScopedMutex guard(mutex_);
  devices_.push_back(util::move(entry));
  if (report) pending_.push_back(util::move(event));
}

uint32_t SystemAudioDevices::retarget() {
  kernel::ScopedMutex guard(mutex_);
  uint32_t moved = 0;
  for (const Entry& e : devices_.view())
    if (e.device->present() && e.device->retarget()) ++moved;
  return moved;
}

bool SystemAudioDevices::has_output() const {
  for (const Entry& e : devices_.view())
    if (e.device->has_output() && e.device->present()) return true;
  return false;
}

void SystemAudioDevices::add_fallback(bool report) {
  List<UniquePointer<IAudioDevice>> one(allocator_);
  try_add<SoftwareAudioDevice>(one, allocator_, clock_, allocator_);
  if (one.count() > 0) adopt(SharedPointer<IAudioDevice>::from(util::move(one[0])), false, report);
}

void SystemAudioDevices::initialize() {
  // PCI sound hardware first, then the USB devices that play, then the software fallback if nothing plays, then the USB devices that only record.
  List<UniquePointer<IAudioDevice>> found(allocator_);
  List<IntelHdaAudioDevice*> found_hda(allocator_);   // (parallel to found: the HD-audio controller, or null)
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    const uint64_t before = found.count();
    IntelHdaAudioDevice* hda = nullptr;
    if (d.vendor_id == kVirtioPciVendor && d.device_id == kVirtioSoundDevice)
      (void)try_add<VirtioSoundDevice>(found, allocator_, pci_, d, allocator_, st_);
    else if (d.class_code == kPciClassMultimedia && d.subclass == kPciSubclassHda)
      hda = try_add<IntelHdaAudioDevice>(found, allocator_, pci_, d, allocator_, st_);
    else if (d.class_code == kPciClassMultimedia && d.subclass == kPciSubclassAudio &&
             Ac97AudioDevice::supports(d.vendor_id, d.device_id))
      (void)try_add<Ac97AudioDevice>(found, allocator_, pci_, d, allocator_, st_);
    if (found.count() > before) found_hda.push_back(hda);
  }
  for (uint64_t i = 0; i < found.count(); ++i)
    adopt(SharedPointer<IAudioDevice>::from(util::move(found[i])), true, false, i < found_hda.count() ? found_hda[i] : nullptr);

  List<UniquePointer<UsbAudioDevice>> usb_audio(allocator_);
  if (usb_) UsbAudioDevice::discover(*usb_, allocator_, usb_audio);
  for (uint64_t i = 0; i < usb_audio.count(); ++i)
    if (usb_audio[i]->has_output()) adopt(SharedPointer<IAudioDevice>::from(util::move(usb_audio[i])), true, false);
  bool need_fallback;
  {
    kernel::ScopedMutex guard(mutex_);
    need_fallback = !has_output();
  }
  if (need_fallback) add_fallback(false);
  for (uint64_t i = 0; i < usb_audio.count(); ++i)
    if (usb_audio[i]) adopt(SharedPointer<IAudioDevice>::from(util::move(usb_audio[i])), true, false);
  watcher_.snapshot();   // what was plugged in at start-up has been taken over; later ports are new
}

void SystemAudioDevices::shutdown() {
  kernel::ScopedMutex guard(mutex_);
  pending_.clear();
  for (uint64_t i = devices_.count(); i > 0; --i) devices_[i - 1].device->shutdown();  // newest first
  devices_.clear();
}

uint32_t SystemAudioDevices::count() const {
  kernel::ScopedMutex guard(mutex_);
  return static_cast<uint32_t>(devices_.count());
}

IAudioDevice* SystemAudioDevices::at(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < devices_.count() ? devices_[index].device.get() : nullptr;
}

uint32_t SystemAudioDevices::id(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < devices_.count() ? devices_[index].id : 0;
}

SharedPointer<IAudioDevice> SystemAudioDevices::share(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < devices_.count() ? devices_[index].device : SharedPointer<IAudioDevice>();
}

bool SystemAudioDevices::is_hardware(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < devices_.count() && devices_[index].hardware;
}

// Takes every device that is no longer present out of the list: shut down first (which waits for a request still running on it), then reported.
void SystemAudioDevices::sweep_removed() {
  for (uint64_t i = 0;;) {
    SharedPointer<IAudioDevice> gone;
    uint32_t id = 0;
    bool hardware = true;
    {
      kernel::ScopedMutex guard(mutex_);
      while (i < devices_.count() && devices_[i].device->present()) ++i;
      if (i >= devices_.count()) break;
      gone = devices_[i].device;
      id = devices_[i].id;
      hardware = devices_[i].hardware;
      devices_.remove_at(i);
    }
    gone->shutdown();   // (outside the list's lock: it waits for a write that may be running on the mixer's thread)
    AudioEvent event;
    event.kind = AudioEvent::Kind::Removed;
    event.id = id;
    event.hardware = hardware;
    event.device = util::move(gone);
    kernel::ScopedMutex guard(mutex_);
    pending_.push_back(util::move(event));
  }
  bool need_fallback;
  {
    kernel::ScopedMutex guard(mutex_);
    need_fallback = !has_output();
  }
  if (need_fallback) add_fallback(true);
}

void SystemAudioDevices::service_power(uint64_t now_ms) {
  // The controllers are served outside the list's lock: a suspend calls the power observer (the GPU manager) and waits for the mixer's write.
  List<SharedPointer<IAudioDevice>> keep(allocator_);
  List<IntelHdaAudioDevice*> controllers(allocator_);
  {
    kernel::ScopedMutex guard(mutex_);
    for (const Entry& e : devices_.view()) {
      if (!e.hda) continue;
      if (keep.push_back(e.device)) (void)controllers.push_back(e.hda);
    }
  }
  for (IntelHdaAudioDevice* hda : controllers.view()) hda->service(now_ms);
}

IntelHdaAudioDevice* SystemAudioDevices::find_hda(PciAddress function, SharedPointer<IAudioDevice>* keep) const {
  kernel::ScopedMutex guard(mutex_);
  for (const Entry& e : devices_.view()) {
    if (!e.hda) continue;
    const PciAddress a = e.hda->pci_address();
    if (a.bus == function.bus && a.device == function.device && a.function == function.function) {
      *keep = e.device;
      return e.hda;
    }
  }
  return nullptr;
}

bool SystemAudioDevices::connections_changed(PciAddress function) {
  SharedPointer<IAudioDevice> keep;
  IntelHdaAudioDevice* hda = find_hda(function, &keep);
  return hda && hda->connections_changed();   // (outside the lock: a resume waits for the GPU)
}

bool SystemAudioDevices::attach_power_observer(PciAddress function, IPciFunctionPowerObserver* observer, uint32_t idle_ms) {
  SharedPointer<IAudioDevice> keep;
  IntelHdaAudioDevice* hda = find_hda(function, &keep);
  if (!hda) return false;
  hda->set_power_observer(observer, idle_ms);   // (outside the lock: it may tell the observer at once)
  return true;
}

bool SystemAudioDevices::poll(AudioEvent* event) {
  service_power(clock_.now_ms());
  for (int rounds = 0; rounds < 64; ++rounds) {
    {
      kernel::ScopedMutex guard(mutex_);
      if (pending_.count() > 0) {
        *event = util::move(pending_[0]);
        pending_.remove_at(0);
        return true;
      }
    }
    if (!usb_) return false;
    watcher_.scan([&](uint32_t, IUsbHostController& host, uint32_t port) {
      sweep_removed();
      List<UniquePointer<UsbAudioDevice>> found(allocator_);
      UsbAudioDevice::discover_port(host, port, allocator_, found);
      for (uint64_t i = 0; i < found.count(); ++i) adopt(SharedPointer<IAudioDevice>::from(util::move(found[i])), true, true);
    });
    sweep_removed();   // (a device that was pulled out and nothing came: no port reports it, but the device knows)
    kernel::ScopedMutex guard(mutex_);
    if (pending_.count() == 0) return false;
  }
  return false;
}

}  // namespace hardware
