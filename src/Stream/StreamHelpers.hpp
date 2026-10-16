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
#include "Utility/StringView.hpp"

namespace stream {

// Conveniences built on IStream's read and write (C#'s ReadByte, CopyTo, ...).

// Reads exactly buffer.size() bytes, looping over short reads. EndOfStream if the
// stream ended first (the bytes that did arrive are in the buffer).
inline StreamStatus read_exactly(IStream& in, ByteView buffer) {
  uint64_t done = 0;
  while (done < buffer.size()) {
    uint64_t got = 0;
    const StreamStatus s = in.read(buffer.subview(done), &got);
    if (s != StreamStatus::Ok) return s;
    if (got == 0) return StreamStatus::EndOfStream;
    done += got;
  }
  return StreamStatus::Ok;
}

// One byte; EndOfStream at the end.
inline StreamStatus read_byte(IStream& in, uint8_t* value) { return read_exactly(in, ByteView(value, 1)); }
inline StreamStatus write_byte(IStream& out, uint8_t value) { return out.write(ConstByteView(&value, 1)); }
// The bytes of `text` (no terminator).
inline StreamStatus write_text(IStream& out, StringView text) {
  return out.write(ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

// Copies from the current position of `from` to its end into `to`. *copied (may be
// null) is how many bytes were written to `to`.
inline StreamStatus copy_to(IStream& from, IStream& to, uint64_t* copied = nullptr) {
  uint8_t buffer[512];
  uint64_t total = 0;
  StreamStatus s = StreamStatus::Ok;
  for (;;) {
    uint64_t got = 0;
    s = from.read(ByteView(buffer), &got);
    if (s != StreamStatus::Ok || got == 0) break;
    s = to.write(ConstByteView(buffer).first(got));
    if (s != StreamStatus::Ok) break;
    total += got;
  }
  if (copied) *copied = total;
  return s;
}

}  // namespace stream
