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
#include "Allocators/IAllocator.hpp"
#include "Stream/IAsyncStream.hpp"

namespace stream {

// A stream over memory, like C#'s MemoryStream. Readable, writable and seekable.
//
//   MemoryStream growable(allocator);            // owns its buffer, grows as written
//   MemoryStream fixed(ByteView(storage), 0, true);   // over caller memory: never grows
//   MemoryStream view(ConstByteView(data));      // read-only over existing data
//
// Writing past the end of a growable stream extends it (OutOfMemory if the
// allocator cannot); past a fixed buffer's capacity it fails with NoSpace and
// writes nothing. Seeking beyond the end is allowed; a write there zero-fills the
// gap. contents() is the bytes written so far (valid until the next change).
class MemoryStream final : public AsyncStreamBase {
 public:
  // Growable, owning; `initial_capacity` bytes are reserved up front.
  explicit MemoryStream(allocators::IAllocator& allocator, uint64_t initial_capacity = 0);
  // Over `buffer` (not owned, never grown), of which the first `length` bytes are in use.
  MemoryStream(ByteView buffer, uint64_t length, bool writable);
  // Read-only over `data`.
  explicit MemoryStream(ConstByteView data);
  ~MemoryStream();
  MemoryStream(const MemoryStream&) = delete;
  MemoryStream& operator=(const MemoryStream&) = delete;

  bool can_read() const override { return !closed_; }
  bool can_write() const override { return !closed_ && writable_; }
  bool can_seek() const override { return !closed_; }

  StreamStatus read(ByteView buffer, uint64_t* count) override;
  StreamStatus write(ConstByteView data) override;
  StreamStatus close() override;
  StreamStatus length(uint64_t* length) const override;
  StreamStatus position(uint64_t* position) const override;
  StreamStatus seek(int64_t offset, SeekOrigin origin, uint64_t* new_position) override;
  StreamStatus set_length(uint64_t length) override;

  // The bytes in the stream (all of them, whatever the position).
  ConstByteView contents() const { return ConstByteView(data_, length_); }
  uint64_t capacity() const { return capacity_; }
  // Makes room for `capacity` bytes without further allocation. NoSpace for a fixed stream.
  StreamStatus reserve(uint64_t capacity);
  // Empties it and goes back to position 0.
  void reset();

 private:
  // Makes the length at least `new_length`, zero-filling what is new.
  StreamStatus extend_to(uint64_t new_length);

  allocators::IAllocator* allocator_;  // null: the buffer is not ours
  uint8_t* data_ = nullptr;
  uint64_t length_ = 0;
  uint64_t capacity_ = 0;
  uint64_t position_ = 0;
  bool writable_;
};

}  // namespace stream
