#pragma once
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

// A mutual-exclusion lock for the cores of this machine. A waiting core spins,
// calling the CPU's pause() between looks so it does not hammer the cache line
// the lock lives on (and so a hyperthread sibling can run).
//
// Keep critical sections short and never wait on anything inside one. The lock
// is not re-entrant: taking it again on the core that holds it, e.g. by calling
// back into the same concurrent container from inside a callback, deadlocks.
// There is no fairness guarantee. Cooperative scheduling (no preemption) and the
// absence of interrupt handlers that take locks are what make spinning safe here.
class SpinLock {
 public:
  explicit SpinLock(const hardware::ICentralProcessingUnit& cpu) : cpu_(cpu) {}
  SpinLock(const SpinLock&) = delete;
  SpinLock& operator=(const SpinLock&) = delete;

  void lock() {
    while (__atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE)) {
      // Wait with plain loads until it looks free; only then retry the exchange.
      while (__atomic_load_n(&locked_, __ATOMIC_RELAXED)) cpu_.pause();
    }
  }
  bool try_lock() { return !__atomic_exchange_n(&locked_, 1, __ATOMIC_ACQUIRE); }
  void unlock() { __atomic_store_n(&locked_, 0, __ATOMIC_RELEASE); }

 private:
  const hardware::ICentralProcessingUnit& cpu_;
  uint32_t locked_ = 0;
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
