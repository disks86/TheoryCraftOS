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

// Thin wrapper over the CPUID instruction (private to CentralProcessingUnit/).
#pragma once
#include <cpuid.h>
#include <stdint.h>

namespace hardware {

struct CpuidResult {
  uint32_t eax, ebx, ecx, edx;
};

inline CpuidResult cpuid(uint32_t leaf, uint32_t subleaf = 0) {
  CpuidResult r{};
  __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
  return r;
}

// Family/model/stepping as the vendors define them (CPUID.1:EAX with the
// extended fields folded in).
struct CpuSignature {
  uint32_t family, model, stepping;
};

inline CpuSignature cpu_signature() {
  uint32_t eax = cpuid(1).eax;
  uint32_t family = (eax >> 8) & 0xF;
  uint32_t model = (eax >> 4) & 0xF;
  if (family == 0xF) family += (eax >> 20) & 0xFF;
  if (family == 0x6 || family >= 0xF) model |= ((eax >> 16) & 0xF) << 4;
  return {family, model, eax & 0xF};
}

}  // namespace hardware
