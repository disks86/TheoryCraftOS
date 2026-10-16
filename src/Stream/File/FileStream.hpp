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
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Stream/IAsyncStream.hpp"
#include "Utility/UniquePointer.hpp"

namespace stream {

// A stream over a file in an IFileSystem, like C#'s FileStream: readable,
// writable (unless opened with OpenMode::Read) and seekable, with its own
// position. Writing at or past the end extends the file; seek(0, End) then write
// appends. Closing (or destroying) the stream closes the file; do that before the
// filesystem goes away.
//
// OpenMode maps onto C#'s FileMode: Read = Open+Read, ReadWrite = Open,
// CreateOrOpen = OpenOrCreate, CreateTruncate = Create.
class FileStream final : public AsyncStreamBase {
 public:
  static filesystem::Status open(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView path,
                                 filesystem::OpenMode mode, UniquePointer<IAsyncStream>* result);

  // Over a file you already opened; `writable` says whether it may be written.
  FileStream(UniquePointer<filesystem::IFile> file, bool writable) : file_(static_cast<UniquePointer<filesystem::IFile>&&>(file)), writable_(writable) {}
  ~FileStream() = default;
  FileStream(const FileStream&) = delete;
  FileStream& operator=(const FileStream&) = delete;

  bool can_read() const override { return !closed_; }
  bool can_write() const override { return !closed_ && writable_; }
  bool can_seek() const override { return !closed_; }

  StreamStatus read(ByteView buffer, uint64_t* count) override;
  StreamStatus write(ConstByteView data) override;
  StreamStatus flush() override;
  StreamStatus close() override;
  StreamStatus length(uint64_t* length) const override;
  StreamStatus position(uint64_t* position) const override;
  StreamStatus seek(int64_t offset, SeekOrigin origin, uint64_t* new_position) override;
  StreamStatus set_length(uint64_t length) override;

 private:
  UniquePointer<filesystem::IFile> file_;
  uint64_t position_ = 0;
  bool writable_;
};

}  // namespace stream
