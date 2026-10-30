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
#include "Hardware/Acpi/AmlInterpreter.hpp"

namespace hardware {

// The machine as AML sees it: operation regions in memory, I/O ports, PCI configuration space and the embedded controller. The embedded controller's
// ports are found in the tables (its _CRS), so whoever reads the tables tells the host where they are.
class IAcpiHost : public acpi::IAmlHost {
 public:
  virtual void configure_ec(uint16_t data_port, uint16_t command_port) = 0;
  // The controller's status byte and its event query (see EmbeddedController); false where there is no controller.
  virtual bool ec_status(uint8_t* value) { (void)value; return false; }
  virtual bool ec_query(uint8_t* event) { (void)event; return false; }

 protected:
  constexpr IAcpiHost() = default;
  ~IAcpiHost() = default;
};

}  // namespace hardware
