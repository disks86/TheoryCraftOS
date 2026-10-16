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

#include "Hardware/CentralProcessingUnit/Zhaoxin/ZhaoxinGenerations.hpp"
#include "Hardware/CentralProcessingUnit/GenerationTable.hpp"
#include "Hardware/CentralProcessingUnit/Zhaoxin/ZhaoxinYongFengCpu.hpp"

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
