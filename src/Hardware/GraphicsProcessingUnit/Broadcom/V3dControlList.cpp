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

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dControlList.hpp"

namespace hardware {

V3dPacketBuilder& V3dPacketBuilder::set(const V3dField& field, uint64_t value) {
  if (!ok_) return *this;
  const uint64_t stored = field.minus_one ? value - 1 : value;
  if (field.minus_one && value == 0) { ok_ = false; return *this; }
  if (field.size < 64 && (stored >> field.size) != 0) { ok_ = false; return *this; }
  for (uint32_t bit = 0; bit < field.size; ++bit) {
    uint8_t* byte = bytes_.try_at((field.start + bit) / 8);
    if (!byte) { ok_ = false; return *this; }
    const uint8_t mask = static_cast<uint8_t>(1u << ((field.start + bit) % 8));
    if ((stored >> bit) & 1) *byte = static_cast<uint8_t>(*byte | mask);
    else *byte = static_cast<uint8_t>(*byte & ~mask);
  }
  return *this;
}

V3dPacketBuilder& V3dPacketBuilder::set_address(const V3dField& field, uint32_t address) {
  if (field.size > 32 || field.word_bit + field.size > 32) { ok_ = false; return *this; }
  const uint32_t shift = field.word_bit;
  const uint32_t kept = field.size == 32 ? address : (address >> shift) & ((1u << field.size) - 1);
  return set(field, kept);
}

V3dPacketBuilder V3dControlList::emit(const V3dPacketInfo& packet) {
  ArrayView<uint8_t> bytes = append(packet.length);
  if (!ok_ || bytes.size() != packet.length) return V3dPacketBuilder(ArrayView<uint8_t>(), false);
  bytes[0] = packet.opcode;
  return V3dPacketBuilder(bytes, true);
}

ArrayView<uint8_t> V3dControlList::append(uint32_t bytes) {
  if (!ok_ || bytes > storage_.size() - used_) {
    ok_ = false;
    return ArrayView<uint8_t>();
  }
  ArrayView<uint8_t> out = storage_.subview(used_, bytes);
  for (uint8_t& b : out) b = 0;
  used_ += bytes;
  return out;
}

void V3dControlList::align(uint32_t alignment) {
  const uint32_t rest = (gpu_address_ + used_) & (alignment - 1);
  if (rest) append(alignment - rest);
}

}  // namespace hardware
