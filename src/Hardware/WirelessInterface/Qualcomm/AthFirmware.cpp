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

#include "Hardware/WirelessInterface/Qualcomm/AthFirmware.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware::ath {

namespace {

constexpr uint64_t kIeHeader = 8;

uint64_t align4(uint64_t n) { return (n + 3) & ~uint64_t{3}; }

// The body after the magic (its NUL included, padded to 4 bytes), or false if the file does not begin with it.
bool skip_magic(ConstByteView file, StringView magic, ConstByteView* body) {
  const uint64_t with_nul = magic.size() + 1;
  if (file.size() < with_nul) return false;
  if (!file.first(magic.size()).equals(magic.bytes()) || file[magic.size()] != 0) return false;
  const uint64_t padded = align4(with_nul);
  if (file.size() < padded) return false;
  *body = file.subview(padded);
  return true;
}

// One IE at the front of `rest`. The drivers loop while more than a header is left, so a trailing header of 8 bytes or less is ignored (End). `strict` is the board
// files' rule (the padded length must fit); otherwise only the data must fit and missing padding at the very end is accepted.
enum class Step : uint8_t { Ie, End, Bad };
Step next_ie(ConstByteView* rest, bool strict, uint32_t* id, ConstByteView* data) {
  if (rest->size() <= kIeHeader) return Step::End;
  ByteReader in(*rest);
  *id = in.le32();
  const uint32_t length = in.le32();
  const uint64_t left = in.remaining();
  if (strict ? align4(length) > left : length > left) return Step::Bad;
  *data = rest->subview(kIeHeader, length);
  const uint64_t step = align4(length);
  *rest = step >= left ? ConstByteView() : rest->subview(kIeHeader + step);
  return Step::Ie;
}

uint32_t le32_of(ConstByteView data) {
  ByteReader in(data);
  return in.le32();
}

uint64_t feature_bits(ConstByteView data) {
  uint64_t bits = 0;
  for (uint64_t i = 0; i < data.size() && i < 8; ++i) bits |= uint64_t{data[i]} << (8 * i);
  return bits;
}

void append_hex(AthBoardName* out, uint32_t value, uint32_t min_digits, bool* ok) {
  char digits[8];
  uint32_t n = 0;
  do {
    const uint32_t d = value & 0xF;
    digits[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
    value >>= 4;
  } while (value && n < 8);
  while (n < min_digits && n < 8) digits[n++] = '0';
  for (uint32_t i = n; i > 0; --i) *ok = *ok && out->push_back(digits[i - 1]);
}

void append(AthBoardName* out, StringView text, bool* ok) { *ok = *ok && out->append(text); }
void append_dec(AthBoardName* out, uint32_t value, bool* ok) { *ok = *ok && out->append_uint(value); }

void append_variant(AthBoardName* out, const AthBoardIds& ids, bool with_variant, bool* ok) {
  if (!with_variant || ids.variant.empty()) return;
  append(out, ",variant=", ok);
  append(out, ids.variant, ok);
}

void append_pci_ids(AthBoardName* out, const AthBoardIds& ids, bool* ok) {
  append(out, ",vendor=", ok);
  append_hex(out, ids.vendor, 4, ok);
  append(out, ",device=", ok);
  append_hex(out, ids.device, 4, ok);
  append(out, ",subsystem-vendor=", ok);
  append_hex(out, ids.subsystem_vendor, 4, ok);
  append(out, ",subsystem-device=", ok);
  append_hex(out, ids.subsystem_device, 4, ok);
}

// ath10k_core_parse_bd_ie_board / ath11k_core_parse_bd_ie_board: the DATA after a NAME equal to `name`.
AthBoardFind find_in_entry(ConstByteView entry, StringView name, ConstByteView* data) {
  bool matched = false;
  ConstByteView rest = entry;
  uint32_t id = 0;
  ConstByteView value;
  for (;;) {
    const Step step = next_ie(&rest, true, &id, &value);
    if (step == Step::End) return AthBoardFind::NotFound;
    if (step == Step::Bad) return AthBoardFind::Invalid;
    if (id == kAthBoardSubIeName) {
      if (value.equals(name.bytes())) matched = true;
    } else if (id == kAthBoardSubIeData && matched) {
      *data = value;
      return AthBoardFind::Found;
    }
  }
}

}  // namespace

const char* ath_parse_name(AthParse result) {
  switch (result) {
    case AthParse::Ok: return "ok";
    case AthParse::BadMagic: return "not a file of this kind (magic)";
    case AthParse::Truncated: return "the file ends inside its header";
    case AthParse::BadLength: return "an element is longer than the rest of the file";
    case AthParse::NoImage: return "the file holds no firmware image";
  }
  return "?";
}

AthParse ath10k_parse_firmware(ConstByteView file, Ath10kFirmwareFile* out) {
  *out = Ath10kFirmwareFile{};
  if (file.size() < 4) return AthParse::Truncated;
  ConstByteView rest;
  if (!skip_magic(file, kAth10kFirmwareMagic, &rest)) return AthParse::BadMagic;
  uint32_t id = 0;
  ConstByteView data;
  for (;;) {
    const Step step = next_ie(&rest, false, &id, &data);
    if (step == Step::End) break;
    if (step == Step::Bad) return AthParse::BadLength;
    switch (static_cast<Ath10kFwIe>(id)) {
      case Ath10kFwIe::FwVersion: {
        // the driver keeps up to 31 characters; a longer version is skipped, as there
        if (data.size() > 31) break;
        out->version.clear();
        for (const uint8_t c : data) {
          if (c == 0) break;
          if (!out->version.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?')) break;
        }
        break;
      }
      case Ath10kFwIe::Timestamp:
        if (data.size() == 4) out->timestamp = le32_of(data);
        break;
      case Ath10kFwIe::Features: out->features = feature_bits(data); break;
      case Ath10kFwIe::FwImage: out->firmware = data; break;
      case Ath10kFwIe::OtpImage: out->otp = data; break;
      case Ath10kFwIe::WmiOpVersion:
        if (data.size() == 4) out->wmi_op = static_cast<Ath10kWmiOp>(le32_of(data));
        break;
      case Ath10kFwIe::HttOpVersion:
        if (data.size() == 4) out->htt_op = static_cast<Ath10kHttOp>(le32_of(data));
        break;
      case Ath10kFwIe::FwCodeSwapImage: out->code_swap = data; break;
      default: ++out->unknown_ies; break;
    }
  }
  if (out->firmware.empty()) return AthParse::NoImage;
  // ath10k_core_init_firmware_features: files before the WMI_OP_VERSION IE say it with feature bits (10.2 is bit 4)
  if (out->wmi_op == Ath10kWmiOp::Unset) {
    if (out->has_feature(4)) out->wmi_op = Ath10kWmiOp::V10_2;
    else if (out->has_feature(kAth10kFeatureWmi10x)) out->wmi_op = Ath10kWmiOp::V10_1;
    else out->wmi_op = Ath10kWmiOp::Main;
  }
  return AthParse::Ok;
}

AthParse ath_parse_qcom_firmware(ConstByteView file, StringView magic, AthQcomFirmwareFile* out) {
  *out = AthQcomFirmwareFile{};
  if (file.size() < 4) return AthParse::Truncated;
  ConstByteView rest;
  if (!skip_magic(file, magic, &rest)) return AthParse::BadMagic;
  uint32_t id = 0;
  ConstByteView data;
  for (;;) {
    const Step step = next_ie(&rest, false, &id, &data);
    if (step == Step::End) break;
    if (step == Step::Bad) return AthParse::BadLength;
    switch (static_cast<AthQcomFwIe>(id)) {
      case AthQcomFwIe::Timestamp:
        if (data.size() == 4) out->timestamp = le32_of(data);
        break;
      case AthQcomFwIe::Features: out->features = feature_bits(data); break;
      case AthQcomFwIe::AmssImage: out->amss = data; break;
      case AthQcomFwIe::M3Image: out->m3 = data; break;
      case AthQcomFwIe::AmssDualMacImage: out->amss_dual_mac = data; break;
      default: ++out->unknown_ies; break;
    }
  }
  return out->amss.empty() ? AthParse::NoImage : AthParse::Ok;
}

bool ath_is_elf32(ConstByteView image) {
  // e_ident: 0x7F 'E' 'L' 'F', class 1 (32-bit), data 1 (little-endian)
  static constexpr uint8_t kIdent[6] = {0x7F, 'E', 'L', 'F', 1, 1};
  return image.size() >= 52 && image.first(6).equals(ConstByteView(kIdent));
}

AthBoardFind ath_board_find(ConstByteView file, StringView magic, uint32_t ie_type, StringView name, ConstByteView* data) {
  ConstByteView rest;
  if (!skip_magic(file, magic, &rest)) return AthBoardFind::Invalid;
  uint32_t id = 0;
  ConstByteView entry;
  for (;;) {
    const Step step = next_ie(&rest, true, &id, &entry);
    if (step == Step::End) return AthBoardFind::NotFound;
    if (step == Step::Bad) return AthBoardFind::Invalid;
    if (id != ie_type) continue;
    const AthBoardFind found = find_in_entry(entry, name, data);
    if (found != AthBoardFind::NotFound) return found;   // found, or a damaged entry: the driver gives up there
  }
}

bool ath_board_for_each(ConstByteView file, StringView magic, AthBoardVisitor visit, void* context) {
  ConstByteView rest;
  if (!skip_magic(file, magic, &rest)) return false;
  uint32_t id = 0;
  ConstByteView entry;
  for (;;) {
    const Step step = next_ie(&rest, true, &id, &entry);
    if (step == Step::End) return true;
    if (step == Step::Bad) return false;
    // every NAME before a DATA names that data (several boards can share one data block)
    ConstByteView names[8];
    uint32_t name_count = 0;
    ConstByteView sub_rest = entry;
    uint32_t sub_id = 0;
    ConstByteView value;
    for (;;) {
      const Step sub = next_ie(&sub_rest, true, &sub_id, &value);
      if (sub == Step::End) break;
      if (sub == Step::Bad) return false;
      if (sub_id == kAthBoardSubIeName) {
        if (name_count < 8) names[name_count++] = value;
      } else if (sub_id == kAthBoardSubIeData) {
        for (uint32_t i = 0; i < name_count; ++i)
          if (!visit(context, id, StringView(names[i]), value)) return true;
        name_count = 0;
      }
    }
  }
}

bool ath10k_board_name(const AthBoardIds& ids, bool with_variant, bool with_chip_id, AthBoardName* out) {
  out->clear();
  bool ok = true;
  append(out, "bus=", &ok);
  append(out, ids.bus, &ok);
  if (ids.bmi_ids_valid) {
    append(out, ",bmi-chip-id=", &ok);
    append_dec(out, ids.bmi_chip_id, &ok);
    append(out, ",bmi-board-id=", &ok);
    append_dec(out, ids.bmi_board_id, &ok);
  } else if (ids.qmi_ids_valid) {
    append(out, ",qmi-board-id=", &ok);
    append_hex(out, ids.qmi_board_id, 1, &ok);
    if (with_chip_id) {
      append(out, ",qmi-chip-id=", &ok);
      append_hex(out, ids.qmi_chip_id, 1, &ok);
    } else {
      return ok;   // the driver leaves the variant off this form
    }
  } else {
    append_pci_ids(out, ids, &ok);
  }
  append_variant(out, ids, with_variant, &ok);
  return ok;
}

bool ath11k_board_name(const AthBoardIds& ids, AthBdfSearch search, bool with_variant, bool bus_type_mode, bool with_default, AthBoardName* out) {
  out->clear();
  bool ok = true;
  append(out, "bus=", &ok);
  append(out, ids.bus, &ok);
  if (search == AthBdfSearch::BusAndBoard) {
    if (bus_type_mode) return ok;
    append_pci_ids(out, ids, &ok);
    append(out, ",qmi-chip-id=", &ok);
    append_dec(out, ids.qmi_chip_id, &ok);
    append(out, ",qmi-board-id=", &ok);
    append_dec(out, ids.qmi_board_id, &ok);
  } else {
    append(out, ",qmi-chip-id=", &ok);
    append_dec(out, ids.qmi_chip_id, &ok);
    append(out, ",qmi-board-id=", &ok);
    append_dec(out, with_default ? kAthBoardIdDefault : ids.qmi_board_id, &ok);
  }
  append_variant(out, ids, with_variant, &ok);
  return ok;
}

AthBoardFind ath_select_board(ConstByteView file, AthFamily family, AthBdfSearch search, uint32_t ie_type, const AthBoardIds& ids, ConstByteView* data,
                              AthBoardName* tried) {
  AthBoardName scratch;
  AthBoardName& name = tried ? *tried : scratch;
  const StringView magic = family == AthFamily::Ath10k ? kAth10kBoardMagic : family == AthFamily::Ath11k ? kAth11kBoardMagic : kAth12kBoardMagic;
  const bool regdb = family != AthFamily::Ath10k && ie_type == kAthBoardIeRegdb;
  for (int attempt = 0; attempt < 2; ++attempt) {
    bool built;
    if (family == AthFamily::Ath10k) built = ath10k_board_name(ids, attempt == 0, true, &name);
    else if (attempt == 0) built = ath11k_board_name(ids, search, true, false, false, &name);
    else if (regdb) built = ath11k_board_name(ids, search, false, true, true, &name);
    else built = ath11k_board_name(ids, search, false, false, true, &name);
    if (!built) return AthBoardFind::NotFound;
    const AthBoardFind found = ath_board_find(file, magic, ie_type, name, data);
    if (found != AthBoardFind::NotFound) return found;
  }
  return AthBoardFind::NotFound;
}

}  // namespace hardware::ath
