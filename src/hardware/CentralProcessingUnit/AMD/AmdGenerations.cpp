#include "hardware/CentralProcessingUnit/AMD/AmdGenerations.hpp"
#include "hardware/CentralProcessingUnit/AMD/AmdBulldozerCpu.hpp"
#include "hardware/CentralProcessingUnit/AMD/AmdK10Cpu.hpp"
#include "hardware/CentralProcessingUnit/AMD/AmdK8Cpu.hpp"
#include "hardware/CentralProcessingUnit/AMD/AmdPiledriverCpu.hpp"
#include "hardware/CentralProcessingUnit/AMD/AmdZen2Cpu.hpp"
#include "hardware/CentralProcessingUnit/AMD/AmdZen3Cpu.hpp"
#include "hardware/CentralProcessingUnit/AMD/AmdZen4Cpu.hpp"
#include "hardware/CentralProcessingUnit/AMD/AmdZen5Cpu.hpp"
#include "hardware/CentralProcessingUnit/AMD/AmdZenCpu.hpp"
#include "hardware/CentralProcessingUnit/GenerationTable.hpp"

namespace hardware {

namespace {
constexpr GenerationRule kRules[] = {
    // Model 0x6B is QEMU's qemu64, which is not a K8: leave it to QemuX64Cpu.
    rule<AmdK8Cpu>(0x0F, 0x00, 0x6A),
    rule<AmdK10Cpu>(0x10, 0x00, 0xFF),
    rule<AmdBulldozerCpu>(0x15, 0x00, 0x01),
    rule<AmdPiledriverCpu>(0x15, 0x02, 0x02),
    rule<AmdPiledriverCpu>(0x15, 0x10, 0x1F),
    rule<AmdZenCpu>(0x17, 0x00, 0x2F),
    rule<AmdZen2Cpu>(0x17, 0x30, 0xFF),
    rule<AmdZen3Cpu>(0x19, 0x00, 0x0F),
    rule<AmdZen4Cpu>(0x19, 0x10, 0x1F),
    rule<AmdZen3Cpu>(0x19, 0x20, 0x5F),
    rule<AmdZen4Cpu>(0x19, 0x60, 0xAF),
    rule<AmdZen5Cpu>(0x1A, 0x00, 0xFF),
};
}  // namespace

ICentralProcessingUnit* create_amd_generation(void* storage, efi::SystemTable* st,
                                              const CpuSignature& sig) {
  return match_generation(kRules, sig, storage, st);
}

}  // namespace hardware
