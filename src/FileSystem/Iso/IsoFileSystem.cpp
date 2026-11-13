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

#include "FileSystem/Iso/IsoFileSystem.hpp"
#include "FileSystem/LinkWalk.hpp"
#include "Kernel/Sync.hpp"
#include "Media/Zlib.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace filesystem {

namespace {

constexpr uint64_t kSector = 2048;                 // volume descriptors and directory records go by 2 KiB sectors
constexpr uint64_t kDescriptorsStart = 16 * kSector;
constexpr uint32_t kMaxDescriptors = 64;
constexpr uint64_t kMaxDirectoryBytes = 32ull << 20;
constexpr uint64_t kMaxContinuation = 1u << 16;
constexpr uint32_t kMaxContinuations = 16;
constexpr uint32_t kModeTypeMask = 0170000, kModeDirectory = 0040000, kModeLink = 0120000;
constexpr uint8_t kFlagDirectory = 0x02, kFlagMultiExtent = 0x80;
constexpr uint8_t kZisofsMagic[8] = {0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07};

uint16_t le16_at(ConstByteView bytes, uint64_t at) {
  ByteReader reader(bytes.subview(at, 2));
  return reader.le16();
}
uint32_t le32_at(ConstByteView bytes, uint64_t at) {
  ByteReader reader(bytes.subview(at, 4));
  return reader.le32();
}

// The 7-byte date of a directory record (and of TF's short form): years since 1900, month, day, hour, minute, second, offset from GMT in 15 minutes.
uint64_t record_time(ConstByteView d) {
  if (d.size() < 7 || d[1] < 1 || d[1] > 12 || d[2] < 1 || d[2] > 31) return 0;
  const int64_t days = days_from_civil(1900 + d[0], d[1], d[2]);
  const int64_t offset = static_cast<int8_t>(d[6]) * 15 * 60;
  const int64_t t = days * 86400 + d[3] * 3600 + d[4] * 60 + d[5] - offset;
  return t > 0 ? static_cast<uint64_t>(t) : 0;
}

// The 17-byte form: "YYYYMMDDHHMMSScc" and the GMT offset.
uint64_t long_time(ConstByteView d) {
  if (d.size() < 17) return 0;
  uint32_t v[7] = {};
  const uint32_t widths[7] = {4, 2, 2, 2, 2, 2, 2};
  uint64_t at = 0;
  for (int i = 0; i < 7; ++i) {
    for (uint32_t k = 0; k < widths[i]; ++k, ++at) {
      if (d[at] < '0' || d[at] > '9') return 0;
      v[i] = v[i] * 10 + (d[at] - '0');
    }
  }
  if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31) return 0;
  const int64_t t = days_from_civil(v[0], v[1], v[2]) * 86400 + v[3] * 3600 + v[4] * 60 + v[5] - static_cast<int8_t>(d[16]) * 15 * 60;
  return t > 0 ? static_cast<uint64_t>(t) : 0;
}

// UCS-2 / UTF-16 big-endian (Joliet) to UTF-8.
bool utf16be_to_utf8(ConstByteView in, String* out) {
  out->clear();
  for (uint64_t i = 0; i + 1 < in.size(); i += 2) {
    uint32_t c = uint32_t(in[i]) << 8 | in[i + 1];
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < in.size()) {
      const uint32_t low = uint32_t(in[i + 2]) << 8 | in[i + 3];
      if (low >= 0xDC00 && low < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
        i += 2;
      }
    }
    char bytes[4];
    uint32_t n;
    if (c < 0x80) { bytes[0] = char(c); n = 1; }
    else if (c < 0x800) { bytes[0] = char(0xC0 | c >> 6); bytes[1] = char(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) { bytes[0] = char(0xE0 | c >> 12); bytes[1] = char(0x80 | ((c >> 6) & 0x3F)); bytes[2] = char(0x80 | (c & 0x3F)); n = 3; }
    else { bytes[0] = char(0xF0 | c >> 18); bytes[1] = char(0x80 | ((c >> 12) & 0x3F)); bytes[2] = char(0x80 | ((c >> 6) & 0x3F)); bytes[3] = char(0x80 | (c & 0x3F)); n = 4; }
    if (!out->append(StringView(bytes, n))) return false;
  }
  return true;
}

// "NAME.EXT;1" -> "NAME.EXT" (and "NAME.;1" -> "NAME").
StringView without_version(StringView name) {
  const uint64_t semicolon = name.rfind(';');
  if (semicolon != StringView::npos) name = name.first(semicolon);
  if (name.size() > 1 && name.ends_with('.')) name = name.first(name.size() - 1);
  return name;
}

}  // namespace

class IsoFileSystem::File final : public IFile {
 public:
  File(const IsoFileSystem& fs, const Node& node) : fs_(fs), node_(node), extents_(fs.allocator_), pointers_(fs.allocator_), block_(fs.allocator_) {}

  struct Extent {
    uint64_t position = 0, size = 0;
  };

  Status load() {
    if (node_.kind != Kind::File) return Status::Ok;
    if (node_.interleaved || node_.sparse) return Status::Unsupported;
    if (node_.multi_extent) {
      if (const Status s = collect_extents(); s != Status::Ok) return s;
    } else {
      const uint64_t stored = node_.zisofs ? node_.stored : node_.size;
      if (const Status s = add_extent(node_.extent, stored); s != Status::Ok) return s;
    }
    if (node_.zisofs) return load_zisofs();
    return Status::Ok;
  }

  uint64_t size() const override { return node_.kind == Kind::File ? node_.size : 0; }

  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    *transferred = 0;
    const uint64_t total = size();
    if (offset >= total || buffer.empty()) return Status::Ok;
    const uint64_t want = total - offset < buffer.size() ? total - offset : buffer.size();
    const ByteView out = buffer.first(want);
    const Status s = node_.zisofs ? read_zisofs(offset, out) : read_stored(offset, out);
    if (s == Status::Ok) *transferred = want;
    return s;
  }
  Status write(uint64_t, ConstByteView, uint64_t* transferred) override { *transferred = 0; return Status::ReadOnly; }
  Status resize(uint64_t) override { return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }

 private:
  Status add_extent(uint64_t block, uint64_t bytes) {
    if (bytes == 0) return Status::Ok;      // (mkisofs gives an empty file the next free block, which may be past the end)
    if (block > fs_.size_ / fs_.block_size_) return Status::Corrupt;
    const uint64_t position = block * fs_.block_size_;
    if (bytes > fs_.size_ - position) return Status::Corrupt;
    Extent extent;
    extent.position = position;
    extent.size = bytes;
    return extents_.push_back(extent) ? Status::Ok : Status::OutOfMemory;
  }

  // A file over several records: they follow each other in its folder, each but the last flagged "multi-extent".
  Status collect_extents() {
    uint64_t at = node_.record, total = 0;
    uint8_t record[255];
    for (uint32_t guard = 0; guard < 100000 && at < node_.directory_end; ++guard) {
      uint8_t length = 0;
      if (const Status s = fs_.read_raw(at, ByteView(&length, 1)); s != Status::Ok) return s;
      if (length == 0) {      // the rest of this sector is padding
        at = (at / kSector + 1) * kSector;
        continue;
      }
      if (length < 34 || at % kSector + length > kSector) return Status::Corrupt;
      if (const Status s = fs_.read_raw(at, ByteView(record).first(length)); s != Status::Ok) return s;
      const ConstByteView r = ConstByteView(record).first(length);
      const uint64_t bytes = le32_at(r, 10);
      if (const Status s = add_extent(le32_at(r, 2), bytes); s != Status::Ok) return s;
      total += bytes;
      at += length;
      if (!(r[25] & kFlagMultiExtent)) {
        if (!node_.zisofs && total != node_.size) return Status::Corrupt;
        return Status::Ok;
      }
    }
    return Status::Corrupt;
  }

  // Bytes of the stored form (the file as it is in the image, over its extents).
  Status read_stored(uint64_t offset, ByteView out) const {
    uint64_t done = 0, base = 0;
    for (const Extent& extent : extents_.view()) {
      if (done == out.size()) break;
      const uint64_t at = offset + done;
      if (at < base + extent.size) {
        const uint64_t within = at - base, n = extent.size - within < out.size() - done ? extent.size - within : out.size() - done;
        if (const Status s = fs_.read_raw(extent.position + within, out.subview(done, n)); s != Status::Ok) return s;
        done += n;
      }
      base += extent.size;
    }
    return done == out.size() ? Status::Ok : Status::Corrupt;
  }

  uint64_t stored_size() const {
    uint64_t total = 0;
    for (const Extent& extent : extents_.view()) total += extent.size;
    return total;
  }

  // zisofs: a 16-byte header (magic, size, header size / 4, log2 of the block size), a table of block positions, then each block as a zlib stream
  // (a block whose start and end are the same is zeros).
  Status load_zisofs() {
    uint8_t header[16];
    if (stored_size() < sizeof(header)) return Status::Corrupt;
    if (const Status s = read_stored(0, ByteView(header)); s != Status::Ok) return s;
    const ConstByteView h(header);
    if (!h.first(8).equals(ConstByteView(kZisofsMagic))) return Status::Corrupt;
    const uint32_t size = le32_at(h, 8), header_size = uint32_t(h[12]) * 4, log2 = h[13];
    if (log2 < 15 || log2 > 17 || header_size < 16 || size != node_.size) return Status::Corrupt;
    block_bytes_ = 1u << log2;
    const uint64_t blocks = (uint64_t(size) + block_bytes_ - 1) / block_bytes_;
    ByteBuffer table(fs_.allocator_);
    if ((blocks + 1) * 4 > stored_size() - header_size || !table.put_zeros((blocks + 1) * 4)) return Status::Corrupt;
    if (const Status s = read_stored(header_size, table.writable_view()); s != Status::Ok) return s;
    uint32_t previous = 0;
    for (uint64_t i = 0; i <= blocks; ++i) {
      const uint32_t p = le32_at(table.view(), i * 4);
      if (p < previous || p > stored_size() || (i == 0 && p < header_size + (blocks + 1) * 4)) return Status::Corrupt;
      if (!pointers_.push_back(p)) return Status::OutOfMemory;
      previous = p;
    }
    return Status::Ok;
  }

  Status read_zisofs(uint64_t offset, ByteView out) {
    uint64_t done = 0;
    while (done < out.size()) {
      const uint64_t at = offset + done, index = at / block_bytes_, within = at % block_bytes_;
      const uint64_t block_size = node_.size - index * block_bytes_ < block_bytes_ ? node_.size - index * block_bytes_ : block_bytes_;
      const uint64_t n = block_size - within < out.size() - done ? block_size - within : out.size() - done;
      if (index + 1 >= pointers_.count()) return Status::Corrupt;
      const uint32_t start = pointers_.view()[index], end = pointers_.view()[index + 1];
      if (start == end) {
        out.subview(done, n).fill(0);
      } else {
        kernel::ScopedMutex lock(mutex_);
        if (cached_ != index) {
          cached_ = ~uint64_t{0};
          ByteBuffer packed(fs_.allocator_);
          if (!packed.put_zeros(end - start)) return Status::OutOfMemory;
          if (const Status s = read_stored(start, packed.writable_view()); s != Status::Ok) return s;
          block_.clear();
          if (!media::inflate(packed.view(), &block_, block_bytes_) || block_.size() != block_size) return Status::Corrupt;
          cached_ = index;
        }
        out.subview(done, n).copy_from(block_.view().subview(within, n));
      }
      done += n;
    }
    return Status::Ok;
  }

  const IsoFileSystem& fs_;
  Node node_;
  List<Extent> extents_;
  List<uint32_t> pointers_;
  uint32_t block_bytes_ = 0;
  kernel::Mutex mutex_;
  ByteBuffer block_;      // (mutex_) the last decompressed block
  uint64_t cached_ = ~uint64_t{0};
};

class IsoFileSystem::Directory final : public IDirectory {
 public:
  Directory(const IsoFileSystem& fs) : fs_(fs), records_(fs.allocator_), path_(fs.allocator_), previous_(fs.allocator_) {}
  Status open(const Node& node, StringView path) {
    node_ = node;
    if (!path_.assign(path)) return Status::OutOfMemory;
    return fs_.read_directory(node, &records_);
  }

  Status next(DirectoryEntry* entry) override {
    String name(fs_.allocator_);
    while (at_ < records_.size()) {
      const uint8_t length = records_.view()[at_];
      if (length == 0) {
        at_ = (at_ / kSector + 1) * kSector;
        continue;
      }
      Node node;
      bool skip = false;
      const uint64_t here = at_;
      if (const Status s = fs_.parse_record(records_.view(), here, node_.extent * fs_.block_size_, node_.extent * fs_.block_size_ + node_.size, &node, &name, &skip); s != Status::Ok) return s;
      at_ += length;
      const bool continuation = previous_multi_ && name.view().equals(previous_.view());      // (a later extent of a multi-extent file)
      previous_multi_ = (records_.view()[here + 25] & kFlagMultiExtent) != 0;
      if (!previous_.assign(name.view())) return Status::OutOfMemory;
      if (skip || continuation) continue;
      if (node.multi_extent) {
        // Its size: every extent's.
        uint64_t total = node.size, scan = at_;
        while (scan < records_.size() && records_.view()[scan] != 0) {
          const ConstByteView r = records_.view().subview(scan);
          if (r.size() < 34) break;
          total += le32_at(r, 10);
          if (!(r[25] & kFlagMultiExtent)) break;
          scan += r[0];
        }
        if (!node.zisofs) node.size = total;
      }
      if (const Status s = fs_.fill_entry(node, name.view(), entry); s != Status::Ok) return s;
      if (node.kind == Kind::Symlink) {
        String full(fs_.allocator_);
        if (!full.assign(path_.view()) || !full.push_back('/') || !full.append(name.view())) return Status::OutOfMemory;
        Node target;
        entry->type = NodeType::File;
        if (fs_.resolve(full.view(), true, &target) == Status::Ok) {
          entry->type = target.kind == Kind::Directory ? NodeType::Directory : NodeType::File;
          entry->size = target.kind == Kind::File ? target.size : 0;
        }
      }
      return Status::Ok;
    }
    return Status::EndOfDirectory;
  }
  void rewind() override {
    at_ = 0;
    previous_multi_ = false;
  }

 private:
  const IsoFileSystem& fs_;
  Node node_;
  ByteBuffer records_;
  String path_, previous_;
  uint64_t at_ = 0;
  bool previous_multi_ = false;
};

IsoFileSystem::IsoFileSystem(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset)
    : allocator_(allocator), source_(util::move(source)), offset_(offset) {}

bool IsoFileSystem::looks_like_iso(IFile& file, uint64_t offset) {
  uint8_t id[6];
  uint64_t got = 0;
  if (offset > file.size() || file.size() - offset < kDescriptorsStart + kSector) return false;
  if (file.read(offset + kDescriptorsStart, ByteView(id), &got) != Status::Ok || got != sizeof(id)) return false;
  return id[1] == 'C' && id[2] == 'D' && id[3] == '0' && id[4] == '0' && id[5] == '1';
}

Status IsoFileSystem::mount(allocators::IAllocator& allocator, UniquePointer<IFile> source, uint64_t offset, UniquePointer<IsoFileSystem>* result) {
  if (!source) return Status::InvalidArgument;
  UniquePointer<IsoFileSystem> fs = UniquePointer<IsoFileSystem>::make(allocator, allocator, util::move(source), offset);
  if (!fs) return Status::OutOfMemory;
  if (const Status s = fs->load(); s != Status::Ok) return s;
  *result = util::move(fs);
  return Status::Ok;
}

Status IsoFileSystem::read_raw(uint64_t position, ByteView out) const {
  if (position > size_ || out.size() > size_ - position) return Status::Corrupt;
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

Status IsoFileSystem::load() {
  const uint64_t file_size = source_->size();
  if (offset_ > file_size) return Status::Corrupt;
  size_ = file_size - offset_;
  uint8_t descriptor[kSector];
  uint8_t primary_root[34] = {}, joliet_root[34] = {};
  bool primary = false, joliet = false;
  uint64_t primary_at = 0;
  for (uint32_t i = 0; i < kMaxDescriptors; ++i) {
    const uint64_t at = kDescriptorsStart + i * kSector;
    if (const Status s = read_raw(at, ByteView(descriptor)); s != Status::Ok) return i == 0 ? Status::Corrupt : s;
    const ConstByteView d(descriptor);
    if (d[1] != 'C' || d[2] != 'D' || d[3] != '0' || d[4] != '0' || d[5] != '1') return Status::Corrupt;
    const uint8_t type = d[0];
    if (type == 255) break;
    if (type == 1 && !primary) {
      primary = true;
      primary_at = at;
      ByteView(primary_root).copy_from(d.subview(156, 34));
      block_size_ = le16_at(d, 128);
      volume_blocks_ = le32_at(d, 80);
      label_.assign(StringView(d.subview(40, 32)).trimmed());
    } else if (type == 2 && !joliet && d[88] == '%' && d[89] == '/' && (d[90] == '@' || d[90] == 'C' || d[90] == 'E')) {
      joliet = true;
      ByteView(joliet_root).copy_from(d.subview(156, 34));
    }
  }
  if (!primary) return Status::Corrupt;
  if (block_size_ != 512 && block_size_ != 1024 && block_size_ != 2048) return Status::Corrupt;

  auto root_from = [&](const uint8_t (&record)[34], uint64_t record_at, Node* node) {
    const ConstByteView r(record);
    if (r[0] < 34 || !(r[25] & kFlagDirectory)) return false;
    *node = Node();
    node->kind = Kind::Directory;
    node->extent = le32_at(r, 2);
    node->size = le32_at(r, 10);
    node->record = record_at;
    node->mtime = record_time(r.subview(18, 7));
    return node->extent <= size_ / block_size_ && node->size <= size_ - node->extent * block_size_;
  };
  if (!root_from(primary_root, primary_at + 156, &root_)) return Status::Corrupt;

  // Rock Ridge: an SP entry first in the System Use area of the root's "." record.
  uint8_t first[255];
  uint8_t length = 0;
  if (const Status s = read_raw(root_.extent * block_size_, ByteView(&length, 1)); s != Status::Ok) return s;
  if (length >= 34) {
    if (const Status s = read_raw(root_.extent * block_size_, ByteView(first).first(length)); s != Status::Ok) return s;
    const ConstByteView r = ConstByteView(first).first(length);
    const uint32_t name_length = r[32], area = 33 + name_length + (name_length % 2 == 0 ? 1 : 0);
    if (area + 7 <= length && r[area] == 'S' && r[area + 1] == 'P' && r[area + 2] >= 7 && r[area + 4] == 0xBE && r[area + 5] == 0xEF) {
      tree_ = Tree::RockRidge;
      susp_skip_ = r[area + 6];
      root_.record = root_.extent * block_size_;      // (its "." record: where Rock Ridge keeps the root's own entries)
    }
  }
  if (tree_ != Tree::RockRidge && joliet) {
    Node joliet_node;
    if (root_from(joliet_root, 0, &joliet_node)) {
      root_ = joliet_node;
      tree_ = Tree::Joliet;
    }
  }
  return Status::Ok;
}

FileSystemInfo IsoFileSystem::info() const {
  FileSystemInfo info;
  info.type_name.assign("iso9660");
  info.label.assign(label_.view());
  info.total_bytes = volume_blocks_ * block_size_;
  info.read_only = true;
  return info;
}

Status IsoFileSystem::read_directory(const Node& directory, ByteBuffer* out) const {
  out->clear();
  if (directory.size > kMaxDirectoryBytes || directory.extent > size_ / block_size_) return Status::Corrupt;
  const uint64_t at = directory.extent * block_size_;
  if (directory.size > size_ - at) return Status::Corrupt;
  if (!out->put_zeros(directory.size)) return Status::OutOfMemory;
  return read_raw(at, out->writable_view());
}

Status IsoFileSystem::parse_susp(ConstByteView area, Susp* susp, uint32_t depth) const {
  uint64_t continuation_block = 0, continuation_offset = 0, continuation_length = 0;
  bool continuation = false;
  for (uint64_t pos = 0; pos + 4 <= area.size();) {
    const uint8_t a = area[pos], b = area[pos + 1], length = area[pos + 2];
    if (length < 4 || pos + length > area.size()) break;
    const ConstByteView e = area.subview(pos, length);
    pos += length;
    if (a == 'S' && b == 'T') break;
    if (a == 'C' && b == 'E' && length >= 28) {
      continuation = true;
      continuation_block = le32_at(e, 4);
      continuation_offset = le32_at(e, 12);
      continuation_length = le32_at(e, 20);
    } else if (a == 'P' && b == 'X' && length >= 36) {
      susp->has_mode = true;
      susp->mode = le32_at(e, 4);
      susp->links = le32_at(e, 12);
      if (length >= 44) susp->serial = le32_at(e, 36);
    } else if (a == 'N' && b == 'M' && length >= 5) {
      if (e[4] & 0x06) continue;      // "." or ".."
      if (!susp->name.append(StringView(e.subview(5)))) return Status::OutOfMemory;
      susp->has_name = true;
    } else if (a == 'S' && b == 'L' && length >= 5) {
      susp->is_link = true;
      for (uint64_t c = 5; c + 2 <= length;) {
        const uint8_t flags = e[c], size = e[c + 1];
        if (c + 2 + size > length) return Status::Corrupt;
        const StringView content(e.subview(c + 2, size));
        c += 2 + size;
        const bool root = flags & 0x08;
        if (!root && !susp->link_continues && !susp->target.empty() && !susp->target.view().equals("/") && !susp->target.push_back('/')) return Status::OutOfMemory;
        bool ok = true;
        if (root) ok = susp->target.assign("/");
        else if (flags & 0x02) ok = susp->target.append(".");
        else if (flags & 0x04) ok = susp->target.append("..");
        else ok = susp->target.append(content);
        if (!ok) return Status::OutOfMemory;
        susp->link_continues = (flags & 0x01) != 0;
      }
      if (susp->target.size() > 4096) return Status::Corrupt;
    } else if (a == 'T' && b == 'F' && length >= 5) {
      const uint8_t flags = e[4];
      const uint64_t step = (flags & 0x80) ? 17 : 7;
      uint64_t at = 5;
      for (uint8_t bit = 0x01; bit <= 0x40; bit <<= 1) {
        if (!(flags & bit)) continue;
        if (at + step > length) break;
        if (bit == 0x02) susp->mtime = step == 17 ? long_time(e.subview(at, 17)) : record_time(e.subview(at, 7));
        at += step;
      }
    } else if (a == 'C' && b == 'L' && length >= 12) {
      susp->has_child = true;
      susp->child = le32_at(e, 4);
    } else if (a == 'R' && b == 'E') {
      susp->relocated = true;
    } else if (a == 'Z' && b == 'F' && length >= 16) {
      if (e[4] == 'p' && e[5] == 'z') {
        susp->zisofs = true;
        susp->zisofs_header = static_cast<uint8_t>(e[6] * 4);
        susp->zisofs_log2 = e[7];
        susp->zisofs_size = le32_at(e, 8);
      } else {
        susp->zisofs_unknown = true;
      }
    } else if (a == 'S' && b == 'F') {
      susp->sparse = true;
    }
  }
  if (!continuation) return Status::Ok;
  if (depth >= kMaxContinuations || continuation_length > kMaxContinuation || continuation_offset >= block_size_ || continuation_block > size_ / block_size_)
    return Status::Corrupt;
  ByteBuffer more(allocator_);
  if (!more.put_zeros(continuation_length)) return Status::OutOfMemory;
  if (const Status s = read_raw(continuation_block * block_size_ + continuation_offset, more.writable_view()); s != Status::Ok) return s;
  return parse_susp(more.view(), susp, depth + 1);
}

Status IsoFileSystem::parse_record(ConstByteView records, uint64_t at, uint64_t base, uint64_t directory_end, Node* node, String* name, bool* skip) const {
  *skip = false;
  if (at >= records.size()) return Status::Corrupt;
  const uint8_t length = records[at];
  if (length < 34 || at + length > records.size() || at % kSector + length > kSector) return Status::Corrupt;
  const ConstByteView r = records.subview(at, length);
  const uint8_t name_length = r[32];
  if (33u + name_length > length) return Status::Corrupt;
  const ConstByteView identifier = r.subview(33, name_length);
  *node = Node();
  node->record = base + at;
  node->directory_end = directory_end;
  node->extent = le32_at(r, 2);
  node->size = le32_at(r, 10);
  node->mtime = record_time(r.subview(18, 7));
  const uint8_t flags = r[25];
  node->kind = (flags & kFlagDirectory) ? Kind::Directory : Kind::File;
  node->multi_extent = (flags & kFlagMultiExtent) != 0;
  node->interleaved = r[26] != 0 || r[27] != 0;
  if (name_length == 1 && (identifier[0] == 0 || identifier[0] == 1)) {
    *skip = true;
    return name->assign(identifier[0] ? StringView("..") : StringView(".")) ? Status::Ok : Status::OutOfMemory;
  }
  if (tree_ == Tree::Joliet) {
    if (!utf16be_to_utf8(identifier, name)) return Status::OutOfMemory;
    if (node->kind == Kind::File) {
      String trimmed(allocator_);
      if (!trimmed.assign(without_version(name->view()))) return Status::OutOfMemory;
      *name = util::move(trimmed);
    }
  } else {
    const StringView plain(identifier);
    if (!name->assign(node->kind == Kind::File ? without_version(plain) : plain)) return Status::OutOfMemory;
    if (tree_ == Tree::Plain) {
      for (uint64_t i = 0; i < name->size(); ++i) {
        const char c = (*name)[i];
        if (c >= 'A' && c <= 'Z') name->set(i, char(c + 32));
      }
    }
  }
  if (tree_ == Tree::RockRidge) {
    const uint64_t area = 33 + name_length + (name_length % 2 == 0 ? 1 : 0) + susp_skip_;
    Susp susp(allocator_);
    if (area < length) {
      if (const Status s = parse_susp(r.subview(area), &susp, 0); s != Status::Ok) return s;
    }
    if (susp.has_name) *name = util::move(susp.name);
    if (susp.relocated) *skip = true;
    if (susp.has_mode) {
      const uint32_t type = susp.mode & kModeTypeMask;
      if (type == kModeLink) node->kind = Kind::Symlink;
      else if (type == kModeDirectory) node->kind = Kind::Directory;
      if (susp.links) node->links = susp.links;
    }
    if (susp.is_link) node->kind = Kind::Symlink;
    if (susp.serial) node->serial = susp.serial;
    if (susp.mtime) node->mtime = susp.mtime;
    node->sparse = susp.sparse || susp.zisofs_unknown;
    if (susp.has_child) {      // a folder mkisofs moved (deeper than 8 levels): its records are at the child link
      if (susp.child > size_ / block_size_) return Status::Corrupt;
      uint8_t dot[34];
      if (const Status s = read_raw(susp.child * block_size_, ByteView(dot)); s != Status::Ok) return s;
      node->kind = Kind::Directory;
      node->extent = susp.child;
      node->size = le32_at(ConstByteView(dot), 10);
    }
    if (susp.zisofs && node->kind == Kind::File) {
      node->zisofs = true;
      node->zisofs_header = susp.zisofs_header;
      node->zisofs_log2 = susp.zisofs_log2;
      node->stored = node->size;
      node->size = susp.zisofs_size;
    }
  }
  if (name->empty() || name->size() > kMaxNameLength || name->view().contains('/')) return Status::Corrupt;
  return Status::Ok;
}

Status IsoFileSystem::record_susp(uint64_t record, Susp* susp) const {
  uint8_t bytes[255];
  uint8_t length = 0;
  if (const Status s = read_raw(record, ByteView(&length, 1)); s != Status::Ok) return s;
  if (length < 34 || record % kSector + length > kSector) return Status::Corrupt;
  if (const Status s = read_raw(record, ByteView(bytes).first(length)); s != Status::Ok) return s;
  const ConstByteView r = ConstByteView(bytes).first(length);
  const uint64_t area = 33 + r[32] + (r[32] % 2 == 0 ? 1 : 0) + susp_skip_;
  if (area >= length) return Status::Ok;
  return parse_susp(r.subview(area), susp, 0);
}

Status IsoFileSystem::read_target(const Node& link, String* target) const {
  Susp susp(allocator_);
  if (const Status s = record_susp(link.record, &susp); s != Status::Ok) return s;
  if (!susp.is_link) return Status::Corrupt;
  return target->assign(susp.target.view()) ? Status::Ok : Status::OutOfMemory;
}

Status IsoFileSystem::lookup(const Node& directory, StringView name, Node* found) const {
  ByteBuffer records(allocator_);
  if (const Status s = read_directory(directory, &records); s != Status::Ok) return s;
  const uint64_t base = directory.extent * block_size_, end = base + directory.size;
  String entry(allocator_);
  for (uint64_t at = 0; at < records.size();) {
    const uint8_t length = records.view()[at];
    if (length == 0) {
      at = (at / kSector + 1) * kSector;
      continue;
    }
    bool skip = false;
    if (const Status s = parse_record(records.view(), at, base, end, found, &entry, &skip); s != Status::Ok) return s;
    at += length;
    if (skip) continue;
    const bool match = tree_ == Tree::Plain ? entry.view().equals_ignore_case(name) : entry.view().equals(name);
    if (!match) continue;
    if (found->multi_extent && !found->zisofs) {
      uint64_t total = found->size;
      for (uint64_t scan = at; scan < records.size() && records.view()[scan] >= 34;) {
        const ConstByteView r = records.view().subview(scan);
        total += le32_at(r, 10);
        if (!(r[25] & kFlagMultiExtent)) break;
        scan += r[0];
      }
      found->size = total;
    }
    return Status::Ok;
  }
  return Status::NotFound;
}

struct IsoFileSystem::WalkOps {
  const IsoFileSystem& fs;
  Status lookup(const Node& directory, StringView name, Node* child) { return fs.lookup(directory, name, child); }
  bool is_directory(const Node& node) const { return node.kind == Kind::Directory; }
  bool is_link(const Node& node) const { return node.kind == Kind::Symlink; }
  Status target(const Node& link, String* text) { return fs.read_target(link, text); }
};

Status IsoFileSystem::resolve(StringView path, bool follow_last, Node* node, String* exit) const {
  WalkOps ops{*this};
  return walk_path(allocator_, path, follow_last, root_, ops, node, exit);
}

Status IsoFileSystem::resolve_exit(StringView path, bool follow_last, String* exit) {
  Node node;
  return resolve(path, follow_last, &node, exit);
}

Status IsoFileSystem::fill_entry(const Node& node, StringView name, DirectoryEntry* entry) const {
  *entry = DirectoryEntry();
  entry->name.assign(name);
  entry->read_only = true;
  entry->modified = node.mtime;
  entry->file_id = node.serial ? node.serial : node.record + 1;
  entry->link_count = node.links;
  entry->type = node.kind == Kind::Directory ? NodeType::Directory : NodeType::File;
  entry->size = node.kind == Kind::File ? node.size : 0;
  entry->is_symlink = node.kind == Kind::Symlink;
  return Status::Ok;
}

Status IsoFileSystem::stat(StringView path, DirectoryEntry* entry) {
  Node node;
  if (const Status s = resolve(path, false, &node); s != Status::Ok) return s;
  const bool is_link = node.kind == Kind::Symlink;
  if (is_link) {
    if (const Status s = resolve(path, true, &node); s != Status::Ok) return s;
  }
  if (const Status s = fill_entry(node, link_walk::last_component(path), entry); s != Status::Ok) return s;
  entry->is_symlink = is_link;
  return Status::Ok;
}

Status IsoFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  Node node;
  const Status s = resolve(path, true, &node);
  if (s != Status::Ok) return s == Status::NotFound && mode != OpenMode::Read && mode != OpenMode::ReadWrite ? Status::ReadOnly : s;
  if (node.kind == Kind::Directory) return Status::IsADirectory;
  if (mode != OpenMode::Read) return Status::ReadOnly;
  UniquePointer<File> opened = UniquePointer<File>::make(allocator_, *this, node);
  if (!opened) return Status::OutOfMemory;
  if (const Status l = opened->load(); l != Status::Ok) return l;
  *file = util::move(opened);
  return Status::Ok;
}

Status IsoFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  Node node;
  if (const Status s = resolve(path, true, &node); s != Status::Ok) return s;
  if (node.kind != Kind::Directory) return Status::NotADirectory;
  UniquePointer<Directory> opened = UniquePointer<Directory>::make(allocator_, *this);
  if (!opened) return Status::OutOfMemory;
  if (const Status s = opened->open(node, path); s != Status::Ok) return s;
  *directory = util::move(opened);
  return Status::Ok;
}

Status IsoFileSystem::read_symbolic_link(StringView path, String* target, bool* target_is_directory) {
  Node node;
  if (const Status s = resolve(path, false, &node); s != Status::Ok) return s;
  if (node.kind != Kind::Symlink) return Status::InvalidArgument;
  if (const Status s = read_target(node, target); s != Status::Ok) return s;
  Node pointed;
  *target_is_directory = resolve(path, true, &pointed) == Status::Ok && pointed.kind == Kind::Directory;
  return Status::Ok;
}

}  // namespace filesystem
