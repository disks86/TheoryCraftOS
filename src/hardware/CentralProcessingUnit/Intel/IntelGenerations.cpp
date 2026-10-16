/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "hardware/CentralProcessingUnit/Intel/IntelGenerations.hpp"
#include "hardware/CentralProcessingUnit/GenerationTable.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelBroadwellCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelCascadeLakeCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelClearwaterForestCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelConroeCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelCooperLakeCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelDenvertonCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelDiamondRapidsCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelGraniteRapidsCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelHaswellCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelIceLakeServerCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelIvyBridgeCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelKnightsMillCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelNehalemCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelPenrynCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelSandyBridgeCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelSapphireRapidsCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelSierraForestCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelSkylakeClientCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelSkylakeServerCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelSnowRidgeCpu.hpp"
#include "hardware/CentralProcessingUnit/Intel/IntelWestmereCpu.hpp"

namespace hardware {

namespace {
// Family 6 unless noted. Model numbers cover the generation's variants.
constexpr GenerationRule kRules[] = {
    rule<IntelConroeCpu>(6, 0x0F, 0x0F),
    rule<IntelConroeCpu>(6, 0x16, 0x16),
    rule<IntelPenrynCpu>(6, 0x17, 0x17),
    rule<IntelPenrynCpu>(6, 0x1D, 0x1D),
    rule<IntelNehalemCpu>(6, 0x1A, 0x1A),
    rule<IntelNehalemCpu>(6, 0x1E, 0x1F),
    rule<IntelNehalemCpu>(6, 0x2E, 0x2E),
    rule<IntelWestmereCpu>(6, 0x25, 0x25),
    rule<IntelWestmereCpu>(6, 0x2C, 0x2C),
    rule<IntelWestmereCpu>(6, 0x2F, 0x2F),
    rule<IntelSandyBridgeCpu>(6, 0x2A, 0x2A),
    rule<IntelSandyBridgeCpu>(6, 0x2D, 0x2D),
    rule<IntelIvyBridgeCpu>(6, 0x3A, 0x3A),
    rule<IntelIvyBridgeCpu>(6, 0x3E, 0x3E),
    rule<IntelHaswellCpu>(6, 0x3C, 0x3C),
    rule<IntelHaswellCpu>(6, 0x3F, 0x3F),
    rule<IntelHaswellCpu>(6, 0x45, 0x46),
    rule<IntelBroadwellCpu>(6, 0x3D, 0x3D),
    rule<IntelBroadwellCpu>(6, 0x47, 0x47),
    rule<IntelBroadwellCpu>(6, 0x4F, 0x4F),
    rule<IntelBroadwellCpu>(6, 0x56, 0x56),
    rule<IntelSkylakeClientCpu>(6, 0x4E, 0x4E),
    rule<IntelSkylakeClientCpu>(6, 0x5E, 0x5E),
    // Model 0x55 is shared by the Xeon Scalable generations; stepping tells them apart.
    rule<IntelSkylakeServerCpu>(6, 0x55, 0x55, 0x0, 0x4),
    rule<IntelCascadeLakeCpu>(6, 0x55, 0x55, 0x5, 0x7),
    rule<IntelCooperLakeCpu>(6, 0x55, 0x55, 0xA, 0xB),
    rule<IntelIceLakeServerCpu>(6, 0x6A, 0x6A),
    rule<IntelIceLakeServerCpu>(6, 0x6C, 0x6C),
    // QEMU's Icelake-Server reports 0x86 stepping 0, which real Snow Ridge never does.
    rule<IntelIceLakeServerCpu>(6, 0x86, 0x86, 0x0, 0x0),
    rule<IntelSnowRidgeCpu>(6, 0x86, 0x86),
    rule<IntelDenvertonCpu>(6, 0x5F, 0x5F),
    rule<IntelKnightsMillCpu>(6, 0x85, 0x85),
    rule<IntelSapphireRapidsCpu>(6, 0x8F, 0x8F),
    rule<IntelGraniteRapidsCpu>(6, 0xAD, 0xAE),
    rule<IntelSierraForestCpu>(6, 0xAF, 0xAF),
    rule<IntelClearwaterForestCpu>(6, 0xDD, 0xDD),
    rule<IntelDiamondRapidsCpu>(19, 0x01, 0x01),
};
}  // namespace

ICentralProcessingUnit* create_intel_generation(void* storage, efi::SystemTable* st,
                                                const CpuSignature& sig) {
  return match_generation(kRules, sig, storage, st);
}

}  // namespace hardware
