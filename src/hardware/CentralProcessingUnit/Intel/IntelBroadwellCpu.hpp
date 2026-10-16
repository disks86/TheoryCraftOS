#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Core 5th gen. QEMU: Broadwell.
class IntelBroadwellCpu final : public IntelX64Cpu {
 public:
  explicit IntelBroadwellCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Broadwell"; }
};

}  // namespace hardware
