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

// The Thunderbolt host interfaces this driver knows. Only the Thunderbolt 3 controllers whose connection manager is the firmware on the controller (ICM: Alpine Ridge and Titan Ridge) are
// listed: the host does not build tunnels itself, it asks the firmware to. The two generations name devices differently (Alpine Ridge by port and depth, Titan Ridge by route string). The USB4
// controllers need other messages (or the host as connection manager) and are not listed - except Ice Lake's, whose firmware is the connection manager as well, speaking Titan Ridge's messages.
// (Tiger Lake and later run the connection manager in the host by default: that is a different, much larger piece of software, not here.)
#pragma once
#include <stdint.h>

namespace hardware {

// Alpine Ridge to Ice Lake have a connection manager in firmware (ThunderboltIcm); Usb4 is Tiger Lake and later, whose host router has none (Usb4Domain does its work).
enum class TbGeneration : uint8_t { AlpineRidge, TitanRidge, IceLake, Usb4 };

struct TbNhiDevice {
  uint16_t device_id;
  const char* name;
  TbGeneration generation;
};

// The entry for a PCI function (Intel, one of the NHI ids), or nullptr.
const TbNhiDevice* tb_find_nhi(uint16_t vendor_id, uint16_t device_id);

}  // namespace hardware
