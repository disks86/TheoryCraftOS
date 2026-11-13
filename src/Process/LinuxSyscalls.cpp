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
#include "Process/LinuxSockets.hpp"
#include "Process/LinuxThreads.hpp"
#include "Process/MemoryFile.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// The Linux system-call interface: the calls a statically linked program needs to talk to the console, read the clock, sleep and exit, its files, threads, and
// AF_UNIX sockets (LinuxSockets.cpp, shared with Windows programs). Numbers differ by
// CPU: x86-64 has its own table, AArch64 and RISC-V 64 share the "generic" one. A call not listed returns -ENOSYS, which is what libc expects from a
// kernel that lacks it. Results are the value or -errno in the result register.
namespace process {

namespace {

constexpr int64_t kEperm = 1, kEnoent = 2, kEnxio = 6, kEnotty = 25, kEspipe = 29, kEnomem = 12, kEacces = 13, kEnodev = 19, kEexist = 17, kEbadf = 9, kEfault = 14, kEinval = 22, kEnosys = 38;
constexpr uint64_t kMaxIoBytes = 0x7FFFF000u;      // what Linux allows in one read or write
constexpr uint64_t kAtFdCwd = ~uint64_t{99};      // -100

enum class Call : uint8_t { None, Clone3, Read, Write, Writev, Close, Getpid, Gettid, Getuid, Getgid, Getresid, ClockGetres, Exit, ExitGroup, Uname, ClockGettime, Nanosleep, ClockNanosleep, SchedYield, SetTidAddress, Brk, Mmap, Munmap, Mprotect, ArchPrctl, Ioctl, Fstat, Newfstatat, Lseek, Openat, Readlinkat, RtSigaction, RtSigprocmask, Sigaltstack,
  SetRobustList, Rseq, Prlimit64, Getrandom, Gettimeofday, Time, Getppid, Madvise,
  Open, Stat, Lstat, Pread, Pwrite, Readv, Getdents64, Mkdir, Mkdirat, Mknod, Mknodat, Rmdir, Unlink, Unlinkat, Rename, Renameat, Renameat2, Getcwd, Chdir, Fchdir, Access, Faccessat,
  Truncate, Ftruncate, Fsync, Dup, Dup2, Dup3, Fcntl, Readlink, Statx, Umask, Clone, Futex, Tgkill, SchedGetaffinity, Prctl, Succeed,
  Socket, Socketpair, Pipe, Pipe2, Eventfd, Eventfd2, TimerfdCreate, TimerfdSettime, TimerfdGettime, Signalfd, Signalfd4, EpollCreate, EpollCreate1,
  EpollCtl, EpollWait, EpollPwait, EpollPwait2, Bind, Connect, Listen, Accept, Accept4, Sendto, Recvfrom, Sendmsg, Recvmsg, Sendmmsg, Recvmmsg, Shutdown, Getsockname, Getpeername,
  Setsockopt, Getsockopt, Poll, Ppoll, Select, Pselect6, MemfdCreate, Fallocate };
constexpr uint16_t kAbsent = 0xFFFF;      // this CPU's table has no such call

struct NumberEntry {
  uint16_t x86_64, generic;
  Call call;
};

constexpr NumberEntry kNumbers[] = {
    {0, 63, Call::Read},           {1, 64, Call::Write},        {20, 66, Call::Writev},       {3, 57, Call::Close},
    {39, 172, Call::Getpid},       {186, 178, Call::Gettid},    {102, 174, Call::Getuid},     {104, 176, Call::Getgid},
    {107, 175, Call::Getuid},      {108, 177, Call::Getgid},    {118, 148, Call::Getresid},   {120, 150, Call::Getresid},      // (geteuid, getegid, getresuid, getresgid: root)
    {60, 93, Call::Exit},          {231, 94, Call::ExitGroup},  {63, 160, Call::Uname},       {228, 113, Call::ClockGettime},
    {229, 114, Call::ClockGetres}, {35, 101, Call::Nanosleep},    {230, 115, Call::ClockNanosleep}, {24, 124, Call::SchedYield}, {218, 96, Call::SetTidAddress},
    {12, 214, Call::Brk},          {9, 222, Call::Mmap},        {11, 215, Call::Munmap},      {10, 226, Call::Mprotect},
    {158, kAbsent, Call::ArchPrctl}, {16, 29, Call::Ioctl},     {5, 80, Call::Fstat},         {262, 79, Call::Newfstatat},
    {8, 62, Call::Lseek},          {257, 56, Call::Openat},     {267, 78, Call::Readlinkat},  {13, 134, Call::RtSigaction},
    {14, 135, Call::RtSigprocmask}, {131, 132, Call::Sigaltstack}, {273, 99, Call::SetRobustList}, {334, 293, Call::Rseq},
    {302, 261, Call::Prlimit64},   {318, 278, Call::Getrandom}, {96, 169, Call::Gettimeofday}, {201, kAbsent, Call::Time}, {110, 173, Call::Getppid},
    {28, 233, Call::Madvise},
    {2, kAbsent, Call::Open},      {4, kAbsent, Call::Stat},    {6, kAbsent, Call::Lstat},    {17, 67, Call::Pread},
    {18, 68, Call::Pwrite},        {19, 65, Call::Readv},       {217, 61, Call::Getdents64},  {83, kAbsent, Call::Mkdir},
    {258, 34, Call::Mkdirat},      {133, kAbsent, Call::Mknod}, {259, 33, Call::Mknodat},      {84, kAbsent, Call::Rmdir},  {87, kAbsent, Call::Unlink},  {263, 35, Call::Unlinkat},
    {82, kAbsent, Call::Rename},   {264, 38, Call::Renameat},   {316, 276, Call::Renameat2},  {79, 17, Call::Getcwd},
    {80, 49, Call::Chdir},         {81, 50, Call::Fchdir},      {21, kAbsent, Call::Access},  {269, 48, Call::Faccessat},
    {439, 439, Call::Faccessat},   {76, 45, Call::Truncate},    {77, 46, Call::Ftruncate},    {74, 82, Call::Fsync},
    {75, 83, Call::Fsync},         {32, 23, Call::Dup},         {33, kAbsent, Call::Dup2},    {292, 24, Call::Dup3},
    {72, 25, Call::Fcntl},         {89, kAbsent, Call::Readlink}, {332, 291, Call::Statx},    {95, 166, Call::Umask},
    {90, kAbsent, Call::Succeed},  {91, 52, Call::Succeed},     {268, 53, Call::Succeed},     {92, kAbsent, Call::Succeed},
    {93, 55, Call::Succeed},       {94, kAbsent, Call::Succeed}, {260, 54, Call::Succeed},    {280, 88, Call::Succeed},
    {162, 81, Call::Succeed},
    {56, 220, Call::Clone},        {435, 435, Call::Clone3},        {202, 98, Call::Futex},      {234, 131, Call::Tgkill},     {200, 130, Call::Tgkill},     {204, 123, Call::SchedGetaffinity},
    {203, 122, Call::Succeed},     {157, 167, Call::Prctl},
    // sockets (AF_UNIX: LinuxSockets.cpp)
    {22, kAbsent, Call::Pipe},     {293, 59, Call::Pipe2},      {284, kAbsent, Call::Eventfd}, {290, 19, Call::Eventfd2},
    {283, 85, Call::TimerfdCreate}, {286, 86, Call::TimerfdSettime}, {287, 87, Call::TimerfdGettime}, {282, kAbsent, Call::Signalfd},
    {289, 74, Call::Signalfd4},    {213, kAbsent, Call::EpollCreate}, {291, 20, Call::EpollCreate1}, {233, 21, Call::EpollCtl},
    {232, kAbsent, Call::EpollWait}, {281, 22, Call::EpollPwait}, {441, 441, Call::EpollPwait2},
    {41, 198, Call::Socket},       {53, 199, Call::Socketpair}, {49, 200, Call::Bind},       {42, 203, Call::Connect},
    {50, 201, Call::Listen},       {43, 202, Call::Accept},     {288, 242, Call::Accept4},   {44, 206, Call::Sendto},
    {45, 207, Call::Recvfrom},     {46, 211, Call::Sendmsg},    {47, 212, Call::Recvmsg},    {307, 269, Call::Sendmmsg},
    {299, 243, Call::Recvmmsg},    {48, 210, Call::Shutdown},   {51, 204, Call::Getsockname}, {52, 205, Call::Getpeername},
    {54, 208, Call::Setsockopt},   {55, 209, Call::Getsockopt}, {7, kAbsent, Call::Poll},    {271, 73, Call::Ppoll},
    {23, kAbsent, Call::Select},   {270, 72, Call::Pselect6},
    // files in memory (MemoryFile.hpp): what Wayland clients share their pictures through
    {319, 279, Call::MemfdCreate}, {285, 47, Call::Fallocate},
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
    case UserArch::LoongArch64: return "loongarch64";
    default: return "riscv64";
  }
}


constexpr uint64_t kMapShared = 0x1, kMapPrivate = 0x2, kMapFixed = 0x10, kMapAnonymous = 0x20;
constexpr uint64_t kMapOtherAccepted = 0x100 | 0x800 | 0x4000 | 0x8000 | 0x10000 | 0x20000 | 0x40000;      // GROWSDOWN, DENYWRITE, NORESERVE, POPULATE, NONBLOCK, STACK, HUGETLB: hints we may ignore
constexpr uint64_t kMapFixedNoreplace = 0x100000;      // MAP_FIXED_NOREPLACE: at exactly that address, EEXIST if anything is mapped there

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

// mmap() of a file in memory (memfd_create): its block is mapped as it is (MAP_SHARED), or as a copy-on-write view (MAP_PRIVATE). The pages past the
// block's end cannot be mapped (ENXIO; Linux would map them and fault on use). A GPU image (a dmabuf of /dev/dxg) maps the CPU's copy of it the same way.
// A regular file (a library ld.so maps, a data file): the pages are a copy of the file made now, private to the mapping. Bytes past the end of the file
// are zero. A shared mapping that is only read is the same copy (what another process writes to the file later is not seen); a shared mapping for
// writing is not supported (ENODEV), as before.
uint64_t map_file_copy(Process& process, AddressSpace& space, OpenObject& object, uint64_t address, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset) {
  if (!object.file) return error(kEnodev);      // (a directory, a link opened itself)
  if (!object.readable || object.no_data_access) return error(kEacces);
  if ((flags & kMapShared) && (prot & kProtWrite)) return error(kEnodev);
  uint64_t where = 0;
  if (flags & kMapFixed) {
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    if (!space.unmap(address, size)) return error(kEnomem);
    where = address;
  } else if (!space.find_free(size, address ? address : process.mmap_cursor(), &where)) {
    return error(kEnomem);
  }
  if (!space.map(where, size, uint8_t(prot))) return error(kEnomem);
  const uint64_t file_size = object.file->size();
  const uint64_t available = offset < file_size ? file_size - offset : 0;
  const uint64_t copy = available < size ? available : size;
  uint8_t chunk[16384];
  for (uint64_t done = 0; done < copy;) {
    const uint64_t want = copy - done < sizeof(chunk) ? copy - done : sizeof(chunk);
    uint64_t got = 0;
    if (object.file->read(offset + done, ByteView(chunk, want), &got) != filesystem::Status::Ok || got == 0 ||
        !space.write_privileged(where + done, ConstByteView(chunk, got))) {
      space.unmap(where, size);
      return error(kEnomem);
    }
    done += got;
  }
  if (!(flags & kMapFixed) && !address) process.set_mmap_cursor(where + size);
  return where;
}

uint64_t map_memory_file(Process& process, AddressSpace& space, uint64_t address, uint64_t size, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
  OpenObject* object = process.files().object(fd);
  if (!object) return error(kEbadf);
  if (offset % AddressSpace::kPageSize != 0) return error(kEinval);
  if (!object->memory && !object->gpu_buffer) return map_file_copy(process, space, *object, address, size, prot, flags, offset);
  const bool shared_write = (flags & kMapShared) && (prot & kProtWrite);
  BlockRef block;
  if (object->gpu_buffer) {      // a GPU image: the CPU's copy of it (DxgBufferFile.hpp); a dmabuf is mapped for writing whatever its descriptor allows, as on Linux
    block = object->gpu_buffer->map_block(shared_write);
    if (!block) return error(kEnodev);
  } else {
    if (!object->readable || (shared_write && !object->writable)) return error(kEacces);
    if (shared_write && (object->memory->seals() & (MemoryFile::kSealWrite | MemoryFile::kSealFutureWrite))) return error(kEperm);
    uint64_t file_size = 0;
    block = object->memory->block(&file_size);
  }
  if (!block || offset >= block.get()->capacity() || size > block.get()->capacity() - offset) return error(kEnxio);
  uint64_t where = 0;
  if (flags & kMapFixed) {
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    if (!space.unmap(address, size)) return error(kEnomem);
    where = address;
  } else if (!space.find_free(size, address ? address : process.mmap_cursor(), &where)) {
    return error(kEnomem);
  }
  if (!space.map_shared(where, size, uint8_t(prot), block.get()->storage() + offset, block.get())) return error(kEnomem);
  if (flags & kMapPrivate) {      // (written pages get copies of their own: AddressSpace::keeps_private_copies)
    PageAttributes attributes;
    attributes.type = PageAttributes::Mapped;
    attributes.protect = attributes.allocation_protect = 0x08;      // PAGE_WRITECOPY
    space.set_attributes(where, size, attributes);
  }
  if (!(flags & kMapFixed) && !address) process.set_mmap_cursor(where + size);
  return where;
}

uint64_t do_mmap(Process& process, uint64_t address, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  uint64_t size = 0;
  if (length == 0 || !AddressSpace::round_up(length, &size) || !valid_protection(prot)) return error(kEinval);
  if (flags & ~(kMapShared | kMapPrivate | kMapFixed | kMapAnonymous | kMapFixedNoreplace | kMapOtherAccepted)) return error(kEinval);
  if (((flags & kMapShared) != 0) == ((flags & kMapPrivate) != 0)) return error(kEinval);      // exactly one of the two
  if (flags & kMapFixedNoreplace) {
    uint64_t free_at = 0;
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    if (!space->find_free(size, address, &free_at) || free_at != address) return error(kEexist);
    flags = (flags & ~kMapFixedNoreplace) | kMapFixed;
  }
  if (!(flags & kMapAnonymous)) return map_memory_file(process, *space, address, size, prot, flags, fd, offset);
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
      case Call::Gettid: return linuxabi::sys_gettid(process);
      case Call::Getuid:
      case Call::Getgid: return 0;
      case Call::ClockGetres: {      // every clock counts in nanoseconds (libdbus asks, for its timeouts)
        if (a[0] > 11) return error(kEinval);
        if (a[1] && (!process.write_user_u64(a[1], 0) || !process.write_user_u64(a[1] + 8, 1))) return error(kEfault);
        return 0;
      }
      case Call::Getresid: {      // real, effective and saved ids: all 0 (libdbus takes a failure here for a setuid program and ignores the environment)
        const uint8_t zero[4] = {};
        for (uint32_t i = 0; i < 3; ++i)
          if (!process.write_user(a[i], ConstByteView(zero, 4))) return error(kEfault);
        return 0;
      }
      case Call::SetTidAddress: return linuxabi::sys_set_tid_address(process, a[0]);
      case Call::SchedYield: process.block_yield(); return 0;
      case Call::Exit: return linuxabi::sys_thread_exit(process, uint32_t(a[0] & 0xFF));      // (a thread; the last one takes the process with it)
      case Call::ExitGroup: process.terminate(uint32_t(a[0] & 0xFF)); return 0;
      case Call::Clone: return linuxabi::sys_clone(process, arch_, a);
      case Call::Clone3: return linuxabi::sys_clone3(process, arch_, a[0], a[1]);
      case Call::Futex: return linuxabi::sys_futex(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::Tgkill: return 0;      // signals are not delivered; asking whether a thread exists, or sending one, succeeds
      case Call::SchedGetaffinity: return linuxabi::sys_sched_getaffinity(process, a[1], a[2]);
      case Call::Prctl:      // PR_SET_NAME and friends succeed; the SVE vector-length calls (50 set, 51 get) and the SME ones (62 set, 63 get) report the emulated AArch64 vector length, 16 bytes
        if ((a[0] == 50 || a[0] == 51 || a[0] == 62 || a[0] == 63) && arch_ == UserArch::AArch64) return 16;
        return 0;
      case Call::Uname: return write_uname(process, a[0]);
      case Call::ClockGettime: return clock_gettime(process, a[0], a[1]);
      case Call::Nanosleep: return nanosleep(process, a[0], a[1]);
      case Call::ClockNanosleep: return clock_nanosleep(process, a[0], a[1], a[2]);
      case Call::Brk: return do_brk(process, a[0]);
      case Call::Mmap: return do_mmap(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::Munmap: return do_munmap(process, a[0], a[1]);
      case Call::Mprotect: return do_mprotect(process, a[0], a[1], a[2]);
      case Call::ArchPrctl: return do_arch_prctl(process, a[0], a[1]);
      case Call::Ioctl: return linuxabi::sys_ioctl(process, a[0], a[1], a[2]);
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
      case Call::Mknod: return linuxabi::sys_mknodat(process, kAtFdCwd, a[0], a[1], a[2]);
      case Call::Mknodat: return linuxabi::sys_mknodat(process, a[0], a[1], a[2], a[3]);
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
      case Call::Time: {      // time(tloc): the seconds since 1970, also stored at tloc when it is not null (x86-64 only: the generic table has no time)
        const uint64_t seconds = process.host().realtime_ns() / 1000000000u;
        if (a[0] != 0 && !process.write_user_u64(a[0], seconds)) return error(kEfault);
        return seconds;
      }
      case Call::Getppid: return 0;
      case Call::MemfdCreate: return linuxabi::sys_memfd_create(process, a[0], a[1]);
      case Call::Fallocate: return linuxabi::sys_fallocate(process, a[0], a[1], a[2], a[3]);
      case Call::Socket: return linuxabi::sys_socket(process, a[0], a[1], a[2]);
      case Call::Socketpair: return linuxabi::sys_socketpair(process, a[0], a[1], a[2], a[3]);
      case Call::Pipe: return linuxabi::sys_pipe2(process, a[0], 0);
      case Call::Pipe2: return linuxabi::sys_pipe2(process, a[0], a[1]);
      case Call::Eventfd: return linuxabi::sys_eventfd2(process, a[0], 0);
      case Call::Eventfd2: return linuxabi::sys_eventfd2(process, a[0], a[1]);
      case Call::TimerfdCreate: return linuxabi::sys_timerfd_create(process, a[0], a[1]);
      case Call::TimerfdSettime: return linuxabi::sys_timerfd_settime(process, a[0], a[1], a[2], a[3]);
      case Call::TimerfdGettime: return linuxabi::sys_timerfd_gettime(process, a[0], a[1]);
      case Call::Signalfd: return linuxabi::sys_signalfd4(process, a[0], a[1], a[2], 0);
      case Call::Signalfd4: return linuxabi::sys_signalfd4(process, a[0], a[1], a[2], a[3]);
      case Call::EpollCreate: return int32_t(a[0]) <= 0 ? error(kEinval) : linuxabi::sys_epoll_create1(process, 0);
      case Call::EpollCreate1: return linuxabi::sys_epoll_create1(process, a[0]);
      case Call::EpollCtl: return linuxabi::sys_epoll_ctl(process, a[0], a[1], a[2], a[3]);
      case Call::EpollWait: case Call::EpollPwait: return linuxabi::sys_epoll_wait(process, a[0], a[1], a[2], a[3]);
      case Call::EpollPwait2: return linuxabi::sys_epoll_pwait2(process, a[0], a[1], a[2], a[3]);
      case Call::Bind: return linuxabi::sys_bind(process, a[0], a[1], a[2]);
      case Call::Connect: return linuxabi::sys_connect(process, a[0], a[1], a[2]);
      case Call::Listen: return linuxabi::sys_listen(process, a[0], a[1]);
      case Call::Accept: return linuxabi::sys_accept4(process, a[0], a[1], a[2], 0);
      case Call::Accept4: return linuxabi::sys_accept4(process, a[0], a[1], a[2], a[3]);
      case Call::Sendto: return linuxabi::sys_sendto(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::Recvfrom: return linuxabi::sys_recvfrom(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::Sendmsg: return linuxabi::sys_sendmsg(process, a[0], a[1], a[2]);
      case Call::Recvmsg: return linuxabi::sys_recvmsg(process, a[0], a[1], a[2]);
      case Call::Sendmmsg: return linuxabi::sys_sendmmsg(process, a[0], a[1], a[2], a[3]);
      case Call::Recvmmsg: return linuxabi::sys_recvmmsg(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Shutdown: return linuxabi::sys_shutdown(process, a[0], a[1]);
      case Call::Getsockname: return linuxabi::sys_getsockname(process, a[0], a[1], a[2]);
      case Call::Getpeername: return linuxabi::sys_getpeername(process, a[0], a[1], a[2]);
      case Call::Setsockopt: return linuxabi::sys_setsockopt(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Getsockopt: return linuxabi::sys_getsockopt(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Poll: {      // (timeout in milliseconds; negative waits for ever)
        const int64_t ms = int32_t(a[2]);
        return linuxabi::sys_poll(process, a[0], a[1], ms < 0 ? ~uint64_t{0} : uint64_t(ms) * 1000000u);
      }
      case Call::Ppoll: return linuxabi::sys_ppoll(process, a[0], a[1], a[2]);
      case Call::Select: return linuxabi::sys_select(process, a[0], a[1], a[2], a[3], a[4], false);
      case Call::Pselect6: return linuxabi::sys_select(process, a[0], a[1], a[2], a[3], a[4], true);
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
    process.block_sleep(seconds > kMaxSeconds ? ~uint64_t{0} : seconds * 1000000000u + nanos);
    if (remaining && (!process.write_user_u64(remaining, 0) || !process.write_user_u64(remaining + 8, 0))) return error(kEfault);
    return 0;
  }

  // clock_nanosleep (what glibc's nanosleep and sleep call): a relative time, or with TIMER_ABSTIME a time of the clock to sleep until (none if it passed).
  // Sleeps are never interrupted, so the time left is never written.
  static uint64_t clock_nanosleep(Process& process, uint64_t clock, uint64_t flags, uint64_t request) {
    constexpr uint64_t kTimerAbstime = 1;
    if (clock != 0 && clock != 1 && clock != 4 && clock != 7) return error(kEinval);      // REALTIME, MONOTONIC, MONOTONIC_RAW, BOOTTIME
    if (flags & ~kTimerAbstime) return error(kEinval);
    uint64_t seconds = 0, nanos = 0;
    if (!process.read_user_u64(request, &seconds) || !process.read_user_u64(request + 8, &nanos)) return error(kEfault);
    if (int64_t(seconds) < 0 || nanos >= 1000000000u) return error(kEinval);
    const uint64_t kMaxSeconds = (~uint64_t{0} - 999999999u) / 1000000000u;
    uint64_t ns = seconds > kMaxSeconds ? ~uint64_t{0} : seconds * 1000000000u + nanos;
    if (flags & kTimerAbstime) {
      const uint64_t now = clock == 0 ? process.host().realtime_ns() : process.host().monotonic_ns();
      ns = ns > now ? ns - now : 0;
    }
    if (ns) process.block_sleep(ns);
    return 0;
  }

  UserArch arch_;
};

namespace {
// Constant-initialised: no run-time setup, nothing to guard.
constexpr LinuxSyscalls kX86(UserArch::X86_64), kArm(UserArch::AArch64), kRiscV(UserArch::RiscV64), kLoong(UserArch::LoongArch64);
}  // namespace

namespace linuxabi {
uint64_t map_user_memory(Process& process, uint64_t bytes) {
  const uint64_t r = do_mmap(process, 0, bytes, kProtRead | kProtWrite, kMapPrivate | kMapAnonymous, ~uint64_t{0}, 0);
  return r > ~uint64_t{4095} ? 0 : r;      // (-errno is a failure)
}
void unmap_user_memory(Process& process, uint64_t address, uint64_t bytes) {
  uint64_t size = 0;
  if (address && AddressSpace::round_up(bytes ? bytes : 1, &size)) do_munmap(process, address, size);
}
}  // namespace linuxabi

const ISyscallInterface& linux_interface(UserArch arch) {
  return arch == UserArch::X86_64 ? kX86 : arch == UserArch::AArch64 ? kArm : arch == UserArch::LoongArch64 ? kLoong : kRiscV;
}

}  // namespace process
