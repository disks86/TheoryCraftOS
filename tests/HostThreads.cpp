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
Thread* current_thread() { return t_current ? t_current : &t_self; }
uint32_t current_core() { return 0; }
uint32_t core_count() { return 4; }
void yield() { sched_yield(); }

void sleep_us(uint64_t us) {
  timespec ts{static_cast<time_t>(us / 1000000), static_cast<long>(us % 1000000) * 1000};
  nanosleep(&ts, nullptr);
}

Thread* spawn(ThreadFunction fn, void* argument, const char* name, int32_t, int8_t, uint32_t) {
  auto* t = new HostThread{};
  t->base.entry = fn;
  t->base.argument = argument;
  t->base.name = name;
  if (pthread_create(&t->handle, nullptr, trampoline, t) != 0) {
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

void Mutex::lock() {
  while (__atomic_exchange_n(&state_, 1u, __ATOMIC_ACQUIRE)) nap();
}
bool Mutex::try_lock() { return !__atomic_exchange_n(&state_, 1u, __ATOMIC_ACQUIRE); }
void Mutex::unlock() { __atomic_store_n(&state_, 0u, __ATOMIC_RELEASE); }

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

}  // namespace kernel
