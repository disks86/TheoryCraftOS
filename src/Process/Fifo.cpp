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


#include "Process/Fifo.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/String.hpp"

namespace process {

using filesystem::DirectoryEntry;
using filesystem::SpecialNode;
using filesystem::Status;

Status make_fifo_node(filesystem::IFileSystem& fs, StringView host_path) {
  DirectoryEntry entry;
  const Status there = fs.stat(host_path, &entry);
  if (there == Status::Ok) return Status::AlreadyExists;
  if (there != Status::NotFound) return there;
  const Status made = fs.create_special(host_path, SpecialNode::Fifo);
  if (made != Status::Unsupported) return made;
  // (a file system without FIFO nodes: an empty file, remembered as a FIFO)
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return Status::OutOfMemory;
  {
    UniquePointer<filesystem::IFile> file;
    const Status opened = fs.open_file(host_path, filesystem::OpenMode::CreateOrOpen, &file);
    if (opened != Status::Ok) return opened;
  }
  if (m->fifo_mark(host_path) != UnixStatus::Ok) {
    (void)fs.remove(host_path);      // (best effort: nothing would know it as a FIFO)
    return Status::OutOfMemory;
  }
  return Status::Ok;
}

bool is_fifo_node(StringView host_path, const DirectoryEntry& entry) {
  if (entry.special == SpecialNode::Fifo) return true;
  if (entry.special != SpecialNode::None || entry.type != filesystem::NodeType::File) return false;
  UnixSocketManager* m = system_unix_sockets();
  return m && m->fifo_marked(host_path);
}

KernelFifo::~KernelFifo() {
  if (UnixSocketManager* m = system_unix_sockets(); m && end_) m->release(end_);
}

Status KernelFifo::open(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView host_path, bool create, UniquePointer<KernelFifo>* out) {
  *out = UniquePointer<KernelFifo>();
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return Status::OutOfMemory;
  DirectoryEntry entry;
  Status there = fs.stat(host_path, &entry);
  if (there == Status::NotFound && create) {
    there = make_fifo_node(fs, host_path);
    if (there == Status::AlreadyExists) there = Status::Ok;      // (made by someone else meanwhile)
    if (there == Status::Ok) there = fs.stat(host_path, &entry);
  }
  if (there != Status::Ok) return there;
  if (!is_fifo_node(host_path, entry)) return Status::InvalidArgument;
  uint32_t end = 0;
  UnixStatus st;
  {
    ISystemSync& sync = m->sync();
    sync.lock();
    st = m->fifo_open_locked(host_path, true, true, true, 0, &end);
    sync.unlock();
  }
  if (st != UnixStatus::Ok) return Status::OutOfMemory;
  UniquePointer<KernelFifo> made = UniquePointer<KernelFifo>::make(allocator, end);
  if (!made) {
    m->release(end);
    return Status::OutOfMemory;
  }
  *out = static_cast<UniquePointer<KernelFifo>&&>(made);
  return Status::Ok;
}

uint32_t KernelFifo::read(ByteView out) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return 0;
  uint32_t got = 0;
  m->sync().lock();
  const UnixStatus st = m->fifo_read_locked(end_, out, &got);
  m->sync().unlock();
  return st == UnixStatus::Ok ? got : 0;
}

uint32_t KernelFifo::write(ConstByteView data) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return 0;
  uint32_t wrote = 0;
  m->sync().lock();
  const UnixStatus st = m->fifo_write_locked(end_, data, &wrote);
  m->sync().unlock();
  return st == UnixStatus::Ok ? wrote : 0;
}

uint32_t KernelFifo::queued() {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return 0;
  m->sync().lock();
  const uint32_t n = m->readable_bytes_locked(end_);
  m->sync().unlock();
  return n;
}

}  // namespace process
