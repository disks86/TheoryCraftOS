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
#include "Allocators/IAllocator.hpp"
#include "Font/GlyphRaster.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "ObjectContainer/List.hpp"

namespace font {

// What identifies a rasterised glyph: which face (FontSet's face id), the glyph, the size in 1/64 px and the
// quarter-pixel phase. All fields are range limited when packed (face < 2^22, glyph < 2^16, size < 2^24).
struct GlyphKey {
  uint32_t face = 0;
  uint32_t glyph = 0;
  Fixed size = 0;
  uint32_t subpixel = 0;
};

struct GlyphCacheLimits {
  uint64_t max_bytes = 1024 * 1024;  // bitmap memory plus a fixed charge per entry for the bookkeeping
  uint32_t max_entries = 4096;
};

// A bounded least-recently-used cache of glyph bitmaps. Memory use is capped: when a new bitmap does not fit
// within max_bytes / max_entries, the least recently used ones are dropped first. A bitmap larger than the whole
// budget is not kept: it is parked in one scratch slot until the next call. Everything is allocated from the
// allocator given and there is no recursion.
//
// A pointer returned by find()/insert() is valid until the next find(), insert(), evict_face() or clear() on this
// cache - copy or draw the glyph before looking up the next one.
class GlyphCache {
 public:
  // Bytes charged per entry on top of the pixels (the slot, the map node, allocator block rounding).
  static constexpr uint64_t kEntryOverhead = 192;

  explicit GlyphCache(allocators::IAllocator& allocator, GlyphCacheLimits limits = GlyphCacheLimits());
  GlyphCache(const GlyphCache&) = delete;
  GlyphCache& operator=(const GlyphCache&) = delete;

  // The cached bitmap, marked most recently used; nullptr on a miss.
  const GlyphBitmap* find(const GlyphKey& key);
  // Stores a bitmap (taking it over) and returns where it now lives. Evicts as needed. Never fails: if it
  // cannot be cached it is kept as the scratch bitmap.
  const GlyphBitmap* insert(const GlyphKey& key, GlyphBitmap&& bitmap);

  void clear();
  // Drops every glyph of one face (a web font being removed).
  void evict_face(uint32_t face);

  uint64_t bytes() const { return bytes_; }
  uint32_t entries() const { return live_; }
  uint64_t hits() const { return hits_; }
  uint64_t misses() const { return misses_; }
  const GlyphCacheLimits& limits() const { return limits_; }

 private:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  struct Slot {
    explicit Slot(allocators::IAllocator& a) : bitmap(a) {}
    uint64_t key = 0;
    uint64_t cost = 0;
    uint32_t prev = kNone;  // towards the most recently used
    uint32_t next = kNone;  // towards the least recently used
    bool used = false;
    GlyphBitmap bitmap;
  };

  static uint64_t pack(const GlyphKey& key);
  void unlink(uint32_t index);
  void push_front(uint32_t index);
  void remove(uint32_t index);

  allocators::IAllocator& allocator_;
  GlyphCacheLimits limits_;
  List<Slot> slots_;
  List<uint32_t> free_;
  Dictionary<uint64_t, uint32_t> map_;
  GlyphBitmap scratch_;
  uint32_t head_ = kNone;  // most recently used
  uint32_t tail_ = kNone;  // least recently used
  uint32_t live_ = 0;
  uint64_t bytes_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
};

}  // namespace font
