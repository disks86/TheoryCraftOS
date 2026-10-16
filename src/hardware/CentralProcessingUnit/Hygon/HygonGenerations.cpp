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
