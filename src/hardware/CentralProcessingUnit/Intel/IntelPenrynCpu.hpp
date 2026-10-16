#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Core 2, 45 nm. QEMU: Penryn.
class IntelPenrynCpu final : public IntelX64Cpu {
 public:
  explicit IntelPenrynCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Penryn"; }
};

}  // namespace hardware
