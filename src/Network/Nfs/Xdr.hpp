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

// XDR (RFC 4506), the encoding of ONC RPC and NFS: big endian numbers, everything padded to four bytes. The writer appends to a ByteBuffer; the
// reader walks a byte view and, on the first thing that does not fit, goes into an error state (every later read gives zeros) so that a parser
// can read a whole message and check ok() once.
#pragma once
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace network {

class XdrWriter {
 public:
  explicit XdrWriter(ByteBuffer* out) : out_(out) {}
  bool ok() const { return ok_; }

  void u32(uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    ok_ = ok_ && out_->append(ConstByteView(b, 4));
  }
  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v >> 32));
    u32(static_cast<uint32_t>(v));
  }
  void boolean(bool v) { u32(v ? 1 : 0); }
  // Variable length opaque data / a string: the length, the bytes, padding.
  void opaque(ConstByteView data) {
    u32(static_cast<uint32_t>(data.size()));
    fixed(data);
  }
  void string(StringView s) { opaque(ConstByteView(reinterpret_cast<const uint8_t*>(s.data()), s.size())); }
  // Fixed length opaque data: the bytes and padding, no length.
  void fixed(ConstByteView data) {
    ok_ = ok_ && out_->append(data);
    if (data.size() % 4) ok_ = ok_ && out_->put_zeros(4 - data.size() % 4);
  }

 private:
  ByteBuffer* out_;
  bool ok_ = true;
};

class XdrReader {
 public:
  explicit XdrReader(ConstByteView data) : data_(data) {}
  bool ok() const { return ok_; }
  uint64_t remaining() const { return ok_ ? data_.size() - at_ : 0; }
  uint64_t position() const { return at_; }

  uint32_t u32() {
    if (!ok_ || at_ + 4 > data_.size()) {
      ok_ = false;
      return 0;
    }
    const uint8_t* p = data_.data() + at_;
    at_ += 4;
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
  }
  uint64_t u64() {
    const uint64_t high = u32();
    return (high << 32) | u32();
  }
  bool boolean() { return u32() != 0; }
  // Variable length opaque data (at most `max` bytes): a view into the message.
  ConstByteView opaque(uint32_t max = 0xFFFFFF) {
    const uint32_t length = u32();
    if (!ok_ || length > max) {
      ok_ = false;
      return ConstByteView();
    }
    return fixed(length);
  }
  ConstByteView fixed(uint32_t length) {
    const uint64_t padded = (uint64_t{length} + 3) & ~uint64_t{3};
    if (!ok_ || at_ + padded > data_.size()) {
      ok_ = false;
      return ConstByteView();
    }
    const ConstByteView v = data_.subview(at_, length);
    at_ += padded;
    return v;
  }
  StringView string(uint32_t max = 4096) {
    const ConstByteView v = opaque(max);
    return StringView(reinterpret_cast<const char*>(v.data()), v.size());
  }
  void skip(uint64_t bytes) {
    if (!ok_ || at_ + bytes > data_.size()) {
      ok_ = false;
      return;
    }
    at_ += bytes;
  }

 private:
  ConstByteView data_;
  uint64_t at_ = 0;
  bool ok_ = true;
};

}  // namespace network
