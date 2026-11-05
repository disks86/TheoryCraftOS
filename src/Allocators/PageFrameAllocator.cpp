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

#include "Allocators/PageFrameAllocator.hpp"

namespace allocators {

namespace {

constexpr uint64_t kFrame = PageFrameAllocator::kFrameSize;
constexpr uint32_t kChunk = PageFrameAllocator::kFramesPerChunk;
constexpr uint32_t kTree = PageFrameAllocator::kChunksPerTree;
constexpr uint32_t kWords = kChunk / 64;            // bitfield words per chunk
constexpr uint64_t kChunkBytes = kChunk * kFrame;   // 2 MiB
constexpr uint32_t kReserved = 1u << 31;            // tree entry: some core holds the tree
constexpr uint32_t kCountMask = kReserved - 1;
constexpr uint32_t kNone = ~0u;
constexpr uint32_t kSmall = 0, kMedium = 1;         // reservation kinds: up to 64 frames, up to a chunk

template <class T> T load(const T* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
template <class T> bool cas(T* p, T& expected, T desired) {
  return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
template <class T> T min_of(T a, T b) { return a < b ? a : b; }

// A reservation word: (free frames << 32) | (tree + 1); 0 when the slot holds no tree.
uint64_t encode(uint32_t tree, uint32_t free) { return (uint64_t{free} << 32) | (tree + 1); }
uint32_t tree_of(uint64_t r) { return static_cast<uint32_t>(r) - 1; }
uint32_t count_of(uint64_t r) { return static_cast<uint32_t>(r >> 32); }

uint64_t bits_mask(uint32_t first, uint32_t n) { return (n == 64 ? ~uint64_t{0} : ((uint64_t{1} << n) - 1)) << first; }

// Bit positions of a word that are multiples of `align` (1..64).
uint64_t align_mask(uint32_t align) {
  uint64_t m = 0;
  for (uint32_t p = 0; p < 64; p += align) m |= uint64_t{1} << p;
  return m;
}

// Positions where `n` (1..64) consecutive bits of `free` are set (each AND doubles the length checked; bits shifted in from the top are 0,
// so a run that would leave the word is never reported).
uint64_t run_starts(uint64_t free, uint32_t n) {
  uint64_t r = free;
  uint32_t len = 1;
  while (len * 2 <= n) {
    r &= r >> len;
    len *= 2;
  }
  if (n > len) r &= r >> (n - len);
  return r;
}

// The first bit at or after `from` (< 512) in `w` that is `set` (or clear when !set), or kChunk.
uint32_t next_bit(const uint64_t (&w)[kWords], uint32_t from, bool set) {
  for (uint32_t word = from / 64; word < kWords; ++word) {
    uint64_t v = set ? w[word] : ~w[word];
    if (word == from / 64) v &= ~uint64_t{0} << (from % 64);
    if (v) return word * 64 + __builtin_ctzll(v);
  }
  return kChunk;
}

// The first start (a multiple of `align`) of `n` clear bits in the chunk `w`, or kNone. Steps over whole runs of set bits.
uint32_t find_range(const uint64_t (&w)[kWords], uint32_t n, uint32_t align) {
  uint32_t pos = 0;
  for (;;) {
    pos = (pos + align - 1) & ~(align - 1);
    if (pos + n > kChunk) return kNone;
    const uint32_t set = next_bit(w, pos, true);
    if (set >= pos + n) return pos;
    pos = next_bit(w, set, false);
    if (pos >= kChunk) return kNone;
  }
}

// The area's metadata sits in memory the allocator owns (the end of the region it describes). Every object placed there is made of plain
// integers (implicit-lifetime, no constructor), the address is 64-byte aligned, and the memory stays reserved for the area's lifetime,
// which is the program's.
template <class T> T* place(uint64_t address) { return reinterpret_cast<T*>(address); }

}  // namespace

struct alignas(64) CoreSlot {
  uint64_t reservation[2];   // [kind]: see encode()
  uint32_t hint[2];          // the chunk the last allocation of that kind came from
};

struct PageFrameAllocator::Area {
  uint64_t base;            // physical address of frame 0 (2 MiB aligned)
  uint64_t first, last;     // the region handed in, shrunk to whole frames: [first, last)
  uint32_t frames;          // frames from base to last
  uint32_t chunks, trees;
  bool low;
  uint64_t* bits;           // chunks * kWords
  uint32_t* chunk_free;     // chunks
  uint32_t* tree;           // trees: free count | kReserved
  uint64_t* tags;           // frames
  uint32_t run_hint;
  CoreSlot slots[kCoreSlots];

  uint32_t chunks_in(uint32_t t) const { return min_of(kTree, chunks - t * kTree); }

  // --- counters -------------------------------------------------------------------------------------------------------------------
  // Takes up to `want` frames from the tree counter; returns how many.
  uint32_t tree_take(uint32_t t, uint32_t want) {
    uint32_t e = load(&tree[t]);
    for (;;) {
      const uint32_t take = min_of(e & kCountMask, want);
      if (!take) return 0;
      if (cas(&tree[t], e, e - take)) return take;
    }
  }
  void tree_add(uint32_t t, uint32_t n) { __atomic_fetch_add(&tree[t], n, __ATOMIC_ACQ_REL); }
  // Clears the reserved flag and gives the tree back `n` frames.
  void unreserve(uint32_t t, uint32_t n) {
    uint32_t e = load(&tree[t]);
    while (!cas(&tree[t], e, (e & kCountMask) + n)) {}
  }
  // Accounts for `n` frames of tree `t`: from its counter, and when that is short from the reservations that hold the tree.
  bool take_accounting(uint32_t t, uint32_t n) {
    uint32_t got = tree_take(t, n);
    for (uint32_t s = 0; got < n && s < kCoreSlots; ++s) {
      for (uint64_t& res : slots[s].reservation) {
        uint64_t r = load(&res);
        while (got < n && r && tree_of(r) == t && count_of(r)) {
          const uint32_t take = min_of(count_of(r), n - got);
          if (cas(&res, r, r - (uint64_t{take} << 32))) got += take;
        }
      }
    }
    if (got < n) {
      if (got) tree_add(t, got);
      return false;
    }
    return true;
  }
  bool chunk_take(uint32_t c, uint32_t n) {
    uint32_t e = load(&chunk_free[c]);
    for (;;) {
      if (e < n) return false;
      if (cas(&chunk_free[c], e, e - n)) return true;
    }
  }

  // --- bits -----------------------------------------------------------------------------------------------------------------------
  // Calls fn(word, mask) for each word that bits [first, first + n) of chunk c touch.
  template <class F>
  void for_each_word(uint32_t c, uint32_t first, uint32_t n, F&& fn) {
    for (uint32_t at = first; at < first + n;) {
      const uint32_t lo = at % 64, len = min_of(64 - lo, first + n - at);
      if (!fn(&bits[c * kWords + at / 64], bits_mask(lo, len))) return;
      at += len;
    }
  }
  void clear_bits(uint32_t c, uint32_t first, uint32_t n) {
    for_each_word(c, first, n, [](uint64_t* w, uint64_t mask) { __atomic_fetch_and(w, ~mask, __ATOMIC_ACQ_REL); return true; });
  }
  bool all_set(uint32_t c, uint32_t first, uint32_t n) {
    bool ok = true;
    for_each_word(c, first, n, [&](uint64_t* w, uint64_t mask) { ok = (load(w) & mask) == mask; return ok; });
    return ok;
  }
  // Sets bits [first, first + n) of chunk c, all of which must be clear; on a conflict undoes what it set and returns false.
  bool claim_bits(uint32_t c, uint32_t first, uint32_t n) {
    uint32_t done = 0;
    bool ok = true;
    for_each_word(c, first, n, [&](uint64_t* w, uint64_t mask) {
      uint64_t v = load(w);
      for (;;) {
        if (v & mask) return ok = false;
        if (cas(w, v, v | mask)) break;
      }
      done += __builtin_popcountll(mask);
      return true;
    });
    if (!ok && done) clear_bits(c, first, done);
    return ok;
  }

  // --- placing frames -------------------------------------------------------------------------------------------------------------
  uint32_t place_in_chunk(uint32_t c, uint32_t n, uint32_t align) {
    if (!chunk_take(c, n)) return kNone;
    for (int attempt = 0; attempt < 8; ++attempt) {
      uint64_t w[kWords];
      for (uint32_t i = 0; i < kWords; ++i) w[i] = load(&bits[c * kWords + i]);
      uint32_t at = kNone;
      if (n <= 64 && align <= 64) {
        const uint64_t aligned = align_mask(align);
        for (uint32_t i = 0; i < kWords && at == kNone; ++i)
          if (const uint64_t starts = run_starts(~w[i], n) & aligned) at = i * 64 + __builtin_ctzll(starts);
      }
      if (at == kNone && n > 1) at = find_range(w, n, align);   // (a run across a word boundary, or longer than a word)
      if (at == kNone) break;
      if (claim_bits(c, at, n)) return c * kChunk + at;
    }
    __atomic_fetch_add(&chunk_free[c], n, __ATOMIC_ACQ_REL);
    return kNone;
  }
  uint32_t place_in_tree(uint32_t t, uint32_t n, uint32_t align, uint32_t hint) {
    const uint32_t first = t * kTree, count = chunks_in(t);
    const uint32_t start = hint >= first && hint < first + count ? hint - first : 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t f = place_in_chunk(first + (start + i) % count, n, align);
      if (f != kNone) return f;
    }
    return kNone;
  }

  // --- reservations ---------------------------------------------------------------------------------------------------------------
  // Reserves a tree other than `after` with at least `n` free frames for slot s: small requests prefer trees already in use, medium ones
  // trees still entirely free (LLFree's policy against fragmentation). The slot's previous tree is released.
  bool reserve(CoreSlot& s, uint32_t kind, uint32_t n, uint32_t after) {
    for (int pass = 0; pass < 2; ++pass) {
      for (uint32_t i = 0; i < trees; ++i) {
        const uint32_t t = (after + 1 + i) % trees;
        uint32_t e = load(&tree[t]);
        for (;;) {
          const uint32_t free = e & kCountMask;
          if ((e & kReserved) || free < n) break;
          const bool whole = free == chunks_in(t) * kChunk;
          if (pass == 0 && (kind == kSmall ? whole : !whole)) break;
          if (!cas(&tree[t], e, kReserved)) continue;
          uint64_t old = load(&s.reservation[kind]);
          while (!cas(&s.reservation[kind], old, encode(t, free))) {}
          __atomic_store_n(&s.hint[kind], t * kTree, __ATOMIC_RELAXED);
          if (old) unreserve(tree_of(old), count_of(old));
          return true;
        }
      }
    }
    return false;
  }
  uint32_t alloc_reserved(uint32_t slot, uint32_t kind, uint32_t n, uint32_t align) {
    CoreSlot& s = slots[slot];
    uint64_t* res = &s.reservation[kind];
    const uint32_t spread = trees ? (slot * 7) % trees : 0;   // (where a slot's first search starts, so cores begin apart)
    for (uint32_t step = 0; step < trees + 4; ++step) {
      const uint64_t r = load(res);
      if (!r) {
        if (!reserve(s, kind, n, spread)) break;
        continue;
      }
      const uint32_t t = tree_of(r);
      if (count_of(r) >= n) {
        uint64_t expected = r;
        if (!cas(res, expected, r - (uint64_t{n} << 32))) continue;
        const uint32_t f = place_in_tree(t, n, align, __atomic_load_n(&s.hint[kind], __ATOMIC_RELAXED));
        if (f != kNone) {
          __atomic_store_n(&s.hint[kind], f / kChunk, __ATOMIC_RELAXED);
          return f;
        }
        tree_add(t, n);   // the tree has the frames but not in one piece that fits: try another
        if (!reserve(s, kind, n, t)) break;
        continue;
      }
      // Pull in what was freed on the tree since, else move to another tree.
      if (const uint32_t got = tree_take(t, kCountMask)) {
        uint64_t expected = r;
        if (!cas(res, expected, r + (uint64_t{got} << 32))) tree_add(t, got);
        continue;
      }
      if (!reserve(s, kind, n, t)) break;
    }
    // No tree to reserve has enough: take the frames from any tree, reserved or not.
    for (uint32_t i = 0; i < trees; ++i) {
      const uint32_t t = (spread + i) % trees;
      if (!take_accounting(t, n)) continue;
      const uint32_t f = place_in_tree(t, n, align, t * kTree);
      if (f != kNone) return f;
      tree_add(t, n);
    }
    return kNone;
  }

  // --- runs -----------------------------------------------------------------------------------------------------------------------
  // Gives frames [f, f + n) back to their chunks and trees (their bits must be set).
  void release(uint32_t f, uint32_t n) {
    for (uint32_t at = f; at < f + n;) {
      const uint32_t c = at / kChunk, len = min_of((c + 1) * kChunk, f + n) - at;
      clear_bits(c, at % kChunk, len);
      __atomic_fetch_add(&chunk_free[c], len, __ATOMIC_ACQ_REL);
      tree_add(c / kTree, len);
      at += len;
    }
  }
  bool claim_run(uint32_t f, uint32_t n) {
    uint32_t at = f;
    while (at < f + n) {
      const uint32_t c = at / kChunk, len = min_of((c + 1) * kChunk, f + n) - at, t = c / kTree;
      if (!take_accounting(t, len)) break;
      if (!chunk_take(c, len)) {
        tree_add(t, len);
        break;
      }
      if (!claim_bits(c, at % kChunk, len)) {
        __atomic_fetch_add(&chunk_free[c], len, __ATOMIC_ACQ_REL);
        tree_add(t, len);
        break;
      }
      at += len;
    }
    if (at == f + n) return true;
    if (at > f) release(f, at - f);
    return false;
  }
  // Any run: one that crosses chunks starts in the free tail of some chunk (or at the start of an empty one), so those are the
  // candidates. Slow (a pass over the chunks) but only for requests the reservations cannot serve.
  uint32_t alloc_run(uint32_t n, uint64_t align) {
    const uint64_t base_frame = base / kFrame;
    const uint32_t start = __atomic_load_n(&run_hint, __ATOMIC_RELAXED) % chunks;
    for (uint32_t i = 0; i < chunks; ++i) {
      const uint32_t c = (start + i) % chunks;
      if (!load(&chunk_free[c])) continue;
      uint32_t top = 0;   // one past the highest used frame of the chunk
      for (uint32_t w = kWords; w-- > 0;) {
        if (const uint64_t v = load(&bits[c * kWords + w])) {
          top = w * 64 + 64 - __builtin_clzll(v);
          break;
        }
      }
      if (top >= kChunk) continue;
      const uint64_t f = ((base_frame + c * uint64_t{kChunk} + top + align - 1) & ~(align - 1)) - base_frame;
      if (f >= (c + 1) * uint64_t{kChunk} || f + n > frames) continue;
      if (claim_run(static_cast<uint32_t>(f), n)) {
        __atomic_store_n(&run_hint, c, __ATOMIC_RELAXED);
        return static_cast<uint32_t>(f);
      }
    }
    return kNone;
  }

  uint32_t alloc(uint32_t slot, uint32_t n, uint64_t align) {
    if (n > frames) return kNone;
    if (n <= kChunk && align <= kChunk) {
      const uint32_t kind = n <= 64 && align <= 64 ? kSmall : kMedium;
      const uint32_t f = alloc_reserved(slot, kind, n, static_cast<uint32_t>(align));
      if (f != kNone) return f;
    }
    return alloc_run(n, align);
  }

  bool free_frames(uint32_t f, uint32_t n) {
    if (f >= frames || n > frames - f) return false;
    for (uint32_t at = f; at < f + n;) {
      const uint32_t c = at / kChunk, len = min_of((c + 1) * kChunk, f + n) - at;
      if (!all_set(c, at % kChunk, len)) return false;
      at += len;
    }
    release(f, n);
    return true;
  }

  uint64_t free_frames_count() const {
    uint64_t total = 0;
    for (uint32_t t = 0; t < trees; ++t) total += load(&tree[t]) & kCountMask;
    for (const CoreSlot& s : slots)
      for (const uint64_t& res : s.reservation) total += count_of(load(&res));
    return total;
  }
};

void PageFrameAllocator::reset() {
  for (Area*& a : areas_) __atomic_store_n(&a, static_cast<Area*>(nullptr), __ATOMIC_RELEASE);
  __atomic_store_n(&area_count_, 0u, __ATOMIC_RELEASE);
}

uint32_t PageFrameAllocator::slot() const {
  uint32_t (*core)() = __atomic_load_n(&core_, __ATOMIC_ACQUIRE);
  return core ? core() % kCoreSlots : 0;
}

bool PageFrameAllocator::add_region(uint64_t base, uint64_t bytes, uint64_t extra_bytes, void** extra) {
  if (extra) *extra = nullptr;
  if (!base || bytes == 0 || bytes > UINT64_MAX - base || extra_bytes > (1ull << 40)) return false;
  const uint64_t first = (base + kFrame - 1) & ~(kFrame - 1);
  uint64_t last = (base + bytes) & ~(kFrame - 1);
  if (last <= first) return false;
  if (first < kLowLimit && last > kLowLimit) return false;
  const uint64_t area_base = first & ~(kChunkBytes - 1);
  if ((last - area_base) / kFrame > UINT32_MAX - kChunk) last = area_base + uint64_t{UINT32_MAX - kChunk} * kFrame;   // (16 TiB per area)

  const uint64_t frames = (last - area_base) / kFrame;
  const uint64_t chunks = (frames + kChunk - 1) / kChunk, trees = (chunks + kTree - 1) / kTree;
  auto round = [](uint64_t n) { return (n + 63) & ~uint64_t{63}; };
  const uint64_t area_bytes = round(sizeof(Area)), bits_bytes = round(chunks * kWords * 8), chunk_bytes = round(chunks * 4),
                 tree_bytes = round(trees * 4), tag_bytes = round(frames * 8), extra_size = round(extra_bytes);
  const uint64_t meta = area_bytes + bits_bytes + chunk_bytes + tree_bytes + tag_bytes + extra_size;
  const uint64_t meta_frames = (meta + kFrame - 1) / kFrame;
  if ((last - first) / kFrame <= meta_frames) return false;
  const uint64_t meta_start = last - meta_frames * kFrame;

  // Claim an entry first, so a full table fails before anything is written.
  uint32_t index = __atomic_load_n(&area_count_, __ATOMIC_ACQUIRE);
  do {
    if (index >= kMaxAreas) return false;
  } while (!cas(&area_count_, index, index + 1));

  for (uint64_t* w = place<uint64_t>(meta_start); w < place<uint64_t>(meta_start + meta); ++w) *w = 0;
  Area& a = *place<Area>(meta_start);
  a.base = area_base;
  a.first = first;
  a.last = last;
  a.frames = static_cast<uint32_t>(frames);
  a.chunks = static_cast<uint32_t>(chunks);
  a.trees = static_cast<uint32_t>(trees);
  a.low = last <= kLowLimit;
  uint64_t at = meta_start + area_bytes;
  a.bits = place<uint64_t>(at);
  a.chunk_free = place<uint32_t>(at += bits_bytes);
  a.tree = place<uint32_t>(at += chunk_bytes);
  a.tags = place<uint64_t>(at += tree_bytes);
  if (extra) *extra = extra_bytes ? place<void>(at + tag_bytes) : nullptr;

  // Frames before the region, the metadata and anything past the end of the last chunk are used for good.
  const uint64_t usable_lo = (first - area_base) / kFrame, usable_hi = (meta_start - area_base) / kFrame;
  for (uint64_t c = 0; c < chunks; ++c) {
    uint32_t free = 0;
    for (uint32_t w = 0; w < kWords; ++w) {
      const uint64_t lo = c * kChunk + w * 64;
      uint64_t used = 0;
      for (uint32_t b = 0; b < 64; ++b)
        if (lo + b < usable_lo || lo + b >= usable_hi) used |= uint64_t{1} << b;
      a.bits[c * kWords + w] = used;
      free += 64 - __builtin_popcountll(used);
    }
    a.chunk_free[c] = free;
    a.tree[c / kTree] += free;
  }
  __atomic_store_n(&areas_[index], &a, __ATOMIC_RELEASE);
  return true;
}

PageFrameAllocator::Area* PageFrameAllocator::area_of(uint64_t address) const {
  const uint32_t count = area_count();
  for (uint32_t i = 0; i < count && i < kMaxAreas; ++i) {
    Area* a = __atomic_load_n(&areas_[i], __ATOMIC_ACQUIRE);
    if (a && address >= a->first && address < a->last) return a;
  }
  return nullptr;
}

uint64_t PageFrameAllocator::allocate(uint64_t frames, uint64_t align_frames, bool low_only) {
  if (frames == 0 || frames > UINT32_MAX || align_frames == 0 || (align_frames & (align_frames - 1)) || align_frames > (1ull << 30)) return 0;
  const uint32_t s = slot();
  const uint32_t count = area_count();
  for (int pass = low_only ? 1 : 0; pass < 2; ++pass) {   // pass 0: the high areas, 1: the low ones
    for (uint32_t i = 0; i < count && i < kMaxAreas; ++i) {
      Area* a = __atomic_load_n(&areas_[i], __ATOMIC_ACQUIRE);
      if (!a || a->low != (pass == 1)) continue;
      const uint32_t f = a->alloc(s, static_cast<uint32_t>(frames), align_frames);
      if (f != kNone) return a->base + uint64_t{f} * kFrame;
    }
  }
  return 0;
}

bool PageFrameAllocator::free(uint64_t address, uint64_t frames) {
  Area* a = area_of(address);
  if (!a || (address & (kFrame - 1)) || frames == 0 || frames > UINT32_MAX) return false;
  return a->free_frames(static_cast<uint32_t>((address - a->base) / kFrame), static_cast<uint32_t>(frames));
}

uint64_t* PageFrameAllocator::frame_tag(uint64_t address) const {
  Area* a = area_of(address);
  return a ? &a->tags[(address - a->base) / kFrame] : nullptr;
}

uint64_t PageFrameAllocator::total_bytes() const {
  uint64_t total = 0;
  const uint32_t count = area_count();
  for (uint32_t i = 0; i < count && i < kMaxAreas; ++i)
    if (const Area* a = __atomic_load_n(&areas_[i], __ATOMIC_ACQUIRE)) total += a->last - a->first;
  return total;
}

uint64_t PageFrameAllocator::free_bytes_in(bool low) const {
  uint64_t total = 0;
  const uint32_t count = area_count();
  for (uint32_t i = 0; i < count && i < kMaxAreas; ++i) {
    const Area* a = __atomic_load_n(&areas_[i], __ATOMIC_ACQUIRE);
    if (a && a->low == low) total += a->free_frames_count() * kFrame;
  }
  return total;
}

uint64_t PageFrameAllocator::free_bytes_low() const { return free_bytes_in(true); }
uint64_t PageFrameAllocator::free_bytes_high() const { return free_bytes_in(false); }

}  // namespace allocators
