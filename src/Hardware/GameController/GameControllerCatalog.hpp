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

// The game controllers people actually plug in, by USB vendor and product id: which protocol each speaks and what it is called. A pad that is
// not listed is still found by its interface (the Xbox protocols have interface classes of their own) or, if it is a HID joystick or gamepad,
// decoded from its report descriptor; the list only adds the name, and the protocol for the HID pads that need their own (Sony, Nintendo).
// For Xbox pads the interface decides the protocol (a pad's interface is what it really speaks); the list's protocol there is for reference.
#pragma once
#include "Hardware/GameController/GamepadTypes.hpp"

namespace hardware {
namespace gamepad {

struct CatalogEntry {
  uint16_t vendor_id, product_id;
  Protocol protocol;
  const char* name;
};

// The entry for a device, or null.
const CatalogEntry* find_controller(uint16_t vendor_id, uint16_t product_id);
// The Xbox protocol a vendor-specific interface speaks (class 0xFF with Microsoft's subclass and protocol), or Protocol::None.
Protocol protocol_for_interface(uint8_t interface_class, uint8_t subclass, uint8_t protocol);
// What a controller of `protocol` can do (Capability bits) apart from what its report shows.
uint32_t protocol_capabilities(Protocol protocol);
// How many entries the catalogue has (for tests and the device list).
uint32_t catalog_size();
const CatalogEntry& catalog_entry(uint32_t index);

}  // namespace gamepad
}  // namespace hardware
