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

#include "Kernel/Numa.hpp"
#include "Hardware/Acpi/Srat.hpp"
#include "Kernel/Thread.hpp"

namespace kernel {

NumaSummary apply_acpi_numa(ConstByteView srat, ConstByteView slit, ConstByteView madt) {
  NumaSummary summary;
  hardware::SratCpu cpus[256];
  const uint32_t listed = hardware::srat_cpus(srat, ArrayView<hardware::SratCpu>(cpus, 256));
  uint32_t highest = 0;
  for (const hardware::SratCpu& cpu : ArrayView<const hardware::SratCpu>(cpus, listed < 256 ? listed : 256)) {
    uint64_t hardware_id = cpu.id;
    if (cpu.kind == hardware::SratCpuId::Gicc && !hardware::madt_gicc_mpidr(madt, cpu.id, &hardware_id)) continue;
    const int32_t core = core_from_hardware_id(hardware_id);
    if (core < 0 || !set_core_numa_node(static_cast<uint32_t>(core), cpu.node)) continue;
    ++summary.cores;
    if (cpu.node > highest) highest = cpu.node;
  }
  if (!summary.cores) return summary;
  summary.nodes = highest + 1;
  for (uint32_t from = 0; from <= highest && from < scheduling::NumaDistances::kMaxNodes; ++from)
    for (uint32_t to = 0; to <= highest && to < scheduling::NumaDistances::kMaxNodes; ++to)
      if (const uint8_t d = hardware::slit_distance(slit, from, to); d && set_numa_distance(from, to, d)) ++summary.distances;
  return summary;
}

}  // namespace kernel
