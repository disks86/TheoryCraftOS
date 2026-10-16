#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon 6 (E-cores). QEMU: SierraForest.
class IntelSierraForestCpu final : public IntelX64Cpu {
 public:
  explicit IntelSierraForestCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Sierra Forest"; }
};

}  // namespace hardware
