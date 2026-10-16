#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Core 4th gen. QEMU: Haswell.
class IntelHaswellCpu final : public IntelX64Cpu {
 public:
  explicit IntelHaswellCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Haswell"; }
};

}  // namespace hardware
