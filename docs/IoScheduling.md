# I/O scheduling

Every storage device the system finds gets an I/O scheduler in front of it (`hardware::ScheduledStorageDevice`, put there by
`SystemStorageDevices`). The scheduler decides which waiting request goes to the device next, and how many requests of each kind the device may
hold at once. Its model is BFQ's: per-issuer queues, B-WF2Q+ weighted fairness, and weight raising for interactive programs. It is reworked for
NVMe and SATA flash, machines with many cores, and low latency.

| File | What it holds |
|---|---|
| `src/IoScheduler/IoTypes.hpp`, `IoCost.cpp` | classes, weights, domains, media kinds, the cost model |
| `src/IoScheduler/FairQueue.{hpp,cpp}` | which waiting request goes next (no locks; the owner locks) |
| `src/IoScheduler/LatencyController.{hpp,cpp}` | per-domain device depth from completion latencies (lock-free) |
| `src/IoScheduler/IoScheduler.hpp` | the two together: lock-free bypass, slot tokens, dispatch on submit and completion |
| `src/Hardware/StorageDevice/ScheduledStorageDevice.{hpp,cpp}` | the kernel side: thread → issuer and priority, waiting on an `Event` |
| `tests/IoSchedulerTest.cpp` | host test (ASan+UBSan and ThreadSanitizer builds) |
| `src/KernelDemo.cpp` `test_io_scheduler` | boot self-test (`TCOS_SELFTESTS=ON`) on a slow RAM disk |

## What a thread asks for

`kernel::set_thread_io_priority(thread, class, weight)` works like Linux's `ioprio_set`. There are three classes, and the order between them is strict:

- **Realtime**: goes before everything else.
- **BestEffort**: the default. Device time is shared by weight (1–1000, default 100).
- **Idle**: goes only when no Realtime or BestEffort request is waiting, and nothing of theirs has used the device for `idle_grace_us`
  (2 ms).

Without a call, the class and weight come from the thread's CPU priority. Priorities 0–2 are Idle, 28–31 are Realtime, and in between the
weight grows with the priority (8 → 100). `kernel::set_thread_io_group(thread, group)` puts threads into one queue, so the threads of one
process share one fair share rather than each getting their own.

## Fair queue (`FairQueue`)

- **Per-issuer queues, B-WF2Q+**. Each queue has a virtual start and finish time. Of the queues whose start has come, the one that finishes
  first goes. Within a class, device time therefore splits by weight; the host test measures 3.99 for weights 400:100.
- **Cost, not sectors**. `IoCostModel::for_media()` gives, per domain, a cost per command and per 4 KiB, in nanoseconds of device occupancy.
  On flash a random 4 KiB read costs the same as a sequential one, and writes and flushes cost more. The models cover NVMe, SATA SSD,
  spinning disk or optical, virtual disk and removable media. Drivers say which they are in `StorageInfo::media`; ATA reads it from IDENTIFY
  word 217.
- **Per request, with no idling**. BFQ serves one queue at a time for a budget, and idles the device waiting for that queue's next request.
  On a deep flash queue that idling costs throughput and buys little. Here each dispatch is a fresh WF2Q+ choice, and any number of queues
  can have requests on the device. A synchronous reader gets fast service rather than its full throughput share, which is the right trade on
  flash.
- **Deadlines**. Every request has one, set per domain by the cost model. On NVMe that is 10 ms for reads and 100 ms for writes; Realtime
  gets a quarter of that and Idle ten times as long. A request whose deadline has passed goes first, earliest deadline first, whatever its
  class. Nothing starves: an Idle request behind a saturating BestEffort issuer goes once its deadline is the earliest that has passed.
- **Weight raising** (BFQ's low-latency heuristic). A queue that has been idle for `interactive_gap_us` (20 ms) and comes back with a
  read gets 10× its weight. The raise lasts for 200 ms or until the queue has had `raise_budget` of service (by default, the cost of reading
  16 MiB). A busy period only ends after such a gap, so a streaming reader that pauses briefly between reads does not count as interactive.
  A queue whose last busy period was longer than the budget is not raised again. Interactive and soft-real-time readers, such as a player
  reading ahead, earn the raise each time they come back.
- **Fixed memory**. A pool of request slots and 64 sets of 4 issuer queues (plus one shared queue per class for issuers whose set is full)
  are allocated once. The I/O path never allocates, and a queue that is idle is handed to the next new issuer.

## Depth controller (`LatencyController`)

A flash drive queues internally. A deep queue of writes makes every read behind it slow, while throughput hardly grows. Kyber deals with
this, and so does this controller:

- **Domains**. Requests count in three domains: read, write and other (flush). Each domain has a limit on how many of its requests may be
  on the device at once. Reads may use the whole depth, writes three quarters of it, other a quarter.
- **Histograms**. Completions go into a histogram relative to the domain's latency target (on NVMe, 1 ms for reads and 5 ms for writes),
  with atomics only.
- **Adjustment once per window** (100 ms):
  - A domain whose 90th percentile is over its target halves the limits of the less important domains that had traffic. The order is
    read > other > write.
  - A domain 8× or more over its target also loses a quarter of its own limit.
  - When every domain is on target, a domain that had to wait for slots, or was idle, grows by a quarter, up to its cap.
- **Lock-free**. Limits never go below 1. The core whose completion ends the window does the adjustment, one core at a time by a flag.

## Concurrency (`IoScheduler`)

- **No dispatcher thread**. A request's own thread runs it on the device once it has a slot. The thread that submits a request, and the
  thread that completes one, hand free slots to whoever the fair queue picks, and wake them with `IoWaiter::grant()` (in the kernel, an
  `Event`) outside the lock. A request that can go at once costs no context switch.
- **Lock-free bypass**. While nothing is queued, a request that finds a slot of its domain free goes straight to the device with one
  compare-and-swap. Idle requests are the exception: they always queue, so that the grace time applies. The fair queue only works while the
  device is saturated, so an idle NVMe drive is barely slowed.
- **Slot word**. All slots live in one 64-bit word: 16 bits for the total in flight, and 16 bits per domain. It is taken and given back
  with atomics. The spinlock, the kernel's IRQ-masking wait-list lock, guards only the fair queue, for one pick and commit at a time.
- **No lost wake-ups**. A submitter counts itself as queued before it looks for a slot. A completer gives its slot back before it reads the
  queued count. Both are sequentially consistent, so one of them always sees the other. As a safety net, and for the Idle grace time, a
  waiter whose wait times out (`wait_hint_us`) calls `kick()`.

## What is not done

- **The drivers are still synchronous**. They poll their device and serialize requests behind one bounce buffer, so every device reports
  `queue_depth = 1` and the depth controller has nothing to throttle yet. The scheduler still decides the order, which is where the fairness
  and latency gains come from on today's drivers. Making the NVMe driver multi-queue (one queue pair per core, real depth, interrupts) is
  the next step. The scheduler is ready for it: set `StorageInfo::queue_depth`, and run one `IoScheduler` per hardware queue if per-queue
  locking is wanted.
- **Some callers don't pass priorities yet**. Windows `FileIoPriorityHintInformation` and process I/O priority are still ignored, and
  Linux `ioprio_set` is not wired to `set_thread_io_priority`. User processes do not yet put their threads into one `io_group`.
- **Requests are not merged**. Callers are synchronous and own their buffers. On flash, merging matters little.
- **No cgroup-style hierarchy**. The fairness is flat: issuers within a class.
- **No statistics view in the System Monitor yet**. `ScheduledStorageDevice::scheduler_statistics()` has the numbers.
