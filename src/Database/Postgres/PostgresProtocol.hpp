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

// The pieces of PostgreSQL's frontend/backend protocol (version 3.0): building the messages a client sends and taking apart the ones the server
// sends. Every message but the first is one type byte, a four byte length (counting itself, not the type byte) and the body, numbers big endian.
#pragma once
#include "Database/DatabaseTypes.hpp"
#include "Utility/ByteBuffer.hpp"

namespace database::postgres {

constexpr uint32_t kProtocolVersion = 196608;   // 3.0
constexpr uint32_t kSslRequestCode = 80877103;  // 1234 << 16 | 5679

// Builds messages in a ByteBuffer: begin() with the type byte (0 for the startup message, which has none), the fields, then end() which fills in the
// length. Any failure (memory) is remembered: ok() says whether all of it fitted.
class MessageWriter {
 public:
  explicit MessageWriter(ByteBuffer& out) : out_(out) {}
  bool ok() const { return ok_; }

  void begin(char type) {
    start_ = out_.size();
    has_type_ = type != 0;
    if (has_type_) put8(static_cast<uint8_t>(type));
    length_at_ = out_.size();
    put32(0);
  }
  void end() {
    const uint32_t length = static_cast<uint32_t>(out_.size() - length_at_);
    uint8_t b[4] = {static_cast<uint8_t>(length >> 24), static_cast<uint8_t>(length >> 16), static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
    for (int i = 0; i < 4; ++i) ok_ = out_.patch8(length_at_ + static_cast<uint64_t>(i), b[i]) && ok_;
  }
  void put8(uint8_t v) { ok_ = out_.put8(v) && ok_; }
  void put16(uint32_t v) {
    put8(static_cast<uint8_t>(v >> 8));
    put8(static_cast<uint8_t>(v));
  }
  void put32(uint32_t v) {
    put16(v >> 16);
    put16(v & 0xFFFF);
  }
  void put_bytes(ConstByteView v) { ok_ = out_.append(v) && ok_; }
  // Text followed by a zero byte.
  void put_cstring(StringView text) {
    put_bytes(text.bytes());
    put8(0);
  }

 private:
  ByteBuffer& out_;
  uint64_t start_ = 0, length_at_ = 0;
  bool has_type_ = false;
  bool ok_ = true;
};

// Reads the fields of one message body. A read past the end makes ok() false and gives zeros.
class MessageReader {
 public:
  explicit MessageReader(ConstByteView body) : data_(body) {}
  bool ok() const { return ok_; }
  bool at_end() const { return pos_ >= data_.size(); }
  uint64_t remaining() const { return data_.size() - pos_; }

  uint8_t get8() { return static_cast<uint8_t>(read(1)); }
  uint32_t get16() { return static_cast<uint32_t>(read(2)); }
  uint32_t get32() { return static_cast<uint32_t>(read(4)); }
  int32_t get_i32() { return static_cast<int32_t>(get32()); }
  // The next `count` bytes (a view into the body).
  ConstByteView get_bytes(uint64_t count) {
    if (count > remaining()) {
      ok_ = false;
      pos_ = data_.size();
      return ConstByteView();
    }
    const ConstByteView v = data_.subview(pos_, count);
    pos_ += count;
    return v;
  }
  // Text up to a zero byte (which is consumed).
  StringView get_cstring() {
    uint64_t end = pos_;
    while (end < data_.size() && data_[end] != 0) ++end;
    if (end >= data_.size()) {
      ok_ = false;
      pos_ = data_.size();
      return StringView();
    }
    const StringView v(reinterpret_cast<const char*>(data_.data()) + pos_, end - pos_);
    pos_ = end + 1;
    return v;
  }
  ConstByteView rest() const { return data_.subview(pos_); }

 private:
  uint64_t read(uint32_t bytes) {
    if (remaining() < bytes) {
      ok_ = false;
      pos_ = data_.size();
      return 0;
    }
    uint64_t v = 0;
    for (uint32_t i = 0; i < bytes; ++i) v = (v << 8) | data_[pos_ + i];
    pos_ += bytes;
    return v;
  }
  ConstByteView data_;
  uint64_t pos_ = 0;
  bool ok_ = true;
};

// PostgreSQL's type numbers (OIDs) for the types a program tells apart.
DbType db_type_for_oid(uint32_t oid);

}  // namespace database::postgres
