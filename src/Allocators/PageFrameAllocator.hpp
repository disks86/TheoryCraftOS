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
#include <stdint.h>

namespace allocators {

// Lock-free page frame allocator after LLFree (Wrenger et al., "LLFree: Scalable and Optionally-Persistent Page-Frame Allocation",
// USENIX ATC 2023). Every operation is a handful of compare-and-swaps on words that are mostly private to one core; nothing ever waits
// for another core, so it is safe from threads, interrupt handlers and early boot alike, and a thread preempted halfway holds nobody up.
//
// Each region of RAM handed in becomes an area. Its metadata lives at the region's end (no allocation needed):
//  - a bitfield with one bit per 4 KiB frame, 512 frames (one 2 MiB chunk, eight words) at a time;
//  - a counter of free frames per chunk, claimed before the bits so that a full chunk is skipped with one load;
//  - a counter per tree of 32 chunks (64 MiB) with a "reserved" flag.
// A core reserves a whole tree: it moves the tree's free count into its own reservation word and then allocates from that tree only,
// so cores work in different parts of memory and do not share cache lines. Frees always go back to the tree counter; the reserving core
// pulls them into its reservation when it runs dry, and moves on to another tree when the one it has is used up. Small requests (up to
// 64 frames) and medium ones (up to a chunk) have separate reservations, so that small frames do not fragment the trees the larger ones
// come from. A request no reservation can serve takes frames from any tree directly (stealing from other cores' reservations when it has
// to), and a run longer than a chunk is claimed chunk by chunk with the same accounting.
//
// The core number only spreads cores over reservation slots: two threads that end up on one slot (a thread migrated, an interrupt
// handler, more cores than slots) are still correct, they just share the slot's cache line.
//
// Chunks are 2 MiB aligned in physical memory (frame 0 of an area is the region's start rounded down to 2 MiB; the frames before the
// region are marked used), so a request aligned to up to 2 MiB is served from within a chunk.
class PageFrameAllocator {
 public:
  static constexpr uint64_t kFrameSize = 4096;
  static constexpr uint32_t kFramesPerChunk = 512;
  static constexpr uint32_t kChunksPerTree = 32;
  static constexpr uint32_t kMaxAreas = 64;
  static constexpr uint32_t kCoreSlots = 64;
  static constexpr uint64_t kLowLimit = 1ull << 32;   // areas below it are the low zone (DMA)

  constexpr PageFrameAllocator() = default;
  PageFrameAllocator(const PageFrameAllocator&) = delete;
  PageFrameAllocator& operator=(const PageFrameAllocator&) = delete;

  // Forgets every area (the memory itself is not touched). Not while other cores use the allocator.
  void reset();
  // Where the current core number comes from (kernel::current_core in the kernel). Without one every caller shares slot 0.
  void set_core_source(uint32_t (*core)()) { __atomic_store_n(&core_, core, __ATOMIC_RELEASE); }

  // Takes over [base, base + bytes) (shrunk to whole frames), which must lie entirely below kLowLimit or entirely at or above it, be
  // mapped 1:1 and not be used by anything else. `extra_bytes` more are set aside with the metadata for the caller: `*extra` gets their
  // address (64-byte aligned, zeroed). False if the region is too small or there are kMaxAreas areas already.
  bool add_region(uint64_t base, uint64_t bytes, uint64_t extra_bytes = 0, void** extra = nullptr);

  // `frames` contiguous frames whose address is a multiple of `align_frames` frames (a power of two). Ordinary requests try the areas
  // at or above 4 GiB first; `low_only` stays below. Returns the physical (= virtual) address, or 0.
  uint64_t allocate(uint64_t frames, uint64_t align_frames, bool low_only);
  // Returns what allocate() gave (any part of it, a frame at a time or all at once). False, with nothing freed, if `address` is not in an
  // area or a frame of the range is already free.
  bool free(uint64_t address, uint64_t frames);

  // One word per frame for whoever owns the frame (the heap records there what an allocation is). nullptr outside every area.
  uint64_t* frame_tag(uint64_t address) const;
  // Is `address` the start of a frame in some area?
  bool contains(uint64_t address) const { return frame_tag(address) != nullptr; }

  uint64_t total_bytes() const;        // all the regions handed in, metadata included
  uint64_t free_bytes() const { return free_bytes_low() + free_bytes_high(); }
  uint64_t free_bytes_low() const;     // free frames below 4 GiB, in bytes
  uint64_t free_bytes_high() const;    // ... at or above
  uint32_t area_count() const { return __atomic_load_n(&area_count_, __ATOMIC_ACQUIRE); }

 private:
  struct Area;
  Area* area_of(uint64_t address) const;
  uint32_t slot() const;
  uint64_t free_bytes_in(bool low) const;

  Area* areas_[kMaxAreas] = {};
  uint32_t area_count_ = 0;        // slots of areas_ handed out (an entry is null until its area is ready)
  uint32_t (*core_)() = nullptr;
};

}  // namespace allocators
