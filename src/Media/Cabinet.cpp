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

#include "Media/Cabinet.hpp"
#include "Media/Lzx.hpp"
#include "Media/Zlib.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {

constexpr uint32_t kSignature = 0x4643534D;      // "MSCF"
constexpr uint64_t kHeaderSize = 36, kReserveSizes = 4, kFolderSize = 8, kFileSize = 16, kDataSize = 8;
constexpr uint16_t kFlagPrevious = 1, kFlagNext = 2, kFlagReserve = 4;
constexpr uint16_t kStored = 0, kMszip = 1, kLzx = 3;
constexpr uint64_t kMaxName = 256;
constexpr uint32_t kBlockSize = 32768;
constexpr uint32_t kMaxPacked = kBlockSize + 6144;      // what a block of 32768 bytes may at most compress to
constexpr uint16_t kFirstContinuedFolder = 0xFFFD;      // the folder numbers of files that go on from or into another cabinet

// Reads `count` bytes at `offset` of the source into `out` (replacing what it holds). False if the source does not have them.
bool read_at(IZipSource& source, uint64_t offset, uint64_t count, ByteBuffer* out) {
  out->clear();
  if (offset > source.size() || count > source.size() - offset) return false;
  return out->put_zeros(count) && source.read(offset, out->writable_view());
}

// The checksum of a cabinet's data blocks: the exclusive or of the data as 32 bit little-endian words, the one to three bytes left over taken with the
// first as the most significant.
uint32_t checksum(ConstByteView data, uint32_t seed) {
  uint32_t value = seed;
  const uint64_t whole = data.size() & ~uint64_t{3};
  ByteReader words(data.first(whole));
  while (!words.at_end()) value ^= words.le32();
  uint32_t last = 0;
  for (const uint8_t byte : data.subview(whole)) last = (last << 8) | byte;
  return value ^ last;
}

// One folder, decoded block by block from its start as its files are asked for.
class FolderStream {
 public:
  FolderStream(allocators::IAllocator& allocator, IZipSource& source, uint64_t data_offset, uint32_t blocks, uint16_t method, uint16_t window_bits,
               uint32_t data_reserve)
      : source_(source), header_(allocator), packed_(allocator), plain_(allocator), lzx_(allocator), next_offset_(data_offset), blocks_left_(blocks),
        method_(method), window_bits_(window_bits), data_reserve_(data_reserve) {}

  // Hands the `size` bytes at `offset` of the folder's contents to the sink. Offsets must not go backwards from call to call.
  bool deliver(uint64_t offset, uint64_t size, ICabinetSink& sink) {
    if (failed_ || offset < block_start_) return false;
    while (size != 0) {
      while (offset - block_start_ >= current_.size()) {      // (also passes over the blocks of files that were not wanted)
        if (!next_block()) {
          failed_ = true;      // (a compressed stream cannot be picked up again after a bad block)
          return false;
        }
      }
      const uint64_t in_block = offset - block_start_;
      const uint64_t left = current_.size() - in_block;
      const uint64_t count = size < left ? size : left;
      if (!sink.write(current_.subview(in_block, count))) return false;
      offset += count;
      size -= count;
    }
    return true;
  }

 private:
  bool next_block() {
    block_start_ += current_.size();
    current_ = ConstByteView();
    if (blocks_left_ == 0) return false;
    if (!read_at(source_, next_offset_, kDataSize + data_reserve_, &header_)) return false;
    ByteReader header(header_.view());
    const uint32_t stored_checksum = header.le32();
    const uint32_t packed_size = header.le16(), plain_size = header.le16();
    // (An unpacked size of 0 marks a block that goes on in the next cabinet.)
    if (!header.ok() || plain_size == 0 || plain_size > kBlockSize || packed_size > kMaxPacked) return false;
    const uint64_t data_offset = next_offset_ + kDataSize + data_reserve_;      // (the header was read from there: cannot wrap)
    if (!read_at(source_, data_offset, packed_size, &packed_)) return false;
    if (stored_checksum != 0 && checksum(header_.view().subview(4, 4), checksum(packed_.view(), 0)) != stored_checksum) return false;

    switch (method_) {
      case kStored:
        if (packed_size != plain_size) return false;
        current_ = packed_.view();
        break;
      case kMszip: {
        // "CK", then deflate data that may refer to the block before: that one is kept in front of this one's output.
        const uint64_t history = current_size_;
        if (plain_.size() > history) {
          const uint64_t before = plain_.size() - history;
          plain_.writable_view().first(history).copy_from(plain_.view().subview(before, history));
          plain_.truncate(history);
        }
        ByteReader data(packed_.view());
        if (data.u8() != 'C' || data.u8() != 'K' || !data.ok()) return false;
        current_size_ = 0;      // (if this fails, what plain_ holds is no history any more)
        if (!inflate_with_history(data.rest(), &plain_, plain_size, history) || plain_.size() - history != plain_size) return false;
        current_ = plain_.view().subview(history);
        break;
      }
      case kLzx:
        if (!started_ && !lzx_.reset(window_bits_)) return false;
        plain_.clear();
        if (!lzx_.decode_block(packed_.view(), plain_size, &plain_)) return false;
        current_ = plain_.view();
        break;
      default:
        return false;
    }
    started_ = true;
    current_size_ = plain_size;
    next_offset_ = data_offset + packed_size;
    --blocks_left_;
    return true;
  }

  IZipSource& source_;
  ByteBuffer header_, packed_, plain_;
  LzxDecoder lzx_;
  ConstByteView current_;           // the block decoded last: the folder's bytes from block_start_ on
  uint64_t block_start_ = 0;
  uint64_t current_size_ = 0;       // of the block decoded last (MSZIP: the history of the next)
  uint64_t next_offset_;            // of the next block's header in the cabinet
  uint32_t blocks_left_;
  uint16_t method_, window_bits_;
  uint32_t data_reserve_;
  bool started_ = false;
  bool failed_ = false;
};

// Collects one file.
class BufferSink final : public ICabinetSink {
 public:
  explicit BufferSink(ByteBuffer* out) : out_(*out) {}
  [[nodiscard]] bool begin_file(const CabinetEntry&, bool*) override { return true; }
  [[nodiscard]] bool write(ConstByteView bytes) override { return out_.append(bytes); }
  [[nodiscard]] bool end_file() override {
    done_ = true;
    return true;
  }
  [[nodiscard]] bool same_file(const CabinetEntry&, const CabinetEntry&) override { return true; }      // (only one file is asked for)
  bool done() const { return done_; }

 private:
  ByteBuffer& out_;
  bool done_ = false;
};

}  // namespace

bool CabinetReader::open() {
  folders_.clear();
  entries_.clear();
  order_.clear();
  data_reserve_ = 0;

  ByteBuffer block(allocator_);
  if (!read_at(source_, 0, kHeaderSize, &block)) return false;
  ByteReader header(block.view());
  if (header.le32() != kSignature) return false;
  header.skip(4 + 4 + 4);      // reserved, the cabinet's size (a signed cabinet is longer than it says), reserved
  const uint64_t files_offset = header.le32();
  header.skip(4 + 2);          // reserved, the format's version
  const uint64_t folder_count = header.le16(), file_count = header.le16();
  const uint16_t flags = header.le16();
  if (!header.ok()) return false;
  if ((flags & (kFlagPrevious | kFlagNext)) != 0) return false;      // one of a set of cabinets
  if (file_count > kMaxEntries || (file_count != 0 && folder_count == 0)) return false;

  // The sizes of the areas the cabinet's maker reserved (a signature lives in the header's), then the header's area itself.
  uint64_t folders_offset = kHeaderSize, folder_reserve = 0;
  if ((flags & kFlagReserve) != 0) {
    if (!read_at(source_, kHeaderSize, kReserveSizes, &block)) return false;
    ByteReader reserve(block.view());
    const uint64_t header_reserve = reserve.le16();
    folder_reserve = reserve.u8();
    data_reserve_ = reserve.u8();
    if (!reserve.ok()) return false;
    folders_offset = kHeaderSize + kReserveSizes + header_reserve;
  }

  const uint64_t folder_size = kFolderSize + folder_reserve;
  if (!read_at(source_, folders_offset, folder_count * folder_size, &block) || !folders_.reserve(folder_count)) return false;
  ByteReader folders(block.view());
  for (uint64_t i = 0; i < folder_count; ++i) {
    Folder folder;
    folder.data_offset = folders.le32();
    folder.blocks = folders.le16();
    const uint16_t compression = folders.le16();
    folders.skip(folder_reserve);
    if (!folders.ok()) return false;
    folder.method = compression & 0x000F;
    folder.window_bits = (compression >> 8) & 0x1F;
    if (folder.method != kStored && folder.method != kMszip && folder.method != kLzx) return false;
    if (folder.method == kLzx && (folder.window_bits < LzxDecoder::kMinWindowBits || folder.window_bits > LzxDecoder::kMaxWindowBits)) return false;
    if (folder.data_offset > source_.size() || !folders_.push_back(folder)) return false;
  }

  // The files: a fixed part and a name that ends in a zero byte, one after another.
  if (files_offset > source_.size()) return false;
  const uint64_t available = source_.size() - files_offset, most = file_count * (kFileSize + kMaxName + 1);
  if (!read_at(source_, files_offset, available < most ? available : most, &block) || !entries_.reserve(file_count)) return false;
  ByteReader files(block.view());
  for (uint64_t i = 0; i < file_count; ++i) {
    CabinetEntry entry(allocator_);
    entry.size = files.le32();
    entry.folder_offset = files.le32();
    entry.folder = files.le16();
    entry.date = files.le16();
    entry.time = files.le16();
    entry.attributes = files.le16();
    if (!files.ok()) return false;
    uint64_t length = 0;
    uint8_t byte = 0;
    while (files.peek_u8(length, &byte) && byte != 0 && length <= kMaxName) ++length;
    if (length == 0 || length > kMaxName || !files.peek_u8(length, &byte)) return false;      // no name, too long a one, or its end is missing
    const ConstByteView name = files.take(length);
    files.skip(1);
    if (entry.folder >= kFirstContinuedFolder || entry.folder >= folder_count) return false;
    ++folders_[entry.folder].count;
    if (!entry.name.assign(StringView(name)) || !entries_.push_back(util::move(entry))) return false;
  }

  // The files of each folder, in the order the cabinet lists them - which has to be the order they lie in, without overlap: they are decoded in one pass.
  uint32_t first = 0;
  for (Folder& folder : folders_.view()) {
    folder.first = first;
    first += folder.count;
    folder.count = 0;
  }
  if (!order_.reserve(file_count)) return false;
  for (uint64_t i = 0; i < file_count; ++i)
    if (!order_.push_back(0)) return false;
  for (uint32_t i = 0; i < file_count; ++i) {
    Folder& folder = folders_[entries_[i].folder];
    order_[folder.first + folder.count++] = i;
  }
  // In each folder, in the order the files lie in it (a heap sort by offset, then by place in the list). They must not overlap - except that a file may
  // be the very bytes of the one before it: the same data under another name.
  for (const Folder& folder : folders_.view()) {
    const ArrayView<uint32_t> files = order_.view().subview(folder.first, folder.count);
    auto before = [&](uint32_t a, uint32_t b) {
      const uint64_t x = entries_[a].folder_offset, y = entries_[b].folder_offset;
      return x != y ? x < y : entries_[a].size != entries_[b].size ? entries_[a].size < entries_[b].size : a < b;
    };
    auto sift = [&](uint64_t root, uint64_t end) {
      for (;;) {
        uint64_t child = root * 2 + 1;
        if (child >= end) return;
        if (child + 1 < end && before(files[child], files[child + 1])) ++child;
        if (!before(files[root], files[child])) return;
        const uint32_t t = files[root];
        files[root] = files[child];
        files[child] = t;
        root = child;
      }
    };
    for (uint64_t start = files.size() / 2; start > 0; --start) sift(start - 1, files.size());
    for (uint64_t end = files.size(); end > 1; --end) {
      const uint32_t t = files[0];
      files[0] = files[end - 1];
      files[end - 1] = t;
      sift(0, end - 1);
    }
    uint64_t start = 0, end = 0;
    const uint64_t limit = uint64_t{folder.blocks} * kBlockSize;
    for (const uint32_t index : files) {
      const CabinetEntry& entry = entries_[index];
      if (entry.folder_offset > limit || entry.size > limit - entry.folder_offset) return false;
      const bool same = entry.folder_offset == start && entry.folder_offset + entry.size == end && index != files[0];
      if (entry.folder_offset < end && !same) return false;
      start = entry.folder_offset;
      end = entry.folder_offset + entry.size;
    }
  }
  return true;
}

const CabinetEntry* CabinetReader::find(StringView name) const {
  for (const CabinetEntry& entry : entries_.view())
    if (entry.name.view().equals(name)) return &entry;
  return nullptr;
}

bool CabinetReader::walk(ICabinetSink& sink, const CabinetEntry* only) {
  for (const Folder& folder : folders_.view()) {
    if (only && &folder != &folders_.view()[only->folder]) continue;
    FolderStream stream(allocator_, source_, folder.data_offset, folder.blocks, folder.method, folder.window_bits, data_reserve_);
    // Files that are the same bytes follow one another: the first of them that the sink wants is written, the others are told to be its copies.
    const CabinetEntry* previous = nullptr;      // the last file looked at
    const CabinetEntry* written = nullptr;       // the file of the same bytes as `previous` that was written, if one was
    for (const uint32_t index : order_.view().subview(folder.first, folder.count)) {
      const CabinetEntry& entry = entries_.view()[index];
      if (only && &entry != only) continue;
      const bool same = previous && entry.folder_offset == previous->folder_offset && entry.size == previous->size;
      if (!same) written = nullptr;
      previous = &entry;
      if (written) {
        if (!sink.same_file(entry, *written)) return false;
        continue;
      }
      bool wanted = true;
      if (!sink.begin_file(entry, &wanted)) return false;
      if (!wanted) continue;
      if (!stream.deliver(entry.folder_offset, entry.size, sink) || !sink.end_file()) return false;
      written = &entry;
    }
  }
  return true;
}

bool CabinetReader::extract_all(ICabinetSink& sink) { return walk(sink, nullptr); }

bool CabinetReader::extract(const CabinetEntry& entry, uint64_t max_size, ByteBuffer* out) {
  const ArrayView<const CabinetEntry> all = entries_.view();
  if (all.empty() || &entry < all.begin() || &entry >= all.end() || entry.size > max_size) return false;
  const uint64_t start = out->size();
  BufferSink sink(out);
  if (walk(sink, &entry) && sink.done() && out->size() - start == entry.size) return true;
  out->truncate(start);
  return false;
}

}  // namespace media
