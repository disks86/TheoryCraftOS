#pragma once
#include "hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Ryzen 3000 / EPYC Rome. QEMU: EPYC-Rome.
class AmdZen2Cpu final : public AmdX64Cpu {
 public:
  explicit AmdZen2Cpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD Zen 2"; }
};

}  // namespace hardware
