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

}  // namespace hardware
