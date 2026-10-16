// Thin wrapper over the CPUID instruction (private to CentralProcessingUnit/).
#pragma once
#include <cpuid.h>
#include <stdint.h>

namespace hardware {

struct CpuidResult {
  uint32_t eax, ebx, ecx, edx;
};

inline CpuidResult cpuid(uint32_t leaf, uint32_t subleaf = 0) {
  CpuidResult r{};
  __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
  return r;
}

// Family/model/stepping as the vendors define them (CPUID.1:EAX with the
// extended fields folded in).
struct CpuSignature {
  uint32_t family, model, stepping;
};

inline CpuSignature cpu_signature() {
  uint32_t eax = cpuid(1).eax;
  uint32_t family = (eax >> 8) & 0xF;
  uint32_t model = (eax >> 4) & 0xF;
  if (family == 0xF) family += (eax >> 20) & 0xFF;
  if (family == 0x6 || family >= 0xF) model |= ((eax >> 16) & 0xF) << 4;
  return {family, model, eax & 0xF};
}

}  // namespace hardware
