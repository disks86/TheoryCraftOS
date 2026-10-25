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

// The Power State Coordination Interface: how the firmware starts the other cores.
#pragma once
#include <stdint.h>
#include "Hardware/Acpi/AcpiTables.hpp"

namespace arch_aarch64 {

enum class PsciConduit : uint8_t { None, Smc, Hvc };

// The conduit the FADT's ARM boot architecture flags name (None if the platform is not PSCI compliant).
PsciConduit psci_conduit(const hardware::AcpiTables& acpi);

// CPU_ON: starts the core with this MPIDR at `entry` (a physical address), passing `context` in x0. True on success.
bool psci_cpu_on(PsciConduit conduit, uint64_t mpidr, uint64_t entry, uint64_t context);

}  // namespace arch_aarch64
