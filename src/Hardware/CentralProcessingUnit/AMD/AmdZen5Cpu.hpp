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
#include "Hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"

namespace hardware {

// Ryzen 9000 / EPYC Turin. QEMU: EPYC-Turin.
class AmdZen5Cpu final : public AmdX64Cpu {
 public:
  explicit AmdZen5Cpu(efi::SystemTable* st) : AmdX64Cpu(st) {}
  const char* name() const override { return "AMD Zen 5"; }
};

}  // namespace hardware
