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
#include "Process/AddressSpace.hpp"
#include "Process/ImageLoader.hpp"
#include "Process/Process.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace process {

// The state a program's first thread starts with.
struct ProgramStart {
  UserArch arch = UserArch::None;
  uint64_t entry = 0;
  uint64_t stack_pointer = 0;
  uint64_t argument = 0;      // first argument register (Windows: the PEB, as the NT process start passes it)
  uint64_t fs_base = 0;       // thread pointers the thread starts with (Windows: gs_base is the TEB)
  uint64_t gs_base = 0;
};

// A user-mode program ready to run: its address space, its Process (with the syscall interface its executable kind selects) and the start state of its
// first thread. load() is the one call that turns the bytes of a file into all of that:
//   identify (ELF or PE) -> parse -> choose the system-call interface -> map the image -> map the stack -> build the startup block -> attach the
//   address space to the process.
// Linux programs get arguments and environment on their stack; Windows programs get them in the PEB's process parameters (the command line is the
// arguments joined by spaces). The first argument is also the program's path.
class UserProgram {
 public:
  UserProgram(allocators::IAllocator& allocator, uint64_t id, IProcessHost& host, uint64_t memory_limit = 256ull << 20)
      : allocator_(allocator), id_(id), host_(host), space_(allocator, memory_limit) {}
  UserProgram(const UserProgram&) = delete;
  UserProgram& operator=(const UserProgram&) = delete;

  // The OS file system the program's tree lives in ("/Linux" for a Linux program, "/Windows" for a Windows one). Set before load(); without it the program
  // has no files. The file system must outlive the program.
  void set_file_system(filesystem::IFileSystem* fs) { files_ = fs; }

  // `random` is the 16 bytes of AT_RANDOM. Call once; on failure the program is unusable (create another).
  LoadStatus load(ConstByteView file, ArrayView<const StringView> arguments, ArrayView<const StringView> environment, ConstByteView random);

  bool loaded() const { return process_ ? true : false; }
  Process* process() { return process_.get(); }
  AddressSpace& address_space() { return space_; }
  const ProgramStart& start() const { return start_; }
  const LoadedImage& image() const { return image_; }

 private:
  allocators::IAllocator& allocator_;
  uint64_t id_;
  IProcessHost& host_;
  AddressSpace space_;
  filesystem::IFileSystem* files_ = nullptr;
  UniquePointer<Process> process_;
  ProgramStart start_;
  LoadedImage image_;
};

}  // namespace process
