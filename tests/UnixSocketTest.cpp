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

// Host tests of AF_UNIX sockets (src/Process/UnixSockets.*, LinuxSockets.*, WindowsUnixSockets.*): the sockaddr_un codec (truncation and bit flips), the Linux
// system calls, the Windows socket services, and - the point of the design - Linux and Windows processes talking to each other over the same sockets, by path
// (C:\... is /mnt/c/...) and by abstract name, from separate threads that block on each other.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/UnixSocketAddress.hpp"
#include "Process/UnixSockets.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;
using filesystem::Status;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static TestAllocator A;

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// The system's one lock and wake-up, as the kernel gives it: a mutex and a condition variable.
struct SystemSync final : ISystemSync {
  std::mutex m;
  std::condition_variable cv;
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

constexpr uint64_t kBase = 0x100000, kSize = 0x400000;
struct FakeMemory final : IUserMemory {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(kSize, 0);
  bool read(uint64_t a, ByteView out) override {
    if (a < kBase || a - kBase > bytes.size() || out.size() > bytes.size() - (a - kBase)) return false;
    memcpy(out.data(), &bytes[a - kBase], out.size());
    return true;
  }
  bool write(uint64_t a, ConstByteView in) override {
    if (a < kBase || a - kBase > bytes.size() || in.size() > bytes.size() - (a - kBase)) return false;
    memcpy(&bytes[a - kBase], in.data(), in.size());
    return true;
  }
  uint8_t* at(uint64_t a) { return &bytes[a - kBase]; }
  void put(uint64_t a, const void* data, size_t n) { memcpy(at(a), data, n); }
  void put32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  uint32_t get32(uint64_t a) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t{bytes[a - kBase + i]} << (8 * i); return v; }
  uint64_t get64(uint64_t a) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{bytes[a - kBase + i]} << (8 * i); return v; }
  std::string str(uint64_t a, size_t n) { return std::string(reinterpret_cast<const char*>(at(a)), n); }
};

struct Host final : IProcessHost {
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  void sleep_ns(uint64_t ns) override { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
  void yield() override { std::this_thread::yield(); }
};
struct Runtime final : IProcessRuntime {
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t) override { return false; }
  void notify() override {}
  bool interrupted() override { return false; }
  bool start_thread(const ThreadStart&, bool) override { return false; }
  bool resume_thread(uint64_t) override { return false; }
  uint64_t current_thread_id() override { return 1; }
};

// Linux x86-64 numbers.
enum : uint64_t { kRead = 0, kWrite = 1, kClose = 3, kStat = 4, kFstat = 5, kPoll = 7, kIoctl = 16, kReadv = 19, kWritev = 20, kSocket = 41, kConnect = 42,
                  kAccept = 43, kSendto = 44, kRecvfrom = 45, kSendmsg = 46, kRecvmsg = 47, kShutdown = 48, kBind = 49, kListen = 50, kGetsockname = 51,
                  kGetpeername = 52, kSocketpair = 53, kSetsockopt = 54, kGetsockopt = 55, kFcntl = 72, kUnlink = 87, kOpen = 2, kDup = 32, kAccept4 = 288 };
constexpr int64_t kEnoent = 2, kEbadf = 9, kEagain = 11, kEinval = 22, kEpipe = 32, kEnotsock = 88, kEprototype = 91, kEafnosupport = 97, kEaddrinuse = 98,
                  kEisconn = 106, kEnotconn = 107, kEconnrefused = 111, kEconnreset = 104;
constexpr uint32_t kStatusWouldBlock = 0xC0000103u, kStatusRefused = 0xC0000236u, kStatusAddressInUse = 0xC000020Au;

// One process: its memory, its file table and a scratch area to build arguments in.
struct Proc {
  FakeMemory memory;
  Host host;
  Runtime runtime;
  Process process;
  SyscallAbi abi;
  std::atomic<uint64_t> cursor{kBase + 0x1000};      // (threads of one process take scratch memory too)
  Proc(uint64_t pid, SyscallAbi a, filesystem::IFileSystem* fs)
      : process(pid, ProcessBinding{a, UserArch::X86_64, syscall_interface(a, UserArch::X86_64)}, memory, host), abi(a) {
    process.attach_runtime(&runtime);
    if (fs) process.attach_file_system(A, *fs);
  }
  ~Proc() { process.release_handles(); }
  int64_t sys(uint64_t n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) {
    SyscallRequest r;
    r.number = n;
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.args[4] = a4; r.args[5] = a5;
    return int64_t(process.syscall(r));
  }
  uint32_t nt(WindowsSyscallNumber n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0,
              uint64_t a7 = 0) {
    SyscallRequest r;
    r.number = uint64_t(n);
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3;
    const uint64_t sp = kBase + 0x200;      // (the arguments past the fourth are on the stack, after the 0x28 bytes of shadow space and return address)
    memory.put64(sp + 0x28, a4); memory.put64(sp + 0x30, a5); memory.put64(sp + 0x38, a6); memory.put64(sp + 0x40, a7);
    r.user_stack = sp;
    return uint32_t(process.syscall(r));
  }
  uint64_t alloc(uint64_t n) {
    const uint64_t at = cursor.fetch_add((n + 15) & ~uint64_t{15});
    if (at + n > kBase + kSize) { printf("test process out of scratch memory\n"); abort(); }
    return at;
  }
  uint64_t text(const std::string& s) { const uint64_t at = alloc(s.size() + 1); memory.put(at, s.c_str(), s.size() + 1); return at; }
  uint64_t data(const std::string& s) { const uint64_t at = alloc(s.size() ? s.size() : 1); memory.put(at, s.data(), s.size()); return at; }
  // A sockaddr_un: the family, then the path and its NUL (an abstract name starts with '\0' in `path` and gets no NUL after it). *length is its length.
  uint64_t unix_address(const std::string& path, uint32_t* length) {
    const uint64_t at = alloc(kSockaddrUnSize);
    memset(memory.at(at), 0, kSockaddrUnSize);
    memory.at(at)[0] = 1;
    memcpy(memory.at(at) + 2, path.data(), path.size());
    *length = uint32_t(2 + path.size() + (path.empty() || path[0] != 0 ? 1 : 0));
    return at;
  }
  // --- Linux helpers
  int64_t lsocket(uint64_t type) { return sys(kSocket, 1, type, 0); }
  int64_t lbind(int64_t fd, const std::string& path) { uint32_t n; const uint64_t a = unix_address(path, &n); return sys(kBind, fd, a, n); }
  int64_t lconnect(int64_t fd, const std::string& path) { uint32_t n; const uint64_t a = unix_address(path, &n); return sys(kConnect, fd, a, n); }
  int64_t lsend(int64_t fd, const std::string& s, uint64_t flags = 0) { return sys(kSendto, fd, data(s), s.size(), flags, 0, 0); }
  std::string lrecv(int64_t fd, uint64_t n, int64_t* result, uint64_t flags = 0) {
    const uint64_t at = alloc(n);
    *result = sys(kRecvfrom, fd, at, n, flags, 0, 0);
    return *result > 0 ? memory.str(at, size_t(*result)) : std::string();
  }
  // The name getsockname (51) / getpeername (52) reports: the path (or "\0..." for an abstract one), "" for unnamed; *length its reported size.
  std::string lname(uint64_t call, int64_t fd, int64_t* result, uint32_t* length = nullptr) {
    const uint64_t at = alloc(kSockaddrUnSize), len_at = alloc(4);
    memory.put32(len_at, kSockaddrUnSize);
    *result = sys(call, fd, at, len_at);
    const uint32_t n = memory.get32(len_at);
    if (length) *length = n;
    if (*result != 0 || n <= 2) return std::string();
    std::string s = memory.str(at + 2, n - 2);
    if (!s.empty() && s[0] != 0 && s.back() == 0) s.pop_back();
    return s;
  }
  // --- Windows helpers (handles are 64-bit values written to memory)
  uint32_t wsocket(uint64_t* handle, uint32_t type = 1) {
    const uint64_t at = alloc(8);
    const uint32_t st = nt(WindowsSyscallNumber::NtCreateSocket, 1, type, 0, at);
    *handle = memory.get64(at);
    return st;
  }
  uint32_t wbind(uint64_t h, const std::string& path) { uint32_t n; const uint64_t a = unix_address(path, &n); return nt(WindowsSyscallNumber::NtBindSocket, h, a, n); }
  uint32_t wconnect(uint64_t h, const std::string& path) { uint32_t n; const uint64_t a = unix_address(path, &n); return nt(WindowsSyscallNumber::NtConnectSocket, h, a, n); }
  uint32_t wlisten(uint64_t h, uint32_t backlog = 5) { return nt(WindowsSyscallNumber::NtListenSocket, h, backlog); }
  uint32_t waccept(uint64_t h, uint64_t* out, std::string* peer = nullptr) {
    const uint64_t at = alloc(8), peer_at = alloc(kSockaddrUnSize), len_at = alloc(4);
    memory.put32(len_at, kSockaddrUnSize);
    const uint32_t st = nt(WindowsSyscallNumber::NtAcceptSocket, h, at, peer_at, len_at);
    *out = memory.get64(at);
    if (peer && st == 0) { const uint32_t n = memory.get32(len_at); *peer = n > 2 ? memory.str(peer_at + 2, n - 2) : std::string(); }
    return st;
  }
  uint32_t wsend(uint64_t h, const std::string& s, uint32_t* sent) {
    const uint64_t sent_at = alloc(4);
    const uint32_t st = nt(WindowsSyscallNumber::NtSendSocket, h, data(s), s.size(), sent_at, 0, 0);
    *sent = memory.get32(sent_at);
    return st;
  }
  uint32_t wrecv(uint64_t h, uint32_t capacity, std::string* out, bool* closed = nullptr) {
    const uint64_t at = alloc(capacity), got_at = alloc(4), closed_at = alloc(1);
    const uint32_t st = nt(WindowsSyscallNumber::NtReceiveSocket, h, at, capacity, got_at, 0, 0, closed_at);
    if (st == 0) *out = memory.str(at, memory.get32(got_at));
    if (closed) *closed = memory.at(closed_at)[0] != 0;
    return st;
  }
  uint32_t wcontrol(uint64_t h, uint32_t op, uint64_t in, uint32_t in_length, uint64_t out, uint32_t out_length) {
    return nt(WindowsSyscallNumber::NtSocketControl, h, op, in, in_length, out, out_length);
  }
  uint32_t wclose(uint64_t h) { return nt(WindowsSyscallNumber::NtClose, h); }
};

// The trees both kinds of program see, in a RAM file system.
static void populate(filesystem::IFileSystem& fs) {
  for (const char* d : {"/Linux", "/Linux/tmp", "/Linux/run", "/Windows", "/Windows/Users", "/Windows/Users/Public", "/Windows/Temp"}) CHECK(fs.create_directory(d) == Status::Ok);
}

// Each test runs with a fresh system: objects (for their lock), a unix socket manager and a file system.
struct System {
  SystemSync sync;
  ObjectManager objects{sync};
  UnixSocketManager sockets{sync, A};
  filesystem::RamFileSystem fs{A};
  System() {
    objects.set_allocator(&A);
    set_system_objects(&objects);
    set_system_unix_sockets(&sockets);
    populate(fs);
  }
  ~System() {
    set_system_unix_sockets(nullptr);
    set_system_objects(nullptr);
  }
};

// --- the address codec ------------------------------------------------------------------------------------------------------------------------------------------

static void test_codec() {
  uint8_t b[kSockaddrUnSize + 4] = {};
  UnixName n;
  // A path: up to its NUL or the length.
  b[0] = 1; memcpy(b + 2, "/tmp/a.sock", 12);
  CHECK(parse_unix_address(ConstByteView(b, 14), &n) == UnixAddressResult::Ok && n.kind == UnixName::Kind::Path && n.text() == StringView("/tmp/a.sock"));
  CHECK(parse_unix_address(ConstByteView(b, 13), &n) == UnixAddressResult::Ok && n.text() == StringView("/tmp/a.sock"));      // (no NUL in the length)
  CHECK(parse_unix_address(ConstByteView(b, 7), &n) == UnixAddressResult::Ok && n.text() == StringView("/tmp/"));
  CHECK(parse_unix_address(ConstByteView(b, 2), &n) == UnixAddressResult::Ok && n.kind == UnixName::Kind::Unnamed);
  for (uint32_t cut = 0; cut < 2; ++cut) CHECK(parse_unix_address(ConstByteView(b, cut), &n) == UnixAddressResult::Invalid);
  CHECK(parse_unix_address(ConstByteView(b, kSockaddrUnSize + 1), &n) == UnixAddressResult::Invalid);      // (longer than a sockaddr_un)
  b[0] = 2;
  CHECK(parse_unix_address(ConstByteView(b, 14), &n) == UnixAddressResult::WrongFamily);
  b[0] = 1;
  // An abstract name: every byte after the first NUL, NULs too.
  uint8_t a[8] = {1, 0, 0, 'x', 0, 'y', 0, 0};
  CHECK(parse_unix_address(ConstByteView(a, 8), &n) == UnixAddressResult::Ok && n.kind == UnixName::Kind::Abstract && n.length == 5 && n.bytes[0] == 'x' && n.bytes[1] == 0);
  CHECK(parse_unix_address(ConstByteView(a, 3), &n) == UnixAddressResult::Ok && n.kind == UnixName::Kind::Abstract && n.length == 0);
  // Writing: the full size is reported, a short buffer gets the start of it.
  UnixName p = UnixName::from(UnixName::Kind::Path, ConstByteView(reinterpret_cast<const uint8_t*>("/run/x"), 6));
  uint8_t out[kSockaddrUnSize + 1];
  CHECK(unix_address_size(p) == 9 && write_unix_address(ByteView(out), p) == 9 && out[0] == 1 && out[1] == 0 && out[2] == '/' && out[8] == 0);
  for (uint32_t cap = 0; cap < 9; ++cap) { memset(out, 0xEE, sizeof(out)); CHECK(write_unix_address(ByteView(out, cap), p) == cap && out[cap] == 0xEE); }
  UnixName ab = UnixName::from(UnixName::Kind::Abstract, ConstByteView(a + 3, 3));
  CHECK(unix_address_size(ab) == 6 && write_unix_address(ByteView(out), ab) == 6 && out[2] == 0 && out[3] == 'x');
  CHECK(unix_address_size(UnixName()) == 2);
  // A path of all 108 bytes (no room for a NUL) round-trips.
  uint8_t full[kSockaddrUnSize];
  full[0] = 1; full[1] = 0;
  for (uint32_t i = 2; i < kSockaddrUnSize; ++i) full[i] = uint8_t('a' + i % 26);
  CHECK(parse_unix_address(ConstByteView(full, kSockaddrUnSize), &n) == UnixAddressResult::Ok && n.length == kUnixPathMax);
  CHECK(write_unix_address(ByteView(out), n) == kSockaddrUnSize + 1);
  // Bit flips of every byte of a valid address: parsed or refused, never read past the bytes given (ASan watches), and what parses writes back within bounds.
  uint8_t base[16] = {1, 0, '/', 't', 'm', 'p', '/', 's', 0, 'z', 'z', 'z', 'z', 'z', 'z', 'z'};
  for (uint32_t i = 0; i < sizeof(base); ++i) {
    for (uint32_t bit = 0; bit < 8; ++bit) {
      uint8_t flipped[16];
      memcpy(flipped, base, sizeof(base));
      flipped[i] ^= uint8_t(1u << bit);
      for (uint32_t cut = 0; cut <= sizeof(flipped); ++cut) {
        std::vector<uint8_t> exact(flipped, flipped + cut);      // (exactly `cut` bytes on the heap: ASan catches a read past them)
        UnixName got;
        const UnixAddressResult r = parse_unix_address(ConstByteView(exact.data(), exact.size()), &got);
        if (r == UnixAddressResult::Ok) { CHECK(got.length <= kUnixPathMax); CHECK(write_unix_address(ByteView(out), got) == unix_address_size(got)); }
      }
    }
  }
}

// --- Linux ------------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_linux_pair() {
  System sys;
  Proc l(100, SyscallAbi::Linux, &sys.fs);
  // socketpair(AF_UNIX, SOCK_STREAM): bytes both ways, read()/write() work on it, shutdown gives the other end EOF, close gives EPIPE.
  const uint64_t pair = l.alloc(8);
  CHECK(l.sys(kSocketpair, 1, 1, 0, pair) == 0);
  const int64_t a = l.memory.get32(pair), b = l.memory.get32(pair + 4);
  CHECK(a >= 3 && b >= 3 && a != b);
  CHECK(l.sys(kWrite, a, l.data("hello"), 5) == 5);
  CHECK(l.lsend(a, " world") == 6);
  int64_t r = 0;
  CHECK(l.lrecv(b, 64, &r) == "hello world" && r == 11);
  CHECK(l.lsend(b, "back") == 4);
  const uint64_t rb = l.alloc(16);
  CHECK(l.sys(kRead, a, rb, 16) == 4 && l.memory.str(rb, 4) == "back");
  // Peek leaves it; FIONREAD counts it.
  CHECK(l.lsend(a, "peek") == 4);
  CHECK(l.lrecv(b, 2, &r, 2 /* MSG_PEEK */) == "pe");
  const uint64_t count_at = l.alloc(4);
  CHECK(l.sys(kIoctl, b, 0x541B, count_at) == 0 && l.memory.get32(count_at) == 4);
  CHECK(l.lrecv(b, 16, &r) == "peek");
  // Non-blocking: nothing there is EAGAIN (MSG_DONTWAIT, and O_NONBLOCK through fcntl).
  l.lrecv(b, 16, &r, 0x40);
  CHECK(r == -kEagain);
  CHECK(l.sys(kFcntl, b, 4, 0x800) == 0 && (l.sys(kFcntl, b, 3) & 0x800) != 0);
  l.lrecv(b, 16, &r);
  CHECK(r == -kEagain);
  // fstat: a socket.
  const uint64_t st = l.alloc(144);
  CHECK(l.sys(kFstat, a, st) == 0 && (l.memory.get32(st + 24) & 0170000) == 0140000);
  // SO_TYPE, SO_PEERCRED.
  const uint64_t value = l.alloc(16), len_at = l.alloc(4);
  l.memory.put32(len_at, 4);
  CHECK(l.sys(kGetsockopt, a, 1, 3, value, len_at) == 0 && l.memory.get32(value) == 1);
  l.memory.put32(len_at, 12);
  CHECK(l.sys(kGetsockopt, a, 1, 17, value, len_at) == 0 && l.memory.get32(value) == 100 && l.memory.get32(len_at) == 12);
  // shutdown(SHUT_WR) on a: b reads EOF; a can still read what b sends.
  CHECK(l.sys(kShutdown, a, 1) == 0);
  CHECK(l.sys(kFcntl, b, 4, 0) == 0);
  l.lrecv(b, 16, &r);
  CHECK(r == 0);
  CHECK(l.lsend(a, "x") == -kEpipe);
  CHECK(l.lsend(b, "still") == 5 && l.lrecv(a, 16, &r) == "still");
  // close(b): a's reads give EOF, writes EPIPE.
  CHECK(l.sys(kClose, b) == 0);
  l.lrecv(a, 16, &r);
  CHECK(r == 0);
  // Not a socket / bad descriptor.
  CHECK(l.sys(kListen, 1, 1) == -kEnotsock);
  CHECK(l.sys(kListen, 900, 1) == -kEbadf);
  CHECK(l.sys(kSocket, 2, 1, 0) == -kEafnosupport);
  CHECK(l.sys(kClose, a) == 0);
}

static void test_linux_messages() {
  System sys;
  Proc l(101, SyscallAbi::Linux, &sys.fs);
  const uint64_t pair = l.alloc(8);
  // SOCK_DGRAM pair: each send is one message; a short buffer cuts it (MSG_TRUNC reports the full size).
  CHECK(l.sys(kSocketpair, 1, 2, 0, pair) == 0);
  int64_t a = l.memory.get32(pair), b = l.memory.get32(pair + 4), r = 0;
  CHECK(l.lsend(a, "one") == 3 && l.lsend(a, "two!") == 4 && l.lsend(a, "") == 0);
  CHECK(l.lrecv(b, 64, &r) == "one");
  CHECK(l.lrecv(b, 2, &r, 0x20 /* MSG_TRUNC */).substr(0, 2) == "tw" && r == 4);      // (the full size is the answer; two bytes were copied)
  CHECK(l.lrecv(b, 64, &r) == "" && r == 0);      // (the empty message)
  CHECK(l.sys(kClose, a) == 0 && l.sys(kClose, b) == 0);
  // SOCK_SEQPACKET: messages over a connection; recvmsg reports MSG_TRUNC in msg_flags.
  CHECK(l.sys(kSocketpair, 1, 5, 0, pair) == 0);
  a = l.memory.get32(pair); b = l.memory.get32(pair + 4);
  CHECK(l.lsend(a, "packet-one") == 10);
  const uint64_t buf = l.alloc(4), iov = l.alloc(16), msg = l.alloc(56);
  l.memory.put64(iov, buf); l.memory.put64(iov + 8, 4);
  memset(l.memory.at(msg), 0, 56);
  l.memory.put64(msg + 16, iov); l.memory.put64(msg + 24, 1);
  CHECK(l.sys(kRecvmsg, b, msg, 0) == 4 && l.memory.str(buf, 4) == "pack" && (l.memory.get32(msg + 48) & 0x20) != 0);
  // writev / readv on a socket: one message.
  const uint64_t v = l.alloc(32);
  l.memory.put64(v, l.data("ab")); l.memory.put64(v + 8, 2); l.memory.put64(v + 16, l.data("cde")); l.memory.put64(v + 24, 3);
  CHECK(l.sys(kWritev, a, v, 2) == 5);
  CHECK(l.lrecv(b, 64, &r) == "abcde");
  CHECK(l.sys(kClose, a) == 0);
  l.lrecv(b, 64, &r);
  CHECK(r == 0);      // (EOF: the other end went)
  CHECK(l.sys(kClose, b) == 0);
}

static void test_linux_named() {
  System sys;
  Proc l(102, SyscallAbi::Linux, &sys.fs);
  const int64_t server = l.lsocket(1);
  CHECK(server >= 0);
  CHECK(l.lbind(server, "/tmp/server.sock") == 0);
  filesystem::DirectoryEntry e;
  CHECK(sys.fs.stat("/Linux/tmp/server.sock", &e) == Status::Ok);      // (the socket's file)
  const uint64_t st = l.alloc(144);
  CHECK(l.sys(kStat, l.text("/tmp/server.sock"), st) == 0 && (l.memory.get32(st + 24) & 0170000) == 0140000);
  CHECK(l.lbind(server, "/tmp/other.sock") == -kEinval);      // (already bound)
  const int64_t client = l.lsocket(1);
  CHECK(l.lconnect(client, "/tmp/server.sock") == -kEconnrefused);      // (bound, not listening)
  CHECK(l.sys(kListen, server, 4) == 0);
  CHECK(l.lconnect(client, "/tmp/missing.sock") == -kEnoent);
  // Relative to the current directory.
  CHECK(l.sys(80 /* chdir */, l.text("/tmp")) == 0);
  CHECK(l.lconnect(client, "server.sock") == 0);
  CHECK(l.lconnect(client, "server.sock") == -kEisconn);
  const int64_t accepted = l.sys(kAccept4, server, 0, 0, 0x80000);
  CHECK(accepted >= 0);
  int64_t r = 0;
  CHECK(l.lname(kGetsockname, server, &r) == "/tmp/server.sock" && r == 0);
  CHECK(l.lname(kGetpeername, client, &r) == "/tmp/server.sock" && r == 0);
  uint32_t length = 0;
  CHECK(l.lname(kGetsockname, client, &r, &length) == "" && r == 0 && length == 2);      // (unnamed)
  CHECK(l.lname(kGetpeername, accepted, &r, &length) == "" && length == 2);
  CHECK(l.lname(kGetpeername, server, &r) == "" && r == -kEnotconn);
  CHECK(l.lsend(client, "ping") == 4 && l.lrecv(accepted, 16, &r) == "ping");
  // A second socket cannot take the name while the file is there; once it is unlinked it can.
  const int64_t again = l.lsocket(1);
  CHECK(l.lbind(again, "/tmp/server.sock") == -kEaddrinuse);
  CHECK(l.sys(kUnlink, l.text("/tmp/server.sock")) == 0);
  CHECK(l.lbind(again, "/tmp/server.sock") == 0 && l.sys(kListen, again, 1) == 0);
  const int64_t late = l.lsocket(1);
  CHECK(l.lconnect(late, "/tmp/server.sock") == 0);      // (the new one)
  const uint64_t fds = l.alloc(16);
  l.memory.put32(fds, uint32_t(again)); l.memory.put32(fds + 4, 1);      // events POLLIN
  l.memory.put32(fds + 8, uint32_t(server)); l.memory.put32(fds + 12, 1);
  CHECK(l.sys(kPoll, fds, 2, 0) == 1 && (l.memory.get32(fds + 4) >> 16) == 1 && (l.memory.get32(fds + 12) >> 16) == 0);
  // A listener closed with a connection it never accepted: the client sees a reset.
  CHECK(l.sys(kClose, again) == 0);
  l.lrecv(late, 8, &r);
  CHECK(r == -kEconnreset);
  // Abstract names: no file; autobind picks one.
  const int64_t abs_server = l.lsocket(1);
  CHECK(l.lbind(abs_server, std::string("\0tcos-test", 10)) == 0 && l.sys(kListen, abs_server, 1) == 0);
  const int64_t abs_client = l.lsocket(1);
  CHECK(l.lbind(abs_client, std::string("\0tcos-test", 10)) == -kEaddrinuse);
  const uint64_t empty = l.alloc(2);
  l.memory.put32(empty, 1);
  CHECK(l.sys(kBind, abs_client, empty, 2) == 0);      // autobind
  CHECK(l.lname(kGetsockname, abs_client, &r, &length).size() == 6 && length == 8);      // ("\0" and five hex digits)
  CHECK(l.lconnect(abs_client, std::string("\0tcos-test", 10)) == 0);
  // A seqpacket socket cannot connect to a stream listener.
  const int64_t seq = l.lsocket(5);
  CHECK(l.lconnect(seq, std::string("\0tcos-test", 10)) == -kEprototype);
  for (int64_t fd : {server, client, accepted, late, abs_server, abs_client, seq}) CHECK(l.sys(kClose, fd) == 0);
}

static void test_linux_datagram_names() {
  System sys;
  Proc l(103, SyscallAbi::Linux, &sys.fs);
  const int64_t server = l.lsocket(2), client = l.lsocket(2);
  CHECK(l.lbind(server, "/run/dgram.sock") == 0);
  CHECK(l.lbind(client, "/run/client.sock") == 0);
  uint32_t n = 0;
  const uint64_t to = l.unix_address("/run/dgram.sock", &n);
  CHECK(l.sys(kSendto, client, l.data("hi"), 2, 0, to, n) == 2);
  const uint64_t from = l.alloc(kSockaddrUnSize), from_len = l.alloc(4), buf = l.alloc(16);
  l.memory.put32(from_len, kSockaddrUnSize);
  CHECK(l.sys(kRecvfrom, server, buf, 16, 0, from, from_len) == 2 && l.memory.str(from + 2, 16) == "/run/client.sock");
  // A connected datagram socket takes datagrams only from its peer.
  const int64_t other = l.lsocket(2);
  CHECK(l.lconnect(server, "/run/client.sock") == 0);
  const uint64_t to_client = l.unix_address("/run/client.sock", &n);
  CHECK(l.sys(kSendto, other, l.data("x"), 1, 0, l.unix_address("/run/dgram.sock", &n), n) == -1);      // EPERM
  CHECK(l.lsend(server, "reply") == 5);
  int64_t r = 0;
  CHECK(l.lrecv(client, 16, &r) == "reply");
  (void)to_client;
  CHECK(l.lsend(other, "nowhere") == -kEnotconn);
  for (int64_t fd : {server, client, other}) CHECK(l.sys(kClose, fd) == 0);
}

// SCM_RIGHTS between two Linux processes: a file descriptor and a socket descriptor travel, the receiver uses them; SCM_CREDENTIALS with SO_PASSCRED.
static void test_linux_rights() {
  System sys;
  Proc sender(200, SyscallAbi::Linux, &sys.fs), receiver(201, SyscallAbi::Linux, &sys.fs);
  const int64_t server = receiver.lsocket(1);
  CHECK(receiver.lbind(server, std::string("\0rights", 7)) == 0 && receiver.sys(kListen, server, 1) == 0);
  const int64_t out = sender.lsocket(1);
  CHECK(sender.lconnect(out, std::string("\0rights", 7)) == 0);
  const int64_t in = receiver.sys(kAccept, server, 0, 0);
  CHECK(in >= 0);
  // A file the sender opened.
  CHECK(sender.sys(kOpen, sender.text("/tmp/shared.txt"), 0x42 /* O_RDWR|O_CREAT */, 0644) >= 0);
  const int64_t file = sender.sys(kOpen, sender.text("/tmp/shared.txt"), 2, 0);
  CHECK(file >= 0 && sender.sys(kWrite, file, sender.data("contents"), 8) == 8);
  CHECK(sender.sys(8 /* lseek */, file, 0, 0) == 0);
  // And one end of a socketpair.
  const uint64_t pair = sender.alloc(8);
  CHECK(sender.sys(kSocketpair, 1, 1, 0, pair) == 0);
  const int64_t pa = sender.memory.get32(pair), pb = sender.memory.get32(pair + 4);
  // sendmsg: one byte, SCM_RIGHTS with both descriptors.
  const uint64_t iov = sender.alloc(16), control = sender.alloc(24), msg = sender.alloc(56);
  sender.memory.put64(iov, sender.data("R")); sender.memory.put64(iov + 8, 1);
  sender.memory.put64(control, 24); sender.memory.put32(control + 8, 1); sender.memory.put32(control + 12, 1);
  sender.memory.put32(control + 16, uint32_t(file)); sender.memory.put32(control + 20, uint32_t(pb));
  memset(sender.memory.at(msg), 0, 56);
  sender.memory.put64(msg + 16, iov); sender.memory.put64(msg + 24, 1); sender.memory.put64(msg + 32, control); sender.memory.put64(msg + 40, 24);
  CHECK(sender.sys(kSendmsg, out, msg, 0) == 1);
  // The sender lets go of both: what travels keeps them.
  CHECK(sender.sys(kClose, file) == 0 && sender.sys(kClose, pb) == 0);
  // The receiver wants credentials too.
  const uint64_t one = receiver.alloc(4);
  receiver.memory.put32(one, 1);
  CHECK(receiver.sys(kSetsockopt, in, 1, 16 /* SO_PASSCRED */, one, 4) == 0);
  CHECK(sender.lsend(out, "C") == 1);
  const uint64_t rbuf = receiver.alloc(4), riov = receiver.alloc(16), rcontrol = receiver.alloc(64), rmsg = receiver.alloc(56);
  receiver.memory.put64(riov, rbuf); receiver.memory.put64(riov + 8, 4);
  memset(receiver.memory.at(rmsg), 0, 56);
  receiver.memory.put64(rmsg + 16, riov); receiver.memory.put64(rmsg + 24, 1); receiver.memory.put64(rmsg + 32, rcontrol); receiver.memory.put64(rmsg + 40, 64);
  CHECK(receiver.sys(kRecvmsg, in, rmsg, 0x40000000 /* MSG_CMSG_CLOEXEC */) == 1 && receiver.memory.str(rbuf, 1) == "R");      // (a segment that brought descriptors ends the read)
  // First SCM_CREDENTIALS (pid 200), then SCM_RIGHTS with two new descriptors.
  CHECK(receiver.memory.get64(rcontrol) == 28 && receiver.memory.get32(rcontrol + 12) == 2 && receiver.memory.get32(rcontrol + 16) == 200);
  const uint64_t rights = rcontrol + 32;
  CHECK(receiver.memory.get64(rights) == 24 && receiver.memory.get32(rights + 8) == 1 && receiver.memory.get32(rights + 12) == 1);
  const int64_t got_file = receiver.memory.get32(rights + 16), got_sock = receiver.memory.get32(rights + 20);
  CHECK(receiver.memory.get64(rmsg + 40) == 56 && (receiver.memory.get32(rmsg + 48) & 0x8) == 0);
  CHECK(receiver.sys(kFcntl, got_file, 1) == 1);      // FD_CLOEXEC
  const uint64_t fbuf = receiver.alloc(16);
  CHECK(receiver.sys(kRead, got_file, fbuf, 16) == 8 && receiver.memory.str(fbuf, 8) == "contents");
  // The socket end works from the other process: the sender keeps the other end.
  CHECK(receiver.lsend(got_sock, "via passed fd") == 13);
  int64_t r = 0;
  CHECK(sender.lrecv(pa, 64, &r) == "via passed fd");
  CHECK(receiver.lrecv(in, 4, &r) == "C");
  // A control buffer too small for the descriptors: MSG_CTRUNC, and the ones that did not fit are released (no leak: checked at the end of main).
  sender.memory.put32(control + 16, uint32_t(pa));
  sender.memory.put64(control, 20);
  sender.memory.put64(msg + 40, 24);
  CHECK(sender.sys(kSendmsg, out, msg, 0) == 1);
  receiver.memory.put64(rmsg + 40, 16);      // (no room for any descriptor)
  receiver.memory.put32(one, 0);
  CHECK(receiver.sys(kSetsockopt, in, 1, 16, one, 4) == 0);
  CHECK(receiver.sys(kRecvmsg, in, rmsg, 0) == 1 && (receiver.memory.get32(rmsg + 48) & 0x8) != 0);
  // A bad descriptor in SCM_RIGHTS: EBADF, nothing sent.
  sender.memory.put32(control + 16, 777);
  CHECK(sender.sys(kSendmsg, out, msg, 0) == -kEbadf);
  for (int64_t fd : {got_file, got_sock, in, server}) CHECK(receiver.sys(kClose, fd) == 0);
  for (int64_t fd : {out, pa}) CHECK(sender.sys(kClose, fd) == 0);
}

// --- Windows -------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_windows() {
  System sys;
  Proc w(300, SyscallAbi::Windows, &sys.fs);
  uint64_t server = 0, client = 0, accepted = 0;
  CHECK(w.wsocket(&server) == 0 && server != 0);
  CHECK(w.wbind(server, "C:\\Users\\Public\\win.sock") == 0);
  filesystem::DirectoryEntry e;
  CHECK(sys.fs.stat("/Windows/Users/Public/win.sock", &e) == Status::Ok);
  CHECK(w.wlisten(server) == 0);
  CHECK(w.wsocket(&client) == 0);
  CHECK(w.wconnect(client, "c:\\users\\PUBLIC\\WIN.SOCK") == 0);      // (Windows names do not care about case)
  std::string peer;
  CHECK(w.waccept(server, &accepted, &peer) == 0 && accepted != 0 && peer.empty());
  uint32_t sent = 0;
  CHECK(w.wsend(client, "hello windows", &sent) == 0 && sent == 13);
  std::string got;
  CHECK(w.wrecv(accepted, 64, &got) == 0 && got == "hello windows");
  // GetSockName with its size; GetSocketInfo; GetPeerPid.
  const uint64_t name = w.alloc(kSockaddrUnSize), size = w.alloc(4);
  CHECK(w.wcontrol(server, 2, size, 4, name, kSockaddrUnSize) == 0 && w.memory.str(name + 2, 24) == "C:\\Users\\Public\\win.sock" && w.memory.get32(size) == 27);
  const uint64_t info = w.alloc(12);
  CHECK(w.wcontrol(client, 7, 0, 0, info, 12) == 0 && w.memory.get32(info) == 1 && w.memory.get32(info + 4) == 1);
  const uint64_t pid = w.alloc(4);
  CHECK(w.wcontrol(client, 11, 0, 0, pid, 4) == 0 && w.memory.get32(pid) == 300);
  // Non-blocking: nothing to read is WSAEWOULDBLOCK.
  const uint8_t on = 1;
  const uint64_t flag = w.alloc(1);
  w.memory.put(flag, &on, 1);
  CHECK(w.wcontrol(accepted, 1, flag, 1, 0, 0) == 0);
  CHECK(w.wrecv(accepted, 64, &got) == kStatusWouldBlock);
  // NtSelectSockets: the accepted one becomes readable once data is there; a timeout of 0 does not wait.
  const uint64_t read_count = w.alloc(4), handles = w.alloc(8), ready = w.alloc(4), timeout = w.alloc(8);
  w.memory.put32(read_count, 1); w.memory.put32(handles, uint32_t(accepted)); w.memory.put64(timeout, 0);
  CHECK(w.nt(WindowsSyscallNumber::NtSelectSockets, read_count, handles, 0, 0, 0, 0, timeout, ready) == 0 && w.memory.get32(ready) == 0);
  CHECK(w.wsend(client, "x", &sent) == 0);
  w.memory.put32(read_count, 1); w.memory.put32(handles, uint32_t(accepted));
  CHECK(w.nt(WindowsSyscallNumber::NtSelectSockets, read_count, handles, 0, 0, 0, 0, 0, ready) == 0 && w.memory.get32(ready) == 1 && w.memory.get32(read_count) == 1);
  // The name is in use while the file is there.
  uint64_t again = 0;
  CHECK(w.wsocket(&again) == 0 && w.wbind(again, "C:\\Users\\Public\\win.sock") == kStatusAddressInUse);
  CHECK(w.wconnect(again, "C:\\Users\\Public\\none.sock") == kStatusRefused);
  // Closing the client: the accepted end reads the end of the connection.
  CHECK(w.wclose(client) == 0);
  bool closed = false;
  CHECK(w.wrecv(accepted, 64, &got, &closed) == 0 && got == "x");
  CHECK(w.wrecv(accepted, 64, &got, &closed) == 0 && got.empty() && closed);
  CHECK(w.wclose(accepted) == 0 && w.wclose(server) == 0 && w.wclose(again) == 0);
}

// --- Linux and Windows together ---------------------------------------------------------------------------------------------------------------------------------

// A Windows server and a Linux client by path (C:\Users\Public\x.sock is /mnt/c/Users/Public/x.sock), and a Linux server with a Windows client by an abstract name.
static void test_cross_world() {
  System sys;
  Proc w(400, SyscallAbi::Windows, &sys.fs), l(401, SyscallAbi::Linux, &sys.fs);
  uint64_t server = 0, accepted = 0;
  CHECK(w.wsocket(&server) == 0 && w.wbind(server, "C:\\Users\\Public\\bridge.sock") == 0 && w.wlisten(server) == 0);
  // The Linux program sees the socket's file, as a socket, at /mnt/c.
  const uint64_t st = l.alloc(144);
  CHECK(l.sys(kStat, l.text("/mnt/c/Users/Public/bridge.sock"), st) == 0 && (l.memory.get32(st + 24) & 0170000) == 0140000);
  const int64_t client = l.lsocket(1);
  CHECK(l.lconnect(client, "/mnt/c/Users/Public/bridge.sock") == 0);
  CHECK(w.waccept(server, &accepted) == 0);
  CHECK(l.lsend(client, "from linux") == 10);
  std::string got;
  CHECK(w.wrecv(accepted, 64, &got) == 0 && got == "from linux");
  uint32_t sent = 0;
  CHECK(w.wsend(accepted, "from windows", &sent) == 0 && sent == 12);
  int64_t r = 0;
  CHECK(l.lrecv(client, 64, &r) == "from windows");
  // Each end knows the other's process.
  const uint64_t value = l.alloc(12), len_at = l.alloc(4), pid = w.alloc(4);
  l.memory.put32(len_at, 12);
  CHECK(l.sys(kGetsockopt, client, 1, 17, value, len_at) == 0 && l.memory.get32(value) == 400);
  CHECK(w.wcontrol(accepted, 11, 0, 0, pid, 4) == 0 && w.memory.get32(pid) == 401);
  // getpeername from Linux: the name as the Windows server spelled it.
  CHECK(l.lname(kGetpeername, client, &r) == "C:\\Users\\Public\\bridge.sock");
  // Linux binds below /mnt/c; Windows connects with its own spelling of it.
  const int64_t lserver = l.lsocket(1);
  CHECK(l.lbind(lserver, "/mnt/c/Users/Public/linux.sock") == 0 && l.sys(kListen, lserver, 2) == 0);
  uint64_t wclient = 0;
  CHECK(w.wsocket(&wclient) == 0 && w.wconnect(wclient, "C:\\Users\\Public\\linux.sock") == 0);
  const int64_t laccepted = l.sys(kAccept, lserver, 0, 0);
  CHECK(laccepted >= 0);
  CHECK(w.wsend(wclient, "w->l", &sent) == 0 && l.lrecv(laccepted, 16, &r) == "w->l");
  // A Linux socket outside /mnt/c is out of a Windows program's reach (its tree is C: only), as on WSL: only the abstract names are shared outside it.
  const int64_t hidden = l.lsocket(1);
  CHECK(l.lbind(hidden, "/tmp/hidden.sock") == 0 && l.sys(kListen, hidden, 1) == 0);
  uint64_t probe = 0;
  CHECK(w.wsocket(&probe) == 0 && w.wconnect(probe, "C:\\tmp\\hidden.sock") == kStatusRefused);
  const int64_t abstract = l.lsocket(1);
  CHECK(l.lbind(abstract, std::string("\0shared-name", 12)) == 0 && l.sys(kListen, abstract, 1) == 0);
  CHECK(w.wconnect(probe, std::string("\0shared-name", 12)) == 0);
  const int64_t from_windows = l.sys(kAccept, abstract, 0, 0);
  CHECK(from_windows >= 0 && w.wsend(probe, "abstract", &sent) == 0 && l.lrecv(from_windows, 16, &r) == "abstract");
  // Linux closes: Windows reads the end; Windows closes: Linux gets EOF, then EPIPE.
  CHECK(l.sys(kClose, from_windows) == 0);
  bool closed = false;
  CHECK(w.wrecv(probe, 16, &got, &closed) == 0 && closed);
  CHECK(w.wclose(accepted) == 0);
  l.lrecv(client, 16, &r);
  CHECK(r == 0);
  CHECK(l.lsend(client, "gone") == -kEpipe);
  for (uint64_t h : {server, wclient, probe}) CHECK(w.wclose(h) == 0);
  for (int64_t fd : {client, lserver, laccepted, hidden, abstract}) CHECK(l.sys(kClose, fd) == 0);
}

// Threads: a Linux echo server and a Windows client in two threads, each blocking in the kernel for the other - more data than the queue holds, so both
// sides wait for room and for data. Then several Linux and Windows clients at once against one server, by abstract name.
static void test_threads() {
  System sys;
  Proc l(500, SyscallAbi::Linux, &sys.fs), w(501, SyscallAbi::Windows, &sys.fs);
  const int64_t server = l.lsocket(1);
  CHECK(l.lbind(server, "/mnt/c/Users/Public/echo.sock") == 0 && l.sys(kListen, server, 8) == 0);
  constexpr uint32_t kTotal = 1u << 20, kPiece = 48 * 1024;
  std::thread linux_side([&] {
    const int64_t c = l.sys(kAccept, server, 0, 0);
    CHECK(c >= 0);
    const uint64_t buf = l.alloc(kPiece);
    uint64_t echoed = 0;
    for (;;) {
      const int64_t n = l.sys(kRead, c, buf, kPiece);
      if (n <= 0) { CHECK(n == 0); break; }
      int64_t off = 0;
      while (off < n) {
        const int64_t m = l.sys(kWrite, c, buf + off, n - off);
        CHECK(m > 0);
        if (m <= 0) return;
        off += m;
      }
      echoed += uint64_t(n);
    }
    CHECK(echoed == kTotal);
    CHECK(l.sys(kClose, c) == 0);
  });
  uint64_t client = 0;
  CHECK(w.wsocket(&client) == 0 && w.wconnect(client, "C:\\Users\\Public\\echo.sock") == 0);
  std::thread windows_reader([&] {
    // (reads on its own thread while the main one writes: a write of more than the two queues hold would otherwise never finish)
    uint64_t got = 0;
    uint32_t expect = 0;
    const uint64_t at = w.alloc(kPiece), got_at = w.alloc(4), closed_at = w.alloc(1);
    while (got < kTotal) {
      const uint32_t st = w.nt(WindowsSyscallNumber::NtReceiveSocket, client, at, kPiece, got_at, 0, 0, closed_at);
      CHECK(st == 0);
      const uint32_t n = w.memory.get32(got_at);
      if (st != 0 || n == 0) break;
      for (uint32_t i = 0; i < n; ++i, ++expect) if (w.memory.at(at)[i] != uint8_t(expect * 7)) { CHECK(!"echoed bytes in order"); return; }
      got += n;
    }
    CHECK(got == kTotal);
  });
  const uint64_t big = w.alloc(kTotal), sent_at = w.alloc(4);
  for (uint32_t i = 0; i < kTotal; ++i) w.memory.at(big)[i] = uint8_t(i * 7);
  CHECK(w.nt(WindowsSyscallNumber::NtSendSocket, client, big, kTotal, sent_at, 0, 0) == 0 && w.memory.get32(sent_at) == kTotal);
  windows_reader.join();
  const uint64_t how = w.alloc(4);
  w.memory.put32(how, 1);
  CHECK(w.wcontrol(client, 0, how, 4, 0, 0) == 0);      // (shutdown(SD_SEND): the server reads the end and stops)
  linux_side.join();
  CHECK(w.wclose(client) == 0 && l.sys(kClose, server) == 0);

  // Several clients of both kinds at once, by abstract name; each sends its number and reads it back doubled.
  const int64_t hub = l.lsocket(1);
  CHECK(l.lbind(hub, std::string("\0hub", 4)) == 0 && l.sys(kListen, hub, 64) == 0);
  constexpr int kClients = 8, kRounds = 50;
  std::thread serve([&] {
    std::vector<int64_t> fds;
    for (int i = 0; i < kClients; ++i) { const int64_t c = l.sys(kAccept, hub, 0, 0); CHECK(c >= 0); fds.push_back(c); }
    // poll() all of them until every client is done.
    int open = kClients;
    const uint64_t pfds = l.alloc(8 * kClients), buf = l.alloc(8);
    while (open > 0) {
      for (int i = 0; i < kClients; ++i) { l.memory.put32(pfds + 8 * i, uint32_t(fds[i])); l.memory.put32(pfds + 8 * i + 4, fds[i] < 0 ? 0 : 1); }
      const int64_t n = l.sys(kPoll, pfds, kClients, uint64_t(-1));
      CHECK(n > 0);
      if (n <= 0) break;
      for (int i = 0; i < kClients; ++i) {
        const uint16_t revents = uint16_t(l.memory.get32(pfds + 8 * i + 4) >> 16);
        if (fds[i] < 0 || !(revents & (1 | 0x10))) continue;
        const int64_t got = l.sys(kRead, fds[i], buf, 4);
        if (got == 0) { CHECK(l.sys(kClose, fds[i]) == 0); fds[i] = -1; --open; continue; }
        CHECK(got == 4);
        l.memory.put32(buf, l.memory.get32(buf) * 2);
        CHECK(l.sys(kWrite, fds[i], buf, 4) == 4);
      }
    }
  });
  std::vector<std::thread> clients;
  std::vector<Proc*> procs;
  for (int i = 0; i < kClients; ++i) procs.push_back(new Proc(600 + i, i % 2 ? SyscallAbi::Windows : SyscallAbi::Linux, nullptr));
  for (int i = 0; i < kClients; ++i) {
    clients.emplace_back([&, i] {
      Proc& p = *procs[i];
      if (p.abi == SyscallAbi::Linux) {
        const int64_t fd = p.lsocket(1);
        CHECK(p.lconnect(fd, std::string("\0hub", 4)) == 0);
        const uint64_t buf = p.alloc(4);
        for (int k = 0; k < kRounds; ++k) {
          p.memory.put32(buf, uint32_t(i * 1000 + k));
          CHECK(p.sys(kWrite, fd, buf, 4) == 4);
          CHECK(p.sys(kRead, fd, buf, 4) == 4 && p.memory.get32(buf) == uint32_t(i * 1000 + k) * 2);
        }
        CHECK(p.sys(kClose, fd) == 0);
      } else {
        uint64_t h = 0;
        CHECK(p.wsocket(&h) == 0 && p.wconnect(h, std::string("\0hub", 4)) == 0);
        const uint64_t buf = p.alloc(4), n_at = p.alloc(4);
        for (int k = 0; k < kRounds; ++k) {
          p.memory.put32(buf, uint32_t(i * 1000 + k));
          CHECK(p.nt(WindowsSyscallNumber::NtSendSocket, h, buf, 4, n_at, 0, 0) == 0 && p.memory.get32(n_at) == 4);
          CHECK(p.nt(WindowsSyscallNumber::NtReceiveSocket, h, buf, 4, n_at, 0, 0, 0) == 0 && p.memory.get32(n_at) == 4);
          CHECK(p.memory.get32(buf) == uint32_t(i * 1000 + k) * 2);
        }
        CHECK(p.wclose(h) == 0);
      }
    });
  }
  for (std::thread& t : clients) t.join();
  serve.join();
  for (Proc* p : procs) delete p;
  CHECK(l.sys(kClose, hub) == 0);
}

// Many threads making, connecting and closing socket pairs and listeners at once: the table and the names stay consistent (ASan/UBSan watch the memory).
static void test_churn() {
  System sys;
  constexpr int kThreads = 6, kLoops = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      Proc p(700 + t, SyscallAbi::Linux, nullptr);
      const std::string name = std::string("\0churn", 6) + char('a' + t);
      for (int k = 0; k < kLoops; ++k) {
        const int64_t s = p.lsocket(1);
        CHECK(s >= 0);
        CHECK(p.lbind(s, name) == 0 && p.sys(kListen, s, 1) == 0);
        const int64_t c = p.lsocket(1 | 0x800);
        CHECK(p.lconnect(c, name) == 0);
        const int64_t a = p.sys(kAccept4, s, 0, 0, 0x800);
        CHECK(a >= 0);
        CHECK(p.lsend(c, "z") == 1);
        int64_t r = 0;
        CHECK(p.lrecv(a, 1, &r) == "z");
        // Close in a varying order.
        const int64_t order[3][3] = {{s, c, a}, {a, s, c}, {c, a, s}};
        for (int64_t fd : order[k % 3]) CHECK(p.sys(kClose, fd) == 0);
      }
    });
  }
  for (std::thread& th : threads) th.join();
}

int main() {
  const int before = A.live.load();
  printf("-- codec\n"); fflush(stdout); test_codec();
  printf("-- linux_pair\n"); fflush(stdout); test_linux_pair();
  printf("-- linux_messages\n"); fflush(stdout); test_linux_messages();
  printf("-- linux_named\n"); fflush(stdout); test_linux_named();
  printf("-- linux_datagram_names\n"); fflush(stdout); test_linux_datagram_names();
  printf("-- linux_rights\n"); fflush(stdout); test_linux_rights();
  printf("-- windows\n"); fflush(stdout); test_windows();
  printf("-- cross_world\n"); fflush(stdout); test_cross_world();
  printf("-- threads\n"); fflush(stdout); test_threads();
  printf("-- churn\n"); fflush(stdout); test_churn();
  if (A.live.load() != before) { printf("FAILED: %d allocations not given back\n", A.live.load() - before); ++g_failed; }
  if (g_failed) { printf("%d check(s) failed\n", g_failed.load()); return 1; }
  printf("UnixSocketTest: all passed\n");
  return 0;
}
