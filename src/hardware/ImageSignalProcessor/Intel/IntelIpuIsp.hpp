#pragma once
#include "hardware/ImageSignalProcessor/Software/SoftwareImageSignalProcessor.hpp"

namespace hardware {

constexpr uint16_t kIntelPciVendor = 0x8086;

// Intel's Image Processing Unit (IPU), the camera ISP in laptop SoCs: IPU6 in
// Tiger Lake through Meteor Lake, IPU7 from Lunar Lake. These classes name the
// device generation; frames are still processed in software until a native
// driver exists, and this is where it will go.
class IntelIpu6Isp final : public SoftwareImageSignalProcessor {
 public:
  const char* name() const override { return "Intel IPU6 ISP (software path)"; }
  static bool matches(uint16_t device_id) {
    switch (device_id) {
      case 0x9A19:  // Tiger Lake
      case 0x4E19:  // Jasper Lake / IPU6SE
      case 0x465D:  // Alder Lake-P
      case 0x462E:  // Alder Lake-N
      case 0xA75D:  // Raptor Lake-P
      case 0x7D19:  // Meteor Lake
        return true;
      default: return false;
    }
  }
};

class IntelIpu7Isp final : public SoftwareImageSignalProcessor {
 public:
  const char* name() const override { return "Intel IPU7 ISP (software path)"; }
  static bool matches(uint16_t device_id) {
    return device_id == 0x645D ||  // Lunar Lake
           device_id == 0xB05D;    // Panther Lake
  }
};

}  // namespace hardware
