#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon Scalable 3rd gen. QEMU: Icelake-Server.
class IntelIceLakeServerCpu final : public IntelX64Cpu {
 public:
  explicit IntelIceLakeServerCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Ice Lake-SP"; }
};

}  // namespace hardware
