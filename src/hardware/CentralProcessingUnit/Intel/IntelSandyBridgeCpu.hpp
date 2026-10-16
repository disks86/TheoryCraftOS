#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Core 2nd gen. QEMU: SandyBridge.
class IntelSandyBridgeCpu final : public IntelX64Cpu {
 public:
  explicit IntelSandyBridgeCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Sandy Bridge"; }
};

}  // namespace hardware
