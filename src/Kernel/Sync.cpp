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

#include "Kernel/Sync.hpp"
#include "Kernel/Delay.hpp"

namespace kernel {

namespace {
inline void pause_hint() { kernel::cpu_relax(); }

// Deadlines are kept as timestamp-counter values; waits take microseconds.
uint64_t deadline_tsc(uint64_t timeout_us) { return hardware::read_tsc() + timeout_us * hardware::tsc_ticks_per_us(); }
uint64_t microseconds_until(uint64_t deadline, bool* expired) {
  const int64_t left = static_cast<int64_t>(deadline - hardware::read_tsc());
  *expired = left <= 0;
  return left > 0 ? static_cast<uint64_t>(left) / hardware::tsc_ticks_per_us() + 1 : 0;
}
}  // namespace

// ---------------------------------------------------------------- Mutex

bool Mutex::try_lock() {
  uint32_t expected = 0;
  return __atomic_compare_exchange_n(&state_, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void Mutex::lock() {
  if (try_lock()) return;
  if (!running()) {  // no scheduler yet: nobody to hand the core to
    while (!try_lock()) pause_hint();
    return;
  }
  for (;;) {
    const uint64_t flags = detail::lock();
    // Announce the wait before the last attempt: an unlock that follows a failed
    // attempt then always sees us and wakes someone (no lost wakeup).
    __atomic_add_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);
    if (try_lock()) {
      __atomic_sub_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);
      detail::unlock(flags);
      return;
    }
    detail::block_current_locked(&list_, 0);
    detail::unlock(flags);
    yield();
    __atomic_sub_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);
  }
}

void Mutex::unlock() {
  __atomic_exchange_n(&state_, 0, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&waiters_, __ATOMIC_SEQ_CST) == 0) return;
  const uint64_t flags = detail::lock();
  detail::wake_one_locked(&list_);
  detail::unlock(flags);
}

// ---------------------------------------------------------------- Semaphore

bool Semaphore::try_acquire() {
  const uint64_t flags = detail::lock();
  const bool ok = count_ > 0;
  if (ok) --count_;
  detail::unlock(flags);
  return ok;
}

bool Semaphore::acquire_for(uint64_t timeout_us) {
  if (!running()) {  // spin until another context releases
    while (!try_acquire()) pause_hint();
    return true;
  }
  const uint64_t deadline = timeout_us ? deadline_tsc(timeout_us) : 0;
  for (;;) {
    const uint64_t flags = detail::lock();
    if (count_ > 0) {
      --count_;
      detail::unlock(flags);
      return true;
    }
    uint64_t remaining = 0;
    if (timeout_us) {
      bool expired;
      remaining = microseconds_until(deadline, &expired);
      if (expired) {
        detail::unlock(flags);
        return false;
      }
    }
    detail::block_current_locked(&list_, remaining);
    detail::unlock(flags);
    yield();
  }
}

void Semaphore::acquire() { acquire_for(0); }

void Semaphore::release(uint32_t count) {
  const uint64_t flags = detail::lock();
  count_ += count;
  for (uint32_t i = 0; i < count; ++i)
    if (!detail::wake_one_locked(&list_)) break;
  detail::unlock(flags);
}

// ---------------------------------------------------------------- Event

void Event::set() {
  const uint64_t flags = detail::lock();
  if (auto_reset_) {
    if (!detail::wake_one_locked(&list_)) __atomic_store_n(&signaled_, true, __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&signaled_, true, __ATOMIC_RELEASE);
    detail::wake_all_locked(&list_);
  }
  detail::unlock(flags);
}

void Event::reset() { __atomic_store_n(&signaled_, false, __ATOMIC_RELEASE); }

bool Event::wait(uint64_t timeout_us) {
  if (!running()) {
    while (!__atomic_load_n(&signaled_, __ATOMIC_ACQUIRE)) pause_hint();
    if (auto_reset_) __atomic_store_n(&signaled_, false, __ATOMIC_RELEASE);
    return true;
  }
  const uint64_t deadline = timeout_us ? deadline_tsc(timeout_us) : 0;
  for (;;) {
    const uint64_t flags = detail::lock();
    if (signaled_) {
      if (auto_reset_) signaled_ = false;
      detail::unlock(flags);
      return true;
    }
    uint64_t remaining = 0;
    if (timeout_us) {
      bool expired;
      remaining = microseconds_until(deadline, &expired);
      if (expired) {
        detail::unlock(flags);
        return false;
      }
    }
    detail::block_current_locked(&list_, remaining);
    detail::unlock(flags);
    yield();
    // An auto-reset set() that found us waiting consumed itself on our behalf.
    if (auto_reset_ && !current_thread()->timed_out) return true;
  }
}

// ---------------------------------------------------------------- ConditionVariable

bool ConditionVariable::wait(Mutex& mutex, uint64_t timeout_us) {
  const uint64_t flags = detail::lock();
  detail::block_current_locked(&list_, timeout_us);  // enqueued before the mutex is released: no missed notify
  // The mutex is let go while the scheduler's lock is still held (interrupts are off): a thread that is marked blocked but has not given up the mutex yet must not be
  // switched away from, or the mutex would stay held until somebody wakes it - and the ones that would wake it may be waiting for that mutex.
  __atomic_exchange_n(&mutex.state_, 0, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&mutex.waiters_, __ATOMIC_SEQ_CST) != 0) detail::wake_one_locked(&mutex.list_);
  detail::unlock(flags);
  yield();
  const bool timed_out = current_thread()->timed_out;
  mutex.lock();
  return !timed_out;
}

void ConditionVariable::notify_one() {
  const uint64_t flags = detail::lock();
  detail::wake_one_locked(&list_);
  detail::unlock(flags);
}

void ConditionVariable::notify_all() {
  const uint64_t flags = detail::lock();
  detail::wake_all_locked(&list_);
  detail::unlock(flags);
}

}  // namespace kernel
