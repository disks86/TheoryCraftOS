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


#include "Hardware/WirelessInterface/Intel/IwlNvm.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

bool iwl_valid_ether_address(const uint8_t address[6]) {
  if (address[0] & 1) return false;
  for (uint32_t i = 0; i < 6; ++i)
    if (address[i]) return true;
  return false;
}

void iwl_flip_hw_address(uint32_t word0, uint32_t word1, uint8_t out[6]) {
  out[0] = static_cast<uint8_t>(word0 >> 24);
  out[1] = static_cast<uint8_t>(word0 >> 16);
  out[2] = static_cast<uint8_t>(word0 >> 8);
  out[3] = static_cast<uint8_t>(word0);
  out[4] = static_cast<uint8_t>(word1 >> 8);
  out[5] = static_cast<uint8_t>(word1);
}

bool iwl_choose_mac_address(ConstByteView mac_override, uint32_t wfmp0, uint32_t wfmp1, uint8_t out[6]) {
  static const uint8_t reserved[6] = {0x02, 0xcc, 0xaa, 0xff, 0xee, 0x00};
  if (mac_override.size() >= iwl_nvm::kMacAddressOverrideOffset + 6) {
    uint8_t candidate[6];
    for (uint32_t i = 0; i < 6; ++i) candidate[i] = mac_override[iwl_nvm::kMacAddressOverrideOffset + i];
    bool is_reserved = true;
    for (uint32_t i = 0; i < 6; ++i)
      if (candidate[i] != reserved[i]) is_reserved = false;
    if (iwl_valid_ether_address(candidate) && !is_reserved) {
      for (uint32_t i = 0; i < 6; ++i) out[i] = candidate[i];
      return true;
    }
  }
  iwl_flip_hw_address(wfmp0, wfmp1, out);
  return iwl_valid_ether_address(out);
}

bool iwl_read_fused_address(IwlPcieDevice& device, uint8_t out[6]) {
  if (!device.grab_nic_access()) return false;
  const uint32_t word0 = device.read_prph(iwl_nvm::kWfmpMacAddr0), word1 = device.read_prph(iwl_nvm::kWfmpMacAddr1);
  device.release_nic_access();
  iwl_flip_hw_address(word0, word1, out);
  return iwl_valid_ether_address(out);
}

void iwl_nvm_read_request(uint16_t section, uint16_t offset, uint16_t length, uint8_t out[8]) {
  out[0] = iwl_nvm::kRead;
  out[1] = iwl_nvm::kTargetCache;
  out[2] = static_cast<uint8_t>(section);
  out[3] = static_cast<uint8_t>(section >> 8);
  out[4] = static_cast<uint8_t>(offset);
  out[5] = static_cast<uint8_t>(offset >> 8);
  out[6] = static_cast<uint8_t>(length);
  out[7] = static_cast<uint8_t>(length >> 8);
}

bool iwl_nvm_parse_response(ConstByteView response, uint16_t section, uint16_t offset, uint16_t requested, ConstByteView* data) {
  ByteReader r(response);
  const uint16_t got_offset = r.le16(), length = r.le16(), type = r.le16(), status = r.le16();
  if (!r.ok() || status != 0 || type != section || got_offset != offset || length > requested || r.remaining() < length) return false;
  *data = r.take(length);
  return r.ok();
}

bool iwl_read_nvm_section(IwlTransport& transport, uint16_t section, ByteBuffer& out, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  out.clear();
  uint8_t response[8 + iwl_nvm::kChunkBytes];
  for (uint32_t offset = 0;;) {
    uint8_t request[8];
    iwl_nvm_read_request(section, static_cast<uint16_t>(offset), iwl_nvm::kChunkBytes, request);
    IwlHostCommand command;
    command.opcode = iwl_nvm::kAccessCommand;
    command.data = ConstByteView(request, sizeof request);
    const int32_t n = transport.send_sync(command, ByteView(response, sizeof response), 2000000, error);
    if (n < 0) return false;
    ConstByteView data;
    if (static_cast<uint32_t>(n) > sizeof response || !iwl_nvm_parse_response(ConstByteView(response, static_cast<uint32_t>(n)), section, static_cast<uint16_t>(offset),
                                                                                  iwl_nvm::kChunkBytes, &data)) {
      *error = "the firmware's NVM response is not valid (section missing?)";
      return false;
    }
    if (data.size() == 0) return true;                        // the end
    if (offset + data.size() > iwl_nvm::kMaxSectionBytes || !out.append(data)) {
      *error = "the NVM section is too large";
      return false;
    }
    offset += static_cast<uint32_t>(data.size());
    if (data.size() < iwl_nvm::kChunkBytes) return true;
  }
}


uint16_t iwl_nvm_channel_number(uint32_t index) {
  if (index < 14) return static_cast<uint16_t>(index + 1);
  index -= 14;
  if (index < 8) return static_cast<uint16_t>(36 + 4 * index);        // 36 - 64
  index -= 8;
  if (index < 8) return static_cast<uint16_t>(68 + 4 * index);        // 68 - 96
  index -= 8;
  if (index < 12) return static_cast<uint16_t>(100 + 4 * index);      // 100 - 144
  index -= 12;
  if (index < 9) return static_cast<uint16_t>(149 + 4 * index);       // 149 - 181
  return 0;
}

uint16_t iwl_channel_frequency(uint16_t number, bool is_5ghz) {
  if (!is_5ghz) return number == 14 ? 2484 : static_cast<uint16_t>(2407 + 5 * number);
  return static_cast<uint16_t>(5000 + 5 * number);
}

bool iwl_read_csr_hw_address(IwlPcieDevice& device, uint8_t out[6]) {
  const uint32_t base = iwl_nvm::kMacAddrCsrBase;
  iwl_flip_hw_address(device.read32(base + 8), device.read32(base + 12), out);
  if (iwl_valid_ether_address(out)) return true;                       // the OEM's fuse
  iwl_flip_hw_address(device.read32(base), device.read32(base + 4), out);
  return iwl_valid_ether_address(out);
}

bool iwl_parse_nvm_get_info(ConstByteView payload, uint8_t firmware_tx, uint8_t firmware_rx, bool lar_capable, IwlNvmData* out, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  // general 8 bytes, MAC SKU 4, PHY SKU 8, then the regulatory part: v3 (LAR flag, 51 16 bit profiles, 2 reserved), v4 (LAR flag, count, 110 32 bit), v5 (the same with 115)
  constexpr uint64_t kFixed = 8 + 4 + 8;
  const uint64_t v3 = kFixed + 4 + 51 * 2 + 2, v4 = kFixed + 8 + 110 * 4, v5 = kFixed + 8 + 115 * 4;
  const uint64_t size = payload.size();
  if (size != v3 && size != v4 && size != v5) {
    *error = "the NVM info response has an unknown size";
    return false;
  }
  ByteReader r(payload);
  const uint32_t flags = r.le32();
  out->nvm_version = r.le16();
  out->board_type = r.u8();
  out->n_hw_addresses = r.u8();
  out->empty_otp = (flags & 1) != 0;
  const uint32_t sku = r.le32();
  out->band_24ghz = (sku & iwl_nvm::kSkuBand24) != 0;
  out->band_5ghz = (sku & iwl_nvm::kSkuBand52) != 0;
  out->ieee80211n = (sku & iwl_nvm::kSku11n) != 0;
  out->ieee80211ac = (sku & iwl_nvm::kSku11ac) != 0;
  out->ieee80211ax = (sku & iwl_nvm::kSku11ax) != 0;
  out->mimo_disabled = (sku & iwl_nvm::kSkuMimoDisabled) != 0;
  out->tx_antennas = static_cast<uint8_t>(r.le32()) & firmware_tx;
  out->rx_antennas = static_cast<uint8_t>(r.le32()) & firmware_rx;
  const uint32_t lar = r.le32();
  out->lar_enabled = lar != 0 && lar_capable;
  uint32_t entries = 51;
  const bool wide = size != v3;                                         // 32 bit profiles with a count
  if (wide) {
    const uint32_t listed = r.le32();
    const uint32_t room = size == v4 ? 110 : 115;
    entries = listed < room ? listed : room;                            // a count beyond the array is clamped, not trusted
  }
  out->channel_count = 0;
  for (uint32_t i = 0; i < entries; ++i) {
    const uint32_t profile = wide ? r.le32() : r.le16();
    if (!r.ok()) {
      *error = "the NVM info response is truncated";
      return false;
    }
    const uint16_t number = iwl_nvm_channel_number(i);
    if (number == 0 || out->channel_count >= iwl_nvm::kMaxChannels) continue;       // the 6 GHz and later entries are not for this card
    IwlChannel& c = out->channels[out->channel_count++];
    c.number = number;
    c.is_5ghz = i >= 14;
    c.frequency_mhz = iwl_channel_frequency(number, c.is_5ghz);
    c.flags = profile;
  }
  return r.ok();
}

}  // namespace hardware
