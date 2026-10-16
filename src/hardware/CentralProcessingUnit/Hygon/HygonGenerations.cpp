#include "hardware/CentralProcessingUnit/Hygon/HygonGenerations.hpp"
#include "hardware/CentralProcessingUnit/GenerationTable.hpp"
#include "hardware/CentralProcessingUnit/Hygon/HygonDhyanaCpu.hpp"

namespace hardware {

namespace {
constexpr GenerationRule kRules[] = {
    rule<HygonDhyanaCpu>(0x18, 0x00, 0xFF),
};
}  // namespace

ICentralProcessingUnit* create_hygon_generation(void* storage, efi::SystemTable* st,
                                                const CpuSignature& sig) {
  return match_generation(kRules, sig, storage, st);
}

}  // namespace hardware
