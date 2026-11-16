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
// fork(), wait and signals with their handlers, end to end: tests/linux/forktest.c and tests/linux/signaltest.c (freestanding Linux programs) are run on the
// emulator, where a forked child is a second emulated process on a host thread, and what each prints is compared with what the same program printed on a
// real Linux kernel: the x86-64 builds on the build machine's own, the AArch64 and RISC-V builds of signaltest.c (checked in) under qemu's user mode.
//   LinuxForkTest <forktest.elf> <its output> {<signaltest.elf> <its output>}...      (signaltest: the x86-64 build, and the AArch64 and RISC-V ones)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Emulation/GuestCpu.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/UnixSockets.hpp"
#include "Process/UserProgram.hpp"

using namespace emulation;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    return aligned_alloc(alignment < 16 ? 16 : alignment, (size + 15) & ~uint64_t{15});
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static TestAllocator A;

struct FakeHost final : process::IProcessHost {
  std::mutex m;      // (a child's output arrives from its own thread)
  std::string out;
  bool write_console(uint32_t, ConstByteView d) override {
    std::lock_guard<std::mutex> hold(m);
    for (uint8_t b : d) out.push_back(char(b));
    return true;
  }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull; }
  uint64_t monotonic_ns() override { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
  void sleep_ns(uint64_t ns) override { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
  void yield() override { std::this_thread::yield(); }
};

struct HostSync final : process::ISystemSync {
  std::mutex m;
  std::condition_variable_any cv;
  void lock() override { m.lock(); }
  void unlock() override { m.unlock(); }
  bool wait(uint64_t timeout_ns) override {
    if (timeout_ns == ~uint64_t{0}) { cv.wait(m); return true; }
    return cv.wait_for(m, std::chrono::nanoseconds(timeout_ns)) != std::cv_status::timeout;
  }
  void notify_all() override { cv.notify_all(); }
  uint64_t now_ns() override { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
};

struct HostThreads final : IThreadSpawner {
  struct Handle { std::thread t; };
  std::atomic<int> started{0};
  void* spawn(void (*fn)(void*), void* arg, const char*) override {
    ++started;
    return new Handle{std::thread(fn, arg)};
  }
  void join(void* handle) override {
    Handle* h = static_cast<Handle*>(handle);
    h->t.join();
    delete h;
  }
  uint64_t self() override { return uint64_t(std::hash<std::thread::id>()(std::this_thread::get_id())); }
};

struct Control final : IRunControl {
  std::atomic<uint64_t> yields{0};
  uint64_t limit = ~uint64_t{0};
  bool stop_requested() override { return yields.load() >= limit; }
  void yield() override { ++yields; std::this_thread::yield(); }
};

static std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> bytes;
  if (FILE* f = fopen(path, "rb")) {
    uint8_t chunk[4096];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
    fclose(f);
  }
  return bytes;
}

// Runs the program; returns what it (and its children) printed. `stop_after`: the run is told to stop after that many time slices (Ctrl+C).
static std::string run(const std::vector<uint8_t>& elf, uint64_t slice, EmulatedOutcome* outcome, int* threads_started, uint64_t stop_after = ~uint64_t{0}) {
  HostSync sync, system_sync;      // the lock the processes run under, and the one of the system's tables (never the same: a call takes the second inside the first)
  process::ObjectManager objects{system_sync};
  objects.set_allocator(&A);
  process::UnixSocketManager sockets{system_sync, A};
  process::set_system_objects(&objects);
  process::set_system_unix_sockets(&sockets);
  std::string printed;
  {
    FakeHost host;
    process::UserProgram program(A, 300, host);
    const StringView argv[] = {"/forktest"};
    const uint8_t random[16] = {};
    if (program.load(ConstByteView(elf.data(), elf.size()), ArrayView<const StringView>(argv, 1), ArrayView<const StringView>(), ConstByteView(random, 16)) == process::LoadStatus::Ok) {
      UniquePointer<IGuestCpu> cpu = create_guest_cpu(program.start().arch, A);
      HostThreads threads;
      Control control;
      control.limit = stop_after;
      *outcome = run_emulated_threads(program, *cpu.get(), control, sync, threads, A, slice);
      *threads_started = threads.started;
    } else {
      CHECK(false);
    }
    printed = host.out;
  }
  process::set_system_unix_sockets(nullptr);
  process::set_system_objects(nullptr);
  return printed;
}

// One program: its output on the emulator against what it printed on Linux, at several slice lengths (how often a guest thread is interrupted), and what is
// left when it is told to stop half-way.
static void test_program(const char* elf_path, const char* expected_path, int processes, const char* last_line) {
  const std::vector<uint8_t> elf = read_file(elf_path), expected = read_file(expected_path);
  CHECK(!elf.empty() && !expected.empty());
  const std::string want(expected.begin(), expected.end());
  CHECK(want.find(last_line) != std::string::npos);      // (the program ran to its end on Linux itself)
  const uint64_t slices[] = {20000, 997, 61};
  for (const uint64_t slice : slices) {
    EmulatedOutcome outcome;
    int started = 0;
    const std::string out = run(elf, slice, &outcome, &started);
    CHECK(outcome.exited && !outcome.faulted && !outcome.killed && outcome.exit_code == 0);
    if (outcome.faulted) printf("  faulted: vector %d address %llx pc %llx\n", outcome.fault.vector, (unsigned long long)outcome.fault.address, (unsigned long long)outcome.fault.pc);
    CHECK(started == processes);      // one host thread for each process the program (and its children) made
    if (started != processes) printf("  %s: %d threads started\n", elf_path, started);
    CHECK(out == want);
    if (out != want) {
      printf("--- %s, slice %llu: got\n%s--- want\n%s", elf_path, (unsigned long long)slice, out.c_str(), want.c_str());
      break;
    }
    CHECK(A.live == 0);      // every child's memory, CPU and descriptors went with it
  }
  {      // told to stop while children run or are waited for: everything ends, nothing is left behind
    EmulatedOutcome outcome;
    int started = 0;
    run(elf, 50, &outcome, &started, 40);
    CHECK(outcome.killed || outcome.exited);
    CHECK(A.live == 0);
  }
}

int main(int argc, char** argv) {
  if (argc < 5 || argc % 2 == 0) {
    printf("usage: LinuxForkTest <forktest.elf> <its output on Linux> {<signaltest.elf> <its output on Linux>}...\n");
    return 2;
  }
  test_program(argv[1], argv[2], 15, "fork ok\n");
  for (int i = 3; i + 1 < argc; i += 2) test_program(argv[i], argv[i + 1], 8, "signals ok\n");      // (the x86-64 build, then the AArch64 and RISC-V ones)
  if (g_failed) {
    printf("LinuxForkTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("LinuxForkTest: all passed\n");
  return 0;
}
