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
#include "Process/LinuxThreads.hpp"

namespace process {
namespace linuxabi {

namespace {

constexpr int64_t kEagain = 11, kEfault = 14, kEinval = 22, kEnosys = 38, kEintr = 4, kEtimedout = 110, kEnomem = 12;
uint64_t error(int64_t e) { return uint64_t(-e); }

constexpr uint64_t kCloneVm = 0x100, kCloneFs = 0x200, kCloneFiles = 0x400, kCloneSighand = 0x800, kCloneThread = 0x10000, kCloneSysvsem = 0x40000000,
                   kCloneSettls = 0x80000, kCloneParentSettid = 0x100000, kCloneChildCleartid = 0x200000, kCloneDetached = 0x400000, kCloneChildSettid = 0x1000000;
constexpr uint64_t kCloneRequired = kCloneVm | kCloneFs | kCloneFiles | kCloneSighand | kCloneThread;
constexpr uint64_t kCloneAllowed = kCloneRequired | kCloneSysvsem | kCloneSettls | kCloneParentSettid | kCloneChildCleartid | kCloneDetached | kCloneChildSettid;

uint64_t current_tid(Process& p) { return p.runtime() ? p.runtime()->current_thread_id() : p.id(); }

Process::LinuxThreadState::Thread* find_thread(Process& p, uint64_t tid) {
  for (Process::LinuxThreadState::Thread& t : p.linux_threads().threads)
    if (t.used && t.tid == tid) return &t;
  return nullptr;
}

}  // namespace

uint64_t futex_wake(Process& p, uint64_t address, uint64_t count, uint32_t bitset) {
  uint64_t woken = 0;
  for (Process::LinuxThreadState::Waiter& w : p.linux_threads().waiters) {
    if (woken >= count) break;
    if (w.used && !w.woken && w.address == address && (w.bitset & bitset) != 0) {
      w.woken = true;
      ++woken;
    }
  }
  if (woken && p.runtime()) p.runtime()->notify();
  return woken;
}

uint64_t sys_clone(Process& p, UserArch arch, const uint64_t* a) {
  IProcessRuntime* runtime = p.runtime();
  if (!runtime || !runtime->supports_linux_threads()) return error(kEnosys);
  const uint64_t flags = a[0], stack = a[1], parent_tid = a[2];
  // x86-64: clone(flags, stack, parent_tid, child_tid, tls); the generic table (AArch64, RISC-V): clone(flags, stack, parent_tid, tls, child_tid)
  const uint64_t child_tid = arch == UserArch::X86_64 ? a[3] : a[4];
  const uint64_t tls = arch == UserArch::X86_64 ? a[4] : a[3];
  if ((flags & 0xFF) != 0) return error(kEinval);      // an exit signal means fork-like semantics
  if ((flags & kCloneRequired) != kCloneRequired || (flags & ~(kCloneAllowed | 0xFF)) != 0) return error(kEnosys);
  Process::LinuxThreadState& state = p.linux_threads();
  if (state.clone.pending) return error(kEagain);
  Process::LinuxThreadState::Thread* slot = nullptr;
  for (Process::LinuxThreadState::Thread& t : state.threads)
    if (!t.used) { slot = &t; break; }
  if (!slot) return error(kEagain);
  const uint64_t tid = p.next_thread_id() & 0x7FFFFFFFu;
  if (flags & kCloneParentSettid) {
    if (!p.write_user_u32(parent_tid, uint32_t(tid))) return error(kEfault);
  }
  if (flags & kCloneChildSettid) {
    if (!p.write_user_u32(child_tid, uint32_t(tid))) return error(kEfault);
  }
  slot->used = true;
  slot->tid = tid;
  slot->clear_child_tid = (flags & kCloneChildCleartid) ? child_tid : 0;
  state.clone.pending = true;
  state.clone.tid = tid;
  state.clone.stack = stack;
  state.clone.set_tls = (flags & kCloneSettls) != 0;
  state.clone.tls = tls;
  return tid;
}

uint64_t sys_set_tid_address(Process& p, uint64_t address) {
  const uint64_t tid = current_tid(p);
  if (Process::LinuxThreadState::Thread* t = find_thread(p, tid)) t->clear_child_tid = address;
  return tid;
}

uint64_t sys_gettid(Process& p) { return current_tid(p); }

uint64_t sys_thread_exit(Process& p, uint32_t code) {
  const uint64_t tid = current_tid(p);
  if (Process::LinuxThreadState::Thread* t = find_thread(p, tid)) {
    const uint64_t clear = t->clear_child_tid;
    t->used = false;
    t->clear_child_tid = 0;
    if (clear && p.write_user_u32(clear, 0)) futex_wake(p, clear, 1, ~0u);      // pthread_join waits on this word
  }
  uint32_t live = 0;
  for (const Process::LinuxThreadState::Thread& t : p.linux_threads().threads) live += t.used ? 1 : 0;
  if (live == 0 || !p.runtime()) p.terminate(code);
  else p.request_thread_exit(code);
  return 0;
}

uint64_t sys_futex(Process& p, uint64_t address, uint64_t op, uint64_t value, uint64_t timeout_or_count, uint64_t address2, uint64_t value3) {
  const uint32_t command = static_cast<uint32_t>(op & ~uint64_t{128 | 256});
  const bool realtime = (op & 256) != 0;
  IProcessRuntime* runtime = p.runtime();
  switch (command) {
    case 1:      // FUTEX_WAKE
      return futex_wake(p, address, static_cast<uint32_t>(value) > 0x7FFFFFFFu ? 0 : static_cast<uint32_t>(value), ~0u);
    case 10:     // FUTEX_WAKE_BITSET
      if (static_cast<uint32_t>(value3) == 0) return error(kEinval);
      return futex_wake(p, address, static_cast<uint32_t>(value) > 0x7FFFFFFFu ? 0 : static_cast<uint32_t>(value), static_cast<uint32_t>(value3));
    case 3: case 4: {      // FUTEX_REQUEUE, FUTEX_CMP_REQUEUE: wake `value`, move up to `timeout_or_count` more to address2
      uint8_t raw[4];
      if (command == 4) {
        if (!p.read_user(address, ByteView(raw, 4))) return error(kEfault);
        const uint32_t current = uint32_t{raw[0]} | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
        if (current != static_cast<uint32_t>(value3)) return error(kEagain);
      }
      uint64_t woken = futex_wake(p, address, static_cast<uint32_t>(value) > 0x7FFFFFFFu ? 0 : static_cast<uint32_t>(value), ~0u);
      uint64_t moved = 0;
      const uint64_t limit = static_cast<uint32_t>(timeout_or_count) > 0x7FFFFFFFu ? 0 : static_cast<uint32_t>(timeout_or_count);
      for (Process::LinuxThreadState::Waiter& w : p.linux_threads().waiters) {
        if (moved >= limit) break;
        if (w.used && !w.woken && w.address == address) { w.address = address2; ++moved; }
      }
      return woken + moved;
    }
    case 0: case 9: {      // FUTEX_WAIT, FUTEX_WAIT_BITSET
      if (!runtime) return error(kEnosys);
      const uint32_t bitset = command == 9 ? static_cast<uint32_t>(value3) : ~0u;
      if (bitset == 0) return error(kEinval);
      uint8_t raw[4];
      if (!p.read_user(address, ByteView(raw, 4))) return error(kEfault);
      const uint32_t current = uint32_t{raw[0]} | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
      if (current != static_cast<uint32_t>(value)) return error(kEagain);
      uint64_t deadline = ~uint64_t{0};
      if (timeout_or_count) {
        uint64_t seconds = 0, nanos = 0;
        if (!p.read_user_u64(timeout_or_count, &seconds) || !p.read_user_u64(timeout_or_count + 8, &nanos)) return error(kEfault);
        if (int64_t(seconds) < 0 || nanos >= 1000000000u) return error(kEinval);
        const uint64_t now = p.host().monotonic_ns();
        const uint64_t max_seconds = (~uint64_t{0} - 999999999u) / 1000000000u;
        const uint64_t ns = seconds > max_seconds ? ~uint64_t{0} : seconds * 1000000000u + nanos;
        if (command == 9) {      // an absolute time on the chosen clock
          const uint64_t clock_now = realtime ? p.host().realtime_ns() : now;
          deadline = ns <= clock_now ? now : (ns - clock_now > ~uint64_t{0} - now ? ~uint64_t{0} : now + (ns - clock_now));
        } else {
          deadline = ns > ~uint64_t{0} - now ? ~uint64_t{0} : now + ns;
        }
      }
      Process::LinuxThreadState::Waiter* w = nullptr;
      for (Process::LinuxThreadState::Waiter& candidate : p.linux_threads().waiters)
        if (!candidate.used) { w = &candidate; break; }
      if (!w) return error(kEnomem);
      w->used = true;
      w->woken = false;
      w->address = address;
      w->bitset = bitset;
      uint64_t result = 0;
      while (!w->woken) {
        if (runtime->interrupted() || p.exited()) { result = error(kEintr); break; }
        uint64_t left = ~uint64_t{0};
        if (deadline != ~uint64_t{0}) {
          const uint64_t now = p.host().monotonic_ns();
          if (now >= deadline) { result = error(kEtimedout); break; }
          left = deadline - now;
        }
        runtime->wait(left);
      }
      w->used = false;
      return result;
    }
    default:
      return error(kEnosys);
  }
}

uint64_t sys_sched_getaffinity(Process& p, uint64_t size, uint64_t mask_address) {
  if (size < 8 || (size & 7) != 0) return error(kEinval);
  const uint32_t cpus = p.runtime() ? p.runtime()->processor_count() : 1;
  const uint64_t mask = cpus >= 64 ? ~uint64_t{0} : (uint64_t{1} << cpus) - 1;
  uint8_t zeros[128] = {};
  const uint64_t n = size < sizeof(zeros) ? size : sizeof(zeros);
  for (uint32_t i = 0; i < 8; ++i) zeros[i] = uint8_t(mask >> (8 * i));
  if (!p.write_user(mask_address, ConstByteView(zeros, n))) return error(kEfault);
  return n;
}

}  // namespace linuxabi
}  // namespace process
