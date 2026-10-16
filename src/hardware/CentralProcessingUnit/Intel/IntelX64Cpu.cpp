#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"
#include "hardware/CentralProcessingUnit/Cpuid.hpp"

namespace hardware {

CoreKind IntelX64Cpu::probe_core_kind() const {
  constexpr uint32_t kHybridBit = 1u << 15;  // CPUID.7.0:EDX
  constexpr uint32_t kAtom = 0x20, kCore = 0x40;
  if (cpuid(0).eax < 0x1A || !(cpuid(7).edx & kHybridBit)) return CoreKind::Performance;
  switch (cpuid(0x1A).eax >> 24) {  // native model ID: core type
    case kAtom: return CoreKind::Efficiency;
    case kCore: return CoreKind::Performance;
    default:    return CoreKind::Performance;
  }
}

}  // namespace hardware
