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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Allocators/PageFrameAllocator.hpp"
#include "Hardware/RandomAccessMemory/IRandomAccessMemory.hpp"

namespace allocators {

// Default allocator, lock-free and safe from every core, thread and interrupt handler at once (no ThreadSafeAllocator needed).
//
// Memory comes from a PageFrameAllocator (LLFree): RAM is managed in 4 KiB frames, in a low pool (below 4 GiB, where initialize() puts
// the RAM's main region) and a high one (add_region() puts everything at or above 4 GiB there). allocate() and allocate_aligned() take
// from the high pool first, so that the scarce low memory stays for what devices can only reach there; allocate_dma() takes from the low
// pool only.
//
//  - Requests up to kMaxSmall bytes come from size classes (32 bytes to 16 KiB). Each class carves spans of a few frames into objects;
//    free objects sit on lists ("magazines"). Every core has one magazine per class, which a caller takes with one atomic exchange,
//    uses privately and puts back with one compare-and-swap; a full or missing magazine goes to or comes from a small per-class depot of
//    exchange slots. Exchanges hand out exclusive ownership, so there is no ABA problem and no lock, and a thread preempted (or an
//    interrupt handler run) in the middle only makes the next caller on that core use the depot.
//  - Larger requests, and those aligned to more than a frame, are whole frames from the frame allocator.
// Every frame has a tag word (kept by the frame allocator) saying whether it heads a large allocation (and how many frames it has) or
// belongs to a span (and where its header is), so allocations need no header: the memory is all payload, and page-aligned requests
// cost one page.
//
// Spans whose objects are all free go back to the frame allocator when trim() runs, or by themselves when a request finds no memory.
// Objects keep their natural alignment: a size that is a multiple of 2^k gets an address that is a multiple of min(2^k, 64), and
// allocate_aligned() picks a class whose objects are multiples of the alignment (up to 4 KiB; beyond that it uses frames).
class Allocator final : public IAllocator {
 public:
  static constexpr uint64_t kLowLimit = PageFrameAllocator::kLowLimit;  // what DMA-capable memory must stay below
  static constexpr uint64_t kMaxSmall = 16 * 1024;                       // the largest request served from a size class
  static constexpr uint32_t kClassCount = 35;

  explicit constexpr Allocator(hardware::IRandomAccessMemory& ram) : ram_(ram) {}

  // Claims the RAM's usable region. Returns false if it is too small.
  bool initialize();
  // Adds more memory (page-aligned, writable, mapped 1:1 and not used by anything else). A region that crosses 4 GiB is split there.
  // False if it is too small to be worth an area.
  bool add_region(void* base, uint64_t bytes);
  // Where the current core number comes from (kernel::current_core once the kernel runs): it picks the per-core magazines and the frame
  // allocator's reservations. Without it every caller shares the first core's (still correct, only slower under contention).
  void set_core_source(uint32_t (*core)());

  void* allocate(uint64_t size) override;
  void* allocate_aligned(uint64_t size, uint64_t alignment) override;
  void* allocate_dma(uint64_t size, uint64_t alignment) override;
  void deallocate(void* ptr) override;

  // Gives the frames of spans whose objects are all free back to the frame allocator, so free_bytes() counts them again (a leak check
  // compares it before and after: call this before each reading). Walks every cached free object; one caller at a time does the work,
  // a second returns at once.
  void trim() { reclaim(); }

  uint64_t total_bytes() const { return frames_.total_bytes(); }  // everything handed to the pools (metadata included)
  uint64_t free_bytes() const { return frames_.free_bytes(); }    // free frames in both pools (objects cached in spans count as used)
  uint64_t free_bytes_low() const { return frames_.free_bytes_low(); }    // ... in the pool below 4 GiB
  uint64_t free_bytes_high() const { return frames_.free_bytes_high(); }  // ... in the pool above

  PageFrameAllocator& frames() { return frames_; }

 private:
  struct Span;
  struct FreeObject;
  struct Caches;

  void* allocate_small(uint32_t cls, bool dma);
  void* allocate_frames(uint64_t size, uint64_t alignment, bool dma);
  void* allocate_with_reclaim(uint64_t size, uint64_t alignment, bool dma);
  void free_small(Span* span, void* ptr);
  FreeObject* new_span(uint32_t cls, bool dma);
  FreeObject* depot_pop(bool dma, uint32_t cls);
  void depot_push(bool dma, uint32_t cls, FreeObject* list);
  FreeObject** core_cell(bool dma, uint32_t cls);
  Span* span_of(const void* object) const;
  bool add_area(uint64_t base, uint64_t bytes);
  void reclaim();

  hardware::IRandomAccessMemory& ram_;
  PageFrameAllocator frames_;
  Caches* caches_ = nullptr;      // in the metadata of the first area
  uint32_t (*core_)() = nullptr;
  uint32_t reclaiming_ = 0;       // someone is in reclaim()
};

}  // namespace allocators
