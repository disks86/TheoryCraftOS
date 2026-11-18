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

#include "Hardware/WirelessInterface/WifiChipFactory.hpp"

namespace hardware {

const char* wifi_chip_state_name(WifiChipState state) {
  switch (state) {
    case WifiChipState::Off: return "off";
    case WifiChipState::Identified: return "identified";
    case WifiChipState::FirmwareLoaded: return "firmware loaded";
    case WifiChipState::Ready: return "ready";
    case WifiChipState::Failed: return "failed";
  }
  return "?";
}

const char* wifi_chip_result_name(WifiChipResult result) {
  switch (result) {
    case WifiChipResult::Ok: return "ok";
    case WifiChipResult::NoRegisters: return "the chip's registers cannot be reached";
    case WifiChipResult::NoResponse: return "the chip does not answer";
    case WifiChipResult::UnknownRevision: return "unknown chip revision";
    case WifiChipResult::FirmwareMissing: return "firmware file not found in /firmware";
    case WifiChipResult::FirmwareInvalid: return "firmware file is not valid";
    case WifiChipResult::FirmwareRejected: return "the chip did not accept its firmware";
    case WifiChipResult::Timeout: return "the chip did not answer in time";
    case WifiChipResult::Unsupported: return "not supported yet";
  }
  return "?";
}

UniquePointer<IWifiChip> make_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator) {
  switch (model.vendor) {
    case WifiVendor::Intel: return make_intel_wifi_chip(model, bus, allocator);
    case WifiVendor::Realtek: return make_realtek_wifi_chip(model, bus, allocator);
    case WifiVendor::MediaTek: return make_mediatek_wifi_chip(model, bus, allocator);
    case WifiVendor::Qualcomm: return make_qualcomm_wifi_chip(model, bus, allocator);
    case WifiVendor::Broadcom: return make_broadcom_wifi_chip(model, bus, allocator);
  }
  return UniquePointer<IWifiChip>();
}

}  // namespace hardware
