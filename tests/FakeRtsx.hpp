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

// A model of the Realtek card reader chip at the level of its registers, written from the same sources as the driver: the register window (HAIMR), the command queue executed from memory (command
// words, the bytes of READ and CHECK commands coming back at the start of the buffer), the SD registers, the ping-pong buffer, ADMA descriptors, the interrupt status register, and a card behind
// it (FakeSd.hpp). It does what the chip is told: SD_TRANSFER with a mode runs a command, a command with a data stage or a DMA.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
#include "FakeSd.hpp"
#include "Hardware/Bus/Rtsx/RtsxRegisters.hpp"

using namespace hardware;

struct FakeRtsx final : IRtsxRegisters {
  FakeSdHost sd;                                  // the slot with its card
  uint8_t regs[0x10000] = {};
  uint32_t hcbar = 0, hdbar = 0, hdbctlr = 0, bier = 0, bipr_events = 0, haimr_result = 0;
  bool sd_event = false;
  // knobs
  bool fail_commands = false, corrupt_crc7 = false, fail_dma = false, dead = false;
  // statistics
  uint32_t queues = 0, stops = 0, dmas = 0, commands = 0;
  std::vector<uint32_t> last_queue;
  // transfer state
  bool dma_pending = false, dma_read = false, write_pending = false;
  uint32_t dma_bytes = 0;
  std::vector<uint8_t> dma_data;
  SdCommand pending_command;
  uint8_t pending_write_index = 0;
  uint32_t pending_write_arg = 0;
  bool was_powered = false;

  FakeRtsx() { memset(regs, 0, sizeof regs); sd.powered = false; }

  uint8_t& reg(uint16_t a) { return regs[a]; }
  static uint8_t* pointer(uint32_t address) { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(address)); }   // the test allocator hands out memory below 4 GiB

  uint32_t read32(uint32_t offset) override {
    if (dead) return 0xFFFFFFFFu;
    switch (offset) {
      case rtsx::kBipr: {
        uint32_t v = bipr_events;
        if (sd.card_present()) v |= rtsx::kSdExist;
        if (sd.write_protect_switch) v |= rtsx::kSdWriteProtect;
        if (sd_event) v |= rtsx::kSdInt;
        return v;
      }
      case rtsx::kHaimr: return haimr_result;
      case rtsx::kBier: return bier;
      default: return 0;
    }
  }
  void write32(uint32_t offset, uint32_t value) override {
    switch (offset) {
      case rtsx::kHcbar: hcbar = value; break;
      case rtsx::kHcbctlr:
        if (value & rtsx::kStopCmd) { ++stops; bipr_events &= ~(rtsx::kTransOkInt | rtsx::kTransFailInt); break; }
        if (value & rtsx::kStartCmd) run_queue(value & 0x00FFFFFF);
        break;
      case rtsx::kHdbar: hdbar = value; break;
      case rtsx::kHdbctlr:
        hdbctlr = value;
        if (value & rtsx::kStopDma) break;
        if (value & rtsx::kTrigDma) run_dma(value);
        break;
      case rtsx::kHaimr: haimr(value); break;
      case rtsx::kBier: bier = value; break;
      case rtsx::kBipr:
        bipr_events &= ~value;                                    // write one to clear
        if (value & rtsx::kSdInt) sd_event = false;
        break;
      default: break;
    }
  }

  void haimr(uint32_t v) {
    const uint16_t addr = static_cast<uint16_t>(0xC000 | ((v >> 16) & 0x3FFF));     // (the address field has 14 bits: the registers are all at 0xC000 and above)
    const uint8_t mask = v >> 8, data = v;
    if (v & 0x40000000) apply(addr, mask, data);                  // write
    haimr_result = reg(addr) & 0xFF;                              // (the busy bit is clear: done at once)
  }

  // A register write, with what it does.
  void apply(uint16_t addr, uint8_t mask, uint8_t data) {
    uint8_t& r = reg(addr);
    r = static_cast<uint8_t>((r & ~mask) | (data & mask));
    if (addr == rtsx::kSdTransfer && (data & rtsx::kSdTransferStart)) transfer(data & 0x0F);
    if (addr == rtsx::kCardPwrCtl || addr == rtsx::kCardOe || addr == rtsx::kCardClkEn) {
      const bool powered = (reg(rtsx::kCardPwrCtl) & 3) == 0 && (reg(rtsx::kCardOe) & rtsx::kSdOutputEn) && (reg(rtsx::kCardClkEn) & rtsx::kSdClkEn);
      if (powered && !was_powered && sd.card) { sd.powered = true; sd.card->reset(); }
      if (!powered) sd.powered = false;
      was_powered = powered;
    }
    if (addr == rtsx::kCardStop && (data & rtsx::kSdClrErr)) reg(rtsx::kSdTransfer) = 0;
    if (addr == rtsx::kSdCfg1) {
      sd.width = (reg(rtsx::kSdCfg1) & 3) == 1 ? 4 : 1;
    }
  }

  uint32_t be(uint16_t first) { return (uint32_t{reg(first)} << 24) | (uint32_t{reg(static_cast<uint16_t>(first + 1))} << 16) | (uint32_t{reg(static_cast<uint16_t>(first + 2))} << 8) | reg(static_cast<uint16_t>(first + 3)); }
  void put_be(uint16_t first, uint32_t v) { for (int i = 0; i < 4; ++i) reg(static_cast<uint16_t>(first + i)) = static_cast<uint8_t>(v >> (24 - 8 * i)); }

  SdResponse response_of(uint8_t cfg2) const {
    switch (cfg2 & 0x0F) {
      case rtsx::kSdRspTypeR0: return SdResponse::None;
      case rtsx::kSdRspTypeR1: return SdResponse::R1;
      case rtsx::kSdRspTypeR1b: return SdResponse::R1b;
      case rtsx::kSdRspTypeR2: return SdResponse::R2;
      case rtsx::kSdRspTypeR3: return SdResponse::R3;
      default: return SdResponse::R1;
    }
  }

  // SD_TRANSFER was started in `mode`.
  void transfer(uint8_t mode) {
    ++commands;
    bool ok = !fail_commands && sd.card && sd.powered;
    reg(rtsx::kSdStat1) = 0;
    const uint8_t index = reg(rtsx::kSdCmd0) & 0x3F;
    const uint32_t arg = be(rtsx::kSdCmd1);
    const uint32_t bytes = reg(rtsx::kSdByteCntL) | (uint32_t{reg(rtsx::kSdByteCntH)} << 8);
    const uint32_t blocks = reg(rtsx::kSdBlockCntL) | (uint32_t{reg(rtsx::kSdBlockCntH)} << 8);
    if (mode == rtsx::kSdTmCmdRsp) {
      SdCommand c;
      c.index = index;
      c.argument = arg;
      c.response_type = response_of(reg(rtsx::kSdCfg2));
      if (index == 24 || index == 25) {                           // a write command: the data follows (AUTO_WRITE_3)
        pending_write_index = index;
        pending_write_arg = arg;
        write_pending = true;
        c.response[0] = sd.card ? sd.card->status() : 0;
      } else if (ok) {
        ok = sd.command(c);
      }
      if (ok) {
        if (c.response_type == SdResponse::R2) {
          reg(rtsx::kPpbufBase2) = 0x3F;
          for (int i = 0; i < 15; ++i) reg(static_cast<uint16_t>(rtsx::kPpbufBase2 + 1 + i)) = static_cast<uint8_t>(c.response[i / 4] >> (24 - 8 * (i % 4)));
        } else if (c.response_type != SdResponse::None) {
          reg(rtsx::kSdCmd0) = c.response_type == SdResponse::R3 ? 0x3F : index;
          put_be(rtsx::kSdCmd1, c.response[0]);
        }
        if (corrupt_crc7 && c.response_type != SdResponse::R3) reg(rtsx::kSdStat1) = rtsx::kSdCrc7Err;
      }
    } else if (mode == rtsx::kSdTmNormalRead) {                   // one block into the ping-pong buffer
      SdCommand c;
      c.index = index;
      c.argument = arg;
      std::vector<uint8_t> data(bytes);
      if (ok) ok = sd.read_blocks(c, bytes, 1, ByteView(data.data(), data.size()));
      if (ok) for (uint32_t i = 0; i < bytes && i < 512; ++i) reg(static_cast<uint16_t>(rtsx::kPpbufBase2 + i)) = data[i];
    } else if (mode == rtsx::kSdTmAutoRead2) {                    // several blocks by DMA (the data moves when the DMA is started)
      SdCommand c;
      c.index = index;
      c.argument = arg;
      dma_data.assign(uint64_t{blocks} * 512, 0);
      if (ok) ok = sd.read_blocks(c, 512, blocks, ByteView(dma_data.data(), dma_data.size()));
      dma_pending = ok;
      dma_read = true;
      dma_bytes = blocks * 512;
    } else if (mode == rtsx::kSdTmAutoWrite3) {
      if (!write_pending || !ok) {
        ok = false;
      } else if (blocks == 1) {                                   // from the ping-pong buffer
        std::vector<uint8_t> data(512);
        for (uint32_t i = 0; i < 512; ++i) data[i] = reg(static_cast<uint16_t>(rtsx::kPpbufBase2 + i));
        SdCommand c;
        c.index = pending_write_index;
        c.argument = pending_write_arg;
        ok = sd.write_blocks(c, 512, 1, ConstByteView(data.data(), 512));
        write_pending = false;
      } else {                                                    // by DMA
        dma_pending = true;
        dma_read = false;
        dma_bytes = blocks * 512;
      }
    } else {
      ok = false;
    }
    reg(rtsx::kSdTransfer) = ok ? (rtsx::kSdTransferEnd | rtsx::kSdStatIdle) : rtsx::kSdTransferErr;
  }

  void run_queue(uint32_t bytes) {
    ++queues;
    const uint32_t words = bytes / 4;
    last_queue.clear();
    std::vector<uint32_t> queue(words);
    for (uint32_t i = 0; i < words; ++i) {
      const uint8_t* p = pointer(hcbar) + 4 * i;
      queue[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    last_queue = queue;
    std::vector<uint8_t> results;
    bool failed = false;
    dma_pending = false;
    for (uint32_t w : queue) {
      const uint32_t type = w >> 30;
      const uint16_t addr = static_cast<uint16_t>(0xC000 | ((w >> 16) & 0x3FFF));
      const uint8_t mask = w >> 8, data = w;
      if (type == rtsx::kWriteRegCmd) {
        apply(addr, mask, data);
      } else if (type == rtsx::kReadRegCmd) {
        results.push_back(reg(addr));
      } else if (type == rtsx::kCheckRegCmd) {
        results.push_back(reg(addr));
        if ((reg(addr) & mask) != (data & mask)) { failed = true; break; }
      }
    }
    uint8_t* out = pointer(hcbar);
    for (size_t i = 0; i < results.size(); ++i) out[i] = results[i];
    if (failed) bipr_events |= rtsx::kTransFailInt;
    else if (!dma_pending) bipr_events |= rtsx::kTransOkInt;       // (with a DMA to follow, the transfer completes with it)
  }

  void run_dma(uint32_t control) {
    ++dmas;
    if (!dma_pending || fail_dma) { bipr_events |= rtsx::kTransFailInt; dma_pending = false; return; }
    const uint8_t* d = pointer(hdbar);
    uint64_t descriptor = 0;
    for (int i = 7; i >= 0; --i) descriptor = (descriptor << 8) | d[i];
    const uint32_t address = static_cast<uint32_t>(descriptor >> 32), length = static_cast<uint32_t>((descriptor >> 12) & 0xFFFFF);
    const bool read = (control >> 29) & 1;
    bool ok = (descriptor & 3) == 3 && (descriptor & 0x20) && length == dma_bytes && read == dma_read;
    if (ok && read) {
      memcpy(pointer(address), dma_data.data(), length);
    } else if (ok) {
      std::vector<uint8_t> data(pointer(address), pointer(address) + length);
      SdCommand c;
      c.index = pending_write_index;
      c.argument = pending_write_arg;
      ok = sd.write_blocks(c, 512, length / 512, ConstByteView(data.data(), data.size()));
      write_pending = false;
    }
    dma_pending = false;
    bipr_events |= ok ? rtsx::kTransOkInt : rtsx::kTransFailInt;
  }
};
