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

// Where the media decoders read a file from, a piece at a time: anything that can say how long it is and hand out the bytes at an
// offset - a file on a volume, a piece of memory, a part of another source. Nothing here needs the whole file to be in memory.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ArrayView.hpp"

namespace media {

class IByteSource {
 public:
  virtual uint64_t size() const = 0;
  // Copies up to out.size() bytes starting at `offset` into `out` and returns how many: fewer only at the end of the data (or 0 when the
  // read failed, e.g. the medium is gone). Not necessarily thread safe: one reader at a time.
  virtual uint64_t read(uint64_t offset, ByteView out) = 0;

 protected:
  constexpr IByteSource() = default;
  ~IByteSource() = default;
};

// Fills all of `out`; false if the source ended or failed first.
inline bool read_exact(IByteSource& source, uint64_t offset, ByteView out) { return source.read(offset, out) == out.size(); }

// A source over memory that is already there (borrowed: it must outlive the source).
class MemoryByteSource final : public IByteSource {
 public:
  MemoryByteSource() = default;
  explicit MemoryByteSource(ConstByteView data) : data_(data) {}
  void reset(ConstByteView data) { data_ = data; }
  ConstByteView data() const { return data_; }
  uint64_t size() const override { return data_.size(); }
  uint64_t read(uint64_t offset, ByteView out) override {
    if (offset >= data_.size()) return 0;
    uint64_t n = data_.size() - offset;
    if (n > out.size()) n = out.size();
    if (n) out.first(n).copy_from(data_.subview(offset, n));
    return n;
  }

 private:
  ConstByteView data_;
};

// A part of another source, as if it were a source of its own.
class RangeByteSource final : public IByteSource {
 public:
  RangeByteSource(IByteSource& inner, uint64_t offset, uint64_t length) : inner_(inner), offset_(offset), length_(length) {}
  uint64_t size() const override { return length_; }
  uint64_t read(uint64_t offset, ByteView out) override {
    if (offset >= length_) return 0;
    if (out.size() > length_ - offset) out = out.first(length_ - offset);
    return inner_.read(offset_ + offset, out);
  }

 private:
  IByteSource& inner_;
  uint64_t offset_, length_;
};

// Remembers the last window that was read from the source it wraps, so that many small reads close together (headers, chunk tables,
// frame after frame of a sound) cost one big read. Reads bigger than the window go straight through.
class CachedByteSource final : public IByteSource {
 public:
  CachedByteSource(IByteSource& inner, allocators::IAllocator& allocator, uint32_t window = 32768) : inner_(inner), allocator_(allocator), window_size_(window) {}
  ~CachedByteSource() { release_view(allocator_, window_); }
  CachedByteSource(const CachedByteSource&) = delete;
  CachedByteSource& operator=(const CachedByteSource&) = delete;

  uint64_t size() const override { return inner_.size(); }
  uint64_t read(uint64_t offset, ByteView out) override {
    uint64_t done = 0;
    while (done < out.size()) {
      const uint64_t at = offset + done;
      if (at >= inner_.size()) break;
      if (!(at >= first_ && at < first_ + valid_)) {
        if (out.size() - done >= window_size_) {   // big: straight into the caller's buffer
          const uint64_t got = inner_.read(at, out.subview(done, out.size() - done));
          return done + got;
        }
        if (window_.empty()) window_ = allocate_view<uint8_t>(allocator_, window_size_);
        if (window_.empty()) return done + inner_.read(at, out.subview(done, out.size() - done));
        valid_ = inner_.read(at, window_);
        first_ = at;
        if (valid_ == 0) break;
      }
      uint64_t n = first_ + valid_ - at;
      if (n > out.size() - done) n = out.size() - done;
      out.subview(done, n).copy_from(ConstByteView(window_).subview(at - first_, n));
      done += n;
    }
    return done;
  }

 private:
  IByteSource& inner_;
  allocators::IAllocator& allocator_;
  const uint32_t window_size_;
  ByteView window_;
  uint64_t first_ = 0, valid_ = 0;
};

// A window onto a source for decoders that parse frame after frame: from(at) returns the bytes from `at` on (at least `need` of them
// unless the source ends sooner), reading a new window only when `at` gets close to the end of the one it has.
class SourceWindow {
 public:
  SourceWindow(IByteSource& source, allocators::IAllocator& allocator, uint32_t size = 262144, uint32_t need = 131072)
      : source_(source), allocator_(allocator), size_(size), need_(need) {}
  ~SourceWindow() { release_view(allocator_, buffer_); }
  SourceWindow(const SourceWindow&) = delete;
  SourceWindow& operator=(const SourceWindow&) = delete;

  ConstByteView from(uint64_t at) {
    if (at >= source_.size()) return ConstByteView();
    if (buffer_.empty()) {
      buffer_ = allocate_view<uint8_t>(allocator_, size_);
      if (buffer_.empty()) return ConstByteView();
    }
    if (!(at >= pos_ && at < pos_ + len_ && (at + need_ <= pos_ + len_ || pos_ + len_ >= source_.size()))) {
      pos_ = at;
      len_ = source_.read(at, buffer_);
    }
    if (at < pos_ || at >= pos_ + len_) return ConstByteView();
    return ConstByteView(buffer_).subview(at - pos_, pos_ + len_ - at);
  }

 private:
  IByteSource& source_;
  allocators::IAllocator& allocator_;
  const uint32_t size_, need_;
  ByteView buffer_;
  uint64_t pos_ = 0, len_ = 0;
};

}  // namespace media
