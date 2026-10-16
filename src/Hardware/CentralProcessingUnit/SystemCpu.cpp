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

#include "Hardware/CentralProcessingUnit/SystemCpu.hpp"
#include <new>
#include "Hardware/CentralProcessingUnit/AMD/AmdGenerations.hpp"
#include "Hardware/CentralProcessingUnit/AMD/AmdX64Cpu.hpp"
#include "Hardware/CentralProcessingUnit/Cpuid.hpp"
#include "Hardware/CentralProcessingUnit/Hygon/HygonGenerations.hpp"
#include "Hardware/CentralProcessingUnit/Hygon/HygonX64Cpu.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelGenerations.hpp"
#include "Hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"
#include "Hardware/CentralProcessingUnit/QEMU/QemuX64Cpu.hpp"
#include "Hardware/CentralProcessingUnit/Zhaoxin/ZhaoxinGenerations.hpp"
#include "Hardware/CentralProcessingUnit/Zhaoxin/ZhaoxinX64Cpu.hpp"

namespace hardware {

namespace {
static_assert(sizeof(IntelX64Cpu) == sizeof(X86_64Cpu) && sizeof(AmdX64Cpu) == sizeof(X86_64Cpu) &&
                  sizeof(HygonX64Cpu) == sizeof(X86_64Cpu) &&
                  sizeof(ZhaoxinX64Cpu) == sizeof(X86_64Cpu) &&
                  sizeof(QemuX64Cpu) == sizeof(X86_64Cpu),
              "CPU classes must fit SystemCpu's storage");

enum class Vendor { Unknown, Intel, Amd, Hygon, Zhaoxin };

bool vendor_is(const CpuidResult& r, const char (&text)[13]) {
  // CPUID.0 returns the 12-byte vendor in EBX, EDX, ECX order.
  uint32_t want[3];
  __builtin_memcpy(want, text, 12);
  return r.ebx == want[0] && r.edx == want[1] && r.ecx == want[2];
}

Vendor detect_vendor() {
  CpuidResult id = cpuid(0);
  if (vendor_is(id, "GenuineIntel")) return Vendor::Intel;
  if (vendor_is(id, "AuthenticAMD")) return Vendor::Amd;
  if (vendor_is(id, "HygonGenuine")) return Vendor::Hygon;
  if (vendor_is(id, "  Shanghai  ") || vendor_is(id, "CentaurHauls")) return Vendor::Zhaoxin;
  return Vendor::Unknown;
}

// QEMU without hardware acceleration announces itself in the hypervisor leaf.
// (Under KVM the signature is "KVMKVMKVM", and the CPU is the real one.)
bool is_qemu_tcg() {
  constexpr uint32_t kHypervisorBit = 1u << 31;  // CPUID.1:ECX
  if (!(cpuid(1).ecx & kHypervisorBit)) return false;
  CpuidResult h = cpuid(0x40000000);
  uint32_t want[3];
  __builtin_memcpy(want, "TCGTCGTCGTCG", 12);
  return h.ebx == want[0] && h.ecx == want[1] && h.edx == want[2];
}
}  // namespace

SystemCpu::SystemCpu(efi::SystemTable* st) {
  const Vendor vendor = detect_vendor();
  const CpuSignature sig = cpu_signature();

  // 1. A known microarchitecture.
  active_ = nullptr;
  switch (vendor) {
    case Vendor::Intel:   active_ = create_intel_generation(storage_, st, sig); break;
    case Vendor::Amd:     active_ = create_amd_generation(storage_, st, sig); break;
    case Vendor::Hygon:   active_ = create_hygon_generation(storage_, st, sig); break;
    case Vendor::Zhaoxin: active_ = create_zhaoxin_generation(storage_, st, sig); break;
    case Vendor::Unknown: break;
  }
  if (active_) return;

  // 2. QEMU's generic models (qemu64 and friends).
  if (is_qemu_tcg()) {
    active_ = new (storage_) QemuX64Cpu(st);
    return;
  }

  // 3. A vendor we recognize but not this model.
  switch (vendor) {
    case Vendor::Intel:   active_ = new (storage_) IntelX64Cpu(st); break;
    case Vendor::Amd:     active_ = new (storage_) AmdX64Cpu(st); break;
    case Vendor::Hygon:   active_ = new (storage_) HygonX64Cpu(st); break;
    case Vendor::Zhaoxin: active_ = new (storage_) ZhaoxinX64Cpu(st); break;
    case Vendor::Unknown: active_ = new (storage_) X86_64Cpu(st); break;
  }
}

}  // namespace hardware
