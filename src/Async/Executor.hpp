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
#include "Async/Task.hpp"
#include "Hardware/Clock/IClock.hpp"

namespace async {

constexpr uint32_t kNoTimeout = 0xFFFFFFFFu;

// Runs tasks cooperatively on one thread, like a single-threaded SynchronizationContext.
// A task runs until it awaits something that is not ready (delay, wait_until, a
// stream read with no data yet, ...); then the executor runs the others, and polls
// the waiting ones until their condition holds. Waiting costs a poll of the
// condition per pass, which suits this OS's polled network stack and storage.
// When nothing is ready the executor sleeps briefly instead of spinning.
//
//   async::Executor executor(clock);
//   executor.spawn(serve(stack));         // runs in the background
//   executor.spawn(report());
//   executor.run();                       // until every task has finished
//   int v = executor.block_on(compute()); // or run one task to its result
//
// An Executor is for ONE thread: call everything on it from that thread. Do not
// call block_on() or run() from inside a task. Destroying an executor destroys the
// tasks that have not finished.
class Executor {
 public:
  explicit Executor(hardware::IClock& clock) : clock_(clock) {}
  ~Executor();
  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  uint64_t now_us() const { return clock_.now_us(); }

  // Starts `task` in the background; it frees itself when it finishes. False if the
  // task is invalid (its frame could not be allocated).
  bool spawn(Task<void> task);

  // Runs until `task` has finished and returns its result, running every spawned
  // task and waiter meanwhile.
  template <class T>
  T block_on(Task<T> task);

  // Runs until no task is left (spawned tasks all finished and nothing is waiting).
  void run();
  // One pass over the pollers and the waiting tasks; true if any task made progress.
  bool run_once();
  // Calls `poll(context)` once per pass, before the waiters (up to 8 of these).
  bool add_poller(void (*poll)(void*), void* context);
  bool remove_poller(void (*poll)(void*), void* context);

  // Spawned tasks that have not finished.
  uint32_t active_tasks() const { return active_; }

  // --- for the awaiters
  void add_waiter(detail::Waiter* waiter);

 private:
  friend class detail::PromiseBase;
  static constexpr uint32_t kMaxPollers = 8;

  void run_until(bool (*done)(void*), void* context);
  void idle();
  void unlink_root(detail::PromiseBase* root);

  hardware::IClock& clock_;
  detail::Waiter* head_ = nullptr;
  detail::Waiter* tail_ = nullptr;
  detail::PromiseBase* roots_ = nullptr;
  uint32_t active_ = 0;
  struct Poller {
    void (*poll)(void*) = nullptr;
    void* context = nullptr;
  } pollers_[kMaxPollers];
};

template <class T>
T Executor::block_on(Task<T> task) {
  if (task.is_coroutine()) {
    typename Task<T>::Handle handle = task.handle();
    handle.promise().executor = this;
    detail::Waiter start;
    start.handle = handle;
    start.ready = [](detail::Waiter*) { return true; };
    add_waiter(&start);
    run_until([](void* h) { return std::coroutine_handle<>::from_address(h).done(); }, handle.address());
  }
  return task.take_result();
}

// ---- awaitables ---------------------------------------------------------------

namespace detail {

// co_await-able that polls `pred` (a callable returning bool) until it is true, or
// until the timeout passes. Evaluates to true if the condition held, false on timeout.
template <class F>
class WaitUntilAwaiter : private Waiter {
 public:
  WaitUntilAwaiter(F pred, uint32_t timeout_ms) : pred_(util::move(pred)), timeout_ms_(timeout_ms) {}

  bool await_ready() { return satisfied_ = pred_(); }  // already true: no suspension at all
  template <class P>
  void await_suspend(std::coroutine_handle<P> self) {
    static_assert(__is_base_of(PromiseBase, P), "wait_until can only be awaited from a Task");
    executor_ = self.promise().executor;
    if (!executor_) fatal("co_await outside an Executor");
    if (timeout_ms_ != kNoTimeout) deadline_us_ = executor_->now_us() + uint64_t{timeout_ms_} * 1000;
    handle = self;
    ready = &WaitUntilAwaiter::check;
    executor_->add_waiter(this);
  }
  bool await_resume() const { return satisfied_; }

 private:
  static bool check(Waiter* waiter) {
    auto* self = static_cast<WaitUntilAwaiter*>(waiter);
    if (self->pred_()) {
      self->satisfied_ = true;
      return true;
    }
    if (self->timeout_ms_ != kNoTimeout && self->executor_->now_us() >= self->deadline_us_) {
      self->satisfied_ = false;
      return true;
    }
    return false;
  }

  F pred_;
  uint32_t timeout_ms_;
  Executor* executor_ = nullptr;
  uint64_t deadline_us_ = 0;
  bool satisfied_ = false;
};

class YieldAwaiter : private Waiter {
 public:
  bool await_ready() const { return false; }
  template <class P>
  void await_suspend(std::coroutine_handle<P> self) {
    static_assert(__is_base_of(PromiseBase, P), "yield_now can only be awaited from a Task");
    if (!self.promise().executor) fatal("co_await outside an Executor");
    handle = self;
    ready = [](Waiter*) { return true; };
    self.promise().executor->add_waiter(this);
  }
  void await_resume() const {}
};

class ExecutorAwaiter {
 public:
  bool await_ready() const { return false; }
  template <class P>
  bool await_suspend(std::coroutine_handle<P> self) {  // never really suspends
    executor_ = self.promise().executor;
    return false;
  }
  Executor* await_resume() const { return executor_; }

 private:
  Executor* executor_ = nullptr;
};

}  // namespace detail

// Waits until `pred()` returns true (it is polled once per executor pass, and tried
// immediately first). With a timeout in milliseconds the result tells which happened:
// true = the condition held, false = timed out. `pred` may have side effects (a
// "try to send" that succeeds when there is room) but must not block.
template <class F>
detail::WaitUntilAwaiter<F> wait_until(F pred, uint32_t timeout_ms = kNoTimeout) {
  return detail::WaitUntilAwaiter<F>(util::move(pred), timeout_ms);
}

// Lets the other tasks run once, then continues.
inline detail::YieldAwaiter yield_now() { return {}; }

// Suspends for at least `milliseconds`.
inline auto delay(uint32_t milliseconds) {
  return wait_until([] { return false; }, milliseconds);
}

// The executor this task runs on.
inline detail::ExecutorAwaiter current_executor() { return {}; }

}  // namespace async
