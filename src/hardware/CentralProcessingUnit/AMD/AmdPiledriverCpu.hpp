#pragma once
#include "hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Family 15h, second gen. QEMU: Opteron_G5.
class AmdPiledriverCpu final : public AmdX64Cpu {
 public:
  explicit AmdPiledriverCpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD Piledriver"; }
};

}  // namespace hardware
