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
#include "Utility/ArrayView.hpp"

namespace hardware {

// Where a field sits in a packet or record: its first bit (counted from the packet's first byte, so past the opcode byte), its width, whether the value is stored
// minus one, and (for an address field, which is stored in place inside a 32-bit word that starts at the first byte after the opcode) the bit of that word it starts at. The layouts themselves are generated (V3dPackets.inc, scripts/gen_v3d_packets.py) from Mesa's description of the hardware.
struct V3dField {
  uint16_t start;
  uint8_t size;
  bool minus_one;
  uint8_t word_bit;
};
struct V3dPacketInfo {
  uint8_t opcode;
  uint8_t length;  // bytes, opcode included
};

#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dPackets.inc"

// Sets fields of one packet or record in place, with every bit and byte checked. A value too wide for its field is a bug in the caller and marks the builder bad (the
// control list then refuses to be used) instead of spilling into the next field.
class V3dPacketBuilder {
 public:
  V3dPacketBuilder(ArrayView<uint8_t> bytes, bool ok) : bytes_(bytes), ok_(ok) {}
  // A float field takes the float's 32 bits (the code that builds lists uses no floating point at all, so it can run in the AArch64 image).
  V3dPacketBuilder& set(const V3dField& field, uint64_t value);
  // An address field: the bits of the address that lie inside the field are stored where they are (a 27-bit field at bit 5 keeps address bits 5..31).
  V3dPacketBuilder& set_address(const V3dField& field, uint32_t address);
  bool ok() const { return ok_; }

 private:
  ArrayView<uint8_t> bytes_;
  bool ok_;
};

// Appends packets to a control list in memory the GPU reads (a byte range the caller owns). When it runs out of room or a packet is malformed the list is marked
// bad and stays bad; check ok() before submitting. The GPU address of the first byte is kept so jumps and sub-list references can be made.
class V3dControlList {
 public:
  V3dControlList(ArrayView<uint8_t> storage, uint32_t gpu_address) : storage_(storage), gpu_address_(gpu_address) {}
  // Starts a packet: its bytes are cleared and the opcode written. Set its fields on the result.
  V3dPacketBuilder emit(const V3dPacketInfo& packet);
  // Starts a packet with no fields to set.
  void emit_simple(const V3dPacketInfo& packet) { emit(packet); }
  // Appends raw bytes (a record such as the shader state record, or alignment padding).
  ArrayView<uint8_t> append(uint32_t bytes);
  // Pads with zero bytes to a multiple of `alignment` (a power of two).
  void align(uint32_t alignment);

  bool ok() const { return ok_; }
  uint32_t size() const { return used_; }
  uint32_t gpu_address() const { return gpu_address_; }                 // of byte 0
  uint32_t gpu_end() const { return gpu_address_ + used_; }             // of the next byte
  ArrayView<uint8_t> bytes() const { return storage_.first(used_); }
  void mark_bad() { ok_ = false; }

 private:
  ArrayView<uint8_t> storage_;
  uint32_t gpu_address_;
  uint32_t used_ = 0;
  bool ok_ = true;
};

}  // namespace hardware
