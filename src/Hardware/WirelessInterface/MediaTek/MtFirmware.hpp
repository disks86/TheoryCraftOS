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

// The firmware files of the MediaTek Wi-Fi chips, from linux-firmware, parsed after the Linux mt76 driver:
//
// - The connac2 ROM patch ("mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin", MT7922's and MT7925's): struct mt76_connac2_patch_hdr (96 bytes: build date, platform "ALPS",
//   hardware/software and patch versions, then a descriptor with the number of regions), followed by one struct mt76_connac2_patch_sec (64 bytes) per region: a type
//   (PATCH_SEC_TYPE_INFO in the low 16 bits), the offset of the region's data in the file, and the address, length and encryption ("sec_key_idx") it loads with. All
//   fields are big endian. After mt76_connac2_load_patch and mt76_connac2_get_data_mode of mt76_connac_mcu.c.
// - The connac2 RAM code ("mediatek/WIFI_RAM_CODE_MT7961_1.bin", MT7922's, and MT7925's, which uses the same layout): struct mt76_connac2_fw_trailer (36 bytes) at the
//   very end of the file (chip, ECO, number of regions, the firmware version and build date as text), and right before it one struct mt76_connac2_fw_region (40 bytes)
//   per region (address, length, feature set). The regions' data lie back to back from the start of the file, in region order. Little endian. After
//   mt76_connac2_load_ram and mt76_connac_mcu_send_ram_firmware / mt76_connac_mcu_gen_dl_mode.
// - The USB firmware of the older chips ("mediatek/mt7662u.bin", "mediatek/mt7610u.bin", "mt7601u.bin"): struct mt76x02_fw_header of mt76x02_mcu.h (the same as
//   mt7601u's struct mt76_fw_header, 32 bytes: the ILM and DLM lengths, build and firmware versions, build time), then the ILM and the DLM, and nothing else.
//
// All three layouts were checked against the files of linux-firmware 2026 (MT7961, MT7922, MT7925 patch and RAM code; mt7662u, mt7610u, mt7601u). The files come from disk
// and go to a chip: every count, offset and length is checked, and the parsed regions are views into the file (which has to stay valid while they are used).
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// --- the download modes of TARGET_ADDRESS_LEN_REQ / PATCH_START_REQ (DL_MODE_* of mt76_connac_mcu.h)
namespace mt_dl_mode {
constexpr uint32_t kEncrypt = 1u << 0, kKeyIndexShift = 1, kKeyIndexMask = 3u << 1, kResetSecIv = 1u << 3, kWorkingPdaCr4 = 1u << 4, kValidRamEntry = 1u << 5,
                   kEncryptModeSel = 1u << 6, kNeedResponse = 1u << 31;
}  // namespace mt_dl_mode

// The ROM patch and RAM code files of a connac2/3 chip by the chip id it reports in MT_HW_CHIPID (mt792x_patch_name / mt792x_ram_name of mt792x.h): 0x7961 (MT7921),
// 0x7922 (MT7922), 0x7920 (MT7920), 0x7925 (MT7925). False for another id.
[[nodiscard]] bool mt792x_firmware_names(uint32_t chip_id, const char** patch, const char** ram);

// --- ROM patch

struct MtPatchSection {
  uint32_t type = 0;           // PATCH_SEC_TYPE_INFO (2) in the low 16 bits
  uint32_t file_offset = 0;    // where its data starts in the file ("offs")
  uint32_t address = 0;        // where it loads (info.addr)
  uint32_t length = 0;         // info.len
  uint32_t sec_key_idx = 0;    // info.sec_key_idx: the encryption type in bits 24-31, the AES key or scramble info below
  ConstByteView data;
};

class MtConnacPatch {
 public:
  static constexpr uint32_t kHeaderBytes = 96, kSectionBytes = 64, kMaxSections = 16;
  static constexpr uint32_t kSectionTypeInfo = 0x2, kSectionTypeMask = 0xFFFF;
  static constexpr uint32_t kEncPlain = 0x00, kEncAes = 0x01, kEncScramble = 0x02, kSecNotSupported = 0xFFFFFFFFu;

  // Parses `file` (which must stay valid). False with a static message in *error when it is not whole.
  [[nodiscard]] bool parse(ConstByteView file, const char** error);

  const char* build_date() const { return build_date_; }   // "20260224110909a" (16 characters, the last usually '\n', which is dropped)
  const char* platform() const { return platform_; }       // "ALPS"
  uint32_t hw_sw_version() const { return hw_sw_version_; }
  uint32_t patch_version() const { return patch_version_; }
  uint32_t section_count() const { return count_; }
  const MtPatchSection& section(uint32_t index) const { return ArrayView<const MtPatchSection>(sections_).first(count_)[index]; }
  // The DL_MODE_* word the chip downloads section `index` with (mt76_connac2_get_data_mode, for MT7921/MT7922/MT7925).
  uint32_t download_mode(uint32_t index) const;

 private:
  char build_date_[17] = {};
  char platform_[5] = {};
  uint32_t hw_sw_version_ = 0, patch_version_ = 0, count_ = 0;
  MtPatchSection sections_[kMaxSections];
};

// --- RAM code

struct MtRamRegion {
  uint32_t address = 0;
  uint32_t length = 0;
  uint32_t file_offset = 0;    // where its data starts (the sum of the lengths of the regions before it)
  uint8_t feature_set = 0;     // FW_FEATURE_*
  uint8_t type = 0;
  ConstByteView data;
  bool downloaded() const;     // false for a FW_FEATURE_NON_DL region (it is in the file but not sent)
};

class MtConnacRam {
 public:
  static constexpr uint32_t kTrailerBytes = 36, kRegionBytes = 40, kMaxRegions = 16;
  // FW_FEATURE_* (the region's feature_set)
  static constexpr uint8_t kFeatureEncrypt = 1u << 0, kFeatureKeyIndexMask = 3u << 1, kFeatureEncryptMode = 1u << 4, kFeatureOverrideAddress = 1u << 5,
                           kFeatureNonDownload = 1u << 6;
  // FW_START_REQ options
  static constexpr uint32_t kStartOverride = 1u << 0, kStartWorkingPdaCr4 = 1u << 2;

  [[nodiscard]] bool parse(ConstByteView file, const char** error);

  uint8_t chip_id() const { return chip_id_; }
  uint8_t eco_code() const { return eco_code_; }
  uint8_t format_version() const { return format_version_; }
  uint8_t format_flag() const { return format_flag_; }
  const char* version() const { return version_; }           // fw_ver, 10 characters ("____010000")
  const char* build_date() const { return build_date_; }     // 15 characters ("20260224110949")
  uint32_t crc() const { return crc_; }
  uint32_t region_count() const { return count_; }
  const MtRamRegion& region(uint32_t index) const { return ArrayView<const MtRamRegion>(regions_).first(count_)[index]; }
  // mt76_connac_mcu_gen_dl_mode for MT7921/MT7922/MT7925 (is_wa false).
  uint32_t download_mode(uint32_t index) const;
  // FW_START_REQ: the address of the last region with FW_FEATURE_OVERRIDE_ADDR (0 when none) and the option word.
  uint32_t start_address() const;
  uint32_t start_option() const { return start_address() ? kStartOverride : 0; }

 private:
  uint8_t chip_id_ = 0, eco_code_ = 0, format_version_ = 0, format_flag_ = 0;
  char version_[11] = {};
  char build_date_[16] = {};
  uint32_t crc_ = 0, count_ = 0;
  MtRamRegion regions_[kMaxRegions];
};

// --- USB firmware of MT76x2U / MT76x0U / MT7601U

class Mt76UsbFirmware {
 public:
  static constexpr uint32_t kHeaderBytes = 32;
  static constexpr uint32_t kIvbBytes = 0x40;   // MT_MCU_IVB_SIZE: the interrupt vector block at the start of the ILM (mt76x0u and mt7601u send it separately)

  [[nodiscard]] bool parse(ConstByteView file, const char** error);

  uint32_t ilm_length() const { return static_cast<uint32_t>(ilm_.size()); }
  uint32_t dlm_length() const { return static_cast<uint32_t>(dlm_.size()); }
  uint16_t build_version() const { return build_version_; }
  uint16_t firmware_version() const { return firmware_version_; }   // major in bits 12-15, minor in 8-11, patch in 0-3
  const char* build_time() const { return build_time_; }
  ConstByteView ilm() const { return ilm_; }
  ConstByteView dlm() const { return dlm_; }
  // "0.1.0 (build 7640)" into `out`; false if it does not fit.
  [[nodiscard]] bool version_text(ArrayView<char> out) const;

 private:
  uint16_t build_version_ = 0, firmware_version_ = 0;
  char build_time_[17] = {};
  ConstByteView ilm_, dlm_;
};

}  // namespace hardware
