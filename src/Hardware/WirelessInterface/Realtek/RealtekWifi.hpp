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

// The Realtek Wi-Fi chips: what the driver knows about each (RtwChipInfo, after the rtw_chip_info / rtw89_chip_info tables of rtw88 and rtw89) and the two chip
// drivers.
//
// RtwChip (rtw88, after rtw88/main.c rtw_chip_info_setup and rtw_core_start, mac.c and fw.c) reads the chip version (REG_SYS_CFG1 0xF0: cut in bits 15:12, BIT(27)
// = 2T2R, BIT(23) clear = MP chip), powers the chip on (rtw_mac_power_on: REG_CR reads 0xEA while the power is off; a chip that is on is switched off first), reads
// the physical eFuse and replays it into the logical map to get the MAC address, then reads and checks the firmware file. Chips with an 8051 (8723D, and the
// 8812A/8821A once they have power tables) get their firmware written page by page through the 0x1000 window and are started (__rtw_download_firmware_legacy); the
// 11ac-MCU chips (8822B/8822C/8821C) download through the reserved-page TX path and the DDMA engine, which needs the host interface's TX queues and is not written:
// start() stops there with Unsupported. Nothing yet sends the H2C commands that would make a chip Ready.
//
// Rtw89Chip (rtw89, after rtw89/core.c rtw89_chip_info_setup and fw.c rtw89_fw_recognize) reads the cut (R_AX_SYS_CFG1 0xF0 bits 15:12), picks the newest firmware
// file format there is, parses the container and the firmware for its cut and stops (Unsupported): power-on (the rtw89 MAC "pwr_on_func"), its eFuse engine and
// the firmware download (H2C FWDL through the HCI) are not written.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"
#include "Hardware/WirelessInterface/Realtek/RealtekUsbRegisters.hpp"
#include "Hardware/WirelessInterface/Realtek/RtwFirmware.hpp"
#include "Hardware/WirelessInterface/Realtek/RtwPowerSequence.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Optional.hpp"
#include "Utility/StringView.hpp"

namespace hardware {

enum class RtwChipKind : uint8_t { Unknown, Rtw8822B, Rtw8822C, Rtw8821C, Rtw8723D, Rtw8812A, Rtw8821A, Rtw8852A, Rtw8852B, Rtw8852BT, Rtw8852C, Rtw8851B, Rtw8922A };

struct RtwChipInfo {
  RtwChipKind kind;
  uint16_t number;                  // 0x8822, 0x8852, ...
  bool rtw89;
  bool wcpu_8051;                   // rtw88: an 8051 MCU (legacy firmware header, page download)
  uint16_t firmware_signature;      // rtw88: what the firmware header's signature must be (see rtw88_parse_firmware)
  uint16_t efuse_physical, efuse_logical, efuse_protect;
  uint16_t mac_pci, mac_usb, mac_sdio;     // the MAC address's offset in the logical eFuse map, per host interface (0: unknown)
  ArrayView<const ArrayView<const RtwPowerCommand>> (*power_on)();   // null: no power tables here
};

// The chip of a catalogue entry, by its chip name ("RTL8822CE", "RTL8811CU", "RTL8832BU" ...).
RtwChipKind rtw_chip_kind(StringView chip);
// Null for Unknown.
const RtwChipInfo* rtw_chip_info(RtwChipKind kind);

// rtw88 registers used here (rtw88/reg.h).
namespace rtw_reg {
constexpr uint32_t kSysFuncEn = 0x0002, kRsvCtrl = 0x001C, kMcuFwCtrl = 0x0080, kSysCfg1 = 0x00F0, kSysStatus1 = 0x00F4, kCr = 0x0100;
constexpr uint32_t kFwStartLegacy = 0x1000, kFwPageLegacy = 0x1000;
constexpr uint8_t kFenCpuEn = 1u << 2;          // REG_SYS_FUNC_EN + 1
constexpr uint8_t kWlMcuIoIf = 1u << 0;         // REG_RSV_CTRL + 1
constexpr uint32_t kMcuFwDlEn = 1u << 0, kMcuFwDlRdy = 1u << 1, kFwDlChkRpt = 1u << 2, kWintIniRdy = 1u << 6, kRamDlSel = 1u << 7;
constexpr uint32_t kRomPageShift = 16, kRomPageMask = 7u << 16, kRomDlEn = 1u << 19;
constexpr uint32_t kFwReadyLegacy = kMcuFwDlRdy | kFwDlChkRpt | kWintIniRdy | kRamDlSel;
constexpr uint8_t kCrPowerOff = 0xEA;
}  // namespace rtw_reg

// What both chip drivers share: the bus, the state and what was learnt.
class RealtekChipBase : public IWifiChip {
 public:
  const WifiAdapterModel& model() const override { return model_; }
  WifiChipState state() const override { return state_; }
  const WifiChipIdentity& identity() const override { return identity_; }
  MacAddress mac_address() const override { return mac_; }
  const char* firmware_name() const override { return firmware_name_; }
  const char* message() const override { return message_; }

 protected:
  RealtekChipBase(const WifiAdapterModel& model, const WifiChipBus& bus, const RtwChipInfo* info, allocators::IAllocator& allocator);
  // The register window: the BAR, or vendor requests on a USB adapter. Null if the bus has neither.
  INicRegisters* registers() { return registers_; }
  WifiChipResult fail(WifiChipResult result, const char* message);
  void set_version(uint32_t a, uint32_t b, uint32_t c, int64_t d);   // "a.b.c" or "a.b.c.d" (d < 0: none)
  void reset_learnt();

  const WifiAdapterModel& model_;
  WifiChipBus bus_;
  const RtwChipInfo* info_;
  allocators::IAllocator& allocator_;
  Optional<RealtekUsbRegisters> usb_registers_;
  INicRegisters* registers_ = nullptr;
  WifiChipState state_ = WifiChipState::Off;
  WifiChipIdentity identity_{};
  MacAddress mac_{};
  const char* firmware_name_ = "";
  const char* message_ = "not started";
  ByteBuffer firmware_;
  char firmware_name_buffer_[64] = {};
};

class RtwChip final : public RealtekChipBase {
 public:
  RtwChip(const WifiAdapterModel& model, const WifiChipBus& bus, const RtwChipInfo* info, allocators::IAllocator& allocator)
      : RealtekChipBase(model, bus, info, allocator) {}

  [[nodiscard]] WifiChipResult start(IWifiFirmwareSource& firmware) override;
  void stop() override;

  uint32_t cut() const { return identity_.revision; }
  bool two_streams() const { return two_streams_; }
  const Rtw88Firmware& parsed_firmware() const { return parsed_; }
  ConstByteView logical_efuse() const { return ConstByteView(logical_efuse_).first(info_ ? info_->efuse_logical : 0); }

 private:
  WifiChipResult power_on();
  WifiChipResult read_efuse();
  WifiChipResult download_legacy();
  void wlan_cpu_enable(bool enable);
  RtwPowerOptions power_options() const;

  bool two_streams_ = false;
  Rtw88Firmware parsed_{};
  uint8_t physical_efuse_[1024] = {};
  uint8_t logical_efuse_[1024] = {};
};

class Rtw89Chip final : public RealtekChipBase {
 public:
  Rtw89Chip(const WifiAdapterModel& model, const WifiChipBus& bus, const RtwChipInfo* info, allocators::IAllocator& allocator)
      : RealtekChipBase(model, bus, info, allocator) {}

  [[nodiscard]] WifiChipResult start(IWifiFirmwareSource& firmware) override;
  void stop() override;

  const Rtw89FirmwareFile& firmware_file() const { return file_; }
  const Rtw89FirmwareImage& image() const { return image_; }
  uint8_t chosen_type() const { return chosen_type_; }

 private:
  Rtw89FirmwareFile file_{};
  Rtw89FirmwareImage image_{};
  uint8_t chosen_type_ = 0;
};

}  // namespace hardware
