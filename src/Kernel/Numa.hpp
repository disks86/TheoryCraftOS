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

// NUMA from the ACPI tables: the SRAT puts each core in its node (by APIC id, x2APIC id, or, on ARM, the ACPI processor UID that the MADT's
// GICC entries turn into an MPIDR), and the SLIT gives the distances between the nodes. Call once kernel::start() has the cores online.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace kernel {

struct NumaSummary {
  uint32_t cores = 0;       // cores put in a node
  uint32_t nodes = 0;       // highest node + 1 (0 when no core was placed)
  uint32_t distances = 0;   // node pairs given a distance
};

NumaSummary apply_acpi_numa(ConstByteView srat, ConstByteView slit, ConstByteView madt);

}  // namespace kernel
