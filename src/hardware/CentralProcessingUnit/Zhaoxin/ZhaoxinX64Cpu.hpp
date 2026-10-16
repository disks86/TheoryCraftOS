#pragma once
#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"

namespace hardware {

// Zhaoxin x86_64 ("  Shanghai  " / "CentaurHauls"). Symmetric cores.
class ZhaoxinX64Cpu : public X86_64Cpu {
 public:
  explicit ZhaoxinX64Cpu(efi::SystemTable* st) : X86_64Cpu(st) {}
  const char* name() const override { return "Zhaoxin x64 CPU"; }
};

}  // namespace hardware
