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

// Host tests of the thread services of the Windows interface: NtCreateThreadEx (stack, TEB and TLS block of the new thread, the thread record, its handle), suspended
// threads, ending a thread, waiting for one, and the table filling up.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsException.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { free(p); }
};
static TestAllocator A;

// The program's stack for the system call arguments and a place for results: [kBase, kBase + kSize).
constexpr uint64_t kBase = 0x100000, kSize = 0x4000;
struct FakeMemory final : IUserMemory {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(kSize, 0);
  bool read(uint64_t a, ByteView out) override {
    if (a < kBase || a - kBase > bytes.size() || out.size() > bytes.size() - (a - kBase)) return false;
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = bytes[a - kBase + i];
    return true;
  }
  bool write(uint64_t a, ConstByteView in) override {
    if (a < kBase || a - kBase > bytes.size() || in.size() > bytes.size() - (a - kBase)) return false;
    for (uint64_t i = 0; i < in.size(); ++i) bytes[a - kBase + i] = in[i];
    return true;
  }
  void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  uint64_t get64(uint64_t a) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{bytes[a - kBase + i]} << (8 * i); return v; }
  uint32_t get32(uint64_t a) { return uint32_t(get64(a)); }
};
struct Host final : IProcessHost {
  uint64_t now = 0;
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override { return now += 1000000; }      // (every look at the clock lets a millisecond pass: nothing else runs to make it)
  void sleep_ns(uint64_t ns) override { now += ns; }
  void yield() override {}
};
struct Runtime final : IProcessRuntime {
  std::vector<ThreadStart> started;
  std::vector<bool> suspended;
  std::vector<uint64_t> resumed;
  uint64_t current = 0;
  bool fail_start = false;
  uint32_t notifications = 0;
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t) override { return false; }      // nothing else runs in a host test: waiting times out
  void notify() override { ++notifications; }
  bool interrupted() override { return false; }
  bool start_thread(const ThreadStart& s, bool susp) override { if (fail_start) return false; started.push_back(s); suspended.push_back(susp); return true; }
  bool resume_thread(uint64_t id) override { resumed.push_back(id); return true; }
  uint64_t current_thread_id() override { return current; }
  std::vector<uint64_t> suspended_ids, resumed_ids, terminated_ids;
  std::vector<uint32_t> terminate_codes;
  bool parked = false;
  X86Context context;
  bool context_set = false;
  void request_suspend(uint64_t id) override { suspended_ids.push_back(id); parked = true; }
  void request_resume(uint64_t id) override { resumed_ids.push_back(id); parked = false; }
  bool is_parked(uint64_t) override { return parked; }
  bool get_context(uint64_t, X86Context* out) override { if (!parked) return false; *out = context; return true; }
  bool set_context(uint64_t, const X86Context& c) override { if (!parked) return false; context = c; context_set = true; return true; }
  void request_terminate(uint64_t id, uint32_t code) override { terminated_ids.push_back(id); terminate_codes.push_back(code); }
  // CreateProcess
  struct Spawned { uint64_t pid; std::string image_path, command_line, directory; std::vector<std::string> environment; uint64_t image_size; bool suspended; uint8_t first_byte; };
  std::vector<Spawned> spawned;
  std::vector<uint64_t> terminated_pids, resumed_pids;
  std::vector<uint32_t> process_codes;
  uint32_t spawn_status = 0;
  uint64_t next_pid = 0x40000;
  uint64_t allocate_process_id() override { return next_pid++; }
  uint32_t spawn_process(const ChildSpec& spec) override {
    if (spawn_status != 0) return spawn_status;
    Spawned s{spec.pid, std::string(spec.image_path.data(), spec.image_path.size()), std::string(spec.command_line.data(), spec.command_line.size()),
              std::string(spec.working_directory.data(), spec.working_directory.size()), {}, spec.image.size(), spec.suspended, spec.image.empty() ? uint8_t(0) : spec.image[0]};
    for (const StringView& e : spec.environment) s.environment.push_back(std::string(e.data(), e.size()));
    spawned.push_back(s);
    return 0;
  }
  bool terminate_process(uint64_t pid, uint32_t code) override { terminated_pids.push_back(pid); process_codes.push_back(code); return true; }
  bool resume_process(uint64_t pid) override { resumed_pids.push_back(pid); return true; }
  // the process lists
  uint32_t list_processes(ProcessEntry* out, uint32_t capacity) override {
    uint32_t n = 0;
    for (uint64_t pid : {uint64_t(7), uint64_t(8)}) {
      if (n >= capacity) break;
      out[n] = ProcessEntry();
      out[n].pid = pid; out[n].parent = pid == 8 ? 7 : 0; out[n].threads = pid == 7 ? 2 : 1; out[n].memory_bytes = 4096 * pid; out[n].create_time = 1000 + pid;
      const char* name = pid == 7 ? "main.exe" : "child.exe";
      for (uint32_t i = 0; name[i]; ++i) out[n].name[i] = name[i];
      ++n;
    }
    return n;
  }
  uint32_t list_threads(uint64_t pid, ThreadEntry* out, uint32_t capacity) override {
    const uint32_t count = pid == 7 ? 2 : pid == 8 ? 1 : 0;
    for (uint32_t i = 0; i < count && i < capacity; ++i) { out[i] = ThreadEntry(); out[i].id = (uint64_t(i + 1) << 20) | pid; out[i].start_address = 0x1000 * (i + 1); out[i].priority = int32_t(i); }
    return count < capacity ? count : capacity;
  }
  uint32_t list_modules(uint64_t pid, ModuleEntry* out, uint32_t capacity) override {
    if (pid != 7 || capacity < 2) return 0;
    out[0] = ModuleEntry(); out[0].base = 0x140000000; out[0].size = 0x2000; out[0].entry = 0x140001000; out[0].program = true; for (int i = 0; "main.exe"[i]; ++i) out[0].name[i] = "main.exe"[i];
    out[1] = ModuleEntry(); out[1].base = 0x180000000; out[1].size = 0x9000; for (int i = 0; "ntdll.dll"[i]; ++i) out[1].name[i] = "ntdll.dll"[i];
    return 2;
  }
};

// One thread drives these tests: the system's lock needs no real locking and a wait just lets time pass.
struct SingleThreadSync final : ISystemSync {
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t) override { return false; }
  void notify_all() override {}
};

struct Setup {
  SingleThreadSync sync;
  ObjectManager objects{sync};
  FakeMemory memory;
  Host host;
  AddressSpace space{A};
  Runtime runtime;
  Process process{7, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, memory, host};
  const ISyscallInterface* iface = syscall_interface(SyscallAbi::Windows, UserArch::X86_64);
  Setup() {
    set_system_objects(&objects);
    process.attach_address_space(space, 0x10000000);
    process.attach_runtime(&runtime);
    Process::ThreadSupport support;
    support.peb = 0x7FFE0000AA00ull;
    support.start_thunk = 0x180001234;
    support.stack_bytes = 1 << 20;
    process.set_thread_support(support);
    process.register_main_thread(7, 0x7FFD0000);
    runtime.current = 7;
  }
  ~Setup() { set_system_objects(nullptr); }
  // NtCreateThreadEx(&handle, access, NULL, -1, start, parameter, flags, 0, stack, stack, NULL): the arguments past the fourth go on the stack, past 32 bytes of home space.
  uint32_t create(uint64_t start, uint64_t parameter, uint64_t flags, uint64_t stack, uint64_t* handle) {
    const uint64_t sp = kBase + 0x1000;
    const uint64_t args[7] = {start, parameter, flags, 0, stack, stack, 0};
    for (int i = 0; i < 7; ++i) memory.put64(sp + 0x28 + 8 * i, args[i]);
    SyscallRequest r;
    r.number = uint64_t(WindowsSyscallNumber::NtCreateThreadEx);
    r.args[0] = kBase + 0x100;
    r.args[1] = 0x1FFFFF;
    r.args[2] = 0;
    r.args[3] = ~uint64_t{0};
    r.user_stack = sp;
    const uint32_t status = uint32_t(iface->dispatch(process, r));
    *handle = memory.get64(kBase + 0x100);
    return status;
  }
  uint32_t call(WindowsSyscallNumber n, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0) {
    SyscallRequest r;
    r.number = uint64_t(n);
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3;
    const uint64_t sp = kBase + 0x1000;
    memory.put64(sp + 0x28, a4);
    r.user_stack = sp;
    return uint32_t(iface->dispatch(process, r));
  }
};

static void test_create() {
  Setup s;
  uint64_t handle = 0;
  CHECK(s.create(0x401000, 0xCAFE, 0, 0, &handle) == 0);
  CHECK(handle != 0 && handle % 4 == 0);
  CHECK(s.runtime.started.size() == 1 && !s.runtime.suspended[0]);
  const ThreadStart& t = s.runtime.started[0];
  CHECK(t.entry == 0x180001234 && t.argument == 0x401000 && t.argument2 == 0xCAFE);
  CHECK(t.gs_base != 0 && t.stack_pointer < t.gs_base && t.stack_pointer > t.gs_base - (1 << 20) && (t.stack_pointer & 7) == 0);
  CHECK(t.id != 7 && (t.id & 0xFFFFF) == 7);
  // the TEB in the new region: stack bounds, ClientId, Self, PEB
  uint8_t teb[0x1500];
  CHECK(s.space.read_privileged(t.gs_base, ByteView(teb, sizeof(teb))));
  auto get64 = [&](uint32_t at) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{teb[at + i]} << (8 * i); return v; };
  CHECK(get64(0x08) == t.gs_base && get64(0x10) == t.gs_base - (1 << 20) && get64(0x30) == t.gs_base && get64(0x40) == 7 && get64(0x48) == t.id && get64(0x60) == 0x7FFE0000AA00ull);
  CHECK(s.process.live_threads() == 2);
  // the stack is mapped and writable
  CHECK(s.space.write_privileged(t.stack_pointer - 8, ConstByteView(teb, 8)));
  // the handle names the thread
  CHECK(s.process.files().kind(handle / 4 - 1) == DescriptorKind::Thread && s.process.files().token(handle / 4 - 1) == t.id);
}

static void test_arguments() {
  Setup s;
  uint64_t handle = 0;
  CHECK(s.create(0, 1, 0, 0, &handle) == 0xC000000Du);      // no start routine
  CHECK(s.create(0xFFFF800000000000ull, 1, 0, 0, &handle) == 0xC000000Du);
  CHECK(s.create(0x401000, 1, 0, 1ull << 40, &handle) == 0xC000009Au);      // a stack too big
  CHECK(s.runtime.started.empty() && s.process.live_threads() == 1);
  CHECK(s.space.mapped_bytes() == 0);      // nothing was left mapped by the failures
  s.runtime.fail_start = true;
  CHECK(s.create(0x401000, 1, 0, 0, &handle) == 0xC000009Au);
  CHECK(s.space.mapped_bytes() == 0 && s.process.live_threads() == 1);      // the region is given back
  s.runtime.fail_start = false;
  // a process of one thread (no runtime)
  Setup single;
  single.process.attach_runtime(nullptr);
  CHECK(single.create(0x401000, 1, 0, 0, &handle) == 0xC0000002u);
  // another process's handle
  SyscallRequest r;
  r.number = uint64_t(WindowsSyscallNumber::NtCreateThreadEx);
  r.args[3] = 0x1234;
  r.user_stack = kBase + 0x1000;
  CHECK(uint32_t(s.iface->dispatch(s.process, r)) == 0xC0000008u);
}

static void test_suspended_resume_wait_terminate() {
  Setup s;
  uint64_t handle = 0;
  CHECK(s.create(0x401000, 1, 1, 0, &handle) == 0);      // CREATE_SUSPENDED
  CHECK(s.runtime.suspended.size() == 1 && s.runtime.suspended[0]);
  const uint64_t id = s.runtime.started[0].id;
  // NtResumeThread
  CHECK(s.call(WindowsSyscallNumber::NtResumeThread, handle, kBase + 0x200) == 0 && s.memory.get32(kBase + 0x200) == 1 && s.runtime.resumed.size() == 1 && s.runtime.resumed[0] == id);
  CHECK(s.call(WindowsSyscallNumber::NtResumeThread, handle, kBase + 0x200) == 0 && s.memory.get32(kBase + 0x200) == 0 && s.runtime.resumed.size() == 1);      // already running
  // NtQueryInformationThread: still active, its TEB and ids
  s.memory.put64(kBase + 0x1000 + 0x28, 0);
  CHECK(s.call(WindowsSyscallNumber::NtQueryInformationThread, handle, 0, kBase + 0x300, 48, 0) == 0);
  CHECK(s.memory.get64(kBase + 0x300) == 259 && s.memory.get64(kBase + 0x310) == 7 && s.memory.get64(kBase + 0x318) == id);
  CHECK(s.call(WindowsSyscallNumber::NtQueryInformationThread, handle, 0, kBase + 0x300, 8, 0) == 0xC0000004u);      // the buffer is too small
  CHECK(s.call(WindowsSyscallNumber::NtQueryInformationThread, handle, 5, kBase + 0x300, 48, 0) == 0xC0000003u);      // not a class this OS has
  // waiting for it: nothing else runs in a host test, so a wait with a timeout times out
  s.memory.put64(kBase + 0x400, uint64_t(0) - 50000);      // -5 ms in 100 ns units
  CHECK(s.call(WindowsSyscallNumber::NtWaitForSingleObject, handle, 0, kBase + 0x400) == 0x102u);
  // the thread ends itself: the calling thread must be the one named
  s.runtime.current = id;
  { uint32_t ignored; s.process.take_thread_exit(&ignored); }
  CHECK(s.call(WindowsSyscallNumber::NtTerminateThread, ~uint64_t{1}, 33) == 0);
  uint32_t code = 0;
  CHECK(s.process.take_thread_exit(&code) && code == 33);
  CHECK(s.process.live_threads() == 1 && !s.process.exited());
  CHECK(s.space.mapped_bytes() == 0);      // its region is unmapped
  s.runtime.current = 7;
  CHECK(s.call(WindowsSyscallNumber::NtWaitForSingleObject, handle, 0, 0) == 0);      // now the wait is over
  CHECK(s.call(WindowsSyscallNumber::NtQueryInformationThread, handle, 0, kBase + 0x300, 48, 0) == 0 && s.memory.get64(kBase + 0x300) == 33);
  // the last thread ends the process, with its code
  CHECK(s.call(WindowsSyscallNumber::NtTerminateThread, ~uint64_t{1}, 5) == 0 && s.process.exited() && s.process.exit_code() == 5);
  // one thread cannot end another
  Setup t2;
  uint64_t h2 = 0;
  CHECK(t2.create(0x401000, 1, 0, 0, &h2) == 0);
  CHECK(t2.call(WindowsSyscallNumber::NtTerminateThread, h2, 1) == 0 && t2.runtime.terminated_ids.size() == 1 && t2.runtime.terminate_codes[0] == 1);      // told to end
  CHECK(t2.call(WindowsSyscallNumber::NtTerminateThread, 0x9999, 1) == 0xC0000008u);
  // waiting for something that is not a thread
  CHECK(t2.call(WindowsSyscallNumber::NtWaitForSingleObject, 4 * 63, 0, 0) == 0xC0000008u);
  CHECK(t2.call(WindowsSyscallNumber::NtWaitForSingleObject, 0, 0, 0) == 0xC0000008u);
}

static void test_suspend_and_context() {
  Setup s;
  uint64_t handle = 0;
  CHECK(s.create(0x401000, 5, 0, 0, &handle) == 0);
  const uint64_t id = s.runtime.started[0].id;
  // SuspendThread counts; only the first stops the thread, only the last lets it go
  CHECK(s.call(WindowsSyscallNumber::NtSuspendThread, handle, kBase + 0x200) == 0 && s.memory.get64(kBase + 0x200) == 0 && s.runtime.suspended_ids.size() == 1);
  CHECK(s.call(WindowsSyscallNumber::NtSuspendThread, handle, kBase + 0x200) == 0 && s.memory.get64(kBase + 0x200) == 1 && s.runtime.suspended_ids.size() == 1);
  // GetThreadContext of the stopped thread; SetThreadContext changes it
  s.runtime.context.rip = 0x401234;
  s.runtime.context.gpr[X86Context::kRsp] = 0x7FFF1000;
  s.runtime.context.gpr[X86Context::kRax] = 0x77;
  const uint64_t context_at = kBase + 0x800;
  for (uint32_t i = 0; i < kContextBytes; ++i) s.memory.bytes[context_at - kBase + i] = 0;
  s.memory.put64(context_at + 0x30, 0x10000B);      // CONTEXT_FULL
  CHECK(s.call(WindowsSyscallNumber::NtGetContextThread, handle, context_at) == 0);
  CHECK(s.memory.get64(context_at + 0xF8) == 0x401234 && s.memory.get64(context_at + 0x98) == 0x7FFF1000 && s.memory.get64(context_at + 0x78) == 0x77);
  s.memory.put64(context_at + 0xF8, 0x402000);
  s.memory.put64(context_at + 0x78, 0x99);
  CHECK(s.call(WindowsSyscallNumber::NtSetContextThread, handle, context_at) == 0 && s.runtime.context_set && s.runtime.context.rip == 0x402000 && s.runtime.context.gpr[X86Context::kRax] == 0x99);
  s.memory.put64(context_at + 0xF8, 0xFFFF800000000000ull);      // a rip the kernel must not accept
  CHECK(s.call(WindowsSyscallNumber::NtSetContextThread, handle, context_at) == 0xC0000005u);
  s.memory.put64(context_at + 0x30, 0x10000B & ~0x100000u);      // not an x64 CONTEXT
  CHECK(s.call(WindowsSyscallNumber::NtGetContextThread, handle, context_at) == 0xC0000005u);
  // a thread that runs, or the caller itself, gives no registers
  CHECK(s.call(WindowsSyscallNumber::NtResumeThread, handle, kBase + 0x200) == 0 && s.memory.get64(kBase + 0x200) == 2 && s.runtime.resumed_ids.empty());
  CHECK(s.call(WindowsSyscallNumber::NtResumeThread, handle, kBase + 0x200) == 0 && s.memory.get64(kBase + 0x200) == 1 && s.runtime.resumed_ids.size() == 1 && s.runtime.resumed_ids[0] == id);
  s.memory.put64(context_at + 0x30, 0x10000B);
  CHECK(s.call(WindowsSyscallNumber::NtGetContextThread, handle, context_at) == 0xC000000Du);      // not suspended
  CHECK(s.call(WindowsSyscallNumber::NtResumeThread, handle, kBase + 0x200) == 0 && s.memory.get64(kBase + 0x200) == 0);      // (nothing to undo)
  s.runtime.current = id;
  CHECK(s.call(WindowsSyscallNumber::NtSuspendThread, ~uint64_t{1}, 0) == 0);      // a thread may suspend itself
  CHECK(s.call(WindowsSyscallNumber::NtGetContextThread, ~uint64_t{1}, context_at) == 0xC000000Du);
  s.runtime.current = 7;
  // a thread created suspended can be given its registers before it runs (the runtime keeps them); one that ended cannot be suspended
  CHECK(s.call(WindowsSyscallNumber::NtTerminateThread, handle, 4) == 0);      // (told to end)
  s.process.find_thread(id)->finished = true;
  CHECK(s.call(WindowsSyscallNumber::NtSuspendThread, handle, 0) == 0xC000004Bu);
  CHECK(s.call(WindowsSyscallNumber::NtTerminateThread, handle, 4) == 0);      // ending an ended thread is not an error
  // a thread that never ran is ended on the spot
  uint64_t idle = 0;
  CHECK(s.create(0x401000, 0, 1, 0, &idle) == 0);
  const uint64_t idle_id = s.runtime.started.back().id;
  const size_t told = s.runtime.terminated_ids.size();
  CHECK(s.call(WindowsSyscallNumber::NtTerminateThread, idle, 6) == 0 && s.runtime.terminated_ids.size() == told && s.process.find_thread(idle_id)->finished);
  CHECK(s.process.find_thread(idle_id)->exit_code == 6);
}

static void test_many_threads() {
  Setup s;
  // More threads than the table has slots, one after the other: ended threads whose handles are closed make room.
  for (int i = 0; i < 200; ++i) {
    uint64_t handle = 0;
    CHECK(s.create(0x401000, uint64_t(i), 0, 0, &handle) == 0);
    if (g_failed) return;
    const uint64_t id = s.runtime.started.back().id;
    s.runtime.current = id;
    CHECK(s.call(WindowsSyscallNumber::NtTerminateThread, ~uint64_t{1}, uint64_t(i)) == 0);
    uint32_t code;
    s.process.take_thread_exit(&code);
    s.runtime.current = 7;
    CHECK(s.process.close_descriptor(handle / 4 - 1));
  }
  CHECK(s.space.mapped_bytes() == 0 && s.process.live_threads() == 1);
  // with the handles kept open the table does fill up
  int made = 0;
  for (int i = 0; i < 100; ++i) { uint64_t h; if (s.create(0x401000, 0, 0, 0, &h) == 0) ++made; }
  CHECK(made < 64 && made > 40);
}

static void test_terminate_process_wakes() {
  Setup s;
  const uint32_t before = s.runtime.notifications;
  s.process.terminate(9);
  CHECK(s.runtime.notifications == before + 1 && s.process.exited() && s.process.exit_code() == 9);
  s.process.terminate(10);      // the first ending counts
  CHECK(s.process.exit_code() == 9);
}

// CreateProcess: the image is read from the file system, the strings arrive as UTF-8, the handles can be waited for and asked, and TerminateProcess / ResumeThread reach the runtime.
static void put_text16(FakeMemory& m, uint64_t structure, uint64_t buffer, const char* text) {
  uint64_t n = 0;
  for (; text[n]; ++n) { m.bytes[buffer - kBase + 2 * n] = uint8_t(text[n]); m.bytes[buffer - kBase + 2 * n + 1] = 0; }
  m.bytes[structure - kBase] = uint8_t(2 * n);
  m.bytes[structure - kBase + 1] = 0;
  m.bytes[structure - kBase + 2] = uint8_t(2 * n + 2);
  m.put64(structure + 8, buffer);
}

static void test_create_process() {
  filesystem::RamFileSystem fs(A);
  prepare_program_folders(fs);
  {
    UniquePointer<filesystem::IFile> f;
    CHECK(fs.open_file("/Windows/child.exe", filesystem::OpenMode::CreateTruncate, &f) == filesystem::Status::Ok);
    const uint8_t bytes[5] = {'M', 'Z', 1, 2, 3};
    uint64_t n = 0;
    CHECK(f->write(0, ConstByteView(bytes, 5), &n) == filesystem::Status::Ok);
  }
  Setup s;
  s.process.attach_file_system(A, fs);
  const uint64_t sp = kBase + 0x1000;
  put_text16(s.memory, kBase + 0x1200, kBase + 0x1300, "C:\\child.exe");
  put_text16(s.memory, kBase + 0x1220, kBase + 0x1400, "child.exe -x \"a b\"");
  put_text16(s.memory, kBase + 0x1240, kBase + 0x1500, "C:\\");
  const char* env[] = {"PATH=C:\\", "A=b"};
  uint64_t at = kBase + 0x1800;
  for (const char* e : env) {
    for (uint64_t i = 0; e[i]; ++i, at += 2) { s.memory.bytes[at - kBase] = uint8_t(e[i]); s.memory.bytes[at - kBase + 1] = 0; }
    at += 2;
  }
  auto create = [&](uint64_t flags, uint64_t image_struct) {
    const uint64_t args[5] = {kBase + 0x1240, kBase + 0x1800, flags, kBase + 0x110, kBase + 0x114};
    for (int i = 0; i < 5; ++i) s.memory.put64(sp + 0x28 + 8 * i, args[i]);
    SyscallRequest r;
    r.number = uint64_t(WindowsSyscallNumber::NtCreateUserProcess);
    r.args[0] = kBase + 0x100;
    r.args[1] = kBase + 0x108;
    r.args[2] = image_struct;
    r.args[3] = kBase + 0x1220;
    r.user_stack = sp;
    return uint32_t(s.iface->dispatch(s.process, r));
  };
  CHECK(create(1, kBase + 0x1200) == 0);
  CHECK(s.runtime.spawned.size() == 1);
  const Runtime::Spawned& c = s.runtime.spawned[0];
  CHECK(c.pid == 0x40000 && c.image_path == "C:\\child.exe" && c.command_line == "child.exe -x \"a b\"" && c.directory == "/");
  CHECK(c.image_size == 5 && c.first_byte == 'M' && c.suspended);
  CHECK(c.environment.size() == 2 && c.environment[0] == "PATH=C:\\" && c.environment[1] == "A=b");
  const uint64_t process = s.memory.get64(kBase + 0x100), thread = s.memory.get64(kBase + 0x108);
  CHECK(process != 0 && thread != 0 && process != thread);
  CHECK(uint32_t(s.memory.get64(kBase + 0x110)) == 0x40000 && uint32_t(s.memory.get64(kBase + 0x114)) == 0x40000);
  // still running
  auto status_of = [&]() {
    CHECK(s.call(WindowsSyscallNumber::NtQueryInformationProcess, process, 0, kBase + 0x200, 48, kBase + 0x2F0) == 0);
    return uint32_t(s.memory.get64(kBase + 0x200));
  };
  CHECK(status_of() == 259);
  CHECK(uint32_t(s.memory.get64(kBase + 0x200 + 32)) == 0x40000);
  s.memory.put64(kBase + 0x300, 0);      // a timeout of 0: look and return
  CHECK(s.call(WindowsSyscallNumber::NtWaitForSingleObject, process, 0, kBase + 0x300) == 0x102);
  // resume (through the thread handle), terminate (through the process handle)
  CHECK(s.call(WindowsSyscallNumber::NtResumeThread, thread, 0) == 0);
  CHECK(s.runtime.resumed_pids.size() == 1 && s.runtime.resumed_pids[0] == 0x40000);
  CHECK(s.call(WindowsSyscallNumber::NtTerminateProcess, process, 77) == 0);
  CHECK(s.runtime.terminated_pids.size() == 1 && s.runtime.process_codes[0] == 77);
  // it ends: the handle is signalled and the code can be read
  s.objects.process_exited(0x40000, 77);
  CHECK(s.call(WindowsSyscallNumber::NtWaitForSingleObject, process, 0, kBase + 0x300) == 0);
  CHECK(s.call(WindowsSyscallNumber::NtWaitForSingleObject, thread, 0, kBase + 0x300) == 0);
  CHECK(status_of() == 77);
  CHECK(s.call(WindowsSyscallNumber::NtClose, process) == 0 && s.call(WindowsSyscallNumber::NtClose, thread) == 0);
  CHECK(s.call(WindowsSyscallNumber::NtQueryInformationProcess, process, 0, kBase + 0x200, 48, 0) == 0xC0000008u);
  // a missing file, a failing runtime
  put_text16(s.memory, kBase + 0x1260, kBase + 0x1600, "C:\\missing.exe");
  CHECK(create(0, kBase + 0x1260) == 0xC0000034u);
  s.runtime.spawn_status = 0xC000007Bu;
  CHECK(create(0, kBase + 0x1200) == 0xC000007Bu);
  s.runtime.spawn_status = 0;
  // (failures leave no handle behind: the table still takes a whole run of handles)
  for (int i = 0; i < 20; ++i) {
    CHECK(create(0, kBase + 0x1200) == 0);
    CHECK(s.call(WindowsSyscallNumber::NtClose, s.memory.get64(kBase + 0x100)) == 0 && s.call(WindowsSyscallNumber::NtClose, s.memory.get64(kBase + 0x108)) == 0);
  }
}

// NtQuerySystemInformation: the process list (class 5, with the threads) and the module list (this OS's class 0x80).
static void test_process_lists() {
  Setup s;
  const uint64_t out = kBase + 0x2000, returned = kBase + 0x1F00;
  CHECK(s.call(WindowsSyscallNumber::NtQuerySystemInformation, 5, out, 16, returned) == 0xC0000004u);      // too small: it says how much
  const uint32_t need = s.memory.get32(returned);
  CHECK(need == (0x100 + 2 * 0x50 + 56) + (0x100 + 1 * 0x50 + 56));
  CHECK(s.call(WindowsSyscallNumber::NtQuerySystemInformation, 5, out, need, returned) == 0);
  const uint32_t first = uint32_t(0x100 + 2 * 0x50 + 56);
  CHECK(s.memory.get32(out) == first && s.memory.get32(out + 4) == 2 && s.memory.get64(out + 0x50) == 7 && s.memory.get64(out + 0x58) == 0);
  CHECK(s.memory.get64(out + 0x20) == 1007 && s.memory.get64(out + 0x90) == 4096 * 7 && (s.memory.get32(out + 0x38) & 0xFFFF) == 16);      // CreateTime, WorkingSetSize, "main.exe" (8 characters)
  CHECK(s.memory.get64(out + 0x30 + 0x100) == ((uint64_t(1) << 20) | 7) && s.memory.get64(out + 0x28 + 0x100) == 7 && s.memory.get64(out + 0x20 + 0x100) == 0x1000);      // the first thread
  CHECK(s.memory.get64(out + 0x30 + 0x150) == ((uint64_t(2) << 20) | 7) && s.memory.get32(out + 0x38 + 0x150) == 9);      // the second: priority 8 + 1
  CHECK(s.memory.get32(out + first) == 0 && s.memory.get64(out + first + 0x50) == 8 && s.memory.get64(out + first + 0x58) == 7);      // the child, the last entry
  const uint64_t name_at = s.memory.get64(out + 0x40);
  CHECK(name_at == out + 0x100 + 2 * 0x50 && s.memory.bytes[name_at - kBase] == 'm' && s.memory.bytes[name_at - kBase + 1] == 0);
  // modules of process 7 (the id is the first 8 bytes of the buffer; 0 is this process)
  s.memory.put64(out, 0);
  CHECK(s.call(WindowsSyscallNumber::NtQuerySystemInformation, 0x80, out, 8 + 2 * 112, returned) == 0);
  CHECK(s.memory.get32(out) == 2 && s.memory.get32(out + 4) == 112 && s.memory.get32(returned) == 8 + 2 * 112);
  CHECK(s.memory.get64(out + 8) == 0x140000000 && s.memory.get64(out + 16) == 0x2000 && s.memory.get64(out + 24) == 0x140001000 && s.memory.get32(out + 32) == 1 && s.memory.get32(out + 36) == 8);
  CHECK(s.memory.get64(out + 8 + 112) == 0x180000000 && s.memory.get32(out + 8 + 112 + 24) == 0 && s.memory.get32(out + 8 + 112 + 28) == 9);
  s.memory.put64(out, 7);
  CHECK(s.call(WindowsSyscallNumber::NtQuerySystemInformation, 0x80, out, 8, returned) == 0xC0000004u && s.memory.get32(returned) == 8 + 2 * 112);
  s.memory.put64(out, 99);
  CHECK(s.call(WindowsSyscallNumber::NtQuerySystemInformation, 0x80, out, 8 + 2 * 112, returned) == 0 && s.memory.get32(out) == 0);      // no such process: an empty list
}

int main() {
  test_process_lists();
  test_create();
  test_arguments();
  test_suspended_resume_wait_terminate();
  test_suspend_and_context();
  test_many_threads();
  test_terminate_process_wakes();
  test_create_process();
  if (g_failed) { printf("WindowsThreadTest: %d failed\n", g_failed); return 1; }
  printf("WindowsThreadTest: all passed\n");
  return 0;
}
