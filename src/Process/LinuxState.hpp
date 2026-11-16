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

namespace process {

// What a Linux process keeps about itself beyond its memory and its descriptors: how it wants its signals handled, who it says it is, its limits, the timers
// it set and the processes it made with fork(). It is part of the Process and is touched with the process lock held. A child made by fork() starts with a
// copy of most of it (LinuxProcess.cpp: inherit_fork_state).
struct LinuxProcessState {
  // --- signals (LinuxSignals.cpp). Signal n is bit n - 1 of a mask.
  static constexpr uint32_t kSignals = 64;
  struct Action {
    uint64_t handler = 0;      // SIG_DFL 0, SIG_IGN 1, or the program's function
    uint64_t flags = 0, restorer = 0, mask = 0;
  };
  Action actions[kSignals + 1];      // [n]: signal n ([0] is not used)
  uint64_t pending = 0;              // raised and not taken yet: blocked, or waiting for a handler this OS cannot run
  uint32_t exit_signal = 0;          // the signal that ended the process (0: it ended by itself)
  bool handler_reported = false;     // the note "handlers are not run" was written once

  // What each thread has of its own: the signals it blocks, its alternate signal stack and its robust futex list.
  static constexpr uint32_t kMaxThreads = 65;
  struct Thread {
    bool used = false;
    uint64_t tid = 0;
    uint64_t blocked = 0;
    uint64_t alt_stack = 0, alt_size = 0;
    uint32_t alt_flags = 2;      // SS_DISABLE
    uint64_t robust_head = 0, robust_size = 0;
  };
  Thread threads[kMaxThreads];

  // --- who the process says it is. Everything runs as root; a program that gives its rights up reads back what it set.
  uint32_t real_user = 0, effective_user = 0, saved_user = 0, file_user = 0;
  uint32_t real_group = 0, effective_group = 0, saved_group = 0, file_group = 0;
  static constexpr uint32_t kMaxGroups = 32;
  uint32_t groups[kMaxGroups] = {};
  uint32_t group_count = 0;
  uint64_t process_group = 0, session = 0;      // 0: the process's own id
  uint32_t file_mode_mask = 022;                // umask

  // --- limits (getrlimit, setrlimit, prlimit64). `set` tells the ones the program changed from the defaults.
  static constexpr uint32_t kLimits = 16;
  struct Limit {
    bool set = false;
    uint64_t current = 0, maximum = 0;
  };
  Limit limits[kLimits];

  // --- scheduling and the odds and ends of prctl
  int32_t nice = 0;
  uint32_t policy = 0, realtime_priority = 0;
  char name[16] = {};      // PR_SET_NAME (not NUL-terminated when it is 16 long)
  bool name_set = false;
  uint32_t dumpable = 1, parent_death_signal = 0, keep_capabilities = 0, no_new_privileges = 0, child_subreaper = 0, thp_disable = 0;
  uint64_t timer_slack_ns = 50000;
  uint64_t personality = 0;

  // --- timers that end in a signal: alarm() and setitimer(). They are looked at when the process makes a system call (signals are not delivered in between).
  struct IntervalTimer {
    uint64_t deadline_ns = 0;      // on the monotonic clock; 0: not armed
    uint64_t interval_ns = 0;
  };
  IntervalTimer timers[3];      // ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF (all on the monotonic clock here)

  // --- the processes fork() made, until wait() has told the program how each one ended
  static constexpr uint32_t kMaxChildren = 64;
  struct Child {
    bool used = false, exited = false;
    uint64_t pid = 0;
    uint32_t code = 0;        // its exit code
    uint32_t signal = 0;      // the signal this process ended it with (kill): wait() reports that instead of the code
  };
  Child children[kMaxChildren];

  // fork() decided to make a process: whoever runs the program copies it (the memory, the calling thread's registers) once the call is over, as with clone()
  // for a thread. See Process::take_fork.
  struct ForkRequest {
    bool pending = false;
    uint64_t pid = 0;                       // the child's process id (and the id of its one thread)
    uint64_t stack = 0;                     // the child's stack pointer (0: the caller's)
    bool set_tls = false;
    uint64_t tls = 0;
    uint64_t set_child_tid = 0;             // where the child's thread id is stored in the child's memory (0: nowhere)
    uint64_t clear_child_tid = 0;           // cleared and woken when the child's first thread ends
    uint64_t caller_tid = 0;                // the thread that called (its signal mask is the child's)
  };
  ForkRequest fork;
};

}  // namespace process
