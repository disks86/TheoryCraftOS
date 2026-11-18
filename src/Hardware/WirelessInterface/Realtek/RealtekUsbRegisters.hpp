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

// The register window of a Realtek USB Wi-Fi adapter (after rtw88/usb.c rtw_usb_read/rtw_usb_write and rtl8xxxu's rtl8xxxu_read8/write8): every register access is a
// vendor control transfer on endpoint 0, request 0x05 (REALTEK_USB_VENQT_CMD_REQ), wValue = the register address, wIndex = 0, 1/2/4 data bytes little-endian;
// IN (0xC0) to read, OUT (0x40) to write. rtw89's USB chips put the address's high half in wIndex (rtw89/usb.c, unverified), which is what this does for addresses
// above 0xFFFF; the rtw88 and rtl8xxxu registers are all below that.
//
// A failed read answers all ones, as a missing PCIe device does, and is counted; so is a failed write.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"

namespace hardware {

class RealtekUsbRegisters final : public INicRegisters {
 public:
  static constexpr uint8_t kRequest = 0x05, kReadType = 0xC0, kWriteType = 0x40;

  RealtekUsbRegisters(IUsbHostController& usb, uint32_t port) : usb_(&usb), port_(port) {}

  uint8_t read8(uint32_t offset) override { return static_cast<uint8_t>(read(offset, 1)); }
  uint16_t read16(uint32_t offset) override { return static_cast<uint16_t>(read(offset, 2)); }
  uint32_t read32(uint32_t offset) override { return read(offset, 4); }
  void write8(uint32_t offset, uint8_t value) override { write(offset, value, 1); }
  void write16(uint32_t offset, uint16_t value) override { write(offset, value, 2); }
  void write32(uint32_t offset, uint32_t value) override { write(offset, value, 4); }

  uint32_t failures() const { return failures_; }

 private:
  uint32_t read(uint32_t offset, uint32_t length);
  void write(uint32_t offset, uint32_t value, uint32_t length);

  IUsbHostController* usb_;
  uint32_t port_;
  uint32_t failures_ = 0;
};

}  // namespace hardware
