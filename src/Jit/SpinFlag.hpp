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

namespace jit {

// A minimal mutual-exclusion flag for the JIT's bookkeeping (cache writers, the code pool): held for a few loads and stores, never across a compile and never
// from an interrupt handler. A plain flag rather than SpinLock because SpinLock masks interrupts, which hosted tests cannot do, and these paths do not need it.
class SpinFlag {
 public:
  void lock() {
    while (__atomic_exchange_n(&flag_, 1, __ATOMIC_ACQUIRE)) kernel::cpu_relax();
  }
  void unlock() { __atomic_store_n(&flag_, 0, __ATOMIC_RELEASE); }

 private:
  uint32_t flag_ = 0;
};

class SpinFlagGuard {
 public:
  explicit SpinFlagGuard(SpinFlag& flag) : flag_(flag) { flag_.lock(); }
  ~SpinFlagGuard() { flag_.unlock(); }
  SpinFlagGuard(const SpinFlagGuard&) = delete;
  SpinFlagGuard& operator=(const SpinFlagGuard&) = delete;

 private:
  SpinFlag& flag_;
};

}  // namespace jit
