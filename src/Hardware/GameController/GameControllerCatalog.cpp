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

#include "Hardware/GameController/GameControllerCatalog.hpp"
#include "Hardware/GameController/XboxProtocol.hpp"

namespace hardware {
namespace gamepad {

namespace {
constexpr CatalogEntry kCatalog[] = {
    // Microsoft
    {0x045E, 0x028E, Protocol::Xbox360, "Xbox 360 Controller"},
    {0x045E, 0x028F, Protocol::Xbox360, "Xbox 360 Wireless Controller (cable)"},
    {0x045E, 0x02D1, Protocol::XboxOne, "Xbox One Controller"},
    {0x045E, 0x02DD, Protocol::XboxOne, "Xbox One Controller"},
    {0x045E, 0x02E3, Protocol::XboxOne, "Xbox One Elite Controller"},
    {0x045E, 0x02EA, Protocol::XboxOne, "Xbox One S Controller"},
    {0x045E, 0x0B00, Protocol::XboxOne, "Xbox Elite Series 2 Controller"},
    {0x045E, 0x0B0A, Protocol::XboxOne, "Xbox Adaptive Controller"},
    {0x045E, 0x0B12, Protocol::XboxOne, "Xbox Series X|S Controller"},
    // Sony
    {0x054C, 0x05C4, Protocol::DualShock4, "DualShock 4"},
    {0x054C, 0x09CC, Protocol::DualShock4, "DualShock 4 (second generation)"},
    {0x054C, 0x0BA0, Protocol::DualShock4, "DualShock 4 USB Wireless Adaptor"},
    {0x054C, 0x0CE6, Protocol::DualSense, "DualSense Wireless Controller"},
    {0x054C, 0x0DF2, Protocol::DualSense, "DualSense Edge Wireless Controller"},
    // Nintendo
    {0x057E, 0x2009, Protocol::SwitchPro, "Nintendo Switch Pro Controller"},
    // Logitech
    {0x046D, 0xC21D, Protocol::Xbox360, "Logitech Gamepad F310"},
    {0x046D, 0xC21E, Protocol::Xbox360, "Logitech Gamepad F510"},
    {0x046D, 0xC21F, Protocol::Xbox360, "Logitech Wireless Gamepad F710"},
    {0x046D, 0xC216, Protocol::Hid, "Logitech Dual Action"},
    {0x046D, 0xC218, Protocol::Hid, "Logitech RumblePad 2"},
    // PowerA, PDP, Hori, Razer, Turtle Beach, Hyperkin
    {0x20D6, 0x2001, Protocol::XboxOne, "PowerA Xbox Series X Controller"},
    {0x20D6, 0xA711, Protocol::Hid, "PowerA Wired Controller for Nintendo Switch"},
    {0x0E6F, 0x02A0, Protocol::XboxOne, "PDP Xbox One Controller"},
    {0x0E6F, 0x02A4, Protocol::XboxOne, "PDP Wired Controller for Xbox One"},
    {0x0F0D, 0x0067, Protocol::XboxOne, "Hori Pad One"},
    {0x0F0D, 0x0092, Protocol::Hid, "Hori Pokken Tournament Pro Pad"},
    {0x1532, 0x0A00, Protocol::XboxOne, "Razer Atrox Arcade Stick"},
    {0x1532, 0x0A03, Protocol::XboxOne, "Razer Wildcat"},
    {0x10F5, 0x7005, Protocol::XboxOne, "Turtle Beach Recon Controller"},
    {0x2E24, 0x0652, Protocol::XboxOne, "Hyperkin Duke"},
    // Generic HID pads common on PCs
    {0x0079, 0x0006, Protocol::Hid, "DragonRise USB Gamepad"},
    {0x0810, 0x0001, Protocol::Hid, "Twin USB Joystick"},
    {0x044F, 0xB10A, Protocol::Hid, "Thrustmaster T.16000M"},
    {0x046D, 0xC215, Protocol::Hid, "Logitech Extreme 3D Pro"},
};
}  // namespace

const char* protocol_name(Protocol protocol) {
  switch (protocol) {
    case Protocol::Xbox360: return "Xbox 360 (XInput)";
    case Protocol::XboxOne: return "Xbox One (GIP)";
    case Protocol::DualShock4: return "DualShock 4";
    case Protocol::DualSense: return "DualSense";
    case Protocol::SwitchPro: return "Switch Pro";
    case Protocol::Hid: return "HID gamepad";
    case Protocol::None: break;
  }
  return "none";
}

const CatalogEntry* find_controller(uint16_t vendor_id, uint16_t product_id) {
  for (const CatalogEntry& e : kCatalog)
    if (e.vendor_id == vendor_id && e.product_id == product_id) return &e;
  return nullptr;
}

Protocol protocol_for_interface(uint8_t interface_class, uint8_t subclass, uint8_t protocol) {
  if (interface_class != xbox::kInterfaceClass) return Protocol::None;
  if (subclass == xbox::kXbox360Subclass && protocol == xbox::kXbox360Protocol) return Protocol::Xbox360;
  if (subclass == xbox::kGipSubclass && protocol == xbox::kGipProtocol) return Protocol::XboxOne;
  return Protocol::None;
}

uint32_t protocol_capabilities(Protocol protocol) {
  switch (protocol) {
    case Protocol::Xbox360: return kCapRumble | kCapPlayerLeds | kCapTriggers | kCapRightStick;
    case Protocol::XboxOne: return kCapRumble | kCapTriggers | kCapRightStick;
    case Protocol::DualShock4: return kCapRumble | kCapLightBar | kCapBattery | kCapTriggers | kCapRightStick;
    case Protocol::DualSense: return kCapRumble | kCapLightBar | kCapPlayerLeds | kCapBattery | kCapTriggers | kCapRightStick;
    case Protocol::SwitchPro: return kCapPlayerLeds | kCapBattery | kCapTriggers | kCapRightStick;
    case Protocol::Hid:
    case Protocol::None: break;
  }
  return 0;
}

uint32_t catalog_size() { return sizeof(kCatalog) / sizeof(kCatalog[0]); }
const CatalogEntry& catalog_entry(uint32_t index) { return kCatalog[index < catalog_size() ? index : 0]; }

}  // namespace gamepad
}  // namespace hardware
