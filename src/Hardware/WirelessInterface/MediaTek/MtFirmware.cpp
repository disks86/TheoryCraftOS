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

#include "Hardware/WirelessInterface/MediaTek/MtFirmware.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {

// A text field of the file as a C string: up to the first NUL or line end, printable ASCII only (anything else ends it). `out` has room for the terminator.
void copy_text(ConstByteView field, ArrayView<char> out) {
  out.fill('\0');
  uint64_t n = 0;
  for (uint8_t c : field) {
    if (c < 0x20 || c > 0x7E || n + 1 >= out.size()) break;
    out[n++] = static_cast<char>(c);
  }
}

bool fail(const char** error, const char* message) {
  if (error) *error = message;
  return false;
}

}  // namespace

bool mt792x_firmware_names(uint32_t chip_id, const char** patch, const char** ram) {
  switch (chip_id) {
    case 0x7961:
      *patch = "mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin";
      *ram = "mediatek/WIFI_RAM_CODE_MT7961_1.bin";
      return true;
    case 0x7922:
      *patch = "mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin";
      *ram = "mediatek/WIFI_RAM_CODE_MT7922_1.bin";
      return true;
    case 0x7920:
      *patch = "mediatek/WIFI_MT7961_patch_mcu_1a_2_hdr.bin";
      *ram = "mediatek/WIFI_RAM_CODE_MT7961_1a.bin";
      return true;
    case 0x7925:
      *patch = "mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin";
      *ram = "mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin";
      return true;
    default:
      return false;
  }
}

// --- ROM patch ----------------------------------------------------------------------------------------------------------------------------------------------------------

bool MtConnacPatch::parse(ConstByteView file, const char** error) {
  *this = MtConnacPatch();
  if (error) *error = nullptr;
  if (file.size() < kHeaderBytes) return fail(error, "patch file shorter than its header");
  ByteReader header(file.first(kHeaderBytes));
  copy_text(header.take(16), ArrayView<char>(build_date_));
  copy_text(header.take(4), ArrayView<char>(platform_));
  hw_sw_version_ = header.be32();
  patch_version_ = header.be32();
  header.skip(2 + 2);                  // checksum, rsv0
  header.skip(4 + 4 + 4);              // desc.patch_ver, desc.subsys, desc.feature
  const uint32_t regions = header.be32();
  if (!header.ok()) return fail(error, "patch header truncated");
  if (regions == 0) return fail(error, "patch has no regions");
  if (regions > kMaxSections) return fail(error, "patch has too many regions");
  const uint64_t table_end = kHeaderBytes + static_cast<uint64_t>(regions) * kSectionBytes;
  if (table_end > file.size()) return fail(error, "patch section table runs past the end of the file");
  for (uint32_t i = 0; i < regions; ++i) {
    ByteReader sec(file.subview(kHeaderBytes + static_cast<uint64_t>(i) * kSectionBytes, kSectionBytes));
    MtPatchSection& s = sections_[i];
    s.type = sec.be32();
    s.file_offset = sec.be32();
    sec.skip(4);                       // size (the whole section; the download uses info.len)
    s.address = sec.be32();
    s.length = sec.be32();
    s.sec_key_idx = sec.be32();
    if (!sec.ok()) return fail(error, "patch section truncated");
    if ((s.type & kSectionTypeMask) != kSectionTypeInfo) return fail(error, "patch section is not of the INFO type");
    if (s.length == 0) return fail(error, "patch section is empty");
    if (s.file_offset < table_end) return fail(error, "patch section data overlaps the header");
    if (!file.try_subview(s.file_offset, s.length, &s.data)) return fail(error, "patch section data runs past the end of the file");
  }
  count_ = regions;
  return true;
}

uint32_t MtConnacPatch::download_mode(uint32_t index) const {
  uint32_t mode = mt_dl_mode::kNeedResponse;
  const uint32_t info = section(index).sec_key_idx;
  if (info == kSecNotSupported) return mode;
  switch (info >> 24) {
    case kEncPlain: break;
    case kEncAes:
      mode |= mt_dl_mode::kEncrypt | ((info & 0xFF) << mt_dl_mode::kKeyIndexShift & mt_dl_mode::kKeyIndexMask) | mt_dl_mode::kResetSecIv;
      break;
    case kEncScramble:
      mode |= mt_dl_mode::kEncrypt | mt_dl_mode::kEncryptModeSel | mt_dl_mode::kResetSecIv;
      break;
    default: break;                    // Linux logs "Encryption type not support" and sends it as plain
  }
  return mode;
}

// --- RAM code -----------------------------------------------------------------------------------------------------------------------------------------------------------

bool MtRamRegion::downloaded() const { return (feature_set & MtConnacRam::kFeatureNonDownload) == 0; }

bool MtConnacRam::parse(ConstByteView file, const char** error) {
  *this = MtConnacRam();
  if (error) *error = nullptr;
  if (file.size() < kTrailerBytes) return fail(error, "RAM code shorter than its trailer");
  const uint64_t trailer_at = file.size() - kTrailerBytes;
  ByteReader trailer(file.subview(trailer_at));
  chip_id_ = trailer.u8();
  eco_code_ = trailer.u8();
  const uint8_t regions = trailer.u8();
  format_version_ = trailer.u8();
  format_flag_ = trailer.u8();
  trailer.skip(2);
  copy_text(trailer.take(10), ArrayView<char>(version_));
  copy_text(trailer.take(15), ArrayView<char>(build_date_));
  crc_ = trailer.le32();
  if (!trailer.ok()) return fail(error, "RAM code trailer truncated");
  if (regions == 0) return fail(error, "RAM code has no regions");
  if (regions > kMaxRegions) return fail(error, "RAM code has too many regions");
  const uint64_t table_bytes = static_cast<uint64_t>(regions) * kRegionBytes;
  if (table_bytes > trailer_at) return fail(error, "RAM code region table runs past the start of the file");
  const uint64_t table_at = trailer_at - table_bytes;
  uint64_t offset = 0;
  for (uint32_t i = 0; i < regions; ++i) {
    ByteReader reg(file.subview(table_at + static_cast<uint64_t>(i) * kRegionBytes, kRegionBytes));
    MtRamRegion& r = regions_[i];
    reg.skip(4 + 4 + 4 + 4);           // decomp_crc, decomp_len, decomp_blk_sz, rsv
    r.address = reg.le32();
    r.length = reg.le32();
    r.feature_set = reg.u8();
    r.type = reg.u8();
    if (!reg.ok()) return fail(error, "RAM code region truncated");
    // the data of all regions (sent or not) lies in order before the table
    if (r.length > table_at - offset) return fail(error, "RAM code region data runs into the region table");
    r.file_offset = static_cast<uint32_t>(offset);
    r.data = file.subview(offset, r.length);
    offset += r.length;
    if (r.downloaded() && r.length == 0) return fail(error, "RAM code region is empty");
  }
  count_ = regions;
  return true;
}

uint32_t MtConnacRam::download_mode(uint32_t index) const {
  const uint8_t features = region(index).feature_set;
  uint32_t mode = 0;
  if (features & kFeatureEncrypt) mode |= mt_dl_mode::kEncrypt | mt_dl_mode::kResetSecIv;
  if (features & kFeatureEncryptMode) mode |= mt_dl_mode::kEncryptModeSel;
  mode |= (static_cast<uint32_t>(features & kFeatureKeyIndexMask) >> 1) << mt_dl_mode::kKeyIndexShift;
  mode |= mt_dl_mode::kNeedResponse;
  return mode;
}

uint32_t MtConnacRam::start_address() const {
  uint32_t address = 0;
  for (uint32_t i = 0; i < count_; ++i) {
    const MtRamRegion& r = region(i);
    if (r.downloaded() && (r.feature_set & kFeatureOverrideAddress)) address = r.address;
  }
  return address;
}

// --- USB firmware -------------------------------------------------------------------------------------------------------------------------------------------------------

bool Mt76UsbFirmware::parse(ConstByteView file, const char** error) {
  *this = Mt76UsbFirmware();
  if (error) *error = nullptr;
  ByteReader r(file);
  const uint32_t ilm = r.le32();
  const uint32_t dlm = r.le32();
  build_version_ = r.le16();
  firmware_version_ = r.le16();
  r.skip(4);
  copy_text(r.take(16), ArrayView<char>(build_time_));
  if (!r.ok()) return fail(error, "USB firmware shorter than its header");
  // mt76x2u_mcu_load_firmware and mt7601u_load_firmware refuse a file whose size is not exactly header + ILM + DLM
  const uint64_t body = file.size() - kHeaderBytes;
  if (ilm > body || dlm != body - ilm) return fail(error, "USB firmware size does not match its ILM and DLM lengths");
  if (ilm <= kIvbBytes) return fail(error, "USB firmware ILM is too short");
  ilm_ = file.subview(kHeaderBytes, ilm);
  dlm_ = file.subview(kHeaderBytes + ilm, dlm);
  return true;
}

bool Mt76UsbFirmware::version_text(ArrayView<char> out) const {
  const uint32_t parts[3] = {static_cast<uint32_t>(firmware_version_ >> 12 & 0xF), static_cast<uint32_t>(firmware_version_ >> 8 & 0xF), static_cast<uint32_t>(firmware_version_ & 0xF)};
  uint64_t n = 0;
  auto put = [&](char c) {
    if (n + 1 >= out.size()) return false;
    out[n++] = c;
    return true;
  };
  auto put_hex = [&](uint32_t v) {
    char digits[8];
    int count = 0;
    do {
      digits[count++] = "0123456789abcdef"[v & 0xF];
      v >>= 4;
    } while (v && count < 8);
    while (count) if (!put(digits[--count])) return false;
    return true;
  };
  if (out.empty()) return false;
  bool ok = put_hex(parts[0]) && put('.') && put_hex(parts[1]) && put('.') && put_hex(parts[2]);
  const char* build = " (build ";
  for (const char* c = build; ok && *c; ++c) ok = put(*c);
  ok = ok && put_hex(build_version_) && put(')');
  out[n] = '\0';
  return ok;
}

}  // namespace hardware
