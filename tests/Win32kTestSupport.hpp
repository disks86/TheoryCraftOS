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
// Shared pieces of the win32k host tests (Win32kGdiTest, Win32kUserTest, ...): a system with an object manager and the win32k services installed, and programs with a
// memory of their own that make win32k system calls the way win32u.dll's stubs do (four register arguments, the rest on the stack at rsp + 0x28).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/Win32k/Win32k.hpp"
#include "Process/WindowsSyscalls.hpp"

namespace w32test {

using namespace process;

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { free(p); }
};

inline std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++w32test::g_failed; } } while (0)

struct SystemSync final : ISystemSync {
  std::mutex m;
  std::condition_variable cv;
  static inline thread_local std::unique_lock<std::mutex>* mine = nullptr;
  void lock() override { mine = new std::unique_lock<std::mutex>(m); }
  void unlock() override { delete mine; mine = nullptr; }
  bool wait(uint64_t ns) override {
    if (ns == ~uint64_t{0}) { cv.wait(*mine); return true; }
    return cv.wait_for(*mine, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout;
  }
  void notify_all() override { cv.notify_all(); }
  uint64_t now_ns() override { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
};

// 1 MiB of "user memory" at kBase; everything else faults. Layout used by Program: the stack arguments at kBase + 0x1000 (rsp), scratch space from kBase + 0x2000.
constexpr uint64_t kBase = 0x100000, kSize = 0x100000, kStack = kBase + 0x1000, kScratch = kBase + 0x2000;
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
  void put8(uint64_t a, uint8_t v) { bytes[a - kBase] = v; }
  void put16(uint64_t a, uint16_t v) { for (int i = 0; i < 2; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  void put32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  uint8_t get8(uint64_t a) { return bytes[a - kBase]; }
  uint16_t get16(uint64_t a) { return uint16_t(bytes[a - kBase] | bytes[a - kBase + 1] << 8); }
  uint32_t get32(uint64_t a) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t{bytes[a - kBase + i]} << (8 * i); return v; }
  uint64_t get64(uint64_t a) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{bytes[a - kBase + i]} << (8 * i); return v; }
  // UTF-16 text (ASCII input) with a terminating NUL; returns the address.
  uint64_t put_wide(uint64_t a, const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) put16(a + 2 * i, uint8_t(text[i]));
    put16(a + 2 * text.size(), 0);
    return a;
  }
  std::string get_wide(uint64_t a, uint32_t chars) { std::string s; for (uint32_t i = 0; i < chars; ++i) s.push_back(char(get16(a + 2 * i))); return s; }
  std::string get_wide_z(uint64_t a) { std::string s; for (uint32_t i = 0; get16(a + 2 * i) != 0; ++i) s.push_back(char(get16(a + 2 * i))); return s; }
  // A UNICODE_STRING at `us` for the text at `chars`.
  uint64_t put_unicode_string(uint64_t us, uint64_t chars, const std::string& text) {
    put_wide(chars, text);
    put16(us, uint16_t(text.size() * 2)); put16(us + 2, uint16_t(text.size() * 2 + 2)); put64(us + 8, chars);
    return us;
  }
};

struct Host final : IProcessHost {
  uint64_t fake_now = 1;      // monotonic_ns() counts real time plus this offset (tests move time with advance())
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull; }
  uint64_t monotonic_ns() override { return fake_now + uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
  void sleep_ns(uint64_t) override {}
  void yield() override {}
  void advance(uint64_t ns) { fake_now += ns; }
};

// The system: an object manager (and with it the system lock) with win32k installed. One per test function; the destructor takes it down again.
struct System {
  TestAllocator allocator;
  SystemSync sync;
  ObjectManager objects{sync};
  System() {
    objects.set_allocator(&allocator);
    set_system_objects(&objects);
    win32k::install();
  }
  ~System() { set_system_objects(nullptr); }
};

// A Windows program with a memory of its own.
struct Program {
  FakeMemory memory;
  Host host;
  Process process;
  const ISyscallInterface* iface = syscall_interface(SyscallAbi::Windows, UserArch::X86_64);
  explicit Program(uint64_t pid) : process(pid, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, memory, host) {}

  // Makes a win32k system call (any number of arguments up to 16) and returns RAX.
  uint64_t call(WindowsSyscallNumber n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0,
                uint64_t a8 = 0, uint64_t a9 = 0, uint64_t a10 = 0, uint64_t a11 = 0, uint64_t a12 = 0, uint64_t a13 = 0, uint64_t a14 = 0, uint64_t a15 = 0) {
    SyscallRequest r;
    r.number = uint64_t(n);
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3;
    const uint64_t rest[12] = {a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15};
    for (uint32_t i = 0; i < 12; ++i) memory.put64(kStack + 0x28 + 8 * i, rest[i]);
    r.user_stack = kStack;
    return iface->dispatch(process, r);
  }
  uint32_t call32(WindowsSyscallNumber n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0, uint64_t a7 = 0) {
    return uint32_t(call(n, a0, a1, a2, a3, a4, a5, a6, a7));
  }
};

inline int finish(const char* name) {
  if (g_failed) { printf("%s: %d check(s) failed\n", name, g_failed.load()); return 1; }
  printf("%s: ok\n", name);
  return 0;
}

}  // namespace w32test
