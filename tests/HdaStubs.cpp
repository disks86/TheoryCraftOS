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

// Stubs for the parts of the kernel the High Definition Audio driver reaches when it is built into a host test: the delays (the simulated controller answers at once), the debug console and
// the firmware driver detach (not used: the test's controller is not behind the PCI code).
#include <stdint.h>
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Kernel/Delay.hpp"
#include "console.hpp"

namespace hardware {
void delay_us(uint64_t) {}
bool detach_firmware_drivers(efi::SystemTable*, PciAddress) { return true; }
bool reattach_firmware_drivers(efi::SystemTable*, PciAddress) { return true; }
}  // namespace hardware

namespace console {
void debug_puts(StringView) {}
}  // namespace console
