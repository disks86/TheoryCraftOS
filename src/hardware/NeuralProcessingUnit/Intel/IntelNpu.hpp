#pragma once
#include "hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"

namespace hardware {

// Intel's NPU ("VPU"), the neural engine in Core Ultra processors: Meteor Lake,
// Arrow Lake, Lunar Lake and Panther Lake. This class names the device; the
// computation still runs on the CPU until a native driver exists, and this is
// where it will go.
class IntelNpu final : public SoftwareNeuralProcessingUnit {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  using SoftwareNeuralProcessingUnit::SoftwareNeuralProcessingUnit;

  const char* name() const override { return "Intel NPU (software path)"; }
  static bool matches(uint16_t device_id) {
    return device_id == 0x7D1D ||  // Meteor Lake
           device_id == 0xAD1D ||  // Arrow Lake
           device_id == 0x643E ||  // Lunar Lake
           device_id == 0xB03E;    // Panther Lake
  }
};

}  // namespace hardware
