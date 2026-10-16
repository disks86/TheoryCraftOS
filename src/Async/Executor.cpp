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

#include "Async/Executor.hpp"
#include "Kernel/Interrupts.hpp"
#include "Kernel/Thread.hpp"

namespace async {

namespace {
allocators::IAllocator* g_frame_allocator = nullptr;
}

void set_frame_allocator(allocators::IAllocator& allocator) { g_frame_allocator = &allocator; }

namespace detail {

void* allocate_frame(uint64_t size) noexcept { return g_frame_allocator ? g_frame_allocator->allocate(size) : nullptr; }
void free_frame(void* frame) noexcept {
  if (g_frame_allocator) g_frame_allocator->deallocate(frame);
}
void fatal(const char* what) { kernel::panic(what, nullptr); }

void PromiseBase::detached_finished() {
  if (executor) executor->unlink_root(this);
}

}  // namespace detail

Executor::~Executor() {
  head_ = tail_ = nullptr;  // the waiters live in the frames destroyed below
  while (roots_) {
    detail::PromiseBase* root = roots_;
    roots_ = root->root_next;
    root->detached = false;  // being destroyed from outside: no self-unlinking
    std::coroutine_handle<>::from_address(root->start.handle.address()).destroy();
  }
}

bool Executor::spawn(Task<void> task) {
  if (!task.valid()) return false;
  if (!task.is_coroutine()) return true;  // already complete
  Task<void>::Handle handle = task.release();
  detail::PromiseBase& root = handle.promise();
  root.executor = this;
  root.detached = true;
  root.root_prev = nullptr;
  root.root_next = roots_;
  if (roots_) roots_->root_prev = &root;
  roots_ = &root;
  ++active_;
  root.start.handle = handle;
  root.start.ready = [](detail::Waiter*) { return true; };
  add_waiter(&root.start);
  return true;
}

void Executor::unlink_root(detail::PromiseBase* root) {
  if (root->root_prev) root->root_prev->root_next = root->root_next;
  else roots_ = root->root_next;
  if (root->root_next) root->root_next->root_prev = root->root_prev;
  root->root_prev = root->root_next = nullptr;
  --active_;
}

void Executor::add_waiter(detail::Waiter* waiter) {
  waiter->next = nullptr;
  if (tail_) tail_->next = waiter;
  else head_ = waiter;
  tail_ = waiter;
}

bool Executor::add_poller(void (*poll)(void*), void* context) {
  for (Poller& p : pollers_)
    if (!p.poll) {
      p.poll = poll;
      p.context = context;
      return true;
    }
  return false;
}

bool Executor::remove_poller(void (*poll)(void*), void* context) {
  for (Poller& p : pollers_)
    if (p.poll == poll && p.context == context) {
      p.poll = nullptr;
      return true;
    }
  return false;
}

bool Executor::run_once() {
  for (Poller& p : pollers_)
    if (p.poll) p.poll(p.context);
  detail::Waiter* pending = head_;  // this pass's waiters; anything added meanwhile waits for the next pass
  head_ = tail_ = nullptr;
  bool progressed = false;
  while (pending) {
    detail::Waiter* waiter = pending;
    pending = waiter->next;
    waiter->next = nullptr;
    if (waiter->ready(waiter)) {
      progressed = true;
      std::coroutine_handle<> handle = waiter->handle;  // the waiter dies with the frame once resumed
      handle.resume();
    } else {
      add_waiter(waiter);
    }
  }
  return progressed;
}

void Executor::idle() { kernel::sleep_us(200); }

void Executor::run_until(bool (*done)(void*), void* context) {
  while (!done(context)) {
    if (!run_once()) idle();
  }
}

void Executor::run() {
  while (active_ != 0 || head_ != nullptr) {
    if (!run_once()) idle();
  }
}

}  // namespace async
