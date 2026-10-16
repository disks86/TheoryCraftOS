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

struct Finder {
  PciDeviceInfo device{};
  bool found = false;
};

bool take_first_display(const PciDeviceInfo& d, void* context) {
  auto* f = static_cast<Finder*>(context);
  if (d.class_code != kPciClassDisplay) return true;
  f->device = d;
  f->found = true;
  return false;
}

// Builds T in `storage`, returning it if it initializes; otherwise tears it down.
template <class T>
IGraphicsProcessingUnit* try_gpu(void* storage, const GpuContext& context) {
  T* gpu = new (storage) T(context);
  if (gpu->initialize()) return gpu;
  gpu->shutdown();
  return nullptr;
}
}  // namespace

SystemGpu::SystemGpu(efi::SystemTable* st, const PciBus& pci) {
  Finder finder;
  pci.enumerate(take_first_display, &finder);
  const GpuContext context{st, &pci, finder.device};

  if (finder.found) {
    const PciDeviceInfo& d = finder.device;
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
