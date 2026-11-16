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
#include "Process/LinuxProcess.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Process.hpp"
#include "Process/ProcessFiles.hpp"
#include "Utility/ByteStream.hpp"

// Linux calls that have little to do here but must answer as a Linux kernel on such a machine would: the memory policy of a machine with one memory node,
// protection keys on a processor without them, comparing two descriptors, another process's memory, and how much of a file is cached.
namespace process::linuxabi {

namespace {

constexpr int64_t kEperm = 1, kEsrch = 3, kEbadf = 9, kEfault = 14, kEnodev = 19, kEinval = 22, kEnospc = 28;
uint64_t error(int64_t e) { return uint64_t(-e); }
constexpr uint64_t kPage = AddressSpace::kPageSize;

// A node mask: `max_node` bits at `address`, in 64-bit words. Clears it and sets node 0 if asked. False if it cannot be written.
bool write_node_mask(Process& p, uint64_t address, uint64_t max_node, bool node_zero) {
  const uint64_t words = (max_node + 63) / 64;
  for (uint64_t i = 0; i < words; ++i)
    if (!p.write_user_u64(address + i * 8, i == 0 && node_zero ? 1 : 0)) return false;
  return true;
}

// A node mask a program hands in names only node 0 (or nothing): this machine has one.
uint64_t check_node_mask(Process& p, uint64_t address, uint64_t max_node) {
  if (address == 0 || max_node == 0) return 0;
  if (max_node > 64 * 1024) return error(kEinval);
  const uint64_t words = (max_node + 63) / 64;
  for (uint64_t i = 0; i < words; ++i) {
    uint64_t word = 0;
    if (!p.read_user_u64(address + i * 8, &word)) return error(kEfault);
    const uint64_t bits = i + 1 == words && (max_node % 64) != 0 ? max_node % 64 : 64;
    if (bits < 64) word &= (uint64_t{1} << bits) - 1;
    if ((i == 0 ? word & ~uint64_t{1} : word) != 0) return error(kEinval);
  }
  return 0;
}

// What a call about another process answers: it may not be touched if it is there (EPERM), and else there is no such process.
uint64_t not_this_process(Process& p, uint64_t pid) { return error(p.runtime() && p.runtime()->process_exists(pid) ? kEperm : kEsrch); }

bool policy_known(uint64_t mode) { return (mode & 0xFF) <= 6 && (mode & ~uint64_t{0xE0FF}) == 0; }      // MPOL_DEFAULT .. MPOL_WEIGHTED_INTERLEAVE, and the mode flags

}  // namespace

// --- memory policy: one node, which every policy comes to ------------------------------------------------------------------------------------------------

// get_mempolicy(int* mode, unsigned long* nodemask, maxnode, addr, flags)
uint64_t sys_get_mempolicy(Process& p, uint64_t mode, uint64_t nodemask, uint64_t max_node, uint64_t address, uint64_t flags) {
  constexpr uint64_t kNode = 1, kAddress = 2, kMemsAllowed = 4;
  if ((flags & ~(kNode | kAddress | kMemsAllowed)) != 0) return error(kEinval);
  if ((flags & kMemsAllowed) && (flags & (kNode | kAddress))) return error(kEinval);
  if (nodemask != 0 && max_node > 64 * 1024) return error(kEinval);
  if (flags & kAddress) {      // (the policy, or with MPOL_F_NODE the node, of the memory at an address: it has to be the program's)
    uint8_t protection = 0;
    if (!p.address_space() || !p.address_space()->protection_at(address, &protection)) return error(kEfault);
  } else if (address != 0) {
    return error(kEinval);
  }
  if (mode != 0) {      // MPOL_DEFAULT, or node 0
    uint8_t raw[4] = {};
    if (!p.write_user(mode, ConstByteView(raw, sizeof(raw)))) return error(kEfault);
  }
  if (nodemask != 0 && !write_node_mask(p, nodemask, max_node, (flags & kMemsAllowed) != 0)) return error(kEfault);
  return 0;
}

uint64_t sys_set_mempolicy(Process& p, uint64_t mode, uint64_t nodemask, uint64_t max_node) {
  if (!policy_known(mode)) return error(kEinval);
  return check_node_mask(p, nodemask, max_node);
}

// mbind(addr, len, mode, nodemask, maxnode, flags)
uint64_t sys_mbind(Process& p, uint64_t address, uint64_t length, uint64_t mode, uint64_t nodemask, uint64_t max_node, uint64_t flags) {
  if ((address & (kPage - 1)) != 0 || !policy_known(mode) || (flags & ~uint64_t{7}) != 0) return error(kEinval);
  if (length > ~uint64_t{0} - address) return error(kEinval);
  return check_node_mask(p, nodemask, max_node);
}

// set_mempolicy_home_node(start, len, home_node, flags)
uint64_t sys_set_mempolicy_home_node(Process&, uint64_t address, uint64_t length, uint64_t node, uint64_t flags) {
  if ((address & (kPage - 1)) != 0 || flags != 0 || node != 0 || length > ~uint64_t{0} - address) return error(kEinval);
  return 0;
}

// migrate_pages(pid, maxnode, old_nodes, new_nodes): nothing can move; the count of pages that did not is 0
uint64_t sys_migrate_pages(Process& p, uint64_t pid, uint64_t max_node, uint64_t old_nodes, uint64_t new_nodes) {
  if (pid != 0 && pid != p.id()) return not_this_process(p, pid);
  if (const uint64_t r = check_node_mask(p, old_nodes, max_node); r != 0) return r;
  return check_node_mask(p, new_nodes, max_node);
}

// move_pages(pid, count, void** pages, int* nodes, int* status, flags): each page's status is the node it is on (0), -EFAULT where there is no page, or
// -ENODEV where it was asked to go to a node that is not there.
uint64_t sys_move_pages(Process& p, uint64_t pid, uint64_t count, uint64_t pages, uint64_t nodes, uint64_t status, uint64_t flags) {
  if ((flags & ~uint64_t{6}) != 0) return error(kEinval);      // MPOL_MF_MOVE, MPOL_MF_MOVE_ALL
  if (pid != 0 && pid != p.id()) return not_this_process(p, pid);
  if (count > (uint64_t{1} << 24)) return error(kEinval);
  AddressSpace* space = p.address_space();
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t page = 0;
    if (!p.read_user_u64(pages + i * 8, &page)) return error(kEfault);
    int32_t result = 0;
    uint8_t protection = 0;
    if (!space || !space->protection_at(page, &protection)) result = -static_cast<int32_t>(kEfault);
    if (nodes != 0) {
      uint8_t raw[4];
      if (!p.read_user(nodes + i * 4, ByteView(raw, sizeof(raw)))) return error(kEfault);
      ByteReader reader{ConstByteView(raw, sizeof(raw))};
      if (reader.le32() != 0) result = -static_cast<int32_t>(kEnodev);
    }
    if (status != 0) {
      uint8_t raw[4];
      ByteWriter writer{ByteView(raw, sizeof(raw))};
      writer.le32(static_cast<uint32_t>(result));
      if (!p.write_user(status + i * 4, ConstByteView(raw, sizeof(raw)))) return error(kEfault);
    }
  }
  return 0;
}

// --- protection keys: the processors this runs on, as far as the programs are told, have none ------------------------------------------------------------

uint64_t sys_pkey_alloc(Process&, uint64_t flags, uint64_t access) {
  if (flags != 0 || (access & ~uint64_t{3}) != 0) return error(kEinval);
  return error(kEnospc);      // (what Linux answers on a processor without them)
}

uint64_t sys_pkey_free(Process&, uint64_t) { return error(kEinval); }

// --- kcmp(pid1, pid2, type, idx1, idx2): do two processes share a thing? Only the caller itself can be asked about -----------------------------------------

uint64_t sys_kcmp(Process& p, uint64_t pid1, uint64_t pid2, uint64_t type, uint64_t index1, uint64_t index2) {
  constexpr uint64_t kFile = 0, kSysvSem = 6, kEpollTfd = 7;
  if (type > kEpollTfd) return error(kEinval);
  if (pid1 != p.id()) return not_this_process(p, pid1);
  if (pid2 != p.id()) return not_this_process(p, pid2);
  if (type == kEpollTfd) return error(kEinval);      // (a descriptor inside an epoll instance: not kept in a form that can be compared)
  if (type != kFile) {
    (void)kSysvSem;
    return 0;      // its own memory, descriptor table, file system state, signal handlers, I/O context and semaphore undo list: the same as its own
  }
  ProcessFiles& files = p.files();
  const DescriptorKind kind1 = files.kind(index1), kind2 = files.kind(index2);
  if (kind1 == DescriptorKind::Closed || kind2 == DescriptorKind::Closed) return error(kEbadf);
  // Two descriptors are one open file when dup() made one from the other: then they share the object (a file) or the token (anything else).
  const OpenObject* object1 = files.object(index1);
  const OpenObject* object2 = files.object(index2);
  const uint64_t token1 = files.token(index1), token2 = files.token(index2);
  if (kind1 == kind2 && object1 == object2 && (object1 != nullptr || token1 == token2)) return 0;
  // Not the same: an order that stays the same between calls (by kind, then by what they refer to), 1 for "less" and 2 for "greater".
  if (kind1 != kind2) return kind1 < kind2 ? 1 : 2;
  if (object1 != object2) return object1 < object2 ? 1 : 2;
  return token1 < token2 ? 1 : 2;
}

// --- process_vm_readv / process_vm_writev(pid, local_iov, liovcnt, remote_iov, riovcnt, flags): bytes between two processes' memory. Between the caller and
// itself they are copied; another process's memory is not open to anyone here (EPERM, which is what a debugger without the right gets).

uint64_t sys_process_vm(Process& p, uint64_t pid, uint64_t local, uint64_t local_count, uint64_t remote, uint64_t remote_count, uint64_t flags, bool write) {
  constexpr uint64_t kMostVectors = 1024;
  if (flags != 0 || local_count > kMostVectors || remote_count > kMostVectors) return error(kEinval);
  if (pid != p.id()) return not_this_process(p, pid);
  // Both lists are walked side by side; bytes go from the remote one to the local one (read) or the other way (write).
  struct Cursor { uint64_t list, count, index = 0, base = 0, left = 0; };
  Cursor here{local, local_count}, there{remote, remote_count};
  auto next = [&](Cursor& c) -> int {      // 1: a range is at hand; 0: the list is over; -1: it cannot be read
    while (c.left == 0) {
      if (c.index == c.count) return 0;
      if (!p.read_user_u64(c.list + c.index * 16, &c.base) || !p.read_user_u64(c.list + c.index * 16 + 8, &c.left)) return -1;
      ++c.index;
    }
    return 1;
  };
  uint64_t done = 0;
  uint8_t chunk[512];
  for (;;) {
    const int a = next(here), b = next(there);
    if (a < 0 || b < 0) return done != 0 ? done : error(kEfault);
    if (a == 0 || b == 0) break;
    uint64_t n = here.left < there.left ? here.left : there.left;
    if (n > sizeof(chunk)) n = sizeof(chunk);
    if (n > 0x7FFFF000u - done) n = 0x7FFFF000u - done;
    if (n == 0) break;
    const uint64_t from = write ? here.base : there.base, to = write ? there.base : here.base;
    if (!p.read_user(from, ByteView(chunk, n)) || !p.write_user(to, ConstByteView(chunk, n))) return done != 0 ? done : error(kEfault);
    here.base += n; here.left -= n;
    there.base += n; there.left -= n;
    done += n;
  }
  return done;
}

// --- cachestat(fd, struct cachestat_range*, struct cachestat*, flags): how many of a file's pages are in the page cache. There is no page cache (files are
// read through the block cache below the file systems), so none are: five zeros.

uint64_t sys_cachestat(Process& p, uint64_t fd, uint64_t range, uint64_t out, uint64_t flags) {
  if (flags != 0) return error(kEinval);
  const DescriptorKind kind = p.files().kind(fd);
  if (kind == DescriptorKind::Closed) return error(kEbadf);
  if (kind != DescriptorKind::File) return error(kEbadf);
  uint64_t offset = 0, length = 0;
  if (!p.read_user_u64(range, &offset) || !p.read_user_u64(range + 8, &length)) return error(kEfault);
  uint8_t raw[40] = {};
  return p.write_user(out, ConstByteView(raw, sizeof(raw))) ? 0 : error(kEfault);
}

}  // namespace process::linuxabi
