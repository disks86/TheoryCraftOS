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
#include "FileSystem/IFileSystem.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

// Named pipes (FIFOs) by the path of the OS's file system (a program's path turned into it by ProcessFiles::host_path_of; "/Linux/tmp/music.output" for a
// Linux program's /tmp/music.output). Where the node lives: on a file system that keeps Unix node types (ext2/3/4) a real FIFO inode
// (IFileSystem::create_special); on the others (FAT, NTFS, RAM, ...) an empty file that the system's UnixSocketManager remembers as a FIFO by its path
// (fifo_mark) until it is removed or renamed through a process (ProcessFiles keeps the list up to date) or the system restarts. The pipe behind the name
// is the manager's (UnixSockets.hpp), shared by every opener: Linux programs (open(2), LinuxSockets.cpp) and the kernel's own services (KernelFifo).
namespace process {

// mkfifo(): makes the node. AlreadyExists if the name is taken (by anything), NotFound without the folder, NoMemory when the manager's list of FIFO names
// is full (the file is not left behind then).
[[nodiscard]] filesystem::Status make_fifo_node(filesystem::IFileSystem& fs, StringView host_path);
// Is the node `host_path`, whose stat() is `entry`, a FIFO?
[[nodiscard]] bool is_fifo_node(StringView host_path, const filesystem::DirectoryEntry& entry);

// One open end of a FIFO the kernel holds itself (PipeWire's module-pipe-sink and -source), opened for reading and writing at once as PulseAudio opens
// its pipes (O_RDWR): it never waits for a partner, a reader of it never sees the end of the file and a writer never EPIPE. Nothing here blocks.
class KernelFifo {
 public:
  ~KernelFifo();
  KernelFifo(const KernelFifo&) = delete;
  KernelFifo& operator=(const KernelFifo&) = delete;

  // Opens the FIFO at `host_path`, making it first when it is missing and `create` says so. Ok; NotFound (missing, not made); InvalidArgument for a name
  // that is something other than a FIFO; OutOfMemory when the manager has no room (or there is no manager).
  [[nodiscard]] static filesystem::Status open(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, StringView host_path, bool create,
                                               UniquePointer<KernelFifo>* out);
  // What is there, up to out.size() bytes (0: nothing now).
  [[nodiscard]] uint32_t read(ByteView out);
  // As much of `data` as there is room for (all of it or nothing for up to UnixSocketManager::kPipeAtomic bytes); 0 when the pipe is full.
  [[nodiscard]] uint32_t write(ConstByteView data);
  uint32_t queued();      // bytes in the pipe
  uint32_t id() const { return end_; }

  explicit KernelFifo(uint32_t end) : end_(end) {}      // (open() makes them)

 private:
  uint32_t end_ = 0;
};

}  // namespace process
