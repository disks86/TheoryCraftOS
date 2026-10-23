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

#include "Hardware/CentralProcessingUnit/Intel/IntelGenerations.hpp"
#include "Hardware/CentralProcessingUnit/GenerationTable.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelAlderLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelArrowLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelBroadwellCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelCascadeLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelClearwaterForestCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelCoffeeLakeRefreshCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelCometLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelConroeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelCooperLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelDenvertonCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelDiamondRapidsCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelGracemontCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelGraniteRapidsCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelHaswellCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelIceLakeClientCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelIceLakeServerCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelIvyBridgeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelKnightsMillCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelLakefieldCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelLunarLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelMeteorLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelNehalemCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelNovaLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelPantherLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelPenrynCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelRaptorLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelRocketLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelSandyBridgeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelSapphireRapidsCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelSierraForestCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelSkylakeClientCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelSkylakeServerCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelSnowRidgeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelTigerLakeCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelTremontCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelWestmereCpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelWildcatLakeCpu.hpp"

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
    // Client parts from Skylake's successors on. Steppings separate the 8th, 9th and 10th generations that share a model.
    rule<IntelCoffeeLakeRefreshCpu>(6, 0x9E, 0x9E, 0xC, 0xD),
    rule<IntelCometLakeCpu>(6, 0x8E, 0x8E, 0xC, 0xD),
    rule<IntelCometLakeCpu>(6, 0xA5, 0xA6),
    rule<IntelIceLakeClientCpu>(6, 0x7D, 0x7E),
    rule<IntelLakefieldCpu>(6, 0x8A, 0x8A),
    rule<IntelTremontCpu>(6, 0x96, 0x96),
    rule<IntelTremontCpu>(6, 0x9C, 0x9C),
    rule<IntelTigerLakeCpu>(6, 0x8C, 0x8D),
    rule<IntelRocketLakeCpu>(6, 0xA7, 0xA7),
    rule<IntelAlderLakeCpu>(6, 0x97, 0x97),
    rule<IntelAlderLakeCpu>(6, 0x9A, 0x9A),
    rule<IntelGracemontCpu>(6, 0xBE, 0xBE),
    rule<IntelRaptorLakeCpu>(6, 0xB7, 0xB7),
    rule<IntelRaptorLakeCpu>(6, 0xBA, 0xBA),
    rule<IntelRaptorLakeCpu>(6, 0xBF, 0xBF),
    rule<IntelMeteorLakeCpu>(6, 0xAA, 0xAA),
    rule<IntelMeteorLakeCpu>(6, 0xAC, 0xAC),
    rule<IntelArrowLakeCpu>(6, 0xB5, 0xB5),
    rule<IntelArrowLakeCpu>(6, 0xC5, 0xC6),
    rule<IntelLunarLakeCpu>(6, 0xBD, 0xBD),
    rule<IntelPantherLakeCpu>(6, 0xCC, 0xCC),
    rule<IntelWildcatLakeCpu>(6, 0xD5, 0xD5),
    rule<IntelNovaLakeCpu>(18, 0x01, 0x01),
    rule<IntelNovaLakeCpu>(18, 0x03, 0x03),
};
}  // namespace

ICentralProcessingUnit* create_intel_generation(void* storage, efi::SystemTable* st,
                                                const CpuSignature& sig) {
  return match_generation(kRules, sig, storage, st);
}

}  // namespace hardware
