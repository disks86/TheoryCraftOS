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

// A thread of its own for the handlers of one control tree, run one at a time in the order they were posted. The desktop window has a single handler thread for
// everything in it (Window::start_handler_thread), so a handler that takes long - a web page that is fetched and whose scripts run - holds up every click behind it,
// the Start menu and the other applications included. The web browser posts its handlers here instead.
//
//  - the thread exists only while something is queued (nothing idles);
//  - the queue is bounded (kMaxQueued) and a handler that is already waiting can be posted again as a no-op (`coalesce`: timers, resizes, loads);
//  - without threads (kernel::running() is false: the tests) a posted handler runs at once in the caller, as a handler does in a window without a handler thread;
//  - stop() / the destructor drop what has not started and wait for the handler that is running (the caller must not hold a lock that handler needs).
#pragma once
#include "Gui/GuiTypes.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"

namespace desktop {

class HandlerThread {
 public:
  static constexpr uint64_t kMaxQueued = 256;

  explicit HandlerThread(allocators::IAllocator& allocator) : queue_(allocator) {}
  ~HandlerThread() { stop(); }
  HandlerThread(const HandlerThread&) = delete;
  HandlerThread& operator=(const HandlerThread&) = delete;

  void post(const gui::Handler& handler, gui::Control& sender, bool coalesce) {
    if (!handler) return;
    if (!kernel::running()) {
      handler(sender);
      return;
    }
    bool start = false;
    {
      kernel::ScopedMutex lock(mutex_);
      if (closed_) return;
      if (coalesce)
        for (const Item& item : queue_)
          if (item.handler.function == handler.function && item.handler.context == handler.context && item.sender == &sender) return;
      if (queue_.count() >= kMaxQueued || !queue_.push_back(Item{handler, &sender})) return;
      if (!active_) {
        active_ = true;
        __atomic_fetch_add(&threads_, 1u, __ATOMIC_ACQ_REL);
        start = true;
      }
    }
    if (!start) return;
    kernel::Thread* thread = kernel::spawn(&HandlerThread::thread_main, this, "browser-handlers");
    if (thread) {
      kernel::detach(thread);
      return;
    }
    {   // (no thread to be had: carry out what is queued here)
      kernel::ScopedMutex lock(mutex_);
      active_ = false;
    }
    __atomic_fetch_sub(&threads_, 1u, __ATOMIC_ACQ_REL);
    drain_here();
  }

  void stop() {
    {
      kernel::ScopedMutex lock(mutex_);
      closed_ = true;
      queue_.clear();
    }
    while (__atomic_load_n(&threads_, __ATOMIC_ACQUIRE) != 0) kernel::sleep_us(1000);
  }

 private:
  struct Item {
    gui::Handler handler;
    gui::Control* sender;
  };

  static void thread_main(void* self) { static_cast<HandlerThread*>(self)->run(); }

  bool next(Item* out) {
    kernel::ScopedMutex lock(mutex_);
    if (queue_.count() == 0 || closed_) {
      active_ = false;
      return false;
    }
    *out = queue_[0];
    queue_.remove_at(0);
    return true;
  }

  void run() {
    Item item;
    while (next(&item)) item.handler(*item.sender);   // (no lock held: this may take as long as it likes)
    __atomic_fetch_sub(&threads_, 1u, __ATOMIC_ACQ_REL);   // (after the unlock, and the last thing this thread does with `this`: stop() may free it now)
  }

  void drain_here() {
    Item item;
    while (next(&item)) item.handler(*item.sender);
  }

  kernel::Mutex mutex_;
  List<Item> queue_;
  bool active_ = false;   // a thread is going to carry out the queue (guarded by mutex_)
  bool closed_ = false;
  uint32_t threads_ = 0;  // threads started and not yet finished (atomic)
};

}  // namespace desktop
