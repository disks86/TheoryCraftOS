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

#pragma once
#include <stdint.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AtomI2c.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {

// A simulated DDC bus: the registers of one clock pad and one data pad (mask, enable, readback, output; literal numbers 0x7000 to 0x7007 and bits), the two lines, and a monitor on them with
// its EDID memory, as an I2C slave written independently of any master: start and stop seen on the lines, bits sampled on the rising clock, acknowledges and data driven on the falling
// clock, clock stretching. Shared by the tests of the bit-banging master and of the card that uses it.
struct FakeI2cMonitor final : IVcnRegisters {
  enum : uint32_t { kMaskClk = 0x7000, kEnClk = 0x7001, kYClk = 0x7002, kAClk = 0x7003, kMaskData = 0x7004, kEnData = 0x7005, kYData = 0x7006, kAData = 0x7007 };
  enum : uint32_t { kMaskClkBit = 1u << 3, kEnClkBit = 1u << 9, kYClkBit = 1u << 13, kAClkBit = 1u << 17, kMaskDataBit = 1u << 5, kEnDataBit = 1u << 11, kYDataBit = 1u << 15, kADataBit = 1u << 19 };
  std::map<uint32_t, uint32_t> regs;
  uint32_t writes = 0;
  std::vector<uint8_t> edid;           // the monitor's memory (a multiple of 128 bytes)
  bool present = true;
  uint32_t corrupt_reads = 0;          // the first reads of this many bytes... see `corrupt_blocks`
  uint32_t corrupt_skip = 0;           // reads that come back whole before the damaged ones start
  uint32_t read_starts = 0;
  uint32_t corrupt_block_reads = 0;    // the next this many block reads (by START of the read message) come back with a flipped byte
  uint32_t stretch_polls = 0;          // at the acknowledge of an address the monitor holds the clock low for this many readbacks of it
  bool stretch_forever = false;
  // what the monitor saw
  std::vector<uint8_t> addresses;      // every address byte received
  std::vector<uint8_t> written;        // data bytes written to it
  uint32_t starts = 0, stops = 0, repeated_starts_seen = 0;
  bool stop_seen_last = false;

  // the lines
  bool scl = true, sda = true;
  bool slave_scl_low = false, slave_sda_low = false;
  uint32_t stretch_left = 0;
  // the slave
  enum class S { Idle, Address, Ack, WriteData, WriteAck, ReadData, ReadAck } state = S::Idle;
  uint32_t bit_count = 0;
  uint8_t shift = 0, address = 0;
  bool reading = false;
  uint8_t pointer = 0, segment = 0;
  bool first_write_byte = true;
  uint32_t read_bytes_since_start = 0;
  uint8_t out_byte = 0;

  FakeI2cMonitor() {
    const uint32_t seed = 0x55555555;
    regs[kMaskClk] = seed & ~kMaskClkBit; regs[kMaskData] = seed & ~kMaskDataBit;
    regs[kEnClk] = seed & ~kEnClkBit; regs[kEnData] = seed & ~kEnDataBit;
    regs[kAClk] = seed | kAClkBit; regs[kAData] = seed | kADataBit;
  }
  AtomI2c::Bus record() const {
    AtomI2c::Bus b;
    b.valid = true;
    b.mask_clk_reg = kMaskClk; b.mask_data_reg = kMaskData; b.en_clk_reg = kEnClk; b.en_data_reg = kEnData; b.y_clk_reg = kYClk; b.y_data_reg = kYData; b.a_clk_reg = kAClk; b.a_data_reg = kAData;
    b.mask_clk_mask = kMaskClkBit; b.mask_data_mask = kMaskDataBit; b.en_clk_mask = kEnClkBit; b.en_data_mask = kEnDataBit; b.y_clk_mask = kYClkBit; b.y_data_mask = kYDataBit;
    b.a_clk_mask = kAClkBit; b.a_data_mask = kADataBit;
    return b;
  }
  uint32_t get(uint32_t r) const { auto it = regs.find(r); return it == regs.end() ? 0 : it->second; }

  uint32_t read(uint32_t r) override {
    if (r == kYClk) {
      if (stretch_left > 0 && !stretch_forever) {
        if (--stretch_left == 0) { slave_scl_low = false; update(); }
      }
      return scl ? uint32_t{kYClkBit} : 0u;
    }
    if (r == kYData) return sda ? uint32_t{kYDataBit} : 0u;
    return get(r);
  }
  void write(uint32_t r, uint32_t v) override {
    ++writes;
    regs[r] = v;
    if (r == kEnClk || r == kEnData) update();
  }

  void update() {
    const bool new_scl = !(get(kEnClk) & kEnClkBit) && !slave_scl_low;
    const bool new_sda = !(get(kEnData) & kEnDataBit) && !slave_sda_low;
    const bool old_scl = scl, old_sda = sda;
    scl = new_scl;
    sda = new_sda;
    if (!present) return;
    if (old_scl && new_scl && old_sda && !new_sda) { on_start(); return; }
    if (old_scl && new_scl && !old_sda && new_sda) { on_stop(); return; }
    if (!old_scl && new_scl) on_rising();
    if (old_scl && !new_scl) on_falling();
  }
  void on_start() {
    if (state != S::Idle) ++repeated_starts_seen;
    ++starts;
    state = S::Address; bit_count = 0; shift = 0; slave_sda_low = false; stop_seen_last = false;
  }
  void on_stop() { ++stops; stop_seen_last = true; state = S::Idle; slave_sda_low = false; }
  void on_rising() {
    switch (state) {
      case S::Address: case S::WriteData:
        shift = static_cast<uint8_t>(shift << 1 | (sda ? 1 : 0));
        ++bit_count;
        break;
      case S::ReadAck: master_acked = !sda; break;
      default: break;
    }
  }
  bool master_acked = false;
  void on_falling() {
    switch (state) {
      case S::Address:
        if (bit_count == 8) {
          addresses.push_back(shift);
          address = shift >> 1;
          reading = shift & 1;
          if (address == 0x50 || (address == 0x30 && !reading)) {
            state = S::Ack; slave_sda_low = true;
            if (stretch_polls) { stretch_left = stretch_polls; slave_scl_low = true; }
            if (stretch_forever) slave_scl_low = true;
            first_write_byte = true;
          } else {
            state = S::Idle; slave_sda_low = false;
          }
        }
        break;
      case S::Ack:
        slave_sda_low = false;
        if (reading) { ++read_starts; state = S::ReadData; bit_count = 0; read_bytes_since_start = 0; load_byte(); drive_bit(); }
        else { state = S::WriteData; bit_count = 0; shift = 0; }
        break;
      case S::WriteData:
        if (bit_count == 8) {
          written.push_back(shift);
          if (first_write_byte) { if (address == 0x30) segment = shift; else pointer = shift; first_write_byte = false; }
          state = S::WriteAck; slave_sda_low = true;
        }
        break;
      case S::WriteAck: slave_sda_low = false; state = S::WriteData; bit_count = 0; shift = 0; break;
      case S::ReadData:
        ++bit_count;
        if (bit_count == 8) { slave_sda_low = false; state = S::ReadAck; }
        else drive_bit();
        break;
      case S::ReadAck:
        if (master_acked) { ++pointer; load_byte(); state = S::ReadData; bit_count = 0; drive_bit(); }
        else { state = S::Idle; slave_sda_low = false; }
        break;
      default: break;
    }
    update_lines_only();
  }
  void update_lines_only() { sda = !(get(kEnData) & kEnDataBit) && !slave_sda_low; }
  void load_byte() {
    const size_t at = static_cast<size_t>(segment) * 256 + pointer;
    out_byte = at < edid.size() ? edid[at] : 0xFF;
    if (corrupt_block_reads && read_starts > corrupt_skip && read_bytes_since_start == 5) { out_byte ^= 0x10; --corrupt_block_reads; }
    ++read_bytes_since_start;
  }
  void drive_bit() { slave_sda_low = !((out_byte >> (7 - bit_count)) & 1); }
  void finish_block() {}
};

inline std::vector<uint8_t> make_edid(uint32_t extensions, uint8_t tag_base = 0x02) {
  std::vector<uint8_t> e(128 * (1 + extensions), 0);
  const uint8_t header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  memcpy(e.data(), header, 8);
  e[8] = 0x10; e[9] = 0xAC;   // "DEL"
  e[10] = 0x34; e[11] = 0x12;
  e[18] = 1; e[19] = 4;
  e[126] = static_cast<uint8_t>(extensions);
  for (uint32_t b = 0; b <= extensions; ++b) {
    if (b > 0) { e[b * 128] = tag_base; e[b * 128 + 1] = 3; e[b * 128 + 2] = 4; for (int i = 3; i < 127; ++i) e[b * 128 + i] = static_cast<uint8_t>(b * 16 + i); }
    else for (int i = 20; i < 126; ++i) e[i] = static_cast<uint8_t>(i * 7);
    uint8_t sum = 0;
    for (int i = 0; i < 127; ++i) sum = static_cast<uint8_t>(sum + e[b * 128 + i]);
    e[b * 128 + 127] = static_cast<uint8_t>(0 - sum);
  }
  return e;
}


}  // namespace hardware
