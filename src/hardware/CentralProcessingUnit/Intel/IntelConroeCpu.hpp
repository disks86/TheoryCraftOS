#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Core 2, 65 nm. QEMU: Conroe, core2duo.
class IntelConroeCpu final : public IntelX64Cpu {
 public:
  explicit IntelConroeCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Conroe/Merom"; }
};

}  // namespace hardware
