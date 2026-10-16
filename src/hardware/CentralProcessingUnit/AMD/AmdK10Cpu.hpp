#pragma once
#include "hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Phenom / Opteron 23xx. QEMU: Opteron_G3, phenom.
class AmdK10Cpu final : public AmdX64Cpu {
 public:
  explicit AmdK10Cpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD K10"; }
};

}  // namespace hardware
