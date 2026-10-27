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
#include "Process/Objects.hpp"
#include "Process/WindowsException.hpp"
#include "Process/Syscall.hpp"
#include "Process/UserProgram.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

// The in-kernel CPU emulator: runs a program built for another CPU (an x86-64 program on an Arm or RISC-V machine) by interpreting its instructions inside the
// kernel. There is no ring 3 involved: the guest's memory is the program's AddressSpace, reached only through the bounds-checked accessors, and its system calls
// go straight to the same process::Process interfaces a native program uses (the Linux tables already cover all three CPUs). So an emulated program needs
// nothing from the CPU it runs on except being a kernel thread.
namespace emulation {

// The guest's view of its memory. Every access is checked against the regions and permissions of the address space: a bad guest pointer is a guest fault, never a
// kernel one.
class GuestMemory {
 public:
  explicit GuestMemory(process::AddressSpace& space) : space_(space) {}
  bool read(uint64_t address, ByteView out) const { return space_.read(address, out); }
  bool write(uint64_t address, ConstByteView data) { return space_.write(address, data); }
  // Reading instructions needs the execute permission, not the read one.
  bool fetch(uint64_t address, ByteView out) const { return space_.check(address, out.size(), process::kProtExecute) && space_.read_privileged(address, out); }
  // How many bytes from `address` on can be fetched (up to `limit`), for decoding an instruction near the end of a mapping.
  uint64_t fetchable(uint64_t address, uint64_t limit) const;

 private:
  process::AddressSpace& space_;
};

enum class StopReason : uint8_t {
  Budget,       // the instruction budget ran out; call run() again
  Syscall,      // the guest executed its system-call instruction: see pending_syscall()
  Fault,        // the guest did something the CPU would trap: see fault()
};

// What went wrong, in x86 terms (the program runner and the Windows exception code already speak them): 0 divide error, 3 breakpoint, 6 invalid opcode, 13 general
// protection, 14 page fault (address = the address that could not be reached).
struct GuestFault {
  uint8_t vector = 0;
  uint64_t address = 0;
  uint64_t pc = 0;
  uint32_t error = 0;      // a page fault's x86 error code bits: 2 = the access was a write, 0x10 = an instruction fetch
};

class IGuestCpu {
 public:
  virtual process::UserArch arch() const = 0;
  // The registers as the program starts: entry point, stack pointer, the first two argument registers, the thread pointers.
  virtual void reset(const process::ProgramStart& start) = 0;
  // Runs at most `max_instructions` instructions (a repeated string instruction counts once per call). Never touches anything but the guest's registers and memory.
  virtual StopReason run(GuestMemory& memory, uint64_t max_instructions) = 0;
  // After StopReason::Syscall: the call, with the registers sorted the way the ABI wants. False if this ABI does not exist on this CPU.
  virtual bool pending_syscall(process::SyscallAbi abi, process::SyscallRequest* out) const = 0;
  // Puts the result where the guest expects it; the next run() continues after the system-call instruction.
  virtual void finish_syscall(uint64_t result) = 0;
  // arch_prctl and friends: the thread pointers changed.
  virtual void set_thread_pointers(bool fs_changed, uint64_t fs, bool gs_changed, uint64_t gs) = 0;
  virtual const GuestFault& fault() const = 0;
  virtual uint64_t instructions() const = 0;      // retired so far
  // A copy of this CPU's registers for a new thread (clone): the system-call result register is 0, the stack pointer is `stack_pointer` (unless it is 0) and, with
  // `set_tls`, the thread pointer is `tls`. Null if there is no memory.
  // The registers as a Windows CONTEXT-style image and back (structured exceptions: the context is saved on the stack when an exception is delivered, and NtContinue
  // restores one). False for a CPU that has no such image (only the x86-64 guest does).
  virtual bool get_x86_context(process::X86Context* out) const { (void)out; return false; }
  virtual bool set_x86_context(const process::X86Context& context) { (void)context; return false; }
  virtual UniquePointer<IGuestCpu> clone_for_thread(allocators::IAllocator& allocator, uint64_t stack_pointer, bool set_tls, uint64_t tls) const = 0;

 protected:
  IGuestCpu() = default;
  ~IGuestCpu() = default;
};

// The emulator for a guest CPU; null if there is none (this OS emulates x86-64 only so far).
UniquePointer<IGuestCpu> create_guest_cpu(process::UserArch guest, allocators::IAllocator& allocator);
bool can_emulate(process::UserArch guest);

// --- Running a whole program ---------------------------------------------------------------------------------------------------------------------
// How an emulated program ended.
struct EmulatedOutcome {
  bool exited = false;      // the process ended itself (exit_group, ExitProcess)
  bool killed = false;      // stopped from outside
  bool faulted = false;
  GuestFault fault;
  uint32_t exit_code = 0;
};

// The hooks the caller supplies for the parts that belong to the kernel thread running the guest: should it stop (Ctrl+C), and let others run.
class IRunControl {
 public:
  virtual bool stop_requested() = 0;
  virtual void yield() = 0;

 protected:
  IRunControl() = default;
  ~IRunControl() = default;
};

// Threads of an emulated program. Each guest thread is a thread of the system (kernel thread, or a host thread in tests); they take turns, one at a time, on the
// process's lock, so the guest sees a sequentially consistent machine and its atomic instructions are atomic for free.
class IThreadSpawner {
 public:
  // Starts fn(arg) on a new thread; null if there is none to be had.
  virtual void* spawn(void (*fn)(void*), void* arg, const char* name) = 0;
  // Waits for the thread to finish and releases it.
  virtual void join(void* handle) = 0;
  // Identifies the calling thread (any value, the same for the whole life of a thread, different between threads).
  virtual uint64_t self() = 0;

 protected:
  IThreadSpawner() = default;
  ~IThreadSpawner() = default;
};

// Runs a loaded program on `cpu` until it exits, faults or is told to stop. `program` must be loaded. Instructions are run in slices of `slice` so that a program in
// a tight loop can still be stopped. Single-threaded: clone() and CreateThread fail.
EmulatedOutcome run_emulated(process::UserProgram& program, IGuestCpu& cpu, IRunControl& control, uint64_t slice = 20000);
// The same with threads: `sync` is the process lock and wake-up (see process::ISystemSync), `threads` starts the guest threads, `allocator` makes their CPUs. `cpu` runs on the
// calling thread; the call returns when every thread of the program has ended.
EmulatedOutcome run_emulated_threads(process::UserProgram& program, IGuestCpu& cpu, IRunControl& control, process::ISystemSync& sync, IThreadSpawner& threads,
                                     allocators::IAllocator& allocator, uint64_t slice = 20000);

}  // namespace emulation
