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

// The TPM 2.0 commands the system uses, over a transport: Startup, GetRandom, PCR_Read, GetCapability (TCG TPM 2.0 Library, part 3; commands without sessions, SHA-256 bank), and the
// ACPI TPM2 table that says which interface the firmware presents. TpmEntropySource lets the TPM's random number generator seed the system's.
#pragma once
#include "Crypto/Random.hpp"
#include "Hardware/Tpm/ITpmTransport.hpp"

namespace hardware {

namespace tpm {
constexpr uint32_t kRcSuccess = 0, kRcInitialize = 0x100;
constexpr uint32_t kPtFamily = 0x100, kPtManufacturer = 0x105, kPtFirmwareVersion1 = 0x10B, kPtFirmwareVersion2 = 0x10C;
}  // namespace tpm

class TpmDevice {
 public:
  TpmDevice(ITpmTransport& transport, allocators::IAllocator& allocator) : allocator_(allocator), transport_(transport), response_(allocator) {}
  // TPM2_Startup(CLEAR): the firmware has done it before the system starts, in which case the TPM answers TPM_RC_INITIALIZE, which counts as success. False if the TPM does not answer.
  bool startup();
  // Fills `out` with bytes from the TPM's generator (in pieces of at most 32 bytes).
  bool get_random(ByteView out);
  // The value of PCR `index` (0..23) of the SHA-256 bank.
  bool pcr_read(uint32_t index, uint8_t digest[32]);
  // A fixed property of the TPM (TPM_PT_...).
  bool property(uint32_t property, uint32_t* value);
  uint32_t last_response_code() const { return response_code_; }
  const char* transport_name() const { return transport_.name(); }

 private:
  bool run(uint32_t command_code, ConstByteView parameters, ConstByteView* body);
  allocators::IAllocator& allocator_;
  ITpmTransport& transport_;
  ByteBuffer response_;
  uint32_t response_code_ = 0;
};

class TpmEntropySource final : public crypto::IEntropySource {
 public:
  explicit TpmEntropySource(TpmDevice& tpm) : tpm_(tpm) {}
  bool read(ByteView out) override { return tpm_.get_random(out); }

 private:
  TpmDevice& tpm_;
};

// The TPM2 table of ACPI (TCG ACPI specification): what the firmware says about the TPM.
struct Tpm2Table {
  uint16_t platform_class = 0;              // 0 client, 1 server
  uint64_t control_area = 0;                // the CRB control area (start methods 6 and up)
  uint32_t start_method = 0;
  static constexpr uint32_t kStartMmio = 0, kStartAcpi = 2, kStartCrb = 6, kStartCrbAcpi = 7, kStartCrbSmc = 8;
};
bool tpm2_parse_table(ConstByteView table, Tpm2Table* out);

}  // namespace hardware
