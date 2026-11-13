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

#include "FileSystem/Squash/SquashFileSystem.hpp"
#include "FileSystem/LinkWalk.hpp"
#include "Media/Lz4.hpp"
#include "Media/Lzo.hpp"
#include "Media/Xz.hpp"
#include "Media/Zlib.hpp"
#include "Media/Zstd.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace filesystem {

using namespace squash;

namespace {

uint16_t le16_at(ConstByteView bytes, uint64_t at) {
  ByteReader reader(bytes.subview(at, 2));
  return reader.le16();
}
uint32_t le32_at(ConstByteView bytes, uint64_t at) {
  ByteReader reader(bytes.subview(at, 4));
  return reader.le32();
}
uint64_t le64_at(ConstByteView bytes, uint64_t at) {
  ByteReader reader(bytes.subview(at, 8));
  return reader.le64();
}

// The bytes of a char buffer, to read names and link targets into. Safe: char and uint8_t have the same size, alignment and representation, and the
// view covers only the `size` chars the caller owns.
ByteView char_bytes(char* text, uint64_t size) { return ByteView(reinterpret_cast<uint8_t*>(text), size); }

}  // namespace

// Reads a stream of metadata blocks (the inode or the directory table) from a position in it, block after block.
class SquashFileSystem::MetaReader {
 public:
  MetaReader(SquashFileSystem& fs, uint64_t table, uint64_t block, uint32_t offset) : fs_(fs), table_(table), block_(block), offset_(offset), buffer_(fs.allocator_) {}

  Status read(ByteView out) {
    uint64_t done = 0;
    while (done < out.size()) {
      if (const Status s = ensure(); s != Status::Ok) return s;
      const uint64_t available = buffer_.size() - offset_, want = out.size() - done, n = available < want ? available : want;
      out.subview(done, n).copy_from(buffer_.view().subview(offset_, n));
      offset_ += static_cast<uint32_t>(n);
      done += n;
    }
    return Status::Ok;
  }
  Status skip(uint64_t count) {
    while (count) {
      if (const Status s = ensure(); s != Status::Ok) return s;
      const uint64_t available = buffer_.size() - offset_, n = available < count ? available : count;
      offset_ += static_cast<uint32_t>(n);
      count -= n;
    }
    return Status::Ok;
  }
  uint64_t block() const { return block_; }
  uint32_t offset() const { return offset_; }

 private:
  // A block is loaded and has bytes left at offset_.
  Status ensure() {
    if (!loaded_) {
      if (const Status s = load(); s != Status::Ok) return s;
      if (offset_ > buffer_.size()) return Status::Corrupt;
    }
    while (offset_ == buffer_.size()) {
      if (disk_size_ > ~uint64_t{0} - block_) return Status::Corrupt;
      block_ += disk_size_;
      offset_ = 0;
      if (const Status s = load(); s != Status::Ok) return s;
    }
    return Status::Ok;
  }
  Status load() {
    if (block_ > ~uint64_t{0} - table_) return Status::Corrupt;
    const Status s = fs_.metadata_block(table_ + block_, &buffer_, &disk_size_);
    if (s != Status::Ok) return s;
    if (buffer_.size() == 0) return Status::Corrupt;
    loaded_ = true;
    return Status::Ok;
  }

  SquashFileSystem& fs_;
  uint64_t table_, block_;
  uint32_t offset_;
  ByteBuffer buffer_;
  uint32_t disk_size_ = 0;
  bool loaded_ = false;
};

class SquashFileSystem::File final : public IFile {
 public:
  File(SquashFileSystem& fs, const Inode& inode) : fs_(fs), inode_(inode), sizes_(fs.allocator_), positions_(fs.allocator_) {}

  // Reads the block list (in pieces, so that a damaged block count runs out of metadata before it runs out of memory).
  Status load() {
    if (inode_.kind != Kind::File || inode_.block_count == 0) return Status::Ok;
    MetaReader reader(fs_, fs_.inode_table_, inode_.list_block, inode_.list_offset);
    uint64_t position = inode_.blocks_start;
    uint8_t chunk[512];
    for (uint64_t left = inode_.block_count; left;) {
      const uint64_t words = left < sizeof(chunk) / 4 ? left : sizeof(chunk) / 4;
      const ByteView bytes = ByteView(chunk).first(words * 4);
      if (const Status s = reader.read(bytes); s != Status::Ok) return s;
      for (uint64_t i = 0; i < words; ++i) {
        const uint32_t word = le32_at(bytes, i * 4), disk = word & kDataSizeMask;
        if (disk > fs_.block_size_ || (word != 0 && disk == 0)) return Status::Corrupt;
        if (position > fs_.bytes_used_ || disk > fs_.bytes_used_ - position) return Status::Corrupt;
        if (!sizes_.push_back(word) || !positions_.push_back(position)) return Status::OutOfMemory;
        position += disk;
      }
      left -= words;
    }
    return Status::Ok;
  }

  uint64_t size() const override { return inode_.kind == Kind::File ? inode_.size : 0; }

  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    *transferred = 0;
    const uint64_t total = size();
    if (offset >= total || buffer.empty()) return Status::Ok;
    const uint64_t bs = fs_.block_size_;
    uint64_t want = total - offset < buffer.size() ? total - offset : buffer.size();
    uint64_t done = 0;
    while (done < want) {
      const uint64_t at = offset + done, index = at / bs, within = at % bs;
      uint64_t n;
      Status s;
      if (index < inode_.block_count) {
        const uint64_t block_bytes = total - index * bs < bs ? total - index * bs : bs;
        n = block_bytes - within < want - done ? block_bytes - within : want - done;
        s = fs_.read_data_block(positions_[index], sizes_[index], static_cast<uint32_t>(block_bytes), within, buffer.subview(done, n));
      } else {
        // The tail, in a fragment block.
        Fragment fragment;
        s = fs_.fragment(inode_.fragment, &fragment);
        if (s != Status::Ok) return s;
        const uint64_t tail = total - inode_.block_count * bs;
        n = tail - within < want - done ? tail - within : want - done;
        s = fs_.read_data_block(fragment.start, fragment.size, static_cast<uint32_t>(inode_.fragment_offset + tail), inode_.fragment_offset + within, buffer.subview(done, n));
      }
      if (s != Status::Ok) return s;
      done += n;
      *transferred = done;
    }
    return Status::Ok;
  }
  Status write(uint64_t, ConstByteView, uint64_t* transferred) override { *transferred = 0; return Status::ReadOnly; }
  Status resize(uint64_t) override { return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }

 private:
  SquashFileSystem& fs_;
  Inode inode_;
  List<uint32_t> sizes_;
  List<uint64_t> positions_;
};

class SquashFileSystem::Directory final : public IDirectory {
 public:
  Directory(SquashFileSystem& fs, const Inode& inode) : fs_(fs), inode_(inode), path_(fs.allocator_) {}
  bool set_path(StringView path) { return path_.assign(path); }

  Status next(DirectoryEntry* entry) override {
    if (!reader_) {
      reader_ = UniquePointer<MetaReader>::make(fs_.allocator_, fs_, fs_.directory_table_, inode_.listing_block, inode_.listing_offset);
      if (!reader_) return Status::OutOfMemory;
      left_ = inode_.listing_size;
      run_ = 0;
    }
    if (run_ == 0) {
      if (left_ < 12) return Status::EndOfDirectory;
      uint8_t header[12];
      if (const Status s = reader_->read(ByteView(header)); s != Status::Ok) return s;
      left_ -= 12;
      const ConstByteView h(header);
      const uint32_t count = le32_at(h, 0);
      if (count >= kMaxDirectoryRun) return Status::Corrupt;
      run_ = count + 1;
      start_ = le32_at(h, 4);
      base_ = le32_at(h, 8);
    }
    uint8_t raw[8];
    if (left_ < 8) return Status::Corrupt;
    if (const Status s = reader_->read(ByteView(raw)); s != Status::Ok) return s;
    const ConstByteView e(raw);
    const uint16_t offset = le16_at(e, 0);
    const uint32_t name_size = uint32_t(le16_at(e, 6)) + 1;
    if (offset >= kMetadataSize || name_size > kMaxNameLength || left_ - 8 < name_size) return Status::Corrupt;
    char name[kMaxNameLength + 1];
    if (const Status s = reader_->read(char_bytes(name, name_size)); s != Status::Ok) return s;
    left_ -= 8 + name_size;
    --run_;
    Inode child;
    if (const Status s = fs_.read_inode(uint64_t(start_) << 16 | offset, &child); s != Status::Ok) return s;
    return fs_.fill_entry(child, StringView(name, name_size), path_.view(), entry);
  }
  void rewind() override { reader_ = UniquePointer<MetaReader>(); }

 private:
  SquashFileSystem& fs_;
  Inode inode_;
  String path_;
  UniquePointer<MetaReader> reader_;
  uint64_t left_ = 0;
  uint32_t run_ = 0, start_ = 0, base_ = 0;
};

SquashFileSystem::SquashFileSystem(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset)
    : allocator_(allocator), source_(util::move(source)), offset_(offset), fragments_(allocator), meta_cache_(allocator), data_cache_(allocator) {}

bool SquashFileSystem::looks_like_squash(ConstByteView head) {
  if (head.size() < kSuperblockSize) return false;
  return le32_at(head, sb::kMagicAt) == kMagic && le16_at(head, sb::kVersionMajor) == 4;
}

Status SquashFileSystem::mount(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset, UniquePointer<SquashFileSystem>* result) {
  if (!source) return Status::InvalidArgument;
  UniquePointer<SquashFileSystem> fs = UniquePointer<SquashFileSystem>::make(allocator, allocator, util::move(source), offset);
  if (!fs) return Status::OutOfMemory;
  if (const Status s = fs->load(); s != Status::Ok) return s;
  *result = util::move(fs);
  return Status::Ok;
}

Status SquashFileSystem::load() {
  const uint64_t file_size = source_->size();
  if (offset_ > file_size || file_size - offset_ < kSuperblockSize) return Status::Corrupt;
  bytes_used_ = kSuperblockSize;      // (for read_raw of the superblock itself)
  uint8_t raw[kSuperblockSize];
  if (const Status s = read_raw(0, ByteView(raw)); s != Status::Ok) return s;
  const ConstByteView h(raw);
  if (le32_at(h, sb::kMagicAt) != kMagic) return Status::Corrupt;
  if (le16_at(h, sb::kVersionMajor) != 4 || le16_at(h, sb::kVersionMinor) != 0) return Status::Unsupported;
  block_size_ = le32_at(h, sb::kBlockSize);
  const uint16_t block_log = le16_at(h, sb::kBlockLog);
  if (block_size_ < kMinBlockSize || block_size_ > kMaxBlockSize || block_log >= 32 || (1u << block_log) != block_size_) return Status::Corrupt;
  const uint16_t compression = le16_at(h, sb::kCompression);
  if (compression < 1 || compression > 6) return Status::Unsupported;
  compression_ = static_cast<Compression>(compression);
  inode_count_ = le32_at(h, sb::kInodeCount);
  fragment_count_ = le32_at(h, sb::kFragmentCount);
  modified_ = le32_at(h, sb::kModified);
  flags_ = le16_at(h, sb::kFlags);
  root_ref_ = le64_at(h, sb::kRootInode);
  const uint64_t bytes_used = le64_at(h, sb::kBytesUsed);
  if (bytes_used < kSuperblockSize || bytes_used > file_size - offset_) return Status::Corrupt;
  bytes_used_ = bytes_used;
  inode_table_ = le64_at(h, sb::kInodeTable);
  directory_table_ = le64_at(h, sb::kDirectoryTable);
  const uint64_t fragment_table = le64_at(h, sb::kFragmentTable);
  if (inode_table_ < kSuperblockSize || inode_table_ >= directory_table_ || directory_table_ >= bytes_used_) return Status::Corrupt;
  if ((root_ref_ & 0xFFFF) >= kMetadataSize) return Status::Corrupt;

  // The caches.
  for (uint32_t i = 0; i < kMetaCacheEntries + kDataCacheEntries; ++i) {
    UniquePointer<CacheEntry> entry = UniquePointer<CacheEntry>::make(allocator_, allocator_);
    if (!entry) return Status::OutOfMemory;
    if (!(i < kMetaCacheEntries ? meta_cache_ : data_cache_).push_back(util::move(entry))) return Status::OutOfMemory;
  }

  // The fragment table: an array of pointers to metadata blocks of 16-byte entries.
  if (fragment_count_ != 0 && !(flags_ & flag::kNoFragments)) {
    if (fragment_count_ > (1u << 24)) return Status::Corrupt;
    const uint64_t blocks = (uint64_t(fragment_count_) + kFragmentsPerBlock - 1) / kFragmentsPerBlock;
    if (fragment_table > bytes_used_ || blocks * 8 > bytes_used_ - fragment_table) return Status::Corrupt;
    ByteBuffer pointers(allocator_), block(allocator_);
    if (!pointers.put_zeros(blocks * 8)) return Status::OutOfMemory;
    if (const Status s = read_raw(fragment_table, pointers.writable_view()); s != Status::Ok) return s;
    if (!fragments_.reserve(fragment_count_)) return Status::OutOfMemory;
    uint64_t left = fragment_count_;
    for (uint64_t b = 0; b < blocks; ++b) {
      uint32_t disk = 0;
      if (const Status s = metadata_block(le64_at(pointers.view(), b * 8), &block, &disk); s != Status::Ok) return s;
      const uint64_t here = left < kFragmentsPerBlock ? left : kFragmentsPerBlock;
      if (block.size() < here * kFragmentEntrySize) return Status::Corrupt;
      for (uint64_t i = 0; i < here; ++i) {
        Fragment fragment;
        fragment.start = le64_at(block.view(), i * kFragmentEntrySize);
        fragment.size = le32_at(block.view(), i * kFragmentEntrySize + 8);
        const uint32_t disk_bytes = fragment.size & kDataSizeMask;
        if (disk_bytes == 0 || disk_bytes > block_size_ || fragment.start > bytes_used_ || disk_bytes > bytes_used_ - fragment.start) return Status::Corrupt;
        if (!fragments_.push_back(fragment)) return Status::OutOfMemory;
      }
      left -= here;
    }
  } else {
    fragment_count_ = 0;
  }

  Inode root;
  if (const Status s = read_inode(root_ref_, &root); s != Status::Ok) return s;
  if (root.kind != Kind::Directory) return Status::Corrupt;
  return Status::Ok;
}

FileSystemInfo SquashFileSystem::info() const {
  FileSystemInfo info;
  info.type_name.assign("squashfs");
  info.total_bytes = bytes_used_;
  info.free_bytes = 0;
  info.read_only = true;
  return info;
}

Status SquashFileSystem::read_raw(uint64_t position, ByteView out) const {
  if (position > bytes_used_ || out.size() > bytes_used_ - position) return Status::Corrupt;
  uint64_t done = 0;
  while (done < out.size()) {
    uint64_t got = 0;
    const Status s = source_->read(offset_ + position + done, out.subview(done), &got);
    if (s != Status::Ok) return s;
    if (got == 0) return Status::IoError;
    done += got;
  }
  return Status::Ok;
}

Status SquashFileSystem::decompress(ConstByteView in, ByteBuffer* out, uint64_t max_output) const {
  out->clear();
  bool ok = false;
  switch (compression_) {
    case Compression::Gzip: ok = media::inflate(in, out, max_output); break;      // (a zlib stream, despite the name)
    case Compression::Lzma: ok = media::lzma_decompress(in, out, max_output); break;
    case Compression::Lzo: ok = media::lzo1x_decompress(in, out, max_output); break;
    case Compression::Xz: ok = media::xz_decompress(in, out, max_output); break;
    case Compression::Lz4: ok = media::lz4_block_decompress(in, out, max_output); break;
    case Compression::Zstd: ok = media::zstd_decompress(in, out, max_output); break;
  }
  return ok ? Status::Ok : Status::Corrupt;
}

Status SquashFileSystem::metadata_block(uint64_t position, ByteBuffer* out, uint32_t* disk_size) {
  {
    kernel::ScopedMutex lock(cache_mutex_);
    for (UniquePointer<CacheEntry>& entry : meta_cache_.view()) {
      if (entry->position != position) continue;
      entry->stamp = ++clock_;
      out->clear();
      if (!out->append(entry->data.view())) return Status::OutOfMemory;
      *disk_size = entry->disk_size;
      return Status::Ok;
    }
  }
  uint8_t header[2];
  if (const Status s = read_raw(position, ByteView(header)); s != Status::Ok) return s;
  const uint16_t word = le16_at(ConstByteView(header), 0);
  const uint32_t size = word & ~kMetadataUncompressed;
  if (size == 0 || size > kMetadataSize) return Status::Corrupt;
  ByteBuffer raw(allocator_);
  if (!raw.put_zeros(size)) return Status::OutOfMemory;
  if (const Status s = read_raw(position + 2, raw.writable_view()); s != Status::Ok) return s;
  if (word & kMetadataUncompressed) {
    out->clear();
    if (!out->append(raw.view())) return Status::OutOfMemory;
  } else if (const Status s = decompress(raw.view(), out, kMetadataSize); s != Status::Ok) {
    return s;
  }
  *disk_size = 2 + size;
  kernel::ScopedMutex lock(cache_mutex_);
  CacheEntry* oldest = nullptr;
  for (UniquePointer<CacheEntry>& entry : meta_cache_.view()) {
    if (entry->position == position) return Status::Ok;      // (another reader got there first)
    if (!oldest || entry->stamp < oldest->stamp) oldest = entry.get();
  }
  if (oldest) {
    oldest->position = ~uint64_t{0};
    oldest->data.clear();
    if (oldest->data.append(out->view())) {
      oldest->position = position;
      oldest->disk_size = *disk_size;
      oldest->stamp = ++clock_;
    }
  }
  return Status::Ok;
}

Status SquashFileSystem::read_data_block(uint64_t position, uint32_t size_word, uint32_t expected, uint64_t offset, ByteView out) {
  if (offset > expected || out.size() > expected - offset) return Status::Corrupt;
  if (size_word == 0) {      // a hole
    out.fill(0);
    return Status::Ok;
  }
  const uint32_t disk = size_word & kDataSizeMask;
  if (disk == 0 || disk > block_size_) return Status::Corrupt;
  {
    kernel::ScopedMutex lock(cache_mutex_);
    for (UniquePointer<CacheEntry>& entry : data_cache_.view()) {
      if (entry->position != position) continue;
      if (entry->data.size() < expected) return Status::Corrupt;
      entry->stamp = ++clock_;
      out.copy_from(entry->data.view().subview(offset, out.size()));
      return Status::Ok;
    }
  }
  ByteBuffer raw(allocator_), data(allocator_);
  if (!raw.put_zeros(disk)) return Status::OutOfMemory;
  if (const Status s = read_raw(position, raw.writable_view()); s != Status::Ok) return s;
  ByteBuffer* block = &raw;
  if (!(size_word & kDataUncompressed)) {
    if (const Status s = decompress(raw.view(), &data, block_size_); s != Status::Ok) return s;
    block = &data;
  }
  if (block->size() < expected) return Status::Corrupt;
  out.copy_from(block->view().subview(offset, out.size()));
  kernel::ScopedMutex lock(cache_mutex_);
  CacheEntry* oldest = nullptr;
  for (UniquePointer<CacheEntry>& entry : data_cache_.view()) {
    if (entry->position == position) return Status::Ok;
    if (!oldest || entry->stamp < oldest->stamp) oldest = entry.get();
  }
  if (oldest) {
    oldest->position = ~uint64_t{0};
    oldest->data.clear();
    if (oldest->data.append(block->view())) {
      oldest->position = position;
      oldest->stamp = ++clock_;
    }
  }
  return Status::Ok;
}

Status SquashFileSystem::fragment(uint32_t index, Fragment* out) const {
  if (index >= fragments_.count()) return Status::Corrupt;
  *out = fragments_.view()[index];
  return Status::Ok;
}

Status SquashFileSystem::read_inode(uint64_t ref, Inode* inode) {
  const uint32_t offset = static_cast<uint32_t>(ref & 0xFFFF);
  if (offset >= kMetadataSize) return Status::Corrupt;
  MetaReader reader(*this, inode_table_, ref >> 16, offset);
  uint8_t header[16];
  if (const Status s = reader.read(ByteView(header)); s != Status::Ok) return s;
  const ConstByteView h(header);
  *inode = Inode();
  inode->ref = ref;
  const uint16_t type = le16_at(h, 0);
  inode->mode = le16_at(h, 2);
  inode->mtime = le32_at(h, 8);
  inode->number = le32_at(h, 12);
  uint8_t body[40];
  const ConstByteView b(body);
  auto take = [&](uint32_t bytes) { return reader.read(ByteView(body).first(bytes)); };
  Status s = Status::Ok;
  switch (static_cast<InodeType>(type)) {
    case InodeType::Directory:
      if ((s = take(16)) != Status::Ok) return s;
      inode->kind = Kind::Directory;
      inode->listing_block = le32_at(b, 0);
      inode->links = le32_at(b, 4);
      inode->listing_size = le16_at(b, 8) >= 3 ? le16_at(b, 8) - 3u : 0;
      inode->listing_offset = le16_at(b, 10);
      break;
    case InodeType::ExtDirectory:
      if ((s = take(24)) != Status::Ok) return s;
      inode->kind = Kind::Directory;
      inode->links = le32_at(b, 0);
      inode->listing_size = le32_at(b, 4) >= 3 ? le32_at(b, 4) - 3u : 0;
      inode->listing_block = le32_at(b, 8);
      inode->listing_offset = le16_at(b, 18);
      break;
    case InodeType::File:
      if ((s = take(16)) != Status::Ok) return s;
      inode->kind = Kind::File;
      inode->blocks_start = le32_at(b, 0);
      inode->fragment = le32_at(b, 4);
      inode->fragment_offset = le32_at(b, 8);
      inode->size = le32_at(b, 12);
      break;
    case InodeType::ExtFile:
      if ((s = take(40)) != Status::Ok) return s;
      inode->kind = Kind::File;
      inode->blocks_start = le64_at(b, 0);
      inode->size = le64_at(b, 8);
      inode->links = le32_at(b, 24);
      inode->fragment = le32_at(b, 28);
      inode->fragment_offset = le32_at(b, 32);
      break;
    case InodeType::Symlink:
    case InodeType::ExtSymlink:
      if ((s = take(8)) != Status::Ok) return s;
      inode->kind = Kind::Symlink;
      inode->links = le32_at(b, 0);
      inode->target_size = le32_at(b, 4);
      if (inode->target_size == 0 || inode->target_size > kMaxTarget) return Status::Corrupt;
      break;
    case InodeType::BlockDevice:
    case InodeType::CharDevice:
      if ((s = take(8)) != Status::Ok) return s;
      inode->links = le32_at(b, 0);
      break;
    case InodeType::ExtBlockDevice:
    case InodeType::ExtCharDevice:
      if ((s = take(12)) != Status::Ok) return s;
      inode->links = le32_at(b, 0);
      break;
    case InodeType::Fifo:
    case InodeType::Socket:
      if ((s = take(4)) != Status::Ok) return s;
      inode->links = le32_at(b, 0);
      break;
    case InodeType::ExtFifo:
    case InodeType::ExtSocket:
      if ((s = take(8)) != Status::Ok) return s;
      inode->links = le32_at(b, 0);
      break;
    default: return Status::Corrupt;
  }
  if (inode->kind == Kind::File) {
    const uint64_t bs = block_size_;
    const bool has_fragment = inode->fragment != kNoFragment;
    inode->block_count = has_fragment ? inode->size / bs : (inode->size + bs - 1) / bs;
    if (inode->size > (uint64_t{1} << 48)) return Status::Corrupt;
    if (has_fragment) {
      const uint64_t tail = inode->size % bs;
      if (tail == 0 || inode->fragment >= fragment_count_ || inode->fragment_offset > bs - tail) return Status::Corrupt;
    }
  }
  if (inode->kind == Kind::Directory && inode->listing_offset >= kMetadataSize) return Status::Corrupt;
  inode->list_block = reader.block();
  inode->list_offset = reader.offset();
  return Status::Ok;
}

Status SquashFileSystem::read_target(const Inode& link, String* target) {
  MetaReader reader(*this, inode_table_, link.list_block, link.list_offset);
  char text[kMaxTarget];
  if (const Status s = reader.read(char_bytes(text, link.target_size)); s != Status::Ok) return s;
  return target->assign(StringView(text, link.target_size)) ? Status::Ok : Status::OutOfMemory;
}

Status SquashFileSystem::lookup(const Inode& directory, StringView name, Inode* found) {
  MetaReader reader(*this, directory_table_, directory.listing_block, directory.listing_offset);
  uint64_t left = directory.listing_size;
  char entry_name[kMaxNameLength + 1];
  while (left >= 12) {
    uint8_t header[12];
    if (const Status s = reader.read(ByteView(header)); s != Status::Ok) return s;
    left -= 12;
    const ConstByteView h(header);
    const uint32_t count = le32_at(h, 0) + 1, start = le32_at(h, 4);
    if (count > kMaxDirectoryRun) return Status::Corrupt;
    for (uint32_t i = 0; i < count; ++i) {
      uint8_t raw[8];
      if (left < 8) return Status::Corrupt;
      if (const Status s = reader.read(ByteView(raw)); s != Status::Ok) return s;
      const ConstByteView e(raw);
      const uint16_t offset = le16_at(e, 0);
      const uint32_t name_size = uint32_t(le16_at(e, 6)) + 1;
      if (offset >= kMetadataSize || name_size > kMaxNameLength || left - 8 < name_size) return Status::Corrupt;
      left -= 8 + name_size;
      if (name_size != name.size()) {
        if (const Status s = reader.skip(name_size); s != Status::Ok) return s;
        continue;
      }
      if (const Status s = reader.read(char_bytes(entry_name, name_size)); s != Status::Ok) return s;
      if (StringView(entry_name, name_size).equals(name)) return read_inode(uint64_t(start) << 16 | offset, found);
    }
  }
  return Status::NotFound;
}

struct SquashFileSystem::WalkOps {
  SquashFileSystem& fs;
  Status lookup(const Inode& directory, StringView name, Inode* child) { return fs.lookup(directory, name, child); }
  bool is_directory(const Inode& inode) const { return inode.kind == Kind::Directory; }
  bool is_link(const Inode& inode) const { return inode.kind == Kind::Symlink; }
  Status target(const Inode& link, String* text) { return fs.read_target(link, text); }
};

Status SquashFileSystem::resolve(StringView path, bool follow_last, Inode* inode, String* exit) {
  Inode root;
  if (const Status s = read_inode(root_ref_, &root); s != Status::Ok) return s;
  WalkOps ops{*this};
  return walk_path(allocator_, path, follow_last, root, ops, inode, exit);
}

Status SquashFileSystem::resolve_exit(StringView path, bool follow_last, String* exit) {
  Inode inode;
  return resolve(path, follow_last, &inode, exit);
}

Status SquashFileSystem::fill_entry(const Inode& inode, StringView name, StringView parent_path, DirectoryEntry* entry) {
  *entry = DirectoryEntry();
  entry->name.assign(name);
  entry->read_only = true;
  entry->modified = inode.mtime;
  entry->file_id = inode.number;
  entry->link_count = inode.links;
  switch (inode.kind) {
    case Kind::Directory: entry->type = NodeType::Directory; break;
    case Kind::File: entry->type = NodeType::File; entry->size = inode.size; break;
    case Kind::Other: entry->type = NodeType::File; break;
    case Kind::Symlink: {
      // A listing shows what the link points to (File when that cannot be found).
      entry->is_symlink = true;
      entry->type = NodeType::File;
      String path(allocator_);
      if (!path.assign(parent_path) || !path.push_back('/') || !path.append(name)) return Status::OutOfMemory;
      Inode target;
      if (resolve(path.view(), true, &target) == Status::Ok) {
        entry->type = target.kind == Kind::Directory ? NodeType::Directory : NodeType::File;
        if (target.kind == Kind::File) entry->size = target.size;
      }
      break;
    }
  }
  return Status::Ok;
}

Status SquashFileSystem::stat(StringView path, DirectoryEntry* entry) {
  Inode inode;
  if (const Status s = resolve(path, false, &inode); s != Status::Ok) return s;
  const bool is_link = inode.kind == Kind::Symlink;
  if (is_link) {
    if (const Status s = resolve(path, true, &inode); s != Status::Ok) return s;
  }
  if (const Status s = fill_entry(inode, link_walk::last_component(path), StringView(), entry); s != Status::Ok) return s;
  entry->is_symlink = is_link;
  return Status::Ok;
}

Status SquashFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  Inode inode;
  const Status s = resolve(path, true, &inode);
  if (s != Status::Ok) return s == Status::NotFound && mode != OpenMode::Read && mode != OpenMode::ReadWrite ? Status::ReadOnly : s;
  if (inode.kind == Kind::Directory) return Status::IsADirectory;
  if (mode != OpenMode::Read) return Status::ReadOnly;
  UniquePointer<File> opened = UniquePointer<File>::make(allocator_, *this, inode);
  if (!opened) return Status::OutOfMemory;
  if (const Status l = opened->load(); l != Status::Ok) return l;
  *file = util::move(opened);
  return Status::Ok;
}

Status SquashFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  Inode inode;
  if (const Status s = resolve(path, true, &inode); s != Status::Ok) return s;
  if (inode.kind != Kind::Directory) return Status::NotADirectory;
  UniquePointer<Directory> opened = UniquePointer<Directory>::make(allocator_, *this, inode);
  if (!opened || !opened->set_path(path)) return Status::OutOfMemory;
  *directory = util::move(opened);
  return Status::Ok;
}

Status SquashFileSystem::read_symbolic_link(StringView path, String* target, bool* target_is_directory) {
  Inode inode;
  if (const Status s = resolve(path, false, &inode); s != Status::Ok) return s;
  if (inode.kind != Kind::Symlink) return Status::InvalidArgument;
  if (const Status s = read_target(inode, target); s != Status::Ok) return s;
  Inode pointed;
  *target_is_directory = resolve(path, true, &pointed) == Status::Ok && pointed.kind == Kind::Directory;
  return Status::Ok;
}

}  // namespace filesystem
