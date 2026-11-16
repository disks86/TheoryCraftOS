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
// The Linux system calls beyond the basic file and thread ones: the smaller file calls (symlink, preadv, sendfile, record locks, ...), the memory calls
// (mremap, mincore, madvise), signals as the kernel keeps them, the process's ids, limits and scheduling settings, and what fork() and wait() do on the
// portable side (the request a runtime carries out, the state a child inherits, the copy of an address space). tests/LinuxForkTest.cpp runs a real
// program that forks.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/LinuxProcess.hpp"
#include "Process/MemoryFile.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/UnixSockets.hpp"

using namespace process;
using filesystem::OpenMode;
using filesystem::Status;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { ++live; return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)
static TestAllocator A;

// A host whose clock the test moves; a sleep moves it too, so a wait with a timeout ends at once.
struct Host final : IProcessHost {
  std::string out;
  uint64_t now = 1000000000ull;
  bool write_console(uint32_t, ConstByteView d) override { for (uint8_t c : d) out.push_back(char(c)); return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull + now; }
  uint64_t monotonic_ns() override { return now; }
  void sleep_ns(uint64_t ns) override { now += ns; }
  void yield() override {}
};

struct NoWaitSync final : ISystemSync {
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t) override { return true; }
  void notify_all() override {}
};

static void make_file(filesystem::IFileSystem& fs, const char* path, const std::string& text) {
  UniquePointer<filesystem::IFile> f;
  CHECK(fs.open_file(path, OpenMode::CreateTruncate, &f) == Status::Ok);
  uint64_t n = 0;
  CHECK(f->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()), &n) == Status::Ok && n == text.size());
}
static std::string read_file(filesystem::IFileSystem& fs, const char* path) {
  UniquePointer<filesystem::IFile> f;
  if (fs.open_file(path, OpenMode::Read, &f) != Status::Ok) return "<missing>";
  std::string s(f->size(), 0);
  uint64_t n = 0;
  f->read(0, ByteView(reinterpret_cast<uint8_t*>(&s[0]), s.size()), &n);
  return s;
}
static void populate(filesystem::IFileSystem& fs) {
  CHECK(fs.create_directory("/Linux") == Status::Ok && fs.create_directory("/Linux/etc") == Status::Ok && fs.create_directory("/Linux/tmp") == Status::Ok);
  make_file(fs, "/Linux/etc/motd", "welcome\n");
  make_file(fs, "/secret.txt", "outside the tree");
}

// One Linux process with two pages of memory for the test's strings and structures (0x10000 .. 0x30000) and an address space mmap can use.
struct Rig {
  AddressSpace space{A};
  Host host;
  Process* process = nullptr;
  uint64_t cursor = 0x10000;
  explicit Rig(filesystem::IFileSystem* fs = nullptr, uint64_t id = 1, UserArch arch = UserArch::X86_64) {
    CHECK(space.map(0x10000, 0x20000, kProtRead | kProtWrite));
    process = new Process(id, ProcessBinding{SyscallAbi::Linux, arch, syscall_interface(SyscallAbi::Linux, arch)}, space, host);
    process->attach_address_space(space, 0x100000);
    if (fs) process->attach_file_system(A, *fs);
  }
  ~Rig() { delete process; }
  uint64_t alloc(uint64_t n) { const uint64_t at = cursor; cursor += (n + 15) & ~uint64_t{15}; CHECK(cursor <= 0x30000); return at; }
  uint64_t str(const std::string& s) {
    const uint64_t at = alloc(s.size() + 1);
    CHECK(space.write_privileged(at, ConstByteView(reinterpret_cast<const uint8_t*>(s.c_str()), s.size() + 1)));
    return at;
  }
  std::string bytes(uint64_t at, uint64_t n) { std::string s(n, 0); CHECK(space.read(at, ByteView(reinterpret_cast<uint8_t*>(&s[0]), n))); return s; }
  uint64_t u64(uint64_t at) { uint64_t v = 0; CHECK(process->read_user_u64(at, &v)); return v; }
  uint32_t u32(uint64_t at) { return uint32_t(u64(at)); }
  void put64(uint64_t at, uint64_t v) { CHECK(process->write_user_u64(at, v)); }
  void put32(uint64_t at, uint32_t v) { CHECK(process->write_user_u32(at, v)); }
  // A structure of 64-bit words.
  uint64_t words(std::initializer_list<uint64_t> values) {
    const uint64_t at = alloc(values.size() * 8);
    uint64_t i = 0;
    for (const uint64_t v : values) put64(at + 8 * i++, v);
    return at;
  }
  uint64_t call(uint64_t n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) {
    SyscallRequest r;
    r.number = n;
    r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.args[4] = a4; r.args[5] = a5;
    return process->syscall(r);
  }
  int64_t s(uint64_t n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) { return int64_t(call(n, a0, a1, a2, a3, a4, a5)); }
};

constexpr uint64_t kAtFdCwd = ~uint64_t{99};
constexpr uint64_t kBad = 0xDEAD0000;      // an address nothing is mapped at

// --- files ---------------------------------------------------------------------------------------------------------------------------------------------

static void test_files() {
  filesystem::RamFileSystem fs(A);
  populate(fs);
  Rig rig(&fs);
  const uint64_t buf = rig.alloc(256);

  // symlink (88) and symlinkat (266): a relative target is kept as written, an absolute one reads back as the program wrote it; both lead to the file
  CHECK(rig.s(88, rig.str("motd"), rig.str("/etc/link")) == 0);
  CHECK(rig.s(89, rig.str("/etc/link"), buf, 64) == 4 && rig.bytes(buf, 4) == "motd");
  int64_t fd = rig.s(2, rig.str("/etc/link"), 0);
  CHECK(fd >= 3 && rig.s(0, fd, buf, 64) == 8 && rig.bytes(buf, 8) == "welcome\n");
  CHECK(rig.s(3, fd) == 0);
  const int64_t tmp = rig.s(2, rig.str("/tmp"), 0x10000);
  CHECK(tmp >= 3 && rig.s(266, rig.str("/etc/motd"), tmp, rig.str("abs")) == 0);
  CHECK(rig.s(89, rig.str("/tmp/abs"), buf, 64) == 9 && rig.bytes(buf, 9) == "/etc/motd");
  fd = rig.s(2, rig.str("/tmp/abs"), 0);
  CHECK(fd >= 3 && rig.s(0, fd, buf, 64) == 8);
  CHECK(rig.s(3, fd) == 0);
  CHECK(rig.s(88, rig.str("/etc"), rig.str("/tmp/dir")) == 0);                         // to a folder
  fd = rig.s(2, rig.str("/tmp/dir/motd"), 0);
  CHECK(fd >= 3 && rig.s(3, fd) == 0);
  CHECK(rig.s(88, rig.str("nowhere"), rig.str("/tmp/dangling")) == 0);                 // a target that is not there is fine
  CHECK(rig.s(88, rig.str("motd"), rig.str("/etc/link")) == -17);                      // EEXIST
  CHECK(rig.s(88, rig.str("x"), rig.str("/tmp/dangling")) == -17);                     // (a dangling link is in the way too)
  CHECK(rig.s(88, rig.str(""), rig.str("/tmp/empty")) == -2);
  CHECK(rig.s(88, rig.str("x"), rig.str("/nothing/l")) == -2);
  CHECK(rig.s(88, kBad, rig.str("/tmp/l")) == -14 && rig.s(88, rig.str("x"), kBad) == -14);
  CHECK(rig.s(266, rig.str("x"), 999, rig.str("rel")) == -9);
  CHECK(rig.s(88, rig.str("/../../secret.txt"), rig.str("/tmp/out")) == 0);            // ".." cannot climb out of the tree: this names /secret.txt of the program's own tree
  CHECK(rig.s(2, rig.str("/tmp/out"), 0) == -2);

  // creat (85), openat2 (437), close_range (436)
  fd = rig.s(85, rig.str("/tmp/new"), 0644);
  CHECK(fd >= 3 && rig.s(1, fd, rig.str("abcdef"), 6) == 6 && rig.s(0, fd, buf, 1) == -9);      // (write-only)
  CHECK(rig.s(3, fd) == 0 && read_file(fs, "/Linux/tmp/new") == "abcdef");
  fd = rig.s(85, rig.str("/tmp/new"), 0644);
  CHECK(fd >= 3 && rig.s(3, fd) == 0 && read_file(fs, "/Linux/tmp/new") == "");               // truncated
  const uint64_t how = rig.words({0x42 /* O_CREAT | O_RDWR */, 0644, 0});
  fd = rig.s(437, kAtFdCwd, rig.str("/tmp/how"), how, 24);
  CHECK(fd >= 3 && rig.s(1, fd, rig.str("0123456789"), 10) == 10);
  CHECK(rig.s(437, kAtFdCwd, rig.str("/tmp/how"), how, 16) == -22);                    // a structure too short
  CHECK(rig.s(437, kAtFdCwd, rig.str("/tmp/how"), rig.words({2, 0, 0x100}), 24) == -22);      // a RESOLVE_ flag nobody knows
  CHECK(rig.s(437, kAtFdCwd, rig.str("/tmp/how"), rig.words({2, 0, 0, 0}), 32) >= 3);         // a longer structure whose tail is zero
  CHECK(rig.s(437, kAtFdCwd, rig.str("/tmp/how"), rig.words({2, 0, 0, 7}), 32) == -7);        // E2BIG: the tail says something
  CHECK(rig.s(437, kAtFdCwd, rig.str("/tmp/how"), kBad, 24) == -14);
  CHECK(rig.s(437, kAtFdCwd, rig.str("/tmp/how"), 0x30000 - 8, 24) == -14);            // runs off the mapping
  const int64_t d1 = rig.s(32, fd), d2 = rig.s(32, fd), d3 = rig.s(32, fd);
  CHECK(d1 > fd && d2 > d1 && d3 > d2);
  CHECK(rig.s(436, d2, d2, 4) == 0 && rig.s(72, d2, 1) == 1 && rig.s(72, d1, 1) == 0);       // CLOSE_RANGE_CLOEXEC marks, closes nothing
  CHECK(rig.s(436, d1, d2, 0) == 0 && rig.s(72, d1, 1) == -9 && rig.s(72, d2, 1) == -9 && rig.s(72, d3, 1) == 0);
  CHECK(rig.s(436, d3, ~uint64_t{0}, 0) == 0 && rig.s(72, d3, 1) == -9 && rig.s(72, fd, 1) == 0);      // to the end of the table
  CHECK(rig.s(436, 5, 4, 0) == -22 && rig.s(436, 4, 5, 8) == -22);

  // preadv (295), pwritev (296), preadv2 (327), pwritev2 (328): at an offset, the position untouched
  const uint64_t part1 = rig.alloc(16), part2 = rig.alloc(16);
  const uint64_t iov = rig.words({part1, 3, part2, 4});
  CHECK(rig.s(8, fd, 0, 1) == 10);
  CHECK(rig.s(295, fd, iov, 2, 2) == 7 && rig.bytes(part1, 3) == "234" && rig.bytes(part2, 4) == "5678");
  CHECK(rig.s(8, fd, 0, 1) == 10);                                                     // lseek(0, SEEK_CUR): where it was
  CHECK(rig.s(295, fd, iov, 2, 8) == 2);                                               // a short read at the end
  CHECK(rig.space.write_privileged(part1, ConstByteView(reinterpret_cast<const uint8_t*>("XYZ"), 3)) && rig.space.write_privileged(part2, ConstByteView(reinterpret_cast<const uint8_t*>("WVUT"), 4)));
  CHECK(rig.s(296, fd, iov, 2, 1) == 7 && read_file(fs, "/Linux/tmp/how") == "0XYZWVUT89");
  CHECK(rig.s(327, fd, iov, 1, 0, 0, 0) == 3 && rig.bytes(part1, 3) == "0XY");
  CHECK(rig.s(8, fd, 4, 0) == 4 && rig.s(327, fd, iov, 1, ~uint64_t{0}, 0, 0) == 3 && rig.bytes(part1, 3) == "WVU" && rig.s(8, fd, 0, 1) == 7);      // offset -1: the position, which moves
  CHECK(rig.s(328, fd, iov, 1, 0, 0, 0x100) == -95);                                   // a flag nobody knows
  CHECK(rig.s(295, fd, iov, 2, ~uint64_t{0}) == -22);                                  // preadv has no "-1"
  CHECK(rig.s(295, 999, iov, 2, 0) == -9 && rig.s(295, 1, iov, 2, 0) == -29);           // ESPIPE: the console
  CHECK(rig.s(295, fd, kBad, 2, 0) == -14 && rig.s(295, fd, iov, 5000, 0) == -22);
  CHECK(rig.s(295, fd, rig.words({kBad, 3}), 1, 0) == -14);

  // sendfile (40): to a file with and without an offset pointer, and to the console
  const int64_t in = rig.s(2, rig.str("/etc/motd"), 0), out = rig.s(85, rig.str("/tmp/copy"), 0644);
  CHECK(in >= 3 && out >= 3);
  CHECK(rig.s(40, out, in, 0, 5) == 5 && read_file(fs, "/Linux/tmp/copy") == "welco");
  CHECK(rig.s(40, out, in, 0, 100) == 3 && read_file(fs, "/Linux/tmp/copy") == "welcome\n" && rig.s(40, out, in, 0, 100) == 0);      // the rest, then the end
  const uint64_t off = rig.words({3});
  CHECK(rig.s(40, 1, in, off, 4) == 4 && rig.host.out == "come" && rig.u64(off) == 7);
  CHECK(rig.s(8, in, 0, 1) == 8);                                                      // (with an offset pointer the position stays)
  CHECK(rig.s(40, out, in, kBad, 4) == -14 && rig.s(40, out, 999, 0, 4) == -9 && rig.s(40, 999, in, 0, 4) == -9);
  CHECK(rig.s(40, out, 1, 0, 4) == -22 && rig.s(40, out, tmp, 0, 4) == -22);            // not from the console or a folder
  CHECK(rig.s(40, in, in, rig.words({0}), 4) == -9);                                   // the target is read-only
  CHECK(rig.space.region_count() == 1);                                                // the page the bytes passed through is gone again
  {      // a file longer than the window the bytes pass through
    std::string big(200000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char('a' + i % 23);
    make_file(fs, "/Linux/tmp/big", big);
    const int64_t b = rig.s(2, rig.str("/tmp/big"), 0), c = rig.s(85, rig.str("/tmp/big2"), 0644);
    CHECK(rig.s(40, c, b, 0, 1 << 20) == 200000 && read_file(fs, "/Linux/tmp/big2") == big);
    // copy_file_range (326): between two files, with offsets
    const int64_t e = rig.s(85, rig.str("/tmp/big3"), 0644);
    const uint64_t from = rig.words({100000}), to = rig.words({5});
    CHECK(rig.s(326, b, from, e, to, 50000, 0) == 50000 && rig.u64(from) == 150000 && rig.u64(to) == 50005);
    CHECK(read_file(fs, "/Linux/tmp/big3").substr(5) == big.substr(100000, 50000));
    CHECK(rig.s(326, b, from, e, 0, 1 << 20, 0) == 50000 && rig.s(326, b, from, e, 0, 10, 0) == 0);      // to the end of the source; then nothing
    CHECK(rig.s(326, b, from, e, to, 10, 1) == -22 && rig.s(326, 1, 0, e, 0, 10, 0) == -22 && rig.s(326, b, 0, b, 0, 10, 0) == -9);
    CHECK(rig.s(326, tmp, 0, e, 0, 10, 0) == -21 && rig.s(326, b, kBad, e, 0, 10, 0) == -14);
    CHECK(rig.s(3, b) == 0 && rig.s(3, c) == 0 && rig.s(3, e) == 0);
  }

  // getdents (78): the old layout, the type in the last byte of each record
  {
    const int64_t etc = rig.s(2, rig.str("/etc"), 0x10000);
    const uint64_t entries = rig.alloc(1024);
    const int64_t n = rig.s(78, etc, entries, 1024);
    CHECK(n > 0);
    bool motd = false, dot = false;
    for (int64_t at = 0; at < n;) {
      const uint64_t reclen = rig.u64(entries + at + 16) & 0xFFFF;
      CHECK(reclen >= 24 && reclen % 8 == 0 && at + int64_t(reclen) <= n);
      if (reclen < 24) break;
      const std::string record = rig.bytes(entries + at, reclen);
      const std::string name(record.c_str() + 18);
      if (name == "motd") motd = uint8_t(record[reclen - 1]) == 8;      // DT_REG
      if (name == ".") dot = uint8_t(record[reclen - 1]) == 4;          // DT_DIR
      at += int64_t(reclen);
    }
    CHECK(motd && dot);
    CHECK(rig.s(78, etc, entries, 1024) == 0 && rig.s(78, in, entries, 1024) == -20 && rig.s(78, 999, entries, 1024) == -9);
    CHECK(rig.s(8, etc, 0, 0) == 0 && rig.s(78, etc, entries, 8) == -22 && rig.s(78, etc, kBad, 1024) == -14);
    CHECK(rig.s(3, etc) == 0);
  }

  // renameat2 (316) with RENAME_NOREPLACE
  CHECK(rig.s(316, kAtFdCwd, rig.str("/tmp/copy"), kAtFdCwd, rig.str("/tmp/how"), 1) == -17);
  CHECK(rig.s(316, kAtFdCwd, rig.str("/tmp/copy"), kAtFdCwd, rig.str("/tmp/moved"), 1) == 0 && read_file(fs, "/Linux/tmp/moved") == "welcome\n");
  CHECK(rig.s(316, kAtFdCwd, rig.str("/tmp/moved"), kAtFdCwd, rig.str("/tmp/new"), 2) == -22);      // RENAME_EXCHANGE
  CHECK(rig.s(316, kAtFdCwd, rig.str("/tmp/moved"), kAtFdCwd, rig.str("/tmp/new"), 0) == 0 && read_file(fs, "/Linux/tmp/new") == "welcome\n");      // without the flag it replaces

  // advice, partial syncs, attributes, and what only an administrator does
  CHECK(rig.s(221, in, 0, 0, 2) == 0 && rig.s(187, in, 0, 4096) == 0 && rig.s(277, in, 0, 0, 0) == 0 && rig.s(306, in) == 0);
  CHECK(rig.s(221, 999, 0, 0, 2) == -9 && rig.s(221, 1, 0, 0, 2) == -22 && rig.s(306, 1) == 0 && rig.s(306, 999) == -9);
  CHECK(rig.s(191, rig.str("/etc/motd"), rig.str("user.x"), buf, 64) == -95 && rig.s(188, rig.str("/etc/motd"), rig.str("user.x"), buf, 1, 0) == -95 && rig.s(196, in, buf, 64) == -95);
  CHECK(rig.s(161, rig.str("/tmp")) == -1 && rig.s(165) == -1 && rig.s(170, buf, 4) == -1 && rig.s(227, 0, buf) == -1);
  CHECK(rig.s(275, in, 0, out, 0, 10, 0) == -22);                                      // splice
  CHECK(rig.s(95, 027) == 022 && rig.s(95, 0777777) == 027 && rig.s(95, 022) == 0777);  // umask reads back, permission bits only
  CHECK(rig.s(132, rig.str("/etc/motd"), 0) == 0 && rig.s(235, rig.str("/etc/motd"), 0) == 0);      // utime, utimes
  CHECK(rig.s(3, in) == 0 && rig.s(3, out) == 0 && rig.s(3, fd) == 0 && rig.s(3, tmp) == 0);
}

// fcntl record locks: F_GETLK 5, F_SETLK 6, F_SETLKW 7; the open-file ones 36, 37, 38. struct flock: type (2), whence (2), pad (4), start (8), length (8), pid (4).
static void test_record_locks() {
  filesystem::RamFileSystem fs(A);
  populate(fs);
  make_file(fs, "/Linux/tmp/db", std::string(4096, 'd'));
  static NoWaitSync sync;
  ObjectManager objects(sync);
  objects.set_allocator(&A);
  UnixSocketManager sockets(sync, A);      // (without one the system makes its own on first use, which lives on)
  set_system_objects(&objects);
  set_system_unix_sockets(&sockets);
  {
    Rig one(&fs, 1), two(&fs, 2);
    constexpr uint64_t RD = 0, WR = 1, UN = 2;
    auto flock = [](Rig& r, uint64_t type, int64_t start, int64_t length, uint64_t whence = 0) {
      const uint64_t at = r.alloc(32);
      r.put64(at, type | whence << 16);
      r.put64(at + 8, uint64_t(start));
      r.put64(at + 16, uint64_t(length));
      r.put64(at + 24, 0);
      return at;
    };
    auto type_of = [](Rig& r, uint64_t at) { return r.u64(at) & 0xFFFF; };
    const int64_t a = one.s(2, one.str("/tmp/db"), 2), b = two.s(2, two.str("/tmp/db"), 2);
    CHECK(a >= 3 && b >= 3);
    // a write lock on [100, 200) of process one keeps process two out, read or write, but not off the bytes next to it
    CHECK(one.s(72, a, 6, flock(one, WR, 100, 100)) == 0);
    CHECK(two.s(72, b, 6, flock(two, WR, 150, 10)) == -11 && two.s(72, b, 6, flock(two, RD, 199, 1)) == -11);
    CHECK(two.s(72, b, 6, flock(two, WR, 200, 10)) == 0 && two.s(72, b, 6, flock(two, WR, 0, 100)) == 0);
    uint64_t probe = flock(two, WR, 120, 500);
    CHECK(two.s(72, b, 5, probe) == 0 && type_of(two, probe) == WR && two.u64(probe + 8) == 100 && two.u64(probe + 16) == 100);      // F_GETLK: who is in the way
    probe = flock(two, WR, 300, 10);
    CHECK(two.s(72, b, 5, probe) == 0 && type_of(two, probe) == UN);
    // the same process again: its own lock is never in its way, through any descriptor; a new lock replaces the old one where they overlap
    const int64_t a2 = one.s(2, one.str("/tmp/db"), 2);
    CHECK(one.s(72, a2, 6, flock(one, RD, 100, 100)) == 0);                            // down to a read lock
    CHECK(two.s(72, b, 6, flock(two, RD, 150, 10)) == 0 && two.s(72, b, 6, flock(two, WR, 150, 10)) == -11);
    CHECK(one.s(72, a, 6, flock(one, WR, 150, 10)) == -11);                            // (two's read lock is in the way of one's upgrade)
    CHECK(two.s(72, b, 6, flock(two, UN, 150, 10)) == 0 && one.s(72, a, 6, flock(one, WR, 150, 10)) == 0);
    // unlocking the middle of a lock leaves both ends: SQLite unlocks two neighbouring bytes at once
    CHECK(one.s(72, a, 6, flock(one, UN, 120, 60)) == 0);
    CHECK(two.s(72, b, 6, flock(two, WR, 120, 60)) == 0 && two.s(72, b, 6, flock(two, WR, 119, 1)) == -11 && two.s(72, b, 6, flock(two, WR, 180, 1)) == -11);
    CHECK(two.s(72, b, 6, flock(two, UN, 0, 0)) == 0);                                 // length 0: to the end, however far
    // "to the end of the file" and the other ways of saying where
    CHECK(one.s(72, a, 6, flock(one, UN, 0, 0)) == 0 && one.s(72, a, 6, flock(one, WR, -96, 0, 2)) == 0);      // SEEK_END - 96: from 4000 on
    CHECK(two.s(72, b, 6, flock(two, WR, 1 << 20, 1)) == -11 && two.s(72, b, 6, flock(two, WR, 3999, 1)) == 0);
    probe = flock(two, RD, 5000, 1);
    CHECK(two.s(72, b, 5, probe) == 0 && type_of(two, probe) == WR && two.u64(probe + 8) == 4000 && two.u64(probe + 16) == 0);      // (length 0: to the end)
    CHECK(one.s(72, a, 6, flock(one, WR, 50, -10)) == 0 && two.s(72, b, 6, flock(two, WR, 40, 10)) == -11 && two.s(72, b, 6, flock(two, WR, 50, 1)) == 0);      // a negative length: the bytes before
    CHECK(one.s(72, a, 6, flock(one, WR, -1, 1)) == -22 && one.s(72, a, 6, flock(one, 3, 0, 1)) == -22 && one.s(72, a, 6, flock(one, WR, 0, 1, 3)) == -22);
    CHECK(one.s(72, a, 6, kBad) == -14 && one.s(72, 999, 6, flock(one, WR, 0, 1)) == -9);
    const int64_t ro = one.s(2, one.str("/tmp/db"), 0);
    CHECK(one.s(72, ro, 6, flock(one, WR, 900, 1)) == -9 && one.s(72, ro, 6, flock(one, RD, 900, 1)) == 0);      // a write lock needs a descriptor open for writing
    CHECK(one.s(72, 1, 6, flock(one, WR, 0, 1)) == 0);                                  // the console: nobody else could hold one
    // F_SETLKW waits; the host's sleep is not a real one here, so it ends as an interrupted wait would only if the lock never comes: it does come once two lets go
    CHECK(two.s(72, b, 6, flock(two, UN, 0, 0)) == 0);
    CHECK(one.s(72, a, 7, flock(one, WR, 0, 0)) == 0 && two.s(72, b, 6, flock(two, RD, 7, 1)) == -11);
    // open-file locks (F_OFD_*): they belong to the open file, so two descriptors of one process can be in each other's way; and they do not meet flock()
    CHECK(one.s(72, a, 6, flock(one, UN, 0, 0)) == 0);
    CHECK(one.s(72, a, 37, flock(one, WR, 0, 10)) == 0 && one.s(72, a2, 37, flock(one, WR, 5, 10)) == -11 && one.s(72, a2, 37, flock(one, WR, 10, 10)) == 0);
    probe = flock(one, WR, 0, 5);
    CHECK(one.s(72, a2, 36, probe) == 0 && type_of(one, probe) == WR && one.u32(probe + 24) == 0xFFFFFFFFu);
    CHECK(one.s(73, a2, 2 | 4) == 0);                                                  // flock(LOCK_EX | LOCK_NB) on the same file: its own kind of lock
    CHECK(one.s(3, a) == 0 && one.s(72, a2, 37, flock(one, WR, 0, 10)) == 0);          // the open file's locks went with it
    // a process's locks go when it ends
    CHECK(two.s(72, b, 6, flock(two, WR, 2000, 10)) == 0 && one.s(72, a2, 6, flock(one, WR, 2000, 10)) == -11);
    delete two.process;
    two.process = nullptr;
    CHECK(one.s(72, a2, 6, flock(one, WR, 2000, 10)) == 0);
  }
  set_system_unix_sockets(nullptr);
  set_system_objects(nullptr);
}

// --- memory --------------------------------------------------------------------------------------------------------------------------------------------

static void test_memory() {
  Rig rig;
  constexpr uint64_t kPage = 4096, kRw = 3, kPrivateAnon = 0x22;
  auto mmap = [&](uint64_t address, uint64_t size, uint64_t flags = 0x22) { return rig.call(9, address, size, kRw, flags, ~uint64_t{0}, 0); };
  auto byte = [&](uint64_t at) { uint8_t b = 0xEE; CHECK(rig.space.read(at, ByteView(&b, 1))); return b; };
  auto poke = [&](uint64_t at, uint8_t v) { CHECK(rig.space.write(at, ConstByteView(&v, 1))); };

  // mremap (25): grows in place when the pages behind are free
  const uint64_t m = mmap(0, 2 * kPage);
  CHECK(int64_t(m) > 0);
  poke(m, 11); poke(m + kPage, 22); poke(m + 2 * kPage - 1, 33);
  CHECK(rig.call(25, m, 2 * kPage, 4 * kPage, 0) == m && byte(m) == 11 && byte(m + 2 * kPage - 1) == 33 && byte(m + 2 * kPage) == 0 && byte(m + 4 * kPage - 1) == 0);
  poke(m + 4 * kPage - 1, 44);
  // something in the way: without MREMAP_MAYMOVE it cannot grow; with it the mapping moves, bytes and permissions and all, and the old place is free
  CHECK(mmap(m + 4 * kPage, kPage, kPrivateAnon | 0x10) == m + 4 * kPage);
  CHECK(rig.s(10, m + kPage, kPage, 1) == 0);                                          // (one page read-only)
  CHECK(rig.s(25, m, 4 * kPage, 8 * kPage, 0) == -12);
  const uint64_t moved = rig.call(25, m, 4 * kPage, 8 * kPage, 1);
  CHECK(int64_t(moved) > 0 && moved != m && byte(moved) == 11 && byte(moved + kPage) == 22 && byte(moved + 2 * kPage - 1) == 33 && byte(moved + 4 * kPage - 1) == 44 && byte(moved + 8 * kPage - 1) == 0);
  CHECK(!rig.space.check(m, kPage, 0) && rig.space.check(m + 4 * kPage, kPage, 0));
  uint8_t protection = 0;
  CHECK(rig.space.protection_at(moved + kPage, &protection) && protection == kProtRead && rig.space.protection_at(moved, &protection) && protection == kRw);
  // shrinking stays where it is and lets the tail go
  CHECK(rig.call(25, moved, 8 * kPage, kPage, 0) == moved && byte(moved) == 11 && !rig.space.check(moved + kPage, kPage, 0));
  CHECK(rig.call(25, moved, kPage, kPage, 0) == moved);
  CHECK(rig.s(25, moved + 1, kPage, 2 * kPage, 1) == -22 && rig.s(25, moved, kPage, 0, 1) == -22 && rig.s(25, moved, 0, kPage, 1) == -22);
  CHECK(rig.s(25, moved, kPage, 2 * kPage, 2) == -22);                                 // MREMAP_FIXED alone
  CHECK(rig.s(25, kBad, kPage, 2 * kPage, 1) == -14 && rig.s(25, moved, 2 * kPage, 3 * kPage, 1) == -14);      // not (all) mapped
  CHECK(rig.s(25, moved, kPage, ~uint64_t{0} - 100, 1) == -22 && rig.s(25, moved, kPage, uint64_t{1} << 50, 1) == -12);

  // mincore (27): a page has memory once it was written
  const uint64_t vec = rig.alloc(16);
  const uint64_t q = mmap(0, 3 * kPage);
  poke(q + kPage, 1);
  CHECK(rig.s(27, q, 3 * kPage, vec) == 0 && rig.bytes(vec, 3) == std::string("\0\1\0", 3));
  CHECK(rig.s(27, q + 1, kPage, vec) == -22 && rig.s(27, q, 4 * kPage, vec) == -12 && rig.s(27, q, kPage, kBad) == -14 && rig.s(27, kBad, kPage, vec) == -12);

  // madvise (28) MADV_DONTNEED: anonymous pages read as zeros again; other advice changes nothing
  poke(q, 7); poke(q + 2 * kPage, 9);
  CHECK(rig.s(28, q, 3 * kPage, 14) == 0 && byte(q) == 7);                              // MADV_HUGEPAGE
  CHECK(rig.s(28, q, 2 * kPage, 4) == 0 && byte(q) == 0 && byte(q + kPage) == 0 && byte(q + 2 * kPage) == 9);
  CHECK(rig.s(27, q, 3 * kPage, vec) == 0 && rig.bytes(vec, 3) == std::string("\0\0\1", 3));
  CHECK(rig.s(28, q + 1, kPage, 4) == -22 && rig.s(28, kBad, kPage, 4) == -12);
  {      // a view of shared memory keeps its bytes (they are the file's, not the mapping's)
    MemoryBlock* block = MemoryBlock::make(A, kPage);
    CHECK(block != nullptr);
    block->bytes()[5] = 77;
    CHECK(rig.space.map_shared(0x400000, kPage, kRw, block->storage(), block));
    CHECK(rig.s(28, 0x400000, kPage, 4) == 0 && byte(0x400005) == 77);
    CHECK(rig.s(26, 0x400000, kPage, 4) == 0);                                         // msync of it
    CHECK(rig.s(25, 0x400000, kPage, 2 * kPage, 1) == -12);                            // and it cannot grow
    CHECK(rig.space.unmap(0x400000, kPage));
    block->release();
  }

  {      // MAP_SHARED | MAP_ANONYMOUS (0x21): one block of memory that a copy of the address space shares; MAP_PRIVATE memory is copied
    Rig files;
    const uint64_t shared = files.call(9, 0, 2 * kPage, kRw, 0x21, ~uint64_t{0}, 0), own = files.call(9, 0, kPage, kRw, 0x22, ~uint64_t{0}, 0);
    CHECK(int64_t(shared) > 0 && int64_t(own) > 0 && files.space.is_shared(shared) && !files.space.is_shared(own));
    const uint8_t one = 1, two = 2;
    uint8_t got = 9;
    CHECK(files.space.read(shared + kPage, ByteView(&got, 1)) && got == 0);             // zeros to begin with
    CHECK(files.space.write(shared, ConstByteView(&one, 1)) && files.space.write(own, ConstByteView(&one, 1)));
    {
      AddressSpace child(A);
      CHECK(child.copy_from(files.space) && child.write(shared, ConstByteView(&two, 1)) && child.write(own, ConstByteView(&two, 1)));
      CHECK(files.space.read(shared, ByteView(&got, 1)) && got == 2 && files.space.read(own, ByteView(&got, 1)) && got == 1);
      CHECK(files.s(11, shared, 2 * kPage) == 0 && child.read(shared, ByteView(&got, 1)) && got == 2);      // the child's mapping keeps the memory alive
    }
    CHECK(files.call(9, own, kPage, kRw, 0x21 | 0x10, ~uint64_t{0}, 0) == own && files.space.is_shared(own) && files.space.read(own, ByteView(&got, 1)) && got == 0);      // MAP_FIXED over something else
    CHECK(files.s(9, own + 1, kPage, kRw, 0x21 | 0x10, ~uint64_t{0}, 0) == -22 && files.s(11, own, kPage) == 0);
  }

  // msync (26), mlock (149), munlock (150), mlock2 (325), mlockall (151), munlockall (152)
  CHECK(rig.s(26, q, kPage, 4) == 0 && rig.s(26, q, kPage, 1 | 4) == -22 && rig.s(26, q, kPage, 8) == -22 && rig.s(26, kBad, kPage, 4) == -12 && rig.s(26, q + 1, kPage, 4) == -22);
  CHECK(rig.s(149, q, kPage) == 0 && rig.s(149, q + 100, 100) == 0 && rig.s(150, q, kPage) == 0 && rig.s(325, q, kPage, 1) == 0 && rig.s(149, kBad, kPage) == -12);
  CHECK(rig.s(151, 3) == 0 && rig.s(152) == 0);
}

// The copy fork() makes of an address space.
static void test_address_space_copy() {
  constexpr uint64_t kPage = 4096;
  MemoryBlock* block = MemoryBlock::make(A, 2 * kPage);
  CHECK(block != nullptr);
  {
    AddressSpace parent(A), child(A);
    CHECK(parent.map(0x10000, 4 * kPage, kProtRead | kProtWrite) && parent.map(0x40000, kPage, kProtRead) && parent.map(0x80000, 16 * kPage, kProtRead | kProtWrite));
    CHECK(parent.protect(0x12000, kPage, kProtRead));                                  // (cuts the first region in three)
    CHECK(parent.map_shared(0x200000, 2 * kPage, kProtRead | kProtWrite, block->storage(), block));
    const uint8_t text[] = "parent";
    CHECK(parent.write(0x10000, ConstByteView(text, 6)) && parent.write_privileged(0x40000, ConstByteView(text, 6)) && parent.write(0x200000, ConstByteView(text, 6)));
    const uint64_t pages_before = parent.resident_bytes();
    CHECK(child.copy_from(parent));
    CHECK(child.region_count() == parent.region_count() && child.mapped_bytes() == parent.mapped_bytes() && child.resident_bytes() == pages_before);
    uint8_t got[6] = {};
    CHECK(child.read(0x10000, ByteView(got, 6)) && memcmp(got, text, 6) == 0);
    CHECK(child.read(0x40000, ByteView(got, 6)) && memcmp(got, text, 6) == 0 && !child.write(0x40000, ConstByteView(text, 1)));      // read-only stays read-only
    CHECK(!child.write(0x12000, ConstByteView(text, 1)) && child.write(0x13000, ConstByteView(text, 1)) && !child.check(0x14000, 1, 0));
    // private memory is the child's own from now on, in both directions
    const uint8_t mine[] = "child!";
    CHECK(child.write(0x10000, ConstByteView(mine, 6)) && parent.read(0x10000, ByteView(got, 6)) && memcmp(got, text, 6) == 0);
    CHECK(parent.write(0x80000, ConstByteView(text, 6)) && child.read(0x80000, ByteView(got, 6)) && got[0] == 0);      // (a page neither had memory for at the copy)
    // shared memory is the same memory
    CHECK(child.is_shared(0x200000) && child.write(0x200000, ConstByteView(mine, 6)) && parent.read(0x200000, ByteView(got, 6)) && memcmp(got, mine, 6) == 0);
    CHECK(block->uses() == 3);                                                         // the test's, and one for each mapping
    AddressSpace again(A);
    CHECK(again.map(0x10000, kPage, kProtRead) && !again.copy_from(parent));            // only into an empty space
    CHECK(!parent.copy_from(parent));
  }
  CHECK(block->uses() == 1);
  block->release();
}

// --- signals -------------------------------------------------------------------------------------------------------------------------------------------

static void test_signals() {
  constexpr uint64_t kUsr1 = 10, kUsr2 = 12, kAlrm = 14, kTerm = 15, kChld = 17;
  auto bit = [](uint64_t signal) { return uint64_t{1} << (signal - 1); };
  {
    Rig rig;
    Process& p = *rig.process;
    const uint64_t act = rig.words({0x400100, 0x04000004, 0x400200, bit(kTerm) | bit(9)}), old = rig.alloc(32 + 8);
    rig.put64(old + 32, 0x4755415244ull);
    // rt_sigaction (13): what is set reads back (the mask without SIGKILL), nothing is written past the 32 bytes
    CHECK(rig.s(13, kUsr1, 0, old, 8) == 0 && rig.u64(old) == 0 && rig.u64(old + 8) == 0 && rig.u64(old + 24) == 0 && rig.u64(old + 32) == 0x4755415244ull);
    CHECK(rig.s(13, kUsr1, act, old, 8) == 0 && rig.u64(old) == 0);
    CHECK(rig.s(13, kUsr1, 0, old, 8) == 0 && rig.u64(old) == 0x400100 && rig.u64(old + 8) == 0x04000004 && rig.u64(old + 16) == 0x400200 && rig.u64(old + 24) == bit(kTerm));
    CHECK(rig.s(13, 9, act, 0, 8) == -22 && rig.s(13, 19, act, 0, 8) == -22 && rig.s(13, 9, 0, old, 8) == 0);      // SIGKILL and SIGSTOP: read, not set
    CHECK(rig.s(13, 0, act, 0, 8) == -22 && rig.s(13, 65, act, 0, 8) == -22 && rig.s(13, kUsr1, act, 0, 16) == -22);
    CHECK(rig.s(13, kUsr1, kBad, 0, 8) == -14 && rig.s(13, kUsr1, 0, kBad, 8) == -14 && rig.s(13, kUsr1, 0x30000 - 16, 0, 8) == -14);
    CHECK(rig.s(13, kUsr1, act, kBad, 8) == -14 && rig.s(13, 64, act, 0, 8) == 0);

    // rt_sigprocmask (14): block, unblock, set; SIGKILL and SIGSTOP are never blocked
    const uint64_t set = rig.alloc(8), was = rig.alloc(8);
    rig.put64(set, bit(kUsr1) | bit(9) | bit(19));
    CHECK(rig.s(14, 0, set, was, 8) == 0 && rig.u64(was) == 0);
    CHECK(rig.s(14, 0, 0, was, 8) == 0 && rig.u64(was) == bit(kUsr1));
    rig.put64(set, bit(kUsr2));
    CHECK(rig.s(14, 0, set, 0, 8) == 0 && rig.s(14, 1, set, was, 8) == 0 && rig.u64(was) == (bit(kUsr1) | bit(kUsr2)));
    CHECK(rig.s(14, 3, set, 0, 8) == -22 && rig.s(14, 0, set, 0, 4) == -22 && rig.s(14, 0, kBad, 0, 8) == -14 && rig.s(14, 0, 0, kBad, 8) == -14);
    CHECK(rig.s(14, 3, 0, was, 8) == 0);                                               // (how does not matter without a set)

    // a blocked signal waits: kill (62), tgkill (234), tkill (200) of the process itself; rt_sigpending (127); rt_sigtimedwait (128) takes the lowest
    CHECK(rig.s(62, 1, kUsr1) == 0 && rig.s(127, was, 8) == 0 && rig.u64(was) == bit(kUsr1));
    CHECK(rig.s(234, 1, 1, kUsr1) == 0 && rig.s(200, 1, kUsr1) == 0);                    // signals of one kind do not queue
    rig.put64(set, bit(kUsr2) | bit(40));
    CHECK(rig.s(14, 0, set, 0, 8) == 0 && rig.s(62, 0, 40) == 0 && rig.s(62, 1, kUsr2) == 0);      // (kill(0, ...): its own process group)
    const uint64_t info = rig.alloc(128 + 8), no_wait = rig.words({0, 0});
    rig.put64(info + 128, 0x4755415244ull);
    rig.put64(set, bit(kUsr2) | bit(40) | bit(kTerm));
    CHECK(rig.s(128, set, info, no_wait, 8) == int64_t(kUsr2) && rig.u32(info) == kUsr2 && rig.u64(info + 128) == 0x4755415244ull);
    CHECK(rig.s(128, set, 0, no_wait, 8) == 40 && rig.s(128, set, info, no_wait, 8) == -11);
    CHECK(rig.s(128, set, info, rig.words({0, 50000000}), 8) == -11);                   // a timeout that runs out
    CHECK(rig.s(128, set, info, rig.words({0, 2000000000}), 8) == -22 && rig.s(128, kBad, info, no_wait, 8) == -14 && rig.s(128, set, info, no_wait, 4) == -22);
    CHECK(rig.s(128, rig.words({bit(kUsr1)}), kBad, no_wait, 8) == -14);
    CHECK(rig.s(127, was, 8) == 0 && rig.u64(was) == 0 && !p.exited());                 // (taken although its siginfo could not be written, as on Linux)
    CHECK(rig.s(62, 1, 65) == -22 && rig.s(62, 1, 0) == 0 && rig.s(234, 1, 77, kUsr1) == -3 && rig.s(234, 1, 0, kUsr1) == -22 && rig.s(62, ~uint64_t{0}, kUsr1) == -1);
    CHECK(rig.s(62, 555, 0) == -3 && rig.s(62, uint64_t(-555), 0) == -3);               // nobody there

    // a signal with a handler cannot be run: it stays pending even when not blocked, and sigsuspend (130) and pause (34) return as if it had been
    rig.put64(set, 0);
    CHECK(rig.s(14, 2, set, 0, 8) == 0 && rig.s(62, 1, kUsr1) == 0 && !p.exited() && (p.linux_state().pending & bit(kUsr1)));
    CHECK(rig.s(130, set, 8) == -4 && p.linux_state().pending == 0);
    CHECK(rig.s(62, 1, kUsr1) == 0 && rig.s(34) == -4 && p.linux_state().pending == 0);
    CHECK(rig.s(130, kBad, 8) == -14 && rig.s(130, set, 4) == -22);

    // ignored ones are dropped: by default (SIGCHLD), or because the program said so; a pending one goes when it becomes ignored
    CHECK(rig.s(62, 1, kChld) == 0 && p.linux_state().pending == 0 && !p.exited());
    rig.put64(set, bit(kTerm));
    CHECK(rig.s(14, 0, set, 0, 8) == 0 && rig.s(62, 1, kTerm) == 0 && p.linux_state().pending == bit(kTerm) && !p.exited());      // SIGTERM, blocked, default action
    CHECK(rig.s(13, kTerm, rig.words({1, 0, 0, 0}), 0, 8) == 0 && p.linux_state().pending == 0);
    CHECK(rig.s(14, 1, set, 0, 8) == 0 && rig.s(62, 1, kTerm) == 0 && !p.exited());

    // sigaltstack (131): kept and read back
    const uint64_t stack = rig.words({0x500000, 0, 65536}), old_stack = rig.alloc(24);
    CHECK(rig.s(131, 0, old_stack) == 0 && rig.u32(old_stack + 8) == 2 && rig.u64(old_stack) == 0);      // SS_DISABLE
    CHECK(rig.s(131, stack, 0) == 0 && rig.s(131, 0, old_stack) == 0 && rig.u64(old_stack) == 0x500000 && rig.u32(old_stack + 8) == 0 && rig.u64(old_stack + 16) == 65536);
    CHECK(rig.s(131, rig.words({0x500000, 0, 100}), 0) == -12 && rig.s(131, rig.words({0x500000, 1, 65536}), 0) == -22 && rig.s(131, kBad, 0) == -14 && rig.s(131, 0, kBad) == -14);
    CHECK(rig.s(131, rig.words({0, 2, 0}), old_stack) == 0 && rig.s(131, 0, old_stack) == 0 && rig.u32(old_stack + 8) == 2);
    CHECK(rig.s(15) == -22);                                                           // rt_sigreturn with no handler entered
  }
  {      // the default action of most signals ends the process: with 128 + the signal as its code, and the signal kept for whoever waits for it
    Rig rig;
    CHECK(rig.call(62, 1, kTerm) == 0 && rig.process->exited() && rig.process->exit_code() == 143 && rig.process->linux_state().exit_signal == kTerm);
  }
  {      // blocked, it waits until the mask lets it through
    Rig rig;
    const uint64_t set = rig.words({bit(kUsr1)});
    CHECK(rig.s(14, 0, set, 0, 8) == 0 && rig.s(62, 1, kUsr1) == 0 && !rig.process->exited());
    rig.call(14, 1, set, 0, 8);
    CHECK(rig.process->exited() && rig.process->exit_code() == 128 + kUsr1);
  }
  {      // SIGKILL cannot be blocked, ignored or caught; abort() raises SIGABRT, resets the handler and raises it again
    Rig rig;
    CHECK(rig.s(14, 0, rig.words({~uint64_t{0}}), 0, 8) == 0);
    rig.call(234, 1, 1, 9);
    CHECK(rig.process->exited() && rig.process->exit_code() == 137);
    Rig abort;
    CHECK(abort.s(13, 6, abort.words({0x400100, 0, 0, 0}), 0, 8) == 0 && abort.s(234, 1, 1, 6) == 0 && !abort.process->exited());
    CHECK(abort.s(13, 6, abort.words({0, 0, 0, 0}), 0, 8) == 0);                        // back to the default: the pending one is carried out
    CHECK(abort.process->exited() && abort.process->exit_code() == 134);
  }
  {      // alarm (37), getitimer (36), setitimer (38): the signal is raised at the first system call after the time
    Rig rig;
    CHECK(rig.s(37, 2) == 0 && rig.s(37, 5) == 2 && rig.s(37, 0) == 5 && rig.s(37, 3) == 0);
    rig.host.now += 2999999999ull;
    CHECK(rig.s(39) == 1 && !rig.process->exited());
    rig.host.now += 1;
    rig.call(39);
    CHECK(rig.process->exited() && rig.process->exit_code() == 128 + kAlrm);
    Rig timer;
    const uint64_t value = timer.words({0, 250000, 1, 500000}), got = timer.alloc(32);      // every 0.25 s, first after 1.5 s
    CHECK(timer.s(14, 0, timer.words({bit(kAlrm)}), 0, 8) == 0);
    CHECK(timer.s(38, 0, value, got) == 0 && timer.u64(got + 16) == 0 && timer.u64(got + 24) == 0);
    timer.host.now += 500000000;
    CHECK(timer.s(36, 0, got) == 0 && timer.u64(got) == 0 && timer.u64(got + 8) == 250000 && timer.u64(got + 16) == 1 && timer.u64(got + 24) == 0);
    CHECK(timer.s(128, timer.words({bit(kAlrm)}), 0, timer.words({0, 0}), 8) == -11);
    CHECK(timer.s(128, timer.words({bit(kAlrm)}), 0, timer.words({5, 0}), 8) == int64_t(kAlrm));      // the wait itself sees the timer run out
    CHECK(timer.host.now >= 2500000000ull && timer.host.now < 2600000000ull);
    CHECK(timer.s(36, 0, got) == 0 && timer.u64(got + 16) == 0 && timer.u64(got + 24) > 200000);     // armed again for the next period
    timer.host.now += 10000000000ull;
    CHECK(timer.s(127, got, 8) == 0 && timer.u64(got) == bit(kAlrm));                   // one signal however many periods passed
    CHECK(timer.s(38, 0, timer.words({0, 0, 0, 0}), 0) == 0 && timer.s(36, 0, got) == 0 && timer.u64(got + 16) == 0 && timer.u64(got + 24) == 0);
    CHECK(timer.s(38, 3, value, 0) == -22 && timer.s(36, 3, got) == -22 && timer.s(38, 0, kBad, 0) == -14 && timer.s(38, 0, value, kBad) == -14 && timer.s(36, 0, kBad) == -14);
    CHECK(timer.s(38, 0, timer.words({0, 1000000, 1, 0}), 0) == -22);                   // a million microseconds is not a fraction
    CHECK(timer.s(38, 1, value, 0) == 0 && timer.s(38, 2, value, 0) == 0);              // ITIMER_VIRTUAL, ITIMER_PROF
  }
  {      // RISC-V's sigaction has no restorer: handler, flags, mask (24 bytes)
    Rig arm(nullptr, 1, UserArch::RiscV64);
    const uint64_t act = arm.words({0x400100, 4, bit(kUsr2)}), old = arm.alloc(32);
    arm.put64(old + 24, 0x4755415244ull);
    CHECK(arm.s(134, kUsr1, act, 0, 8) == 0 && arm.s(134, kUsr1, 0, old, 8) == 0);
    CHECK(arm.u64(old) == 0x400100 && arm.u64(old + 8) == 4 && arm.u64(old + 16) == bit(kUsr2) && arm.u64(old + 24) == 0x4755415244ull);
    CHECK(arm.s(129, 1, kUsr1) == 0 && (arm.process->linux_state().pending & bit(kUsr1)));      // kill is 129 there
  }
}

// --- the process itself --------------------------------------------------------------------------------------------------------------------------------

static void test_identity() {
  Rig rig(nullptr, 42);
  const uint64_t a = rig.alloc(8), b = rig.alloc(8), c = rig.alloc(8);
  // root to begin with; what is set reads back
  CHECK(rig.s(102) == 0 && rig.s(107) == 0 && rig.s(104) == 0 && rig.s(108) == 0);
  CHECK(rig.s(118, a, b, c) == 0 && rig.u32(a) == 0 && rig.u32(b) == 0 && rig.u32(c) == 0);
  CHECK(rig.s(117, 1000, 1001, ~uint64_t{0}) == 0 && rig.s(118, a, b, c) == 0 && rig.u32(a) == 1000 && rig.u32(b) == 1001 && rig.u32(c) == 0);      // setresuid, -1 keeps
  CHECK(rig.s(102) == 1000 && rig.s(107) == 1001);
  CHECK(rig.s(105, 500) == 0 && rig.s(102) == 500 && rig.s(107) == 500 && rig.s(118, a, b, c) == 0 && rig.u32(c) == 500);
  CHECK(rig.s(113, ~uint64_t{0}, 7) == 0 && rig.s(102) == 500 && rig.s(107) == 7);      // setreuid
  CHECK(rig.s(106, 20) == 0 && rig.s(114, 21, ~uint64_t{0}) == 0 && rig.s(119, ~uint64_t{0}, 22, 23) == 0);
  CHECK(rig.s(120, a, b, c) == 0 && rig.u32(a) == 21 && rig.u32(b) == 22 && rig.u32(c) == 23 && rig.s(104) == 21 && rig.s(108) == 22);
  CHECK(rig.s(122, 9) == 7 && rig.s(122, ~uint64_t{0}) == 9 && rig.s(123, 8) == 22 && rig.s(123, 8) == 8);      // setfsuid, setfsgid answer the old one
  CHECK(rig.s(105, ~uint64_t{0}) == -22 && rig.s(118, kBad, b, c) == -14 && rig.s(120, a, b, kBad) == -14);
  // groups
  const uint64_t list = rig.alloc(32 * 4 + 8);
  CHECK(rig.s(115, 0, 0) == 0);
  rig.put32(list, 4); rig.put32(list + 4, 24); rig.put32(list + 8, 27);
  CHECK(rig.s(116, 3, list) == 0 && rig.s(115, 0, 0) == 3 && rig.s(115, 2, list) == -22);
  rig.put64(list, 0); rig.put64(list + 8, 0x4755415244ull << 32);
  CHECK(rig.s(115, 8, list) == 3 && rig.u32(list) == 4 && rig.u32(list + 4) == 24 && rig.u32(list + 8) == 27 && rig.u32(list + 12) == 0x55415244u);
  CHECK(rig.s(116, 33, list) == -22 && rig.s(116, 2, kBad) == -14 && rig.s(115, 3, kBad) == -14 && rig.s(116, 0, 0) == 0 && rig.s(115, 0, 0) == 0);
  // the process group and the session: its own to begin with
  CHECK(rig.s(39) == 42 && rig.s(110) == 0 && rig.s(111) == 42 && rig.s(121, 0) == 42 && rig.s(121, 42) == 42 && rig.s(124, 0) == 42);
  CHECK(rig.s(109, 0, 77) == 0 && rig.s(111) == 77 && rig.s(109, 42, 0) == 0 && rig.s(111) == 42);
  CHECK(rig.s(112) == 42 && rig.s(124, 42) == 42);
  CHECK(rig.s(121, 4242) == -3 && rig.s(109, 4242, 0) == -3 && rig.s(124, 4242) == -3 && rig.s(109, 0, ~uint64_t{0}) == -22);
  // capabilities: all of them; a version nobody knows is answered with the one to use
  const uint64_t header = rig.alloc(8), data = rig.alloc(24 + 8);
  rig.put32(header, 0x20080522); rig.put32(header + 4, 0);
  rig.put64(data + 24, 0x4755415244ull);
  CHECK(rig.s(125, header, data) == 0 && rig.u32(data) == 0xFFFFFFFFu && rig.u32(data + 4) == 0xFFFFFFFFu && rig.u32(data + 8) == 0 && rig.u32(data + 12) == 0x1FF && rig.u64(data + 24) == 0x4755415244ull);
  CHECK(rig.s(126, header, data) == 0 && rig.s(126, header, kBad) == -14);
  rig.put32(header, 0x19980330); rig.put64(data + 8, 0x1122334455667788ull);
  CHECK(rig.s(125, header, data) == 0 && rig.u32(data + 8) == 0 && rig.u32(data + 12) == 0x11223344u);      // version 1: one set, 12 bytes
  rig.put32(header, 0x1234);
  CHECK(rig.s(125, header, data) == -22 && rig.u32(header) == 0x20080522);
  rig.put32(header, 0);
  CHECK(rig.s(125, header, 0) == 0 && rig.u32(header) == 0x20080522);                  // how a library asks which version there is
  rig.put32(header + 4, 4242);
  CHECK(rig.s(125, header, data) == -3 && rig.s(125, kBad, data) == -14);
}

static void test_limits_and_scheduling() {
  Rig rig(nullptr, 42);
  const uint64_t lim = rig.alloc(16 + 8), buf = rig.alloc(256);
  constexpr uint64_t kInf = ~uint64_t{0};
  rig.put64(lim + 16, 0x4755415244ull);
  // getrlimit (97), setrlimit (160), prlimit64 (302)
  CHECK(rig.s(97, 7, lim) == 0 && rig.u64(lim) == 1024 && rig.u64(lim + 8) == 1024 && rig.u64(lim + 16) == 0x4755415244ull);      // RLIMIT_NOFILE: the table
  CHECK(rig.s(97, 3, lim) == 0 && rig.u64(lim) == (8u << 20) && rig.u64(lim + 8) == kInf);                                       // RLIMIT_STACK
  CHECK(rig.s(97, 4, lim) == 0 && rig.u64(lim) == 0 && rig.s(97, 9, lim) == 0 && rig.u64(lim) == kInf);                          // RLIMIT_CORE, RLIMIT_AS
  CHECK(rig.s(160, 7, rig.words({256, 512})) == 0 && rig.s(302, 0, 7, 0, lim) == 0 && rig.u64(lim) == 256 && rig.u64(lim + 8) == 512);
  CHECK(rig.s(302, 42, 7, rig.words({100, 512}), lim) == 0 && rig.u64(lim) == 256 && rig.s(97, 7, lim) == 0 && rig.u64(lim) == 100);      // new and old in one call
  CHECK(rig.s(160, 7, rig.words({600, 512})) == -22 && rig.s(160, 7, rig.words({100, 4096})) == -1 && rig.s(97, 16, lim) == -22 && rig.s(97, 7, kBad) == -14);
  CHECK(rig.s(160, 7, kBad) == -14 && rig.s(160, 7, 0) == -14 && rig.s(302, 4242, 7, 0, lim) == -3);
  CHECK(rig.s(302, 0, 7, rig.words({50, 512}), kBad) == -14 && rig.s(97, 7, lim) == 0 && rig.u64(lim) == 100);                    // a failed call changes nothing
  // getrusage (98), times (100), sysinfo (99)
  rig.put64(buf + 144, 0x4755415244ull);
  CHECK(rig.s(98, 0, buf) == 0 && rig.u64(buf + 32) > 0 && rig.u64(buf + 144) == 0x4755415244ull);      // ru_maxrss: the pages of this rig
  CHECK(rig.s(98, uint64_t(-1), buf) == 0 && rig.u64(buf + 32) == 0 && rig.s(98, 1, buf) == 0 && rig.s(98, 2, buf) == -22 && rig.s(98, 0, kBad) == -14);
  CHECK(rig.s(100, buf) == 100 && rig.s(100, 0) == 100 && rig.s(100, kBad) == -14);     // (one second since the start: 100 ticks)
  rig.put64(buf + 112, 0x4755415244ull);
  CHECK(rig.s(99, buf) == 0 && rig.u64(buf) == 1 && rig.u64(buf + 32) > 0 && rig.u64(buf + 40) <= rig.u64(buf + 32) && rig.u32(buf + 104) == 1 && rig.u64(buf + 112) == 0x4755415244ull);
  CHECK(rig.s(99, kBad) == -14);
  // nice
  CHECK(rig.s(140, 0, 0) == 20 && rig.s(141, 0, 0, 5) == 0 && rig.s(140, 0, 42) == 15 && rig.s(141, 0, 0, uint64_t(-50)) == 0 && rig.s(140, 0, 0) == 40);
  CHECK(rig.s(140, 3, 0) == -22 && rig.s(140, 0, 4242) == -3 && rig.s(141, 0, 4242, 1) == -3);
  // the scheduler's settings read back
  const uint64_t param = rig.alloc(8);
  CHECK(rig.s(145, 0) == 0 && rig.s(146, 1) == 99 && rig.s(147, 1) == 1 && rig.s(146, 0) == 0 && rig.s(147, 3) == 0 && rig.s(146, 9) == -22);
  rig.put32(param, 10);
  CHECK(rig.s(144, 0, 1, param) == 0 && rig.s(145, 42) == 1 && rig.s(143, 0, param) == 0 && rig.u32(param) == 10);      // SCHED_FIFO at 10
  rig.put32(param, 0);
  CHECK(rig.s(142, 0, param) == -22 && rig.s(144, 0, 0, param) == 0 && rig.s(145, 0) == 0);      // priority 0 is not one of SCHED_FIFO's
  CHECK(rig.s(144, 0, 1, param) == -22 && rig.s(144, 0, 7, param) == -22 && rig.s(144, 4242, 0, param) == -3 && rig.s(144, 0, 0, kBad) == -14 && rig.s(143, 0, 0) == -22);
  CHECK(rig.s(148, 0, buf) == 0 && rig.u64(buf) == 0 && rig.u64(buf + 8) == 100000000 && rig.s(148, 0, kBad) == -14);
  CHECK(rig.s(141, 0, 0, 3) == 0 && rig.s(315, 0, buf, 48, 0) == 0 && rig.u32(buf) == 48 && rig.u32(buf + 4) == 0 && rig.u32(buf + 16) == 3);      // sched_getattr
  CHECK(rig.s(315, 0, buf, 40, 0) == -22 && rig.s(315, 0, buf, 48, 1) == -22 && rig.s(315, 0, kBad, 48, 0) == -14);
  rig.put32(buf + 4, 3); rig.put32(buf + 16, uint32_t(-7));
  CHECK(rig.s(314, 0, buf, 0) == 0 && rig.s(145, 0) == 3 && rig.s(140, 0, 0) == 27 && rig.s(314, 0, kBad, 0) == -14 && rig.s(314, 0, buf, 1) == -22);      // sched_setattr: SCHED_BATCH, nice -7
  // affinity (203 set, 204 get), getcpu (309): one processor without a runtime
  rig.put64(buf, 1);
  CHECK(rig.s(203, 0, 8, buf) == 0 && rig.s(204, 0, 8, buf) == 8 && rig.u64(buf) == 1);
  rig.put64(buf, 2);
  CHECK(rig.s(203, 0, 8, buf) == -22 && rig.s(203, 0, 8, kBad) == -14 && rig.s(203, uint64_t(-1), 8, buf) == -22);
  rig.put64(buf, 0x1111111111111111ull);
  CHECK(rig.s(309, buf, buf + 4, 0) == 0 && rig.u64(buf) == 0 && rig.s(309, 0, 0, 0) == 0 && rig.s(309, kBad, 0, 0) == -14);
  // personality (135), membarrier (324), robust lists (273, 274)
  CHECK(rig.s(135, 0xFFFFFFFF) == 0 && rig.s(135, 0x0040000) == 0 && rig.s(135, 0xFFFFFFFF) == 0x0040000);
  CHECK(rig.s(324, 0, 0) == 25 && rig.s(324, 16, 0) == 0 && rig.s(324, 8, 0) == 0 && rig.s(324, 1, 0) == 0 && rig.s(324, 2, 0) == -22 && rig.s(324, 8, 1) == -22);
  CHECK(rig.s(273, 0x7000, 24) == 0 && rig.s(273, 0x7000, 16) == -22 && rig.s(274, 0, buf, buf + 8) == 0 && rig.u64(buf) == 0x7000 && rig.u64(buf + 8) == 24);
  CHECK(rig.s(274, 4242, buf, buf + 8) == -3 && rig.s(274, 0, kBad, buf) == -14);
  // clocks: the CPU-time ones, the coarse ones, boot time and TAI
  for (const uint64_t clock : {uint64_t{2}, uint64_t{3}, uint64_t{5}, uint64_t{6}, uint64_t{7}, uint64_t{11}}) CHECK(rig.s(228, clock, buf) == 0);
  CHECK(rig.s(228, 7, buf) == 0 && rig.u64(buf) == 1 && rig.s(228, 11, buf) == 0 && rig.u64(buf) == 1700000000ull + 1 + 37 && rig.s(228, 8, buf) == -22);
}

static void test_prctl() {
  filesystem::RamFileSystem fs(A);
  populate(fs);
  Rig rig(&fs);
  const uint64_t buf = rig.alloc(32);
  CHECK(rig.process->files().set_executable("/usr/bin/a-program-with-a-long-name"));
  rig.put64(buf + 16, 0x4755415244ull);
  CHECK(rig.s(157, 16, buf) == 0 && rig.bytes(buf, 16) == std::string("a-program-with-\0", 16) && rig.u64(buf + 16) == 0x4755415244ull);      // PR_GET_NAME: 15 characters of the file's name
  CHECK(rig.s(157, 15, rig.str("worker-thread-number-nine")) == 0 && rig.s(157, 16, buf) == 0 && rig.bytes(buf, 16) == std::string("worker-thread-n\0", 16));
  CHECK(rig.s(157, 15, rig.str("x")) == 0 && rig.s(157, 16, buf) == 0 && rig.bytes(buf, 16) == std::string("x\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16));
  CHECK(rig.s(157, 15, kBad) == -14 && rig.s(157, 16, kBad) == -14);
  CHECK(rig.space.write_privileged(0x30000 - 3, ConstByteView(reinterpret_cast<const uint8_t*>("abc"), 3)) && rig.s(157, 15, 0x30000 - 3) == -14);                                           // a name that runs off the mapping without its NUL
  CHECK(rig.s(157, 3) == 1 && rig.s(157, 4, 0) == 0 && rig.s(157, 3) == 0 && rig.s(157, 4, 2) == -22);      // dumpable
  CHECK(rig.s(157, 1, 15) == 0 && rig.s(157, 2, buf) == 0 && rig.u32(buf) == 15 && rig.s(157, 1, 65) == -22 && rig.s(157, 2, kBad) == -14);      // PDEATHSIG
  CHECK(rig.s(157, 39) == 0 && rig.s(157, 38, 1) == 0 && rig.s(157, 39) == 1 && rig.s(157, 38, 0) == -22);      // NO_NEW_PRIVS
  CHECK(rig.s(157, 30) == 50000 && rig.s(157, 29, 1000) == 0 && rig.s(157, 30) == 1000);      // timer slack
  CHECK(rig.s(157, 36, 1) == 0 && rig.s(157, 37, buf) == 0 && rig.u32(buf) == 1);      // child subreaper
  CHECK(rig.s(157, 23, 5) == 1 && rig.s(157, 23, 99) == -22 && rig.s(157, 21) == 0 && rig.s(157, 22, 1) == -22);
  CHECK(rig.s(157, 50, 0) == -22 && rig.s(157, 59, 0) == -22 && rig.s(157, 9999) == -22);      // the SVE call on x86-64; syscall user dispatch; nonsense
  Rig arm(nullptr, 1, UserArch::AArch64);
  CHECK(arm.s(167, 51) == 16 && arm.s(167, 63) == 16);
}

// --- fork and wait -----------------------------------------------------------------------------------------------------------------------------------------

// A runtime that says it can fork and lets the test play the children.
struct ForkRuntime final : IProcessRuntime {
  uint64_t next_pid = 500;
  struct Kid { uint64_t pid = 0; bool exited = false; uint32_t code = 0, signal = 0; bool known = true; };
  std::vector<Kid> kids;
  std::vector<std::pair<uint64_t, uint32_t>> terminated;
  bool can_fork = true;
  void lock() override {}
  void unlock() override {}
  bool wait(uint64_t) override { return true; }
  void notify() override {}
  bool interrupted() override { return false; }
  bool start_thread(const ThreadStart&, bool) override { return false; }
  bool resume_thread(uint64_t) override { return false; }
  uint64_t current_thread_id() override { return 1; }
  bool supports_linux_fork() override { return can_fork; }
  uint64_t allocate_process_id() override { return next_pid++; }
  Kid* find(uint64_t pid) { for (Kid& k : kids) if (k.pid == pid) return &k; return nullptr; }
  bool query_child(uint64_t pid, bool* exited, uint32_t* code) override {
    Kid* k = find(pid);
    if (!k || !k->known) return false;
    *exited = k->exited;
    *code = k->code;
    return true;
  }
  uint32_t child_exit_signal(uint64_t pid) override { Kid* k = find(pid); return k ? k->signal : 0; }
  bool process_exists(uint64_t pid) override { Kid* k = find(pid); return (k && !k->exited) || pid == 900; }
  bool terminate_process(uint64_t pid, uint32_t code) override {
    terminated.push_back({pid, code});
    Kid* k = find(pid);
    if (!k || k->exited) return false;
    k->exited = true;
    k->code = code;
    return true;
  }
};

static void test_fork_and_wait() {
  constexpr uint64_t kSigChld = 17, kCloneVm = 0x100, kCloneVfork = 0x4000, kCloneThread = 0x10000, kCloneParentSettid = 0x100000, kCloneChildCleartid = 0x200000,
                     kCloneChildSettid = 0x1000000, kCloneSettls = 0x80000, kCloneFiles = 0x400, kCloneNewns = 0x20000;
  {      // nobody to make the child: ENOSYS, and nothing is left asking for one
    Rig rig;
    LinuxProcessState::ForkRequest request;
    CHECK(rig.s(57) == -38 && rig.s(58) == -38 && rig.s(56, kSigChld, 0, 0, 0, 0) == -38 && !rig.process->take_fork(&request));
    ForkRuntime none;
    none.can_fork = false;
    rig.process->attach_runtime(&none);
    CHECK(rig.s(57) == -38 && !rig.process->take_fork(&request));
    CHECK(rig.s(61, uint64_t(-1), 0, 0, 0) == -10);                                    // wait4: ECHILD
    rig.process->attach_runtime(nullptr);
  }
  Rig rig;
  ForkRuntime runtime;
  rig.process->attach_runtime(&runtime);
  Process& p = *rig.process;
  LinuxProcessState::ForkRequest request;
  auto play = [&](uint64_t pid) { runtime.kids.push_back(ForkRuntime::Kid{pid}); };
  const uint64_t status = rig.alloc(16), usage = rig.alloc(144 + 8), word = rig.alloc(8);

  // fork (57): the child's id is the answer and a request waits for the runtime; a second fork before it was taken is refused
  CHECK(rig.s(57) == 500 && rig.s(57) == -11);
  CHECK(p.take_fork(&request) && request.pid == 500 && request.stack == 0 && !request.set_tls && request.set_child_tid == 0 && request.caller_tid == 1 && !p.take_fork(&request));
  play(500);
  // what glibc's fork() calls: clone(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD, 0, 0, &tid, 0)
  CHECK(rig.s(56, kCloneChildSettid | kCloneChildCleartid | kSigChld, 0, 0, 0x7100, 0) == 501 && p.take_fork(&request) && request.set_child_tid == 0x7100 && request.clear_child_tid == 0x7100);
  play(501);
  // what posix_spawn calls: CLONE_VM | CLONE_VFORK | SIGCHLD with a stack; and with the child's id stored for the parent, and a thread pointer
  CHECK(rig.s(56, kCloneVm | kCloneVfork | kSigChld, 0x7F00, 0, 0, 0) == 502 && p.take_fork(&request) && request.stack == 0x7F00);
  play(502);
  CHECK(rig.s(56, kCloneParentSettid | kCloneSettls | kSigChld, 0, word, 0, 0x9000) == 503 && rig.u32(word) == 503 && p.take_fork(&request) && request.set_tls && request.tls == 0x9000);
  play(503);
  CHECK(rig.s(56, kCloneParentSettid | kSigChld, 0, kBad, 0, 0) == -14 && !p.take_fork(&request));      // the id cannot be stored: no child
  // clone3 (435) with an exit signal and no CLONE_THREAD is a fork too
  const uint64_t args = rig.words({0, 0, 0, 0, kSigChld, 0x60000, 0x1000, 0, 0, 0, 0});
  CHECK(rig.s(435, args, 88) == 505 && p.take_fork(&request) && request.pid == 505 && request.stack == 0x61000);
  play(505);
  // refused: sharing the descriptors without being a thread, a namespace, a thread with an exit signal, a pidfd
  CHECK(rig.s(56, kCloneFiles | kSigChld, 0, 0, 0, 0) == -22 && rig.s(56, kCloneNewns | kSigChld, 0, 0, 0, 0) == -22 && rig.s(56, 0x1000 | kSigChld, 0, 0, 0, 0) == -22);
  CHECK(rig.s(435, rig.words({kCloneThread | kCloneVm, 0, 0, 0, kSigChld, 0, 0, 0}), 64) == -22 && rig.s(435, rig.words({0, 0, 0, 0, 65, 0, 0, 0}), 64) == -22);
  CHECK(!p.take_fork(&request));

  // wait4 (61): nothing has ended; then by id, any, and how each ended
  CHECK(rig.s(61, uint64_t(-1), status, 1, 0) == 0 && rig.s(61, 500, status, 1, 0) == 0);      // WNOHANG
  CHECK(rig.s(61, 4242, status, 1, 0) == -10 && rig.s(61, uint64_t(-1), status, 0x10, 0) == -22);
  runtime.find(501)->exited = true;
  runtime.find(501)->code = 7;
  rig.put64(usage + 144, 0x4755415244ull);
  rig.put64(status, 0x1111111111111111ull);
  CHECK(rig.s(61, 500, status, 1, 0) == 0);
  CHECK(rig.s(61, uint64_t(-1), status, 0, usage) == 501 && rig.u64(status) == (0x1111111100000000ull | 7 << 8) && rig.u64(usage + 144) == 0x4755415244ull);
  CHECK(rig.s(61, 501, status, 0, 0) == -10);                                          // waited for: gone
  runtime.find(500)->exited = true;
  runtime.find(500)->code = 0x1FF;                                                     // (only the low byte of an exit code is the parent's to see)
  CHECK(rig.s(61, 500, kBad, 0, 0) == -14 && rig.s(61, 500, 0, 0, 0) == -10);           // (a status that cannot be stored: the child is gone all the same, as on Linux)
  // kill (62) of a child ends it where the default action would; wait then says which signal
  CHECK(rig.s(62, 502, 0) == 0 && rig.s(62, 502, 28) == 0 && runtime.terminated.empty());      // 0 asks; SIGWINCH would be ignored
  CHECK(rig.s(62, 502, 15) == 0 && runtime.terminated.size() == 1 && runtime.terminated[0].first == 502 && runtime.terminated[0].second == 143);
  CHECK(rig.s(62, 502, 15) == 0 && runtime.terminated.size() == 1);                     // (a zombie takes any signal)
  CHECK(rig.s(61, 502, status, 0, 0) == 502 && rig.u32(status) == 15);
  CHECK(rig.s(62, 502, 0) == -3 && rig.s(62, 900, 0) == 0 && rig.s(62, 900, 15) == -1);  // gone; someone else's process: there, but not ours to end
  // a child that ended itself with a signal (the runtime knows which)
  runtime.find(503)->exited = true;
  runtime.find(503)->code = 134;
  runtime.find(503)->signal = 6;
  // waitid (247): P_PID 1, P_ALL 0; WEXITED 4, WNOHANG 1, WNOWAIT 0x1000000
  const uint64_t info = rig.alloc(128 + 8);
  rig.put64(info + 128, 0x4755415244ull);
  CHECK(rig.s(247, 1, 505, info, 4 | 1, 0) == 0 && rig.u32(info) == 0 && rig.u32(info + 16) == 0);      // nothing yet: a zeroed siginfo
  CHECK(rig.s(247, 0, 0, info, 4 | 0x1000000, 0) == 0 && rig.u32(info) == 17 && rig.u32(info + 8) == 2 && rig.u32(info + 16) == 503 && rig.u32(info + 24) == 6);      // CLD_KILLED
  CHECK(rig.u64(info + 128) == 0x4755415244ull);
  CHECK(rig.s(247, 1, 503, info, 4, usage) == 0 && rig.u32(info + 16) == 503 && rig.s(247, 1, 503, info, 4, 0) == -10);      // WNOWAIT left it; now it is taken
  CHECK(rig.s(247, 1, 505, info, 0, 0) == -22 && rig.s(247, 3, 505, info, 4, 0) == -22 && rig.s(247, 1, 0, info, 4, 0) == -22 && rig.s(247, 1, 505, info, 4 | 0x10, 0) == -22);
  CHECK(rig.s(247, 1, 505, info, 2 | 1, 0) == 0 && rig.s(247, 1, 505, info, 2, 0) == -22);      // only stops asked for: there are none
  // a child whose record the runtime let go of: how it ended was noted first (as the kernel does when it frees one)
  linuxabi::note_child_exit(p, 505, 9, 0);
  runtime.find(505)->known = false;
  CHECK(rig.s(247, 1, 505, info, 4, 0) == 0 && rig.u32(info + 8) == 1 && rig.u32(info + 24) == 9);      // CLD_EXITED
  CHECK(rig.s(61, uint64_t(-1), status, 0, 0) == -10);
  // a fork the runtime could not make is forgotten
  CHECK(rig.s(57) == 506 && p.take_fork(&request));
  linuxabi::fork_failed(p, request.pid);
  CHECK(rig.s(61, uint64_t(-1), status, 1, 0) == -10);
  // more children than the table holds
  for (uint32_t i = 0; i < LinuxProcessState::kMaxChildren; ++i) {
    CHECK(rig.s(57) > 0 && p.take_fork(&request));
    play(request.pid);
  }
  CHECK(rig.s(57) == -11 && !p.take_fork(&request));
  CHECK(rig.s(302, runtime.kids.back().pid, 7, 0, usage) == -1 && rig.s(121, runtime.kids.back().pid) == 1 && rig.s(109, runtime.kids.back().pid, 0) == 0);      // (a child: not ours to set limits for; in our group)
  rig.process->attach_runtime(nullptr);
}

// What the child of a fork starts with.
static void test_fork_inheritance() {
  filesystem::RamFileSystem fs(A);
  populate(fs);
  static NoWaitSync sync;
  ObjectManager objects(sync);
  objects.set_allocator(&A);
  UnixSocketManager sockets(sync, A);
  set_system_objects(&objects);
  set_system_unix_sockets(&sockets);
  {
    Rig parent(&fs, 10);
    Process& p = *parent.process;
    auto bit = [](uint64_t signal) { return uint64_t{1} << (signal - 1); };
    // the parent: a file at a position, a close-on-exec descriptor, a pipe, a folder it stands in, handlers, a mask, ids, a limit, a name, a pending signal, a timer
    const int64_t file = parent.s(2, parent.str("/etc/motd"), 0), cloexec = parent.s(2, parent.str("/etc/motd"), 0x80000);
    const uint64_t pipe = parent.alloc(8), buf = parent.alloc(64);
    CHECK(file >= 3 && cloexec >= 3 && parent.s(293, pipe, 0) == 0 && parent.s(0, file, buf, 3) == 3 && parent.s(80, parent.str("/tmp")) == 0);
    const uint64_t read_end = parent.u32(pipe), write_end = parent.u32(pipe + 4);
    CHECK(parent.s(3, 0) == 0);                                                        // (standard input closed: the child must not get a new one)
    CHECK(parent.s(13, 10, parent.words({0x400100, 0, 0, 0}), 0, 8) == 0 && parent.s(13, 15, parent.words({1, 0, 0, 0}), 0, 8) == 0);
    CHECK(parent.s(14, 2, parent.words({bit(10) | bit(14)}), 0, 8) == 0 && parent.s(62, 10, 10) == 0 && p.linux_state().pending == bit(10));
    CHECK(parent.s(117, 1000, 1000, 1000) == 0 && parent.s(160, 7, parent.words({64, 128})) == 0 && parent.s(157, 15, parent.str("parent")) == 0 && parent.s(37, 100) == 0);
    CHECK(parent.s(95, 077) == 022 && parent.s(131, parent.words({0x500000, 0, 65536}), 0) == 0);
    p.set_memory_cursors(0x123000, 0x7F0000100000ull);

    Rig child(&fs, 11);
    Process& c = *child.process;
    LinuxProcessState::ForkRequest request;
    request.pending = true;
    request.pid = 11;
    request.caller_tid = 10;
    request.set_child_tid = 0x10800;
    request.clear_child_tid = 0x10808;
    linuxabi::inherit_fork_state(c, p, request);
    // descriptors: the same open files (one position), flags and all; what was closed stays closed
    CHECK(child.s(0, file, buf, 5) == 5 && child.bytes(buf, 5) == "come\n" && parent.s(8, file, 0, 1) == 8);
    CHECK(child.s(72, cloexec, 1) == 1 && child.s(72, file, 1) == 0 && child.s(72, 0, 1) == -9 && child.s(72, 1, 1) == 0);
    CHECK(parent.s(1, write_end, parent.str("ping"), 4) == 4 && child.s(0, read_end, buf, 16) == 4 && child.bytes(buf, 4) == "ping");
    CHECK(child.s(3, write_end) == 0 && parent.s(3, write_end) == 0 && parent.s(0, read_end, buf, 16) == 0);      // the pipe ends when both writers are gone
    CHECK(child.s(79, buf, 64) == 5 && child.bytes(buf, 5) == std::string("/tmp\0", 5));                          // getcwd
    // signals: the handlers and the caller's mask and stack, nothing pending, no timer
    const uint64_t old = child.alloc(32), set = child.alloc(8), stack = child.alloc(24);
    CHECK(child.s(13, 10, 0, old, 8) == 0 && child.u64(old) == 0x400100 && child.s(13, 15, 0, old, 8) == 0 && child.u64(old) == 1);
    CHECK(child.s(14, 0, 0, set, 8) == 0 && child.u64(set) == (bit(10) | bit(14)) && child.s(127, set, 8) == 0 && child.u64(set) == 0);
    CHECK(child.s(131, 0, stack) == 0 && child.u64(stack) == 0x500000 && child.s(37, 0) == 0 && parent.s(37, 0) == 100);
    // ids, limits, the name, the mask for new files; the parent, and the parent's group and session
    CHECK(child.s(102) == 1000 && child.s(97, 7, old) == 0 && child.u64(old) == 64 && child.s(157, 16, buf) == 0 && child.bytes(buf, 6) == "parent" && child.s(95, 022) == 077);
    CHECK(child.s(39) == 11 && child.s(110) == 10 && child.s(111) == 10 && child.s(124, 0) == 10 && child.s(112) == 11 && child.s(111) == 11);
    CHECK(c.heap_break() == 0x123000 && c.mmap_cursor() == 0x7F0000100000ull);
    // the child's own id where it asked for it, and its first thread's clear-on-exit word
    CHECK(child.u32(0x10800) == 11 && c.linux_threads().threads[0].clear_child_tid == 0x10808);
    // no children of its own, and nothing of the fork left over
    CHECK(child.s(61, uint64_t(-1), 0, 1, 0) == -10 && !c.take_fork(&request));
    // what the child changes is its own
    CHECK(child.s(105, 0) == 0 && parent.s(102) == 1000 && child.s(80, child.str("/etc")) == 0 && parent.s(79, buf, 64) == 5);
  }
  set_system_unix_sockets(nullptr);
  set_system_objects(nullptr);
}

// Handlers: the frame a handler is entered with, the way back (rt_sigreturn), calls that start again, faults, and what a program can do to the frame.
static void test_signal_handlers() {
  constexpr uint64_t kUsr1 = 10, kUsr2 = 12, kSegv = 11, kChld = 17, kRestorer = 0x04000000, kSigInfo = 4, kRestart = 0x10000000, kOnStack = 0x08000000;
  constexpr uint64_t kHandler = 0x400100, kReturn = 0x400200, kStackTop = 0x2F000;      // (the rig's two pages of memory are the stack too)
  auto bit = [](uint64_t signal) { return uint64_t{1} << (signal - 1); };
  static NoWaitSync sync;
  ObjectManager objects(sync);
  objects.set_allocator(&A);
  UnixSocketManager sockets(sync, A);
  set_system_objects(&objects);
  set_system_unix_sockets(&sockets);
  // A thread's registers with something to recognise in each.
  auto registers = [&](uint64_t stack) {
    X86Context c;
    for (uint32_t i = 0; i < 16; ++i) c.gpr[i] = 0x1111111111111111ull * (i + 1);
    c.gpr[X86Context::kRsp] = stack;
    c.rip = 0x400502;
    c.rflags = 0x246 | 0x400;      // (with the direction flag set)
    for (uint32_t i = 0; i < 512; ++i) c.fpu[i] = uint8_t(i * 7 + 1);
    c.fpu[26] = c.fpu[27] = 0;
    return c;
  };
  struct Running {
    Rig rig;
    ForkRuntime runtime;
    Running() { rig.process->attach_runtime(&runtime); rig.process->linux_state().handlers_run = true; }
    ~Running() { rig.process->attach_runtime(nullptr); }
  };
  {
    Running run;
    Rig& rig = run.rig;
    Process& p = *rig.process;
    CHECK(rig.s(13, kUsr1, rig.words({kHandler, kRestorer | kSigInfo, kReturn, bit(kUsr2)}), 0, 8) == 0);
    CHECK(!linuxabi::signals_wanted(p) && !linuxabi::signal_interrupts(p) && !linuxabi::signal_hint(p, 1, 0));
    // kill() of itself: the call returns, and the handler is entered on the way out
    CHECK(rig.s(62, 1, kUsr1) == 0 && !p.exited() && linuxabi::signals_wanted(p) && linuxabi::signal_interrupts(p) && linuxabi::signal_hint(p, 1, 0));
    const X86Context before = registers(kStackTop);
    X86Context c = before;
    CHECK(linuxabi::deliver_signals(p, &c, nullptr));
    const uint64_t frame = c.gpr[X86Context::kRsp], uc = frame + 8, info = uc + 304;
    CHECK(c.rip == kHandler && c.gpr[X86Context::kRdi] == kUsr1 && c.gpr[X86Context::kRsi] == info && c.gpr[X86Context::kRdx] == uc && c.gpr[X86Context::kRax] == 0);
    CHECK(frame % 16 == 8 && frame + 8 + 304 + 128 <= kStackTop - 128 - 512 && (c.rflags & 0x400) == 0);      // below the red zone and the FPU image; DF clear
    CHECK(rig.u64(frame) == kReturn);
    CHECK(rig.u64(uc + 40) == before.gpr[X86Context::kR8] && rig.u64(uc + 40 + 8 * 8) == before.gpr[X86Context::kRdi] && rig.u64(uc + 40 + 13 * 8) == before.gpr[X86Context::kRax]);
    CHECK(rig.u64(uc + 40 + 15 * 8) == kStackTop && rig.u64(uc + 40 + 16 * 8) == before.rip && rig.u64(uc + 40 + 17 * 8) == before.rflags);
    CHECK(rig.u64(uc + 296) == 0 && rig.u32(uc + 24) == 2);                           // the mask to go back to; no alternate stack (SS_DISABLE)
    const uint64_t fpu_at = rig.u64(uc + 40 + 184);
    CHECK(fpu_at % 64 == 0 && fpu_at >= info + 128 && fpu_at + 512 <= kStackTop - 128 && rig.bytes(fpu_at, 512) == std::string(reinterpret_cast<const char*>(before.fpu), 512));
    CHECK(rig.u32(info) == kUsr1 && rig.u32(info + 8) == 0 && rig.u32(info + 16) == 1);      // si_signo, SI_USER, si_pid
    const uint64_t mask = rig.alloc(8);
    CHECK(rig.s(14, 0, 0, mask, 8) == 0 && rig.u64(mask) == (bit(kUsr1) | bit(kUsr2)));     // blocked while the handler runs
    CHECK(!linuxabi::signals_wanted(p));
    // the handler returns: rt_sigreturn (15) with the stack pointer just past the return address
    X86Context back = registers(0);
    back.gpr[X86Context::kRsp] = uc;
    CHECK(rig.s(15) == 0 && p.take_signal_return() && !p.take_signal_return());
    CHECK(linuxabi::signal_return(p, &back));
    for (uint32_t i = 0; i < 16; ++i) CHECK(back.gpr[i] == before.gpr[i]);
    CHECK(back.rip == before.rip && back.rflags == before.rflags && memcmp(back.fpu, before.fpu, 512) == 0);
    CHECK(rig.s(14, 0, 0, mask, 8) == 0 && rig.u64(mask) == 0);

    // a handler changes what the program goes back to; what it must not be able to set is not taken from it
    CHECK(rig.s(62, 1, kUsr1) == 0);
    c = before;
    CHECK(linuxabi::deliver_signals(p, &c, nullptr));
    rig.put64(uc + 40 + 16 * 8, 0x400900);                                              // rip
    rig.put64(uc + 40 + 17 * 8, ~uint64_t{0});                                          // every flag
    rig.put64(uc + 296, ~uint64_t{0});                                                  // every signal blocked
    rig.put32(fpu_at + 24, 0xFFFF1F80u);                                                // MXCSR with reserved bits
    back.gpr[X86Context::kRsp] = uc;
    CHECK(linuxabi::signal_return(p, &back) && back.rip == 0x400900);
    CHECK((back.rflags & 0x3000) == 0 && (back.rflags & 0x200) != 0 && (back.rflags & 0x4000) == 0 && (back.rflags & 0xCD5) == 0xCD5);      // no I/O privilege, interrupts on, no nested task
    CHECK(back.fpu[24] == 0x80 && back.fpu[25] == 0x1F && back.fpu[26] == 0 && back.fpu[27] == 0);
    CHECK(rig.s(14, 0, 0, mask, 8) == 0 && rig.u64(mask) == (~uint64_t{0} & ~(bit(9) | bit(19))));      // SIGKILL and SIGSTOP stay unblocked
    CHECK(rig.s(14, 2, rig.words({0}), 0, 8) == 0);
    // a frame that says to go on outside the program's addresses, or that is not there, or whose FPU image is not: refused
    for (const uint64_t bad_rip : {uint64_t{0x0000800000000000ull}, uint64_t{0xFFFF800000001000ull}, ~uint64_t{0}}) {
      rig.put64(uc + 40 + 16 * 8, bad_rip);
      back.gpr[X86Context::kRsp] = uc;
      CHECK(!linuxabi::signal_return(p, &back));
    }
    rig.put64(uc + 40 + 16 * 8, 0x400900);
    rig.put64(uc + 40 + 184, kBad);
    back.gpr[X86Context::kRsp] = uc;
    CHECK(!linuxabi::signal_return(p, &back));
    rig.put64(uc + 40 + 184, 0);                                                        // no FPU image at all is fine: the state stays
    back = registers(uc);
    CHECK(linuxabi::signal_return(p, &back) && back.fpu[100] == registers(0).fpu[100]);
    for (const uint64_t bad_stack : {kBad, uint64_t{0}, uint64_t{0x30000 - 100}, ~uint64_t{0} - 8, uint64_t{0x0000800000000000ull}}) {
      back.gpr[X86Context::kRsp] = bad_stack;
      CHECK(!linuxabi::signal_return(p, &back));
    }
    // any bytes at all where a frame should be: nothing but a refusal or registers inside the program's reach
    uint32_t seed = 12345;
    for (int round = 0; round < 2000; ++round) {
      uint8_t junk[304];
      for (uint8_t& b : junk) { seed = seed * 1664525u + 1013904223u; b = uint8_t(seed >> 24); }
      if (round % 3 == 0) memset(junk + 40 + 16 * 8 + 6, 0, 2);      // (often with an instruction pointer that could be a user address)
      if (round % 2 == 0) memset(junk + 40 + 184, 0, 8);             // (and often without an FPU image)
      CHECK(rig.space.write_privileged(0x20000, ConstByteView(junk, sizeof(junk))));
      back = registers(0x20000);
      if (linuxabi::signal_return(p, &back)) CHECK(back.rip < AddressSpace::kUserTop && (back.rflags & 0x3000) == 0 && (back.rflags & 0x200) != 0);
    }
    CHECK(rig.s(14, 2, rig.words({0}), 0, 8) == 0 && !p.exited());
  }
  {      // a call that was cut short starts again after the handler when the action says so and the call is one that does
    SyscallRequest read_call, sleep_call, futex_call;
    read_call.number = 0;
    sleep_call.number = 35;
    futex_call.number = 202;
    CHECK(linuxabi::call_restarts(UserArch::X86_64, read_call) && !linuxabi::call_restarts(UserArch::X86_64, sleep_call) && linuxabi::call_restarts(UserArch::X86_64, futex_call));
    futex_call.args[3] = 0x7000;      // with a timeout
    CHECK(!linuxabi::call_restarts(UserArch::X86_64, futex_call));
    read_call.number = 63;            // read on the other CPUs
    CHECK(linuxabi::call_restarts(UserArch::AArch64, read_call) && !linuxabi::call_restarts(UserArch::X86_64, read_call));
    for (const bool restart : {true, false}) {
      for (const bool restartable : {true, false}) {
        Running run;
        Rig& rig = run.rig;
        CHECK(rig.s(13, kUsr1, rig.words({kHandler, kRestorer | (restart ? kRestart : 0), kReturn, 0}), 0, 8) == 0 && rig.s(62, 1, kUsr1) == 0);
        X86Context c = registers(kStackTop);
        c.gpr[X86Context::kRax] = uint64_t(-4);      // the call's answer: EINTR
        const linuxabi::InterruptedCall call{0, restartable};
        CHECK(linuxabi::deliver_signals(*rig.process, &c, &call));
        const uint64_t uc = c.gpr[X86Context::kRsp] + 8;
        const bool again = restart && restartable;
        CHECK(rig.u64(uc + 40 + 13 * 8) == (again ? 0 : uint64_t(-4)) && rig.u64(uc + 40 + 16 * 8) == (again ? 0x400500 : 0x400502));      // the call's number, at the SYSCALL instruction
      }
    }
  }
  {      // no way to enter the handler: no restorer, a handler that is no user address, no stack. The program ends by SIGSEGV.
    const uint64_t actions[][3] = {{kHandler, kSigInfo, 0}, {kHandler, kRestorer, 0}, {0x0000800000000000ull, kRestorer, kReturn}, {~uint64_t{7}, kRestorer, kReturn}};
    for (const auto& action : actions) {
      Running run;
      Rig& rig = run.rig;
      CHECK(rig.s(13, kUsr1, rig.words({action[0], action[1], action[2], 0}), 0, 8) == 0 && rig.s(62, 1, kUsr1) == 0);
      X86Context c = registers(kStackTop);
      CHECK(!linuxabi::deliver_signals(*rig.process, &c, nullptr) && rig.process->exited() && rig.process->exit_code() == 139 && rig.process->linux_state().exit_signal == kSegv);
    }
    for (const uint64_t stack : {kBad, uint64_t{0}, uint64_t{100}, uint64_t{0x10000 + 600}, ~uint64_t{0}, uint64_t{0x0000800000000000ull}}) {
      Running run;
      Rig& rig = run.rig;
      CHECK(rig.s(13, kUsr1, rig.words({kHandler, kRestorer, kReturn, 0}), 0, 8) == 0 && rig.s(62, 1, kUsr1) == 0);
      X86Context c = registers(stack);
      CHECK(!linuxabi::deliver_signals(*rig.process, &c, nullptr) && rig.process->exited() && rig.process->exit_code() == 139);
    }
  }
  {      // faults: the handler is told where and why; with none, or with the signal blocked, the program ends
    Running run;
    Rig& rig = run.rig;
    Process& p = *rig.process;
    CHECK(rig.s(13, kSegv, rig.words({kHandler, kRestorer | kSigInfo, kReturn, 0}), 0, 8) == 0);
    X86Context c = registers(kStackTop);
    CHECK(linuxabi::deliver_fault(p, &c, 14, 6, 0x1234) && c.rip == kHandler && c.gpr[X86Context::kRdi] == kSegv);
    const uint64_t uc = c.gpr[X86Context::kRsp] + 8, info = uc + 304;
    CHECK(rig.u32(info) == kSegv && rig.u32(info + 8) == 1 && rig.u64(info + 16) == 0x1234);      // SEGV_MAPERR, si_addr
    CHECK(rig.u64(uc + 40 + 152) == 6 && rig.u64(uc + 40 + 160) == 14 && rig.u64(uc + 40 + 176) == 0x1234 && rig.u64(uc + 40 + 16 * 8) == 0x400502);      // err, trapno, cr2, rip
    c = registers(kStackTop);
    CHECK(!linuxabi::deliver_fault(p, &c, 14, 7, 0x1234) && p.exited() && p.exit_code() == 139);      // (SIGSEGV is blocked while its handler runs)
    Running plain;
    c = registers(kStackTop);
    CHECK(!linuxabi::deliver_fault(*plain.rig.process, &c, 0, 0, 0) && plain.rig.process->exited() && plain.rig.process->exit_code() == 136 && plain.rig.process->linux_state().exit_signal == 8);
    Running codes;
    CHECK(codes.rig.s(13, 8, codes.rig.words({kHandler, kRestorer, kReturn, 0}), 0, 8) == 0 && codes.rig.s(13, 4, codes.rig.words({kHandler, kRestorer, kReturn, 0}), 0, 8) == 0);
    c = registers(kStackTop);
    CHECK(linuxabi::deliver_fault(*codes.rig.process, &c, 0, 0, 0) && c.gpr[X86Context::kRdi] == 8 && codes.rig.u32(c.gpr[X86Context::kRsi] + 8) == 1 && codes.rig.u64(c.gpr[X86Context::kRsi] + 16) == 0x400502);
    c = registers(kStackTop);
    CHECK(linuxabi::deliver_fault(*codes.rig.process, &c, 6, 0, 0) && c.gpr[X86Context::kRdi] == 4 && codes.rig.u32(c.gpr[X86Context::kRsi] + 8) == 2);
  }
  {      // signals from outside the process's own calls: posted without its lock, found at the next look; an ignored one is dropped
    Running run;
    Rig& rig = run.rig;
    Process& p = *rig.process;
    p.post_linux_signal(kChld);      // (ignored by default)
    CHECK(linuxabi::signal_hint(p, 1, 0) && !linuxabi::signal_interrupts(p) && !linuxabi::signals_wanted(p));
    p.post_linux_signal(0);
    p.post_linux_signal(65);
    CHECK(!linuxabi::signals_wanted(p));
    CHECK(rig.s(13, kUsr1, rig.words({kHandler, kRestorer, kReturn, 0}), 0, 8) == 0);
    const uint64_t set = rig.words({bit(kUsr1)}), buf = rig.alloc(32);
    CHECK(rig.s(14, 0, set, 0, 8) == 0);
    p.post_linux_signal(kUsr1);
    CHECK(!linuxabi::signal_interrupts(p) && rig.s(127, buf, 8) == 0 && rig.u64(buf) == bit(kUsr1));      // blocked: pending, nobody is interrupted
    CHECK(!linuxabi::signal_hint(p, 1, 0) && linuxabi::signal_hint(p, 2, 0));          // (another thread would not block it)
    // sigsuspend lets it in: the call ends, its mask stays until the handler has been entered, and the handler goes back to the mask from before
    CHECK(rig.s(130, rig.words({bit(kUsr2)}), 8) == -4);
    X86Context c = registers(kStackTop);
    CHECK(linuxabi::deliver_signals(p, &c, nullptr) && rig.u64(c.gpr[X86Context::kRsp] + 8 + 296) == bit(kUsr1));
    CHECK(rig.s(14, 0, 0, buf, 8) == 0 && rig.u64(buf) == (bit(kUsr1) | bit(kUsr2)));
    X86Context back = registers(c.gpr[X86Context::kRsp] + 8);
    CHECK(linuxabi::signal_return(p, &back) && rig.s(14, 0, 0, buf, 8) == 0 && rig.u64(buf) == bit(kUsr1));
    CHECK(rig.s(14, 2, rig.words({0}), 0, 8) == 0);
    // a call that waits is cut short: a read of an empty pipe, a sleep (which says how long was left), wait4 with a child that runs
    const uint64_t fds = rig.alloc(8);
    CHECK(rig.s(293, fds, 0) == 0);
    p.post_linux_signal(kUsr1);
    CHECK(rig.s(0, rig.u32(fds), buf, 1) == -4);
    CHECK(rig.s(35, rig.words({5, 0}), buf) == -4 && rig.u64(buf) == 5 && rig.u64(buf + 8) == 0);
    CHECK(rig.s(57) > 0);
    LinuxProcessState::ForkRequest request;
    CHECK(p.take_fork(&request));
    run.runtime.kids.push_back(ForkRuntime::Kid{request.pid});
    CHECK(rig.s(61, uint64_t(-1), 0, 0, 0) == -4);
    c = registers(kStackTop);
    CHECK(linuxabi::deliver_signals(p, &c, nullptr) && !linuxabi::signals_wanted(p));
    // a signal of another process is handed over where that can be done, and ends the other process where it cannot
    CHECK(rig.s(62, request.pid, kUsr1) == 0 && run.runtime.terminated.size() == 1 && run.runtime.terminated[0].second == 128 + kUsr1);      // (this runtime hands nothing over)
    CHECK(rig.s(3, rig.u32(fds)) == 0 && rig.s(3, rig.u32(fds + 4)) == 0);
  }
  {      // the alternate stack: used by an action that asks for it, unless the thread is on it already; a thread made by clone() starts with its maker's mask
    Running run;
    Rig& rig = run.rig;
    Process& p = *rig.process;
    constexpr uint64_t kAlt = 0x18000, kAltSize = 0x4000;
    CHECK(rig.s(131, rig.words({kAlt, 0, kAltSize}), 0) == 0);
    CHECK(rig.s(13, kUsr1, rig.words({kHandler, kRestorer | kOnStack, kReturn, 0}), 0, 8) == 0 && rig.s(13, kUsr2, rig.words({kHandler, kRestorer, kReturn, 0}), 0, 8) == 0);
    CHECK(rig.s(62, 1, kUsr1) == 0);
    X86Context c = registers(kStackTop);
    CHECK(linuxabi::deliver_signals(p, &c, nullptr) && c.gpr[X86Context::kRsp] > kAlt && c.gpr[X86Context::kRsp] < kAlt + kAltSize - 512);
    CHECK(rig.u64(c.gpr[X86Context::kRsp] + 8 + 16) == kAlt && rig.u32(c.gpr[X86Context::kRsp] + 8 + 24) == 0 && rig.u64(c.gpr[X86Context::kRsp] + 8 + 32) == kAltSize);
    const uint64_t on_alt = c.gpr[X86Context::kRsp];
    CHECK(rig.s(14, 2, rig.words({0}), 0, 8) == 0 && rig.s(62, 1, kUsr1) == 0);
    CHECK(linuxabi::deliver_signals(p, &c, nullptr) && c.gpr[X86Context::kRsp] < on_alt - 128 && c.gpr[X86Context::kRsp] > kAlt && rig.u32(c.gpr[X86Context::kRsp] + 8 + 24) == 1);      // nested: further down the same stack (SS_ONSTACK)
    CHECK(rig.s(62, 1, kUsr2) == 0);
    c = registers(kStackTop);
    CHECK(linuxabi::deliver_signals(p, &c, nullptr) && c.gpr[X86Context::kRsp] > kAlt + kAltSize);      // an action that does not ask stays on the thread's stack
    CHECK(rig.s(14, 2, rig.words({bit(kUsr2) | bit(15)}), 0, 8) == 0);
    linuxabi::inherit_thread_mask(p, 77);
    bool found = false;
    for (const LinuxProcessState::Thread& t : p.linux_state().threads) found = found || (t.used && t.tid == 77 && t.blocked == (bit(kUsr2) | bit(15)));
    CHECK(found);
  }
  set_system_unix_sockets(nullptr);
  set_system_objects(nullptr);
}

// Handlers of AArch64 and RISC-V programs: each CPU's own frame, the page a handler returns through (there is no vDSO), and the way back.
static void test_signal_handlers_other_cpus() {
  using linuxabi::LinuxSignalContext;
  constexpr uint64_t kUsr1 = 10, kUsr2 = 12, kSegv = 11, kIll = 4, kHandler = 0x400100, kStackTop = 0x2F000, kRestorerFlag = 0x04000000, kRestart = 0x10000000;
  constexpr uint64_t kInfo = 128, kHead = 176;      // siginfo_t, and what comes before uc_mcontext in struct ucontext
  auto bit = [](uint64_t signal) { return uint64_t{1} << (signal - 1); };
  for (const UserArch arch : {UserArch::AArch64, UserArch::RiscV64}) {
    const bool arm = arch == UserArch::AArch64;
    auto registers = [&](uint64_t stack) {
      LinuxSignalContext c;
      c.arch = arch;
      for (uint32_t i = 0; i < 32; ++i) c.x[i] = 0x0101010101010101ull * (i + 1);
      for (uint32_t i = 0; i < 64; ++i) c.vector[i] = 0xF0F0F0F000000000ull + i;
      c.set_stack_pointer(stack);
      c.pc = 0x400504;
      c.pstate = 0x60000000;
      c.fpcr = arm ? 0x00C00000 : 0xE1;
      c.fpsr = arm ? 0x1F : 0;
      return c;
    };
    // struct sigaction: AArch64 has a restorer field (handler, flags, restorer, mask), RISC-V has none (handler, flags, mask)
    auto action = [&](Rig& r, uint64_t handler, uint64_t flags, uint64_t mask, uint64_t restorer = 0) {
      return arm ? r.words({handler, flags, restorer, mask}) : r.words({handler, flags, mask, 0x4755415244ull});
    };
    struct Running {
      Rig rig;
      ForkRuntime runtime;
      explicit Running(UserArch a) : rig(nullptr, 1, a) { rig.process->attach_runtime(&runtime); rig.process->linux_state().handlers_run = true; }
      ~Running() { rig.process->attach_runtime(nullptr); }
    };
    {
      Running run(arch);
      Rig& rig = run.rig;
      Process& p = *rig.process;
      const uint64_t old = rig.alloc(40);
      rig.put64(old + (arm ? 32 : 24), 0x4755415244ull);
      CHECK(rig.s(134, kUsr1, action(rig, kHandler, 4, bit(kUsr2)), 0, 8) == 0 && rig.s(134, kUsr1, 0, old, 8) == 0);
      CHECK(rig.u64(old) == kHandler && rig.u64(old + 8) == 4 && rig.u64(old + (arm ? 24 : 16)) == bit(kUsr2) && rig.u64(old + (arm ? 32 : 24)) == 0x4755415244ull);
      // the handler is entered: its arguments, its stack, and the frame it was given
      CHECK(rig.s(129, 1, kUsr1) == 0 && linuxabi::signals_wanted(p));
      const LinuxSignalContext before = registers(kStackTop);
      LinuxSignalContext c = before;
      const uint64_t regions = rig.space.region_count();
      CHECK(linuxabi::deliver_signals(p, &c, nullptr));
      const uint64_t frame = c.stack_pointer(), uc = frame + kInfo, regs = uc + kHead;
      CHECK(c.pc == kHandler && c.x[arm ? 0 : 10] == kUsr1 && c.x[arm ? 1 : 11] == frame && c.x[arm ? 2 : 12] == uc && frame % 16 == 0 && frame < kStackTop - (arm ? 4688 : 1088) + 16);
      CHECK(rig.u32(frame) == kUsr1 && rig.u32(frame + 8) == 0 && rig.u32(frame + 16) == 1);      // si_signo, SI_USER, si_pid
      CHECK(rig.u64(uc + 40) == 0 && rig.u32(uc + 24) == 2);                                      // uc_sigmask; no alternate stack
      const uint64_t trampoline = p.linux_state().signal_trampoline, mask = rig.alloc(8);
      CHECK(trampoline != 0 && rig.space.region_count() == regions + 1 && c.x[arm ? 30 : 1] == trampoline);
      uint8_t protection = 0;
      CHECK(rig.space.protection_at(trampoline, &protection) && protection == (kProtRead | kProtExecute));      // code the program runs and cannot change
      CHECK(rig.u64(trampoline) == (arm ? 0xD4000001D2801168ull : 0x0000007308B00893ull));        // mov x8, #139; svc #0  /  li a7, 139; ecall
      if (arm) {
        CHECK(rig.u64(regs) == 0 && rig.u64(regs + 8) == before.x[0] && rig.u64(regs + 8 + 30 * 8) == before.x[30] && rig.u64(regs + 256) == kStackTop);
        CHECK(rig.u64(regs + 264) == before.pc && rig.u64(regs + 272) == before.pstate);
        CHECK(rig.u32(regs + 288) == 0x46508001u && rig.u32(regs + 292) == 528 && rig.u32(regs + 296) == before.fpsr && rig.u32(regs + 300) == before.fpcr);
        CHECK(rig.u64(regs + 304) == before.vector[0] && rig.u64(regs + 304 + 63 * 8) == before.vector[63] && rig.u64(regs + 288 + 528) == 0);      // V0..V31, then the end of the records
        CHECK(c.x[29] == kStackTop - 16 && rig.u64(c.x[29]) == before.x[29] && rig.u64(c.x[29] + 8) == before.x[30]);      // the frame record a stack walk goes through
      } else {
        CHECK(rig.u64(regs) == before.pc && rig.u64(regs + 8) == before.x[1] && rig.u64(regs + 16) == kStackTop && rig.u64(regs + 31 * 8) == before.x[31]);
        CHECK(rig.u64(regs + 256) == before.vector[0] && rig.u64(regs + 256 + 31 * 8) == before.vector[31] && rig.u32(regs + 256 + 256) == before.fpcr);
      }
      CHECK(rig.s(135, 0, 0, mask, 8) == 0 && rig.u64(mask) == (bit(kUsr1) | bit(kUsr2)));
      // the handler returns (its stack pointer is at the frame again): everything is as it was
      LinuxSignalContext back = registers(frame);
      for (uint64_t& v : back.x) v = 0;
      back.set_stack_pointer(frame);
      CHECK(rig.s(139) == 0 && p.take_signal_return() && linuxabi::signal_return(p, &back));
      for (uint32_t i = arm ? 0 : 1; i < (arm ? 31u : 32u); ++i) CHECK(back.x[i] == before.x[i]);
      CHECK(back.stack_pointer() == kStackTop && back.pc == before.pc && back.pstate == (arm ? before.pstate : back.pstate) && back.fpcr == before.fpcr && back.fpsr == before.fpsr);
      for (uint32_t i = 0; i < (arm ? 64u : 32u); ++i) CHECK(back.vector[i] == before.vector[i]);
      CHECK(rig.s(135, 0, 0, mask, 8) == 0 && rig.u64(mask) == 0);
      // a second handler uses the same page to return through; with a restorer given (AArch64 only), that is used instead
      CHECK(rig.s(129, 1, kUsr1) == 0);
      c = before;
      CHECK(linuxabi::deliver_signals(p, &c, nullptr) && p.linux_state().signal_trampoline == trampoline && rig.space.region_count() == regions + 1);
      CHECK(rig.s(135, 2, rig.words({0}), 0, 8) == 0);
      if (arm) {
        CHECK(rig.s(134, kUsr1, action(rig, kHandler, kRestorerFlag, 0, 0x400300), 0, 8) == 0 && rig.s(129, 1, kUsr1) == 0);
        c = before;
        CHECK(linuxabi::deliver_signals(p, &c, nullptr) && c.x[30] == 0x400300);
        CHECK(rig.s(135, 2, rig.words({0}), 0, 8) == 0);
      }
      // what a handler (or anything else) wrote where a frame should be
      c = before;
      CHECK(rig.s(134, kUsr1, action(rig, kHandler, 0, 0), 0, 8) == 0 && rig.s(129, 1, kUsr1) == 0 && linuxabi::deliver_signals(p, &c, nullptr));
      const uint64_t good = c.stack_pointer(), good_uc = good + kInfo, good_regs = good_uc + kHead;
      auto returns = [&](uint64_t at) { LinuxSignalContext r = registers(at); return linuxabi::signal_return(p, &r); };
      CHECK(returns(good));
      for (const uint64_t bad : {kBad, uint64_t{0}, uint64_t{0x30000 - 64}, ~uint64_t{0}, ~uint64_t{0} - 5000, uint64_t{0x0000800000000000ull}}) CHECK(!returns(bad));
      rig.put64(good_uc + 40, ~uint64_t{0});      // every signal blocked: SIGKILL and SIGSTOP are not
      CHECK(returns(good) && rig.s(135, 0, 0, mask, 8) == 0 && rig.u64(mask) == (~uint64_t{0} & ~(bit(9) | bit(19))));
      if (arm) {
        const uint64_t records = good_regs + 288;
        rig.put64(good_regs + 272, ~uint64_t{0});      // every bit of PSTATE: only the condition flags are taken
        LinuxSignalContext r = registers(good);
        CHECK(linuxabi::signal_return(p, &r) && r.pstate == 0xF0000000u);
        // the records after the registers: the floating-point one has to be there, once, and whole
        const uint32_t fpsimd = 0x46508001u;
        rig.put32(records, 0x12345678);              // some other record of that size in its place: no floating-point state
        CHECK(!returns(good));
        rig.put32(records, fpsimd);
        for (const uint32_t size : {0u, 8u, 15u, 24u, 512u, 544u, 4096u, 4112u, 0xFFFFFFF0u}) {
          rig.put32(records + 4, size);
          CHECK(!returns(good));
        }
        rig.put32(records + 4, 528);
        rig.put32(records + 528, fpsimd); rig.put32(records + 532, 528);      // twice
        CHECK(!returns(good));
        rig.put32(records + 528, 0x45535201); rig.put32(records + 532, 16);   // an esr_context, then one nobody knows, then the end
        rig.put32(records + 544, 0x0BADC0DE); rig.put32(records + 548, 32);
        rig.put64(records + 576, 0);
        CHECK(returns(good));
        rig.put32(records + 548, 4096);                                       // a record that runs past the space for them
        CHECK(!returns(good));
        rig.put32(records + 548, 32);
      }
      // bits flipped anywhere in a good frame, and frames of random bytes: a refusal or a return, never anything else
      const uint64_t frame_bytes = arm ? 128 + 176 + 288 + 640 : 1088;
      const std::string image = rig.bytes(good, frame_bytes);
      uint32_t seed = 4242;
      for (int round = 0; round < 3000; ++round) {
        std::string changed = image;
        for (int flips = 0; flips < 1 + round % 4; ++flips) {
          seed = seed * 1664525u + 1013904223u;
          changed[(seed >> 8) % frame_bytes] ^= char(1u << (seed >> 29));
        }
        if (round % 5 == 4) for (char& b : changed) { seed = seed * 1664525u + 1013904223u; b = char(seed >> 24); }
        CHECK(rig.space.write_privileged(good, ConstByteView(reinterpret_cast<const uint8_t*>(changed.data()), frame_bytes)));
        returns(good);
      }
      CHECK(rig.space.write_privileged(good, ConstByteView(reinterpret_cast<const uint8_t*>(image.data()), frame_bytes)) && returns(good));
      // a frame cut off by the end of the program's memory, at every length
      const uint64_t needed = arm ? 128 + 176 + 288 + 528 + 8 : 128 + 176 + 256 + 32 * 8 + 4;      // (up to the record that ends the list; up to fcsr)
      for (uint64_t have = 0; have < needed; have += arm ? 24 : 40) {
        const uint64_t at = 0x30000 - have;
        CHECK(rig.space.write_privileged(at, ConstByteView(reinterpret_cast<const uint8_t*>(image.data()), have)));
        CHECK(!returns(at));
      }
      CHECK(!p.exited());
    }
    {      // a call that was cut short starts again: the first argument is back in its register, the program counter at the call instruction
      for (const bool restart : {true, false}) {
        Running run(arch);
        Rig& rig = run.rig;
        CHECK(rig.s(134, kUsr1, action(rig, kHandler, restart ? kRestart : 0, 0), 0, 8) == 0 && rig.s(129, 1, kUsr1) == 0);
        LinuxSignalContext c = registers(kStackTop);
        c.result() = uint64_t(-4);
        const linuxabi::InterruptedCall call{63, true, 0x7777};
        CHECK(linuxabi::deliver_signals(*rig.process, &c, &call));
        const uint64_t regs = c.stack_pointer() + kInfo + kHead;
        CHECK(rig.u64(regs + (arm ? 8 : 80)) == (restart ? 0x7777 : uint64_t(-4)) && rig.u64(regs + (arm ? 264 : 0)) == (restart ? 0x400500 : 0x400504));
      }
    }
    {      // faults; no stack to put a frame on; the alternate stack
      Running run(arch);
      Rig& rig = run.rig;
      Process& p = *rig.process;
      CHECK(rig.s(134, kSegv, action(rig, kHandler, 4, 0), 0, 8) == 0 && rig.s(134, kIll, action(rig, kHandler, 4, 0), 0, 8) == 0);
      LinuxSignalContext c = registers(kStackTop);
      CHECK(linuxabi::deliver_fault(p, &c, 14, 0, 0x1234) && c.pc == kHandler && c.x[arm ? 0 : 10] == kSegv);
      const uint64_t frame = c.stack_pointer(), regs = frame + kInfo + kHead;
      CHECK(rig.u32(frame) == kSegv && rig.u32(frame + 8) == 1 && rig.u64(frame + 16) == 0x1234 && rig.u64(regs + (arm ? 264 : 0)) == 0x400504);      // SEGV_MAPERR, si_addr; the pc of the fault
      if (arm) CHECK(rig.u64(regs) == 0x1234 && rig.u32(regs + 288 + 528) == 0x45535201u && rig.u32(regs + 288 + 532) == 16 && rig.u64(regs + 288 + 544) == 0);      // fault_address; an esr_context
      c = registers(kStackTop);
      CHECK(linuxabi::deliver_fault(p, &c, 6, 0, 0) && rig.u32(c.stack_pointer()) == kIll && rig.u32(c.stack_pointer() + 8) == 1 && rig.u64(c.stack_pointer() + 16) == 0x400504);      // ILL_ILLOPC at the instruction
      constexpr uint64_t kAlt = 0x12000, kAltSize = 0x6000;
      CHECK(rig.s(132, rig.words({kAlt, 0, kAltSize}), 0) == 0 && rig.s(134, kUsr1, action(rig, kHandler, 0x08000000, 0), 0, 8) == 0 && rig.s(129, 1, kUsr1) == 0);
      c = registers(kStackTop);
      CHECK(linuxabi::deliver_signals(p, &c, nullptr) && c.stack_pointer() > kAlt && c.stack_pointer() < kAlt + kAltSize && rig.u64(c.stack_pointer() + kInfo + 16) == kAlt);
      for (const uint64_t stack : {kBad, uint64_t{0}, uint64_t{64}, uint64_t{0x10000 + 512}, ~uint64_t{0}}) {
        Running none(arch);
        CHECK(none.rig.s(134, kUsr1, action(none.rig, kHandler, 0, 0), 0, 8) == 0 && none.rig.s(129, 1, kUsr1) == 0);
        LinuxSignalContext s = registers(stack);
        CHECK(!linuxabi::deliver_signals(*none.rig.process, &s, nullptr) && none.rig.process->exited() && none.rig.process->exit_code() == 139);
      }
    }
  }
  {      // registers of no CPU this knows: nothing is entered, nothing is returned to
    Rig rig(nullptr, 1, UserArch::LoongArch64);
    ForkRuntime runtime;
    rig.process->attach_runtime(&runtime);
    rig.process->linux_state().handlers_run = true;
    CHECK(rig.s(134, 10, rig.words({0x400100, 0, 0}), 0, 8) == 0 && rig.s(129, 1, 10) == 0);
    linuxabi::LinuxSignalContext c;
    c.arch = UserArch::LoongArch64;
    CHECK(!linuxabi::signal_return(*rig.process, &c));
    CHECK(!linuxabi::deliver_signals(*rig.process, &c, nullptr) && rig.process->exited());
    rig.process->attach_runtime(nullptr);
  }
}

// What tests/linux/sigframe_loongarch.c printed under qemu's user mode (tests/linux/build_sigframe_loongarch.sh): the frames Linux gives a LoongArch
// program's handlers, byte for byte.
static const char* g_data_directory = "tests/data/emulation";
struct ReferenceFrame {
  uint64_t signal = 0, gap = 0, info = 0, context = 0;
  uint32_t way_back[2] = {};
  std::string bytes;
  uint64_t u64(uint64_t at) const { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | uint8_t(bytes[at + uint64_t(i)]); return v; }
  uint32_t u32(uint64_t at) const { return uint32_t(u64(at)); }
};
static bool read_reference_frames(const char* kind, std::vector<ReferenceFrame>* out, std::string* returned) {
  const std::string path = std::string(g_data_directory) + "/sigframes-loongarch64-" + kind + ".txt";
  FILE* file = fopen(path.c_str(), "rb");
  if (!file) return false;
  std::string text;
  char chunk[4096];
  for (size_t n; (n = fread(chunk, 1, sizeof(chunk), file)) > 0;) text.append(chunk, n);
  fclose(file);
  bool ended = false;
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(at, end - at);
    at = end + 1;
    unsigned long long a = 0, b = 0, c = 0;
    unsigned w0 = 0, w1 = 0;
    if (sscanf(line.c_str(), "signal %llu", &a) == 1) {
      out->push_back(ReferenceFrame());
      out->back().signal = a;
    } else if (!out->empty() && sscanf(line.c_str(), "at - gap %llu info %llu context %llu", &a, &b, &c) == 3) {
      out->back().gap = a; out->back().info = b; out->back().context = c;
    } else if (!out->empty() && sscanf(line.c_str(), "way-back %x %x", &w0, &w1) == 2) {
      out->back().way_back[0] = w0; out->back().way_back[1] = w1;
    } else if (!out->empty() && line.compare(0, 6, "frame ") == 0) {
      if ((line.size() - 6) % 2 != 0) return false;
      for (size_t i = 6; i + 1 < line.size(); i += 2) {
        unsigned byte = 0;
        if (sscanf(line.substr(i, 2).c_str(), "%2x", &byte) != 1) return false;
        out->back().bytes.push_back(char(byte));
      }
    } else if (line.compare(0, 9, "returned ") == 0) {
      *returned = line.substr(9);
    } else if (line == "frames ok") {
      ended = true;
    }
  }
  if (!ended || out->size() != 2) return false;
  for (const ReferenceFrame& f : *out)
    if (f.bytes.size() != f.gap || f.gap < 128 + 448 + 16 || f.info != 0 || f.context != 128) return false;
  return true;
}

// Handlers of LoongArch programs: the frame is compared with the ones Linux makes (as qemu's user mode writes them), and rt_sigreturn is given those.
// (LinuxForkTest runs a LoongArch program's handlers end to end.)
static void test_signal_handlers_loongarch() {
  using linuxabi::LinuxSignalContext;
  constexpr UserArch arch = UserArch::LoongArch64;
  constexpr uint64_t kUsr1 = 10, kUsr2 = 12, kSegv = 11, kIll = 4, kHandler = 0x400100, kStackTop = 0x2F000, kRestart = 0x10000000;
  constexpr uint64_t kInfo = 128, kHead = 176, kRegs = 272, kRecords = kInfo + kHead + kRegs, kOwnFrame = 880;
  constexpr uint32_t kFp = 0x46505501u, kLsx = 0x53580001u, kLasx = 0x41535801u, kLbt = 0x42540001u;
  auto bit = [](uint64_t signal) { return uint64_t{1} << (signal - 1); };
  auto registers = [&](uint64_t stack) {
    LinuxSignalContext c;
    c.arch = arch;
    for (uint32_t i = 1; i < 32; ++i) c.x[i] = 0x0101010101010101ull * (i + 1);
    for (uint32_t i = 0; i < 32; ++i) c.vector[i] = 0xF0F0F0F000000000ull + i;
    c.set_stack_pointer(stack);
    c.pc = 0x400504;
    c.pstate = 0x0100000100010001ull;      // fcc0, fcc2, fcc4, fcc7
    c.fpcr = 0x0300;                       // rounding mode
    return c;
  };
  auto action = [&](Rig& r, uint64_t handler, uint64_t flags, uint64_t mask) { return r.words({handler, flags, mask, 0x4755415244ull}); };      // (no restorer field)
  struct Running {
    Rig rig;
    ForkRuntime runtime;
    Running() : rig(nullptr, 1, UserArch::LoongArch64) { rig.process->attach_runtime(&runtime); rig.process->linux_state().handlers_run = true; }
    ~Running() { rig.process->attach_runtime(nullptr); }
  };
  auto put = [](Rig& rig, uint64_t at, const std::string& bytes) {
    return rig.space.write_privileged(at, ConstByteView(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  };
  CHECK(registers(0x1230).stack_pointer() == 0x1230 && registers(0x1230).x[3] == 0x1230 && &registers(0).result() != nullptr);
  {
    LinuxSignalContext c = registers(0);
    CHECK(&c.result() == &c.x[4]);
  }

  {      // the frame, the page a handler returns through, and the way back
    Running run;
    Rig& rig = run.rig;
    Process& p = *rig.process;
    const uint64_t old = rig.alloc(40);
    rig.put64(old + 24, 0x4755415244ull);
    CHECK(rig.s(134, kUsr1, action(rig, kHandler, 4, bit(kUsr2)), 0, 8) == 0 && rig.s(134, kUsr1, 0, old, 8) == 0);
    CHECK(rig.u64(old) == kHandler && rig.u64(old + 8) == 4 && rig.u64(old + 16) == bit(kUsr2) && rig.u64(old + 24) == 0x4755415244ull);      // 24 bytes
    CHECK(rig.s(129, 1, kUsr1) == 0 && linuxabi::signals_wanted(p));
    const LinuxSignalContext before = registers(kStackTop);
    LinuxSignalContext c = before;
    const uint64_t regions = rig.space.region_count();
    CHECK(linuxabi::deliver_signals(p, &c, nullptr));
    const uint64_t frame = c.stack_pointer(), uc = frame + kInfo, regs = uc + kHead, records = frame + kRecords;
    CHECK(c.pc == kHandler && c.x[4] == kUsr1 && c.x[5] == frame && c.x[6] == uc && frame == kStackTop - kOwnFrame);
    CHECK(rig.u32(frame) == kUsr1 && rig.u32(frame + 8) == 0 && rig.u32(frame + 16) == 1);
    CHECK(rig.u64(uc + 40) == 0 && rig.u32(uc + 24) == 2);
    const uint64_t trampoline = p.linux_state().signal_trampoline, mask = rig.alloc(8);
    CHECK(trampoline != 0 && rig.space.region_count() == regions + 1 && c.x[1] == trampoline);
    uint8_t protection = 0;
    CHECK(rig.space.protection_at(trampoline, &protection) && protection == (kProtRead | kProtExecute));
    CHECK(rig.u64(trampoline) == 0x002B000003822C0Bull);      // ori $a7, $zero, 139; syscall 0
    CHECK(rig.u64(regs) == before.pc && rig.u64(regs + 8) == 0 && rig.u64(regs + 16) == before.x[1] && rig.u64(regs + 8 + 3 * 8) == kStackTop && rig.u64(regs + 8 + 31 * 8) == before.x[31]);
    CHECK(rig.u32(regs + 264) == 1 && rig.u32(regs + 268) == 0);      // flags: the floating-point registers are there
    CHECK(rig.u32(records) == kFp && rig.u32(records + 4) == 288 && rig.u64(records + 8) == 0 && rig.u64(records + 16) == before.vector[0] && rig.u64(records + 16 + 31 * 8) == before.vector[31]);
    CHECK(rig.u64(records + 272) == before.pstate && rig.u32(records + 280) == before.fpcr && rig.u64(records + 288) == 0 && rig.u64(records + 296) == 0);
    CHECK(rig.s(135, 0, 0, mask, 8) == 0 && rig.u64(mask) == (bit(kUsr1) | bit(kUsr2)));
    LinuxSignalContext back;
    back.arch = arch;
    back.set_stack_pointer(frame);
    CHECK(rig.s(139) == 0 && p.take_signal_return() && linuxabi::signal_return(p, &back));
    for (uint32_t i = 1; i < 32; ++i) CHECK(back.x[i] == before.x[i]);
    for (uint32_t i = 0; i < 32; ++i) CHECK(back.vector[i] == before.vector[i]);
    CHECK(back.x[0] == 0 && back.pc == before.pc && back.pstate == before.pstate && back.fpcr == before.fpcr);
    CHECK(rig.s(135, 0, 0, mask, 8) == 0 && rig.u64(mask) == 0);
    CHECK(rig.s(129, 1, kUsr1) == 0);      // a second handler returns through the same page
    c = before;
    CHECK(linuxabi::deliver_signals(p, &c, nullptr) && p.linux_state().signal_trampoline == trampoline && rig.space.region_count() == regions + 1);
    CHECK(rig.s(135, 2, rig.words({0}), 0, 8) == 0);

    // what a handler (or anything else) wrote where a frame should be
    c = before;
    CHECK(rig.s(134, kUsr1, action(rig, kHandler, 0, 0), 0, 8) == 0 && rig.s(129, 1, kUsr1) == 0 && linuxabi::deliver_signals(p, &c, nullptr));
    const uint64_t good = c.stack_pointer(), good_regs = good + kInfo + kHead, good_records = good + kRecords;
    auto returns = [&](uint64_t at) { LinuxSignalContext r = registers(at); return linuxabi::signal_return(p, &r); };
    CHECK(returns(good));
    for (const uint64_t bad : {kBad, uint64_t{0}, uint64_t{0x30000 - 64}, ~uint64_t{0}, ~uint64_t{0} - 5000, uint64_t{0x0000800000000000ull}}) CHECK(!returns(bad));
    rig.put64(good + kInfo + 40, ~uint64_t{0});
    CHECK(returns(good) && rig.s(135, 0, 0, mask, 8) == 0 && rig.u64(mask) == (~uint64_t{0} & ~(bit(9) | bit(19))));
    const std::string image = rig.bytes(good, kOwnFrame);
    {      // r0 stays zero whatever the frame says; of the condition flags and fcsr only the bits that exist are taken
      rig.put64(good_regs + 8, 0x1234);
      rig.put64(good_records + 272, ~uint64_t{0});
      rig.put32(good_records + 280, ~uint32_t{0});
      LinuxSignalContext r = registers(good);
      r.x[0] = 0;
      CHECK(linuxabi::signal_return(p, &r) && r.x[0] == 0 && r.pstate == 0x0101010101010101ull && r.fpcr == 0x1F1F031Fu);
      CHECK(put(rig, good, image));
    }
    {      // the records: a kind nobody knows, a size that is not the kind's, the floating-point state twice, a list without an end
      rig.put32(good_records, 0x12345678);
      CHECK(!returns(good));
      rig.put32(good_records, kFp);
      for (const uint32_t size : {0u, 8u, 16u, 280u, 283u, 284u, 352u, 544u, 4096u, 0xFFFFFFF0u, 0xFFFFFFFFu}) {
        rig.put32(good_records + 4, size);
        CHECK(!returns(good));
      }
      rig.put32(good_records + 4, 288);
      CHECK(returns(good));
      rig.put32(good_records + 288 + 4, 16);      // the end's magic with a size
      CHECK(!returns(good));
      // a frame as long as the stack has room for, at the top of the program's memory
      const uint64_t at = 0x30000 - 4096 - kRecords - 16;
      std::string longer = image.substr(0, kRecords) + std::string(4096 + 16, '\0');
      auto word = [&](uint64_t where, uint32_t value) { for (int i = 0; i < 4; ++i) longer[kRecords + where + uint64_t(i)] = char(value >> (8 * i)); };
      word(0, kLbt); word(4, 96);           // binary translation state: passed over
      word(96, kFp); word(100, 288);
      CHECK(put(rig, at, longer) && returns(at));
      word(384, kFp); word(388, 288);       // twice
      CHECK(put(rig, at, longer) && !returns(at));
      word(384, kLbt); word(388, 96);       // the other kind twice
      CHECK(put(rig, at, longer) && !returns(at));
      word(384, 0); word(388, 0);
      word(0, 0); word(4, 0);               // none at all: the floating-point registers stay as they are
      CHECK(put(rig, at, longer));
      LinuxSignalContext r = registers(at);
      CHECK(linuxabi::signal_return(p, &r) && r.vector[7] == registers(0).vector[7] && r.pstate == registers(0).pstate && r.x[9] == registers(0).x[9]);
      word(0, kLbt); word(4, 4080);         // one record up to the last 16 bytes, and the end there
      CHECK(put(rig, at, longer) && returns(at));
      word(4, 4088);                        // ... and one that leaves no room for the end
      CHECK(put(rig, at, longer) && !returns(at));
      word(4, 4096);
      CHECK(put(rig, at, longer) && !returns(at));
      for (uint64_t where = 0; where < 4096; where += 16) { word(where, kLbt); word(where + 4, 16); }      // records all the way, and no end
      CHECK(put(rig, at, longer) && !returns(at));
      CHECK(put(rig, good, image) && returns(good));
    }
    // bits flipped anywhere in a good frame, and frames of random bytes: a refusal or a return, never anything else
    uint32_t seed = 9191;
    for (int round = 0; round < 3000; ++round) {
      std::string changed = image;
      for (int flips = 0; flips < 1 + round % 4; ++flips) {
        seed = seed * 1664525u + 1013904223u;
        changed[(seed >> 8) % kOwnFrame] ^= char(1u << (seed >> 29));
      }
      if (round % 5 == 4) for (char& b : changed) { seed = seed * 1664525u + 1013904223u; b = char(seed >> 24); }
      CHECK(put(rig, good, changed));
      returns(good);
    }
    CHECK(put(rig, good, image) && returns(good));
    // a frame cut off by the end of the program's memory, at every length (up to the record that ends the list)
    for (uint64_t have = 0; have < kOwnFrame - 8; have += 8) {
      const uint64_t at = 0x30000 - have;
      CHECK(put(rig, at, image.substr(0, have)));
      CHECK(!returns(at));
    }
    CHECK(put(rig, 0x30000 - kOwnFrame, image) && returns(0x30000 - kOwnFrame));
    CHECK(!p.exited());
  }
  for (const bool restart : {true, false}) {      // a call that was cut short starts again: a0 has its argument back, the program counter is at SYSCALL
    Running run;
    Rig& rig = run.rig;
    CHECK(rig.s(134, kUsr1, action(rig, kHandler, restart ? kRestart : 0, 0), 0, 8) == 0 && rig.s(129, 1, kUsr1) == 0);
    LinuxSignalContext c = registers(kStackTop);
    c.result() = uint64_t(-4);
    const linuxabi::InterruptedCall call{63, true, 0x7777};
    CHECK(linuxabi::deliver_signals(*rig.process, &c, &call));
    const uint64_t regs = c.stack_pointer() + kInfo + kHead;
    CHECK(rig.u64(regs + 8 + 4 * 8) == (restart ? 0x7777 : uint64_t(-4)) && rig.u64(regs) == (restart ? 0x400500 : 0x400504));
  }
  {      // faults; the alternate stack; no stack to put a frame on
    Running run;
    Rig& rig = run.rig;
    Process& p = *rig.process;
    CHECK(rig.s(134, kSegv, action(rig, kHandler, 4, 0), 0, 8) == 0 && rig.s(134, kIll, action(rig, kHandler, 4, 0), 0, 8) == 0);
    LinuxSignalContext c = registers(kStackTop);
    CHECK(linuxabi::deliver_fault(p, &c, 14, 0, 0x1234) && c.pc == kHandler && c.x[4] == kSegv);
    const uint64_t frame = c.stack_pointer();
    CHECK(rig.u32(frame) == kSegv && rig.u32(frame + 8) == 1 && rig.u64(frame + 16) == 0x1234 && rig.u64(frame + kInfo + kHead) == 0x400504);
    c = registers(kStackTop);
    CHECK(linuxabi::deliver_fault(p, &c, 6, 0, 0) && rig.u32(c.stack_pointer()) == kIll && rig.u32(c.stack_pointer() + 8) == 0 && rig.u64(c.stack_pointer() + 16) == 0x400504);
    constexpr uint64_t kAlt = 0x12000, kAltSize = 0x6000;
    CHECK(rig.s(132, rig.words({kAlt, 0, kAltSize}), 0) == 0 && rig.s(134, kUsr1, action(rig, kHandler, 0x08000000, 0), 0, 8) == 0 && rig.s(129, 1, kUsr1) == 0);
    c = registers(kStackTop);
    CHECK(linuxabi::deliver_signals(p, &c, nullptr) && c.stack_pointer() == kAlt + kAltSize - kOwnFrame && rig.u64(c.stack_pointer() + kInfo + 16) == kAlt &&
          rig.u32(c.stack_pointer() + kInfo + 24) == 0 && rig.u64(c.stack_pointer() + kInfo + 32) == kAltSize);
    for (const uint64_t stack : {kBad, uint64_t{0}, uint64_t{64}, uint64_t{0x10000 + 512}, ~uint64_t{0}}) {
      Running none;
      CHECK(none.rig.s(134, kUsr1, action(none.rig, kHandler, 0, 0), 0, 8) == 0 && none.rig.s(129, 1, kUsr1) == 0);
      LinuxSignalContext s = registers(stack);
      CHECK(!linuxabi::deliver_signals(*none.rig.process, &s, nullptr) && none.rig.process->exited() && none.rig.process->exit_code() == 139);
    }
  }

  // --- against the frames Linux makes
  for (const char* kind : {"fpu", "lsx", "lasx"}) {
    std::vector<ReferenceFrame> frames;
    std::string returned;
    const bool have = read_reference_frames(kind, &frames, &returned);
    CHECK(have);
    if (!have) continue;
    const uint32_t magic = kind[1] == 'p' ? kFp : kind[2] == 'x' ? kLsx : kLasx;
    const uint64_t words = magic == kFp ? 1 : magic == kLsx ? 2 : 4;
    CHECK(returned == "00000000005e7bac");      // (the handler changed s0 in its frame, and the program found it changed: the registers are where this test reads them)
    for (const ReferenceFrame& reference : frames) {
      const bool fault = reference.signal == kSegv;
      CHECK(reference.signal == (fault ? kSegv : kUsr1) && reference.way_back[0] == 0x03822C0Bu && reference.way_back[1] == 0x002B0000u);
      CHECK(reference.u32(kRecords) == magic && reference.u64(kInfo + kHead + 8) == 0 && reference.u32(kInfo + kHead + 264) == 1);
      CHECK(reference.gap == kRecords + reference.u32(kRecords + 4) + 16);      // (the records follow one another up to the interrupted stack pointer)
      Running run;
      Rig& rig = run.rig;
      Process& p = *rig.process;
      // rt_sigreturn takes Linux's frame: every register comes back as the frame has it
      const uint64_t at = kStackTop - reference.gap;
      CHECK(put(rig, at, reference.bytes));
      LinuxSignalContext restored;
      restored.arch = arch;
      restored.set_stack_pointer(at);
      const bool taken = linuxabi::signal_return(p, &restored);
      CHECK(taken);
      if (!taken) continue;
      CHECK(restored.pc == reference.u64(kInfo + kHead) && restored.x[0] == 0);
      for (uint32_t i = 1; i < 32; ++i) CHECK(restored.x[i] == reference.u64(kInfo + kHead + 8 + i * 8));
      for (uint32_t i = 0; i < 32; ++i) CHECK(restored.vector[i] == reference.u64(kRecords + 16 + i * 8 * words));
      CHECK(restored.pstate == 0x0001000000000100ull && restored.fpcr == 0);      // fcc1 and fcc6, as the program set them
      // (the program's marks: s1..s8 and f24..f31; s0 too, until the first handler changed it)
      for (uint32_t i = 24; i < 32; ++i) CHECK(restored.x[i] == 0xA500000000000000ull + ((i - 22) << 8) + ((i / 10) << 4) + i % 10);
      CHECK(restored.x[23] == (fault ? 0x5E7BACull : 0xA500000000000123ull));
      for (uint32_t i = 24; i < 32; ++i) CHECK(restored.vector[i] == 0xA500000000000000ull + ((i - 23) << 8) + (((i - 1) / 10) << 4) + (i - 1) % 10);
      if (magic != kFp) CHECK(reference.u64(kRecords + 16 + 24 * 8 * words + 8) == 0xA500000000000123ull);      // (the upper part of vr24: this build did use the vector registers)
      if (fault) {
        CHECK(reference.u32(0) == kSegv && reference.u32(8) == 1 && reference.u64(16) == 0x10 && restored.x[12] == 0x10);      // SEGV_MAPERR at the address in t0
      } else {
        CHECK(reference.u32(8) == 0 && restored.x[4] == 0 && restored.x[5] == kUsr1 && restored.x[11] == 129);      // SI_USER; kill() had returned 0
      }
      // The frame made here for the same registers at the same moment is Linux's, byte for byte, where the thread had used no more than the floating-point
      // registers. (Where it had used the vector registers Linux's frame has them in full; this one has their low halves, in the same places up to there.)
      const uint64_t stack = restored.x[3];
      LinuxSignalContext c = restored;
      c.set_stack_pointer(kStackTop);
      CHECK(rig.s(134, reference.signal, action(rig, kHandler, 4, 0), 0, 8) == 0);
      if (fault) {
        CHECK(linuxabi::deliver_fault(p, &c, 14, 0, 0x10));
      } else {
        CHECK(rig.s(129, 1, kUsr1) == 0 && linuxabi::deliver_signals(p, &c, nullptr));
      }
      const uint64_t frame = c.stack_pointer();
      CHECK(frame == kStackTop - kOwnFrame && c.x[4] == reference.signal && c.x[5] == frame + reference.info && c.x[6] == frame + reference.context);
      CHECK(rig.u32(p.linux_state().signal_trampoline) == reference.way_back[0] && rig.u32(p.linux_state().signal_trampoline + 4) == reference.way_back[1]);
      std::string expected = reference.bytes;
      for (int i = 0; i < 8; ++i) expected[kInfo + kHead + 8 + 3 * 8 + uint64_t(i)] = char((kStackTop + (stack & 15)) >> (8 * i));      // (the stack is elsewhere here)
      CHECK((stack & 15) == 0);
      const std::string made = rig.bytes(frame, kOwnFrame);
      CHECK(made.substr(kInfo, kRecords - kInfo) == expected.substr(kInfo, kRecords - kInfo));      // struct ucontext up to the records
      if (fault) CHECK(made.substr(0, kInfo) == expected.substr(0, kInfo));                          // siginfo_t of the fault
      else CHECK(made.substr(0, 16) == expected.substr(0, 16) && made.substr(24, kInfo - 24) == expected.substr(24, kInfo - 24));      // (si_pid and si_uid are this OS's)
      if (magic == kFp) {
        CHECK(reference.gap == kOwnFrame && made.substr(kRecords) == expected.substr(kRecords));
      } else {
        CHECK(rig.u32(frame + kRecords) == kFp);
        for (uint32_t i = 0; i < 32; ++i) CHECK(rig.u64(frame + kRecords + 16 + i * 8) == reference.u64(kRecords + 16 + i * 8 * words));
        CHECK(made.substr(kRecords + 16 + 256, 16) == expected.substr(kRecords + 16 + 256 * words, 16));      // the condition flags and fcsr
      }
      // Linux's frames with bits flipped, and cut off: a refusal or a return
      uint32_t seed = 77 + uint32_t(words);
      for (int round = 0; round < 1500; ++round) {
        std::string changed = reference.bytes;
        for (int flips = 0; flips < 1 + round % 4; ++flips) {
          seed = seed * 1664525u + 1013904223u;
          changed[(seed >> 8) % changed.size()] ^= char(1u << (seed >> 29));
        }
        CHECK(put(rig, at, changed));
        LinuxSignalContext r = registers(at);
        linuxabi::signal_return(p, &r);
      }
      for (uint64_t have_bytes = 0; have_bytes < reference.gap - 8; have_bytes += 24) {
        CHECK(put(rig, 0x30000 - have_bytes, reference.bytes.substr(0, have_bytes)));
        LinuxSignalContext r = registers(0x30000 - have_bytes);
        CHECK(!linuxabi::signal_return(p, &r));
      }
    }
  }
}

// The generic table (AArch64, RISC-V, LoongArch) has the same calls under other numbers.
static void test_generic_numbers() {
  filesystem::RamFileSystem fs(A);
  populate(fs);
  for (const UserArch arch : {UserArch::AArch64, UserArch::RiscV64, UserArch::LoongArch64}) {
    Rig rig(&fs, 5, arch);
    const uint64_t buf = rig.alloc(256);
    CHECK(rig.s(260, uint64_t(-1), 0, 1, 0) == -10 && rig.s(95, 0, 0, buf, 5, 0) == -10);      // wait4, waitid
    const std::string link = "/tmp/link-" + std::to_string(int(arch));
    CHECK(rig.s(36, rig.str("motd"), kAtFdCwd, rig.str(link)) == 0 && rig.s(78, kAtFdCwd, rig.str(link), buf, 64) == 4);      // symlinkat, readlinkat
    const uint64_t m = rig.call(222, 0, 4096, 3, 0x22, ~uint64_t{0}, 0);
    CHECK(int64_t(m) > 0 && rig.call(216, m, 4096, 8192, 1) != 0 && rig.s(232, kBad, 4096, buf) == -12 && rig.s(227, kBad, 4096, 4) == -12);      // mremap, mincore, msync
    CHECK(rig.s(155, 0) == 5 && rig.s(157) == 5 && rig.s(156, 0) == 5 && rig.s(146, 12) == 0 && rig.s(174) == 12 && rig.s(175) == 12);      // getpgid, setsid, getsid, setuid, getuid, geteuid
    CHECK(rig.s(163, 7, buf) == 0 && rig.u64(buf) == 1024 && rig.s(165, 0, buf) == 0 && rig.s(153, 0) >= 0 && rig.s(179, buf) == 0);      // getrlimit, getrusage, times, sysinfo
    CHECK(rig.s(136, buf, 8) == 0 && rig.s(102, 0, buf) == 0 && rig.s(141, 0, 0) == 20 && rig.s(168, buf, 0, 0) == 0 && rig.s(283, 0, 0) == 25);      // rt_sigpending, getitimer, getpriority, getcpu, membarrier
    CHECK(rig.s(51, rig.str("/")) == -1 && rig.s(8, rig.str("/etc/motd"), rig.str("user.a"), buf, 8) == -95 && rig.s(436, 100, 200, 0) == 0);      // chroot, getxattr, close_range
    CHECK(rig.s(220, 17, 0, 0, 0, 0) == -38 && rig.s(57, 3) == -9);                    // clone as a fork (nobody to make it); 57 is close here, not fork
    rig.call(129, 5, 15);                                                              // kill
    CHECK(rig.process->exited() && rig.process->exit_code() == 143);
  }
}

// Pointers and sizes a hostile program would pass: nothing may crash, read or write outside the program's memory.
static void test_hostile_arguments() {
  filesystem::RamFileSystem fs(A);
  populate(fs);
  static NoWaitSync sync;
  ObjectManager objects(sync);
  objects.set_allocator(&A);
  UnixSocketManager sockets(sync, A);      // (without one the system makes its own on first use, which lives on)
  set_system_objects(&objects);
  set_system_unix_sockets(&sockets);
  {
    const uint64_t pointers[] = {0, 1, 0xFFF, 0x2FFFF, 0x30000, kBad, 0x00007FFFFFFFFFFFull, 0x0000800000000000ull, 0xFFFF800000000000ull, ~uint64_t{0}, ~uint64_t{7}};
    const uint64_t numbers[] = {13, 14, 15, 25, 26, 27, 28, 34, 36, 37, 38, 40, 61, 62, 78, 85, 88, 95, 97, 98, 99, 100, 105, 109, 115, 116, 117, 118, 120, 121, 125, 126, 127, 128, 130, 131,
                                135, 140, 141, 142, 143, 144, 145, 148, 149, 157, 160, 187, 200, 203, 221, 234, 247, 266, 273, 274, 295, 296, 302, 309, 314, 315, 316, 324,
                                326, 327, 328, 436, 437};
    for (const uint64_t number : numbers) {
      for (const uint64_t pointer : pointers) {
        Rig rig(&fs);      // (a new one each time: a call may end the process)
        const int64_t fd = rig.s(2, rig.str("/etc/motd"), 0);
        // (the alarm and timer calls with a huge time are fine too: nothing is waited for)
        // pause and sigsuspend wait until a signal with a handler is there: one is, before each call
        const bool waits = number == 34 || number == 130;
        if (waits) rig.call(13, 10, rig.words({0x400100, 0, 0, 0}), 0, 8);
        auto arm = [&] { if (waits) rig.process->linux_state().pending = 1 << 9; };
        arm(); rig.call(number, pointer, pointer, pointer, pointer, pointer, pointer);
        arm(); rig.call(number, uint64_t(fd), pointer, pointer, pointer, pointer, pointer);
        arm(); rig.call(number, 0, 0x10000, pointer, pointer, 8, pointer);
        arm(); rig.call(number, 1, pointer, 0x10000, 8, pointer, 0);
      }
    }
  }
  set_system_unix_sockets(nullptr);
  set_system_objects(nullptr);
}

// LinuxSyscallTest [<the directory of tests/data/emulation>]
int main(int argc, char** argv) {
  if (argc > 1) g_data_directory = argv[1];
  struct { const char* name; void (*run)(); } const tests[] = {
      {"files", test_files}, {"record locks", test_record_locks}, {"memory", test_memory}, {"address space copy", test_address_space_copy}, {"signals", test_signals},
      {"identity", test_identity}, {"limits and scheduling", test_limits_and_scheduling}, {"prctl", test_prctl}, {"fork and wait", test_fork_and_wait},
      {"fork inheritance", test_fork_inheritance}, {"signal handlers", test_signal_handlers}, {"signal handlers, AArch64 and RISC-V", test_signal_handlers_other_cpus},
      {"signal handlers, LoongArch", test_signal_handlers_loongarch}, {"generic numbers", test_generic_numbers}, {"hostile arguments", test_hostile_arguments}};
  for (const auto& test : tests) {
    test.run();
    if (A.live != 0) {      // every test gives back what it took
      printf("FAILED: %d allocation(s) left after \"%s\"\n", A.live, test.name);
      ++g_failed;
      A.live = 0;
    }
  }
  CHECK(A.live == 0);
  if (g_failed) { printf("LinuxSyscallTest: %d check(s) failed\n", g_failed); return 1; }
  printf("LinuxSyscallTest: all passed\n");
  return 0;
}
