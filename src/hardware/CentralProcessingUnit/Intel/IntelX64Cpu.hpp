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

#pragma once
#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"

namespace hardware {

// Intel x86_64 ("GenuineIntel"). Hybrid parts (Alder Lake and later) mix
// P-cores and E-cores; each core reports its type in CPUID leaf 0x1A.
class IntelX64Cpu : public X86_64Cpu {
 public:
  explicit IntelX64Cpu(efi::SystemTable* st) : X86_64Cpu(st) {}
  const char* name() const override { return "Intel x64 CPU"; }

 protected:
  CoreKind probe_core_kind() const override;
};

}  // namespace hardware
