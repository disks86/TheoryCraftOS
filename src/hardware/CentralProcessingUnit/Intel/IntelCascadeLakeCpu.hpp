#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon Scalable 2nd gen. QEMU: Cascadelake-Server.
class IntelCascadeLakeCpu final : public IntelX64Cpu {
 public:
  explicit IntelCascadeLakeCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Cascade Lake"; }
};

}  // namespace hardware
