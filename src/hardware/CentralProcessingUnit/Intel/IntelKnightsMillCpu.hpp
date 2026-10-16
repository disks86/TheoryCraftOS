#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon Phi. QEMU: KnightsMill.
class IntelKnightsMillCpu final : public IntelX64Cpu {
 public:
  explicit IntelKnightsMillCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Knights Mill"; }
};

}  // namespace hardware
