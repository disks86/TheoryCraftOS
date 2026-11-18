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

#include "Hardware/WirelessInterface/Realtek/RealtekWifi.hpp"
#include "Hardware/WirelessInterface/Realtek/RtwEfuse.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

using namespace rtw_reg;

namespace {

// eFuse sizes and MAC offsets: rtw88 from the chips' rtw_chip_info (physical/logical/protect sizes) and struct rtw88xx{e,u,s}_efuse; rtw89 from rtw89_chip_info and
// struct rtw88xx_{e,u}_efuse. The rtw89 values, the 8812A/8821A ones and the rtw89 protect sizes are unverified.
constexpr RtwChipInfo kChips[] = {
    {RtwChipKind::Rtw8822B, 0x8822, false, false, 0x8822, 1024, 768, 96, 0x0D0, 0x107, 0x11A, &rtw8822b_power_on},
    {RtwChipKind::Rtw8822C, 0x8822, false, false, 0x8822, 512, 512, 96, 0x120, 0x157, 0x16A, &rtw8822c_power_on},
    {RtwChipKind::Rtw8821C, 0x8821, false, false, 0x8821, 512, 512, 96, 0x0D0, 0x107, 0x11A, &rtw8821c_power_on},
    {RtwChipKind::Rtw8723D, 0x8723, false, true, 0x23D0, 512, 512, 96, 0x0D0, 0x107, 0x11A, &rtw8723d_power_on},
    {RtwChipKind::Rtw8812A, 0x8812, false, true, 0x9500, 512, 512, 96, 0x0D0, 0x0D7, 0, nullptr},
    {RtwChipKind::Rtw8821A, 0x8821, false, true, 0x2100, 512, 512, 96, 0x0D0, 0x107, 0, nullptr},
    {RtwChipKind::Rtw8852A, 0x8852, true, false, 0, 1216, 1536, 4, 0x400, 0x438, 0, nullptr},
    {RtwChipKind::Rtw8852B, 0x8852, true, false, 0, 1216, 2048, 4, 0x400, 0x488, 0, nullptr},
    {RtwChipKind::Rtw8852BT, 0x8852, true, false, 0, 1216, 2048, 4, 0x400, 0x488, 0, nullptr},
    {RtwChipKind::Rtw8852C, 0x8852, true, false, 0, 1216, 2048, 4, 0x400, 0x488, 0, nullptr},
    {RtwChipKind::Rtw8851B, 0x8851, true, false, 0, 1216, 2048, 4, 0x400, 0x488, 0, nullptr},
    {RtwChipKind::Rtw8922A, 0x8922, true, false, 0, 1216, 2048, 4, 0x400, 0, 0, nullptr},
};

struct ChipName {
  const char* prefix;
  RtwChipKind kind;
};
// Longest prefixes first ("RTL8852BT" before "RTL8852B").
constexpr ChipName kChipNames[] = {
    {"RTL8852BT", RtwChipKind::Rtw8852BT}, {"RTL8822B", RtwChipKind::Rtw8822B}, {"RTL8812B", RtwChipKind::Rtw8822B}, {"RTL8822C", RtwChipKind::Rtw8822C},
    {"RTL8812C", RtwChipKind::Rtw8822C},   {"RTL8821C", RtwChipKind::Rtw8821C}, {"RTL8811C", RtwChipKind::Rtw8821C}, {"RTL8723D", RtwChipKind::Rtw8723D},
    {"RTL8812A", RtwChipKind::Rtw8812A},   {"RTL8821A", RtwChipKind::Rtw8821A}, {"RTL8811A", RtwChipKind::Rtw8821A}, {"RTL8852A", RtwChipKind::Rtw8852A},
    {"RTL8832A", RtwChipKind::Rtw8852A},   {"RTL8852B", RtwChipKind::Rtw8852B}, {"RTL8832B", RtwChipKind::Rtw8852B}, {"RTL8852C", RtwChipKind::Rtw8852C},
    {"RTL8832C", RtwChipKind::Rtw8852C},   {"RTL8851B", RtwChipKind::Rtw8851B}, {"RTL8922A", RtwChipKind::Rtw8922A},
};

}  // namespace

RtwChipKind rtw_chip_kind(StringView chip) {
  for (const ChipName& name : kChipNames)
    if (chip.starts_with(name.prefix)) return name.kind;
  return RtwChipKind::Unknown;
}

const RtwChipInfo* rtw_chip_info(RtwChipKind kind) {
  for (const RtwChipInfo& info : kChips)
    if (info.kind == kind) return &info;
  return nullptr;
}

// ---------------------------------------------------------------- shared

RealtekChipBase::RealtekChipBase(const WifiAdapterModel& model, const WifiChipBus& bus, const RtwChipInfo* info, allocators::IAllocator& allocator)
    : model_(model), bus_(bus), info_(info), allocator_(allocator), firmware_(allocator) {
  firmware_name_ = model.firmware ? model.firmware : "";
  if (bus.registers) {
    registers_ = bus.registers;
  } else if (bus.usb) {
    registers_ = &usb_registers_.emplace(*bus.usb, bus.usb_port);
  }
}

WifiChipResult RealtekChipBase::fail(WifiChipResult result, const char* message) {
  message_ = message;
  if (result != WifiChipResult::Unsupported) state_ = WifiChipState::Failed;
  return result;
}

void RealtekChipBase::set_version(uint32_t a, uint32_t b, uint32_t c, int64_t d) {
  FixedString<48> text;
  (void)text.append_uint(a);
  (void)text.push_back('.');
  (void)text.append_uint(b);
  (void)text.push_back('.');
  (void)text.append_uint(c);
  if (d >= 0) {
    (void)text.push_back('.');
    (void)text.append_uint(static_cast<uint64_t>(d));
  }
  (void)text.view().copy_to(ArrayView<char>(identity_.firmware_version));
}

void RealtekChipBase::reset_learnt() {
  identity_ = WifiChipIdentity{};
  mac_ = MacAddress{};
  state_ = WifiChipState::Off;
  firmware_.clear();
}

UniquePointer<IWifiChip> make_realtek_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator) {
  const RtwChipInfo* info = rtw_chip_info(rtw_chip_kind(model.chip ? model.chip : ""));
  if (!info) return UniquePointer<IWifiChip>();
  if ((model.family == WifiDriverFamily::Rtw88 || model.family == WifiDriverFamily::Rtl8812au) && !info->rtw89)
    return UniquePointer<IWifiChip>::make_as<RtwChip>(allocator, model, bus, info, allocator);
  if (model.family == WifiDriverFamily::Rtw89 && info->rtw89) return UniquePointer<IWifiChip>::make_as<Rtw89Chip>(allocator, model, bus, info, allocator);
  return UniquePointer<IWifiChip>();
}

// ---------------------------------------------------------------- rtw88

RtwPowerOptions RtwChip::power_options() const {
  RtwPowerOptions options;
  options.interface_mask = model_.bus == WifiBus::Usb ? rtw_power::kInterfaceUsb : model_.bus == WifiBus::Sdio ? rtw_power::kInterfaceSdio : rtw_power::kInterfacePci;
  options.cut_mask = rtw_power::cut_mask(identity_.revision);
  options.pcie = model_.bus == WifiBus::Pci;
  options.toggle_8723d = info_->kind == RtwChipKind::Rtw8723D;
  return options;
}

WifiChipResult RtwChip::power_on() {
  INicRegisters& r = *registers_;
  // rtw_mac_pre_system_cfg: unlock the ISO/CLK/power control registers.
  r.write8(kRsvCtrl, 0);
  // rtw_mac_power_switch: is the power on now?
  bool powered = r.read8(kCr) != kCrPowerOff;
  if (model_.bus == WifiBus::Usb && (r.read8(kSysStatus1 + 1) & 1)) powered = false;
  const RtwPowerOptions options = power_options();
  if (powered && rtw_run_power_sequence(r, rtw_power_off_mac(), options) != RtwPowerResult::Ok) return fail(WifiChipResult::Timeout, "the chip did not power off");
  switch (rtw_run_power_sequence(r, info_->power_on(), options)) {
    case RtwPowerResult::Ok: return WifiChipResult::Ok;
    case RtwPowerResult::Timeout: return fail(WifiChipResult::Timeout, "the power-on sequence timed out");
    case RtwPowerResult::BadCommand: break;
  }
  return fail(WifiChipResult::UnknownRevision, "bad power sequence command");
}

WifiChipResult RtwChip::read_efuse() {
  if (info_->efuse_physical > sizeof physical_efuse_ || info_->efuse_logical > sizeof logical_efuse_) return fail(WifiChipResult::UnknownRevision, "eFuse larger than expected");
  const ByteView physical = ByteView(physical_efuse_).first(info_->efuse_physical);
  const ByteView logical = ByteView(logical_efuse_).first(info_->efuse_logical);
  if (!rtw88_read_physical_efuse(*registers_, physical)) return fail(WifiChipResult::Timeout, "eFuse read failed");
  if (!rtw88_efuse_logical_map(physical, info_->efuse_protect, logical)) return fail(WifiChipResult::NoResponse, "the eFuse map is damaged");
  const uint16_t offset = model_.bus == WifiBus::Usb ? info_->mac_usb : model_.bus == WifiBus::Sdio ? info_->mac_sdio : info_->mac_pci;
  if (offset == 0 || !rtw_efuse_mac_address(logical, offset, &mac_)) {
    mac_ = MacAddress{};
    return fail(WifiChipResult::NoResponse, "the eFuse holds no MAC address");
  }
  return WifiChipResult::Ok;
}

void RtwChip::wlan_cpu_enable(bool enable) {
  INicRegisters& r = *registers_;
  if (enable) {
    r.write8(kRsvCtrl + 1, static_cast<uint8_t>(r.read8(kRsvCtrl + 1) | kWlMcuIoIf));
    r.write8(kSysFuncEn + 1, static_cast<uint8_t>(r.read8(kSysFuncEn + 1) | kFenCpuEn));
  } else {
    r.write8(kSysFuncEn + 1, static_cast<uint8_t>(r.read8(kSysFuncEn + 1) & ~kFenCpuEn));
    r.write8(kRsvCtrl + 1, static_cast<uint8_t>(r.read8(kRsvCtrl + 1) & ~kWlMcuIoIf));
  }
}

// After rtw88/fw.c en_download_firmware_legacy, download_firmware_legacy, write_firmware_page and download_firmware_validate_legacy.
WifiChipResult RtwChip::download_legacy() {
  INicRegisters& r = *registers_;
  // Enable the download.
  wlan_cpu_enable(false);
  wlan_cpu_enable(true);
  r.write8(kMcuFwCtrl, static_cast<uint8_t>(r.read8(kMcuFwCtrl) | kMcuFwDlEn));
  bool enabled = false;
  for (uint32_t tries = 0; tries < 10 && !enabled; ++tries) {
    enabled = (r.read8(kMcuFwCtrl) & kMcuFwDlEn) != 0;
    if (!enabled) {
      r.write8(kMcuFwCtrl, static_cast<uint8_t>(r.read8(kMcuFwCtrl) | kMcuFwDlEn));
      delay_us(20000);
    }
  }
  if (!enabled) return fail(WifiChipResult::Timeout, "the firmware download did not get ready");
  r.write32(kMcuFwCtrl, r.read32(kMcuFwCtrl) & ~kRomDlEn);

  // The body, a 4 KiB page at a time, 32-bit little-endian words into the window.
  const ConstByteView body = parsed_.body;
  for (uint64_t start = 0, page = 0; start < body.size(); start += kFwPageLegacy, ++page) {
    if (page > (kRomPageMask >> kRomPageShift)) return fail(WifiChipResult::FirmwareInvalid, "the firmware is larger than the download window");
    const ConstByteView chunk = body.subview(start).at_most(kFwPageLegacy);
    uint32_t control = r.read32(kMcuFwCtrl) & ~kRomPageMask;
    control |= (static_cast<uint32_t>(page) << kRomPageShift) & kRomPageMask;
    r.write32(kMcuFwCtrl, control);
    ByteReader reader(chunk);
    uint32_t address = kFwStartLegacy;
    while (reader.remaining() >= 4) {
      r.write32(address, reader.le32());
      address += 4;
    }
    if (reader.remaining()) {
      uint32_t last = 0;
      for (uint32_t shift = 0; !reader.at_end(); shift += 8) last |= uint32_t{reader.u8()} << shift;
      r.write32(address, last);
    }
  }
  bool reported = false;
  for (uint32_t tries = 0; tries < 1000 && !reported; ++tries) {
    reported = (r.read32(kMcuFwCtrl) & kFwDlChkRpt) != 0;
    if (!reported) delay_us(1);
  }
  r.write8(kMcuFwCtrl, static_cast<uint8_t>(r.read8(kMcuFwCtrl) & ~kMcuFwDlEn));
  if (!reported) return fail(WifiChipResult::FirmwareRejected, "the chip did not confirm the firmware download");

  // Validate: say the download is done, restart the CPU and wait for it to report.
  uint32_t control = r.read32(kMcuFwCtrl);
  control |= kMcuFwDlRdy;
  control &= ~kWintIniRdy;
  r.write32(kMcuFwCtrl, control);
  wlan_cpu_enable(false);
  wlan_cpu_enable(true);
  for (uint32_t tries = 0; tries < 10; ++tries) {
    if ((r.read32(kMcuFwCtrl) & kFwReadyLegacy) == kFwReadyLegacy) return WifiChipResult::Ok;
    delay_us(20000);
  }
  return fail(WifiChipResult::FirmwareRejected, "the firmware did not start");
}

WifiChipResult RtwChip::start(IWifiFirmwareSource& firmware) {
  reset_learnt();
  if (!info_) return fail(WifiChipResult::UnknownRevision, "not an rtw88 chip");
  if (!registers_) return fail(WifiChipResult::NoRegisters, "no register window");

  // rtw_chip_parameter_setup / rtw_chip_info_setup: the version register.
  const uint32_t version = registers_->read32(kSysCfg1);
  if (version == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "the chip does not answer");
  identity_.chip_id = info_->number;
  identity_.revision = (version >> 12) & 0xF;
  two_streams_ = (version & (1u << 27)) != 0;
  state_ = WifiChipState::Identified;
  message_ = "identified";

  if (!info_->power_on) return fail(WifiChipResult::Unsupported, "no power-on sequence for this chip yet");
  WifiChipResult result = power_on();
  if (result != WifiChipResult::Ok) return result;
  result = read_efuse();
  if (result != WifiChipResult::Ok) return result;

  if (!firmware_name_[0]) return fail(WifiChipResult::FirmwareMissing, "no firmware file named for this chip");
  if (!firmware.read(firmware_name_, firmware_)) return fail(WifiChipResult::FirmwareMissing, "firmware file not found");
  if (rtw88_parse_firmware(firmware_.view(), info_->wcpu_8051, info_->firmware_signature, &parsed_) != RtwFirmwareStatus::Ok)
    return fail(WifiChipResult::FirmwareInvalid, "the firmware file is not valid for this chip");
  set_version(parsed_.version, parsed_.sub_version, parsed_.sub_index, -1);

  if (!info_->wcpu_8051) return fail(WifiChipResult::Unsupported, "firmware checked; its reserved-page/DDMA download is not written yet");
  result = download_legacy();
  if (result != WifiChipResult::Ok) return result;
  state_ = WifiChipState::FirmwareLoaded;
  return fail(WifiChipResult::Unsupported, "firmware running; H2C commands are not written yet");
}

void RtwChip::stop() {
  if (registers_ && state_ != WifiChipState::Off && info_ && info_->power_on) {
    if (info_->wcpu_8051) wlan_cpu_enable(false);
    (void)rtw_run_power_sequence(*registers_, rtw_power_off_mac(), power_options());
  }
  state_ = WifiChipState::Off;
  message_ = "stopped";
}

// ---------------------------------------------------------------- rtw89

WifiChipResult Rtw89Chip::start(IWifiFirmwareSource& firmware) {
  reset_learnt();
  if (!info_) return fail(WifiChipResult::UnknownRevision, "not an rtw89 chip");
  if (!registers_) return fail(WifiChipResult::NoRegisters, "no register window");
  // rtw89_read_chip_ver: R_AX_SYS_CFG1, the cut in bits 15:12.
  const uint32_t version = registers_->read32(kSysCfg1);
  if (version == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "the chip does not answer");
  identity_.chip_id = info_->number;
  identity_.revision = (version >> 12) & 0xF;
  state_ = WifiChipState::Identified;
  message_ = "identified";

  // rtw89_load_firmware: the newest format first ("..._fw-N.bin"), then older ones down to the plain name.
  const StringView name(model_.firmware ? model_.firmware : "");
  if (name.empty() || !name.ends_with(".bin")) return fail(WifiChipResult::FirmwareMissing, "no firmware file named for this chip");
  StringView stem(name.data(), name.size() - 4);
  uint64_t format = 0;
  const uint64_t dash = stem.rfind('-');
  if (dash != StringView::npos) {
    StringView digits;
    if (stem.try_substr(dash + 1, stem.size() - dash - 1, &digits) && digits.parse_uint(&format) && format < 10) stem = StringView(stem.data(), dash);
    else format = 0;
  }
  // A file of a newer format may have dropped old cuts (rtw8922a_fw-3.bin has no A-cut firmware): then the next older file is tried.
  const uint8_t cv = static_cast<uint8_t>(identity_.revision);
  const Rtw89FirmwareEntry* entry = nullptr;
  WifiChipResult missing = WifiChipResult::FirmwareMissing;
  const char* why = "firmware file not found";
  for (int64_t f = static_cast<int64_t>(format); f >= 0 && !entry; --f) {
    FixedString<64> candidate;
    bool fits = candidate.assign(stem);
    if (f > 0) fits = fits && candidate.push_back('-') && candidate.append_uint(static_cast<uint64_t>(f));
    fits = fits && candidate.append(".bin");
    if (!fits || !candidate.view().copy_to(ArrayView<char>(firmware_name_buffer_))) continue;
    if (!firmware.read(firmware_name_buffer_, firmware_)) continue;
    firmware_name_ = firmware_name_buffer_;
    if (file_.parse(firmware_.view()) != RtwFirmwareStatus::Ok) {
      missing = WifiChipResult::FirmwareInvalid;
      why = "the firmware file is not valid";
      continue;
    }
    entry = file_.find(Rtw89FirmwareType::NormalCe, cv);
    chosen_type_ = static_cast<uint8_t>(Rtw89FirmwareType::NormalCe);
    if (!entry) {
      entry = file_.find(Rtw89FirmwareType::Normal, cv);
      chosen_type_ = static_cast<uint8_t>(Rtw89FirmwareType::Normal);
    }
    if (!entry) {
      missing = WifiChipResult::UnknownRevision;
      why = "the firmware files have no firmware for this chip cut";
    }
  }
  if (!entry) return fail(missing, why);
  if (rtw89_parse_firmware_image(entry->data, &image_) != RtwFirmwareStatus::Ok) return fail(WifiChipResult::FirmwareInvalid, "the firmware image is not valid");
  if (image_.chip_id != info_->number) return fail(WifiChipResult::FirmwareInvalid, "the firmware is for another chip");
  set_version(image_.major, image_.minor, image_.sub_version, image_.sub_index);
  return fail(WifiChipResult::Unsupported, "firmware checked; rtw89 power-on and firmware download are not written yet");
}

void Rtw89Chip::stop() {
  state_ = WifiChipState::Off;
  message_ = "stopped";
}

}  // namespace hardware
