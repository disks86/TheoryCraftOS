#pragma once
#include "hardware/CentralProcessingUnit/Cpuid.hpp"
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "uefi.hpp"

namespace hardware {

ICentralProcessingUnit* create_hygon_generation(void* storage, efi::SystemTable* st,
                                                const CpuSignature& sig);

}  // namespace hardware
