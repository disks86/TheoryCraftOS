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

#include "Stream/IStream.hpp"

namespace stream {

const char* stream_status_name(StreamStatus status) {
  switch (status) {
    case StreamStatus::Ok: return "ok";
    case StreamStatus::NotSupported: return "not supported";
    case StreamStatus::Closed: return "closed";
    case StreamStatus::InvalidArgument: return "invalid argument";
    case StreamStatus::EndOfStream: return "end of stream";
    case StreamStatus::TimedOut: return "timed out";
    case StreamStatus::ConnectionLost: return "connection lost";
    case StreamStatus::HostNotFound: return "host not found";
    case StreamStatus::ReadOnly: return "read-only";
    case StreamStatus::NoSpace: return "no space";
    case StreamStatus::OutOfMemory: return "out of memory";
    case StreamStatus::IoError: return "I/O error";
  }
  return "unknown";
}

}  // namespace stream
