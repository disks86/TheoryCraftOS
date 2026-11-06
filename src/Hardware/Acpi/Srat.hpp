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

// The ACPI System Resource Affinity Table (SRAT, ACPI 6.5 section 5.2.16): which proximity domain (NUMA node) each processor and each range of
// memory belongs to, and the System Locality Information Table (SLIT, 5.2.17): the relative distance between two domains. The thread scheduler
// uses the processor nodes to keep a thread's work on the node of the device and the cache it came from.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

enum class SratCpuId : uint8_t {
  LocalApic,   // `id` is the 8-bit local APIC id (entry type 0)
  X2Apic,      // the 32-bit x2APIC id (type 2)
  Gicc,        // the ACPI processor UID of an ARM GIC CPU interface (type 3)
};

struct SratCpu {
  uint32_t node = 0;
  uint32_t id = 0;
  SratCpuId kind = SratCpuId::LocalApic;
};

struct SratMemory {
  uint32_t node = 0;
  uint64_t base = 0;
  uint64_t length = 0;
  bool hot_pluggable = false;
  bool non_volatile = false;
};

// The enabled processors of the table, into `out` as far as it goes. Returns how many there are (more than out.size() when it was too small).
// 0 for anything that is not an SRAT; an entry that does not fit the table ends the list, and an entry shorter than its type is skipped.
uint32_t srat_cpus(ConstByteView srat, ArrayView<SratCpu> out);
// The enabled memory ranges, the same way.
uint32_t srat_memory(ConstByteView srat, ArrayView<SratMemory> out);
// The MPIDR affinity (Aff3..Aff0, the other bits cleared) of the enabled GIC CPU interface whose ACPI processor UID is `uid`, from the MADT:
// what an SRAT GICC entry names a core by. False if the MADT has no such entry.
bool madt_gicc_mpidr(ConstByteView madt, uint32_t uid, uint64_t* mpidr);
// The distance from `from` to `to` in the SLIT (10 is local; 255 unreachable), or 0 if the table is not a SLIT or does not cover both.
uint8_t slit_distance(ConstByteView slit, uint32_t from, uint32_t to);

}  // namespace hardware
