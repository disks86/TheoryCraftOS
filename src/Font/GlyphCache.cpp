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
#include "Font/GlyphCache.hpp"

namespace font {

GlyphCache::GlyphCache(allocators::IAllocator& allocator, GlyphCacheLimits limits)
    : allocator_(allocator), limits_(limits), slots_(allocator), free_(allocator), map_(allocator), scratch_(allocator) {
  if (limits_.max_entries == 0) limits_.max_entries = 1;
}

uint64_t GlyphCache::pack(const GlyphKey& key) {
  const uint64_t size = static_cast<uint64_t>(key.size < 0 ? 0 : key.size) & 0xFFFFFF;
  return (uint64_t{key.face & 0x3FFFFF} << 42) | (uint64_t{key.glyph & 0xFFFF} << 26) | (size << 2) | (key.subpixel & 3);
}

void GlyphCache::unlink(uint32_t index) {
  Slot& s = slots_[index];
  if (s.prev != kNone) slots_[s.prev].next = s.next;
  else head_ = s.next;
  if (s.next != kNone) slots_[s.next].prev = s.prev;
  else tail_ = s.prev;
  s.prev = s.next = kNone;
}

void GlyphCache::push_front(uint32_t index) {
  Slot& s = slots_[index];
  s.prev = kNone;
  s.next = head_;
  if (head_ != kNone) slots_[head_].prev = index;
  head_ = index;
  if (tail_ == kNone) tail_ = index;
}

void GlyphCache::remove(uint32_t index) {
  Slot& s = slots_[index];
  unlink(index);
  map_.remove(s.key);
  bytes_ -= s.cost;
  s.cost = 0;
  s.used = false;
  s.bitmap = GlyphBitmap(allocator_);  // frees the pixels
  --live_;
  free_.push_back(index);  // if this fails the slot is simply never reused
}

const GlyphBitmap* GlyphCache::find(const GlyphKey& key) {
  const uint32_t* index = map_.find(pack(key));
  if (!index) {
    ++misses_;
    return nullptr;
  }
  ++hits_;
  const uint32_t i = *index;
  if (head_ != i) {
    unlink(i);
    push_front(i);
  }
  return &slots_[i].bitmap;
}

const GlyphBitmap* GlyphCache::insert(const GlyphKey& key, GlyphBitmap&& bitmap) {
  const uint64_t packed = pack(key);
  if (const uint32_t* existing = map_.find(packed)) remove(*existing);
  const uint64_t cost = bitmap.memory_bytes() + kEntryOverhead;
  if (cost > limits_.max_bytes) {
    scratch_ = static_cast<GlyphBitmap&&>(bitmap);
    return &scratch_;
  }
  while (tail_ != kNone && (bytes_ + cost > limits_.max_bytes || live_ >= limits_.max_entries)) remove(tail_);

  uint32_t index = kNone;
  if (free_.count()) {
    index = free_[free_.count() - 1];
    free_.remove_at(free_.count() - 1);
  } else if (slots_.count() < limits_.max_entries && slots_.push_back(Slot(allocator_))) {
    index = static_cast<uint32_t>(slots_.count() - 1);
  }
  if (index == kNone || !map_.put(packed, index)) {
    if (index != kNone) free_.push_back(index);
    scratch_ = static_cast<GlyphBitmap&&>(bitmap);
    return &scratch_;
  }
  Slot& s = slots_[index];
  s.key = packed;
  s.cost = cost;
  s.used = true;
  s.bitmap = static_cast<GlyphBitmap&&>(bitmap);
  bytes_ += cost;
  ++live_;
  push_front(index);
  return &s.bitmap;
}

void GlyphCache::clear() {
  while (tail_ != kNone) remove(tail_);
  scratch_ = GlyphBitmap(allocator_);
}

void GlyphCache::evict_face(uint32_t face) {
  const uint64_t wanted = uint64_t{face & 0x3FFFFF};
  for (uint32_t i = 0; i < slots_.count(); ++i) {
    if (slots_[i].used && (slots_[i].key >> 42) == wanted) remove(i);
  }
}

}  // namespace font
