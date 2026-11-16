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
#include "Process/LinuxFiles.hpp"
#include "Process/LinuxSockets.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Objects.hpp"
#include "Utility/ByteStream.hpp"

namespace process::linuxabi {

namespace {

constexpr int64_t kEperm = 1, kEsrch = 3, kEintr = 4, kEbadf = 9, kEchild = 10, kEagain = 11, kEfault = 14, kEinval = 22, kEnosys = 38;
uint64_t error(int64_t e) { return uint64_t(-e); }

uint64_t current_tid(Process& p) { return p.runtime() ? p.runtime()->current_thread_id() : p.id(); }
bool is_self(Process& p, uint64_t pid) { return int32_t(pid) == 0 || uint32_t(pid) == uint32_t(p.id()); }

LinuxProcessState::Child* find_child(Process& p, uint64_t pid) {
  for (LinuxProcessState::Child& c : p.linux_state().children)
    if (c.used && c.pid == pid) return &c;
  return nullptr;
}

bool write_u32s(Process& p, uint64_t address, ArrayView<const uint32_t> values) {
  uint8_t raw[4 * LinuxProcessState::kMaxGroups];
  ByteWriter writer{ByteView(raw, sizeof(raw))};
  for (const uint32_t v : values) writer.le32(v);
  return writer.ok() && p.write_user(address, writer.written());
}

}  // namespace

// --- fork and wait ---------------------------------------------------------------------------------------------------------------------------------------

uint64_t sys_fork(Process& p, const ForkOptions& options) {
  IProcessRuntime* runtime = p.runtime();
  if (!runtime || !runtime->supports_linux_fork()) return error(kEnosys);
  LinuxProcessState& state = p.linux_state();
  if (state.fork.pending) return error(kEagain);
  LinuxProcessState::Child* slot = nullptr;
  for (LinuxProcessState::Child& c : state.children)
    if (!c.used && !slot) slot = &c;
  if (!slot) return error(kEagain);      // (as many children as the table holds have not been waited for)
  const uint64_t pid = runtime->allocate_process_id();
  if (pid == 0) return error(kEagain);
  if (options.parent_tid_at != 0 && !p.write_user_u32(options.parent_tid_at, uint32_t(pid))) return error(kEfault);
  *slot = LinuxProcessState::Child();
  slot->used = true;
  slot->pid = pid;
  state.fork.pending = true;
  state.fork.pid = pid;
  state.fork.stack = options.stack;
  state.fork.set_tls = options.set_tls;
  state.fork.tls = options.tls;
  state.fork.set_child_tid = options.set_child_tid;
  state.fork.clear_child_tid = options.clear_child_tid;
  state.fork.caller_tid = current_tid(p);
  return pid;
}

void fork_failed(Process& parent, uint64_t pid) {
  if (LinuxProcessState::Child* c = find_child(parent, pid)) *c = LinuxProcessState::Child();
}

void note_child_exit(Process& parent, uint64_t pid, uint32_t code, uint32_t signal) {
  if (LinuxProcessState::Child* c = find_child(parent, pid)) {
    c->exited = true;
    c->code = code;
    if (c->signal == 0) c->signal = signal;
  }
}

void inherit_fork_state(Process& child, Process& parent, const LinuxProcessState::ForkRequest& request) {
  LinuxProcessState& to = child.linux_state();
  const LinuxProcessState& from = parent.linux_state();
  to = from;
  // Not inherited: what is pending, the timers, the children, and the fork that is being made.
  to.pending = 0;
  to.exit_signal = 0;
  to.return_requested = false;
  to.handlers_run = false;      // (whoever runs the child says so again)
  for (LinuxProcessState::Info& info : to.info) info = LinuxProcessState::Info();
  for (LinuxProcessState::IntervalTimer& timer : to.timers) timer = LinuxProcessState::IntervalTimer();
  for (LinuxProcessState::PosixTimer& timer : to.posix_timers) timer = LinuxProcessState::PosixTimer();
  for (LinuxProcessState::Child& c : to.children) c = LinuxProcessState::Child();
  to.fork = LinuxProcessState::ForkRequest();
  // The child has one thread, the copy of the one that called: its mask and alternate stack are the child's first thread's.
  LinuxProcessState::Thread caller;
  for (const LinuxProcessState::Thread& t : from.threads)
    if (t.used && t.tid == request.caller_tid) caller = t;
  for (LinuxProcessState::Thread& t : to.threads) t = LinuxProcessState::Thread();
  caller.used = true;
  caller.tid = child.id();
  caller.robust_head = caller.robust_size = 0;
  caller.pending = 0;
  if (caller.has_saved_mask) caller.blocked = caller.saved_mask;
  caller.has_saved_mask = false;
  to.threads[0] = caller;
  // It stays in its parent's process group and session.
  to.process_group = from.process_group ? from.process_group : parent.id();
  to.session = from.session ? from.session : parent.id();
  child.set_parent_id(parent.id());
  child.set_memory_cursors(parent.heap_break(), parent.mmap_cursor());
  child.files().inherit_for_fork(parent.files(), [](DescriptorKind kind, uint64_t token) { descriptor_token_add(kind, token); });
  Process::LinuxThreadState& threads = child.linux_threads();
  threads.threads[0].clear_child_tid = request.clear_child_tid;      // ([0] is the first thread: the Process made it so)
  if (request.set_child_tid != 0) child.write_user_u32(request.set_child_tid, uint32_t(child.id()));      // (a bad address is the child's own problem, as on Linux)
}

namespace {

// Looks at the children that match `pid` (0: any) for one that has ended. 1: *out is it (and, unless `keep`, it is forgotten); 0: there are children but none
// has ended and the caller does not wait; -errno.
int64_t wait_for_child(Process& p, uint64_t pid, bool no_hang, bool keep, LinuxProcessState::Child* out) {
  LinuxProcessState& state = p.linux_state();
  IProcessRuntime* runtime = p.runtime();
  for (;;) {
    bool any = false;
    for (LinuxProcessState::Child& c : state.children) {
      if (!c.used || (pid != 0 && c.pid != pid)) continue;
      any = true;
      if (!c.exited) {
        bool exited = false;
        uint32_t code = 0;
        // (a child the runtime no longer knows has ended; had it said how, note_child_exit would have kept that)
        if (!runtime || !runtime->query_child(c.pid, &exited, &code)) { exited = true; code = 0; }
        if (exited) {
          c.exited = true;
          c.code = code;
          if (c.signal == 0 && runtime) c.signal = runtime->child_exit_signal(c.pid);      // (it ended itself with a signal: abort(), a signal nobody handled)
        }
      }
      if (!c.exited) continue;
      *out = c;
      if (!keep) c = LinuxProcessState::Child();
      return 1;
    }
    if (!any) return -kEchild;
    if (no_hang) return 0;
    if (p.exited() || (runtime && runtime->interrupted()) || p.signal_interrupts()) return -kEintr;
    p.block_sleep(2000000);      // (looked at again every 2 ms: nothing tells a parent that a child has ended)
  }
}

constexpr uint64_t kWNoHang = 1, kWUntraced = 2, kWExited = 4, kWContinued = 8, kWNoWait = 0x01000000, kWNoThread = 0x20000000, kWAll = 0x40000000, kWClone = 0x80000000;
constexpr uint64_t kUsageBytes = 144;      // struct rusage

bool write_zero_usage(Process& p, uint64_t address) {
  const uint8_t zeros[kUsageBytes] = {};
  return address == 0 || p.write_user(address, ConstByteView(zeros, sizeof(zeros)));
}

// Which children wait(pid) means: a pid, or any (-1; also a process group, 0 or below -1: a child is in its parent's group unless it left, which it is not asked).
uint64_t wanted_child(uint64_t pid) { return int32_t(pid) > 0 ? uint32_t(pid) : 0; }

}  // namespace

uint64_t sys_wait4(Process& p, uint64_t pid, uint64_t status_at, uint64_t options, uint64_t usage_at) {
  if (options & ~(kWNoHang | kWUntraced | kWContinued | kWNoThread | kWAll | kWClone)) return error(kEinval);
  LinuxProcessState::Child child;
  const int64_t r = wait_for_child(p, wanted_child(pid), (options & kWNoHang) != 0, false, &child);
  if (r <= 0) return r < 0 ? error(-r) : 0;
  // The status word: the exit code in the second byte, or the signal that ended it in the low seven bits.
  const uint32_t status = child.signal ? (child.signal & 0x7F) : (child.code & 0xFF) << 8;
  if (status_at != 0 && !p.write_user_u32(status_at, status)) return error(kEfault);
  if (!write_zero_usage(p, usage_at)) return error(kEfault);
  return child.pid;
}

// waitid(idtype, id, siginfo_t*, options, rusage*): siginfo_t for SIGCHLD is si_signo, si_errno, si_code (4 each), padding (4), si_pid (4), si_uid (4), si_status (4).
uint64_t sys_waitid(Process& p, uint64_t id_type, uint64_t id, uint64_t info_at, uint64_t options, uint64_t usage_at) {
  constexpr uint64_t kPAll = 0, kPPid = 1, kPPgid = 2, kPPidfd = 3;
  bool descriptor_does_not_wait = false;
  if (id_type == kPPidfd) {      // a child named by a descriptor for it (pidfd_open); one opened with PIDFD_NONBLOCK answers EAGAIN where the call would wait
    uint64_t pid = 0;
    uint32_t mode = 0;
    if (linked_of(p, id, UnixType::Pidfd, &pid, &mode, &descriptor_does_not_wait) != 0) return error(kEbadf);
    id_type = kPPid;
    id = pid;
  }
  constexpr uint32_t kSigChld = 17, kCldExited = 1, kCldKilled = 2;
  if ((options & ~(kWNoHang | kWUntraced | kWExited | kWContinued | kWNoWait | kWNoThread | kWAll | kWClone)) || !(options & (kWExited | kWUntraced | kWContinued))) return error(kEinval);
  if (id_type != kPAll && id_type != kPPid && id_type != kPPgid) return error(kEinval);
  if (id_type == kPPid && int32_t(id) <= 0) return error(kEinval);
  uint8_t raw[128] = {};
  LinuxProcessState::Child child;
  int64_t r = 0;
  if (options & kWExited) {
    r = wait_for_child(p, id_type == kPPid ? uint32_t(id) : 0, (options & kWNoHang) != 0 || descriptor_does_not_wait, (options & kWNoWait) != 0, &child);
    if (r == 0 && descriptor_does_not_wait && !(options & kWNoHang)) return error(kEagain);
  } else {      // only stops and continues are asked for, and children neither stop nor continue here
    r = wait_for_child(p, id_type == kPPid ? uint32_t(id) : 0, true, true, &child);
    if (r > 0) r = 0;
    if (r == 0 && !(options & kWNoHang)) return error(kEinval);
  }
  if (r < 0) return error(-r);
  if (r > 0) {
    ByteWriter writer{ByteView(raw, sizeof(raw))};
    writer.le32(kSigChld);
    writer.le32(0);
    writer.le32(child.signal ? kCldKilled : kCldExited);
    writer.le32(0);
    writer.le32(uint32_t(child.pid));
    writer.le32(0);
    writer.le32(child.signal ? child.signal : (child.code & 0xFF));
  }
  if (info_at != 0 && !p.write_user(info_at, ConstByteView(raw, sizeof(raw)))) return error(kEfault);
  if (!write_zero_usage(p, usage_at)) return error(kEfault);
  return 0;
}

// --- who the process is ------------------------------------------------------------------------------------------------------------------------------------

uint64_t sys_id(Process& p, IdCall call, uint64_t a, uint64_t b, uint64_t c) {
  LinuxProcessState& s = p.linux_state();
  constexpr uint32_t kKeep = 0xFFFFFFFFu;      // -1: leave this one as it is
  const uint32_t x = uint32_t(a), y = uint32_t(b), z = uint32_t(c);
  auto set = [](uint32_t* id, uint32_t value) { if (value != kKeep) *id = value; };
  switch (call) {
    case IdCall::Getuid: return s.real_user;
    case IdCall::Geteuid: return s.effective_user;
    case IdCall::Getgid: return s.real_group;
    case IdCall::Getegid: return s.effective_group;
    // Every change is allowed (the program started as root, and nothing checks an id afterwards): the ids only read back as they were set.
    case IdCall::Setuid:
      if (x == kKeep) return error(kEinval);
      s.real_user = s.effective_user = s.saved_user = s.file_user = x;
      return 0;
    case IdCall::Setgid:
      if (x == kKeep) return error(kEinval);
      s.real_group = s.effective_group = s.saved_group = s.file_group = x;
      return 0;
    case IdCall::Setreuid:
      set(&s.real_user, x);
      set(&s.effective_user, y);
      if (x != kKeep || y != kKeep) s.saved_user = s.effective_user;
      s.file_user = s.effective_user;
      return 0;
    case IdCall::Setregid:
      set(&s.real_group, x);
      set(&s.effective_group, y);
      if (x != kKeep || y != kKeep) s.saved_group = s.effective_group;
      s.file_group = s.effective_group;
      return 0;
    case IdCall::Setresuid:
      set(&s.real_user, x);
      set(&s.effective_user, y);
      set(&s.saved_user, z);
      s.file_user = s.effective_user;
      return 0;
    case IdCall::Setresgid:
      set(&s.real_group, x);
      set(&s.effective_group, y);
      set(&s.saved_group, z);
      s.file_group = s.effective_group;
      return 0;
    case IdCall::Getresuid:
      return p.write_user_u32(a, s.real_user) && p.write_user_u32(b, s.effective_user) && p.write_user_u32(c, s.saved_user) ? 0 : error(kEfault);
    case IdCall::Getresgid:
      return p.write_user_u32(a, s.real_group) && p.write_user_u32(b, s.effective_group) && p.write_user_u32(c, s.saved_group) ? 0 : error(kEfault);
    case IdCall::Setfsuid: {      // (returns the id it had, changed or not)
      const uint32_t old = s.file_user;
      set(&s.file_user, x);
      return old;
    }
    case IdCall::Setfsgid: {
      const uint32_t old = s.file_group;
      set(&s.file_group, x);
      return old;
    }
  }
  return error(kEinval);
}

uint64_t sys_getgroups(Process& p, uint64_t size, uint64_t list) {
  const LinuxProcessState& s = p.linux_state();
  if (int32_t(size) < 0) return error(kEinval);
  if (uint32_t(size) == 0) return s.group_count;
  if (uint32_t(size) < s.group_count) return error(kEinval);
  if (s.group_count != 0 && !write_u32s(p, list, ArrayView<const uint32_t>(s.groups, s.group_count))) return error(kEfault);
  return s.group_count;
}

uint64_t sys_setgroups(Process& p, uint64_t size, uint64_t list) {
  LinuxProcessState& s = p.linux_state();
  if (size > LinuxProcessState::kMaxGroups) return error(kEinval);
  uint8_t raw[4 * LinuxProcessState::kMaxGroups];
  if (size != 0 && !p.read_user(list, ByteView(raw, size * 4))) return error(kEfault);
  ByteReader reader{ConstByteView(raw, size * 4)};
  for (uint32_t i = 0; i < size; ++i) s.groups[i] = reader.le32();
  s.group_count = uint32_t(size);
  return 0;
}

uint64_t sys_getpgid(Process& p, uint64_t pid) {
  const LinuxProcessState& s = p.linux_state();
  const uint64_t own = s.process_group ? s.process_group : p.id();
  if (is_self(p, pid)) return own;
  if (find_child(p, uint32_t(pid))) return own;      // (a child stays in its parent's group: see sys_setpgid)
  return p.runtime() && p.runtime()->process_exists(uint32_t(pid)) ? uint32_t(pid) : error(kEsrch);
}

uint64_t sys_setpgid(Process& p, uint64_t pid, uint64_t group) {
  if (int32_t(group) < 0) return error(kEinval);
  if (is_self(p, pid)) {
    p.linux_state().process_group = uint32_t(group) ? uint32_t(group) : p.id();
    return 0;
  }
  // A shell puts each child in a group of its own for job control. There is none here (no terminal hands out a foreground group, nothing is stopped), and a
  // child's group is not kept on this side: the call is taken as done.
  return find_child(p, uint32_t(pid)) ? 0 : error(kEsrch);
}

uint64_t sys_getsid(Process& p, uint64_t pid) {
  const LinuxProcessState& s = p.linux_state();
  const uint64_t own = s.session ? s.session : p.id();
  if (is_self(p, pid) || find_child(p, uint32_t(pid))) return own;
  return p.runtime() && p.runtime()->process_exists(uint32_t(pid)) ? uint32_t(pid) : error(kEsrch);
}

uint64_t sys_setsid(Process& p) {
  // (Linux refuses a process that leads its group. Every program started here would, having no shell above it that made the group; so it is always allowed.)
  LinuxProcessState& s = p.linux_state();
  s.session = s.process_group = p.id();
  return p.id();
}

namespace {
constexpr uint32_t kCapabilityV1 = 0x19980330u, kCapabilityV2 = 0x20071026u, kCapabilityV3 = 0x20080522u;

// The header of capget/capset: version (4), pid (4). How many 12-byte sets the version has; 0 for a version this does not know (the header is then told
// which one it should use, as Linux does).
uint32_t capability_sets(Process& p, uint64_t header, uint64_t* failure) {
  uint8_t raw[8];
  *failure = 0;
  if (!p.read_user(header, ByteView(raw, sizeof(raw)))) { *failure = error(kEfault); return 0; }
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  const uint32_t version = reader.le32();
  const uint32_t pid = reader.le32();
  if (version != kCapabilityV1 && version != kCapabilityV2 && version != kCapabilityV3) {
    *failure = p.write_user_u32(header, kCapabilityV3) ? error(kEinval) : error(kEfault);
    return 0;
  }
  if (int32_t(pid) < 0) { *failure = error(kEinval); return 0; }
  if (!is_self(p, pid) && !find_child(p, pid)) { *failure = error(kEsrch); return 0; }
  return version == kCapabilityV1 ? 1 : 2;
}
}  // namespace

uint64_t sys_capget(Process& p, uint64_t header, uint64_t data) {
  uint64_t failure = 0;
  const uint32_t sets = capability_sets(p, header, &failure);
  if (sets == 0) return data == 0 && failure == error(kEinval) ? 0 : failure;      // (capget(header, NULL) is how a program asks for the version)
  if (data == 0) return 0;
  // Each set: effective, permitted, inheritable (4 each). Root has every capability; none is handed on.
  uint8_t raw[24];
  ByteWriter writer{ByteView(raw, sizeof(raw))};
  for (uint32_t i = 0; i < sets; ++i) {
    writer.le32(i == 0 ? 0xFFFFFFFFu : 0x000001FFu);      // (41 capabilities: 32 in the first set, 9 in the second)
    writer.le32(i == 0 ? 0xFFFFFFFFu : 0x000001FFu);
    writer.le32(0);
  }
  return p.write_user(data, writer.written()) ? 0 : error(kEfault);
}

uint64_t sys_capset(Process& p, uint64_t header, uint64_t data) {
  uint64_t failure = 0;
  const uint32_t sets = capability_sets(p, header, &failure);
  if (sets == 0) return failure;
  uint8_t raw[24];
  if (!p.read_user(data, ByteView(raw, sets * 12))) return error(kEfault);
  return 0;      // (a program may give capabilities up; nothing here checks one afterwards)
}

// --- limits, usage, scheduling -------------------------------------------------------------------------------------------------------------------------------

namespace {
constexpr uint64_t kInfinity = ~uint64_t{0};
constexpr uint32_t kLimitStack = 3, kLimitCore = 4, kLimitFiles = 7;

LinuxProcessState::Limit default_limit(uint32_t resource) {
  LinuxProcessState::Limit limit;
  limit.current = limit.maximum = kInfinity;
  if (resource == kLimitStack) limit.current = 8ull << 20;      // the 8 MiB the first thread's stack was mapped with
  if (resource == kLimitCore) limit.current = 0;                // no core files
  if (resource == kLimitFiles) limit.current = limit.maximum = ProcessFiles::kMaxDescriptors;
  return limit;
}
}  // namespace

uint64_t sys_prlimit(Process& p, uint64_t pid, uint64_t resource, uint64_t new_limit, uint64_t old_limit) {
  if (!is_self(p, pid)) return find_child(p, uint32_t(pid)) ? error(kEperm) : error(kEsrch);
  if (uint32_t(resource) >= LinuxProcessState::kLimits) return error(kEinval);
  LinuxProcessState::Limit& kept = p.linux_state().limits[uint32_t(resource)];
  const LinuxProcessState::Limit now = kept.set ? kept : default_limit(uint32_t(resource));
  LinuxProcessState::Limit next = now;
  if (new_limit != 0) {
    if (!p.read_user_u64(new_limit, &next.current) || !p.read_user_u64(new_limit + 8, &next.maximum)) return error(kEfault);
    if (next.current > next.maximum) return error(kEinval);
    if (uint32_t(resource) == kLimitFiles && next.maximum > ProcessFiles::kMaxDescriptors) return error(kEperm);      // the table is that long
    next.set = true;
  }
  if (old_limit != 0 && (!p.write_user_u64(old_limit, now.current) || !p.write_user_u64(old_limit + 8, now.maximum))) return error(kEfault);
  if (new_limit != 0) kept = next;
  return 0;
}

uint64_t cpu_time_ns(Process& p, bool thread) {
  IProcessRuntime* runtime = p.runtime();
  uint64_t hundred_ns = 0;
  if (!runtime || !runtime->cpu_time(thread ? runtime->current_thread_id() : 0, &hundred_ns)) return 0;
  return hundred_ns > kInfinity / 100 ? kInfinity : hundred_ns * 100;
}

// struct rusage: ru_utime, ru_stime (timevals), ru_maxrss (kilobytes), then counters that nothing here counts.
uint64_t sys_getrusage(Process& p, uint64_t who, uint64_t usage) {
  constexpr int32_t kSelf = 0, kChildren = -1, kThread = 1;
  const int32_t which = int32_t(who);
  if (which != kSelf && which != kChildren && which != kThread) return error(kEinval);
  uint8_t raw[144] = {};
  if (which != kChildren) {
    ByteWriter writer{ByteView(raw, sizeof(raw))};
    const uint64_t ns = cpu_time_ns(p, which == kThread);
    writer.le64(ns / 1000000000u);
    writer.le64(ns % 1000000000u / 1000);
    writer.le64(0);
    writer.le64(0);
    writer.le64(p.address_space() ? p.address_space()->resident_bytes() / 1024 : 0);
  }
  return p.write_user(usage, ConstByteView(raw, sizeof(raw))) ? 0 : error(kEfault);
}

// times(struct tms*): the CPU time in clock ticks (100 a second, what sysconf(_SC_CLK_TCK) says), and the ticks since the system started as the result.
uint64_t sys_times(Process& p, uint64_t buffer) {
  constexpr uint64_t kTickNs = 10000000u;
  if (buffer != 0) {
    uint8_t raw[32] = {};
    ByteWriter writer{ByteView(raw, sizeof(raw))};
    writer.le64(cpu_time_ns(p, false) / kTickNs);
    if (!p.write_user(buffer, ConstByteView(raw, sizeof(raw)))) return error(kEfault);
  }
  return (p.host().monotonic_ns() / kTickNs) & 0x7FFFFFFFFFFFFFFFull;
}

// struct sysinfo (112 bytes): uptime (8), loads[3] (24), totalram, freeram, sharedram, bufferram, totalswap, freeswap (8 each), procs (2), padding (6),
// totalhigh, freehigh (8 each), mem_unit (4), padding.
uint64_t sys_sysinfo(Process& p, uint64_t info) {
  uint64_t total = 0, free_bytes = 0;
  if (p.runtime()) p.runtime()->memory_status(&total, &free_bytes);
  if (total == 0) { total = 256ull << 20; free_bytes = 128ull << 20; }      // (nobody says: a machine that programs can live with)
  uint8_t raw[112] = {};
  ByteWriter writer{ByteView(raw, sizeof(raw))};
  writer.le64(p.host().monotonic_ns() / 1000000000u);
  writer.zeros(24);
  writer.le64(total);
  writer.le64(free_bytes);
  writer.zeros(32);
  writer.le16(1);
  writer.zeros(6 + 16);
  writer.le32(1);
  return p.write_user(info, ConstByteView(raw, sizeof(raw))) ? 0 : error(kEfault);
}

uint64_t sys_getpriority(Process& p, uint64_t which, uint64_t who) {
  if (which > 2) return error(kEinval);
  if (which == 0 && !is_self(p, who)) return find_child(p, uint32_t(who)) ? 20 : error(kEsrch);
  return uint64_t(20 - p.linux_state().nice);      // (the call answers 20 - nice, 1..40, so that it never looks like an error)
}

uint64_t sys_setpriority(Process& p, uint64_t which, uint64_t who, uint64_t priority) {
  if (which > 2) return error(kEinval);
  if (which == 0 && !is_self(p, who)) return find_child(p, uint32_t(who)) ? 0 : error(kEsrch);
  const int32_t nice = int32_t(priority);
  p.linux_state().nice = nice < -20 ? -20 : nice > 19 ? 19 : nice;
  return 0;
}

// The scheduling policy calls. The kernel's scheduler has its own priorities; what a program asks for here is kept and read back, and changes nothing.
uint64_t sys_sched(Process& p, SchedCall call, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  constexpr uint32_t kFifo = 1, kRoundRobin = 2, kDeadline = 6, kResetOnFork = 0x40000000u;
  constexpr uint64_t kAttrBytes = 48;      // struct sched_attr, its first version: size, policy (4 each), flags (8), nice, priority (4 each), runtime, deadline, period (8 each)
  LinuxProcessState& s = p.linux_state();
  auto known_thread = [&](uint64_t pid) {
    if (is_self(p, pid)) return true;
    for (const Process::LinuxThreadState::Thread& t : p.linux_threads().threads)
      if (t.used && uint32_t(t.tid) == uint32_t(pid)) return true;
    return false;
  };
  auto valid = [&](uint32_t policy, uint32_t priority) {
    if (policy > kDeadline || policy == 4) return false;
    const bool realtime = policy == kFifo || policy == kRoundRobin;
    return realtime ? priority >= 1 && priority <= 99 : priority == 0;
  };
  switch (call) {
    case SchedCall::PriorityMax:
    case SchedCall::PriorityMin: {
      const uint32_t policy = uint32_t(a);
      if (policy > kDeadline || policy == 4) return error(kEinval);
      const bool realtime = policy == kFifo || policy == kRoundRobin;
      return !realtime ? 0 : call == SchedCall::PriorityMax ? 99 : 1;
    }
    case SchedCall::GetScheduler: return int32_t(a) < 0 ? error(kEinval) : known_thread(a) ? s.policy : error(kEsrch);
    case SchedCall::GetParam:
      if (int32_t(a) < 0 || b == 0) return error(kEinval);
      if (!known_thread(a)) return error(kEsrch);
      return p.write_user_u32(b, s.realtime_priority) ? 0 : error(kEfault);
    case SchedCall::SetParam:
    case SchedCall::SetScheduler: {
      const uint64_t param = call == SchedCall::SetParam ? b : c;
      if (int32_t(a) < 0 || param == 0) return error(kEinval);
      if (!known_thread(a)) return error(kEsrch);
      uint8_t raw[4];
      if (!p.read_user(param, ByteView(raw, sizeof(raw)))) return error(kEfault);
      ByteReader reader{ConstByteView(raw, sizeof(raw))};
      const uint32_t priority = reader.le32();
      const uint32_t policy = call == SchedCall::SetParam ? s.policy : uint32_t(b) & ~kResetOnFork;
      if (!valid(policy, priority)) return error(kEinval);
      s.policy = policy;
      s.realtime_priority = priority;
      return 0;
    }
    case SchedCall::RrInterval:      // a time slice of 100 ms, whatever the policy
      if (int32_t(a) < 0) return error(kEinval);
      if (!known_thread(a)) return error(kEsrch);
      return p.write_user_u64(b, 0) && p.write_user_u64(b + 8, 100000000u) ? 0 : error(kEfault);
    case SchedCall::GetAttr: {      // (pid, attr, size, flags)
      if (int32_t(a) < 0 || b == 0 || c < kAttrBytes || c > 4096 || d != 0) return error(kEinval);
      if (!known_thread(a)) return error(kEsrch);
      uint8_t raw[kAttrBytes] = {};
      ByteWriter writer{ByteView(raw, sizeof(raw))};
      writer.le32(kAttrBytes);
      writer.le32(s.policy);
      writer.le64(0);
      writer.le32(uint32_t(s.nice));
      writer.le32(s.realtime_priority);
      return p.write_user(b, ConstByteView(raw, sizeof(raw))) ? 0 : error(kEfault);
    }
    case SchedCall::SetAttr: {      // (pid, attr, flags)
      if (int32_t(a) < 0 || b == 0 || c != 0) return error(kEinval);
      if (!known_thread(a)) return error(kEsrch);
      uint8_t raw[kAttrBytes];
      if (!p.read_user(b, ByteView(raw, sizeof(raw)))) return error(kEfault);
      ByteReader reader{ConstByteView(raw, sizeof(raw))};
      const uint32_t size = reader.le32(), policy = reader.le32();
      reader.skip(8);
      const int32_t nice = int32_t(reader.le32());
      const uint32_t priority = reader.le32();
      if (size != 0 && size < kAttrBytes) return error(kEinval);
      if (!valid(policy, priority) || policy == kDeadline) return error(kEinval);
      s.policy = policy;
      s.realtime_priority = priority;
      s.nice = nice < -20 ? -20 : nice > 19 ? 19 : nice;
      return 0;
    }
  }
  return error(kEinval);
}

uint64_t sys_sched_setaffinity(Process& p, uint64_t pid, uint64_t size, uint64_t mask_address) {
  if (int32_t(pid) < 0) return error(kEinval);
  uint8_t raw[8] = {};
  const uint64_t n = size < sizeof(raw) ? size : sizeof(raw);
  if (n != 0 && !p.read_user(mask_address, ByteView(raw, n))) return error(kEfault);
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  uint64_t mask = reader.le64();
  IProcessRuntime* runtime = p.runtime();
  const uint32_t cpus = runtime ? runtime->processor_count() : 1;
  mask &= cpus >= 64 ? kInfinity : (uint64_t{1} << cpus) - 1;
  if (mask == 0) return error(kEinval);      // no processor that exists
  if (!runtime) return 0;
  const uint64_t thread = int32_t(pid) == 0 ? runtime->current_thread_id() : uint32_t(pid);
  if (int32_t(pid) != 0 && uint32_t(pid) != uint32_t(p.id())) {
    bool found = false;
    for (const Process::LinuxThreadState::Thread& t : p.linux_threads().threads)
      if (t.used && uint32_t(t.tid) == uint32_t(pid)) found = true;
    if (!found) return error(kEsrch);
  }
  runtime->set_affinity(thread, mask);      // (a runtime that keeps its threads where they are still has the call succeed: the mask is a wish)
  return 0;
}

uint64_t sys_getcpu(Process& p, uint64_t cpu, uint64_t node) {
  const uint32_t current = p.runtime() ? p.runtime()->current_processor() : 0;
  if (cpu != 0 && !p.write_user_u32(cpu, current)) return error(kEfault);
  if (node != 0 && !p.write_user_u32(node, 0)) return error(kEfault);
  return 0;
}

uint64_t sys_personality(Process& p, uint64_t persona) {
  LinuxProcessState& s = p.linux_state();
  const uint64_t old = s.personality;
  if (uint32_t(persona) != 0xFFFFFFFFu) s.personality = uint32_t(persona);
  return old;
}

uint64_t sys_prctl(Process& p, UserArch arch, uint64_t option, uint64_t a, uint64_t b) {
  LinuxProcessState& s = p.linux_state();
  auto store = [&](uint32_t value) { return p.write_user_u32(a, value) ? uint64_t{0} : error(kEfault); };
  switch (option) {
    case 1: if (a > LinuxProcessState::kSignals) return error(kEinval); s.parent_death_signal = uint32_t(a); return 0;      // PR_SET_PDEATHSIG
    case 2: return store(s.parent_death_signal);                                                                             // PR_GET_PDEATHSIG
    case 3: return s.dumpable;                                                                                               // PR_GET_DUMPABLE
    case 4: if (a > 1) return error(kEinval); s.dumpable = uint32_t(a); return 0;                                           // PR_SET_DUMPABLE
    case 7: return s.keep_capabilities;                                                                                      // PR_GET_KEEPCAPS
    case 8: if (a > 1) return error(kEinval); s.keep_capabilities = uint32_t(a); return 0;                                  // PR_SET_KEEPCAPS
    case 15: {      // PR_SET_NAME: up to 15 characters and a NUL
      char name[16] = {};
      for (uint32_t i = 0; i < 15; ++i) {
        uint8_t c = 0;
        if (!p.read_user(a + i, ByteView(&c, 1))) return error(kEfault);
        if (c == 0) break;
        name[i] = char(c);
      }
      for (uint32_t i = 0; i < 16; ++i) s.name[i] = name[i];
      s.name_set = true;
      return 0;
    }
    case 16: {      // PR_GET_NAME: 16 bytes; a program that never set one is called after its file
      uint8_t name[16] = {};
      if (s.name_set) {
        for (uint32_t i = 0; i < 15; ++i) name[i] = uint8_t(s.name[i]);
      } else {
        StringView file = p.files().executable();
        for (uint64_t i = 0; i < file.size(); ++i)
          if (file[i] == '/') { file = file.without_prefix(i + 1); i = ~uint64_t{0}; }
        for (uint64_t i = 0; i < file.size() && i < 15; ++i) name[i] = uint8_t(file[i]);
      }
      return p.write_user(a, ConstByteView(name, sizeof(name))) ? 0 : error(kEfault);
    }
    case 21: return 0;                    // PR_GET_SECCOMP: not in a sandbox
    case 22: return error(kEinval);       // PR_SET_SECCOMP: there is none to enter
    case 23: return a <= 40 ? 1 : error(kEinval);      // PR_CAPBSET_READ: every capability is in the bounding set
    case 24: return a <= 40 ? 0 : error(kEinval);      // PR_CAPBSET_DROP
    case 27: return 0;                    // PR_GET_SECUREBITS
    case 28: return 0;                    // PR_SET_SECUREBITS
    case 29: s.timer_slack_ns = a ? a : 50000; return 0;      // PR_SET_TIMERSLACK
    case 30: return s.timer_slack_ns;                         // PR_GET_TIMERSLACK
    case 36: s.child_subreaper = a ? 1 : 0; return 0;         // PR_SET_CHILD_SUBREAPER
    case 37: return store(s.child_subreaper);                 // PR_GET_CHILD_SUBREAPER
    case 38: if (a != 1) return error(kEinval); s.no_new_privileges = 1; return 0;      // PR_SET_NO_NEW_PRIVS
    case 39: return s.no_new_privileges;                      // PR_GET_NO_NEW_PRIVS
    case 41: s.thp_disable = a ? 1 : 0; return 0;             // PR_SET_THP_DISABLE
    case 42: return s.thp_disable;                            // PR_GET_THP_DISABLE
    case 50: case 51: case 62: case 63:      // the SVE vector length (50 set, 51 get) and the SME one (62, 63): the emulated AArch64 has 16-byte vectors
      return arch == UserArch::AArch64 ? 16 : error(kEinval);
    case 0x53564d41: return 0;      // PR_SET_VMA: a name for a piece of memory (nothing shows names)
    case 0x59616d61: return 0;      // PR_SET_PTRACER (Yama): nothing traces anything
    default: (void)b; return error(kEinval);      // what the kernel does not have, as Linux answers
  }
}

uint64_t sys_membarrier(Process& p, uint64_t command, uint64_t flags) {
  constexpr uint64_t kQuery = 0, kGlobal = 1, kPrivateExpedited = 8, kRegisterPrivateExpedited = 16;
  if (flags != 0) return error(kEinval);
  if (command == kQuery) return kGlobal | kPrivateExpedited | kRegisterPrivateExpedited;
  if (command == kRegisterPrivateExpedited) return 0;
  if (command != kGlobal && command != kPrivateExpedited) return error(kEinval);
  if (p.runtime()) p.runtime()->flush_write_buffers();      // every core has passed a barrier when this returns
  return 0;
}

uint64_t sys_set_robust_list(Process& p, uint64_t head, uint64_t size) {
  if (size != 24) return error(kEinval);      // sizeof(struct robust_list_head)
  LinuxProcessState::Thread& thread = thread_state(p);
  thread.robust_head = head;
  thread.robust_size = size;
  return 0;
}

uint64_t sys_get_robust_list(Process& p, uint64_t pid, uint64_t head_at, uint64_t size_at) {
  if (int32_t(pid) < 0) return error(kEinval);
  const LinuxProcessState::Thread* thread = nullptr;
  if (int32_t(pid) == 0) {
    thread = &thread_state(p);
  } else {
    for (const LinuxProcessState::Thread& t : p.linux_state().threads)
      if (t.used && uint32_t(t.tid) == uint32_t(pid)) thread = &t;
    if (!thread) return error(kEsrch);
  }
  return p.write_user_u64(head_at, thread->robust_head) && p.write_user_u64(size_at, thread->robust_size) ? 0 : error(kEfault);
}

// --- pidfd: a descriptor that stands for a process -----------------------------------------------------------------------------------------------------
// It is a "linked" descriptor of the AF_UNIX manager whose link is the process id. poll() reports it readable once the process has ended.

namespace {
bool process_there(Process& p, uint64_t pid) {
  if (pid == p.id()) return true;
  for (const LinuxProcessState::Child& child : p.linux_state().children)
    if (child.used && child.pid == pid) return true;      // (also one that ended and was not waited for yet)
  return p.runtime() && p.runtime()->process_exists(pid);
}
}  // namespace

bool pidfd_process(Process& p, uint64_t fd, uint64_t* pid) {
  uint32_t mode = 0;
  return linked_of(p, fd, UnixType::Pidfd, pid, &mode) == 0;
}

bool pidfd_ready(Process& p, uint64_t pid) {
  if (pid == p.id()) return false;
  for (const LinuxProcessState::Child& child : p.linux_state().children)
    if (child.used && child.pid == pid && child.exited) return true;
  IProcessRuntime* runtime = p.runtime();
  if (!runtime) return true;      // (nobody runs other processes: whatever it was is not running)
  bool exited = false;
  uint32_t code = 0;
  if (runtime->query_child(pid, &exited, &code)) return exited;
  return !runtime->process_exists(pid);
}

// pidfd_open(pid, flags): PIDFD_NONBLOCK (O_NONBLOCK) is kept for waitid(P_PIDFD); a descriptor for a single thread (PIDFD_THREAD) is not to be had.
uint64_t sys_pidfd_open(Process& p, uint64_t pid, uint64_t flags) {
  constexpr uint64_t kNonBlock = 0x800;
  if ((flags & ~kNonBlock) != 0 || int32_t(pid) <= 0) return error(kEinval);
  if (!process_there(p, uint32_t(pid))) return error(kEsrch);
  return open_linked(p, UnixType::Pidfd, uint32_t(pid), 0, true, (flags & kNonBlock) != 0);      // (always close-on-exec)
}

// pidfd_send_signal(pidfd, signal, siginfo_t*, flags): kill() by descriptor. (A siginfo of the caller's making is not kept, as with rt_sigqueueinfo.)
uint64_t sys_pidfd_send_signal(Process& p, uint64_t fd, uint64_t signal, uint64_t info, uint64_t flags) {
  uint64_t pid = 0;
  if (!pidfd_process(p, fd, &pid)) return error(kEbadf);
  if (flags != 0) return error(kEinval);
  if (info != 0) {
    uint8_t probe[16];
    if (!p.read_user(info, ByteView(probe, sizeof(probe)))) return error(kEfault);
  }
  return sys_kill(p, pid, signal);
}

// pidfd_getfd(pidfd, fd, flags): a copy of another process's descriptor. Only of the caller's own: nothing here may reach into another process.
uint64_t sys_pidfd_getfd(Process& p, uint64_t fd, uint64_t target, uint64_t flags) {
  uint64_t pid = 0;
  if (!pidfd_process(p, fd, &pid)) return error(kEbadf);
  if (flags != 0) return error(kEinval);
  if (pid != p.id()) return error(process_there(p, pid) ? kEperm : kEsrch);
  if (p.files().kind(target) == DescriptorKind::Closed) return error(kEbadf);
  const uint64_t copy = sys_dup(p, target);
  if (int64_t(copy) >= 0) p.files().set_close_on_exec(copy, true);
  return copy;
}

}  // namespace process::linuxabi
