#pragma once
#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"

namespace hardware {

// Intel x86_64 ("GenuineIntel"). Hybrid parts (Alder Lake and later) mix
// P-cores and E-cores; each core reports its type in CPUID leaf 0x1A.
class IntelX64Cpu : public X86_64Cpu {
 public:
  explicit IntelX64Cpu(efi::SystemTable* st) : X86_64Cpu(st) {}
  const char* name() const override { return "Intel x64 CPU"; }

 protected:
  CoreKind probe_core_kind() const override;
};

}  // namespace hardware
