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

#include "Hardware/WirelessInterface/Intel/IntelWifiChip.hpp"
#include "Hardware/WirelessInterface/Intel/IwlPcie.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"

namespace hardware {

uint32_t iwl_mac_step(uint32_t hw_rev) {
  return iwl_mac_type(hw_rev) >= iwl_mac::kBz ? (hw_rev & 0xF) : (hw_rev & 0xC) >> 2;
}

bool iwl_gen2_firmware_prefix(uint32_t hw_rev, uint32_t rf_id, IwlFirmwareName* out) {
  out->clear();
  const char* mac;
  char mac_step = 0;
  switch (iwl_mac_type(hw_rev)) {
    case iwl_mac::kPu: mac = "9000-pu"; mac_step = 'b'; break;
    case iwl_mac::kTh: mac = "9260-th"; mac_step = 'b'; break;
    case iwl_mac::kQu: mac = "Qu"; break;
    case iwl_mac::kQuZ: mac = "QuZ"; mac_step = 'a'; break;     // all QuZ parts run the A0 firmware
    case iwl_mac::kSo:
    case iwl_mac::kSoF: mac = "so"; mac_step = 'a'; break;
    case iwl_mac::kTy: mac = "ty"; mac_step = 'a'; break;
    case iwl_mac::kMa: mac = "ma"; break;
    case iwl_mac::kBz:
    case iwl_mac::kBzW: mac = "bz"; break;
    case iwl_mac::kGl: mac = "gl"; break;
    case iwl_mac::kSc:
    case iwl_mac::kSc2:
    case iwl_mac::kSc2F: mac = "sc"; break;
    default: return false;
  }
  if (!mac_step) {
    const uint32_t step = iwl_mac_step(hw_rev);
    if (step > 15) return false;
    mac_step = static_cast<char>('a' + step);
  }
  const char* rf;
  char rf_step = static_cast<char>('a' + iwl_rf_step(rf_id));
  switch (iwl_rf_type(rf_id)) {
    case iwl_rf::kHr1:
    case iwl_rf::kHr2: rf = "hr"; rf_step = 'b'; break;
    case iwl_rf::kGf: rf = iwl_rf_cdb(rf_id) ? "gf4" : "gf"; rf_step = 'a'; break;
    case iwl_rf::kJf1:
    case iwl_rf::kJf2: rf = "jf"; rf_step = 'b'; break;
    case iwl_rf::kFm: rf = "fm"; break;
    case iwl_rf::kWh: rf = "wh"; break;
    default: return false;
  }
  const char mac_step_text[3] = {mac_step, '0', 0}, rf_step_text[3] = {rf_step, '0', 0};
  return out->append("iwlwifi-") && out->append(mac) && out->append("-") && out->append(StringView(mac_step_text)) && out->append("-") && out->append(rf) &&
         out->append("-") && out->append(StringView(rf_step_text));
}

bool iwl_versioned_firmware_name(StringView prefix, uint32_t api, bool core, IwlFirmwareName* out) {
  out->clear();
  return out->assign(prefix) && out->append(core ? "-c" : "-") && out->append_uint(api) && out->append(".ucode");
}

bool iwl_split_firmware_name(StringView name, IwlFirmwareName* prefix, uint32_t* api) {
  if (!name.ends_with(StringView(".ucode"))) return false;
  const StringView stem = name.substr(0, name.size() - 6);
  const uint64_t dash = stem.rfind('-');
  if (dash == StringView::npos || dash + 1 >= stem.size()) return false;
  uint64_t at = dash + 1;
  if (stem[at] == 'c') ++at;
  if (at >= stem.size() || stem.size() - at > 4) return false;
  uint32_t value = 0;
  for (; at < stem.size(); ++at) {
    const char c = stem[at];
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint32_t>(c - '0');
  }
  *api = value;
  return prefix->assign(stem.substr(0, dash));
}

IwlApiRange iwl_firmware_api_range(WifiDriverFamily family) {
  switch (family) {
    case WifiDriverFamily::IwlMvm7000: return {17, 29};
    case WifiDriverFamily::IwlMvm8000: return {22, 36};
    case WifiDriverFamily::IwlMvm9000: return {30, 46};
    case WifiDriverFamily::IwlMvm22000: return {50, 77};
    case WifiDriverFamily::IwlMvmAx210: return {59, 89};
    case WifiDriverFamily::IwlMldBe200: return {83, 120};
    default: return {0, 0};
  }
}

WifiChipResult IntelWifiChip::fail(WifiChipResult result, const char* message) {
  message_ = message;
  if (result != WifiChipResult::Unsupported) state_ = WifiChipState::Failed;
  return result;
}

bool IntelWifiChip::firmware_prefix(IwlFirmwareName* prefix, IwlApiRange* range) {
  StringView name(model_.firmware);
  IwlFirmwareName seven_d;
  // The 7265 IDs carry either a 7265 or a 7265D; the D wants its own (newer) firmware.
  if (name.size() == 0 && model_.family == WifiDriverFamily::IwlMvm7000) {
    name = (hw_rev_ & 0xFFF0) == kIwlHwRevType7265D ? StringView("iwlwifi-7265D-29.ucode") : StringView("iwlwifi-7265-17.ucode");
  }
  if (name.size() != 0) {
    uint32_t api = 0;
    if (!iwl_split_firmware_name(name, prefix, &api)) return false;
    const IwlApiRange family = iwl_firmware_api_range(model_.family);
    // DVM files are named by their one API; the others are searched from the family's newest down to its oldest.
    *range = family.max == 0 ? IwlApiRange{api, api} : IwlApiRange{family.min, family.max > api ? family.max : api};
    return true;
  }
  *range = iwl_firmware_api_range(model_.family);
  return iwl_gen2_firmware_prefix(hw_rev_, rf_id_, prefix);
}

WifiChipResult IntelWifiChip::start(IWifiFirmwareSource& source) {
  state_ = WifiChipState::Off;
  identity_ = WifiChipIdentity{};
  firmware_name_.clear();
  if (!bus_.registers) return fail(WifiChipResult::NoRegisters, "no register window");
  hw_rev_ = bus_.registers->read32(iwl_csr::kHwRev);
  if (hw_rev_ == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "the card's registers read as all ones");
  rf_id_ = bus_.registers->read32(iwl_csr::kHwRfId);
  identity_.chip_id = iwl_mac_type(hw_rev_);
  identity_.revision = iwl_mac_step(hw_rev_);
  state_ = WifiChipState::Identified;

  IwlFirmwareName prefix;
  IwlApiRange range{};
  if (!firmware_prefix(&prefix, &range)) return fail(WifiChipResult::UnknownRevision, "no firmware name for this MAC and radio");
  bool found = false;
  for (uint32_t api = range.max; api + 1 > range.min && api != 0 && !found; --api) {
    // From API 100 on try "-c<api>" first, then the plain name linux-firmware keeps for some.
    for (uint32_t form = 0; form < (api >= 100 ? 2u : 1u) && !found; ++form) {
      IwlFirmwareName name;
      if (!iwl_versioned_firmware_name(prefix.view(), api, api >= 100 && form == 0, &name)) continue;
      if (source.read(name.c_str(), file_)) {
        firmware_name_ = name;
        found = true;
      }
    }
  }
  if (!found) {
    // Name the newest, so the message says what to put into /firmware.
    (void)iwl_versioned_firmware_name(prefix.view(), range.max, range.max >= 100, &firmware_name_);
    return fail(WifiChipResult::FirmwareMissing, "no firmware file of a supported API version in /firmware");
  }
  const char* error = "";
  if (!firmware_.parse(ConstByteView(file_.view()), &error)) return fail(WifiChipResult::FirmwareInvalid, error);
  if (!firmware_.has_image(IwlImage::Regular)) return fail(WifiChipResult::FirmwareInvalid, "the firmware file has no runtime image");
  FixedString<48> version;
  if (firmware_.human_readable()[0] != 0) {
    const StringView text(firmware_.human_readable());
    (void)version.assign(text.first(text.size() < version.capacity() ? text.size() : version.capacity()));
  } else {
    (void)(version.append_uint(firmware_.fw_major()) && version.append(".") && version.append_uint(firmware_.fw_minor()));
  }
  for (uint64_t i = 0; i < version.size() && i + 1 < sizeof identity_.firmware_version; ++i) identity_.firmware_version[i] = version[i];
  return fail(WifiChipResult::Unsupported, "firmware found and checked; starting it is not written for this generation yet");
}

UniquePointer<IWifiChip> make_intel_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator) {
  // The 9000 series has its own complete driver (IwlWirelessInterface), and DVM cards have no chip class.
  if (model.family == WifiDriverFamily::IwlMvm9000 || model.family == WifiDriverFamily::IwlDvm) return UniquePointer<IWifiChip>();
  return UniquePointer<IWifiChip>::make_as<IntelWifiChip>(allocator, model, bus, allocator);
}

}  // namespace hardware
