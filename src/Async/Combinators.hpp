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
#include "Async/Executor.hpp"

namespace async {

namespace detail {
inline Task<void> run_and_count(Task<void>* task, uint32_t* remaining) {
  co_await *task;
  --*remaining;
}
}  // namespace detail

// Runs the `count` tasks in `tasks` concurrently (they interleave at their awaits)
// and finishes when all of them have. The array must stay alive until then.
inline Task<void> when_all(Task<void>* tasks, uint32_t count) {
  Executor* executor = co_await current_executor();
  uint32_t remaining = count;
  for (uint32_t i = 0; i < count; ++i) {
    // If a helper cannot be spawned (out of memory) that task simply runs here, one after another.
    if (!executor->spawn(detail::run_and_count(&tasks[i], &remaining))) {
      co_await tasks[i];
      --remaining;
    }
  }
  co_await wait_until([&remaining] { return remaining == 0; });
}

// when_all for a few tasks written out: co_await async::when_all(a(), b(), c());
template <class... Rest>
Task<void> when_all(Task<void> first, Task<void> second, Rest... rest) {
  Task<void> tasks[] = {util::move(first), util::move(second), util::move(rest)...};
  co_await when_all(tasks, sizeof...(rest) + 2);
}

// A flag tasks can wait for, like C#'s ManualResetEventSlim/TaskCompletionSource<void>.
//   AsyncEvent ready;   ... co_await ready.wait();   ... ready.set();
class AsyncEvent {
 public:
  void set() { set_ = true; }
  void reset() { set_ = false; }
  bool is_set() const { return set_; }
  // co_await: true once set; with a timeout false if it passed first.
  auto wait(uint32_t timeout_ms = kNoTimeout) { return wait_until([this] { return set_; }, timeout_ms); }

 private:
  bool set_ = false;
};

// Mutual exclusion across awaits (tasks on one executor never run at the same time,
// but can interleave at each co_await while holding a resource). First come, first served.
//   co_await mutex.lock();  ... co_await something ...;  mutex.unlock();
class AsyncMutex {
 public:
  bool try_lock() {
    if (locked_) return false;
    locked_ = true;
    return true;
  }
  auto lock(uint32_t timeout_ms = kNoTimeout) { return wait_until([this] { return try_lock(); }, timeout_ms); }
  void unlock() { locked_ = false; }
  bool is_locked() const { return locked_; }

 private:
  bool locked_ = false;
};

}  // namespace async
