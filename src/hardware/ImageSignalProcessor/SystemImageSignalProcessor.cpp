#include "hardware/ImageSignalProcessor/SystemImageSignalProcessor.hpp"
#include <new>
#include "hardware/ImageSignalProcessor/Intel/IntelIpuIsp.hpp"

namespace hardware {

namespace {
static_assert(sizeof(IntelIpu6Isp) == sizeof(SoftwareImageSignalProcessor) &&
                  sizeof(IntelIpu7Isp) == sizeof(SoftwareImageSignalProcessor),
              "ISP classes must fit SystemImageSignalProcessor's storage");

// Camera ISPs enumerate as "multimedia: other" (class 0x04, subclass 0x80).
constexpr uint8_t kClassMultimedia = 0x04;
constexpr uint8_t kSubclassOther = 0x80;
}  // namespace

SystemImageSignalProcessor::SystemImageSignalProcessor(const IPciBus& pci) {
  PciDeviceInfo d;
  for (uint32_t i = 0; !active_ && pci.pci_device(i, &d); ++i) {
    if (d.class_code != kClassMultimedia || d.subclass != kSubclassOther) continue;
    if (d.vendor_id != kIntelPciVendor) continue;
    if (IntelIpu6Isp::matches(d.device_id)) active_ = new (storage_) IntelIpu6Isp();
    else if (IntelIpu7Isp::matches(d.device_id)) active_ = new (storage_) IntelIpu7Isp();
  }
  if (!active_) active_ = new (storage_) SoftwareImageSignalProcessor();
  active_->initialize();
}

}  // namespace hardware
