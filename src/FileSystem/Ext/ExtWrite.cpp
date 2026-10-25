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

// The writing half of ExtFileSystem: allocation, the block map, file data, directory changes and the namespace operations.
#include "FileSystem/Ext/ExtFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "Utility/Move.hpp"

namespace filesystem {

using namespace ext;

namespace {
constexpr uint32_t kTimeStamp = 315532800;  // 1980-01-01: there is no clock here, so new inodes get this
constexpr uint32_t kDefaultExtraIsize = 32;

// The rec_len stored for a record of `length` bytes (a 64 KiB block's single record is 65535).
uint32_t record_length_to_disk(uint32_t length) { return length >= 65536 ? 65535 : length; }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// Blocks and inodes

Status ExtFileSystem::load_block_bitmap(uint32_t group, ByteBuffer* bitmap) {
  bitmap->clear();
  if (!bitmap->put_zeros(block_size_)) return Status::OutOfMemory;
  return read_block(gd_block_bitmap(group), bitmap->writable_view());
}

Status ExtFileSystem::store_block_bitmap(uint32_t group, ByteBuffer& bitmap) { return write_block(gd_block_bitmap(group), bitmap.view()); }

// Takes a run of up to `want` free blocks (at least one), as near to `goal_block` as possible: its group first, then the others in order.
Status ExtFileSystem::alloc_blocks(uint32_t goal_group, uint64_t goal_block, uint64_t want, uint64_t* start, uint64_t* got) {
  if (goal_group >= group_count_) goal_group = 0;
  ByteBuffer bitmap(allocator_);
  for (uint32_t step = 0; step < group_count_; ++step) {
    const uint32_t group = (goal_group + step) % group_count_;
    if (gd_free_blocks(group) == 0) continue;
    Status s = load_block_bitmap(group, &bitmap);
    if (s != Status::Ok) return s;
    const uint32_t count = group_block_count(group);
    const uint64_t first = group_first_block(group);
    uint64_t from = 0;
    if (step == 0 && goal_block >= first && goal_block - first < count) from = goal_block - first;
    uint64_t index = bit_find_zero(bitmap.view(), from, count);
    if (index == count && from > 0) index = bit_find_zero(bitmap.view(), 0, from);
    if (index >= count) continue;  // (the descriptor promised a free block that the bitmap does not have)
    uint64_t length = 1;
    while (length < want && index + length < count && !bit_test(bitmap.view(), index + length)) ++length;
    for (uint64_t k = 0; k < length; ++k) bit_set(bitmap.writable_view(), index + k);
    s = store_block_bitmap(group, bitmap);
    if (s != Status::Ok) return s;
    gd_set(group, kGdFreeBlocks, kGdFreeBlocksHi, true, gd_free_blocks(group) - length);
    s = store_group(group);
    if (s != Status::Ok) return s;
    free_blocks_ -= length;
    *start = first + index;
    *got = length;
    return Status::Ok;
  }
  return Status::NoSpace;
}

Status ExtFileSystem::alloc_one(const Inode& inode, uint64_t goal, uint64_t* block) {
  uint32_t group = (inode.number - 1) / inodes_per_group_;
  if (goal >= first_data_block_ && goal < blocks_count_) group = static_cast<uint32_t>((goal - first_data_block_) / blocks_per_group_);
  else goal = 0;
  uint64_t got = 0;
  return alloc_blocks(group, goal, 1, block, &got);
}

Status ExtFileSystem::free_blocks(uint64_t start, uint64_t count) {
  if (count == 0) return Status::Ok;
  if (start < first_data_block_ || start >= blocks_count_ || count > blocks_count_ - start) return Status::Corrupt;
  ByteBuffer bitmap(allocator_);
  while (count > 0) {
    const uint32_t group = static_cast<uint32_t>((start - first_data_block_) / blocks_per_group_);
    const uint64_t index = start - group_first_block(group);
    const uint64_t here = count < group_block_count(group) - index ? count : group_block_count(group) - index;
    Status s = load_block_bitmap(group, &bitmap);
    if (s != Status::Ok) return s;
    for (uint64_t k = 0; k < here; ++k)
      if (!bit_test(bitmap.view(), index + k)) return Status::Corrupt;  // freeing a block that is not in use
    for (uint64_t k = 0; k < here; ++k) bit_clear(bitmap.writable_view(), index + k);
    s = store_block_bitmap(group, bitmap);
    if (s != Status::Ok) return s;
    gd_set(group, kGdFreeBlocks, kGdFreeBlocksHi, true, gd_free_blocks(group) + here);
    s = store_group(group);
    if (s != Status::Ok) return s;
    free_blocks_ += here;
    start += here;
    count -= here;
  }
  return Status::Ok;
}

// A block that is given back is remembered, and neighbours are returned together when the operation ends or the run breaks off.
Status ExtFileSystem::queue_free(Inode* inode, uint64_t block) {
  if (block == 0) return Status::Ok;
  const Status s = check_pointer(block);
  if (s != Status::Ok) return s;
  if (pending_count_ > 0 && block == pending_start_ + pending_count_) {
    ++pending_count_;
  } else {
    const Status f = flush_frees();
    if (f != Status::Ok) return f;
    pending_start_ = block;
    pending_count_ = 1;
  }
  inode_add_blocks(inode, -1);
  return Status::Ok;
}

Status ExtFileSystem::flush_frees() {
  if (pending_count_ == 0) return Status::Ok;
  const uint64_t start = pending_start_, count = pending_count_;
  pending_count_ = 0;
  return free_blocks(start, count);
}

Status ExtFileSystem::alloc_inode(bool directory, uint32_t parent, uint32_t* number) {
  const uint32_t parent_group = parent > 0 ? (parent - 1) / inodes_per_group_ : 0;
  ByteBuffer bitmap(allocator_);
  for (uint32_t step = 0; step < group_count_; ++step) {
    const uint32_t group = (parent_group + step) % group_count_;
    if (gd_free_inodes(group) == 0) continue;
    bitmap.clear();
    if (!bitmap.put_zeros(block_size_)) return Status::OutOfMemory;
    Status s = read_block(gd_inode_bitmap(group), bitmap.writable_view());
    if (s != Status::Ok) return s;
    const uint64_t from = group == 0 ? first_inode_ - 1 : 0;
    const uint64_t index = bit_find_zero(bitmap.view(), from, inodes_per_group_);
    if (index >= inodes_per_group_) {
      bitmap.clear();
      continue;
    }
    bit_set(bitmap.writable_view(), index);
    s = write_block(gd_inode_bitmap(group), bitmap.view());
    if (s != Status::Ok) return s;
    gd_set(group, kGdFreeInodes, kGdFreeInodesHi, true, gd_free_inodes(group) - 1);
    if (directory) gd_set(group, kGdUsedDirs, kGdUsedDirsHi, true, gd_used_dirs(group) + 1);
    s = store_group(group);
    if (s != Status::Ok) return s;
    --free_inodes_;
    *number = group * inodes_per_group_ + static_cast<uint32_t>(index) + 1;
    return Status::Ok;
  }
  return Status::NoSpace;
}

Status ExtFileSystem::free_inode(uint32_t number, bool directory) {
  if (number == 0 || number > inodes_count_) return Status::Corrupt;
  const uint32_t group = (number - 1) / inodes_per_group_;
  const uint32_t index = (number - 1) % inodes_per_group_;
  ByteBuffer bitmap(allocator_);
  if (!bitmap.put_zeros(block_size_)) return Status::OutOfMemory;
  Status s = read_block(gd_inode_bitmap(group), bitmap.writable_view());
  if (s != Status::Ok) return s;
  if (!bit_test(bitmap.view(), index)) return Status::Corrupt;
  bit_clear(bitmap.writable_view(), index);
  s = write_block(gd_inode_bitmap(group), bitmap.view());
  if (s != Status::Ok) return s;
  gd_set(group, kGdFreeInodes, kGdFreeInodesHi, true, gd_free_inodes(group) + 1);
  if (directory && gd_used_dirs(group) > 0) gd_set(group, kGdUsedDirs, kGdUsedDirsHi, true, gd_used_dirs(group) - 1);
  s = store_group(group);
  if (s != Status::Ok) return s;
  ++free_inodes_;
  return Status::Ok;
}

// An extended attribute block can be shared by several inodes: the last one to let go frees it.
Status ExtFileSystem::release_xattr_block(uint64_t block) {
  if (block == 0) return Status::Ok;
  if (check_pointer(block) != Status::Ok) return Status::Corrupt;
  ByteBuffer data(allocator_);
  if (!data.put_zeros(block_size_)) return Status::OutOfMemory;
  Status s = read_block(block, data.writable_view());
  if (s != Status::Ok) return s;
  if (rd32(data.view(), 0) != kXattrMagic) return Status::Corrupt;
  const uint32_t references = rd32(data.view(), 4);
  if (references > 1) {
    wr32(data.writable_view(), 4, references - 1);
    return write_block(block, data.view());
  }
  return free_blocks(block, 1);
}

Status ExtFileSystem::zero_block(uint64_t block) {
  ByteBuffer zeros(allocator_);
  if (!zeros.put_zeros(block_size_)) return Status::OutOfMemory;
  return write_block(block, zeros.view());
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The block map

// A new, empty indirect block.
Status ExtFileSystem::new_indirect(Inode* inode, uint64_t goal, uint64_t* block) {
  Status s = alloc_one(*inode, goal, block);
  if (s != Status::Ok) return s;
  s = zero_block(*block);
  if (s != Status::Ok) {
    free_blocks(*block, 1);
    return s;
  }
  inode_add_blocks(inode, 1);
  return Status::Ok;
}

// Block `index` of the file, allocated if it is a hole (with the indirect blocks on the way to it).
Status ExtFileSystem::bm_alloc(Inode* inode, uint64_t index, uint64_t goal, Mapping* result) {
  const uint64_t n = block_size_ / 4;
  uint32_t level = 0;
  uint64_t rest = index;
  if (index >= 12) {
    rest -= 12;
    if (rest < n) {
      level = 1;
    } else if ((rest -= n) < n * n) {
      level = 2;
    } else if ((rest -= n * n) < n * n * n) {
      level = 3;
    } else {
      return Status::NoSpace;
    }
  }
  if (level == 0) {
    const uint32_t pointer = inode->block_pointer(static_cast<uint32_t>(index));
    Status s = check_pointer(pointer);
    if (s != Status::Ok) return s;
    if (pointer != 0) {
      *result = Mapping{pointer, false};
      return Status::Ok;
    }
    uint64_t block = 0;
    s = alloc_one(*inode, goal, &block);
    if (s != Status::Ok) return s;
    inode->set_block_pointer(static_cast<uint32_t>(index), static_cast<uint32_t>(block));
    inode_add_blocks(inode, 1);
    *result = Mapping{block, true};
    return Status::Ok;
  }
  uint64_t node = inode->block_pointer(11 + level);
  Status s = check_pointer(node);
  if (s != Status::Ok) return s;
  if (node == 0) {
    s = new_indirect(inode, goal, &node);
    if (s != Status::Ok) return s;
    inode->set_block_pointer(11 + level, static_cast<uint32_t>(node));
  }
  uint64_t divisor = level == 3 ? n * n : level == 2 ? n : 1;
  for (;; divisor /= n) {
    const uint64_t slot = rest / divisor;
    rest %= divisor;
    uint32_t next = 0;
    s = read_word(node, slot, &next);
    if (s != Status::Ok) return s;
    s = check_pointer(next);
    if (s != Status::Ok) return s;
    if (divisor == 1) {  // the last level: the data block
      if (next != 0) {
        *result = Mapping{next, false};
        return Status::Ok;
      }
      uint64_t block = 0;
      s = alloc_one(*inode, goal, &block);
      if (s != Status::Ok) return s;
      s = write_word(node, slot, static_cast<uint32_t>(block));
      if (s != Status::Ok) {
        free_blocks(block, 1);
        return s;
      }
      inode_add_blocks(inode, 1);
      *result = Mapping{block, true};
      return Status::Ok;
    }
    if (next == 0) {
      uint64_t fresh = 0;
      s = new_indirect(inode, goal, &fresh);
      if (s != Status::Ok) return s;
      s = write_word(node, slot, static_cast<uint32_t>(fresh));
      if (s != Status::Ok) return s;
      next = static_cast<uint32_t>(fresh);
    }
    node = next;
  }
}

// Gives back the blocks of an indirect block's subtree that are at file block `keep` or later. *emptied: nothing is left in it.
Status ExtFileSystem::bm_free_node(Inode* inode, uint64_t block, uint32_t level, uint64_t base, uint64_t keep, bool* emptied) {
  const uint64_t n = block_size_ / 4;
  uint64_t span = 1;
  for (uint32_t k = 1; k < level; ++k) span *= n;
  *emptied = false;
  if (base + n * span <= keep) return Status::Ok;  // all of it stays
  ByteBuffer node(allocator_);
  if (!node.put_zeros(block_size_)) return Status::OutOfMemory;
  Status s = read_block(block, node.writable_view());
  if (s != Status::Ok) return s;
  bool modified = false, any_left = false;
  for (uint64_t i = 0; i < n; ++i) {
    const uint32_t pointer = rd32(node.view(), i * 4);
    if (pointer == 0) continue;
    s = check_pointer(pointer);
    if (s != Status::Ok) return s;
    const uint64_t child_base = base + i * span;
    if (child_base + span <= keep) {
      any_left = true;
      continue;
    }
    if (level == 1) {
      s = queue_free(inode, pointer);
      if (s != Status::Ok) return s;
      wr32(node.writable_view(), i * 4, 0);
      modified = true;
    } else {
      bool child_empty = false;
      s = bm_free_node(inode, pointer, level - 1, child_base, keep, &child_empty);
      if (s != Status::Ok) return s;
      if (child_empty) {
        s = queue_free(inode, pointer);
        if (s != Status::Ok) return s;
        wr32(node.writable_view(), i * 4, 0);
        modified = true;
      } else {
        any_left = true;
      }
    }
  }
  if (modified && any_left) {
    s = write_block(block, node.view());
    if (s != Status::Ok) return s;
  }
  *emptied = !any_left;
  return Status::Ok;
}

Status ExtFileSystem::bm_free_from(Inode* inode, uint64_t keep) {
  for (uint32_t i = 0; i < 12; ++i) {
    const uint32_t pointer = inode->block_pointer(i);
    if (pointer == 0 || i < keep) continue;
    const Status s = queue_free(inode, pointer);
    if (s != Status::Ok) return s;
    inode->set_block_pointer(i, 0);
  }
  const uint64_t n = block_size_ / 4;
  uint64_t base = 12, span = n;
  for (uint32_t level = 1; level <= 3; ++level, base += span, span *= n) {
    const uint32_t root = inode->block_pointer(11 + level);
    if (root == 0) continue;
    Status s = check_pointer(root);
    if (s != Status::Ok) return s;
    bool emptied = false;
    s = bm_free_node(inode, root, level, base, keep, &emptied);
    if (s != Status::Ok) return s;
    if (emptied) {
      s = queue_free(inode, root);
      if (s != Status::Ok) return s;
      inode->set_block_pointer(11 + level, 0);
    }
  }
  return Status::Ok;
}

Status ExtFileSystem::free_from(Inode* inode, uint64_t first_block) {
  if (is_fast_symlink(*inode)) return Status::Ok;
  Status s = bm_free_from(inode, first_block);
  const Status f = flush_frees();
  return s != Status::Ok ? s : f;
}

Status ExtFileSystem::map_range(Inode* inode, uint64_t first, uint32_t count, bool create, ArrayView<Mapping> out, uint32_t* done) {
  *done = 0;
  uint64_t previous = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t index = first + i;
    Mapping mapping;
    Status s;
    if (!create) {
      s = bm_get(*inode, index, &mapping.block);
    } else {
      uint64_t goal = previous ? previous + 1 : 0;
      if (previous == 0 && index > 0) {
        uint64_t before = 0;
        if (bm_get(*inode, index - 1, &before) == Status::Ok && before != 0) goal = before + 1;
      }
      s = bm_alloc(inode, index, goal, &mapping);
    }
    if (s != Status::Ok) return s;
    out[i] = mapping;
    previous = mapping.block;
    *done = i + 1;
  }
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// File data

// Once a file passes 2 GiB the volume must say so (the large_file feature), or e2fsck objects.
void ExtFileSystem::note_size(const Inode& inode) {
  if (inode.is_regular() && inode.size() >= 0x80000000ull && !(ro_compat_ & kRoLargeFile)) {
    ro_compat_ |= kRoLargeFile;
    wr32(sb_.writable_view(), kSbFeatureRoCompat, ro_compat_);
  }
}

Status ExtFileSystem::zero_tail(Inode* inode, uint64_t size) {
  const uint32_t within = static_cast<uint32_t>(size % block_size_);
  if (within == 0) return Status::Ok;
  uint64_t block = 0, run = 0;
  Status s = map_run(*inode, size / block_size_, 1, &block, &run);
  if (s != Status::Ok || block == 0) return s;
  ByteBuffer zeros(allocator_);
  if (!zeros.put_zeros(block_size_ - within)) return Status::OutOfMemory;
  return write_bytes(block * block_size_ + within, zeros.view());
}

Status ExtFileSystem::write_file_data(Inode* inode, uint64_t offset, ConstByteView data, uint64_t* transferred) {
  *transferred = 0;
  if (data.empty()) return Status::Ok;
  if (offset > UINT64_MAX - data.size()) return Status::NoSpace;
  Status s = check_size(offset + data.size());
  if (s != Status::Ok) return s;
  if (offset > inode->size()) {
    s = zero_tail(inode, inode->size());
    if (s != Status::Ok) return s;
  }
  ByteBuffer partial(allocator_);
  uint64_t position = offset;
  ConstByteView rest = data;
  while (!rest.empty()) {
    const uint64_t first = position / block_size_;
    const uint64_t last = (position + rest.size() - 1) / block_size_;
    const uint32_t count = static_cast<uint32_t>(last - first + 1 < kChunk ? last - first + 1 : kChunk);
    Mapping mappings[kChunk];
    uint32_t mapped = 0;
    const Status mapping_status = map_range(inode, first, count, true, ArrayView<Mapping>(mappings), &mapped);
    uint64_t consumed = 0;
    for (uint32_t i = 0; i < mapped;) {
      const uint32_t within = i == 0 ? static_cast<uint32_t>(position % block_size_) : 0;
      const uint64_t available = rest.size() - consumed;
      const uint64_t take = block_size_ - within < available ? block_size_ - within : available;
      const Mapping m = mappings[i];
      if (m.block == 0) return Status::Corrupt;
      if (within == 0 && take == block_size_) {  // a run of whole blocks that follow each other on the disk
        uint32_t j = i + 1;
        uint64_t bytes = block_size_;
        while (j < mapped && mappings[j].block == mappings[j - 1].block + 1 && available - bytes >= block_size_) {
          bytes += block_size_;
          ++j;
        }
        s = write_bytes(m.block * block_size_, rest.subview(consumed, bytes));
        if (s != Status::Ok) break;
        consumed += bytes;
        i = j;
      } else {
        partial.clear();
        if (!partial.put_zeros(block_size_)) {
          s = Status::OutOfMemory;
          break;
        }
        if (!m.fresh) {
          s = read_bytes(m.block * block_size_, partial.writable_view());
          if (s != Status::Ok) break;
        }
        partial.writable_view().subview(within, take).copy_from(rest.subview(consumed, take));
        s = write_bytes(m.block * block_size_, partial.view());
        if (s != Status::Ok) break;
        consumed += take;
        ++i;
      }
    }
    position += consumed;
    rest = rest.dropped(consumed);
    *transferred += consumed;
    if (position > inode->size()) {
      inode->set_size(position);
      note_size(*inode);
    }
    const Status stored = write_inode(*inode);  // (also when allocation failed part way: the blocks that were taken belong to the file)
    if (s != Status::Ok) return s;
    if (stored != Status::Ok) return stored;
    if (mapping_status != Status::Ok) return mapping_status;
  }
  return Status::Ok;
}

Status ExtFileSystem::resize_inode(Inode* inode, uint64_t new_size) {
  const uint64_t old_size = inode->size();
  if (new_size == old_size) return Status::Ok;
  Status s = check_size(new_size);
  if (s != Status::Ok) return s;
  if (new_size > old_size) {
    s = zero_tail(inode, old_size);
    if (s != Status::Ok) return s;
    inode->set_size(new_size);
    note_size(*inode);
    return write_inode(*inode);
  }
  s = free_from(inode, (new_size + block_size_ - 1) / block_size_);
  if (s == Status::Ok) s = zero_tail(inode, new_size);
  inode->set_size(new_size);
  const Status stored = write_inode(*inode);
  return s != Status::Ok ? s : stored;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Directory changes

uint8_t ExtFileSystem::entry_type(uint16_t mode) const {
  if (!(incompat_ & kIncompatFiletype)) return kFtUnknown;
  switch (mode & kModeTypeMask) {
    case kModeDirectory: return kFtDirectory;
    case kModeLink: return kFtSymlink;
    default: return kFtRegular;
  }
}

void ExtFileSystem::put_entry(ByteView block, uint32_t offset, uint32_t number, uint32_t record_length, StringView name, uint8_t type) {
  wr32(block, offset, number);
  wr16(block, offset + 4, record_length_to_disk(record_length));
  wr8(block, offset + 6, static_cast<uint32_t>(name.size()));
  wr8(block, offset + 7, type);
  for (uint64_t i = 0; i < name.size(); ++i) block[offset + kDirHeader + i] = static_cast<uint8_t>(name[i]);
}

// A directory's first block: "." and ".." and nothing else.
void ExtFileSystem::dir_init_block(ByteView block, uint32_t self, uint32_t parent) {
  block.fill(0);
  const uint8_t type = (incompat_ & kIncompatFiletype) ? kFtDirectory : kFtUnknown;
  put_entry(block, 0, self, 12, ".", type);
  put_entry(block, 12, parent, block_size_ - 12, "..", type);
}

// A directory made by ext3/ext4 may have a hash tree, which this code does not keep up to date. The tree is just a layer over the
// entries, which stay a valid linear list, so dropping the flag leaves a plain directory.
Status ExtFileSystem::dir_flatten(Inode* dir) {
  wr32(dir->bytes(), kInFlags, dir->flags() & ~kFlagIndex);
  return write_inode(*dir);
}

Status ExtFileSystem::dir_add(Inode* dir, StringView name, uint32_t number, uint8_t type) {
  if (dir->flags() & kFlagIndex) {
    const Status s = dir_flatten(dir);
    if (s != Status::Ok) return s;
  }
  const uint32_t need = align4(kDirHeader + static_cast<uint32_t>(name.size()));
  ByteBuffer block(allocator_);
  if (!block.put_zeros(block_size_)) return Status::OutOfMemory;
  const uint64_t blocks = (dir->size() + block_size_ - 1) / block_size_;
  for (uint64_t index = 0; index < blocks; ++index) {
    uint64_t disk = 0, run = 0;
    Status s = map_run(*dir, index, 1, &disk, &run);
    if (s != Status::Ok) return s;
    if (disk == 0) continue;
    s = read_block(disk, block.writable_view());
    if (s != Status::Ok) return s;
    for (uint32_t offset = 0; offset < block_size_;) {
      DirEntry found;
      s = dir_parse(block.view(), offset, &found);
      if (s != Status::Ok) return s;
      const uint32_t used = found.inode == 0 ? 0 : align4(kDirHeader + found.name_length);
      if (found.record_length - used >= need) {
        if (found.inode == 0) {
          put_entry(block.writable_view(), offset, number, found.record_length, name, type);
        } else {
          wr16(block.writable_view(), offset + 4, record_length_to_disk(used));
          put_entry(block.writable_view(), offset + used, number, found.record_length - used, name, type);
        }
        return write_block(disk, block.view());
      }
      offset += found.record_length;
    }
  }
  // No room: the directory grows by a block.
  Mapping mapping[1];
  uint32_t mapped = 0;
  const Status s = map_range(dir, blocks, 1, true, ArrayView<Mapping>(mapping), &mapped);
  if (s != Status::Ok) {
    write_inode(*dir);
    return s;
  }
  block.writable_view().fill(0);
  put_entry(block.writable_view(), 0, number, block_size_, name, type);
  Status w = write_block(mapping[0].block, block.view());
  if (w != Status::Ok) return w;
  dir->set_size((blocks + 1) * block_size_);
  return write_inode(*dir);
}

Status ExtFileSystem::dir_remove(const Inode& dir, StringView name) {
  ByteBuffer block(allocator_);
  if (!block.put_zeros(block_size_)) return Status::OutOfMemory;
  const uint64_t blocks = (dir.size() + block_size_ - 1) / block_size_;
  for (uint64_t index = 0; index < blocks; ++index) {
    uint64_t disk = 0, run = 0;
    Status s = map_run(dir, index, 1, &disk, &run);
    if (s != Status::Ok) return s;
    if (disk == 0) continue;
    s = read_block(disk, block.writable_view());
    if (s != Status::Ok) return s;
    bool have_previous = false;
    DirEntry previous;
    for (uint32_t offset = 0; offset < block_size_;) {
      DirEntry found;
      s = dir_parse(block.view(), offset, &found);
      if (s != Status::Ok) return s;
      const StringView text(reinterpret_cast<const char*>(block.view().subview(found.offset + kDirHeader, found.name_length).data()), found.name_length);
      if (found.inode != 0 && text.equals(name)) {
        if (!have_previous) {
          wr32(block.writable_view(), found.offset, 0);  // the first entry of a block stays as an empty record
        } else {
          wr16(block.writable_view(), previous.offset + 4, record_length_to_disk(previous.record_length + found.record_length));
        }
        return write_block(disk, block.view());
      }
      previous = found;
      have_previous = true;
      offset += found.record_length;
    }
  }
  return Status::NotFound;
}

// Points a directory's ".." at another parent.
Status ExtFileSystem::dir_set_parent(const Inode& dir, uint32_t parent) {
  uint64_t index = 0;
  DirEntry entry;
  Status s = dir_find(dir, "..", &index, &entry);
  if (s != Status::Ok) return s;
  uint64_t disk = 0, run = 0;
  s = map_run(dir, index, 1, &disk, &run);
  if (s != Status::Ok) return s;
  if (disk == 0) return Status::Corrupt;
  return write_word(disk, entry.offset / 4, parent);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The namespace

Status ExtFileSystem::init_inode(Inode* inode, uint16_t mode, uint32_t links) {
  inode->length = inode_size_;
  inode->bytes().fill(0);
  ByteView v = inode->bytes();
  wr16(v, kInMode, mode);
  wr16(v, kInLinks, links);
  wr32(v, kInAtime, kTimeStamp);
  wr32(v, kInCtime, kTimeStamp);
  wr32(v, kInMtime, kTimeStamp);
  if (inode_size_ > kOldInodeSize) {
    uint32_t extra = sb16(kSbWantExtraIsize);
    if (extra == 0) extra = kDefaultExtraIsize;
    if (extra > inode_size_ - kOldInodeSize) extra = inode_size_ - kOldInodeSize;
    extra &= ~3u;
    wr16(v, kInExtraIsize, extra);
    if (extra >= 24) wr32(v, 0x90, kTimeStamp);  // i_crtime
  }
  return Status::Ok;
}

Status ExtFileSystem::create_node(StringView path, bool directory, uint32_t* number) {
  Status s = begin_write();
  if (s != Status::Ok) return s;
  Resolved parent;
  StringView leaf;
  s = split(path, &parent, &leaf);
  if (s != Status::Ok) return s;
  if (!valid_name(leaf)) return Status::InvalidPath;
  Inode dir;
  s = read_inode(parent.number, &dir);
  if (s != Status::Ok) return s;
  uint32_t existing = 0;
  s = dir_lookup(dir, leaf, &existing, nullptr);
  if (s == Status::Ok) return Status::AlreadyExists;
  if (s != Status::NotFound) return s;
  if (directory && dir.links() >= 65000) return Status::NoSpace;

  uint32_t ino = 0;
  s = alloc_inode(directory, parent.number, &ino);
  if (s != Status::Ok) return s;
  Inode node;
  node.number = ino;
  init_inode(&node, directory ? 040755 : 0100644, directory ? 2 : 1);
  if (directory) {
    Mapping mapping[1];
    uint32_t mapped = 0;
    s = map_range(&node, 0, 1, true, ArrayView<Mapping>(mapping), &mapped);
    if (s == Status::Ok) {
      ByteBuffer block(allocator_);
      if (!block.put_zeros(block_size_)) {
        s = Status::OutOfMemory;
      } else {
        dir_init_block(block.writable_view(), ino, parent.number);
        s = write_block(mapping[0].block, block.view());
        node.set_size(block_size_);
      }
    }
  }
  if (s == Status::Ok) s = write_inode(node);
  if (s == Status::Ok) s = dir_add(&dir, leaf, ino, entry_type(node.mode()));
  if (s != Status::Ok) {
    delete_inode(&node);
    return s;
  }
  if (directory) {
    wr16(dir.bytes(), kInLinks, dir.links() + 1);
    s = write_inode(dir);
    if (s != Status::Ok) return s;
  }
  *number = ino;
  return Status::Ok;
}

// Frees an inode and everything it owns.
Status ExtFileSystem::delete_inode(Inode* inode) {
  const bool directory = inode->is_directory();
  Status s = free_from(inode, 0);
  if (s != Status::Ok) return s;
  s = release_xattr_block(rd32(inode->bytes(), kInFileAcl) | (uint64_t{rd16(inode->bytes(), kInFileAclHi)} << 32));
  if (s != Status::Ok) return s;
  inode->bytes().fill(0);
  s = write_inode(*inode);
  if (s != Status::Ok) return s;
  return free_inode(inode->number, directory);
}

Status ExtFileSystem::create_directory(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  if (!writable_) return Status::ReadOnly;
  uint32_t number = 0;
  return create_node(path, true, &number);
}

Status ExtFileSystem::remove(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  if (!writable_) return Status::ReadOnly;
  Resolved r;
  Status s = resolve(path, &r);
  if (s != Status::Ok) return s;
  if (r.number == kRootInode) return Status::InvalidPath;
  if (is_open(r.number)) return Status::Busy;
  s = begin_write();
  if (s != Status::Ok) return s;
  Inode node;
  s = read_inode(r.number, &node);
  if (s != Status::Ok) return s;
  if (inode_immutable(node)) return Status::ReadOnly;
  Resolved parent;
  StringView leaf;
  s = split(path, &parent, &leaf);
  if (s != Status::Ok) return s;
  Inode dir;
  s = read_inode(parent.number, &dir);
  if (s != Status::Ok) return s;
  if (r.directory) {
    bool empty = false;
    s = dir_empty(node, &empty);
    if (s != Status::Ok) return s;
    if (!empty) return Status::NotEmpty;
  }
  s = dir_remove(dir, leaf);
  if (s != Status::Ok) return s;
  if (r.directory) {
    if (dir.links() > 2) wr16(dir.bytes(), kInLinks, dir.links() - 1);
    s = write_inode(dir);
    if (s != Status::Ok) return s;
    return delete_inode(&node);
  }
  if (node.links() > 1) {
    wr16(node.bytes(), kInLinks, node.links() - 1);
    return write_inode(node);
  }
  return delete_inode(&node);
}

Status ExtFileSystem::rename(StringView from, StringView to) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  if (!writable_) return Status::ReadOnly;
  Resolved source;
  Status s = resolve(from, &source);
  if (s != Status::Ok) return s;
  if (source.number == kRootInode) return Status::InvalidPath;
  if (is_open(source.number)) return Status::Busy;
  Resolved source_parent;
  StringView source_leaf;
  s = split(from, &source_parent, &source_leaf);
  if (s != Status::Ok) return s;
  Resolved target_parent;
  StringView target_leaf;
  s = split(to, &target_parent, &target_leaf);
  if (s != Status::Ok) return s;
  if (!valid_name(target_leaf)) return Status::InvalidPath;
  s = begin_write();
  if (s != Status::Ok) return s;
  Inode node;
  s = read_inode(source.number, &node);
  if (s != Status::Ok) return s;
  if (inode_immutable(node)) return Status::ReadOnly;
  Inode target_dir;
  s = read_inode(target_parent.number, &target_dir);
  if (s != Status::Ok) return s;
  uint32_t existing = 0;
  s = dir_lookup(target_dir, target_leaf, &existing, nullptr);
  if (s == Status::Ok) return Status::AlreadyExists;
  if (s != Status::NotFound) return s;
  if (source.directory) {  // not into itself or below
    uint32_t walk = target_parent.number;
    for (uint32_t depth = 0; walk != kRootInode; ++depth) {
      if (walk == source.number || depth > 4096) return Status::InvalidArgument;
      Inode up;
      s = read_inode(walk, &up);
      if (s != Status::Ok) return s;
      s = dir_parent(up, &walk);
      if (s != Status::Ok) return s;
    }
  }
  s = dir_add(&target_dir, target_leaf, source.number, entry_type(node.mode()));
  if (s != Status::Ok) return s;
  Inode old_dir;
  s = read_inode(source_parent.number, &old_dir);  // (read again: it is the same inode as target_dir when they share a directory)
  if (s != Status::Ok) return s;
  s = dir_remove(old_dir, source_leaf);
  if (s != Status::Ok) return s;
  if (source.directory && source_parent.number != target_parent.number) {
    s = dir_set_parent(node, target_parent.number);
    if (s != Status::Ok) return s;
    if (old_dir.links() > 2) wr16(old_dir.bytes(), kInLinks, old_dir.links() - 1);
    s = write_inode(old_dir);
    if (s != Status::Ok) return s;
    s = read_inode(target_parent.number, &target_dir);
    if (s != Status::Ok) return s;
    wr16(target_dir.bytes(), kInLinks, target_dir.links() + 1);
    s = write_inode(target_dir);
    if (s != Status::Ok) return s;
  }
  return Status::Ok;
}

}  // namespace filesystem
