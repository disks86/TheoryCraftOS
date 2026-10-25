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

// The ARM Generic Interrupt Controller, GICv2 or GICv3/v4 (found through the ACPI MADT).
#pragma once
#include <stdint.h>
#include "Hardware/Acpi/AcpiTables.hpp"

namespace kernel::gic {

// Boot core: reads the MADT, picks the GIC version and brings the distributor and this core's interface up.
// Remembers the firmware's enable state of the private interrupts so restore_firmware() can put it back.
bool init_boot_core(const hardware::AcpiTables& acpi);
// Brings this core's interface up (every core, after it has its exception vectors).
void enable_this_core();
// Boot core, on shutdown of the scheduler: disables what we enabled, restores the firmware's private interrupt enables.
void restore_firmware();

// 2 or 3 (0 before init).
uint32_t version();

// Enables / disables one interrupt for this core (a PPI) or the system (an SPI), at a mid priority.
void enable_interrupt(uint32_t intid);
void disable_interrupt(uint32_t intid);

// Reads the pending interrupt id and acknowledges it; 1020 or more means spurious.
uint32_t acknowledge();
void end_of_interrupt(uint32_t intid);

// Software-generated interrupt (inter-processor) to the core with this MPIDR affinity value (MPIDR_EL1 & 0xFF00FFFFFF).
void send_sgi(uint64_t mpidr, uint32_t intid);

}  // namespace kernel::gic
