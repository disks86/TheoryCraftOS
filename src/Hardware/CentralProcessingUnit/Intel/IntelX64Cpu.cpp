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

#include "Hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"
#include "Hardware/CentralProcessingUnit/Cpuid.hpp"

namespace hardware {

CoreKind IntelX64Cpu::probe_core_kind() const {
  constexpr uint32_t kHybridBit = 1u << 15;  // CPUID.7.0:EDX
  constexpr uint32_t kAtom = 0x20, kCore = 0x40;
  if (cpuid(0).eax < 0x1A || !(cpuid(7).edx & kHybridBit)) return CoreKind::Performance;
  switch (cpuid(0x1A).eax >> 24) {  // native model ID: core type
    case kAtom: return CoreKind::Efficiency;
    case kCore: return CoreKind::Performance;
    default:    return CoreKind::Performance;
  }
}

}  // namespace hardware
