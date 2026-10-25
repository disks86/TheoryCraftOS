/*
 * TheoryCraftOS - a minimal UEFI operating system.
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

#include "Arch/AArch64/Psci.hpp"

namespace arch_aarch64 {

namespace {
constexpr uint64_t kCpuOn64 = 0xC4000003;
constexpr uint32_t kFadtBootFlagsOffset = 129;   // ARM Boot Architecture Flags (FADT 5.1 and later)
constexpr uint16_t kPsciCompliant = 1u << 0, kPsciUseHvc = 1u << 1;

int64_t call(PsciConduit conduit, uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3) {
  register uint64_t r0 __asm__("x0") = function;
  register uint64_t r1 __asm__("x1") = a1;
  register uint64_t r2 __asm__("x2") = a2;
  register uint64_t r3 __asm__("x3") = a3;
  // SMCCC lets the callee use x4-x17.
  #define TCOS_PSCI_CLOBBERS "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory"
  if (conduit == PsciConduit::Hvc) __asm__ volatile("hvc #0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)::TCOS_PSCI_CLOBBERS);
  else __asm__ volatile("smc #0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)::TCOS_PSCI_CLOBBERS);
  #undef TCOS_PSCI_CLOBBERS
  return static_cast<int64_t>(r0);
}
}  // namespace

PsciConduit psci_conduit(const hardware::AcpiTables& acpi) {
  const ConstByteView fadt = acpi.find("FACP");
  if (fadt.size() < kFadtBootFlagsOffset + 2) return PsciConduit::None;
  const uint16_t flags = static_cast<uint16_t>(fadt[kFadtBootFlagsOffset] | (fadt[kFadtBootFlagsOffset + 1] << 8));
  if (!(flags & kPsciCompliant)) return PsciConduit::None;
  return (flags & kPsciUseHvc) ? PsciConduit::Hvc : PsciConduit::Smc;
}

bool psci_cpu_on(PsciConduit conduit, uint64_t mpidr, uint64_t entry, uint64_t context) {
  if (conduit == PsciConduit::None) return false;
  return call(conduit, kCpuOn64, mpidr, entry, context) == 0;
}

}  // namespace arch_aarch64
