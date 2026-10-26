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
struct X86Context;

// The static thread-local data of one module (its TLS directory): the initial bytes, and the zero fill after them. Each thread gets its own copy of every module's.
struct TlsTemplate {
  uint64_t address = 0, bytes = 0, zero_fill = 0;
};
constexpr uint32_t kMaxTlsModules = 16;

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

// Where a new thread of the program starts. The thread has its own stack and thread pointer (the TEB, for Windows); everything else is the process's.
struct ThreadStart {
  uint64_t entry = 0;                     // the first instruction
  uint64_t argument = 0, argument2 = 0;   // the first two argument registers (RCX, RDX)
  uint64_t stack_pointer = 0;
  uint64_t fs_base = 0, gs_base = 0;
  uint64_t id = 0;
};

// A program for CreateProcess: the file's bytes and the strings the new process starts with (all valid for the length of the call).
struct ChildSpec {
  uint64_t pid = 0;                      // chosen beforehand (IProcessRuntime::allocate_process_id)
  ConstByteView image;                   // the executable file
  StringView image_path;                 // as the program sees it ("C:\\dir\\program.exe")
  StringView command_line;
  StringView working_directory;          // in the program's world ("/dir" is C:\\dir)
  ArrayView<const StringView> environment;      // "NAME=value" each
  bool suspended = false;                // it does not run until resume_process()
};

// What a process needs to run more than one thread: supplied by whoever runs the program (the kernel). Without one a process has a single thread. The process has
// one lock; a system call holds it while it runs and gives it up while it blocks (sleeping, waiting, reading the console), so the threads of a process never
// look at its tables at the same time.
class IProcessRuntime {
 public:
  virtual void lock() = 0;
  virtual void unlock() = 0;
  // With the lock held: sleeps (the lock is released meanwhile) until notify() or the timeout (nanoseconds, ~0 = none); the lock is held again on return. False on timeout.
  virtual bool wait(uint64_t timeout_ns) = 0;
  virtual void notify() = 0;                          // wakes every thread in wait()
  virtual bool interrupted() = 0;                     // the program was told to stop (Ctrl+C): waits end
  virtual bool start_thread(const ThreadStart& start, bool suspended) = 0;
  virtual bool resume_thread(uint64_t id) = 0;        // a thread created suspended starts
  virtual uint64_t current_thread_id() = 0;
  // Stopping and ending other threads. A thread asked to suspend stops at its next system call or timer tick ("parked"); its registers are then kept.
  virtual void request_suspend(uint64_t id) { (void)id; }
  virtual void request_resume(uint64_t id) { (void)id; }
  virtual bool is_parked(uint64_t id) { (void)id; return false; }
  // The registers of a thread that is parked (or has not started yet); false if it is neither. set_context takes effect when it runs again.
  virtual bool get_context(uint64_t id, X86Context* out) { (void)id; (void)out; return false; }
  virtual bool set_context(uint64_t id, const X86Context& context) { (void)id; (void)context; return false; }
  // The thread ends itself (as if it had called ExitThread with `code`) as soon as it is told: at its next system call or timer tick.
  virtual void request_terminate(uint64_t id, uint32_t code) { (void)id; (void)code; }
  // Other processes (CreateProcess). The new process shares the console of this one. spawn_process returns an NTSTATUS (0xC0000002: not supported here).
  virtual uint64_t allocate_process_id() { return 0; }
  virtual uint32_t spawn_process(const ChildSpec& spec) { (void)spec; return 0xC0000002u; }
  virtual bool terminate_process(uint64_t pid, uint32_t code) { (void)pid; (void)code; return false; }
  virtual bool resume_process(uint64_t pid) { (void)pid; return false; }

 protected:
  IProcessRuntime() = default;
  ~IProcessRuntime() = default;
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
  // Optional: the process ended while one of its threads may be blocked in read_console() or sleep_ns(): make that return.
  virtual void wake_blocked() {}

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
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  uint64_t id() const { return id_; }
  SyscallAbi abi() const { return binding_.abi; }
  UserArch arch() const { return binding_.arch; }

  // Runs one system call through the process's interface. A process without an interface (or one that already exited) gets the ABI's "no such call".
  uint64_t syscall(const SyscallRequest& request);

  bool exited() const { return __atomic_load_n(&exited_, __ATOMIC_ACQUIRE); }
  uint32_t exit_code() const { return exit_code_; }
  // The first ending counts: a process that already exited (ExitProcess) keeps its code when its other threads are stopped.
  void terminate(uint32_t code);

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

  // Structured exception handling (Windows): where a fault of the program is delivered (ntdll's KiUserExceptionDispatcher; 0 = nowhere, the program ends), and the
  // CONTEXT that NtContinue asked for (whoever runs the program loads it into the CPU's registers after the call: take_context_restore()).
  uint64_t exception_dispatcher() const { return exception_dispatcher_; }
  void set_exception_dispatcher(uint64_t address) { exception_dispatcher_ = address; }
  void request_context_restore(uint64_t address) { context_restore_ = address; context_restore_pending_ = true; }
  bool take_context_restore(uint64_t* address) { *address = context_restore_; const bool p = context_restore_pending_; context_restore_pending_ = false; return p; }

  // --- threads (Windows). The table lives here; the runtime starts the CPU's threads.
  static constexpr uint32_t kMaxThreads = 64;
  struct ThreadRecord {
    uint64_t id = 0;
    bool used = false, started = false, finished = false;
    int32_t suspend_count = 0;                        // SuspendThread calls not yet undone (a thread created suspended starts at 1)
    uint32_t exit_code = 259;                         // STILL_ACTIVE
    uint64_t teb = 0, region_base = 0, region_size = 0;      // the thread's TEB and the one region that holds its stack, TEB and TLS block
  };
  void attach_runtime(IProcessRuntime* runtime) { runtime_ = runtime; }
  IProcessRuntime* runtime() { return runtime_; }
  // The thread the process started with.
  void register_main_thread(uint64_t id, uint64_t teb);
  // The Windows startup data a new thread needs: the PEB, the image's static TLS template (see build_windows_thread), and the ntdll function new threads begin in.
  struct ThreadSupport {
    uint64_t peb = 0, start_thunk = 0;
    TlsTemplate tls[kMaxTlsModules];      // module index -> its template
    uint32_t tls_count = 0;
    uint64_t stack_bytes = 1ull << 20;                // the default size of a new thread's stack
  };
  void set_thread_support(const ThreadSupport& support) { thread_support_ = support; }
  const ThreadSupport& thread_support() const { return thread_support_; }
  ThreadRecord* find_thread(uint64_t id);
  ThreadRecord* add_thread(uint64_t id);
  uint32_t live_threads() const;
  uint64_t next_thread_id() { return (uint64_t{++thread_counter_} << 20) | id_; }
  // The calling thread ended itself (NtTerminateThread): whoever runs the program ends the CPU thread once the call returns. Returns the code.
  void request_thread_exit(uint32_t code) { thread_exit_pending_ = true; thread_exit_code_ = code; }
  bool take_thread_exit(uint32_t* code) { *code = thread_exit_code_; const bool p = thread_exit_pending_; thread_exit_pending_ = false; return p; }

  // --- blocking helpers for the system calls: they give up the process lock while the thread is stuck.
  void block_sleep(uint64_t ns);
  void block_yield();
  bool block_read_console(ByteView out, uint64_t* got);

  // The process's files and descriptors. Without a file system attached only the console descriptors work.
  ProcessFiles& files() { return files_; }
  // Gives the process its tree: "/Linux" is the root of a Linux process, "/Windows" is C:\\ of a Windows one, both in `fs`.
  void attach_file_system(allocators::IAllocator& allocator, filesystem::IFileSystem& fs) {
    files_.attach(allocator, fs, binding_.abi == SyscallAbi::Windows ? StringView("/Windows") : StringView("/Linux"));
  }
  DescriptorKind descriptor(uint64_t slot) const { return files_.kind(slot); }
  // Closes a descriptor; a handle to a synchronization object lets go of the object.
  bool close_descriptor(uint64_t slot);

 private:
  uint64_t id_;
  ProcessBinding binding_;
  IUserMemory& memory_;
  IProcessHost& host_;
  bool exited_ = false;
  uint32_t exit_code_ = 0;
  IProcessRuntime* runtime_ = nullptr;
  ThreadRecord threads_[kMaxThreads];
  ThreadSupport thread_support_;
  uint32_t thread_counter_ = 0, thread_exit_code_ = 0;
  bool thread_exit_pending_ = false;
  uint64_t fs_base_ = 0, gs_base_ = 0;
  bool fs_changed_ = false, gs_changed_ = false;
  uint64_t exception_dispatcher_ = 0, context_restore_ = 0;
  bool context_restore_pending_ = false;
  AddressSpace* space_ = nullptr;
  uint64_t heap_start_ = 0, heap_break_ = 0, mmap_cursor_ = 0x00007F0000000000ull;
  ProcessFiles files_;
};

// Moves bytes between the program and the console in small pieces, so a huge or half-mapped buffer cannot make the kernel allocate or fault. Return
// the number of bytes moved; `*faulted` says the program's memory ended the transfer early.
uint64_t copy_user_to_console(Process& process, uint32_t stream, uint64_t address, uint64_t count, bool* faulted);
uint64_t copy_console_to_user(Process& process, uint64_t address, uint64_t count, bool* faulted);

}  // namespace process
