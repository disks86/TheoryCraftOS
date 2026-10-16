#include "hardware/CentralProcessingUnit/Zhaoxin/ZhaoxinGenerations.hpp"
#include "hardware/CentralProcessingUnit/GenerationTable.hpp"
#include "hardware/CentralProcessingUnit/Zhaoxin/ZhaoxinYongFengCpu.hpp"

namespace hardware {

namespace {
constexpr GenerationRule kRules[] = {
    rule<ZhaoxinYongFengCpu>(0x07, 0x0B, 0x0B),
};
}  // namespace

ICentralProcessingUnit* create_zhaoxin_generation(void* storage, efi::SystemTable* st,
                                                  const CpuSignature& sig) {
  return match_generation(kRules, sig, storage, st);
}

}  // namespace hardware
