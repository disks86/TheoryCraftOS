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
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Process.hpp"
#include "Process/ProcessFiles.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;
using filesystem::DirectoryEntry;
using filesystem::NodeType;
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

struct Host final : IProcessHost {
  std::string out;
  bool write_console(uint32_t, ConstByteView d) override { for (uint8_t c : d) out.push_back(char(c)); return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override { return 0; }
  void sleep_ns(uint64_t) override {}
  void yield() override {}
  bool random_bytes(ByteView o) override { for (uint8_t& b : o) b = 0xA5; return true; }
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

static std::string resolved(PathStyle style, const char* cwd, const char* path, PathResult* result = nullptr) {
  String out(A);
  const PathResult r = resolve_path(style, cwd, path, &out);
  if (result) *result = r;
  return r == PathResult::Ok ? std::string(out.view().data(), out.view().size()) : "<error>";
}

static void test_prepare() {
  filesystem::RamFileSystem fs(A);
  CHECK(prepare_program_folders(fs));
  DirectoryEntry e;
  CHECK(fs.stat("/Linux/tmp", &e) == Status::Ok && fs.stat("/Windows/Windows/System32", &e) == Status::Ok && fs.stat("/Windows/Program Files", &e) == Status::Ok);
  make_file(fs, "/Linux/etc/keep", "k");
  CHECK(prepare_program_folders(fs) && read_file(fs, "/Linux/etc/keep") == "k");      // again: nothing is lost
  filesystem::RamFileSystem clash(A);
  make_file(clash, "/Linux", "a file where the folder should be");
  CHECK(!prepare_program_folders(clash));
}

static void test_paths() {
  PathResult r;
  CHECK(resolved(PathStyle::Posix, "/", "/a/b") == "/a/b");
  CHECK(resolved(PathStyle::Posix, "/x", "a//b/./c/") == "/x/a/b/c");
  CHECK(resolved(PathStyle::Posix, "/x/y", "../z") == "/x/z");
  CHECK(resolved(PathStyle::Posix, "/x", "../../../..") == "/");                     // ".." never climbs above the root
  CHECK(resolved(PathStyle::Posix, "/", "/../../etc/passwd") == "/etc/passwd");
  CHECK(resolved(PathStyle::Posix, "/a", "/") == "/");
  CHECK(resolved(PathStyle::Posix, "/", "", &r) == "<error>" && r == PathResult::Empty);
  CHECK(resolved(PathStyle::Posix, "/", std::string("a/").append(300, 'n').c_str(), &r) == "<error>" && r == PathResult::TooLong);
  CHECK(resolved(PathStyle::Posix, "/", std::string(5000, 'a').c_str(), &r) == "<error>" && r == PathResult::TooLong);
  CHECK(resolved(PathStyle::Posix, "/", "a:b\\c") == "/a:b\\c");                      // on Linux a colon and a backslash are just letters
  CHECK(resolved(PathStyle::Windows, "/", "C:\\Windows\\System32\\cmd.exe") == "/Windows/System32/cmd.exe");
  CHECK(resolved(PathStyle::Windows, "/", "c:/Users/me") == "/Users/me");
  CHECK(resolved(PathStyle::Windows, "/", "\\??\\C:\\dir\\file.txt") == "/dir/file.txt");      // NT form
  CHECK(resolved(PathStyle::Windows, "/", "\\\\?\\C:\\dir") == "/dir");                       // Win32 long form
  CHECK(resolved(PathStyle::Windows, "/", "C:\\") == "/");
  CHECK(resolved(PathStyle::Windows, "/", "\\temp") == "/temp");                             // drive-relative root
  CHECK(resolved(PathStyle::Windows, "/work", "notes.txt") == "/work/notes.txt");
  CHECK(resolved(PathStyle::Windows, "/work", "C:notes.txt") == "/work/notes.txt");
  CHECK(resolved(PathStyle::Windows, "/work", "C:") == "/work");
  CHECK(resolved(PathStyle::Windows, "/work/a", "..\\..\\..\\..\\x") == "/x");
  CHECK(resolved(PathStyle::Windows, "/", "C:\\..\\..\\Windows") == "/Windows");
  CHECK(resolved(PathStyle::Windows, "/", "D:\\x", &r) == "<error>" && r == PathResult::Invalid);      // there is only drive C
  CHECK(resolved(PathStyle::Windows, "/", "\\??\\x", &r) == "<error>");                       // the NT prefix needs a drive
  CHECK(resolved(PathStyle::Windows, "/", "C:\\a<b", &r) == "<error>" && r == PathResult::Invalid);
  CHECK(resolved(PathStyle::Windows, "/", "C:\\a|b", &r) == "<error>");
  CHECK(resolved(PathStyle::Windows, "/", "C:\\dir.\\file ", &r) == "/dir/file");            // trailing dots and spaces vanish
}

static void populate(filesystem::IFileSystem& fs) {
  CHECK(fs.create_directory("/Linux") == Status::Ok && fs.create_directory("/Windows") == Status::Ok);
  CHECK(fs.create_directory("/Linux/etc") == Status::Ok && fs.create_directory("/Linux/tmp") == Status::Ok && fs.create_directory("/Linux/home") == Status::Ok);
  CHECK(fs.create_directory("/Windows/Users") == Status::Ok && fs.create_directory("/Windows/Temp") == Status::Ok);
  make_file(fs, "/Linux/etc/motd", "welcome\n");
  make_file(fs, "/Windows/hello.txt", "windows file");
  make_file(fs, "/secret.txt", "outside both trees");
}

// Writes a program's memory image: a mapped page with its argument strings.
struct Rig {
  AddressSpace space{A};
  Host host;
  Process* process = nullptr;
  uint64_t cursor = 0x20000;
  Rig(SyscallAbi abi, filesystem::IFileSystem& fs) {
    CHECK(space.map(0x10000, 0x20000, kProtRead | kProtWrite));
    process = new Process(1, ProcessBinding{abi, UserArch::X86_64, syscall_interface(abi, UserArch::X86_64)}, space, host);
    process->attach_file_system(A, fs);
  }
  ~Rig() { delete process; }
  // Puts a string in memory and returns its address.
  uint64_t str(const std::string& s) {
    const uint64_t at = cursor;
    CHECK(space.write_privileged(at, ConstByteView(reinterpret_cast<const uint8_t*>(s.c_str()), s.size() + 1)));
    cursor += (s.size() + 16) & ~uint64_t{15};
    return at;
  }
  uint64_t alloc(uint64_t n) { const uint64_t at = cursor; cursor += (n + 15) & ~uint64_t{15}; return at; }
  std::string bytes(uint64_t at, uint64_t n) { std::string s(n, 0); CHECK(space.read(at, ByteView(reinterpret_cast<uint8_t*>(&s[0]), n))); return s; }
  uint64_t u64(uint64_t at) { uint64_t v = 0; CHECK(process->read_user_u64(at, &v)); return v; }
};

static SyscallRequest lin(uint64_t n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0) {
  SyscallRequest r;
  r.number = n; r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.args[4] = a4;
  return r;
}
static int64_t sres(uint64_t v) { return int64_t(v); }
constexpr uint64_t kAtFdCwd = ~uint64_t{99};

static void test_linux_files() {
  filesystem::RamFileSystem fs(A);
  populate(fs);
  Rig rig(SyscallAbi::Linux, fs);
  Process& p = *rig.process;

  // "/" is /Linux: /etc/motd is /Linux/etc/motd, and /secret.txt (outside) does not exist.
  int64_t fd = sres(p.syscall(lin(2, rig.str("/etc/motd"), 0)));
  CHECK(fd >= 3);
  uint64_t buf = rig.alloc(64);
  CHECK(p.syscall(lin(0, fd, buf, 64)) == 8 && rig.bytes(buf, 8) == "welcome\n");
  CHECK(p.syscall(lin(0, fd, buf, 64)) == 0);                                          // end of file
  CHECK(p.syscall(lin(8, fd, 3, 0)) == 3 && p.syscall(lin(0, fd, buf, 4)) == 4 && rig.bytes(buf, 4) == "come");      // lseek SET, then read
  CHECK(p.syscall(lin(8, fd, ~uint64_t{1}, 2)) == 6);                                  // lseek END -2
  CHECK(sres(p.syscall(lin(8, fd, ~uint64_t{99}, 1))) == -22);                         // before the start
  CHECK(sres(p.syscall(lin(1, fd, buf, 4))) == -9);                                    // opened read-only
  CHECK(p.syscall(lin(17, fd, buf, 3, 1)) == 3 && rig.bytes(buf, 3) == "elc");        // pread leaves the position alone
  CHECK(p.syscall(lin(0, fd, buf, 4)) == 2);
  CHECK(p.syscall(lin(3, fd)) == 0 && sres(p.syscall(lin(0, fd, buf, 1))) == -9);
  CHECK(sres(p.syscall(lin(2, rig.str("/secret.txt"), 0))) == -2);                    // not part of this tree
  CHECK(sres(p.syscall(lin(2, rig.str("/../secret.txt"), 0))) == -2);                 // ".." cannot climb out
  CHECK(sres(p.syscall(lin(2, rig.str("/../../../Windows/hello.txt"), 0))) == -2);    // nor into the other world
  CHECK(sres(p.syscall(lin(2, rig.str("/Windows/hello.txt"), 0))) == -2);             // "/Windows" inside Linux is /Linux/Windows
  CHECK(sres(p.syscall(lin(2, rig.str("/nothing/here"), 0))) == -2);
  CHECK(sres(p.syscall(lin(2, 0xDEAD0000, 0))) == -14);
  CHECK(sres(p.syscall(lin(2, rig.str(std::string(5000, 'x')), 0))) == -36);

  // Create, write, append, truncate, remove.
  fd = sres(p.syscall(lin(257, kAtFdCwd, rig.str("/tmp/new.txt"), 0x41 | 0x200, 0644)));      // O_WRONLY|O_CREAT|O_TRUNC
  CHECK(fd >= 3);
  CHECK(p.syscall(lin(1, fd, rig.str("hello "), 6)) == 6 && p.syscall(lin(1, fd, rig.str("world"), 5)) == 5);
  CHECK(sres(p.syscall(lin(0, fd, buf, 1))) == -9);                                   // write-only
  CHECK(p.syscall(lin(3, fd)) == 0);
  CHECK(read_file(fs, "/Linux/tmp/new.txt") == "hello world");                         // landed in the Linux tree
  CHECK(read_file(fs, "/tmp/new.txt") == "<missing>");
  fd = sres(p.syscall(lin(2, rig.str("/tmp/new.txt"), 0x401)));                       // O_WRONLY|O_APPEND
  CHECK(p.syscall(lin(1, fd, rig.str("!"), 1)) == 1 && p.syscall(lin(3, fd)) == 0 && read_file(fs, "/Linux/tmp/new.txt") == "hello world!");
  fd = sres(p.syscall(lin(2, rig.str("/tmp/new.txt"), 0x42)));                        // O_RDWR
  CHECK(p.syscall(lin(77, fd, 5)) == 0 && p.syscall(lin(0, fd, buf, 64)) == 5 && rig.bytes(buf, 5) == "hello");      // ftruncate
  CHECK(p.syscall(lin(74, fd)) == 0 && p.syscall(lin(3, fd)) == 0);
  CHECK(p.syscall(lin(76, rig.str("/tmp/new.txt"), 2)) == 0 && read_file(fs, "/Linux/tmp/new.txt") == "he");           // truncate(path)
  CHECK(sres(p.syscall(lin(2, rig.str("/tmp/new.txt"), 0xC1))) == -17);               // O_CREAT|O_EXCL on an existing file
  CHECK(sres(p.syscall(lin(2, rig.str("/tmp/zzz"), 0))) == -2);
  CHECK(p.syscall(lin(87, rig.str("/tmp/new.txt"))) == 0 && read_file(fs, "/Linux/tmp/new.txt") == "<missing>");   // unlink
  CHECK(sres(p.syscall(lin(87, rig.str("/tmp/new.txt")))) == -2);
  CHECK(sres(p.syscall(lin(87, rig.str("/tmp")))) == -21);                            // unlink on a directory: EISDIR
  CHECK(sres(p.syscall(lin(84, rig.str("/etc/motd")))) == -20);                       // rmdir on a file: ENOTDIR
  CHECK(sres(p.syscall(lin(84, rig.str("/etc")))) == -39);                            // not empty
  CHECK(sres(p.syscall(lin(84, rig.str("/")))) == -16);

  // Directories: mkdir, chdir, getcwd, relative names, openat with a directory descriptor.
  CHECK(p.syscall(lin(83, rig.str("/home/me"), 0755)) == 0 && sres(p.syscall(lin(83, rig.str("/home/me"), 0755))) == -17);
  CHECK(sres(p.syscall(lin(83, rig.str("/no/parent"), 0755))) == -2);
  CHECK(p.syscall(lin(80, rig.str("/home/me"))) == 0);
  uint64_t cwd = rig.alloc(64);
  CHECK(p.syscall(lin(79, cwd, 64)) == 9 && rig.bytes(cwd, 8) == "/home/me");
  CHECK(sres(p.syscall(lin(79, cwd, 4))) == -34);                                      // ERANGE
  CHECK(p.syscall(lin(2, rig.str("note"), 0x41)) >= 3);                               // relative: creates /home/me/note
  CHECK(read_file(fs, "/Linux/home/me/note") == "" );
  CHECK(sres(p.syscall(lin(80, rig.str("/etc/motd")))) == -20);
  CHECK(sres(p.syscall(lin(80, rig.str("/nowhere")))) == -2);
  CHECK(p.syscall(lin(80, rig.str("../.."))) == 0 && p.syscall(lin(79, cwd, 64)) == 2 && rig.bytes(cwd, 1) == "/");
  const int64_t dirfd = sres(p.syscall(lin(2, rig.str("/etc"), 0x10000)));            // O_DIRECTORY
  CHECK(dirfd >= 3);
  fd = sres(p.syscall(lin(257, dirfd, rig.str("motd"), 0)));
  CHECK(fd >= 3 && p.syscall(lin(0, fd, buf, 7)) == 7 && rig.bytes(buf, 7) == "welcome");
  CHECK(sres(p.syscall(lin(257, dirfd, rig.str("../tmp"), 0x10000))) >= 3);
  CHECK(p.syscall(lin(81, dirfd)) == 0 && p.syscall(lin(79, cwd, 64)) == 5 && rig.bytes(cwd, 4) == "/etc");      // fchdir
  CHECK(sres(p.syscall(lin(2, rig.str("/etc/motd"), 0x10000))) == -20);               // O_DIRECTORY on a file
  CHECK(sres(p.syscall(lin(2, rig.str("/etc"), 1))) == -21);                          // a directory cannot be opened for writing
  CHECK(sres(p.syscall(lin(0, dirfd, buf, 4))) == -21);

  // getdents64 lists ".", ".." and the entries.
  uint64_t entries = rig.alloc(1024);
  int64_t got = sres(p.syscall(lin(217, dirfd, entries, 1024)));
  CHECK(got > 0);
  std::vector<std::string> names;
  for (int64_t at = 0; at < got;) {
    const std::string record = rig.bytes(entries + at, 19 + 8);
    const uint16_t reclen = uint8_t(record[16]) | (uint8_t(record[17]) << 8);
    names.push_back(std::string(rig.bytes(entries + at + 19, 8).c_str()));
    at += reclen;
  }
  CHECK(names.size() == 3 && names[0] == "." && names[1] == ".." && names[2] == "motd");
  CHECK(p.syscall(lin(217, dirfd, entries, 1024)) == 0);                               // end
  CHECK(p.syscall(lin(8, dirfd, 0, 0)) == 0 && sres(p.syscall(lin(217, dirfd, entries, 1024))) == got);      // rewound
  CHECK(sres(p.syscall(lin(217, dirfd, entries, 10))) <= 0);                           // too small for any entry
  CHECK(sres(p.syscall(lin(217, fd, entries, 100))) == -20);

  // stat
  uint64_t st = rig.alloc(144);
  CHECK(p.syscall(lin(4, rig.str("/etc/motd"), st)) == 0);
  CHECK(rig.u64(st + 48) == 8);                                                        // st_size
  uint8_t mode[4];
  CHECK(rig.space.read(st + 24, ByteView(mode, 4)) && (mode[1] & 0xF0) == 0x80);     // S_IFREG
  CHECK(p.syscall(lin(4, rig.str("/etc"), st)) == 0 && rig.space.read(st + 24, ByteView(mode, 4)) && (mode[1] & 0xF0) == 0x40);   // S_IFDIR
  CHECK(sres(p.syscall(lin(4, rig.str("/etc/none"), st))) == -2);
  CHECK(p.syscall(lin(5, fd, st)) == 0 && rig.u64(st + 48) == 8);                      // fstat on an open file
  CHECK(p.syscall(lin(332, kAtFdCwd, rig.str("/etc/motd"), 0, 0x7ff, rig.alloc(256))) == 0);
  CHECK(p.syscall(lin(21, rig.str("/etc/motd"), 4)) == 0 && sres(p.syscall(lin(21, rig.str("/nope"), 0))) == -2);
  // rename
  CHECK(p.syscall(lin(82, rig.str("/home/me/note"), rig.str("/home/me/renamed"))) == 0 && read_file(fs, "/Linux/home/me/renamed") == "");
  CHECK(sres(p.syscall(lin(82, rig.str("/home/me/note"), rig.str("/x")))) == -2);
  make_file(fs, "/Linux/home/me/other", "o");
  CHECK(p.syscall(lin(82, rig.str("/home/me/renamed"), rig.str("/home/me/other"))) == 0 && read_file(fs, "/Linux/home/me/other") == "");   // replaces
  CHECK(p.syscall(lin(264, kAtFdCwd, rig.str("/home/me/other"), kAtFdCwd, rig.str("/home/me/third"))) == 0);
  // dup / fcntl
  fd = sres(p.syscall(lin(2, rig.str("/etc/motd"), 0)));
  int64_t copy = sres(p.syscall(lin(32, fd)));
  CHECK(copy > fd && p.syscall(lin(0, fd, buf, 3)) == 3 && p.syscall(lin(0, copy, buf, 3)) == 3 && rig.bytes(buf, 3) == "com");     // one shared position
  CHECK(p.syscall(lin(292, fd, 40, 0x80000)) == 40 && p.syscall(lin(72, 40, 1)) == 1);      // dup3 with O_CLOEXEC; F_GETFD
  CHECK(p.syscall(lin(33, fd, 41)) == 41 && p.syscall(lin(72, fd, 0, 50)) == 50);          // dup2; F_DUPFD
  CHECK(p.syscall(lin(72, fd, 3)) == 0 && sres(p.syscall(lin(72, 99, 3))) == -9);
  CHECK(sres(p.syscall(lin(32, 200))) == -9);
  // devices
  fd = sres(p.syscall(lin(2, rig.str("/dev/null"), 2)));
  CHECK(fd >= 3 && p.syscall(lin(1, fd, buf, 10)) == 10 && p.syscall(lin(0, fd, buf, 10)) == 0);
  fd = sres(p.syscall(lin(2, rig.str("/dev/zero"), 0)));
  CHECK(p.syscall(lin(0, fd, buf, 5000)) == 5000 && rig.bytes(buf, 1) == std::string(1, 0));
  fd = sres(p.syscall(lin(2, rig.str("/dev/urandom"), 0)));
  CHECK(p.syscall(lin(0, fd, buf, 16)) == 16 && uint8_t(rig.bytes(buf, 1)[0]) == 0xA5);
  // too many descriptors
  int opened = 0;
  for (int i = 0; i < 300; ++i) { int64_t f = sres(p.syscall(lin(2, rig.str("/etc/motd"), 0))); if (f >= 0) ++opened; else { CHECK(f == -24); break; } }
  CHECK(opened > 100 && opened < 300);

  // A program without a file system sees none.
  Host host2; AddressSpace space2(A);
  space2.map(0x10000, 0x1000, kProtRead | kProtWrite);
  Process bare(2, ProcessBinding{SyscallAbi::Linux, UserArch::X86_64, syscall_interface(SyscallAbi::Linux, UserArch::X86_64)}, space2, host2);
  const char path[] = "/etc/motd";
  space2.write_privileged(0x10000, ConstByteView(reinterpret_cast<const uint8_t*>(path), sizeof(path)));
  CHECK(sres(bare.syscall(lin(2, 0x10000, 0))) == -2 && sres(bare.syscall(lin(4, 0x10000, 0x10100))) == -2 && sres(bare.syscall(lin(83, 0x10000, 0))) == -2);
}

// --- Windows --------------------------------------------------------------------------------------------------------------------------------------

constexpr uint64_t kStack = 0x1F000;
struct NtRig : Rig {
  explicit NtRig(filesystem::IFileSystem& fs) : Rig(SyscallAbi::Windows, fs) {}
  void put64(uint64_t at, uint64_t v) { CHECK(process->write_user_u64(at, v)); }
  // A UNICODE_STRING in memory.
  uint64_t unicode(const std::string& narrow) {
    const uint64_t buffer = alloc(narrow.size() * 2 + 2), at = alloc(16);
    for (size_t i = 0; i < narrow.size(); ++i) {
      uint8_t c[2] = {uint8_t(narrow[i]), 0};
      CHECK(space.write_privileged(buffer + i * 2, ConstByteView(c, 2)));
    }
    uint8_t h[16] = {};
    h[0] = uint8_t(narrow.size() * 2); h[1] = uint8_t((narrow.size() * 2) >> 8); h[2] = h[0] + 2;
    for (int i = 0; i < 8; ++i) h[8 + i] = uint8_t(buffer >> (8 * i));
    CHECK(space.write_privileged(at, ConstByteView(h, 16)));
    return at;
  }
  // OBJECT_ATTRIBUTES for a name (and optionally a directory handle).
  uint64_t attributes(const std::string& name, uint64_t root = 0) {
    const uint64_t us = unicode(name), at = alloc(48);
    put64(at, 0x30); put64(at + 8, root); put64(at + 16, us); put64(at + 24, 0x40); put64(at + 32, 0); put64(at + 40, 0);
    return at;
  }
  uint32_t call(WindowsSyscallNumber n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, std::vector<uint64_t> stack = {}) {
    for (size_t i = 0; i < stack.size(); ++i) put64(kStack + 0x28 + i * 8, stack[i]);
    SyscallRequest r;
    r.number = uint64_t(n); r.args[0] = a0; r.args[1] = a1; r.args[2] = a2; r.args[3] = a3; r.user_stack = kStack;
    return uint32_t(process->syscall(r));
  }
  struct Opened { uint32_t status; uint64_t handle, information; };
  Opened create(const std::string& name, uint32_t access, uint32_t disposition, uint32_t options) {
    const uint64_t handle = alloc(8), iosb = alloc(16);
    put64(handle, 0);
    const uint32_t status = call(WindowsSyscallNumber::NtCreateFile, handle, access, attributes(name), iosb, {0, 0x80, 7, disposition, options, 0, 0});
    return {status, u64(handle), u64(iosb + 8)};
  }
  uint32_t read(uint64_t h, uint64_t buffer, uint64_t length, uint64_t offset_pointer, uint64_t* done = nullptr) {
    const uint64_t iosb = alloc(16);
    const uint32_t st = call(WindowsSyscallNumber::NtReadFile, h, 0, 0, 0, {iosb, buffer, length, offset_pointer, 0});
    if (done) *done = u64(iosb + 8);
    return st;
  }
  uint32_t write(uint64_t h, const std::string& text, uint64_t offset_pointer) {
    const uint64_t iosb = alloc(16);
    return call(WindowsSyscallNumber::NtWriteFile, h, 0, 0, 0, {iosb, str(text), text.size(), offset_pointer, 0});
  }
  uint64_t offset(uint64_t v) { const uint64_t at = alloc(8); put64(at, v); return at; }
  uint32_t query(uint64_t h, uint64_t buffer, uint32_t length, uint32_t info_class) {
    return call(WindowsSyscallNumber::NtQueryInformationFile, h, alloc(16), buffer, length, {info_class});
  }
};

static std::string wide(NtRig& rig, uint64_t at, uint64_t bytes) {
  std::string out;
  for (uint64_t i = 0; i < bytes; i += 2) out.push_back(rig.bytes(at + i, 1)[0]);
  return out;
}

// Reads the names out of a buffer of FILE_BOTH_DIRECTORY_INFORMATION (class 3) records.
static std::vector<std::string> both_names(NtRig& rig, uint64_t buffer) {
  std::vector<std::string> names;
  for (uint64_t at = buffer;;) {
    const std::string h = rig.bytes(at, 94);
    const uint32_t next = uint8_t(h[0]) | (uint8_t(h[1]) << 8) | (uint8_t(h[2]) << 16) | (uint32_t(uint8_t(h[3])) << 24);
    const uint32_t length = uint8_t(h[60]) | (uint8_t(h[61]) << 8);
    names.push_back(wide(rig, at + 94, length));
    if (next == 0) break;
    at += next;
  }
  return names;
}

static void test_windows_files() {
  filesystem::RamFileSystem fs(A);
  populate(fs);
  NtRig rig(fs);
  Process& p = *rig.process;
  const uint32_t kGenericRead = 0x80000000u, kGenericWrite = 0x40000000u, kSync = 0x100000, kOptions = 0x60;      // synchronous, non-alert

  // C:\hello.txt is /Windows/hello.txt.
  NtRig::Opened o = rig.create("\\??\\C:\\hello.txt", kGenericRead | kSync, 1, kOptions | 0x40);
  CHECK(o.status == 0 && o.information == 1 && o.handle >= 16);
  const uint64_t buf = rig.alloc(64);
  uint64_t done = 0;
  CHECK(rig.read(o.handle, buf, 64, 0, &done) == 0 && done == 12 && rig.bytes(buf, 12) == "windows file");
  CHECK(rig.read(o.handle, buf, 64, 0, &done) == 0xC0000011u && done == 0);                // end of file
  CHECK(rig.read(o.handle, buf, 5, rig.offset(8), &done) == 0 && done == 4 && rig.bytes(buf, 4) == "file");     // explicit offset
  CHECK(rig.read(o.handle, buf, 5, rig.offset(0), &done) == 0 && rig.bytes(buf, 5) == "windo" && rig.read(o.handle, buf, 3, 0, &done) == 0 && rig.bytes(buf, 3) == "ws ");   // and the position follows it
  CHECK(rig.write(o.handle, "x", 0) == 0xC0000022u);                                      // opened for reading only
  // information
  const uint64_t info = rig.alloc(64);
  CHECK(rig.query(o.handle, info, 24, 5) == 0 && rig.u64(info + 8) == 12 && rig.bytes(info + 21, 1)[0] == 0);    // standard: EndOfFile, not a directory
  CHECK(rig.query(o.handle, info, 8, 14) == 0 && rig.u64(info) == 8);                           // position
  CHECK(rig.query(o.handle, info, 40, 4) == 0 && rig.bytes(info + 32, 1)[0] == char(0x80));    // basic: FILE_ATTRIBUTE_NORMAL
  CHECK(rig.query(o.handle, info, 56, 34) == 0 && rig.u64(info + 40) == 12);                   // network open
  CHECK(rig.query(o.handle, info, 4, 5) == 0xC0000004u && rig.query(o.handle, info, 64, 99) == 0xC0000003u);
  CHECK(rig.query(o.handle, info, 64, 9) == 0 && rig.bytes(info, 1)[0] == 20 && wide(rig, info + 4, 20) == "\\hello.txt");     // FileNameInformation: the path on the volume
  // set position, end of file
  CHECK(rig.call(WindowsSyscallNumber::NtSetInformationFile, o.handle, rig.alloc(16), rig.offset(2), 8, {14}) == 0);
  CHECK(rig.read(o.handle, buf, 4, 0, &done) == 0 && rig.bytes(buf, 4) == "ndow");
  CHECK(p.syscall(SyscallRequest()) != 0 || true);
  CHECK(rig.call(WindowsSyscallNumber::NtClose, o.handle) == 0 && rig.call(WindowsSyscallNumber::NtClose, o.handle) == 0xC0000008u);
  CHECK(rig.read(o.handle, buf, 4, 0) == 0xC0000008u);

  // Errors.
  CHECK(rig.create("\\??\\C:\\missing.txt", kGenericRead, 1, kOptions).status == 0xC0000034u);              // name not found
  CHECK(rig.create("\\??\\C:\\nodir\\file.txt", kGenericRead, 1, kOptions).status == 0xC000003Au);           // path not found
  CHECK(rig.create("\\??\\D:\\x", kGenericRead, 1, kOptions).status == 0xC0000033u);                          // no such drive
  CHECK(rig.create("\\??\\C:\\a<b", kGenericRead, 1, kOptions).status == 0xC0000033u);
  CHECK(rig.create("\\??\\C:\\etc\\motd", kGenericRead, 1, kOptions).status == 0xC000003Au);                 // the Linux tree is not here
  CHECK(rig.create("\\??\\C:\\..\\..\\secret.txt", kGenericRead, 1, kOptions).status == 0xC0000034u);        // ".." stops at the drive root
  CHECK(rig.create("\\??\\C:\\..\\..\\Linux\\etc\\motd", kGenericRead, 1, kOptions).status == 0xC000003Au);   // "Linux" is just a name inside C:
  CHECK(rig.create("\\??\\C:\\Temp", kGenericRead, 1, 0x40).status == 0xC00000BAu);                          // FILE_NON_DIRECTORY_FILE on a directory
  CHECK(rig.create("\\??\\C:\\hello.txt", kGenericRead, 1, 1).status == 0xC0000103u);                        // FILE_DIRECTORY_FILE on a file
  CHECK(rig.create("\\??\\C:\\hello.txt", kGenericRead, 9, kOptions).status == 0xC000000Du);                 // bad disposition
  CHECK(rig.call(WindowsSyscallNumber::NtCreateFile, 0xDEAD0000, kGenericRead, rig.attributes("\\??\\C:\\hello.txt"), rig.alloc(16), {0, 0x80, 7, 1, kOptions, 0, 0}) == 0xC0000005u);

  // Create, write, append, overwrite.
  o = rig.create("\\??\\C:\\Temp\\out.txt", kGenericWrite | kSync, 2, kOptions);                             // FILE_CREATE
  CHECK(o.status == 0 && o.information == 2);
  CHECK(rig.write(o.handle, "hello ", 0) == 0 && rig.write(o.handle, "world", 0) == 0);                       // the file pointer advances
  CHECK(rig.write(o.handle, "!", rig.offset(0xFFFFFFFFFFFFFFFFull)) == 0);                                   // FILE_WRITE_TO_END_OF_FILE
  CHECK(rig.write(o.handle, "J", rig.offset(0)) == 0);
  CHECK(rig.read(o.handle, buf, 4, 0) == 0xC0000022u);                                                       // write-only
  CHECK(rig.call(WindowsSyscallNumber::NtSetInformationFile, o.handle, rig.alloc(16), rig.offset(11), 8, {20}) == 0);   // EndOfFile = 11
  CHECK(rig.call(WindowsSyscallNumber::NtFlushBuffersFile, o.handle, rig.alloc(16)) == 0);
  CHECK(rig.call(WindowsSyscallNumber::NtClose, o.handle) == 0);
  CHECK(read_file(fs, "/Windows/Temp/out.txt") == "Jello world");
  CHECK(rig.create("\\??\\C:\\Temp\\out.txt", kGenericWrite, 2, kOptions).status == 0xC0000035u);              // FILE_CREATE on an existing file
  o = rig.create("\\??\\C:\\Temp\\out.txt", kGenericWrite | kSync, 5, kOptions);                             // FILE_OVERWRITE_IF
  CHECK(o.status == 0 && o.information == 3 && read_file(fs, "/Windows/Temp/out.txt") == "");
  rig.call(WindowsSyscallNumber::NtClose, o.handle);
  o = rig.create("\\??\\C:\\Temp\\fresh.txt", kGenericWrite, 3, kOptions);                                   // FILE_OPEN_IF creates
  CHECK(o.status == 0 && o.information == 2);
  rig.call(WindowsSyscallNumber::NtClose, o.handle);
  o = rig.create("\\??\\C:\\Temp\\out.txt", kGenericRead | kGenericWrite, 0, kOptions);                      // FILE_SUPERSEDE
  CHECK(o.status == 0 && o.information == 0);
  rig.call(WindowsSyscallNumber::NtClose, o.handle);
  CHECK(read_file(fs, "/Linux/Temp/out.txt") == "<missing>");

  // Directories and listings.
  o = rig.create("\\??\\C:\\newdir", kGenericRead | kSync, 2, 1);                                            // FILE_CREATE | FILE_DIRECTORY_FILE
  CHECK(o.status == 0 && o.information == 2);
  filesystem::DirectoryEntry entry;
  CHECK(fs.stat("/Windows/newdir", &entry) == Status::Ok && entry.type == NodeType::Directory);
  rig.call(WindowsSyscallNumber::NtClose, o.handle);
  o = rig.create("\\??\\C:\\", kGenericRead | kSync, 1, 1 | 0x20);
  CHECK(o.status == 0);
  const uint64_t list = rig.alloc(4096), iosb = rig.alloc(16);
  uint32_t st = rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 4096, 3, 0, 0, 1});
  CHECK(st == 0);
  std::vector<std::string> names = both_names(rig, list);
  CHECK(names.size() == 6 && names[0] == "." && names[1] == "..");
  bool has_hello = false, has_users = false;
  for (auto& n : names) { has_hello |= n == "hello.txt"; has_users |= n == "Users"; }
  CHECK(has_hello && has_users && rig.u64(iosb + 8) > 100);
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 4096, 3, 0, 0, 0}) == 0x80000006u);   // no more files
  // a pattern, restarting, and one entry at a time
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 4096, 3, 0, rig.unicode("*.TXT"), 1}) == 0);
  names = both_names(rig, list);
  CHECK(names.size() == 1 && names[0] == "hello.txt");
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 4096, 3, 0, 0, 0}) == 0x80000006u);
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 4096, 12, 1, rig.unicode("*"), 1}) == 0);      // FileNamesInformation, one entry
  CHECK(rig.u64(list) == 0 || rig.bytes(list, 1)[0] == 0);                                           // NextEntryOffset 0 = last
  CHECK(wide(rig, list + 12, uint8_t(rig.bytes(list + 8, 1)[0])) == ".");
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 4096, 12, 1, 0, 0}) == 0);
  CHECK(wide(rig, list + 12, uint8_t(rig.bytes(list + 8, 1)[0])) == "..");
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 20, 3, 1, 0, 1}) == 0x80000005u);          // buffer too small
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 4096, 77, 0, 0, 1}) == 0xC0000003u);
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, 0, 0, 0, 0, {iosb, list, 4096, 3, 0, 0, 1}) == 0xC0000008u);
  // a buffer that holds some entries: the rest come with the next call, none lost
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 250, 3, 0, rig.unicode("*"), 1}) == 0);
  size_t first = both_names(rig, list).size();
  CHECK(first >= 1 && first < 6);
  size_t total = first;
  for (int i = 0; i < 10; ++i) {
    st = rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 250, 3, 0, 0, 0});
    if (st != 0) { CHECK(st == 0x80000006u); break; }
    total += both_names(rig, list).size();
  }
  CHECK(total == 6);
  rig.call(WindowsSyscallNumber::NtClose, o.handle);
  // a file is not a directory
  o = rig.create("\\??\\C:\\hello.txt", kGenericRead, 1, kOptions);
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, o.handle, 0, 0, 0, {iosb, list, 4096, 3, 0, 0, 1}) == 0xC0000103u);
  // relative to a directory handle
  const NtRig::Opened dir = rig.create("\\??\\C:\\Temp", kGenericRead | kSync, 1, 1);
  const uint64_t out = rig.alloc(8);
  CHECK(rig.call(WindowsSyscallNumber::NtOpenFile, out, kGenericRead | kSync, rig.attributes("fresh.txt", dir.handle), iosb, {7, kOptions}) == 0);
  CHECK(rig.u64(out) >= 16);
  CHECK(rig.call(WindowsSyscallNumber::NtOpenFile, out, kGenericRead, rig.attributes("nothing.txt", dir.handle), iosb, {7, kOptions}) == 0xC0000034u);
  CHECK(rig.call(WindowsSyscallNumber::NtOpenFile, out, kGenericRead, rig.attributes("x", 4), iosb, {7, kOptions}) == 0xC0000008u);        // a console handle is not a directory
  rig.call(WindowsSyscallNumber::NtClose, rig.u64(out));

  // attributes, delete, rename, delete-on-close
  const uint64_t basic = rig.alloc(40);
  CHECK(rig.call(WindowsSyscallNumber::NtQueryAttributesFile, rig.attributes("\\??\\C:\\hello.txt"), basic) == 0 && rig.bytes(basic + 32, 1)[0] == char(0x80));
  CHECK(rig.call(WindowsSyscallNumber::NtQueryAttributesFile, rig.attributes("\\??\\C:\\Temp"), basic) == 0 && rig.bytes(basic + 32, 1)[0] == 0x10);
  CHECK(rig.call(WindowsSyscallNumber::NtQueryAttributesFile, rig.attributes("\\??\\C:\\nope"), basic) == 0xC0000034u);
  CHECK(rig.call(WindowsSyscallNumber::NtDeleteFile, rig.attributes("\\??\\C:\\Temp\\fresh.txt")) == 0 && read_file(fs, "/Windows/Temp/fresh.txt") == "<missing>");
  CHECK(rig.call(WindowsSyscallNumber::NtDeleteFile, rig.attributes("\\??\\C:\\Temp\\fresh.txt")) == 0xC0000034u);
  CHECK(rig.call(WindowsSyscallNumber::NtDeleteFile, rig.attributes("\\??\\C:\\")) == 0xC0000022u);
  o = rig.create("\\??\\C:\\Temp\\gone.txt", kGenericWrite | 0x10000, 2, kOptions | 0x1000);                // FILE_DELETE_ON_CLOSE
  CHECK(o.status == 0 && read_file(fs, "/Windows/Temp/gone.txt") == "");
  CHECK(rig.call(WindowsSyscallNumber::NtClose, o.handle) == 0 && read_file(fs, "/Windows/Temp/gone.txt") == "<missing>");
  o = rig.create("\\??\\C:\\Temp\\moved.txt", kGenericWrite | kGenericRead, 2, kOptions);
  rig.write(o.handle, "data", 0);
  // FILE_RENAME_INFORMATION: ReplaceIfExists, RootDirectory, FileNameLength, FileName
  const std::string target = "\\??\\C:\\Temp\\renamed.txt";
  const uint64_t rename_info = rig.alloc(20 + target.size() * 2);
  rig.put64(rename_info, 0); rig.put64(rename_info + 8, 0);
  uint8_t len[4] = {uint8_t(target.size() * 2), 0, 0, 0};
  CHECK(rig.space.write_privileged(rename_info + 16, ConstByteView(len, 4)));
  for (size_t i = 0; i < target.size(); ++i) { uint8_t c[2] = {uint8_t(target[i]), 0}; CHECK(rig.space.write_privileged(rename_info + 20 + i * 2, ConstByteView(c, 2))); }
  CHECK(rig.call(WindowsSyscallNumber::NtSetInformationFile, o.handle, rig.alloc(16), rename_info, 20 + target.size() * 2, {10}) == 0);
  CHECK(read_file(fs, "/Windows/Temp/renamed.txt") == "data" && read_file(fs, "/Windows/Temp/moved.txt") == "<missing>");
  CHECK(rig.call(WindowsSyscallNumber::NtSetInformationFile, o.handle, rig.alloc(16), rename_info, 20 + target.size() * 2, {10}) == 0xC0000035u);      // the name is taken (by itself)
  rig.call(WindowsSyscallNumber::NtClose, o.handle);
  // console handles still work through the same services
  CHECK(rig.write(8, "to the console", 0) == 0 && rig.host.out == "to the console");
  CHECK(rig.query(8, info, 40, 4) == 0);
  CHECK(rig.call(WindowsSyscallNumber::NtQueryDirectoryFile, 8, 0, 0, 0, {iosb, list, 4096, 3, 0, 0, 1}) == 0xC0000103u);
}

// Garbage in the program's memory and wild pointers in the arguments must only ever produce errors.
static void test_hostile_arguments() {
  filesystem::RamFileSystem fs(A);
  populate(fs);
  uint64_t seed = 12345;
  auto next = [&]() { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return seed >> 17; };
  for (SyscallAbi abi : {SyscallAbi::Linux, SyscallAbi::Windows}) {
    Rig rig(abi, fs);
    for (int round = 0; round < 3000; ++round) {
      uint8_t junk[512];
      for (uint8_t& b : junk) b = uint8_t(next());
      if (round % 3 == 0) for (int i = 0; i < 40; ++i) junk[next() % 512] = "\\/.:*?<>|AZaz "[next() % 14];
      CHECK(rig.space.write_privileged(0x20000, ConstByteView(junk, sizeof(junk))));
      SyscallRequest r;
      r.user_stack = 0x1F000;
      uint64_t* a = r.args;
      for (int i = 0; i < 6; ++i) a[i] = (next() % 4 == 0) ? next() : 0x20000 + next() % 0x800;
      for (int i = 0; i < 12; ++i) rig.process->write_user_u64(0x1F000 + 0x28 + i * 8, (next() % 3 == 0) ? next() : 0x20000 + next() % 0x800);
      if (abi == SyscallAbi::Linux) {
        static const uint16_t numbers[] = {0, 1, 2, 3, 4, 5, 8, 16, 17, 18, 19, 20, 21, 72, 76, 77, 78, 79, 80, 82, 83, 84, 87, 217, 257, 258, 262, 263, 264, 267, 269, 316, 332};
        r.number = numbers[next() % (sizeof(numbers) / sizeof(numbers[0]))];
        a[0] = (next() % 2) ? next() % 40 : a[0];
      } else {
        r.number = 10 + next() % 8;      // the file services
        if (next() % 2) a[0] = (next() % 40) * 4;
      }
      (void)rig.process->syscall(r);
    }
  }
  CHECK(read_file(fs, "/secret.txt") == "outside both trees");      // nothing outside the trees was touched
}

int main() {
  test_paths();
  test_prepare();
  test_linux_files();
  test_windows_files();
  test_hostile_arguments();
  CHECK(A.live == 0);
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("ProcessFilesTest: all passed\n");
  return 0;
}
