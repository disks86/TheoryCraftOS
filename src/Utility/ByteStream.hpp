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

// Bounds-checked reading and writing of binary data, byte order included. This
// is how packets and descriptors are parsed and built: every read and write is
// checked against the view, a failed one sets a sticky error flag (ok() turns
// false, later reads return zeros) and nothing outside the view is ever touched.
//
//   ByteReader in(packet);
//   const uint8_t version = in.u8();
//   const uint16_t length = in.be16();
//   ConstByteView payload = in.take(length);
//   if (!in.ok()) return;            // truncated or lying length field: drop it

class ByteReader {
 public:
  constexpr ByteReader() = default;
  explicit constexpr ByteReader(ConstByteView data) : data_(data) {}

  bool ok() const { return !failed_; }
  uint64_t position() const { return position_; }
  uint64_t remaining() const { return data_.size() - position_; }
  bool at_end() const { return position_ == data_.size(); }
  // The part not read yet (does not advance).
  ConstByteView rest() const { return data_.subview(position_); }
  ConstByteView whole() const { return data_; }

  uint8_t u8() { return static_cast<uint8_t>(read(1)); }
  uint16_t be16() { return static_cast<uint16_t>(read(2)); }
  uint32_t be32() { return static_cast<uint32_t>(read(4)); }
  uint16_t le16() { return static_cast<uint16_t>(read_le(2)); }
  uint32_t le32() { return static_cast<uint32_t>(read_le(4)); }
  uint64_t le64() { return read_le(8); }

  // The next `count` bytes as a view into the same memory (advances). On
  // failure returns an empty view and sets the error.
  ConstByteView take(uint64_t count) {
    if (failed_ || count > remaining()) {
      failed_ = true;
      return ConstByteView();
    }
    ConstByteView result = data_.subview(position_, count);
    position_ += count;
    return result;
  }
  // Copies exactly out.size() bytes into `out`.
  bool read_into(ByteView out) {
    ConstByteView bytes = take(out.size());
    if (failed_) return false;
    return out.copy_from(bytes);
  }
  bool skip(uint64_t count) {
    take(count);
    return !failed_;
  }
  // Moves to an absolute position inside the data.
  bool seek(uint64_t position) {
    if (position > data_.size()) {
      failed_ = true;
      return false;
    }
    position_ = position;
    return !failed_;
  }
  // Looks at the byte `offset` bytes ahead without consuming; sets no error. False if beyond the end.
  bool peek_u8(uint64_t offset, uint8_t* out) const { return data_.try_get(position_ + offset, out); }

 private:
  uint64_t read(uint32_t bytes) {
    if (failed_ || bytes > remaining()) {
      failed_ = true;
      return 0;
    }
    uint64_t value = 0;
    for (uint32_t i = 0; i < bytes; ++i) value = (value << 8) | data_[position_ + i];
    position_ += bytes;
    return value;
  }
  uint64_t read_le(uint32_t bytes) {
    if (failed_ || bytes > remaining()) {
      failed_ = true;
      return 0;
    }
    uint64_t value = 0;
    for (uint32_t i = 0; i < bytes; ++i) value |= uint64_t{data_[position_ + i]} << (8 * i);
    position_ += bytes;
    return value;
  }

  ConstByteView data_;
  uint64_t position_ = 0;
  bool failed_ = false;
};

class ByteWriter {
 public:
  constexpr ByteWriter() = default;
  explicit constexpr ByteWriter(ByteView out) : out_(out) {}

  bool ok() const { return !failed_; }
  uint64_t position() const { return position_; }
  uint64_t space() const { return out_.size() - position_; }
  // What has been written so far (a view into the output).
  ByteView written() const { return out_.first(position_); }

  void u8(uint8_t v) { put(v, 1); }
  void be16(uint16_t v) { put(v, 2); }
  void be32(uint32_t v) { put(v, 4); }
  void le16(uint16_t v) { put_le(v, 2); }
  void le32(uint32_t v) { put_le(v, 4); }
  void le64(uint64_t v) { put_le(v, 8); }
  void bytes(ConstByteView source) {
    if (failed_ || source.size() > space()) {
      failed_ = true;
      return;
    }
    out_.subview(position_, source.size()).copy_from(source);
    position_ += source.size();
  }
  void zeros(uint64_t count) {
    if (failed_ || count > space()) {
      failed_ = true;
      return;
    }
    out_.subview(position_, count).fill(0);
    position_ += count;
  }
  // Overwrites bytes already written (a length or checksum filled in last).
  void patch_bytes(uint64_t offset, ConstByteView source) {
    if (failed_ || offset > position_ || position_ - offset < source.size()) {
      failed_ = true;
      return;
    }
    out_.subview(offset, source.size()).copy_from(source);
  }
  void patch_be16(uint64_t offset, uint16_t v) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    patch_bytes(offset, ConstByteView(bytes));
  }
  // Reserves `count` bytes to fill in directly; empty view and error on overflow.
  ByteView reserve(uint64_t count) {
    if (failed_ || count > space()) {
      failed_ = true;
      return ByteView();
    }
    ByteView result = out_.subview(position_, count);
    position_ += count;
    return result;
  }

 private:
  void put(uint64_t v, uint32_t bytes) {
    if (failed_ || bytes > space()) {
      failed_ = true;
      return;
    }
    for (uint32_t i = 0; i < bytes; ++i) out_[position_ + i] = static_cast<uint8_t>(v >> (8 * (bytes - 1 - i)));
    position_ += bytes;
  }
  void put_le(uint64_t v, uint32_t bytes) {
    if (failed_ || bytes > space()) {
      failed_ = true;
      return;
    }
    for (uint32_t i = 0; i < bytes; ++i) out_[position_ + i] = static_cast<uint8_t>(v >> (8 * i));
    position_ += bytes;
  }

  ByteView out_;
  uint64_t position_ = 0;
  bool failed_ = false;
};
