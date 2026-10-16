#pragma once
#include "ObjectContainer/BinaryTree.hpp"
#include "ObjectContainer/Dictionary.hpp"  // for KeyValue

// A map from K to V that keeps its entries sorted by key (smallest first), so
// iteration yields them in order and the smallest/largest/nearest key is cheap.
// Built on BinaryTree, so put, find and remove take O(log n) and entries never
// move: a pointer from find() stays valid until that entry is removed.
// Compare is a "less" functor on keys (default: operator<; C strings compare by
// content, and only the pointer is stored, so the string must outlive the entry).
// V must be copy-constructible and copy-assignable. Every allocation goes
// through the IAllocator given.
template <class K, class V, class Less = DefaultLess<K>>
class SortedDictionary final : public IObjectContainer<KeyValue<K, V>> {
 public:
  using Entry = KeyValue<K, V>;

  explicit SortedDictionary(allocators::IAllocator& allocator, Less less = Less())
      : tree_(allocator, EntryCompare{less}) {}

  uint64_t count() const override { return tree_.count(); }
  void clear() override { tree_.clear(); }

  // Stores `value` under `key`, replacing any earlier value. False if memory ran out.
  bool put(const K& key, const V& value) {
    if (Entry* existing = tree_.find_key(key)) {
      existing->value = value;
      return true;
    }
    return tree_.insert(Entry(key, value)) == Tree::InsertResult::Inserted;
  }

  V* find(const K& key) {
    Entry* e = tree_.find_key(key);
    return e ? &e->value : nullptr;
  }
  const V* find(const K& key) const {
    const Entry* e = tree_.find_key(key);
    return e ? &e->value : nullptr;
  }
  bool contains(const K& key) const { return find(key) != nullptr; }
  bool remove(const K& key) { return tree_.remove_key(key); }

  // Entries at the ends of the order, or nullptr when empty.
  Entry* first_entry() { return tree_.min(); }
  Entry* last_entry() { return tree_.max(); }
  const Entry* first_entry() const { return tree_.min(); }
  const Entry* last_entry() const { return tree_.max(); }
  // The entry with the smallest key that is not less than `key`, or nullptr.
  Entry* lower_bound(const K& key) const { return tree_.lower_bound_key(key); }

  // --- IObjectContainer iteration: ascending key order.
  bool first(ObjectCursor* cursor) const override { return tree_.first(cursor); }
  bool next(ObjectCursor* cursor) const override { return tree_.next(cursor); }
  Entry* element(const ObjectCursor& cursor) const override { return tree_.element(cursor); }

 private:
  // Orders entries by key, and lets the tree search by a bare key.
  struct EntryCompare {
    Less less;
    bool operator()(const Entry& a, const Entry& b) const { return less(a.key, b.key); }
    bool operator()(const K& a, const Entry& b) const { return less(a, b.key); }
    bool operator()(const Entry& a, const K& b) const { return less(a.key, b); }
  };
  using Tree = BinaryTree<Entry, EntryCompare>;

  Tree tree_;
};
