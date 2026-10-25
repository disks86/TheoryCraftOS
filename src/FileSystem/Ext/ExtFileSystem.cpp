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

// Mounting, reading and the namespace of ExtFileSystem; the writing half is in ExtWrite.cpp.
#include "FileSystem/Ext/ExtFileSystem.hpp"
#include "FileSystem/Ext/ExtChecksum.hpp"
#include "FileSystem/Path.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/Move.hpp"

namespace filesystem {

using namespace ext;
using util::move;

namespace {
constexpr uint64_t kMaxGroupTable = uint64_t{1} << 26;  // a group descriptor table bigger than this (64 MiB) is not read
constexpr uint32_t kMaxGroups = 1u << 22;

// What this code reads, and what it also writes (anything else in the superblock makes the volume read only or unusable).
constexpr uint32_t kReadableIncompat = kIncompatFiletype | kIncompatRecover | kIncompatFlexBg | kIncompatExtents | kIncompat64Bit | kIncompatMmp |
                                       kIncompatCsumSeed | kIncompatLargeDir;
constexpr uint32_t kWritableIncompat = kIncompatFiletype | kIncompatFlexBg | kIncompatExtents | kIncompat64Bit | kIncompatCsumSeed | kIncompatLargeDir;
constexpr uint32_t kWritableCompat = 0x1 /*dir_prealloc*/ | kCompatHasJournal | kCompatExtAttr | kCompatResizeInode | kCompatDirIndex | kCompatSparseSuper2 |
                                     kCompatStableInodes | kCompatOrphanFile;
constexpr uint32_t kWritableRoCompat = kRoSparseSuper | kRoLargeFile | kRoBtreeDir | kRoHugeFile | kRoGdtCsum | kRoDirNlink | kRoExtraIsize | kRoMetadataCsum;

bool power_of_two(uint64_t x) { return x != 0 && (x & (x - 1)) == 0; }
}  // namespace

// An open file or directory: registered with the filesystem, which refuses to remove or rename what is open.
class ExtFileSystem::File final : public IFile {
 public:
  File(ExtFileSystem& fs, uint32_t number, bool writable) : fs_(fs), number_(number), writable_(writable) {
    handle_.number = number;
    handle_.next = fs_.open_;
    fs_.open_ = &handle_;
  }
  ~File() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    for (OpenHandle** link = &fs_.open_; *link; link = &(*link)->next) {
      if (*link == &handle_) {
        *link = handle_.next;
        break;
      }
    }
  }

  uint64_t size() const override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    Inode inode;
    return fs_.read_inode(number_, &inode) == Status::Ok ? inode.size() : 0;
  }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    Inode inode;
    const Status s = fs_.read_inode(number_, &inode);
    if (s != Status::Ok) return s;
    return fs_.read_data(inode, offset, buffer, transferred);
  }
  Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!writable_) return Status::ReadOnly;
    Status s = fs_.begin_write();
    if (s != Status::Ok) return s;
    Inode inode;
    s = fs_.read_inode(number_, &inode);
    if (s != Status::Ok) return s;
    if (!inode.is_regular()) return Status::Unsupported;
    return fs_.write_file_data(&inode, offset, data, transferred);
  }
  Status resize(uint64_t new_size) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::ReadOnly;
    Status s = fs_.begin_write();
    if (s != Status::Ok) return s;
    Inode inode;
    s = fs_.read_inode(number_, &inode);
    if (s != Status::Ok) return s;
    if (!inode.is_regular()) return Status::Unsupported;
    return fs_.resize_inode(&inode, new_size);
  }
  Status flush() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    return fs_.flush();
  }

 private:
  ExtFileSystem& fs_;
  const uint32_t number_;
  const bool writable_;
  OpenHandle handle_;
};

class ExtFileSystem::Directory final : public IDirectory {
 public:
  Directory(ExtFileSystem& fs, uint32_t number) : fs_(fs), number_(number) {
    handle_.number = number;
    handle_.next = fs_.open_;
    fs_.open_ = &handle_;
  }
  ~Directory() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    for (OpenHandle** link = &fs_.open_; *link; link = &(*link)->next) {
      if (*link == &handle_) {
        *link = handle_.next;
        break;
      }
    }
  }
  Status next(DirectoryEntry* entry) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    Inode dir;
    Status s = fs_.read_inode(number_, &dir);
    if (s != Status::Ok) return s;
    for (;;) {
      DirEntry found;
      FixedString<kMaxNameLength + 1> name;
      s = fs_.dir_next(dir, &cursor_, &found, &name);
      if (s != Status::Ok) return s;
      Inode inode;
      s = fs_.read_inode(found.inode, &inode);
      if (s != Status::Ok) return s;
      s = fs_.describe(inode, name.view(), entry);
      if (s == Status::NotFound) continue;  // a device, pipe or socket
      return s;
    }
  }
  void rewind() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    cursor_ = Cursor();
  }

 private:
  ExtFileSystem& fs_;
  const uint32_t number_;
  OpenHandle handle_;
  Cursor cursor_;
};

Status ExtFileSystem::mount(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block,
                            UniquePointer<IFileSystem>* result) {
  UniquePointer<IFileSystem> fs = UniquePointer<IFileSystem>::make_as<ExtFileSystem>(allocator, allocator, device, first_block);
  if (!fs) return Status::OutOfMemory;
  const Status s = static_cast<ExtFileSystem*>(fs.get())->init();
  if (s != Status::Ok) return s;
  *result = move(fs);
  return Status::Ok;
}

ExtFileSystem::ExtFileSystem(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block)
    : allocator_(allocator), device_(device), first_block_(first_block), sb_(allocator), gd_(allocator) {}

ExtFileSystem::~ExtFileSystem() {
  if (dirty_set_) finish_write();
  release_view(allocator_, block_);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Device access

Status ExtFileSystem::read_bytes(uint64_t offset, ByteView out) {
  if (offset > UINT64_MAX - first_block_ * device_block_size_) return Status::Corrupt;
  uint64_t absolute = first_block_ * device_block_size_ + offset;
  uint64_t done = 0;
  while (done < out.size()) {
    const uint64_t block = absolute / device_block_size_;
    const uint32_t within = static_cast<uint32_t>(absolute % device_block_size_);
    const uint64_t left = out.size() - done;
    if (within == 0 && left >= device_block_size_) {  // whole blocks straight into the caller's buffer
      const uint64_t bytes = left / device_block_size_ * device_block_size_;
      if (!device_.read(block, out.subview(done, bytes))) return Status::IoError;
      done += bytes;
      absolute += bytes;
      continue;
    }
    if (block_number_ != block) {
      block_number_ = ~uint64_t{0};
      if (!device_.read(block, block_)) return Status::IoError;
      block_number_ = block;
    }
    uint64_t take = device_block_size_ - within;
    if (take > left) take = left;
    out.subview(done, take).copy_from(ConstByteView(block_).subview(within, take));
    done += take;
    absolute += take;
  }
  return Status::Ok;
}

Status ExtFileSystem::write_bytes(uint64_t offset, ConstByteView data) {
  if (!writable_) return Status::ReadOnly;
  if (offset > UINT64_MAX - first_block_ * device_block_size_) return Status::Corrupt;
  uint64_t absolute = first_block_ * device_block_size_ + offset;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t block = absolute / device_block_size_;
    const uint32_t within = static_cast<uint32_t>(absolute % device_block_size_);
    const uint64_t left = data.size() - done;
    if (within == 0 && left >= device_block_size_) {  // whole blocks straight from the caller's buffer
      const uint64_t bytes = left / device_block_size_ * device_block_size_;
      if (block_number_ != ~uint64_t{0} && block_number_ >= block && block_number_ < block + bytes / device_block_size_) block_number_ = ~uint64_t{0};
      if (!device_.write(block, data.subview(done, bytes))) return Status::IoError;
      done += bytes;
      absolute += bytes;
      continue;
    }
    if (block_number_ != block) {
      block_number_ = ~uint64_t{0};
      if (!device_.read(block, block_)) return Status::IoError;
      block_number_ = block;
    }
    uint64_t take = device_block_size_ - within;
    if (take > left) take = left;
    block_.subview(within, take).copy_from(data.subview(done, take));
    if (!device_.write(block, ConstByteView(block_))) {
      block_number_ = ~uint64_t{0};
      return Status::IoError;
    }
    done += take;
    absolute += take;
  }
  return Status::Ok;
}

Status ExtFileSystem::read_block(uint64_t block, ByteView out) {
  if (block >= blocks_count_ || out.size() != block_size_) return Status::Corrupt;
  return read_bytes(block * block_size_, out);
}

Status ExtFileSystem::write_block(uint64_t block, ConstByteView data) {
  if (block >= blocks_count_ || data.size() != block_size_) return Status::Corrupt;
  return write_bytes(block * block_size_, data);
}

Status ExtFileSystem::read_word(uint64_t block, uint64_t index, uint32_t* value) {
  if (block >= blocks_count_ || index >= block_size_ / 4) return Status::Corrupt;
  uint8_t word[4];
  const Status s = read_bytes(block * block_size_ + index * 4, ByteView(word));
  if (s == Status::Ok) *value = le32(word);
  return s;
}

Status ExtFileSystem::write_word(uint64_t block, uint64_t index, uint32_t value) {
  if (block >= blocks_count_ || index >= block_size_ / 4) return Status::Corrupt;
  uint8_t word[4];
  wr32(ByteView(word), 0, value);
  return write_bytes(block * block_size_ + index * 4, ConstByteView(word));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Mounting

Status ExtFileSystem::init() {
  const hardware::StorageInfo storage = device_.info();
  device_block_size_ = storage.block_size;
  if (!power_of_two(device_block_size_) || first_block_ >= storage.block_count) return Status::InvalidArgument;
  block_ = allocate_view<uint8_t>(allocator_, device_block_size_);
  if (block_.empty()) return Status::OutOfMemory;

  Status s = load_volume();
  if (s != Status::Ok) return s;
  // A journal that still holds committed changes is replayed first (this needs a device that can be written).
  if ((incompat_ & kIncompatRecover) && !storage.read_only) {
    s = recover();
    if (s != Status::Ok && s != Status::Unsupported) return s;  // (Unsupported: a journal that cannot be replayed here: the volume stays read only)
  }

  // The root must be a directory.
  Inode root;
  s = read_inode(kRootInode, &root);
  if (s != Status::Ok) return s;
  if (!root.is_directory() || root.links() == 0) return Status::Corrupt;
  return Status::Ok;
}

// Reads and checks the superblock and the group descriptors (again, after a journal replay changed them).
Status ExtFileSystem::load_volume() {
  const hardware::StorageInfo storage = device_.info();
  sb_.clear();
  gd_.clear();
  free_blocks_ = free_inodes_ = 0;
  label_.clear();
  compat_ = incompat_ = ro_compat_ = 0;
  block_number_ = ~uint64_t{0};

  if (!sb_.put_zeros(kSuperblockSize)) return Status::OutOfMemory;
  Status s = read_bytes(kSuperblockOffset, sb_.writable_view());
  if (s != Status::Ok) return s;
  if (sb16(kSbMagic) != kMagic) return Status::Corrupt;

  const uint32_t log_block_size = sb32(kSbLogBlockSize);
  if (log_block_size > 6) return Status::Corrupt;
  block_size_ = 1024u << log_block_size;
  const uint32_t revision = sb32(kSbRevLevel);
  if (revision > 1) return Status::Unsupported;
  if (revision == 0) {
    first_inode_ = kOldFirstInode;
    inode_size_ = kOldInodeSize;
  } else {
    first_inode_ = sb32(kSbFirstIno);
    inode_size_ = sb16(kSbInodeSize);
    compat_ = sb32(kSbFeatureCompat);
    incompat_ = sb32(kSbFeatureIncompat);
    ro_compat_ = sb32(kSbFeatureRoCompat);
  }
  if (incompat_ & ~kReadableIncompat) return Status::Unsupported;
  if (ro_compat_ & kRoBigalloc) return Status::Unsupported;
  if (!power_of_two(inode_size_) || inode_size_ < kOldInodeSize || inode_size_ > kMaxInodeSize || inode_size_ > block_size_) return Status::Corrupt;
  if (first_inode_ < kRootInode + 1) return Status::Corrupt;

  blocks_count_ = sb32(kSbBlocksCount);
  if (incompat_ & kIncompat64Bit) blocks_count_ |= uint64_t{sb32(kSbBlocksCountHi)} << 32;
  metadata_csum_ = (ro_compat_ & kRoMetadataCsum) != 0;
  group_csum_ = metadata_csum_ || (ro_compat_ & kRoGdtCsum) != 0;
  if (metadata_csum_) {
    if (sb_.view()[kSbChecksumType] != 1) return Status::Unsupported;  // (crc32c is the only kind there is)
    csum_seed_ = (incompat_ & kIncompatCsumSeed) ? sb32(kSbChecksumSeed) : crc32c(~0u, sb_.view().subview(kSbUuid, 16));
  }
  first_data_block_ = sb32(kSbFirstDataBlock);
  blocks_per_group_ = sb32(kSbBlocksPerGroup);
  inodes_per_group_ = sb32(kSbInodesPerGroup);
  inodes_count_ = sb32(kSbInodesCount);
  if (first_data_block_ != (block_size_ == 1024 ? 1u : 0u)) return Status::Corrupt;
  if (blocks_per_group_ == 0 || blocks_per_group_ > 8ull * block_size_ || inodes_per_group_ == 0 || inodes_per_group_ > 8ull * block_size_) return Status::Corrupt;
  if (blocks_count_ <= first_data_block_ || blocks_count_ > UINT64_MAX / block_size_) return Status::Corrupt;
  const uint64_t device_bytes = (storage.block_count - first_block_) * device_block_size_;
  if (blocks_count_ * block_size_ > device_bytes) return Status::Corrupt;
  const uint64_t groups = (blocks_count_ - first_data_block_ + blocks_per_group_ - 1) / blocks_per_group_;
  if (groups == 0 || groups > kMaxGroups) return Status::Corrupt;
  group_count_ = static_cast<uint32_t>(groups);
  if (uint64_t{group_count_} * inodes_per_group_ != inodes_count_ || first_inode_ > inodes_count_) return Status::Corrupt;
  itable_blocks_ = static_cast<uint32_t>((uint64_t{inodes_per_group_} * inode_size_ + block_size_ - 1) / block_size_);

  descriptor_size_ = 32;
  if (incompat_ & kIncompat64Bit) {
    descriptor_size_ = sb16(kSbDescSize);
    if (!power_of_two(descriptor_size_) || descriptor_size_ < 64 || descriptor_size_ > 1024 || descriptor_size_ > block_size_) return Status::Corrupt;
  }
  gdt_start_ = first_data_block_ + 1;
  const uint64_t table_bytes = uint64_t{group_count_} * descriptor_size_;
  if (table_bytes > kMaxGroupTable) return Status::Unsupported;
  if (gdt_start_ + (table_bytes + block_size_ - 1) / block_size_ > blocks_count_) return Status::Corrupt;
  if (!gd_.put_zeros(table_bytes)) return Status::OutOfMemory;
  s = read_bytes(gdt_start_ * block_size_, gd_.writable_view());
  if (s != Status::Ok) return s;
  for (uint32_t g = 0; g < group_count_; ++g) {
    const uint64_t table = gd_inode_table(g);
    if (gd_block_bitmap(g) >= blocks_count_ || gd_inode_bitmap(g) >= blocks_count_ || table >= blocks_count_ || itable_blocks_ > blocks_count_ - table)
      return Status::Corrupt;
    if (gd_free_blocks(g) > group_block_count(g) || gd_free_inodes(g) > inodes_per_group_) return Status::Corrupt;
    free_blocks_ += gd_free_blocks(g);
    free_inodes_ += gd_free_inodes(g);
  }
  for (uint32_t i = 0; i < 16 && sb_.view()[kSbVolumeName + i] != 0; ++i) label_.push_back(static_cast<char>(sb_.view()[kSbVolumeName + i]));

  decide_writable();
  return Status::Ok;
}

// A volume is written only if everything about it is understood and it was left clean.
void ExtFileSystem::decide_writable() {
  writable_ = false;
  if (device_.info().read_only) return;
  const uint16_t state = sb16(kSbState);
  if (!(state & kStateValid) || (state & (kStateError | kStateOrphans)) || sb32(kSbLastOrphan) != 0 || (compat_ & kCompatOrphanPresent)) return;
  if (incompat_ & ~kWritableIncompat) return;
  if (compat_ & ~kWritableCompat) return;
  if (ro_compat_ & ~kWritableRoCompat) return;
  writable_ = true;
}

uint32_t ExtFileSystem::group_block_count(uint32_t group) const {
  if (group + 1 < group_count_) return blocks_per_group_;
  return static_cast<uint32_t>(blocks_count_ - group_first_block(group));
}

bool ExtFileSystem::group_has_super(uint32_t group) const {
  if (group == 0) return true;
  if (compat_ & kCompatSparseSuper2) return group == sb32(kSbBackupBgs) || group == sb32(kSbBackupBgs + 4);
  if (!(ro_compat_ & kRoSparseSuper)) return true;
  if (group == 1) return true;
  for (uint64_t base = 3; base <= 7; base += 2) {  // (powers of 3, 5 and 7)
    uint64_t power = base;
    while (power < group) power *= base;
    if (power == group) return true;
  }
  return false;
}

uint64_t ExtFileSystem::gd_get(uint32_t group, uint32_t low, uint32_t high, bool wide16) const {
  ConstByteView d = gd_.view().subview(uint64_t{group} * descriptor_size_, descriptor_size_);
  uint64_t value = wide16 ? rd16(d, low) : rd32(d, low);
  if (descriptor_size_ >= 64) value |= (wide16 ? uint64_t{rd16(d, high)} << 16 : uint64_t{rd32(d, high)} << 32);
  return value;
}

void ExtFileSystem::gd_set(uint32_t group, uint32_t low, uint32_t high, bool wide16, uint64_t value) {
  ByteView d = gd_.writable_view().subview(uint64_t{group} * descriptor_size_, descriptor_size_);
  if (wide16) wr16(d, low, static_cast<uint32_t>(value));
  else wr32(d, low, static_cast<uint32_t>(value));
  if (descriptor_size_ >= 64) {
    if (wide16) wr16(d, high, static_cast<uint32_t>(value >> 16));
    else wr32(d, high, static_cast<uint32_t>(value >> 32));
  }
}

void ExtFileSystem::gd_set_flags(uint32_t group, uint32_t flags) {
  wr16(gd_.writable_view().subview(uint64_t{group} * descriptor_size_, descriptor_size_), kGdFlags, flags);
}

// The checksum of a group descriptor: crc32c with metadata_csum (low 16 bits of it), otherwise the crc16 of uninit_bg.
void ExtFileSystem::group_checksum_set(uint32_t group) {
  if (!group_csum_) return;
  ByteView d = gd_.writable_view().subview(uint64_t{group} * descriptor_size_, descriptor_size_);
  const ConstByteView rest = descriptor_size_ > kGdBlockBitmapHi ? ConstByteView(d).subview(kGdBlockBitmapHi, descriptor_size_ - kGdBlockBitmapHi) : ConstByteView();
  uint32_t crc;
  if (metadata_csum_) {
    crc = crc32c_u32(csum_seed_, group);
    crc = crc32c(crc, ConstByteView(d).first(kGdChecksum));
    crc = crc32c_zeros(crc, 2);
    crc = crc32c(crc, rest);
  } else {
    const uint8_t number[4] = {static_cast<uint8_t>(group), static_cast<uint8_t>(group >> 8), static_cast<uint8_t>(group >> 16), static_cast<uint8_t>(group >> 24)};
    uint16_t sum = crc16(0xFFFF, sb_.view().subview(kSbUuid, 16));
    sum = crc16(sum, ConstByteView(number));
    sum = crc16(sum, ConstByteView(d).first(kGdChecksum));
    if (incompat_ & kIncompat64Bit) sum = crc16(sum, rest);
    crc = sum;
  }
  wr16(d, kGdChecksum, crc & 0xFFFF);
}

// The checksum of a bitmap lives in the group descriptor.
void ExtFileSystem::bitmap_checksum_set(uint32_t group, bool inode_bitmap, ConstByteView bitmap) {
  if (!metadata_csum_) return;
  ByteView d = gd_.writable_view().subview(uint64_t{group} * descriptor_size_, descriptor_size_);
  const uint32_t crc = crc32c(csum_seed_, bitmap.first((inode_bitmap ? inodes_per_group_ : blocks_per_group_) / 8));
  if (inode_bitmap) {
    wr16(d, kGdInodeBitmapCsum, crc & 0xFFFF);
    if (descriptor_size_ >= kGdInodeBitmapCsumHi + 2) wr16(d, kGdInodeBitmapCsumHi, crc >> 16);
  } else {
    wr16(d, kGdBlockBitmapCsum, crc & 0xFFFF);
    if (descriptor_size_ >= kGdBlockBitmapCsumHi + 2) wr16(d, kGdBlockBitmapCsumHi, crc >> 16);
  }
}

// The block bitmap of a group that was never used (and so was never written): the superblock and descriptor copies, its own bitmaps and
// inode table where they lie inside it, and the padding after the last block.
void ExtFileSystem::synth_block_bitmap(uint32_t group, ByteView bitmap) const {
  bitmap.fill(0);
  const uint64_t first = group_first_block(group), count = group_block_count(group);
  auto mark = [&](uint64_t block) {
    if (block >= first && block - first < count) bit_set(bitmap, block - first);
  };
  if (group_has_super(group)) {
    const uint64_t table_blocks = (uint64_t{group_count_} * descriptor_size_ + block_size_ - 1) / block_size_;
    for (uint64_t k = 0; k < 1 + table_blocks + sb16(kSbReservedGdt); ++k) mark(first + k);
  }
  mark(gd_block_bitmap(group));
  mark(gd_inode_bitmap(group));
  for (uint32_t k = 0; k < itable_blocks_; ++k) mark(gd_inode_table(group) + k);
  for (uint64_t bit = count; bit < uint64_t{block_size_} * 8; ++bit) bit_set(bitmap, bit);
}

Status ExtFileSystem::store_group(uint32_t group) {
  group_checksum_set(group);
  const uint64_t offset = uint64_t{group} * descriptor_size_;
  return write_bytes(gdt_start_ * block_size_ + offset, gd_.view().subview(offset, descriptor_size_));
}

Status ExtFileSystem::write_super() {
  ByteView v = sb_.writable_view();
  wr32(v, kSbFreeBlocks, static_cast<uint32_t>(free_blocks_));
  if (incompat_ & kIncompat64Bit) wr32(v, kSbFreeBlocksHi, static_cast<uint32_t>(free_blocks_ >> 32));
  wr32(v, kSbFreeInodes, static_cast<uint32_t>(free_inodes_));
  if (metadata_csum_) wr32(v, kSbChecksum, crc32c(~0u, sb_.view().first(kSbChecksum)));
  return write_bytes(kSuperblockOffset, sb_.view());
}

Status ExtFileSystem::begin_write() {
  if (!writable_) return Status::ReadOnly;
  if (dirty_set_) return Status::Ok;
  const uint16_t state = sb16(kSbState);
  wr16(sb_.writable_view(), kSbState, state & ~kStateValid);
  const Status s = write_super();
  if (s != Status::Ok) {
    wr16(sb_.writable_view(), kSbState, state);
    return s;
  }
  dirty_set_ = true;
  return Status::Ok;
}

Status ExtFileSystem::finish_write() {
  if (!device_.flush()) return Status::IoError;
  wr16(sb_.writable_view(), kSbState, sb16(kSbState) | kStateValid);
  const Status s = write_super();
  if (s != Status::Ok) return s;
  if (!device_.flush()) return Status::IoError;
  dirty_set_ = false;
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Inodes

Status ExtFileSystem::inode_offset(uint32_t number, uint64_t* offset) {
  if (number == 0 || number > inodes_count_) return Status::Corrupt;
  const uint32_t index = number - 1;
  const uint32_t group = index / inodes_per_group_;
  *offset = gd_inode_table(group) * block_size_ + uint64_t{index % inodes_per_group_} * inode_size_;
  return Status::Ok;
}

Status ExtFileSystem::read_inode(uint32_t number, Inode* inode) {
  uint64_t offset = 0;
  Status s = inode_offset(number, &offset);
  if (s != Status::Ok) return s;
  inode->number = number;
  inode->length = inode_size_;
  return read_bytes(offset, inode->bytes());
}

uint32_t ExtFileSystem::inode_seed(const Inode& inode) const {
  return crc32c_u32(crc32c_u32(csum_seed_, inode.number), rd32(inode.bytes(), kInGeneration));
}

void ExtFileSystem::inode_checksum_set(Inode* inode) const {
  ByteView v = inode->bytes();
  const bool has_high = inode_size_ > kGoodOldInodeSize && rd16(v, kInExtraIsize) >= 4;  // (the high half fits in the part that is used)
  wr16(v, kInChecksumLo, 0);
  if (has_high) wr16(v, kInChecksumHi, 0);
  const uint32_t crc = crc32c(inode_seed(*inode), v);
  wr16(v, kInChecksumLo, crc & 0xFFFF);
  if (has_high) wr16(v, kInChecksumHi, crc >> 16);
}

Status ExtFileSystem::write_inode(const Inode& inode) {
  uint64_t offset = 0;
  const Status s = inode_offset(inode.number, &offset);
  if (s != Status::Ok) return s;
  if (!metadata_csum_) return write_bytes(offset, inode.bytes());
  Inode copy = inode;
  inode_checksum_set(&copy);
  return write_bytes(offset, copy.bytes());
}

uint64_t ExtFileSystem::inode_blocks(const Inode& inode) const {
  uint64_t count = rd32(inode.bytes(), kInBlocksLo);
  if (ro_compat_ & kRoHugeFile) count |= uint64_t{rd16(inode.bytes(), kInBlocksHi)} << 32;
  return count;
}

// Adds to i_blocks, which counts 512 byte sectors (filesystem blocks for a file marked huge).
void ExtFileSystem::inode_add_blocks(Inode* inode, int64_t delta) {
  const bool huge = (ro_compat_ & kRoHugeFile) && (inode->flags() & kFlagHugeFile);
  const int64_t unit = huge ? 1 : block_size_ / 512;
  int64_t value = static_cast<int64_t>(inode_blocks(*inode)) + delta * unit;
  if (value < 0) value = 0;
  wr32(inode->bytes(), kInBlocksLo, static_cast<uint32_t>(value));
  if (ro_compat_ & kRoHugeFile) wr16(inode->bytes(), kInBlocksHi, static_cast<uint32_t>(static_cast<uint64_t>(value) >> 32));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Mapping blocks of a file

Status ExtFileSystem::check_pointer(uint64_t block) const {
  if (block == 0) return Status::Ok;
  return block < first_data_block_ || block >= blocks_count_ ? Status::Corrupt : Status::Ok;
}

// The disk block holding block `index` of a file with the classic block map (0: a hole).
Status ExtFileSystem::bm_get(const Inode& inode, uint64_t index, uint64_t* block) {
  *block = 0;
  const uint64_t n = block_size_ / 4;
  if (index < 12) {
    *block = inode.block_pointer(static_cast<uint32_t>(index));
    return check_pointer(*block);
  }
  index -= 12;
  uint32_t level = 0;
  if (index < n) {
    level = 1;
  } else if ((index -= n) < n * n) {
    level = 2;
  } else if ((index -= n * n) < n * n * n) {
    level = 3;
  } else {
    return Status::Ok;  // past the largest file the map can describe
  }
  uint64_t pointer = inode.block_pointer(11 + level);
  Status s = check_pointer(pointer);
  if (s != Status::Ok) return s;
  uint64_t divisor = level == 3 ? n * n : level == 2 ? n : 1;
  for (; level > 0 && pointer != 0; --level, divisor /= n) {
    uint32_t next = 0;
    s = read_word(pointer, index / divisor, &next);
    if (s != Status::Ok) return s;
    index %= divisor;
    pointer = next;
    s = check_pointer(pointer);
    if (s != Status::Ok) return s;
  }
  *block = pointer;
  return Status::Ok;
}

// Looks at the blocks from `first` on: *block is the disk block of the first (0 for a hole) and *run how many of the next blocks (at
// most `max`) are in the same situation: all holes, or consecutive disk blocks.
Status ExtFileSystem::map_run(const Inode& inode, uint64_t first, uint64_t max, uint64_t* block, uint64_t* run) {
  if (uses_extents(inode)) return (incompat_ & kIncompatExtents) ? ext_map_run(inode, first, max, block, run) : Status::Corrupt;
  uint64_t start = 0;
  Status s = bm_get(inode, first, &start);
  if (s != Status::Ok) return s;
  uint64_t length = 1;
  while (length < max) {
    uint64_t next = 0;
    s = bm_get(inode, first + length, &next);
    if (s != Status::Ok) return s;
    if (start == 0 ? next != 0 : next != start + length) break;
    ++length;
  }
  *block = start;
  *run = length;
  return Status::Ok;
}

// The largest file the block map can hold, in blocks.
Status ExtFileSystem::check_size(const Inode& inode, uint64_t size) const {
  if (uses_extents(inode)) return size / block_size_ >= 0xFFFFFFFFull ? Status::NoSpace : Status::Ok;
  const uint64_t n = block_size_ / 4;
  const uint64_t blocks = 12 + n + n * n + n * n * n;
  const uint64_t limit = blocks > 0xFFFFFFFFull ? 0xFFFFFFFFull : blocks;
  return size / block_size_ >= limit ? Status::NoSpace : Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// File data

bool ExtFileSystem::is_fast_symlink(const Inode& inode) const {
  const uint64_t extra = rd32(inode.bytes(), kInFileAcl) != 0 ? block_size_ / 512 : 0;
  return inode.is_symlink() && inode.size() < 60 && !(inode.flags() & kFlagExtents) && inode_blocks(inode) == extra;
}

Status ExtFileSystem::read_data(const Inode& inode, uint64_t offset, ByteView out, uint64_t* transferred) {
  *transferred = 0;
  const uint64_t size = inode.size();
  if (offset >= size || out.empty()) return Status::Ok;
  const uint64_t want = size - offset < out.size() ? size - offset : out.size();
  if (is_fast_symlink(inode)) {
    out.first(want).copy_from(inode.bytes().subview(kInBlock + offset, want));
    *transferred = want;
    return Status::Ok;
  }
  uint64_t done = 0;
  while (done < want) {
    const uint64_t position = offset + done;
    const uint64_t index = position / block_size_;
    const uint32_t within = static_cast<uint32_t>(position % block_size_);
    const uint64_t blocks_left = (within + (want - done) + block_size_ - 1) / block_size_;
    uint64_t block = 0, run = 0;
    const Status s = map_run(inode, index, blocks_left, &block, &run);
    if (s != Status::Ok) return s;
    uint64_t bytes = run * block_size_ - within;
    if (bytes > want - done) bytes = want - done;
    if (block == 0) {
      out.subview(done, bytes).fill(0);
    } else {
      if (run > blocks_count_ - block) return Status::Corrupt;
      const Status r = read_bytes(block * block_size_ + within, out.subview(done, bytes));
      if (r != Status::Ok) return r;
    }
    done += bytes;
    *transferred = done;
  }
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Directories

// Checks the entry at `offset` of a directory block and fills `entry`.
Status ExtFileSystem::dir_parse(ConstByteView block, uint32_t offset, DirEntry* entry) const {
  if (offset > block_size_ || block_size_ - offset < kDirHeader) return Status::Corrupt;
  uint32_t length = rd16(block, offset + 4);
  if (length == 0 || length == 65535) length = block_size_ == 65536 ? 65536 : length;
  else length = (length & 65532) | ((length & 3) << 16);
  const uint32_t name_length = rd8(block, offset + 6);
  if (length < kDirHeader || (length & 3) != 0 || length > block_size_ - offset || kDirHeader + name_length > length) return Status::Corrupt;
  entry->inode = rd32(block, offset);
  entry->offset = offset;
  entry->record_length = length;
  entry->name_length = name_length;
  entry->type = (incompat_ & kIncompatFiletype) ? rd8(block, offset + 7) : 0;
  return Status::Ok;
}

// The next name of a directory, not counting "." and "..".
Status ExtFileSystem::dir_next(const Inode& dir, Cursor* cursor, DirEntry* entry, FixedString<kMaxNameLength + 1>* name) {
  ByteBuffer block(allocator_);
  if (!block.put_zeros(block_size_)) return Status::OutOfMemory;
  const uint64_t size = dir.size();
  bool loaded = false;
  uint64_t loaded_index = 0;
  for (;;) {
    if (cursor->block >= (size + block_size_ - 1) / block_size_) return Status::EndOfDirectory;
    if (!loaded || loaded_index != cursor->block) {
      uint64_t disk = 0, run = 0;
      Status s = map_run(dir, cursor->block, 1, &disk, &run);
      if (s != Status::Ok) return s;
      if (disk == 0) {  // a hole in a directory: nothing there
        ++cursor->block;
        cursor->offset = 0;
        continue;
      }
      s = read_block(disk, block.writable_view());
      if (s != Status::Ok) return s;
      loaded = true;
      loaded_index = cursor->block;
    }
    if (cursor->offset >= block_size_) {
      ++cursor->block;
      cursor->offset = 0;
      continue;
    }
    DirEntry found;
    const Status s = dir_parse(block.view(), cursor->offset, &found);
    if (s != Status::Ok) {
      if (cursor->offset == 0) return s;  // damaged from its start
      ++cursor->block;                    // (the directory changed under the cursor: go on with the next block)
      cursor->offset = 0;
      continue;
    }
    cursor->offset += found.record_length;
    if (found.inode == 0 || found.name_length == 0) continue;
    const StringView text(reinterpret_cast<const char*>(block.view().subview(found.offset + kDirHeader, found.name_length).data()), found.name_length);
    if (text.equals(".") || text.equals("..")) continue;
    if (found.inode < first_inode_ || found.inode > inodes_count_) continue;  // (the journal and the other reserved inodes are not files)
    if (!name->assign(text)) continue;
    *entry = found;
    return Status::Ok;
  }
}

// Finds `name` in a directory: the block index (within the directory) and the entry.
Status ExtFileSystem::dir_find(const Inode& dir, StringView name, uint64_t* block_index, DirEntry* entry) {
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
    for (uint32_t offset = 0; offset < block_size_;) {
      DirEntry found;
      s = dir_parse(block.view(), offset, &found);
      if (s != Status::Ok) return s;
      offset += found.record_length;
      if (found.inode == 0 || found.name_length != name.size()) continue;
      const StringView text(reinterpret_cast<const char*>(block.view().subview(found.offset + kDirHeader, found.name_length).data()), found.name_length);
      if (!text.equals(name)) continue;
      *block_index = index;
      *entry = found;
      return Status::Ok;
    }
  }
  return Status::NotFound;
}

Status ExtFileSystem::dir_lookup(const Inode& dir, StringView name, uint32_t* number, uint8_t* type) {
  uint64_t index = 0;
  DirEntry entry;
  const Status s = dir_find(dir, name, &index, &entry);
  if (s != Status::Ok) return s;
  if (entry.inode > inodes_count_) return Status::Corrupt;
  *number = entry.inode;
  if (type) *type = entry.type;
  return Status::Ok;
}

// A directory is empty when nothing but "." and ".." is left.
Status ExtFileSystem::dir_empty(const Inode& dir, bool* empty) {
  ByteBuffer block(allocator_);
  if (!block.put_zeros(block_size_)) return Status::OutOfMemory;
  const uint64_t blocks = (dir.size() + block_size_ - 1) / block_size_;
  *empty = true;
  for (uint64_t index = 0; index < blocks; ++index) {
    uint64_t disk = 0, run = 0;
    Status s = map_run(dir, index, 1, &disk, &run);
    if (s != Status::Ok) return s;
    if (disk == 0) continue;
    s = read_block(disk, block.writable_view());
    if (s != Status::Ok) return s;
    for (uint32_t offset = 0; offset < block_size_;) {
      DirEntry found;
      s = dir_parse(block.view(), offset, &found);
      if (s != Status::Ok) return s;
      offset += found.record_length;
      if (found.inode == 0) continue;
      const StringView text(reinterpret_cast<const char*>(block.view().subview(found.offset + kDirHeader, found.name_length).data()), found.name_length);
      if (text.equals(".") || text.equals("..")) continue;
      *empty = false;
      return Status::Ok;
    }
  }
  return Status::Ok;
}

Status ExtFileSystem::dir_parent(const Inode& dir, uint32_t* parent) {
  uint8_t type = 0;
  return dir_lookup(dir, "..", parent, &type);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Paths

Status ExtFileSystem::describe(const Inode& inode, StringView name, DirectoryEntry* entry) {
  DirectoryEntry result;
  if (inode.is_directory()) {
    result.type = NodeType::Directory;
  } else if (inode.is_regular() || inode.is_symlink()) {
    result.type = NodeType::File;
    result.size = inode.size();
  } else {
    return Status::NotFound;
  }
  if (!result.name.assign(name)) return Status::InvalidPath;
  result.read_only = inode_immutable(inode);
  result.modified = inode_time(inode, kInMtime, kInMtimeExtra);
  if (inode_size_ > kGoodOldInodeSize && rd16(inode.bytes(), kInExtraIsize) >= 24) result.created = inode_time(inode, kInCrtime, kInCrtimeExtra);
  *entry = result;
  return Status::Ok;
}

// Walks `path` from the root.
Status ExtFileSystem::resolve(StringView path, Resolved* result) {
  if (block_.empty()) return Status::InvalidArgument;
  Resolved current;
  current.number = current.parent = kRootInode;
  current.directory = true;
  StringView rest = path, component;
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    if (component.equals("..") || component.size() > kMaxNameLength) return Status::InvalidPath;
    if (!current.directory) return Status::NotADirectory;
    Inode dir;
    Status s = read_inode(current.number, &dir);
    if (s != Status::Ok) return s;
    uint32_t number = 0;
    s = dir_lookup(dir, component, &number, nullptr);
    if (s != Status::Ok) return s;
    if (number < first_inode_ && number != kRootInode) return Status::NotFound;
    Inode inode;
    s = read_inode(number, &inode);
    if (s != Status::Ok) return s;
    if (inode.mode() == 0 || inode.links() == 0) return Status::NotFound;  // an entry whose inode is free
    if (!inode.is_directory() && !inode.is_regular() && !inode.is_symlink()) return Status::NotFound;
    Resolved next;
    next.parent = current.number;
    next.number = number;
    next.directory = inode.is_directory();
    current = next;
  }
  *result = current;
  return Status::Ok;
}

Status ExtFileSystem::split(StringView path, Resolved* parent, StringView* leaf) {
  StringView parent_path;
  if (!split_parent(path, &parent_path, leaf)) return Status::InvalidPath;
  const Status s = resolve(parent_path, parent);
  if (s != Status::Ok) return s;
  return parent->directory ? Status::Ok : Status::NotADirectory;
}

bool ExtFileSystem::is_open(uint32_t number) const {
  for (const OpenHandle* h = open_; h; h = h->next)
    if (h->number == number) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The filesystem interface

FileSystemInfo ExtFileSystem::info() const {
  FileSystemInfo result;
  const bool ext4 = (incompat_ & (kIncompatExtents | kIncompat64Bit | kIncompatFlexBg | kIncompatMmp | kIncompatEaInode)) ||
                    (ro_compat_ & (kRoHugeFile | kRoGdtCsum | kRoDirNlink | kRoExtraIsize | kRoMetadataCsum));
  result.type_name.assign(ext4 ? "ext4" : (compat_ & kCompatHasJournal) ? "ext3" : "ext2");
  result.label = label_;
  result.total_bytes = blocks_count_ * block_size_;
  result.read_only = !writable_;
  if (writable_) {
    kernel::ScopedRecursiveMutex lock(mutex_);
    result.free_bytes = free_blocks_ * block_size_;
  }
  return result;
}

Status ExtFileSystem::stat(StringView path, DirectoryEntry* entry) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Resolved r;
  Status s = resolve(path, &r);
  if (s != Status::Ok) return s;
  if (r.number == kRootInode) {
    DirectoryEntry root;
    root.type = NodeType::Directory;
    *entry = root;
    return Status::Ok;
  }
  Inode inode;
  s = read_inode(r.number, &inode);
  if (s != Status::Ok) return s;
  Resolved parent;
  StringView leaf;
  s = split(path, &parent, &leaf);
  if (s != Status::Ok) return s;
  return describe(inode, leaf, entry);
}

Status ExtFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Resolved r;
  Status s = resolve(path, &r);
  if (s != Status::Ok) return s;
  if (!r.directory) return Status::NotADirectory;
  UniquePointer<IDirectory> handle = UniquePointer<IDirectory>::make_as<Directory>(allocator_, *this, r.number);
  if (!handle) return Status::OutOfMemory;
  *directory = move(handle);
  return Status::Ok;
}

Status ExtFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  const bool write = mode != OpenMode::Read;
  if (write && !writable_) return Status::ReadOnly;
  Resolved r;
  Status s = resolve(path, &r);
  uint32_t number = 0;
  if (s == Status::NotFound && (mode == OpenMode::CreateOrOpen || mode == OpenMode::CreateTruncate)) {
    s = create_node(path, false, &number);
    if (s != Status::Ok) return s;
  } else if (s != Status::Ok) {
    return s;
  } else {
    if (r.directory) return Status::IsADirectory;
    number = r.number;
    Inode inode;
    s = read_inode(number, &inode);
    if (s != Status::Ok) return s;
    if (write && inode_immutable(inode)) return Status::ReadOnly;
    if (write && !inode.is_regular()) return Status::Unsupported;
  }
  UniquePointer<IFile> handle = UniquePointer<IFile>::make_as<File>(allocator_, *this, number, write);
  if (!handle) return Status::OutOfMemory;
  if (mode == OpenMode::CreateTruncate) {
    const Status t = handle->resize(0);
    if (t != Status::Ok) return t;
  }
  *file = move(handle);
  return Status::Ok;
}

Status ExtFileSystem::flush() {
  kernel::ScopedRecursiveMutex lock(mutex_);
  if (dirty_set_) {
    const Status s = finish_write();
    if (s != Status::Ok) return s;
  }
  return device_.flush() ? Status::Ok : Status::IoError;
}

}  // namespace filesystem
