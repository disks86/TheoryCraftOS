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
#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// The Linux calls about the process itself: its signals (LinuxSignals.cpp), and fork, wait, ids, limits, scheduling and the like (LinuxProcess.cpp). Each
// sys_ function returns the value for the result register: the result, or -errno.
namespace process::linuxabi {

// --- signals ---------------------------------------------------------------------------------------------------------------------------------------------
// The kernel keeps what a program says about its signals (handlers, masks, the alternate stack) and gives it back, and it carries out what needs no handler:
// a signal whose action is the default ends the process (or is ignored, for the ones that are by default), an ignored one is dropped, a blocked one waits.
// It cannot interrupt a thread to run a handler: a signal with a handler stays pending, where sigpending, sigtimedwait (sigwait) and sigsuspend (which then
// returns) find it. Signals come from the process itself (kill, tgkill, raise, abort), from its timers (alarm, setitimer: looked at when the process makes a
// system call) and, between processes, only as an ending: kill() of another process ends it when the signal's default action would.

// What the calling thread keeps for itself (made on first use; the first thread's if the table is full).
LinuxProcessState::Thread& thread_state(Process& p);
// A thread ended: its entry is free again.
void thread_gone(Process& p, uint64_t tid);
// The process raises `signal` (1..64) for itself: see above. Anything else is ignored.
void raise_signal(Process& p, uint32_t signal);
// Raises the signals of the timers that ran out (alarm, setitimer). Cheap when none is armed: the system-call dispatcher calls it for every call.
void check_timers(Process& p);

uint64_t sys_rt_sigaction(Process& p, UserArch arch, uint64_t signal, uint64_t action, uint64_t old_action, uint64_t set_size);
uint64_t sys_rt_sigprocmask(Process& p, uint64_t how, uint64_t set, uint64_t old_set, uint64_t set_size);
uint64_t sys_sigaltstack(Process& p, uint64_t stack, uint64_t old_stack);
uint64_t sys_rt_sigpending(Process& p, uint64_t set, uint64_t set_size);
uint64_t sys_rt_sigtimedwait(Process& p, uint64_t set, uint64_t info, uint64_t timeout, uint64_t set_size);
// sigsuspend(mask) and pause() (`has_mask` false: with the mask the thread has): waits until a signal with a handler arrives (EINTR) or one ends the process.
uint64_t sys_rt_sigsuspend(Process& p, uint64_t mask, uint64_t set_size, bool has_mask);
uint64_t sys_kill(Process& p, uint64_t pid, uint64_t signal);
// tgkill(tgid, tid, signal); tkill(tid, signal) is the same with the caller's own process.
uint64_t sys_tgkill(Process& p, uint64_t tgid, uint64_t tid, uint64_t signal);
uint64_t sys_alarm(Process& p, uint64_t seconds);
uint64_t sys_getitimer(Process& p, uint64_t which, uint64_t value);
uint64_t sys_setitimer(Process& p, uint64_t which, uint64_t value, uint64_t old_value);

// --- fork and wait ---------------------------------------------------------------------------------------------------------------------------------------
// fork() makes a second process that is a copy of the first: its memory (copied at once, page by page: there is no copy-on-write), its descriptors (the open
// files are shared, as on Linux), its signal settings, and one thread that returns 0 from the call the parent's thread returns the child's id from. vfork()
// and clone() without CLONE_THREAD (what posix_spawn uses, with CLONE_VM | CLONE_VFORK) are the same copy: the child does not share the parent's memory and
// the parent does not wait for it, which only a program that counts on either could tell. The runtime does the copying once the call is over
// (Process::take_fork, UserProgram::fork_from); without one that can (IProcessRuntime::supports_linux_fork) the answer is ENOSYS.
struct ForkOptions {
  uint64_t stack = 0;                // the child's stack pointer (0: the caller's)
  bool set_tls = false;
  uint64_t tls = 0;
  uint64_t parent_tid_at = 0;        // CLONE_PARENT_SETTID: the child's id is stored here, in the parent
  uint64_t set_child_tid = 0;        // CLONE_CHILD_SETTID: and here, in the child
  uint64_t clear_child_tid = 0;      // CLONE_CHILD_CLEARTID
};
uint64_t sys_fork(Process& p, const ForkOptions& options);
// The child of a fork() gets what the parent has (LinuxProcessState, the descriptors, the break): called by UserProgram::fork_from once the child's memory is a copy.
void inherit_fork_state(Process& child, Process& parent, const LinuxProcessState::ForkRequest& request);
// The fork could not be made after all: the parent forgets the child it was told about.
void fork_failed(Process& parent, uint64_t pid);
// For a runtime that lets go of a child's record when it has ended: the parent keeps how it ended until wait() asks.
void note_child_exit(Process& parent, uint64_t pid, uint32_t code, uint32_t signal);
uint64_t sys_wait4(Process& p, uint64_t pid, uint64_t status_at, uint64_t options, uint64_t usage_at);
uint64_t sys_waitid(Process& p, uint64_t id_type, uint64_t id, uint64_t info_at, uint64_t options, uint64_t usage_at);

// --- who the process is ------------------------------------------------------------------------------------------------------------------------------------
enum class IdCall : uint8_t { Getuid, Geteuid, Getgid, Getegid, Setuid, Setgid, Setreuid, Setregid, Setresuid, Setresgid, Getresuid, Getresgid, Setfsuid, Setfsgid };
uint64_t sys_id(Process& p, IdCall call, uint64_t a, uint64_t b, uint64_t c);
uint64_t sys_getgroups(Process& p, uint64_t size, uint64_t list);
uint64_t sys_setgroups(Process& p, uint64_t size, uint64_t list);
uint64_t sys_getpgid(Process& p, uint64_t pid);
uint64_t sys_setpgid(Process& p, uint64_t pid, uint64_t group);
uint64_t sys_getsid(Process& p, uint64_t pid);
uint64_t sys_setsid(Process& p);
uint64_t sys_capget(Process& p, uint64_t header, uint64_t data);
uint64_t sys_capset(Process& p, uint64_t header, uint64_t data);

// --- limits, usage, scheduling -------------------------------------------------------------------------------------------------------------------------------
// prlimit64(pid, resource, new, old); getrlimit and setrlimit are it with one of the two.
uint64_t sys_prlimit(Process& p, uint64_t pid, uint64_t resource, uint64_t new_limit, uint64_t old_limit);
uint64_t sys_getrusage(Process& p, uint64_t who, uint64_t usage);
uint64_t sys_times(Process& p, uint64_t buffer);
uint64_t sys_sysinfo(Process& p, uint64_t info);
uint64_t sys_getpriority(Process& p, uint64_t which, uint64_t who);
uint64_t sys_setpriority(Process& p, uint64_t which, uint64_t who, uint64_t priority);
enum class SchedCall : uint8_t { SetParam, GetParam, SetScheduler, GetScheduler, PriorityMax, PriorityMin, RrInterval, SetAttr, GetAttr };
uint64_t sys_sched(Process& p, SchedCall call, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
uint64_t sys_sched_setaffinity(Process& p, uint64_t pid, uint64_t size, uint64_t mask_address);
uint64_t sys_getcpu(Process& p, uint64_t cpu, uint64_t node);
uint64_t sys_personality(Process& p, uint64_t persona);
uint64_t sys_prctl(Process& p, UserArch arch, uint64_t option, uint64_t a, uint64_t b);
uint64_t sys_membarrier(Process& p, uint64_t command, uint64_t flags);
uint64_t sys_set_robust_list(Process& p, uint64_t head, uint64_t size);
uint64_t sys_get_robust_list(Process& p, uint64_t pid, uint64_t head_at, uint64_t size_at);
// The CPU time of the process (`thread` false) or of the calling thread, in nanoseconds (0 where nothing counts it).
uint64_t cpu_time_ns(Process& p, bool thread);

}  // namespace process::linuxabi
