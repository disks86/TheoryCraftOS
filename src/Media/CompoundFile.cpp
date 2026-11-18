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

#include "Media/CompoundFile.hpp"
#include "Utility/ByteStream.hpp"

namespace media {

namespace {

constexpr uint32_t kHeaderBytes = 512, kEntryBytes = 128, kMiniShift = 6;
constexpr uint32_t kFreeSector = 0xFFFFFFFFu, kEndOfChain = 0xFFFFFFFEu, kNoEntry = 0xFFFFFFFFu;
constexpr uint8_t kTypeStream = 2, kTypeRoot = 5;
constexpr uint8_t kSignature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

}  // namespace

bool CompoundFile::read_sector(uint32_t sector, uint64_t offset, ByteView out) {
  const uint64_t sector_bytes = uint64_t{1} << sector_shift_;
  if (sector >= sector_count_ || offset > sector_bytes || out.size() > sector_bytes - offset) return false;
  return source_.read(((uint64_t{sector} + 1) << sector_shift_) + offset, out);
}

bool CompoundFile::next_in(const ByteBuffer& table, uint32_t sector, uint32_t* next) const {
  ConstByteView entry;
  if (!table.view().try_subview(uint64_t{sector} * 4, 4, &entry)) return false;
  *next = ByteReader(entry).le32();
  return true;
}

// The first `sectors` sectors of the chain that starts at `first`.
bool CompoundFile::build_chain(const ByteBuffer& table, uint32_t first, uint64_t sectors, List<uint32_t>* chain) const {
  chain->clear();
  if (sectors > table.size() / 4 || !chain->reserve(sectors)) return false;      // (a chain longer than its table has a loop in it)
  uint32_t at = first;
  for (uint64_t i = 0; i < sectors; ++i) {
    uint32_t next = 0;
    if (!next_in(table, at, &next) || !chain->push_back(at)) return false;
    at = next;
  }
  return true;
}

// The sectors of a chain of the file allocation table, one after the other: a table that is itself kept in a chain (the mini table, the directory).
bool CompoundFile::load_table(uint32_t first, uint64_t count, ByteBuffer* out) {
  List<uint32_t> chain(allocator_);
  const uint64_t sector_bytes = uint64_t{1} << sector_shift_;
  if (count > kMaxTableSectors || !build_chain(fat_, first, count, &chain) || !out->put_zeros(count * sector_bytes)) return false;
  uint64_t at = 0;
  for (const uint32_t sector : static_cast<const List<uint32_t>&>(chain).view()) {
    if (!read_sector(sector, 0, out->writable_view().subview(at, sector_bytes))) return false;
    at += sector_bytes;
  }
  return true;
}

bool CompoundFile::open() {
  entries_.clear();
  fat_.clear();
  mini_fat_.clear();
  uint8_t raw[kHeaderBytes];
  if (source_.size() < kHeaderBytes || !source_.read(0, ByteView(raw))) return false;
  ByteReader in{ConstByteView(raw)};
  for (const uint8_t byte : kSignature)
    if (in.u8() != byte) return false;
  in.skip(16 + 2);      // class id, minor version
  const uint16_t major = in.le16(), byte_order = in.le16(), sector_shift = in.le16(), mini_shift = in.le16();
  in.skip(6 + 4);       // reserved, directory sectors (version 4 only)
  const uint32_t fat_sectors = in.le32(), first_directory = in.le32();
  in.skip(4);           // transaction signature
  const uint32_t mini_cutoff = in.le32(), first_mini_fat = in.le32(), mini_fat_sectors = in.le32();
  uint32_t difat_sector = in.le32();
  const uint32_t difat_sectors = in.le32();
  if (!in.ok() || byte_order != 0xFFFE || mini_shift != kMiniShift || !((major == 3 && sector_shift == 9) || (major == 4 && sector_shift == 12))) return false;
  sector_shift_ = sector_shift;
  mini_cutoff_ = mini_cutoff;
  const uint64_t sector_bytes = uint64_t{1} << sector_shift_, per_sector = sector_bytes / 4;
  if (source_.size() < sector_bytes) return false;
  sector_count_ = (source_.size() - sector_bytes + sector_bytes - 1) >> sector_shift_;      // (a file may end inside its last sector)
  if (fat_sectors == 0 || fat_sectors > sector_count_ || fat_sectors > kMaxTableSectors / per_sector || mini_fat_sectors > sector_count_ || difat_sectors > sector_count_) return false;

  // The allocation table: its sectors are listed in the header (the first 109) and in the chain of DIFAT sectors.
  if (!fat_.put_zeros(uint64_t{fat_sectors} * sector_bytes)) return false;
  uint32_t loaded = 0;
  auto add_fat_sector = [&](uint32_t sector) {
    if (sector == kFreeSector || loaded == fat_sectors) return true;
    if (!read_sector(sector, 0, fat_.writable_view().subview(uint64_t{loaded} * sector_bytes, sector_bytes))) return false;
    ++loaded;
    return true;
  };
  for (uint32_t i = 0; i < 109; ++i)
    if (!add_fat_sector(in.le32())) return false;
  ByteBuffer block(allocator_);
  if (!block.put_zeros(sector_bytes)) return false;
  for (uint32_t i = 0; i < difat_sectors && loaded < fat_sectors && difat_sector != kEndOfChain && difat_sector != kFreeSector; ++i) {
    if (!read_sector(difat_sector, 0, block.writable_view())) return false;
    ByteReader list(block.view());
    for (uint64_t k = 0; k + 1 < per_sector; ++k)
      if (!add_fat_sector(list.le32())) return false;
    difat_sector = list.le32();
  }
  if (loaded != fat_sectors) return false;
  if (mini_fat_sectors != 0 && !load_table(first_mini_fat, mini_fat_sectors, &mini_fat_)) return false;

  // The directory: as many sectors as its chain has (the header of version 3 does not say), each holding entries of 128 bytes.
  ByteBuffer directory(allocator_);
  {
    uint64_t sectors = 0;
    for (uint32_t at = first_directory; at != kEndOfChain; ++sectors) {
      if (sectors >= sector_count_ || (sectors + 1) * sector_bytes > uint64_t{kMaxEntries} * kEntryBytes || !next_in(fat_, at, &at)) return false;
    }
    if (sectors == 0 || !load_table(first_directory, sectors, &directory)) return false;
  }
  const uint64_t count = directory.size() / kEntryBytes;
  struct Node {
    uint32_t left, right, child, first_sector;
    uint64_t size;
    uint8_t type;
  };
  auto node_at = [&](uint64_t index, Node* node, CompoundEntry* entry) {
    ConstByteView bytes;
    if (!directory.view().try_subview(index * kEntryBytes, kEntryBytes, &bytes)) return false;
    ByteReader e(bytes);
    uint16_t name[32];
    for (uint16_t& unit : name) unit = e.le16();
    const uint16_t name_bytes = e.le16();
    node->type = e.u8();
    e.skip(1);
    node->left = e.le32(), node->right = e.le32(), node->child = e.le32();
    e.skip(16 + 4 + 8 + 8);
    node->first_sector = e.le32();
    node->size = e.le64();
    if (major == 3) node->size &= 0xFFFFFFFFu;      // (the high half is not kept by version 3, and may hold anything)
    if (entry) {
      if (name_bytes < 2 || name_bytes > 64 || (name_bytes & 1)) return false;
      entry->name_units = name_bytes / 2 - 1;
      ArrayView<uint16_t>(entry->name).copy_from(ArrayView<const uint16_t>(name));
      entry->size = node->size;
      entry->first_sector = node->first_sector;
    }
    return e.ok();
  };
  Node root{};
  if (!node_at(0, &root, nullptr) || root.type != kTypeRoot) return false;

  // The mini stream is the root's own.
  root_.file_ = this;
  root_.mini_ = false;
  root_.size_ = root.size;
  if (!build_chain(fat_, root.first_sector, (root.size + sector_bytes - 1) >> sector_shift_, &root_.chain_)) return false;

  // The root's children: a binary tree of siblings. Each entry is visited once, whatever the tree's links say.
  List<uint32_t> pending(allocator_);
  ByteBuffer seen(allocator_);
  if (!seen.put_zeros(count)) return false;
  if (root.child != kNoEntry && !pending.push_back(root.child)) return false;
  while (pending.count() > 0) {
    const uint32_t index = pending[pending.count() - 1];
    pending.remove_at(pending.count() - 1);
    if (index >= count || seen.view()[index] != 0) return false;
    seen.writable_view()[index] = 1;
    Node node{};
    CompoundEntry entry;
    if (!node_at(index, &node, &entry)) return false;
    if (node.type == kTypeStream && !entries_.push_back(entry)) return false;
    if (node.left != kNoEntry && !pending.push_back(node.left)) return false;
    if (node.right != kNoEntry && !pending.push_back(node.right)) return false;
  }
  return true;
}

const CompoundEntry* CompoundFile::find(ArrayView<const uint16_t> name) const {
  for (const CompoundEntry& entry : entries_.view()) {
    if (entry.name_units != name.size()) continue;
    bool same = true;
    for (uint64_t i = 0; i < name.size(); ++i) same &= entry.name[i] == name[i];
    if (same) return &entry;
  }
  return nullptr;
}

bool CompoundFile::open_stream(const CompoundEntry& entry, CompoundStream* stream) {
  stream->file_ = this;
  stream->size_ = entry.size;
  stream->mini_ = entry.size < mini_cutoff_;
  const uint32_t shift = stream->mini_ ? kMiniShift : sector_shift_;
  const uint64_t sectors = (entry.size >> shift) + ((entry.size & ((uint64_t{1} << shift) - 1)) != 0 ? 1 : 0);
  return build_chain(stream->mini_ ? mini_fat_ : fat_, entry.first_sector, sectors, &stream->chain_);
}

bool CompoundFile::read_stream(const CompoundEntry& entry, uint64_t max_size, ByteBuffer* out) {
  if (entry.size > max_size) return false;
  CompoundStream stream(allocator_);
  const uint64_t before = out->size();
  if (!open_stream(entry, &stream) || !out->put_zeros(entry.size)) return false;
  if (!stream.read(0, out->writable_view().subview(before))) {
    out->truncate(before);
    return false;
  }
  return true;
}

bool CompoundStream::read(uint64_t offset, ByteView out) {
  if (!file_ || offset > size_ || out.size() > size_ - offset) return false;
  const uint32_t shift = mini_ ? kMiniShift : file_->sector_shift_;
  const uint64_t unit = uint64_t{1} << shift;
  uint64_t done = 0;
  while (done < out.size()) {
    const uint64_t at = offset + done, within = at & (unit - 1);
    const uint64_t piece = out.size() - done < unit - within ? out.size() - done : unit - within;
    const uint32_t* sector = chain_.get(at >> shift);
    if (!sector) return false;
    const ByteView into = out.subview(done, piece);
    if (mini_ ? !file_->root_.read((uint64_t{*sector} << kMiniShift) + within, into) : !file_->read_sector(*sector, within, into)) return false;
    done += piece;
  }
  return true;
}

}  // namespace media
