#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Core 3rd gen. QEMU: IvyBridge.
class IntelIvyBridgeCpu final : public IntelX64Cpu {
 public:
  explicit IntelIvyBridgeCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Ivy Bridge"; }
};

}  // namespace hardware
