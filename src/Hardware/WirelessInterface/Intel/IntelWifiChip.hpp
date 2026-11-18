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

// The Intel adapters other than the 9000 series (which IwlWirelessInterface runs): the DVM cards, the 7000 and 8000 series, the Wi-Fi 6 22000 family (AX200, AX201), the
// Wi-Fi 6E AX210 family and the Wi-Fi 7 BE200 family. IntelWifiChip reads the hardware revision and radio ID, names the firmware the way iwlwifi does (iwl-drv.c,
// iwl_drv_get_fwname_pre, written after the 6.x kernels), finds the newest API of it in /firmware and checks the file with IwlFirmware (the TLV container is the same for
// every generation since the 7000 series). Starting the firmware is not written for these generations yet: the 7000/8000 series load sections like the 9000 series
// (IwlFwLoad) but keep their own NVM and power quirks; the 22000 family and later boot through a context-information structure (gen2) and the AX210 family and later
// need the PNVM file and the image loader. start() therefore stops at Identified with WifiChipResult::Unsupported. Only registers that are safe to read on a running
// card are touched (CSR_HW_REV, CSR_HW_RF_ID): nothing is written.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"
#include "Hardware/WirelessInterface/Intel/IwlFirmware.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

using IwlFirmwareName = FixedString<64>;

// CSR_HW_REV / CSR_HW_RF_ID fields (iwl-csr.h).
inline uint32_t iwl_mac_type(uint32_t hw_rev) { return (hw_rev & 0x000FFF0) >> 4; }
inline uint32_t iwl_rf_type(uint32_t rf_id) { return (rf_id & 0x0FFF000) >> 12; }
inline uint32_t iwl_rf_step(uint32_t rf_id) { return (rf_id >> 8) & 0xF; }
inline bool iwl_rf_cdb(uint32_t rf_id) { return (rf_id >> 28 & 1) != 0; }
// The MAC's silicon step: bits 2-3 up to the 22000/AX210 MACs, bits 0-3 from the BZ MAC on (where the dash bits were dropped). Unverified for SC.
uint32_t iwl_mac_step(uint32_t hw_rev);

namespace iwl_mac {
constexpr uint32_t kPu = 0x31, kTh = 0x32, kQu = 0x33, kQuZ = 0x35, kSo = 0x37, kTy = 0x42, kSoF = 0x43, kMa = 0x44, kBz = 0x46, kGl = 0x47, kSc = 0x48, kSc2 = 0x49, kSc2F = 0x4A, kBzW = 0x4B;
}
namespace iwl_rf {
constexpr uint32_t kJf2 = 0x105, kJf1 = 0x108, kHr2 = 0x10A, kHr1 = 0x10C, kGf = 0x10D, kFm = 0x112, kWh = 0x113;
}
constexpr uint32_t kIwlHwRevType7265D = 0x210;   // (CSR_HW_REV & 0xFFF0) of a 7265D

// "iwlwifi-<mac>-<step>0-<rf>-<step>0" from the registers of a 22000-or-later card ("iwlwifi-so-a0-gf-a0", "iwlwifi-QuZ-a0-hr-b0"); false for a MAC or radio it does
// not know (the radio ID reads 0 on some CNVi parts until the CNVi registers are read, which is not done here).
[[nodiscard]] bool iwl_gen2_firmware_prefix(uint32_t hw_rev, uint32_t rf_id, IwlFirmwareName* out);
// "<prefix>-<api>.ucode"; from API 100 on linux-firmware calls the file "<prefix>-c<api>.ucode" (`core` true), and keeps some under the plain name too.
[[nodiscard]] bool iwl_versioned_firmware_name(StringView prefix, uint32_t api, bool core, IwlFirmwareName* out);
// "iwlwifi-7260-17.ucode" -> "iwlwifi-7260" and 17. False if the name is not of that form.
[[nodiscard]] bool iwl_split_firmware_name(StringView name, IwlFirmwareName* prefix, uint32_t* api);

struct IwlApiRange {
  uint32_t min, max;
};
// The firmware API versions a family's driver understood (cfg/*.c, IWL_*_UCODE_API_MIN/MAX), widened to what linux-firmware ships.
IwlApiRange iwl_firmware_api_range(WifiDriverFamily family);

class IntelWifiChip final : public IWifiChip {
 public:
  IntelWifiChip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator)
      : model_(model), bus_(bus), file_(allocator), firmware_(allocator) {}

  const WifiAdapterModel& model() const override { return model_; }
  [[nodiscard]] WifiChipResult start(IWifiFirmwareSource& firmware) override;
  void stop() override { state_ = WifiChipState::Off; }
  WifiChipState state() const override { return state_; }
  const WifiChipIdentity& identity() const override { return identity_; }
  MacAddress mac_address() const override { return MacAddress{}; }     // from the NVM, once the firmware runs
  const char* firmware_name() const override { return firmware_name_.c_str(); }
  const char* message() const override { return message_; }

  uint32_t hw_rev() const { return hw_rev_; }
  uint32_t rf_id() const { return rf_id_; }
  const IwlFirmware& firmware() const { return firmware_; }

 private:
  WifiChipResult fail(WifiChipResult result, const char* message);
  // The prefix and the API to start the search at.
  bool firmware_prefix(IwlFirmwareName* prefix, IwlApiRange* range);

  const WifiAdapterModel& model_;
  WifiChipBus bus_;
  ByteBuffer file_;
  IwlFirmware firmware_;
  WifiChipIdentity identity_;
  WifiChipState state_ = WifiChipState::Off;
  IwlFirmwareName firmware_name_;
  const char* message_ = "";
  uint32_t hw_rev_ = 0, rf_id_ = 0;
};

}  // namespace hardware
