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
#include "Jit/CodeCache.hpp"
#include "Utility/AllocatedView.hpp"

namespace jit {

namespace {

// Smallest power of two >= n (n >= 16), and its log2.
uint64_t round_slots(uint64_t n, uint32_t* log2_out) {
  uint32_t bits = 4;
  while (bits < 40 && (uint64_t{1} << bits) < n) ++bits;
  *log2_out = bits;
  return uint64_t{1} << bits;
}

// Fibonacci hashing: the multiplicative constant spreads the (often aligned, often sequential) guest addresses over the high bits.
uint64_t hash_slot(uint64_t guest_start, uint32_t bits) { return ((guest_start ^ guest_start >> 17) * 0x9E3779B97F4A7C15ull) >> (64 - bits); }

bool overlaps(const CompiledBlock* block, uint64_t start, uint64_t size) {
  // Ranges [a, a+n) and [b, b+m) overlap when each starts before the other ends; written so the sums cannot wrap.
  return block->guest_start - start < size || start - block->guest_start < block->guest_size;
}

}  // namespace

void CompiledBlockDeleter::operator()(CompiledBlock* block) const {
  allocators::IAllocator* allocator = block->allocator;
  if (block->memory) block->memory->release(block->region);
  block->~CompiledBlock();
  allocator->deallocate(block);
}

CodeCache::CodeCache(allocators::IAllocator& allocator, uint64_t slots, kernel::RcuDomain& domain) : allocator_(allocator), domain_(domain) {
  uint32_t bits = 0;
  const uint64_t count = round_slots(slots, &bits);
  slots_ = allocate_view<CompiledBlock*>(allocator_, count);
  if (slots_.empty()) return;
  slots_.fill(nullptr);
  shift_ = bits;
}

CodeCache::~CodeCache() {
  flush();
  domain_.barrier();   // (the deleters of what was retired must run before the cache, whose allocator they use, goes away)
  release_view(allocator_, slots_);
}

uint64_t CodeCache::slot_of(uint64_t guest_start) const { return hash_slot(guest_start, shift_); }

const CompiledBlock* CodeCache::lookup(uint64_t guest_start) const {
  if (slots_.empty()) return nullptr;
  const CompiledBlock* block = kernel::rcu_dereference(&slots_.data()[slot_of(guest_start)]);
  if (block && block->guest_start == guest_start) {
    __atomic_fetch_add(&hits_, 1, __ATOMIC_RELAXED);
    return block;
  }
  __atomic_fetch_add(&misses_, 1, __ATOMIC_RELAXED);
  return nullptr;
}

void CodeCache::publish(CompiledBlock* block) {
  if (slots_.empty()) {
    block->retire(CompiledBlockDeleter{}, domain_);
    return;
  }
  CompiledBlock** slot = &slots_.data()[slot_of(block->guest_start)];
  lock();
  CompiledBlock* old = *slot;
  kernel::rcu_assign_pointer(slot, block);
  if (!old) __atomic_fetch_add(&count_, 1, __ATOMIC_RELAXED);
  unlock();
  if (old) old->retire(CompiledBlockDeleter{}, domain_);
}

uint64_t CodeCache::invalidate(uint64_t start, uint64_t size) {
  uint64_t removed = 0;
  if (size == 0) return 0;
  for (uint64_t i = 0; i < slots_.size(); ++i) {
    CompiledBlock** slot = &slots_.data()[i];
    lock();
    CompiledBlock* block = *slot;
    const bool hit = block && overlaps(block, start, size);
    if (hit) {
      kernel::rcu_assign_pointer(slot, static_cast<CompiledBlock*>(nullptr));
      __atomic_fetch_sub(&count_, 1, __ATOMIC_RELAXED);
    }
    unlock();
    if (hit) {
      block->retire(CompiledBlockDeleter{}, domain_);
      ++removed;
    }
  }
  return removed;
}

uint64_t CodeCache::flush() {
  uint64_t removed = 0;
  for (uint64_t i = 0; i < slots_.size(); ++i) {
    CompiledBlock** slot = &slots_.data()[i];
    lock();
    CompiledBlock* block = *slot;
    if (block) {
      kernel::rcu_assign_pointer(slot, static_cast<CompiledBlock*>(nullptr));
      __atomic_fetch_sub(&count_, 1, __ATOMIC_RELAXED);
    }
    unlock();
    if (block) {
      block->retire(CompiledBlockDeleter{}, domain_);
      ++removed;
    }
  }
  return removed;
}

HotnessTracker::HotnessTracker(allocators::IAllocator& allocator, uint64_t slots, uint32_t threshold) : allocator_(allocator), threshold_(threshold) {
  uint32_t bits = 0;
  const uint64_t count = round_slots(slots, &bits);
  counters_ = allocate_view<uint32_t>(allocator_, count);
  if (counters_.empty()) return;
  counters_.fill(0);
  shift_ = bits;
}

HotnessTracker::~HotnessTracker() { release_view(allocator_, counters_); }

uint64_t HotnessTracker::slot_of(uint64_t guest_start) const { return hash_slot(guest_start, shift_); }

bool HotnessTracker::record(uint64_t guest_start) {
  if (counters_.empty()) return false;
  uint32_t* counter = &counters_.data()[slot_of(guest_start)];
  return __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED) == threshold_;
}

uint32_t HotnessTracker::count(uint64_t guest_start) const {
  if (counters_.empty()) return 0;
  return __atomic_load_n(&counters_.data()[slot_of(guest_start)], __ATOMIC_RELAXED);
}

void HotnessTracker::reset(uint64_t guest_start) {
  if (!counters_.empty()) __atomic_store_n(&counters_.data()[slot_of(guest_start)], 0, __ATOMIC_RELAXED);
}

void HotnessTracker::reset_all() {
  for (uint32_t& c : counters_) __atomic_store_n(&c, 0, __ATOMIC_RELAXED);
}

}  // namespace jit
