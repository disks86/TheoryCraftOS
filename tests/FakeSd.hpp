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

// A model of an SD card and of a host that talks to it at the level of ISdHost: the states of the identification sequence, the registers (OCR, CID, CSD, SCR, status), addressing of
// standard and high capacity cards, single and multiple block transfers with the stop command, a write that keeps the card busy for a while, and the slot (card present, write
// protect switch, generation).
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
#include "Hardware/StorageDevice/Sd/ISdHost.hpp"

using namespace hardware;

struct FakeSdCard {
  // what kind of card
  bool version2 = true, high_capacity = true, supports_4bit = true, is_sd = true, csd_write_protected = false;
  uint64_t blocks = 4096;                      // of 512 bytes
  uint32_t rca = 0x1234;
  uint32_t power_up_polls = 3;                 // ACMD41 answers busy this many times
  uint32_t busy_after_write = 2;               // CMD13 says "not ready" this many times after a write
  std::vector<uint8_t> data;
  // state
  enum class State { Idle, Ready, Identification, Standby, Transfer, Receiving } state = State::Idle;
  bool app_next = false;
  uint32_t polls_left = 0, busy_left = 0, block_length = 512;
  uint64_t next_block = 0;
  bool streaming = false;
  bool inject_error = false;
  uint32_t errors_seen = 0;

  explicit FakeSdCard(uint64_t block_count = 4096) : blocks(block_count) { data.assign(blocks * 512, 0); }
  void reset() { state = State::Idle; app_next = false; polls_left = power_up_polls; busy_left = 0; block_length = 512; streaming = false; }

  static void put_bits(uint32_t w[4], int hi, int lo, uint32_t value) {
    for (int bit = lo; bit <= hi; ++bit) {
      const uint32_t v = (value >> (bit - lo)) & 1u;
      uint32_t& word = w[3 - bit / 32];
      word = (word & ~(1u << (bit % 32))) | (v << (bit % 32));
    }
  }
  void cid(uint32_t w[4]) const {
    memset(w, 0, 16);
    put_bits(w, 127, 120, 0x03);
    const char name[] = "SD16G";
    for (int i = 0; i < 5; ++i) put_bits(w, 103 - 8 * i, 96 - 8 * i, static_cast<uint8_t>(name[i]));
    put_bits(w, 55, 24, 0xCAFE0123u);
  }
  void csd(uint32_t w[4]) const {
    memset(w, 0, 16);
    if (high_capacity) {
      put_bits(w, 127, 126, 1);
      put_bits(w, 69, 48, static_cast<uint32_t>(blocks / 1024 - 1));
    } else {
      put_bits(w, 127, 126, 0);
      put_bits(w, 83, 80, 9);                                  // 512 byte blocks
      // blocks = (C_SIZE + 1) * 2^(C_SIZE_MULT + 2): use mult 7 (x512) so C_SIZE + 1 = blocks / 512
      put_bits(w, 49, 47, 7);
      put_bits(w, 73, 62, static_cast<uint32_t>(blocks / 512 - 1));
    }
    put_bits(w, 13, 12, csd_write_protected ? 1 : 0);
  }
  uint32_t status() const {
    uint32_t s = (static_cast<uint32_t>(state == State::Transfer ? 4 : state == State::Standby ? 3 : state == State::Identification ? 2 : state == State::Ready ? 1 : 0) << 9);
    if (busy_left == 0) s |= 1u << 8;                          // READY_FOR_DATA
    if (app_next) s |= 1u << 5;                                // APP_CMD
    return s;
  }
  bool address_ok(uint32_t argument, uint64_t* block) const {
    if (high_capacity) { *block = argument; return true; }
    if (argument % 512) return false;
    *block = argument / 512;
    return true;
  }

  // Returns false when the card does not answer.
  bool command(SdCommand& c) {
    const bool app = app_next;
    app_next = false;
    if (!is_sd && c.index != 0) return false;                  // an MMC card ignores SD commands (CMD0 only)
    switch (c.index) {
      case 0: reset(); return true;
      case 8:
        if (!version2 || state != State::Idle) return false;
        c.response[0] = c.argument & 0xFFF;
        return true;
      case 55:
        if (state == State::Idle || state == State::Ready || state == State::Standby || state == State::Transfer) { app_next = true; c.response[0] = status() | (1u << 5); return true; }
        return false;
      case 41:
        if (!app) return false;
        if (state != State::Idle) return false;
        {
          uint32_t ocr = 0x00FF8000;
          if (polls_left > 0) --polls_left;
          if (polls_left == 0) {
            ocr |= 0x80000000u;
            if (version2 && high_capacity && (c.argument & 0x40000000u)) ocr |= 0x40000000u;
            state = State::Ready;
          }
          c.response[0] = ocr;
        }
        return true;
      case 2: if (state != State::Ready) return false; cid(c.response); state = State::Identification; return true;
      case 3: if (state != State::Identification) return false; state = State::Standby; c.response[0] = (rca << 16) | status(); return true;
      case 9: if (state != State::Standby || (c.argument >> 16) != rca) return false; csd(c.response); return true;
      case 7:
        if ((c.argument >> 16) != rca || state != State::Standby) return false;
        state = State::Transfer;
        c.response[0] = status();
        return true;
      case 13:
        if ((c.argument >> 16) != rca) return false;
        c.response[0] = status();
        if (busy_left > 0) --busy_left;
        return true;
      case 16: block_length = c.argument; c.response[0] = status(); return true;
      case 6:
        if (!app || state != State::Transfer) return false;
        c.response[0] = status();
        return true;
      case 12: streaming = false; c.response[0] = status(); return true;
      default: return false;
    }
  }
};

struct FakeSdHost final : ISdHost {
  FakeSdCard* card = nullptr;
  bool write_protect_switch = false;
  uint32_t gen = 1;
  bool powered = false;
  bool fail_power = false;
  uint32_t clock = 0, width = 0, power_ons = 0, power_offs = 0, max = 64;
  std::vector<uint32_t> clocks;
  std::vector<uint8_t> commands_seen;
  uint32_t transfers = 0;

  bool card_present() override { return card != nullptr; }
  bool write_protected() override { return write_protect_switch; }
  uint32_t generation() override { return gen; }
  void insert(FakeSdCard* c) { card = c; ++gen; if (c) c->reset(); }
  void remove() { card = nullptr; powered = false; ++gen; }

  bool power_on() override { if (fail_power || !card) return false; powered = true; ++power_ons; card->reset(); return true; }
  void power_off() override { powered = false; ++power_offs; }
  bool set_clock(uint32_t hz) override { clock = hz; clocks.push_back(hz); return powered; }
  bool set_bus_width(uint32_t bits) override { width = bits; return powered; }
  uint32_t max_blocks() const override { return max; }

  bool command(SdCommand& c) override {
    commands_seen.push_back(c.index);
    if (!card || !powered) return false;
    if (c.index == 6 && card->app_next && c.argument == 2 && !card->supports_4bit) { card->app_next = false; return false; }
    return card->command(c);
  }
  bool read_blocks(SdCommand& c, uint32_t block_size, uint32_t blocks, ByteView data) override {
    commands_seen.push_back(c.index);
    if (!card || !powered || data.size() != uint64_t{block_size} * blocks) return false;
    ++transfers;
    if (c.index == 51) {                                       // SCR
      const bool app = card->app_next;
      card->app_next = false;
      if (!app || block_size != 8 || card->state != FakeSdCard::State::Transfer) return false;
      uint8_t scr[8] = {0x02, static_cast<uint8_t>(card->supports_4bit ? 0x05 : 0x01), 0, 0, 0, 0, 0, 0};
      for (size_t i = 0; i < 8; ++i) data[i] = scr[i];
      c.response[0] = card->status();
      return true;
    }
    if ((c.index != 17 && c.index != 18) || card->state != FakeSdCard::State::Transfer || block_size != 512) return false;
    if ((c.index == 17) != (blocks == 1)) return false;
    uint64_t block;
    if (!card->address_ok(c.argument, &block) || block + blocks > card->blocks) { c.response[0] = 1u << 31; return true; }
    if (card->inject_error) { card->inject_error = false; ++card->errors_seen; return false; }
    for (size_t i = 0; i < data.size(); ++i) data[i] = card->data[block * 512 + i];
    c.response[0] = card->status();
    card->streaming = blocks > 1;
    return true;
  }
  bool write_blocks(SdCommand& c, uint32_t block_size, uint32_t blocks, ConstByteView data) override {
    commands_seen.push_back(c.index);
    if (!card || !powered || data.size() != uint64_t{block_size} * blocks) return false;
    ++transfers;
    if ((c.index != 24 && c.index != 25) || card->state != FakeSdCard::State::Transfer || block_size != 512) return false;
    if ((c.index == 24) != (blocks == 1)) return false;
    uint64_t block;
    if (!card->address_ok(c.argument, &block) || block + blocks > card->blocks) { c.response[0] = 1u << 31; return true; }
    if (card->inject_error) { card->inject_error = false; ++card->errors_seen; return false; }
    for (size_t i = 0; i < data.size(); ++i) card->data[block * 512 + i] = data[i];
    card->busy_left = card->busy_after_write;
    c.response[0] = card->status();
    card->streaming = blocks > 1;
    return true;
  }
};
