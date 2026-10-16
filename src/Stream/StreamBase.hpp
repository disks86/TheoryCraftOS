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
#include "Stream/IStream.hpp"

namespace stream {

// Defaults for the parts of a stream most streams do not have: no reading, writing,
// seeking or timeouts (each reports NotSupported, or Closed once closed). A
// stream overrides what it can do. `closed_` is the stream's own state: set it
// in close() (and call check() at the top of every operation). StreamBase is the
// base for plain IStreams; AsyncStreamBase (IAsyncStream.hpp) for streams that
// are also asynchronous.
template <class Interface>
class BasicStreamBase : public Interface {
 public:
  bool can_read() const override { return false; }
  bool can_write() const override { return false; }
  bool can_seek() const override { return false; }
  bool can_timeout() const override { return false; }
  bool is_closed() const override { return closed_; }

  StreamStatus read(ByteView, uint64_t* count) override {
    *count = 0;
    return unsupported();
  }
  StreamStatus write(ConstByteView) override { return unsupported(); }
  StreamStatus flush() override { return closed_ ? StreamStatus::Closed : StreamStatus::Ok; }
  StreamStatus close() override {
    closed_ = true;
    return StreamStatus::Ok;
  }
  StreamStatus length(uint64_t*) const override { return unsupported(); }
  StreamStatus position(uint64_t*) const override { return unsupported(); }
  StreamStatus seek(int64_t, SeekOrigin, uint64_t*) override { return unsupported(); }
  StreamStatus set_length(uint64_t) override { return unsupported(); }
  StreamStatus set_read_timeout(uint32_t) override { return unsupported(); }
  StreamStatus set_write_timeout(uint32_t) override { return unsupported(); }

 protected:
  constexpr BasicStreamBase() = default;
  ~BasicStreamBase() = default;

  StreamStatus unsupported() const { return closed_ ? StreamStatus::Closed : StreamStatus::NotSupported; }
  // Closed if closed, else Ok.
  StreamStatus check() const { return closed_ ? StreamStatus::Closed : StreamStatus::Ok; }

  // The position `offset` from `origin` given the current position and length, or
  // InvalidArgument if it would be before the start or overflow.
  static StreamStatus resolve_seek(int64_t offset, SeekOrigin origin, uint64_t position, uint64_t length, uint64_t* result) {
    const uint64_t base = origin == SeekOrigin::Begin ? 0 : origin == SeekOrigin::Current ? position : length;
    if (offset >= 0) {
      if (static_cast<uint64_t>(offset) > ~uint64_t{0} - base) return StreamStatus::InvalidArgument;
      *result = base + static_cast<uint64_t>(offset);
    } else {
      const uint64_t back = 0 - static_cast<uint64_t>(offset);
      if (back > base) return StreamStatus::InvalidArgument;
      *result = base - back;
    }
    return StreamStatus::Ok;
  }

  bool closed_ = false;
};

using StreamBase = BasicStreamBase<IStream>;

}  // namespace stream
