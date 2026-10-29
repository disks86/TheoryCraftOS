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

#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnPm4.hpp"

namespace gcn {

using namespace pm4;

void Pm4Writer::put(uint32_t word) {
  if (count_ >= out_.size()) { ok_ = false; return; }
  out_[count_++] = word;
}

void Pm4Writer::set_reg(uint32_t opcode, uint32_t start, uint32_t end, uint32_t reg, ArrayView<const uint32_t> values) {
  if (values.size() == 0 || reg < start || reg >= end || values.size() > end - reg || values.size() > 0x3FFE) { ok_ = false; return; }
  put(packet3_header(opcode, static_cast<uint32_t>(values.size())));
  put(reg - start);
  for (const uint32_t v : values) put(v);
}

void Pm4Writer::set_sh_reg(uint32_t reg, ArrayView<const uint32_t> values) { set_reg(kPacket3SetShReg, kShRegStart, kShRegEnd, reg, values); }
void Pm4Writer::set_context_reg(uint32_t reg, ArrayView<const uint32_t> values) { set_reg(kPacket3SetContextReg, kContextRegStart, kContextRegEnd, reg, values); }
void Pm4Writer::set_uconfig_reg(uint32_t reg, ArrayView<const uint32_t> values) { set_reg(kPacket3SetUconfigReg, kUconfigRegStart, kUconfigRegEnd, reg, values); }

void Pm4Writer::index_type(uint32_t vgt_index_type) {
  put(packet3_header(kPacket3IndexType, 0));
  put(vgt_index_type);
}

void Pm4Writer::num_instances(uint32_t instances) {
  put(packet3_header(kPacket3NumInstances, 0));
  put(instances);
}

void Pm4Writer::draw_index_auto(uint32_t vertex_count) {
  put(packet3_header(kPacket3DrawIndexAuto, 1));
  put(vertex_count);
  put(kDI_SRC_SEL_AUTO_INDEX);
}

void Pm4Writer::draw_index_2(uint32_t max_size, uint64_t index_address, uint32_t index_count) {
  if ((index_address & 1) != 0 || (index_address >> 48) != 0) { ok_ = false; return; }
  put(packet3_header(kPacket3DrawIndex2, 4));
  put(max_size);
  put(static_cast<uint32_t>(index_address));
  put(static_cast<uint32_t>(index_address >> 32));
  put(index_count);
  put(kDI_SRC_SEL_DMA);
}

void Pm4Writer::event_write(uint32_t event_type, uint32_t event_index) {
  put(packet3_header(kPacket3EventWrite, 0));
  put((event_type & 0x3F) | ((event_index & 0xF) << 8));
}

void Pm4Writer::event_write_eop(uint32_t event_type, uint32_t cache_actions, uint64_t address, uint64_t value, uint32_t data_sel, uint32_t int_sel) {
  if ((address & 7) != 0 || (address >> 40) != 0 || data_sel > 7 || int_sel > 3) { ok_ = false; return; }
  put(packet3_header(kPacket3EventWriteEop, 4));
  put(cache_actions | (event_type & 0x3F) | (5u << 8));                    // EVENT_INDEX 5: end of pipe
  put(static_cast<uint32_t>(address));
  put(static_cast<uint32_t>(address >> 32) | (data_sel << 29) | (int_sel << 24));
  put(static_cast<uint32_t>(value));
  put(static_cast<uint32_t>(value >> 32));
}

void Pm4Writer::acquire_mem(uint32_t coher_cntl) {
  put(packet3_header(kPacket3AcquireMem, 5));
  put(coher_cntl);
  put(0xFFFFFFFFu);        // CP_COHER_SIZE: everything
  put(0xFFu);              // CP_COHER_SIZE_HI
  put(0);                  // CP_COHER_BASE
  put(0);                  // CP_COHER_BASE_HI
  put(0x0000000Au);        // poll interval
}

void Pm4Writer::write_data(uint64_t address, ArrayView<const uint32_t> values) {
  if ((address & 3) != 0 || (address >> 48) != 0 || values.size() == 0 || values.size() > 0x3FF0) { ok_ = false; return; }
  put(packet3_header(kPacket3WriteData, static_cast<uint32_t>(values.size()) + 2));
  put((5u << 8) | (1u << 20));                                           // DST_SEL 5 (memory, direct), WR_CONFIRM
  put(static_cast<uint32_t>(address));
  put(static_cast<uint32_t>(address >> 32));
  for (const uint32_t v : values) put(v);
}

bool Pm4Reader::next(Pm4Packet* packet) {
  if (error_ || pos_ >= words_.size()) return false;
  const uint32_t header = words_[pos_];
  if ((header >> 30) != 3) { error_ = true; return false; }
  // the ring filler (pad_ib): a NOP of count 0x3FFF is one dword
  if (((header >> 8) & 0xFF) == kPacket3Nop && ((header >> 16) & 0x3FFF) == 0x3FFF) {
    packet->opcode = kPacket3Nop;
    packet->body = ArrayView<const uint32_t>();
    ++pos_;
    return true;
  }
  const uint64_t body = static_cast<uint64_t>((header >> 16) & 0x3FFF) + 1;
  if (body > words_.size() - pos_ - 1) { error_ = true; return false; }
  packet->opcode = (header >> 8) & 0xFF;
  packet->body = words_.subview(pos_ + 1, body);
  pos_ += 1 + body;
  return true;
}

}  // namespace gcn
