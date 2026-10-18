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

// Copying and removing files and folders, with progress. A FileTransfer first measures what is to be copied (measure()), then copy() walks the
// tree and reports each file it starts and the bytes it has moved so far through the callbacks; the cancel flag, read before every chunk, stops it
// early (Status::Busy is returned for a cancelled copy). It works on any IFileSystem - a folder on a disk, a mounted network share - and may run
// on any thread; the callbacks run on that thread.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "Utility/String.hpp"

namespace desktop {

class FileTransfer {
 public:
  static constexpr filesystem::Status kCancelled = filesystem::Status::Busy;

  // Called when copying starts on a file: its path, how many files are done and how many there are in all.
  using FileFunction = void (*)(void* context, StringView path, uint64_t files_done, uint64_t files_total);
  // Called after each chunk: bytes copied so far of the total.
  using BytesFunction = void (*)(void* context, uint64_t bytes_done, uint64_t bytes_total);

  FileTransfer(allocators::IAllocator& allocator, filesystem::IFileSystem& fs) : allocator_(allocator), fs_(fs) {}

  void set_callbacks(FileFunction on_file, BytesFunction on_bytes, void* context) {
    on_file_ = on_file;
    on_bytes_ = on_bytes;
    context_ = context;
  }
  // Read (atomically) before every chunk and every file; when it is true the work stops with kCancelled.
  void set_cancel_flag(const bool* flag) { cancel_ = flag; }

  uint64_t files_total() const { return files_total_; }
  uint64_t bytes_total() const { return bytes_total_; }

  // Counts the files and bytes under `path` (a file or a folder) into the totals. Call before copy().
  filesystem::Status measure(StringView path);
  // Copies `from` (a file or a folder with everything in it) to `to`, which must not exist.
  filesystem::Status copy(StringView from, StringView to);
  // Removes a file, or a folder with everything in it.
  static filesystem::Status remove_tree(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView path, uint32_t depth = 0);

 private:
  filesystem::Status measure_at(StringView path, uint32_t depth);
  filesystem::Status copy_at(StringView from, StringView to, uint32_t depth);
  bool cancelled() const { return cancel_ && __atomic_load_n(cancel_, __ATOMIC_ACQUIRE); }

  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& fs_;
  FileFunction on_file_ = nullptr;
  BytesFunction on_bytes_ = nullptr;
  void* context_ = nullptr;
  const bool* cancel_ = nullptr;
  uint64_t files_total_ = 0, bytes_total_ = 0, files_done_ = 0, bytes_done_ = 0;
};

}  // namespace desktop
