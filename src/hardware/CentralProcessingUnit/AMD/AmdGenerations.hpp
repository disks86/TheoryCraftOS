#pragma once
#include "hardware/CentralProcessingUnit/Cpuid.hpp"
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "uefi.hpp"

namespace hardware {

// Constructs the AMD generation class for this signature in `storage`
// (at least sizeof(X86_64Cpu) bytes). Returns nullptr for unknown models.
ICentralProcessingUnit* create_amd_generation(void* storage, efi::SystemTable* st,
                                              const CpuSignature& sig);

}  // namespace hardware
