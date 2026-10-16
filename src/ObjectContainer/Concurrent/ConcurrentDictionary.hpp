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
#include "ObjectContainer/Dictionary.hpp"
#include "Synchronization/Synchronized.hpp"

// Thread-safe Dictionary: every operation takes a SpinLock, and values are
// copied in and out (find() fills a caller-owned V). To change a value in place
// atomically - a counter, say - use update(). See ConcurrentList for what is and
// is not atomic.
template <class K, class V, class Hash = DefaultHash<K>, class Equal = DefaultEqual<K>>
class ConcurrentDictionary : public Synchronized<Dictionary<K, V, Hash, Equal>> {
  using Inner = Dictionary<K, V, Hash, Equal>;
  using Base = Synchronized<Inner>;

 public:
  ConcurrentDictionary(allocators::IAllocator& allocator, const hardware::ICentralProcessingUnit& cpu,
                       Hash hash = Hash(), Equal equal = Equal())
      : Base(cpu, allocator, hash, equal) {}

  uint64_t count() const {
    return this->with([](const Inner& d) { return d.count(); });
  }
  void clear() {
    this->with([](Inner& d) { d.clear(); });
  }
  bool put(const K& key, const V& value) {
    return this->with([&](Inner& d) { return d.put(key, value); });
  }
  // Copies the value stored under `key` into *out; false if there is none.
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
  // Calls modify(V&) on the value under `key`, atomically. False if absent.
  template <class Modify>
  bool update(const K& key, Modify&& modify) {
    return this->with([&](Inner& d) {
      V* v = d.find(key);
      if (v) modify(*v);
      return v != nullptr;
    });
  }
  // Calls visitor(const KeyValue&) for every entry, under the lock.
  template <class Visitor>
  void for_each(Visitor&& visitor) const {
    this->with([&](const Inner& d) {
      for (const auto& entry : d) visitor(entry);
    });
  }
};
