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

// The generic timer as the preemption tick of each core: the virtual timer (PPI 27) when running at EL1, the
// hypervisor physical timer (PPI 26) at EL2.
#pragma once
#include <stdint.h>

namespace kernel::gtimer {

// Counter frequency in Hz (CNTFRQ_EL0).
uint64_t frequency();
// The GIC interrupt id the tick arrives as.
uint32_t interrupt();
// Starts the periodic tick on this core, `hz` per second, and enables its interrupt in the GIC.
void start(uint32_t hz);
// Called from the tick handler: schedules the next tick (which also clears the interrupt).
void rearm();
void stop();

}  // namespace kernel::gtimer
