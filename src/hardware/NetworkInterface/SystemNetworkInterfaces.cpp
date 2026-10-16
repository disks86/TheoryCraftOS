#include "hardware/NetworkInterface/SystemNetworkInterfaces.hpp"
#include "hardware/NetworkInterface/Intel/E1000NetworkInterface.hpp"
#include "hardware/NetworkInterface/Realtek/Rtl8139NetworkInterface.hpp"
#include "hardware/NetworkInterface/UEFI/UefiSimpleNetworkInterface.hpp"
#include "hardware/NetworkInterface/Virtio/VirtioNetworkInterface.hpp"

namespace hardware {

namespace {
constexpr uint8_t kPciClassNetwork = 0x02;

// Builds a T, initializes it, and keeps it in `list` only if that all works.
template <class T, class... Args>
void try_add(List<UniquePointer<INetworkInterface>>& list, allocators::IAllocator& allocator, Args&&... args) {
  UniquePointer<T> nic = UniquePointer<T>::make(allocator, static_cast<Args&&>(args)...);
  if (!nic) return;
  if (!nic->initialize()) {
    nic->shutdown();  // hands the device back to the firmware
    return;
  }
  list.push_back(UniquePointer<INetworkInterface>(util::move(nic)));
}
}  // namespace

void SystemNetworkInterfaces::find_native() {
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.class_code != kPciClassNetwork) continue;
    if (d.vendor_id == kVirtioPciVendor &&
        (d.device_id == kVirtioNetDevice || d.device_id == kVirtioNetTransitionalDevice))
      try_add<VirtioNetworkInterface>(interfaces_, allocator_, pci_, d, allocator_, st_);
    else if (E1000NetworkInterface::supports(d.vendor_id, d.device_id))
      try_add<E1000NetworkInterface>(interfaces_, allocator_, pci_, d, allocator_, st_);
    else if (Rtl8139NetworkInterface::supports(d.vendor_id, d.device_id))
      try_add<Rtl8139NetworkInterface>(interfaces_, allocator_, pci_, d, allocator_, st_);
  }
}

// NICs without a native driver: whatever is still bound to a firmware driver.
// (Native drivers detach the firmware's, so those NICs no longer show up here.)
void SystemNetworkInterfaces::find_firmware() {
  efi::BootServices* bs = st_->boot_services;
  efi::Uintn count = 0;
  efi::Handle* handles = nullptr;
  if (bs->locate_handle_buffer(efi::LocateSearchType::ByProtocol, &efi::kSimpleNetworkGuid, nullptr, &count,
                               &handles) != efi::kSuccess)
    return;
  for (efi::Uintn i = 0; i < count; ++i) {
    efi::SimpleNetwork* snp = nullptr;
    if (bs->handle_protocol(handles[i], &efi::kSimpleNetworkGuid, reinterpret_cast<void**>(&snp)) != efi::kSuccess ||
        !snp)
      continue;
    try_add<UefiSimpleNetworkInterface>(interfaces_, allocator_, snp, st_);
  }
  bs->free_pool(handles);
}

void SystemNetworkInterfaces::initialize() {
  find_native();
  find_firmware();
}

void SystemNetworkInterfaces::shutdown() {
  for (uint64_t i = interfaces_.count(); i > 0; --i) interfaces_[i - 1]->shutdown();  // newest first
  interfaces_.clear();
}

}  // namespace hardware
