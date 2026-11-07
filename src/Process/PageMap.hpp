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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"

namespace process {

// The pages of memory behind the private pages of an address space, by user address: a sparse tree shaped like the x86-64 page tables (four levels of 512 entries,
// a level made only when a page below it gets memory). A page without memory reads as zeros, so reserving or committing a range costs nothing until it is touched.
// The map does not own the pages: whoever takes a page out (take, remove) frees it. Addresses are below 2^48 and page-aligned.
class PageMap {
 public:
  static constexpr uint64_t kPageSize = 4096;
  static constexpr uint64_t kTop = uint64_t{1} << 48;

  explicit PageMap(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~PageMap();      // frees the tree (the pages still in it must have been taken out, or they are lost)
  PageMap(const PageMap&) = delete;
  PageMap& operator=(const PageMap&) = delete;

  uint8_t* get(uint64_t page) const;
  // Puts `frame` at `page`, which must have none. False if a level of the tree could not be made.
  bool set(uint64_t page, uint8_t* frame);
  // Takes the page's memory out of the map (nullptr if it had none).
  uint8_t* take(uint64_t page);
  // Calls fn(page, frame) for every page in [start, end) that has memory, in address order; empty parts of the tree are skipped.
  template <typename F>
  void for_each(uint64_t start, uint64_t end, F&& fn) const {
    if (root_ && start < end) walk(root_, 0, 0, start, end, fn);
  }
  // Takes every page in [start, end) out of the map, calling fn(frame) for each; levels left empty are freed.
  template <typename F>
  void remove(uint64_t start, uint64_t end, F&& fn) {
    if (root_ && start < end) prune(root_, 0, 0, start, end, fn);
  }
  // Whether any page of [start, end) has memory.
  bool any(uint64_t start, uint64_t end) const;
  uint64_t count() const { return count_; }

 private:
  static constexpr uint32_t kEntries = 512;
  struct Node {
    void* entries[kEntries];
    uint32_t used;
  };
  static constexpr uint32_t shift(uint32_t level) { return 39 - 9 * level; }
  static constexpr uint32_t index(uint64_t address, uint32_t level) { return uint32_t(address >> shift(level)) & (kEntries - 1); }
  Node* new_node();
  void free_tree(Node* node, uint32_t level);
  static bool any_below(const Node* node, uint32_t level, uint64_t base, uint64_t start, uint64_t end);
  // The tree holds pages at level 3 and nodes above it: the two casts that tell them apart are here.
  static Node* node_of(void* entry) { return static_cast<Node*>(entry); }
  static uint8_t* frame_of(void* entry) { return static_cast<uint8_t*>(entry); }

  template <typename F>
  static void walk(const Node* node, uint32_t level, uint64_t base, uint64_t start, uint64_t end, F& fn) {
    const uint64_t span = uint64_t{1} << shift(level);
    const uint32_t first = start > base ? uint32_t((start - base) >> shift(level)) : 0;
    for (uint32_t i = first; i < kEntries; ++i) {
      const uint64_t at = base + uint64_t{i} * span;
      if (at >= end) return;
      void* entry = node->entries[i];
      if (!entry) continue;
      if (level == 3) fn(at, frame_of(entry));
      else walk(node_of(entry), level + 1, at, start, end, fn);
    }
  }
  // Returns whether the node is empty afterwards.
  template <typename F>
  bool prune(Node* node, uint32_t level, uint64_t base, uint64_t start, uint64_t end, F& fn) {
    const uint64_t span = uint64_t{1} << shift(level);
    const uint32_t first = start > base ? uint32_t((start - base) >> shift(level)) : 0;
    for (uint32_t i = first; i < kEntries && node->used; ++i) {
      const uint64_t at = base + uint64_t{i} * span;
      if (at >= end) break;
      void* entry = node->entries[i];
      if (!entry) continue;
      if (level == 3) {
        fn(frame_of(entry));
        node->entries[i] = nullptr;
        --node->used;
        --count_;
      } else if (prune(node_of(entry), level + 1, at, start, end, fn)) {
        allocator_.deallocate(entry);
        node->entries[i] = nullptr;
        --node->used;
      }
    }
    return node->used == 0 && node != root_;
  }

  allocators::IAllocator& allocator_;
  Node* root_ = nullptr;
  uint64_t count_ = 0;
};

}  // namespace process
