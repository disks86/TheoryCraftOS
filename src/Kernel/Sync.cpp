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

// Each object's wait list carries its own lock, which also guards the object's state; blocking and waking follow docs/SchedulerLocking.md:
// check under the list lock, prepare_wait, unlock, yield, finish_wait.
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
    const uint64_t flags = detail::lock_list(&list_);
    // Announce the wait before the last attempt: an unlock that follows a failed
    // attempt then always sees us and wakes someone (no lost wakeup).
    __atomic_add_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);
    if (try_lock()) {
      __atomic_sub_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);
      detail::unlock_list(&list_, flags);
      return;
    }
    detail::prepare_wait(&list_, 0);
    detail::unlock_list(&list_, flags);
    yield();
    detail::finish_wait(&list_);
    __atomic_sub_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);
    if (fair_ && __atomic_exchange_n(&handoff_, 0u, __ATOMIC_ACQUIRE)) return;   // handed over: state_ stayed 1, for this thread
  }
}

void Mutex::unlock() {
  if (fair_ && running()) {
    // With someone waiting, the mutex stays held and goes to the oldest waiter (wake_one wakes the oldest): nobody can take it in between.
    // The list's lock orders this against a thread about to wait (it announces itself and queues under the same lock).
    const uint64_t flags = detail::lock_list(&list_);
    if (__atomic_load_n(&waiters_, __ATOMIC_SEQ_CST) != 0) {
      __atomic_store_n(&handoff_, 1u, __ATOMIC_RELEASE);
      if (detail::wake_one(&list_)) {
        detail::unlock_list(&list_, flags);
        return;
      }
      __atomic_store_n(&handoff_, 0u, __ATOMIC_RELEASE);
    }
    __atomic_exchange_n(&state_, 0, __ATOMIC_SEQ_CST);
    detail::unlock_list(&list_, flags);
    return;
  }
  __atomic_exchange_n(&state_, 0, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&waiters_, __ATOMIC_SEQ_CST) == 0) return;
  const uint64_t flags = detail::lock_list(&list_);
  detail::wake_one(&list_);
  detail::unlock_list(&list_, flags);
}

// ---------------------------------------------------------------- Semaphore

bool Semaphore::try_acquire() {
  const uint64_t flags = detail::lock_list(&list_);
  const bool ok = count_ > 0;
  if (ok) --count_;
  detail::unlock_list(&list_, flags);
  return ok;
}

bool Semaphore::acquire_for(uint64_t timeout_us) {
  if (!running()) {  // spin until another context releases
    while (!try_acquire()) pause_hint();
    return true;
  }
  const uint64_t deadline = timeout_us ? deadline_tsc(timeout_us) : 0;
  for (;;) {
    const uint64_t flags = detail::lock_list(&list_);
    if (count_ > 0) {
      --count_;
      detail::unlock_list(&list_, flags);
      return true;
    }
    uint64_t remaining = 0;
    if (timeout_us) {
      bool expired;
      remaining = microseconds_until(deadline, &expired);
      if (expired) {
        detail::unlock_list(&list_, flags);
        return false;
      }
    }
    detail::prepare_wait(&list_, remaining);
    detail::unlock_list(&list_, flags);
    yield();
    detail::finish_wait(&list_);
  }
}

void Semaphore::acquire() { acquire_for(0); }

void Semaphore::release(uint32_t count) {
  const uint64_t flags = detail::lock_list(&list_);
  count_ += count;
  for (uint32_t i = 0; i < count; ++i)
    if (!detail::wake_one(&list_)) break;
  detail::unlock_list(&list_, flags);
}

// ---------------------------------------------------------------- Event

void Event::set() {
  const uint64_t flags = detail::lock_list(&list_);
  if (auto_reset_) {
    if (!detail::wake_one(&list_)) __atomic_store_n(&signaled_, true, __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&signaled_, true, __ATOMIC_RELEASE);
    detail::wake_all(&list_);
  }
  detail::unlock_list(&list_, flags);
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
    const uint64_t flags = detail::lock_list(&list_);
    if (signaled_) {
      if (auto_reset_) signaled_ = false;
      detail::unlock_list(&list_, flags);
      return true;
    }
    uint64_t remaining = 0;
    if (timeout_us) {
      bool expired;
      remaining = microseconds_until(deadline, &expired);
      if (expired) {
        detail::unlock_list(&list_, flags);
        return false;
      }
    }
    detail::prepare_wait(&list_, remaining);
    detail::unlock_list(&list_, flags);
    yield();
    // An auto-reset set() that found us waiting consumed itself on our behalf (a set() that came after our timeout passed us over).
    if (!detail::finish_wait(&list_) && auto_reset_) return true;
  }
}

// ---------------------------------------------------------------- ConditionVariable

bool ConditionVariable::wait(Mutex& mutex, uint64_t timeout_us) {
  const uint64_t flags = detail::lock_list(&list_);
  detail::prepare_wait(&list_, timeout_us);  // enqueued before the mutex is released: no missed notify
  // The mutex is let go while this list's lock is still held (interrupts are off): a thread that is marked blocked but has not given up the mutex
  // yet must not be switched away from, or the mutex would stay held until somebody wakes it - and the ones that would wake it may be waiting for
  // that mutex. (The mutex's list lock after this one's: the one place two wait-list locks are held, always in this order.)
  __atomic_exchange_n(&mutex.state_, 0, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&mutex.waiters_, __ATOMIC_SEQ_CST) != 0) {
    const uint64_t inner = detail::lock_list(&mutex.list_);
    detail::wake_one(&mutex.list_);
    detail::unlock_list(&mutex.list_, inner);
  }
  detail::unlock_list(&list_, flags);
  yield();
  const bool timed_out = detail::finish_wait(&list_);
  mutex.lock();
  return !timed_out;
}

void ConditionVariable::notify_one() {
  const uint64_t flags = detail::lock_list(&list_);
  detail::wake_one(&list_);
  detail::unlock_list(&list_, flags);
}

void ConditionVariable::notify_all() {
  const uint64_t flags = detail::lock_list(&list_);
  detail::wake_all(&list_);
  detail::unlock_list(&list_, flags);
}

}  // namespace kernel
