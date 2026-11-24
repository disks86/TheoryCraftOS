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
// What a DXVA decoder keeps per surface (the decoded picture and, for AV1, the state a reference carries), found by the surface's key (DxvaPicture::surface_keys):
// a small table of at most config.surfaces + 1 live entries. When it is full, the entry used longest ago that the current picture does not name is given up.
#pragma once
#include <stdint.h>
#include "Hardware/VideoCodec/Dxva/DxvaDecoder.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace dxva {

// The key surface index `index` of `picture` stands for (false: the index is out of range: Invalid).
[[nodiscard]] inline bool surface_key(const DxvaPicture& picture, uint32_t surfaces, uint32_t index, uint32_t* key) {
  if (picture.surface_keys.empty()) {
    if (index >= surfaces) return false;
    *key = index;
    return true;
  }
  return picture.surface_keys.try_get(index, key);
}

// The key the current picture is kept under: DxvaPicture::output_key when given (the index is then not looked up), else that of its index.
[[nodiscard]] inline bool output_surface_key(const DxvaPicture& picture, uint32_t surfaces, uint32_t index, uint32_t* key) {
  if (picture.output_key != kNoDxvaKey) {
    *key = picture.output_key;
    return true;
  }
  return surface_key(picture, surfaces, index, key);
}

template <class State>
class SurfaceTable {
 public:
  static constexpr uint32_t kCapacity = 129;   // 128 surfaces and the current picture

  explicit SurfaceTable(uint32_t live_limit) : limit_(live_limit == 0 || live_limit > kCapacity ? kCapacity : live_limit) {}

  // The state kept under `key`, or null.
  State* find(uint32_t key) {
    for (Entry& e : entries_)
      if (e.used && e.key == key) return &e.state;
    return nullptr;
  }
  // The entry for `key`, made if there is none (`*fresh` says so: its state is what the entry held before, the caller resets what it needs). An entry whose key is in
  // `keep` is never given up for it. Null only when every entry is kept (cannot happen with keep.size() < the limit).
  State* claim(uint32_t key, ArrayView<const uint32_t> keep, bool* fresh) {
    ++clock_;
    for (Entry& e : entries_)
      if (e.used && e.key == key) {
        e.stamp = clock_;
        *fresh = false;
        return &e.state;
      }
    *fresh = true;
    uint32_t live = 0;
    for (const Entry& e : entries_) live += e.used ? 1 : 0;
    Entry* pick = nullptr;
    if (live < limit_) {
      for (Entry& e : entries_)
        if (!e.used) {
          pick = &e;
          break;
        }
    }
    if (!pick) {
      for (Entry& e : entries_) {
        if (!e.used) continue;
        bool kept = false;
        for (uint32_t k : keep) kept = kept || k == e.key;
        if (!kept && (!pick || e.stamp < pick->stamp)) pick = &e;
      }
    }
    if (!pick) return nullptr;
    pick->used = true;
    pick->key = key;
    pick->stamp = clock_;
    return &pick->state;
  }
  // Marks the entry of `key` used now (a reference was read from it).
  void touch(uint32_t key) {
    ++clock_;
    for (Entry& e : entries_)
      if (e.used && e.key == key) e.stamp = clock_;
  }
  // Forgets every key (the states stay allocated, for reuse).
  void clear() {
    for (Entry& e : entries_) e.used = false;
  }
  template <class F>
  void for_each_state(F&& f) {
    for (Entry& e : entries_) f(e.state);
  }

 private:
  struct Entry {
    uint32_t key = 0;
    bool used = false;
    uint64_t stamp = 0;
    State state;
  };
  Entry entries_[kCapacity];
  uint32_t limit_;
  uint64_t clock_ = 0;
};

}  // namespace dxva
}  // namespace hardware
