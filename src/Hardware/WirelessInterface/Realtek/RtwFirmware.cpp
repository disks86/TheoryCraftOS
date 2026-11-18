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

#include "Hardware/WirelessInterface/Realtek/RtwFirmware.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

const char* rtw_firmware_status_name(RtwFirmwareStatus status) {
  switch (status) {
    case RtwFirmwareStatus::Ok: return "ok";
    case RtwFirmwareStatus::Truncated: return "the file is cut short";
    case RtwFirmwareStatus::BadSignature: return "not a firmware file for this chip";
    case RtwFirmwareStatus::SizeMismatch: return "the blocks do not add up to the file size";
    case RtwFirmwareStatus::BadChecksum: return "a block checksum does not match";
    case RtwFirmwareStatus::BadLayout: return "the container or section table is inconsistent";
  }
  return "?";
}

// ---------------------------------------------------------------- rtw88

uint16_t rtw88_block_checksum(ConstByteView block) {
  uint16_t sum = 0;
  uint64_t i = 0;
  for (; i + 1 < block.size(); i += 2) sum ^= static_cast<uint16_t>(block[i] | block[i + 1] << 8);
  if (i < block.size()) sum ^= block[i];
  return sum;
}

namespace {

// One block and its 8-byte checksum trailer at `*offset`; moves the offset past both.
RtwFirmwareStatus take_block(ConstByteView file, uint64_t* offset, uint32_t size, ConstByteView* block) {
  ConstByteView with_trailer;
  if (size > UINT64_MAX - kRtw88ChecksumSize || !file.try_subview(*offset, size + uint64_t{kRtw88ChecksumSize}, &with_trailer)) return RtwFirmwareStatus::Truncated;
  *block = with_trailer.first(size);
  const ConstByteView trailer = with_trailer.subview(size);
  const uint16_t stored = static_cast<uint16_t>(trailer[0] | trailer[1] << 8);
  *offset += size + uint64_t{kRtw88ChecksumSize};
  return stored == rtw88_block_checksum(*block) ? RtwFirmwareStatus::Ok : RtwFirmwareStatus::BadChecksum;
}

}  // namespace

RtwFirmwareStatus rtw88_parse_firmware(ConstByteView file, bool legacy, uint16_t expected_signature, Rtw88Firmware* out) {
  *out = Rtw88Firmware{};
  out->legacy = legacy;
  ByteReader reader(file);
  if (legacy) {
    if (file.size() < kRtw88LegacyHeaderSize) return RtwFirmwareStatus::Truncated;
    out->signature = reader.le16();
    out->category = reader.u8();
    out->function = reader.u8();
    out->version = reader.le16();
    out->sub_version = reader.u8();
    out->sub_index = reader.u8();
    out->month = reader.u8();
    out->day = reader.u8();
    out->hour = reader.u8();
    out->minute = reader.u8();
    const uint16_t size = reader.le16();
    if (!reader.ok()) return RtwFirmwareStatus::Truncated;
    if (expected_signature && (out->signature & 0xFFF0) != (expected_signature & 0xFFF0)) return RtwFirmwareStatus::BadSignature;
    out->body = file.subview(kRtw88LegacyHeaderSize);
    if (size > out->body.size()) return RtwFirmwareStatus::Truncated;
    // Every legacy file in linux-firmware has exactly `size` bytes after the header.
    if (size != out->body.size()) return RtwFirmwareStatus::SizeMismatch;
    if (out->body.empty()) return RtwFirmwareStatus::SizeMismatch;
    return RtwFirmwareStatus::Ok;
  }

  if (file.size() < kRtw88HeaderSize) return RtwFirmwareStatus::Truncated;
  out->signature = reader.le16();
  out->category = reader.u8();
  out->function = reader.u8();
  out->version = reader.le16();          // 0x04
  out->sub_version = reader.u8();
  out->sub_index = reader.u8();
  reader.skip(4);                        // 0x08 rsvd
  out->feature = reader.le32();          // 0x0C
  out->month = reader.u8();              // 0x10
  out->day = reader.u8();
  out->hour = reader.u8();
  out->minute = reader.u8();
  out->year = reader.le16();             // 0x14
  reader.skip(2);
  out->mem_usage = reader.u8();          // 0x18
  reader.skip(3);
  out->h2c_format_version = reader.le16();   // 0x1C
  reader.skip(2);
  out->dmem_address = reader.le32();     // 0x20
  const uint32_t dmem_size = reader.le32();
  reader.skip(8);                        // 0x28 rsvd6, rsvd7
  const uint32_t imem_size = reader.le32();  // 0x30
  const uint32_t emem_size_field = reader.le32();
  out->emem_address = reader.le32();
  out->imem_address = reader.le32();
  if (!reader.ok()) return RtwFirmwareStatus::Truncated;
  if (expected_signature && out->signature != expected_signature) return RtwFirmwareStatus::BadSignature;
  const bool has_emem = (out->mem_usage & 0x10) != 0;
  const uint32_t emem_size = has_emem ? emem_size_field : 0;

  // check_firmware_size: header + each block + its checksum must be the file, exactly. (64-bit sums of 32-bit sizes cannot wrap.)
  const uint64_t expected = uint64_t{kRtw88HeaderSize} + dmem_size + kRtw88ChecksumSize + imem_size + kRtw88ChecksumSize +
                            (emem_size ? uint64_t{emem_size} + kRtw88ChecksumSize : 0);
  if (expected > file.size()) return RtwFirmwareStatus::Truncated;
  if (expected != file.size()) return RtwFirmwareStatus::SizeMismatch;
  if (dmem_size == 0 || imem_size == 0) return RtwFirmwareStatus::SizeMismatch;

  uint64_t offset = kRtw88HeaderSize;
  RtwFirmwareStatus status = take_block(file, &offset, dmem_size, &out->dmem);
  if (status != RtwFirmwareStatus::Ok) return status;
  status = take_block(file, &offset, imem_size, &out->imem);
  if (status != RtwFirmwareStatus::Ok) return status;
  if (emem_size) {
    status = take_block(file, &offset, emem_size, &out->emem);
    if (status != RtwFirmwareStatus::Ok) return status;
  }
  return RtwFirmwareStatus::Ok;
}

// ---------------------------------------------------------------- rtw89

namespace {

constexpr uint8_t kMfwSignature = 0xFF;
constexpr uint32_t kMfwHeaderSize = 16, kMfwInfoSize = 16, kElementHeaderSize = 32, kElementAlign = 16;
constexpr uint8_t kPoolMagic[8] = {'M', 'S', 'S', 'K', 'P', 'O', 'O', 'L'};

uint32_t bits(uint32_t word, uint32_t high, uint32_t low) { return (word >> low) & ((high - low == 31) ? 0xFFFFFFFFu : ((1u << (high - low + 1)) - 1)); }

uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

bool header_parsed(uint8_t type) {
  return type == static_cast<uint8_t>(Rtw89FirmwareType::Normal) || type == static_cast<uint8_t>(Rtw89FirmwareType::Wowlan) ||
         type == static_cast<uint8_t>(Rtw89FirmwareType::NormalCe);
}

}  // namespace

RtwFirmwareStatus rtw89_parse_firmware_image(ConstByteView data, Rtw89FirmwareImage* out) {
  *out = Rtw89FirmwareImage{};
  out->whole = data;
  ByteReader reader(data);
  uint32_t w[12] = {};
  for (uint32_t i = 0; i < 8; ++i) w[i] = reader.le32();
  if (!reader.ok()) return RtwFirmwareStatus::Truncated;
  out->header_version = static_cast<uint8_t>(bits(w[3], 31, 24));
  if (out->header_version > 1) return RtwFirmwareStatus::BadLayout;
  uint32_t fixed = 32;
  if (out->header_version == 1) {
    for (uint32_t i = 8; i < 12; ++i) w[i] = reader.le32();
    if (!reader.ok()) return RtwFirmwareStatus::Truncated;
    fixed = 48;
  }
  out->chip_id = static_cast<uint16_t>(bits(w[0], 31, 16));
  out->major = static_cast<uint8_t>(bits(w[1], 7, 0));
  out->minor = static_cast<uint8_t>(bits(w[1], 15, 8));
  out->sub_version = static_cast<uint8_t>(bits(w[1], 23, 16));
  out->sub_index = static_cast<uint8_t>(bits(w[1], 31, 24));
  out->commit_id = w[2];
  out->month = static_cast<uint8_t>(bits(w[4], 7, 0));
  out->day = static_cast<uint8_t>(bits(w[4], 15, 8));
  out->hour = static_cast<uint8_t>(bits(w[4], 23, 16));
  out->minute = static_cast<uint8_t>(bits(w[4], 31, 24));
  out->year = w[5];
  out->section_count = bits(w[6], 15, 8);
  out->dynamic_header = bits(w[7], 16, 16) != 0;
  out->command_version = static_cast<uint8_t>(bits(w[7], 31, 24));
  if (out->section_count == 0 || out->section_count > kRtw89MaxSections) return RtwFirmwareStatus::BadLayout;

  const uint64_t base = fixed + uint64_t{16} * out->section_count;
  if (base > data.size()) return RtwFirmwareStatus::Truncated;
  uint64_t header_length = base;
  if (out->dynamic_header) {
    // The dynamic header (struct rtw89_fw_dynhdr_hdr) starts with its own length.
    ByteReader dynamic(data.subview(base));
    const uint32_t dynamic_length = dynamic.le32();
    if (!dynamic.ok()) return RtwFirmwareStatus::Truncated;
    header_length = base + dynamic_length;
  }
  if (out->header_version == 0) {
    // Version 0 also states the whole length in word 3 (bits 23:16); it has to agree.
    const uint32_t stated = bits(w[3], 23, 16);
    if (stated != header_length) return RtwFirmwareStatus::BadLayout;
  }
  if (header_length > data.size()) return RtwFirmwareStatus::Truncated;
  out->header_length = static_cast<uint32_t>(header_length);

  // The section headers, then where the contents lie.
  uint64_t position = header_length;
  uint64_t signatures_min = 0;
  int32_t pool_after = -1;               // the security section whose contents the key pool follows
  for (uint32_t i = 0; i < out->section_count; ++i) {
    ByteReader section(data.subview(fixed + uint64_t{16} * i, 16));
    const uint32_t s0 = section.le32(), s1 = section.le32(), s2 = section.le32();
    Rtw89Section& s = out->sections[i];
    s.download_address = s0;
    s.type = static_cast<uint8_t>(bits(s1, 27, 24));
    s.checksum = bits(s1, 28, 28) != 0;
    s.redownload = bits(s1, 29, 29) != 0;
    uint64_t length = bits(s1, 23, 0);
    if (s.checksum) length += kRtw89SectionChecksumLength;
    if (s.type == kRtw89SecuritySectionType) {
      s.mssc = static_cast<uint8_t>(bits(s2, 7, 0));
      if (s.mssc == 0xFF) {
        if (pool_after < 0) pool_after = static_cast<int32_t>(i);
        signatures_min += sizeof kPoolMagic;
      } else {
        signatures_min += uint64_t{s.mssc} * kRtw89SignatureLength;
      }
    }
    if (length > data.size() - position) return RtwFirmwareStatus::Truncated;
    position += length;
  }
  const uint64_t trailing = data.size() - position;
  if (trailing < signatures_min) return RtwFirmwareStatus::Truncated;
  if (signatures_min == 0 && trailing != 0) return RtwFirmwareStatus::SizeMismatch;
  out->signature_bytes = static_cast<uint32_t>(trailing);

  // Hand out the views. With a key pool, the pool sits right after the first pooled security section and the remaining sections follow it.
  position = header_length;
  for (uint32_t i = 0; i < out->section_count; ++i) {
    Rtw89Section& s = out->sections[i];
    ByteReader section(data.subview(fixed + uint64_t{16} * i, 16));
    section.skip(4);
    uint64_t length = bits(section.le32(), 23, 0);
    if (s.checksum) length += kRtw89SectionChecksumLength;
    s.data = data.subview(position, length);
    position += length;
    if (static_cast<int32_t>(i) == pool_after) {
      ConstByteView magic;
      if (!data.try_subview(position, sizeof kPoolMagic, &magic) || !magic.equals(ConstByteView(kPoolMagic))) return RtwFirmwareStatus::BadLayout;
      position += trailing;
    }
  }
  return RtwFirmwareStatus::Ok;
}

RtwFirmwareStatus Rtw89FirmwareFile::parse(ConstByteView file) {
  *this = Rtw89FirmwareFile{};
  file_ = file;
  if (file.empty()) return RtwFirmwareStatus::Truncated;
  if (file[0] != kMfwSignature) {
    // An old single-firmware file.
    entries_[0] = Rtw89FirmwareEntry{0xFF, static_cast<uint8_t>(Rtw89FirmwareType::Normal), 0, file};
    entry_count_ = 1;
    Rtw89FirmwareImage image;
    const RtwFirmwareStatus status = rtw89_parse_firmware_image(file, &image);
    return status == RtwFirmwareStatus::BadLayout ? RtwFirmwareStatus::BadSignature : status;
  }
  multi_ = true;
  if (file.size() < kMfwHeaderSize) return RtwFirmwareStatus::Truncated;
  const uint32_t count = file[1];
  if (count == 0 || count > kRtw89MaxEntries) return RtwFirmwareStatus::BadLayout;
  for (uint32_t i = 0; i < 4; ++i) container_version_[i] = file[4 + i];
  const uint64_t table_end = kMfwHeaderSize + uint64_t{kMfwInfoSize} * count;
  if (table_end > file.size()) return RtwFirmwareStatus::Truncated;

  uint64_t end = table_end;
  for (uint32_t i = 0; i < count; ++i) {
    ByteReader info(file.subview(kMfwHeaderSize + uint64_t{kMfwInfoSize} * i, kMfwInfoSize));
    Rtw89FirmwareEntry& entry = entries_[i];
    entry.cv = info.u8();
    entry.type = info.u8();
    entry.mp = info.u8();
    info.skip(1);
    const uint32_t shift = info.le32();
    const uint32_t size = info.le32();
    if (shift < table_end || size == 0) return RtwFirmwareStatus::BadLayout;
    if (!file.try_subview(shift, size, &entry.data)) return RtwFirmwareStatus::Truncated;
    if (uint64_t{shift} + size > end) end = uint64_t{shift} + size;
    if (header_parsed(entry.type)) {
      Rtw89FirmwareImage image;
      const RtwFirmwareStatus status = rtw89_parse_firmware_image(entry.data, &image);
      if (status != RtwFirmwareStatus::Ok) return status;
    }
  }
  entry_count_ = count;
  // No two firmwares may overlap.
  for (uint32_t i = 0; i < count; ++i)
    for (uint32_t j = i + 1; j < count; ++j) {
      const uint8_t* a = entries_[i].data.data();
      const uint8_t* b = entries_[j].data.data();
      if (a < b + entries_[j].data.size() && b < a + entries_[i].data.size()) return RtwFirmwareStatus::BadLayout;
    }

  // rtw89_mfw_get_size takes the last entry's end; the largest end is the same on every real file and cannot point into a firmware.
  elements_start_ = align_up(end, kElementAlign);
  uint64_t offset = elements_start_;
  while (offset < file.size()) {
    ByteReader header(file.subview(offset));
    header.le32();
    const uint32_t size = header.le32();
    if (!header.ok() || file.size() - offset < kElementHeaderSize) return RtwFirmwareStatus::Truncated;
    if (size > file.size() - offset - kElementHeaderSize) return RtwFirmwareStatus::Truncated;
    ++element_count_;
    offset = align_up(offset + kElementHeaderSize + size, kElementAlign);
  }
  return RtwFirmwareStatus::Ok;
}

const Rtw89FirmwareEntry* Rtw89FirmwareFile::find(Rtw89FirmwareType type, uint8_t cv) const {
  for (uint32_t i = 0; i < entry_count_; ++i) {
    const Rtw89FirmwareEntry& entry = entries_[i];
    if (entry.type == static_cast<uint8_t>(type) && entry.mp == 0 && (entry.cv == cv || entry.cv == 0xFF)) return &entry;
  }
  return nullptr;
}

bool Rtw89FirmwareFile::element(uint32_t index, Rtw89FirmwareElement* out) const {
  if (index >= element_count_) return false;
  uint64_t offset = elements_start_;
  for (uint32_t i = 0; offset < file_.size(); ++i) {
    ByteReader header(file_.subview(offset));
    const uint32_t id = header.le32();
    const uint32_t size = header.le32();
    const uint32_t version = header.le32();
    if (!header.ok() || file_.size() - offset < kElementHeaderSize || size > file_.size() - offset - kElementHeaderSize) return false;
    if (i == index) {
      *out = Rtw89FirmwareElement{id, version, file_.subview(offset + kElementHeaderSize, size)};
      return true;
    }
    offset = align_up(offset + kElementHeaderSize + size, kElementAlign);
  }
  return false;
}

}  // namespace hardware
