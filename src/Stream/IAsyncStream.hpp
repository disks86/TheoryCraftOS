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
#include "Async/CancellationToken.hpp"
#include "Async/Task.hpp"
#include "Stream/StreamBase.hpp"

namespace stream {

// The outcome of an asynchronous read: how many bytes, and the status.
struct IoResult {
  StreamStatus status = StreamStatus::Ok;
  uint64_t count = 0;  // read: bytes read (0 with Ok = end of stream)
};

}  // namespace stream

namespace async {
// A task that could not allocate its coroutine reports it as out of memory.
template <>
struct FailedResult<stream::StreamStatus> {
  static stream::StreamStatus value() { return stream::StreamStatus::OutOfMemory; }
};
template <>
struct FailedResult<stream::IoResult> {
  static stream::IoResult value() { return {stream::StreamStatus::OutOfMemory, 0}; }
};
}  // namespace async

namespace stream {

// An IStream that can also be used with co_await, like C#'s Stream (Read/ReadAsync).
// The synchronous calls still work and block; the async ones suspend only the calling
// task, so one executor can serve many streams at once.
//
//   async::Task<void> echo(IAsyncStream& s) {
//     uint8_t buffer[256];
//     for (;;) {
//       IoResult r = co_await s.read_async(ByteView(buffer));
//       if (r.status != StreamStatus::Ok || r.count == 0) co_return;
//       if (co_await s.write_async(ConstByteView(buffer).first(r.count)) != StreamStatus::Ok) co_return;
//     }
//   }
//
// The buffer (and `data`) must stay valid until the awaited task completes. The
// token cancels a pending operation: it then returns StreamStatus::Cancelled
// (data already transferred stays transferred). Streams that cannot wait (memory,
// files) complete at once. Like IStream, use one stream from one task at a time.
class IAsyncStream : public IStream {
 public:
  async::Task<IoResult> read_async(ByteView buffer, async::CancellationToken token = {}) { return on_read_async(buffer, token); }
  async::Task<StreamStatus> write_async(ConstByteView data, async::CancellationToken token = {}) { return on_write_async(data, token); }
  async::Task<StreamStatus> flush_async(async::CancellationToken token = {}) { return on_flush_async(token); }

 protected:
  constexpr IAsyncStream() = default;
  ~IAsyncStream() = default;

  // What implementations override (the public calls above just add the default token).
  virtual async::Task<IoResult> on_read_async(ByteView buffer, async::CancellationToken token) = 0;
  virtual async::Task<StreamStatus> on_write_async(ConstByteView data, async::CancellationToken token) = 0;
  virtual async::Task<StreamStatus> on_flush_async(async::CancellationToken token) = 0;
};

// StreamBase for async streams. The async operations default to running the
// synchronous one and returning its result already completed - right for memory
// and files, whose operations never wait. A stream that does wait (TcpStream)
// overrides them.
class AsyncStreamBase : public BasicStreamBase<IAsyncStream> {
 protected:
  async::Task<IoResult> on_read_async(ByteView buffer, async::CancellationToken token) override {
    IoResult result;
    if (token.cancelled()) result.status = StreamStatus::Cancelled;
    else result.status = read(buffer, &result.count);
    return async::Task<IoResult>::from_value(result);
  }
  async::Task<StreamStatus> on_write_async(ConstByteView data, async::CancellationToken token) override {
    return async::Task<StreamStatus>::from_value(token.cancelled() ? StreamStatus::Cancelled : write(data));
  }
  async::Task<StreamStatus> on_flush_async(async::CancellationToken token) override {
    return async::Task<StreamStatus>::from_value(token.cancelled() ? StreamStatus::Cancelled : flush());
  }

  constexpr AsyncStreamBase() = default;
  ~AsyncStreamBase() = default;
};

}  // namespace stream
