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
#include "hardware/Bus/Usb/IUsbHostController.hpp"

namespace hardware {

// An xHCI (USB 3.x) host controller: the generic controller interface plus the
// capabilities xHCI reports about itself.
class IXhciController : public IUsbHostController {
 public:
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }

  virtual uint16_t interface_version() const = 0;  // BCD, e.g. 0x0100 = xHCI 1.0
  virtual uint32_t max_device_slots() const = 0;
  virtual bool uses_64_byte_contexts() const = 0;

 protected:
  constexpr IXhciController() = default;
  ~IXhciController() = default;
};

}  // namespace hardware
