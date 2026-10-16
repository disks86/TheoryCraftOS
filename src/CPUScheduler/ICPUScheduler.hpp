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

namespace scheduling {

// Mirrors the design doc: high-performance work (rendering, compiling, ...)
// and efficiency work (background sync, ...) are queued separately.
enum class TaskQueue : uint8_t { HighPerformance, Efficiency };

using TaskFunction = void (*)(void* context);

// Cooperative scheduling: a task runs until it returns. To "yield", a task
// submits its continuation and returns. Tasks may run on any core, so they
// must not call firmware services; submit() is safe from any core.
class ICPUScheduler {
 public:
  // Queues a task. Returns false if fn is null or the allocator ran out of memory.
  virtual bool submit(TaskFunction fn, void* context,
                      TaskQueue queue = TaskQueue::HighPerformance) = 0;
  // Starts the next task, HighPerformance before Efficiency, on a free core of
  // the queue's kind (any free core if none; this core if all are busy). Returns
  // once the task is started - possibly still running on another core - or false
  // if nothing was queued.
  virtual bool run_next() = 0;
  // Runs until both queues are empty and every started task has finished.
  virtual void run() = 0;
  // Tasks queued but not yet started, across both queues.
  virtual uint64_t pending() const = 0;

 protected:
  constexpr ICPUScheduler() = default;
  ~ICPUScheduler() = default;
};

}  // namespace scheduling
