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

// Extent trees (ext4): finding the blocks of a file, loading a tree into a list, and writing a list back as the smallest tree that holds it.
#include "FileSystem/Ext/ExtChecksum.hpp"
#include "FileSystem/Ext/ExtFileSystem.hpp"

namespace filesystem {

using namespace ext;

namespace {
constexpr uint32_t kUnknownDepth = ~0u;       // the depth of the root is not known beforehand
constexpr uint64_t kMaxExtents = 1u << 22;    // a file with more extents than this is treated as damaged
constexpr uint64_t kNoLimit = ~uint64_t{0};

struct NodeHeader {
  uint32_t entries = 0, max = 0, depth = 0;
};

Status read_header(ConstByteView node, uint32_t capacity, NodeHeader* header) {
  if (node.size() < kExtentHeader || rd16(node, 0) != kExtentMagic) return Status::Corrupt;
  header->entries = rd16(node, 2);
  header->max = rd16(node, 4);
  header->depth = rd16(node, 6);
  if (header->max > capacity || header->entries > header->max || kExtentHeader + uint64_t{header->max} * kExtentEntry > node.size()) return Status::Corrupt;
  return Status::Ok;
}

uint64_t index_child(ConstByteView node, uint32_t i) {
  const uint32_t at = kExtentHeader + i * kExtentEntry;
  return rd32(node, at + 4) | (uint64_t{rd16(node, at + 8)} << 32);
}
}  // namespace

// How many tree blocks (besides the root in the inode) a list of `extents` extents needs, and how deep that tree is.
uint64_t ExtFileSystem::ext_nodes_needed(uint64_t extents, uint32_t capacity, uint32_t* depth) {
  *depth = 0;
  if (extents <= kExtentRootMax) return 0;
  uint64_t total = 0, count = extents;
  for (;;) {
    const uint64_t nodes = (count + capacity - 1) / capacity;
    total += nodes;
    ++*depth;
    if (nodes <= kExtentRootMax) return total;
    count = nodes;
  }
}

// The root of an empty tree, in i_block.
void ExtFileSystem::ext_init_root(Inode* inode) const {
  ByteView root = inode->bytes().subview(kInBlock, kInlineBlocks * 4);
  root.fill(0);
  wr16(root, 0, kExtentMagic);
  wr16(root, 4, kExtentRootMax);
}

void ExtFileSystem::extent_checksum_set(const Inode& inode, ByteView node) const {
  if (!metadata_csum_) return;
  const uint64_t tail = kExtentHeader + uint64_t{rd16(node, 4)} * kExtentEntry;
  wr32(node, tail, crc32c(inode_seed(inode), node.first(tail)));
}

// Finds the extent holding file block `block`. *mapped is false for a hole, and *hole_end then a block up to which it certainly reaches.
Status ExtFileSystem::ext_lookup(const Inode& inode, uint64_t block, Extent* found, bool* mapped, uint64_t* hole_end) {
  *mapped = false;
  *hole_end = kNoLimit;
  ByteBuffer buffer(allocator_);
  ConstByteView node = inode.bytes().subview(kInBlock, kInlineBlocks * 4);
  uint32_t expected = kUnknownDepth;
  for (uint32_t level = 0;; ++level) {
    NodeHeader header;
    Status s = read_header(node, level == 0 ? kExtentRootMax : ext_node_capacity(), &header);
    if (s != Status::Ok) return s;
    if (level == 0 ? header.depth > kExtentMaxDepth : header.depth != expected) return Status::Corrupt;
    int64_t chosen = -1;
    for (uint32_t i = 0; i < header.entries; ++i) {
      const uint64_t start = rd32(node, kExtentHeader + i * kExtentEntry);
      if (start <= block) {
        chosen = i;
      } else {
        if (start < *hole_end) *hole_end = start;
        break;
      }
    }
    if (header.depth == 0) {
      if (chosen >= 0) {
        const uint32_t at = kExtentHeader + static_cast<uint32_t>(chosen) * kExtentEntry;
        uint32_t length = rd16(node, at + 4);
        bool unwritten = false;
        if (length > kExtentMaxInit) {
          length -= kExtentMaxInit;
          unwritten = true;
        }
        const uint64_t first = rd32(node, at), start = rd32(node, at + 8) | (uint64_t{rd16(node, at + 6)} << 32);
        if (length == 0 || start == 0 || start >= blocks_count_ || length > blocks_count_ - start) return Status::Corrupt;
        if (block < first + length) {
          found->block = first;
          found->start = start;
          found->length = length;
          found->unwritten = unwritten;
          *mapped = true;
          return Status::Ok;
        }
      }
      if (*hole_end <= block) return Status::Corrupt;  // (entries out of order)
      return Status::Ok;
    }
    if (chosen < 0) {  // before the first entry
      if (*hole_end == kNoLimit || *hole_end <= block) return Status::Corrupt;
      return Status::Ok;
    }
    const uint64_t child = index_child(node, static_cast<uint32_t>(chosen));
    if (child == 0 || child >= blocks_count_) return Status::Corrupt;
    if (buffer.size() == 0 && !buffer.put_zeros(block_size_)) return Status::OutOfMemory;
    s = read_block(child, buffer.writable_view());
    if (s != Status::Ok) return s;
    node = buffer.view();
    expected = header.depth - 1;
  }
}

Status ExtFileSystem::ext_map_run(const Inode& inode, uint64_t first, uint64_t max, uint64_t* block, uint64_t* run) {
  Extent extent;
  bool mapped = false;
  uint64_t hole_end = 0;
  const Status s = ext_lookup(inode, first, &extent, &mapped, &hole_end);
  if (s != Status::Ok) return s;
  uint64_t available;
  if (mapped) {
    *block = extent.unwritten ? 0 : extent.start + (first - extent.block);  // (an unwritten extent reads as zeros)
    available = extent.block + extent.length - first;
  } else {
    *block = 0;
    available = hole_end - first;
  }
  *run = available < max ? available : max;
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Loading and storing a whole tree

Status ExtFileSystem::ext_load_node(const Inode& inode, ConstByteView node, uint32_t expected_depth, ExtentList* extents, BlockList* nodes) {
  NodeHeader header;
  Status s = read_header(node, expected_depth == kUnknownDepth ? kExtentRootMax : ext_node_capacity(), &header);
  if (s != Status::Ok) return s;
  if (expected_depth == kUnknownDepth ? header.depth > kExtentMaxDepth : header.depth != expected_depth) return Status::Corrupt;
  if (header.depth == 0) {
    for (uint32_t i = 0; i < header.entries; ++i) {
      const uint32_t at = kExtentHeader + i * kExtentEntry;
      Extent extent;
      uint32_t length = rd16(node, at + 4);
      if (length > kExtentMaxInit) {
        length -= kExtentMaxInit;
        extent.unwritten = true;
      }
      extent.block = rd32(node, at);
      extent.length = length;
      extent.start = rd32(node, at + 8) | (uint64_t{rd16(node, at + 6)} << 32);
      if (length == 0 || extent.start == 0 || extent.start >= blocks_count_ || length > blocks_count_ - extent.start) return Status::Corrupt;
      if (extents->count() > 0) {
        const Extent& last = (*extents)[extents->count() - 1];
        if (extent.block < last.block + last.length) return Status::Corrupt;  // out of order or overlapping
      }
      if (extents->count() >= kMaxExtents || !extents->push_back(extent)) return Status::Corrupt;
    }
    return Status::Ok;
  }
  ByteBuffer child_node(allocator_);
  if (!child_node.put_zeros(block_size_)) return Status::OutOfMemory;
  for (uint32_t i = 0; i < header.entries; ++i) {
    const uint64_t child = index_child(node, i);
    if (child == 0 || child >= blocks_count_) return Status::Corrupt;
    if (!nodes->push_back(child)) return Status::OutOfMemory;
    s = read_block(child, child_node.writable_view());
    if (s != Status::Ok) return s;
    s = ext_load_node(inode, child_node.view(), header.depth - 1, extents, nodes);
    if (s != Status::Ok) return s;
  }
  return Status::Ok;
}

Status ExtFileSystem::ext_load(const Inode& inode, ExtentList* extents, BlockList* nodes) {
  return ext_load_node(inode, inode.bytes().subview(kInBlock, kInlineBlocks * 4), kUnknownDepth, extents, nodes);
}

// Writes `extents` as a tree: in i_block if there are at most four, otherwise leaves filled one after the other and as many index levels
// above them as it takes to get down to four entries in the root. `nodes` are the blocks the old tree used (more are taken if the new one
// needs them, and those it no longer needs are given back).
Status ExtFileSystem::ext_store(Inode* inode, const ExtentList& extents, BlockList* nodes) {
  const uint32_t capacity = ext_node_capacity();
  uint32_t depth = 0;
  const uint64_t needed = ext_nodes_needed(extents.count(), capacity, &depth);
  while (nodes->count() < needed) {
    uint64_t block = 0;
    const Status s = alloc_one(*inode, extents.count() > 0 ? extents[0].start : 0, &block);
    if (s != Status::Ok) return s;
    if (!nodes->push_back(block)) {
      free_blocks(block, 1);
      return Status::OutOfMemory;
    }
    inode_add_blocks(inode, 1);
  }
  ByteView root = inode->bytes().subview(kInBlock, kInlineBlocks * 4);
  auto put_extent = [&](ByteView node, uint32_t slot, const Extent& e) {
    const uint32_t at = kExtentHeader + slot * kExtentEntry;
    wr32(node, at, static_cast<uint32_t>(e.block));
    wr16(node, at + 4, e.unwritten ? e.length + kExtentMaxInit : e.length);
    wr16(node, at + 6, static_cast<uint32_t>(e.start >> 32));
    wr32(node, at + 8, static_cast<uint32_t>(e.start));
  };
  auto put_index = [&](ByteView node, uint32_t slot, uint64_t first, uint64_t child) {
    const uint32_t at = kExtentHeader + slot * kExtentEntry;
    wr32(node, at, static_cast<uint32_t>(first));
    wr32(node, at + 4, static_cast<uint32_t>(child));
    wr16(node, at + 8, static_cast<uint32_t>(child >> 32));
    wr16(node, at + 10, 0);
  };
  auto put_header = [&](ByteView node, uint32_t entries, uint32_t max, uint32_t node_depth) {
    wr16(node, 0, kExtentMagic);
    wr16(node, 2, entries);
    wr16(node, 4, max);
    wr16(node, 6, node_depth);
    wr32(node, 8, 0);
  };

  if (depth == 0) {
    root.fill(0);
    put_header(root, static_cast<uint32_t>(extents.count()), kExtentRootMax, 0);
    for (uint32_t i = 0; i < extents.count(); ++i) put_extent(root, i, extents[i]);
  } else {
    ByteBuffer block(allocator_);
    if (!block.put_zeros(block_size_)) return Status::OutOfMemory;
    List<uint64_t> level_blocks(allocator_), level_first(allocator_);  // the nodes of the level just written, and the first file block under each
    uint64_t used = 0;
    // the leaves
    const uint64_t leaves = (extents.count() + capacity - 1) / capacity;
    for (uint64_t leaf = 0; leaf < leaves; ++leaf) {
      const uint64_t from = leaf * capacity, to = from + capacity < extents.count() ? from + capacity : extents.count();
      const uint64_t where = (*nodes)[used++];
      block.writable_view().fill(0);
      put_header(block.writable_view(), static_cast<uint32_t>(to - from), capacity, 0);
      for (uint64_t i = from; i < to; ++i) put_extent(block.writable_view(), static_cast<uint32_t>(i - from), extents[i]);
      extent_checksum_set(*inode, block.writable_view());
      Status s = write_block(where, block.view());
      if (s != Status::Ok) return s;
      if (!level_blocks.push_back(where) || !level_first.push_back(extents[from].block)) return Status::OutOfMemory;
    }
    // the index levels (the topmost goes into the inode)
    for (uint32_t level = 1; level < depth; ++level) {
      List<uint64_t> next_blocks(allocator_), next_first(allocator_);
      const uint64_t count = level_blocks.count();
      for (uint64_t from = 0; from < count; from += capacity) {
        const uint64_t to = from + capacity < count ? from + capacity : count;
        const uint64_t where = (*nodes)[used++];
        block.writable_view().fill(0);
        put_header(block.writable_view(), static_cast<uint32_t>(to - from), capacity, level);
        for (uint64_t i = from; i < to; ++i) put_index(block.writable_view(), static_cast<uint32_t>(i - from), level_first[i], level_blocks[i]);
        extent_checksum_set(*inode, block.writable_view());
        Status s = write_block(where, block.view());
        if (s != Status::Ok) return s;
        if (!next_blocks.push_back(where) || !next_first.push_back(level_first[from])) return Status::OutOfMemory;
      }
      level_blocks.clear();
      level_first.clear();
      for (uint64_t i = 0; i < next_blocks.count(); ++i) {
        if (!level_blocks.push_back(next_blocks[i]) || !level_first.push_back(next_first[i])) return Status::OutOfMemory;
      }
    }
    root.fill(0);
    put_header(root, static_cast<uint32_t>(level_blocks.count()), kExtentRootMax, depth);
    for (uint64_t i = 0; i < level_blocks.count(); ++i) put_index(root, static_cast<uint32_t>(i), level_first[i], level_blocks[i]);
  }
  // blocks the smaller tree does not need
  for (uint64_t i = needed; i < nodes->count(); ++i) {
    const Status s = queue_free(inode, (*nodes)[i]);
    if (s != Status::Ok) return s;
  }
  while (nodes->count() > needed) nodes->remove_at(nodes->count() - 1);
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Mapping and freeing

Status ExtFileSystem::ext_map_range(Inode* inode, uint64_t first, uint32_t count, bool create, ArrayView<Mapping> out, uint32_t* done) {
  *done = 0;
  uint32_t i = 0;
  // What is mapped already, from the tree.
  while (i < count) {
    Extent extent;
    bool mapped = false;
    uint64_t hole_end = 0;
    const Status s = ext_lookup(*inode, first + i, &extent, &mapped, &hole_end);
    if (s != Status::Ok) return s;
    if (!mapped && create) break;
    if (mapped && extent.unwritten && create) return Status::Unsupported;  // (writing into preallocated space would need the extent split and converted)
    const uint64_t position = first + i;
    const uint64_t available = mapped ? extent.block + extent.length - position : hole_end - position;
    const uint32_t run = static_cast<uint32_t>(available < count - i ? available : count - i);
    for (uint32_t k = 0; k < run; ++k) out[i + k] = Mapping{mapped && !extent.unwritten ? extent.start + (position - extent.block) + k : 0, false};
    i += run;
    *done = i;
  }
  if (i == count) return Status::Ok;

  // Holes to fill: the whole list is edited and written back.
  ExtentList list(allocator_);
  BlockList nodes(allocator_);
  Status s = ext_load(*inode, &list, &nodes);
  if (s != Status::Ok) return s;
  const uint32_t capacity = ext_node_capacity();
  Status result = Status::Ok;
  bool changed = false;
  while (i < count) {
    const uint64_t position = first + i;
    uint64_t low = 0, high = list.count();  // the first extent that ends after `position`
    while (low < high) {
      const uint64_t mid = (low + high) / 2;
      if (list[mid].block + list[mid].length > position) high = mid;
      else low = mid + 1;
    }
    const uint64_t p = low;
    if (p < list.count() && list[p].block <= position) {
      const Extent extent = list[p];
      if (extent.unwritten) {
        result = Status::Unsupported;
        break;
      }
      const uint64_t available = extent.block + extent.length - position;
      const uint32_t run = static_cast<uint32_t>(available < count - i ? available : count - i);
      for (uint32_t k = 0; k < run; ++k) out[i + k] = Mapping{extent.start + (position - extent.block) + k, false};
      i += run;
      *done = i;
      continue;
    }
    const uint64_t limit = p < list.count() ? list[p].block : kNoLimit;
    const uint64_t want = limit - position < count - i ? limit - position : count - i;
    uint64_t goal = 0;
    if (i > 0 && out[i - 1].block != 0) goal = out[i - 1].block + 1;
    else if (p > 0) goal = list[p - 1].start + list[p - 1].length;
    // room for one more extent in the tree first
    uint32_t depth = 0;
    const uint64_t needed = ext_nodes_needed(list.count() + 1, capacity, &depth);
    while (nodes.count() < needed) {
      uint64_t node_block = 0;
      s = alloc_one(*inode, goal, &node_block);
      if (s != Status::Ok) {
        result = s;
        break;
      }
      if (!nodes.push_back(node_block)) {
        free_blocks(node_block, 1);
        result = Status::OutOfMemory;
        break;
      }
      inode_add_blocks(inode, 1);
      changed = true;
    }
    if (result != Status::Ok) break;
    uint32_t group = (inode->number - 1) / inodes_per_group_;
    if (goal >= first_data_block_ && goal < blocks_count_) group = static_cast<uint32_t>((goal - first_data_block_) / blocks_per_group_);
    else goal = 0;
    uint64_t start = 0, got = 0;
    s = alloc_blocks(group, goal, want, &start, &got);
    if (s != Status::Ok) {
      result = s;
      break;
    }
    inode_add_blocks(inode, static_cast<int64_t>(got));
    changed = true;
    uint64_t at = p;  // where the new blocks ended up in the list
    bool merged = false;
    if (p > 0) {
      Extent& before = list[p - 1];
      if (!before.unwritten && before.block + before.length == position && before.start + before.length == start && before.length + got <= kExtentMaxInit) {
        before.length += static_cast<uint32_t>(got);
        at = p - 1;
        merged = true;
      }
    }
    if (!merged) {
      Extent fresh;
      fresh.block = position;
      fresh.start = start;
      fresh.length = static_cast<uint32_t>(got);
      if (!list.insert(p, fresh)) {
        free_blocks(start, got);
        inode_add_blocks(inode, -static_cast<int64_t>(got));
        result = Status::OutOfMemory;
        break;
      }
    }
    if (at + 1 < list.count()) {  // joins the next extent if it follows on
      Extent& now = list[at];
      const Extent after = list[at + 1];
      if (!after.unwritten && now.block + now.length == after.block && now.start + now.length == after.start && now.length + after.length <= kExtentMaxInit) {
        now.length += after.length;
        list.remove_at(at + 1);
      }
    }
    for (uint64_t k = 0; k < got; ++k) out[i + k] = Mapping{start + k, true};
    i += static_cast<uint32_t>(got);
    *done = i;
  }
  if (changed) {
    const Status stored = ext_store(inode, list, &nodes);
    const Status freed = flush_frees();
    if (result == Status::Ok) result = stored != Status::Ok ? stored : freed;
  }
  return result;
}

// Gives back all blocks of the file from file block `keep` on (the extents are cut or dropped, and the tree is written again).
Status ExtFileSystem::ext_free_from(Inode* inode, uint64_t keep) {
  ExtentList list(allocator_);
  BlockList nodes(allocator_);
  Status s = ext_load(*inode, &list, &nodes);
  if (s != Status::Ok) return s;
  bool changed = false;
  while (list.count() > 0) {
    Extent& last = list[list.count() - 1];
    if (last.block >= keep) {
      for (uint64_t k = 0; k < last.length; ++k) {
        s = queue_free(inode, last.start + k);
        if (s != Status::Ok) return s;
      }
      list.remove_at(list.count() - 1);
      changed = true;
      continue;
    }
    if (last.block + last.length > keep) {
      const uint32_t kept = static_cast<uint32_t>(keep - last.block);
      for (uint64_t k = kept; k < last.length; ++k) {
        s = queue_free(inode, last.start + k);
        if (s != Status::Ok) return s;
      }
      last.length = kept;
      changed = true;
    }
    break;
  }
  if (!changed) return Status::Ok;
  return ext_store(inode, list, &nodes);
}

}  // namespace filesystem
