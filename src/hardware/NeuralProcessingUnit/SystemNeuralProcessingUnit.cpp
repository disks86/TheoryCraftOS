#include "hardware/NeuralProcessingUnit/SystemNeuralProcessingUnit.hpp"
#include <new>
#include "hardware/NeuralProcessingUnit/AMD/AmdXdnaNpu.hpp"
#include "hardware/NeuralProcessingUnit/Intel/IntelNpu.hpp"

namespace hardware {

namespace {
static_assert(sizeof(IntelNpu) == sizeof(SoftwareNeuralProcessingUnit) &&
                  sizeof(AmdXdnaNpu) == sizeof(SoftwareNeuralProcessingUnit),
              "NPU classes must fit SystemNeuralProcessingUnit's storage");
}

SystemNeuralProcessingUnit::SystemNeuralProcessingUnit(const IPciBus& pci, ICentralProcessingUnit& cpu,
                                                       scheduling::ICPUScheduler& scheduler) {
  PciDeviceInfo d;
  for (uint32_t i = 0; !active_ && pci.pci_device(i, &d); ++i) {
    if (d.vendor_id == IntelNpu::kPciVendor && IntelNpu::matches(d.device_id))
      active_ = new (storage_) IntelNpu(cpu, scheduler);
    else if (d.vendor_id == AmdXdnaNpu::kPciVendor && AmdXdnaNpu::matches(d.device_id))
      active_ = new (storage_) AmdXdnaNpu(cpu, scheduler);
  }
  if (!active_) active_ = new (storage_) SoftwareNeuralProcessingUnit(cpu, scheduler);
  active_->initialize();
}

}  // namespace hardware
