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
#include "Kernel/Interrupts.hpp"

namespace kernel {

// A spinlock with no dependencies, for the lowest layers (config-space access,
// console output). Interrupts are masked while it is held, so it is safe to take
// from any context, including with other locks held; keep the critical section short.
class RawSpinLock {
 public:
  constexpr RawSpinLock() = default;
  RawSpinLock(const RawSpinLock&) = delete;
  RawSpinLock& operator=(const RawSpinLock&) = delete;

  void lock() {
    const uint64_t flags = irq_save();
    while (__atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE)) {
      while (__atomic_load_n(&locked_, __ATOMIC_RELAXED)) __asm__ volatile("pause");
    }
    saved_flags_ = flags;
  }
  void unlock() {
    const uint64_t flags = saved_flags_;
    __atomic_store_n(&locked_, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
  }

 // Forcibly releases the lock (panic paths only).
  void reset() { __atomic_store_n(&locked_, 0, __ATOMIC_RELEASE); }

 private:
  uint32_t locked_ = 0;
  uint64_t saved_flags_ = 0;
};

class RawSpinGuard {
 public:
  explicit RawSpinGuard(RawSpinLock& lock) : lock_(lock) { lock_.lock(); }
  ~RawSpinGuard() { lock_.unlock(); }
  RawSpinGuard(const RawSpinGuard&) = delete;
  RawSpinGuard& operator=(const RawSpinGuard&) = delete;

 private:
  RawSpinLock& lock_;
};

}  // namespace kernel
