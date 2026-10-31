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

#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "Hardware/StorageDevice/Ahci/AhciStorageDevice.hpp"
#include "Hardware/StorageDevice/Ide/IdeStorageDevice.hpp"
#include "Hardware/StorageDevice/NVMe/NvmeStorageDevice.hpp"
#include "Hardware/StorageDevice/Usb/UsbMassStorageDevice.hpp"
#include "Hardware/StorageDevice/UEFI/UefiBlockStorageDevice.hpp"
#include "Hardware/StorageDevice/Virtio/VirtioBlockDevice.hpp"
#include "Hardware/StorageDevice/Virtio/VirtioScsiDevice.hpp"
#include "Hardware/StorageDevice/VMware/PvscsiDevice.hpp"

namespace hardware {

namespace {
constexpr uint8_t kPciClassMassStorage = 0x01;

// A controller shared by several devices: initialize it, let `discover` add the
// devices behind it, and hand the controller back to the firmware if none came.
template <class Controller, class Device, class... Args>
void add_controller(List<UniquePointer<IStorageDevice>>& list, allocators::IAllocator& allocator, Args&&... args) {
  SharedPointer<Controller> controller = SharedPointer<Controller>::make(allocator, static_cast<Args&&>(args)...);
  if (!controller) return;
  if (!controller->initialize()) {
    controller->shutdown();
    return;
  }
  const uint64_t before = list.count();
  Device::discover(controller, allocator, list);
  if (list.count() == before) controller->shutdown();
}

// A controller with exactly one device.
template <class T, class... Args>
void add_device(List<UniquePointer<IStorageDevice>>& list, allocators::IAllocator& allocator, Args&&... args) {
  UniquePointer<T> device = UniquePointer<T>::make(allocator, static_cast<Args&&>(args)...);
  if (!device) return;
  if (!device->initialize()) {
    device->shutdown();  // hands the device back to the firmware
    return;
  }
  list.push_back(UniquePointer<IStorageDevice>(util::move(device)));
}
}  // namespace

void SystemStorageDevices::find_native(List<UniquePointer<IStorageDevice>>& found) {
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.class_code != kPciClassMassStorage) continue;
    if (d.vendor_id == kVirtioPciVendor) {
      if (d.device_id == kVirtioBlockDevice || d.device_id == kVirtioBlockTransitionalDevice)
        add_device<VirtioBlockDevice>(found, allocator_, pci_, d, allocator_, st_);
      else if (d.device_id == kVirtioScsiDevice || d.device_id == kVirtioScsiTransitionalDevice)
        add_controller<VirtioScsiController, VirtioScsiDevice>(found, allocator_, pci_, d, allocator_, st_);
    } else if (PvscsiController::supports(d)) {
      add_controller<PvscsiController, PvscsiDevice>(found, allocator_, pci_, d, allocator_, st_);
    } else if (NvmeStorageDevice::supports(d)) {
      add_device<NvmeStorageDevice>(found, allocator_, pci_, d, allocator_, st_);
    } else if (AhciController::supports(d)) {
      add_controller<AhciController, AhciStorageDevice>(found, allocator_, pci_, d, st_);
    } else if (IdeController::supports(d)) {
      add_controller<IdeController, IdeStorageDevice>(found, allocator_, pci_, d, st_);
    }
  }
  if (usb_) UsbMassStorageDevice::discover(*usb_, allocator_, st_, found);
}

// Whole devices still bound to a firmware driver. (Native drivers detach the
// firmware's, so their devices no longer show up here.)
void SystemStorageDevices::find_firmware(List<UniquePointer<IStorageDevice>>& found) {
  efi::BootServices* bs = st_->boot_services;
  efi::Uintn count = 0;
  efi::Handle* handles = nullptr;
  if (bs->locate_handle_buffer(efi::LocateSearchType::ByProtocol, &efi::kBlockIoGuid, nullptr, &count, &handles) !=
      efi::kSuccess)
    return;
  for (efi::Uintn i = 0; i < count; ++i) {
    efi::BlockIo* io = nullptr;
    if (bs->handle_protocol(handles[i], &efi::kBlockIoGuid, reinterpret_cast<void**>(&io)) != efi::kSuccess || !io ||
        !io->media || io->media->logical_partition || !io->media->media_present)
      continue;
    add_device<UefiBlockStorageDevice>(found, allocator_, io, allocator_, st_);
  }
  bs->free_pool(handles);
}

void SystemStorageDevices::adopt(List<UniquePointer<IStorageDevice>>& found, bool report) {
  for (uint64_t i = 0; i < found.count(); ++i) {
    Entry entry;
    entry.device = SharedPointer<IStorageDevice>::from(util::move(found[i]));
    if (!entry.device) continue;   // (out of memory: the device is dropped with the list)
    entry.id = next_id_++;
    StorageEvent event;
    event.kind = StorageEvent::Kind::Added;
    event.id = entry.id;
    event.device = entry.device;
    kernel::ScopedMutex guard(mutex_);
    devices_.push_back(util::move(entry));
    if (report) pending_.push_back(util::move(event));
  }
  found.clear();
}

void SystemStorageDevices::initialize() {
  List<UniquePointer<IStorageDevice>> found(allocator_);
  find_native(found);
  find_firmware(found);
  adopt(found, false);
  watcher_.snapshot();   // what was plugged in at start-up has been taken over; later ports are new
}

bool SystemStorageDevices::add_source(IRemovableMediaSource* source) {
  if (!source) return false;
  for (IRemovableMediaSource*& slot : sources_)
    if (!slot) { slot = source; return true; }
  return false;
}

void SystemStorageDevices::shutdown() {
  kernel::ScopedMutex guard(mutex_);
  pending_.clear();
  for (uint64_t i = devices_.count(); i > 0; --i) devices_[i - 1].device->shutdown();  // newest first
  devices_.clear();
}

uint32_t SystemStorageDevices::count() const {
  kernel::ScopedMutex guard(mutex_);
  return static_cast<uint32_t>(devices_.count());
}

IStorageDevice* SystemStorageDevices::at(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < devices_.count() ? devices_[index].device.get() : nullptr;
}

uint32_t SystemStorageDevices::id(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < devices_.count() ? devices_[index].id : 0;
}

SharedPointer<IStorageDevice> SystemStorageDevices::share(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < devices_.count() ? devices_[index].device : SharedPointer<IStorageDevice>();
}

// Takes every device that is no longer present out of the list: shut down first (which waits for a request still running on it), then reported.
void SystemStorageDevices::sweep_removed() {
  for (uint64_t i = 0;;) {
    SharedPointer<IStorageDevice> gone;
    uint32_t id = 0;
    {
      kernel::ScopedMutex guard(mutex_);
      while (i < devices_.count() && devices_[i].device->present()) ++i;
      if (i >= devices_.count()) return;
      gone = devices_[i].device;
      id = devices_[i].id;
      devices_.remove_at(i);
    }
    gone->shutdown();   // (outside the list's lock: it waits for requests, which may be using the list through a mount)
    StorageEvent event;
    event.kind = StorageEvent::Kind::Removed;
    event.id = id;
    event.ejected = gone->ejected();
    event.device = util::move(gone);
    kernel::ScopedMutex guard(mutex_);
    pending_.push_back(util::move(event));
  }
}

SharedPointer<IStorageDevice> SystemStorageDevices::find(uint32_t id) const {
  kernel::ScopedMutex guard(mutex_);
  for (const Entry& e : devices_.view())
    if (e.id == id) return e.device;
  return SharedPointer<IStorageDevice>();
}

bool SystemStorageDevices::can_eject(uint32_t id) const {
  SharedPointer<IStorageDevice> device = find(id);
  return device && device->present() && device->can_eject();
}

EjectResult SystemStorageDevices::eject(uint32_t id) {
  SharedPointer<IStorageDevice> device = find(id);
  if (!device || !device->present()) return EjectResult::NotFound;
  if (!device->can_eject()) return EjectResult::NotSupported;
  if (!device->eject()) return EjectResult::Failed;
  sweep_removed();   // it is no longer present: out of the list, shut down, reported
  return EjectResult::Ok;
}

bool SystemStorageDevices::poll(StorageEvent* event) {
  for (int rounds = 0; rounds < 64; ++rounds) {
    {
      kernel::ScopedMutex guard(mutex_);
      if (pending_.count() > 0) {
        *event = util::move(pending_[0]);
        pending_.remove_at(0);
        return true;
      }
    }
    // Removable media outside the USB ports (the SD slot): what went is taken out first, then what came is added.
    sweep_removed();
    for (IRemovableMediaSource* source : sources_) {
      if (!source) continue;
      List<UniquePointer<IStorageDevice>> found(allocator_);
      source->discover(found);
      adopt(found, true);
    }
    // Look for the USB sticks that went (their port moved on to another device or to none), then for new ones.
    if (usb_) {
      watcher_.scan([&](uint32_t, IUsbHostController& host, uint32_t port) {
        sweep_removed();
        List<UniquePointer<IStorageDevice>> found(allocator_);
        UsbMassStorageDevice::discover_port(host, port, allocator_, st_, found);
        adopt(found, true);
      });
    }
    sweep_removed();   // (a stick that was pulled out and nothing came: no port reports it, but the device knows)
    kernel::ScopedMutex guard(mutex_);
    if (pending_.count() == 0) return false;
  }
  return false;
}

}  // namespace hardware
