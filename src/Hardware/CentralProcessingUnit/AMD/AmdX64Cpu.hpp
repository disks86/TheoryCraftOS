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
#include "Hardware/CentralProcessingUnit/X86_64Cpu.hpp"

namespace hardware {

// AMD x86_64 ("AuthenticAMD", also Hygon's AMD-derived parts). Current AMD
// CPUs are symmetric for scheduling purposes, so every core is Performance.
class AmdX64Cpu : public X86_64Cpu {
 public:
  explicit AmdX64Cpu(efi::SystemTable* st) : X86_64Cpu(st) {}
  const char* name() const override { return "AMD x64 CPU"; }
};

}  // namespace hardware
