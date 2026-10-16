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

#include "KernelDemo.hpp"
#include "console.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"

using namespace kernel;

namespace {

bool report(const char* what, bool ok) {
  console::puts("  ");
  console::puts(what);
  console::puts(ok ? ": ok\n" : ": FAILED\n");
  return ok;
}

// ---- fairness: more busy threads than cores must all make progress ----

struct Spinner {
  volatile bool* stop;
  uint64_t count = 0;
  uint64_t cores_seen = 0;  // bit per core it ran on
};

void spinner_main(void* argument) {
  Spinner* s = static_cast<Spinner*>(argument);
  while (!__atomic_load_n(s->stop, __ATOMIC_ACQUIRE)) {
    ++s->count;
    s->cores_seen |= uint64_t{1} << (current_core() & 63);
  }
}

bool test_preemption(allocators::IAllocator& allocator) {
  const uint32_t threads = core_count() * 2 + 1;
  volatile bool stop = false;
  Spinner* spinners = static_cast<Spinner*>(allocator.allocate(sizeof(Spinner) * threads));
  Thread** handles = static_cast<Thread**>(allocator.allocate(sizeof(Thread*) * threads));
  if (!spinners || !handles) return false;
  for (uint32_t i = 0; i < threads; ++i) {
    spinners[i] = Spinner{&stop, 0, 0};
    handles[i] = spawn(spinner_main, &spinners[i], "spinner");
  }
  sleep_ms(400);
  __atomic_store_n(&stop, true, __ATOMIC_RELEASE);
  bool all_progressed = true;
  uint64_t cores = 0;
  for (uint32_t i = 0; i < threads; ++i) {
    all_progressed &= handles[i] && join(handles[i], 2000000);
    all_progressed &= spinners[i].count > 0;
    cores |= spinners[i].cores_seen;
  }
  uint32_t cores_used = 0;
  for (uint32_t i = 0; i < 64; ++i) cores_used += (cores >> i) & 1;
  console::puts("  ");
  console::put_dec(threads);
  console::puts(" busy threads on ");
  console::put_dec(core_count());
  console::puts(" cores; ran on ");
  console::put_dec(cores_used);
  console::puts(" distinct cores\n");
  allocator.deallocate(spinners);
  allocator.deallocate(handles);
  return report("every busy thread got CPU time (preemption)", all_progressed) && (core_count() == 1 || cores_used > 1);
}

// ---- mutual exclusion ----

struct Counter {
  Mutex mutex;
  uint64_t value = 0;  // deliberately not atomic: only the mutex protects it
  uint32_t rounds = 0;
};

void counter_main(void* argument) {
  Counter* c = static_cast<Counter*>(argument);
  for (uint32_t i = 0; i < c->rounds; ++i) {
    c->mutex.lock();
    const uint64_t v = c->value;
    if ((i & 255) == 0) yield();  // invite a context switch inside the critical section
    c->value = v + 1;
    c->mutex.unlock();
  }
}

bool test_mutex(allocators::IAllocator& allocator) {
  const uint32_t threads = core_count() * 2;
  Counter counter;
  counter.rounds = 5000;
  Thread** handles = static_cast<Thread**>(allocator.allocate(sizeof(Thread*) * threads));
  if (!handles) return false;
  for (uint32_t i = 0; i < threads; ++i) handles[i] = spawn(counter_main, &counter, "counter");
  for (uint32_t i = 0; i < threads; ++i) join(handles[i]);
  allocator.deallocate(handles);
  return report("mutex keeps a plain counter exact under contention", counter.value == uint64_t{threads} * counter.rounds);
}

// ---- bounded queue with semaphores ----

struct Pipe {
  static constexpr uint32_t kSlots = 8;
  Semaphore free_slots{kSlots};
  Semaphore items{0};
  Mutex mutex;
  uint32_t buffer[kSlots] = {};
  uint32_t head = 0, tail = 0;
  uint64_t sum = 0, consumed = 0;
  uint32_t per_producer = 0, producers = 0;
};

void producer_main(void* argument) {
  Pipe* p = static_cast<Pipe*>(argument);
  for (uint32_t i = 1; i <= p->per_producer; ++i) {
    p->free_slots.acquire();
    p->mutex.lock();
    p->buffer[p->tail++ % Pipe::kSlots] = i;
    p->mutex.unlock();
    p->items.release();
  }
}

void consumer_main(void* argument) {
  Pipe* p = static_cast<Pipe*>(argument);
  while (p->items.acquire_for(500000)) {
    p->mutex.lock();
    const uint32_t v = p->buffer[p->head++ % Pipe::kSlots];
    p->sum += v;
    ++p->consumed;
    p->mutex.unlock();
    p->free_slots.release();
  }
}

bool test_pipe() {
  Pipe pipe;
  pipe.per_producer = 2000;
  pipe.producers = 3;
  Thread* producers[3];
  Thread* consumers[2];
  for (Thread*& t : producers) t = spawn(producer_main, &pipe, "producer");
  for (Thread*& t : consumers) t = spawn(consumer_main, &pipe, "consumer");
  for (Thread* t : producers) join(t);
  for (Thread* t : consumers) join(t);
  const uint64_t expected = uint64_t{pipe.producers} * pipe.per_producer * (pipe.per_producer + 1) / 2;
  return report("producer/consumer queue delivers every item once",
                pipe.consumed == uint64_t{pipe.producers} * pipe.per_producer && pipe.sum == expected);
}

// ---- events, condition variables, sleep, join ----

struct Signals {
  Event manual;
  Event automatic{true};
  Mutex mutex;
  ConditionVariable condition;
  bool ready = false;
  uint32_t woken = 0;
};

void waiter_main(void* argument) {
  Signals* s = static_cast<Signals*>(argument);
  if (s->manual.wait(2000000)) __atomic_add_fetch(&s->woken, 1, __ATOMIC_SEQ_CST);
}

void auto_waiter_main(void* argument) {
  Signals* s = static_cast<Signals*>(argument);
  if (s->automatic.wait(300000)) __atomic_add_fetch(&s->woken, 1, __ATOMIC_SEQ_CST);
}

void condition_waiter_main(void* argument) {
  Signals* s = static_cast<Signals*>(argument);
  s->mutex.lock();
  while (!s->ready) s->condition.wait(s->mutex, 2000000);
  ++s->woken;
  s->mutex.unlock();
}

int exit_value_thread_result = 0;
void exiting_main(void*) {
  exit_value_thread_result = 7;
  exit_thread();
}

bool test_blocking() {
  bool ok = true;

  const uint64_t t0 = hardware::uptime_us();
  sleep_ms(50);
  const uint64_t slept = hardware::uptime_us() - t0;
  ok &= report("sleep_ms(50) sleeps about 50 ms", slept >= 50000 && slept < 200000);

  Signals s;
  const uint64_t t1 = hardware::uptime_us();
  const bool timed_out = !s.manual.wait(30000);
  const uint64_t waited = hardware::uptime_us() - t1;
  ok &= report("event wait times out", timed_out && waited >= 30000 && waited < 200000);

  Thread* waiters[4];
  for (Thread*& t : waiters) t = spawn(waiter_main, &s, "waiter");
  sleep_ms(30);
  s.manual.set();
  for (Thread* t : waiters) join(t);
  ok &= report("manual-reset event releases every waiter", s.woken == 4);

  s.woken = 0;
  Thread* autos[3];
  for (Thread*& t : autos) t = spawn(auto_waiter_main, &s, "auto");
  sleep_ms(30);
  s.automatic.set();
  s.automatic.set();
  for (Thread* t : autos) join(t);
  ok &= report("auto-reset event releases one waiter per set", s.woken == 2);

  s.woken = 0;
  Thread* conds[3];
  for (Thread*& t : conds) t = spawn(condition_waiter_main, &s, "cond");
  sleep_ms(30);
  s.mutex.lock();
  s.ready = true;
  s.mutex.unlock();
  s.condition.notify_all();
  for (Thread* t : conds) join(t);
  ok &= report("condition variable notify_all wakes every waiter", s.woken == 3);

  Thread* exiting = spawn(exiting_main, nullptr, "exiting");
  ok &= report("join returns after the thread exits", join(exiting, 1000000) && exit_value_thread_result == 7);

  Thread* never = spawn([](void*) { sleep_ms(300); }, nullptr, "slow");
  const bool early = !join(never, 20000);
  join(never);
  ok &= report("join times out on a thread that is still running", early);
  return ok;
}

}  // namespace

bool run_kernel_demo(allocators::IAllocator& allocator) {
  bool ok = true;
  ok &= test_preemption(allocator);
  ok &= test_mutex(allocator);
  ok &= test_pipe();
  ok &= test_blocking();
  return ok;
}
