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

namespace hardware {

// PCI Express: PCI plus 4 KiB of config space per function and the extended
// capability list that lives in the part above 256 bytes.
class IPcieBus : public IPciBus {
 public:
  // Offset (>= 0x100) of the extended capability with this ID, or 0.
  virtual uint16_t find_extended_capability(PciAddress address, uint16_t id) const = 0;

 protected:
  constexpr IPcieBus() = default;
  ~IPcieBus() = default;
};

}  // namespace hardware
