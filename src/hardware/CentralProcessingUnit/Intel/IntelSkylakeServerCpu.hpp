#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon Scalable 1st gen. QEMU: Skylake-Server.
class IntelSkylakeServerCpu final : public IntelX64Cpu {
 public:
  explicit IntelSkylakeServerCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Skylake-SP"; }
};

}  // namespace hardware
