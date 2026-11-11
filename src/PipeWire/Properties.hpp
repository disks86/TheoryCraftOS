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

// The key/value dictionaries of the PipeWire graph ("node.name" = "...", "media.class" = "Audio/Sink"): what every object carries, what the native
// protocol sends as a dict and PulseAudio as a property list. Kept in insertion order; a key is there once.
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace pipewire {

struct Property {
  explicit Property(allocators::IAllocator& a) : key(a), value(a) {}
  String key;
  String value;
};

class Properties {
 public:
  static constexpr uint64_t kMaxItems = 256;
  enum class Mode : uint32_t { kSet = 0, kMerge = 1, kReplace = 2 };      // (PulseAudio's pa_update_mode_t)

  explicit Properties(allocators::IAllocator& a) : allocator_(a), items_(a) {}
  Properties(const Properties&) = delete;
  Properties& operator=(const Properties&) = delete;
  Properties(Properties&&) = default;

  allocators::IAllocator& allocator() const { return allocator_; }
  // Adds or replaces. False when out of memory or kMaxItems are there.
  bool set(StringView key, StringView value) {
    for (Property& p : items_.view())
      if (p.key == key) return p.value.assign(value);
    if (items_.count() >= kMaxItems) return false;
    Property p(allocator_);
    if (!p.key.assign(key) || !p.value.assign(value)) return false;
    return items_.push_back(static_cast<Property&&>(p));
  }
  bool set_uint(StringView key, uint64_t value) {
    String text(allocator_);
    return text.append_uint(value) && set(key, text.view());
  }
  bool remove(StringView key) {
    for (uint64_t i = 0; i < items_.count(); ++i)
      if (items_[i].key == key) return items_.remove_at(i);
    return false;
  }
  // The value of `key` (empty when it is not there).
  StringView get(StringView key) const {
    for (const Property& p : items_.view())
      if (p.key == key) return p.value.view();
    return StringView();
  }
  bool has(StringView key) const {
    for (const Property& p : items_.view())
      if (p.key == key) return true;
    return false;
  }
  uint64_t count() const { return items_.count(); }
  const Property& at(uint64_t i) const { return items_[i]; }
  void clear() { items_.clear(); }
  // kSet: becomes `other`; kMerge: adds what is missing; kReplace: adds or overwrites.
  bool update(const Properties& other, Mode mode) {
    if (mode == Mode::kSet) items_.clear();
    for (const Property& p : other.items_.view()) {
      if (mode == Mode::kMerge && has(p.key.view())) continue;
      if (!set(p.key.view(), p.value.view())) return false;
    }
    return true;
  }
  bool copy_from(const Properties& other) { return update(other, Mode::kSet); }

 private:
  allocators::IAllocator& allocator_;
  List<Property> items_;
};

}  // namespace pipewire
