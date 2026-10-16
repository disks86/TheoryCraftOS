#include "hardware/StorageDevice/SystemStorageDevices.hpp"
#include "hardware/StorageDevice/Ahci/AhciStorageDevice.hpp"
#include "hardware/StorageDevice/Ide/IdeStorageDevice.hpp"
#include "hardware/StorageDevice/NVMe/NvmeStorageDevice.hpp"
#include "hardware/StorageDevice/Usb/UsbMassStorageDevice.hpp"
#include "hardware/StorageDevice/UEFI/UefiBlockStorageDevice.hpp"
#include "hardware/StorageDevice/Virtio/VirtioBlockDevice.hpp"
#include "hardware/StorageDevice/Virtio/VirtioScsiDevice.hpp"

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

void SystemStorageDevices::find_native() {
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.class_code != kPciClassMassStorage) continue;
    if (d.vendor_id == kVirtioPciVendor) {
      if (d.device_id == kVirtioBlockDevice || d.device_id == kVirtioBlockTransitionalDevice)
        add_device<VirtioBlockDevice>(devices_, allocator_, pci_, d, allocator_, st_);
      else if (d.device_id == kVirtioScsiDevice || d.device_id == kVirtioScsiTransitionalDevice)
        add_controller<VirtioScsiController, VirtioScsiDevice>(devices_, allocator_, pci_, d, allocator_, st_);
    } else if (NvmeStorageDevice::supports(d)) {
      add_device<NvmeStorageDevice>(devices_, allocator_, pci_, d, allocator_, st_);
    } else if (AhciController::supports(d)) {
      add_controller<AhciController, AhciStorageDevice>(devices_, allocator_, pci_, d, st_);
    } else if (IdeController::supports(d)) {
      add_controller<IdeController, IdeStorageDevice>(devices_, allocator_, pci_, d, st_);
    }
  }
  if (usb_) UsbMassStorageDevice::discover(*usb_, allocator_, st_, devices_);
}

// Whole devices still bound to a firmware driver. (Native drivers detach the
// firmware's, so their devices no longer show up here.)
void SystemStorageDevices::find_firmware() {
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
    add_device<UefiBlockStorageDevice>(devices_, allocator_, io, allocator_, st_);
  }
  bs->free_pool(handles);
}

void SystemStorageDevices::initialize() {
  find_native();
  find_firmware();
}

void SystemStorageDevices::shutdown() {
  for (uint64_t i = devices_.count(); i > 0; --i) devices_[i - 1]->shutdown();  // newest first
  devices_.clear();
}

}  // namespace hardware
