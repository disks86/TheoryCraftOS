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


// The card's non-volatile memory, as iwlwifi reads it (mvm/nvm.c, nvm.c) for the 9000 family: sections are fetched from the running firmware with NVM_ACCESS_CMD in chunks, and
// the MAC address is the override section's when that holds a usable one, otherwise the one fused into the chip, read from the WFMP registers.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"
#include "Hardware/WirelessInterface/Intel/IwlTransport.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

namespace iwl_nvm {
constexpr uint8_t kAccessCommand = 0x88;                        // NVM_ACCESS_CMD (group 0)
constexpr uint8_t kRead = 0;
constexpr uint8_t kTargetCache = 0;
constexpr uint16_t kSectionSw = 1, kSectionRegulatory = 3, kSectionCalibration = 4, kSectionProduction = 5, kSectionRegulatorySdp = 8, kSectionMacOverride = 11, kSectionPhySku = 12;
constexpr uint32_t kMaxSections = 13;
constexpr uint16_t kChunkBytes = 2048;                          // IWL_NVM_DEFAULT_CHUNK_SIZE
constexpr uint32_t kMaxSectionBytes = 64 * 1024;                // (the sections are a few KiB; a firmware that goes on longer is lying)
constexpr uint32_t kWfmpMacAddr0 = 0xA03080, kWfmpMacAddr1 = 0xA03084;
constexpr uint32_t kMacAddressOverrideOffset = 2;               // MAC_ADDRESS_OVERRIDE_EXT_NVM (1) 16 bit words into the section
}  // namespace iwl_nvm

// Not multicast and not all zero (is_valid_ether_addr).
bool iwl_valid_ether_address(const uint8_t address[6]);
// The two WFMP words in the order the hardware keeps them (iwl_flip_hw_address).
void iwl_flip_hw_address(uint32_t word0, uint32_t word1, uint8_t out[6]);
// The override section's address when it is valid and not the reserved one, else the fused one from the words. False when neither is usable.
bool iwl_choose_mac_address(ConstByteView mac_override, uint32_t wfmp0, uint32_t wfmp1, uint8_t out[6]);
// The fused address from the chip (needs the MAC awake; the card must be started).
bool iwl_read_fused_address(IwlPcieDevice& device, uint8_t out[6]);

// The NVM_ACCESS_CMD request for a read (8 bytes) and the checks on its response: status 0, the section type, the offset asked for and no more data than asked.
void iwl_nvm_read_request(uint16_t section, uint16_t offset, uint16_t length, uint8_t out[8]);
bool iwl_nvm_parse_response(ConstByteView response, uint16_t section, uint16_t offset, uint16_t requested, ConstByteView* data);

// Reads a whole section into `out` (replacing its content). False with a message on a refused or malformed response.
bool iwl_read_nvm_section(IwlTransport& transport, uint16_t section, ByteBuffer& out, const char** error);

}  // namespace hardware
