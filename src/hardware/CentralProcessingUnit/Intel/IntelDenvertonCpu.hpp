#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Atom C3000 (Goldmont). QEMU: Denverton.
class IntelDenvertonCpu final : public IntelX64Cpu {
 public:
  explicit IntelDenvertonCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Denverton"; }
};

}  // namespace hardware
