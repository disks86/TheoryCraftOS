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
#include "Stream/IAsyncStream.hpp"
#include "Utility/StringView.hpp"

namespace stream {

// The async counterparts of StreamHelpers.hpp. They are coroutines: the streams and
// buffers you pass must stay alive until the returned task completes.

// Reads exactly buffer.size() bytes, looping over short reads. EndOfStream if the
// stream ended first.
inline async::Task<StreamStatus> read_exactly_async(IAsyncStream& in, ByteView buffer, async::CancellationToken token = {}) {
  uint64_t done = 0;
  while (done < buffer.size()) {
    const IoResult r = co_await in.read_async(buffer.subview(done), token);
    if (r.status != StreamStatus::Ok) co_return r.status;
    if (r.count == 0) co_return StreamStatus::EndOfStream;
    done += r.count;
  }
  co_return StreamStatus::Ok;
}

inline async::Task<StreamStatus> write_text_async(IAsyncStream& out, StringView text, async::CancellationToken token = {}) {
  co_return co_await out.write_async(ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()), token);
}

// Copies from the current position of `from` to its end into `to`; *copied (may be
// null) is how many bytes were written.
inline async::Task<StreamStatus> copy_to_async(IAsyncStream& from, IAsyncStream& to, uint64_t* copied = nullptr,
                                               async::CancellationToken token = {}) {
  uint8_t buffer[512];
  uint64_t total = 0;
  StreamStatus status = StreamStatus::Ok;
  for (;;) {
    const IoResult r = co_await from.read_async(ByteView(buffer), token);
    if (r.status != StreamStatus::Ok) {
      status = r.status;
      break;
    }
    if (r.count == 0) break;
    status = co_await to.write_async(ConstByteView(buffer).first(r.count), token);
    if (status != StreamStatus::Ok) break;
    total += r.count;
  }
  if (copied) *copied = total;
  co_return status;
}

}  // namespace stream
