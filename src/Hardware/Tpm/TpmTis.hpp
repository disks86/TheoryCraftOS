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

// The TPM FIFO interface (TCG PC Client Platform TPM Profile, "TIS"), locality 0: the interface of the discrete TPMs on LPC / SPI and of firmware TPMs that present it. A command is written
// byte by byte to the data FIFO within the burst count the TPM allows, started with tpmGo, and the response read the same way when dataAvail is up.
#pragma once
#include "Hardware/Tpm/ITpmTransport.hpp"

namespace hardware {

namespace tis {
constexpr uint32_t kAccess = 0x00, kIntfCapability = 0x14, kSts = 0x18, kDataFifo = 0x24, kDidVid = 0xF00;
constexpr uint8_t kAccessEstablishment = 0x01, kAccessRequestUse = 0x02, kAccessPendingRequest = 0x04, kAccessSeize = 0x08, kAccessBeenSeized = 0x10, kAccessActiveLocality = 0x20, kAccessValid = 0x80;
constexpr uint8_t kStsDataExpect = 0x08, kStsDataAvail = 0x10, kStsGo = 0x20, kStsCommandReady = 0x40, kStsValid = 0x80;
}  // namespace tis

class TpmTis final : public ITpmTransport {
 public:
  explicit TpmTis(ITpmRegisters& registers) : registers_(registers) {}
  // Is there a TPM here? (the access register reads as a valid one, not all ones)
  bool probe();
  uint32_t vendor_device() { return registers_.read32(tis::kDidVid); }
  const char* name() const override { return "TPM 2.0 (FIFO)"; }
  bool transmit(ConstByteView command, ByteBuffer* response) override;

 private:
  bool request_locality();
  uint32_t burst_count();
  bool wait_status(uint8_t mask, uint8_t value);
  bool go_ready();
  bool read_bytes(uint8_t* out, uint32_t count);
  ITpmRegisters& registers_;
};

}  // namespace hardware
