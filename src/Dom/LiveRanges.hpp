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

// The live ranges and node iterators of a page (DOM standard, "live range" and "NodeIterator"): where each one is, kept here so that the tree operations
// can move them as the standard says the moment a node is taken out, put in, or has its text changed - the "live range pre-remove steps", the insert steps,
// "replace data", "split a Text node", normalize, and the "NodeIterator pre-removing steps".
//
// A Document that has a registry (Document::set_live) tells it of every change; while the registry is empty (active() is false) that is one test per change, so
// a page that never makes a Range or a NodeIterator pays nothing. The offsets of text are in the units the owner counts in (the script engine: UTF-16 code
// units); this class never looks into the text.
//
// The entries belong to whoever made them (a script object): `owner` is that owner's own pointer, never followed here. sweep() drops the entries whose owner
// has gone (the script engine's collector says which), so the registry does not keep a range alive and does not grow with every range a page ever made.
#pragma once
#include "Dom/Dom.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/Optional.hpp"

namespace dom {

struct LiveRange {
  Node *start = nullptr, *end = nullptr;
  uint32_t start_offset = 0, end_offset = 0;
  const void* owner = nullptr;   // nullptr: the slot is free
};

struct LiveIterator {
  Node *root = nullptr, *reference = nullptr;
  bool before = true;            // the pointer is before the reference node
  const void* owner = nullptr;   // nullptr: the slot is free
};

class LiveRanges {
 public:
  explicit LiveRanges(allocators::IAllocator& allocator) : ranges_(allocator), iterators_(allocator) {}
  LiveRanges(const LiveRanges&) = delete;
  LiveRanges& operator=(const LiveRanges&) = delete;

  // Is there anything to keep up to date?
  bool active() const { return used_ranges_ + used_iterators_ > 0; }
  uint32_t range_count() const { return used_ranges_; }
  uint32_t iterator_count() const { return used_iterators_; }

  // A new range collapsed at (node, 0), or a new iterator before `root`. The id names it until remove_*() or sweep() frees it. Empty if memory ran out.
  [[nodiscard]] Optional<uint32_t> add_range(Node* node, const void* owner);
  [[nodiscard]] Optional<uint32_t> add_iterator(Node* root, const void* owner);
  void remove_range(uint32_t id);
  void remove_iterator(uint32_t id);
  // The entry of an id, or nullptr if there is none. The pointer is good until the next add_*().
  LiveRange* range(uint32_t id) { return id < ranges_.count() && ranges_[id].owner ? &ranges_[id] : nullptr; }
  LiveIterator* iterator(uint32_t id) { return id < iterators_.count() && iterators_[id].owner ? &iterators_[id] : nullptr; }

  // Frees every entry whose owner `gone(owner)` says is gone.
  template <class F>
  void sweep(F&& gone) {
    for (uint64_t i = 0; i < ranges_.count() && used_ranges_ > 0; ++i)
      if (ranges_[i].owner && gone(ranges_[i].owner)) remove_range(static_cast<uint32_t>(i));
    for (uint64_t i = 0; i < iterators_.count() && used_iterators_ > 0; ++i)
      if (iterators_[i].owner && gone(iterators_[i].owner)) remove_iterator(static_cast<uint32_t>(i));
  }

  // --- What the tree operations tell (only worth calling while active()).
  // `node` is about to be taken out of its parent (it still has it).
  void removing(Node* node);
  // `node` was put into its parent.
  void inserted(Node* node);
  // `count` units of the text of `node` from `offset` on were replaced by `added` units.
  void data_replaced(Node* node, uint32_t offset, uint32_t count, uint32_t added);
  // The text of `node` after `offset` went to the new node `tail`, which is already its next sibling if `node` has a parent ("split a Text node", before the
  // text is cut: follow with data_replaced(node, offset, <units cut>, 0)).
  void text_split(Node* node, uint32_t offset, Node* tail);
  // normalize(): the text of `from` (a sibling still in the tree) is about to be put at the end of `into`, which is `length` units long before that.
  void text_merging(Node* from, Node* into, uint32_t length);
  // `from` was made again as `to` (a node adopted into another document).
  void node_moved(Node* from, Node* to);

 private:
  List<LiveRange> ranges_;
  List<LiveIterator> iterators_;
  uint32_t used_ranges_ = 0, used_iterators_ = 0;
  uint32_t free_range_hint_ = 0, free_iterator_hint_ = 0;   // (no free slot below these)
};

// The number of siblings before `node`.
uint32_t child_index(const Node* node);

}  // namespace dom
