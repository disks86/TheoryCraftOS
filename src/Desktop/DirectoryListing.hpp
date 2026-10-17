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

// Reading and describing folders, shared by the file explorer and the file dialogs.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace desktop {

// Reads the folder at `path` into `entries` (replacing what it held): folders first, then files, each group by name without
// regard to case. The status of opening the folder, or Ok.
filesystem::Status read_sorted_directory(filesystem::IFileSystem& fs, StringView path, List<filesystem::DirectoryEntry>* entries);

// "Folder", "TXT File" for "notes.txt", or "File" without an extension. False if memory ran out.
bool describe_entry_type(const filesystem::DirectoryEntry& entry, String* out);

// Whether the file name ends in one of the extensions in `extensions`: names without the dot, separated by ';' ("png;jpg"), compared
// without regard to case. An empty list or "*" matches every name.
bool name_has_extension(StringView name, StringView extensions);

// The first extension of the list ("png" for "png;jpg"), or empty.
StringView first_extension(StringView extensions);

}  // namespace desktop
