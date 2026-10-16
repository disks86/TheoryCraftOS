#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Nehalem shrink. QEMU: Westmere.
class IntelWestmereCpu final : public IntelX64Cpu {
 public:
  explicit IntelWestmereCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Westmere"; }
};

}  // namespace hardware
