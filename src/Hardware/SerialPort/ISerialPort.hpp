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
#include "Hardware/SerialPort/SerialTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// A serial port. Like the other devices it is polled and never blocks: write() queues bytes and poll() moves them to the wire, poll() also
// takes what arrived into a receive queue that read() empties.
class ISerialPort {
 public:
  virtual const char* name() const = 0;
  // Finds out whether the hardware is there; false if not (nothing has been changed then). The port stays closed until open().
  virtual bool initialize() = 0;
  // False once the port is gone (a USB adapter that was unplugged): the queues are lost and write() takes nothing.
  virtual bool present() const { return true; }
  virtual void shutdown() = 0;

  // Applies `config` and clears both queues. False if the hardware cannot do it (see SerialConfig::valid, and the baud rate limit).
  virtual bool open(const SerialConfig& config) = 0;
  virtual void close() = 0;
  virtual SerialState state() const = 0;
  virtual const SerialConfig& config() const = 0;

  // Queues as much of `data` as fits and returns how many bytes that was (the rest is for the caller to offer again).
  virtual uint32_t write(ConstByteView data) = 0;
  // Copies out received bytes, oldest first, and returns how many.
  virtual uint32_t read(ByteView out) = 0;
  virtual uint32_t bytes_readable() const = 0;
  virtual uint32_t bytes_writable() const = 0;   // room left in the send queue
  // True while queued bytes have not all left the port.
  virtual bool transmit_pending() const = 0;
  virtual void discard_input() = 0;

  // Moves bytes between the queues and the hardware.
  virtual void poll() = 0;

  // The modem lines (SerialLines bits); set_lines() drives kRts and kDtr, the other bits are ignored.
  virtual uint8_t lines() const { return 0; }
  virtual void set_lines(uint8_t) {}
  // Holds the line at the break level until set_break(false).
  virtual void set_break(bool) {}

  virtual SerialStatistics statistics() const = 0;

 protected:
  constexpr ISerialPort() = default;
  ~ISerialPort() = default;
};

}  // namespace hardware
