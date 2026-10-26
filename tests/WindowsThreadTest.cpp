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
#include <vector>
#include "Process/AddressSpace.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
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
  uint64_t monotonic_ns() override { return now; }
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
};

struct Setup {
  FakeMemory memory;
  Host host;
  AddressSpace space{A};
  Runtime runtime;
  Process process{7, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, memory, host};
  const ISyscallInterface* iface = syscall_interface(SyscallAbi::Windows, UserArch::X86_64);
  Setup() {
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
  CHECK(t2.call(WindowsSyscallNumber::NtTerminateThread, h2, 1) == 0xC0000002u);
  CHECK(t2.call(WindowsSyscallNumber::NtTerminateThread, 0x9999, 1) == 0xC0000008u);
  // waiting for something that is not a thread
  CHECK(t2.call(WindowsSyscallNumber::NtWaitForSingleObject, 4 * 63, 0, 0) == 0xC0000008u);
  CHECK(t2.call(WindowsSyscallNumber::NtWaitForSingleObject, 0, 0, 0) == 0xC0000008u);
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

int main() {
  test_create();
  test_arguments();
  test_suspended_resume_wait_terminate();
  test_many_threads();
  test_terminate_process_wakes();
  if (g_failed) { printf("WindowsThreadTest: %d failed\n", g_failed); return 1; }
  printf("WindowsThreadTest: all passed\n");
  return 0;
}
