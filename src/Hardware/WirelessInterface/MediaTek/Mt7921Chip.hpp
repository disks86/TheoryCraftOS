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

// The MT7921 / MT7922 / MT7920 on PCIe (AMD RZ608 / RZ616), from power-on to "firmware running", after mt7921_pci_probe (mt7921/pci.c), mt7921_dma_init,
// mt7921e_mcu_init and mt792x_load_firmware / mt76_connac2_load_patch / mt76_connac2_load_ram of Linux mt76:
//
//   1. the own handshake: give the chip to the firmware, then take it back for the driver (MT_CONN_ON_LPCTL);
//   2. the chip id and revision (MT_HW_CHIPID / MT_HW_REV through the L1 window), which name the firmware files;
//   3. the WFSYS reset;
//   4. the ROM patch and RAM code files, parsed;
//   5. WFDMA0: stopped, the command (17), firmware download (16) and MCU event (RX 0) rings set up, started;
//   6. the patch: PATCH_SEM_CONTROL (get), per section PATCH_START_REQ + FW_SCATTER, PATCH_FINISH_REQ, PATCH_SEM_CONTROL (release); skipped if the chip says the
//      patch is there already;
//   7. the RAM code: per region TARGET_ADDRESS_LEN_REQ + FW_SCATTER, then FW_START_REQ with the override address;
//   8. waiting for MT_CONN_ON_MISC to say the N9 firmware runs.
//
// After that the chip is FirmwareLoaded and start() returns Unsupported: the first commands to the running firmware (NIC capability, which carries the MAC address,
// and everything after it) are not written. Commands are polled; interrupts stay off. Tested against a register and DMA model only (tests/MediaTekWifiTest.cpp).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"
#include "Hardware/WirelessInterface/MediaTek/Mt7921Pcie.hpp"
#include "Hardware/WirelessInterface/MediaTek/MtFirmware.hpp"
#include "Hardware/WirelessInterface/MediaTek/MtMcu.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

class Mt7921Chip final : public IWifiChip {
 public:
  static constexpr uint32_t kScatterChunk = 4096;          // __mt76_mcu_send_firmware max_len on PCIe
  static constexpr uint32_t kCommandBuffer = 512;
  static constexpr uint32_t kResponseTries = 3000, kResponseStepUs = 1000;   // 3 s, as mt7921_mcu_send_message's timeout
  static constexpr uint32_t kTxTries = 10000, kTxStepUs = 10;

  Mt7921Chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);
  ~Mt7921Chip() { stop(); }

  const WifiAdapterModel& model() const override { return model_; }
  [[nodiscard]] WifiChipResult start(IWifiFirmwareSource& firmware) override;
  void stop() override;
  WifiChipState state() const override { return state_; }
  const WifiChipIdentity& identity() const override { return identity_; }
  MacAddress mac_address() const override { return MacAddress{{0, 0, 0, 0, 0, 0}}; }
  const char* firmware_name() const override { return ram_name_; }
  const char* message() const override { return message_; }

  const char* patch_name() const { return patch_name_; }
  const MtConnacPatch& patch() const { return patch_; }
  const MtConnacRam& ram() const { return ram_; }
  bool patch_was_present() const { return patch_was_present_; }

 private:
  WifiChipResult fail(WifiChipResult result, const char* message);
  WifiChipResult identify(Mt7921Registers& regs);
  WifiChipResult load_files(IWifiFirmwareSource& firmware);
  WifiChipResult init_dma(Mt7921Registers& regs);
  WifiChipResult download_patch(Mt7921Registers& regs);
  WifiChipResult download_ram(Mt7921Registers& regs);
  // Sends a legacy MCU command on the WM ring; with `wait`, waits for the event with its sequence number (and gives its status byte, if asked).
  WifiChipResult command(Mt7921Registers& regs, uint8_t cid, ConstByteView payload, bool wait, uint8_t* status);
  WifiChipResult scatter(Mt7921Registers& regs, ConstByteView data);
  void release_rings();

  const WifiAdapterModel& model_;
  WifiChipBus bus_;
  allocators::IAllocator& allocator_;
  WifiChipState state_ = WifiChipState::Off;
  WifiChipIdentity identity_;
  const char* message_ = "not started";
  const char* patch_name_ = "";
  const char* ram_name_;
  ByteBuffer patch_file_, ram_file_;
  MtConnacPatch patch_;
  MtConnacRam ram_;
  Mt7921Ring fwdl_, wm_, event_;
  MtMcuSequence sequence_;
  bool patch_was_present_ = false;
};

}  // namespace hardware
