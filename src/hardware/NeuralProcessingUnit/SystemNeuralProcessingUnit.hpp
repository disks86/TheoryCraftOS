#pragma once
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"

namespace hardware {

// Picks the NPU class for this machine: a vendor class when a known hardware
// NPU is on the PCI bus, otherwise the software NPU. There is always one.
class SystemNeuralProcessingUnit {
 public:
  SystemNeuralProcessingUnit(const IPciBus& pci, ICentralProcessingUnit& cpu,
                             scheduling::ICPUScheduler& scheduler);

  INeuralProcessingUnit& npu() { return *active_; }

 private:
  // Vendor classes add no data, so the software NPU's size covers them all.
  alignas(16) unsigned char storage_[sizeof(SoftwareNeuralProcessingUnit)];
  INeuralProcessingUnit* active_ = nullptr;
};

}  // namespace hardware
