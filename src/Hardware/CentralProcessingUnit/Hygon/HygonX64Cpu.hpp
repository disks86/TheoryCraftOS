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

// Hygon x86_64 ("HygonGenuine"): AMD Zen derivatives sold in China. Symmetric cores.
class HygonX64Cpu : public X86_64Cpu {
 public:
  explicit HygonX64Cpu(efi::SystemTable* st) : X86_64Cpu(st) {}
  const char* name() const override { return "Hygon x64 CPU"; }
};

}  // namespace hardware
