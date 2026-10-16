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

class FatFileSystem::File final : public IFile {
 public:
  File(FatFileSystem& fs, const Node& node) : fs_(fs), first_(node.cluster), size_(node.entry.size) {}

  uint64_t size() const override { return size_; }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (offset >= size_ || buffer.empty()) return Status::Ok;
    uint64_t remaining = size_ - offset;
    if (remaining > buffer.size()) remaining = buffer.size();
    const uint32_t cluster_bytes = fs_.cluster_bytes_;
    const uint64_t target = offset / cluster_bytes;
    if (!cached_cluster_ || cached_index_ > target) {
      cached_cluster_ = first_;
      cached_index_ = 0;
    }
    while (cached_index_ < target) {
      uint32_t next = 0;
      bool end = false;
      Status s = fs_.next_cluster(cached_cluster_, &next, &end);
      if (s != Status::Ok) return s;
      if (end) return Status::Corrupt;  // the chain is shorter than the file says
      cached_cluster_ = next;
      ++cached_index_;
    }
    uint64_t done = 0;
    while (remaining) {
      const uint32_t within = static_cast<uint32_t>((offset + done) % cluster_bytes);
      uint64_t chunk = cluster_bytes - within;
      if (chunk > remaining) chunk = remaining;
      Status s = fs_.read_bytes(fs_.cluster_offset(cached_cluster_) + within, buffer.subview(done, chunk));
      if (s != Status::Ok) {
        *transferred = done;
        return s;
      }
      done += chunk;
      remaining -= chunk;
      if (within + chunk == cluster_bytes && remaining) {
        uint32_t next = 0;
        bool end = false;
        s = fs_.next_cluster(cached_cluster_, &next, &end);
        if (s == Status::Ok && end) s = Status::Corrupt;
        if (s != Status::Ok) {
          *transferred = done;
          return s;
        }
        cached_cluster_ = next;
        ++cached_index_;
      }
    }
    *transferred = done;
    return Status::Ok;
  }
  Status write(uint64_t, ConstByteView, uint64_t* transferred) override {
    *transferred = 0;
    return Status::ReadOnly;
  }
  Status resize(uint64_t) override { return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }

 private:
  FatFileSystem& fs_;
  const uint32_t first_;
  const uint64_t size_;
  uint32_t cached_cluster_ = 0;  // cluster number at chain index cached_index_ (0: nothing cached)
  uint64_t cached_index_ = 0;
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
  result.read_only = true;
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

// The FAT entry for `cluster`: *end is true for an end-of-chain mark. A free,
// bad or out-of-range link is Corrupt.
Status FatFileSystem::next_cluster(uint32_t cluster, uint32_t* next, bool* end) {
  if (cluster < 2 || cluster >= cluster_count_ + 2) return Status::Corrupt;
  uint8_t raw[4] = {};
  uint32_t value = 0, end_mark = 0;
  if (kind_ == Kind::Fat12) {
    Status s = read_bytes(uint64_t{fat_start_} * bytes_per_sector_ + cluster + cluster / 2, ByteView(raw, 2));
    if (s != Status::Ok) return s;
    value = le16(raw);
    value = (cluster & 1) ? value >> 4 : value & 0xFFF;
    end_mark = 0xFF8;
  } else if (kind_ == Kind::Fat16) {
    Status s = read_bytes(uint64_t{fat_start_} * bytes_per_sector_ + uint64_t{cluster} * 2, ByteView(raw, 2));
    if (s != Status::Ok) return s;
    value = le16(raw);
    end_mark = 0xFFF8;
  } else {
    Status s = read_bytes(uint64_t{fat_start_} * bytes_per_sector_ + uint64_t{cluster} * 4, ByteView(raw, 4));
    if (s != Status::Ok) return s;
    value = le32(raw) & 0x0FFFFFFF;
    end_mark = 0x0FFFFFF8;
  }
  *end = value >= end_mark;
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
  uint8_t raw[32];
  for (;;) {
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
      } else if (lfn_expected != ordinal || raw[13] != lfn_checksum) {
        lfn_expected = 0, lfn_total = 0;
        continue;
      }
      static const uint8_t kOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
      for (uint32_t i = 0; i < 13; ++i) lfn[(ordinal - 1) * 13 + i] = le16(raw + kOffsets[i]);
      lfn_expected = ordinal - 1;
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
    lfn_total = 0;
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
  if (mode != OpenMode::Read) return Status::ReadOnly;
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node node;
  const Status s = resolve(path, &node);
  if (s != Status::Ok) return s;
  if (node.entry.type == NodeType::Directory) return Status::IsADirectory;
  UniquePointer<IFile> handle = UniquePointer<IFile>::make_as<File>(allocator_, *this, node);
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

}  // namespace filesystem
