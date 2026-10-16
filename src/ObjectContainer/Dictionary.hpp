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
#include "ObjectContainer/ContainerSupport.hpp"
#include "Utility/ArrayView.hpp"
#include "ObjectContainer/Hashing.hpp"
#include "ObjectContainer/IObjectContainer.hpp"

// What a Dictionary holds and iterates over. The key cannot be changed in place.
template <class K, class V>
struct KeyValue {
  const K key;
  V value;
  KeyValue(const K& k, const V& v) : key(k), value(v) {}
};

// A hash map from K to V with separate chaining: each entry is its own
// allocation (so entries never move, and a pointer from find() stays valid until
// that entry is removed), and the bucket table doubles when it gets 3/4 full.
// If the table cannot grow for lack of memory the dictionary keeps working with
// longer chains. Iteration visits every entry once, in no particular order.
// V must be copy-constructible and copy-assignable. Every allocation goes
// through the IAllocator given.
template <class K, class V, class Hash = DefaultHash<K>, class Equal = DefaultEqual<K>>
class Dictionary final : public IObjectContainer<KeyValue<K, V>> {
 public:
  explicit Dictionary(allocators::IAllocator& allocator, Hash hash = Hash(), Equal equal = Equal())
      : allocator_(allocator), hash_(hash), equal_(equal) {}
  ~Dictionary() {
    clear();
    allocator_.deallocate(buckets_.data());
  }

  uint64_t count() const override { return size_; }

  void clear() override {
    for (uint64_t b = 0; b < buckets_.size(); ++b) {
      for (Node* n = buckets_[b]; n;) {
        Node* following = n->next;
        destroy(n);
        n = following;
      }
      buckets_[b] = nullptr;
    }
    size_ = 0;
  }

  // Stores `value` under `key`, replacing any earlier value. False if memory ran out.
  bool put(const K& key, const V& value) {
    const uint64_t h = hash_(key);
    if (Node* existing = locate(key, h)) {
      existing->entry.value = value;
      return true;
    }
    if (buckets_.empty() && !resize(kInitialBuckets)) return false;
    if (size_ * 4 >= buckets_.size() * 3) resize(buckets_.size() * 2);  // best effort

    void* memory = allocator_.allocate(sizeof(Node));
    if (!memory) return false;
    Node** slot = &buckets_[h & (buckets_.size() - 1)];
    *slot = new (memory) Node{*slot, h, KeyValue<K, V>(key, value)};
    ++size_;
    return true;
  }

  // The value stored under `key`, or nullptr.
  V* find(const K& key) {
    Node* n = locate(key, hash_(key));
    return n ? &n->entry.value : nullptr;
  }
  const V* find(const K& key) const {
    const Node* n = locate(key, hash_(key));
    return n ? &n->entry.value : nullptr;
  }
  bool contains(const K& key) const { return find(key) != nullptr; }

  bool remove(const K& key) {
    if (buckets_.empty()) return false;
    const uint64_t h = hash_(key);
    for (Node** link = &buckets_[h & (buckets_.size() - 1)]; *link; link = &(*link)->next) {
      if ((*link)->hash == h && equal_((*link)->entry.key, key)) {
        Node* doomed = *link;
        *link = doomed->next;
        destroy(doomed);
        --size_;
        return true;
      }
    }
    return false;
  }

  // --- IObjectContainer iteration: `index` is the bucket, `node` the entry.
  bool first(ObjectCursor* cursor) const override { return scan(0, cursor); }
  bool next(ObjectCursor* cursor) const override {
    Node* following = static_cast<Node*>(cursor->node)->next;
    if (following) {
      cursor->node = following;
      return true;
    }
    return scan(cursor->index + 1, cursor);
  }
  KeyValue<K, V>* element(const ObjectCursor& cursor) const override {
    return &static_cast<Node*>(cursor.node)->entry;
  }

 private:
  struct Node {
    Node* next;
    uint64_t hash;
    KeyValue<K, V> entry;
  };
  static constexpr uint64_t kInitialBuckets = 16;  // always a power of two

  Node* locate(const K& key, uint64_t h) const {
    if (buckets_.empty()) return nullptr;
    for (Node* n = buckets_[h & (buckets_.size() - 1)]; n; n = n->next)
      if (n->hash == h && equal_(n->entry.key, key)) return n;
    return nullptr;
  }

  void destroy(Node* node) {
    node->~Node();
    allocator_.deallocate(node);
  }

  // Finds the first entry in bucket `from` or later.
  bool scan(uint64_t from, ObjectCursor* cursor) const {
    for (uint64_t b = from; b < buckets_.size(); ++b) {
      if (buckets_[b]) {
        cursor->index = b;
        cursor->node = buckets_[b];
        return true;
      }
    }
    return false;
  }

  // Moves to a table of `buckets` buckets, relinking the existing entries.
  bool resize(uint64_t buckets) {
    Node** fresh_memory = container_support::allocate_array<Node*>(allocator_, buckets);
    if (!fresh_memory) return false;
    const ArrayView<Node*> fresh(fresh_memory, buckets);
    fresh.fill(nullptr);
    for (uint64_t b = 0; b < buckets_.size(); ++b) {
      for (Node* n = buckets_[b]; n;) {
        Node* following = n->next;
        Node** slot = &fresh[n->hash & (buckets - 1)];
        n->next = *slot;
        *slot = n;
        n = following;
      }
    }
    allocator_.deallocate(buckets_.data());
    buckets_ = fresh;
    return true;
  }

  allocators::IAllocator& allocator_;
  Hash hash_;
  Equal equal_;
  ArrayView<Node*> buckets_;  // the table; its size is the (power of two) bucket count
  uint64_t size_ = 0;
};
