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

// The supervisor timer as the preemption tick of each hart: the time CSR counts, SBI set_timer arms the next
// supervisor timer interrupt.
#pragma once
#include <stdint.h>

namespace kernel::stimer {

// True if the firmware can arm the timer (SBI time extension or the legacy call).
bool available();
// Counter frequency in Hz (the platform's timebase). Set from the ACPI RHCT table; 0 = unknown.
void set_frequency(uint64_t hz);
uint64_t frequency();
// Starts the periodic tick on this hart, `hz` per second.
void start(uint32_t hz);
// Called from the tick handler: schedules the next tick (which also clears the interrupt).
void rearm();
void stop();

}  // namespace kernel::stimer
