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

// One shard of the block cache (BlockCache.hpp, docs/BlockCache.md): the pages that hash to it, kept by Adaptive Replacement (ARC,
// Megiddo and Modha 2003). Pages used once sit in T1 and pages used again in T2. B1 and B2 remember pages lately evicted from each (their
// numbers only), and a hit there moves the target size of T1 (`target()`) towards whichever list would have kept the page. So a scan of a
// big file passes through T1 without pushing out the working set in T2, and a loop slightly bigger than the cache still hits.
//
// Each page holds up to 64 device blocks, with a bit per block for "holds the device's data or newer" (valid) and "newer than the
// device" (dirty). A page that is being read from the device, is being written back, or holds dirty blocks cannot be evicted. ARC then
// takes the next page of its list that can be, and if there is none the caller has to write back or wait.
//
// Nothing here locks: the cache holds the shard's lock around every call. All memory for the entries is allocated in initialize(); page
// buffers are allocated as pages are first needed, up to the capacity, and kept until release().
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"

namespace blockcache {

constexpr uint32_t kNone = 0xFFFFFFFFu;

enum class ArcList : uint8_t { Free, T1, T2, B1, B2 };

struct CacheEntry {
  uint64_t page = 0;
  uint64_t valid = 0;          // blocks of the page that hold data
  uint64_t dirty = 0;          // blocks newer than the device (always also valid)
  uint64_t dirty_seq = 0;      // the cache's write sequence when the page last went from clean to dirty (flush() writes those up to its own)
  uint64_t dirtied_us = 0;     // and when that was (the flusher writes pages older than the expiry)
  uint32_t hash_next = kNone;
  uint32_t prev = kNone, next = kNone;             // its ARC list (prev towards the MRU end), or the free list
  uint32_t dirty_prev = kNone, dirty_next = kNone; // the shard's dirty pages, oldest first
  uint32_t buffer = kNone;     // its page buffer while in T1 or T2
  uint32_t generation = 0;     // bumped by invalidate(): a read from the device that started before must not fill the page
  ArcList list = ArcList::Free;
  bool loading = false;        // a thread is reading it from the device
  bool writeback = false;      // a thread is writing it to the device
  bool in_dirty_list = false;
};

struct ArcShardStatistics {
  uint64_t hits = 0;           // access() found the page in T1 or T2
  uint64_t ghost_hits[2] = {}; // ... in B1, B2
  uint64_t inserts = 0;        // ... nowhere
  uint64_t evictions = 0;
  uint64_t full = 0;           // no page could be evicted
};

class ArcShard {
 public:
  explicit ArcShard(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~ArcShard() { release(); }
  ArcShard(const ArcShard&) = delete;
  ArcShard& operator=(const ArcShard&) = delete;

  // Room for `capacity` pages of `page_bytes` (and as many remembered ones). False if out of memory or a size is 0.
  bool initialize(uint32_t capacity, uint32_t page_bytes);
  void release();

  // The entry of a page in T1 or T2, or kNone. Changes nothing.
  uint32_t find(uint64_t page) const;
  // A use of `page`: a hit moves it to the front of T2, a miss makes room by ARC's rules and gives it a page buffer (with nothing valid).
  // kNone if room could not be made because no page can be evicted now. *inserted: the page was not in T1 or T2 before.
  uint32_t access(uint64_t page, bool* inserted);
  // Drops a page of T1 or T2 that holds nothing (no valid blocks) and is not being read or written, without remembering it.
  void forget_if_empty(uint32_t index);

  CacheEntry& entry(uint32_t index) { return entries_[index]; }
  const CacheEntry& entry(uint32_t index) const { return entries_[index]; }
  // The page buffer of an entry in T1 or T2.
  ByteView data(uint32_t index) { return buffers_[entries_[index].buffer]; }

  // Dirty bookkeeping: marks blocks dirty (and valid); a page that was clean joins the end of the dirty list with `seq` and `now_us`.
  void mark_dirty(uint32_t index, uint64_t mask, uint64_t seq, uint64_t now_us);
  // Clears dirty blocks; a page with none left leaves the dirty list.
  void clear_dirty(uint32_t index, uint64_t mask);
  uint32_t oldest_dirty() const { return dirty_head_; }
  uint32_t next_dirty(uint32_t index) const { return entries_[index].dirty_next; }

  uint32_t capacity() const { return capacity_; }
  uint32_t resident() const { return size_[1] + size_[2]; }
  uint32_t ghosts() const { return size_[3] + size_[4]; }
  uint32_t dirty_pages() const { return dirty_pages_; }
  uint32_t target() const { return p_; }   // ARC's target size of T1
  uint32_t list_size(ArcList list) const { return size_[static_cast<uint32_t>(list)]; }
  uint32_t entry_count() const { return static_cast<uint32_t>(entries_.size()); }
  const ArcShardStatistics& statistics() const { return stats_; }

 private:
  static constexpr uint32_t kEvictScan = 64;   // pages looked at from the end of a list for one that can be evicted

  bool evictable(const CacheEntry& e) const {
    return (e.list == ArcList::T1 || e.list == ArcList::T2) && !e.loading && !e.writeback && e.dirty == 0;
  }
  uint32_t bucket(uint64_t page) const;
  uint32_t lookup(uint64_t page) const;   // in any list
  void hash_insert(uint32_t index);
  void hash_remove(uint32_t index);
  void unlink(uint32_t index);                 // from its list
  void push_front(uint32_t index, ArcList list);
  uint32_t lru_evictable(ArcList list) const;  // from the LRU end, kNone if none in the first kEvictScan
  // ARC's REPLACE: moves the LRU page (that can be evicted) of T1 or T2 into its ghost list. False if neither has one.
  bool replace(bool hit_in_b2);
  void evict(uint32_t index, ArcList ghost);   // to `ghost`, or to Free when it is ArcList::Free
  void delete_lru_ghost(ArcList list);
  uint32_t take_free_entry();
  uint32_t take_buffer();                      // kNone when out of buffers
  void give_buffer(uint32_t buffer);
  void dirty_unlink(uint32_t index);

  allocators::IAllocator& allocator_;
  ArrayView<CacheEntry> entries_;
  ArrayView<uint32_t> buckets_;
  ArrayView<ByteView> buffers_;      // allocated as needed: buffers_[i] is empty until then
  ArrayView<uint32_t> free_buffers_; // a stack of indices into buffers_
  uint32_t free_buffer_count_ = 0;
  uint32_t buffers_made_ = 0;
  uint32_t page_bytes_ = 0;
  uint32_t capacity_ = 0;
  uint32_t bucket_shift_ = 0;
  uint32_t p_ = 0;
  uint32_t head_[5] = {kNone, kNone, kNone, kNone, kNone};   // MRU end of each list
  uint32_t tail_[5] = {kNone, kNone, kNone, kNone, kNone};   // LRU end
  uint32_t size_[5] = {};
  uint32_t dirty_head_ = kNone, dirty_tail_ = kNone;
  uint32_t dirty_pages_ = 0;
  ArcShardStatistics stats_;
};

}  // namespace blockcache
