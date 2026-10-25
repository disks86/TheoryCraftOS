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
constexpr uint32_t kVolumeClean = 0x434c454e, kVolumeDirty = 0x44495254;
constexpr uint32_t kMinLogBlocks = 256;  // (a transaction holds up to kTxHard blocks, and its log entry has to fit)
constexpr uint32_t kInodeDeleted = 0x10;
constexpr uint32_t kTxHard = 192;           // most blocks one transaction holds
constexpr uint32_t kDataPieceBlocks = 64;     // most blocks of file data one write puts into one transaction
constexpr uint32_t kTxSoft = 96;            // a long operation commits what it has when it holds this many
constexpr uint32_t kArrayBlocks = 4;        // length of the arrays of block runs we create (as Be's file system does)
constexpr uint32_t kMaxRunBlocks = 0xFFFF;
constexpr uint32_t kTypeMask = 0xF000, kTypeLink = 0xA000;
constexpr uint64_t kSuperOffset = 512;
constexpr uint32_t kSuperSize = 132, kInodeBytes = 232, kInlineLink = 144, kTreeHeaderSize = 40, kNodeHeaderSize = 28;
constexpr uint32_t kMinNodeSize = 64, kMaxNodeSize = 1u << 16;
constexpr uint32_t kSeekBudget = 1u << 18;  // array blocks read for one lookup of a file block: stops loops in damaged run tables
constexpr uint32_t kMaxDepth = 64;          // B+tree levels followed before the tree is taken for a loop
constexpr uint64_t kNone = ~uint64_t{0};       // BPLUSTREE_NULL (-1)
constexpr uint64_t kFreeNode = ~uint64_t{1};   // BPLUSTREE_FREE (-2): marks a node on the free list
constexpr uint32_t kMaxWriteNodeSize = 1u << 15;  // the key length fields are 16 bits

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
  File(BefsFileSystem& fs, uint64_t inode_block, bool writable) : fs_(fs), inode_block_(inode_block), writable_(writable) {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    handle_.block = inode_block;
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
    const uint64_t wanted_blocks = (end + fs_.block_size_ - 1) >> fs_.block_shift_, mapped = mapped_blocks(inode, fs_.block_shift_);
    if (wanted_blocks > mapped && wanted_blocks - mapped > fs_.block_count_ - fs_.used_blocks_) return Status::NoSpace;  // (a write that cannot fit changes nothing)
    if (offset > inode.size) {  // a gap: the file first grows to where the data starts (zeros, with the new blocks)
      s = fs_.resize_file(inode_block_, offset);
      if (s != Status::Ok) return s;
    }
    // The data goes through the journal with the changes to the file's blocks, so a crash leaves each piece either old or new. A piece is
    // as many blocks as a transaction holds with room for the bookkeeping; if one turns out not to fit, the pieces get smaller.
    uint32_t piece_blocks = kDataPieceBlocks;
    uint64_t done = 0;
    while (done < data.size()) {
      const uint64_t position = offset + done;
      const uint64_t piece_end = ((position >> fs_.block_shift_) + piece_blocks) << fs_.block_shift_;
      const uint64_t length = piece_end - position < data.size() - done ? piece_end - position : data.size() - done;
      s = fs_.write_piece(inode_block_, position, data.subview(done, length));
      if (s == Status::NoSpace && piece_blocks > 1) {
        piece_blocks /= 2;
        continue;
      }
      if (s != Status::Ok) return s;  // (*transferred says how much of it did get written)
      done += length;
      *transferred = done;
    }
    return Status::Ok;
  }
  Status resize(uint64_t new_size) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::ReadOnly;
    Status s = fs_.writable();
    if (s != Status::Ok) return s;
    return fs_.resize_file(inode_block_, new_size);
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
  OpenHandle handle_;
};

// An open directory: walks the leaves of its B+tree from the left. If the tree is changed meanwhile it looks again, starting after the
// last name it gave out (entries may then be skipped or repeated, as IDirectory says).
class BefsFileSystem::Directory final : public IDirectory {
 public:
  Directory(BefsFileSystem& fs, const Inode& inode, const Tree& tree, ByteView buffer)
      : fs_(fs), inode_(inode), tree_(tree), buffer_(buffer) {
    handle_.block = inode.block;
    handle_.next = fs_.open_;
    fs_.open_ = &handle_;  // (the caller holds the lock)
  }
  ~Directory() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    for (OpenHandle** link = &fs_.open_; *link; link = &(*link)->next) {
      if (*link == &handle_) {
        *link = handle_.next;
        break;
      }
    }
    release_view(fs_.allocator_, buffer_);
  }

  Status next(DirectoryEntry* entry) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (ended_) return Status::EndOfDirectory;
    if (loaded_ && epoch_ != fs_.tree_epoch_) {
      const Status s = refresh();
      if (s != Status::Ok) return s;
    }
    if (!loaded_) {
      epoch_ = fs_.tree_epoch_;
      const Status s = descend();
      if (s != Status::Ok) return s;
      loaded_ = true;
    }
    for (;;) {
      const uint64_t max_nodes = inode_.size / tree_.node_size + 1;
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
        last_ = name;
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
    last_.clear();
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
  // The tree changed: read the directory's inode and tree again and go on after the last name given out.
  Status refresh() {
    Status s = fs_.read_inode(inode_.block, &inode_);
    if (s != Status::Ok) return s;
    if (!inode_.is_directory()) return Status::Corrupt;
    Tree tree;
    s = fs_.open_tree(inode_, &tree);
    if (s != Status::Ok) return s;
    if (tree.node_size != tree_.node_size) return Status::Corrupt;
    tree_ = tree;
    epoch_ = fs_.tree_epoch_;
    hops_ = 0;
    if (last_.empty()) {
      loaded_ = false;
      return Status::Ok;
    }
    return fs_.seek_after(inode_, tree_, last_.view(), buffer_, &node_, &index_);
  }

  BefsFileSystem& fs_;
  Inode inode_;
  Tree tree_;
  ByteView buffer_;
  Node node_;
  OpenHandle handle_;
  FixedString<kMaxNameLength + 1> last_;
  uint32_t index_ = 0;
  uint64_t hops_ = 0, epoch_ = 0;
  bool loaded_ = false, ended_ = false;
};

// A node of a directory tree taken apart for changing: its keys, their values and the links. Whole nodes are read into it and written
// out from it, so inserting and removing are plain array operations; fits() says whether it can be written back.
class BefsFileSystem::NodeEdit {
 public:
  NodeEdit(allocators::IAllocator& allocator, uint32_t node_size)
      : allocator_(allocator),
        node_size_(node_size),
        capacity_(node_size / 10 + 2),
        keys_(allocate_view<uint8_t>(allocator, uint64_t{node_size} + 256)),
        ends_(allocate_view<uint32_t>(allocator, node_size / 10 + 2)),
        values_(allocate_view<uint64_t>(allocator, node_size / 10 + 2)) {}
  ~NodeEdit() {
    release_view(allocator_, keys_);
    release_view(allocator_, ends_);
    release_view(allocator_, values_);
  }
  NodeEdit(const NodeEdit&) = delete;
  NodeEdit& operator=(const NodeEdit&) = delete;

  bool ok() const { return !keys_.empty() && !ends_.empty() && !values_.empty(); }
  uint32_t count() const { return count_; }
  bool leaf() const { return overflow == kNone; }
  void reset_leaf() {
    left = right = overflow = kNone;
    count_ = keys_size_ = 0;
  }
  ConstByteView key(uint32_t i) const { return ConstByteView(keys_).subview(i ? ends_[i - 1] : 0, ends_[i] - (i ? ends_[i - 1] : 0)); }
  uint64_t value(uint32_t i) const { return values_[i]; }
  void set_value(uint32_t i, uint64_t v) { values_[i] = v; }
  uint32_t entry_bytes(uint32_t i) const { return static_cast<uint32_t>(key(i).size()) + 10; }
  uint64_t total_bytes() const { return uint64_t{keys_size_} + uint64_t{count_} * 10; }
  bool fits() const { return ((uint64_t{kNodeHeaderSize} + keys_size_ + 7) & ~uint64_t{7}) + uint64_t{count_} * 10 <= node_size_; }

  Status load(ConstByteView node) {
    uint64_t c = 0, k = 0;
    if (!field(node, 0, 8, &left) || !field(node, 8, 8, &right) || !field(node, 16, 8, &overflow) || !field(node, 24, 2, &c) ||
        !field(node, 26, 2, &k))
      return Status::Corrupt;
    const uint64_t ends_at = (uint64_t{kNodeHeaderSize} + k + 7) & ~uint64_t{7};
    const uint64_t values_at = ends_at + c * 2;
    if (c > capacity_ || k > keys_.size() || values_at + c * 8 > node.size()) return Status::Corrupt;
    uint64_t previous = 0;
    for (uint32_t i = 0; i < c; ++i) {
      uint64_t end = 0, v = 0;
      if (!field(node, ends_at + uint64_t{i} * 2, 2, &end) || !field(node, values_at + uint64_t{i} * 8, 8, &v)) return Status::Corrupt;
      if (end < previous || end > k) return Status::Corrupt;
      ends_[i] = static_cast<uint32_t>(end);
      values_[i] = v;
      previous = end;
    }
    ConstByteView keys;
    if (!node.try_subview(kNodeHeaderSize, k, &keys)) return Status::Corrupt;
    if (k) keys_.first(k).copy_from(keys);
    count_ = static_cast<uint32_t>(c);
    keys_size_ = static_cast<uint32_t>(k);
    return Status::Ok;
  }
  bool insert(uint32_t i, ConstByteView name, uint64_t v) {
    const uint32_t length = static_cast<uint32_t>(name.size());
    if (i > count_ || count_ >= capacity_ || keys_size_ + uint64_t{length} > keys_.size()) return false;
    const uint32_t start = i ? ends_[i - 1] : 0;
    for (uint32_t b = keys_size_; b > start; --b) keys_[b - 1 + length] = keys_[b - 1];
    for (uint32_t b = 0; b < length; ++b) keys_[start + b] = name[b];
    for (uint32_t j = count_; j > i; --j) {
      ends_[j] = ends_[j - 1] + length;
      values_[j] = values_[j - 1];
    }
    ends_[i] = start + length;
    values_[i] = v;
    ++count_;
    keys_size_ += length;
    return true;
  }
  void remove(uint32_t i) {
    const uint32_t start = i ? ends_[i - 1] : 0, length = ends_[i] - start;
    for (uint32_t b = start; b + length < keys_size_; ++b) keys_[b] = keys_[b + length];
    for (uint32_t j = i; j + 1 < count_; ++j) {
      ends_[j] = ends_[j + 1] - length;
      values_[j] = values_[j + 1];
    }
    --count_;
    keys_size_ -= length;
  }
  void truncate(uint32_t n) {
    if (n >= count_) return;
    keys_size_ = n ? ends_[n - 1] : 0;
    count_ = n;
  }
  Status store(ByteView out) const {
    if (out.size() != node_size_ || !fits()) return Status::Corrupt;
    out.fill(0);
    ByteWriter w{out};
    w.le64(left);
    w.le64(right);
    w.le64(overflow);
    w.le16(static_cast<uint16_t>(count_));
    w.le16(static_cast<uint16_t>(keys_size_));
    if (keys_size_) w.bytes(ConstByteView(keys_).first(keys_size_));
    w.zeros(((uint64_t{kNodeHeaderSize} + keys_size_ + 7) & ~uint64_t{7}) - (uint64_t{kNodeHeaderSize} + keys_size_));
    for (uint32_t i = 0; i < count_; ++i) w.le16(static_cast<uint16_t>(ends_[i]));
    for (uint32_t i = 0; i < count_; ++i) w.le64(values_[i]);
    return w.ok() ? Status::Ok : Status::Corrupt;
  }

  uint64_t left = kNone, right = kNone, overflow = kNone;

 private:
  allocators::IAllocator& allocator_;
  const uint32_t node_size_, capacity_;
  ByteView keys_;
  ArrayView<uint32_t> ends_;
  ArrayView<uint64_t> values_;
  uint32_t count_ = 0, keys_size_ = 0;
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

  Status s = parse_super();
  if (s != Status::Ok) return s;
  scratch_ = allocate_view<uint8_t>(allocator_, uint64_t{block_size_} * 2);
  if (scratch_.empty()) return Status::OutOfMemory;

  // A volume that was not shut down cleanly: put the logged blocks where they belong first (that can change the root, even the super
  // block). Without that the volume can be read only as it is.
  bool replay_failed = false;
  if (!storage.read_only && (volume_flags_ == kVolumeDirty || log_start_ != log_end_) && log_usable_ &&
      (volume_flags_ == kVolumeDirty || volume_flags_ == kVolumeClean)) {
    bool touched = false;
    if (replay_log(&touched) != Status::Ok) {
      replay_failed = true;
    } else if (touched) {
      s = parse_super();
      if (s != Status::Ok) return s;
    }
  }

  Inode root;
  s = read_inode(root_block_, &root);
  if (s != Status::Ok) return s;
  if (!root.is_directory()) return Status::Corrupt;

  // Writable only for a clean volume (nothing in the log) on a device that can be written, with a log and a bitmap we understand.
  if (!storage.read_only && !replay_failed && volume_flags_ == kVolumeClean && log_start_ == log_end_ && log_usable_ && group_shift_ <= 16 &&
      inode_size_ == block_size_) {
    bitmap_per_group_ = static_cast<uint32_t>((((uint64_t{1} << group_shift_) + (uint64_t{block_size_} << 3) - 1)) / (uint64_t{block_size_} << 3));
    if (bitmap_per_group_ == bitmap_groups_field_) {
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

// Reads and checks the super block.
Status BefsFileSystem::parse_super() {
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
  bitmap_groups_field_ = in.le32();
  group_shift_ = in.le32();
  group_count_ = in.le32();
  volume_flags_ = in.le32();
  uint32_t group = 0;
  uint16_t start = 0, length = 0;
  if (read_run(&in, &group, &start, &length) != Status::Ok) return Status::Corrupt;
  const Run log_run{group, start, length};
  log_start_ = in.le64();
  log_end_ = in.le64();
  if (in.le32() != kSuperMagic3) return Status::Corrupt;
  if (read_run(&in, &group, &start, &length) != Status::Ok) return Status::Corrupt;
  root_run_ = Run{group, start, length};
  if (block_size_ < 1024 || block_size_ > 8192 || block_shift_ > 13 || (1u << block_shift_) != block_size_) return Status::Corrupt;
  if (block_count_ == 0 || block_count_ > (uint64_t{1} << 50) || used_blocks_ > block_count_) return Status::Corrupt;
  if (inode_size < kInodeBytes || inode_size > (1u << 16)) return Status::Corrupt;
  inode_size_ = inode_size;
  if (group_shift_ > 40 || group_count_ == 0 || group_count_ > (1u << 20)) return Status::Corrupt;
  if (!run_to_block(root_run_, &root_block_)) return Status::Corrupt;

  // The log is a ring of blocks; the positions are block numbers in it.
  log_usable_ = run_to_block(log_run, &log_first_) && log_run.length >= kMinLogBlocks && log_start_ < log_run.length && log_end_ < log_run.length;
  log_len_ = log_usable_ ? log_run.length : 0;

  FixedString<kMaxNameLength + 1> text;
  for (uint8_t c : name) {
    if (c == 0) break;
    text.push_back(static_cast<char>(c));
  }
  label_.assign(text.view().first(text.size() < label_.capacity() ? text.size() : label_.capacity()));
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
  result.created_time = in.le64();
  result.modified_time = in.le64();
  in.seek(44);
  uint32_t parent_group = 0;
  uint16_t parent_start = 0, parent_length = 0;
  read_run(&in, &parent_group, &parent_start, &parent_length);
  uint64_t parent_block = 0;
  if (run_to_block(Run{parent_group, parent_start, parent_length}, &parent_block)) result.parent = parent_block;
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

// Walks down the tree to the leaf where `name` is or would be. The leaf is left in `buffer`.
Status BefsFileSystem::descend_to(const Inode& directory, const Tree& tree, StringView name, ByteView buffer, Node* leaf) {
  uint64_t at = tree.root;
  for (uint32_t depth = 0; depth < kMaxDepth; ++depth) {
    Node node;
    const Status s = read_node(directory, tree, at, buffer, &node);
    if (s != Status::Ok) return s;
    if (node.leaf()) {
      *leaf = node;
      return Status::Ok;
    }
    // Each key of an inner node is at least the largest of its child; names above all of them are in the overflow child.
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

// The inode block of entry `name` in `directory`.
Status BefsFileSystem::find(const Inode& directory, StringView name, uint64_t* inode_block) {
  Tree tree;
  Status s = open_tree(directory, &tree);
  if (s != Status::Ok) return s;
  Owned buffer(allocator_, tree.node_size);
  if (!buffer.ok()) return Status::OutOfMemory;
  Node node;
  s = descend_to(directory, tree, name, buffer.view(), &node);
  if (s != Status::Ok) return s;
  for (uint32_t i = 0; i < node.count; ++i) {
    ConstByteView key;
    if (!node_key(node, i, &key)) return Status::Corrupt;
    const int order = compare_names(key, name);
    if (order == 0) return node_value(node, i, inode_block) ? Status::Ok : Status::Corrupt;
    if (order > 0) break;
  }
  return Status::NotFound;
}

// Leaves `leaf` (in `buffer`) at the entry after `name`: *index is the first entry that sorts after it.
Status BefsFileSystem::seek_after(const Inode& directory, const Tree& tree, StringView name, ByteView buffer, Node* leaf, uint32_t* index) {
  const Status s = descend_to(directory, tree, name, buffer, leaf);
  if (s != Status::Ok) return s;
  uint32_t i = 0;
  for (; i < leaf->count; ++i) {
    ConstByteView key;
    if (!node_key(*leaf, i, &key)) return Status::Corrupt;
    if (compare_names(key, name) > 0) break;
  }
  *index = i;
  return Status::Ok;
}

Status BefsFileSystem::directory_empty(const Inode& directory, bool* empty) {
  Tree tree;
  Status s = open_tree(directory, &tree);
  if (s != Status::Ok) return s;
  Owned buffer(allocator_, tree.node_size);
  if (!buffer.ok()) return Status::OutOfMemory;
  Node node;
  uint64_t at = tree.root;
  bool at_leaf = false;
  for (uint32_t depth = 0; depth < kMaxDepth && !at_leaf; ++depth) {
    s = read_node(directory, tree, at, buffer.view(), &node);
    if (s != Status::Ok) return s;
    if (node.leaf()) {
      at_leaf = true;
    } else if (node.count == 0) {
      at = node.overflow;
    } else if (!node_value(node, 0, &at)) {
      return Status::Corrupt;
    }
  }
  if (!at_leaf) return Status::Corrupt;
  const uint64_t max_nodes = directory.size / tree.node_size + 1;
  for (uint64_t hops = 0; hops <= max_nodes; ++hops) {
    for (uint32_t i = 0; i < node.count; ++i) {
      ConstByteView key;
      if (!node_key(node, i, &key)) return Status::Corrupt;
      if (!is_dot_or_dotdot(key)) {
        *empty = false;
        return Status::Ok;
      }
    }
    if (node.right == kNone) {
      *empty = true;
      return Status::Ok;
    }
    s = read_node(directory, tree, node.right, buffer.view(), &node);
    if (s != Status::Ok) return s;
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
  entry->modified = inode.modified_time >> 16;
  entry->created = inode.created_time >> 16;
}

Status BefsFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Inode inode;
  StringView leaf;
  Status s = resolve(path, &inode, &leaf);
  bool created = false;
  if (s == Status::NotFound && mode != OpenMode::Read) {
    if (!writable_) return Status::ReadOnly;
    if (mode == OpenMode::ReadWrite) return Status::NotFound;
    uint64_t block = 0;
    s = create_node(path, false, &block);
    if (s == Status::Ok) s = read_inode(block, &inode);
    created = true;
  }
  if (s != Status::Ok) return s;
  if (inode.is_directory()) return Status::IsADirectory;
  if (!inode.is_file()) return Status::Unsupported;
  if (mode != OpenMode::Read) {
    if (!writable_ || (inode.mode & 0222) == 0) return Status::ReadOnly;
    if (!inode.is_regular()) return Status::Unsupported;
  }
  UniquePointer<IFile> opened = UniquePointer<IFile>::make_as<File>(allocator_, *this, inode.block, mode != OpenMode::Read);
  if (!opened) return Status::OutOfMemory;
  if (mode == OpenMode::CreateTruncate && !created) {
    s = opened->resize(0);
    if (s != Status::Ok) return s;
  }
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
      const Status s = chunk == block_size_ ? Status::Ok : raw_read(block << block_shift_, copy);  // (a whole block is replaced: no need to load it)
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

// Sets the length of a file: longer adds zeros, shorter gives blocks back.
Status BefsFileSystem::resize_file(uint64_t inode_block, uint64_t new_size) {
  if (new_size > max_file_size()) return Status::NoSpace;
  Inode inode;
  Status s = read_inode(inode_block, &inode);
  if (s != Status::Ok) return s;
  if (!inode.is_regular()) return Status::Unsupported;
  const uint64_t old_size = inode.size;
  const uint64_t blocks = (new_size + block_size_ - 1) >> block_shift_;
  tx_begin();
  if (new_size > old_size) {
    s = grow_stream(&inode, blocks);
    if (s == Status::Ok) s = zero_stream(inode, old_size, new_size);
    inode.size = new_size;
  } else {  // (also when the size stays: blocks a failed grow left behind past the end go back)
    inode.size = new_size;  // the size never claims more than the blocks that are left
    s = store_inode(inode);
    if (s == Status::Ok) s = shrink_stream(&inode, blocks);
  }
  if (s == Status::Ok) s = store_inode(inode);
  if (s == Status::Ok) s = touch(&inode);
  if (s == Status::Ok) s = tx_commit();
  if (s != Status::Ok) tx_rollback();
  return s;
}

// Writes `data` at `offset` (not past the end of the file) in one transaction together with the blocks the file needs for it and its new
// size: file data is journaled like the rest, so the piece is written entirely or not at all.
Status BefsFileSystem::write_piece(uint64_t inode_block, uint64_t offset, ConstByteView data) {
  Inode inode;
  Status s = read_inode(inode_block, &inode);
  if (s != Status::Ok) return s;
  const uint64_t end = offset + data.size();
  tx_begin();
  s = grow_stream(&inode, (end + block_size_ - 1) >> block_shift_);
  if (s == Status::Ok) {
    if (end > inode.size) inode.size = end;
    s = tx_write_stream(inode, offset, data);
  }
  if (s == Status::Ok) s = store_inode(inode);
  if (s == Status::Ok) s = touch(&inode);
  if (s == Status::Ok) s = tx_commit();
  if (s != Status::Ok) tx_rollback();
  return s;
}

Status BefsFileSystem::tx_zero(uint64_t block, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const Status s = tx_write((block + i) << block_shift_, ConstByteView(zeros_));
    if (s != Status::Ok) return s;
  }
  return Status::Ok;
}

// Commits the transaction through the journal (see the class comment): log entry, flush, super block dirty with the new end of the log
// and used-blocks count, flush, the blocks in their places, flush, super block clean. Before the super block says so the entry is not
// there for anyone, so a failure up to then just drops the transaction; after it the volume is left to the next mount's replay.
Status BefsFileSystem::tx_commit() {
  if (tx_count_ == 0) {
    tx_used_blocks_ = used_blocks_;
    return Status::Ok;
  }
  for (uint32_t i = 1; i < tx_count_; ++i) {  // insertion sort: there are few
    const TxBlock moved = tx_[i];
    uint32_t j = i;
    for (; j > 0 && tx_[j - 1].block > moved.block; --j) tx_[j] = tx_[j - 1];
    tx_[j] = moved;
  }
  const uint32_t capacity = journal_capacity();
  const uint64_t entry_blocks = tx_count_ + (tx_count_ + capacity - 1) / capacity;
  if (entry_blocks >= log_len_) return Status::NoSpace;  // (the caller drops the transaction)
  Status s = journal_write();
  if (s == Status::Ok && !device_.flush()) s = Status::IoError;
  if (s != Status::Ok) return s;
  const uint64_t new_end = (log_end_ + entry_blocks) % log_len_;
  s = write_super(kVolumeDirty, log_start_, new_end, &used_blocks_);
  if (s == Status::Ok && !device_.flush()) s = Status::IoError;
  if (s != Status::Ok) {
    tx_abandon();
    return s;
  }
  log_end_ = new_end;
  for (uint32_t i = 0; i < tx_count_ && s == Status::Ok; ++i) s = raw_write(tx_[i].block << block_shift_, ConstByteView(tx_[i].data));
  if (s == Status::Ok && !device_.flush()) s = Status::IoError;
  if (s == Status::Ok) s = write_super(kVolumeClean, new_end, new_end, nullptr);
  if (s != Status::Ok) {
    tx_abandon();
    return s;
  }
  log_start_ = new_end;
  for (uint32_t i = 0; i < tx_count_; ++i) release_view(allocator_, tx_[i].data);
  tx_count_ = 0;
  tx_used_blocks_ = used_blocks_;
  return Status::Ok;
}

// After a failure once the log said the transaction was there: nothing more is written to this volume until it is mounted again (and
// its log replayed).
void BefsFileSystem::tx_abandon() {
  for (uint32_t i = 0; i < tx_count_; ++i) release_view(allocator_, tx_[i].data);
  tx_count_ = 0;
  writable_ = false;
}

uint32_t BefsFileSystem::journal_capacity() const {
  const uint32_t fit = (block_size_ - 8) / 8;  // (Be's file system also stops at 127)
  return fit < 127 ? fit : 127;
}

// Writes the transaction's blocks to the log at log_end_ as arrays of runs, each followed by the blocks it names.
Status BefsFileSystem::journal_write() {
  const uint32_t capacity = journal_capacity();
  const ByteView array = scratch_.first(block_size_);
  uint64_t position = log_end_;
  for (uint32_t done = 0; done < tx_count_;) {
    const uint32_t n = tx_count_ - done < capacity ? tx_count_ - done : capacity;
    array.fill(0);
    ByteWriter out{array};
    out.le32(n);
    out.le32(capacity + 1);  // (Be's file system stores the maximum plus one)
    for (uint32_t i = 0; i < n; ++i) {
      Run run;
      if (!block_to_run(tx_[done + i].block, &run)) return Status::Corrupt;
      uint8_t raw[8];
      encode_run(run, raw);
      out.bytes(ConstByteView(raw));
    }
    if (!out.ok()) return Status::Corrupt;
    Status s = raw_write((log_first_ + position) << block_shift_, ConstByteView(array));
    position = (position + 1) % log_len_;
    for (uint32_t i = 0; i < n && s == Status::Ok; ++i) {
      s = raw_write((log_first_ + position) << block_shift_, ConstByteView(tx_[done + i].data));
      position = (position + 1) % log_len_;
    }
    if (s != Status::Ok) return s;
    done += n;
  }
  return Status::Ok;
}

// Changes the fields of the super block that the journal and the used-blocks count live in (all in one sector).
Status BefsFileSystem::write_super(uint32_t flags, uint64_t log_start, uint64_t log_end, const uint64_t* used_blocks) {
  uint8_t raw[56];  // bytes 56 to 111 of the super block
  Status s = raw_read(kSuperOffset + 56, ByteView(raw));
  if (s != Status::Ok) return s;
  if (used_blocks) {
    ByteWriter w{ByteView(raw).subview(0, 8)};
    w.le64(*used_blocks);
  }
  ByteWriter f{ByteView(raw).subview(28, 4)};  // flags at 84
  f.le32(flags);
  ByteWriter a{ByteView(raw).subview(40, 8)};  // log_start at 96
  a.le64(log_start);
  ByteWriter b{ByteView(raw).subview(48, 8)};  // log_end at 104
  b.le64(log_end);
  return raw_write(kSuperOffset + 56, ConstByteView(raw));
}

// Puts the blocks in the log where they belong, then marks the volume clean. The log is checked as Be's file system does: the arrays
// have to be well formed and everything must lie inside the volume, and a block 0 that is written has to be a super block.
Status BefsFileSystem::replay_log(bool* super_touched) {
  *super_touched = false;
  const uint32_t capacity = journal_capacity();
  Owned array(allocator_, block_size_);
  Owned blocks(allocator_, uint64_t{capacity} * block_size_);
  if (!array.ok() || !blocks.ok()) return Status::OutOfMemory;
  uint64_t position = log_start_;
  for (uint64_t guard = 0; position != log_end_; ++guard) {
    if (guard > log_len_) return Status::Corrupt;
    Status s = raw_read((log_first_ + position) << block_shift_, array.view());
    if (s != Status::Ok) return s;
    ByteReader in{ConstByteView(array.view())};
    const uint32_t count = in.le32(), maximum = in.le32();
    if (count == 0 || count > capacity || maximum != capacity + 1) return Status::Corrupt;
    if (1 + uint64_t{count} > (log_end_ + log_len_ - position) % log_len_) return Status::Corrupt;  // (it must end before log_end)
    uint64_t targets[127];
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t group = 0;
      uint16_t start = 0, length = 0;
      if (read_run(&in, &group, &start, &length) != Status::Ok || length != 1 || !run_to_block(Run{group, start, length}, &targets[i]))
        return Status::Corrupt;
    }
    for (uint32_t i = 0; i < count; ++i) {  // first read and check everything, then write
      const ByteView block = blocks.view().subview(uint64_t{i} * block_size_, block_size_);
      s = raw_read((log_first_ + (position + 1 + i) % log_len_) << block_shift_, block);
      if (s != Status::Ok) return s;
      uint64_t m1 = 0, m2 = 0, m3 = 0;
      if (targets[i] == 0 && (!field(ConstByteView(block), kSuperOffset + 32, 4, &m1) || !field(ConstByteView(block), kSuperOffset + 68, 4, &m2) ||
                              !field(ConstByteView(block), kSuperOffset + 112, 4, &m3) || m1 != kSuperMagic1 || m2 != kSuperMagic2 || m3 != kSuperMagic3))
        return Status::Corrupt;
    }
    for (uint32_t i = 0; i < count; ++i) {
      s = raw_write(targets[i] << block_shift_, ConstByteView(blocks.view().subview(uint64_t{i} * block_size_, block_size_)));
      if (s != Status::Ok) return s;
      if (targets[i] == 0) *super_touched = true;
    }
    position = (position + 1 + count) % log_len_;
  }
  if (!device_.flush()) return Status::IoError;
  Status s = write_super(kVolumeClean, log_end_, log_end_, nullptr);
  if (s == Status::Ok && !device_.flush()) s = Status::IoError;
  if (s != Status::Ok) return s;
  log_start_ = log_end_;
  volume_flags_ = kVolumeClean;
  return Status::Ok;
}

// Drops everything the transaction changed.
void BefsFileSystem::tx_rollback() {
  if (tx_.empty()) return;
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
  // The log must be marked used too, or its blocks could be handed out for something else.
  for (uint64_t block = log_first_; block < log_first_ + log_len_; ++block) {
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

uint64_t BefsFileSystem::time_now() { return unix_time_now() << 16; }

Status BefsFileSystem::store_times(const Inode& inode) {
  uint8_t raw[16];
  ByteWriter out{ByteView(raw)};
  out.le64(inode.created_time);
  out.le64(inode.modified_time);
  if (!out.ok()) return Status::Corrupt;
  return tx_write((inode.block << block_shift_) + 28, ConstByteView(raw));
}

Status BefsFileSystem::touch(Inode* inode) {
  const uint64_t now = time_now();
  if (now == 0) return Status::Ok;
  inode->modified_time = now;
  return store_times(*inode);
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


// ---- Changing directory trees --------------------------------------------------------------------------------------------------------
// A directory's stream holds a B+tree: a header node, then nodes of node_size bytes (named by their offset in the stream). Leaves hold
// name -> inode block, inner nodes hold name -> child with the child's largest name, and the overflow link is the last child. Nodes of
// one level are linked left to right. Freed nodes hang on a list from the header, chained through left_link and marked in overflow_link.

Status BefsFileSystem::tree_load(const Inode& directory, TreeCtx* ctx) {
  uint8_t raw[kTreeHeaderSize];
  if (directory.size < kTreeHeaderSize) return Status::Corrupt;
  const Status s = read_stream(directory, 0, ByteView(raw));
  if (s != Status::Ok) return s;
  ByteReader in{ConstByteView(raw)};
  if (in.le32() != kTreeMagic) return Status::Corrupt;
  TreeCtx result;
  result.node_size = in.le32();
  result.levels = in.le32();
  in.skip(4);  // data type
  result.root = in.le64();
  result.free_node = in.le64();
  result.max_size = in.le64();
  if (!in.ok() || result.node_size < kMinNodeSize || result.node_size % 8 != 0 || result.max_size > directory.size) return Status::Corrupt;
  if (result.node_size > kMaxWriteNodeSize) return Status::Unsupported;
  *ctx = result;
  return Status::Ok;
}

Status BefsFileSystem::tree_save(const Inode& directory, const TreeCtx& ctx) {
  uint8_t levels[4], rest[24];
  ByteWriter a{ByteView(levels)}, b{ByteView(rest)};
  a.le32(ctx.levels);
  b.le64(ctx.root);
  b.le64(ctx.free_node);
  b.le64(ctx.max_size);
  Status s = tx_write_stream(directory, 8, ConstByteView(levels));
  if (s == Status::Ok) s = tx_write_stream(directory, 16, ConstByteView(rest));
  return s;
}

// Writes into the stream through the transaction.
Status BefsFileSystem::tx_write_stream(const Inode& inode, uint64_t offset, ConstByteView data) {
  if (offset > inode.size || data.size() > inode.size - offset) return Status::Corrupt;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t at = offset + done;
    uint64_t block = 0, left = 0;
    Status s = locate(inode, at >> block_shift_, &block, &left);
    if (s != Status::Ok) return s;
    const uint64_t within = at & (block_size_ - 1);
    uint64_t chunk = (left << block_shift_) - within;
    if (chunk > data.size() - done) chunk = data.size() - done;
    s = tx_write((block << block_shift_) + within, data.subview(done, chunk));
    if (s != Status::Ok) return s;
    done += chunk;
  }
  return Status::Ok;
}

Status BefsFileSystem::edit_read(const Inode& directory, ByteView buffer, uint64_t at, NodeEdit* node) {
  const Status s = read_stream(directory, at, buffer);
  return s == Status::Ok ? node->load(ConstByteView(buffer)) : s;
}

Status BefsFileSystem::edit_write(const Inode& directory, ByteView buffer, uint64_t at, const NodeEdit& node) {
  const Status s = node.store(buffer);
  return s == Status::Ok ? tx_write_stream(directory, at, ConstByteView(buffer)) : s;
}

// A node for the tree: one from the free list, or a new one at the end of the stream (which may need blocks, and changes the
// directory's size - the caller stores the directory's inode).
Status BefsFileSystem::tree_alloc_node(Inode* directory, TreeCtx* ctx, uint64_t* offset) {
  if (ctx->free_node != kNone) {
    uint8_t raw[8];
    const uint64_t node = ctx->free_node;
    if (node % 8 != 0) return Status::Corrupt;
    const Status s = read_stream(*directory, node, ByteView(raw));
    if (s != Status::Ok) return s;
    ByteReader in{ConstByteView(raw)};
    ctx->free_node = in.le64();  // (left_link: the next free node)
    *offset = node;
    return Status::Ok;
  }
  const uint64_t start = ctx->max_size, end = start + ctx->node_size;
  if (end < start || end > max_file_size()) return Status::NoSpace;
  const Status s = grow_stream(directory, (end + block_size_ - 1) >> block_shift_);
  if (s != Status::Ok) return s;
  if (end > directory->size) directory->size = end;
  ctx->max_size = end;
  *offset = start;
  return Status::Ok;
}

Status BefsFileSystem::tree_free_node(const Inode& directory, TreeCtx* ctx, uint64_t offset) {
  uint8_t raw[kNodeHeaderSize] = {};
  ByteWriter w{ByteView(raw)};
  w.le64(ctx->free_node);
  w.le64(kNone);
  w.le64(kFreeNode);
  const Status s = tx_write_stream(directory, offset, ConstByteView(raw));
  if (s == Status::Ok) ctx->free_node = offset;
  return s;
}

// Takes a node out of its level's list of links.
Status BefsFileSystem::tree_unlink(const Inode& directory, uint64_t left, uint64_t right) {
  uint8_t raw[8];
  Status s = Status::Ok;
  if (left != kNone) {
    ByteWriter w{ByteView(raw)};
    w.le64(right);
    s = tx_write_stream(directory, left + 8, ConstByteView(raw));
  }
  if (s == Status::Ok && right != kNone) {
    ByteWriter w{ByteView(raw)};
    w.le64(left);
    s = tx_write_stream(directory, right, ConstByteView(raw));
  }
  return s;
}

// The tree of a new, empty directory: the header node and an empty root leaf.
Status BefsFileSystem::init_tree(Inode* directory) {
  const uint32_t node_size = block_size_;
  if (node_size > kMaxWriteNodeSize) return Status::Unsupported;
  Status s = grow_stream(directory, (uint64_t{node_size} * 2 + block_size_ - 1) >> block_shift_);
  if (s != Status::Ok) return s;
  directory->size = uint64_t{node_size} * 2;
  Owned buffer(allocator_, node_size);
  if (!buffer.ok()) return Status::OutOfMemory;
  buffer.view().fill(0);
  ByteWriter w{buffer.view()};
  w.le32(kTreeMagic);
  w.le32(node_size);
  w.le32(1);   // levels
  w.le32(0);   // data type: strings
  w.le64(node_size);
  w.le64(kNone);
  w.le64(uint64_t{node_size} * 2);
  s = tx_write_stream(*directory, 0, ConstByteView(buffer.view()));
  if (s != Status::Ok) return s;
  NodeEdit root(allocator_, node_size);
  if (!root.ok()) return Status::OutOfMemory;
  root.reset_leaf();
  return edit_write(*directory, buffer.view(), node_size, root);
}

// Adds `name` -> `inode_block` to the directory's tree, splitting nodes that get too full.
Status BefsFileSystem::tree_insert(Inode* directory, StringView name, uint64_t inode_block) {
  TreeCtx ctx;
  Status s = tree_load(*directory, &ctx);
  if (s != Status::Ok) return s;
  Owned buffer(allocator_, ctx.node_size);
  NodeEdit cur(allocator_, ctx.node_size), sibling(allocator_, ctx.node_size);
  if (!buffer.ok() || !cur.ok() || !sibling.ok()) return Status::OutOfMemory;
  PathStep path[kMaxDepth];
  uint32_t depth = 0;
  uint64_t at = ctx.root;
  for (;;) {  // down to the leaf
    s = edit_read(*directory, buffer.view(), at, &cur);
    if (s != Status::Ok) return s;
    if (cur.leaf()) break;
    if (depth == kMaxDepth) return Status::Corrupt;
    uint32_t i = 0;
    while (i < cur.count() && compare_names(cur.key(i), name) < 0) ++i;
    path[depth++] = PathStep{at, i};
    at = i < cur.count() ? cur.value(i) : cur.overflow;
  }
  uint32_t position = 0;
  for (; position < cur.count(); ++position) {
    const int order = compare_names(cur.key(position), name);
    if (order == 0) return Status::AlreadyExists;
    if (order > 0) break;
  }
  if (!cur.insert(position, name.bytes(), inode_block)) return Status::Corrupt;

  bool written = false;  // (the new root is written where it is made)
  while (!cur.fits()) {
    const uint32_t count = cur.count();
    const bool leaf = cur.leaf();
    if (count < (leaf ? 2u : 3u)) return Status::Corrupt;
    // Split by size: the left part is about half of the bytes.
    uint32_t split = 0;
    uint64_t sum = 0;
    while (split < count && sum * 2 < cur.total_bytes()) sum += cur.entry_bytes(split++);
    const uint32_t lowest = 1, highest = leaf ? count - 1 : count - 2;
    if (split < lowest) split = lowest;
    if (split > highest) split = highest;

    uint64_t other = 0;
    s = tree_alloc_node(directory, &ctx, &other);
    if (s != Status::Ok) return s;
    uint8_t separator[kMaxNameLength + 1];
    uint32_t separator_size = 0;
    sibling.reset_leaf();
    if (leaf) {  // the left part keeps its entries, its largest name goes up
      const ConstByteView last = cur.key(split - 1);
      if (last.size() > sizeof separator) return Status::Corrupt;
      separator_size = static_cast<uint32_t>(last.size());
      ByteView(separator, separator_size).copy_from(last);
      for (uint32_t j = split; j < count; ++j) {
        if (!sibling.insert(sibling.count(), cur.key(j), cur.value(j))) return Status::Corrupt;
      }
      cur.truncate(split);
    } else {  // the entry at `split` goes up; its child becomes the left part's last child
      const ConstByteView middle = cur.key(split);
      if (middle.size() > sizeof separator) return Status::Corrupt;
      separator_size = static_cast<uint32_t>(middle.size());
      ByteView(separator, separator_size).copy_from(middle);
      sibling.overflow = cur.overflow;
      for (uint32_t j = split + 1; j < count; ++j) {
        if (!sibling.insert(sibling.count(), cur.key(j), cur.value(j))) return Status::Corrupt;
      }
      const uint64_t middle_child = cur.value(split);
      cur.truncate(split);
      cur.overflow = middle_child;
    }
    const uint64_t old_right = cur.right;
    sibling.left = at;
    sibling.right = old_right;
    cur.right = other;
    s = edit_write(*directory, buffer.view(), at, cur);
    if (s == Status::Ok) s = edit_write(*directory, buffer.view(), other, sibling);
    if (s == Status::Ok && old_right != kNone) {
      uint8_t raw[8];
      ByteWriter w{ByteView(raw)};
      w.le64(other);
      s = tx_write_stream(*directory, old_right, ConstByteView(raw));
    }
    if (s != Status::Ok) return s;

    const ConstByteView up(separator, separator_size);
    if (depth == 0) {  // the root split: a new root over the two halves
      uint64_t root = 0;
      s = tree_alloc_node(directory, &ctx, &root);
      if (s != Status::Ok) return s;
      cur.reset_leaf();
      cur.overflow = other;
      if (!cur.insert(0, up, at)) return Status::Corrupt;
      s = edit_write(*directory, buffer.view(), root, cur);
      if (s != Status::Ok) return s;
      ctx.root = root;
      ++ctx.levels;
      written = true;
      break;
    }
    const PathStep step = path[--depth];
    const uint64_t halved = at;
    at = step.offset;
    s = edit_read(*directory, buffer.view(), at, &cur);
    if (s != Status::Ok) return s;
    if (step.index < cur.count()) {
      cur.set_value(step.index, other);
    } else {
      cur.overflow = other;
    }
    if (!cur.insert(step.index, up, halved)) return Status::Corrupt;
  }
  if (!written) {
    s = edit_write(*directory, buffer.view(), at, cur);
    if (s != Status::Ok) return s;
  }
  s = tree_save(*directory, ctx);
  ++tree_epoch_;
  return s;
}

// Removes `name` from the directory's tree. A node that ends up empty is freed (and an inner node left with a single child is replaced
// by that child); nodes are not merged.
Status BefsFileSystem::tree_remove(Inode* directory, StringView name) {
  TreeCtx ctx;
  Status s = tree_load(*directory, &ctx);
  if (s != Status::Ok) return s;
  Owned buffer(allocator_, ctx.node_size);
  NodeEdit cur(allocator_, ctx.node_size);
  if (!buffer.ok() || !cur.ok()) return Status::OutOfMemory;
  PathStep path[kMaxDepth];
  uint32_t depth = 0;
  uint64_t at = ctx.root;
  for (;;) {
    s = edit_read(*directory, buffer.view(), at, &cur);
    if (s != Status::Ok) return s;
    if (cur.leaf()) break;
    if (depth == kMaxDepth) return Status::Corrupt;
    uint32_t i = 0;
    while (i < cur.count() && compare_names(cur.key(i), name) < 0) ++i;
    path[depth++] = PathStep{at, i};
    at = i < cur.count() ? cur.value(i) : cur.overflow;
  }
  uint32_t position = 0;
  while (position < cur.count() && compare_names(cur.key(position), name) < 0) ++position;
  if (position == cur.count() || compare_names(cur.key(position), name) != 0) return Status::NotFound;
  cur.remove(position);
  ++tree_epoch_;
  if (depth == 0 || cur.count() > 0) return edit_write(*directory, buffer.view(), at, cur);

  // The leaf is empty: take it out of its level and out of its parent, and so on upwards while the parents lose their last child. (An
  // inner node that keeps a single child, its overflow link, stays: every leaf has to stay at the same depth.)
  for (;;) {
    if (depth == 0) {  // the root itself has nothing left below it
      cur.reset_leaf();
      s = edit_write(*directory, buffer.view(), at, cur);
      if (s != Status::Ok) return s;
      ctx.levels = 1;
      break;
    }
    s = tree_unlink(*directory, cur.left, cur.right);
    if (s == Status::Ok) s = tree_free_node(*directory, &ctx, at);
    if (s != Status::Ok) return s;
    const PathStep step = path[--depth];
    at = step.offset;
    s = edit_read(*directory, buffer.view(), at, &cur);
    if (s != Status::Ok) return s;
    if (step.index < cur.count()) {
      cur.remove(step.index);
    } else if (cur.count() > 0) {  // it was the overflow child: the child of the last entry takes that place
      cur.overflow = cur.value(cur.count() - 1);
      cur.remove(cur.count() - 1);
    } else {  // it was the only child: this node is empty now too
      continue;
    }
    s = edit_write(*directory, buffer.view(), at, cur);
    if (s != Status::Ok) return s;
    break;
  }
  // A root with a single child gives way to it (the tree gets one level shallower).
  for (uint32_t guard = 0; guard < kMaxDepth; ++guard) {
    s = edit_read(*directory, buffer.view(), ctx.root, &cur);
    if (s != Status::Ok) return s;
    if (cur.leaf() || cur.count() > 0) break;
    const uint64_t old_root = ctx.root;
    ctx.root = cur.overflow;
    if (ctx.levels > 1) --ctx.levels;
    s = tree_free_node(*directory, &ctx, old_root);
    if (s != Status::Ok) return s;
  }
  return tree_save(*directory, ctx);
}

// ---- Creating, removing and renaming ------------------------------------------------------------------------------------------------

namespace {
// Keeps an operation's transaction in one piece (no commits in between) for as long as it lives.
class AtomicScope {
 public:
  explicit AtomicScope(bool* flag) : flag_(flag) { *flag_ = true; }
  ~AtomicScope() { *flag_ = false; }
  AtomicScope(const AtomicScope&) = delete;
  AtomicScope& operator=(const AtomicScope&) = delete;

 private:
  bool* flag_;
};
}  // namespace

bool BefsFileSystem::is_open(uint64_t block) const {
  for (const OpenHandle* handle = open_; handle; handle = handle->next) {
    if (handle->block == block) return true;
  }
  return false;
}

bool BefsFileSystem::block_to_run(uint64_t block, Run* run) const {
  const uint64_t group = block >> group_shift_;
  if (block >= block_count_ || group >= group_count_) return false;
  *run = Run{static_cast<uint32_t>(group), static_cast<uint16_t>(block & ((uint64_t{1} << group_shift_) - 1)), 1};
  return true;
}

// The directory that holds `path` and the name in it.
Status BefsFileSystem::split_path(StringView path, Inode* parent, StringView* leaf) {
  StringView parent_path;
  if (!split_parent(path, &parent_path, leaf) || !valid_name(*leaf)) return Status::InvalidPath;
  StringView ignored;
  const Status s = resolve(parent_path, parent, &ignored);
  if (s != Status::Ok) return s;
  return parent->is_directory() ? Status::Ok : Status::NotADirectory;
}

Status BefsFileSystem::create_body(Inode* parent, StringView leaf, bool directory, uint64_t* inode_block) {
  Run run;
  Status s = allocate_any(parent->block, 1, 1, &run);
  uint64_t block = 0;
  if (s == Status::Ok && !run_to_block(run, &block)) s = Status::Corrupt;
  if (s == Status::Ok) s = tx_zero(block, 1);
  if (s != Status::Ok) return s;
  Run parent_run;
  if (!block_to_run(parent->block, &parent_run)) return Status::Corrupt;
  Inode fresh;
  fresh.block = block;
  fresh.mode = directory ? 0x41ED : 0x81A4;
  fresh.flags = kInodeInUse;
  uint8_t raw[72] = {};
  ByteWriter w{ByteView(raw)};
  w.le32(kInodeMagic);
  w.le32(run.group);
  w.le16(run.start);
  w.le16(run.length);
  w.le32(0);  // owner and group
  w.le32(0);
  w.le32(fresh.mode);
  w.le32(fresh.flags);
  fresh.created_time = fresh.modified_time = time_now();
  w.le64(fresh.created_time);
  w.le64(fresh.modified_time);
  w.le32(parent_run.group);
  w.le16(parent_run.start);
  w.le16(parent_run.length);
  w.zeros(8);   // attributes
  w.le32(0);    // type
  w.le32(inode_size_);
  w.le32(0);
  if (!w.ok() || w.position() != sizeof raw) return Status::Corrupt;
  s = tx_write(block << block_shift_, ConstByteView(raw));
  if (s == Status::Ok && directory) s = init_tree(&fresh);
  if (s == Status::Ok) s = store_inode(fresh);
  if (s == Status::Ok) s = tree_insert(parent, leaf, block);
  if (s == Status::Ok) s = store_inode(*parent);
  if (s == Status::Ok) s = touch(parent);
  if (s != Status::Ok) return s;
  *inode_block = block;
  return Status::Ok;
}

Status BefsFileSystem::create_node(StringView path, bool directory, uint64_t* inode_block) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Status s = writable();
  if (s != Status::Ok) return s;
  Inode parent;
  StringView leaf;
  s = split_path(path, &parent, &leaf);
  if (s != Status::Ok) return s;
  uint64_t existing = 0;
  s = find(parent, leaf, &existing);
  if (s == Status::Ok) return Status::AlreadyExists;
  if (s != Status::NotFound) return s;
  AtomicScope atomic(&atomic_);
  tx_begin();
  s = create_body(&parent, leaf, directory, inode_block);
  if (s == Status::Ok) s = tx_commit();
  if (s != Status::Ok) tx_rollback();
  return s;
}

Status BefsFileSystem::remove_body(Inode* parent, StringView leaf, uint64_t inode_block) {
  Status s = tree_remove(parent, leaf);
  if (s == Status::Ok) s = store_inode(*parent);
  if (s == Status::Ok) s = touch(parent);
  if (s == Status::Ok) s = set_bits(inode_block, 1, false);
  if (s == Status::Ok) {  // (leave no inode behind that would still pass for a live one)
    uint8_t raw[4];
    ByteWriter w{ByteView(raw)};
    w.le32(kInodeDeleted);
    s = tx_write((inode_block << block_shift_) + 24, ConstByteView(raw));
  }
  return s;
}

Status BefsFileSystem::remove(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Status s = writable();
  if (s != Status::Ok) return s;
  Inode parent;
  StringView leaf;
  s = split_path(path, &parent, &leaf);
  if (s != Status::Ok) return s;
  uint64_t block = 0;
  s = find(parent, leaf, &block);
  if (s != Status::Ok) return s;
  Inode node;
  s = read_inode(block, &node);
  if (s != Status::Ok) return s;
  if (is_open(block)) return Status::Busy;
  if (node.is_directory()) {
    bool empty = false;
    s = directory_empty(node, &empty);
    if (s != Status::Ok) return s;
    if (!empty) return Status::NotEmpty;
  }
  if (node.is_directory()) {
    // A directory goes in one piece, tree and name together, so that no crash can leave a name that leads to a directory without a tree.
    // (If its tree is too big for one transaction the steps below take over.)
    {
      AtomicScope atomic(&atomic_);
      tx_begin();
      Inode gone = node;
      gone.size = 0;
      s = shrink_stream(&gone, 0);
      if (s == Status::Ok) s = remove_body(&parent, leaf, block);
      if (s == Status::Ok) s = tx_commit();
      if (s == Status::Ok) return s;
      tx_rollback();
      if (s != Status::NoSpace) return s;
    }
  }
  if (!node.inline_link && (node.size != 0 || node.max_direct != 0)) {  // give the data back first, in steps
    tx_begin();
    node.size = 0;
    s = store_inode(node);
    if (s == Status::Ok) s = shrink_stream(&node, 0);
    if (s == Status::Ok) s = store_inode(node);
    if (s == Status::Ok) s = tx_commit();
    if (s != Status::Ok) {
      tx_rollback();
      return s;
    }
  }
  AtomicScope atomic(&atomic_);
  tx_begin();
  s = remove_body(&parent, leaf, block);
  if (s == Status::Ok) s = tx_commit();
  if (s != Status::Ok) tx_rollback();
  return s;
}

Status BefsFileSystem::rename_body(Inode* from_parent, StringView from_leaf, Inode* to_parent, StringView to_leaf, const Inode& node) {
  Status s = tree_insert(to_parent, to_leaf, node.block);
  if (s == Status::Ok) s = tree_remove(from_parent, from_leaf);
  if (s == Status::Ok) s = store_inode(*to_parent);
  if (s == Status::Ok) s = touch(to_parent);
  if (s == Status::Ok && from_parent != to_parent) s = store_inode(*from_parent);
  if (s == Status::Ok && from_parent != to_parent) s = touch(from_parent);
  if (s == Status::Ok && node.is_directory() && from_parent != to_parent) {  // its parent link
    Run run;
    if (!block_to_run(to_parent->block, &run)) return Status::Corrupt;
    uint8_t raw[8];
    encode_run(run, raw);
    s = tx_write((node.block << block_shift_) + 44, ConstByteView(raw));
  }
  return s;
}

Status BefsFileSystem::rename(StringView from, StringView to) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Status s = writable();
  if (s != Status::Ok) return s;
  Inode from_parent, to_parent;
  StringView from_leaf, to_leaf;
  s = split_path(from, &from_parent, &from_leaf);
  if (s != Status::Ok) return s;
  s = split_path(to, &to_parent, &to_leaf);
  if (s != Status::Ok) return s;
  uint64_t block = 0, taken = 0;
  s = find(from_parent, from_leaf, &block);
  if (s != Status::Ok) return s;
  s = find(to_parent, to_leaf, &taken);
  if (s == Status::Ok) return Status::AlreadyExists;
  if (s != Status::NotFound) return s;
  Inode node;
  s = read_inode(block, &node);
  if (s != Status::Ok) return s;
  if (node.is_directory()) {  // not into itself: walk up from the new parent
    uint64_t up = to_parent.block;
    for (uint32_t guard = 0; guard < 4096; ++guard) {
      if (up == block) return Status::InvalidArgument;
      if (up == root_block_) break;
      Inode above;
      s = read_inode(up, &above);
      if (s != Status::Ok) return s;
      if (above.parent == 0 || above.parent == up) break;
      up = above.parent;
    }
  }
  const bool same_directory = from_parent.block == to_parent.block;
  AtomicScope atomic(&atomic_);
  tx_begin();
  s = rename_body(same_directory ? &to_parent : &from_parent, from_leaf, &to_parent, to_leaf, node);
  if (s == Status::Ok) s = tx_commit();
  if (s != Status::Ok) tx_rollback();
  return s;
}

Status BefsFileSystem::create_directory(StringView path) {
  uint64_t block = 0;
  return create_node(path, true, &block);
}

Status BefsFileSystem::flush() {
  kernel::ScopedRecursiveMutex lock(mutex_);
  return writable_ && !device_.flush() ? Status::IoError : Status::Ok;
}

}  // namespace filesystem
