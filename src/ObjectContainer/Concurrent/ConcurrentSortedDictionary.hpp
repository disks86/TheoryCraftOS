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
#include "ObjectContainer/SortedDictionary.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe SortedDictionary: every operation takes a SpinLock and keys and
// values are copied in and out. update() changes a value in place atomically.
// See ConcurrentList for what is and is not atomic.
//
// WARNING: Do not attempt to acquire the same lock recursively - this can lead to deadlock if the underlying synchronization primitive does not support re-entrancy.
template <class K, class V, class Less = DefaultLess<K>>
class ConcurrentSortedDictionary : public Synchronized<SortedDictionary<K, V, Less>> {
  using Inner = SortedDictionary<K, V, Less>;
  using Base = Synchronized<Inner>;

 public:
  ConcurrentSortedDictionary(allocators::IAllocator& allocator, const hardware::ICentralProcessingUnit& cpu,
                             Less less = Less())
      : Base(cpu, allocator, less) {}

  uint64_t count() const {
    return this->with([](const Inner& d) { return d.count(); });
  }
  void clear() {
    this->with([](Inner& d) { d.clear(); });
  }
  bool put(const K& key, const V& value) {
    return this->with([&](Inner& d) { return d.put(key, value); });
  }
  bool find(const K& key, V* out) const {
    return this->with([&](const Inner& d) {
      const V* v = d.find(key);
      if (v) *out = *v;
      return v != nullptr;
    });
  }
  bool contains(const K& key) const {
    return this->with([&](const Inner& d) { return d.contains(key); });
  }
  bool remove(const K& key) {
    return this->with([&](Inner& d) { return d.remove(key); });
  }
  template <class Modify>
  bool update(const K& key, Modify&& modify) {
    return this->with([&](Inner& d) {
      V* v = d.find(key);
      if (v) modify(*v);
      return v != nullptr;
    });
  }
  // Copies the entry with the smallest / largest key; false when empty.
  bool first_entry(K* key, V* value) const {
    return this->with([&](const Inner& d) { return copy_entry(d.first_entry(), key, value); });
  }
  bool last_entry(K* key, V* value) const {
    return this->with([&](const Inner& d) { return copy_entry(d.last_entry(), key, value); });
  }
  // Copies the entry with the smallest key that is not less than `bound`.
  bool lower_bound(const K& bound, K* key, V* value) const {
    return this->with([&](const Inner& d) { return copy_entry(d.lower_bound(bound), key, value); });
  }
  // Ascending by key, under the lock.
  // WARNING: Do not attempt to acquire the same lock recursively from within 
  // this visitor callback as this will cause deadlock.
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const Inner& d) {
      for (const auto& entry : d) visitor(entry);
    });
  }

 private:
  static bool copy_entry(const KeyValue<K, V>* entry, K* key, V* value) {
    if (!entry) return false;
    if (key) *key = entry->key;
    if (value) *value = entry->value;
    return true;
  }
};
