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

// An open file of a filesystem as a byte source for the media decoders: they read it a piece at a time, from whichever thread plays.
#pragma once
#include "FileSystem/IFile.hpp"
#include "Kernel/Sync.hpp"
#include "Media/ByteSource.hpp"
#include "Utility/UniquePointer.hpp"
#include "Utility/Move.hpp"

namespace desktop {

class FileByteSource final : public media::IByteSource {
 public:
  explicit FileByteSource(UniquePointer<filesystem::IFile> file) : file_(util::move(file)) {}

  uint64_t size() const override { return file_->size(); }
  uint64_t read(uint64_t offset, ByteView out) override {
    kernel::ScopedMutex guard(mutex_);   // (the sound and the picture of a video are read by different threads)
    uint64_t got = 0;
    return file_->read(offset, out, &got) == filesystem::Status::Ok ? got : 0;
  }

 private:
  UniquePointer<filesystem::IFile> file_;
  kernel::Mutex mutex_;
};

}  // namespace desktop
