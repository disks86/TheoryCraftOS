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
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "uefi.hpp"

namespace hardware {

// Asks the firmware to disconnect its driver(s) from a PCI function, so the OS
// can program the device without the firmware's driver (e.g. its USB stack)
// changing things underneath it. Returns true when no firmware driver is left
// bound to the function (including when none ever was).
bool detach_firmware_drivers(efi::SystemTable* st, PciAddress address);

// Lets the firmware bind its drivers to the function again (e.g. so its USB
// keyboard works for firmware console input once the OS is done with the
// controller). The device must be idle and reset.
bool reattach_firmware_drivers(efi::SystemTable* st, PciAddress address);

}  // namespace hardware
