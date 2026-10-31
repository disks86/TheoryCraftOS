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

// The controller side of an SD card slot, as the card protocol needs it: power, clock and bus width, commands with their responses, and data stages. SdCard (Sd/SdCard.hpp) speaks the SD
// protocol through this; a host is the Realtek PCI-E card reader (Bus/Rtsx), or any other controller (SDHCI, SPI) that implements it.
#pragma once
#include <stdint.h>
#include "Hardware/IHardware.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

enum class SdResponse : uint8_t {
  None,      // no response
  R1,        // 48 bits with a CRC7 (also R6 and R7)
  R1b,       // R1, then the card may hold DAT0 low while it is busy
  R2,        // 136 bits: CID or CSD
  R3,        // 48 bits without a CRC (the OCR)
};

struct SdCommand {
  uint8_t index = 0;
  uint32_t argument = 0;
  SdResponse response_type = SdResponse::None;
  // R1 / R1b / R3: response[0] is the 32 bit status (OCR) of the response. R2: the four words of the 128 bit register, the most significant first (bits 127..96 in response[0]).
  uint32_t response[4] = {};
};

class ISdHost {
 public:
  virtual bool card_present() = 0;
  virtual bool write_protected() = 0;
  // Counts the cards that have been seen go and come: a new value means that the card now in the slot is not the one that was there before.
  virtual uint32_t generation() = 0;

  // Powers the slot and clocks the card at 400 kHz or less in the 1 bit mode (at least 74 clocks are sent). False if the card cannot be powered.
  virtual bool power_on() = 0;
  virtual void power_off() = 0;
  virtual bool set_clock(uint32_t hz) = 0;
  virtual bool set_bus_width(uint32_t bits) = 0;      // 1 or 4

  // A command without a data stage. False on a timeout or a CRC error (the card did not answer, or the answer was damaged).
  virtual bool command(SdCommand& command) = 0;
  // A command followed by `blocks` blocks of `block_size` bytes from the card / to the card. The caller sends the stop command (CMD12) of a multi-block transfer. `data` is exactly
  // blocks * block_size bytes.
  virtual bool read_blocks(SdCommand& command, uint32_t block_size, uint32_t blocks, ByteView data) = 0;
  virtual bool write_blocks(SdCommand& command, uint32_t block_size, uint32_t blocks, ConstByteView data) = 0;
  // The most blocks one read_blocks / write_blocks can move.
  virtual uint32_t max_blocks() const { return 64; }

 protected:
  constexpr ISdHost() = default;
  ~ISdHost() = default;
};

}  // namespace hardware
