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
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// The Linux system-call interface: the calls a statically linked program needs to talk to the console, read the clock, sleep and exit. Numbers differ by
// CPU: x86-64 has its own table, AArch64 and RISC-V 64 share the "generic" one. A call not listed returns -ENOSYS, which is what libc expects from a
// kernel that lacks it. Results are the value or -errno in the result register.
namespace process {

namespace {

constexpr int64_t kEnomem = 12, kEnodev = 19, kEbadf = 9, kEfault = 14, kEinval = 22, kEnosys = 38;
constexpr uint64_t kIovMax = 1024, kMaxIoBytes = 0x7FFFF000u;      // what Linux allows in one read or write

enum class Call : uint8_t { None, Read, Write, Writev, Close, Getpid, Gettid, Getuid, Getgid, Exit, ExitGroup, Uname, ClockGettime, Nanosleep, SchedYield, SetTidAddress, Brk, Mmap, Munmap, Mprotect };

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
};

uint64_t error(int64_t errno_value) { return uint64_t(-errno_value); }

Call translate(UserArch arch, uint64_t number) {
  for (const NumberEntry& entry : kNumbers) {
    if (number == (arch == UserArch::X86_64 ? entry.x86_64 : entry.generic)) return entry.call;
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

uint64_t do_write(Process& process, uint64_t fd, uint64_t address, uint64_t count) {
  const DescriptorKind kind = process.descriptor(fd);
  if (kind == DescriptorKind::Closed) return error(kEbadf);
  if (kind == DescriptorKind::ConsoleInput) return error(kEbadf);        // opened read-only
  if (count > kMaxIoBytes) count = kMaxIoBytes;
  bool faulted = false;
  const uint64_t done = copy_user_to_console(process, kind == DescriptorKind::ConsoleError ? 2 : 1, address, count, &faulted);
  if (faulted && done == 0) return error(kEfault);
  return done;
}

uint64_t do_read(Process& process, uint64_t fd, uint64_t address, uint64_t count) {
  const DescriptorKind kind = process.descriptor(fd);
  if (kind != DescriptorKind::ConsoleInput) return error(kEbadf);
  if (count > kMaxIoBytes) count = kMaxIoBytes;
  bool faulted = false;
  const uint64_t got = copy_console_to_user(process, address, count, &faulted);
  if (faulted) return error(kEfault);
  return got;
}

uint64_t do_writev(Process& process, uint64_t fd, uint64_t iov, uint64_t count) {
  if (count > kIovMax) return error(kEinval);
  if (process.descriptor(fd) == DescriptorKind::Closed) return error(kEbadf);
  uint64_t total = 0;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t base = 0, length = 0;
    if (iov > ~uint64_t{0} - i * 16 - 15) return total ? total : error(kEfault);
    if (!process.read_user_u64(iov + i * 16, &base) || !process.read_user_u64(iov + i * 16 + 8, &length)) return total ? total : error(kEfault);
    if (length > kMaxIoBytes - total) return total ? total : error(kEinval);          // the sum must stay within what one call may move
    const uint64_t result = do_write(process, fd, base, length);
    if (int64_t(result) < 0) return total ? total : result;
    total += result;
    if (result < length) break;      // a short write ends the call
  }
  return total;
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

}  // namespace

class LinuxSyscalls final : public ISyscallInterface {
 public:
  explicit constexpr LinuxSyscalls(UserArch arch) : arch_(arch) {}
  SyscallAbi abi() const override { return SyscallAbi::Linux; }
  UserArch arch() const override { return arch_; }

  uint64_t dispatch(Process& process, const SyscallRequest& r) const override {
    const uint64_t* a = r.args;
    switch (translate(arch_, r.number)) {
      case Call::Read: return do_read(process, a[0], a[1], a[2]);
      case Call::Write: return do_write(process, a[0], a[1], a[2]);
      case Call::Writev: return do_writev(process, a[0], a[1], a[2]);
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
      default: return error(kEnosys);
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
