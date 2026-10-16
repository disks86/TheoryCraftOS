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
#include <coroutine>
#include <new>
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/Move.hpp"
#include "Utility/Optional.hpp"

// Async/await for the kernel: C++20 coroutines in the shape of C#'s Task.
//
//   async::Task<uint32_t> add_later(uint32_t a, uint32_t b) {
//     co_await async::delay(10);          // other tasks run meanwhile
//     co_return a + b;
//   }
//   async::Task<void> caller() {
//     uint32_t sum = co_await add_later(1, 2);
//   }
//   executor.block_on(caller());          // or executor.spawn(caller())
//
// A Task is lazy: nothing runs until it is awaited, block_on()ed or spawn()ed. It
// is move-only and owns its coroutine; destroying an unfinished, unstarted Task just
// frees it. Everything runs on one Executor thread (see Executor.hpp): tasks take
// turns at each co_await, so they need no locks against each other.
//
// There are no exceptions: a failed coroutine-frame allocation makes the Task
// "invalid" and awaiting it gives FailedResult<T>::value() (T{} unless specialized;
// stream results specialize it to OutOfMemory). Frames come from the allocator
// given to set_frame_allocator() - call that once at startup.
namespace async {

class Executor;

// What awaiting a Task whose coroutine could not be allocated yields.
template <class T>
struct FailedResult {
  static T value() { return T(); }
};

void set_frame_allocator(allocators::IAllocator& allocator);

namespace detail {

void* allocate_frame(uint64_t size) noexcept;
void free_frame(void* frame) noexcept;
[[noreturn]] void fatal(const char* what);

// A suspended coroutine that waits for something. Executors keep these in a list
// and resume `handle` once ready(this) is true. The Waiter lives inside the awaiter,
// i.e. inside the suspended coroutine's frame: waiting needs no allocation.
struct Waiter {
  Waiter* next = nullptr;
  std::coroutine_handle<> handle;
  bool (*ready)(Waiter*) = nullptr;
};

class PromiseBase {
 public:
  static void* operator new(size_t size) noexcept { return allocate_frame(size); }
  static void operator delete(void* frame) noexcept { free_frame(frame); }

  std::suspend_always initial_suspend() noexcept { return {}; }
  void unhandled_exception() noexcept { __builtin_trap(); }  // exceptions are off; unreachable

  Executor* executor = nullptr;            // the executor this task runs on (set when awaited / spawned)
  std::coroutine_handle<> continuation;    // the coroutine awaiting this task
  bool detached = false;                   // a spawned root: destroys itself when done
  PromiseBase* root_prev = nullptr;        // the executor's list of spawned roots
  PromiseBase* root_next = nullptr;
  Waiter start;                            // how a spawned root first gets resumed

  void detached_finished();                // unlinks from the executor (Executor.cpp)
};

struct FinalAwaiter {
  bool await_ready() noexcept { return false; }
  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> self) noexcept {
    PromiseBase& promise = self.promise();
    if (promise.detached) {
      promise.detached_finished();
      self.destroy();  // legal here: nothing touches the frame afterwards
      return std::noop_coroutine();
    }
    return promise.continuation ? promise.continuation : std::noop_coroutine();
  }
  void await_resume() noexcept {}
};

template <class T>
class Promise;

}  // namespace detail

template <class T>
class [[nodiscard]] Task {
 public:
  using promise_type = detail::Promise<T>;
  using Handle = std::coroutine_handle<promise_type>;

  Task() = default;
  Task(Task&& other) : handle_(other.handle_), immediate_(util::move(other.immediate_)) { other.handle_ = nullptr; }
  Task& operator=(Task&& other) {
    if (this != &other) {
      destroy();
      handle_ = other.handle_;
      immediate_ = util::move(other.immediate_);
      other.handle_ = nullptr;
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() { destroy(); }

  // A task that is already finished with `value` (no coroutine, no allocation).
  static Task from_value(T value) {
    Task task;
    task.immediate_ = util::move(value);
    return task;
  }

  // False for a task whose coroutine frame could not be allocated.
  bool valid() const { return handle_ || immediate_.has_value(); }
  // Finished (or already complete when created).
  bool done() const { return handle_ ? handle_.done() : true; }

  // --- for Executor: not part of the application interface
  bool is_coroutine() const { return static_cast<bool>(handle_); }
  Handle handle() const { return handle_; }
  Handle release() {
    Handle h = handle_;
    handle_ = nullptr;
    return h;
  }
  T take_result() {
    if (handle_) return handle_.promise().take();
    if (immediate_.has_value()) return util::move(*immediate_);
    return FailedResult<T>::value();
  }

  auto operator co_await() & { return Awaiter{*this}; }
  auto operator co_await() && { return Awaiter{*this}; }

 private:
  friend class detail::Promise<T>;
  explicit Task(Handle h) : handle_(h) {}
  void destroy() {
    if (handle_) handle_.destroy();
    handle_ = nullptr;
  }

  struct Awaiter {
    Task& task;
    bool await_ready() const { return !task.handle_ || task.handle_.done(); }
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) {
      static_assert(__is_base_of(detail::PromiseBase, P), "tasks can only be awaited from other tasks");
      task.handle_.promise().executor = parent.promise().executor;
      task.handle_.promise().continuation = parent;
      return task.handle_;  // run the child now, on this stack
    }
    T await_resume() { return task.take_result(); }
  };

  Handle handle_;
  Optional<T> immediate_;
};

template <>
class [[nodiscard]] Task<void> {
 public:
  using promise_type = detail::Promise<void>;
  using Handle = std::coroutine_handle<promise_type>;

  Task() = default;
  Task(Task&& other) : handle_(other.handle_), immediate_(other.immediate_) { other.handle_ = nullptr; other.immediate_ = false; }
  Task& operator=(Task&& other) {
    if (this != &other) {
      destroy();
      handle_ = other.handle_;
      immediate_ = other.immediate_;
      other.handle_ = nullptr;
      other.immediate_ = false;
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() { destroy(); }

  static Task completed() {
    Task task;
    task.immediate_ = true;
    return task;
  }

  bool valid() const { return handle_ || immediate_; }
  bool done() const { return handle_ ? handle_.done() : true; }

  bool is_coroutine() const { return static_cast<bool>(handle_); }
  Handle handle() const { return handle_; }
  Handle release() {
    Handle h = handle_;
    handle_ = nullptr;
    return h;
  }
  void take_result() {}

  auto operator co_await() & { return Awaiter{*this}; }
  auto operator co_await() && { return Awaiter{*this}; }

 private:
  friend class detail::Promise<void>;
  explicit Task(Handle h) : handle_(h) {}
  void destroy() {
    if (handle_) handle_.destroy();
    handle_ = nullptr;
  }

  struct Awaiter {
    Task& task;
    bool await_ready() const { return !task.handle_ || task.handle_.done(); }
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent);  // below, once the promise is complete
    void await_resume() {}
  };

  Handle handle_;
  bool immediate_ = false;
};

namespace detail {

template <class T>
class Promise : public PromiseBase {
 public:
  Task<T> get_return_object() { return Task<T>(std::coroutine_handle<Promise>::from_promise(*this)); }
  static Task<T> get_return_object_on_allocation_failure() noexcept { return Task<T>(); }
  FinalAwaiter final_suspend() noexcept { return {}; }
  template <class U>
  void return_value(U&& value) {
    value_ = T(static_cast<U&&>(value));
  }
  T take() { return util::move(*value_); }

 private:
  Optional<T> value_;
};

template <>
class Promise<void> : public PromiseBase {
 public:
  Task<void> get_return_object() { return Task<void>(std::coroutine_handle<Promise>::from_promise(*this)); }
  static Task<void> get_return_object_on_allocation_failure() noexcept { return Task<void>(); }
  FinalAwaiter final_suspend() noexcept { return {}; }
  void return_void() {}
  void take() {}
};

}  // namespace detail

template <class P>
std::coroutine_handle<> Task<void>::Awaiter::await_suspend(std::coroutine_handle<P> parent) {
  static_assert(__is_base_of(detail::PromiseBase, P), "tasks can only be awaited from other tasks");
  task.handle_.promise().executor = parent.promise().executor;
  task.handle_.promise().continuation = parent;
  return task.handle_;  // run the child now, on this stack
}

}  // namespace async
