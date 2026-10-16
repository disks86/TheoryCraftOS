#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon (E-cores). QEMU: ClearwaterForest.
class IntelClearwaterForestCpu final : public IntelX64Cpu {
 public:
  explicit IntelClearwaterForestCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Clearwater Forest"; }
};

}  // namespace hardware
