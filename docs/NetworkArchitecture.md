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
- **Polls let applications in.** `NetworkStack::poll()` handles received frames in batches of 16 (at most 64 per poll), and between batches
  it lets go of the lock if another thread is waiting for it (`Mutex::contended()`).
- **The stack's lock is fair** (`kernel::Mutex(true)`, through `RecursiveMutex(true)`). An unlock with threads waiting hands the mutex to the
  one that has waited longest (`wake_one` wakes the oldest), and that thread holds it without trying again. Nobody can take it in between,
  and the thread that let go queues behind. The ordinary mutex lets the releasing thread barge: with a flood of frames arriving, an
  application's datagram took 17 ms to go out; with the fair lock it takes 0.3 ms (`NetworkThreadTest`). On the host, the fair mode is a
  ticket lock, and a thread that holds the lock in long stretches and one that needs it briefly take strict turns.

## Waking waiters instead of polling them

- **The stack wakes its service thread** (`NetworkStack::ServiceHooks::wake`) when it has work its next poll should do: a packet sent to
  itself over the loopback, or frames waiting in the transmit queue. A loopback round trip between two threads takes about 270 µs on the
  host (ThreadSanitizer build), not the 10 ms of two idle periods.
- **Applications wait for progress.** After a poll that changed something sockets may be waiting for (frames received or delivered, queued
  frames sent), the stack calls `ServiceHooks::progress`. `NetworkStackSet` counts these (`progress()`) and wakes everyone in
  `wait_for_progress(seen, timeout)`. The pattern: read `progress()`, try the operation, and if it would block, wait with what was read, so
  nothing that happens in between is missed. ws2_32's blocking calls and `select()` wait this way (`WindowsSockets.cpp`, `wait_for_network`),
  in 2 ms slices so that an exiting process is noticed. Before, they slept 2 ms between tries.
- **Each stack has its own progress counter too** (`INetworkStack::progress()`, `wait_for_progress()`, and `serviced()`, true when a service
  thread polls it). `HttpClient::run` and the synchronous `TcpStream` waits use it. So does an `async::Executor` given
  `TcpStream::idle_wake(stack)`: when no task can go on, it waits for the stack instead of sleeping 200 µs. With a service thread they also
  stop polling the stack themselves, which only contended for the lock.

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
- **CoDel** (RFC 8289) runs per flow in the queue. Once a flow's frames have waited longer than 5 ms for a whole 100 ms, a frame is
  dropped, then more often (the interval over the square root of the count) while that lasts. A frame whose packet is ECN-capable is marked
  CE instead, with the IPv4 header checksum redone, and is still sent.
- **TCP congestion control** (`TcpSocket`):
  - RFC 5681 slow start and congestion avoidance, with appropriate byte counting (at most two segments per ACK in slow start).
  - Three duplicate ACKs halve the window and start RFC 6582 recovery. A timeout drops the window to one segment.
  - The initial window is ten segments (RFC 6928), and it restarts from there after an idle period longer than an RTO.
  - Connections sharing a bottleneck elsewhere on the path share it.
  - Go-back-N now keeps the highest sequence number sent (`snd_max_`). Before, an ACK for data the peer already had, beyond the rewound
    `snd_nxt_`, was thrown away, and a connection could stall for good once the congestion window stopped it resending everything at once.
- **SACK** (RFC 2018 and RFC 6675), negotiated on the SYN:
  - **Receiver:** out-of-order segments are kept, up to 8 ranges within the receive window (`SequenceRanges`, the bytes in a 16 KiB area
    indexed by sequence number), and ACKs carry SACK blocks, the latest range first.
  - **Sender:** keeps a scoreboard, and in recovery resends only the holes, counting what is in flight without what was SACKed or lost.
  - **Loss rule:** a segment counts as lost once three segments' worth beyond it are SACKed, or after three duplicate ACKs (an ACK that
    brings new SACK information is one). Receive coalescing merges the segments after a gap into one, which is acknowledged once, so three
    duplicate ACKs never came, and only the retransmission timer found the loss, 165 ms later. Now the segment is resent in the next
    millisecond, and alone.
  - After a timeout it falls back to go-back-N.
- **ECN** (RFC 3168), negotiated on the SYN. New data goes out ECT(0); retransmissions and control segments do not. The receiver echoes a CE
  mark with ECE until the sender's CWR. The sender cuts its window as for a loss, once per window, and retransmits nothing.
- **Pacing.** Data leaves at twice the congestion window per smoothed round trip in slow start and 1.2 times it after (Linux's factors),
  with up to 2 ms' worth (at least four segments) as a burst. On a 40 ms path, at most 4 segments leave in any millisecond, where unpaced the
  whole window (11 segments) left at once. On a LAN, where the round trip is under the clock's millisecond, nothing is paced. A stack with
  paced data waiting is polled again within 500 µs.
- **Measured** (`NetworkFairnessTest`, an interface that takes 8 frames per millisecond):
  - Two bulk connections get 46.3 MB and 46.7 MB (Jain's index 1.000).
  - A request/answer connection beside them is answered in 2 steps (2 ms) every time.
  - Without the transmit queue, they got 0.45 MB and 0.21 MB, and the slowest answer took 467 ms.

## Tests

- `NetworkFairnessTest` covers (besides the earlier cases): `SequenceRanges` against a model, across the 2^32 wrap; a lost segment resent at
  once and alone with SACK; CoDel drops, or marks for ECN; ECN end to end; and pacing on a 40 ms path (the fake wire's `delay_us`). It also
  covers:
  - The scheduler: a sparse flow goes ahead of a backlogged one, two bulk flows alternate, frames keep their order through a refusing
    interface, the sojourn limit, and the slot and byte limits drop from the fattest flow.
  - The flow classifier, against every truncation and bit flip of IPv4 and IPv6 frames.
  - The congestion window: initial value, growth, the cut after loss, and recovery.
  - The shared bottleneck above (`FakeNic::frames_per_ms` in `NetworkHarness.hpp` makes one).
- `NetworkThreadTest` also checks the fair lock's turn-taking and the executor's wake-up (async `TcpStream` round trips).
- `NetworkThreadTest`, with real threads (`HostThreads.cpp`) under ThreadSanitizer, runs two `NetworkStackSet`s with service threads, joined by a
  wire whose interfaces interrupt. It checks:
  - Six connections echoing 96 KiB at once, a thread on each side.
  - The loopback ping-pong latency.
  - Datagrams sent while a thread floods the interface.

## Not done

- **One lock per stack**, not per socket or per receive queue, and a multi-queue interface's receive queues (RSS) are handled by that stack's
  one thread. Changing either means making socket lookup, ARP/neighbour state, the TCP manager and every socket safe on their own.
- **CoDel's precision:** it works at the clock's millisecond, so its 5 ms target is coarse.
- **TCP:** no timestamps (RFC 7323), no window scaling (the window stays within 16 KiB), and no RACK.
- **Pacing** only applies on paths whose round trip is at least a millisecond.
- **An ordinary `kernel::Mutex`** elsewhere still lets the releasing thread barge; only the stacks use the fair mode.
