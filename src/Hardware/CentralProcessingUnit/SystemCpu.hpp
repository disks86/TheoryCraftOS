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

// Owns the CPU class matching the machine we booted on. The class is chosen
// from the CPUID vendor and family/model/stepping (a microarchitecture such
// as IntelSkylakeClientCpu or AmdZen3Cpu), falling back to the vendor class,
// or to QemuX64Cpu for QEMU's generic models. Storage is inline because there
// is no heap yet; every CPU class adds no data over X86_64Cpu.
class SystemCpu {
 public:
  explicit SystemCpu(efi::SystemTable* st);

  ICentralProcessingUnit& cpu() { return *active_; }

 private:
  alignas(16) unsigned char storage_[sizeof(X86_64Cpu)];
  ICentralProcessingUnit* active_;
};

}  // namespace hardware
