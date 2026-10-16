#pragma once
#include "hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Ryzen 5000 / EPYC Milan. QEMU: EPYC-Milan.
class AmdZen3Cpu final : public AmdX64Cpu {
 public:
  explicit AmdZen3Cpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD Zen 3"; }
};

}  // namespace hardware
