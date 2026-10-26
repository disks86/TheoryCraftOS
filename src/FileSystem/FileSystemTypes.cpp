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

#include "FileSystem/FileSystemTypes.hpp"

namespace filesystem {

namespace {
UnixClock g_clock = nullptr;
void* g_clock_context = nullptr;
int32_t g_local_offset = 0;
}  // namespace

void set_unix_clock(UnixClock clock, void* context) {
  __atomic_store_n(&g_clock_context, context, __ATOMIC_RELEASE);
  __atomic_store_n(&g_clock, clock, __ATOMIC_RELEASE);
}

void set_local_offset_seconds(int32_t seconds) { __atomic_store_n(&g_local_offset, seconds, __ATOMIC_RELAXED); }
int32_t local_offset_seconds() { return __atomic_load_n(&g_local_offset, __ATOMIC_RELAXED); }

uint64_t unix_time_now() {
  UnixClock clock = __atomic_load_n(&g_clock, __ATOMIC_ACQUIRE);
  return clock ? clock(__atomic_load_n(&g_clock_context, __ATOMIC_ACQUIRE)) : 0;
}

const char* status_name(Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::EndOfDirectory: return "end of directory";
    case Status::NotFound: return "not found";
    case Status::AlreadyExists: return "already exists";
    case Status::NotADirectory: return "not a directory";
    case Status::IsADirectory: return "is a directory";
    case Status::NotEmpty: return "directory not empty";
    case Status::Busy: return "busy";
    case Status::ReadOnly: return "read-only";
    case Status::NoSpace: return "no space";
    case Status::InvalidPath: return "invalid path";
    case Status::InvalidArgument: return "invalid argument";
    case Status::IoError: return "I/O error";
    case Status::Corrupt: return "corrupt filesystem";
    case Status::Unsupported: return "unsupported";
    case Status::OutOfMemory: return "out of memory";
    case Status::NoMedia: return "no media";
    case Status::AccessDenied: return "access denied";
    case Status::LoginRequired: return "login required";
    case Status::LoginFailed: return "login failed";
    case Status::UntrustedHost: return "untrusted host";
    case Status::Network: return "network error";
    case Status::TooManyLinks: return "too many levels of symbolic links";
  }
  return "unknown";
}

}  // namespace filesystem
