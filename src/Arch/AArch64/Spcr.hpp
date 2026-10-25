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

// The debug UART the firmware describes in the ACPI SPCR table.
#pragma once
#include <stdint.h>
#include "Hardware/Acpi/AcpiTables.hpp"

namespace arch_aarch64 {

// MMIO base of a PL011-compatible serial port from the SPCR table, or 0 if the table is missing,
// names another UART type, or does not give a memory-mapped register block.
uint64_t spcr_uart_base(const hardware::AcpiTables& acpi);

}  // namespace arch_aarch64
