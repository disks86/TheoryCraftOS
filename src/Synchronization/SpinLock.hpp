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
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "Kernel/Interrupts.hpp"

// A mutual-exclusion lock for the cores of this machine. A waiting core spins,
// calling the CPU's pause() between looks so it does not hammer the cache line
// the lock lives on (and so a hyperthread sibling can run).
//
// Keep critical sections short and never wait on anything inside one. The lock
// is not re-entrant: taking it again on the core that holds it, e.g. by calling
// back into the same concurrent container from inside a callback, deadlocks.
// There is no fairness guarantee. Masking interrupts while held is what
// makes spinning safe with preemptive threads.
class SpinLock {
 public:
  explicit SpinLock(const hardware::ICentralProcessingUnit& cpu) : cpu_(cpu) {}
  SpinLock(const SpinLock&) = delete;
  SpinLock& operator=(const SpinLock&) = delete;

  // Interrupts are masked while the lock is held, so a thread cannot be preempted
  // inside a critical section (another thread on this core would then spin on the
  // lock for its whole time slice) and an interrupt handler can never find the
  // lock held by the code it interrupted.
  void lock() {
    const uint64_t flags = kernel::irq_save();
    while (__atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE)) {
      // Wait with plain loads until it looks free; only then retry the exchange.
      while (__atomic_load_n(&locked_, __ATOMIC_RELAXED)) cpu_.pause();
    }
    saved_flags_ = flags;
  }
  bool try_lock() {
    const uint64_t flags = kernel::irq_save();
    if (__atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE)) {
      kernel::irq_restore(flags);
      return false;
    }
    saved_flags_ = flags;
    return true;
  }
  void unlock() {
    const uint64_t flags = saved_flags_;
    __atomic_store_n(&locked_, 0, __ATOMIC_RELEASE);
    kernel::irq_restore(flags);
  }

 private:
  const hardware::ICentralProcessingUnit& cpu_;
  uint32_t locked_ = 0;
  uint64_t saved_flags_ = 0;  // interrupt state of the holder, restored on unlock
};

// Holds a SpinLock for the lifetime of the object.
class ScopedLock {
 public:
  explicit ScopedLock(SpinLock& lock) : lock_(lock) { lock_.lock(); }
  ~ScopedLock() { lock_.unlock(); }
  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;

 private:
  SpinLock& lock_;
};
