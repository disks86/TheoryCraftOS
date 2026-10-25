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

#include "FileSystem/Befs/BefsFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace filesystem {

namespace {
constexpr uint32_t kSuperMagic1 = 0x42465331, kSuperMagic2 = 0xdd121031, kSuperMagic3 = 0x15b6830e;
constexpr uint32_t kByteOrderLittle = 0x42494745, kByteOrderBig = 0x45474942;
constexpr uint32_t kInodeMagic = 0x3bbe0ad9, kTreeMagic = 0x69f6c2e8;
constexpr uint32_t kInodeInUse = 0x1, kLongSymlink = 0x40;
constexpr uint32_t kVolumeClean = 0x434c454e;
constexpr uint32_t kTxHard = 192;           // most blocks one transaction holds
constexpr uint32_t kTxSoft = 96;            // a long operation commits what it has when it holds this many
constexpr uint32_t kArrayBlocks = 4;        // length of the arrays of block runs we create (as Be's file system does)
constexpr uint32_t kMaxRunBlocks = 0xFFFF;
constexpr uint32_t kTypeMask = 0xF000, kTypeLink = 0xA000;
constexpr uint64_t kSuperOffset = 512;
constexpr uint32_t kSuperSize = 132, kInodeBytes = 232, kInlineLink = 144, kTreeHeaderSize = 40, kNodeHeaderSize = 28;
constexpr uint32_t kMinNodeSize = 64, kMaxNodeSize = 1u << 16;
constexpr uint32_t kSeekBudget = 1u << 18;  // array blocks read for one lookup of a file block: stops loops in damaged run tables
constexpr uint32_t kMaxDepth = 64;          // B+tree levels followed before the tree is taken for a loop
constexpr uint64_t kNone = ~uint64_t{0};

// Memory that is given back at the end of the scope.
class Owned {
 public:
  Owned(allocators::IAllocator& allocator, uint64_t size) : allocator_(allocator), view_(allocate_view<uint8_t>(allocator, size)) {}
  ~Owned() { release_view(allocator_, view_); }
  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;
  bool ok() const { return !view_.empty(); }
  ByteView view() const { return view_; }

 private:
  allocators::IAllocator& allocator_;
  ByteView view_;
};

// Reads `count` (2, 4 or 8) little-endian bytes at `at`; false if they are not inside `data`.
bool field(ConstByteView data, uint64_t at, uint32_t count, uint64_t* value) {
  ConstByteView part;
  if (!data.try_subview(at, count, &part)) return false;
  ByteReader in(part);
  *value = count == 2 ? in.le16() : count == 4 ? in.le32() : in.le64();
  return in.ok();
}

// Byte order of BeFS names: plain unsigned bytes, then length.
int compare_names(ConstByteView a, StringView b) {
  const uint64_t common = a.size() < b.size() ? a.size() : b.size();
  for (uint64_t i = 0; i < common; ++i) {
    const uint8_t x = a[i], y = static_cast<uint8_t>(b[i]);
    if (x != y) return x < y ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

bool is_dot_or_dotdot(ConstByteView name) { return (name.size() == 1 && name[0] == '.') || (name.size() == 2 && name[0] == '.' && name[1] == '.'); }

Status read_run(ByteReader* in, uint32_t* group, uint16_t* start, uint16_t* length) {
  *group = in->le32();
  *start = in->le16();
  *length = in->le16();
  return in->ok() ? Status::Ok : Status::Corrupt;
}
}  // namespace

// An open file. It keeps only where its inode is: the inode is read again for every call, so several handles on one file (and a cut
// made through another handle) stay consistent.
class BefsFileSystem::File final : public IFile {
 public:
  File(BefsFileSystem& fs, uint64_t inode_block, bool writable) : fs_(fs), inode_block_(inode_block), writable_(writable) {}

  uint64_t size() const override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    Inode inode;
    if (fs_.read_inode(inode_block_, &inode) != Status::Ok) return 0;
    return length_of(inode);
  }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    Inode inode;
    Status s = fs_.read_inode(inode_block_, &inode);
    if (s != Status::Ok) return s;
    const uint64_t length = length_of(inode);
    if (offset >= length || buffer.empty()) return Status::Ok;
    uint64_t count = length - offset;
    if (count > buffer.size()) count = buffer.size();
    if (inode.inline_link) {
      buffer.first(count).copy_from(ConstByteView(inode.link).subview(offset, count));
    } else {
      s = fs_.read_stream(inode, offset, buffer.first(count));
      if (s != Status::Ok) return s;
    }
    *transferred = count;
    return Status::Ok;
  }
  Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!writable_) return Status::ReadOnly;
    Status s = fs_.writable();
    if (s != Status::Ok) return s;
    if (data.empty()) return Status::Ok;
    Inode inode;
    s = fs_.read_inode(inode_block_, &inode);
    if (s != Status::Ok) return s;
    if (!inode.is_regular()) return Status::Unsupported;
    const uint64_t end = offset + data.size();
    if (end < offset || end > fs_.max_file_size()) return Status::NoSpace;
    const uint64_t old_size = inode.size;
    fs_.tx_begin();
    s = fs_.grow_stream(&inode, (end + fs_.block_size_ - 1) >> fs_.block_shift_);
    if (s == Status::Ok && offset > old_size) s = fs_.zero_stream(inode, old_size, offset);
    if (s == Status::Ok) {
      s = fs_.write_stream(inode, offset, data);
      if (s != Status::Ok && offset < old_size) *transferred = old_size - offset < data.size() ? old_size - offset : data.size();
    }
    if (s == Status::Ok) {
      if (end > old_size) inode.size = end;
      s = fs_.store_inode(inode);
    }
    if (s == Status::Ok) s = fs_.tx_commit();
    if (s != Status::Ok) {
      fs_.tx_rollback();
      return s;
    }
    *transferred = data.size();
    return Status::Ok;
  }
  Status resize(uint64_t new_size) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::ReadOnly;
    Status s = fs_.writable();
    if (s != Status::Ok) return s;
    if (new_size > fs_.max_file_size()) return Status::NoSpace;
    Inode inode;
    s = fs_.read_inode(inode_block_, &inode);
    if (s != Status::Ok) return s;
    if (!inode.is_regular()) return Status::Unsupported;
    const uint64_t old_size = inode.size;
    const uint64_t blocks = (new_size + fs_.block_size_ - 1) >> fs_.block_shift_;
    fs_.tx_begin();
    if (new_size > old_size) {
      s = fs_.grow_stream(&inode, blocks);
      if (s == Status::Ok) s = fs_.zero_stream(inode, old_size, new_size);
      inode.size = new_size;
    } else {  // (also when the size stays: blocks a failed grow left behind past the end go back)
      inode.size = new_size;  // the size never claims more than the blocks that are left
      s = fs_.store_inode(inode);
      if (s == Status::Ok) s = fs_.shrink_stream(&inode, blocks);
    }
    if (s == Status::Ok) s = fs_.store_inode(inode);
    if (s == Status::Ok) s = fs_.tx_commit();
    if (s != Status::Ok) fs_.tx_rollback();
    return s;
  }
  Status flush() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    return fs_.flush();
  }

 private:
  static uint64_t length_of(const Inode& inode) { return inode.inline_link ? inode.link_size : inode.size; }

  BefsFileSystem& fs_;
  const uint64_t inode_block_;
  const bool writable_;
};

// An open directory: walks the leaves of its B+tree from the left.
class BefsFileSystem::Directory final : public IDirectory {
 public:
  Directory(BefsFileSystem& fs, const Inode& inode, const Tree& tree, ByteView buffer)
      : fs_(fs), inode_(inode), tree_(tree), buffer_(buffer) {}
  ~Directory() { release_view(fs_.allocator_, buffer_); }

  Status next(DirectoryEntry* entry) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (ended_) return Status::EndOfDirectory;
    if (!loaded_) {
      Status s = descend();
      if (s != Status::Ok) return s;
      loaded_ = true;
    }
    const uint64_t max_nodes = inode_.size / tree_.node_size + 1;
    for (;;) {
      while (index_ < node_.count) {
        ConstByteView key;
        uint64_t inode_block = 0;
        if (!node_key(node_, index_, &key) || !node_value(node_, index_, &inode_block)) return Status::Corrupt;
        ++index_;
        if (is_dot_or_dotdot(key)) continue;
        FixedString<kMaxNameLength + 1> name;
        if (!name.assign(StringView(key))) return Status::Corrupt;
        Inode child;
        const Status s = fs_.read_inode(inode_block, &child);
        if (s != Status::Ok) return s;
        fill_entry(child, name.view(), entry);
        return Status::Ok;
      }
      if (node_.right == kNone || ++hops_ > max_nodes) {
        ended_ = true;
        return hops_ > max_nodes ? Status::Corrupt : Status::EndOfDirectory;
      }
      const Status s = fs_.read_node(inode_, tree_, node_.right, buffer_, &node_);
      if (s != Status::Ok) return s;
      if (!node_.leaf()) return Status::Corrupt;
      index_ = 0;
    }
  }
  void rewind() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    loaded_ = ended_ = false;
    index_ = 0;
    hops_ = 0;
  }

 private:
  // Loads the leftmost leaf.
  Status descend() {
    uint64_t at = tree_.root;
    for (uint32_t depth = 0; depth < kMaxDepth; ++depth) {
      const Status s = fs_.read_node(inode_, tree_, at, buffer_, &node_);
      if (s != Status::Ok) return s;
      if (node_.leaf()) {
        index_ = 0;
        hops_ = 0;
        return Status::Ok;
      }
      if (node_.count == 0) {
        at = node_.overflow;
      } else if (!node_value(node_, 0, &at)) {
        return Status::Corrupt;
      }
    }
    return Status::Corrupt;
  }

  BefsFileSystem& fs_;
  const Inode inode_;
  const Tree tree_;
  ByteView buffer_;
  Node node_;
  uint32_t index_ = 0;
  uint64_t hops_ = 0;
  bool loaded_ = false, ended_ = false;
};

Status BefsFileSystem::mount(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block,
                             UniquePointer<IFileSystem>* result) {
  UniquePointer<IFileSystem> fs = UniquePointer<IFileSystem>::make_as<BefsFileSystem>(allocator, allocator, device, first_block);
  if (!fs) return Status::OutOfMemory;
  const Status s = static_cast<BefsFileSystem*>(fs.get())->init();
  if (s != Status::Ok) return s;
  *result = util::move(fs);
  return Status::Ok;
}

BefsFileSystem::BefsFileSystem(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block)
    : allocator_(allocator), device_(device), first_block_(first_block) {}

BefsFileSystem::~BefsFileSystem() {
  tx_rollback();
  release_view(allocator_, block_);
  release_view(allocator_, scratch_);
  release_view(allocator_, bitmap_);
  release_view(allocator_, zeros_);
  release_view(allocator_, tx_);
}

Status BefsFileSystem::init() {
  const hardware::StorageInfo storage = device_.info();
  device_block_size_ = storage.block_size;
  if (device_block_size_ == 0 || (device_block_size_ & (device_block_size_ - 1)) != 0 || first_block_ >= storage.block_count)
    return Status::InvalidArgument;
  if (storage.block_count > ~uint64_t{0} / device_block_size_) return Status::InvalidArgument;
  volume_bytes_ = (storage.block_count - first_block_) * device_block_size_;
  block_ = allocate_view<uint8_t>(allocator_, device_block_size_);
  if (block_.empty()) return Status::OutOfMemory;

  uint8_t raw[kSuperSize];
  Status s = read_bytes(kSuperOffset, ByteView(raw));
  if (s != Status::Ok) return s;
  ByteReader in{ConstByteView(raw)};
  ConstByteView name = in.take(32);
  if (in.le32() != kSuperMagic1) return Status::Corrupt;
  const uint32_t order = in.le32();
  if (order == kByteOrderBig) return Status::Unsupported;
  if (order != kByteOrderLittle) return Status::Corrupt;
  block_size_ = in.le32();
  block_shift_ = in.le32();
  block_count_ = in.le64();
  used_blocks_ = in.le64();
  const uint32_t inode_size = in.le32();
  if (in.le32() != kSuperMagic2) return Status::Corrupt;
  const uint32_t bitmap_per_group = in.le32();
  group_shift_ = in.le32();
  group_count_ = in.le32();
  const uint32_t volume_flags = in.le32();
  in.skip(8);  // the journal's blocks
  const uint64_t log_start = in.le64(), log_end = in.le64();
  if (in.le32() != kSuperMagic3) return Status::Corrupt;
  uint32_t group = 0;
  uint16_t start = 0, length = 0;
  if (read_run(&in, &group, &start, &length) != Status::Ok) return Status::Corrupt;
  root_run_ = Run{group, start, length};
  if (block_size_ < 1024 || block_size_ > 8192 || block_shift_ > 13 || (1u << block_shift_) != block_size_) return Status::Corrupt;
  if (block_count_ == 0 || block_count_ > (uint64_t{1} << 50) || used_blocks_ > block_count_) return Status::Corrupt;
  if (inode_size < kInodeBytes || inode_size > (1u << 16)) return Status::Corrupt;
  if (group_shift_ > 40 || group_count_ == 0 || group_count_ > (1u << 20)) return Status::Corrupt;
  if (!run_to_block(root_run_, &root_block_)) return Status::Corrupt;

  FixedString<kMaxNameLength + 1> text;
  for (uint8_t c : name) {
    if (c == 0) break;
    text.push_back(static_cast<char>(c));
  }
  label_.assign(text.view().first(text.size() < label_.capacity() ? text.size() : label_.capacity()));

  scratch_ = allocate_view<uint8_t>(allocator_, uint64_t{block_size_} * 2);
  if (scratch_.empty()) return Status::OutOfMemory;
  Inode root;
  s = read_inode(root_block_, &root);
  if (s != Status::Ok) return s;
  if (!root.is_directory()) return Status::Corrupt;

  // Writable only for a clean volume (nothing in the journal to replay) on a device that can be written, with a bitmap we understand.
  if (!storage.read_only && volume_flags == kVolumeClean && log_start == log_end && group_shift_ <= 16) {
    bitmap_per_group_ = static_cast<uint32_t>((((uint64_t{1} << group_shift_) + (uint64_t{block_size_} << 3) - 1)) / (uint64_t{block_size_} << 3));
    if (bitmap_per_group == bitmap_per_group_) {
      bitmap_ = allocate_view<uint8_t>(allocator_, block_size_);
      zeros_ = allocate_view<uint8_t>(allocator_, block_size_);
      tx_ = allocate_view<TxBlock>(allocator_, kTxHard);
      if (bitmap_.empty() || zeros_.empty() || tx_.empty()) return Status::OutOfMemory;
      zeros_.fill(0);
      writable_ = check_bitmap() == Status::Ok;
    }
  }
  return Status::Ok;
}

FileSystemInfo BefsFileSystem::info() const {
  FileSystemInfo result;
  result.type_name.assign("BeFS");
  result.label = label_;
  result.total_bytes = block_count_ << block_shift_;
  result.read_only = !writable_;
  if (writable_) {
    kernel::ScopedRecursiveMutex lock(mutex_);
    result.free_bytes = (block_count_ - used_blocks_) << block_shift_;
  }
  return result;
}

Status BefsFileSystem::raw_read(uint64_t offset, ByteView out) {
  if (offset > volume_bytes_ || out.size() > volume_bytes_ - offset) return Status::Corrupt;
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

// Reads through the open transaction: blocks it has changed are taken from it.
Status BefsFileSystem::read_bytes(uint64_t offset, ByteView out) {
  const Status s = raw_read(offset, out);
  if (s != Status::Ok || tx_count_ == 0) return s;
  for (uint32_t i = 0; i < tx_count_; ++i) {
    const uint64_t begin = tx_[i].block << block_shift_, end = begin + block_size_;
    const uint64_t lo = begin > offset ? begin : offset;
    const uint64_t hi = end < offset + out.size() ? end : offset + out.size();
    if (lo < hi) out.subview(lo - offset, hi - lo).copy_from(ConstByteView(tx_[i].data).subview(lo - begin, hi - lo));
  }
  return Status::Ok;
}

// Writes to the medium, a device block at a time (the ones only partly covered are read first).
Status BefsFileSystem::raw_write(uint64_t offset, ConstByteView data) {
  if (offset > volume_bytes_ || data.size() > volume_bytes_ - offset) return Status::Corrupt;
  uint64_t absolute = first_block_ * device_block_size_ + offset;
  uint64_t done = 0;
  block_number_ = ~uint64_t{0};
  while (done < data.size()) {
    const uint64_t block = absolute / device_block_size_;
    const uint32_t within = static_cast<uint32_t>(absolute % device_block_size_);
    const uint64_t left = data.size() - done;
    if (within == 0 && left >= device_block_size_) {
      const uint64_t bytes = left / device_block_size_ * device_block_size_;
      if (!device_.write(block, data.subview(done, bytes))) return Status::IoError;
      done += bytes;
      absolute += bytes;
      continue;
    }
    if (!device_.read(block, block_)) return Status::IoError;
    uint64_t take = device_block_size_ - within;
    if (take > left) take = left;
    block_.subview(within, take).copy_from(data.subview(done, take));
    if (!device_.write(block, block_)) return Status::IoError;
    done += take;
    absolute += take;
  }
  return Status::Ok;
}

// The first block of `run`, if the whole run lies inside the volume.
bool BefsFileSystem::run_to_block(const Run& run, uint64_t* block) const {
  if (run.group >= group_count_) return false;
  const uint64_t first = (uint64_t{run.group} << group_shift_) + run.start;
  if (first >= block_count_ || run.length > block_count_ - first) return false;
  *block = first;
  return true;
}

Status BefsFileSystem::read_inode(uint64_t block, Inode* inode) {
  if (block >= block_count_) return Status::Corrupt;
  uint8_t raw[kInodeBytes];
  Status s = read_bytes(block << block_shift_, ByteView(raw));
  if (s != Status::Ok) return s;
  ByteReader in{ConstByteView(raw)};
  if (in.le32() != kInodeMagic) return Status::Corrupt;
  uint32_t group = 0;
  uint16_t start = 0, length = 0;
  read_run(&in, &group, &start, &length);
  uint64_t self = 0;
  if (!run_to_block(Run{group, start, length}, &self) || self != block) return Status::Corrupt;
  in.skip(8);  // owner
  Inode result;
  result.block = block;
  result.mode = in.le32();
  result.flags = in.le32();
  if (!in.ok() || !(result.flags & kInodeInUse)) return Status::Corrupt;
  in.seek(72);  // the data stream, or a short link
  if ((result.mode & kTypeMask) == kTypeLink && !(result.flags & kLongSymlink)) {
    ConstByteView text = in.take(kInlineLink);
    if (!in.ok()) return Status::Corrupt;
    uint32_t length_of_text = 0;
    while (length_of_text < kInlineLink && text[length_of_text] != 0) ++length_of_text;
    ByteView(result.link).copy_from(text);
    result.link_size = length_of_text;
    result.inline_link = true;
    in.seek(72 + 144);
  } else {
    for (Run& run : result.direct) {
      read_run(&in, &run.group, &run.start, &run.length);
    }
    result.max_direct = in.le64();
    read_run(&in, &result.indirect.group, &result.indirect.start, &result.indirect.length);
    result.max_indirect = in.le64();
    read_run(&in, &result.double_indirect.group, &result.double_indirect.start, &result.double_indirect.length);
    result.max_double = in.le64();
    result.size = in.le64();
    if (!in.ok() || result.size > (uint64_t{1} << 62)) return Status::Corrupt;
  }
  *inode = result;
  return Status::Ok;
}

// One run of a file's block table: either the one holding file block `seek->want`, or part of what lies before it.
Status BefsFileSystem::consider(Seek* seek, const Run& run) {
  if (run.length == 0) {
    seek->ended = true;
    return Status::Ok;
  }
  uint64_t first = 0;
  if (!run_to_block(run, &first)) return Status::Corrupt;
  const uint64_t into = seek->want - seek->skipped;
  if (into < run.length) {
    seek->found = true;
    seek->block = first + into;
    seek->left = run.length - into;
    return Status::Ok;
  }
  seek->skipped += run.length;
  return Status::Ok;
}

// Goes through an array of block runs stored in the blocks of `array`. At depth 1 its entries are data runs, at depth 2 they are arrays
// of data runs. Each depth reads into its own half of scratch_.
Status BefsFileSystem::walk_array(Seek* seek, const Run& array, uint32_t depth) {
  uint64_t first = 0;
  if (!run_to_block(array, &first)) return Status::Corrupt;
  const ByteView level = scratch_.subview((depth - 1) * uint64_t{block_size_}, block_size_);
  for (uint32_t b = 0; b < array.length; ++b) {
    if (seek->budget == 0) return Status::Corrupt;
    --seek->budget;
    Status s = read_bytes((first + b) << block_shift_, level);
    if (s != Status::Ok) return s;
    for (uint64_t at = 0; at + 8 <= block_size_; at += 8) {
      ByteReader in(ConstByteView(level).subview(at, 8));
      Run run;
      read_run(&in, &run.group, &run.start, &run.length);
      if (depth == 1) {
        s = consider(seek, run);
      } else if (run.length == 0) {
        seek->ended = true;
      } else {
        s = walk_array(seek, run, depth - 1);
      }
      if (s != Status::Ok) return s;
      if (seek->found || seek->ended) return Status::Ok;
    }
  }
  return Status::Ok;
}

// Finds where file block number `file_block` of the stream is: *block on the volume and *left, the blocks of the same run from there.
Status BefsFileSystem::locate(const Inode& inode, uint64_t file_block, uint64_t* block, uint64_t* left) {
  Seek seek;
  seek.want = file_block;
  seek.budget = kSeekBudget;
  Status s = Status::Ok;
  for (const Run& run : inode.direct) {
    s = consider(&seek, run);
    if (s != Status::Ok) return s;
    if (seek.found || seek.ended) break;
  }
  if (!seek.found && !seek.ended && inode.indirect.length) s = walk_array(&seek, inode.indirect, 1);
  if (s == Status::Ok && !seek.found && !seek.ended && inode.double_indirect.length) s = walk_array(&seek, inode.double_indirect, 2);
  if (s != Status::Ok) return s;
  if (!seek.found) return Status::Corrupt;  // the size claims more than the block table maps
  *block = seek.block;
  *left = seek.left;
  return Status::Ok;
}

// Reads exactly out.size() bytes of the stream at `offset`; the caller keeps the range inside the stream.
Status BefsFileSystem::read_stream(const Inode& inode, uint64_t offset, ByteView out) {
  if (offset > inode.size || out.size() > inode.size - offset) return Status::Corrupt;
  uint64_t done = 0;
  while (done < out.size()) {
    const uint64_t at = offset + done;
    uint64_t block = 0, left = 0;
    const Status s = locate(inode, at >> block_shift_, &block, &left);
    if (s != Status::Ok) return s;
    const uint64_t within = at & (block_size_ - 1);
    uint64_t chunk = (left << block_shift_) - within;
    if (chunk > out.size() - done) chunk = out.size() - done;
    const Status r = read_bytes((block << block_shift_) + within, out.subview(done, chunk));
    if (r != Status::Ok) return r;
    done += chunk;
  }
  return Status::Ok;
}

Status BefsFileSystem::open_tree(const Inode& directory, Tree* tree) {
  uint8_t raw[kTreeHeaderSize];
  if (directory.size < kTreeHeaderSize) return Status::Corrupt;
  const Status s = read_stream(directory, 0, ByteView(raw));
  if (s != Status::Ok) return s;
  ByteReader in{ConstByteView(raw)};
  if (in.le32() != kTreeMagic) return Status::Corrupt;
  const uint32_t node_size = in.le32();
  in.skip(8);  // levels, data type
  const uint64_t root = in.le64();
  if (!in.ok() || node_size < kMinNodeSize || node_size > kMaxNodeSize || node_size % 8 != 0) return Status::Corrupt;
  tree->node_size = node_size;
  tree->root = root;
  return Status::Ok;
}

// Reads the node at stream offset `at` into `buffer` (node_size bytes) and checks that its tables lie inside it.
Status BefsFileSystem::read_node(const Inode& directory, const Tree& tree, uint64_t at, ByteView buffer, Node* node) {
  if (buffer.size() < tree.node_size) return Status::InvalidArgument;
  const ByteView data = buffer.first(tree.node_size);
  const Status s = read_stream(directory, at, data);
  if (s != Status::Ok) return s;
  ByteReader in{ConstByteView(data)};
  in.skip(8);
  Node result;
  result.right = in.le64();
  result.overflow = in.le64();
  result.count = in.le16();
  result.keys_size = in.le16();
  if (!in.ok()) return Status::Corrupt;
  const uint64_t ends_at = (uint64_t{kNodeHeaderSize} + result.keys_size + 7) & ~uint64_t{7};
  const uint64_t values_at = ends_at + uint64_t{result.count} * 2;
  if (values_at + uint64_t{result.count} * 8 > tree.node_size) return Status::Corrupt;
  result.data = ConstByteView(data);
  result.ends_at = static_cast<uint32_t>(ends_at);
  result.values_at = static_cast<uint32_t>(values_at);
  *node = result;
  return Status::Ok;
}

bool BefsFileSystem::node_key(const Node& node, uint32_t index, ConstByteView* key) {
  if (index >= node.count) return false;
  uint64_t end = 0, begin = 0;
  if (!field(node.data, node.ends_at + uint64_t{index} * 2, 2, &end)) return false;
  if (index > 0 && !field(node.data, node.ends_at + uint64_t{index - 1} * 2, 2, &begin)) return false;
  if (begin > end || end > node.keys_size) return false;
  return node.data.try_subview(kNodeHeaderSize + begin, end - begin, key);
}

bool BefsFileSystem::node_value(const Node& node, uint32_t index, uint64_t* value) {
  return index < node.count && field(node.data, node.values_at + uint64_t{index} * 8, 8, value);
}

// The inode block of entry `name` in `directory`.
Status BefsFileSystem::find(const Inode& directory, StringView name, uint64_t* inode_block) {
  Tree tree;
  Status s = open_tree(directory, &tree);
  if (s != Status::Ok) return s;
  Owned buffer(allocator_, tree.node_size);
  if (!buffer.ok()) return Status::OutOfMemory;
  uint64_t at = tree.root;
  for (uint32_t depth = 0; depth < kMaxDepth; ++depth) {
    Node node;
    s = read_node(directory, tree, at, buffer.view(), &node);
    if (s != Status::Ok) return s;
    if (node.leaf()) {
      for (uint32_t i = 0; i < node.count; ++i) {
        ConstByteView key;
        if (!node_key(node, i, &key)) return Status::Corrupt;
        const int order = compare_names(key, name);
        if (order == 0) return node_value(node, i, inode_block) ? Status::Ok : Status::Corrupt;
        if (order > 0) break;
      }
      return Status::NotFound;
    }
    // Each key of an inner node is the largest of its child; names above all of them are in the overflow child.
    uint64_t child = node.overflow;
    for (uint32_t i = 0; i < node.count; ++i) {
      ConstByteView key;
      if (!node_key(node, i, &key)) return Status::Corrupt;
      if (compare_names(key, name) >= 0) {
        if (!node_value(node, i, &child)) return Status::Corrupt;
        break;
      }
    }
    at = child;
  }
  return Status::Corrupt;
}

Status BefsFileSystem::resolve(StringView path, Inode* inode, StringView* leaf) {
  Status s = read_inode(root_block_, inode);
  if (s != Status::Ok) return s;
  *leaf = StringView();
  StringView rest = path, component;
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    if (!valid_name(component)) return Status::InvalidPath;
    if (!inode->is_directory()) return Status::NotADirectory;
    uint64_t block = 0;
    s = find(*inode, component, &block);
    if (s != Status::Ok) return s;
    s = read_inode(block, inode);
    if (s != Status::Ok) return s;
    *leaf = component;
  }
  return Status::Ok;
}

void BefsFileSystem::fill_entry(const Inode& inode, StringView name, DirectoryEntry* entry) {
  entry->name.assign(name);
  entry->type = inode.is_directory() ? NodeType::Directory : NodeType::File;
  entry->size = inode.is_directory() ? 0 : inode.inline_link ? inode.link_size : inode.is_file() ? inode.size : 0;
  entry->read_only = (inode.mode & 0222) == 0;
}

Status BefsFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Inode inode;
  StringView leaf;
  const Status s = resolve(path, &inode, &leaf);
  if (s != Status::Ok) return s == Status::NotFound && mode != OpenMode::Read ? (writable_ ? Status::Unsupported : Status::ReadOnly) : s;
  if (inode.is_directory()) return Status::IsADirectory;
  if (!inode.is_file()) return Status::Unsupported;
  if (mode != OpenMode::Read) {
    if (!writable_ || (inode.mode & 0222) == 0) return Status::ReadOnly;
    if (!inode.is_regular()) return Status::Unsupported;
  }
  UniquePointer<IFile> opened = UniquePointer<IFile>::make_as<File>(allocator_, *this, inode.block, mode != OpenMode::Read);
  if (opened && mode == OpenMode::CreateTruncate) {
    const Status s = opened->resize(0);
    if (s != Status::Ok) return s;
  }
  if (!opened) return Status::OutOfMemory;
  *file = util::move(opened);
  return Status::Ok;
}

Status BefsFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Inode inode;
  StringView leaf;
  Status s = resolve(path, &inode, &leaf);
  if (s != Status::Ok) return s;
  if (!inode.is_directory()) return Status::NotADirectory;
  Tree tree;
  s = open_tree(inode, &tree);
  if (s != Status::Ok) return s;
  ByteView buffer = allocate_view<uint8_t>(allocator_, tree.node_size);
  if (buffer.empty()) return Status::OutOfMemory;
  UniquePointer<IDirectory> opened = UniquePointer<IDirectory>::make_as<Directory>(allocator_, *this, inode, tree, buffer);
  if (!opened) {
    release_view(allocator_, buffer);
    return Status::OutOfMemory;
  }
  *directory = util::move(opened);
  return Status::Ok;
}

Status BefsFileSystem::stat(StringView path, DirectoryEntry* entry) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Inode inode;
  StringView leaf;
  const Status s = resolve(path, &inode, &leaf);
  if (s != Status::Ok) return s;
  fill_entry(inode, leaf, entry);
  return Status::Ok;
}


// ---- Transactions -----------------------------------------------------------------------------------------------------------------

uint32_t BefsFileSystem::tx_soft_limit() { return kTxSoft; }

void BefsFileSystem::tx_begin() { tx_used_blocks_ = used_blocks_; }

// Puts `data` into the transaction (whole blocks, each loaded from the medium the first time it is touched).
Status BefsFileSystem::tx_write(uint64_t offset, ConstByteView data) {
  if (!writable_) return Status::ReadOnly;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t at = offset + done;
    const uint64_t block = at >> block_shift_;
    const uint64_t within = at & (block_size_ - 1);
    uint64_t chunk = block_size_ - within;
    if (chunk > data.size() - done) chunk = data.size() - done;
    if (block >= block_count_) return Status::Corrupt;
    TxBlock* entry = nullptr;
    for (uint32_t i = 0; i < tx_count_; ++i) {
      if (tx_[i].block == block) {
        entry = &tx_[i];
        break;
      }
    }
    if (!entry) {
      if (tx_count_ == kTxHard) return Status::NoSpace;
      ByteView copy = allocate_view<uint8_t>(allocator_, block_size_);
      if (copy.empty()) return Status::OutOfMemory;
      const Status s = raw_read(block << block_shift_, copy);
      if (s != Status::Ok) {
        release_view(allocator_, copy);
        return s;
      }
      tx_[tx_count_] = TxBlock{block, copy};
      entry = &tx_[tx_count_++];
    }
    entry->data.subview(within, chunk).copy_from(data.subview(done, chunk));
    done += chunk;
  }
  return Status::Ok;
}

Status BefsFileSystem::tx_zero(uint64_t block, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const Status s = tx_write((block + i) << block_shift_, ConstByteView(zeros_));
    if (s != Status::Ok) return s;
  }
  return Status::Ok;
}

// Writes the changed blocks (and the used-blocks count of the super block) to the medium, lowest block first.
Status BefsFileSystem::tx_commit() {
  if (tx_count_ == 0 && used_blocks_ == tx_used_blocks_) return Status::Ok;
  if (used_blocks_ != tx_used_blocks_) {
    uint8_t raw[8];
    ByteWriter out{ByteView(raw)};
    out.le64(used_blocks_);
    const Status s = tx_write(kSuperOffset + 56, ConstByteView(raw));
    if (s != Status::Ok) return s;  // the caller drops the transaction
  }
  for (uint32_t i = 1; i < tx_count_; ++i) {  // insertion sort: there are few
    const TxBlock moved = tx_[i];
    uint32_t j = i;
    for (; j > 0 && tx_[j - 1].block > moved.block; --j) tx_[j] = tx_[j - 1];
    tx_[j] = moved;
  }
  Status result = Status::Ok;
  for (uint32_t i = 0; i < tx_count_ && result == Status::Ok; ++i) result = raw_write(tx_[i].block << block_shift_, ConstByteView(tx_[i].data));
  if (result != Status::Ok) {
    tx_rollback();
    return result;
  }
  for (uint32_t i = 0; i < tx_count_; ++i) release_view(allocator_, tx_[i].data);
  tx_count_ = 0;
  tx_used_blocks_ = used_blocks_;
  return Status::Ok;
}

// Drops everything the transaction changed.
void BefsFileSystem::tx_rollback() {
  if (!writable_) return;
  for (uint32_t i = 0; i < tx_count_; ++i) release_view(allocator_, tx_[i].data);
  tx_count_ = 0;
  used_blocks_ = tx_used_blocks_;
  bitmap_loaded_ = ~uint64_t{0};
}

Status BefsFileSystem::checkpoint(const Inode& inode) {
  Status s = store_inode(inode);
  if (s == Status::Ok) s = tx_commit();
  if (s != Status::Ok) return s;
  tx_begin();
  return Status::Ok;
}

// ---- The allocation bitmap --------------------------------------------------------------------------------------------------------
// Group g's bitmap starts at block 1 + g * bitmap_per_group_; bit n of it (least significant bit of each byte first) is block
// (g << group_shift_) + n. A set bit is a block in use.

uint32_t BefsFileSystem::group_bits(uint32_t group) const {
  const uint64_t start = uint64_t{group} << group_shift_;
  if (start >= block_count_) return 0;
  const uint64_t left = block_count_ - start;
  return static_cast<uint32_t>(left < (uint64_t{1} << group_shift_) ? left : uint64_t{1} << group_shift_);
}

Status BefsFileSystem::load_bitmap(uint64_t bitmap_block) {
  if (bitmap_loaded_ == bitmap_block) return Status::Ok;
  bitmap_loaded_ = ~uint64_t{0};
  if (bitmap_block >= block_count_) return Status::Corrupt;
  const Status s = read_bytes(bitmap_block << block_shift_, bitmap_);
  if (s == Status::Ok) bitmap_loaded_ = bitmap_block;
  return s;
}

// The blocks that hold the super block and the bitmap itself must be marked in use, or this is not a bitmap we know.
Status BefsFileSystem::check_bitmap() {
  const uint32_t block_bits = block_size_ << 3;
  const uint32_t last_group = static_cast<uint32_t>((block_count_ - 1) >> group_shift_);
  if (last_group >= group_count_) return Status::Corrupt;
  const uint64_t reserved = 1 + uint64_t{last_group} * bitmap_per_group_ + (group_bits(last_group) + block_bits - 1) / block_bits;
  if (reserved >= block_count_) return Status::Corrupt;
  for (uint64_t block = 0; block < reserved; ++block) {
    const uint32_t group = static_cast<uint32_t>(block >> group_shift_);
    const uint32_t bit = static_cast<uint32_t>(block & ((uint64_t{1} << group_shift_) - 1));
    const Status s = load_bitmap(1 + uint64_t{group} * bitmap_per_group_ + bit / block_bits);
    if (s != Status::Ok) return s;
    const uint32_t in_block = bit % block_bits;
    if (!(bitmap_[in_block >> 3] & (1u << (in_block & 7)))) return Status::Corrupt;
  }
  return Status::Ok;
}

// How many blocks in a row are free from bit `from` of `group` on (at most `max`, and none at or after bit `limit`).
Status BefsFileSystem::clear_run(uint32_t group, uint32_t from, uint32_t limit, uint32_t max, uint32_t* count) {
  const uint32_t block_bits = block_size_ << 3;
  uint32_t n = 0, pos = from;
  while (n < max && pos < limit) {
    const Status s = load_bitmap(1 + uint64_t{group} * bitmap_per_group_ + pos / block_bits);
    if (s != Status::Ok) return s;
    const uint32_t in_block = pos % block_bits;
    const uint8_t byte = bitmap_[in_block >> 3];
    if ((in_block & 7) == 0 && byte == 0 && max - n >= 8 && limit - pos >= 8) {
      n += 8;
      pos += 8;
      continue;
    }
    if (byte & (1u << (in_block & 7))) break;
    ++n;
    ++pos;
  }
  *count = n;
  return Status::Ok;
}

// The first run of at least `min` free blocks in `group` (at most `want` of them).
Status BefsFileSystem::find_free(uint32_t group, uint32_t limit, uint32_t want, uint32_t min, uint32_t* start, uint32_t* length) {
  const uint32_t block_bits = block_size_ << 3;
  uint32_t pos = 0;
  while (pos < limit) {
    const Status s = load_bitmap(1 + uint64_t{group} * bitmap_per_group_ + pos / block_bits);
    if (s != Status::Ok) return s;
    const uint32_t in_block = pos % block_bits;
    const uint8_t byte = bitmap_[in_block >> 3];
    if ((in_block & 7) == 0 && byte == 0xFF) {
      pos += 8;
      continue;
    }
    if (byte & (1u << (in_block & 7))) {
      ++pos;
      continue;
    }
    uint32_t n = 0;
    const Status r = clear_run(group, pos, limit, want, &n);
    if (r != Status::Ok) return r;
    if (n >= min) {
      *start = pos;
      *length = n;
      return Status::Ok;
    }
    pos += n;
  }
  return Status::NoSpace;
}

// Marks `count` blocks (all in one group) used or free. Marking a block that already is so means the bitmap is wrong.
Status BefsFileSystem::set_bits(uint64_t first, uint32_t count, bool used) {
  if (count == 0) return Status::Ok;
  if (first >= block_count_ || count > block_count_ - first || (first >> group_shift_) != ((first + count - 1) >> group_shift_)) return Status::Corrupt;
  const uint32_t block_bits = block_size_ << 3;
  const uint64_t group = first >> group_shift_;
  const uint32_t bit = static_cast<uint32_t>(first & ((uint64_t{1} << group_shift_) - 1));
  uint32_t done = 0;
  while (done < count) {
    const uint32_t pos = bit + done;
    const uint64_t bitmap_block = 1 + group * bitmap_per_group_ + pos / block_bits;
    const uint32_t in_block = pos % block_bits;
    uint32_t n = block_bits - in_block;
    if (n > count - done) n = count - done;
    Status s = load_bitmap(bitmap_block);
    if (s != Status::Ok) return s;
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t p = in_block + k;
      uint8_t& byte = bitmap_[p >> 3];
      const uint8_t mask = static_cast<uint8_t>(1u << (p & 7));
      if (((byte & mask) != 0) == used) {
        bitmap_loaded_ = ~uint64_t{0};
        return Status::Corrupt;
      }
      byte ^= mask;
    }
    s = tx_write(bitmap_block << block_shift_, ConstByteView(bitmap_));
    if (s != Status::Ok) {
      bitmap_loaded_ = ~uint64_t{0};
      return s;
    }
    done += n;
  }
  if (used) {
    used_blocks_ += count;
  } else {
    used_blocks_ = count > used_blocks_ ? 0 : used_blocks_ - count;
  }
  return Status::Ok;
}

// Takes the free blocks that directly follow `first` (up to `want`); *got is 0 if the block at `first` is not free.
Status BefsFileSystem::allocate_at(uint64_t first, uint32_t want, uint32_t* got) {
  *got = 0;
  if (first >= block_count_) return Status::Ok;
  const uint32_t group = static_cast<uint32_t>(first >> group_shift_);
  const uint32_t from = static_cast<uint32_t>(first & ((uint64_t{1} << group_shift_) - 1));
  uint32_t n = 0;
  Status s = clear_run(group, from, group_bits(group), want, &n);
  if (s != Status::Ok || n == 0) return s;
  s = set_bits(first, n, true);
  if (s == Status::Ok) *got = n;
  return s;
}

// Takes the first run of `min` to `want` free blocks, looking in the group of `hint` first.
Status BefsFileSystem::allocate_any(uint64_t hint, uint32_t want, uint32_t min, Run* run) {
  if (want > kMaxRunBlocks) want = kMaxRunBlocks;
  uint32_t first_group = static_cast<uint32_t>(hint >> group_shift_);
  if (first_group >= group_count_) first_group = 0;
  for (uint32_t i = 0; i < group_count_; ++i) {
    const uint32_t group = static_cast<uint32_t>((uint64_t{first_group} + i) % group_count_);
    const uint32_t limit = group_bits(group);
    if (limit == 0) continue;
    uint32_t start = 0, length = 0;
    Status s = find_free(group, limit, want, min, &start, &length);
    if (s == Status::NoSpace) continue;
    if (s != Status::Ok) return s;
    s = set_bits((uint64_t{group} << group_shift_) + start, length, true);
    if (s != Status::Ok) return s;
    *run = Run{group, static_cast<uint16_t>(start), static_cast<uint16_t>(length)};
    return Status::Ok;
  }
  return Status::NoSpace;
}

Status BefsFileSystem::free_run(const Run& run) {
  uint64_t first = 0;
  if (!run_to_block(run, &first)) return Status::Corrupt;
  return set_bits(first, run.length, false);
}

void BefsFileSystem::encode_run(const Run& run, uint8_t out[8]) {
  ByteWriter w{ByteView(out, 8)};
  w.le32(run.group);
  w.le16(run.start);
  w.le16(run.length);
}

// ---- Block run tables of a stream ---------------------------------------------------------------------------------------------------
// A stream maps its blocks with 12 direct runs, then the runs in the indirect array (a run of blocks full of 8-byte runs), then the
// double indirect array: a run of blocks full of runs, each of which is a level-2 array of data runs. A run of length 0 ends a table.
// The arrays we create are 4 blocks long, and the data runs of the double indirect area exactly 4 blocks, as Be's file system has them.

// Writes the stream part of the inode (runs, ranges, size) into the transaction.
Status BefsFileSystem::store_inode(const Inode& inode) {
  uint8_t raw[144];
  ByteWriter out{ByteView(raw)};
  for (const Run& run : inode.direct) {
    out.le32(run.group);
    out.le16(run.start);
    out.le16(run.length);
  }
  out.le64(inode.max_direct);
  out.le32(inode.indirect.group);
  out.le16(inode.indirect.start);
  out.le16(inode.indirect.length);
  out.le64(inode.max_indirect);
  out.le32(inode.double_indirect.group);
  out.le16(inode.double_indirect.start);
  out.le16(inode.double_indirect.length);
  out.le64(inode.max_double);
  out.le64(inode.size);
  if (!out.ok() || out.position() != sizeof raw) return Status::Corrupt;
  return tx_write((inode.block << block_shift_) + 72, ConstByteView(raw));
}

Status BefsFileSystem::read_entry(const Run& array, uint32_t index, Run* out) {
  uint64_t first = 0;
  if (!run_to_block(array, &first) || index >= array.length * (block_size_ / 8)) return Status::Corrupt;
  uint8_t raw[8];
  const Status s = read_bytes((first << block_shift_) + uint64_t{index} * 8, ByteView(raw));
  if (s != Status::Ok) return s;
  ByteReader in{ConstByteView(raw)};
  return read_run(&in, &out->group, &out->start, &out->length);
}

Status BefsFileSystem::write_entry(const Run& array, uint32_t index, const Run& run) {
  uint64_t first = 0;
  if (!run_to_block(array, &first) || index >= array.length * (block_size_ / 8)) return Status::Corrupt;
  uint8_t raw[8];
  encode_run(run, raw);
  return tx_write((first << block_shift_) + uint64_t{index} * 8, ConstByteView(raw));
}

// How many runs an array holds (those before its first run of length 0).
Status BefsFileSystem::count_entries(const Run& array, uint32_t* count) {
  uint64_t first = 0;
  if (!run_to_block(array, &first)) return Status::Corrupt;
  const uint32_t per = block_size_ / 8;
  const ByteView level = scratch_.first(block_size_);
  for (uint32_t b = 0; b < array.length; ++b) {
    const Status s = read_bytes((first + b) << block_shift_, level);
    if (s != Status::Ok) return s;
    for (uint32_t e = 0; e < per; ++e) {
      uint64_t length = 0;
      if (!field(ConstByteView(level), uint64_t{e} * 8 + 6, 2, &length)) return Status::Corrupt;
      if (length == 0) {
        *count = b * per + e;
        return Status::Ok;
      }
    }
  }
  *count = array.length * per;
  return Status::Ok;
}

// Finds the end of the run table: the last run there is, and the slot for the next one (and which arrays that needs created).
Status BefsFileSystem::find_tail(const Inode& inode, Tail* tail) {
  const uint32_t per = block_size_ / 8;
  Tail t;
  uint32_t direct = 0;
  while (direct < 12 && inode.direct[direct].length) ++direct;
  if (direct < 12) {
    t.next.index = direct;
    if (direct > 0) {
      t.has_last = true;
      t.last.index = direct - 1;
      t.last_run = inode.direct[direct - 1];
    }
    *tail = t;
    return Status::Ok;
  }
  t.has_last = true;
  t.last.index = 11;
  t.last_run = inode.direct[11];
  if (inode.indirect.length == 0) {
    t.next.level = 1;
    t.need_indirect = true;
    *tail = t;
    return Status::Ok;
  }
  uint32_t used = 0;
  Status s = count_entries(inode.indirect, &used);
  if (s != Status::Ok) return s;
  if (used > 0) {
    t.last.level = 1;
    t.last.index = used - 1;
    s = read_entry(inode.indirect, used - 1, &t.last_run);
    if (s != Status::Ok) return s;
  }
  if (used < inode.indirect.length * per) {
    t.next.level = 1;
    t.next.index = used;
    *tail = t;
    return Status::Ok;
  }
  t.next.level = 2;
  if (inode.double_indirect.length == 0) {
    t.need_top = t.need_level2 = true;
    *tail = t;
    return Status::Ok;
  }
  uint32_t tops = 0;
  s = count_entries(inode.double_indirect, &tops);
  if (s != Status::Ok) return s;
  if (tops == 0) {
    t.need_level2 = true;
    *tail = t;
    return Status::Ok;
  }
  Run array;
  s = read_entry(inode.double_indirect, tops - 1, &array);
  if (s != Status::Ok) return s;
  uint32_t entries = 0;
  s = count_entries(array, &entries);
  if (s != Status::Ok) return s;
  if (entries > 0) {
    t.last.level = 2;
    t.last.index = entries - 1;
    t.last.top_index = tops - 1;
    t.last.array = array;
    s = read_entry(array, entries - 1, &t.last_run);
    if (s != Status::Ok) return s;
  }
  if (entries < array.length * per) {
    t.next.index = entries;
    t.next.top_index = tops - 1;
    t.next.array = array;
  } else if (tops < inode.double_indirect.length * per) {
    t.next.top_index = tops;
    t.need_level2 = true;
  } else {
    t.full = true;
  }
  *tail = t;
  return Status::Ok;
}

Status BefsFileSystem::set_slot(Inode* inode, const Slot& slot, const Run& run) {
  if (slot.level == 0) {
    if (slot.index >= 12) return Status::Corrupt;
    inode->direct[slot.index] = run;
    return Status::Ok;
  }
  return write_entry(slot.level == 1 ? inode->indirect : slot.array, slot.index, run);
}

uint64_t BefsFileSystem::mapped_blocks(const Inode& inode, uint32_t shift) {
  const uint64_t bytes = inode.max_double ? inode.max_double : inode.max_indirect ? inode.max_indirect : inode.max_direct;
  return bytes >> shift;
}

// The ranges count from the start of the file: each area's end, once the area is used.
void BefsFileSystem::adjust_ranges(Inode* inode, uint8_t level, int64_t delta) {
  const uint64_t change = static_cast<uint64_t>(delta);  // (wraps for a negative delta, as intended)
  if (level == 0) {
    inode->max_direct += change;
    if (inode->max_indirect) inode->max_indirect += change;
    if (inode->max_double) inode->max_double += change;
  } else if (level == 1) {
    if (inode->max_indirect == 0) inode->max_indirect = inode->max_direct;
    inode->max_indirect += change;
    if (inode->max_double) inode->max_double += change;
  } else {
    if (inode->max_double == 0) inode->max_double = inode->max_indirect ? inode->max_indirect : inode->max_direct;
    inode->max_double += change;
  }
}

// Maps at least `blocks` blocks (more only because the double indirect area comes in runs of 4). Each step leaves the volume consistent,
// so a long grow commits as it goes.
Status BefsFileSystem::grow_stream(Inode* inode, uint64_t blocks) {
  for (;;) {
    const uint64_t have = mapped_blocks(*inode, block_shift_);
    if (have >= blocks) return Status::Ok;
    const uint64_t need = blocks - have;
    Status s = Status::Ok;
    if (tx_full()) {
      s = checkpoint(*inode);
      if (s != Status::Ok) return s;
    }
    Tail tail;
    s = find_tail(*inode, &tail);
    if (s != Status::Ok) return s;
    if (tail.full) return Status::NoSpace;
    uint64_t hint = inode->block;
    if (tail.has_last) {
      uint64_t last_block = 0;
      if (!run_to_block(tail.last_run, &last_block)) return Status::Corrupt;
      hint = last_block;
      if (tail.last.level != 2 && tail.last_run.length < kMaxRunBlocks) {  // first try to make the last run longer
        const uint32_t room = kMaxRunBlocks - tail.last_run.length;
        uint32_t got = 0;
        s = allocate_at(last_block + tail.last_run.length, need < room ? static_cast<uint32_t>(need) : room, &got);
        if (s != Status::Ok) return s;
        if (got) {
          Run longer = tail.last_run;
          longer.length = static_cast<uint16_t>(longer.length + got);
          s = set_slot(inode, tail.last, longer);
          if (s != Status::Ok) return s;
          adjust_ranges(inode, tail.last.level, static_cast<int64_t>(uint64_t{got} << block_shift_));
          continue;
        }
      }
    }
    Run array;
    if (tail.need_indirect) {
      s = allocate_any(hint, kArrayBlocks, kArrayBlocks, &array);
      uint64_t first = 0;
      if (s == Status::Ok && run_to_block(array, &first)) s = tx_zero(first, array.length);
      if (s != Status::Ok) return s;
      inode->indirect = array;
    }
    if (tail.need_top) {
      s = allocate_any(hint, kArrayBlocks, kArrayBlocks, &array);
      uint64_t first = 0;
      if (s == Status::Ok && run_to_block(array, &first)) s = tx_zero(first, array.length);
      if (s != Status::Ok) return s;
      inode->double_indirect = array;
    }
    if (tail.need_level2) {
      s = allocate_any(hint, kArrayBlocks, kArrayBlocks, &array);
      uint64_t first = 0;
      if (s == Status::Ok && run_to_block(array, &first)) s = tx_zero(first, array.length);
      if (s == Status::Ok) s = write_entry(inode->double_indirect, tail.next.top_index, array);
      if (s != Status::Ok) return s;
      tail.next.array = array;
    }
    Run data;
    if (tail.next.level == 2) {
      s = allocate_any(hint, kArrayBlocks, kArrayBlocks, &data);
    } else {
      s = allocate_any(hint, need < kMaxRunBlocks ? static_cast<uint32_t>(need) : kMaxRunBlocks, 1, &data);
    }
    if (s != Status::Ok) return s;
    s = set_slot(inode, tail.next, data);
    if (s != Status::Ok) return s;
    adjust_ranges(inode, tail.next.level, static_cast<int64_t>(uint64_t{data.length} << block_shift_));
  }
}

// Gives back blocks from the end until at most `blocks` are mapped (a few more stay where the last run cannot be cut).
Status BefsFileSystem::shrink_stream(Inode* inode, uint64_t blocks) {
  for (;;) {
    const uint64_t have = mapped_blocks(*inode, block_shift_);
    if (have <= blocks) return Status::Ok;
    Status s = Status::Ok;
    if (tx_full()) {
      s = checkpoint(*inode);
      if (s != Status::Ok) return s;
    }
    Tail tail;
    s = find_tail(*inode, &tail);
    if (s != Status::Ok) return s;
    if (!tail.has_last) return Status::Corrupt;  // the ranges claim blocks the tables do not have
    const Run last = tail.last_run;
    const uint64_t excess = have - blocks;
    if (excess < last.length) {
      if (tail.last.level == 2) return Status::Ok;  // runs of the double indirect area stay whole
      Run cut{last.group, static_cast<uint16_t>(last.start + (last.length - excess)), static_cast<uint16_t>(excess)};
      s = free_run(cut);
      if (s != Status::Ok) return s;
      Run kept = last;
      kept.length = static_cast<uint16_t>(last.length - excess);
      s = set_slot(inode, tail.last, kept);
      if (s != Status::Ok) return s;
      adjust_ranges(inode, tail.last.level, -static_cast<int64_t>(excess << block_shift_));
      continue;
    }
    s = free_run(last);
    if (s != Status::Ok) return s;
    s = set_slot(inode, tail.last, Run());
    if (s != Status::Ok) return s;
    adjust_ranges(inode, tail.last.level, -static_cast<int64_t>(uint64_t{last.length} << block_shift_));
    if (tail.last.level == 1 && tail.last.index == 0) {  // the indirect array is empty now
      s = free_run(inode->indirect);
      if (s != Status::Ok) return s;
      inode->indirect = Run();
      inode->max_indirect = 0;
    } else if (tail.last.level == 2 && tail.last.index == 0) {
      s = free_run(tail.last.array);
      if (s == Status::Ok) s = write_entry(inode->double_indirect, tail.last.top_index, Run());
      if (s != Status::Ok) return s;
      if (tail.last.top_index == 0) {
        s = free_run(inode->double_indirect);
        if (s != Status::Ok) return s;
        inode->double_indirect = Run();
        inode->max_double = 0;
      }
    }
  }
}

// Writes into blocks the stream already maps.
Status BefsFileSystem::write_stream(const Inode& inode, uint64_t offset, ConstByteView data) {
  const uint64_t limit = mapped_blocks(inode, block_shift_) << block_shift_;
  if (offset > limit || data.size() > limit - offset) return Status::Corrupt;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t at = offset + done;
    uint64_t block = 0, left = 0;
    const Status s = locate(inode, at >> block_shift_, &block, &left);
    if (s != Status::Ok) return s;
    const uint64_t within = at & (block_size_ - 1);
    uint64_t chunk = (left << block_shift_) - within;
    if (chunk > data.size() - done) chunk = data.size() - done;
    const Status w = raw_write((block << block_shift_) + within, data.subview(done, chunk));
    if (w != Status::Ok) return w;
    done += chunk;
  }
  return Status::Ok;
}

// Writes zeros over bytes [from, to) of the stream.
Status BefsFileSystem::zero_stream(const Inode& inode, uint64_t from, uint64_t to) {
  const uint64_t limit = mapped_blocks(inode, block_shift_) << block_shift_;
  if (from > to || to > limit) return Status::Corrupt;
  uint64_t at = from;
  while (at < to) {
    uint64_t block = 0, left = 0;
    const Status s = locate(inode, at >> block_shift_, &block, &left);
    if (s != Status::Ok) return s;
    uint64_t within = at & (block_size_ - 1);
    uint64_t chunk = (left << block_shift_) - within;
    if (chunk > to - at) chunk = to - at;
    uint64_t done = 0;
    while (done < chunk) {
      const uint64_t piece = chunk - done < block_size_ ? chunk - done : block_size_;
      const Status w = raw_write((block << block_shift_) + within + done, ConstByteView(zeros_).first(piece));
      if (w != Status::Ok) return w;
      done += piece;
    }
    at += chunk;
  }
  return Status::Ok;
}

Status BefsFileSystem::create_directory(StringView) { return writable_ ? Status::Unsupported : Status::ReadOnly; }
Status BefsFileSystem::remove(StringView) { return writable_ ? Status::Unsupported : Status::ReadOnly; }
Status BefsFileSystem::rename(StringView, StringView) { return writable_ ? Status::Unsupported : Status::ReadOnly; }

Status BefsFileSystem::flush() {
  kernel::ScopedRecursiveMutex lock(mutex_);
  return writable_ && !device_.flush() ? Status::IoError : Status::Ok;
}

}  // namespace filesystem
