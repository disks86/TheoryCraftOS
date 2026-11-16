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
#include "Utility/ByteStream.hpp"

namespace process::linuxabi {

namespace {

constexpr int64_t kEperm = 1, kEsrch = 3, kEintr = 4, kEagain = 11, kEnomem = 12, kEfault = 14, kEinval = 22;
uint64_t error(int64_t e) { return uint64_t(-e); }

constexpr uint32_t kSigHup = 1, kSigKill = 9, kSigAlrm = 14, kSigChld = 17, kSigCont = 18, kSigStop = 19, kSigTstp = 20, kSigTtin = 21, kSigTtou = 22, kSigUrg = 23,
                   kSigVtalrm = 26, kSigProf = 27, kSigWinch = 28;
constexpr uint64_t kSigDfl = 0, kSigIgn = 1;
constexpr uint32_t kSignals = LinuxProcessState::kSignals;

constexpr uint64_t bit(uint32_t signal) { return uint64_t{1} << (signal - 1); }
constexpr uint64_t kUnblockable = bit(kSigKill) | bit(kSigStop);
// What the default action does not end the process for: the signals that are ignored by default, and the ones that stop or continue it (there is no job
// control here: a stopped process would have nobody to continue it).
constexpr uint64_t kDefaultHarmless = bit(kSigChld) | bit(kSigCont) | bit(kSigUrg) | bit(kSigWinch) | bit(kSigStop) | bit(kSigTstp) | bit(kSigTtin) | bit(kSigTtou);

uint64_t current_tid(Process& p) { return p.runtime() ? p.runtime()->current_thread_id() : p.id(); }

// Carries out a signal that is not blocked. True if that is the end of it (ignored, or the process ended); false if it has a handler, which nothing here can
// call: it stays pending for sigtimedwait and sigsuspend.
bool dispose(Process& p, uint32_t signal) {
  LinuxProcessState& state = p.linux_state();
  const uint64_t handler = signal == kSigKill || signal == kSigStop ? kSigDfl : state.actions[signal].handler;
  if (handler == kSigIgn) return true;
  if (handler == kSigDfl) {
    if (kDefaultHarmless & bit(signal)) return true;
    state.exit_signal = signal;
    p.terminate(128 + signal);      // (what a shell reports for a program a signal ended)
    return true;
  }
  if (!state.handler_reported) {
    state.handler_reported = true;
    process_log("linux: a signal with a handler was raised; handlers are not run (it stays pending)");
  }
  return false;
}

// The pending signals the thread does not block are carried out (after a mask changed, or a timer ran out).
void deliver_unblocked(Process& p, uint64_t blocked) {
  LinuxProcessState& state = p.linux_state();
  for (uint32_t signal = 1; signal <= kSignals && !p.exited(); ++signal) {
    if (!(state.pending & bit(signal)) || (blocked & bit(signal))) continue;
    if (dispose(p, signal)) state.pending &= ~bit(signal);
  }
}

bool read_set(Process& p, uint64_t address, uint64_t* out) { return p.read_user_u64(address, out); }

// A timespec or timeval (seconds, then nanoseconds or microseconds) as nanoseconds; false for one that is not valid.
bool to_ns(uint64_t seconds, uint64_t fraction, uint64_t per_second, uint64_t* out) {
  if (int64_t(seconds) < 0 || fraction >= per_second) return false;
  const uint64_t scale = 1000000000u / per_second;
  const uint64_t limit = (~uint64_t{0} - 999999999u) / 1000000000u;
  *out = seconds > limit ? ~uint64_t{0} : seconds * 1000000000u + fraction * scale;
  return true;
}

uint32_t timer_signal(uint32_t which) { return which == 0 ? kSigAlrm : which == 1 ? kSigVtalrm : kSigProf; }

}  // namespace

LinuxProcessState::Thread& thread_state(Process& p) {
  LinuxProcessState& state = p.linux_state();
  const uint64_t tid = current_tid(p);
  LinuxProcessState::Thread* free_entry = nullptr;
  for (LinuxProcessState::Thread& t : state.threads) {
    if (t.used && t.tid == tid) return t;
    if (!t.used && !free_entry) free_entry = &t;
  }
  if (!free_entry) return state.threads[0];
  *free_entry = LinuxProcessState::Thread();
  free_entry->used = true;
  free_entry->tid = tid;
  return *free_entry;
}

void thread_gone(Process& p, uint64_t tid) {
  for (LinuxProcessState::Thread& t : p.linux_state().threads)
    if (t.used && t.tid == tid) t = LinuxProcessState::Thread();
}

void raise_signal(Process& p, uint32_t signal) {
  if (signal == 0 || signal > kSignals) return;
  LinuxProcessState& state = p.linux_state();
  const uint64_t blocked = thread_state(p).blocked & ~kUnblockable;
  if ((blocked & bit(signal)) || !dispose(p, signal)) state.pending |= bit(signal);
}

void check_timers(Process& p) {
  LinuxProcessState& state = p.linux_state();
  if ((state.timers[0].deadline_ns | state.timers[1].deadline_ns | state.timers[2].deadline_ns) == 0) return;
  const uint64_t now = p.host().monotonic_ns();
  for (uint32_t which = 0; which < 3 && !p.exited(); ++which) {
    LinuxProcessState::IntervalTimer& timer = state.timers[which];
    if (timer.deadline_ns == 0 || now < timer.deadline_ns) continue;
    // (one signal however many periods went by: signals of one kind do not queue)
    timer.deadline_ns = timer.interval_ns == 0 ? 0 : timer.interval_ns > ~uint64_t{0} - now ? ~uint64_t{0} : now + timer.interval_ns;
    raise_signal(p, timer_signal(which));
  }
}

// struct sigaction as the kernel has it: handler (8), flags (8), then on x86-64 the restorer (8), then the mask (8).
uint64_t sys_rt_sigaction(Process& p, UserArch arch, uint64_t signal, uint64_t action, uint64_t old_action, uint64_t set_size) {
  if (set_size != 8 || signal == 0 || signal > kSignals) return error(kEinval);
  const bool has_restorer = arch == UserArch::X86_64;
  const uint64_t bytes = has_restorer ? 32 : 24;
  LinuxProcessState& state = p.linux_state();
  LinuxProcessState::Action next = state.actions[signal];
  if (action != 0) {
    if (signal == kSigKill || signal == kSigStop) return error(kEinval);
    uint8_t raw[32];
    if (!p.read_user(action, ByteView(raw, bytes))) return error(kEfault);
    ByteReader reader{ConstByteView(raw, bytes)};
    next.handler = reader.le64();
    next.flags = reader.le64();
    next.restorer = has_restorer ? reader.le64() : 0;
    next.mask = reader.le64() & ~kUnblockable;
  }
  if (old_action != 0) {
    const LinuxProcessState::Action& old = state.actions[signal];
    uint8_t raw[32];
    ByteWriter writer{ByteView(raw, bytes)};
    writer.le64(old.handler);
    writer.le64(old.flags);
    if (has_restorer) writer.le64(old.restorer);
    writer.le64(old.mask);
    if (!p.write_user(old_action, ConstByteView(raw, bytes))) return error(kEfault);
  }
  if (action != 0) {
    state.actions[signal] = next;
    // A pending signal that is now ignored is dropped, as POSIX has it; one whose action became the default is carried out if nothing blocks it.
    if (state.pending & bit(signal)) {
      const bool ignored = next.handler == kSigIgn || (next.handler == kSigDfl && (kDefaultHarmless & bit(signal)));
      if (ignored) state.pending &= ~bit(signal);
      else if (next.handler == kSigDfl) deliver_unblocked(p, thread_state(p).blocked);
    }
  }
  return 0;
}

uint64_t sys_rt_sigprocmask(Process& p, uint64_t how, uint64_t set, uint64_t old_set, uint64_t set_size) {
  if (set_size != 8) return error(kEinval);
  LinuxProcessState::Thread& thread = thread_state(p);
  const uint64_t before = thread.blocked;
  uint64_t after = before;
  if (set != 0) {
    uint64_t bits = 0;
    if (!read_set(p, set, &bits)) return error(kEfault);
    if (how == 0) after = before | bits;             // SIG_BLOCK
    else if (how == 1) after = before & ~bits;       // SIG_UNBLOCK
    else if (how == 2) after = bits;                 // SIG_SETMASK
    else return error(kEinval);
    after &= ~kUnblockable;
  }
  if (old_set != 0 && !p.write_user_u64(old_set, before)) return error(kEfault);
  thread.blocked = after;
  if (before & ~after) deliver_unblocked(p, after);
  return 0;
}

// stack_t: ss_sp (8), ss_flags (4), padding (4), ss_size (8). Handlers are not run, so the stack is only kept and given back.
uint64_t sys_sigaltstack(Process& p, uint64_t stack, uint64_t old_stack) {
  constexpr uint32_t kSsDisable = 2, kSsAutodisarm = 0x80000000u;
  constexpr uint64_t kMinStack = 2048;
  LinuxProcessState::Thread& thread = thread_state(p);
  LinuxProcessState::Thread next = thread;
  if (stack != 0) {
    uint8_t raw[24];
    if (!p.read_user(stack, ByteView(raw, sizeof(raw)))) return error(kEfault);
    ByteReader reader{ConstByteView(raw, sizeof(raw))};
    const uint64_t base = reader.le64();
    const uint32_t flags = reader.le32();
    reader.skip(4);
    const uint64_t size = reader.le64();
    if (flags & ~(kSsDisable | kSsAutodisarm)) return error(kEinval);
    if (flags & kSsDisable) {
      next.alt_stack = next.alt_size = 0;
      next.alt_flags = kSsDisable;
    } else {
      if (size < kMinStack) return error(kEnomem);
      next.alt_stack = base;
      next.alt_size = size;
      next.alt_flags = flags;
    }
  }
  if (old_stack != 0) {
    uint8_t raw[24];
    ByteWriter writer{ByteView(raw, sizeof(raw))};
    writer.le64(thread.alt_stack);
    writer.le32(thread.alt_flags);
    writer.le32(0);
    writer.le64(thread.alt_size);
    if (!p.write_user(old_stack, ConstByteView(raw, sizeof(raw)))) return error(kEfault);
  }
  thread = next;
  return 0;
}

uint64_t sys_rt_sigpending(Process& p, uint64_t set, uint64_t set_size) {
  if (set_size > 8) return error(kEinval);
  check_timers(p);
  uint8_t raw[8];
  ByteWriter writer{ByteView(raw, sizeof(raw))};
  writer.le64(p.linux_state().pending & thread_state(p).blocked);
  return p.write_user(set, ConstByteView(raw, set_size)) ? 0 : error(kEfault);
}

// rt_sigtimedwait(set, info, timeout, sigsetsize): takes the lowest pending signal of `set` (what sigwait and sigwaitinfo are made of), waiting for one up to
// the timeout (none: for ever). siginfo_t is 128 bytes: si_signo, si_errno, si_code (4 each); the rest is zero (SI_USER, from this process).
uint64_t sys_rt_sigtimedwait(Process& p, uint64_t set, uint64_t info, uint64_t timeout, uint64_t set_size) {
  if (set_size != 8) return error(kEinval);
  uint64_t wanted = 0;
  if (!read_set(p, set, &wanted)) return error(kEfault);
  wanted &= ~kUnblockable;
  uint64_t wait_ns = ~uint64_t{0};
  if (timeout != 0) {
    uint64_t seconds = 0, nanos = 0;
    if (!p.read_user_u64(timeout, &seconds) || !p.read_user_u64(timeout + 8, &nanos)) return error(kEfault);
    if (!to_ns(seconds, nanos, 1000000000u, &wait_ns)) return error(kEinval);
  }
  LinuxProcessState& state = p.linux_state();
  const uint64_t start = p.host().monotonic_ns();
  const uint64_t deadline = wait_ns > ~uint64_t{0} - start ? ~uint64_t{0} : start + wait_ns;
  for (;;) {
    check_timers(p);
    if (p.exited()) return error(kEintr);
    if (const uint64_t hit = state.pending & wanted) {
      uint32_t signal = 1;
      while (!(hit & bit(signal))) ++signal;
      state.pending &= ~bit(signal);
      if (info != 0) {
        uint8_t raw[128] = {};
        ByteWriter writer{ByteView(raw, sizeof(raw))};
        writer.le32(signal);
        if (!p.write_user(info, ConstByteView(raw, sizeof(raw)))) return error(kEfault);
      }
      return signal;
    }
    const uint64_t now = p.host().monotonic_ns();
    if (now >= deadline) return error(kEagain);
    if (p.runtime() && p.runtime()->interrupted()) return error(kEintr);
    const uint64_t left = deadline - now;
    p.block_sleep(left < 2000000u ? left : 2000000u);      // (looked at again every 2 ms: another thread's kill() or a timer may have raised one)
  }
}

uint64_t sys_rt_sigsuspend(Process& p, uint64_t mask, uint64_t set_size, bool has_mask) {
  LinuxProcessState& state = p.linux_state();
  uint64_t blocked = thread_state(p).blocked;
  if (has_mask) {
    if (set_size != 8) return error(kEinval);
    if (!read_set(p, mask, &blocked)) return error(kEfault);
    blocked &= ~kUnblockable;
  }
  for (;;) {
    check_timers(p);
    deliver_unblocked(p, blocked);
    if (p.exited()) return error(kEintr);
    // What is left unblocked has a handler: the call ends as if it had run (the signal is taken; the handler is not called).
    if (const uint64_t hit = state.pending & ~blocked) {
      uint32_t signal = 1;
      while (!(hit & bit(signal))) ++signal;
      state.pending &= ~bit(signal);
      return error(kEintr);
    }
    if (p.runtime() && p.runtime()->interrupted()) return error(kEintr);
    p.block_sleep(10000000u);      // (looked at again every 10 ms: another thread's kill() or a timer may have raised one)
  }
}

namespace {

// kill() of another process: there is no way to hand it a signal, only to end it. So a signal whose default action ends a process does (the other process's
// own handlers are not asked), 0 only asks whether it is there, and the rest is taken and dropped.
uint64_t kill_other(Process& p, uint64_t pid, uint32_t signal) {
  IProcessRuntime* runtime = p.runtime();
  LinuxProcessState& state = p.linux_state();
  LinuxProcessState::Child* child = nullptr;
  for (LinuxProcessState::Child& c : state.children)
    if (c.used && c.pid == pid) child = &c;
  const bool exists = child != nullptr || (runtime && runtime->process_exists(pid));
  if (!exists) return error(kEsrch);
  if (signal == 0 || (kDefaultHarmless & bit(signal) & ~bit(kSigStop))) return 0;
  if (child && !child->exited && runtime) {      // (has it ended since it was last looked at?)
    bool exited = false;
    uint32_t code = 0;
    if (runtime->query_child(pid, &exited, &code) && exited) {
      child->exited = true;
      child->code = code;
      if (child->signal == 0) child->signal = runtime->child_exit_signal(pid);
    }
  }
  if (child && child->exited) return 0;      // (a zombie takes any signal)
  if (!runtime || !runtime->terminate_process(pid, 128 + signal)) return child ? 0 : error(kEperm);
  if (child) child->signal = signal;
  return 0;
}

}  // namespace

uint64_t sys_kill(Process& p, uint64_t pid, uint64_t signal) {
  if (signal > kSignals) return error(kEinval);
  const int64_t target = int32_t(pid);
  const LinuxProcessState& state = p.linux_state();
  const uint64_t group = state.process_group ? state.process_group : p.id();
  // The process itself: by its id, as its process group (0, or the group's id negated). -1 (everything the caller may signal) is refused: nothing here
  // should be able to end every program at once.
  if (target == -1) return error(kEperm);
  if (target == 0 || uint64_t(target) == p.id() || (target < 0 && uint64_t(-target) == group)) {
    raise_signal(p, uint32_t(signal));
    return 0;
  }
  if (target < 0) return error(kEsrch);
  return kill_other(p, uint64_t(target), uint32_t(signal));
}

uint64_t sys_tgkill(Process& p, uint64_t tgid, uint64_t tid, uint64_t signal) {
  if (signal > kSignals || int32_t(tgid) <= 0 || int32_t(tid) <= 0) return error(kEinval);
  if (uint32_t(tgid) != uint32_t(p.id())) return kill_other(p, uint32_t(tgid), uint32_t(signal));
  // A thread of this process. Signals are kept for the process as a whole (which thread would run a handler does not matter when none is run); a thread that is
  // not there is ESRCH, which is how a program asks whether one still lives.
  bool found = uint32_t(tid) == uint32_t(p.id());
  for (const Process::LinuxThreadState::Thread& t : p.linux_threads().threads)
    if (t.used && uint32_t(t.tid) == uint32_t(tid)) found = true;
  if (!found) return error(kEsrch);
  raise_signal(p, uint32_t(signal));
  return 0;
}

uint64_t sys_alarm(Process& p, uint64_t seconds) {
  LinuxProcessState::IntervalTimer& timer = p.linux_state().timers[0];
  const uint64_t now = p.host().monotonic_ns();
  uint64_t left = 0;
  if (timer.deadline_ns > now) left = (timer.deadline_ns - now + 999999999u) / 1000000000u;      // (rounded up: an alarm that is still to come is never reported as 0)
  const uint64_t s = uint32_t(seconds);
  timer.interval_ns = 0;
  timer.deadline_ns = s == 0 ? 0 : now + s * 1000000000u;
  return left;
}

// struct itimerval: it_interval then it_value, each a timeval (seconds 8, microseconds 8).
uint64_t sys_getitimer(Process& p, uint64_t which, uint64_t value) {
  if (which > 2) return error(kEinval);
  const LinuxProcessState::IntervalTimer& timer = p.linux_state().timers[which];
  const uint64_t now = p.host().monotonic_ns();
  const uint64_t left = timer.deadline_ns > now ? timer.deadline_ns - now : timer.deadline_ns ? 1000 : 0;
  uint8_t raw[32];
  ByteWriter writer{ByteView(raw, sizeof(raw))};
  writer.le64(timer.interval_ns / 1000000000u);
  writer.le64(timer.interval_ns % 1000000000u / 1000);
  writer.le64(left / 1000000000u);
  writer.le64(left % 1000000000u / 1000);
  return p.write_user(value, ConstByteView(raw, sizeof(raw))) ? 0 : error(kEfault);
}

uint64_t sys_setitimer(Process& p, uint64_t which, uint64_t value, uint64_t old_value) {
  if (which > 2) return error(kEinval);
  uint64_t interval = 0, first = 0;
  if (value != 0) {
    uint8_t raw[32];
    if (!p.read_user(value, ByteView(raw, sizeof(raw)))) return error(kEfault);
    ByteReader reader{ConstByteView(raw, sizeof(raw))};
    const uint64_t interval_s = reader.le64(), interval_us = reader.le64(), value_s = reader.le64(), value_us = reader.le64();
    if (!to_ns(interval_s, interval_us, 1000000u, &interval) || !to_ns(value_s, value_us, 1000000u, &first)) return error(kEinval);
  }
  if (old_value != 0) {
    if (const uint64_t r = sys_getitimer(p, which, old_value); r != 0) return r;
  }
  LinuxProcessState::IntervalTimer& timer = p.linux_state().timers[which];
  const uint64_t now = p.host().monotonic_ns();
  timer.interval_ns = first == 0 ? 0 : interval;
  timer.deadline_ns = first == 0 ? 0 : first > ~uint64_t{0} - now ? ~uint64_t{0} : now + first;
  return 0;
}

}  // namespace process::linuxabi
