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

// How the UCSI core reaches the platform policy manager: the four registers of the shared data structure. An implementation is the ACPI mailbox
// (AcpiUcsiTransport: shared memory kicked by _DSM), and could be an I2C controller (a Cypress CCGx, a TI TPS6598x) or the ChromeOS EC.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

class IUcsiTransport {
 public:
  // The UCSI version (BCD). False if it cannot be read.
  [[nodiscard]] virtual bool read_version(uint16_t* version) = 0;
  // Asks the PPM for its current state and reads CCI (for ACPI: the _DSM read, then the memory).
  [[nodiscard]] virtual bool read_cci(uint32_t* cci) = 0;
  // MESSAGE IN as of the last read_cci(); `out` is at most message_capacity() bytes.
  [[nodiscard]] virtual bool read_message_in(ByteView out) = 0;
  [[nodiscard]] virtual bool write_message_out(ConstByteView data) = 0;
  // Writes CONTROL and hands the command to the PPM.
  [[nodiscard]] virtual bool write_control(uint64_t control) = 0;
  // How many bytes MESSAGE IN holds (16 before UCSI 2.0; up to 256 after).
  virtual uint32_t message_capacity() const = 0;

 protected:
  constexpr IUcsiTransport() = default;
  ~IUcsiTransport() = default;
};

// Time for the command timeouts. The kernel's is the uptime and a thread sleep; a test's moves on when asked to sleep.
class IUcsiClock {
 public:
  virtual uint64_t now_us() = 0;
  virtual void sleep_us(uint32_t us) = 0;

 protected:
  constexpr IUcsiClock() = default;
  ~IUcsiClock() = default;
};

}  // namespace hardware
