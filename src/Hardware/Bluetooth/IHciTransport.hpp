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

// What the host controller interface runs over (USB, or UART and PCIe elsewhere). Packets are unframed: a command is opcode, length, parameters; an event is code, length, parameters; an
// ACL packet is handle and flags, length, data. Nothing here waits: the polls return false when nothing has arrived.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

class IHciTransport {
 public:
  virtual bool send_command(ConstByteView packet) = 0;
  virtual bool send_acl(ConstByteView packet) = 0;
  // Copies the oldest packet that has arrived into `buffer`, with its length in `*length`.
  virtual bool poll_event(ByteView buffer, uint16_t* length) = 0;
  virtual bool poll_acl(ByteView buffer, uint16_t* length) = 0;
  // False once the controller was unplugged or reset away.
  virtual bool present() const = 0;

 protected:
  constexpr IHciTransport() = default;
  ~IHciTransport() = default;
};

}  // namespace hardware
