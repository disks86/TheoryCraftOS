# Scheduler locking and the wake-up protocol

The thread scheduler (`src/Kernel/ThreadScheduler.cpp`) and the blocking primitives (`src/Kernel/Sync.cpp`) used to share one global spinlock.
Every switch, wake-up, block and timeout on every core took it. They now use several smaller locks. This page says what each lock guards, the
order they are taken in, and why a wake-up cannot be lost.

## Locks

Every lock is a spinlock that also masks interrupts on the core holding it (`IrqLock`), so an interrupt handler on that core cannot spin on a
lock its own core holds.

| Lock | Where | Guards |
| --- | --- | --- |
| wait-list lock | `WaitList::lock`, one per waitable object (each `Mutex`, `Semaphore`, `Event`, `ConditionVariable`, a thread's `joiners`, the balancer's list) | the list of waiting threads, each waiter's `waiting_on`, and the object's own state (a semaphore's count, an event's flag) |
| thread lock | `Thread::wake_lock`, one per thread | the thread's `state` changes from and to `Blocked`, `on_cpu`, `wait_seq`, `timed_out` |
| core lock | one per core (`PerCore::lock`) | that core's run queue, its sleeper list, its `current` / `leaving` thread and the user-mode fields of its page |
| threads lock | one, global | the list of all threads and the zombie list |
| steering lock | one, global | the interrupt-steering tallies and callbacks |

Some state is read without any lock:
- the per-level ready counts, queue lengths and idle flags that placement reads (atomics);
- a thread's priority, affinity and core mask (atomic stores, read once per decision).

## Order

**wait-list lock, then thread lock, then core lock.** A thread never takes two wait-list locks, except that a condition variable's lock may be
followed by its mutex's (to hand the mutex over while still enqueued). It never holds two thread locks, and it holds a second core lock only
through `try_lock`, when stealing work.

The threads lock and the steering lock are taken alone.

Paths that start from the other end let go first:
- The timer starts from its core lock: it unlinks the due sleepers, lets go, and then takes each sleeper's thread lock.
- A wake-up decides on steering under the thread lock but does the steering after letting go.

## Thread states

`Running`, `Ready`, `Blocked`, `Zombie`. `on_cpu` is set while a core holds the thread's registers, from the switch onto it until
`finish_switch` on the next thread's stack.

Two kinds of change:
- `Blocked` → `Ready` only under the thread lock.
- `Ready` → `Running` only by the core that takes the thread out of a run queue, under that queue's core lock. Nobody else touches a
  `Ready` thread that is in a queue.

## Blocking (`detail::prepare_wait` / `finish_wait`)

1. The waiter takes the object's wait-list lock and checks its condition. If it doesn't have to wait, it lets go and returns.
2. `prepare_wait(list, timeout)`: under the thread lock, `state = Blocked` and `wait_seq += 1`. It appends itself to the list
   (`waiting_on = list`) and, with a timeout, to its own core's sleeper list, remembering the core (`sleep_core`) and the `wait_seq`.
3. It lets go of the wait-list lock and calls `yield()`.
4. When it runs again, `finish_wait(list)` takes it off the sleeper list (under `sleep_core`'s lock, if it is still there) and off the wait
   list (under the wait-list lock, if `waiting_on` is still that list), then looks at `timed_out`.

A thread is on at most one wait list and one sleeper list, and only between steps 2 and 4 of its current wait.

## Waking (`detail::wake_one`, `try_wake`)

`wake_one(list)` runs with the wait-list lock held. It takes the first waiter off the list (`waiting_on = nullptr`) and calls
`try_wake(t, any sequence)`.

`try_wake` takes `t`'s thread lock. If `t` is no longer `Blocked` (its timeout won), it fails and `wake_one` goes on to the next waiter, so
the wake-up is not used up on a thread that had already given up.

Otherwise `state = Ready`, and:
- if `t` is on a core (`on_cpu`: it has not switched away yet, or its core is still leaving it), nothing more. That core sees `Ready`.
- if not, `t` is put in a run queue (`enqueue_ready`, under the core lock of the queue chosen).

`try_wake` can't touch a freed thread: a waiter that is still on the list cannot finish its wait, so it cannot exit.

## Timeouts

A core's tick takes its core lock, unlinks the due sleepers, and pins each with `timer_refs += 1`. Then it lets go and calls
`try_wake(t, the wait_seq the sleeper was armed with)` for each, with `timed_out` set. Afterwards it unpins.

- A thread woken some other way meanwhile has moved on: it is no longer `Blocked`, or it is blocked in a later wait with a higher
  `wait_seq`. So a stale timeout never ends a later wait.
- `timer_refs` keeps `join` and the zombie reaper from freeing a thread the tick still points at.

## Switching (`reschedule` / `finish_switch`)

`reschedule` runs on the core, in an interrupt, with interrupts masked. It takes the outgoing thread's lock, then its own core lock.

**Preemption.** A preempted `Running` thread becomes `Ready`.
- The outgoing thread goes into its own core's queue for the duration of the pick, so it competes as before.
- The pick takes the next thread out of a queue: its own under the lock it holds, another core's only through `try_lock` (a busy queue
  is skipped this time, and the next tick looks again).
- If another thread wins, the outgoing one comes out of the queue again and is recorded as `leaving`.

**A thread that blocked** is simply not queued.

`finish_switch` runs on the new thread's stack, once the old stack is no longer in use. Under the old thread's lock it:
- clears `on_cpu`;
- if the old thread is `Ready` (preempted, or woken while it was on its way out), puts it in a run queue.

Because `try_wake` reads `on_cpu` and `finish_switch` clears it under the same thread lock, exactly one of them queues the thread. The
wake-up that arrives between "decided to switch away" and "off the core" is not lost.

## Thread exit and join

`exit_thread` takes its `joiners` lock, sets `Zombie` under its thread lock and wakes every joiner. Then, under the threads lock, it puts
itself on the zombie list if it is detached; `detach` does the same check from the other side, and `zombie_listed` keeps it from being
listed twice.

`join` and the reaper free a thread only when it is a `Zombie`, `on_cpu` is clear and `timer_refs` is 0.

## Tests

The boot self test (`TCOS_SELFTESTS`, `KernelDemo.cpp`) stresses this on every core:
- mutex hand-offs;
- semaphore ping-pong between pairs of threads;
- event waits racing their own timeouts;
- condition variables;
- joins of threads that exit at once.

Each test checks counts that a lost or doubled wake-up would break, and finishes within a deadline (a lost wake-up shows as a hang the
deadline catches).

Results: in the final round, eight boots under QEMU passed all five stress tests and every other self check:
- KVM with 2, 4 and 8 cores, twice each;
- TCG with 2 and 4 cores.

The nine KVM runs of the round before also passed all five stress tests.

Bugs this caught:
- A tick treated "no sleeper" (`~0`) as due, through a signed comparison.
- `scheduling::Scheduler::run()` could return before its last task thread was reaped. It now joins the threads it starts.
