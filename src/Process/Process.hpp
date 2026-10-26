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
#include "Process/Syscall.hpp"
#include "Utility/ArrayView.hpp"

namespace process {

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

 protected:
  IProcessHost() = default;
  ~IProcessHost() = default;
};

// What a descriptor (Linux file descriptor, Windows handle) refers to.
enum class DescriptorKind : uint8_t { Closed, ConsoleInput, ConsoleOutput, ConsoleError };

// One user-mode process as far as system calls go: its ABI, its descriptors, its exit state. Both interfaces share the descriptor table; Linux
// uses the slot number as the file descriptor, Windows uses (slot + 1) * 4 as the handle value.
class Process {
 public:
  static constexpr uint32_t kMaxDescriptors = 16;

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

  DescriptorKind descriptor(uint64_t slot) const { return slot < kMaxDescriptors ? slots_[slot] : DescriptorKind::Closed; }
  bool close_descriptor(uint64_t slot);

 private:
  uint64_t id_;
  ProcessBinding binding_;
  IUserMemory& memory_;
  IProcessHost& host_;
  bool exited_ = false;
  uint32_t exit_code_ = 0;
  DescriptorKind slots_[kMaxDescriptors] = {DescriptorKind::ConsoleInput, DescriptorKind::ConsoleOutput, DescriptorKind::ConsoleError};
};

// Moves bytes between the program and the console in small pieces, so a huge or half-mapped buffer cannot make the kernel allocate or fault. Return
// the number of bytes moved; `*faulted` says the program's memory ended the transfer early.
uint64_t copy_user_to_console(Process& process, uint32_t stream, uint64_t address, uint64_t count, bool* faulted);
uint64_t copy_console_to_user(Process& process, uint64_t address, uint64_t count, bool* faulted);

}  // namespace process
