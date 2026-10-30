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
#include "ObjectContainer/IObjectContainer.hpp"
#include "Utility/ArrayView.hpp"

// A hive (the C++26 std::hive): an unordered collection with O(1) insertion and
// erasure whose elements NEVER move. A pointer to an element stays valid until
// that element is erased (or the hive is cleared or destroyed), no matter how
// many other elements come and go - which is what makes it the container for
// objects other code points at (tasks, connections, buffers, descriptors).
//
// The elements live in blocks that are allocated as the hive grows (8 elements
// at first, doubling up to 1024). A block keeps a bitmap of its live slots and a
// free list through its erased slots, so insert() reuses a hole before it takes
// fresh space, and iteration skips holes a word of the bitmap at a time. A block
// whose last element is erased is given back, except for the only one left.
// The order of iteration is not the order of insertion, and it can change when
// holes are refilled. T needs no move or copy constructor for emplace().
// Every allocation goes through the IAllocator given.
//
//   Hive<Task> tasks(allocator);
//   Task* t = tasks.emplace(args...);     // nullptr if memory ran out
//   tasks.erase(t);                       // false if t is not a live element of this hive
//   for (Task& task : tasks) { ... }      // range-for, no allocation
//
//   ObjectCursor c;                       // erase while walking
//   for (bool more = tasks.first(&c); more;)
//     more = done(*tasks.element(c)) ? tasks.erase(&c) : tasks.next(&c);
//
// Cursors and iterators stay valid across insertions of other elements only if
// they are not at the element being erased; an insertion may add elements before
// or after a walk's position, so do not insert while walking unless you do not
// care whether the walk sees them. Erasing an element invalidates cursors to it.
template <class T>
class Hive final : public IObjectContainer<T> {
 public:
  static constexpr uint64_t kMinBlock = 8;
  static constexpr uint64_t kMaxBlock = 1024;

  explicit Hive(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Hive() { clear(); }

  uint64_t count() const override { return size_; }
  // Elements that fit before another block is allocated.
  uint64_t capacity() const { return capacity_; }

  // Destroys every element and gives all the memory back.
  void clear() override {
    while (head_) release_block(head_, true);
    free_head_ = nullptr;
    size_ = 0;
    capacity_ = 0;
    next_block_size_ = kMinBlock;
  }

  // Makes room for at least `total` elements without further allocation. False if memory ran out.
  bool reserve(uint64_t total) {
    while (capacity_ < total) {
      if (!add_block()) return false;
    }
    return true;
  }

  // Constructs a new element in place and returns it, or nullptr if memory ran out.
  template <class... Args>
  T* emplace(Args&&... args) {
    Block* block = free_head_ ? free_head_ : add_block();
    if (!block) return nullptr;
    uint64_t slot;
    if (block->free_slot != kNone) {
      slot = block->free_slot;
      block->free_slot = block->links[slot];
    } else {
      slot = block->fresh++;  // never used before; fresh < capacity because the block has room
    }
    T* element = new (&block->slots[slot]) T(static_cast<Args&&>(args)...);
    block->live[slot / 64] |= uint64_t{1} << (slot % 64);
    if (++block->count == block->slots.size()) unlink_free(block);
    ++size_;
    return element;
  }
  T* insert(const T& value) { return emplace(value); }
  T* insert(T&& value) { return emplace(container_support::move(value)); }

  // Erases the element `element` points at. False (and nothing happens) if the
  // pointer is not a live element of this hive. O(number of blocks).
  bool erase(const T* element) {
    ObjectCursor cursor;
    return cursor_of(element, &cursor) && erase_at(cursor);
  }

  // Erases the element at `*cursor` and moves the cursor to the element that
  // followed it. Returns true if there is one; false when the erased element was
  // the last (the cursor is then not valid, and also if the cursor was not valid
  // to begin with, in which case nothing is erased). O(number of blocks).
  bool erase(ObjectCursor* cursor) {
    ObjectCursor following = *cursor;
    bool more = next(&following);
    if (!erase_at(*cursor)) return false;
    if (more) *cursor = following;
    return more;
  }

  // True if `element` points at a live element of this hive. O(number of blocks).
  bool contains(const T* element) const {
    ObjectCursor cursor;
    return cursor_of(element, &cursor);
  }

  // Finds the cursor of the element `element` points at; false if it is not a live element of this hive.
  bool cursor_of(const T* element, ObjectCursor* cursor) const {
    const uint64_t address = reinterpret_cast<uint64_t>(element);
    for (Block* block = head_; block; block = block->next) {
      const uint64_t base = reinterpret_cast<uint64_t>(block->slots.data());
      if (address < base || address - base >= block->slots.size_bytes()) continue;
      if ((address - base) % sizeof(T) != 0) return false;
      const uint64_t slot = (address - base) / sizeof(T);
      if (!is_live(*block, slot)) return false;
      cursor->index = slot;
      cursor->node = block;
      return true;
    }
    return false;
  }

  // --- IObjectContainer iteration: the cursor's `node` is the block and `index` the slot in it.
  bool first(ObjectCursor* cursor) const override { return seek(head_, 0, cursor); }
  bool next(ObjectCursor* cursor) const override {
    Block* block = static_cast<Block*>(cursor->node);
    return block && seek(block, cursor->index + 1, cursor);
  }
  T* element(const ObjectCursor& cursor) const override {
    Block* block = static_cast<Block*>(cursor.node);
    return &block->slots[cursor.index];
  }

 private:
  static constexpr uint64_t kNone = UINT64_MAX;

  struct Block {
    ArrayView<T> slots;          // the storage; only slots with a bit set in `live` hold an element
    ArrayView<uint64_t> live;    // one bit per slot
    ArrayView<uint64_t> links;   // per slot: the next erased slot, while the slot is on the free chain
    uint64_t count = 0;          // live elements
    uint64_t fresh = 0;          // slots below this have been used before; those from here on never
    uint64_t free_slot = kNone;  // head of the chain of erased slots below `fresh`
    Block* previous = nullptr;   // all blocks, in allocation order
    Block* next = nullptr;
    Block* free_previous = nullptr;  // blocks with room left (a hole or fresh slots)
    Block* free_next = nullptr;
    bool on_free_list = false;
  };

  static bool is_live(const Block& block, uint64_t slot) {
    return slot < block.slots.size() && (block.live[slot / 64] >> (slot % 64) & 1) != 0;
  }

  // Moves `cursor` to the first live slot at or after `from` in `block` or a later block.
  static bool seek(Block* block, uint64_t from, ObjectCursor* cursor) {
    for (; block; block = block->next, from = 0) {
      if (block->count == 0) continue;
      for (uint64_t word = from / 64; word < block->live.size(); ++word, from = word * 64) {
        uint64_t bits = block->live[word];
        if (from % 64) bits &= ~uint64_t{0} << (from % 64);
        if (!bits) continue;
        cursor->index = word * 64 + static_cast<uint64_t>(__builtin_ctzll(bits));
        cursor->node = block;
        return true;
      }
    }
    return false;
  }

  Block* add_block() {
    const uint64_t slots = next_block_size_;
    Block* block = container_support::allocate_array<Block>(allocator_, 1);
    if (!block) return nullptr;
    new (block) Block();
    T* storage = container_support::allocate_array<T>(allocator_, slots);
    uint64_t* live = container_support::allocate_array<uint64_t>(allocator_, (slots + 63) / 64);
    uint64_t* links = container_support::allocate_array<uint64_t>(allocator_, slots);
    if (!storage || !live || !links) {
      if (storage) allocator_.deallocate(storage);
      if (live) allocator_.deallocate(live);
      if (links) allocator_.deallocate(links);
      block->~Block();
      allocator_.deallocate(block);
      return nullptr;
    }
    block->slots = ArrayView<T>(storage, slots);
    block->live = ArrayView<uint64_t>(live, (slots + 63) / 64);
    block->links = ArrayView<uint64_t>(links, slots);
    block->live.fill(0);
    block->previous = tail_;
    (tail_ ? tail_->next : head_) = block;
    tail_ = block;
    link_free(block);
    capacity_ += slots;
    if (next_block_size_ < kMaxBlock) next_block_size_ *= 2;
    return block;
  }

  // Unlinks and frees a block; `destroy` also destroys the elements still in it.
  void release_block(Block* block, bool destroy) {
    if (destroy) {
      for (uint64_t slot = 0; slot < block->fresh; ++slot) {
        if (is_live(*block, slot)) block->slots[slot].~T();
      }
      size_ -= block->count;
    }
    if (block->on_free_list) unlink_free(block);
    (block->previous ? block->previous->next : head_) = block->next;
    (block->next ? block->next->previous : tail_) = block->previous;
    capacity_ -= block->slots.size();
    allocator_.deallocate(block->slots.data());
    allocator_.deallocate(block->live.data());
    allocator_.deallocate(block->links.data());
    block->~Block();
    allocator_.deallocate(block);
  }

  void link_free(Block* block) {
    block->free_previous = nullptr;
    block->free_next = free_head_;
    if (free_head_) free_head_->free_previous = block;
    free_head_ = block;
    block->on_free_list = true;
  }
  void unlink_free(Block* block) {
    (block->free_previous ? block->free_previous->free_next : free_head_) = block->free_next;
    if (block->free_next) block->free_next->free_previous = block->free_previous;
    block->free_previous = block->free_next = nullptr;
    block->on_free_list = false;
  }

  // Erases the element at a cursor, which is checked to name a live slot of one of this hive's blocks.
  bool erase_at(const ObjectCursor& cursor) {
    Block* block = head_;
    while (block && block != cursor.node) block = block->next;
    if (!block || !is_live(*block, cursor.index)) return false;
    const uint64_t slot = cursor.index;
    block->slots[slot].~T();
    block->live[slot / 64] &= ~(uint64_t{1} << (slot % 64));
    block->links[slot] = block->free_slot;
    block->free_slot = slot;
    if (!block->on_free_list) link_free(block);
    --block->count;
    --size_;
    if (block->count == 0) {
      if (head_ != tail_) {
        release_block(block, false);
      } else {
        block->fresh = 0;  // the only block, now empty: start over instead of keeping a chain of holes
        block->free_slot = kNone;
      }
    }
    return true;
  }

  allocators::IAllocator& allocator_;
  Block* head_ = nullptr;
  Block* tail_ = nullptr;
  Block* free_head_ = nullptr;
  uint64_t size_ = 0;
  uint64_t capacity_ = 0;
  uint64_t next_block_size_ = kMinBlock;
};
