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

// Plain-language reasons for failed filesystem operations: describe_status() for status lines, and describe_failure() for the messages of the
// file explorer and its dialogs, which say what happened, why, and what can be done about it (docs/UserInterfaceGuidelines.md, 11.8).
#pragma once
#include "Desktop/Format.hpp"
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

// Why it failed, as the words that follow "because" in a sentence: "... could not be copied because the drive is full."
inline const char* status_reason(filesystem::Status status) {
  using filesystem::Status;
  switch (status) {
    case Status::NotFound: return "it could not be found";
    case Status::ReadOnly: return "the drive cannot be written to";
    case Status::NoSpace: return "the drive is full";
    case Status::IsADirectory: return "a folder has that name";
    case Status::NotADirectory: return "part of its location is a file, not a folder";
    case Status::InvalidPath: return "the name cannot be used here";
    case Status::OutOfMemory: return "there is not enough memory";
    case Status::IoError: return "the drive reported a problem";
    case Status::AlreadyExists: return "that name is already used";
    case Status::Busy: return "it is in use";
    case Status::NotEmpty: return "the folder is not empty";
    case Status::Unsupported: return "this place does not allow it";
    case Status::NoMedia: return "the drive was removed";
    case Status::AccessDenied: return "you are not allowed to change it";
    case Status::LoginRequired: return "a user name and password are needed";
    case Status::LoginFailed: return "the user name or password was not accepted";
    case Status::UntrustedHost: return "the server's identity could not be confirmed";
    case Status::Network: return "the server could not be reached";
    default: return "of an unexpected problem";
  }
}

// What the user can do about it: a whole sentence, or nothing where there is no useful advice.
inline const char* status_advice(filesystem::Status status) {
  using filesystem::Status;
  switch (status) {
    case Status::NotFound: return "It may have been moved, renamed or deleted.";
    case Status::ReadOnly: return "Choose a place on another drive.";
    case Status::NoSpace: return "Free some space, or choose another drive, and try again.";
    case Status::InvalidPath: return "Choose another name.";
    case Status::AlreadyExists: return "Choose another name.";
    case Status::OutOfMemory: return "Close something you do not need and try again.";
    case Status::IoError: return "Try again.";
    case Status::Busy: return "Close it where it is open and try again.";
    case Status::NoMedia: return "Put the drive back and try again.";
    case Status::LoginRequired:
    case Status::LoginFailed: return "Connect to the server again.";
    case Status::UntrustedHost:
    case Status::Network: return "Check the network and try again.";
    default: return "";
  }
}

// The whole message for something that failed. `pattern` is the sentence that says what happened and why, with %1 for the name of the object
// and %2 for the reason: "\"%1\" could not be renamed because %2." The advice, where there is some, follows as a sentence of its own.
// Replaces the contents of `out`; false if memory ran out.
inline bool describe_failure(String* out, StringView pattern, StringView name, filesystem::Status status) {
  if (!format_message(out, pattern, name, status_reason(status))) return false;
  const StringView advice(status_advice(status));
  return advice.empty() || (out->push_back(' ') && out->append(advice));
}

}  // namespace desktop
