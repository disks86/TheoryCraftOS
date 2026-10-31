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

// An SD memory card (SD, SDHC and SDXC; not MMC, eMMC or SDIO) over an ISdHost: the identification sequence (CMD0, CMD8, ACMD41 with the 3.3 V window, CMD2, CMD3), the card's CID
// and CSD (capacity, write protection, name and serial), selection, the 4 bit bus when the card's SCR says it has it, and block reads and writes. Default speed (25 MHz, no 1.8 V
// signalling, no high speed switch).
#pragma once
#include "Hardware/StorageDevice/Sd/ISdHost.hpp"

namespace hardware {

struct SdCardInfo {
  uint64_t block_count = 0;           // blocks of 512 bytes
  bool high_capacity = false;         // SDHC / SDXC: addressed by block number
  bool read_only = false;             // the card's own write protection (the CSD bits; the slot's switch is the host's)
  bool bus_4bit = false;
  uint32_t relative_address = 0;
  char name[8] = {};                  // the product name of the CID
  uint32_t serial = 0;
  uint8_t manufacturer = 0;
};

// The fields of the registers, for the host tests and for anyone who has the words.
bool sd_parse_csd(const uint32_t csd[4], uint64_t* block_count, bool* read_only);
void sd_parse_cid(const uint32_t cid[4], SdCardInfo* info);

class SdCard {
 public:
  static constexpr uint32_t kBlockSize = 512;
  explicit SdCard(ISdHost& host) : host_(host) {}

  // Brings the card up from power-on to the transfer state. False if there is no card, it is not an SD memory card, or it fails to answer.
  bool initialize();
  const SdCardInfo& info() const { return info_; }
  bool read(uint64_t lba, uint32_t blocks, ByteView data);
  bool write(uint64_t lba, uint32_t blocks, ConstByteView data);
  uint32_t max_blocks() const { return host_.max_blocks(); }

 private:
  bool app_command(SdCommand& command);
  bool wait_ready();
  uint32_t address_of(uint64_t lba) const { return static_cast<uint32_t>(info_.high_capacity ? lba : lba * kBlockSize); }
  ISdHost& host_;
  SdCardInfo info_;
};

}  // namespace hardware
