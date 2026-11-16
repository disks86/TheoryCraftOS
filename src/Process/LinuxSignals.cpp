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
#include "Utility/ByteStream.hpp"

namespace process::linuxabi {

namespace {

constexpr int64_t kEperm = 1, kEsrch = 3, kEintr = 4, kEagain = 11, kEnomem = 12, kEfault = 14, kEinval = 22;
constexpr uint32_t kSigIll = 4, kSigTrap = 5, kSigBus = 7, kSigFpe = 8, kSigSegv = 11;
constexpr uint64_t kSaNoDefer = 0x40000000u, kSaResetHand = 0x80000000u, kSaRestart = 0x10000000u, kSaOnStack = 0x08000000u, kSaRestorer = 0x04000000u;
constexpr uint32_t kSsOnStack = 1, kSsDisable = 2, kSsAutodisarm = 0x80000000u;
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

// Is the signal one that nothing happens for as things stand: ignored by the program, or by default?
bool is_ignored(const LinuxProcessState& state, uint32_t signal) {
  if (signal == kSigKill || signal == kSigStop) return signal == kSigStop;
  const uint64_t handler = state.actions[signal].handler;
  return handler == kSigIgn || (handler == kSigDfl && (kDefaultHarmless & bit(signal)));
}

// Carries out a signal that is not blocked. True if that is the end of it (ignored, or the process ended); false if it has a handler: it stays pending until
// whoever runs the program enters the handler (deliver_signals), or, where nothing can, for sigtimedwait and sigsuspend.
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
  if (!state.handlers_run && !state.handler_reported) {
    state.handler_reported = true;
    process_log("linux: a signal with a handler was raised; handlers are not run (it stays pending)");
  }
  return false;
}

// The pending signals the thread does not block are carried out (after a mask changed, or a timer ran out).
void deliver_unblocked(Process& p, uint64_t blocked) {
  LinuxProcessState& state = p.linux_state();
  LinuxProcessState::Thread& thread = thread_state(p);
  for (uint32_t signal = 1; signal <= kSignals && !p.exited(); ++signal) {
    if (!((state.pending | thread.pending) & bit(signal)) || (blocked & bit(signal))) continue;
    if (dispose(p, signal)) {
      state.pending &= ~bit(signal);
      thread.pending &= ~bit(signal);
    }
  }
}

// What was posted from outside (Process::post_linux_signal) becomes pending for the process; what would be ignored is dropped here already.
void fold_posted(Process& p) {
  if (p.posted_signals() == 0) return;
  LinuxProcessState& state = p.linux_state();
  const uint64_t posted = p.take_posted_signals();
  for (uint32_t signal = 1; signal <= kSignals; ++signal) {
    if (!(posted & bit(signal)) || is_ignored(state, signal)) continue;
    if (!(state.pending & bit(signal))) state.info[signal] = LinuxProcessState::Info();
    state.pending |= bit(signal);
  }
}

LinuxProcessState::Thread& thread_of(Process& p, uint64_t tid) {
  LinuxProcessState& state = p.linux_state();
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

// --- entering a handler (x86-64) ----------------------------------------------------------------------------------------------------------------------
// What the kernel puts on the program's stack before a handler runs, and reads back at rt_sigreturn: struct rt_sigframe of Linux/x86-64.
//   +0    the return address of the handler: the action's restorer (which calls rt_sigreturn)
//   +8    struct ucontext: uc_flags (8), uc_link (8), uc_stack (ss_sp 8, ss_flags 4, padding 4, ss_size 8), uc_mcontext (struct sigcontext, 256), uc_sigmask (8)
//   +312  siginfo_t (128)
// and, above it, the 512-byte FXSAVE image uc_mcontext.fpstate points to. struct sigcontext: r8..r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags
// (8 each), cs, gs, fs, ss (2 each), err, trapno, oldmask, cr2, fpstate (8 each), 8 reserved words.
constexpr uint64_t kUcontextBytes = 304, kSiginfoBytes = 128, kFrameBytes = 8 + kUcontextBytes + kSiginfoBytes, kFpuBytes = 512, kRedZone = 128;
constexpr uint64_t kUserFlags = 0x0CD5 | 0x400 | 0x100 | 0x40000 | 0x200000;      // the status flags, DF, TF, AC, ID: what a program may set in RFLAGS
// The order struct sigcontext keeps the registers in, as indices of X86Context::gpr.
constexpr uint32_t kContextOrder[16] = {X86Context::kR8,  X86Context::kR9,  X86Context::kR10, X86Context::kR11, X86Context::kR12, X86Context::kR13,
                                        X86Context::kR14, X86Context::kR15, X86Context::kRdi, X86Context::kRsi, X86Context::kRbp, X86Context::kRbx,
                                        X86Context::kRdx, X86Context::kRax, X86Context::kRcx, X86Context::kRsp};

struct FaultDetail {
  uint64_t error = 0, trap = 0, address = 0;
};

// Builds the frame for `signal` and points the registers at its handler. False if the frame cannot be written (no stack left, no restorer to return through):
// the caller ends the process, as Linux does with SIGSEGV.
bool push_frame(Process& p, LinuxProcessState::Thread& thread, uint32_t signal, const LinuxProcessState::Action& action, uint64_t old_mask, const FaultDetail& fault,
                X86Context* context) {
  LinuxProcessState& state = p.linux_state();
  if (!(action.flags & kSaRestorer) || action.restorer == 0) return false;      // (x86-64 has no other way back; every C library gives one)
  // The handler's address goes into the instruction pointer as it is: one that is no user address must never get there (the processor would fault in the
  // kernel on the way back to user mode).
  if (action.handler >= AddressSpace::kUserTop) return false;
  uint64_t sp = context->gpr[X86Context::kRsp];
  const bool has_alt = thread.alt_size != 0 && !(thread.alt_flags & kSsDisable);
  const bool on_alt = has_alt && sp > thread.alt_stack && sp - thread.alt_stack <= thread.alt_size;
  const bool switches = (action.flags & kSaOnStack) && has_alt && !on_alt;
  if (switches) {
    if (thread.alt_size > ~uint64_t{0} - thread.alt_stack) return false;
    sp = thread.alt_stack + thread.alt_size;
  } else if (sp >= kRedZone) {
    sp -= kRedZone;      // (the 128 bytes below the stack pointer are the interrupted function's)
  }
  if (sp < kFpuBytes + kFrameBytes + 64) return false;
  const uint64_t fpu_at = (sp - kFpuBytes) & ~uint64_t{63};
  const uint64_t frame_at = ((fpu_at - kFrameBytes) & ~uint64_t{15}) - 8;      // (as after a call: the stack pointer + 8 is 16-byte aligned at the handler's first instruction)
  const uint64_t context_at = frame_at + 8, info_at = context_at + kUcontextBytes;

  uint8_t raw[kFrameBytes] = {};
  ByteWriter writer{ByteView(raw, sizeof(raw))};
  writer.le64(action.restorer);
  writer.le64(0);                                   // uc_flags
  writer.le64(0);                                   // uc_link
  writer.le64(thread.alt_stack);                    // uc_stack
  writer.le32(has_alt ? (on_alt ? kSsOnStack : 0) : kSsDisable);      // (of the code that was interrupted, as Linux reports it)
  writer.le32(0);
  writer.le64(thread.alt_size);
  for (const uint32_t index : kContextOrder) writer.le64(context->gpr[index]);
  writer.le64(context->rip);
  writer.le64(context->rflags);
  writer.le16(0x33);                                // cs: 64-bit user code
  writer.le16(0);
  writer.le16(0);
  writer.le16(0x2B);                                // ss
  writer.le64(fault.error);
  writer.le64(fault.trap);
  writer.le64(old_mask);                            // oldmask
  writer.le64(fault.address);                       // cr2
  writer.le64(fpu_at);                              // fpstate
  writer.zeros(64);
  writer.le64(old_mask);                            // uc_sigmask
  // siginfo_t: si_signo, si_errno, si_code (4 each), padding (4), then what the kind of signal has: the address of a fault, or who sent it (pid, uid) and, for
  // SIGCHLD, how the child ended.
  const LinuxProcessState::Info& info = state.info[signal];
  writer.le32(signal);
  writer.le32(0);
  writer.le32(uint32_t(info.code));
  writer.le32(0);
  if (signal == kSigSegv || signal == kSigBus || signal == kSigFpe || signal == kSigIll || signal == kSigTrap) {
    writer.le64(info.address);
  } else {
    writer.le32(info.sender);
    writer.le32(0);
    writer.le32(uint32_t(info.status));
  }
  if (!writer.ok()) return false;
  if (!p.write_user(fpu_at, ConstByteView(context->fpu, kFpuBytes)) || !p.write_user(frame_at, ConstByteView(raw, sizeof(raw)))) return false;

  context->gpr[X86Context::kRsp] = frame_at;
  context->gpr[X86Context::kRdi] = signal;
  context->gpr[X86Context::kRsi] = info_at;
  context->gpr[X86Context::kRdx] = context_at;
  context->gpr[X86Context::kRax] = 0;
  context->rip = action.handler;
  context->rflags &= ~uint64_t{0x400 | 0x100};      // the direction and trap flags are clear in a handler
  // While the handler runs: its own signal (unless SA_NODEFER) and the action's mask are blocked on top of what was.
  thread.blocked = (thread.blocked | action.mask | ((action.flags & kSaNoDefer) ? 0 : bit(signal))) & ~kUnblockable;
  if (action.flags & kSaResetHand) state.actions[signal].handler = kSigDfl;
  if (switches && (thread.alt_flags & kSsAutodisarm)) {
    thread.alt_stack = thread.alt_size = 0;
    thread.alt_flags = kSsDisable;
  }
  state.info[signal] = LinuxProcessState::Info();
  return true;
}

void end_by(Process& p, uint32_t signal) {
  p.linux_state().exit_signal = signal;
  p.terminate(128 + signal);
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

LinuxProcessState::Thread& thread_state(Process& p) { return thread_of(p, current_tid(p)); }

void inherit_thread_mask(Process& p, uint64_t new_tid) {
  const uint64_t blocked = thread_state(p).blocked;
  thread_of(p, new_tid).blocked = blocked;
}

void thread_gone(Process& p, uint64_t tid) {
  for (LinuxProcessState::Thread& t : p.linux_state().threads)
    if (t.used && t.tid == tid) t = LinuxProcessState::Thread();
}

void raise_signal(Process& p, uint32_t signal, uint64_t for_thread, int32_t code) {
  if (signal == 0 || signal > kSignals) return;
  LinuxProcessState& state = p.linux_state();
  if (is_ignored(state, signal)) return;      // (dropped at once, blocked or not)
  LinuxProcessState::Thread& caller = thread_state(p);
  const bool own = for_thread == 0 || for_thread == caller.tid;
  // For the calling thread (or the process, which the calling thread may act for): carried out at once when it can be. For another thread: left for that thread.
  if (own && !(caller.blocked & ~kUnblockable & bit(signal)) && dispose(p, signal)) return;
  uint64_t& pending = for_thread == 0 ? state.pending : thread_of(p, for_thread).pending;
  if (!(pending & bit(signal))) {
    state.info[signal] = LinuxProcessState::Info();
    state.info[signal].code = code;
    state.info[signal].sender = uint32_t(p.id());
  }
  pending |= bit(signal);
  if (!own && p.runtime()) p.runtime()->notify();      // (the other thread may be waiting in a call)
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
    raise_signal(p, timer_signal(which), 0, 0x80);      // (SI_KERNEL)
  }
}

bool signal_interrupts(Process& p) {
  LinuxProcessState& state = p.linux_state();
  fold_posted(p);
  check_timers(p);
  if (p.exited()) return true;
  LinuxProcessState::Thread& thread = thread_state(p);
  const uint64_t ready = (state.pending | thread.pending) & ~thread.blocked;
  if (ready == 0) return false;
  for (uint32_t signal = 1; signal <= kSignals; ++signal) {
    if (!(ready & bit(signal))) continue;
    if (is_ignored(state, signal)) {      // (it became ignored while it waited)
      state.pending &= ~bit(signal);
      thread.pending &= ~bit(signal);
      continue;
    }
    if (state.actions[signal].handler == kSigDfl || signal == kSigKill) return true;      // it ends the process: the call must not go on waiting
    if (state.handlers_run) return true;
  }
  return false;
}

bool signals_wanted(Process& p) {
  const LinuxProcessState& state = p.linux_state();
  if (p.posted_signals() != 0 || state.pending != 0) return true;
  if ((state.timers[0].deadline_ns | state.timers[1].deadline_ns | state.timers[2].deadline_ns) != 0) return true;
  for (const LinuxProcessState::Thread& t : state.threads)
    if (t.used && (t.pending != 0 || t.has_saved_mask)) return true;
  return false;
}

bool signal_hint(Process& p, uint64_t tid, uint64_t now_ns) {
  // (No lock: every value is read once and only compared. A wrong answer costs one look too many or a tick's delay.)
  const LinuxProcessState& state = p.linux_state();
  if (!__atomic_load_n(&state.handlers_run, __ATOMIC_RELAXED)) return false;
  if (p.posted_signals() != 0) return true;
  for (const LinuxProcessState::IntervalTimer& timer : state.timers) {
    const uint64_t deadline = __atomic_load_n(&timer.deadline_ns, __ATOMIC_RELAXED);
    if (deadline != 0 && now_ns >= deadline) return true;
  }
  const uint64_t for_process = __atomic_load_n(&state.pending, __ATOMIC_RELAXED);
  for (const LinuxProcessState::Thread& t : state.threads) {
    if (!__atomic_load_n(&t.used, __ATOMIC_RELAXED) || __atomic_load_n(&t.tid, __ATOMIC_RELAXED) != tid) continue;
    return ((for_process | __atomic_load_n(&t.pending, __ATOMIC_RELAXED)) & ~__atomic_load_n(&t.blocked, __ATOMIC_RELAXED)) != 0;
  }
  return for_process != 0;      // (a thread that has no entry yet blocks nothing)
}

bool deliver_signals(Process& p, X86Context* context, const InterruptedCall* call) {
  LinuxProcessState& state = p.linux_state();
  fold_posted(p);
  check_timers(p);
  LinuxProcessState::Thread& thread = thread_state(p);
  const uint64_t old_mask = thread.has_saved_mask ? thread.saved_mask : thread.blocked;      // (what the handler's return goes back to)
  bool entered = false;
  uint64_t left_alone = 0;      // signals with a handler where no handler can be entered: they stay pending
  while (!p.exited()) {
    const uint64_t ready = (state.pending | thread.pending) & ~thread.blocked & ~left_alone;
    if (ready == 0) break;
    uint32_t signal = 1;
    while (!(ready & bit(signal))) ++signal;
    const LinuxProcessState::Action action = state.actions[signal];
    const bool by_default = action.handler == kSigDfl || signal == kSigKill || signal == kSigStop;
    if (!by_default && action.handler != kSigIgn && !state.handlers_run) {
      left_alone |= bit(signal);
      continue;
    }
    state.pending &= ~bit(signal);
    thread.pending &= ~bit(signal);
    if (is_ignored(state, signal)) continue;
    if (by_default) {
      end_by(p, signal);
      break;
    }
    // A call the signal cut short starts again after the handler if the action says so (SA_RESTART) and the call is one that can: the registers the handler
    // returns to are those of the call about to be made.
    if (call && call->restarts && context->gpr[X86Context::kRax] == uint64_t(-kEintr) && (action.flags & kSaRestart)) {
      context->gpr[X86Context::kRax] = call->number;
      context->rip -= 2;      // (the SYSCALL instruction is two bytes long)
    }
    if (!push_frame(p, thread, signal, action, old_mask, FaultDetail(), context)) {
      end_by(p, kSigSegv);
      break;
    }
    entered = true;
    break;      // (one handler at a time: the next is entered when this one returns, at its rt_sigreturn)
  }
  if (thread.has_saved_mask) {      // sigsuspend's mask has done its work
    if (!entered) thread.blocked = thread.saved_mask;
    thread.has_saved_mask = false;
  }
  return entered;
}

bool signal_return(Process& p, X86Context* context) {
  uint8_t raw[kUcontextBytes];
  if (!p.read_user(context->gpr[X86Context::kRsp], ByteView(raw, sizeof(raw)))) return false;
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  reader.skip(40);      // uc_flags, uc_link, uc_stack
  for (const uint32_t index : kContextOrder) context->gpr[index] = reader.le64();
  context->rip = reader.le64();
  if (context->rip >= AddressSpace::kUserTop) return false;      // (the frame is the program's to write: see push_frame)
  const uint64_t flags = reader.le64();
  context->rflags = (flags & kUserFlags) | 0x202;      // (only what a program may set itself; interrupts stay on)
  reader.skip(8 + 8 + 8 + 8 + 8);                      // the segments, err, trapno, oldmask, cr2
  const uint64_t fpu_at = reader.le64();
  reader.skip(64);
  const uint64_t mask = reader.le64();
  if (fpu_at != 0) {
    uint8_t fpu[kFpuBytes];
    if (!p.read_user(fpu_at, ByteView(fpu, sizeof(fpu)))) return false;
    fpu[26] = fpu[27] = 0;      // (MXCSR's upper half is reserved: a set bit there would fault when the state is loaded)
    ByteView(context->fpu, kFpuBytes).copy_from(ConstByteView(fpu, sizeof(fpu)));
  }
  thread_state(p).blocked = mask & ~kUnblockable;
  return true;
}

bool deliver_fault(Process& p, X86Context* context, uint64_t vector, uint64_t error, uint64_t address) {
  // What a processor exception is as a signal, and its si_code.
  uint32_t signal = kSigSegv;
  int32_t code = 0x80;      // SI_KERNEL
  uint64_t where = address;
  switch (vector) {
    case 0: signal = kSigFpe; code = 1; where = context->rip; break;                 // divide error: FPE_INTDIV
    case 1: case 3: signal = kSigTrap; code = vector == 3 ? 1 : 2; where = context->rip; break;      // TRAP_BRKPT, TRAP_TRACE
    case 6: signal = kSigIll; code = 2; where = context->rip; break;                 // invalid opcode: ILL_ILLOPN
    case 14: signal = kSigSegv; code = (error & 1) ? 2 : 1; break;                   // page fault: SEGV_ACCERR when the page is there, else SEGV_MAPERR
    case 16: case 19: signal = kSigFpe; code = 7; where = context->rip; break;       // x87 and SIMD exceptions: FPE_FLTINV (which one is not looked up)
    case 17: signal = kSigBus; code = 1; break;                                      // alignment check: BUS_ADRALN
    default: where = 0; break;                                                       // general protection and the rest: SIGSEGV from the kernel
  }
  LinuxProcessState& state = p.linux_state();
  LinuxProcessState::Thread& thread = thread_state(p);
  const LinuxProcessState::Action action = state.actions[signal];
  // A fault cannot wait: with no handler, or with its signal blocked, the process ends, as on Linux.
  if (!state.handlers_run || action.handler == kSigDfl || action.handler == kSigIgn || (thread.blocked & bit(signal))) {
    end_by(p, signal);
    return false;
  }
  state.info[signal] = LinuxProcessState::Info();
  state.info[signal].code = code;
  state.info[signal].address = where;
  FaultDetail detail;
  detail.error = error;
  detail.trap = vector;
  detail.address = vector == 14 ? address : 0;
  if (!push_frame(p, thread, signal, action, thread.blocked, detail, context)) {
    end_by(p, kSigSegv);
    return false;
  }
  return true;
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
    if (is_ignored(state, uint32_t(signal))) {
      state.pending &= ~bit(uint32_t(signal));
      for (LinuxProcessState::Thread& t : state.threads) t.pending &= ~bit(uint32_t(signal));
    } else if (next.handler == kSigDfl) {
      deliver_unblocked(p, thread_state(p).blocked);
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
  fold_posted(p);
  check_timers(p);
  uint8_t raw[8];
  ByteWriter writer{ByteView(raw, sizeof(raw))};
  const LinuxProcessState::Thread& thread = thread_state(p);
  writer.le64((p.linux_state().pending | thread.pending) & thread.blocked);
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
    fold_posted(p);
    check_timers(p);
    if (p.exited()) return error(kEintr);
    LinuxProcessState::Thread& thread = thread_state(p);
    if (const uint64_t hit = (state.pending | thread.pending) & wanted) {
      uint32_t signal = 1;
      while (!(hit & bit(signal))) ++signal;
      state.pending &= ~bit(signal);
      thread.pending &= ~bit(signal);
      const LinuxProcessState::Info detail = state.info[signal];
      state.info[signal] = LinuxProcessState::Info();
      if (info != 0) {
        uint8_t raw[128] = {};
        ByteWriter writer{ByteView(raw, sizeof(raw))};
        writer.le32(signal);
        writer.le32(0);
        writer.le32(uint32_t(detail.code));
        writer.le32(0);
        writer.le32(detail.sender);
        if (!p.write_user(info, ConstByteView(raw, sizeof(raw)))) return error(kEfault);
      }
      return signal;
    }
    if (signal_interrupts(p)) return error(kEintr);      // another signal, one that is not waited for here, has to be seen to
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
    fold_posted(p);
    check_timers(p);
    deliver_unblocked(p, blocked);
    if (p.exited()) return error(kEintr);
    LinuxProcessState::Thread& thread = thread_state(p);
    // What is left unblocked has a handler. Where handlers are entered, the call's mask stays in place until that has happened (the handler then returns to the
    // mask the thread had before the call); where they are not, the signal is taken and the call ends as if the handler had run.
    if (const uint64_t hit = (state.pending | thread.pending) & ~blocked) {
      if (state.handlers_run) {
        if (!thread.has_saved_mask) {
          thread.saved_mask = thread.blocked;
          thread.has_saved_mask = true;
        }
        thread.blocked = blocked;
      } else {
        uint32_t signal = 1;
        while (!(hit & bit(signal))) ++signal;
        state.pending &= ~bit(signal);
        thread.pending &= ~bit(signal);
      }
      return error(kEintr);
    }
    if (p.runtime() && p.runtime()->interrupted()) return error(kEintr);
    p.block_sleep(p.wait_slice(10000000u));      // (looked at again every 10 ms at most: another thread's kill() or a timer may have raised one)
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
  if (signal == 0) return 0;
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
  // A Linux process is handed the signal and does with it what its own settings say (a handler, the default action, nothing). SIGKILL, and a process that
  // cannot be handed one, is ended from here when the signal's default action would.
  if (signal != kSigKill && runtime && runtime->send_signal(pid, signal)) return 0;
  if (kDefaultHarmless & bit(signal)) return 0;
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
    raise_signal(p, uint32_t(signal), 0, 0);      // (SI_USER)
    return 0;
  }
  if (target < 0) return error(kEsrch);
  return kill_other(p, uint64_t(target), uint32_t(signal));
}

uint64_t sys_tgkill(Process& p, uint64_t tgid, uint64_t tid, uint64_t signal) {
  if (signal > kSignals || int32_t(tgid) <= 0 || int32_t(tid) <= 0) return error(kEinval);
  if (uint32_t(tgid) != uint32_t(p.id())) return kill_other(p, uint32_t(tgid), uint32_t(signal));
  // A thread of this process: the signal is that thread's alone. A thread that is not there is ESRCH, which is how a program asks whether one still lives.
  uint64_t thread = 0;
  for (const Process::LinuxThreadState::Thread& t : p.linux_threads().threads)
    if (t.used && uint32_t(t.tid) == uint32_t(tid)) thread = t.tid;
  if (thread == 0) return error(kEsrch);
  raise_signal(p, uint32_t(signal), thread, -6);      // (SI_TKILL)
  return 0;
}

uint64_t sys_rt_sigreturn(Process& p) {
  if (!p.linux_state().handlers_run) return error(kEinval);      // no handler was entered, so there is no frame to return from
  p.linux_state().return_requested = true;      // (the runtime loads the registers the frame holds: the value returned here is never seen)
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
