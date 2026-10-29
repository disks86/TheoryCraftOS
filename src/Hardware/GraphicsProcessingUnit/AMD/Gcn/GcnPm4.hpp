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

// PM4 packets for the graphics ring of GFX8 (vid.h): a writer that builds them into a caller's buffer with every length checked, and a reader that walks a buffer packet by packet.
// Only type 3 packets (and type 2 / type 0 headers, which the reader refuses) are dealt with; the register ranges of SET_*_REG are those of vid.h.
#pragma once

#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace gcn {

namespace pm4 {
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnRegisters.inc"

constexpr uint32_t kShRegStart = 0x2C00, kShRegEnd = 0x3000, kContextRegStart = 0xA000, kContextRegEnd = 0xA400, kUconfigRegStart = 0xC000, kUconfigRegEnd = 0xC400;

constexpr uint32_t packet3_header(uint32_t opcode, uint32_t count) { return (3u << 30) | ((count & 0x3FFFu) << 16) | ((opcode & 0xFFu) << 8); }
}  // namespace pm4

class Pm4Writer {
 public:
  explicit Pm4Writer(ArrayView<uint32_t> out) : out_(out) {}
  bool ok() const { return ok_; }
  uint32_t count() const { return count_; }              // dwords written
  ArrayView<const uint32_t> words() const { return ArrayView<const uint32_t>(out_.data(), count_); }

  // SET_SH_REG / SET_CONTEXT_REG / SET_UCONFIG_REG: `values` go to consecutive registers from `reg` (the register number as in the headers, dwords). The range must lie in the
  // packet's register space.
  void set_sh_reg(uint32_t reg, ArrayView<const uint32_t> values);
  void set_context_reg(uint32_t reg, ArrayView<const uint32_t> values);
  void set_uconfig_reg(uint32_t reg, ArrayView<const uint32_t> values);
  void set_sh_reg(uint32_t reg, uint32_t value) { set_sh_reg(reg, ArrayView<const uint32_t>(&value, 1)); }
  void set_context_reg(uint32_t reg, uint32_t value) { set_context_reg(reg, ArrayView<const uint32_t>(&value, 1)); }
  void set_uconfig_reg(uint32_t reg, uint32_t value) { set_uconfig_reg(reg, ArrayView<const uint32_t>(&value, 1)); }

  void index_type(uint32_t vgt_index_type);                      // INDEX_TYPE: 0 16 bit, 1 32 bit
  void num_instances(uint32_t instances);
  void draw_index_auto(uint32_t vertex_count);                    // DRAW_INDEX_AUTO with the auto index source
  void draw_index_2(uint32_t max_size, uint64_t index_address, uint32_t index_count);       // DRAW_INDEX_2 reading the indices at the address (DMA source); max_size: the indices the buffer holds
  void event_write(uint32_t event_type, uint32_t event_index);   // EVENT_WRITE (partial flushes and the like)
  // EVENT_WRITE_EOP: when the event has gone through the pipe, the cache actions, and a 64 bit `value` (data_sel 2) or a 32 bit one (1) at `address`, and an interrupt (int_sel 2)
  void event_write_eop(uint32_t event_type, uint32_t cache_actions, uint64_t address, uint64_t value, uint32_t data_sel, uint32_t int_sel);
  void acquire_mem(uint32_t coher_cntl);                         // ACQUIRE_MEM over the whole address space: the cache actions of coher_cntl (kCoher*)
  void write_data(uint64_t address, ArrayView<const uint32_t> values);                      // WRITE_DATA to memory through the ME

 private:
  void put(uint32_t word);
  void set_reg(uint32_t opcode, uint32_t start, uint32_t end, uint32_t reg, ArrayView<const uint32_t> values);
  ArrayView<uint32_t> out_;
  uint32_t count_ = 0;
  bool ok_ = true;
};

struct Pm4Packet {
  uint32_t opcode = 0;
  ArrayView<const uint32_t> body;         // the dwords after the header
};

// Walks a buffer of packets. next() gives false at the end of the buffer or on a malformed packet (error() then tells which).
class Pm4Reader {
 public:
  explicit Pm4Reader(ArrayView<const uint32_t> words) : words_(words) {}
  bool next(Pm4Packet* packet);
  bool error() const { return error_; }
  bool at_end() const { return pos_ >= words_.size(); }

 private:
  ArrayView<const uint32_t> words_;
  uint64_t pos_ = 0;
  bool error_ = false;
};

}  // namespace gcn
