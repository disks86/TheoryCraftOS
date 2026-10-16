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
#include "ObjectContainer/Comparison.hpp"
#include "ObjectContainer/ContainerSupport.hpp"
#include "ObjectContainer/IObjectContainer.hpp"

// An ordered set of unique values kept in a self-balancing (AVL) binary search
// tree: insert, find and remove take O(log n) and the tree's height never
// exceeds about 1.44 log2(n). Each value is its own allocation and never moves,
// so a pointer from find() stays valid until that value is removed.
//
// Iteration is an in-order walk (smallest first) that needs no stack: nodes
// keep a parent pointer, so the cursor is just the current node. The pre/post
// order visitors recurse, but only as deep as the tree is high.
//
// Compare is a "less" functor (see Comparison.hpp). To look values up by
// something smaller than a whole T (as SortedDictionary looks up by key), give
// Compare extra overloads taking (Key, T) and (T, Key) and use the *_key
// functions. Every allocation goes through the IAllocator given.
template <class T, class Compare = DefaultLess<T>>
class BinaryTree final : public IObjectContainer<T> {
 public:
  enum class InsertResult : uint8_t { Inserted, Duplicate, OutOfMemory };

  explicit BinaryTree(allocators::IAllocator& allocator, Compare compare = Compare())
      : allocator_(allocator), compare_(compare) {}
  ~BinaryTree() { clear(); }

  uint64_t count() const override { return size_; }

  void clear() override {
    destroy_subtree(root_);
    root_ = nullptr;
    size_ = 0;
  }

  // Adds `value` unless an equivalent one is already stored.
  InsertResult insert(const T& value) {
    Node* parent = nullptr;
    Node** link = &root_;
    while (*link) {
      parent = *link;
      if (compare_(value, parent->value)) link = &parent->left;
      else if (compare_(parent->value, value)) link = &parent->right;
      else return InsertResult::Duplicate;
    }
    static_assert(container_support::kFitsAllocatorAlignment<Node>, "type is over-aligned for the allocator");
    void* memory = allocator_.allocate(sizeof(Node));
    if (!memory) return InsertResult::OutOfMemory;
    Node* node = new (memory) Node{nullptr, nullptr, parent, 1, value};
    *link = node;
    ++size_;
    rebalance_up(parent);
    return InsertResult::Inserted;
  }

  // Removes the value equivalent to `value` / to `key`.
  bool remove(const T& value) { return remove_key(value); }
  template <class Key>
  bool remove_key(const Key& key) {
    Node* node = locate(key);
    if (!node) return false;
    erase(node);
    return true;
  }

  T* find(const T& value) { return value_of(locate(value)); }
  const T* find(const T& value) const { return value_of(locate(value)); }
  template <class Key>
  T* find_key(const Key& key) { return value_of(locate(key)); }
  template <class Key>
  const T* find_key(const Key& key) const { return value_of(locate(key)); }
  bool contains(const T& value) const { return locate(value) != nullptr; }

  // The smallest value that does not sort before `key` (nullptr if none).
  template <class Key>
  T* lower_bound_key(const Key& key) const {
    Node* best = nullptr;
    for (Node* n = root_; n;) {
      if (compare_(n->value, key)) {
        n = n->right;
      } else {
        best = n;
        n = n->left;
      }
    }
    return value_of(best);
  }

  T* min() { return value_of(leftmost(root_)); }
  T* max() { return value_of(rightmost(root_)); }
  const T* min() const { return value_of(leftmost(root_)); }
  const T* max() const { return value_of(rightmost(root_)); }

  // Number of levels (0 when empty, 1 for a single value).
  uint32_t height() const { return height_of(root_); }

  // Calls visitor(T&) on every value, a node before its subtrees / after them.
  template <class Visitor>
  void visit_pre_order(Visitor&& visitor) { pre_order(root_, visitor); }
  template <class Visitor>
  void visit_post_order(Visitor&& visitor) { post_order(root_, visitor); }

  // --- IObjectContainer iteration: in-order, `node` is the current node.
  bool first(ObjectCursor* cursor) const override {
    Node* n = leftmost(root_);
    if (!n) return false;
    cursor->index = 0;
    cursor->node = n;
    return true;
  }
  bool next(ObjectCursor* cursor) const override {
    Node* n = static_cast<Node*>(cursor->node);
    if (n->right) {
      n = leftmost(n->right);
    } else {
      Node* child = n;
      n = n->parent;
      while (n && n->right == child) {  // climb out of right subtrees
        child = n;
        n = n->parent;
      }
      if (!n) return false;
    }
    ++cursor->index;
    cursor->node = n;
    return true;
  }
  T* element(const ObjectCursor& cursor) const override { return &static_cast<Node*>(cursor.node)->value; }

 private:
  struct Node {
    Node* left;
    Node* right;
    Node* parent;
    uint32_t height;
    T value;
  };

  static T* value_of(Node* n) { return n ? &n->value : nullptr; }
  static uint32_t height_of(const Node* n) { return n ? n->height : 0; }
  static Node* leftmost(Node* n) {
    while (n && n->left) n = n->left;
    return n;
  }
  static Node* rightmost(Node* n) {
    while (n && n->right) n = n->right;
    return n;
  }

  template <class Key>
  Node* locate(const Key& key) const {
    for (Node* n = root_; n;) {
      if (compare_(key, n->value)) n = n->left;
      else if (compare_(n->value, key)) n = n->right;
      else return n;
    }
    return nullptr;
  }

  void destroy_subtree(Node* n) {
    if (!n) return;
    destroy_subtree(n->left);
    destroy_subtree(n->right);
    n->~Node();
    allocator_.deallocate(n);
  }

  template <class Visitor>
  void pre_order(Node* n, Visitor& visitor) {
    if (!n) return;
    visitor(n->value);
    pre_order(n->left, visitor);
    pre_order(n->right, visitor);
  }
  template <class Visitor>
  void post_order(Node* n, Visitor& visitor) {
    if (!n) return;
    post_order(n->left, visitor);
    post_order(n->right, visitor);
    visitor(n->value);
  }

  // --- AVL machinery
  void update_height(Node* n) {
    const uint32_t l = height_of(n->left), r = height_of(n->right);
    n->height = (l > r ? l : r) + 1;
  }

  // Puts `replacement` where `old` hangs (its parent's link, or the root).
  void replace_child(Node* old, Node* replacement) {
    Node* parent = old->parent;
    if (replacement) replacement->parent = parent;
    if (!parent) root_ = replacement;
    else if (parent->left == old) parent->left = replacement;
    else parent->right = replacement;
  }

  Node* rotate_left(Node* x) {
    Node* y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    replace_child(x, y);
    y->left = x;
    x->parent = y;
    update_height(x);
    update_height(y);
    return y;
  }
  Node* rotate_right(Node* x) {
    Node* y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    replace_child(x, y);
    y->right = x;
    x->parent = y;
    update_height(x);
    update_height(y);
    return y;
  }

  // Walks from `n` to the root fixing heights and rotating where a node's
  // subtrees differ in height by more than one.
  void rebalance_up(Node* n) {
    while (n) {
      update_height(n);
      const int32_t balance = static_cast<int32_t>(height_of(n->left)) - static_cast<int32_t>(height_of(n->right));
      if (balance > 1) {
        if (height_of(n->left->left) < height_of(n->left->right)) rotate_left(n->left);
        n = rotate_right(n);
      } else if (balance < -1) {
        if (height_of(n->right->right) < height_of(n->right->left)) rotate_right(n->right);
        n = rotate_left(n);
      }
      n = n->parent;
    }
  }

  // Unlinks `z` (relinking nodes, never moving values) and frees it.
  void erase(Node* z) {
    Node* start;  // deepest node whose subtree heights changed
    if (!z->left || !z->right) {
      Node* child = z->left ? z->left : z->right;
      start = z->parent;
      replace_child(z, child);
    } else {
      Node* successor = leftmost(z->right);  // takes z's place
      if (successor->parent != z) {
        start = successor->parent;
        replace_child(successor, successor->right);
        successor->right = z->right;
        successor->right->parent = successor;
      } else {
        start = successor;
      }
      replace_child(z, successor);
      successor->left = z->left;
      successor->left->parent = successor;
    }
    z->~Node();
    allocator_.deallocate(z);
    --size_;
    rebalance_up(start);
  }

  allocators::IAllocator& allocator_;
  Compare compare_;
  Node* root_ = nullptr;
  uint64_t size_ = 0;
};
