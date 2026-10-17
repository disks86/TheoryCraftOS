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

#include "SystemMonitor.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/AllocatedView.hpp"

uint32_t SystemMonitor::ticks_per_second() const { return kernel::tick_rate(); }
uint32_t SystemMonitor::core_count() const { return kernel::core_count(); }

bool SystemMonitor::core(uint32_t index, desktop::TaskCore* out) {
  kernel::CoreInfo info;
  if (!kernel::core_info(index, &info)) return false;
  out->online = info.online;
  out->efficiency = info.kind == hardware::CoreKind::Efficiency;
  out->busy_ticks = info.busy_ticks;
  out->idle_ticks = info.idle_ticks;
  return true;
}

uint32_t SystemMonitor::threads(desktop::TaskThread* out, uint32_t capacity) {
  ArrayView<kernel::ThreadInfo> info = allocate_view<kernel::ThreadInfo>(shared_, capacity);  // too big for a thread stack
  if (info.empty()) return 0;
  const uint32_t n = kernel::thread_snapshot(info.data(), static_cast<uint32_t>(info.size()));
  for (uint32_t i = 0; i < n; ++i) {
    desktop::TaskThread& t = out[i];
    t.id = info[i].id;
    __builtin_memcpy(t.name, info[i].name, sizeof t.name);
    t.state = info[i].state == kernel::ThreadState::Running ? desktop::TaskState::Running
              : info[i].state == kernel::ThreadState::Blocked ? desktop::TaskState::Blocked
                                                              : desktop::TaskState::Ready;
    t.idle = info[i].idle;
    t.core = info[i].core;
    t.cpu_ticks = info[i].cpu_ticks;
  }
  release_view(shared_, info);
  return n;
}

desktop::TaskMemory SystemMonitor::memory() {
  desktop::TaskMemory m;
  shared_.locked([&] {
    m.total_bytes = heap_.total_bytes();
    m.free_bytes = heap_.free_bytes();
  });
  return m;
}
