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
#include "Process/PageMap.hpp"

namespace process {

PageMap::~PageMap() {
  if (root_) free_tree(root_, 0);
}

void PageMap::free_tree(Node* node, uint32_t level) {
  if (level < 3) {
    for (void* entry : node->entries) if (entry) free_tree(node_of(entry), level + 1);
  }
  allocator_.deallocate(node);
}

PageMap::Node* PageMap::new_node() {
  Node* node = static_cast<Node*>(allocator_.allocate(sizeof(Node)));
  if (!node) return nullptr;
  for (void*& entry : node->entries) entry = nullptr;
  node->used = 0;
  return node;
}

uint8_t* PageMap::get(uint64_t page) const {
  if (page >= kTop) return nullptr;
  const Node* node = root_;
  for (uint32_t level = 0; node && level < 3; ++level) node = node_of(node->entries[index(page, level)]);
  return node ? frame_of(node->entries[index(page, 3)]) : nullptr;
}

bool PageMap::set(uint64_t page, uint8_t* frame) {
  if (page >= kTop || page % kPageSize != 0 || !frame) return false;
  if (!root_ && !(root_ = new_node())) return false;
  Node* node = root_;
  for (uint32_t level = 0; level < 3; ++level) {
    void*& entry = node->entries[index(page, level)];
    if (!entry) {
      Node* next = new_node();
      if (!next) return false;      // (levels made on the way stay: they are empty and cost a little until the space goes)
      entry = next;
      ++node->used;
    }
    node = node_of(entry);
  }
  void*& leaf = node->entries[index(page, 3)];
  if (leaf) return false;
  leaf = frame;
  ++node->used;
  ++count_;
  return true;
}

uint8_t* PageMap::take(uint64_t page) {
  uint8_t* frame = nullptr;
  if (page < kTop) remove(page, page + kPageSize, [&](uint8_t* f) { frame = f; });
  return frame;
}

bool PageMap::any_below(const Node* node, uint32_t level, uint64_t base, uint64_t start, uint64_t end) {
  const uint64_t span = uint64_t{1} << shift(level);
  const uint32_t first = start > base ? uint32_t((start - base) >> shift(level)) : 0;
  for (uint32_t i = first; i < kEntries; ++i) {
    const uint64_t at = base + uint64_t{i} * span;
    if (at >= end) return false;
    if (node->entries[i] && (level == 3 || any_below(node_of(node->entries[i]), level + 1, at, start, end))) return true;
  }
  return false;
}

bool PageMap::any(uint64_t start, uint64_t end) const {
  return root_ && start < end && any_below(root_, 0, 0, start, end);
}

}  // namespace process
