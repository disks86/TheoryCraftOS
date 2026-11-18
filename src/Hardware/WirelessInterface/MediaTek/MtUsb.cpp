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

#include "Hardware/WirelessInterface/MediaTek/MtUsb.hpp"
#include "Hardware/WirelessInterface/MediaTek/Mt7921Pcie.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace mt_usb;

// --- registers ----------------------------------------------------------------------------------------------------------------------------------------------------------

uint32_t MtUsbRegisters::read32(uint32_t offset) {
  uint8_t request = kVendReadExt;
  uint8_t type = kDirIn | kConnacTypeVendor;
  uint32_t address = offset;
  if (protocol_ == MtUsbProtocol::Mt76u) {
    type = kDirIn | kTypeVendor;
    switch (offset & kVendTypeMask) {
      case kVendTypeEeprom: request = kVendReadEeprom; break;
      case kVendTypeCfg: request = kVendReadCfg; break;
      default: request = kVendMultiRead; break;
    }
    address = offset & ~kVendTypeMask;
  }
  uint8_t data[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  if (!usb_.control_in(port_, type, request, static_cast<uint16_t>(address >> 16), static_cast<uint16_t>(address), ByteView(data))) {
    failed_ = true;
    return 0xFFFFFFFFu;
  }
  return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 | static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
}

void MtUsbRegisters::write32(uint32_t offset, uint32_t value) {
  uint8_t request = kVendWriteExt;
  uint8_t type = kConnacTypeVendor;
  uint32_t address = offset;
  if (protocol_ == MtUsbProtocol::Mt76u) {
    type = kTypeVendor;
    request = (offset & kVendTypeMask) == kVendTypeCfg ? kVendWriteCfg : kVendMultiWrite;
    address = offset & ~kVendTypeMask;
  }
  const uint8_t data[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  if (!usb_.control_out_data(port_, type, request, static_cast<uint16_t>(address >> 16), static_cast<uint16_t>(address), ConstByteView(data))) failed_ = true;
}

// The chip has no byte or half-word requests: the containing word is read (and written back with the part changed).
uint8_t MtUsbRegisters::read8(uint32_t offset) { return static_cast<uint8_t>(read32(offset & ~3u) >> (8 * (offset & 3))); }
uint16_t MtUsbRegisters::read16(uint32_t offset) {
  const uint32_t shift = 8 * (offset & 2);
  return static_cast<uint16_t>(read32(offset & ~3u) >> shift);
}
void MtUsbRegisters::write8(uint32_t offset, uint8_t value) {
  const uint32_t shift = 8 * (offset & 3);
  write32(offset & ~3u, (read32(offset & ~3u) & ~(0xFFu << shift)) | static_cast<uint32_t>(value) << shift);
}
void MtUsbRegisters::write16(uint32_t offset, uint16_t value) {
  const uint32_t shift = 8 * (offset & 2);
  write32(offset & ~3u, (read32(offset & ~3u) & ~(0xFFFFu << shift)) | static_cast<uint32_t>(value) << shift);
}

MtUsbProtocol mt_usb_protocol(WifiDriverFamily family) {
  return family == WifiDriverFamily::Mt7921 || family == WifiDriverFamily::Mt7925 ? MtUsbProtocol::Connac : MtUsbProtocol::Mt76u;
}

// --- chip ---------------------------------------------------------------------------------------------------------------------------------------------------------------

WifiChipResult MtUsbChip::fail(WifiChipResult result, const char* message) {
  message_ = message;
  state_ = WifiChipState::Failed;
  return result;
}

WifiChipResult MtUsbChip::start(IWifiFirmwareSource& firmware) {
  stop();
  identity_ = WifiChipIdentity();
  if (!bus_.usb) return fail(WifiChipResult::NoRegisters, "no USB controller");
  MtUsbRegisters regs(*bus_.usb, bus_.usb_port, mt_usb_protocol(model_.family));
  return mt_usb_protocol(model_.family) == MtUsbProtocol::Connac ? start_connac(regs, firmware) : start_mt76u(regs, firmware);
}

WifiChipResult MtUsbChip::start_connac(MtUsbRegisters& regs, IWifiFirmwareSource& firmware) {
  // mt7921u_probe / mt7925u_probe: the revision is read before anything else
  const uint32_t chip = regs.read32(mt7921::kHwChipId);
  const uint32_t revision = regs.read32(mt7921::kHwRev);
  if (regs.failed() || chip == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "the adapter does not answer vendor requests");
  identity_.chip_id = chip & 0xFFFF;
  identity_.revision = revision & 0xFF;
  const char* patch = nullptr;
  const char* ram = nullptr;
  const bool mt7925 = identity_.chip_id == 0x7925;
  if (!mt792x_firmware_names(identity_.chip_id, &patch, &ram) || mt7925 != (model_.family == WifiDriverFamily::Mt7925))
    return fail(WifiChipResult::UnknownRevision, "the chip id does not belong to this driver family");
  firmware_name_ = ram;
  state_ = WifiChipState::Identified;
  const char* error = nullptr;
  if (!firmware.read(patch, patch_file_)) return fail(WifiChipResult::FirmwareMissing, "the ROM patch file is missing");
  if (!patch_.parse(patch_file_.view(), &error)) return fail(WifiChipResult::FirmwareInvalid, error ? error : "the ROM patch file is not valid");
  if (!firmware.read(ram, file_)) return fail(WifiChipResult::FirmwareMissing, "the RAM code file is missing");
  if (!ram_.parse(file_.view(), &error)) return fail(WifiChipResult::FirmwareInvalid, error ? error : "the RAM code file is not valid");
  state_ = WifiChipState::Identified;
  message_ = "identified, firmware parsed; the USB firmware download is not written yet";
  return WifiChipResult::Unsupported;
}

WifiChipResult MtUsbChip::start_mt76u(MtUsbRegisters& regs, IWifiFirmwareSource& firmware) {
  // mt76x02_wait_for_mac: the MAC answers with something other than 0 or all ones once it is up
  uint32_t asic = 0;
  for (uint32_t i = 0; i < 100; ++i) {
    asic = regs.read32(kAsicVersion);
    if (regs.failed()) return fail(WifiChipResult::NoResponse, "the adapter does not answer vendor requests");
    if (asic != 0 && asic != 0xFFFFFFFFu) break;
    delay_us(5000);
  }
  if (asic == 0 || asic == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "MT_ASIC_VERSION never became valid");
  identity_.chip_id = asic >> 16;
  identity_.revision = asic & 0xFFFF;
  bool known = false;
  switch (model_.family) {
    case WifiDriverFamily::Mt76x2u: known = identity_.chip_id == 0x7612 || identity_.chip_id == 0x7662 || identity_.chip_id == 0x7602; break;   // is_mt76x2
    case WifiDriverFamily::Mt76x0u: known = identity_.chip_id == 0x7610 || identity_.chip_id == 0x7630 || identity_.chip_id == 0x7650; break;   // is_mt76x0
    case WifiDriverFamily::Mt7601u: known = identity_.chip_id == 0x7601; break;
    default: break;
  }
  if (!known) return fail(WifiChipResult::UnknownRevision, "the chip id does not belong to this driver family");
  state_ = WifiChipState::Identified;
  const char* error = nullptr;
  if (!firmware.read(firmware_name_, file_)) return fail(WifiChipResult::FirmwareMissing, "the firmware file is missing");
  if (!usb_firmware_.parse(file_.view(), &error)) return fail(WifiChipResult::FirmwareInvalid, error ? error : "the firmware file is not valid");
  (void)usb_firmware_.version_text(ArrayView<char>(identity_.firmware_version));
  message_ = "identified, firmware parsed; the USB firmware upload (ILM/DLM) is not written yet";
  return WifiChipResult::Unsupported;
}

}  // namespace hardware
