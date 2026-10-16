#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Core 6th gen. QEMU: Skylake-Client.
class IntelSkylakeClientCpu final : public IntelX64Cpu {
 public:
  explicit IntelSkylakeClientCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Skylake (client)"; }
};

}  // namespace hardware
