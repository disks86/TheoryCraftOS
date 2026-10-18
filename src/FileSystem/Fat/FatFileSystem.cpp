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

#include "FileSystem/Fat/FatFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/Move.hpp"
#include "Utility/Utf8.hpp"

namespace filesystem {

namespace {
constexpr uint8_t kAttrReadOnly = 0x01, kAttrVolumeLabel = 0x08, kAttrDirectory = 0x10, kAttrLongName = 0x0F;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }

uint8_t short_name_checksum(const uint8_t* name11) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; ++i) sum = static_cast<uint8_t>(((sum & 1) << 7) + (sum >> 1) + name11[i]);
  return sum;
}

// Trims trailing blanks and copies into `out`, lower-casing ASCII letters if asked.
void append_padded(FixedString<kMaxNameLength + 1>* out, const uint8_t* field, uint32_t length, bool lower) {
  while (length && field[length - 1] == ' ') --length;
  for (uint32_t i = 0; i < length; ++i) {
    char c = static_cast<char>(field[i]);
    if (lower && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out->push_back(c);
  }
}
}  // namespace

// An open file. It keeps no copy of the file's first cluster or size: both are read from its directory entry each time, so several
// handles on one file (and a truncation through another handle) stay consistent. Handles are registered with the filesystem, which
// refuses to remove or rename a file that is open.
class FatFileSystem::File final : public IFile {
 public:
  File(FatFileSystem& fs, const Node& node, bool writable) : fs_(fs), entry_offset_(node.short_offset), writable_(writable) {
    next_open_ = fs_.open_files_;
    fs_.open_files_ = this;
  }
  ~File() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    for (File** link = &fs_.open_files_; *link; link = &(*link)->next_open_) {
      if (*link == this) {
        *link = next_open_;
        break;
      }
    }
  }
  uint64_t entry_offset() const { return entry_offset_; }
  File* next_open() const { return next_open_; }

  uint64_t size() const override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    uint32_t first = 0, size = 0;
    return load(&first, &size) == Status::Ok ? size : 0;
  }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    uint32_t first = 0, size = 0;
    Status s = load(&first, &size);
    if (s != Status::Ok) return s;
    if (offset >= size || buffer.empty()) return Status::Ok;
    uint64_t remaining = size - offset;
    if (remaining > buffer.size()) remaining = buffer.size();
    const uint32_t cluster_bytes = fs_.cluster_bytes_;
    uint64_t done = 0;
    while (remaining) {
      const uint32_t within = static_cast<uint32_t>((offset + done) % cluster_bytes);
      uint64_t chunk = cluster_bytes - within;
      if (chunk > remaining) chunk = remaining;
      s = fs_.seek_cluster(first, (offset + done) / cluster_bytes, &cursor_);
      if (s == Status::Ok) s = fs_.read_bytes(fs_.cluster_offset(cursor_.cluster) + within, buffer.subview(done, chunk));
      if (s != Status::Ok) {
        *transferred = done;
        return s;
      }
      done += chunk;
      remaining -= chunk;
    }
    *transferred = done;
    return Status::Ok;
  }
  Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!writable_) return Status::ReadOnly;
    Status s = fs_.writable();
    if (s != Status::Ok) return s;
    if (data.empty()) return Status::Ok;
    uint32_t first = 0, size = 0;
    s = load(&first, &size);
    if (s != Status::Ok) return s;
    const uint64_t end = offset + data.size();
    if (end > 0xFFFFFFFFull || end < offset) return Status::NoSpace;
    if (end > size) {  // grow first (zeros fill any gap), then the data goes over it
      s = fs_.resize_chain(&first, size, static_cast<uint32_t>(end));
      if (s != Status::Ok) return s;
      s = fs_.update_entry(entry_offset_, first, static_cast<uint32_t>(end));
      if (s != Status::Ok) return s;
    }
    const uint32_t cluster_bytes = fs_.cluster_bytes_;
    uint64_t done = 0;
    while (done < data.size()) {
      const uint32_t within = static_cast<uint32_t>((offset + done) % cluster_bytes);
      uint64_t chunk = cluster_bytes - within;
      if (chunk > data.size() - done) chunk = data.size() - done;
      s = fs_.seek_cluster(first, (offset + done) / cluster_bytes, &cursor_);
      if (s == Status::Ok) s = fs_.write_bytes(fs_.cluster_offset(cursor_.cluster) + within, data.subview(done, chunk));
      if (s != Status::Ok) {
        *transferred = done;
        return s;
      }
      done += chunk;
    }
    *transferred = done;
    return Status::Ok;
  }
  Status resize(uint64_t new_size) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::ReadOnly;
    Status s = fs_.writable();
    if (s != Status::Ok) return s;
    if (new_size > 0xFFFFFFFFull) return Status::NoSpace;
    uint32_t first = 0, size = 0;
    s = load(&first, &size);
    if (s != Status::Ok) return s;
    if (new_size == size) return Status::Ok;
    s = fs_.resize_chain(&first, size, static_cast<uint32_t>(new_size));
    if (s != Status::Ok) return s;
    return fs_.update_entry(entry_offset_, first, static_cast<uint32_t>(new_size));
  }
  Status flush() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    return fs_.flush();
  }

 private:
  Status load(uint32_t* first, uint32_t* size) const {
    uint8_t raw[32];
    const Status s = fs_.read_bytes(entry_offset_, ByteView(raw, 32));
    if (s != Status::Ok) return s;
    *first = le16(raw + 26);
    if (fs_.kind_ == Kind::Fat32) *first |= uint32_t{le16(raw + 20)} << 16;
    *size = le32(raw + 28);
    return Status::Ok;
  }

  FatFileSystem& fs_;
  const uint64_t entry_offset_;
  const bool writable_;
  File* next_open_ = nullptr;
  ChainCursor cursor_;
};

class FatFileSystem::Directory final : public IDirectory {
 public:
  Directory(FatFileSystem& fs, const Position& start) : fs_(fs), start_(start), position_(start) {}

  Status next(DirectoryEntry* entry) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    Node node;
    const Status s = fs_.next_node(&position_, &node);
    if (s == Status::Ok) *entry = node.entry;
    return s;
  }
  void rewind() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    position_ = start_;
  }

 private:
  FatFileSystem& fs_;
  const Position start_;
  Position position_;
};

Status FatFileSystem::mount(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block,
                            UniquePointer<IFileSystem>* result) {
  UniquePointer<IFileSystem> fs = UniquePointer<IFileSystem>::make_as<FatFileSystem>(allocator, allocator, device, first_block);
  if (!fs) return Status::OutOfMemory;
  const Status s = static_cast<FatFileSystem*>(fs.get())->init();
  if (s != Status::Ok) return s;
  *result = util::move(fs);
  return Status::Ok;
}

FatFileSystem::FatFileSystem(allocators::IAllocator& allocator, hardware::IStorageDevice& device, uint64_t first_block)
    : allocator_(allocator), device_(device), first_block_(first_block) {}

FatFileSystem::~FatFileSystem() { release_view(allocator_, block_); }

Status FatFileSystem::init() {
  const hardware::StorageInfo storage = device_.info();
  block_size_ = storage.block_size;
  if (block_size_ == 0 || (block_size_ & (block_size_ - 1)) != 0 || first_block_ >= storage.block_count) return Status::InvalidArgument;
  block_ = allocate_view<uint8_t>(allocator_, block_size_);
  if (block_.empty()) return Status::OutOfMemory;

  uint8_t boot[512];
  Status s = read_bytes(0, ByteView(boot));
  if (s != Status::Ok) return s;
  if (boot[510] != 0x55 || boot[511] != 0xAA) return Status::Corrupt;

  bytes_per_sector_ = le16(boot + 11);
  sectors_per_cluster_ = boot[13];
  const uint32_t reserved = le16(boot + 14);
  const uint32_t fats = boot[16];
  root_entries_ = le16(boot + 17);
  uint32_t total = le16(boot + 19);
  if (total == 0) total = le32(boot + 32);
  uint32_t fat_sectors = le16(boot + 22);
  const bool fat32_layout = fat_sectors == 0;
  if (fat32_layout) fat_sectors = le32(boot + 36);

  if (bytes_per_sector_ < 512 || bytes_per_sector_ > 4096 || (bytes_per_sector_ & (bytes_per_sector_ - 1)) != 0) return Status::Corrupt;
  if (sectors_per_cluster_ == 0 || (sectors_per_cluster_ & (sectors_per_cluster_ - 1)) != 0) return Status::Corrupt;
  if (reserved == 0 || fats == 0 || fat_sectors == 0 || total == 0) return Status::Corrupt;
  cluster_bytes_ = bytes_per_sector_ * sectors_per_cluster_;
  if (cluster_bytes_ > 1u << 20) return Status::Corrupt;

  fat_start_ = reserved;
  fat_count_ = fats;
  fat_sectors_ = fat_sectors;
  read_only_ = storage.read_only;
  root_dir_start_ = reserved + fats * fat_sectors;
  const uint32_t root_sectors = (root_entries_ * 32 + bytes_per_sector_ - 1) / bytes_per_sector_;
  data_start_ = root_dir_start_ + root_sectors;
  if (data_start_ >= total) return Status::Corrupt;
  cluster_count_ = (total - data_start_) / sectors_per_cluster_;
  kind_ = cluster_count_ < 4085 ? Kind::Fat12 : cluster_count_ < 65525 ? Kind::Fat16 : Kind::Fat32;

  // The sizes decide the type, the boot sector's layout must agree with it.
  if ((kind_ == Kind::Fat32) != fat32_layout) return Status::Corrupt;
  const uint8_t* label = boot + (fat32_layout ? 71 : 43);
  if (fat32_layout) {
    if (le16(boot + 42) != 0) return Status::Unsupported;  // a newer FAT32 version
    root_cluster_ = le32(boot + 44);
    if (root_entries_ != 0 || root_cluster_ < 2 || root_cluster_ >= cluster_count_ + 2) return Status::Corrupt;
  } else if (root_entries_ == 0) {
    return Status::Corrupt;
  }
  FixedString<kMaxNameLength + 1> text;
  append_padded(&text, label, 11, false);
  if (!text.equals("NO NAME")) label_.assign(text.view().first(text.size() < label_.capacity() ? text.size() : label_.capacity()));
  return Status::Ok;
}

FileSystemInfo FatFileSystem::info() const {
  FileSystemInfo result;
  result.type_name.assign(kind_ == Kind::Fat12 ? "FAT12" : kind_ == Kind::Fat16 ? "FAT16" : "FAT32");
  result.label = label_;
  result.total_bytes = uint64_t{cluster_count_} * cluster_bytes_;
  result.read_only = read_only_;
  if (!read_only_) {
    kernel::ScopedRecursiveMutex lock(mutex_);
    result.free_bytes = uint64_t{const_cast<FatFileSystem*>(this)->free_cluster_count()} * cluster_bytes_;
  }
  return result;
}

Status FatFileSystem::read_bytes(uint64_t offset, ByteView out) {
  uint64_t absolute = first_block_ * block_size_ + offset;
  uint64_t done = 0;
  while (done < out.size()) {
    const uint64_t block = absolute / block_size_;
    const uint32_t within = static_cast<uint32_t>(absolute % block_size_);
    const uint64_t left = out.size() - done;
    if (within == 0 && left >= block_size_) {  // whole blocks straight into the caller's buffer
      const uint64_t bytes = left / block_size_ * block_size_;
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
    uint64_t take = block_size_ - within;
    if (take > left) take = left;
    out.subview(done, take).copy_from(ConstByteView(block_).subview(within, take));
    done += take;
    absolute += take;
  }
  return Status::Ok;
}

// The raw FAT entry for `cluster`.
Status FatFileSystem::get_fat(uint32_t cluster, uint32_t* value) {
  if (cluster >= cluster_count_ + 2) return Status::Corrupt;
  uint8_t raw[4] = {};
  const uint64_t fat = uint64_t{fat_start_} * bytes_per_sector_;
  Status s;
  if (kind_ == Kind::Fat12) {
    s = read_bytes(fat + cluster + cluster / 2, ByteView(raw, 2));
    if (s != Status::Ok) return s;
    *value = le16(raw);
    *value = (cluster & 1) ? *value >> 4 : *value & 0xFFF;
  } else if (kind_ == Kind::Fat16) {
    s = read_bytes(fat + uint64_t{cluster} * 2, ByteView(raw, 2));
    if (s != Status::Ok) return s;
    *value = le16(raw);
  } else {
    s = read_bytes(fat + uint64_t{cluster} * 4, ByteView(raw, 4));
    if (s != Status::Ok) return s;
    *value = le32(raw) & 0x0FFFFFFF;
  }
  return Status::Ok;
}

// Stores `value` as the entry for `cluster` in every copy of the FAT.
Status FatFileSystem::set_fat(uint32_t cluster, uint32_t value) {
  if (cluster < 2 || cluster >= cluster_count_ + 2) return Status::Corrupt;
  for (uint32_t copy = 0; copy < fat_count_; ++copy) {
    const uint64_t fat = (uint64_t{fat_start_} + uint64_t{copy} * fat_sectors_) * bytes_per_sector_;
    uint8_t raw[4] = {};
    Status s;
    if (kind_ == Kind::Fat12) {
      const uint64_t at = fat + cluster + cluster / 2;
      s = read_bytes(at, ByteView(raw, 2));
      if (s != Status::Ok) return s;
      uint32_t word = le16(raw);
      word = (cluster & 1) ? (word & 0x000F) | ((value & 0xFFF) << 4) : (word & 0xF000) | (value & 0xFFF);
      raw[0] = static_cast<uint8_t>(word);
      raw[1] = static_cast<uint8_t>(word >> 8);
      s = write_bytes(at, ConstByteView(raw, 2));
    } else if (kind_ == Kind::Fat16) {
      raw[0] = static_cast<uint8_t>(value);
      raw[1] = static_cast<uint8_t>(value >> 8);
      s = write_bytes(fat + uint64_t{cluster} * 2, ConstByteView(raw, 2));
    } else {
      const uint64_t at = fat + uint64_t{cluster} * 4;
      s = read_bytes(at, ByteView(raw, 4));
      if (s != Status::Ok) return s;
      const uint32_t word = (le32(raw) & 0xF0000000u) | (value & 0x0FFFFFFF);  // the top four bits are reserved: kept
      for (int i = 0; i < 4; ++i) raw[i] = static_cast<uint8_t>(word >> (8 * i));
      s = write_bytes(at, ConstByteView(raw, 4));
    }
    if (s != Status::Ok) return s;
  }
  return Status::Ok;
}

// The next cluster of a chain: *end is true for an end-of-chain mark. A free,
// bad or out-of-range link is Corrupt.
Status FatFileSystem::next_cluster(uint32_t cluster, uint32_t* next, bool* end) {
  if (cluster < 2 || cluster >= cluster_count_ + 2) return Status::Corrupt;
  uint32_t value = 0;
  const Status s = get_fat(cluster, &value);
  if (s != Status::Ok) return s;
  *end = value >= (kind_ == Kind::Fat12 ? 0xFF8u : kind_ == Kind::Fat16 ? 0xFFF8u : 0x0FFFFFF8u);
  *next = value;
  if (*end) return Status::Ok;
  return value < 2 || value >= cluster_count_ + 2 ? Status::Corrupt : Status::Ok;
}

FatFileSystem::Position FatFileSystem::start_of(const Node& directory) const {
  Position p;
  p.first = directory.cluster;
  p.cluster = directory.cluster;
  return p;
}

uint64_t FatFileSystem::entry_offset(const Position& p) const {
  if (p.first == 0) return uint64_t{root_dir_start_} * bytes_per_sector_ + uint64_t{p.index} * 32;
  return cluster_offset(p.cluster) + uint64_t{p.index % (cluster_bytes_ / 32)} * 32;
}

Status FatFileSystem::read_raw_entry(Position* p, uint8_t out[32]) {
  if (p->ended) return Status::EndOfDirectory;
  uint64_t offset;
  const uint32_t per_cluster = cluster_bytes_ / 32;
  if (p->first == 0) {  // FAT12/16 root: a fixed run of sectors
    if (p->index >= root_entries_) {
      p->ended = true;
      return Status::EndOfDirectory;
    }
    offset = uint64_t{root_dir_start_} * bytes_per_sector_ + uint64_t{p->index} * 32;
  } else {
    offset = cluster_offset(p->cluster) + uint64_t{p->index % per_cluster} * 32;
  }
  const Status s = read_bytes(offset, ByteView(out, 32));
  if (s != Status::Ok) return s;
  ++p->index;
  if (p->first != 0 && p->index % per_cluster == 0) {
    uint32_t next = 0;
    bool end = false;
    const Status link = next_cluster(p->cluster, &next, &end);
    if (link != Status::Ok) return link;
    if (end || ++p->hops > cluster_count_) {
      p->ended = true;
    } else {
      p->cluster = next;
    }
  }
  return Status::Ok;
}

// The next real entry: long names assembled, deleted entries, the volume label,
// "." and ".." left out.
Status FatFileSystem::next_node(Position* position, Node* node) {
  uint16_t lfn[20 * 13];
  uint32_t lfn_expected = 0;  // ordinal of the long-name piece due next (0: none being collected)
  uint32_t lfn_total = 0;
  uint8_t lfn_checksum = 0;
  Position group_start;     // where the long-name pieces being collected begin
  uint32_t group_slots = 0;  // pieces collected so far
  uint8_t raw[32];
  for (;;) {
    const Position at = *position;
    const Status s = read_raw_entry(position, raw);
    if (s != Status::Ok) return s;
    if (raw[0] == 0x00) {  // nothing after this
      position->ended = true;
      return Status::EndOfDirectory;
    }
    if (raw[0] == 0xE5) {
      lfn_expected = 0, lfn_total = 0;
      continue;
    }
    const uint8_t attributes = raw[11];
    if ((attributes & 0x3F) == kAttrLongName) {
      const uint32_t ordinal = raw[0] & 0x1F;
      if (ordinal < 1 || ordinal > 20) {
        lfn_expected = 0, lfn_total = 0;
        continue;
      }
      if (raw[0] & 0x40) {
        lfn_total = ordinal;
        lfn_checksum = raw[13];
        group_start = at;
        group_slots = 0;
      } else if (lfn_expected != ordinal || raw[13] != lfn_checksum) {
        lfn_expected = 0, lfn_total = 0;
        continue;
      }
      static const uint8_t kOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
      for (uint32_t i = 0; i < 13; ++i) lfn[(ordinal - 1) * 13 + i] = le16(raw + kOffsets[i]);
      lfn_expected = ordinal - 1;
      ++group_slots;
      continue;
    }
    if ((attributes & kAttrVolumeLabel) != 0) {
      lfn_expected = 0, lfn_total = 0;
      continue;
    }
    if (raw[0] == '.' && (raw[1] == ' ' || (raw[1] == '.' && raw[2] == ' '))) {
      lfn_expected = 0, lfn_total = 0;
      continue;
    }

    node->entry = DirectoryEntry();
    bool named = false;
    if (lfn_total != 0 && lfn_expected == 0 && lfn_checksum == short_name_checksum(raw)) {
      named = true;
      for (uint32_t i = 0; i < lfn_total * 13 && named; ++i) {
        uint32_t cp = lfn[i];
        if (cp == 0x0000 || cp == 0xFFFF) break;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < lfn_total * 13 && lfn[i + 1] >= 0xDC00 && lfn[i + 1] < 0xE000)
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lfn[++i] - 0xDC00);
        named = append_utf8(&node->entry.name, cp);
      }
      if (!named || node->entry.name.empty()) node->entry.name.clear(), named = false;
    }
    if (!named) {
      uint8_t base[8];
      __builtin_memcpy(base, raw, 8);
      if (base[0] == 0x05) base[0] = 0xE5;
      append_padded(&node->entry.name, base, 8, (raw[12] & 0x08) != 0);
      if (raw[8] != ' ') {
        node->entry.name.push_back('.');
        append_padded(&node->entry.name, raw + 8, 3, (raw[12] & 0x10) != 0);
      }
    }
    lfn_expected = 0, lfn_total = 0;
    node->short_offset = entry_offset(at);
    node->first_pos = named ? group_start : at;
    node->slots = named ? group_slots + 1 : 1;
    node->entry.type = (attributes & kAttrDirectory) ? NodeType::Directory : NodeType::File;
    node->entry.size = node->entry.type == NodeType::File ? le32(raw + 28) : 0;
    node->entry.read_only = (attributes & kAttrReadOnly) != 0;
    node->cluster = le16(raw + 26);
    if (kind_ == Kind::Fat32) node->cluster |= uint32_t{le16(raw + 20)} << 16;
    // A directory must have clusters; a file with data must too.
    if (node->cluster == 0 && (node->entry.type == NodeType::Directory || node->entry.size != 0)) return Status::Corrupt;
    if (node->cluster != 0 && (node->cluster < 2 || node->cluster >= cluster_count_ + 2)) return Status::Corrupt;
    return Status::Ok;
  }
}

Status FatFileSystem::find(const Node& directory, StringView name, Node* found) {
  Position position = start_of(directory);
  for (;;) {
    Status s = next_node(&position, found);
    if (s == Status::EndOfDirectory) return Status::NotFound;
    if (s != Status::Ok) return s;
    if (found->entry.name.view().equals_ignore_case(name)) return Status::Ok;
  }
}

Status FatFileSystem::resolve(StringView path, Node* node) {
  if (block_.empty()) return Status::InvalidArgument;
  Node current;
  current.entry.type = NodeType::Directory;
  current.cluster = kind_ == Kind::Fat32 ? root_cluster_ : 0;
  StringView rest = path, component;
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    if (component.equals("..") || component.size() > kMaxNameLength) return Status::InvalidPath;
    if (current.entry.type != NodeType::Directory) return Status::NotADirectory;
    Node child;
    const Status s = find(current, component, &child);
    if (s != Status::Ok) return s;
    current = child;
  }
  *node = current;
  return Status::Ok;
}

Status FatFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  const bool write = mode != OpenMode::Read;
  if (write && read_only_) return Status::ReadOnly;
  Node node;
  Status s = resolve(path, &node);
  if (s == Status::NotFound && (mode == OpenMode::CreateOrOpen || mode == OpenMode::CreateTruncate)) {
    Node parent;
    StringView leaf;
    s = split(path, &parent, &leaf);
    if (s != Status::Ok) return s;
    s = make_entry(parent, leaf, 0x20, 0, 0, 0, &node);
    if (s != Status::Ok) return s;
  } else if (s != Status::Ok) {
    return s;
  } else {
    if (node.entry.type == NodeType::Directory) return Status::IsADirectory;
    if (write && node.entry.read_only) return Status::ReadOnly;
    if (mode == OpenMode::CreateTruncate && node.entry.size != 0) {
      uint32_t first = node.cluster;
      s = resize_chain(&first, static_cast<uint32_t>(node.entry.size), 0);
      if (s != Status::Ok) return s;
      s = update_entry(node.short_offset, first, 0);
      if (s != Status::Ok) return s;
      node.cluster = first;
      node.entry.size = 0;
    }
  }
  UniquePointer<IFile> handle = UniquePointer<IFile>::make_as<File>(allocator_, *this, node, write);
  if (!handle) return Status::OutOfMemory;
  *file = util::move(handle);
  return Status::Ok;
}

Status FatFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node node;
  const Status s = resolve(path, &node);
  if (s != Status::Ok) return s;
  if (node.entry.type != NodeType::Directory) return Status::NotADirectory;
  UniquePointer<IDirectory> handle = UniquePointer<IDirectory>::make_as<Directory>(allocator_, *this, start_of(node));
  if (!handle) return Status::OutOfMemory;
  *directory = util::move(handle);
  return Status::Ok;
}

Status FatFileSystem::stat(StringView path, DirectoryEntry* entry) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node node;
  const Status s = resolve(path, &node);
  if (s != Status::Ok) return s;
  *entry = node.entry;
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------------------------------------------------

namespace {

// UTF-8 to code points; false for malformed text, surrogates, anything past U+10FFFF or more than `capacity` characters.
bool decode_utf8(StringView text, uint32_t* out, uint32_t capacity, uint32_t* count) {
  uint32_t n = 0;
  uint64_t i = 0;
  while (i < text.size()) {
    const uint8_t b = static_cast<uint8_t>(text[i]);
    uint32_t cp, extra;
    if (b < 0x80) cp = b, extra = 0;
    else if ((b & 0xE0) == 0xC0) cp = b & 0x1F, extra = 1;
    else if ((b & 0xF0) == 0xE0) cp = b & 0x0F, extra = 2;
    else if ((b & 0xF8) == 0xF0) cp = b & 0x07, extra = 3;
    else return false;
    if (i + extra >= text.size() && extra != 0) return false;
    for (uint32_t k = 1; k <= extra; ++k) {
      const uint8_t c = static_cast<uint8_t>(text[i + k]);
      if ((c & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3F);
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000) || n == capacity) return false;
    out[n++] = cp;
    i += extra + 1;
  }
  *count = n;
  return true;
}

// Characters an 8.3 name may hold (upper case letters, digits and these).
bool alias_char(uint32_t c) {
  if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
  for (const char* p = "$%'-_@~`!(){}^#&"; *p; ++p)
    if (c == static_cast<uint32_t>(*p)) return true;
  return false;
}

bool forbidden_char(uint32_t c) {
  if (c < 0x20 || c == 0x7F) return true;
  for (const char* p = "\"*/:<>?\\|"; *p; ++p)
    if (c == static_cast<uint32_t>(*p)) return true;
  return false;
}

// Is `inner` strictly inside the folder `outer` (both paths, "." skipped)?
bool strictly_inside(StringView outer, StringView inner) {
  StringView o = outer, i = inner, oc, ic;
  for (;;) {
    bool have_o = next_component(&o, &oc);
    while (have_o && is_dot(oc)) have_o = next_component(&o, &oc);
    bool have_i = next_component(&i, &ic);
    while (have_i && is_dot(ic)) have_i = next_component(&i, &ic);
    if (!have_o) return have_i;
    if (!have_i || !oc.equals_ignore_case(ic)) return false;
  }
}

constexpr uint8_t kDot11[11] = {'.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
constexpr uint8_t kDotDot11[11] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
constexpr uint16_t kStampDate = 0x0021;  // 1980-01-01: new entries have no real time
}  // namespace

Status FatFileSystem::flush() {
  kernel::ScopedRecursiveMutex lock(mutex_);
  return device_.flush() ? Status::Ok : Status::IoError;
}

Status FatFileSystem::write_bytes(uint64_t offset, ConstByteView data) {
  if (read_only_) return Status::ReadOnly;
  uint64_t absolute = first_block_ * block_size_ + offset;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t block = absolute / block_size_;
    const uint32_t within = static_cast<uint32_t>(absolute % block_size_);
    const uint64_t left = data.size() - done;
    if (within == 0 && left >= block_size_) {  // whole blocks straight from the caller's buffer
      const uint64_t bytes = left / block_size_ * block_size_;
      if (block_number_ != ~uint64_t{0} && block_number_ >= block && block_number_ < block + bytes / block_size_) block_number_ = ~uint64_t{0};
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
    uint64_t take = block_size_ - within;
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

uint32_t FatFileSystem::free_cluster_count() {
  if (!free_known_) {
    if (cluster_count_ > 262144) return 0;  // scanning a big FAT would stall a mount or a status line: "unknown"
    uint32_t free = 0;
    for (uint32_t c = 2; c < cluster_count_ + 2; ++c) {
      uint32_t value = 0;
      if (get_fat(c, &value) == Status::Ok && value == 0) ++free;
    }
    free_clusters_ = free;
    free_known_ = true;
  }
  return free_clusters_;
}

Status FatFileSystem::zero_cluster(uint32_t cluster) {
  uint8_t zeros[4096] = {};
  const uint64_t base = cluster_offset(cluster);
  for (uint32_t done = 0; done < cluster_bytes_;) {
    const uint32_t n = cluster_bytes_ - done < sizeof zeros ? cluster_bytes_ - done : static_cast<uint32_t>(sizeof zeros);
    const Status s = write_bytes(base + done, ConstByteView(zeros, n));
    if (s != Status::Ok) return s;
    done += n;
  }
  return Status::Ok;
}

Status FatFileSystem::allocate_cluster(uint32_t* result, uint32_t link_from) {
  Status s = writable();
  if (s != Status::Ok) return s;
  if (free_known_ && free_clusters_ == 0) return Status::NoSpace;
  uint32_t c = alloc_hint_ < 2 || alloc_hint_ >= cluster_count_ + 2 ? 2 : alloc_hint_;
  for (uint32_t tried = 0; tried < cluster_count_; ++tried) {
    uint32_t value = 0;
    s = get_fat(c, &value);
    if (s != Status::Ok) return s;
    if (value == 0) {
      s = set_fat(c, end_mark());
      if (s != Status::Ok) return s;
      s = zero_cluster(c);
      if (s == Status::Ok && link_from != 0) s = set_fat(link_from, c);
      if (s != Status::Ok) {
        set_fat(c, 0);
        return s;
      }
      alloc_hint_ = c + 1;
      if (free_known_) --free_clusters_;
      *result = c;
      return Status::Ok;
    }
    c = c + 1 >= cluster_count_ + 2 ? 2 : c + 1;
  }
  return Status::NoSpace;
}

Status FatFileSystem::free_chain(uint32_t first) {
  ++epoch_;
  uint32_t c = first;
  for (uint32_t hops = 0; c >= 2 && hops <= cluster_count_; ++hops) {
    uint32_t next = 0;
    bool end = false;
    const Status link = next_cluster(c, &next, &end);
    const Status s = set_fat(c, 0);
    if (s != Status::Ok) return s;
    if (free_known_) ++free_clusters_;
    if (alloc_hint_ > c) alloc_hint_ = c;
    if (link != Status::Ok || end) break;
    c = next;
  }
  return Status::Ok;
}

Status FatFileSystem::seek_cluster(uint32_t first, uint64_t index, ChainCursor* cursor) {
  if (first < 2) return Status::Corrupt;
  if (cursor->first != first || cursor->epoch != epoch_ || cursor->cluster == 0 || cursor->index > index) {
    cursor->first = first;
    cursor->epoch = epoch_;
    cursor->cluster = first;
    cursor->index = 0;
  }
  while (cursor->index < index) {
    uint32_t next = 0;
    bool end = false;
    const Status s = next_cluster(cursor->cluster, &next, &end);
    if (s != Status::Ok) return s;
    if (end) return Status::Corrupt;  // the chain is shorter than the file says
    cursor->cluster = next;
    ++cursor->index;
  }
  return Status::Ok;
}

Status FatFileSystem::truncate_chain(uint32_t first, uint64_t keep) {
  ChainCursor cursor;
  Status s = seek_cluster(first, keep - 1, &cursor);
  if (s != Status::Ok) return s;
  uint32_t next = 0;
  bool end = false;
  s = next_cluster(cursor.cluster, &next, &end);
  s = set_fat(cursor.cluster, end_mark());
  if (s != Status::Ok) return s;
  ++epoch_;
  return end ? Status::Ok : free_chain(next);
}

Status FatFileSystem::write_chain(uint32_t first, uint64_t offset, const uint8_t* data, uint64_t length, ChainCursor* cursor) {
  uint8_t zeros[4096] = {};
  uint64_t done = 0;
  while (done < length) {
    const uint32_t within = static_cast<uint32_t>((offset + done) % cluster_bytes_);
    uint64_t chunk = cluster_bytes_ - within;
    if (chunk > length - done) chunk = length - done;
    Status s = seek_cluster(first, (offset + done) / cluster_bytes_, cursor);
    if (s != Status::Ok) return s;
    const uint64_t base = cluster_offset(cursor->cluster) + within;
    if (data) {
      s = write_bytes(base, ConstByteView(data + done, chunk));
    } else {
      for (uint64_t part = 0; part < chunk && s == Status::Ok;) {
        const uint64_t n = chunk - part < sizeof zeros ? chunk - part : sizeof zeros;
        s = write_bytes(base + part, ConstByteView(zeros, n));
        part += n;
      }
    }
    if (s != Status::Ok) return s;
    done += chunk;
  }
  return Status::Ok;
}

// Gives the chain of a file of `old_size` bytes the clusters a file of `new_size` bytes needs (all of them freed for 0). Growing zeroes the
// bytes between the two sizes. *first is the file's first cluster (0: none yet) and changes when the chain starts or disappears.
Status FatFileSystem::resize_chain(uint32_t* first, uint32_t old_size, uint32_t new_size) {
  const uint64_t cb = cluster_bytes_;
  const uint64_t have = (uint64_t{old_size} + cb - 1) / cb, need = (uint64_t{new_size} + cb - 1) / cb;
  Status s;
  if (need < have) {
    if (need == 0) {
      s = free_chain(*first);
      *first = 0;
      return s;
    }
    return truncate_chain(*first, need);
  }
  if (new_size <= old_size) return Status::Ok;
  ChainCursor cursor;
  const uint32_t original_first = *first;
  uint32_t last = 0;
  if (have > 0) {
    // The part of the last cluster past the old end may hold stale bytes: they must read as zeros.
    const uint64_t tail_end = have * cb < new_size ? have * cb : new_size;
    s = write_chain(*first, old_size, nullptr, tail_end - old_size, &cursor);
    if (s != Status::Ok) return s;
    s = seek_cluster(*first, have - 1, &cursor);
    if (s != Status::Ok) return s;
    last = cursor.cluster;
  }
  for (uint64_t n = have; n < need; ++n) {
    uint32_t c = 0;
    s = allocate_cluster(&c, last);
    if (s != Status::Ok) {  // give back what this call added
      if (have == 0) {
        if (*first != 0) free_chain(*first);
        *first = original_first;
      } else {
        truncate_chain(original_first, have);
      }
      return s;
    }
    if (last == 0) *first = c;
    last = c;
  }
  return Status::Ok;
}

Status FatFileSystem::update_entry(uint64_t short_offset, uint32_t first, uint32_t size) {
  uint8_t raw[32];
  Status s = read_bytes(short_offset, ByteView(raw, 32));
  if (s != Status::Ok) return s;
  raw[26] = static_cast<uint8_t>(first);
  raw[27] = static_cast<uint8_t>(first >> 8);
  if (kind_ == Kind::Fat32) {
    raw[20] = static_cast<uint8_t>(first >> 16);
    raw[21] = static_cast<uint8_t>(first >> 24);
  }
  for (int i = 0; i < 4; ++i) raw[28 + i] = static_cast<uint8_t>(size >> (8 * i));
  return write_bytes(short_offset, ConstByteView(raw, 32));
}

// Finds `needed` free slots in a row in a directory (growing it if need be) and returns their byte offsets.
Status FatFileSystem::find_slots(const Node& directory, uint32_t needed, uint64_t* offsets) {
  uint32_t run = 0;
  auto consider = [&](uint64_t offset, bool* done) -> Status {
    uint8_t first = 0;
    const Status s = read_bytes(offset, ByteView(&first, 1));
    if (s != Status::Ok) return s;
    if (first == 0x00 || first == 0xE5) {
      offsets[run++] = offset;
      if (run == needed) *done = true;
    } else {
      run = 0;
    }
    return Status::Ok;
  };
  bool done = false;
  if (directory.cluster == 0) {  // the fixed FAT12/16 root cannot grow
    for (uint32_t i = 0; i < root_entries_ && !done; ++i) {
      const Status s = consider(uint64_t{root_dir_start_} * bytes_per_sector_ + uint64_t{i} * 32, &done);
      if (s != Status::Ok) return s;
    }
    return done ? Status::Ok : Status::NoSpace;
  }
  uint32_t cluster = directory.cluster;
  for (uint32_t hops = 0; hops <= cluster_count_; ++hops) {
    for (uint32_t i = 0; i < cluster_bytes_ / 32 && !done; ++i) {
      const Status s = consider(cluster_offset(cluster) + uint64_t{i} * 32, &done);
      if (s != Status::Ok) return s;
    }
    if (done) return Status::Ok;
    uint32_t next = 0;
    bool end = false;
    Status s = next_cluster(cluster, &next, &end);
    if (s != Status::Ok) return s;
    if (end) {  // the directory is full: add a cluster (empty, so all of it is free)
      s = allocate_cluster(&next, cluster);
      if (s != Status::Ok) return s;
    }
    cluster = next;
  }
  return Status::Corrupt;
}

bool FatFileSystem::alias_taken(const Node& directory, const uint8_t name11[11], uint64_t ignore_offset) {
  Position p = start_of(directory);
  uint8_t raw[32];
  for (;;) {
    const uint64_t offset = entry_offset(p);
    if (read_raw_entry(&p, raw) != Status::Ok || raw[0] == 0x00) return false;
    if (raw[0] == 0xE5 || (raw[11] & 0x3F) == kAttrLongName || offset == ignore_offset) continue;
    if (__builtin_memcmp(raw, name11, 11) == 0) return true;
  }
}

// Adds a directory entry called `name` (a long-name entry too when 8.3 cannot spell it) to `parent`. `ignore_offset` names an entry that
// does not count as a clash (the one being renamed).
Status FatFileSystem::make_entry(const Node& parent, StringView name, uint8_t attributes, uint32_t cluster, uint32_t size, uint64_t ignore_offset,
                                 Node* created) {
  Status s = writable();
  if (s != Status::Ok) return s;
  if (parent.entry.type != NodeType::Directory) return Status::NotADirectory;
  if (!valid_name(name)) return Status::InvalidPath;
  uint32_t cps[kMaxNameLength + 1], n = 0;
  if (!decode_utf8(name, cps, kMaxNameLength, &n) || n == 0) return Status::InvalidPath;
  uint16_t units[kMaxNameLength + 2];
  uint32_t unit_count = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (forbidden_char(cps[i])) return Status::InvalidPath;
    if (cps[i] >= 0x10000) {
      units[unit_count++] = static_cast<uint16_t>(0xD800 + ((cps[i] - 0x10000) >> 10));
      units[unit_count++] = static_cast<uint16_t>(0xDC00 + ((cps[i] - 0x10000) & 0x3FF));
    } else {
      units[unit_count++] = static_cast<uint16_t>(cps[i]);
    }
  }
  if (unit_count > 255 || cps[n - 1] == ' ' || cps[n - 1] == '.') return Status::InvalidPath;

  Node existing;
  s = find(parent, name, &existing);
  if (s == Status::Ok) {
    if (existing.short_offset != ignore_offset) return Status::AlreadyExists;
  } else if (s != Status::NotFound) {
    return s;
  }

  // The 8.3 form: upper case, only the characters it allows, 8 + 3 long. Anything lost (or a mix of cases) needs a long name as well.
  uint32_t dot = ~0u;
  for (uint32_t i = 0; i < n; ++i)
    if (cps[i] == '.') dot = i;
  uint32_t base_from = 0, base_to = n, ext_from = n, ext_to = n;
  bool lossy = false;
  if (dot == 0) base_from = 1, lossy = true;        // ".profile": the dot cannot be kept
  else if (dot != ~0u) base_to = dot, ext_from = dot + 1;
  uint8_t name11[11];
  __builtin_memset(name11, ' ', 11);
  bool lower[2] = {false, false}, upper[2] = {false, false};
  auto convert = [&](uint32_t from, uint32_t to, uint8_t* out, uint32_t room, int part) {
    uint32_t length = 0;
    for (uint32_t i = from; i < to; ++i) {
      uint32_t c = cps[i];
      if (c == ' ' || c == '.') {
        lossy = true;
        continue;
      }
      if (c >= 'a' && c <= 'z') lower[part] = true, c -= 'a' - 'A';
      else if (c >= 'A' && c <= 'Z') upper[part] = true;
      if (!alias_char(c)) c = '_', lossy = true;
      if (length < room) out[length++] = static_cast<uint8_t>(c);
      else lossy = true;
    }
    return length;
  };
  uint32_t base_length = convert(base_from, base_to, name11, 8, 0);
  convert(ext_from, ext_to, name11 + 8, 3, 1);
  if (base_length == 0) name11[0] = '_', base_length = 1, lossy = true;
  const bool mixed = (lower[0] && upper[0]) || (lower[1] && upper[1]);
  bool needs_long = lossy || mixed;
  uint8_t flags = 0;
  if (!needs_long) flags = static_cast<uint8_t>((lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0));
  if (!lossy && alias_taken(parent, name11, ignore_offset)) lossy = true, needs_long = true, flags = 0;
  if (lossy) {  // BASE~N: the first free N
    uint8_t base[8];
    __builtin_memcpy(base, name11, 8);
    for (uint32_t number = 1;; ++number) {
      char digits[12];
      uint32_t d = 0;
      for (uint32_t v = number; v; v /= 10) digits[d++] = static_cast<char>('0' + v % 10);
      if (d + 1 > 7) return Status::NoSpace;
      uint32_t keep = base_length < 8 - (d + 1) ? base_length : 8 - (d + 1);
      __builtin_memset(name11, ' ', 8);
      __builtin_memcpy(name11, base, keep);
      name11[keep] = '~';
      for (uint32_t i = 0; i < d; ++i) name11[keep + 1 + i] = static_cast<uint8_t>(digits[d - 1 - i]);
      if (!alias_taken(parent, name11, ignore_offset)) break;
    }
    flags = 0;
    needs_long = true;
  }

  const uint32_t pieces = needs_long ? (unit_count + 12) / 13 : 0;
  uint64_t offsets[22];
  s = find_slots(parent, pieces + 1, offsets);
  if (s != Status::Ok) return s;

  static const uint8_t kOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  const uint8_t checksum = short_name_checksum(name11);
  for (uint32_t k = 0; k < pieces; ++k) {  // the last piece comes first on disk
    const uint32_t ordinal = pieces - k;
    uint8_t raw[32] = {};
    raw[0] = static_cast<uint8_t>(ordinal | (k == 0 ? 0x40 : 0));
    raw[11] = kAttrLongName;
    raw[13] = checksum;
    for (uint32_t i = 0; i < 13; ++i) {
      const uint32_t at = (ordinal - 1) * 13 + i;
      const uint16_t unit = at < unit_count ? units[at] : at == unit_count ? 0x0000 : 0xFFFF;
      raw[kOffsets[i]] = static_cast<uint8_t>(unit);
      raw[kOffsets[i] + 1] = static_cast<uint8_t>(unit >> 8);
    }
    s = write_bytes(offsets[k], ConstByteView(raw, 32));
    if (s != Status::Ok) return s;
  }
  uint8_t raw[32] = {};
  __builtin_memcpy(raw, name11, 11);
  raw[11] = attributes;
  raw[12] = flags;
  for (int i = 0; i < 2; ++i) {
    raw[16 + i] = static_cast<uint8_t>(kStampDate >> (8 * i));  // created
    raw[18 + i] = static_cast<uint8_t>(kStampDate >> (8 * i));  // accessed
    raw[24 + i] = static_cast<uint8_t>(kStampDate >> (8 * i));  // written
  }
  raw[26] = static_cast<uint8_t>(cluster);
  raw[27] = static_cast<uint8_t>(cluster >> 8);
  if (kind_ == Kind::Fat32) {
    raw[20] = static_cast<uint8_t>(cluster >> 16);
    raw[21] = static_cast<uint8_t>(cluster >> 24);
  }
  for (int i = 0; i < 4; ++i) raw[28 + i] = static_cast<uint8_t>(size >> (8 * i));
  s = write_bytes(offsets[pieces], ConstByteView(raw, 32));
  if (s != Status::Ok) return s;
  if (created) {
    *created = Node();
    created->cluster = cluster;
    created->short_offset = offsets[pieces];
    created->slots = pieces + 1;
    created->entry.type = (attributes & kAttrDirectory) ? NodeType::Directory : NodeType::File;
    created->entry.size = size;
  }
  return Status::Ok;
}

Status FatFileSystem::delete_slots(const Node& node) {
  Position p = node.first_pos;
  uint8_t raw[32];
  for (uint32_t i = 0; i < node.slots; ++i) {
    const uint64_t offset = entry_offset(p);
    Status s = read_raw_entry(&p, raw);
    if (s != Status::Ok) return s;
    const uint8_t mark = 0xE5;
    s = write_bytes(offset, ConstByteView(&mark, 1));
    if (s != Status::Ok) return s;
  }
  return Status::Ok;
}

Status FatFileSystem::split(StringView path, Node* parent, StringView* leaf) {
  StringView parent_path;
  if (!split_parent(path, &parent_path, leaf)) return Status::InvalidPath;
  const Status s = resolve(parent_path, parent);
  if (s != Status::Ok) return s;
  return parent->entry.type == NodeType::Directory ? Status::Ok : Status::NotADirectory;
}

bool FatFileSystem::is_open(uint64_t short_offset) const {
  for (const File* f = open_files_; f; f = f->next_open())
    if (f->entry_offset() == short_offset) return true;
  return false;
}

Status FatFileSystem::create_directory(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Status s = writable();
  if (s != Status::Ok) return s;
  Node parent;
  StringView leaf;
  s = split(path, &parent, &leaf);
  if (s != Status::Ok) return s;
  Node existing;
  s = find(parent, leaf, &existing);
  if (s == Status::Ok) return Status::AlreadyExists;
  if (s != Status::NotFound) return s;
  if (!valid_name(leaf)) return Status::InvalidPath;
  uint32_t cluster = 0;
  s = allocate_cluster(&cluster, 0);
  if (s != Status::Ok) return s;
  const bool root_parent = parent.cluster == 0 || (kind_ == Kind::Fat32 && parent.cluster == root_cluster_);
  const uint32_t parent_cluster = root_parent ? 0 : parent.cluster;
  for (int which = 0; which < 2 && s == Status::Ok; ++which) {
    const uint32_t target = which == 0 ? cluster : parent_cluster;
    uint8_t raw[32] = {};
    __builtin_memcpy(raw, which == 0 ? kDot11 : kDotDot11, 11);
    raw[11] = kAttrDirectory;
    for (int i = 0; i < 2; ++i) {
      raw[16 + i] = static_cast<uint8_t>(kStampDate >> (8 * i));
      raw[18 + i] = static_cast<uint8_t>(kStampDate >> (8 * i));
      raw[24 + i] = static_cast<uint8_t>(kStampDate >> (8 * i));
    }
    raw[26] = static_cast<uint8_t>(target);
    raw[27] = static_cast<uint8_t>(target >> 8);
    if (kind_ == Kind::Fat32) {
      raw[20] = static_cast<uint8_t>(target >> 16);
      raw[21] = static_cast<uint8_t>(target >> 24);
    }
    s = write_bytes(cluster_offset(cluster) + static_cast<uint64_t>(which) * 32, ConstByteView(raw, 32));
  }
  if (s == Status::Ok) s = make_entry(parent, leaf, kAttrDirectory, cluster, 0, 0, nullptr);
  if (s != Status::Ok) free_chain(cluster);
  return s;
}

Status FatFileSystem::remove(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Status s = writable();
  if (s != Status::Ok) return s;
  Node node;
  s = resolve(path, &node);
  if (s != Status::Ok) return s;
  if (node.slots == 0) return Status::InvalidPath;  // the root
  if (is_open(node.short_offset)) return Status::Busy;
  if (node.entry.read_only) return Status::ReadOnly;
  if (node.entry.type == NodeType::Directory) {
    Position p = start_of(node);
    Node child;
    s = next_node(&p, &child);
    if (s == Status::Ok) return Status::NotEmpty;
    if (s != Status::EndOfDirectory) return s;
  }
  s = delete_slots(node);  // the entry first: a crash then leaks clusters instead of leaving an entry that points at free ones
  if (s != Status::Ok) return s;
  return node.cluster >= 2 ? free_chain(node.cluster) : Status::Ok;
}

Status FatFileSystem::rename(StringView from, StringView to) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Status s = writable();
  if (s != Status::Ok) return s;
  Node source;
  s = resolve(from, &source);
  if (s != Status::Ok) return s;
  if (source.slots == 0) return Status::InvalidPath;
  if (is_open(source.short_offset)) return Status::Busy;
  Node target_parent;
  StringView leaf;
  s = split(to, &target_parent, &leaf);
  if (s != Status::Ok) return s;
  if (source.entry.type == NodeType::Directory && strictly_inside(from, to)) return Status::InvalidArgument;
  Node clash;
  s = find(target_parent, leaf, &clash);
  if (s == Status::Ok) {
    if (clash.short_offset != source.short_offset) return Status::AlreadyExists;  // (the same entry: only the case changes)
  } else if (s != Status::NotFound) {
    return s;
  }
  uint8_t old_raw[32];
  s = read_bytes(source.short_offset, ByteView(old_raw, 32));
  if (s != Status::Ok) return s;
  Node created;
  s = make_entry(target_parent, leaf, old_raw[11], source.cluster, static_cast<uint32_t>(source.entry.size), source.short_offset, &created);
  if (s != Status::Ok) return s;
  uint8_t new_raw[32];  // keep the time stamps
  s = read_bytes(created.short_offset, ByteView(new_raw, 32));
  if (s != Status::Ok) return s;
  __builtin_memcpy(new_raw + 13, old_raw + 13, 7);
  __builtin_memcpy(new_raw + 22, old_raw + 22, 4);
  s = write_bytes(created.short_offset, ConstByteView(new_raw, 32));
  if (s != Status::Ok) return s;
  s = delete_slots(source);
  if (s != Status::Ok) return s;
  if (source.entry.type == NodeType::Directory && source.cluster >= 2) {  // ".." follows the directory to its new parent
    const bool root_parent = target_parent.cluster == 0 || (kind_ == Kind::Fat32 && target_parent.cluster == root_cluster_);
    const uint32_t parent_cluster = root_parent ? 0 : target_parent.cluster;
    uint8_t field[6];
    field[0] = static_cast<uint8_t>(parent_cluster >> 16);
    field[1] = static_cast<uint8_t>(parent_cluster >> 24);
    s = write_bytes(cluster_offset(source.cluster) + 32 + 20, ConstByteView(field, kind_ == Kind::Fat32 ? 2 : 0));
    if (s != Status::Ok) return s;
    field[0] = static_cast<uint8_t>(parent_cluster);
    field[1] = static_cast<uint8_t>(parent_cluster >> 8);
    s = write_bytes(cluster_offset(source.cluster) + 32 + 26, ConstByteView(field, 2));
  }
  return s;
}

}  // namespace filesystem
