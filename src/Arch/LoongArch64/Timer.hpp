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

// The per-core timer as the preemption tick of each core: the stable counter counts, CSR.TCFG arms a periodic timer
// interrupt (ESTAT.IS bit 11) that CSR.TICLR acknowledges.
#pragma once
#include <stdint.h>

namespace kernel::stimer {

// Always true: every LoongArch core has the timer.
bool available();
// Counter frequency in Hz. Set from firmware data if known; otherwise CPUCFG 4 and 5 say.
void set_frequency(uint64_t hz);
uint64_t frequency();
// Starts the periodic tick on this core, `hz` per second.
void start(uint32_t hz);
// Called from the tick handler: clears the interrupt (the timer is periodic and reloads itself).
void rearm();
void stop();

}  // namespace kernel::stimer
