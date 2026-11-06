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

## NVMe: a queue pair per core

`NvmeStorageDevice` is the first driver with real depth (`StorageInfo::queue_depth` > 1), and it is laid out the way NVMe is meant to be
used on a machine with many cores:

- **Queue pairs.** There is one I/O queue pair per online core, up to 16 and up to what the controller grants (Set Features, Number of
  Queues). A thread submits on its own core's queue, under that queue's lock only. With fewer queues than cores, the cores share them in
  order.
- **Command slots.** There are 64 command slots, or fewer if `CAP.MQES` is small; that number is the queue depth reported to the scheduler.
  Each slot has its own 64 KiB DMA buffer (capped by MDTS) and its own PRP list. A request holds one slot, and a big request moves 64 KiB
  at a time through it. Every queue can hold every slot, so a queue can never overflow.
- **Completions.** On x86 each queue's MSI-X entry is sent to its own core, with a vector of its own from `kVectorStorageFirst` (0xA0–0xBF,
  shared by all controllers). The handler there takes the completions off the queue and wakes the waiting threads, and the thread
  scheduler runs each one where its data arrived. Entry 0, the admin queue's, shares queue 1's vector; the admin queue itself is polled
  and used only at start-up.
- **Fallbacks.** Interrupt delivery is checked on every queue at start. Without MSI-X, before the scheduler runs, or when a check fails, the
  waiting threads poll their own queue instead, still in parallel. A waiter whose 2 ms wait times out looks at the queue itself, so a lost
  interrupt costs time, not the request. A command that outlives the controller's timeout fails, and its slot is retired for good: the
  device may still write into it.
- **Properties.** `describe()` shows the queues, the depth, the largest transfer and the interrupts taken per core. The scheduled device
  adds the scheduler's numbers (requests queued and bypassed, peak in flight, read and write slots).

Checked in QEMU (`build-selftest`, NVMe image with the `TCOS-SCRATCH` marker so the writers run on it) in several shapes: KVM with 4 cores
(4 queues, 16 requests in flight at once), 8 cores on 3 queue pairs (`max_ioqpairs=3`), 1 core, TCG, and polled (`msix_qsize=1`). The
boot log's thread-safety section prints the properties.

## virtio-blk: a request queue per core

`VirtioBlockDevice` follows the same pattern as NVMe:

- **Queues.** With `VIRTIO_BLK_F_MQ` it uses one request queue per online core (QEMU gives one per vCPU), up to 16.
- **Slots.** A pool of up to 64 slots, each with a 64 KiB DMA buffer and its own header and status byte. Slot *s* always uses descriptors
  3*s*..3*s*+2 of whichever queue it goes on, so any slot fits any queue, and the used ring's id names the slot.
- **Interrupts.** On x86, an MSI-X entry per queue goes to that queue's core (`VirtioPciDevice::enable_msix_queues`). The transport now
  handles up to 17 queues.
- **Fallback.** Delivery is checked per queue; without it, waiters poll.

## AHCI: Native Command Queuing

`AhciStorageDevice` uses NCQ when both the HBA (`CAP.SNCQ`) and the drive (IDENTIFY word 76 bit 8) support it:

- **Tags.** As many tags as both sides have, up to 32 (`CAP.NCS`, IDENTIFY word 75). Each tag has its own command table and 64 KiB
  buffer. Commands are READ/WRITE FPDMA QUEUED (count in the features registers, tag in the count register). `PxSACT` is set before
  `PxCI`, under the port's lock.
- **Completions.** They arrive as Set Device Bits FISes; the tags whose `PxSACT` bit cleared are done.
- **Interrupt.** The HBA has one MSI, sent to a core the thread scheduler picks (`choose_io_interrupt_core`). The handler goes through the
  ports in `IS` and lets each drive reap. Without MSI, waiters poll.
- **Non-queued commands.** A cache flush, IDENTIFY or ATAPI packet cannot be queued. Such a command takes an exclusive gate: new queued
  commands wait, and the ones on the drive finish first. It then runs in slot 0, polled.
- **Errors.** A task-file or host-bus error fails every outstanding tag. The next command restarts the port with the gate exclusive, and
  each failed chunk is retried up to three times. A command that never completes restarts the port too, and its tag is retired.
- **Without NCQ.** Optical drives and disks without NCQ keep the single-slot path (`queue_depth = 1`).

- **Error log.** After an NCQ error, the restart reads the NCQ Command Error log (READ LOG EXT, log 10h). A real drive aborts every command
  until that log is read. The tag and LBA it reports appear in the properties.
  - This only happens when the drive advertises General Purpose Logging (IDENTIFY word 84 or 87, bit 5).
  - If the read is aborted, the port is restarted once more: an aborted READ LOG EXT leaves it in error.
  - QEMU's ATA drive advertises no GPL, so this path is untested. QEMU did show that sending the read anyway breaks recovery.
- **Discard.** TRIM is DATA SET MANAGEMENT, not queued, with up to 64 ranges a command, used when IDENTIFY word 169 bit 0 is set.

## virtio-scsi

`VirtioScsiController` keeps the queues and slots for every logical unit behind it:

- **Queues.** One request queue per online core (`num_queues`; request queues start at virtqueue 2, after control and events), each with
  its MSI-X entry on its core (`enable_msix_queues(..., first_queue = 2)`).
- **Slots.** Up to 64 slots shared by the units, each with a 64 KiB buffer and its request and response headers.
- **Commands.** Reads and writes go out in parallel chunks. A chunk interrupted by a unit attention is sent again, up to three times.

## Shared pieces

`NvmeStorageDevice`, `VirtioBlockDevice`, `VirtioScsiController` and `AhciStorageDevice` share `QueuedStorage.hpp`:

- **Vectors.** The completion vectors (0xA0–0xBF), none when the build sets `TCOS_STORAGE_POLLED`.
- **Slots.** A lock-free `SlotPool`.
- **Completion wait.** `CommandCompletion`: sleep on the event, reap the queue yourself when the 2 ms wait times out, poll without
  interrupts, retire the slot on timeout.
- **Parallel chunks.** `acquire_slots` and `run_chunks`: a request over 64 KiB takes its first slot as usual, plus up to 7 more if they are
  free right now, and keeps that many chunks on the device at once.
- **Properties.** `describe_queues`.

**Discard.** `IStorageDevice::discard(lba, blocks)` with `StorageInfo::discard` goes through the I/O scheduler as `IoOp::Discard`, counted
in the "other" domain:

- NVMe: Dataset Management with deallocate, when ONCS bit 2 is set; up to 256 ranges a command.
- virtio-blk: `VIRTIO_BLK_F_DISCARD`, one segment of at most `max_discard_sectors` a request.
- AHCI: TRIM, as above.

**File systems discard what they free.** FAT, ext, NTFS and BeFS each keep a `DiscardBatch` (`src/FileSystem/DiscardBatch.hpp`):

- **What goes in.** Every range they free is recorded in device blocks, and touching ranges merge. A range allocated again before it went
  out is cut out of the batch.
- **When it goes out.** Only in `flush()`, after the device flush has made the freeing durable, as Linux's ext4 does with its discard
  option. After a crash, no file still points at a discarded block, and new data is never discarded.
- **BeFS transactions.** Its frees join the batch uncommitted. `tx_commit` commits them, and a rolled-back or abandoned transaction drops
  them.
- **Partial device blocks.** Frees round inward to whole device blocks, so a device block partly in use (512-byte FAT clusters on a
  4 KiB disk) is never discarded. Reuse rounds outward. FAT hands consecutive clusters over as one run.
- **Bounds.** At most 64 ranges; when the batch is full, the smallest is dropped (discard is only a hint). Nothing is batched on a
  read-only volume or a device without discard. `PartitionStorageDevice` passes discards through, and `StorageStatistics` counts them.
- **Tests.**
  - Each file system's host test runs a workload against a memory disk that overwrites discarded blocks with a poison byte. Every live
    file must read back exactly after each flush and after a remount, and e2fsck (ext) and the NTFS and BeFS checkers must find the
    volume clean. Removing FAT's reuse hook makes this test fail, as it should.
  - `DiscardBatchTest` checks merging, splitting, commit and rollback, the full-batch rule and rounding, plus a random run against a
    block-level model.
  - In the boot self-test, deleting 1 MiB on a scratch FAT32 virtio-blk disk (marked by `TCOSSCR.TXT`) sends one merged 2048-block
    discard.

**Discards do not wait for a flush.** A file system sends its batch early, flushing the device first, once 32 MiB of committed ranges
are waiting or the batch is full of them. `IFileSystem::send_discards()` sends whatever is committed, again after a device flush, and
does nothing when nothing is waiting. `SystemFileSystem`'s service thread calls it for every volume every 5 s.

**fstrim.** `IFileSystem::trim(path, &bytes)` discards all the free space of the volume that holds `path`; a mount table's root means
every volume. It is the shell command `fstrim [folder]`.

- **Order.** Under the file system's lock, it flushes the device (so every free so far is durable), sends the batch, then walks the
  free-space map and discards each free run, rounded inward to whole device blocks.
- **Maps.** FAT: the FAT entries. ext: the group block bitmaps. NTFS: `$Bitmap`. BeFS: the group bitmaps, only with no transaction
  open.
- **Tests.** The host tests trim after the workload, with every live file read back through the poison disk. In the boot self-test, the
  scratch FAT32 volume's 62 MiB of free space is trimmed.

**Scheduled fstrim.** `SystemFileSystem`'s service trims every volume (`MountTable::trim_all`) ten minutes after start-up, then once a
week by the wall clock, as Linux's `fstrim.timer` does.

- **Remembering.** The last run is kept in `/AppStore/FileSystem/fstrim.state` (`layout::kFstrimState`), so a machine started every day
  still trims about weekly.
- **No clock.** Without a working clock (unix time 0, or a clock that went backwards) it trims once per start.
- **Policy.** It lives in `FileSystem/TrimSchedule` (`TrimScheduleTest`). The run is announced on the storage notifier ("Storage: Trimmed
  the free space of the volumes: N MiB").

**Discard on every SCSI transport.** `scsi::probe_unmap` and `scsi::unmap_range` (`ScsiCommands.hpp`) work over any transport's
`run(cdb, data, data_in)`, and virtio-scsi, pvscsi and USB mass storage use them. A USB SSD that is thin provisioned gets discard; a
stick that refuses the commands is left as it was.

**SD erase.** A card whose CSD lists command class 5 gets `discard()` as an SD erase (CMD32 and CMD33 for the range, then CMD38), 8192
blocks at a time, waiting while the card is busy. The SCR's DATA_STAT_AFTER_ERASE says whether erased blocks read as zeros or ones. It is
tested against the fake card in `SdCardTest`: high and standard capacity, chunking, range checks and write protection.

**What discarded blocks read back as.** `StorageInfo::discard_zeroes` comes from:

- NVMe: DLFEAT = 001b.
- ATA: IDENTIFY word 69, bits 14 and 5 (deterministic read after TRIM, and it reads zeros).
- SCSI: READ CAPACITY(16) LBPRZ.
- SD: the erased state.

It appears in the storage demo and the properties. It is for information only: NVMe deallocate and ATA TRIM are advisory (a device may
leave part of a range as it was), so nothing uses a discard to zero data. Zeroing would need Write Zeroes or WRITE SAME. In QEMU only the
NVMe disk claims zeros.

**virtio-scsi discard.**

- **Detection.** UNMAP, when READ CAPACITY(16) reports a thin-provisioned unit (LBPME) and the Logical Block Provisioning VPD page says
  UNMAP works (LBPU). The Block Limits VPD page caps one UNMAP; a zero there means no UNMAP after all, and a missing page gives a
  conservative 2 GiB.
- **Command.** One block descriptor per command (`scsi::build_unmap`).
- **Tests.** The parsers are in ParserFuzzTest, with exact-value checks. In QEMU, `scsi-hd` with `discard=unmap` reports it, and the
  scratch-disk discard test now covers four devices.

**Polled build.** `-DTCOS_STORAGE_POLLED=ON` builds a kernel whose storage drivers never turn on completion interrupts. It is for
machines with broken MSI, and it is how the polled paths, AHCI's included, are tested.

**Checks.** In QEMU, every disk type was present at once: virtio-blk twice, NVMe, virtio-scsi, and SATA twice. The checks, all of which
passed:

- concurrent readers on every disk, and writers on a scratch disk;
- 600 KiB writes read back on every scratch disk;
- a 1100-block read compared with a piecewise read. On the ESP (QEMU's virtual FAT), which the system writes to as it boots, the check
  sees the FAT change between the reads (block 653) and says so.
- discard on the scratch disks that support it.

The configurations: KVM with 4, 8 and 1 cores (8 with `num-queues=3` for virtio-blk and 2 for virtio-scsi), the polled build, TCG, and a
`blkdebug` read error on a SATA disk.

## What is not done

- **Some drivers are synchronous by nature**. USB mass storage uses Bulk-Only Transport, one command at a time; UAS would lift that. SD,
  IDE, and the firmware fallback (`UefiBlockStorageDevice`) also keep `queue_depth = 1`. **pvscsi** has a ring and could be queued like
  virtio-scsi, but is not yet.
- **Copies and slot limits.**
  - Every queued driver copies through slot buffers. Zero-copy needs to know that a caller's buffer is device-reachable, which kernel
    buffers are and user-mode buffers may not be.
  - A big request takes extra slots only if they are free, so on a busy device it runs mostly in series. The I/O scheduler still counts it
    as one request.
- **Discard gaps.**
  - There is no Write Zeroes or WRITE SAME, so file systems still write zeros where they need them.
  - SD uses ERASE, not the faster DISCARD (CMD38 argument 1, SD 3.0).
  - UAS (USB Attached SCSI) is not supported, so USB disks stay one command at a time.
- **Driver features still missing.**
  - NVMe: no interrupt coalescing (left off on purpose, for latency), only the first namespace.
  - AHCI: no port multipliers.
  - virtio-scsi, pvscsi and USB: only the first LUN of each target, and one UNMAP descriptor a command.
  - None of it has been tried on real hardware.
- **Some callers don't pass priorities yet**. Windows `FileIoPriorityHintInformation` and process I/O priority are still ignored, and
  Linux `ioprio_set` is not wired to `set_thread_io_priority`. User processes do not yet put their threads into one `io_group`.
- **Requests are not merged**. Callers are synchronous and own their buffers. On flash, merging matters little.
- **No cgroup-style hierarchy**. The fairness is flat: issuers within a class.
- **No statistics view in the System Monitor yet**. `ScheduledStorageDevice::scheduler_statistics()` has the numbers.
