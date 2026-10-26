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

#include "Hardware/Platform/RaspberryPi/RaspberryPiDetect.hpp"

namespace hardware {

bool acpi_says_raspberry_pi(const AcpiTables& acpi) {
  const ConstByteView fadt = acpi.find("FACP");
  AcpiTableHeader header;
  if (fadt.size() < sizeof(header)) return false;
  for (uint64_t i = 0; i < sizeof(header); ++i) reinterpret_cast<uint8_t*>(&header)[i] = fadt[i];  // the header is packed plain data: copy it out byte by byte
  return header.oem_id[0] == 'R' && header.oem_id[1] == 'P' && header.oem_id[2] == 'I';
}

}  // namespace hardware
