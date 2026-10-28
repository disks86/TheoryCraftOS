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
#include "Utility/ArrayView.hpp"

namespace hardware {

// The connectors of an AMD card, from the object table of its video BIOS: what the card's outputs are (HDMI, DisplayPort, ...) and which hot plug detect pin each is on, so that a hot
// plug event on a pin can say what was plugged in. A port of the connector part of the Linux amdgpu driver's amdgpu_atombios_get_connector_info_from_object_table (atombios.h and
// ObjectID.h for the layouts, amdgpu_atombios_lookup_gpio and _get_hpd_info_from_gpio for the pin; Copyright Advanced Micro Devices, Inc.; MIT license). The display path table lists, for
// each device the board supports (a bit of the header's device vector), the connector it ends in; the connector's object has records, one of which names the GPIO of its hot plug pin;
// the GPIO_Pin_LUT data table says which register bit that GPIO is, and the DC_GPIO_HPD_A bit is the pin.
//
// What differs from the driver, because the image is untrusted bytes: every read is checked (a table that runs past the image ends the walk and what was found so far is kept), the
// number of paths and records walked is bounded, and the part that is not about naming is left out: the encoders (and their capabilities), the routers and the DDC lines.
// Connectors that more than one path ends in are one connector with the devices merged, as amdgpu_connector_add does (the first path's pin is kept unless it has none).
class AtomConnectors {
 public:
  enum class Type : uint8_t { DviI, DviD, Vga, Composite, SVideo, DIN, HdmiA, HdmiB, Lvds, DisplayPort, EmbeddedDisplayPort };

  static constexpr uint8_t kNoPin = 0xFF;
  static constexpr uint32_t kMaxConnectors = 16;

  struct Connector {
    Type type = Type::DviI;
    uint8_t number = 1;               // 1 for the first connector of this type, 2 for the second, ...: the "1" of "HDMI-A-1"
    uint8_t object_id = 0;            // the connector object's id (CONNECTOR_OBJECT_ID_*)
    uint8_t enum_id = 0;              // the enumeration of the object id: which of several of the same object
    uint8_t hpd_pin = kNoPin;         // the hot plug detect pin, 0 to 5, or kNoPin
    uint8_t plugged_state = 0;        // ATOM_HPD_INT_RECORD's ucPlugged_PinState: the level of the GPIO when something is plugged in
    uint16_t devices = 0;             // the ATOM_DEVICE_*_SUPPORT bits of the devices that end in this connector
  };

  // The connector's name as the kernel's DRM names it: "HDMI-A-1", "DP-2", "eDP-1". `buffer` gets the text and a terminating zero, cut short if it does not fit.
  // HDMI and DisplayPort carry audio with the picture (the others the connector reader knows - analogue, DVI, LVDS, eDP - do not, in the sense of the audio endpoints being used for them).
  static bool carries_audio(Type type) { return type == Type::HdmiA || type == Type::HdmiB || type == Type::DisplayPort; }
  static const char* type_name(Type type);
  static uint32_t name(const Connector& connector, ArrayView<char> buffer);

  // Reads the connectors of the video BIOS image into `out` (at most out.size(), and kMaxConnectors are looked for) and returns how many. 0 if the image is no AtomBIOS image, has no
  // object table, or the table is older than revision 2.
  static uint32_t parse(ConstByteView bios, ArrayView<Connector> out);

  // The connector on a hot plug pin, or null.
  static const Connector* find_pin(ArrayView<const Connector> connectors, uint32_t pin);
};

}  // namespace hardware
