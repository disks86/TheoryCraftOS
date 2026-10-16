#pragma once
#include "hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Athlon 64 / Opteron. QEMU: Opteron_G1, Opteron_G2, kvm64.
class AmdK8Cpu final : public AmdX64Cpu {
 public:
  explicit AmdK8Cpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD K8"; }
};

}  // namespace hardware
