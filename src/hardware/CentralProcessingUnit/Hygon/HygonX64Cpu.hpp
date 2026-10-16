#pragma once
#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"

namespace hardware {

// Hygon x86_64 ("HygonGenuine"): AMD Zen derivatives sold in China. Symmetric cores.
class HygonX64Cpu : public X86_64Cpu {
 public:
  explicit HygonX64Cpu(efi::SystemTable* st) : X86_64Cpu(st) {}
  const char* name() const override { return "Hygon x64 CPU"; }
};

}  // namespace hardware
