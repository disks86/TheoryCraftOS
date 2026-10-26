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
#include "Process/AddressSpace.hpp"
#include "Process/LinuxFiles.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// The Linux system-call interface: the calls a statically linked program needs to talk to the console, read the clock, sleep and exit. Numbers differ by
// CPU: x86-64 has its own table, AArch64 and RISC-V 64 share the "generic" one. A call not listed returns -ENOSYS, which is what libc expects from a
// kernel that lacks it. Results are the value or -errno in the result register.
namespace process {

namespace {

constexpr int64_t kEnoent = 2, kEnotty = 25, kEspipe = 29, kEnomem = 12, kEnodev = 19, kEbadf = 9, kEfault = 14, kEinval = 22, kEnosys = 38;
constexpr uint64_t kMaxIoBytes = 0x7FFFF000u;      // what Linux allows in one read or write
constexpr uint64_t kAtFdCwd = ~uint64_t{99};      // -100

enum class Call : uint8_t { None, Read, Write, Writev, Close, Getpid, Gettid, Getuid, Getgid, Exit, ExitGroup, Uname, ClockGettime, Nanosleep, SchedYield, SetTidAddress, Brk, Mmap, Munmap, Mprotect, ArchPrctl, Ioctl, Fstat, Newfstatat, Lseek, Openat, Readlinkat, RtSigaction, RtSigprocmask, Sigaltstack,
  SetRobustList, Rseq, Prlimit64, Getrandom, Gettimeofday, Getppid, Madvise,
  Open, Stat, Lstat, Pread, Pwrite, Readv, Getdents64, Mkdir, Mkdirat, Rmdir, Unlink, Unlinkat, Rename, Renameat, Renameat2, Getcwd, Chdir, Fchdir, Access, Faccessat,
  Truncate, Ftruncate, Fsync, Dup, Dup2, Dup3, Fcntl, Readlink, Statx, Umask, Succeed };
constexpr uint16_t kAbsent = 0xFFFF;      // this CPU's table has no such call

struct NumberEntry {
  uint16_t x86_64, generic;
  Call call;
};

constexpr NumberEntry kNumbers[] = {
    {0, 63, Call::Read},           {1, 64, Call::Write},        {20, 66, Call::Writev},       {3, 57, Call::Close},
    {39, 172, Call::Getpid},       {186, 178, Call::Gettid},    {102, 174, Call::Getuid},     {104, 176, Call::Getgid},
    {60, 93, Call::Exit},          {231, 94, Call::ExitGroup},  {63, 160, Call::Uname},       {228, 113, Call::ClockGettime},
    {35, 101, Call::Nanosleep},    {24, 124, Call::SchedYield}, {218, 96, Call::SetTidAddress},
    {12, 214, Call::Brk},          {9, 222, Call::Mmap},        {11, 215, Call::Munmap},      {10, 226, Call::Mprotect},
    {158, kAbsent, Call::ArchPrctl}, {16, 29, Call::Ioctl},     {5, 80, Call::Fstat},         {262, 79, Call::Newfstatat},
    {8, 62, Call::Lseek},          {257, 56, Call::Openat},     {267, 78, Call::Readlinkat},  {13, 134, Call::RtSigaction},
    {14, 135, Call::RtSigprocmask}, {131, 132, Call::Sigaltstack}, {273, 99, Call::SetRobustList}, {334, 293, Call::Rseq},
    {302, 261, Call::Prlimit64},   {318, 278, Call::Getrandom}, {96, 169, Call::Gettimeofday}, {110, 173, Call::Getppid},
    {28, 233, Call::Madvise},
    {2, kAbsent, Call::Open},      {4, kAbsent, Call::Stat},    {6, kAbsent, Call::Lstat},    {17, 67, Call::Pread},
    {18, 68, Call::Pwrite},        {19, 65, Call::Readv},       {217, 61, Call::Getdents64},  {83, kAbsent, Call::Mkdir},
    {258, 34, Call::Mkdirat},      {84, kAbsent, Call::Rmdir},  {87, kAbsent, Call::Unlink},  {263, 35, Call::Unlinkat},
    {82, kAbsent, Call::Rename},   {264, 38, Call::Renameat},   {316, 276, Call::Renameat2},  {79, 17, Call::Getcwd},
    {80, 49, Call::Chdir},         {81, 50, Call::Fchdir},      {21, kAbsent, Call::Access},  {269, 48, Call::Faccessat},
    {439, 439, Call::Faccessat},   {76, 45, Call::Truncate},    {77, 46, Call::Ftruncate},    {74, 82, Call::Fsync},
    {75, 83, Call::Fsync},         {32, 23, Call::Dup},         {33, kAbsent, Call::Dup2},    {292, 24, Call::Dup3},
    {72, 25, Call::Fcntl},         {89, kAbsent, Call::Readlink}, {332, 291, Call::Statx},    {95, 166, Call::Umask},
    {90, kAbsent, Call::Succeed},  {91, 52, Call::Succeed},     {268, 53, Call::Succeed},     {92, kAbsent, Call::Succeed},
    {93, 55, Call::Succeed},       {94, kAbsent, Call::Succeed}, {260, 54, Call::Succeed},    {280, 88, Call::Succeed},
    {162, 81, Call::Succeed},
};

uint64_t error(int64_t errno_value) { return uint64_t(-errno_value); }

Call translate(UserArch arch, uint64_t number) {
  for (const NumberEntry& entry : kNumbers) {
    const uint16_t wanted = arch == UserArch::X86_64 ? entry.x86_64 : entry.generic;
    if (wanted != kAbsent && number == wanted) return entry.call;
  }
  return Call::None;
}

StringView machine_name(UserArch arch) {
  switch (arch) {
    case UserArch::X86_64: return "x86_64";
    case UserArch::AArch64: return "aarch64";
    default: return "riscv64";
  }
}


constexpr uint64_t kMapShared = 0x1, kMapPrivate = 0x2, kMapFixed = 0x10, kMapAnonymous = 0x20;
constexpr uint64_t kMapOtherAccepted = 0x100 | 0x800 | 0x4000 | 0x8000 | 0x10000 | 0x40000;      // GROWSDOWN, DENYWRITE, NORESERVE, POPULATE, NONBLOCK, STACK/HUGETLB: hints we may ignore

bool valid_protection(uint64_t prot) { return (prot & ~uint64_t{kProtRead | kProtWrite | kProtExecute}) == 0; }

// brk(addr): moves the program break within the heap region right after the image. Like Linux, a request it cannot meet returns the unchanged break.
uint64_t do_brk(Process& process, uint64_t request) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  const uint64_t start = process.heap_start(), current = process.heap_break();
  if (request < start) return current;      // includes 0: "where is it?"
  uint64_t old_end = 0, new_end = 0;
  if (!AddressSpace::round_up(current, &old_end) || !AddressSpace::round_up(request, &new_end)) return current;
  if (new_end == old_end) { process.set_heap_break(request); return request; }
  const uint64_t old_size = old_end - start, new_size = new_end - start;
  bool ok;
  if (old_size == 0) ok = space->map(start, new_size, kProtRead | kProtWrite);
  else ok = space->resize(start, new_size);
  if (!ok) return current;
  process.set_heap_break(request);
  return request;
}

uint64_t do_mmap(Process& process, uint64_t address, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  (void)fd;
  uint64_t size = 0;
  if (length == 0 || !AddressSpace::round_up(length, &size) || !valid_protection(prot)) return error(kEinval);
  if (flags & ~(kMapShared | kMapPrivate | kMapFixed | kMapAnonymous | kMapOtherAccepted)) return error(kEinval);
  if (((flags & kMapShared) != 0) == ((flags & kMapPrivate) != 0)) return error(kEinval);      // exactly one of the two
  if (!(flags & kMapAnonymous)) return error(kEnodev);      // files cannot be mapped yet
  uint64_t where = 0;
  if (flags & kMapFixed) {
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    if (!space->unmap(address, size) || !space->map(address, size, uint8_t(prot))) return error(kEnomem);
    return address;
  }
  if (!space->find_free(size, address ? address : process.mmap_cursor(), &where)) return error(kEnomem);
  if (!space->map(where, size, uint8_t(prot))) return error(kEnomem);
  if (!address) process.set_mmap_cursor(where + size);
  return where;
}

uint64_t do_munmap(Process& process, uint64_t address, uint64_t length) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  if (length == 0 || address % AddressSpace::kPageSize != 0) return error(kEinval);
  return space->unmap(address, length) ? 0 : error(kEinval);
}

uint64_t do_mprotect(Process& process, uint64_t address, uint64_t length, uint64_t prot) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  if (address % AddressSpace::kPageSize != 0 || !valid_protection(prot)) return error(kEinval);
  if (length == 0) return 0;
  return space->protect(address, length, uint8_t(prot)) ? 0 : error(kEnomem);      // part of the range is not mapped
}

// --- thread pointer, terminal-ish and signal calls that C libraries make while starting up -------------------------------------------------------------

constexpr uint64_t kArchSetGs = 0x1001, kArchSetFs = 0x1002, kArchGetFs = 0x1003, kArchGetGs = 0x1004;

uint64_t do_arch_prctl(Process& process, uint64_t code, uint64_t address) {
  switch (code) {
    case kArchSetFs:
    case kArchSetGs:
      if (address >= AddressSpace::kUserTop) return error(kEinval);      // must be a user address (this also keeps it canonical)
      if (code == kArchSetFs) process.set_fs_base(address);
      else process.set_gs_base(address);
      return 0;
    case kArchGetFs: return process.write_user_u64(address, process.fs_base()) ? 0 : error(kEfault);
    case kArchGetGs: return process.write_user_u64(address, process.gs_base()) ? 0 : error(kEfault);
    default: return error(kEinval);
  }
}

// Writes `count` zero bytes to user memory (an empty structure the program asked to be filled).
bool write_zeros(Process& process, uint64_t address, uint64_t count) {
  uint8_t zeros[256] = {};
  return count <= sizeof(zeros) && process.write_user(address, ConstByteView(zeros, count));
}

uint64_t do_prlimit(Process& process, uint64_t resource, uint64_t old_limit) {
  if (!old_limit) return 0;
  const uint64_t infinity = ~uint64_t{0};
  const uint64_t current = resource == 3 ? 8ull << 20 : infinity;      // RLIMIT_STACK is the 8 MiB the stack was mapped with
  return process.write_user_u64(old_limit, current) && process.write_user_u64(old_limit + 8, resource == 3 ? infinity : infinity) ? 0 : error(kEfault);
}

uint64_t do_getrandom(Process& process, uint64_t address, uint64_t length) {
  if (length > kMaxIoBytes) length = kMaxIoBytes;
  uint8_t chunk[256];
  uint64_t done = 0;
  while (done < length) {
    const uint64_t n = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
    if (!process.host().random_bytes(ByteView(chunk, n))) return done ? done : error(kEnosys);
    if (!process.write_user(address + done, ConstByteView(chunk, n))) return done ? done : error(kEfault);
    done += n;
  }
  return done;
}

uint64_t do_gettimeofday(Process& process, uint64_t tv) {
  if (!tv) return 0;
  const uint64_t ns = process.host().realtime_ns();
  return process.write_user_u64(tv, ns / 1000000000u) && process.write_user_u64(tv + 8, ns % 1000000000u / 1000) ? 0 : error(kEfault);
}


}  // namespace

class LinuxSyscalls final : public ISyscallInterface {
 public:
  explicit constexpr LinuxSyscalls(UserArch arch) : arch_(arch) {}
  SyscallAbi abi() const override { return SyscallAbi::Linux; }
  UserArch arch() const override { return arch_; }

  uint64_t dispatch(Process& process, const SyscallRequest& r) const override {
    const uint64_t* a = r.args;
    switch (translate(arch_, r.number)) {
      case Call::Read: return linuxabi::sys_read(process, a[0], a[1], a[2]);
      case Call::Write: return linuxabi::sys_write(process, a[0], a[1], a[2]);
      case Call::Writev: return linuxabi::sys_writev(process, a[0], a[1], a[2]);
      case Call::Close: return process.close_descriptor(a[0]) ? 0 : error(kEbadf);
      case Call::Getpid: return process.id();
      case Call::Gettid: return process.id();        // one thread per process so far
      case Call::Getuid:
      case Call::Getgid: return 0;
      case Call::SetTidAddress: return process.id();
      case Call::SchedYield: process.host().yield(); return 0;
      case Call::Exit:
      case Call::ExitGroup: process.terminate(uint32_t(a[0] & 0xFF)); return 0;
      case Call::Uname: return write_uname(process, a[0]);
      case Call::ClockGettime: return clock_gettime(process, a[0], a[1]);
      case Call::Nanosleep: return nanosleep(process, a[0], a[1]);
      case Call::Brk: return do_brk(process, a[0]);
      case Call::Mmap: return do_mmap(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Munmap: return do_munmap(process, a[0], a[1]);
      case Call::Mprotect: return do_mprotect(process, a[0], a[1], a[2]);
      case Call::ArchPrctl: return do_arch_prctl(process, a[0], a[1]);
      case Call::Ioctl: return linuxabi::sys_ioctl(process, a[0]);
      case Call::Fstat: return linuxabi::sys_fstat(process, arch_, a[0], a[1]);
      case Call::Newfstatat: return linuxabi::sys_statat(process, arch_, a[0], a[1], a[2], a[3]);
      case Call::Stat:
      case Call::Lstat: return linuxabi::sys_statat(process, arch_, kAtFdCwd, a[0], a[1], 0);
      case Call::Statx: return linuxabi::sys_statx(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Lseek: return linuxabi::sys_lseek(process, a[0], a[1], a[2]);
      case Call::Openat: return linuxabi::sys_openat(process, arch_, a[0], a[1], a[2]);
      case Call::Open: return linuxabi::sys_openat(process, arch_, kAtFdCwd, a[0], a[1]);
      case Call::Readlinkat: return linuxabi::sys_readlinkat(process, a[0], a[1]);
      case Call::Readlink: return linuxabi::sys_readlinkat(process, kAtFdCwd, a[0]);
      case Call::Pread: return linuxabi::sys_pread(process, a[0], a[1], a[2], a[3]);
      case Call::Pwrite: return linuxabi::sys_pwrite(process, a[0], a[1], a[2], a[3]);
      case Call::Readv: return linuxabi::sys_readv(process, a[0], a[1], a[2]);
      case Call::Getdents64: return linuxabi::sys_getdents64(process, a[0], a[1], a[2]);
      case Call::Mkdir: return linuxabi::sys_mkdirat(process, kAtFdCwd, a[0]);
      case Call::Mkdirat: return linuxabi::sys_mkdirat(process, a[0], a[1]);
      case Call::Rmdir: return linuxabi::sys_unlinkat(process, kAtFdCwd, a[0], 0x200);
      case Call::Unlink: return linuxabi::sys_unlinkat(process, kAtFdCwd, a[0], 0);
      case Call::Unlinkat: return linuxabi::sys_unlinkat(process, a[0], a[1], a[2]);
      case Call::Rename: return linuxabi::sys_renameat(process, kAtFdCwd, a[0], kAtFdCwd, a[1]);
      case Call::Renameat:
      case Call::Renameat2: return linuxabi::sys_renameat(process, a[0], a[1], a[2], a[3]);
      case Call::Getcwd: return linuxabi::sys_getcwd(process, a[0], a[1]);
      case Call::Chdir: return linuxabi::sys_chdir(process, a[0]);
      case Call::Fchdir: return linuxabi::sys_fchdir(process, a[0]);
      case Call::Access: return linuxabi::sys_faccessat(process, kAtFdCwd, a[0], a[1]);
      case Call::Faccessat: return linuxabi::sys_faccessat(process, a[0], a[1], a[2]);
      case Call::Truncate: return linuxabi::sys_truncate(process, a[0], a[1]);
      case Call::Ftruncate: return linuxabi::sys_ftruncate(process, a[0], a[1]);
      case Call::Fsync: return linuxabi::sys_fsync(process, a[0]);
      case Call::Dup: return linuxabi::sys_dup(process, a[0]);
      case Call::Dup2: return linuxabi::sys_dup3(process, a[0], a[1], ~uint64_t{0});
      case Call::Dup3: return linuxabi::sys_dup3(process, a[0], a[1], a[2]);
      case Call::Fcntl: return linuxabi::sys_fcntl(process, a[0], a[1], a[2]);
      case Call::Umask: return 022;
      case Call::Succeed: return 0;      // permissions, owners, times and syncs: there is nothing to change here
      case Call::RtSigaction: return a[2] && !write_zeros(process, a[2], 32) ? error(kEfault) : 0;
      case Call::RtSigprocmask: return a[2] && !write_zeros(process, a[2], 8) ? error(kEfault) : 0;
      case Call::Sigaltstack: return a[1] && !write_zeros(process, a[1], 24) ? error(kEfault) : 0;
      case Call::SetRobustList:
      case Call::Madvise: return 0;
      case Call::Rseq: return error(kEnosys);
      case Call::Prlimit64: return do_prlimit(process, a[1], a[3]);
      case Call::Getrandom: return do_getrandom(process, a[0], a[1]);
      case Call::Gettimeofday: return do_gettimeofday(process, a[0]);
      case Call::Getppid: return 0;
      default: process.host().unhandled_syscall(SyscallAbi::Linux, r.number); return error(kEnosys);
    }
  }

 private:
  uint64_t write_uname(Process& process, uint64_t address) const {
    constexpr uint32_t kField = 65;
    uint8_t buffer[kField * 6] = {};
    ByteView all(buffer, sizeof(buffer));
    const StringView fields[6] = {"Linux", "tcos", "6.1.0-tcos", "#1 TheoryCraftOS", machine_name(arch_), ""};
    for (uint32_t i = 0; i < 6; ++i) {
      all.subview(i * kField, kField).copy_from(text_bytes(fields[i].data()));      // every field is a literal, so NUL-terminated and shorter than kField
    }
    return process.write_user(address, ConstByteView(buffer, sizeof(buffer))) ? 0 : error(kEfault);
  }

  static uint64_t clock_gettime(Process& process, uint64_t clock, uint64_t address) {
    uint64_t ns = 0;
    if (clock == 0) ns = process.host().realtime_ns();            // CLOCK_REALTIME
    else if (clock == 1 || clock == 4) ns = process.host().monotonic_ns();      // CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW
    else return error(kEinval);
    if (!process.write_user_u64(address, ns / 1000000000u) || !process.write_user_u64(address + 8, ns % 1000000000u)) return error(kEfault);
    return 0;
  }

  static uint64_t nanosleep(Process& process, uint64_t request, uint64_t remaining) {
    uint64_t seconds = 0, nanos = 0;
    if (!process.read_user_u64(request, &seconds) || !process.read_user_u64(request + 8, &nanos)) return error(kEfault);
    if (int64_t(seconds) < 0 || nanos >= 1000000000u) return error(kEinval);
    const uint64_t kMaxSeconds = (~uint64_t{0} - 999999999u) / 1000000000u;
    process.host().sleep_ns(seconds > kMaxSeconds ? ~uint64_t{0} : seconds * 1000000000u + nanos);
    if (remaining && (!process.write_user_u64(remaining, 0) || !process.write_user_u64(remaining + 8, 0))) return error(kEfault);
    return 0;
  }

  UserArch arch_;
};

namespace {
// Constant-initialised: no run-time setup, nothing to guard.
constexpr LinuxSyscalls kX86(UserArch::X86_64), kArm(UserArch::AArch64), kRiscV(UserArch::RiscV64);
}  // namespace

const ISyscallInterface& linux_interface(UserArch arch) {
  return arch == UserArch::X86_64 ? kX86 : arch == UserArch::AArch64 ? kArm : kRiscV;
}

}  // namespace process
