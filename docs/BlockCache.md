# Block cache

Every disk the system finds gets a block cache in front of its I/O scheduler (`hardware::CachedStorageDevice`, put there by
`SystemStorageDevices`): file systems → cache → I/O scheduler ([IoScheduling.md](IoScheduling.md)) → driver. Reads are served from memory
where they can be. Writes go to memory and return; dirty data reaches the device later, in long runs. The aims are low latency, many cores
at once, and fairness between the threads that use a disk. Optical drives get no cache, because their medium changes under the same device.

| File | What it holds |
|---|---|
| `src/BlockCache/ArcShard.{hpp,cpp}` | one shard: Adaptive Replacement lists, block masks, the dirty list (no locks; the owner locks) |
| `src/BlockCache/BlockCache.hpp` | the cache: shards and their locks, reads in runs, write-back, coalesced runs, flush, discard |
| `src/Hardware/StorageDevice/CachedStorageDevice.{hpp,cpp}` | the kernel side: kernel mutexes, the writeback thread, eject and shutdown |
| `tests/BlockCacheTest.cpp` | host test (ASan+UBSan and ThreadSanitizer builds) |
| `src/ThreadSafetyDemo.cpp` `test_storage` | boot self-test (`TCOS_SELFTESTS=ON`): repeated reads stay off the driver, cold reads overlap |

## Pages and blocks

The cache works in pages of 4 KiB, or one block when blocks are bigger. A page holds up to 64 device blocks, with two bits per block:

- **valid**: the cache holds the device's data, or newer data;
- **dirty**: the cache's data is newer than the device's (always valid as well).

So a write of one 512-byte block needs no read first: only that block becomes valid and dirty. A read that needs a block that is not valid
reads the whole page and fills in only the blocks that are not valid yet, which keeps the newer data. The last page of a device may be
short.

## Replacement: ARC

Each shard keeps its pages by Adaptive Replacement (Megiddo and Modha, 2003):

- **T1** holds pages used once lately, and **T2** pages used at least twice.
- **B1** and **B2** remember the numbers of pages lately evicted from each list.
- A hit in B1 means T1 was too small, so ARC moves its target size for T1 (`p`) up; a hit in B2 moves it down.

A scan of a big file passes through T1 and does not push the working set out of T2. A loop that is a little bigger than the cache still
gets hits. ARC needs no tuning: `p` adapts to the workload.

A page cannot be evicted while it is being read from the device, being written back, or holding dirty blocks. ARC then takes the next page
from the end of its list that can be evicted (it looks at up to 64), then tries the other list. If neither has one, the caller writes back
or waits.

## Concurrency

- **Shards.** Up to 64 shards, each with its own lock (`kernel::Mutex`), its own ARC lists and its own dirty list. A page's shard comes from a
  hash of its number divided by 16, so 16 pages in a row (64 KiB) share one: a run usually stays under one lock.
- **Short critical sections.** A lock is held to look up a page and copy at most a page in or out, never across device I/O. Readers of
  different pages never wait for each other or for the device. Only one shard lock is held at a time, so there is no lock order to get
  wrong.
- **One read per page.** A miss marks the pages it reads (`loading`). A second reader of the same page waits on the shard's condition
  variable for that read instead of reading again. A write that lands during the read is kept: the read fills only blocks that are still
  not valid.
- **Generations.** Discard, write zeroes and eviction bump a page's generation. A read from the device that started earlier then does not
  fill the page with data that is now stale.

## Reads

The pages a request misses in a row (up to `max_read_bytes`, 256 KiB) become one device read. When the run is whole pages of the
caller's buffer, it goes straight into that buffer and is then copied into the cache. Otherwise it goes through a buffer of its own.
Pages that are hits are copied out under their shard's lock.

## Writes and writeback

A write copies into the cache, marks the blocks dirty, and returns. The dirty data reaches the device by one of these:

- **The writeback thread** (one per device, 64 KiB stack). It wakes every 100 ms, or when the cache passes `background_percent` dirty. It
  writes back what has been dirty for longer than `expire_us` (2 s for fixed disks, 250 ms for removable media), and then the oldest
  data while the cache is dirtier than `background_percent`. It runs at BestEffort with half the default weight, so readers keep their
  latency through the fair queue below.
- **The writer itself.** A shard with more than `dirty_percent` (25%) dirty pages makes the thread writing to it write back, down to half
  that, at its own I/O priority. This is the fairness rule: a thread that writes a lot pays for its writeback, and readers and light
  writers are not held up behind it.
- **`flush()`.** Writes back everything that was dirty when it was called, waits for runs already going, then flushes the device. File
  systems keep their ordering guarantees.

**Coalescing.** A writeback run starts from the oldest dirty page of a shard. It looks back for dirty pages that run into it, then takes
consecutive dirty blocks forward across pages and shards, up to `max_write_bytes` (512 KiB), as one device write. Inside a page the run
covers the first to the last dirty block, as long as the blocks in between are valid. A block that is not valid ends the run, so a write
never puts stale data on the device. 1 MiB written in 4 KiB pieces, in reverse order, reaches the device as 5 writes.

**Order.** While a page is being written back it is marked `writeback`:

- it cannot be evicted, so a later read from the device cannot see data older than the cache had;
- no second run takes it, so two writes of the same blocks are never on the device at once, and the older one cannot land last.

New writes to it during the run are allowed: they dirty it again, and it goes out in a later run. If the device write fails, the blocks
are dirty again and the next flush reports the failure.

**Run slots.** At most 32 runs are in flight at once, each with a 512 KiB buffer that is allocated on first use. A run gets a ticket when it
starts, and `flush()` waits for every ticket up to its own. Pages that were dirtied again during a run are looked at again until none are
older than the flush.

## Discard and write zeroes

Both drop the range from the cache (dirty data included, since it is being discarded or zeroed), wait for writeback runs that touch it,
send the command, and then drop again what was read in meanwhile. That second pass keeps dirty data, which was written after the command
started. A range of more than 4096 pages (a trim of a whole volume) is handled by walking what the shards hold, not every page of the
range.

## Eject, removal and shutdown

- **`eject()`** writes everything back, without a device flush, then calls the device's own eject. That flushes the device, or lets off
  a stick that does not know SYNCHRONIZE CACHE.
- **A device that is gone** (`present()` false) drops everything, dirty data included, as it would have been lost without the cache too.
  That is why removable media keep less dirty data, for a shorter time.
- **Shutdown.** `stop_interrupts()`, which runs before `kernel::stop()`, ends the writeback thread and writes everything back while
  interrupts still work. The final `flush()` after that is polled.

## Sizes

| Device | Cache | Dirty data kept at most | Expiry |
|---|---|---|---|
| fixed disk (NVMe, SATA, virtio, SCSI) | 32 MiB | 25% of a shard | 2 s |
| removable (USB stick, SD card) | 8 MiB | 25% of a shard, flusher at 5% | 250 ms |

Page buffers are allocated as they are first needed. The entries (two per page, for ARC's ghosts) are allocated up front: about 160 bytes
for each 4 KiB page of capacity.

## Properties

`describe()` adds the following to the device's properties: cache size and shards, how much is cached and dirty, hits, misses and hit
rate, pages in T2, writebacks and their average size, writebacks done by busy writers, and errors.

## Tests

- **Host:** `tests/run_host_tests.sh BlockCacheTest`, built with ASan+UBSan and with ThreadSanitizer. It covers:
  - ARC's list sizes and target against a reference model over 20000 accesses, at five capacities, and scan resistance;
  - geometry checks;
  - random reads, writes, flushes, discards and write zeroes against a model of the disk, on a cache 1/12 of the disk's size;
  - coalescing;
  - expiry and writer throttling;
  - device errors, drop-all, and trims of large ranges;
  - eight readers of one missing page;
  - eight threads with a flusher, on a device that completes in random order and checks that no two writes of a block overlap.
- **Boot:** the storage self-test passes with the cache in front of every disk:
  - repeated reads do not reach the driver;
  - cold reads on the devices with a deep queue still reach 12 in flight;
  - the boot counter in `/AppStore/System/system.db` survives a power-button shutdown, written through the cache.

## What is not done

- **No read-ahead.** Sequential readers get one device read per request, not one ahead of them.
- **No memory pressure hook.** Each cache has a fixed size and does not shrink when the system is short of memory, and there is no global
  budget across devices.
- **No per-process dirty accounting.** Throttling is per shard: a light writer that happens to write to a shard a heavy writer filled
  pays for some of the heavy writer's writeback.
- **No write-through mode.** A device whose own cache cannot be flushed loses up to the expiry's worth of writes on a power cut, as with
  any write-back cache.
