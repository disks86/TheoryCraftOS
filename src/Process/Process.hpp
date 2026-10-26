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

// One library that LoadLibrary brought into the process.
struct LoadedModule {
  uint64_t base = 0, size = 0, entry = 0;      // entry: DllMain (0 if none)
  char name[40] = {};                           // canonical ("msvcrt.dll"), NUL-terminated
};
// What loads and unloads libraries in a Windows process after it has started (the user-program layer implements it; the process does not know how libraries are found).
class IModuleHost {
 public:
  // Loads the library in `file` under `canonical_name` and the libraries it imports that are missing, and says in `out` (at most `capacity`) which libraries are new, in the
  // order their entry points must run. Returns an NTSTATUS; nothing stays loaded on failure.
  virtual uint32_t load_module(ConstByteView file, StringView canonical_name, LoadedModule* out, uint32_t capacity, uint32_t* count) = 0;
  // LoadLibrary of a library that is loaded already (by this canonical name): one more use. False if there is none.
  virtual bool reference_module(StringView canonical_name, uint64_t* base) = 0;
  // FreeLibrary: one use less. The libraries that nobody uses any more are listed in `out` (at most `capacity`) in the order their entry points have to be called with
  // DLL_PROCESS_DETACH; they stay mapped until unload_module(). Returns an NTSTATUS.
  virtual uint32_t release_module(uint64_t base, LoadedModule* out, uint32_t capacity, uint32_t* count) = 0;
  virtual uint32_t unload_module(uint64_t base) = 0;

 protected:
  IModuleHost() = default;
  ~IModuleHost() = default;
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
  bool inherit_handles = false;          // the child gets the parent's inheritable handles, at the same numbers
  bool use_std_handles = false;          // the child's standard input, output and error are these handles of the parent (which are inherited too)
  uint64_t std_handles[3] = {};
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
  // The machine: how many cores threads can run on, and the memory.
  virtual uint32_t processor_count() { return 1; }
  virtual void memory_status(uint64_t* total_bytes, uint64_t* free_bytes) { *total_bytes = *free_bytes = 0; }
  // Which cores a thread (id 0: every thread of the process, and the ones made later) may run on: bit n is core n. False if the mask names no core that exists.
  virtual bool set_affinity(uint64_t thread_id, uint64_t mask) { (void)thread_id; (void)mask; return mask == 1; }
  virtual uint64_t affinity(uint64_t thread_id) { (void)thread_id; return 1; }
  // The thread's priority in the kernel scheduler: 0 (lowest) .. 31 (highest); a thread always runs ahead of every lower one, round-robin among equals. False if the
  // thread is unknown. (Windows' SetThreadPriority is relative to the process's priority class; windowsabi works that out before calling this.)
  virtual bool set_priority(uint64_t thread_id, int32_t absolute_priority) { (void)thread_id; (void)absolute_priority; return false; }
  // CPU time used, in units of 100 ns (thread_id 0: the whole process, including threads that ended). False if the thread is unknown.
  virtual bool cpu_time(uint64_t thread_id, uint64_t* hundred_ns) { (void)thread_id; *hundred_ns = 0; return true; }
  // The core the calling thread runs on right now.
  virtual uint32_t current_processor() { return 0; }
  // The running programs (Toolhelp, EnumProcesses): returns how many entries it wrote.
  struct ProcessEntry { uint64_t pid = 0, parent = 0; uint32_t threads = 0; char name[24] = {}; uint64_t memory_bytes = 0, create_time = 0; };
  virtual uint32_t list_processes(ProcessEntry* out, uint32_t capacity) { (void)out; (void)capacity; return 0; }
  // Work on the memory and threads of another running program (by process id). The runtime takes that program's lock and gives up the caller's meanwhile; the work is done by
  // windowsabi::run_remote(). Returns an NTSTATUS.
  struct RemoteOp {
    enum Kind : uint8_t { Read, Write, Allocate, Free, Protect, Query, CreateThread, ThreadStatus, TerminateThread, ResumeThread } kind = Read;
    uint64_t address = 0, size = 0;       // Allocate/Free/Protect: in and out (as the NT services have them); Read/Write: the address; CreateThread: the start routine; thread calls: the thread id
    ByteView out;                         // Read: where the bytes go
    ConstByteView in;                     // Write: the bytes
    uint64_t done = 0;                    // Read/Write: how many bytes were copied
    uint32_t type = 0, protect = 0, old_protect = 0;
    uint8_t info[48] = {};                // Query: MEMORY_BASIC_INFORMATION
    uint64_t argument = 0, stack_size = 0, thread_id = 0;      // CreateThread (thread_id comes back); ThreadStatus: exit code (STILL_ACTIVE if it runs) comes back in `size`
    bool suspended = false;
  };
  virtual uint32_t remote_operation(uint64_t pid, RemoteOp& op) { (void)pid; (void)op; return 0xC0000002u; }
  // The live threads and the loaded modules of the running program with this process id (the program first among the modules); each returns how many it wrote.
  struct ThreadEntry { uint64_t id = 0, start_address = 0; int32_t priority = 0; };
  struct ModuleEntry { uint64_t base = 0, size = 0, entry = 0; bool program = false; char name[40] = {}; };
  virtual uint32_t list_threads(uint64_t pid, ThreadEntry* out, uint32_t capacity) { (void)pid; (void)out; (void)capacity; return 0; }
  virtual uint32_t list_modules(uint64_t pid, ModuleEntry* out, uint32_t capacity) { (void)pid; (void)out; (void)capacity; return 0; }
  // Is there a running program with this process id (OpenProcess)?
  virtual bool process_exists(uint64_t pid) { (void)pid; return false; }

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
  // Optional: how many bytes of standard input could be read without waiting (a read would also return at once at the end of input or after Ctrl+C: *at_end).
  virtual uint64_t console_input_available(bool* at_end) { *at_end = false; return 0; }
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
  bool write_user_u32(uint64_t address, uint32_t value);

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
    uint64_t extra_tls[kMaxTlsModules] = {};          // blocks of libraries loaded after the thread began: [index] = base (each is one region of extra_tls_size[index] bytes)
    uint64_t extra_tls_size[kMaxTlsModules] = {};
    struct Apc { uint64_t routine = 0, a1 = 0, a2 = 0, a3 = 0; };      // an asynchronous procedure call waiting for the thread (QueueUserAPC, a timer, ReadFileEx)
    static constexpr uint32_t kMaxApcs = 32;
    Apc apcs[kMaxApcs];
    uint32_t apc_count = 0;
    uint64_t start_address = 0;                       // where the thread began (the start routine), 0 if not known
    int32_t priority = 0;                             // THREAD_PRIORITY_NORMAL; kept for the program to read back
    uint64_t teb = 0, region_base = 0, region_size = 0;      // the thread's TEB and the one region that holds its stack, TEB and TLS block
  };
  // What the program can set and read back about itself.
  uint32_t priority_class = 0x20;                     // NORMAL_PRIORITY_CLASS
  uint32_t error_mode = 0;
  uint64_t create_time = 0;                           // 100 ns units since 1601 (0: not known)
  void set_module_host(IModuleHost* host) { module_host_ = host; }
  IModuleHost* module_host() { return module_host_; }
  void attach_runtime(IProcessRuntime* runtime) { runtime_ = runtime; }
  IProcessRuntime* runtime() { return runtime_; }
  // The thread the process started with.
  void register_main_thread(uint64_t id, uint64_t teb);
  // The Windows startup data a new thread needs: the PEB, the image's static TLS template (see build_windows_thread), and the ntdll function new threads begin in.
  struct ThreadSupport {
    uint64_t peb = 0, start_thunk = 0;
    TlsTemplate tls[kMaxTlsModules];      // module index -> its template
    uint32_t tls_count = 0;
    bool tls_taken[kMaxTlsModules] = {};              // slots in use (the start-up modules' and those of libraries loaded since); a library's slot is free again when it is unloaded
    uint64_t stack_bytes = 1ull << 20;                // the default size of a new thread's stack
  };
  void set_thread_support(const ThreadSupport& support) { thread_support_ = support; }
  const ThreadSupport& thread_support() const { return thread_support_; }
  ThreadRecord* find_thread(uint64_t id);
  static bool queue_apc(ThreadRecord& t, const ThreadRecord::Apc& apc) {
    if (t.apc_count >= ThreadRecord::kMaxApcs) return false;
    t.apcs[t.apc_count++] = apc;
    return true;
  }
  static bool take_apc(ThreadRecord& t, ThreadRecord::Apc* out) {
    if (t.apc_count == 0) return false;
    *out = t.apcs[0];
    for (uint32_t i = 1; i < t.apc_count; ++i) t.apcs[i - 1] = t.apcs[i];
    --t.apc_count;
    return true;
  }
  // The threads that have started and not ended, for the process lists.
  uint32_t thread_snapshot(uint64_t* ids, uint64_t* starts, int32_t* priorities, uint32_t capacity) const;
  ThreadRecord* thread_at(uint32_t index) { return index < kMaxThreads ? &threads_[index] : nullptr; }
  ThreadSupport& thread_support_mutable() { return thread_support_; }
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
  // Lets go of every handle (what a process's end does to its table): the other side of a pipe sees the end, a mutex it owned is free. Only for when no thread of it runs.
  void release_handles();

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
  IModuleHost* module_host_ = nullptr;
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
