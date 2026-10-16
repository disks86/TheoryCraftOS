#pragma once
#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"

namespace hardware {

// AMD x86_64 ("AuthenticAMD", also Hygon's AMD-derived parts). Current AMD
// CPUs are symmetric for scheduling purposes, so every core is Performance.
class AmdX64Cpu : public X86_64Cpu {
 public:
  explicit AmdX64Cpu(efi::SystemTable* st) : X86_64Cpu(st) {}
  const char* name() const override { return "AMD x64 CPU"; }
};

}  // namespace hardware
