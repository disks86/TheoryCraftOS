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

#include "Hardware/WirelessInterface/Realtek/RealtekUsbRegisters.hpp"

namespace hardware {

uint32_t RealtekUsbRegisters::read(uint32_t offset, uint32_t length) {
  uint8_t data[4] = {};
  const ByteView buffer = ByteView(data).first(length);
  if (!usb_->control_in(port_, kReadType, kRequest, static_cast<uint16_t>(offset), static_cast<uint16_t>(offset >> 16), buffer)) {
    ++failures_;
    return length == 4 ? 0xFFFFFFFFu : (1u << (8 * length)) - 1;
  }
  uint32_t value = 0;
  for (uint32_t i = 0; i < length; ++i) value |= uint32_t{data[i]} << (8 * i);
  return value;
}

void RealtekUsbRegisters::write(uint32_t offset, uint32_t value, uint32_t length) {
  uint8_t data[4] = {};
  for (uint32_t i = 0; i < length; ++i) data[i] = static_cast<uint8_t>(value >> (8 * i));
  if (!usb_->control_out_data(port_, kWriteType, kRequest, static_cast<uint16_t>(offset), static_cast<uint16_t>(offset >> 16), ConstByteView(data).first(length)))
    ++failures_;
}

}  // namespace hardware
