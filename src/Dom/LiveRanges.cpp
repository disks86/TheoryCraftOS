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

#include "Dom/LiveRanges.hpp"

namespace dom {

uint32_t child_index(const Node* node) {
  uint32_t i = 0;
  for (const Node* s = node->prev; s; s = s->prev) ++i;
  return i;
}

Optional<uint32_t> LiveRanges::add_range(Node* node, const void* owner) {
  if (!node || !owner) return {};
  uint32_t id = free_range_hint_;
  while (id < ranges_.count() && ranges_[id].owner) ++id;
  if (id == ranges_.count() && !ranges_.push_back(LiveRange{})) return {};
  LiveRange& r = ranges_[id];
  r.start = r.end = node;
  r.start_offset = r.end_offset = 0;
  r.owner = owner;
  free_range_hint_ = id + 1;
  ++used_ranges_;
  return id;
}

Optional<uint32_t> LiveRanges::add_iterator(Node* root, const void* owner) {
  if (!root || !owner) return {};
  uint32_t id = free_iterator_hint_;
  while (id < iterators_.count() && iterators_[id].owner) ++id;
  if (id == iterators_.count() && !iterators_.push_back(LiveIterator{})) return {};
  LiveIterator& it = iterators_[id];
  it.root = it.reference = root;
  it.before = true;
  it.owner = owner;
  free_iterator_hint_ = id + 1;
  ++used_iterators_;
  return id;
}

void LiveRanges::remove_range(uint32_t id) {
  if (!range(id)) return;
  ranges_[id] = LiveRange{};
  --used_ranges_;
  if (id < free_range_hint_) free_range_hint_ = id;
}

void LiveRanges::remove_iterator(uint32_t id) {
  if (!iterator(id)) return;
  iterators_[id] = LiveIterator{};
  --used_iterators_;
  if (id < free_iterator_hint_) free_iterator_hint_ = id;
}

// "Live range pre-remove steps" and "NodeIterator pre-removing steps".
void LiveRanges::removing(Node* node) {
  Node* parent = node->parent;
  if (!parent) return;
  const uint32_t index = child_index(node);
  if (used_ranges_ > 0) {
    auto fix = [&](Node*& at, uint32_t& offset) {
      if (at == parent) {
        if (offset > index) --offset;
      } else if (contains(node, at)) {
        at = parent;
        offset = index;
      }
    };
    for (LiveRange& r : ranges_.view()) {
      if (!r.owner) continue;
      fix(r.start, r.start_offset);
      fix(r.end, r.end_offset);
    }
  }
  if (used_iterators_ == 0) return;
  for (LiveIterator& it : iterators_.view()) {
    // (only a node inside the root that is the reference node or above it matters)
    if (!it.owner || node == it.root || !contains(node, it.reference) || !contains(it.root, node)) continue;
    if (it.before) {
      // the first node after `node` and what is in it, still inside the root
      Node* next = nullptr;
      for (Node* n = node; n && n != it.root; n = n->parent) {
        if (n->next) {
          next = n->next;
          break;
        }
      }
      if (next) {
        it.reference = next;
        continue;
      }
      it.before = false;
    }
    if (!node->prev) {
      it.reference = parent;
    } else {
      Node* last = node->prev;
      while (last->last_child) last = last->last_child;
      it.reference = last;
    }
  }
}

// The insert steps: boundaries of the parent after the new child move up by one.
void LiveRanges::inserted(Node* node) {
  Node* parent = node->parent;
  if (!parent || used_ranges_ == 0 || !node->next) return;   // (the last child: no boundary of the parent is after it)
  const uint32_t index = child_index(node);
  for (LiveRange& r : ranges_.view()) {
    if (!r.owner) continue;
    if (r.start == parent && r.start_offset > index) ++r.start_offset;
    if (r.end == parent && r.end_offset > index) ++r.end_offset;
  }
}

// "Replace data", steps 8 to 11.
void LiveRanges::data_replaced(Node* node, uint32_t offset, uint32_t count, uint32_t added) {
  if (used_ranges_ == 0) return;
  auto fix = [&](Node* at, uint32_t& o) {
    if (at != node || o <= offset) return;
    if (o - offset <= count) o = offset;
    else o = o - count + added;
  };
  for (LiveRange& r : ranges_.view()) {
    if (!r.owner) continue;
    fix(r.start, r.start_offset);
    fix(r.end, r.end_offset);
  }
}

// "Split a Text node", steps 7.2 to 7.5.
void LiveRanges::text_split(Node* node, uint32_t offset, Node* tail) {
  Node* parent = node->parent;
  if (used_ranges_ == 0 || !parent) return;
  const uint32_t after = child_index(node) + 1;
  auto fix = [&](Node*& at, uint32_t& o) {
    if (at == node && o > offset) {
      at = tail;
      o -= offset;
    } else if (at == parent && o == after) {
      ++o;
    }
  };
  for (LiveRange& r : ranges_.view()) {
    if (!r.owner) continue;
    fix(r.start, r.start_offset);
    fix(r.end, r.end_offset);
  }
}

// normalize(), step 6.
void LiveRanges::text_merging(Node* from, Node* into, uint32_t length) {
  if (used_ranges_ == 0) return;
  Node* parent = from->parent;
  const uint32_t index = child_index(from);
  auto fix = [&](Node*& at, uint32_t& o) {
    if (at == from) {
      at = into;
      o += length;
    } else if (parent && at == parent && o == index) {
      at = into;
      o = length;
    }
  };
  for (LiveRange& r : ranges_.view()) {
    if (!r.owner) continue;
    fix(r.start, r.start_offset);
    fix(r.end, r.end_offset);
  }
}

void LiveRanges::node_moved(Node* from, Node* to) {
  for (LiveRange& r : ranges_.view()) {
    if (!r.owner) continue;
    if (r.start == from) r.start = to;
    if (r.end == from) r.end = to;
  }
  for (LiveIterator& it : iterators_.view()) {
    if (!it.owner) continue;
    if (it.root == from) it.root = to;
    if (it.reference == from) it.reference = to;
  }
}

}  // namespace dom
