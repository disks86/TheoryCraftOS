// Helpers for mapping a CPUID family/model/stepping to a generation class.
// Private to CentralProcessingUnit/.
#pragma once
#include <new>
#include "hardware/CentralProcessingUnit/Cpuid.hpp"
#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"

namespace hardware {

using CpuFactory = ICentralProcessingUnit* (*)(void* storage, efi::SystemTable* st);

// Inclusive ranges. The first matching rule wins, so list specific rules first.
struct GenerationRule {
  uint32_t family;
  uint32_t model_lo, model_hi;
  uint32_t stepping_lo, stepping_hi;
  CpuFactory create;
};

// Generation classes only change identity, so they must fit the shared storage.
template <class T>
ICentralProcessingUnit* create_cpu(void* storage, efi::SystemTable* st) {
  static_assert(sizeof(T) == sizeof(X86_64Cpu), "generation classes add no data");
  return new (storage) T(st);
}

template <class T>
constexpr GenerationRule rule(uint32_t family, uint32_t model_lo, uint32_t model_hi) {
  return {family, model_lo, model_hi, 0, 0xF, create_cpu<T>};
}

template <class T>
constexpr GenerationRule rule(uint32_t family, uint32_t model_lo, uint32_t model_hi,
                              uint32_t stepping_lo, uint32_t stepping_hi) {
  return {family, model_lo, model_hi, stepping_lo, stepping_hi, create_cpu<T>};
}

template <unsigned N>
ICentralProcessingUnit* match_generation(const GenerationRule (&rules)[N], const CpuSignature& sig,
                                         void* storage, efi::SystemTable* st) {
  for (const GenerationRule& r : rules)
    if (sig.family == r.family && sig.model >= r.model_lo && sig.model <= r.model_hi &&
        sig.stepping >= r.stepping_lo && sig.stepping <= r.stepping_hi)
      return r.create(storage, st);
  return nullptr;
}

}  // namespace hardware
