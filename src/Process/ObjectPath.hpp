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
#include "Utility/StringView.hpp"

namespace process::objpath {

// The names of the object namespace of the Windows ABI ("\BaseNamedObjects\Dir\Name", "\??\C:", ...) and how a name is walked: directories, symbolic links that are followed
// (as the parse of the object manager of Windows does), the directories that are always there, and the syntax errors an NtCreate/NtOpen call reports.
//
// The object manager of this OS (Objects.hpp) keeps its named objects in one table by a *flat name*: the path below \BaseNamedObjects without the folder ("Name", "Dir\Name"),
// "\x01" and the path below \Sessions\1\BaseNamedObjects for the session directory, and the whole path with its leading backslash for every other place ("\??\x", "\Device\y").
// Walking a path ends in the flat name of the object it names, so that every NtCreate* and NtOpen* finds the same object whichever way (a link, a relative name below a
// directory handle) the program named it.

constexpr uint32_t kMaxPath = 256;       // the bytes of a flat name or of an absolute path (UTF-8)
constexpr uint32_t kMaxParts = 32;       // the folders of a path
constexpr char kSessionMark = 1;         // the first character of the flat name of a name in \Sessions\1\BaseNamedObjects

// NTSTATUS values a walk reports.
constexpr uint32_t kStatusTypeMismatch = 0xC0000024u, kStatusNameInvalid = 0xC0000033u, kStatusNameNotFound = 0xC0000034u, kStatusNameCollision = 0xC0000035u,
                   kStatusPathNotFound = 0xC000003Au, kStatusPathSyntaxBad = 0xC000003Bu, kStatusNameTooLong = 0xC0000106u;

enum class EntryKind : uint8_t { None, Directory, Link, Object };

// What is at a name.
struct Entry {
  EntryKind kind = EntryKind::None;
  bool builtin = false;                // a directory or link that is always there (no object behind it)
  uint8_t object_kind = 0;             // Object: the ObjectKind of the object manager
  uint32_t text_size = 0;
  char text[kMaxPath] = {};            // Link: the target; Directory: its absolute path
  uint32_t flat_size = 0;
  char flat[kMaxPath] = {};            // the flat name of the object found (its own spelling, if it was found in any case)
};

// Finds the object with a flat name (the object manager). Returns false if there is none.
class ILookup {
 public:
  virtual bool find(StringView flat, bool case_insensitive, Entry* out) = 0;

 protected:
  ILookup() = default;
  ~ILookup() = default;
};

// An absolute path as its folders.
class Path {
 public:
  uint32_t count() const { return count_; }
  StringView part(uint32_t index) const { return index < count_ ? StringView(text_ + begin_[index], length_[index]) : StringView(); }
  bool push(StringView part);                    // false if the path would be too long
  bool replace_last(StringView part);
  // "\a\b" -> a, b. "\" -> none. False for an empty folder ("\\", a trailing backslash), a path that does not start with a backslash, or one that is too long.
  bool parse(StringView text);
  // "\a\b" (just "\" with no folder).
  bool format(char* out, uint32_t capacity, uint32_t* size) const;
  // The flat name (see above).
  bool to_flat(char* out, uint32_t capacity, uint32_t* size) const;
  // \BaseNamedObjects\Session\<n>\x is \Sessions\<n>\BaseNamedObjects\x
  void apply_alias();

 private:
  char text_[kMaxPath] = {};
  uint16_t begin_[kMaxParts] = {};
  uint16_t length_[kMaxParts] = {};
  uint32_t count_ = 0, used_ = 0;
};

// The path a flat name stands for. False if it does not fit.
bool flat_to_path(StringView flat, Path* out);
// The directory a flat name is in ("" is \BaseNamedObjects, "\x01" the session's, "\" the root) and the name in it.
void split_flat(StringView flat, StringView* directory, StringView* leaf);

struct ResolveRequest {
  bool has_root = false;               // a RootDirectory handle was given: the name is below it
  StringView root_path;                // the absolute path of that directory
  bool name_given = false;             // the OBJECT_ATTRIBUTES had a name (it may be empty)
  StringView name;
  bool case_insensitive = false;       // OBJ_CASE_INSENSITIVE: the folders that are always there are found in any case
  bool open_link = false;              // OBJ_OPENLINK: a link at the end of the path is the answer, not followed
};

struct Resolved {
  uint32_t status = 0;                 // 0, or the NTSTATUS the call fails with
  Entry entry;                         // what is there (kind None: the name is free, and the directory it would be in exists)
  bool is_root = false;                // the name was empty: the root directory itself
  uint32_t path_size = 0, flat_size = 0;
  char path[kMaxPath] = {};            // the absolute path of the name, links followed
  char flat[kMaxPath] = {};
};

void resolve(ILookup& lookup, const ResolveRequest& request, Resolved* out);

}  // namespace process::objpath
