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

// The Broadcom/Cypress FullMAC chip (brcmfmac) on PCIe and SDIO, from power-on to "firmware running", after Linux brcmfmac chip.c (brcmf_chip_attach), pcie.c
// (brcmf_pcie_setup, brcmf_pcie_download_fw_nvram) and sdio.c (brcmf_sdio_download_firmware):
//
//   1. reach the backplane (PCIe: BAR0 window; SDIO: function 1, ALP clock), read ChipID, read the enumeration ROM into memory and list the cores;
//   2. hold the ARM (CR4, CA7 or CM3) halted, on PCIe reset the chip through chipcommon's watchdog, find the RAM's base and size;
//   3. name the files from the chip ID and revision (BrcmfFirmware.hpp), board-specific names first, and load the image, the NVRAM (stripped to what the firmware
//      reads), the CLM and txcap blobs (kept for the FullMAC step);
//   4. image at the start of RAM, NVRAM (and on Apple chips a random seed) at its end, the ARM released with the image's first word as the reset vector;
//   5. wait for the firmware to put the address of its shared structure into the last RAM word, check and parse it, and read the firmware's console for its version.
//
// Then the chip is FirmwareLoaded and start() returns Unsupported: the FullMAC commands that would follow (msgbuf rings on PCIe, BCDC over SDPCM frames on SDIO,
// "cur_etheraddr", the CLM download) are not written yet, so the hardware address stays unknown. USB adapters (a bootloader protocol on bulk endpoints) have no chip
// here; make_broadcom_wifi_chip returns null for them and for the SoftMAC (brcmsmac, b43) and proprietary (wl) families.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Crypto/Random.hpp"
#include "Hardware/WirelessInterface/Broadcom/BrcmfBackplane.hpp"
#include "Hardware/WirelessInterface/Broadcom/BrcmfFirmware.hpp"
#include "Hardware/WirelessInterface/Broadcom/BrcmfPcie.hpp"
#include "Hardware/WirelessInterface/Broadcom/BrcmfSdio.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class BrcmfChip final : public IWifiChip, private IBrcmfActivate {
 public:
  BrcmfChip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);
  ~BrcmfChip();
  BrcmfChip(const BrcmfChip&) = delete;
  BrcmfChip& operator=(const BrcmfChip&) = delete;

  // Board types tried, in this order, before the plain file names (brcm_alt_fw_path): the devicetree compatible ("raspberrypi,4-model-b"), the DMI one
  // ("Raspberry Pi Foundation-Raspberry Pi 4 Model B"), see brcmf_board_type_from_*. False when full or too long.
  [[nodiscard]] bool add_board_type(StringView board_type);
  // Where the random seed comes from (chips that want one); without, a fixed sequence is used and the firmware gets no real entropy.
  void set_random(crypto::IRandom* random) { random_ = random; }
  // Overrides whether the seed is written (default: the Apple chips 4364, 4377, 4378, 4387, which Linux gives one to).
  void set_random_seed(bool enabled) { seed_override_ = enabled ? 1 : 2; }
  // A platform address that replaces the NVRAM's macaddr (brcmf_fw_nvram_strip with a platform MAC).
  void set_platform_mac(const MacAddress& mac);

  const WifiAdapterModel& model() const override { return model_; }
  [[nodiscard]] WifiChipResult start(IWifiFirmwareSource& firmware) override;
  void stop() override;
  WifiChipState state() const override { return state_; }
  const WifiChipIdentity& identity() const override { return identity_; }
  MacAddress mac_address() const override { return MacAddress{}; }
  const char* firmware_name() const override;
  const char* message() const override { return message_; }

  // What start() found (valid from Identified on).
  const BrcmfChipId& chip_id() const { return chip_id_; }
  const BrcmfCoreList& cores() const { return cores_; }
  const BrcmfRamInfo& ram() const { return ram_; }
  const char* chip_name() const { return chip_name_.c_str(); }
  // The files used ("" for an optional one that was not there).
  const char* nvram_name() const { return nvram_name_.c_str(); }
  const char* clm_name() const { return clm_name_.c_str(); }
  const char* txcap_name() const { return txcap_name_.c_str(); }
  uint64_t nvram_bytes() const { return nvram_bytes_; }      // the stripped image written to the chip
  uint64_t clm_bytes() const { return clm_.size(); }
  // Valid from FirmwareLoaded on.
  uint32_t shared_address() const { return shared_address_; }
  const BrcmfPcieShared& pcie_shared() const { return pcie_shared_; }
  const BrcmfSdioShared& sdio_shared() const { return sdio_shared_; }

 private:
  WifiChipResult fail(WifiChipResult result, const char* message);
  [[nodiscard]] WifiChipResult identify();
  [[nodiscard]] WifiChipResult download(IWifiFirmwareSource& firmware);
  [[nodiscard]] WifiChipResult wait_for_firmware(uint32_t baseline);
  [[nodiscard]] bool load(IWifiFirmwareSource& firmware, const char* extension, ByteBuffer& out, BrcmfFileName& used);
  [[nodiscard]] bool fill_random(ByteView out);
  void read_console(uint32_t console_addr);
  bool activate(uint32_t reset_vector) override;
  bool wants_seed() const;

  const WifiAdapterModel& model_;
  WifiChipBus bus_;
  allocators::IAllocator& allocator_;
  BrcmfBusType bus_type_;
  UniquePointer<BrcmfPcieBackplane> pcie_;
  UniquePointer<BrcmfSdioBackplane> sdio_;
  IBrcmfBackplane* backplane_ = nullptr;       // the one of the two in use (non-owning)

  WifiChipState state_ = WifiChipState::Off;
  WifiChipIdentity identity_{};
  const char* message_ = "not started";
  BrcmfChipId chip_id_{};
  BrcmfChipName chip_name_;
  BrcmfCoreList cores_{};
  BrcmfRamInfo ram_{};
  uint32_t erom_[brcmf_bp::kEromBytes / 4] = {};

  static constexpr uint32_t kMaxBoardTypes = 2;
  BrcmfFileName board_types_[kMaxBoardTypes];
  uint32_t board_type_count_ = 0;
  BrcmfFileName firmware_name_, nvram_name_, clm_name_, txcap_name_;
  ByteBuffer clm_;
  ByteBuffer txcap_;
  uint64_t nvram_bytes_ = 0;
  crypto::IRandom* random_ = nullptr;
  uint8_t seed_override_ = 0;                  // 0: by chip, 1: on, 2: off
  bool has_platform_mac_ = false;
  uint8_t platform_mac_[6] = {};

  uint32_t shared_address_ = 0;
  BrcmfPcieShared pcie_shared_{};
  BrcmfSdioShared sdio_shared_{};
};

}  // namespace hardware
