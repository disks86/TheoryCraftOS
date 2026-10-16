#include "hardware/GraphicsProcessingUnit/SystemGpu.hpp"
#include <new>
#include "hardware/GraphicsProcessingUnit/AMD/AmdGpu.hpp"
#include "hardware/GraphicsProcessingUnit/Intel/IntelGpu.hpp"
#include "hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpu.hpp"

namespace hardware {

namespace {
static_assert(sizeof(IntelGpu) == sizeof(UefiGopGpu) && sizeof(AmdGpu) == sizeof(UefiGopGpu) &&
                  sizeof(NvidiaGpu) == sizeof(UefiGopGpu),
              "vendor GPU classes must fit SystemGpu's storage");

// Builds T in `storage`, returning it if it initializes; otherwise tears it down.
template <class T>
IGraphicsProcessingUnit* try_gpu(void* storage, const GpuContext& context) {
  T* gpu = new (storage) T(context);
  if (gpu->initialize()) return gpu;
  gpu->shutdown();
  return nullptr;
}
}  // namespace

SystemGpu::SystemGpu(efi::SystemTable* st, const IPciBus& pci) {
  // The first display-class device is assumed to be the primary one.
  PciDeviceInfo d{};
  bool found = false;
  for (uint32_t i = 0; !found && pci.pci_device(i, &d); ++i) found = d.class_code == kPciClassDisplay;
  if (!found) d = {};
  const GpuContext context{st, &pci, d};

  if (found) {
    if (d.vendor_id == QemuStdVgaGpu::kPciVendor && d.device_id == QemuStdVgaGpu::kPciDevice)
      active_ = try_gpu<QemuStdVgaGpu>(storage_, context);
    else if (d.vendor_id == IntelGpu::kPciVendor)
      active_ = try_gpu<IntelGpu>(storage_, context);
    else if (d.vendor_id == AmdGpu::kPciVendor)
      active_ = try_gpu<AmdGpu>(storage_, context);
    else if (d.vendor_id == NvidiaGpu::kPciVendor)
      active_ = try_gpu<NvidiaGpu>(storage_, context);
  }
  if (!active_) active_ = try_gpu<UefiGopGpu>(storage_, context);  // always-available fallback
}

}  // namespace hardware
