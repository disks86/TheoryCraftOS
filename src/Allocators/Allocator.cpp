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

#include "Allocators/Allocator.hpp"

namespace allocators {

// Occupies exactly one block at the start of every run.
struct alignas(Allocator::kBlockSize) Allocator::Header {
  static constexpr uint64_t kFree = 0x46524545'46524545;
  static constexpr uint64_t kUsed = 0x55534544'55534544;
  // Sits just before an over-aligned pointer and leads back to the real block.
  static constexpr uint64_t kAligned = 0x414C4947'414C4947;

  uint64_t magic;
  uint64_t blocks;  // run length including this header block
  Header* next;     // next free run (free runs only)
};
namespace {
constexpr uint64_t kBlock = Allocator::kBlockSize;
}

// Puts [start, end) (block-aligned, at least two blocks) on the free list of its pool, merging with neighbours.
void Allocator::add_run(uint64_t start, uint64_t end) {
  auto* h = reinterpret_cast<Header*>(start);
  h->magic = Header::kUsed;  // deallocate() does the insertion and merging for a used run
  h->blocks = (end - start) / kBlock;
  h->next = nullptr;
  deallocate(reinterpret_cast<uint8_t*>(h) + kBlock);
}

bool Allocator::initialize() {
  static_assert(sizeof(Header) == kBlockSize, "header must fill exactly one block");
  free_low_ = free_high_ = nullptr;
  auto start = reinterpret_cast<uint64_t>(ram_.base());
  uint64_t end = start + ram_.usable_bytes();
  start = (start + kBlock - 1) & ~(kBlock - 1);
  end &= ~(kBlock - 1);
  if (end < start + 2 * kBlock) return false;
  if (start < kLowLimit && end > kLowLimit) return add_region(reinterpret_cast<void*>(start), end - start);
  add_run(start, end);
  return true;
}

bool Allocator::add_region(void* base, uint64_t bytes) {
  uint64_t start = (reinterpret_cast<uint64_t>(base) + kBlock - 1) & ~(kBlock - 1);
  uint64_t end = (reinterpret_cast<uint64_t>(base) + bytes) & ~(kBlock - 1);
  if (!base || bytes == 0 || end < start + 2 * kBlock) return false;
  if (start < kLowLimit && end > kLowLimit) {  // one run per pool
    if (kLowLimit - start >= 2 * kBlock) add_run(start, kLowLimit);
    if (end - kLowLimit >= 2 * kBlock) add_run(kLowLimit, end);
    return true;
  }
  add_run(start, end);
  return true;
}

void* Allocator::allocate_from(Header** list, uint64_t size) {
  if (size == 0 || size > UINT64_MAX - 2 * kBlock) return nullptr;
  const uint64_t need = 1 + (size + kBlock - 1) / kBlock;  // header + payload

  Header** link = list;
  for (Header* h = *list; h; link = &h->next, h = h->next) {
    if (h->blocks < need) continue;
    if (h->blocks - need >= 2) {  // split: keep the tail as a free run
      auto* tail = reinterpret_cast<Header*>(reinterpret_cast<uint8_t*>(h) + need * kBlock);
      tail->magic = Header::kFree;
      tail->blocks = h->blocks - need;
      tail->next = h->next;
      *link = tail;
      h->blocks = need;
    } else {
      *link = h->next;
    }
    h->magic = Header::kUsed;
    h->next = nullptr;
    return reinterpret_cast<uint8_t*>(h) + kBlock;
  }
  return nullptr;
}

void* Allocator::allocate_aligned_from(Header** list, uint64_t size, uint64_t alignment) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
  if (alignment <= kBlock) return allocate_from(list, size);  // blocks are already aligned
  if (size > UINT64_MAX - alignment) return nullptr;

  // Over-allocate, then place a marker block in front of the aligned address.
  auto* raw = static_cast<uint8_t*>(allocate_from(list, size + alignment));
  if (!raw) return nullptr;
  uint64_t aligned = (reinterpret_cast<uint64_t>(raw) + alignment - 1) & ~(alignment - 1);
  if (aligned == reinterpret_cast<uint64_t>(raw)) return raw;

  // aligned >= raw + kBlock, so the marker block lies inside our own payload.
  auto* marker = reinterpret_cast<Header*>(aligned - kBlock);
  marker->magic = Header::kAligned;
  marker->blocks = 0;
  marker->next = reinterpret_cast<Header*>(raw - kBlock);  // the real header
  return reinterpret_cast<void*>(aligned);
}

void* Allocator::allocate(uint64_t size) {
  if (void* p = allocate_from(&free_high_, size)) return p;
  return allocate_from(&free_low_, size);
}

void* Allocator::allocate_aligned(uint64_t size, uint64_t alignment) {
  if (void* p = allocate_aligned_from(&free_high_, size, alignment)) return p;
  return allocate_aligned_from(&free_low_, size, alignment);
}

void* Allocator::allocate_dma(uint64_t size, uint64_t alignment) { return allocate_aligned_from(&free_low_, size, alignment); }

void Allocator::deallocate(void* ptr) {
  if (!ptr) return;
  auto* h = reinterpret_cast<Header*>(static_cast<uint8_t*>(ptr) - kBlock);
  if (h->magic == Header::kAligned) h = h->next;  // over-aligned: free the real block
  if (h->magic != Header::kUsed) return;  // not ours, or a double free

  h->magic = Header::kFree;
  Header** list = list_for(h);
  Header* prev = nullptr;
  Header* cur = *list;
  while (cur && cur < h) { prev = cur; cur = cur->next; }

  h->next = cur;
  if (prev) prev->next = h; else *list = h;

  auto end_of = [](Header* x) {
    return reinterpret_cast<uint8_t*>(x) + x->blocks * kBlock;
  };
  if (cur && end_of(h) == reinterpret_cast<uint8_t*>(cur)) {  // merge forward
    h->blocks += cur->blocks;
    h->next = cur->next;
    cur->magic = 0;
  }
  if (prev && end_of(prev) == reinterpret_cast<uint8_t*>(h)) {  // merge back
    prev->blocks += h->blocks;
    prev->next = h->next;
    h->magic = 0;
  }
}

uint64_t Allocator::free_bytes_of(const Header* list) {
  uint64_t total = 0;
  for (const Header* h = list; h; h = h->next) total += (h->blocks - 1) * kBlock;
  return total;
}

uint64_t Allocator::free_bytes() const { return free_bytes_of(free_low_) + free_bytes_of(free_high_); }
uint64_t Allocator::free_bytes_low() const { return free_bytes_of(free_low_); }
uint64_t Allocator::free_bytes_high() const { return free_bytes_of(free_high_); }

}  // namespace allocators
