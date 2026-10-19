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

// The processor's temperature, read from the chip's own sensor: Intel's digital thermal sensor (package reading when the part has one,
// else the core running the call) and AMD's (Zen and later) Tctl register behind the host bridge. A virtual machine has no sensor.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"

namespace hardware {

// The temperature in thousandths of a degree Celsius, or -1 if this machine cannot tell. `pci` (may be nullptr) is needed on AMD.
int32_t cpu_temperature_millicelsius(const IPciBus* pci);

}  // namespace hardware
