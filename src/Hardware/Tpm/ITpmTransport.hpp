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

// How a command gets to a TPM 2.0 and the response back: a transport (the FIFO interface TIS, the command/response buffer CRB) takes a complete command and returns a complete response. The
// commands themselves are Tpm2 (TpmDevice.hpp).
#pragma once
#include "Utility/ByteBuffer.hpp"

namespace hardware {

class ITpmTransport {
 public:
  virtual const char* name() const = 0;
  // Sends `command` (a complete TPM 2.0 command, header included) and reads the whole response into `response` (replacing what it held). False if the TPM did not take the command, did
  // not answer in time, or the response is shorter than its own header says.
  virtual bool transmit(ConstByteView command, ByteBuffer* response) = 0;

 protected:
  constexpr ITpmTransport() = default;
  ~ITpmTransport() = default;
};

// The 32 bit registers of a TPM interface in memory (the 4 KiB at 0xFED40000 for locality 0, or wherever the ACPI TPM2 table says). Byte access for the FIFO registers.
class ITpmRegisters {
 public:
  virtual uint8_t read8(uint32_t offset) = 0;
  virtual void write8(uint32_t offset, uint8_t value) = 0;
  virtual uint32_t read32(uint32_t offset) = 0;
  virtual void write32(uint32_t offset, uint32_t value) = 0;

 protected:
  constexpr ITpmRegisters() = default;
  ~ITpmRegisters() = default;
};

}  // namespace hardware
