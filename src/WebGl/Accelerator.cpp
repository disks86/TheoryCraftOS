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

#include "WebGl/Accelerator.hpp"

namespace webgl {
namespace {
IAccelerator* g_accelerator = nullptr;
}

uint32_t AccelTextureCache::get(IAccelerator& acc, uint64_t key, uint32_t w, uint32_t h, ConstByteView rgba) {
  const uint64_t bytes = uint64_t{w} * h * 4;
  if (rgba.size() != bytes) return 0;
  // FNV-1a over the texels, a word at a time
  uint64_t hash = 0xcbf29ce484222325ull;
  uint64_t i = 0;
  for (; i + 8 <= bytes; i += 8) {
    uint64_t word;
    __builtin_memcpy(&word, rgba.data() + i, 8);   // (unaligned reads of a byte buffer: memcpy is the defined way)
    hash = (hash ^ word) * 0x100000001b3ull;
    hash ^= hash >> 29;
  }
  for (; i < bytes; ++i) hash = (hash ^ rgba[i]) * 0x100000001b3ull;
  for (uint64_t k = 0; k < entries_.count(); ++k) {
    Entry& e = entries_[k];
    if (e.key != key) continue;
    if (e.w == w && e.h == h) {
      if (e.hash != hash) {
        if (!acc.write_texture(e.handle, rgba)) return 0;
        e.hash = hash;
      }
      return e.handle;
    }
    acc.destroy_texture(e.handle);   // another size: a new texture
    entries_.remove_at(k);
    break;
  }
  if (entries_.count() >= kMaxEntries) {
    acc.destroy_texture(entries_[0].handle);
    entries_.remove_at(0);
  }
  const uint32_t handle = acc.create_texture(w, h);
  if (!handle) return 0;
  if (!acc.write_texture(handle, rgba) || !entries_.push_back(Entry{key, hash, handle, w, h})) {
    acc.destroy_texture(handle);
    return 0;
  }
  return handle;
}

void AccelTextureCache::forget(IAccelerator& acc, uint64_t key) {
  for (uint64_t k = 0; k < entries_.count(); ++k)
    if (entries_[k].key == key) {
      acc.destroy_texture(entries_[k].handle);
      entries_.remove_at(k);
      return;
    }
}

void AccelTextureCache::release(IAccelerator& acc) {
  for (const Entry& e : entries_) acc.destroy_texture(e.handle);
  entries_.clear();
}

void set_accelerator(IAccelerator* accelerator) { g_accelerator = accelerator; }
IAccelerator* accelerator() { return g_accelerator; }

}  // namespace webgl
