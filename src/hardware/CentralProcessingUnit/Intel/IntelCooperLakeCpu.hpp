#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon Scalable 3rd gen (4-8 socket). QEMU: Cooperlake.
class IntelCooperLakeCpu final : public IntelX64Cpu {
 public:
  explicit IntelCooperLakeCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Cooper Lake"; }
};

}  // namespace hardware
