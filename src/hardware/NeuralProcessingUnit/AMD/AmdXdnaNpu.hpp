#pragma once
#include "hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"

namespace hardware {

// AMD's XDNA NPU in Ryzen AI processors (Phoenix, Hawk Point, Strix). This
// class names the device; the computation still runs on the CPU until a native
// driver exists, and this is where it will go.
class AmdXdnaNpu final : public SoftwareNeuralProcessingUnit {
 public:
  static constexpr uint16_t kPciVendor = 0x1022;

  using SoftwareNeuralProcessingUnit::SoftwareNeuralProcessingUnit;

  const char* name() const override { return "AMD XDNA NPU (software path)"; }
  static bool matches(uint16_t device_id) {
    return device_id == 0x1502 ||  // Phoenix / Hawk Point
           device_id == 0x17F0;    // Strix and later
  }
};

}  // namespace hardware
