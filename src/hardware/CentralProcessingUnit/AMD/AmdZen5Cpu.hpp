#pragma once
#include "hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Ryzen 9000 / EPYC Turin. QEMU: EPYC-Turin.
class AmdZen5Cpu final : public AmdX64Cpu {
 public:
  explicit AmdZen5Cpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD Zen 5"; }
};

}  // namespace hardware
