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
#include "Utility/FixedString.hpp"

namespace filesystem {

// Result of every filesystem operation (there are no exceptions). Ok means the
// operation completed; anything else leaves the filesystem as it was.
enum class Status : uint8_t {
  Ok,
  EndOfDirectory,   // IDirectory::next(): no more entries (not an error)
  NotFound,
  AlreadyExists,
  NotADirectory,
  IsADirectory,
  NotEmpty,         // removing a directory that still has entries
  Busy,             // removing or replacing something that is still open
  ReadOnly,
  NoSpace,
  InvalidPath,      // empty name, ".." component, name too long, ...
  InvalidArgument,
  IoError,          // the storage device failed
  Corrupt,          // the on-disk structures make no sense
  Unsupported,
  OutOfMemory,
  NoMedia,          // the medium is gone: a removable volume that was unplugged or ejected
  AccessDenied,     // permission refused (a network share that wants other rights)
  LoginRequired,    // a network server wants a user name and password and none was given
  LoginFailed,      // the user name or password was refused
  UntrustedHost,    // the server is not who it was: its host key or certificate changed since the first visit, or was refused
  Network,          // the network failed: the server could not be found or reached, or the connection broke
};

const char* status_name(Status status);

// The wall clock, for filesystems that stamp what they create and change: seconds since 1970-01-01 UTC, or 0 while no clock is known
// (the firmware's real-time clock can only be read from the boot core, so whoever owns it installs a function that answers from a value it
// keeps current). Install the source once, before the filesystems are used.
using UnixClock = uint64_t (*)(void* context);
void set_unix_clock(UnixClock clock, void* context);
uint64_t unix_time_now();
// How far the local time (what the taskbar shows, and what FAT stores) is ahead of UTC, in seconds; 0 when not known or the same.
void set_local_offset_seconds(int32_t seconds);
int32_t local_offset_seconds();

// Days since 1970-01-01 of a calendar date (month 1..12, day 1..31).
constexpr int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400, yoe = year - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
// The calendar date and time of day (UTC) of a number of seconds since 1970.
struct CivilTime {
  int32_t year = 1970;
  uint8_t month = 1, day = 1, hour = 0, minute = 0, second = 0;
};
constexpr CivilTime civil_from_unix(uint64_t seconds) {
  const int64_t days = static_cast<int64_t>(seconds / 86400), z = days + 719468;
  const int64_t era = z / 146097, doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  CivilTime t;
  t.day = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
  t.month = static_cast<uint8_t>(mp < 10 ? mp + 3 : mp - 9);
  t.year = static_cast<int32_t>(yoe + era * 400 + (t.month <= 2));
  const uint64_t rest = seconds % 86400;
  t.hour = static_cast<uint8_t>(rest / 3600);
  t.minute = static_cast<uint8_t>(rest % 3600 / 60);
  t.second = static_cast<uint8_t>(rest % 60);
  return t;
}

enum class NodeType : uint8_t { File, Directory };

// How IFileSystem::open_file() treats the path.
enum class OpenMode : uint8_t {
  Read,            // existing file, read only
  ReadWrite,       // existing file
  CreateOrOpen,    // read/write; created empty if missing
  CreateTruncate,  // read/write; created, or emptied if it exists
};

constexpr uint64_t kMaxNameLength = 255;

// One name in a directory (also what IFileSystem::stat() reports).
struct DirectoryEntry {
  FixedString<kMaxNameLength + 1> name;  // the last component only; empty for the root
  NodeType type = NodeType::File;
  uint64_t size = 0;                     // bytes; 0 for directories
  bool read_only = false;
  uint64_t modified = 0;                 // last change of the contents, seconds since 1970-01-01 UTC; 0 when the filesystem has none
  uint64_t created = 0;                  // the same for creation; 0 when unknown
};

struct FileSystemInfo {
  FixedString<16> type_name;  // "ramfs", "FAT32", ...
  FixedString<32> label;      // volume label; may be empty
  uint64_t total_bytes = 0;   // 0 = unbounded (a RAM filesystem without a limit)
  uint64_t free_bytes = 0;    // 0 when unknown (read-only filesystems do not count)
  bool read_only = false;
};

}  // namespace filesystem
