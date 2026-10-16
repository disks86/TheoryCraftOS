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

namespace stream {

// What a stream operation returns (there are no exceptions): the cases C#'s Stream
// reports by throwing.
enum class StreamStatus : uint8_t {
  Ok,
  NotSupported,    // seeking a socket, writing a read-only stream, ...
  Closed,          // the stream was closed
  InvalidArgument, // negative or overflowing position/length
  EndOfStream,     // only from helpers that need a full amount (read_exactly)
  TimedOut,
  ConnectionLost,  // network: reset, refused, or closed while data was still owed
  HostNotFound,    // network: the name did not resolve
  ReadOnly,
  NoSpace,         // a fixed buffer, a size limit or the medium is full
  OutOfMemory,
  IoError,
  Cancelled,       // an async operation was cancelled through its CancellationToken
};

const char* stream_status_name(StreamStatus status);

enum class SeekOrigin : uint8_t { Begin, Current, End };

constexpr uint32_t kInfiniteTimeout = 0xFFFFFFFFu;

// A sequence of bytes you read and write in order, like C#'s System.IO.Stream:
// memory, a file, a network connection - all used the same way, and what a
// particular stream cannot do (seeking a socket) is reported by can_seek() and
// NotSupported rather than missing. Implementations: Memory/MemoryStream,
// File/FileStream, Tcp/TcpStream; helpers (read_exactly, copy_to, ...) are in
// StreamHelpers.hpp.
//
//   UniquePointer<IStream> in;
//   if (FileStream::open(allocator, fs, "/etc/motd", OpenMode::Read, &in) == filesystem::Status::Ok) {
//     uint8_t buffer[64]; uint64_t got;
//     while (in->read(ByteView(buffer), &got) == StreamStatus::Ok && got) use(ConstByteView(buffer).first(got));
//   }
//
// Streams are not thread safe: use one from one thread at a time. Destroying a
// stream (its UniquePointer) closes it, like C#'s Dispose.
class IStream {
 public:
  virtual bool can_read() const = 0;
  virtual bool can_write() const = 0;
  virtual bool can_seek() const = 0;     // length(), position(), seek() and set_length() work
  virtual bool can_timeout() const = 0;  // set_read_timeout() and set_write_timeout() work
  virtual bool is_closed() const = 0;

  // Reads at least one byte (blocking until there is one) and at most
  // buffer.size(), and stores how many in *count. Ok with *count == 0 means the
  // end of the stream (an empty buffer also gives 0).
  virtual StreamStatus read(ByteView buffer, uint64_t* count) = 0;
  // Writes all of `data` or fails; a failure may have written part of it.
  virtual StreamStatus write(ConstByteView data) = 0;
  // Pushes buffered data to its destination (the medium, or the network's peer).
  virtual StreamStatus flush() = 0;
  // Releases the stream; later calls return Closed. Closing twice is fine.
  virtual StreamStatus close() = 0;

  virtual StreamStatus length(uint64_t* length) const = 0;
  virtual StreamStatus position(uint64_t* position) const = 0;
  // Moves the position (it may go past the end; writing there zero-fills the gap)
  // and stores the new one in *new_position (may be null).
  virtual StreamStatus seek(int64_t offset, SeekOrigin origin, uint64_t* new_position) = 0;
  // Cuts or extends (with zeros) to `length`; a position beyond it stays where it is.
  virtual StreamStatus set_length(uint64_t length) = 0;

  // How long a read / write may wait for progress, in milliseconds
  // (kInfiniteTimeout waits forever). Expiring returns TimedOut.
  virtual StreamStatus set_read_timeout(uint32_t milliseconds) = 0;
  virtual StreamStatus set_write_timeout(uint32_t milliseconds) = 0;

 protected:
  constexpr IStream() = default;
  ~IStream() = default;
};

}  // namespace stream
