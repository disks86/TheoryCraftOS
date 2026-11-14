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

#include "FileSystem/Dwarfs/DwarfsFileSystem.hpp"
#include "FileSystem/LinkWalk.hpp"
#include "Media/Lz4.hpp"
#include "Media/Xxh3.hpp"
#include "Media/Xz.hpp"
#include "Media/Zstd.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace filesystem {

using namespace dwarfs;

namespace {

// The section header: "DWARFS", major, minor, SHA-512/256 (not checked), XXH3-64 of the rest (from 0x30 to the end of the data),
// section number, type, compression, data length.
constexpr uint64_t kHeaderSize = 64, kChecksumAt = 0x28, kCheckedFrom = 0x30;
constexpr uint8_t kMajor = 2, kMinMinor = 2, kMaxMinor = 5;
enum SectionType : uint16_t { kBlock = 0, kSchema = 7, kMetadata = 8, kSectionIndex = 9, kHistory = 10 };
enum Compression : uint16_t { kNone = 0, kLzma = 1, kZstd = 2, kLz4 = 3, kLz4Hc = 4 };

constexpr uint64_t kMaxSchema = uint64_t{1} << 20, kMaxMetadata = uint64_t{512} << 20;
// The decompressed blocks kept: as many as fit this budget, at least two, at most sixteen.
constexpr uint64_t kCacheBudget = uint64_t{64} << 20;
constexpr uint32_t kMinCacheEntries = 2, kMaxCacheEntries = 16;
constexpr uint32_t kMaxTarget = 4096;

// POSIX file types of the inodes that are neither directories, links nor regular files.
constexpr uint32_t kTypeFifo = 0010000, kTypeCharDevice = 0020000, kTypeBlockDevice = 0060000, kTypeSocket = 0140000;

uint64_t le_at(ConstByteView bytes, uint64_t at, uint32_t size) {
  ByteReader reader(bytes.subview(at, size));
  return size == 2 ? reader.le16() : size == 4 ? reader.le32() : reader.le64();
}

// The bytes of a decoded name or target as text. Safe: char and uint8_t have the same size, alignment and representation, and the view
// keeps the bounds of the buffer it was made from (which outlives it).
StringView as_text(ConstByteView bytes) { return StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }

}  // namespace

class DwarfsFileSystem::File final : public IFile {
 public:
  File(const DwarfsFileSystem& fs, uint32_t first, uint32_t last) : fs_(fs), first_(first), last_(last), starts_(fs.allocator_) {}

  // Where each chunk starts in the file (and the size at the end), for finding the chunk of an offset.
  Status load() {
    if (!starts_.reserve(uint64_t(last_ - first_) + 1) || !starts_.push_back(0)) return Status::OutOfMemory;
    uint64_t total = 0;
    for (uint32_t c = first_; c < last_; ++c) {
      uint64_t length = 0;
      if (const Status s = fs_.chunk_length(fs_.meta_.chunks[c], &length); s != Status::Ok) return s;
      if (length > ~uint64_t{0} - total) return Status::Corrupt;
      total += length;
      if (!starts_.push_back(total)) return Status::OutOfMemory;
    }
    return Status::Ok;
  }

  uint64_t size() const override { return starts_[starts_.count() - 1]; }

  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    *transferred = 0;
    const uint64_t total = size();
    if (offset >= total || buffer.empty()) return Status::Ok;
    const uint64_t want = total - offset < buffer.size() ? total - offset : buffer.size();
    // The chunk holding `offset`: the last start at or before it.
    uint64_t low = 0, high = starts_.count() - 1;
    while (high - low > 1) {
      const uint64_t mid = low + (high - low) / 2;
      if (starts_[mid] <= offset) low = mid;
      else high = mid;
    }
    uint64_t done = 0;
    for (uint64_t index = low; done < want; ++index) {
      const Chunk& chunk = fs_.meta_.chunks[first_ + index];
      const uint64_t at = offset + done, within = at - starts_[index], length = starts_[index + 1] - starts_[index];
      if (within >= length) continue;      // (an empty chunk)
      const uint64_t n = length - within < want - done ? length - within : want - done;
      const ByteView part = buffer.subview(done, n);
      if (fs_.is_hole(chunk)) {
        part.fill(0);
      } else if (const Status s = fs_.read_block(chunk.block, uint64_t(chunk.offset) + within, part); s != Status::Ok) {
        return s;
      }
      done += n;
      *transferred = done;
    }
    return Status::Ok;
  }
  Status write(uint64_t, ConstByteView, uint64_t* transferred) override { *transferred = 0; return Status::ReadOnly; }
  Status resize(uint64_t) override { return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }

 private:
  const DwarfsFileSystem& fs_;
  uint32_t first_, last_;
  List<uint64_t> starts_;
};

class DwarfsFileSystem::Directory final : public IDirectory {
 public:
  Directory(const DwarfsFileSystem& fs, uint32_t first, uint32_t last) : fs_(fs), first_(first), last_(last), next_(first), path_(fs.allocator_), name_(fs.allocator_) {}
  bool set_path(StringView path) { return path_.assign(path); }

  Status next(DirectoryEntry* entry) override {
    while (next_ < last_) {
      const uint32_t index = next_++;
      if (const Status s = fs_.entry_name(index, &name_); s != Status::Ok) return s;
      if (name_.size() == 0) continue;      // (mkdwarfs before 0.15.4 could write entries without a name, from --input-list; its reader skips them too)
      return fs_.fill_entry(Node{fs_.meta_.entries[index].inode}, as_text(name_.view()), path_.view(), entry);
    }
    return Status::EndOfDirectory;
  }
  void rewind() override { next_ = first_; }

 private:
  const DwarfsFileSystem& fs_;
  uint32_t first_, last_, next_;
  String path_;
  ByteBuffer name_;
};

DwarfsFileSystem::DwarfsFileSystem(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset)
    : allocator_(allocator), source_(util::move(source)), offset_(offset), meta_(allocator), blocks_(allocator), cache_(allocator) {}

bool DwarfsFileSystem::looks_like_dwarfs(ConstByteView head) {
  if (head.size() < 8) return false;
  const char magic[6] = {'D', 'W', 'A', 'R', 'F', 'S'};
  for (int i = 0; i < 6; ++i)
    if (head[i] != uint8_t(magic[i])) return false;
  return head[6] == kMajor && head[7] >= kMinMinor && head[7] <= kMaxMinor;
}

Status DwarfsFileSystem::mount(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset, UniquePointer<DwarfsFileSystem>* result) {
  if (!source) return Status::InvalidArgument;
  UniquePointer<DwarfsFileSystem> fs = UniquePointer<DwarfsFileSystem>::make(allocator, allocator, util::move(source), offset);
  if (!fs) return Status::OutOfMemory;
  if (const Status s = fs->load(); s != Status::Ok) return s;
  *result = util::move(fs);
  return Status::Ok;
}

Status DwarfsFileSystem::read_raw(uint64_t position, ByteView out) const {
  if (position > image_size_ || out.size() > image_size_ - position) return Status::Corrupt;
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

Status DwarfsFileSystem::read_header(uint64_t position, Section* section) const {
  if (position > image_size_ || image_size_ - position < kHeaderSize) return Status::Corrupt;
  uint8_t raw[kHeaderSize];
  if (const Status s = read_raw(position, ByteView(raw)); s != Status::Ok) return s;
  const ConstByteView h(raw);
  if (!looks_like_dwarfs(h)) {
    // A format this reader does not know: older (2.0 and 2.1 had another section header) or newer.
    const bool magic = h[0] == 'D' && h[1] == 'W' && h[2] == 'A' && h[3] == 'R' && h[4] == 'F' && h[5] == 'S';
    return magic ? Status::Unsupported : Status::Corrupt;
  }
  section->position = position;
  section->checksum = le_at(h, kChecksumAt, 8);
  section->type = static_cast<uint16_t>(le_at(h, 0x34, 2));
  section->compression = static_cast<uint16_t>(le_at(h, 0x36, 2));
  section->length = le_at(h, 0x38, 8);
  if (section->length > image_size_ - position - kHeaderSize) return Status::Corrupt;
  return Status::Ok;
}

Status DwarfsFileSystem::section_data(const Section& section, uint64_t max_output, ByteBuffer* out) const {
  out->clear();
  // The checked part: the last 16 bytes of the header and the data.
  const uint64_t checked = kHeaderSize - kCheckedFrom + section.length;
  ByteBuffer raw(allocator_);
  if (!raw.put_zeros(checked)) return Status::OutOfMemory;
  if (const Status s = read_raw(section.position + kCheckedFrom, raw.writable_view()); s != Status::Ok) return s;
  if (media::xxh3_64(raw.view()) != section.checksum) return Status::Corrupt;
  const ConstByteView in = raw.view().subview(kHeaderSize - kCheckedFrom);
  bool ok = false;
  switch (section.compression) {
    case kNone:
      if (in.size() > max_output) return Status::Corrupt;
      ok = out->append(in);
      if (!ok) return Status::OutOfMemory;
      break;
    case kLzma: ok = media::xz_decompress(in, out, max_output); break;
    case kZstd: ok = media::zstd_decompress(in, out, max_output); break;
    case kLz4:
    case kLz4Hc: {
      // The decompressed size (32 bits) and a raw LZ4 block.
      if (in.size() < 4) return Status::Corrupt;
      const uint64_t size = le_at(in, 0, 4);
      if (size > max_output) return Status::Corrupt;
      ok = media::lz4_block_decompress(in.subview(4), out, size) && out->size() == size;
      break;
    }
    default: return Status::Unsupported;      // Brotli, FLAC, ricepp
  }
  return ok ? Status::Ok : Status::Corrupt;
}

Status DwarfsFileSystem::load() {
  const uint64_t file_size = source_->size();
  if (offset_ > file_size) return Status::Corrupt;
  image_size_ = file_size - offset_;

  // The sections, one after the other to the end of the file.
  Section schema, metadata;
  bool have_schema = false, have_metadata = false;
  for (uint64_t position = 0; position < image_size_;) {
    Section section;
    if (const Status s = read_header(position, &section); s != Status::Ok) return s;
    if (section.type == kBlock) {
      if (blocks_.count() >= 0xFFFFFFFFu) return Status::Corrupt;
      if (!blocks_.push_back(section)) return Status::OutOfMemory;
    } else if (section.type == kSchema || section.type == kMetadata) {
      bool& have = section.type == kSchema ? have_schema : have_metadata;
      if (have) return Status::Corrupt;
      have = true;
      (section.type == kSchema ? schema : metadata) = section;
    }
    position += kHeaderSize + section.length;
  }
  if (!have_schema || !have_metadata) return Status::Corrupt;

  {
    ByteBuffer schema_bytes(allocator_), metadata_bytes(allocator_);
    if (const Status s = section_data(schema, kMaxSchema, &schema_bytes); s != Status::Ok) return s;
    if (const Status s = section_data(metadata, kMaxMetadata, &metadata_bytes); s != Status::Ok) return s;
    if (const Status s = decode_metadata(allocator_, schema_bytes.view(), metadata_bytes.view(), static_cast<uint32_t>(blocks_.count()), &meta_);
        s != Status::Ok)
      return s;
  }

  uint64_t entries = kCacheBudget / meta_.block_size;
  if (entries < kMinCacheEntries) entries = kMinCacheEntries;
  if (entries > kMaxCacheEntries) entries = kMaxCacheEntries;
  for (uint64_t i = 0; i < entries; ++i) {
    UniquePointer<CacheEntry> entry = UniquePointer<CacheEntry>::make(allocator_, allocator_);
    if (!entry || !cache_.push_back(util::move(entry))) return Status::OutOfMemory;
  }
  return Status::Ok;
}

FileSystemInfo DwarfsFileSystem::info() const {
  FileSystemInfo info;
  info.type_name.assign("dwarfs");
  info.total_bytes = image_size_;
  info.free_bytes = 0;
  info.read_only = true;
  return info;
}

Status DwarfsFileSystem::read_block(uint32_t block, uint64_t offset, ByteView out) const {
  if (block >= blocks_.count()) return Status::Corrupt;
  {
    kernel::ScopedMutex lock(cache_mutex_);
    for (UniquePointer<CacheEntry>& entry : cache_.view()) {
      if (entry->block != block) continue;
      const ConstByteView data = entry->data.view();
      if (offset > data.size() || out.size() > data.size() - offset) return Status::Corrupt;
      entry->stamp = ++clock_;
      out.copy_from(data.subview(offset, out.size()));
      return Status::Ok;
    }
  }
  UniquePointer<CacheEntry> fresh = UniquePointer<CacheEntry>::make(allocator_, allocator_);
  if (!fresh) return Status::OutOfMemory;
  if (const Status s = section_data(blocks_[block], meta_.block_size, &fresh->data); s != Status::Ok) return s;
  const ConstByteView data = fresh->data.view();
  if (offset > data.size() || out.size() > data.size() - offset) return Status::Corrupt;
  out.copy_from(data.subview(offset, out.size()));
  fresh->block = block;

  // Into the cache in place of the least recently used block (which is freed after the lock is let go).
  UniquePointer<CacheEntry> evicted;
  kernel::ScopedMutex lock(cache_mutex_);
  uint64_t oldest = 0;
  for (uint64_t i = 0; i < cache_.count(); ++i) {
    if (cache_[i]->block == block) return Status::Ok;      // (another reader got there first)
    if (cache_[i]->stamp < cache_[oldest]->stamp) oldest = i;
  }
  if (cache_.count() == 0) return Status::Ok;
  fresh->stamp = ++clock_;
  evicted = util::move(cache_[oldest]);
  cache_[oldest] = util::move(fresh);
  return Status::Ok;
}

DwarfsFileSystem::Kind DwarfsFileSystem::kind(uint32_t inode) const {
  uint64_t bound = meta_.directory_count;
  if (inode < bound) return Kind::Directory;
  bound += meta_.link_count;
  if (inode < bound) return Kind::Link;
  bound += meta_.file_count;
  if (inode < bound) return Kind::File;
  return Kind::Other;
}

Status DwarfsFileSystem::file_chunks(uint32_t inode, uint32_t* first, uint32_t* last) const {
  if (kind(inode) != Kind::File) return Status::InvalidArgument;
  const uint64_t k = uint64_t(inode) - meta_.directory_count - meta_.link_count;
  uint64_t index = k;
  if (k >= meta_.unique_files) index = uint64_t(meta_.shared_files[k - meta_.unique_files]) + meta_.unique_files;
  if (index + 1 >= meta_.chunk_table.count()) return Status::Corrupt;
  *first = meta_.chunk_table[index];
  *last = meta_.chunk_table[index + 1];
  return Status::Ok;
}

Status DwarfsFileSystem::chunk_length(const Chunk& chunk, uint64_t* length) const {
  if (!is_hole(chunk)) {
    *length = chunk.size;
    return Status::Ok;
  }
  // A hole: size * block size + offset, or (offset = block size - 1) an index into the large holes.
  if (chunk.offset == meta_.block_size - 1) {
    if (chunk.size >= meta_.large_holes.count()) return Status::Corrupt;
    *length = meta_.large_holes[chunk.size];
  } else {
    *length = uint64_t(chunk.size) * meta_.block_size + chunk.offset;
  }
  return Status::Ok;
}

Status DwarfsFileSystem::file_size(uint32_t inode, uint64_t* size) const {
  uint32_t first, last;
  if (const Status s = file_chunks(inode, &first, &last); s != Status::Ok) return s;
  uint64_t total = 0;
  for (uint32_t c = first; c < last; ++c) {
    uint64_t length = 0;
    if (const Status s = chunk_length(meta_.chunks[c], &length); s != Status::Ok) return s;
    if (length > ~uint64_t{0} - total) return Status::Corrupt;
    total += length;
  }
  *size = total;
  return Status::Ok;
}

Status DwarfsFileSystem::entry_name(uint32_t entry, ByteBuffer* out) const {
  if (entry == 0 || entry >= meta_.entries.count()) return Status::Corrupt;      // (entry 0 is the root, which has no name)
  if (!meta_.names.get(meta_.entries[entry].name, out)) return Status::Corrupt;
  if (out->size() > kMaxNameLength) return Status::Corrupt;
  // A name is one path component: no '/' or NUL in it, and not "." or ".." (which would lead a walk back up the tree).
  const StringView name = as_text(out->view());
  if (name.equals(".") || name.equals("..")) return Status::Corrupt;
  for (const char c : name)
    if (c == '/' || c == '\0') return Status::Corrupt;
  return Status::Ok;
}

Status DwarfsFileSystem::lookup(const Node& directory, StringView name, Node* found) const {
  if (kind(directory.inode) != Kind::Directory) return Status::NotADirectory;
  // The entries of a directory are sorted by name (as bytes).
  uint64_t low = meta_.directories[directory.inode], high = meta_.directories[uint64_t(directory.inode) + 1];
  ByteBuffer text(allocator_);
  while (low < high) {
    const uint64_t mid = low + (high - low) / 2;
    if (const Status s = entry_name(static_cast<uint32_t>(mid), &text); s != Status::Ok) return s;
    const int order = as_text(text.view()).compare(name);
    if (order == 0) {
      found->inode = meta_.entries[mid].inode;
      return Status::Ok;
    }
    if (order < 0) low = mid + 1;
    else high = mid;
  }
  return Status::NotFound;
}

Status DwarfsFileSystem::read_target(const Node& link, String* target) const {
  if (kind(link.inode) != Kind::Link) return Status::InvalidArgument;
  const uint32_t index = meta_.symlink_table[link.inode - meta_.directory_count];
  ByteBuffer text(allocator_);
  if (!meta_.links.get(index, &text)) return Status::Corrupt;
  if (text.size() == 0 || text.size() > kMaxTarget) return Status::Corrupt;
  if (!target->assign(as_text(text.view()))) return Status::OutOfMemory;
  if (meta_.backslash_links) {
    String converted(allocator_);
    for (char c : target->view())
      if (!converted.push_back(c == '\\' ? '/' : c)) return Status::OutOfMemory;
    if (!target->assign(converted.view())) return Status::OutOfMemory;
  }
  return Status::Ok;
}

struct DwarfsFileSystem::WalkOps {
  const DwarfsFileSystem& fs;
  Status lookup(const Node& directory, StringView name, Node* child) { return fs.lookup(directory, name, child); }
  bool is_directory(const Node& node) const { return fs.kind(node.inode) == Kind::Directory; }
  bool is_link(const Node& node) const { return fs.kind(node.inode) == Kind::Link; }
  Status target(const Node& link, String* text) { return fs.read_target(link, text); }
};

Status DwarfsFileSystem::resolve(StringView path, bool follow_last, Node* node, String* exit) const {
  const Node root{0};
  WalkOps ops{*this};
  return walk_path(allocator_, path, follow_last, root, ops, node, exit);
}

Status DwarfsFileSystem::resolve_exit(StringView path, bool follow_last, String* exit) {
  Node node;
  return resolve(path, follow_last, &node, exit);
}

Status DwarfsFileSystem::fill_entry(const Node& node, StringView name, StringView parent_path, DirectoryEntry* entry) const {
  if (name.size() > kMaxNameLength) return Status::Corrupt;
  *entry = DirectoryEntry();
  entry->name.assign(name);
  entry->read_only = true;
  const InodeInfo& info = meta_.inodes[node.inode];
  entry->modified = info.modified;
  entry->file_id = uint64_t(node.inode) + 1;
  entry->link_count = info.links;
  switch (kind(node.inode)) {
    case Kind::Directory: entry->type = NodeType::Directory; break;
    case Kind::File: {
      entry->type = NodeType::File;
      if (const Status s = file_size(node.inode, &entry->size); s != Status::Ok) return s;
      break;
    }
    case Kind::Other: {
      entry->type = NodeType::File;
      switch (info.mode & kTypeMask) {
        case kTypeFifo: entry->special = SpecialNode::Fifo; break;
        case kTypeSocket: entry->special = SpecialNode::Socket; break;
        case kTypeCharDevice: entry->special = SpecialNode::CharDevice; break;
        case kTypeBlockDevice: entry->special = SpecialNode::BlockDevice; break;
        default: break;
      }
      break;
    }
    case Kind::Link: {
      // A listing shows what the link points to (File when that cannot be found).
      entry->is_symlink = true;
      entry->type = NodeType::File;
      String path(allocator_);
      if (!path.assign(parent_path) || !path.push_back('/') || !path.append(name)) return Status::OutOfMemory;
      Node target;
      if (resolve(path.view(), true, &target) == Status::Ok) {
        const Kind k = kind(target.inode);
        entry->type = k == Kind::Directory ? NodeType::Directory : NodeType::File;
        if (k == Kind::File && file_size(target.inode, &entry->size) != Status::Ok) entry->size = 0;
      }
      break;
    }
  }
  return Status::Ok;
}

Status DwarfsFileSystem::stat(StringView path, DirectoryEntry* entry) {
  Node node;
  if (const Status s = resolve(path, false, &node); s != Status::Ok) return s;
  const bool is_link = kind(node.inode) == Kind::Link;
  if (is_link) {
    if (const Status s = resolve(path, true, &node); s != Status::Ok) return s;
  }
  if (const Status s = fill_entry(node, link_walk::last_component(path), StringView(), entry); s != Status::Ok) return s;
  entry->is_symlink = is_link;
  return Status::Ok;
}

Status DwarfsFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  Node node;
  const Status s = resolve(path, true, &node);
  if (s != Status::Ok) return s == Status::NotFound && mode != OpenMode::Read && mode != OpenMode::ReadWrite ? Status::ReadOnly : s;
  const Kind k = kind(node.inode);
  if (k == Kind::Directory) return Status::IsADirectory;
  if (mode != OpenMode::Read) return Status::ReadOnly;
  uint32_t first = 0, last = 0;      // (a device, FIFO or socket reads as an empty file)
  if (k == Kind::File) {
    if (const Status c = file_chunks(node.inode, &first, &last); c != Status::Ok) return c;
  }
  UniquePointer<File> opened = UniquePointer<File>::make(allocator_, *this, first, last);
  if (!opened) return Status::OutOfMemory;
  if (const Status l = opened->load(); l != Status::Ok) return l;
  *file = util::move(opened);
  return Status::Ok;
}

Status DwarfsFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  Node node;
  if (const Status s = resolve(path, true, &node); s != Status::Ok) return s;
  if (kind(node.inode) != Kind::Directory) return Status::NotADirectory;
  UniquePointer<Directory> opened =
      UniquePointer<Directory>::make(allocator_, *this, meta_.directories[node.inode], meta_.directories[uint64_t(node.inode) + 1]);
  if (!opened || !opened->set_path(path)) return Status::OutOfMemory;
  *directory = util::move(opened);
  return Status::Ok;
}

Status DwarfsFileSystem::read_symbolic_link(StringView path, String* target, bool* target_is_directory) {
  Node node;
  if (const Status s = resolve(path, false, &node); s != Status::Ok) return s;
  if (kind(node.inode) != Kind::Link) return Status::InvalidArgument;
  if (const Status s = read_target(node, target); s != Status::Ok) return s;
  Node pointed;
  *target_is_directory = resolve(path, true, &pointed) == Status::Ok && kind(pointed.inode) == Kind::Directory;
  return Status::Ok;
}

}  // namespace filesystem
