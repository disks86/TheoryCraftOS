#pragma once
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/ImageSignalProcessor/IImageSignalProcessor.hpp"
#include "hardware/ImageSignalProcessor/Software/SoftwareImageSignalProcessor.hpp"

namespace hardware {

// Picks the ISP class for this machine: a vendor class when a known hardware
// ISP is on the PCI bus, otherwise the software ISP. There is always one.
class SystemImageSignalProcessor {
 public:
  explicit SystemImageSignalProcessor(const IPciBus& pci);

  IImageSignalProcessor& isp() { return *active_; }

 private:
  // Vendor classes add no data, so the software ISP's size covers them all.
  alignas(16) unsigned char storage_[sizeof(SoftwareImageSignalProcessor)];
  IImageSignalProcessor* active_ = nullptr;
};

}  // namespace hardware
