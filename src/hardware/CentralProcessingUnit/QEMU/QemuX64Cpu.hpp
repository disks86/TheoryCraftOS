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

// A CPU emulated by QEMU's TCG (no hardware passthrough). It borrows an
// Intel or AMD vendor string, so it is recognized by the hypervisor leaf
// instead. Cores are uniform. With KVM the real vendor class is used.
class QemuX64Cpu final : public X86_64Cpu {
 public:
  explicit QemuX64Cpu(efi::SystemTable* st) : X86_64Cpu(st) {}
  const char* name() const override { return "QEMU x64 CPU"; }
};

}  // namespace hardware
