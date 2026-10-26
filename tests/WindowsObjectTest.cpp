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

// Host tests of the synchronization objects: events, mutexes and semaphores through the NT services, shared by name between two processes (each driven from a thread of its
// own, with a real lock and condition variable under the system's waiting), waits for any and for all, time-outs, abandoned mutexes, and handle duplication.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct SystemSync final : ISystemSync {
  std::mutex m;
  std::condition_variable cv;
  std::unique_lock<std::mutex> held{m, std::defer_lock};
  // (a lock held by the thread that calls lock(): one unique_lock per thread)
  static thread_local std::unique_lock<std::mutex>* mine;
  void lock() override { mine = new std::unique_lock<std::mutex>(m); }
  void unlock() override { delete mine; mine = nullptr; }
  bool wait(uint64_t ns) override {
    if (ns == ~uint64_t{0}) { cv.wait(*mine); return true; }
    return cv.wait_for(*mine, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout;
  }
  void notify_all() override { cv.notify_all(); }
};
thread_local std::unique_lock<std::mutex>* SystemSync::mine = nullptr;

// One page of "user memory" at kBase.
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
};
struct Host final : IProcessHost {
  std::atomic<uint64_t> now{0};
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
  void sleep_ns(uint64_t) override {}
  void yield() override {}
};

// A process with a memory of its own.
struct Prog {
  FakeMemory memory;
  Host host;
  Process process;
  const ISyscallInterface* iface = syscall_interface(SyscallAbi::Windows, UserArch::X86_64);
  explicit Prog(uint64_t id) : process(id, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, memory, host) {}

  // Puts OBJECT_ATTRIBUTES for `name` (a UTF-8 string; empty: none) at kBase + 0x400 and returns its address (0 for no name).
  uint64_t attributes(const std::string& name) {
    if (name.empty()) return 0;
    const std::string full = "\\BaseNamedObjects\\" + name;
    const uint64_t text = kBase + 0x800, unicode = kBase + 0x600, attr = kBase + 0x400;
    for (size_t i = 0; i < full.size(); ++i) { memory.bytes[text - kBase + 2 * i] = uint8_t(full[i]); memory.bytes[text - kBase + 2 * i + 1] = 0; }
    memory.put64(unicode, uint64_t(full.size() * 2) | (uint64_t(full.size() * 2 + 2) << 16));
    memory.put64(unicode + 8, text);
    memory.put64(attr, 0x30);
    memory.put64(attr + 8, 0);
    memory.put64(attr + 16, unicode);
    return attr;
  }
  uint32_t call(WindowsSyscallNumber n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0) {
    SyscallRequest r;
    r.number = uint64_t(n);
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3;
    const uint64_t sp = kBase + 0x1000;
    memory.put64(sp + 0x28, a4);
    memory.put64(sp + 0x30, a5);
    memory.put64(sp + 0x38, a6);
    r.user_stack = sp;
    return uint32_t(iface->dispatch(process, r));
  }
  uint64_t out() { return memory.get64(kBase + 0x100); }
  // The handle-making calls write the handle at kBase + 0x100.
  uint32_t make_event(const std::string& name, bool manual, bool initial, uint64_t* handle) {
    const uint32_t s = call(WindowsSyscallNumber::NtCreateEvent, kBase + 0x100, 0x1F0003, attributes(name), manual ? 0 : 1, initial ? 1 : 0);
    *handle = out();
    return s;
  }
  uint32_t make_mutex(const std::string& name, bool owned, uint64_t* handle) {
    const uint32_t s = call(WindowsSyscallNumber::NtCreateMutant, kBase + 0x100, 0x1F0001, attributes(name), owned ? 1 : 0);
    *handle = out();
    return s;
  }
  uint32_t make_semaphore(const std::string& name, int initial, int maximum, uint64_t* handle) {
    const uint32_t s = call(WindowsSyscallNumber::NtCreateSemaphore, kBase + 0x100, 0x1F0003, attributes(name), uint64_t(initial), uint64_t(maximum));
    *handle = out();
    return s;
  }
  // Waits with a timeout in milliseconds (-1: none).
  uint32_t wait(uint64_t handle, int64_t ms) {
    uint64_t timeout = 0;
    if (ms >= 0) { memory.put64(kBase + 0x200, uint64_t(0) - uint64_t(ms) * 10000); timeout = kBase + 0x200; }
    return call(WindowsSyscallNumber::NtWaitForSingleObject, handle, 0, timeout);
  }
  uint32_t wait_many(std::vector<uint64_t> handles, bool all, int64_t ms) {
    for (size_t i = 0; i < handles.size(); ++i) memory.put64(kBase + 0x300 + 8 * i, handles[i]);
    uint64_t timeout = 0;
    if (ms >= 0) { memory.put64(kBase + 0x200, uint64_t(0) - uint64_t(ms) * 10000); timeout = kBase + 0x200; }
    return call(WindowsSyscallNumber::NtWaitForMultipleObjects, handles.size(), kBase + 0x300, all ? 0 : 1, 0, timeout);
  }
};

static void test_events() {
  SystemSync sync;
  ObjectManager objects(sync);
  set_system_objects(&objects);
  {
    Prog p(1);
    uint64_t manual = 0, automatic = 0;
    CHECK(p.make_event("", true, false, &manual) == 0 && p.make_event("", false, true, &automatic) == 0 && manual != automatic);
    CHECK(p.wait(manual, 0) == 0x102);      // not set
    CHECK(p.call(WindowsSyscallNumber::NtSetEvent, manual, kBase + 0x500) == 0 && p.memory.get64(kBase + 0x500) == 0);
    CHECK(p.wait(manual, 0) == 0 && p.wait(manual, 0) == 0);      // manual reset: stays set
    CHECK(p.call(WindowsSyscallNumber::NtResetEvent, manual, kBase + 0x500) == 0 && p.memory.get64(kBase + 0x500) == 1 && p.wait(manual, 0) == 0x102);
    CHECK(p.wait(automatic, 0) == 0 && p.wait(automatic, 0) == 0x102);      // auto reset: one wait takes it
    CHECK(p.wait(automatic, 20) == 0x102);      // a real time-out
    CHECK(p.call(WindowsSyscallNumber::NtSetEvent, 0x7777) == 0xC0000008u);
    CHECK(p.call(WindowsSyscallNumber::NtSetEvent, automatic) == 0 && p.call(WindowsSyscallNumber::NtPulseEvent, manual) == 0 && p.wait(manual, 0) == 0x102);      // a pulse with nobody waiting
    CHECK(p.call(WindowsSyscallNumber::NtClose, manual) == 0 && p.call(WindowsSyscallNumber::NtSetEvent, manual) == 0xC0000008u);
    CHECK(p.make_event("x", true, false, &manual) == 0 && p.call(WindowsSyscallNumber::NtCreateEvent, kBase + 0x100, 0, 0, 5, 0) == 0xC000000Du);      // a bad type
  }
  set_system_objects(nullptr);
}

static void test_names_between_processes() {
  SystemSync sync;
  ObjectManager objects(sync);
  set_system_objects(&objects);
  {
    Prog a(1), b(2);
    uint64_t ha = 0, hb = 0, hc = 0;
    CHECK(a.make_event("Shared", true, false, &ha) == 0);
    CHECK(b.make_event("shared", true, true, &hb) == 0x40000000u);      // the name exists (names ignore case): the same object, the state of the first
    CHECK(a.wait(ha, 0) == 0x102 && b.wait(hb, 0) == 0x102);
    CHECK(a.call(WindowsSyscallNumber::NtSetEvent, ha) == 0 && b.wait(hb, 0) == 0);      // set by one, seen by the other
    CHECK(b.call(WindowsSyscallNumber::NtOpenEvent, kBase + 0x100, 0x1F0003, b.attributes("SHARED")) == 0);
    const uint64_t opened = b.out();
    CHECK(b.wait(opened, 0) == 0 && b.call(WindowsSyscallNumber::NtClose, opened) == 0);
    CHECK(b.call(WindowsSyscallNumber::NtOpenEvent, kBase + 0x100, 0x1F0003, b.attributes("nothere")) == 0xC0000034u);
    CHECK(b.call(WindowsSyscallNumber::NtOpenMutant, kBase + 0x100, 0x1F0001, b.attributes("Shared")) == 0xC0000024u);      // an event, not a mutex
    CHECK(b.make_mutex("Shared", false, &hc) == 0xC0000024u);
    // the object lives as long as one handle does
    CHECK(a.call(WindowsSyscallNumber::NtClose, ha) == 0);
    CHECK(b.wait(hb, 0) == 0);
    CHECK(b.call(WindowsSyscallNumber::NtClose, hb) == 0);
    uint64_t again = 0;
    CHECK(a.make_event("Shared", true, false, &again) == 0);      // gone with its last handle: a new object
    CHECK(a.wait(again, 0) == 0x102);
    // a long name
    CHECK(a.make_event(std::string(300, 'n'), true, false, &again) == 0xC0000106u);
  }
  set_system_objects(nullptr);
}

static void test_blocking_between_processes() {
  SystemSync sync;
  ObjectManager objects(sync);
  set_system_objects(&objects);
  {
    Prog a(1), b(2);
    uint64_t ha = 0, hb = 0;
    CHECK(a.make_event("Gate", true, false, &ha) == 0);
    CHECK(b.make_event("Gate", true, false, &hb) == 0x40000000u);
    std::atomic<uint32_t> status{0xFFFFFFFF};
    std::atomic<bool> started{false};
    std::thread waiter([&] { started = true; status = b.wait(hb, -1); });      // waits for ever
    while (!started) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(status == 0xFFFFFFFF);      // still waiting
    CHECK(a.call(WindowsSyscallNumber::NtSetEvent, ha) == 0);
    waiter.join();
    CHECK(status == 0);

    // a mutex: the second process waits until the first lets go; a recursive acquire
    uint64_t ma = 0, mb = 0;
    CHECK(a.make_mutex("Lock", true, &ma) == 0 && b.make_mutex("Lock", false, &mb) == 0x40000000u);
    CHECK(b.wait(mb, 0) == 0x102);      // owned by the other process
    CHECK(a.wait(ma, 0) == 0);          // (its owner may take it again)
    CHECK(a.call(WindowsSyscallNumber::NtReleaseMutant, ma, kBase + 0x500) == 0 && a.memory.get64(kBase + 0x500) == 2);
    status = 0xFFFFFFFF;
    started = false;
    std::thread locker([&] { started = true; status = b.wait(mb, 5000); });
    while (!started) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(status == 0xFFFFFFFF);
    CHECK(a.call(WindowsSyscallNumber::NtReleaseMutant, ma, 0) == 0);
    locker.join();
    CHECK(status == 0);      // b owns it now
    CHECK(a.call(WindowsSyscallNumber::NtReleaseMutant, ma, 0) == 0xC0000046u);      // not a's
    CHECK(a.wait(ma, 0) == 0x102);
    CHECK(b.call(WindowsSyscallNumber::NtReleaseMutant, mb, 0) == 0);
    CHECK(a.wait(ma, 0) == 0);
  }
  set_system_objects(nullptr);
}

static void test_abandoned() {
  SystemSync sync;
  ObjectManager objects(sync);
  set_system_objects(&objects);
  {
    Prog a(1), b(2);
    uint64_t ma = 0, mb = 0;
    CHECK(a.make_mutex("Held", true, &ma) == 0 && b.make_mutex("Held", false, &mb) == 0x40000000u);
    CHECK(b.wait(mb, 0) == 0x102);
    a.process.terminate(3);      // the owner's process ends without letting go
    CHECK(b.wait(mb, 0) == 0x80);      // WAIT_ABANDONED_0
    CHECK(b.call(WindowsSyscallNumber::NtReleaseMutant, mb, 0) == 0);
    CHECK(b.wait(mb, 0) == 0);      // the next owner sees an ordinary mutex
  }
  set_system_objects(nullptr);
}

static void test_semaphores_and_multiple() {
  SystemSync sync;
  ObjectManager objects(sync);
  set_system_objects(&objects);
  {
    Prog p(1);
    uint64_t sem = 0, ev1 = 0, ev2 = 0, mtx = 0;
    CHECK(p.make_semaphore("", 1, 3, &sem) == 0);
    CHECK(p.call(WindowsSyscallNumber::NtCreateSemaphore, kBase + 0x100, 0, 0, 5, 3) == 0xC000000Du);      // initial above maximum
    CHECK(p.wait(sem, 0) == 0 && p.wait(sem, 0) == 0x102);
    CHECK(p.call(WindowsSyscallNumber::NtReleaseSemaphore, sem, 2, kBase + 0x500) == 0 && p.memory.get64(kBase + 0x500) == 0);
    CHECK(p.call(WindowsSyscallNumber::NtReleaseSemaphore, sem, 2, 0) == 0xC0000047u);      // would pass the maximum
    CHECK(p.call(WindowsSyscallNumber::NtReleaseSemaphore, sem, 0, 0) == 0xC000000Du);
    CHECK(p.wait(sem, 0) == 0 && p.wait(sem, 0) == 0 && p.wait(sem, 0) == 0x102);
    // waiting for any
    CHECK(p.make_event("", true, false, &ev1) == 0 && p.make_event("", true, false, &ev2) == 0 && p.make_mutex("", false, &mtx) == 0);
    CHECK(p.wait_many({ev1, ev2}, false, 0) == 0x102);
    CHECK(p.call(WindowsSyscallNumber::NtSetEvent, ev2) == 0 && p.wait_many({ev1, ev2}, false, 0) == 1);
    CHECK(p.wait_many({ev1, ev2, mtx}, false, 0) == 1);      // the first that is ready
    // waiting for all takes nothing until all are ready
    CHECK(p.wait_many({ev1, ev2}, true, 0) == 0x102);
    CHECK(p.call(WindowsSyscallNumber::NtSetEvent, ev1) == 0 && p.wait_many({ev1, ev2, mtx}, true, 0) == 0);
    CHECK(p.wait(mtx, 0) == 0);      // it was taken by the wait: this thread owns it (recursively)
    CHECK(p.wait_many({}, false, 0) == 0xC000000Du);
    CHECK(p.wait_many({ev1, ev1}, false, 0) == 0xC000000Du);      // the same handle twice
    std::vector<uint64_t> many(65, ev1);
    CHECK(p.wait_many(many, false, 0) == 0xC000000Du);
  }
  set_system_objects(nullptr);
}

static void test_duplicate_and_close() {
  SystemSync sync;
  ObjectManager objects(sync);
  set_system_objects(&objects);
  {
    Prog p(1);
    uint64_t ev = 0;
    CHECK(p.make_event("Dup", true, true, &ev) == 0);
    // NtDuplicateObject(-1, handle, -1, &target, 0, 0, options): the options are the seventh argument (on the stack)
    CHECK(p.call(WindowsSyscallNumber::NtDuplicateObject, ~uint64_t{0}, ev, ~uint64_t{0}, kBase + 0x120, 0, 0, 0) == 0);
    const uint64_t copy = p.memory.get64(kBase + 0x120);
    CHECK(copy != 0 && copy != ev && p.wait(copy, 0) == 0);
    CHECK(p.call(WindowsSyscallNumber::NtClose, ev) == 0);
    CHECK(p.wait(copy, 0) == 0);      // the copy keeps the object
    CHECK(p.call(WindowsSyscallNumber::NtDuplicateObject, ~uint64_t{0}, copy, ~uint64_t{0}, kBase + 0x120, 0, 0, 1) == 0);      // close the source
    CHECK(p.wait(copy, 0) == 0xC0000008u);
    CHECK(p.wait(p.memory.get64(kBase + 0x120), 0) == 0);
    CHECK(p.call(WindowsSyscallNumber::NtDuplicateObject, 0x1234, copy, ~uint64_t{0}, kBase + 0x120, 0, 0, 0) == 0xC0000002u);      // another process
    // the pseudo handle of the calling thread becomes a real one (what thread libraries do to find themselves again); it needs a thread record
    CHECK(p.call(WindowsSyscallNumber::NtDuplicateObject, ~uint64_t{0}, ~uint64_t{1}, ~uint64_t{0}, kBase + 0x120, 0, 0, 0) == 0xC0000008u);
    p.process.register_main_thread(1, 0x7FFD0000);
    CHECK(p.call(WindowsSyscallNumber::NtDuplicateObject, ~uint64_t{0}, ~uint64_t{1}, ~uint64_t{0}, kBase + 0x120, 0, 0, 0) == 0);
    CHECK(p.process.files().kind(p.memory.get64(kBase + 0x120) / 4 - 1) == DescriptorKind::Thread && p.wait(p.memory.get64(kBase + 0x120), 0) == 0x102);      // (it has not ended)
    // a thread handle and a file are not synchronization objects, but a thread handle can be waited for
    CHECK(p.call(WindowsSyscallNumber::NtDuplicateObject, ~uint64_t{0}, 0x4444, ~uint64_t{0}, kBase + 0x120, 0, 0, 0) == 0xC0000008u);
  }
  // handles left open when a process goes away give the objects back
  {
    Prog p(2);
    uint64_t ev = 0;
    CHECK(p.make_event("Leak", true, false, &ev) == 0);
  }
  {
    Prog p(3);
    uint64_t ev = 0;
    CHECK(p.make_event("Leak", true, true, &ev) == 0);      // a new object (it would say the name exists if the first one was still there)
  }
  set_system_objects(nullptr);
}

static void test_many_objects() {
  SystemSync sync;
  ObjectManager objects(sync);
  set_system_objects(&objects);
  {
    Prog p(1);
    int made = 0;
    uint64_t last = 0;
    for (int i = 0; i < 1100; ++i) {
      uint64_t h = 0;
      if (p.make_event("", true, false, &h) == 0) { ++made; last = h; }
      else break;
    }
    CHECK(made > 0 && made <= int(ObjectManager::kCapacity) && made < 1100 && p.process.files().kind(0) != DescriptorKind::Closed);
    (void)last;
  }
  set_system_objects(nullptr);
}

int main() {
  test_events();
  test_names_between_processes();
  test_blocking_between_processes();
  test_abandoned();
  test_semaphores_and_multiple();
  test_duplicate_and_close();
  test_many_objects();
  if (g_failed) { printf("WindowsObjectTest: %d failed\n", int(g_failed)); return 1; }
  printf("WindowsObjectTest: all passed\n");
  return 0;
}
