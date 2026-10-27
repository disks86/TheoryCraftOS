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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Emulation/GuestCpu.hpp"
#include "Process/Process.hpp"
#include "Process/ModuleLoader.hpp"
#include "Process/UserProgram.hpp"
#include "Process/Objects.hpp"
#include "Process/ProgramEnvironment.hpp"
#include "Executable/ExecutableKind.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
using namespace emulation;

// Runs a Linux ELF or a Windows PE on the CPU emulator with real host threads, printing what the program writes to standard output and, last,
// "[exit=... faulted=... killed=... vector=... addr=... pc=...]".   Usage: EmulationRun <program> [<folder with the DLLs>]
// Used by aarch64_diff.sh and emulated_windows.sh. Gives up after 60 seconds.
struct A final : allocators::IAllocator { void* allocate(uint64_t s) override { return malloc(s); } void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); } void deallocate(void* p) override { free(p); } };
static uint64_t now() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
struct H final : process::IProcessHost {
  std::mutex m;
  bool write_console(uint32_t, ConstByteView d) override { std::lock_guard<std::mutex> g(m); fwrite(d.data(), 1, d.size(), stdout); fflush(stdout); return true; }
  bool read_console(ByteView, uint64_t* g) override { *g = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull + now(); } uint64_t monotonic_ns() override { return now(); }
  void sleep_ns(uint64_t ns) override { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); } void yield() override { std::this_thread::yield(); }
};
struct Sync final : process::ISystemSync {
  std::mutex m; std::condition_variable_any cv;
  void lock() override { m.lock(); } void unlock() override { m.unlock(); }
  bool wait(uint64_t ns) override { if (ns == ~uint64_t{0}) { cv.wait(m); return true; } return cv.wait_for(m, std::chrono::nanoseconds(ns)) != std::cv_status::timeout; }
  void notify_all() override { cv.notify_all(); } uint64_t now_ns() override { return now(); }
};
struct Threads final : IThreadSpawner {
  struct Hd { std::thread t; };
  void* spawn(void (*fn)(void*), void* a, const char*) override { return new Hd{std::thread(fn, a)}; }
  void join(void* h) override { static_cast<Hd*>(h)->t.join(); delete static_cast<Hd*>(h); }
  uint64_t self() override { return std::hash<std::thread::id>()(std::this_thread::get_id()); }
};
struct Ctl final : IRunControl { std::atomic<bool> stop{false}; bool stop_requested() override { return stop; } void yield() override { std::this_thread::yield(); } };
static std::vector<uint8_t> slurp(const std::string& p, bool* ok) { FILE* f = fopen(p.c_str(), "rb"); *ok = f; std::vector<uint8_t> b; if (!f) return b; uint8_t buf[4096]; size_t n; while ((n = fread(buf, 1, 4096, f)) > 0) b.insert(b.end(), buf, buf + n); fclose(f); return b; }
struct Libs final : process::ILibraryProvider {
  std::string dir; std::map<std::string, std::vector<uint8_t>> cache;
  bool find(StringView name, ConstByteView* image) override {
    std::string n(name.data(), name.size());
    auto it = cache.find(n);
    if (it == cache.end()) { bool ok; auto b = slurp(dir + "/" + n, &ok); if (!ok) return false; it = cache.emplace(n, b).first; }
    *image = ConstByteView(it->second.data(), it->second.size());
    return true;
  }
};
int main(int argc, char** argv) {
  bool ok; auto b = slurp(argv[1], &ok); if (!ok) { printf("cannot read\n"); return 2; }
  A a; H h; Libs libs; if (argc > 2) libs.dir = argv[2];
  filesystem::RamFileSystem fs(a);
  process::prepare_program_folders(fs);
  process::UserProgram p(a, 100, h);
  p.set_file_system(&fs);
  p.set_library_provider(&libs);
  p.set_processor_count(1);
  StringView av[] = {"C:\\test.exe"}; uint8_t rnd[16] = {};
  auto st = p.load(ConstByteView(b.data(), b.size()), ArrayView<const StringView>(av, 1), process::default_environment(process::abi_for_executable(exe::identify_executable(ConstByteView(b.data(), b.size()).at_most(exe::kIdentifyBytes)))), ConstByteView(rnd, 16));
  if (st != process::LoadStatus::Ok) { printf("load failed %d (%.*s)\n", (int)st, (int)p.load_detail().size(), p.load_detail().data()); return 2; }
  auto cpu = create_guest_cpu(p.start().arch, a);
  Sync sync; Threads th; Ctl ctl;
  Sync osync; process::ObjectManager objects(osync); process::set_system_objects(&objects);
  std::thread watchdog([&] { for (int i = 0; i < 600 && !ctl.stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100)); ctl.stop = true; });
  auto o = run_emulated_threads(p, *cpu.get(), ctl, sync, th, a, 20000);
  bool wasstop = ctl.stop; ctl.stop = true; watchdog.join();
  printf("[exit=%u faulted=%d killed=%d vector=%d addr=%llx pc=%llx]\n", o.exit_code, o.faulted, o.killed, o.fault.vector, (unsigned long long)o.fault.address, (unsigned long long)o.fault.pc);
}
