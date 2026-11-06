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

// What an SMBus transaction ended with.
enum class SmbusStatus : uint8_t {
  Ok,
  NoController,   // no usable host controller (none found, or the firmware left it disabled)
  NoDevice,       // nothing acknowledged the address (DEV_ERR): no device there, or it refused the command
  Busy,           // the bus or the controller is in use by someone else (the firmware, the embedded controller, the IMC)
  Collision,      // lost arbitration on the bus (BUS_ERR)
  Failed,         // the controller reported a failed transaction (FAILED), or a block was malformed
  Timeout,        // the transaction did not finish; it was killed
  BadArgument,    // an address outside 7 bits, a block longer than 32 bytes
};
const char* smbus_status_name(SmbusStatus status);

// An SMBus host: the System Management Bus of the chipset that carries the memory modules' SPD EEPROMs, temperature sensors, clock generators and
// the like. Addresses are 7-bit. Transactions are the SMBus 2.0 protocols the Intel (i801) and AMD (PIIX4/FCH) hosts implement in hardware.
class ISmbusController {
 public:
  virtual SmbusStatus quick(uint8_t address, bool read) = 0;
  virtual SmbusStatus read_byte(uint8_t address, uint8_t* value) = 0;
  virtual SmbusStatus read_byte_data(uint8_t address, uint8_t command, uint8_t* value) = 0;
  virtual SmbusStatus write_byte_data(uint8_t address, uint8_t command, uint8_t value) = 0;
  virtual SmbusStatus read_word_data(uint8_t address, uint8_t command, uint16_t* value) = 0;
  // An SMBus block read: the device sends a count (1..32) and that many bytes. `out` must hold 32; `length` gets the count.
  virtual SmbusStatus read_block_data(uint8_t address, uint8_t command, ArrayView<uint8_t> out, uint8_t* length) = 0;

 protected:
  constexpr ISmbusController() = default;
  ~ISmbusController() = default;
};

}  // namespace hardware
