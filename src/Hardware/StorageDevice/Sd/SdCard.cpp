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

#include "Hardware/StorageDevice/Sd/SdCard.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint8_t kGoIdle = 0, kAllSendCid = 2, kSendRelativeAddress = 3, kSetBusWidth = 6, kSelectCard = 7, kSendIfCond = 8, kSendCsd = 9, kStopTransmission = 12, kSendStatus = 13,
                  kSetBlockLength = 16, kReadSingle = 17, kReadMultiple = 18, kWriteSingle = 24, kWriteMultiple = 25, kSdSendOpCond = 41, kSendScr = 51, kAppCommand = 55;
constexpr uint32_t kCardErrors = 0xFDF80008u;                       // the error bits of the card status
constexpr uint32_t kReadyForData = 1u << 8;
constexpr uint32_t kOcrVoltageWindow = 0x00FF8000, kOcrBusy = 0x80000000u, kOcrCcs = 0x40000000u;
constexpr uint32_t kReadyPolls = 100, kReadyUs = 10000;              // up to a second for the card to leave the idle state or finish a write

// Bits hi..lo of the 128 bit register held as four words, the most significant first.
uint32_t field(const uint32_t w[4], int hi, int lo) {
  uint32_t value = 0;
  for (int bit = hi; bit >= lo; --bit) value = (value << 1) | ((w[3 - bit / 32] >> (bit % 32)) & 1u);
  return value;
}
}  // namespace

bool sd_parse_csd(const uint32_t csd[4], uint64_t* block_count, bool* read_only) {
  const uint32_t structure = field(csd, 127, 126);
  uint64_t blocks;
  if (structure == 0) {                                              // CSD version 1.0 (standard capacity)
    const uint32_t read_bl_len = field(csd, 83, 80), c_size = field(csd, 73, 62), c_size_mult = field(csd, 49, 47);
    if (read_bl_len < 9 || read_bl_len > 11) return false;
    const uint64_t bytes = (uint64_t{c_size} + 1) << (c_size_mult + 2) << read_bl_len;
    blocks = bytes / 512;
  } else if (structure == 1) {                                       // CSD version 2.0 (SDHC and SDXC): (C_SIZE + 1) * 512 KiB
    blocks = (uint64_t{field(csd, 69, 48)} + 1) * 1024;
  } else {
    return false;
  }
  if (blocks == 0) return false;
  *block_count = blocks;
  *read_only = field(csd, 13, 12) != 0;                              // permanent or temporary write protect
  return true;
}

void sd_parse_cid(const uint32_t cid[4], SdCardInfo* info) {
  info->manufacturer = static_cast<uint8_t>(field(cid, 127, 120));
  for (int i = 0; i < 5; ++i) {
    const char c = static_cast<char>(field(cid, 103 - 8 * i, 96 - 8 * i));
    info->name[i] = c >= 0x20 && c < 0x7F ? c : '?';
  }
  info->name[5] = 0;
  info->serial = field(cid, 55, 24);
}

bool SdCard::app_command(SdCommand& command) {
  SdCommand prefix;
  prefix.index = kAppCommand;
  prefix.argument = info_.relative_address << 16;
  prefix.response_type = SdResponse::R1;
  if (!host_.command(prefix) || (prefix.response[0] & kCardErrors)) return false;
  return host_.command(command);
}

bool SdCard::initialize() {
  info_ = SdCardInfo();
  if (!host_.card_present() || !host_.power_on()) return false;
  if (!host_.set_clock(400000) || !host_.set_bus_width(1)) return false;
  SdCommand c;
  c.index = kGoIdle;
  host_.command(c);                                                  // (no response)
  // CMD8 tells a version 2 card (which answers with the pattern back) from a version 1 card (which does not answer)
  bool version2 = false;
  c = SdCommand();
  c.index = kSendIfCond;
  c.argument = 0x1AA;
  c.response_type = SdResponse::R1;
  if (host_.command(c)) {
    if ((c.response[0] & 0xFFF) != 0x1AA) return false;              // a card that does not work at 2.7 to 3.6 V
    version2 = true;
  }
  // ACMD41 until the card has powered up
  bool ready = false;
  uint32_t ocr = 0;
  for (uint32_t i = 0; i < kReadyPolls && !ready; ++i) {
    c = SdCommand();
    c.index = kSdSendOpCond;
    c.argument = kOcrVoltageWindow | (version2 ? kOcrCcs : 0u);
    c.response_type = SdResponse::R3;
    if (!app_command(c)) return false;                               // (no answer to CMD55 / ACMD41: not an SD memory card)
    ocr = c.response[0];
    ready = ocr & kOcrBusy;
    if (!ready) delay_us(kReadyUs);
  }
  if (!ready) return false;
  info_.high_capacity = version2 && (ocr & kOcrCcs);
  c = SdCommand();
  c.index = kAllSendCid;
  c.response_type = SdResponse::R2;
  if (!host_.command(c)) return false;
  sd_parse_cid(c.response, &info_);
  c = SdCommand();
  c.index = kSendRelativeAddress;
  c.response_type = SdResponse::R1;
  if (!host_.command(c)) return false;
  info_.relative_address = c.response[0] >> 16;
  if (info_.relative_address == 0) return false;
  c = SdCommand();
  c.index = kSendCsd;
  c.argument = info_.relative_address << 16;
  c.response_type = SdResponse::R2;
  if (!host_.command(c) || !sd_parse_csd(c.response, &info_.block_count, &info_.read_only)) return false;
  c = SdCommand();
  c.index = kSelectCard;
  c.argument = info_.relative_address << 16;
  c.response_type = SdResponse::R1b;
  if (!host_.command(c) || (c.response[0] & kCardErrors)) return false;
  // the SCR says whether the card has the 4 bit bus
  uint8_t scr[8] = {};
  c = SdCommand();
  c.index = kSendScr;
  c.response_type = SdResponse::R1;
  SdCommand prefix;
  prefix.index = kAppCommand;
  prefix.argument = info_.relative_address << 16;
  prefix.response_type = SdResponse::R1;
  if (host_.command(prefix) && host_.read_blocks(c, 8, 1, ByteView(scr, 8)) && (scr[1] & 0x04)) {
    c = SdCommand();
    c.index = kSetBusWidth;
    c.argument = 2;
    c.response_type = SdResponse::R1;
    if (app_command(c) && !(c.response[0] & kCardErrors) && host_.set_bus_width(4)) info_.bus_4bit = true;
  }
  if (!info_.high_capacity) {
    c = SdCommand();
    c.index = kSetBlockLength;
    c.argument = kBlockSize;
    c.response_type = SdResponse::R1;
    if (!host_.command(c) || (c.response[0] & kCardErrors)) return false;
  }
  return host_.set_clock(25000000);
}

bool SdCard::wait_ready() {
  for (uint32_t i = 0; i < kReadyPolls; ++i) {
    SdCommand c;
    c.index = kSendStatus;
    c.argument = info_.relative_address << 16;
    c.response_type = SdResponse::R1;
    if (!host_.command(c) || (c.response[0] & kCardErrors)) return false;
    if (c.response[0] & kReadyForData) return true;
    delay_us(kReadyUs);
  }
  return false;
}

bool SdCard::read(uint64_t lba, uint32_t blocks, ByteView data) {
  if (blocks == 0 || data.size() != uint64_t{blocks} * kBlockSize || blocks > host_.max_blocks() || lba >= info_.block_count || blocks > info_.block_count - lba) return false;
  SdCommand c;
  c.index = blocks == 1 ? kReadSingle : kReadMultiple;
  c.argument = address_of(lba);
  c.response_type = SdResponse::R1;
  const bool ok = host_.read_blocks(c, kBlockSize, blocks, data) && !(c.response[0] & kCardErrors);
  if (blocks > 1) {
    SdCommand stop;
    stop.index = kStopTransmission;
    stop.response_type = SdResponse::R1b;
    if (!host_.command(stop) && ok) return false;
  }
  return ok;
}

bool SdCard::write(uint64_t lba, uint32_t blocks, ConstByteView data) {
  if (info_.read_only || blocks == 0 || data.size() != uint64_t{blocks} * kBlockSize || blocks > host_.max_blocks() || lba >= info_.block_count || blocks > info_.block_count - lba) return false;
  SdCommand c;
  c.index = blocks == 1 ? kWriteSingle : kWriteMultiple;
  c.argument = address_of(lba);
  c.response_type = SdResponse::R1;
  const bool ok = host_.write_blocks(c, kBlockSize, blocks, data) && !(c.response[0] & kCardErrors);
  if (blocks > 1) {
    SdCommand stop;
    stop.index = kStopTransmission;
    stop.response_type = SdResponse::R1b;
    if (!host_.command(stop) && ok) return false;
  }
  return ok && wait_ready();
}

}  // namespace hardware
