#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Atom P5000 (Tremont). QEMU: Snowridge.
class IntelSnowRidgeCpu final : public IntelX64Cpu {
 public:
  explicit IntelSnowRidgeCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Snow Ridge"; }
};

}  // namespace hardware
