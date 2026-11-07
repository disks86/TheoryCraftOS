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

// Real threads for the host tests: implements the kernel's threading API (threads, mutexes, events,
// semaphores) on top of pthreads, so code written against Kernel/Thread.hpp and Kernel/Sync.hpp can be run
// - and checked with ThreadSanitizer - on the development machine. Waits poll with short sleeps: simple, and
// correct if not fast.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // (pthread_getattr_np)
#endif
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "Kernel/Sync.hpp"

namespace {
uint64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t{static_cast<uint64_t>(ts.tv_sec)} * 1000000 + static_cast<uint64_t>(ts.tv_nsec) / 1000;
}

void nap() {
  timespec ts{0, 50000};
  nanosleep(&ts, nullptr);
}

thread_local kernel::Thread t_self;  // every thread (the main one and test-created ones too) has its own identity
thread_local kernel::Thread* t_current = nullptr;

struct HostThread {
  kernel::Thread base;  // first: a Thread* is a HostThread*
  pthread_t handle;
};

void* trampoline(void* argument) {
  auto* t = static_cast<HostThread*>(argument);
  t_current = &t->base;
  pthread_attr_t attr;   // (where its stack is, as the kernel's threads know theirs: code that asks how much stack is left gets an answer)
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* low = nullptr;
    size_t size = 0;
    if (pthread_attr_getstack(&attr, &low, &size) == 0) {
      t->base.stack = static_cast<uint8_t*>(low);
      t->base.stack_top = static_cast<uint8_t*>(low) + size;
    }
    pthread_attr_destroy(&attr);
  }
  t->base.entry(t->base.argument);
  return nullptr;
}
}  // namespace

// A bounds violation is a failed test.
void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

namespace kernel {

bool running() { return true; }

namespace detail {
// The wait-list lock as a plain spinlock (the Mutex, Event... here do not use wait lists; code that borrows the lock for itself does).
uint64_t lock_list(WaitList* list) {
  while (__atomic_exchange_n(&list->lock, 1u, __ATOMIC_ACQUIRE)) sched_yield();
  return 0;
}
void unlock_list(WaitList* list, uint64_t) { __atomic_store_n(&list->lock, 0u, __ATOMIC_RELEASE); }
}  // namespace detail
Thread* current_thread() { return t_current ? t_current : &t_self; }
uint32_t current_core() { return 0; }
uint32_t core_count() { return 4; }
// Device interrupts: none on the host (drivers that want them fall back to polling).
uint64_t online_core_mask() { return 0xF; }
uint64_t core_hardware_id(uint32_t) { return ~uint64_t{0}; }
uint32_t choose_io_interrupt_core(int8_t, int64_t) { return 0; }
int32_t register_io_device(const char*, int32_t) { return -1; }
void unregister_io_device(int32_t) {}
bool set_device_interrupt_handler(Vector, int32_t, InterruptHandler, void*) { return false; }
void set_interrupt_handler(Vector, InterruptHandler, void*) {}
namespace apic { void eoi() {} }
void yield() { sched_yield(); }

void sleep_us(uint64_t us) {
  timespec ts{static_cast<time_t>(us / 1000000), static_cast<long>(us % 1000000) * 1000};
  nanosleep(&ts, nullptr);
}

Thread* spawn(ThreadFunction fn, void* argument, const char* name, int32_t, int8_t, uint32_t stack_bytes, int32_t) {
  auto* t = new HostThread{};
  t->base.entry = fn;
  t->base.argument = argument;
  t->base.name = name;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (stack_bytes) pthread_attr_setstacksize(&attr, stack_bytes < 65536 ? 65536 : stack_bytes);   // (the size the caller asked for, as the kernel gives it)
  const int created = pthread_create(&t->handle, &attr, trampoline, t);
  pthread_attr_destroy(&attr);
  if (created != 0) {
    delete t;
    return nullptr;
  }
  return &t->base;
}

bool join(Thread* thread, uint64_t) {
  auto* t = reinterpret_cast<HostThread*>(thread);
  pthread_join(t->handle, nullptr);
  delete t;
  return true;
}

void detach(Thread* thread) { pthread_detach(reinterpret_cast<HostThread*>(thread)->handle); }  // (leaked: tests only)

// A fair mutex here is a ticket lock (the mutex's wait list is not used on the host otherwise): list_.lock hands out tickets, handoff_ is the
// ticket being served, so the mutex goes to waiters strictly in the order they came - the kernel's hands it to its oldest waiter.
void Mutex::lock() {
  if (fair_) {
    const uint32_t ticket = __atomic_fetch_add(&list_.lock, 1u, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&handoff_, __ATOMIC_ACQUIRE) == ticket) return;
    __atomic_add_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);   // (what contended() reads)
    while (__atomic_load_n(&handoff_, __ATOMIC_ACQUIRE) != ticket) nap();
    __atomic_sub_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);
    return;
  }
  if (!__atomic_exchange_n(&state_, 1u, __ATOMIC_ACQUIRE)) return;
  __atomic_add_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);
  while (__atomic_exchange_n(&state_, 1u, __ATOMIC_ACQUIRE)) nap();
  __atomic_sub_fetch(&waiters_, 1, __ATOMIC_SEQ_CST);
}
bool Mutex::try_lock() {
  if (fair_) {   // (only when nobody holds or waits for it: the next ticket is the one being served)
    uint32_t serving = __atomic_load_n(&handoff_, __ATOMIC_ACQUIRE);
    return __atomic_compare_exchange_n(&list_.lock, &serving, serving + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
  }
  return !__atomic_exchange_n(&state_, 1u, __ATOMIC_ACQUIRE);
}
void Mutex::unlock() {
  if (fair_) {
    __atomic_fetch_add(&handoff_, 1u, __ATOMIC_RELEASE);
    return;
  }
  __atomic_store_n(&state_, 0u, __ATOMIC_RELEASE);
}

// A condition variable that polls: the wait list's lock word counts notifications here (nothing else takes this list's lock on the host). A waiter reads the count
// while it still holds the mutex, so a notification sent after it let go is not missed.
bool ConditionVariable::wait(Mutex& mutex, uint64_t timeout_us) {
  const uint32_t seen = __atomic_load_n(&list_.lock, __ATOMIC_ACQUIRE);
  mutex.unlock();
  const uint64_t start = now_us();
  bool notified = false;
  for (;;) {
    if (__atomic_load_n(&list_.lock, __ATOMIC_ACQUIRE) != seen) { notified = true; break; }
    if (timeout_us && now_us() - start >= timeout_us) break;
    nap();
  }
  mutex.lock();
  return notified;
}
void ConditionVariable::notify_one() { __atomic_fetch_add(&list_.lock, 1u, __ATOMIC_ACQ_REL); }      // (wakes every waiter: allowed, waits may end spuriously)
void ConditionVariable::notify_all() { __atomic_fetch_add(&list_.lock, 1u, __ATOMIC_ACQ_REL); }

void Semaphore::acquire() { acquire_for(0); }
bool Semaphore::try_acquire() {
  uint32_t c = __atomic_load_n(&count_, __ATOMIC_ACQUIRE);
  while (c > 0)
    if (__atomic_compare_exchange_n(&count_, &c, c - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return true;
  return false;
}
bool Semaphore::acquire_for(uint64_t timeout_us) {
  const uint64_t start = now_us();
  for (;;) {
    if (try_acquire()) return true;
    if (timeout_us && now_us() - start >= timeout_us) return false;
    nap();
  }
}
void Semaphore::release(uint32_t count) { __atomic_fetch_add(&count_, count, __ATOMIC_ACQ_REL); }

void Event::set() { __atomic_store_n(&signaled_, true, __ATOMIC_RELEASE); }
void Event::reset() { __atomic_store_n(&signaled_, false, __ATOMIC_RELEASE); }
bool Event::wait(uint64_t timeout_us) {
  const uint64_t start = now_us();
  for (;;) {
    if (auto_reset_ ? __atomic_exchange_n(&signaled_, false, __ATOMIC_ACQ_REL) : __atomic_load_n(&signaled_, __ATOMIC_ACQUIRE)) return true;
    if (timeout_us && now_us() - start >= timeout_us) return false;
    nap();
  }
}

void set_thread_io_priority(Thread*, uint8_t, uint16_t) {}

}  // namespace kernel

namespace hardware {
uint64_t read_tsc() { return now_us(); }   // (a counter running at 1 MHz)
uint64_t tsc_ticks_per_us() { return 1; }
}  // namespace hardware
