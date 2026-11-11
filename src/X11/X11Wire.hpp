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

// The X11 wire format in the byte order a client chose at connection setup ('B': most significant byte first, 'l': least).
//
// WireReader reads a request (bounds-checked: a request shorter than its fields reads zeros and !ok()); WireWriter appends a reply to a client's output
// buffer. Events and errors are 32-byte blocks built in one canonical (little-endian) form and turned into the client's order as they are sent
// (Packet, which knows where its 16- and 32-bit fields are), because one event often goes to several clients of both kinds.
#pragma once
#include <stdint.h>
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"

namespace x11 {

class WireReader {
 public:
  WireReader(ConstByteView data, bool big) : in_(data), big_(big) {}
  bool ok() const { return in_.ok(); }
  uint64_t remaining() const { return in_.remaining(); }
  uint64_t position() const { return in_.position(); }
  bool big() const { return big_; }
  uint8_t u8() { return in_.u8(); }
  int8_t i8() { return static_cast<int8_t>(in_.u8()); }
  uint16_t u16() { return big_ ? in_.be16() : in_.le16(); }
  uint32_t u32() { return big_ ? in_.be32() : in_.le32(); }
  int16_t i16() { return static_cast<int16_t>(u16()); }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  ConstByteView take(uint64_t count) { return in_.take(count); }
  bool skip(uint64_t count) { return in_.skip(count); }
  ConstByteView rest() const { return in_.rest(); }

 private:
  ByteReader in_;
  bool big_;
};

// Appends to `out` in the client's order. A failed append (no memory) is remembered: ok() is then false and the caller drops the client.
class WireWriter {
 public:
  WireWriter(ByteBuffer& out, bool big) : out_(out), big_(big), start_(out.size()) {}
  bool ok() const { return ok_; }
  uint64_t written() const { return out_.size() - start_; }
  void u8(uint8_t v) { ok_ &= out_.put8(v); }
  void u16(uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(big_ ? v >> 8 : v), static_cast<uint8_t>(big_ ? v : v >> 8)};
    ok_ &= out_.append(ConstByteView(b, 2));
  }
  void u32(uint32_t v) {
    uint8_t b[4];
    for (uint32_t i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(v >> (big_ ? 24 - 8 * i : 8 * i));
    ok_ &= out_.append(ConstByteView(b, 4));
  }
  void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
  void bytes(ConstByteView v) { ok_ &= out_.append(v); }
  void zeros(uint64_t count) { ok_ &= out_.put_zeros(count); }
  // Pads what was written since this writer began to a multiple of four.
  void pad4() { zeros((4 - written() % 4) % 4); }
  // Rewrites a 32-bit field written earlier (at `offset` bytes from where this writer began).
  void patch_u32(uint64_t offset, uint32_t v) {
    if (offset > written() || written() - offset < 4) { ok_ = false; return; }
    ByteView at = out_.writable_view().subview(start_ + offset, 4);
    for (uint32_t i = 0; i < 4; ++i) at[i] = static_cast<uint8_t>(v >> (big_ ? 24 - 8 * i : 8 * i));
  }
  void patch_u16(uint64_t offset, uint16_t v) {
    if (offset > written() || written() - offset < 2) { ok_ = false; return; }
    ByteView at = out_.writable_view().subview(start_ + offset, 2);
    at[0] = static_cast<uint8_t>(big_ ? v >> 8 : v);
    at[1] = static_cast<uint8_t>(big_ ? v : v >> 8);
  }

 private:
  ByteBuffer& out_;
  bool big_;
  uint64_t start_;
  bool ok_ = true;
};

// One 32-byte event or error, held little-endian with a note of which offsets start a 16- or 32-bit field (so it can be written in either order).
struct Packet {
  uint8_t bytes[32] = {};
  uint32_t wide16 = 0, wide32 = 0;      // bit n: a 16- / 32-bit field starts at byte n

  void u8(uint32_t at, uint8_t v) { if (at < 32) bytes[at] = v; }
  void u16(uint32_t at, uint16_t v) {
    if (at > 30) return;
    bytes[at] = static_cast<uint8_t>(v);
    bytes[at + 1] = static_cast<uint8_t>(v >> 8);
    wide16 |= 1u << at;
  }
  void u32(uint32_t at, uint32_t v) {
    if (at > 28) return;
    for (uint32_t i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(v >> (8 * i));
    wide32 |= 1u << at;
  }
  void i16(uint32_t at, int32_t v) { u16(at, static_cast<uint16_t>(v)); }
  uint16_t get16(uint32_t at) const { return at > 30 ? 0 : static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8); }
  uint32_t get32(uint32_t at) const {
    if (at > 28) return 0;
    return static_cast<uint32_t>(bytes[at]) | static_cast<uint32_t>(bytes[at + 1]) << 8 | static_cast<uint32_t>(bytes[at + 2]) << 16 |
           static_cast<uint32_t>(bytes[at + 3]) << 24;
  }
  // The block in a client's order.
  void write(ByteBuffer& out, bool big, bool* ok) const {
    uint8_t b[32];
    for (uint32_t i = 0; i < 32; ++i) b[i] = bytes[i];
    if (big) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (wide32 & (1u << i)) {
          const uint8_t t0 = b[i], t1 = b[i + 1];
          b[i] = b[i + 3]; b[i + 1] = b[i + 2]; b[i + 2] = t1; b[i + 3] = t0;
        } else if (wide16 & (1u << i)) {
          const uint8_t t = b[i];
          b[i] = b[i + 1]; b[i + 1] = t;
        }
      }
    }
    *ok &= out.append(ConstByteView(b, 32));
  }
};

}  // namespace x11
