#pragma once
#include "hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Ryzen 1000 / EPYC Naples. QEMU: EPYC.
class AmdZenCpu final : public AmdX64Cpu {
 public:
  explicit AmdZenCpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD Zen"; }
};

}  // namespace hardware
