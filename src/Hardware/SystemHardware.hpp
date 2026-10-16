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

// Picks the hardware classes for the architecture this image is built for.
// SystemCpu picks Intel/AMD at runtime; SystemRam is the RAM class for the
// architecture. main.cpp never names a platform.
#pragma once

#if defined(__x86_64__)
#include "Hardware/CentralProcessingUnit/SystemCpu.hpp"
#include "Hardware/RandomAccessMemory/UefiRandomAccessMemory.hpp"

namespace hardware {
using SystemRam = UefiRandomAccessMemory;
}  // namespace hardware
#else
#error "No hardware classes for this architecture"
#endif
