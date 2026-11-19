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
#include "Allocators/IAllocator.hpp"
#include "Crypto/Crypto.hpp"
#include "Desktop/AppMan/AppManInstaller.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

// What the App Manager's installers (AppManInstaller.hpp for Linux, WingetInstaller.hpp for Windows) do with files: paths, folders, small text files,
// taking a folder away again, and a download that goes into a file as it comes.
namespace desktop::appman {

// *out = a + b + c + d. False if memory ran out.
[[nodiscard]] bool join(String* out, StringView a, StringView b = {}, StringView c = {}, StringView d = {});
// Makes the folder at `path` and every folder above it that is missing.
[[nodiscard]] bool make_folders(filesystem::IFileSystem& files, StringView path);
// The first `limit` bytes of the file at `path`, at most. False if it cannot be read.
[[nodiscard]] bool read_text_file(filesystem::IFileSystem& files, allocators::IAllocator& allocator, StringView path, uint64_t limit, String* text);
// Makes (or replaces) the file at `path` with `text` in it.
[[nodiscard]] bool write_text_file(filesystem::IFileSystem& files, StringView path, StringView text);
// Deletes the file or the folder at `path`, with everything in it. A link is deleted, not followed. True if nothing is there afterwards.
[[nodiscard]] bool remove_tree(filesystem::IFileSystem& files, allocators::IAllocator& allocator, StringView path, uint32_t depth = 0);

// Downloads `url` into the file at `path` (made or replaced), reporting to `progress` (which may be null) and stopping when it says cancel. `hasher`
// (which may be null) is fed every byte. On anything but Ok the file is deleted again and *detail says what went wrong:
// Cancelled, DiskFull (it did not fit), FileError (the file could not be written otherwise) or DownloadFailed (the network, or an empty answer).
[[nodiscard]] Result download_to_file(filesystem::IFileSystem& files, INetwork& network, StringView url, StringView path, Progress* progress, crypto::Hasher* hasher,
                                      String* detail);

}  // namespace desktop::appman
