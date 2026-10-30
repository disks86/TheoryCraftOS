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


// ---- NVM_GET_INFO: what the firmware reports about the SKU, the radio and the regulatory state ------------------------------------------------------------------------------

namespace iwl_nvm {
constexpr uint8_t kRegulatoryGroup = 0x0C, kGetInfo = 0x02, kAccessComplete = 0x00;
constexpr uint32_t kMacAddrCsrBase = 0x380;                     // OTP at +0 / +4, the strap (the OEM's fuse) at +8 / +12
constexpr uint32_t kMaxChannels = 51;                           // the extended NVM channel list of the 9000 family (2.4 and 5 GHz)
constexpr uint32_t kSkuBand24 = 1u << 0, kSkuBand52 = 1u << 1, kSku11n = 1u << 2, kSku11ac = 1u << 3, kSku11ax = 1u << 4, kSkuMimoDisabled = 1u << 5;
constexpr uint32_t kChannelValid = 1u << 0, kChannelIbss = 1u << 1, kChannelActive = 1u << 3, kChannelRadar = 1u << 4, kChannelIndoorOnly = 1u << 5, kChannelGoConcurrent = 1u << 6,
                   kChannel20Mhz = 1u << 8, kChannel40Mhz = 1u << 9, kChannel80Mhz = 1u << 10, kChannel160Mhz = 1u << 11;
}  // namespace iwl_nvm

struct IwlChannel {
  uint16_t number = 0;
  uint16_t frequency_mhz = 0;
  uint32_t flags = 0;                   // iwl_nvm::kChannel*
  bool is_5ghz = false;
  bool valid() const { return (flags & iwl_nvm::kChannelValid) != 0; }
};

struct IwlNvmData {
  uint8_t hw_address[6] = {};
  uint16_t nvm_version = 0;
  uint8_t n_hw_addresses = 0, board_type = 0;
  bool empty_otp = false, lar_enabled = false;
  bool band_24ghz = false, band_5ghz = false, ieee80211n = false, ieee80211ac = false, ieee80211ax = false, mimo_disabled = false;
  uint8_t tx_antennas = 0, rx_antennas = 0;     // chains the SKU has and the firmware drives
  uint32_t channel_count = 0;
  IwlChannel channels[iwl_nvm::kMaxChannels];
  uint32_t valid_channels() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < channel_count; ++i) n += channels[i].valid() ? 1 : 0;
    return n;
  }
};

// The channel number at position `index` of the NVM's channel list (1-14, then 36 to 181 in steps of 4); 0 beyond the list.
uint16_t iwl_nvm_channel_number(uint32_t index);
// The centre frequency of a channel in its band.
uint16_t iwl_channel_frequency(uint16_t number, bool is_5ghz);
// The address fused into the chip, read from the CSR block (strap first, the OTP when the strap is empty or invalid). The registers need no wake-up.
bool iwl_read_csr_hw_address(IwlPcieDevice& device, uint8_t out[6]);
// The antenna masks from the firmware's PHY configuration word (TX chains bits 8-11, RX chains bits 12-15).
inline uint8_t iwl_phy_tx_antennas(uint32_t phy_config) { return static_cast<uint8_t>((phy_config >> 8) & 0xF); }
inline uint8_t iwl_phy_rx_antennas(uint32_t phy_config) { return static_cast<uint8_t>((phy_config >> 12) & 0xF); }
// Parses the response of NVM_GET_INFO in any of its three layouts (told apart by the length). `firmware_tx` / `firmware_rx` are the chains the firmware drives, `lar_capable` whether
// it supports location aware regulatory. The hardware address is not in the response: the caller fills it. False with a message for a length that is none of the layouts.
bool iwl_parse_nvm_get_info(ConstByteView payload, uint8_t firmware_tx, uint8_t firmware_rx, bool lar_capable, IwlNvmData* out, const char** error);

}  // namespace hardware
