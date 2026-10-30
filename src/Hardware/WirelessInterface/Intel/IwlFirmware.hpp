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


// An iwlwifi firmware file ("iwlwifi-9260-th-b0-jf-b0-46.ucode", from linux-firmware): a header (zero, the magic "IWL\n", a human readable version, the version and build numbers)
// and a list of TLVs (type, length, data padded to 4 bytes). Parsed after iwl_parse_tlv_firmware of iwl-drv.c: the images' sections (each with the device address it loads at),
// the flags, the API and capability bit sets, the PHY configuration, the number of processors, the paging size, the firmware version, the command versions and the image loader.
// Types that are not needed (debug, region and trigger descriptions, ...) are skipped by their length. The file comes from disk and goes to a DMA-capable chip: every length is
// checked and the sections are views into the file (which has to stay valid).
//
// Sections of an image are listed in the file's order: the first processor's (LMAC) sections, a separator section (offset 0xFFFFCCCC), the second's (UMAC), a separator
// (0xAAAABBBB) and the paging sections.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

enum class IwlImage : uint8_t { Regular = 0, Init = 1, Wowlan = 2 };

struct IwlSection {
  uint32_t device_offset = 0;            // where in the device's memory it loads (or a separator)
  ConstByteView data;
  bool secure = false;                   // from a SECURE_SEC_* TLV (the chip's ROM checks it)
  bool is_cpu_separator() const { return device_offset == 0xFFFFCCCCu; }
  bool is_paging_separator() const { return device_offset == 0xAAAABBBBu; }
};

struct IwlCommandVersion {
  uint8_t command, group, command_version, notification_version;      // (the file's order: the command id first)
};

class IwlFirmware {
 public:
  static constexpr uint32_t kMaxSections = 64, kApiWords = 8;
  static constexpr uint32_t kMagic = 0x0a4c5749;

  explicit IwlFirmware(allocators::IAllocator& allocator) : regular_(allocator), init_(allocator), wowlan_(allocator) {}

  // Parses `file` (which must stay valid). False with a static message in *error for a file that is not whole.
  bool parse(ConstByteView file, const char** error);

  uint32_t version() const { return version_; }
  uint32_t api_version() const { return (version_ & 0x0000FF00) >> 8; }
  uint32_t build() const { return build_; }
  const char* human_readable() const { return human_readable_; }
  uint32_t flags() const { return flags_; }
  uint32_t phy_config() const { return phy_config_; }
  uint32_t cpu_count() const { return cpu_count_; }
  uint32_t paging_size() const { return paging_size_; }
  // How many channel entries the firmware's scan request has room for (its layout depends on it).
  uint32_t scan_channels() const { return scan_channels_; }
  uint32_t probe_max_length() const { return probe_max_length_; }
  uint32_t fw_major() const { return fw_major_; }
  uint32_t fw_minor() const { return fw_minor_; }
  bool has_api(uint32_t bit) const { return bit / 32 < kApiWords && (api_[bit / 32] >> (bit % 32) & 1); }
  bool has_capability(uint32_t bit) const { return bit / 32 < kApiWords && (capa_[bit / 32] >> (bit % 32) & 1); }
  ConstByteView image_loader() const { return image_loader_; }
  uint32_t command_version_count() const { return static_cast<uint32_t>(command_versions_.size() / 4); }
  IwlCommandVersion command_version(uint32_t i) const {
    if (i >= command_version_count()) return IwlCommandVersion{0, 0, 0, 0};
    return IwlCommandVersion{command_versions_[4 * i], command_versions_[4 * i + 1], command_versions_[4 * i + 2], command_versions_[4 * i + 3]};
  }
  // The version of a command (0 if the firmware does not say).
  uint32_t command_version_of(uint8_t group, uint8_t command) const;

  const List<IwlSection>& sections(IwlImage image) const { return image == IwlImage::Regular ? regular_ : image == IwlImage::Init ? init_ : wowlan_; }
  bool has_image(IwlImage image) const { return sections(image).count() != 0; }
  // The sections of one part of an image: 0 the first processor's, 1 the second's, 2 paging. `out` is filled in the file's order; returns how many.
  uint32_t part(IwlImage image, uint32_t which, IwlSection* out, uint32_t capacity) const;

 private:
  bool add_section(List<IwlSection>& list, ConstByteView tlv, bool secure, const char** error);

  List<IwlSection> regular_, init_, wowlan_;
  uint32_t scan_channels_ = 40;
  uint32_t version_ = 0, build_ = 0, flags_ = 0, phy_config_ = 0, cpu_count_ = 1, paging_size_ = 0, probe_max_length_ = 0, fw_major_ = 0, fw_minor_ = 0;
  char human_readable_[65] = {};
  uint32_t api_[kApiWords] = {}, capa_[kApiWords] = {};
  ConstByteView image_loader_, command_versions_;
};

}  // namespace hardware
