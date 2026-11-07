# Network architecture: concurrency, fair sharing and latency

How the kernel's network stack (`src/Network`) runs on several cores, shares an interface fairly between connections, and keeps waits short.
The protocol details (IPv4/IPv6, TCP, UDP, DNS, offloads) are in the headers; this is about who runs what, when, and in which order.

## Threads and locks

- **One stack per interface.** `NetworkStackSet` builds a `NetworkStack` for each network interface. Each stack has its own lock (a
  `kernel::RecursiveMutex`, `INetworkStack::mutex()`) that guards it, its sockets and its resolver. Stacks share nothing, so interfaces
  never contend with each other.
- **One service thread per stack.** `NetworkStackSet::start_service()` starts a "network" thread for each stack. The thread polls its stack.
  Its interface's interrupt wakes it (the interrupt masks itself until `rearm_interrupts()`, so a burst costs one interrupt), it polls again
  at once while frames keep coming (`saturated()`), and otherwise it sleeps 5 ms (500 µs for a polled interface, or while frames wait for
  room in the interface). Two network cards are serviced on two cores, and a busy one does not delay the other. (Until November 2026 one
  thread polled every stack in turn.)
- **Application threads** call socket operations (`send()`, `receive()`, `accept()`…) directly. Each one takes the stack's lock for the time
  of the call. `send()` puts data on the wire from the caller's thread, so a send never waits for the service thread.
- **Polls let applications in.** `NetworkStack::poll()` handles received frames in batches of 16 (at most 64 per poll). Between batches it lets
  go of the lock if another thread is waiting for it (`Mutex::contended()`), and waits, yielding, until that thread has the lock (at most
  200 µs). The mutex does not queue, so without this the polling thread retook the lock straight away. With a flood of frames arriving, an
  application's datagram took 17 ms to go out; with the hand-over it takes 0.35 ms (`NetworkThreadTest`).

## Waking waiters instead of polling them

- **The stack wakes its service thread** (`NetworkStack::ServiceHooks::wake`) when it has work its next poll should do: a packet sent to
  itself over the loopback, or frames waiting in the transmit queue. A loopback round trip between two threads takes about 270 µs on the
  host (ThreadSanitizer build), not the 10 ms of two idle periods.
- **Applications wait for progress.** After a poll that changed something sockets may be waiting for (frames received or delivered, queued
  frames sent), the stack calls `ServiceHooks::progress`. `NetworkStackSet` counts these (`progress()`) and wakes everyone in
  `wait_for_progress(seen, timeout)`. The pattern: read `progress()`, try the operation, and if it would block, wait with what was read, so
  nothing that happens in between is missed. ws2_32's blocking calls and `select()` wait this way (`WindowsSockets.cpp`, `wait_for_network`),
  in 2 ms slices so that an exiting process is noticed. Before, they slept 2 ms between tries.

## Fair sharing of an interface

- **The transmit queue.** An interface's transmit ring can be full. A frame that does not fit, and any frame sent while others are already
  waiting, goes to the stack's `EgressQueue` (`src/Network/EgressQueue.hpp`) instead of being dropped. Before, it was dropped, and TCP waited a
  retransmission timeout (200 ms or more) to send it again.
- **The scheduler.** It is RFC 8290 (FQ-CoDel) without CoDel. Frames are hashed by flow (destination address, protocol, ports) into 64 queues,
  which take turns by deficit round robin, one quantum of 1514 bytes per turn. A flow with nothing queued (an ACK, a DNS query, an
  interactive packet) starts on the *new* list, which is served first.
- **Limits.** The queue holds at most 128 frames and 1 MiB. When it is full, the oldest frame of the flow with the most bytes queued is dropped.
  A frame that has waited 100 ms is dropped when its turn comes, which bounds both the latency and the cost of a frame the interface keeps
  refusing.
- **When it drains.** The queue drains at the start and end of every poll, and before any new frame is sent.
- **TCP holds back** while 4 of its frames are queued (`TcpSocket::kMaxQueuedFrames`, through `IIpLayer::queued_for`), and goes on from its
  timer once they are out, much like Linux's TCP small queues. With 2, the connection whose ACKs were handled first took what the queue left,
  and two bulk connections through a bottleneck split it 3:1. With 4 they split it evenly.
- **TCP congestion control** (`TcpSocket`):
  - RFC 5681 slow start and congestion avoidance, with appropriate byte counting (at most two segments per ACK in slow start).
  - Three duplicate ACKs halve the window and start RFC 6582 recovery. A timeout drops the window to one segment.
  - The initial window is ten segments (RFC 6928), and it restarts from there after an idle period longer than an RTO.
  - Connections sharing a bottleneck elsewhere on the path share it.
  - Go-back-N now keeps the highest sequence number sent (`snd_max_`). Before, an ACK for data the peer already had, beyond the rewound
    `snd_nxt_`, was thrown away, and a connection could stall for good once the congestion window stopped it resending everything at once.
- **Measured** (`NetworkFairnessTest`, an interface that takes 8 frames per millisecond):
  - Two bulk connections get 46.3 MB and 46.7 MB (Jain's index 1.000).
  - A request/answer connection beside them is answered in 2 steps (2 ms) every time.
  - Without the transmit queue, they got 0.45 MB and 0.21 MB, and the slowest answer took 467 ms.

## Tests

- `NetworkFairnessTest` covers:
  - The scheduler: a sparse flow goes ahead of a backlogged one, two bulk flows alternate, frames keep their order through a refusing
    interface, the sojourn limit, and the slot and byte limits drop from the fattest flow.
  - The flow classifier, against every truncation and bit flip of IPv4 and IPv6 frames.
  - The congestion window: initial value, growth, the cut after loss, and recovery.
  - The shared bottleneck above (`FakeNic::frames_per_ms` in `NetworkHarness.hpp` makes one).
- `NetworkThreadTest`, with real threads (`HostThreads.cpp`) under ThreadSanitizer, runs two `NetworkStackSet`s with service threads, joined by a
  wire whose interfaces interrupt. It checks:
  - Six connections echoing 96 KiB at once, a thread on each side.
  - The loopback ping-pong latency.
  - Datagrams sent while a thread floods the interface.

## Not done

- **One lock per stack**, not per socket or per receive queue. The receive queues of a multi-queue interface (RSS) are still handled by
  that stack's one thread.
- **No CoDel** (sojourn-based early drops), only the hard 100 ms limit. No pacing, no ECN.
- **TCP:** no SACK, and out-of-order segments are dropped (go-back-N recovery).
- **Lock fairness between application threads** is up to the mutex, which does not queue. Only the polling thread hands the lock over.
- **Other waiters:** `HttpClient::run` and the async `TcpStream` still poll on short timers rather than waiting for progress.
