#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// First Core i7. QEMU: Nehalem.
class IntelNehalemCpu final : public IntelX64Cpu {
 public:
  explicit IntelNehalemCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Nehalem"; }
};

}  // namespace hardware
