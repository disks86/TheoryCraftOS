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

// Realtek Wi-Fi firmware files, as linux-firmware ships them.
//
// rtw88 (after drivers/net/wireless/realtek/rtw88/fw.c and fw.h). Two header layouts:
//  - struct rtw_fw_hdr (64 bytes), for the chips whose Wi-Fi CPU is the 11ac MCU (8822B, 8822C, 8821C): signature (the chip number, 0x8822), version, sub version
//    and index, feature bits, build date, memory usage, H2C format version, then DMEM address/size, IMEM size, EMEM size/address, IMEM address. The file is the header,
//    DMEM, an 8-byte checksum, IMEM, an 8-byte checksum and, when bit 4 of mem_usage says so, EMEM and its checksum; its size must be exactly that (check_firmware_size).
//    The checksum is the XOR of the block's 16-bit little-endian words, stored in the trailer's first two bytes (the DDMA engine checks it while copying; found by
//    comparing with the real files, rtw88 itself does not compute it).
//  - struct rtw_fw_hdr_legacy (32 bytes), for the chips with an 8051 (8723D, 8703B, 8723B, 8821A, 8812A, 8814A): signature (0x23D1 ...), version, sub versions, date,
//    the body size and index. The body follows directly and is written to the chip page by page through the 0x1000 window.
//
// rtw89 (after drivers/net/wireless/realtek/rtw89/fw.c and fw.h). A file is a multi-firmware container (struct rtw89_mfw_hdr, signature byte 0xFF): the number of
// firmwares, a container version, then 16-byte entries (struct rtw89_mfw_info: chip cut "cv", type, mp, offset, size). After the last firmware, at a 16-byte boundary,
// come "elements" (struct rtw89_fw_element_hdr: id, size, version, then a 32-byte header in all and the contents; the next element at the next 16-byte boundary): BB/RF
// register tables, TX power tables and the BB MCU images of newer chips. An old file without the 0xFF signature is a single firmware.
// Each firmware has a header (struct rtw89_fw_hdr, 32 bytes, or rtw89_fw_hdr_v1, 48 bytes, by the header version in word 3), an optional dynamic header, and section
// headers (16 bytes: download address, size, type, checksum and re-download flags, MSS count), then the sections' contents in order. A security section (type 9) is
// followed by its signatures: MSS count * 512 bytes, or (count 0xFF) a "MSSKPOOL" key pool. The pool's own layout (keys per type, the bitmap rtw89 picks a key with)
// is not decoded here: its length is taken as whatever the entry has left, which is consistent on every file of linux-firmware 2025 but unverified against rtw89.
//
// The file comes from disk and goes to a chip: every offset and length is checked, and the parsed views point into the file, which has to outlive them.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

enum class RtwFirmwareStatus : uint8_t {
  Ok,
  Truncated,          // the file ends inside a header or a block
  BadSignature,       // not this chip's (or not a firmware file)
  SizeMismatch,       // the blocks the header describes do not add up to the file
  BadChecksum,        // an rtw88 block's checksum does not match
  BadLayout,          // an rtw89 container or section table that does not make sense (overlaps, no entries, too many sections)
};

const char* rtw_firmware_status_name(RtwFirmwareStatus status);

// ---------------------------------------------------------------- rtw88

struct Rtw88Firmware {
  bool legacy = false;            // rtw_fw_hdr_legacy (8051 chips)
  uint16_t signature = 0;
  uint8_t category = 0, function = 0;
  uint16_t version = 0;
  uint8_t sub_version = 0, sub_index = 0;
  uint32_t feature = 0;           // (11ac header only)
  uint16_t year = 0;              // (legacy: 0; the legacy header has no year)
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  uint8_t mem_usage = 0;
  uint16_t h2c_format_version = 0;
  uint32_t dmem_address = 0, imem_address = 0, emem_address = 0;
  ConstByteView dmem, imem, emem;     // the blocks without their checksums (emem empty when not used)
  ConstByteView body;                 // legacy: what is written to the chip
};

constexpr uint32_t kRtw88HeaderSize = 64, kRtw88LegacyHeaderSize = 32, kRtw88ChecksumSize = 8;

// `expected_signature`: 0 accepts any; otherwise the 11ac header's signature must equal it (0x8822, 0x8821) and a legacy one must match it in all but the low
// nibble (0x23D0 for 8723D, as rtlwifi's IS_FW_HEADER_EXIST does).
[[nodiscard]] RtwFirmwareStatus rtw88_parse_firmware(ConstByteView file, bool legacy, uint16_t expected_signature, Rtw88Firmware* out);
// The DDMA checksum of a block: the XOR of its 16-bit little-endian words (an odd last byte counts as the low byte of a word).
uint16_t rtw88_block_checksum(ConstByteView block);

// ---------------------------------------------------------------- rtw89

enum class Rtw89FirmwareType : uint8_t { Normal = 1, Wowlan = 3, NormalCe = 5, BbMcu0 = 64, BbMcu1 = 65, LogFormat = 255 };
constexpr uint8_t kRtw89SecuritySectionType = 9;
constexpr uint32_t kRtw89SignatureLength = 512, kRtw89SectionChecksumLength = 8, kRtw89MaxSections = 8, kRtw89MaxEntries = 16;

struct Rtw89Section {
  uint32_t download_address = 0;     // in the chip's address space (rtw89 masks it with 0x1FFFFFFF before use)
  uint8_t type = 0;
  bool checksum = false;             // an 8-byte checksum follows the contents (and is part of `data`)
  bool redownload = false;
  uint8_t mssc = 0;                  // security sections: how many 512-byte signatures follow, 0xFF for a key pool
  ConstByteView data;
};

struct Rtw89FirmwareImage {
  uint8_t header_version = 0;        // 0: rtw89_fw_hdr, 1: rtw89_fw_hdr_v1
  uint16_t chip_id = 0;              // word 0's high half (0x8852, 0x8922)
  uint8_t major = 0, minor = 0, sub_version = 0, sub_index = 0;
  uint32_t commit_id = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  uint32_t year = 0;
  uint8_t command_version = 0;
  bool dynamic_header = false;
  uint32_t header_length = 0;        // fixed + section headers + dynamic header: where the first section starts
  uint32_t section_count = 0;
  Rtw89Section sections[kRtw89MaxSections];
  uint32_t signature_bytes = 0;      // the signatures / key pool after the security section
  ConstByteView whole;
};

struct Rtw89FirmwareEntry {
  uint8_t cv = 0;                    // the chip cut it is for (0 = A cut); 0xFF in a single-firmware file
  uint8_t type = 0;                  // Rtw89FirmwareType
  uint8_t mp = 0;                    // 1: manufacturing-test firmware
  ConstByteView data;
};

struct Rtw89FirmwareElement {
  uint32_t id = 0;                   // enum rtw89_fw_element_id (0/1 BB MCU, 2..5 BB/RF tables, 8 TX power by rate, ...)
  uint32_t version = 0;              // ver[4] little-endian
  ConstByteView contents;            // after the 32-byte header
};

class Rtw89FirmwareFile {
 public:
  // Parses the container, every entry's header and section table (except LogFormat and BB MCU entries, which have other layouts), and walks the elements.
  [[nodiscard]] RtwFirmwareStatus parse(ConstByteView file);

  bool multi() const { return multi_; }
  uint8_t container_version(uint32_t i) const { return i < 4 ? container_version_[i] : 0; }
  uint32_t entry_count() const { return entry_count_; }
  const Rtw89FirmwareEntry& entry(uint32_t i) const { return entries_[i < entry_count_ ? i : 0]; }
  // The firmware of this type for this cut (and not an MP one), as rtw89_mfw_recognize picks it; the single firmware of an old file matches any cut. Null if none.
  const Rtw89FirmwareEntry* find(Rtw89FirmwareType type, uint8_t cv) const;
  uint32_t element_count() const { return element_count_; }
  // The n-th element, walking from the start. False past the end.
  bool element(uint32_t index, Rtw89FirmwareElement* out) const;

 private:
  bool multi_ = false;
  uint8_t container_version_[4] = {};
  uint32_t entry_count_ = 0, element_count_ = 0;
  uint64_t elements_start_ = 0;
  Rtw89FirmwareEntry entries_[kRtw89MaxEntries];
  ConstByteView file_;
};

// Parses one firmware's header and section table (an entry's data).
[[nodiscard]] RtwFirmwareStatus rtw89_parse_firmware_image(ConstByteView data, Rtw89FirmwareImage* out);

}  // namespace hardware
