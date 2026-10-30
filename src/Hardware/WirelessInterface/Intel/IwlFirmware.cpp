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


#include "Hardware/WirelessInterface/Intel/IwlFirmware.hpp"

namespace hardware {

namespace {

// iwl_ucode_tlv_type
constexpr uint32_t kTlvProbeMaxLen = 6, kTlvFlags = 18, kTlvSecRt = 19, kTlvSecInit = 20, kTlvSecWowlan = 21, kTlvPhySku = 23, kTlvSecureSecRt = 24, kTlvSecureSecInit = 25,
                   kTlvSecureSecWowlan = 26, kTlvNumOfCpu = 27, kTlvApiChangesSet = 29, kTlvEnabledCapabilities = 30, kTlvNScanChannels = 31, kTlvPaging = 32, kTlvFwVersion = 36, kTlvCmdVersions = 48,
                   kTlvIml = 52;
constexpr uint32_t kHeaderBytes = 8 + 64 + 4 + 4 + 8;

uint32_t le32(ConstByteView d, uint64_t at) {
  return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) | (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}

}  // namespace

bool IwlFirmware::add_section(List<IwlSection>& list, ConstByteView tlv, bool secure, const char** error) {
  if (tlv.size() < 4) { *error = "a section TLV is shorter than its offset"; return false; }
  if (list.count() >= kMaxSections) { *error = "too many sections"; return false; }
  IwlSection s;
  s.device_offset = le32(tlv, 0);
  s.data = tlv.subview(4, tlv.size() - 4);
  s.secure = secure;
  if (!list.push_back(s)) { *error = "out of memory"; return false; }
  return true;
}

bool IwlFirmware::parse(ConstByteView d, const char** error) {
  const char* scratch = nullptr;
  if (!error) error = &scratch;
  regular_.clear();
  init_.clear();
  wowlan_.clear();
  for (uint32_t& w : api_) w = 0;
  for (uint32_t& w : capa_) w = 0;
  flags_ = phy_config_ = paging_size_ = probe_max_length_ = fw_major_ = fw_minor_ = 0;
  scan_channels_ = 40;                                              // IWL_DEFAULT_SCAN_CHANNELS
  cpu_count_ = 1;
  image_loader_ = command_versions_ = ConstByteView();
  if (d.size() < kHeaderBytes) { *error = "the file is shorter than its header"; return false; }
  if (le32(d, 0) != 0 || le32(d, 4) != kMagic) { *error = "not a TLV firmware file"; return false; }
  for (uint32_t i = 0; i < 64; ++i) human_readable_[i] = d[8 + i] >= 32 && d[8 + i] < 127 ? static_cast<char>(d[8 + i]) : (d[8 + i] == 0 ? 0 : '?');
  human_readable_[64] = 0;
  version_ = le32(d, 72);
  build_ = le32(d, 76);

  uint64_t at = kHeaderBytes;
  while (at + 8 <= d.size()) {
    const uint32_t type = le32(d, at), length = le32(d, at + 4);
    at += 8;
    const uint64_t remaining = d.size() - at;
    if (length > remaining) { *error = "a TLV runs past the end of the file"; return false; }
    const uint64_t aligned = (static_cast<uint64_t>(length) + 3) & ~3ull;
    if (aligned > remaining) { *error = "a TLV's padding runs past the end of the file"; return false; }
    const ConstByteView body = d.subview(at, length);
    at += aligned;
    switch (type) {
      case kTlvProbeMaxLen:
        if (length != 4) { *error = "bad PROBE_MAX_LEN"; return false; }
        probe_max_length_ = le32(body, 0);
        break;
      case kTlvFlags:
        if (length < 4 || length % 4 != 0) { *error = "bad FLAGS"; return false; }
        flags_ = le32(body, 0);
        break;
      case kTlvApiChangesSet: case kTlvEnabledCapabilities: {
        if (length != 8) { *error = "bad API or capability set"; return false; }
        const uint32_t index = le32(body, 0), bits = le32(body, 4);
        if (index < kApiWords) (type == kTlvApiChangesSet ? api_ : capa_)[index] = bits;
        break;
      }
      case kTlvSecRt: if (!add_section(regular_, body, false, error)) return false; break;
      case kTlvSecInit: if (!add_section(init_, body, false, error)) return false; break;
      case kTlvSecWowlan: if (!add_section(wowlan_, body, false, error)) return false; break;
      case kTlvSecureSecRt: if (!add_section(regular_, body, true, error)) return false; break;
      case kTlvSecureSecInit: if (!add_section(init_, body, true, error)) return false; break;
      case kTlvSecureSecWowlan: if (!add_section(wowlan_, body, true, error)) return false; break;
      case kTlvPhySku:
        if (length != 4) { *error = "bad PHY_SKU"; return false; }
        phy_config_ = le32(body, 0);
        break;
      case kTlvNumOfCpu:
        if (length != 4) { *error = "bad NUM_OF_CPU"; return false; }
        cpu_count_ = le32(body, 0);
        if (cpu_count_ != 1 && cpu_count_ != 2) { *error = "the firmware wants a number of processors the chip does not have"; return false; }
        break;
      case kTlvNScanChannels:
        if (body.size() != 4) { *error = "scan channel count TLV has the wrong length"; return false; }
        scan_channels_ = le32(body, 0);
        if (scan_channels_ == 0 || scan_channels_ > 128) { *error = "scan channel count out of range"; return false; }
        break;
      case kTlvPaging:
        if (length != 4) { *error = "bad PAGING"; return false; }
        paging_size_ = le32(body, 0);
        break;
      case kTlvFwVersion:
        if (length != 12) { *error = "bad FW_VERSION"; return false; }
        fw_major_ = le32(body, 0);
        fw_minor_ = le32(body, 4);
        break;
      case kTlvCmdVersions:
        if (length % 4 != 0) { *error = "bad CMD_VERSIONS"; return false; }
        command_versions_ = body;
        break;
      case kTlvIml:
        image_loader_ = body;
        break;
      default:
        break;                                                // (debug destinations, regions, triggers, ... are not needed)
    }
  }
  if (at != d.size()) { *error = "bytes after the last TLV"; return false; }
  if (regular_.count() == 0) { *error = "the file has no runtime image"; return false; }
  return true;
}

uint32_t IwlFirmware::command_version_of(uint8_t group, uint8_t command) const {
  for (uint32_t i = 0; i < command_version_count(); ++i) {
    const IwlCommandVersion v = command_version(i);
    if (v.group == group && v.command == command) return v.command_version;
  }
  return 0;
}

uint32_t IwlFirmware::part(IwlImage image, uint32_t which, IwlSection* out, uint32_t capacity) const {
  const List<IwlSection>& list = sections(image);
  uint32_t current = 0, n = 0;
  for (uint64_t i = 0; i < list.count(); ++i) {
    const IwlSection& s = list[i];
    if (s.is_cpu_separator() || s.is_paging_separator()) {
      ++current;
      continue;
    }
    if (current == which && n < capacity) out[n++] = s;
  }
  return n;
}

}  // namespace hardware
