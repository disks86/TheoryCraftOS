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

// The data types of the SSH protocols (RFC 4251 section 5): big endian numbers, strings with a four byte length (no padding), multiple precision
// integers and comma separated name lists. The reader goes into an error state at the first thing that does not fit (later reads give zeros).
#pragma once
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace network {

class SshWriter {
 public:
  explicit SshWriter(ByteBuffer* out) : out_(out) {}
  bool ok() const { return ok_; }

  void u8(uint8_t v) { ok_ = ok_ && out_->put8(v); }
  void boolean(bool v) { u8(v ? 1 : 0); }
  void u32(uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    raw(ConstByteView(b, 4));
  }
  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v >> 32));
    u32(static_cast<uint32_t>(v));
  }
  void raw(ConstByteView data) { ok_ = ok_ && out_->append(data); }
  void string(ConstByteView data) {
    u32(static_cast<uint32_t>(data.size()));
    raw(data);
  }
  void string(StringView text) { string(ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size())); }
  void string(const char* text) { string(StringView(text)); }
  // A non-negative integer given as big endian bytes: leading zeros dropped, a zero byte put in front if the top bit is set.
  void mpint(ConstByteView magnitude) {
    uint64_t skip = 0;
    while (skip < magnitude.size() && magnitude[skip] == 0) ++skip;
    const ConstByteView v = magnitude.subview(skip);
    const bool pad = !v.empty() && (v[0] & 0x80);
    u32(static_cast<uint32_t>(v.size() + (pad ? 1 : 0)));
    if (pad) u8(0);
    raw(v);
  }

 private:
  ByteBuffer* out_;
  bool ok_ = true;
};

class SshReader {
 public:
  explicit SshReader(ConstByteView data) : data_(data) {}
  bool ok() const { return ok_; }
  uint64_t remaining() const { return ok_ ? data_.size() - at_ : 0; }

  uint8_t u8() {
    if (!ok_ || at_ + 1 > data_.size()) {
      ok_ = false;
      return 0;
    }
    return data_[at_++];
  }
  bool boolean() { return u8() != 0; }
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
  ConstByteView raw(uint64_t count) {
    if (!ok_ || at_ + count > data_.size()) {
      ok_ = false;
      return ConstByteView();
    }
    const ConstByteView v = data_.subview(at_, count);
    at_ += count;
    return v;
  }
  // A string of at most `max` bytes: a view into the message.
  ConstByteView string(uint32_t max = 1u << 20) {
    const uint32_t length = u32();
    if (!ok_ || length > max) {
      ok_ = false;
      return ConstByteView();
    }
    return raw(length);
  }
  StringView text(uint32_t max = 1u << 16) {
    const ConstByteView v = string(max);
    return StringView(reinterpret_cast<const char*>(v.data()), v.size());
  }
  // An mpint's magnitude (leading zero bytes dropped). Negative numbers are an error.
  ConstByteView mpint(uint32_t max = 1024) {
    ConstByteView v = string(max);
    if (!v.empty() && (v[0] & 0x80)) ok_ = false;
    uint64_t skip = 0;
    while (skip < v.size() && v[skip] == 0) ++skip;
    return v.subview(skip);
  }
  void skip(uint64_t count) { raw(count); }

 private:
  ConstByteView data_;
  uint64_t at_ = 0;
  bool ok_ = true;
};

// Whether the comma separated `list` has `name` in it.
inline bool ssh_list_has(StringView list, StringView name) {
  uint64_t start = 0;
  while (start <= list.size()) {
    uint64_t end = start;
    while (end < list.size() && list[end] != ',') ++end;
    if (list.substr(start, end - start).equals(name)) return true;
    start = end + 1;
  }
  return false;
}

}  // namespace network
