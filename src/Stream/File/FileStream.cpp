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

#include "Stream/File/FileStream.hpp"
#include "Utility/Move.hpp"

namespace stream {

namespace {
StreamStatus from_file_status(filesystem::Status s) {
  switch (s) {
    case filesystem::Status::Ok: return StreamStatus::Ok;
    case filesystem::Status::ReadOnly: return StreamStatus::ReadOnly;
    case filesystem::Status::NoSpace: return StreamStatus::NoSpace;
    case filesystem::Status::OutOfMemory: return StreamStatus::OutOfMemory;
    case filesystem::Status::InvalidArgument: return StreamStatus::InvalidArgument;
    case filesystem::Status::Unsupported: return StreamStatus::NotSupported;
    default: return StreamStatus::IoError;
  }
}
}  // namespace

filesystem::Status FileStream::open(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView path,
                                    filesystem::OpenMode mode, UniquePointer<IAsyncStream>* result) {
  UniquePointer<filesystem::IFile> file;
  const filesystem::Status s = fs.open_file(path, mode, &file);
  if (s != filesystem::Status::Ok) return s;
  UniquePointer<IAsyncStream> opened =
      UniquePointer<IAsyncStream>::make_as<FileStream>(allocator, util::move(file), mode != filesystem::OpenMode::Read);
  if (!opened) return filesystem::Status::OutOfMemory;
  *result = util::move(opened);
  return filesystem::Status::Ok;
}

StreamStatus FileStream::read(ByteView buffer, uint64_t* count) {
  *count = 0;
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  if (buffer.empty()) return StreamStatus::Ok;
  uint64_t got = 0;
  const StreamStatus s = from_file_status(file_->read(position_, buffer, &got));
  position_ += got;
  *count = got;
  return s;
}

StreamStatus FileStream::write(ConstByteView data) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  if (!writable_) return StreamStatus::ReadOnly;
  if (data.empty()) return StreamStatus::Ok;
  uint64_t put = 0;
  const StreamStatus s = from_file_status(file_->write(position_, data, &put));
  position_ += put;
  return s != StreamStatus::Ok ? s : (put == data.size() ? StreamStatus::Ok : StreamStatus::IoError);
}

StreamStatus FileStream::flush() {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  return from_file_status(file_->flush());
}

StreamStatus FileStream::close() {
  if (closed_) return StreamStatus::Ok;
  StreamStatus s = StreamStatus::Ok;
  if (writable_) s = from_file_status(file_->flush());  // like C#: closing a written file flushes it
  file_.reset();
  closed_ = true;
  return s;
}

StreamStatus FileStream::length(uint64_t* length) const {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  *length = file_->size();
  return StreamStatus::Ok;
}

StreamStatus FileStream::position(uint64_t* position) const {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  *position = position_;
  return StreamStatus::Ok;
}

StreamStatus FileStream::seek(int64_t offset, SeekOrigin origin, uint64_t* new_position) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  uint64_t target = 0;
  const StreamStatus s = resolve_seek(offset, origin, position_, file_->size(), &target);
  if (s != StreamStatus::Ok) return s;
  position_ = target;
  if (new_position) *new_position = target;
  return StreamStatus::Ok;
}

StreamStatus FileStream::set_length(uint64_t length) {
  if (const StreamStatus s = check(); s != StreamStatus::Ok) return s;
  if (!writable_) return StreamStatus::ReadOnly;
  return from_file_status(file_->resize(length));
}

}  // namespace stream
