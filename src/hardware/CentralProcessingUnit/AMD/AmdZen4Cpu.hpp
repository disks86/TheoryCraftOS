#pragma once
#include "hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Ryzen 7000 / EPYC Genoa. QEMU: EPYC-Genoa.
class AmdZen4Cpu final : public AmdX64Cpu {
 public:
  explicit AmdZen4Cpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD Zen 4"; }
};

}  // namespace hardware
