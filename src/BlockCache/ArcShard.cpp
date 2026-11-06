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

#include "BlockCache/ArcShard.hpp"
#include "Utility/AllocatedView.hpp"

namespace blockcache {

namespace {
constexpr uint32_t index_of(ArcList list) { return static_cast<uint32_t>(list); }
}  // namespace

bool ArcShard::initialize(uint32_t capacity, uint32_t page_bytes) {
  release();
  if (capacity == 0 || page_bytes == 0 || capacity > (kNone >> 2)) return false;
  // ARC keeps at most `capacity` pages and as many ghosts.
  const uint32_t entries = 2 * capacity;
  uint32_t buckets = 1, shift = 64;
  while (buckets < entries) {
    buckets <<= 1;
    --shift;
  }
  entries_ = allocate_view<CacheEntry>(allocator_, entries);
  buckets_ = allocate_view<uint32_t>(allocator_, buckets);
  buffers_ = allocate_view<ByteView>(allocator_, capacity);
  free_buffers_ = allocate_view<uint32_t>(allocator_, capacity);
  if (entries_.empty() || buckets_.empty() || buffers_.empty() || free_buffers_.empty()) {
    release();
    return false;
  }
  for (CacheEntry& e : entries_) e = CacheEntry{};
  buckets_.fill(kNone);
  for (ByteView& b : buffers_) b = ByteView();
  capacity_ = capacity;
  page_bytes_ = page_bytes;
  bucket_shift_ = shift;
  // Every entry starts on the free list.
  for (uint32_t i = 0; i < entries; ++i) push_front(i, ArcList::Free);
  return true;
}

void ArcShard::release() {
  for (uint32_t i = 0; i < buffers_made_; ++i) release_view(allocator_, buffers_[i]);
  release_view(allocator_, entries_);
  release_view(allocator_, buckets_);
  release_view(allocator_, buffers_);
  release_view(allocator_, free_buffers_);
  free_buffer_count_ = buffers_made_ = capacity_ = page_bytes_ = p_ = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    head_[i] = tail_[i] = kNone;
    size_[i] = 0;
  }
  dirty_head_ = dirty_tail_ = kNone;
  dirty_pages_ = 0;
  stats_ = ArcShardStatistics{};
}

uint32_t ArcShard::bucket(uint64_t page) const {
  return static_cast<uint32_t>((page * 0x9E3779B97F4A7C15ull) >> bucket_shift_);
}

uint32_t ArcShard::lookup(uint64_t page) const {
  if (buckets_.empty()) return kNone;
  for (uint32_t i = buckets_[bucket(page)]; i != kNone; i = entries_[i].hash_next)
    if (entries_[i].page == page) return i;
  return kNone;
}

uint32_t ArcShard::find(uint64_t page) const {
  const uint32_t i = lookup(page);
  return i != kNone && (entries_[i].list == ArcList::T1 || entries_[i].list == ArcList::T2) ? i : kNone;
}

void ArcShard::hash_insert(uint32_t index) {
  uint32_t& head = buckets_[bucket(entries_[index].page)];
  entries_[index].hash_next = head;
  head = index;
}

void ArcShard::hash_remove(uint32_t index) {
  uint32_t* link = &buckets_[bucket(entries_[index].page)];
  while (*link != kNone && *link != index) link = &entries_[*link].hash_next;
  if (*link == index) *link = entries_[index].hash_next;
  entries_[index].hash_next = kNone;
}

void ArcShard::unlink(uint32_t index) {
  CacheEntry& e = entries_[index];
  const uint32_t l = index_of(e.list);
  if (e.prev != kNone) entries_[e.prev].next = e.next; else head_[l] = e.next;
  if (e.next != kNone) entries_[e.next].prev = e.prev; else tail_[l] = e.prev;
  e.prev = e.next = kNone;
  --size_[l];
}

void ArcShard::push_front(uint32_t index, ArcList list) {
  CacheEntry& e = entries_[index];
  const uint32_t l = index_of(list);
  e.list = list;
  e.prev = kNone;
  e.next = head_[l];
  if (head_[l] != kNone) entries_[head_[l]].prev = index; else tail_[l] = index;
  head_[l] = index;
  ++size_[l];
}

uint32_t ArcShard::lru_evictable(ArcList list) const {
  uint32_t i = tail_[index_of(list)];
  for (uint32_t looked = 0; i != kNone && looked < kEvictScan; ++looked, i = entries_[i].prev)
    if (evictable(entries_[i])) return i;
  return kNone;
}

uint32_t ArcShard::take_buffer() {
  if (free_buffer_count_) return free_buffers_[--free_buffer_count_];
  if (buffers_made_ >= capacity_) return kNone;
  buffers_[buffers_made_] = allocate_view<uint8_t>(allocator_, page_bytes_, 64);
  if (buffers_[buffers_made_].empty()) return kNone;
  return buffers_made_++;
}

void ArcShard::give_buffer(uint32_t buffer) {
  if (buffer != kNone) free_buffers_[free_buffer_count_++] = buffer;
}

void ArcShard::evict(uint32_t index, ArcList ghost) {
  CacheEntry& e = entries_[index];
  unlink(index);
  give_buffer(e.buffer);
  e.buffer = kNone;
  e.valid = 0;
  ++e.generation;
  if (ghost == ArcList::Free) {
    hash_remove(index);
    push_front(index, ArcList::Free);
  } else {
    push_front(index, ghost);
  }
  ++stats_.evictions;
}

void ArcShard::delete_lru_ghost(ArcList list) {
  const uint32_t i = tail_[index_of(list)];
  if (i == kNone) return;
  unlink(i);
  hash_remove(i);
  push_front(i, ArcList::Free);
}

bool ArcShard::replace(bool hit_in_b2) {
  const uint32_t t1 = size_[index_of(ArcList::T1)];
  const bool from_t1 = t1 > 0 && (t1 > p_ || (hit_in_b2 && t1 == p_));
  // The list ARC asks for first; when every page it looked at there is busy or dirty, the other one.
  uint32_t i = lru_evictable(from_t1 ? ArcList::T1 : ArcList::T2);
  bool took_t1 = from_t1;
  if (i == kNone) {
    i = lru_evictable(from_t1 ? ArcList::T2 : ArcList::T1);
    took_t1 = !from_t1;
  }
  if (i == kNone) return false;
  evict(i, took_t1 ? ArcList::B1 : ArcList::B2);
  return true;
}

uint32_t ArcShard::take_free_entry() {
  if (head_[index_of(ArcList::Free)] == kNone) {
    // (Only when pages that could not be evicted pushed the lists past ARC's bounds: forget the oldest ghost.)
    delete_lru_ghost(size_[index_of(ArcList::B1)] ? ArcList::B1 : ArcList::B2);
  }
  const uint32_t i = head_[index_of(ArcList::Free)];
  if (i != kNone) unlink(i);
  return i;
}

uint32_t ArcShard::access(uint64_t page, bool* inserted) {
  *inserted = false;
  if (entries_.empty()) return kNone;
  const uint32_t c = capacity_;
  const uint32_t i = lookup(page);
  if (i != kNone && (entries_[i].list == ArcList::T1 || entries_[i].list == ArcList::T2)) {
    // Case I: a hit. To the front of T2.
    unlink(i);
    push_front(i, ArcList::T2);
    ++stats_.hits;
    return i;
  }
  *inserted = true;
  if (i != kNone) {
    // Cases II and III: a ghost hit. Move the target towards the list that would have kept the page, make room, and keep it in T2.
    const bool in_b2 = entries_[i].list == ArcList::B2;
    const uint32_t b1 = size_[index_of(ArcList::B1)], b2 = size_[index_of(ArcList::B2)];
    if (!in_b2) {
      const uint32_t delta = b1 >= b2 ? 1 : b2 / b1;
      p_ = p_ + delta > c ? c : p_ + delta;
    } else {
      const uint32_t delta = b2 >= b1 ? 1 : b1 / b2;
      p_ = p_ > delta ? p_ - delta : 0;
    }
    uint32_t buffer = resident() < c ? take_buffer() : kNone;
    if (buffer == kNone) {
      if (!replace(in_b2) || (buffer = take_buffer()) == kNone) {
        ++stats_.full;
        *inserted = false;
        return kNone;
      }
    }
    ++stats_.ghost_hits[in_b2 ? 1 : 0];
    unlink(i);
    CacheEntry& e = entries_[i];
    e.buffer = buffer;
    e.valid = e.dirty = 0;
    push_front(i, ArcList::T2);
    return i;
  }

  // Case IV: not seen lately.
  const uint32_t t1 = size_[index_of(ArcList::T1)], b1 = size_[index_of(ArcList::B1)];
  bool made_room = false;
  if (t1 + b1 >= c) {
    if (t1 < c) {
      delete_lru_ghost(ArcList::B1);
    } else {
      // T1 alone fills the cache: its LRU page goes without a ghost.
      const uint32_t victim = lru_evictable(ArcList::T1);
      if (victim != kNone) {
        evict(victim, ArcList::Free);
        made_room = true;
      }
    }
  } else if (resident() + ghosts() >= 2 * c) {
    delete_lru_ghost(ArcList::B2);
  }
  uint32_t buffer = resident() < c ? take_buffer() : kNone;
  if (buffer == kNone && !made_room) {
    if (replace(false)) buffer = take_buffer();
  } else if (buffer == kNone) {
    buffer = take_buffer();
  }
  if (buffer == kNone) {
    ++stats_.full;
    *inserted = false;
    return kNone;
  }
  const uint32_t n = take_free_entry();
  if (n == kNone) {   // (cannot happen: there are twice as many entries as buffers)
    give_buffer(buffer);
    ++stats_.full;
    *inserted = false;
    return kNone;
  }
  CacheEntry& e = entries_[n];
  const uint32_t generation = e.generation + 1;
  e = CacheEntry{};
  e.page = page;
  e.buffer = buffer;
  e.generation = generation;
  hash_insert(n);
  push_front(n, ArcList::T1);
  ++stats_.inserts;
  return n;
}

void ArcShard::forget_if_empty(uint32_t index) {
  CacheEntry& e = entries_[index];
  if ((e.list != ArcList::T1 && e.list != ArcList::T2) || e.valid || e.dirty || e.loading || e.writeback) return;
  unlink(index);
  give_buffer(e.buffer);
  e.buffer = kNone;
  ++e.generation;
  hash_remove(index);
  push_front(index, ArcList::Free);
}

void ArcShard::mark_dirty(uint32_t index, uint64_t mask, uint64_t seq, uint64_t now_us) {
  CacheEntry& e = entries_[index];
  e.valid |= mask;
  e.dirty |= mask;
  if (e.in_dirty_list || !e.dirty) return;
  e.in_dirty_list = true;
  e.dirty_seq = seq;
  e.dirtied_us = now_us;
  e.dirty_prev = dirty_tail_;
  e.dirty_next = kNone;
  if (dirty_tail_ != kNone) entries_[dirty_tail_].dirty_next = index; else dirty_head_ = index;
  dirty_tail_ = index;
  ++dirty_pages_;
}

void ArcShard::dirty_unlink(uint32_t index) {
  CacheEntry& e = entries_[index];
  if (e.dirty_prev != kNone) entries_[e.dirty_prev].dirty_next = e.dirty_next; else dirty_head_ = e.dirty_next;
  if (e.dirty_next != kNone) entries_[e.dirty_next].dirty_prev = e.dirty_prev; else dirty_tail_ = e.dirty_prev;
  e.dirty_prev = e.dirty_next = kNone;
  e.in_dirty_list = false;
  --dirty_pages_;
}

void ArcShard::clear_dirty(uint32_t index, uint64_t mask) {
  CacheEntry& e = entries_[index];
  e.dirty &= ~mask;
  if (!e.dirty && e.in_dirty_list) dirty_unlink(index);
}

}  // namespace blockcache
