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
#include <new>
#include "console.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#if defined(__x86_64__)
#include "Kernel/Apic.hpp"
#endif

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

#if defined(__x86_64__)
// ---- device-aware placement: the thread a device's interrupts wake runs on the core they arrive on ----
// The "device" is an inter-processor interrupt sent to one core with a vector whose handler was set with set_device_interrupt_handler: to the
// scheduler it is the same as an MSI landing there.

struct IoProbe {
  Event arrived{true};
  volatile bool stop = false;
  int32_t device_core = -1;
  uint32_t rounds = 0, on_device_core = 0;
  int32_t first_core = -1;
  // steering: where main sends the interrupt (the retarget callback changes it), and what arrived on the pinned consumer's core
  uint32_t send_to = 0;
  int32_t pinned = -1, moved_to = -1;
  uint32_t retargets = 0, arrived_on_pinned = 0;
};

Frame* on_io_probe_interrupt(Frame* frame, void* context) {
  apic::eoi();
  IoProbe* p = static_cast<IoProbe*>(context);
  if (static_cast<int32_t>(current_core()) == p->pinned) __atomic_add_fetch(&p->arrived_on_pinned, 1, __ATOMIC_RELAXED);
  p->arrived.set();
  return frame;
}

bool retarget_io_probe(void* context, uint32_t core, uint64_t hardware_id) {
  IoProbe* p = static_cast<IoProbe*>(context);
  p->moved_to = static_cast<int32_t>(core);
  ++p->retargets;
  __atomic_store_n(&p->send_to, static_cast<uint32_t>(hardware_id), __ATOMIC_RELEASE);
  return true;
}

void io_consumer_main(void* argument) {
  IoProbe* p = static_cast<IoProbe*>(argument);
  while (p->arrived.wait(500000) && !p->stop) {
    ++p->rounds;
    if (static_cast<int32_t>(current_core()) == p->device_core) ++p->on_device_core;
  }
}

void io_bound_main(void* argument) { static_cast<IoProbe*>(argument)->first_core = static_cast<int32_t>(current_core()); }

bool test_io_affinity() {
  if (core_count() < 2) return report("I/O affinity (one core: nothing to place)", true);
  bool ok = true;
  IoProbe probe;
  const uint32_t core = choose_io_interrupt_core();
  probe.device_core = static_cast<int32_t>(core);
  const int32_t device = register_io_device("selftest", probe.device_core);
  const uint64_t apic_id = core_hardware_id(core);
  if (device < 0 || apic_id > 0xFF || !set_device_interrupt_handler(kVectorIoSelfTest, device, on_io_probe_interrupt, &probe)) {
    unregister_io_device(device);
    return report("I/O affinity: device set up", false);
  }
  ok &= report("I/O affinity: the device's interrupt goes to a core other than the boot core", core != current_core());

  Thread* consumer = spawn(io_consumer_main, &probe, "io-consumer");
  constexpr uint32_t kInterrupts = 40;
  for (uint32_t i = 0; i < kInterrupts && consumer; ++i) {
    apic::send_ipi(static_cast<uint32_t>(apic_id), kVectorIoSelfTest);
    sleep_ms(3);
  }
  probe.stop = true;
  probe.arrived.set();
  if (consumer) join(consumer);
  scheduling::IoDeviceInfo info;
  const bool have_info = io_device_info(device, &info);
  ok &= report("I/O affinity: every interrupt counted and woke the consumer", have_info && info.interrupts == kInterrupts && info.wakes > 0 && info.wakes <= info.interrupts && probe.rounds >= kInterrupts - 2);
  ok &= report("I/O affinity: the woken thread runs on the interrupting core", probe.rounds && probe.on_device_core + 2 >= probe.rounds);

  Thread* bound = spawn(io_bound_main, &probe, "io-bound", -1, -1, kDefaultStackBytes, device);
  if (bound) join(bound);
  ok &= report("I/O affinity: a thread spawned for the device starts on its core", probe.first_core == probe.device_core);

  // Steering: the consumer is pinned to another core, so the interrupt should follow it there.
  probe.pinned = static_cast<int32_t>(core + 1 < core_count() ? core + 1 : 0);
  if (probe.pinned == probe.device_core) probe.pinned = (probe.pinned + 1) % static_cast<int32_t>(core_count());
  probe.stop = false;
  probe.rounds = probe.on_device_core = 0;
  probe.send_to = static_cast<uint32_t>(apic_id);
  probe.arrived.reset();
  set_io_device_retarget(device, retarget_io_probe, &probe);
  Thread* pinned = spawn(io_consumer_main, &probe, "io-pinned", probe.pinned);
  for (uint32_t i = 0; i < 2 * kInterrupts && pinned; ++i) {
    apic::send_ipi(__atomic_load_n(&probe.send_to, __ATOMIC_ACQUIRE), kVectorIoSelfTest);
    sleep_ms(2);
  }
  probe.stop = true;
  probe.arrived.set();
  if (pinned) join(pinned);
  set_io_device_retarget(device, nullptr, nullptr);
  const bool moved = io_device_info(device, &info) && info.interrupt_core == probe.pinned;
  ok &= report("I/O affinity: the interrupt moves to the core its pinned consumer runs on",
               probe.retargets == 1 && probe.moved_to == probe.pinned && moved && probe.arrived_on_pinned >= kInterrupts / 2);

  set_device_interrupt_handler(kVectorIoSelfTest, device, nullptr, nullptr);
  unregister_io_device(device);
  return ok;
}
#endif

// ---- stress: the wake-up protocol under contention on every core (docs/SchedulerLocking.md) ----
// Each check counts something a lost or doubled wake-up would change, and every wait for the workers has a deadline, so a lost wake-up shows as a
// failure instead of a hang.

constexpr uint64_t kStressJoinUs = 20000000;   // (generous: TCG with 8 cores is slow)

uint32_t next_random(uint32_t* state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *state = x;
}

bool join_all(Thread** threads, uint32_t count) {
  bool ok = true;
  for (uint32_t i = 0; i < count; ++i) ok &= threads[i] && join(threads[i], kStressJoinUs);
  return ok;
}

// Timeouts racing releases: a waiter whose timeout fired is passed over by a release, so nothing is lost: what was taken plus what is left is what
// was given.
struct SemaphoreRace {
  Semaphore semaphore{0};
  uint32_t released = 0, taken = 0;
  volatile bool stop = false;
  uint32_t seed = 0;
};

void semaphore_taker(void* argument) {
  SemaphoreRace* r = static_cast<SemaphoreRace*>(argument);
  uint32_t seed = __atomic_add_fetch(&r->seed, 7919, __ATOMIC_RELAXED) | 1;
  while (!r->stop) {
    if (r->semaphore.acquire_for(20 + next_random(&seed) % 200)) __atomic_add_fetch(&r->taken, 1, __ATOMIC_RELAXED);
  }
}

void semaphore_giver(void* argument) {
  SemaphoreRace* r = static_cast<SemaphoreRace*>(argument);
  uint32_t seed = __atomic_add_fetch(&r->seed, 104729, __ATOMIC_RELAXED) | 1;
  for (uint32_t i = 0; i < 3000; ++i) {
    r->semaphore.release();
    __atomic_add_fetch(&r->released, 1, __ATOMIC_RELAXED);
    if (next_random(&seed) % 4 == 0) yield();
    else if (next_random(&seed) % 16 == 0) sleep_us(50);
  }
}

bool stress_semaphore_timeouts() {
  SemaphoreRace race;
  const uint32_t takers = core_count() + 1, givers = core_count();
  Thread* threads[2 * 128 + 2] = {};
  for (uint32_t i = 0; i < takers; ++i) threads[i] = spawn(semaphore_taker, &race, "taker");
  for (uint32_t i = 0; i < givers; ++i) threads[takers + i] = spawn(semaphore_giver, &race, "giver");
  bool ok = join_all(threads + takers, givers);
  race.stop = true;
  ok &= join_all(threads, takers);
  uint32_t left = 0;
  while (race.semaphore.try_acquire()) ++left;
  return report("stress: semaphore releases racing timeouts, none lost", ok && race.taken + left == race.released && race.released == 3000 * givers);
}

// Ping-pong: each pair hands a turn back and forth through two semaphores; one lost wake-up stops the pair.
struct PingPong {
  Semaphore ping{0}, pong{0};
  uint32_t rounds = 0;
};

constexpr uint32_t kPingPongRounds = 2000;

void ping_main(void* argument) {
  PingPong* p = static_cast<PingPong*>(argument);
  for (uint32_t i = 0; i < kPingPongRounds; ++i) {
    p->ping.release();
    if (!p->pong.acquire_for(5000000)) return;
  }
}

void pong_main(void* argument) {
  PingPong* p = static_cast<PingPong*>(argument);
  for (uint32_t i = 0; i < kPingPongRounds; ++i) {
    if (!p->ping.acquire_for(5000000)) return;
    ++p->rounds;
    p->pong.release();
  }
}

bool stress_ping_pong(allocators::IAllocator& allocator) {
  const uint32_t pairs = core_count() * 2;
  PingPong* games = static_cast<PingPong*>(allocator.allocate(sizeof(PingPong) * pairs));
  Thread** threads = static_cast<Thread**>(allocator.allocate(sizeof(Thread*) * pairs * 2));
  if (!games || !threads) return report("stress: semaphore ping-pong (memory)", false);
  for (uint32_t i = 0; i < pairs; ++i) {
    new (&games[i]) PingPong();
    threads[2 * i] = spawn(ping_main, &games[i], "ping");
    threads[2 * i + 1] = spawn(pong_main, &games[i], "pong");
  }
  bool ok = join_all(threads, pairs * 2);
  for (uint32_t i = 0; i < pairs; ++i) ok &= games[i].rounds == kPingPongRounds;
  allocator.deallocate(threads);
  allocator.deallocate(games);
  return report("stress: semaphore ping-pong pairs on every core finish every round", ok);
}

// A mutex handed between many threads that also yield and sleep while holding it.
struct MutexStorm {
  Mutex mutex;
  uint64_t counter = 0;
  uint32_t seed = 0;
};

constexpr uint32_t kMutexRounds = 3000;

void mutex_storm_main(void* argument) {
  MutexStorm* m = static_cast<MutexStorm*>(argument);
  uint32_t seed = __atomic_add_fetch(&m->seed, 31337, __ATOMIC_RELAXED) | 1;
  for (uint32_t i = 0; i < kMutexRounds; ++i) {
    ScopedMutex hold(m->mutex);
    const uint64_t before = m->counter;
    const uint32_t r = next_random(&seed) % 64;
    if (r == 0) sleep_us(30);
    else if (r < 4) yield();
    m->counter = before + 1;
  }
}

bool stress_mutex() {
  MutexStorm storm;
  const uint32_t count = core_count() * 3;
  Thread* threads[3 * 128] = {};
  for (uint32_t i = 0; i < count; ++i) threads[i] = spawn(mutex_storm_main, &storm, "mutex");
  const bool ok = join_all(threads, count);
  return report("stress: mutex hand-offs keep a plain counter exact", ok && storm.counter == uint64_t{kMutexRounds} * count);
}

// A bounded queue through a condition variable; consumers wait with short timeouts so that notifications race timeouts.
struct Channel {
  Mutex mutex;
  ConditionVariable not_empty, not_full;
  uint32_t items[16] = {};
  uint32_t head = 0, count = 0;
  uint64_t produced_sum = 0, consumed_sum = 0;
  uint32_t consumed = 0, total = 0;
};

void channel_producer(void* argument) {
  Channel* c = static_cast<Channel*>(argument);
  for (uint32_t i = 1; i <= 2000; ++i) {
    ScopedMutex hold(c->mutex);
    while (c->count == 16) c->not_full.wait(c->mutex, 1000);
    c->items[(c->head + c->count) % 16] = i;
    ++c->count;
    c->produced_sum += i;
    c->not_empty.notify_one();
  }
}

void channel_consumer(void* argument) {
  Channel* c = static_cast<Channel*>(argument);
  ScopedMutex hold(c->mutex);
  for (;;) {
    while (c->count == 0 && c->consumed < c->total) c->not_empty.wait(c->mutex, 300);
    if (c->consumed == c->total) break;
    c->consumed_sum += c->items[c->head];
    c->head = (c->head + 1) % 16;
    --c->count;
    ++c->consumed;
    c->not_full.notify_one();
    if (c->consumed == c->total) c->not_empty.notify_all();
  }
}

bool stress_condition() {
  Channel channel;
  const uint32_t producers = core_count(), consumers = core_count();
  channel.total = 2000 * producers;
  Thread* threads[2 * 128] = {};
  for (uint32_t i = 0; i < consumers; ++i) threads[i] = spawn(channel_consumer, &channel, "consumer");
  for (uint32_t i = 0; i < producers; ++i) threads[consumers + i] = spawn(channel_producer, &channel, "producer");
  const bool ok = join_all(threads, producers + consumers);
  return report("stress: condition-variable queue delivers every item once", ok && channel.consumed == channel.total && channel.consumed_sum == channel.produced_sum);
}

// Threads that end at once: joined, or detached and reaped; and sleepers on every core that must all wake.
uint32_t g_quick_runs = 0;
void quick_main(void*) { __atomic_add_fetch(&g_quick_runs, 1, __ATOMIC_RELAXED); }

void sleeper_main(void* argument) {
  uint32_t seed = static_cast<uint32_t>(reinterpret_cast<uint64_t>(argument)) | 1;
  for (uint32_t i = 0; i < 200; ++i) sleep_us(10 + next_random(&seed) % 300);
}

bool stress_join_and_sleep() {
  bool ok = true;
  __atomic_store_n(&g_quick_runs, 0u, __ATOMIC_RELAXED);
  uint32_t spawned = 0;
  for (uint32_t round = 0; round < 50; ++round) {
    Thread* batch[16] = {};
    for (Thread*& t : batch) {
      t = spawn(quick_main, nullptr, "quick", -1, -1, 64 * 1024);
      spawned += t ? 1 : 0;
    }
    for (uint32_t i = 0; i < 16; i += 2) ok &= batch[i] && join(batch[i], kStressJoinUs);
    for (uint32_t i = 1; i < 16; i += 2) detach(batch[i]);
  }
  const bool joined = ok;
  // The detached half may not all have run yet: wait for them, then for their cores to leave them (a zombie is reaped once off its core).
  for (uint32_t i = 0; i < 5000 && __atomic_load_n(&g_quick_runs, __ATOMIC_RELAXED) != spawned; ++i) sleep_ms(1);
  uint32_t unreaped = reap_zombies();
  for (uint32_t i = 0; i < 1000 && unreaped != 0; ++i) {
    sleep_ms(1);
    unreaped = reap_zombies();
  }
  Thread* sleepers[2 * 128] = {};
  const uint32_t count = core_count() * 2;
  for (uint32_t i = 0; i < count; ++i) sleepers[i] = spawn(sleeper_main, reinterpret_cast<void*>(uint64_t{i} * 2654435761u), "sleeper");
  const bool slept = join_all(sleepers, count);
  const uint32_t runs = __atomic_load_n(&g_quick_runs, __ATOMIC_RELAXED);
  const bool all = joined && unreaped == 0 && slept && runs == spawned && spawned == 800;
  if (!all) {
    auto dec = [](uint64_t v) { char b[24]; int n = 0; do { b[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v); while (n) { char c[2] = {b[--n], 0}; console::puts(c); } };
    console::puts("  (joined ");
    dec(joined);
    console::puts(", unreaped ");
    dec(unreaped);
    console::puts(", sleepers ");
    dec(slept);
    console::puts(", ran ");
    dec(runs);
    console::puts(" of ");
    dec(spawned);
    console::puts(")\n");
  }
  return report("stress: threads that end at once are joined or reaped, sleepers on every core all wake", all);
}

bool test_stress(allocators::IAllocator& allocator) {
  bool ok = true;
  ok &= stress_semaphore_timeouts();
  ok &= stress_ping_pong(allocator);
  ok &= stress_mutex();
  ok &= stress_condition();
  ok &= stress_join_and_sleep();
  return ok;
}

// ---- the tick takes a lock only when it has something to do ----

bool test_quiet_ticks() {
  auto totals = [](uint64_t* ticks, uint64_t* lock_free) {
    *ticks = *lock_free = 0;
    for (uint32_t i = 0; i < core_count(); ++i) {
      CoreInfo info;
      if (!core_info(i, &info) || !info.online) continue;
      *ticks += info.busy_ticks + info.idle_ticks;
      *lock_free += info.lock_free_ticks;
    }
  };
  uint64_t ticks0, free0, ticks1, free1;
  totals(&ticks0, &free0);
  sleep_ms(200);
  totals(&ticks1, &free1);
  const uint64_t ticks = ticks1 - ticks0, lock_free = free1 - free0;
  char percent[] = "  (000% of the ticks)\n";
  const uint64_t p = ticks ? lock_free * 100 / ticks : 0;
  percent[3] = static_cast<char>('0' + p / 100 % 10);
  percent[4] = static_cast<char>('0' + p / 10 % 10);
  percent[5] = static_cast<char>('0' + p % 10);
  console::puts(percent);
  return report("quiet ticks mostly take no lock", ticks > 0 && lock_free * 2 >= ticks);
}

}  // namespace

bool run_kernel_demo(allocators::IAllocator& allocator) {
  bool ok = true;
  ok &= test_preemption(allocator);
  ok &= test_mutex(allocator);
  ok &= test_pipe();
  ok &= test_blocking();
  ok &= test_quiet_ticks();
  ok &= test_stress(allocator);
#if defined(__x86_64__)
  ok &= test_io_affinity();
#endif
  return ok;
}
