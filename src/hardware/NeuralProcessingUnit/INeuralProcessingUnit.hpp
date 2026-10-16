#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

// A neural / tensor accelerator. Implementations live beside this file.
class INeuralProcessingUnit : public IHardware {
 public:
  virtual uint32_t compute_units() const = 0;  // independent execution units
  virtual uint64_t memory_bytes() const = 0;   // dedicated on-device memory

 protected:
  constexpr INeuralProcessingUnit() = default;
  ~INeuralProcessingUnit() = default;
};

}  // namespace hardware
