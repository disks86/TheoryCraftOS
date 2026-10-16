#pragma once
#include "hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Family 15h, first gen. QEMU: Opteron_G4.
class AmdBulldozerCpu final : public AmdX64Cpu {
 public:
  explicit AmdBulldozerCpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD Bulldozer"; }
};

}  // namespace hardware
