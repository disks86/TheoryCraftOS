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
#include "Process/Objects.hpp"
#include "Process/Process.hpp"

namespace process::windowsabi {

constexpr uint64_t kWaitPending = ~uint64_t{0};      // (what an attempt returns to say: not yet)

// Runs attempt() with the system lock held until it gives something other than kWaitPending, waiting between tries (the process's own lock is given up while it waits).
template <typename Attempt>
inline uint32_t wait_until(Process& p, uint64_t timeout_ns, Attempt&& attempt) {
  ObjectManager* objects = system_objects();
  if (!objects) return 0xC0000002u;
  ISystemSync& sync = objects->sync();
  IProcessRuntime* runtime = p.runtime();
  const uint64_t started = p.host().monotonic_ns();
  sync.lock();
  for (;;) {
    const uint64_t status = attempt();
    if (status != kWaitPending) { sync.unlock(); return uint32_t(status); }
    uint64_t left = ~uint64_t{0};
    if (timeout_ns != ~uint64_t{0}) {
      const uint64_t used = p.host().monotonic_ns() - started;
      if (used >= timeout_ns) { sync.unlock(); return 0x102u; }
      left = timeout_ns - used;
    }
    if (p.exited() || (runtime && runtime->interrupted())) { sync.unlock(); return 0x101u; }
    if (runtime) {      // asked to suspend while it waits: it stops here, with its registers kept (GetThreadContext), and goes on waiting when resumed
      sync.unlock();
      const int parked = runtime->park_point();
      sync.lock();
      if (parked == 2) { sync.unlock(); return 0x101u; }
      if (parked == 1) continue;
    }
    if (runtime) runtime->unlock();
    else p.host().yield();
    sync.wait(left);
    sync.unlock();
    if (runtime) runtime->lock();
    sync.lock();
  }
}


}  // namespace process::windowsabi
