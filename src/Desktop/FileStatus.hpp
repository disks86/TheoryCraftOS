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

// Plain-language reasons for failed filesystem operations, for status lines.
#pragma once
#include "FileSystem/FileSystemTypes.hpp"

namespace desktop {

inline const char* describe_status(filesystem::Status status) {
  using filesystem::Status;
  switch (status) {
    case Status::NotFound: return "not found";
    case Status::ReadOnly: return "the volume is read-only";
    case Status::NoSpace: return "no space left";
    case Status::IsADirectory: return "that is a folder";
    case Status::NotADirectory: return "a folder in the path is a file";
    case Status::InvalidPath: return "invalid path";
    case Status::OutOfMemory: return "out of memory";
    case Status::IoError: return "storage error";
    case Status::AlreadyExists: return "that name is already used";
    case Status::Busy: return "it is in use";
    case Status::NotEmpty: return "the folder is not empty";
    case Status::Unsupported: return "not possible here";
    case Status::NoMedia: return "the drive is gone";
    case Status::AccessDenied: return "access denied";
    case Status::LoginRequired: return "a user name and password are needed";
    case Status::LoginFailed: return "the user name or password was refused";
    case Status::UntrustedHost: return "the server's identity has changed or cannot be trusted";
    case Status::Network: return "the network share could not be reached";
    default: return "failed";
  }
}

}  // namespace desktop
