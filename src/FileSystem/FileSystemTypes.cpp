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
  }
  return "unknown";
}

}  // namespace filesystem
