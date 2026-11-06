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

// The block cache (docs/BlockCache.md): a write-back cache of a block device's data in pages of `page_bytes`, kept for low latency on many
// cores and fair to the threads that use it.
//
// - Sharded. A page belongs to one of up to 64 shards (16 pages in a row share one, so a run of pages is mostly under one lock), each with
//   its own lock, its own Adaptive Replacement lists (ArcShard.hpp) and its own dirty list. Locks are held only to look up a page and copy
//   it, never across device I/O, so readers on different pages never wait for each other or for the device.
// - Misses are read in runs: the consecutive pages a request misses (up to max_read_bytes) become one device read, straight into the
//   caller's buffer when it covers whole pages. Pages being read are marked, so a second reader of the same page waits for that read
//   instead of reading again, and a write that lands meanwhile is kept (the read fills only blocks that are not valid yet).
// - Writes go to the cache and return. Dirty data is written back in coalesced runs (consecutive dirty blocks across pages and shards, up to
//   max_write_bytes, as one device write) by the flusher once older than expire_us, or once there is a lot of it, and by flush().
// - Fair: a shard with more dirty pages than dirty_percent of it makes the thread writing to it write back first, at that thread's own I/O
//   priority (the device below is the I/O scheduler, ScheduledStorageDevice). So a thread that writes a lot pays for it, and readers and
//   light writers keep their latency. Readers that miss go to the device at their own priority too.
// - Correct under concurrency: a page being read or written back is never evicted (a later read from the device could see data older
//   than the cache had); a page is written back by one run at a time, so writes reach the device in order; discard and write zeroes wait
//   for runs that touch their range and invalidate before and after.
//
// `Env` gives Mutex (lock/unlock), Condition (wait(Mutex&, timeout_us) -> false on timeout, notify_all) and static now_us(). In the kernel
// they are kernel::Mutex and kernel::ConditionVariable (CachedStorageDevice.hpp); the host tests use the standard library's.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "BlockCache/ArcShard.hpp"
#include "ObjectContainer/Array.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace blockcache {

// What the cache reads from and writes to: the device (in the kernel, the I/O scheduler in front of the driver).
class IBlockBackend {
 public:
  virtual bool read(uint64_t lba, ByteView buffer) = 0;
  virtual bool write(uint64_t lba, ConstByteView buffer) = 0;
  virtual bool flush() = 0;
  virtual bool discard(uint64_t lba, uint64_t blocks) = 0;
  virtual bool write_zeroes(uint64_t lba, uint64_t blocks) = 0;

 protected:
  ~IBlockBackend() = default;
};

struct BlockCacheConfig {
  uint32_t block_size = 512;
  uint64_t block_count = 0;
  uint32_t page_bytes = 4096;              // a multiple of block_size, at most 64 blocks
  uint64_t capacity_bytes = 32ull << 20;
  uint32_t shards = 0;                     // a power of two up to kMaxShards; 0 picks one from the capacity
  uint32_t dirty_percent = 25;             // a shard dirtier than this makes its writers write back
  uint32_t background_percent = 10;        // the flusher writes back down to this when the whole cache is dirtier
  uint64_t expire_us = 1000000;            // the flusher writes dirty data older than this
  uint32_t max_write_bytes = 512u << 10;   // the longest coalesced write
  uint32_t max_read_bytes = 256u << 10;    // the longest read of missing pages
};

struct BlockCacheStatistics {
  uint64_t read_hits = 0, read_misses = 0;      // pages a read found / had to read from the device
  uint64_t device_reads = 0, device_read_bytes = 0;
  uint64_t write_pages = 0;                     // pages written into the cache
  uint64_t writebacks = 0, writeback_pages = 0, writeback_bytes = 0;   // coalesced device writes
  uint64_t throttled = 0;                       // runs a writer wrote back itself (it was over the dirty limit)
  uint64_t read_errors = 0, write_errors = 0;
  uint64_t ghost_hits[2] = {};                  // ARC: pages found in B1 / B2
  uint64_t evictions = 0;
  uint64_t full = 0;                            // a page could not be cached at once (everything busy or dirty)
  uint32_t capacity_pages = 0, resident_pages = 0, dirty_pages = 0;
  uint32_t target_t1_pages = 0;                 // ARC's target for T1, summed over the shards
  uint32_t t2_pages = 0;
};

template <class Env>
class BlockCache {
 public:
  static constexpr uint32_t kMaxShards = 64;
  static constexpr uint32_t kMaxRuns = 32;   // writeback runs in flight at once

  explicit BlockCache(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~BlockCache() { release(); }
  BlockCache(const BlockCache&) = delete;
  BlockCache& operator=(const BlockCache&) = delete;

  // False for a geometry it cannot cache (page not a multiple of the block, more than 64 blocks a page) or out of memory.
  bool initialize(IBlockBackend& backend, const BlockCacheConfig& config);
  void release();

  // As IStorageDevice: whole blocks, inside the device.
  bool read(uint64_t lba, ByteView buffer);
  bool write(uint64_t lba, ConstByteView buffer);
  // Writes back everything that was dirty when it was called, waits for runs already going, then flushes the device.
  bool flush();
  // The same without flushing the device (whoever calls it does that its own way, as eject does).
  bool write_back_all();
  bool discard(uint64_t lba, uint64_t blocks);
  bool write_zeroes(uint64_t lba, uint64_t blocks);

  // The flusher's work: writes back what is older than expire_us, then the oldest data while the cache is dirtier than background_percent.
  // Returns the pages written.
  uint64_t write_back_due(uint64_t now_us);
  // True when write_back_due() has something to do now.
  bool writeback_due(uint64_t now_us) const;
  // Forgets everything, dirty data included (the device is gone).
  void drop_all();
  // Called (from a writer, with no lock held) when the cache passes background_percent dirty: wakes the flusher.
  void set_dirty_notifier(void (*notify)(void* context), void* context) {
    notify_ = notify;
    notify_context_ = context;
  }

  BlockCacheStatistics statistics() const;
  uint32_t dirty_pages() const { return __atomic_load_n(&dirty_pages_, __ATOMIC_RELAXED); }
  uint32_t page_bytes() const { return config_.page_bytes; }
  uint32_t shard_count() const { return shard_count_; }

 private:
  using Mutex = typename Env::Mutex;
  using Condition = typename Env::Condition;

  struct Hold {
    explicit Hold(Mutex& m) : mutex(m) { mutex.lock(); }
    ~Hold() { mutex.unlock(); }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
    Mutex& mutex;
  };

  struct Shard {
    explicit Shard(allocators::IAllocator& allocator) : arc(allocator) {}
    Mutex mutex;
    Condition changed;   // a page's read or writeback finished
    ArcShard arc;
  };

  // A writeback run: the pages it claimed and the bytes it writes.
  struct RunItem {
    uint32_t shard = 0, index = kNone;
    uint64_t mask = 0;
  };
  struct RunSlot {
    uint64_t ticket = 0;   // 0: free
    ByteView buffer;       // max_write_bytes, allocated on first use
  };

  struct Filter {
    uint64_t max_seq = ~uint64_t{0};   // pages dirtied at or before this write sequence
    uint64_t dirtied_by_us = ~uint64_t{0};   // ... and at or before this time
  };

  static uint64_t bits(uint32_t first, uint32_t count) {
    return count >= 64 ? ~uint64_t{0} << first : ((uint64_t{1} << count) - 1) << first;
  }
  uint32_t shard_of(uint64_t page) const {
    return static_cast<uint32_t>(((page >> 4) * 0x9E3779B97F4A7C15ull) >> 58) & (shard_count_ - 1);
  }
  // The blocks of a page that exist (the last page of a device may be short).
  uint32_t page_blocks(uint64_t page) const {
    const uint64_t first = page * blocks_per_page_;
    const uint64_t left = config_.block_count - first;
    return left < blocks_per_page_ ? static_cast<uint32_t>(left) : blocks_per_page_;
  }
  bool in_range(uint64_t lba, uint64_t blocks) const {
    return blocks <= config_.block_count && lba <= config_.block_count - blocks;
  }
  void count(uint64_t& counter, uint64_t n = 1) { __atomic_fetch_add(&counter, n, __ATOMIC_RELAXED); }
  void dirty_changed(int64_t delta) {
    if (delta) __atomic_fetch_add(&dirty_pages_, static_cast<uint32_t>(delta), __ATOMIC_RELAXED);
  }

  // Reads the blocks of one page that a request wants from the cache, if they are all there; waits for a read of the page in progress.
  bool read_hit(uint64_t page, uint32_t first, uint32_t blocks, ByteView out);
  // Reads the run of missing pages that starts with the request's next block; returns the blocks of the request it completed (0 when the
  // first page turned out to be a hit after all) or -1 on a device error.
  int64_t read_miss(uint64_t lba, uint64_t blocks, ByteView out);
  // Copies one page's part of a write into the cache, making room by writing back when the shard has none.
  bool write_page(uint64_t page, uint32_t first, uint32_t blocks, ConstByteView data);
  // A writer over the dirty limit of a shard writes back down to the background level of it.
  void balance(uint32_t shard);

  // The span of an entry's dirty blocks one write can take: from its first dirty block over valid blocks to the last dirty one reachable.
  static bool dirty_span(const CacheEntry& e, uint32_t* first, uint32_t* last);
  // Writes back one coalesced run seeded by the oldest dirty page of `shard` that passes `filter`. 0: nothing to write, 1: written, -1: the
  // device failed (the pages are dirty again).
  int write_back_one(uint32_t shard, const Filter& filter);
  // A free run slot (waits for one), with its ticket.
  uint64_t take_run_slot(uint32_t* slot);
  void give_run_slot(uint32_t slot);
  // True if a run that started at or before `ticket` is still going.
  bool runs_before(uint64_t ticket);

  // Takes `range` blocks out of the cache. drop_dirty: dirty data in it goes too (it is being discarded or zeroed); otherwise only clean
  // data, so writes made meanwhile survive. False when a writeback run touching the range is still going (wait and call again).
  bool invalidate(uint64_t lba, uint64_t blocks, bool drop_dirty);
  void invalidate_entry(Shard& s, uint32_t index, uint64_t mask, bool drop_dirty, bool* busy);
  void invalidate_wait(uint64_t lba, uint64_t blocks, bool drop_dirty);

  allocators::IAllocator& allocator_;
  IBlockBackend* backend_ = nullptr;
  BlockCacheConfig config_;
  uint32_t blocks_per_page_ = 0;
  uint32_t shard_count_ = 0;
  uint32_t shard_dirty_limit_ = 0, shard_dirty_target_ = 0;
  uint32_t background_pages_ = 0;
  Array<UniquePointer<Shard>, kMaxShards> shards_;
  uint32_t dirty_pages_ = 0;     // (atomic)
  uint64_t write_seq_ = 0;       // (atomic) bumped each time a page goes from clean to dirty
  Mutex runs_mutex_;
  Condition runs_changed_;
  Array<RunSlot, kMaxRuns> runs_;
  uint64_t next_ticket_ = 0;     // (under runs_mutex_)
  uint32_t flusher_shard_ = 0;   // (atomic) where write_back_due starts, round robin
  void (*notify_)(void*) = nullptr;
  void* notify_context_ = nullptr;
  BlockCacheStatistics stats_;   // the counters (atomic); statistics() adds the rest
};

// ---- implementation ----

template <class Env>
bool BlockCache<Env>::initialize(IBlockBackend& backend, const BlockCacheConfig& config) {
  release();
  if (config.block_size == 0 || config.page_bytes < config.block_size || config.page_bytes % config.block_size ||
      config.page_bytes / config.block_size > 64 || config.block_count == 0 || config.max_write_bytes < config.page_bytes ||
      config.max_read_bytes < config.page_bytes)
    return false;
  config_ = config;
  blocks_per_page_ = config.page_bytes / config.block_size;
  uint64_t pages = config.capacity_bytes / config.page_bytes;
  if (pages < 16) pages = 16;
  if (pages > (uint64_t{1} << 30)) pages = uint64_t{1} << 30;
  uint32_t shards = config.shards;
  if (shards == 0) {
    shards = 1;
    while (shards < kMaxShards && pages / (shards * 2) >= 64) shards *= 2;   // (at least 64 pages a shard)
  }
  if (shards > kMaxShards || (shards & (shards - 1))) return false;
  const uint32_t per_shard = static_cast<uint32_t>((pages + shards - 1) / shards);
  for (uint32_t i = 0; i < shards; ++i) {
    shards_[i] = UniquePointer<Shard>::make(allocator_, allocator_);
    if (!shards_[i] || !shards_[i]->arc.initialize(per_shard, config.page_bytes)) {
      release();
      return false;
    }
  }
  shard_count_ = shards;
  shard_dirty_limit_ = per_shard * config.dirty_percent / 100;
  if (shard_dirty_limit_ < 1) shard_dirty_limit_ = 1;
  shard_dirty_target_ = shard_dirty_limit_ / 2;
  background_pages_ = static_cast<uint32_t>(uint64_t{per_shard} * shards * config.background_percent / 100);
  backend_ = &backend;
  return true;
}

template <class Env>
void BlockCache<Env>::release() {
  for (UniquePointer<Shard>& s : shards_.view()) s = UniquePointer<Shard>();
  for (RunSlot& r : runs_.view()) {
    release_view(allocator_, r.buffer);
    r.ticket = 0;
  }
  shard_count_ = 0;
  backend_ = nullptr;
  dirty_pages_ = 0;
}

template <class Env>
bool BlockCache<Env>::read_hit(uint64_t page, uint32_t first, uint32_t blocks, ByteView out) {
  Shard& s = *shards_[shard_of(page)];
  const uint64_t want = bits(first, blocks);
  Hold hold(s.mutex);
  for (;;) {
    const uint32_t i = s.arc.find(page);
    if (i == kNone) return false;
    const CacheEntry& e = s.arc.entry(i);
    if ((e.valid & want) == want) {
      out.copy_from(ConstByteView(s.arc.data(i).subview(uint64_t{first} * config_.block_size, uint64_t{blocks} * config_.block_size)));
      bool inserted;
      s.arc.access(page, &inserted);   // (a hit: to the front of T2)
      count(stats_.read_hits);
      return true;
    }
    if (!e.loading) return false;
    s.changed.wait(s.mutex, 1000);   // another thread is reading the page: wait for it rather than read it again
  }
}

template <class Env>
int64_t BlockCache<Env>::read_miss(uint64_t lba, uint64_t blocks, ByteView out) {
  struct Item {
    uint32_t shard = 0, index = kNone, generation = 0;
  };
  const uint32_t bs = config_.block_size;
  const uint64_t first_page = lba / blocks_per_page_;
  const uint64_t end_lba = lba + blocks;
  const uint32_t max_pages = config_.max_read_bytes / config_.page_bytes;
  constexpr uint32_t kMaxItems = 256;
  Item items[kMaxItems];
  uint32_t n = 0;
  uint64_t run_blocks = 0;
  for (uint64_t page = first_page; n < max_pages && n < kMaxItems && page * blocks_per_page_ < end_lba; ++page) {
    const uint64_t page_lba = page * blocks_per_page_;
    const uint64_t from = lba > page_lba ? lba : page_lba;
    const uint64_t to = end_lba < page_lba + page_blocks(page) ? end_lba : page_lba + page_blocks(page);
    const uint64_t want = bits(static_cast<uint32_t>(from - page_lba), static_cast<uint32_t>(to - from));
    const uint32_t si = shard_of(page);
    Shard& s = *shards_[si];
    Hold hold(s.mutex);
    uint32_t i = s.arc.find(page);
    if (i != kNone) {
      const CacheEntry& e = s.arc.entry(i);
      // A page that is there, or being read by someone else, ends the run (the first one goes back to read_hit).
      if ((e.valid & want) == want || e.loading) break;
    }
    bool inserted;
    i = s.arc.access(page, &inserted);
    Item& item = items[n++];
    item.shard = si;
    item.index = i;
    if (i != kNone) {
      CacheEntry& e = s.arc.entry(i);
      e.loading = true;
      item.generation = e.generation;
    } else {
      count(stats_.full);
    }
    run_blocks += page_blocks(page);
    count(stats_.read_misses);
  }
  if (n == 0) return 0;

  const uint64_t run_lba = first_page * blocks_per_page_;
  const uint64_t run_bytes = run_blocks * bs;
  // Straight into the caller's buffer when the run is whole pages of it; through a buffer of its own otherwise.
  const bool direct = run_lba == lba && run_lba + run_blocks <= end_lba;
  ByteView bounce;
  if (!direct) bounce = allocate_view<uint8_t>(allocator_, run_bytes, 64);
  const ByteView device = direct ? out.first(run_bytes) : bounce;
  bool ok = !device.empty() && backend_->read(run_lba, device);
  count(stats_.device_reads);
  if (ok) count(stats_.device_read_bytes, run_bytes);
  else count(stats_.read_errors);

  for (uint32_t k = 0; k < n; ++k) {
    const uint64_t page = first_page + k;
    const uint64_t page_lba = page * blocks_per_page_;
    const uint32_t pb = page_blocks(page);
    const uint64_t from = lba > page_lba ? lba : page_lba;
    const uint64_t to = end_lba < page_lba + pb ? end_lba : page_lba + pb;
    const ConstByteView from_device = ok ? ConstByteView(device.subview((page_lba - run_lba) * bs, uint64_t{pb} * bs)) : ConstByteView();
    const ByteView dest = out.subview((from - lba) * bs, (to - from) * bs);
    Shard& s = *shards_[items[k].shard];
    {
      Hold hold(s.mutex);
      const uint32_t i = items[k].index;
      if (i != kNone) {
        CacheEntry& e = s.arc.entry(i);
        e.loading = false;
        if (ok && e.generation == items[k].generation) {
          // Fill the blocks nobody wrote meanwhile, then give the caller the page as the cache has it.
          const ByteView page_data = s.arc.data(i);
          const uint64_t missing = bits(0, pb) & ~e.valid;
          for (uint32_t b = 0; b < pb; ++b)
            if (missing >> b & 1)
              page_data.subview(uint64_t{b} * bs, bs).copy_from(from_device.subview(uint64_t{b} * bs, bs));
          e.valid |= bits(0, pb);
          dest.copy_from(ConstByteView(page_data.subview((from - page_lba) * bs, (to - from) * bs)));
        } else if (ok && !direct) {
          dest.copy_from(from_device.subview((from - page_lba) * bs, (to - from) * bs));
        }
        s.arc.forget_if_empty(i);
      } else if (ok && !direct) {
        dest.copy_from(from_device.subview((from - page_lba) * bs, (to - from) * bs));
      }
    }
    s.changed.notify_all();
  }
  release_view(allocator_, bounce);
  if (!ok) return -1;
  const uint64_t run_end = run_lba + run_blocks;
  return static_cast<int64_t>((run_end < end_lba ? run_end : end_lba) - lba);
}

template <class Env>
bool BlockCache<Env>::read(uint64_t lba, ByteView buffer) {
  const uint32_t bs = config_.block_size;
  if (!backend_ || buffer.size() % bs) return false;
  const uint64_t blocks = buffer.size() / bs;
  if (!in_range(lba, blocks)) return false;
  uint64_t done = 0;
  while (done < blocks) {
    const uint64_t at = lba + done;
    const uint64_t page = at / blocks_per_page_;
    const uint32_t first = static_cast<uint32_t>(at % blocks_per_page_);
    const uint64_t left = blocks - done;
    const uint32_t n = left < page_blocks(page) - first ? static_cast<uint32_t>(left) : page_blocks(page) - first;
    if (read_hit(page, first, n, buffer.subview(done * bs, uint64_t{n} * bs))) {
      done += n;
      continue;
    }
    const int64_t got = read_miss(at, left, buffer.subview(done * bs));
    if (got < 0) return false;
    done += static_cast<uint64_t>(got);   // (0: it became a hit meanwhile, read_hit takes it next time round)
  }
  return true;
}

template <class Env>
bool BlockCache<Env>::write_page(uint64_t page, uint32_t first, uint32_t blocks, ConstByteView data) {
  const uint32_t si = shard_of(page);
  Shard& s = *shards_[si];
  for (uint32_t attempt = 0;; ++attempt) {
    {
      Hold hold(s.mutex);
      bool inserted;
      const uint32_t i = s.arc.access(page, &inserted);
      if (i != kNone) {
        s.arc.data(i).subview(uint64_t{first} * config_.block_size, data.size()).copy_from(data);
        const uint32_t before = s.arc.dirty_pages();
        s.arc.mark_dirty(i, bits(first, blocks), __atomic_add_fetch(&write_seq_, 1, __ATOMIC_RELAXED), Env::now_us());
        dirty_changed(static_cast<int64_t>(s.arc.dirty_pages()) - before);
        count(stats_.write_pages);
        return true;
      }
      count(stats_.full);
    }
    // No page of the shard can be evicted: they are dirty (write some back, at this writer's priority) or busy (wait for them).
    if (write_back_one(si, Filter{}) == 0) {
      Hold hold(s.mutex);
      s.changed.wait(s.mutex, 1000);
    }
    if (attempt > 100000) return false;   // (a device that never completes: give up rather than hang)
  }
}

template <class Env>
bool BlockCache<Env>::write(uint64_t lba, ConstByteView buffer) {
  const uint32_t bs = config_.block_size;
  if (!backend_ || buffer.size() % bs) return false;
  const uint64_t blocks = buffer.size() / bs;
  if (!in_range(lba, blocks)) return false;
  uint64_t done = 0;
  uint32_t last_shard = kNone;
  while (done < blocks) {
    const uint64_t at = lba + done;
    const uint64_t page = at / blocks_per_page_;
    const uint32_t first = static_cast<uint32_t>(at % blocks_per_page_);
    const uint64_t left = blocks - done;
    const uint32_t n = left < page_blocks(page) - first ? static_cast<uint32_t>(left) : page_blocks(page) - first;
    if (!write_page(page, first, n, buffer.subview(done * bs, uint64_t{n} * bs))) return false;
    done += n;
    // Pay for this write's dirt shard by shard, as it moves on.
    const uint32_t si = shard_of(page);
    if (si != last_shard && last_shard != kNone) balance(last_shard);
    last_shard = si;
  }
  if (last_shard != kNone) balance(last_shard);
  if (notify_ && dirty_pages() > background_pages_) notify_(notify_context_);
  return true;
}

template <class Env>
void BlockCache<Env>::balance(uint32_t shard) {
  Shard& s = *shards_[shard];
  uint32_t dirty;
  {
    Hold hold(s.mutex);
    dirty = s.arc.dirty_pages();
  }
  if (dirty <= shard_dirty_limit_) return;
  while (dirty > shard_dirty_target_) {
    if (write_back_one(shard, Filter{}) <= 0) return;
    count(stats_.throttled);
    Hold hold(s.mutex);
    dirty = s.arc.dirty_pages();
  }
}

template <class Env>
bool BlockCache<Env>::dirty_span(const CacheEntry& e, uint32_t* first, uint32_t* last) {
  if (!e.dirty) return false;
  const uint32_t f = static_cast<uint32_t>(__builtin_ctzll(e.dirty));
  uint32_t l = f;
  for (uint32_t b = f; b < 64 && (e.valid >> b & 1); ++b)
    if (e.dirty >> b & 1) l = b;
  *first = f;
  *last = l;
  return true;
}

template <class Env>
uint64_t BlockCache<Env>::take_run_slot(uint32_t* slot) {
  Hold hold(runs_mutex_);
  for (;;) {
    for (uint32_t i = 0; i < kMaxRuns; ++i) {
      RunSlot& r = runs_[i];
      if (r.ticket) continue;
      if (r.buffer.empty()) r.buffer = allocate_view<uint8_t>(allocator_, config_.max_write_bytes, 64);
      if (r.buffer.empty()) continue;
      r.ticket = ++next_ticket_;
      *slot = i;
      return r.ticket;
    }
    runs_changed_.wait(runs_mutex_, 1000);
  }
}

template <class Env>
void BlockCache<Env>::give_run_slot(uint32_t slot) {
  {
    Hold hold(runs_mutex_);
    runs_[slot].ticket = 0;
  }
  runs_changed_.notify_all();
}

template <class Env>
bool BlockCache<Env>::runs_before(uint64_t ticket) {
  Hold hold(runs_mutex_);
  for (;;) {
    bool going = false;
    for (const RunSlot& r : runs_.view())
      if (r.ticket && r.ticket <= ticket) going = true;
    if (!going) return false;
    runs_changed_.wait(runs_mutex_, 1000);
  }
}

template <class Env>
int BlockCache<Env>::write_back_one(uint32_t shard, const Filter& filter) {
  const uint32_t bs = config_.block_size;
  // The seed: the oldest dirty page of the shard that passes the filter and no run is writing.
  uint64_t seed;
  {
    Shard& s = *shards_[shard];
    Hold hold(s.mutex);
    uint32_t i = s.arc.oldest_dirty();
    while (i != kNone && s.arc.entry(i).writeback) i = s.arc.next_dirty(i);
    if (i == kNone) return 0;
    const CacheEntry& e = s.arc.entry(i);
    if (e.dirty_seq > filter.max_seq || e.dirtied_us > filter.dirtied_by_us) return 0;   // (the list is oldest first)
    seed = e.page;
  }
  const uint32_t max_pages = config_.max_write_bytes / config_.page_bytes;
  // Look back for dirty pages that run into the seed, so the write starts where the dirty run does.
  uint64_t start = seed;
  while (start > 0 && seed - start + 1 < max_pages) {
    const uint64_t q = start - 1;
    Shard& s = *shards_[shard_of(q)];
    Hold hold(s.mutex);
    const uint32_t i = s.arc.find(q);
    uint32_t f, l;
    if (i == kNone || s.arc.entry(i).writeback || !dirty_span(s.arc.entry(i), &f, &l) || l != page_blocks(q) - 1) break;
    start = q;
  }

  uint32_t slot;
  take_run_slot(&slot);
  const ByteView buffer = runs_[slot].buffer;
  RunItem items[256];
  const uint32_t limit = max_pages < 256 ? max_pages : 256;
  uint32_t n = 0;
  uint64_t bytes = 0, run_lba = 0;
  for (int pass = 0; pass < 2 && n == 0; ++pass) {
    if (pass == 1) start = seed;   // (the pages before the seed changed meanwhile: start at the seed)
    for (uint64_t q = start; n < limit && q < config_.block_count / blocks_per_page_ + 1; ++q) {
      const uint32_t si = shard_of(q);
      Shard& s = *shards_[si];
      Hold hold(s.mutex);
      const uint32_t i = s.arc.find(q);
      uint32_t f, l;
      if (i == kNone || s.arc.entry(i).writeback || !dirty_span(s.arc.entry(i), &f, &l)) break;
      if (n > 0 && f != 0) break;   // (a gap before this page's dirty blocks)
      CacheEntry& e = s.arc.entry(i);
      const uint64_t span_bytes = uint64_t{l - f + 1} * bs;
      if (n == 0) run_lba = q * blocks_per_page_ + f;
      buffer.subview(bytes, span_bytes).copy_from(ConstByteView(s.arc.data(i).subview(uint64_t{f} * bs, span_bytes)));
      bytes += span_bytes;
      const uint64_t mask = bits(f, l - f + 1);
      e.writeback = true;
      const uint32_t before = s.arc.dirty_pages();
      s.arc.clear_dirty(i, mask);
      dirty_changed(static_cast<int64_t>(s.arc.dirty_pages()) - before);
      items[n++] = RunItem{si, i, mask};
      if (l != page_blocks(q) - 1) break;   // (the run cannot go on past a page that ends early)
    }
  }
  if (n == 0) {
    give_run_slot(slot);
    return 0;
  }
  const bool ok = backend_->write(run_lba, ConstByteView(buffer.first(bytes)));
  count(stats_.writebacks);
  if (ok) {
    count(stats_.writeback_pages, n);
    count(stats_.writeback_bytes, bytes);
  } else {
    count(stats_.write_errors);
  }
  for (uint32_t k = 0; k < n; ++k) {
    Shard& s = *shards_[items[k].shard];
    {
      Hold hold(s.mutex);
      CacheEntry& e = s.arc.entry(items[k].index);
      e.writeback = false;
      if (!ok) {
        // Dirty again (the next flush or the flusher tries again), as a page written just now.
        const uint32_t before = s.arc.dirty_pages();
        s.arc.mark_dirty(items[k].index, items[k].mask, __atomic_add_fetch(&write_seq_, 1, __ATOMIC_RELAXED), Env::now_us());
        dirty_changed(static_cast<int64_t>(s.arc.dirty_pages()) - before);
      }
    }
    s.changed.notify_all();
  }
  give_run_slot(slot);
  return ok ? 1 : -1;
}

template <class Env>
bool BlockCache<Env>::flush() {
  if (!backend_) return false;
  const bool ok = write_back_all();
  return backend_->flush() && ok;
}

template <class Env>
bool BlockCache<Env>::write_back_all() {
  if (!backend_) return false;
  const uint64_t target = __atomic_load_n(&write_seq_, __ATOMIC_RELAXED);
  const uint64_t errors_before = __atomic_load_n(&stats_.write_errors, __ATOMIC_RELAXED);
  Filter filter;
  filter.max_seq = target;
  bool ok = true;
  // Pages dirtied before the flush may be in a run of someone else, or become dirty again while one runs: wait for those runs and look again
  // until nothing at or before `target` is dirty (later writes get later sequence numbers, so this ends).
  for (uint32_t round = 0; round < 64; ++round) {
    for (uint32_t i = 0; i < shard_count_; ++i) {
      int r;
      while ((r = write_back_one(i, filter)) > 0) {}
      if (r < 0) ok = false;
    }
    uint64_t ticket;
    {
      Hold hold(runs_mutex_);
      ticket = next_ticket_;
    }
    runs_before(ticket);
    if (!ok) break;
    bool left = false;
    for (uint32_t i = 0; i < shard_count_ && !left; ++i) {
      Shard& s = *shards_[i];
      Hold hold(s.mutex);
      for (uint32_t e = s.arc.oldest_dirty(); e != kNone && !left; e = s.arc.next_dirty(e))
        if (s.arc.entry(e).dirty_seq <= target) left = true;
    }
    if (!left) break;
  }
  if (__atomic_load_n(&stats_.write_errors, __ATOMIC_RELAXED) != errors_before) ok = false;
  return ok;
}

template <class Env>
bool BlockCache<Env>::writeback_due(uint64_t now_us) const {
  if (dirty_pages() == 0) return false;
  if (dirty_pages() > background_pages_) return true;
  if (now_us < config_.expire_us) return false;
  const uint64_t by = now_us - config_.expire_us;
  for (uint32_t i = 0; i < shard_count_; ++i) {
    Shard& s = *shards_[i];
    Hold hold(s.mutex);
    for (uint32_t e = s.arc.oldest_dirty(); e != kNone; e = s.arc.next_dirty(e)) {
      if (s.arc.entry(e).writeback) continue;
      if (s.arc.entry(e).dirtied_us <= by) return true;
      break;
    }
  }
  return false;
}

template <class Env>
uint64_t BlockCache<Env>::write_back_due(uint64_t now_us) {
  if (!backend_) return 0;
  uint64_t before = __atomic_load_n(&stats_.writeback_pages, __ATOMIC_RELAXED);
  const uint32_t start = __atomic_fetch_add(&flusher_shard_, 1, __ATOMIC_RELAXED);
  if (now_us >= config_.expire_us) {
    Filter filter;
    filter.dirtied_by_us = now_us - config_.expire_us;
    for (uint32_t k = 0; k < shard_count_; ++k) {
      const uint32_t i = (start + k) & (shard_count_ - 1);
      while (write_back_one(i, filter) > 0) {}
    }
  }
  // Too much dirty data overall: the oldest of each shard in turn, down to the background level.
  for (uint32_t k = 0; dirty_pages() > background_pages_ && k < 4 * shard_count_ * 64; ++k) {
    bool any = false;
    for (uint32_t j = 0; j < shard_count_ && dirty_pages() > background_pages_; ++j)
      if (write_back_one((start + j) & (shard_count_ - 1), Filter{}) > 0) any = true;
    if (!any) break;
  }
  return __atomic_load_n(&stats_.writeback_pages, __ATOMIC_RELAXED) - before;
}

template <class Env>
void BlockCache<Env>::invalidate_entry(Shard& s, uint32_t index, uint64_t mask, bool drop_dirty, bool* busy) {
  CacheEntry& e = s.arc.entry(index);
  if (drop_dirty && e.writeback) {
    *busy = true;
    return;
  }
  if (drop_dirty) {
    const uint32_t before = s.arc.dirty_pages();
    s.arc.clear_dirty(index, mask);
    dirty_changed(static_cast<int64_t>(s.arc.dirty_pages()) - before);
    e.valid &= ~mask;
  } else {
    e.valid &= ~(mask & ~e.dirty);
  }
  ++e.generation;   // (a read from the device in progress must not fill the page)
  s.arc.forget_if_empty(index);
}

template <class Env>
bool BlockCache<Env>::invalidate(uint64_t lba, uint64_t blocks, bool drop_dirty) {
  bool busy = false;
  const uint64_t end = lba + blocks;
  const uint64_t first_page = lba / blocks_per_page_;
  const uint64_t last_page = (end - 1) / blocks_per_page_;
  auto mask_of = [&](uint64_t page) {
    const uint64_t page_lba = page * blocks_per_page_;
    const uint64_t from = lba > page_lba ? lba : page_lba;
    const uint64_t to = end < page_lba + blocks_per_page_ ? end : page_lba + blocks_per_page_;
    return bits(static_cast<uint32_t>(from - page_lba), static_cast<uint32_t>(to - from));
  };
  if (last_page - first_page < 4096) {
    for (uint64_t page = first_page; page <= last_page; ++page) {
      Shard& s = *shards_[shard_of(page)];
      Hold hold(s.mutex);
      const uint32_t i = s.arc.find(page);
      if (i != kNone) invalidate_entry(s, i, mask_of(page), drop_dirty, &busy);
    }
  } else {
    // A big range (a trim of a whole volume): look at what is cached instead of at every page of it.
    for (uint32_t k = 0; k < shard_count_; ++k) {
      Shard& s = *shards_[k];
      Hold hold(s.mutex);
      for (uint32_t i = 0; i < s.arc.entry_count(); ++i) {
        const CacheEntry& e = s.arc.entry(i);
        if ((e.list == ArcList::T1 || e.list == ArcList::T2) && e.page >= first_page && e.page <= last_page)
          invalidate_entry(s, i, mask_of(e.page), drop_dirty, &busy);
      }
    }
  }
  return !busy;
}

template <class Env>
void BlockCache<Env>::invalidate_wait(uint64_t lba, uint64_t blocks, bool drop_dirty) {
  while (!invalidate(lba, blocks, drop_dirty)) {
    // A run is writing old data of the range: it must reach the device before the discard or zeroing does.
    uint64_t ticket;
    {
      Hold hold(runs_mutex_);
      ticket = next_ticket_;
    }
    runs_before(ticket);
  }
}

template <class Env>
bool BlockCache<Env>::discard(uint64_t lba, uint64_t blocks) {
  if (!backend_ || blocks == 0 || !in_range(lba, blocks)) return false;
  invalidate_wait(lba, blocks, true);
  const bool ok = backend_->discard(lba, blocks);
  invalidate_wait(lba, blocks, false);   // (what was read in while the device worked)
  return ok;
}

template <class Env>
bool BlockCache<Env>::write_zeroes(uint64_t lba, uint64_t blocks) {
  if (!backend_ || blocks == 0 || !in_range(lba, blocks)) return false;
  invalidate_wait(lba, blocks, true);
  const bool ok = backend_->write_zeroes(lba, blocks);
  invalidate_wait(lba, blocks, false);
  return ok;
}

template <class Env>
void BlockCache<Env>::drop_all() {
  for (uint32_t k = 0; k < shard_count_; ++k) {
    Shard& s = *shards_[k];
    {
      Hold hold(s.mutex);
      for (uint32_t i = 0; i < s.arc.entry_count(); ++i) {
        CacheEntry& e = s.arc.entry(i);
        if (e.list != ArcList::T1 && e.list != ArcList::T2) continue;
        const uint32_t before = s.arc.dirty_pages();
        s.arc.clear_dirty(i, ~uint64_t{0});
        dirty_changed(static_cast<int64_t>(s.arc.dirty_pages()) - before);
        e.valid = 0;
        ++e.generation;
        s.arc.forget_if_empty(i);
      }
    }
    s.changed.notify_all();
  }
}

template <class Env>
BlockCacheStatistics BlockCache<Env>::statistics() const {
  BlockCacheStatistics out;
  out.read_hits = __atomic_load_n(&stats_.read_hits, __ATOMIC_RELAXED);
  out.read_misses = __atomic_load_n(&stats_.read_misses, __ATOMIC_RELAXED);
  out.device_reads = __atomic_load_n(&stats_.device_reads, __ATOMIC_RELAXED);
  out.device_read_bytes = __atomic_load_n(&stats_.device_read_bytes, __ATOMIC_RELAXED);
  out.write_pages = __atomic_load_n(&stats_.write_pages, __ATOMIC_RELAXED);
  out.writebacks = __atomic_load_n(&stats_.writebacks, __ATOMIC_RELAXED);
  out.writeback_pages = __atomic_load_n(&stats_.writeback_pages, __ATOMIC_RELAXED);
  out.writeback_bytes = __atomic_load_n(&stats_.writeback_bytes, __ATOMIC_RELAXED);
  out.throttled = __atomic_load_n(&stats_.throttled, __ATOMIC_RELAXED);
  out.read_errors = __atomic_load_n(&stats_.read_errors, __ATOMIC_RELAXED);
  out.write_errors = __atomic_load_n(&stats_.write_errors, __ATOMIC_RELAXED);
  out.full = __atomic_load_n(&stats_.full, __ATOMIC_RELAXED);
  out.dirty_pages = dirty_pages();
  for (uint32_t k = 0; k < shard_count_; ++k) {
    Shard& s = *shards_[k];
    Hold hold(s.mutex);
    const ArcShardStatistics& a = s.arc.statistics();
    out.ghost_hits[0] += a.ghost_hits[0];
    out.ghost_hits[1] += a.ghost_hits[1];
    out.evictions += a.evictions;
    out.capacity_pages += s.arc.capacity();
    out.resident_pages += s.arc.resident();
    out.target_t1_pages += s.arc.target();
    out.t2_pages += s.arc.list_size(ArcList::T2);
  }
  return out;
}

}  // namespace blockcache
