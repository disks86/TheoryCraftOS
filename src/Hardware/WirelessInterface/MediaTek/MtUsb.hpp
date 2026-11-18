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

// The USB MediaTek Wi-Fi adapters: their registers over control transfers, and the first steps of bring-up.
//
// MtUsbRegisters, after mt76u_rr / mt76u_wr (mt76/usb.c) and mt792xu_rr / mt792xu_wr (mt792x_usb.c): a 32 bit register is read with a vendor request on endpoint 0 that
// carries the address's high half in wValue and its low half in wIndex, with four little endian bytes of data. Two protocols:
// - Mt76u (MT76x2U, MT76x0U, MT7601U): MT_VEND_MULTI_READ (0x07) / MT_VEND_MULTI_WRITE (0x06), request type vendor to device; an address with bit 31 set
//   (MT_VEND_TYPE_EEPROM) is read with MT_VEND_READ_EEPROM (0x09), one with bit 30 (MT_VEND_TYPE_CFG) with MT_VEND_READ_CFG / WRITE_CFG (0x47 / 0x46);
// - Connac (MT7921AU, MT7925U): MT_VEND_READ_EXT (0x63) / MT_VEND_WRITE_EXT (0x66), request type MT_USB_TYPE_VENDOR (vendor | 0x1f), the chip's full 32 bit addresses
//   (no remap windows on USB).
// 8 and 16 bit accesses are made of the 32 bit one around them.
//
// MtUsbChip identifies the chip (MT_HW_CHIPID / MT_HW_REV on connac, MT_ASIC_VERSION on the older ones), names and parses its firmware, and stops there with
// Unsupported: the USB firmware download (connac: the same MCU messages as PCIe, over the bulk endpoints with a USB transfer header; MT76x2U/MT76x0U/MT7601U: the ILM and
// DLM pushed with MT_VEND_WRITE_FCE and bulk DMA) is not written. Tested against a model only.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"
#include "Hardware/WirelessInterface/MediaTek/MtFirmware.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

namespace mt_usb {
// enum mt_vendor_req (mt76.h)
constexpr uint8_t kVendDevMode = 0x01, kVendWrite = 0x02, kVendPowerOn = 0x04, kVendMultiWrite = 0x06, kVendMultiRead = 0x07, kVendReadEeprom = 0x09,
                  kVendWriteFce = 0x42, kVendWriteCfg = 0x46, kVendReadCfg = 0x47, kVendReadExt = 0x63, kVendWriteExt = 0x66, kVendFeatureSet = 0x91;
constexpr uint8_t kTypeVendor = 0x40, kDirIn = 0x80;
constexpr uint8_t kConnacTypeVendor = kTypeVendor | 0x1F;    // MT_USB_TYPE_VENDOR
constexpr uint32_t kVendTypeEeprom = 1u << 31, kVendTypeCfg = 1u << 30, kVendTypeMask = kVendTypeEeprom | kVendTypeCfg;
// MT76x02 / MT7601U identification
constexpr uint32_t kAsicVersion = 0x0000, kMacCsr0 = 0x1000;
}  // namespace mt_usb

enum class MtUsbProtocol : uint8_t { Mt76u, Connac };

class MtUsbRegisters final : public INicRegisters {
 public:
  MtUsbRegisters(IUsbHostController& usb, uint32_t port, MtUsbProtocol protocol) : usb_(usb), port_(port), protocol_(protocol) {}

  uint8_t read8(uint32_t offset) override;
  uint16_t read16(uint32_t offset) override;
  uint32_t read32(uint32_t offset) override;          // all ones when the transfer failed
  void write8(uint32_t offset, uint8_t value) override;
  void write16(uint32_t offset, uint16_t value) override;
  void write32(uint32_t offset, uint32_t value) override;

  // True once a control transfer failed (sticky until clear_error()).
  bool failed() const { return failed_; }
  void clear_error() { failed_ = false; }

 private:
  IUsbHostController& usb_;
  uint32_t port_;
  MtUsbProtocol protocol_;
  bool failed_ = false;
};

// The register protocol a driver family uses on USB.
MtUsbProtocol mt_usb_protocol(WifiDriverFamily family);

class MtUsbChip final : public IWifiChip {
 public:
  MtUsbChip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator)
      : model_(model), bus_(bus), firmware_name_(model.firmware), file_(allocator), patch_file_(allocator) {}

  const WifiAdapterModel& model() const override { return model_; }
  [[nodiscard]] WifiChipResult start(IWifiFirmwareSource& firmware) override;
  void stop() override { state_ = WifiChipState::Off; }
  WifiChipState state() const override { return state_; }
  const WifiChipIdentity& identity() const override { return identity_; }
  MacAddress mac_address() const override { return MacAddress{{0, 0, 0, 0, 0, 0}}; }
  const char* firmware_name() const override { return firmware_name_; }
  const char* message() const override { return message_; }

  const MtConnacPatch& patch() const { return patch_; }
  const MtConnacRam& ram() const { return ram_; }
  const Mt76UsbFirmware& usb_firmware() const { return usb_firmware_; }

 private:
  WifiChipResult fail(WifiChipResult result, const char* message);
  WifiChipResult start_connac(MtUsbRegisters& regs, IWifiFirmwareSource& firmware);
  WifiChipResult start_mt76u(MtUsbRegisters& regs, IWifiFirmwareSource& firmware);

  const WifiAdapterModel& model_;
  WifiChipBus bus_;
  WifiChipState state_ = WifiChipState::Off;
  WifiChipIdentity identity_;
  const char* firmware_name_;
  const char* message_ = "not started";
  ByteBuffer file_, patch_file_;
  MtConnacPatch patch_;
  MtConnacRam ram_;
  Mt76UsbFirmware usb_firmware_;
};

}  // namespace hardware
