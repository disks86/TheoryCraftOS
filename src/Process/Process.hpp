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
#include "Process/ProcessFiles.hpp"
#include "Process/Syscall.hpp"
#include "Utility/ArrayView.hpp"

namespace process {

class AddressSpace;

// The program's address space, as the kernel reaches it from a system call. A read or write fails (false) when any byte of the range is not mapped
// for the program (or not writable): a bad pointer from user mode must never fault the kernel.
class IUserMemory {
 public:
  virtual bool read(uint64_t address, ByteView out) = 0;
  virtual bool write(uint64_t address, ConstByteView data) = 0;

 protected:
  IUserMemory() = default;
  ~IUserMemory() = default;
};

// What a process needs from the rest of the system.
class IProcessHost {
 public:
  virtual bool write_console(uint32_t stream, ConstByteView data) = 0;     // stream 1 = standard output, 2 = standard error
  virtual bool read_console(ByteView out, uint64_t* got) = 0;              // *got = 0 at end of input
  virtual uint64_t realtime_ns() = 0;                                      // nanoseconds since 1970-01-01 UTC
  virtual uint64_t monotonic_ns() = 0;
  virtual void sleep_ns(uint64_t ns) = 0;
  virtual void yield() = 0;
  // Optional: fills `out` with random bytes (getrandom); false if there is no source, which the program sees as "not supported".
  virtual bool random_bytes(ByteView out) { (void)out; return false; }
  // Optional: a program made a system call this OS does not have (for finding out what a program needs).
  virtual void unhandled_syscall(SyscallAbi abi, uint64_t number) { (void)abi; (void)number; }

 protected:
  IProcessHost() = default;
  ~IProcessHost() = default;
};

// One user-mode process as far as system calls go: its ABI, its descriptors, its exit state. Both interfaces share the descriptor table; Linux
// uses the slot number as the file descriptor, Windows uses (slot + 1) * 4 as the handle value.
class Process {
 public:
  static constexpr uint32_t kMaxDescriptors = ProcessFiles::kMaxDescriptors;

  Process(uint64_t id, const ProcessBinding& binding, IUserMemory& memory, IProcessHost& host);
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  uint64_t id() const { return id_; }
  SyscallAbi abi() const { return binding_.abi; }
  UserArch arch() const { return binding_.arch; }

  // Runs one system call through the process's interface. A process without an interface (or one that already exited) gets the ABI's "no such call".
  uint64_t syscall(const SyscallRequest& request);

  bool exited() const { return exited_; }
  uint32_t exit_code() const { return exit_code_; }
  void terminate(uint32_t code) { exited_ = true; exit_code_ = code; }

  IProcessHost& host() { return host_; }
  bool read_user(uint64_t address, ByteView out) { return memory_.read(address, out); }
  bool write_user(uint64_t address, ConstByteView data) { return memory_.write(address, data); }
  bool read_user_u64(uint64_t address, uint64_t* out);
  bool write_user_u64(uint64_t address, uint64_t value);

  // The address space the process runs in, when the kernel manages it (memory system calls need it; without one they fail). `heap_start` is where the
  // Linux program break begins (page-aligned, just past the image).
  void attach_address_space(AddressSpace& space, uint64_t heap_start) { space_ = &space; heap_start_ = heap_break_ = heap_start; }
  AddressSpace* address_space() { return space_; }
  uint64_t heap_start() const { return heap_start_; }
  uint64_t heap_break() const { return heap_break_; }
  void set_heap_break(uint64_t value) { heap_break_ = value; }
  // Where the next search for free memory starts (mmap, NtAllocateVirtualMemory); moves up as memory is handed out.
  uint64_t mmap_cursor() const { return mmap_cursor_; }
  void set_mmap_cursor(uint64_t value) { mmap_cursor_ = value; }

  // The thread pointers of the program's (one) thread: the FS base (Linux thread-local storage, set by arch_prctl) and the GS base (the Windows TEB). They
  // live here, in the portable part; whoever runs the program applies a changed one to the CPU after the system call (take_*_change()).
  uint64_t fs_base() const { return fs_base_; }
  uint64_t gs_base() const { return gs_base_; }
  void set_fs_base(uint64_t value) { fs_base_ = value; fs_changed_ = true; }
  void set_gs_base(uint64_t value) { gs_base_ = value; gs_changed_ = true; }
  // The program starts with these (not reported as a change).
  void set_initial_thread_pointers(uint64_t fs, uint64_t gs) { fs_base_ = fs; gs_base_ = gs; fs_changed_ = gs_changed_ = false; }
  // True once after the base was set.
  bool take_fs_change(uint64_t* value) { *value = fs_base_; const bool c = fs_changed_; fs_changed_ = false; return c; }
  bool take_gs_change(uint64_t* value) { *value = gs_base_; const bool c = gs_changed_; gs_changed_ = false; return c; }

  // The process's files and descriptors. Without a file system attached only the console descriptors work.
  ProcessFiles& files() { return files_; }
  // Gives the process its tree: "/Linux" is the root of a Linux process, "/Windows" is C:\\ of a Windows one, both in `fs`.
  void attach_file_system(allocators::IAllocator& allocator, filesystem::IFileSystem& fs) {
    files_.attach(allocator, fs, binding_.abi == SyscallAbi::Windows ? StringView("/Windows") : StringView("/Linux"));
  }
  DescriptorKind descriptor(uint64_t slot) const { return files_.kind(slot); }
  bool close_descriptor(uint64_t slot) { return files_.close(slot); }

 private:
  uint64_t id_;
  ProcessBinding binding_;
  IUserMemory& memory_;
  IProcessHost& host_;
  bool exited_ = false;
  uint32_t exit_code_ = 0;
  uint64_t fs_base_ = 0, gs_base_ = 0;
  bool fs_changed_ = false, gs_changed_ = false;
  AddressSpace* space_ = nullptr;
  uint64_t heap_start_ = 0, heap_break_ = 0, mmap_cursor_ = 0x00007F0000000000ull;
  ProcessFiles files_;
};

// Moves bytes between the program and the console in small pieces, so a huge or half-mapped buffer cannot make the kernel allocate or fault. Return
// the number of bytes moved; `*faulted` says the program's memory ended the transfer early.
uint64_t copy_user_to_console(Process& process, uint32_t stream, uint64_t address, uint64_t count, bool* faulted);
uint64_t copy_console_to_user(Process& process, uint64_t address, uint64_t count, bool* faulted);

}  // namespace process
