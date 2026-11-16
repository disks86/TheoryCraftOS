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
#include "Process/LinuxBpf.hpp"

#include "Utility/ByteStream.hpp"

namespace process::linuxabi::bpf {

namespace {

// code: the class in the low three bits; for loads a size (0x18) and a mode (0xE0); for arithmetic and jumps an operation (0xF0) and a source (0x08)
constexpr uint16_t kLd = 0, kLdx = 1, kSt = 2, kStx = 3, kAlu = 4, kJmp = 5, kRet = 6, kMisc = 7;
constexpr uint16_t kWord = 0x00, kHalf = 0x08, kByte = 0x10;
constexpr uint16_t kImm = 0x00, kAbs = 0x20, kInd = 0x40, kMem = 0x60, kLen = 0x80, kMsh = 0xA0;
constexpr uint16_t kAdd = 0x00, kSub = 0x10, kMul = 0x20, kDiv = 0x30, kOr = 0x40, kAnd = 0x50, kLsh = 0x60, kRsh = 0x70, kNeg = 0x80, kMod = 0x90, kXor = 0xA0;
constexpr uint16_t kJa = 0x00, kJeq = 0x10, kJgt = 0x20, kJge = 0x30, kJset = 0x40;
constexpr uint16_t kSourceX = 0x08;
constexpr uint32_t kMemoryWords = 16;
// Offsets below zero name things beside the packet: the link header, the network header, and facts about the packet ("ancillary data").
constexpr uint32_t kLinkBase = 0xFFE00000u, kNetworkBase = 0xFFF00000u, kAncillaryBase = 0xFFFFF000u;

struct Instruction {
  uint16_t code = 0;
  uint8_t jt = 0, jf = 0;
  uint32_t k = 0;
};

Instruction at(ConstByteView program, uint32_t index) {
  ByteReader in{program.subview(uint64_t{index} * kInstructionSize, kInstructionSize)};
  Instruction i;
  i.code = in.le16();
  i.jt = in.u8();
  i.jf = in.u8();
  i.k = in.le32();
  return i;
}

// `size` bytes at `offset` of `from`, big endian (as packets are). False if they are not all there.
bool load_from(ConstByteView from, uint64_t offset, uint32_t size, uint32_t* out) {
  ConstByteView bytes;
  if (offset > from.size() || size > from.size() - offset) return false;
  bytes = from.subview(offset, size);
  uint32_t v = 0;
  for (const uint8_t b : bytes) v = v << 8 | b;
  *out = v;
  return true;
}

bool load(const Packet& p, uint32_t k, uint32_t size, uint32_t* out) {
  if (k < kLinkBase) return load_from(p.data, k, size, out);
  if (k >= kAncillaryBase) {      // SKF_AD_*: only the ones a packet has here; anything else reads as nothing
    switch (k - kAncillaryBase) {
      case 0: *out = p.protocol; return true;             // SKF_AD_PROTOCOL
      case 4: *out = p.packet_type; return true;          // SKF_AD_PKTTYPE
      case 8: *out = p.interface_index; return true;      // SKF_AD_IFINDEX
      default: *out = 0; return true;
    }
  }
  if (k >= kNetworkBase) return load_from(p.data, uint64_t{p.network_offset} + (k - kNetworkBase), size, out);
  return p.link.empty() ? load_from(p.data, k - kLinkBase, size, out) : load_from(p.link, k - kLinkBase, size, out);
}

}  // namespace

bool valid(ConstByteView program) {
  if (program.empty() || program.size() % kInstructionSize != 0 || program.size() / kInstructionSize > kMostInstructions) return false;
  const uint32_t count = uint32_t(program.size() / kInstructionSize);
  for (uint32_t pc = 0; pc < count; ++pc) {
    const Instruction i = at(program, pc);
    const uint16_t kind = i.code & 7, rest = i.code & ~uint16_t{7};
    switch (kind) {
      case kLd: {
        const uint16_t size = rest & 0x18, mode = rest & 0xE0;
        if ((rest & ~uint16_t{0xF8}) != 0 || size == 0x18) return false;
        if (mode == kAbs || mode == kInd) break;
        if (size != kWord || (mode != kImm && mode != kMem && mode != kLen)) return false;
        if (mode == kMem && i.k >= kMemoryWords) return false;
        break;
      }
      case kLdx:
        if (rest == (kByte | kMsh)) break;
        if (rest != kImm && rest != kMem && rest != kLen) return false;
        if (rest == kMem && i.k >= kMemoryWords) return false;
        break;
      case kSt: case kStx:
        if (rest != 0 || i.k >= kMemoryWords) return false;
        break;
      case kAlu: {
        const uint16_t operation = rest & 0xF0;
        const bool by_x = (rest & kSourceX) != 0;
        if ((rest & ~uint16_t{0xF8}) != 0 || operation > kXor) return false;
        if (operation == kNeg) break;
        if (!by_x && (operation == kDiv || operation == kMod) && i.k == 0) return false;
        if (!by_x && (operation == kLsh || operation == kRsh) && i.k >= 32) return false;
        break;
      }
      case kJmp: {
        const uint16_t operation = rest & 0xF0;
        if ((rest & ~uint16_t{0xF8}) != 0 || operation > kJset) return false;
        const uint32_t left = count - pc - 1;      // instructions after this one: a jump has to land on one of them
        if (operation == kJa) {
          if ((rest & kSourceX) != 0 || i.k >= left) return false;
        } else if (i.jt >= left || i.jf >= left) {
          return false;
        }
        break;
      }
      case kRet:
        if (rest != 0x00 && rest != 0x08 && rest != 0x10) return false;      // a constant, X, or the accumulator
        break;
      case kMisc:
        if (rest != 0x00 && rest != 0x80) return false;
        break;
      default: return false;
    }
  }
  return (at(program, count - 1).code & 7) == kRet;
}

uint32_t run(ConstByteView program, const Packet& packet) {
  if (!valid(program)) return 0;
  const uint32_t count = uint32_t(program.size() / kInstructionSize);
  uint32_t a = 0, x = 0, memory[kMemoryWords] = {};
  for (uint32_t pc = 0; pc < count; ++pc) {      // (jumps only go forward: this ends)
    const Instruction i = at(program, pc);
    const uint16_t kind = i.code & 7, rest = i.code & ~uint16_t{7};
    switch (kind) {
      case kLd: {
        const uint16_t mode = rest & 0xE0;
        const uint32_t size = (rest & 0x18) == kWord ? 4 : (rest & 0x18) == kHalf ? 2 : 1;
        if (mode == kImm) a = i.k;
        else if (mode == kMem) a = memory[i.k];
        else if (mode == kLen) a = packet.wire_length;
        else if (!load(packet, mode == kInd ? x + i.k : i.k, size, &a)) return 0;
        break;
      }
      case kLdx:
        if (rest == kImm) x = i.k;
        else if (rest == kMem) x = memory[i.k];
        else if (rest == kLen) x = packet.wire_length;
        else {      // four times the low half of a byte: the length of an IP header
          uint32_t byte = 0;
          if (!load(packet, i.k, 1, &byte)) return 0;
          x = (byte & 0x0F) * 4;
        }
        break;
      case kSt: memory[i.k] = a; break;
      case kStx: memory[i.k] = x; break;
      case kAlu: {
        const uint32_t by = (rest & kSourceX) ? x : i.k;
        switch (rest & 0xF0) {
          case kAdd: a += by; break;
          case kSub: a -= by; break;
          case kMul: a *= by; break;
          case kDiv: if (by == 0) return 0; a /= by; break;
          case kMod: if (by == 0) return 0; a %= by; break;
          case kOr: a |= by; break;
          case kAnd: a &= by; break;
          case kXor: a ^= by; break;
          case kLsh: a = by >= 32 ? 0 : a << by; break;
          case kRsh: a = by >= 32 ? 0 : a >> by; break;
          case kNeg: a = 0u - a; break;
          default: return 0;
        }
        break;
      }
      case kJmp: {
        const uint32_t with = (rest & kSourceX) ? x : i.k;
        bool taken = false;
        switch (rest & 0xF0) {
          case kJa: pc += i.k; continue;
          case kJeq: taken = a == with; break;
          case kJgt: taken = a > with; break;
          case kJge: taken = a >= with; break;
          case kJset: taken = (a & with) != 0; break;
          default: return 0;
        }
        pc += taken ? i.jt : i.jf;
        break;
      }
      case kRet: return rest == 0x10 ? a : rest == 0x08 ? x : i.k;
      case kMisc:
        if (rest == 0) x = a;
        else a = x;
        break;
      default: return 0;
    }
  }
  return 0;
}

}  // namespace process::linuxabi::bpf
