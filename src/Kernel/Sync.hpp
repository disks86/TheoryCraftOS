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
#include "Kernel/Thread.hpp"

// Blocking synchronization between threads: a waiting thread gives up its core
// instead of spinning. (Use SpinLock for critical sections of a few instructions;
// use these when waiting may take a while - device I/O, other threads' work.)
// Before kernel::start() they degrade to spinning, so early code may use them too.
namespace kernel {

class Mutex {
 public:
  Mutex() = default;
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  void lock();
  bool try_lock();
  void unlock();

 private:
  uint32_t state_ = 0;    // 1 while held
  uint32_t waiters_ = 0;  // threads in, or about to be in, list_
  WaitList list_;
  friend class ConditionVariable;
};

class ScopedMutex {
 public:
  explicit ScopedMutex(Mutex& mutex) : mutex_(mutex) { mutex_.lock(); }
  ~ScopedMutex() { mutex_.unlock(); }
  ScopedMutex(const ScopedMutex&) = delete;
  ScopedMutex& operator=(const ScopedMutex&) = delete;

 private:
  Mutex& mutex_;
};

// A mutex the owning thread may take again (each lock() needs its unlock()).
class RecursiveMutex {
 public:
  RecursiveMutex() = default;
  RecursiveMutex(const RecursiveMutex&) = delete;
  RecursiveMutex& operator=(const RecursiveMutex&) = delete;

  void lock() {
    Thread* me = current_thread();
    if (depth_ > 0 && owner_ == me) {
      ++depth_;
      return;
    }
    mutex_.lock();
    owner_ = me;
    depth_ = 1;
  }
  void unlock() {
    if (--depth_ == 0) {
      owner_ = nullptr;
      mutex_.unlock();
    }
  }
  // True if the calling thread holds it.
  bool held_by_me() const { return depth_ > 0 && owner_ == current_thread(); }

 private:
  Mutex mutex_;
  Thread* owner_ = nullptr;
  uint32_t depth_ = 0;
};

class ScopedRecursiveMutex {
 public:
  explicit ScopedRecursiveMutex(RecursiveMutex& mutex) : mutex_(mutex) { mutex_.lock(); }
  ~ScopedRecursiveMutex() { mutex_.unlock(); }
  ScopedRecursiveMutex(const ScopedRecursiveMutex&) = delete;
  ScopedRecursiveMutex& operator=(const ScopedRecursiveMutex&) = delete;

 private:
  RecursiveMutex& mutex_;
};

class Semaphore {
 public:
  explicit Semaphore(uint32_t initial = 0) : count_(initial) {}
  Semaphore(const Semaphore&) = delete;
  Semaphore& operator=(const Semaphore&) = delete;

  void acquire();
  bool try_acquire();
  // Waits at most timeout_us (0 = forever). False on timeout.
  bool acquire_for(uint64_t timeout_us);
  void release(uint32_t count = 1);

 private:
  uint32_t count_;
  WaitList list_;
};

// A flag threads can wait for. Manual-reset events stay set until reset() and
// release every waiter; auto-reset events release one waiter per set().
class Event {
 public:
  explicit Event(bool auto_reset = false) : auto_reset_(auto_reset) {}
  Event(const Event&) = delete;
  Event& operator=(const Event&) = delete;

  void set();
  void reset();
  bool is_set() const { return __atomic_load_n(&signaled_, __ATOMIC_ACQUIRE); }
  bool wait(uint64_t timeout_us = 0);

 private:
  bool signaled_ = false;
  bool auto_reset_;
  WaitList list_;
};

class ConditionVariable {
 public:
  ConditionVariable() = default;
  ConditionVariable(const ConditionVariable&) = delete;
  ConditionVariable& operator=(const ConditionVariable&) = delete;

  // Releases `mutex`, waits for a notification (or the timeout, 0 = forever) and
  // takes the mutex again. May wake spuriously: recheck the condition. False on timeout.
  bool wait(Mutex& mutex, uint64_t timeout_us = 0);
  void notify_one();
  void notify_all();

 private:
  WaitList list_;
};

}  // namespace kernel
