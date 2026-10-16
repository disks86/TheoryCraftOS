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
#include <stdint.h>

namespace kernel::apic {

// The local APIC of each core: the timer that drives preemption and the
// inter-processor interrupts that wake idle cores. (xAPIC registers in memory, or
// x2APIC through MSRs if the firmware switched to it.)

// Boot core: finds the APIC, calibrates its timer against the timestamp counter
// (needs hardware::delay_init()). False if there is no usable APIC.
bool init_boot_core();
// Every core, once its interrupt tables are loaded: enables the APIC.
void enable_this_core();
// This core's APIC id (read from CPUID, valid before enable_this_core()).
uint32_t this_apic_id();
// Starts the periodic preemption tick on this core (kVectorTimer), `hz` per second.
void start_timer(uint32_t hz);
void stop_timer();
void eoi();
// Fixed-delivery IPI to the core with the given APIC id.
void send_ipi(uint32_t apic_id, uint8_t vector);
// Acknowledges an interrupt nobody handles (also the legacy PIC for its vector range).
void acknowledge_stray(uint8_t vector);

}  // namespace kernel::apic
