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

#include "FileSystem/Dwarfs/DwarfsMetadata.hpp"

namespace filesystem::dwarfs {

namespace {

// Thrift compact protocol type ids.
enum CompactType : uint8_t {
  kStop = 0, kTrue = 1, kFalse = 2, kByte = 3, kI16 = 4, kI32 = 5, kI64 = 6, kDouble = 7, kBinary = 8, kList = 9, kSet = 10, kMap = 11,
  kStruct = 12, kUuid = 13,
};

constexpr int kMaxNesting = 32;
constexpr int32_t kMaxLayouts = 4096;
// A list whose items take no bits at all (every item zero) is only bounded by its count. Such a list never has more items than there are
// inodes or entries, and those lists do take bits, so a real image has fewer than one per bit of metadata (and at least a few).
constexpr uint64_t kMinEmptyItems = 4096;

// Reads the Thrift compact protocol. A read past the end, or a varint longer than ten bytes, sets failed() and returns zero.
class CompactReader {
 public:
  explicit CompactReader(ConstByteView data) : data_(data) {}
  bool failed() const { return failed_; }

  uint8_t byte() {
    if (failed_ || position_ >= data_.size()) {
      failed_ = true;
      return 0;
    }
    return data_[position_++];
  }
  uint64_t varint() {
    uint64_t value = 0;
    for (int shift = 0; shift < 70; shift += 7) {
      const uint8_t b = byte();
      if (failed_) return 0;
      if (shift == 63 && b > 1) break;
      value |= uint64_t(b & 0x7F) << shift;
      if (!(b & 0x80)) return value;
    }
    failed_ = true;
    return 0;
  }
  int64_t zigzag() {
    const uint64_t v = varint();
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
  }
  bool skip_bytes(uint64_t count) {
    if (failed_ || count > data_.size() - position_) return fail();
    position_ += count;
    return true;
  }
  // A field header: false at the stop byte (or on failure). *type keeps the boolean value for kTrue/kFalse.
  bool field(int16_t* last_id, int16_t* id, uint8_t* type) {
    const uint8_t b = byte();
    if (failed_ || b == kStop) return false;
    *type = b & 0x0F;
    const uint8_t delta = b >> 4;
    const int64_t next = delta ? int64_t(*last_id) + delta : zigzag();
    if (next < -32768 || next > 32767) return fail();
    *id = *last_id = static_cast<int16_t>(next);
    return !failed_;
  }
  // A list or set header.
  bool collection(uint64_t* size, uint8_t* element) {
    const uint8_t b = byte();
    *size = b >> 4;
    *element = b & 0x0F;
    if (*size == 15) *size = varint();
    return !failed_ && *size <= data_.size();
  }
  // A map header; the key and value types are only there when the map is not empty.
  bool map(uint64_t* size, uint8_t* key, uint8_t* value) {
    *size = varint();
    *key = *value = 0;
    if (*size) {
      const uint8_t types = byte();
      *key = types >> 4;
      *value = types & 0x0F;
    }
    return !failed_ && *size <= data_.size();
  }
  // Skips a value of `type` (as a field: a bool field carries its value in the type and has no body).
  bool skip(uint8_t type, int depth, bool in_collection = false) {
    if (depth > kMaxNesting) return fail();
    switch (type) {
      case kTrue:
      case kFalse: return in_collection ? skip_bytes(1) : true;
      case kByte: return skip_bytes(1);
      case kI16:
      case kI32:
      case kI64: varint(); return !failed_;
      case kDouble: return skip_bytes(8);
      case kUuid: return skip_bytes(16);
      case kBinary: return skip_bytes(varint());
      case kList:
      case kSet: {
        uint64_t size;
        uint8_t element;
        if (!collection(&size, &element)) return fail();
        for (uint64_t i = 0; i < size; ++i)
          if (!skip(element, depth + 1, true)) return false;
        return true;
      }
      case kMap: {
        uint64_t size;
        uint8_t key, value;
        if (!map(&size, &key, &value)) return fail();
        for (uint64_t i = 0; i < size; ++i)
          if (!skip(key, depth + 1, true) || !skip(value, depth + 1, true)) return false;
        return true;
      }
      case kStruct: {
        int16_t last = 0, id;
        uint8_t field_type;
        while (field(&last, &id, &field_type))
          if (!skip(field_type, depth + 1)) return false;
        return !failed_;
      }
      default: return fail();
    }
  }
  bool fail() {
    failed_ = true;
    return false;
  }

 private:
  ConstByteView data_;
  uint64_t position_ = 0;
  bool failed_ = false;
};

bool small_int(CompactReader& reader, uint8_t type, int64_t low, int64_t high, int64_t* value) {
  if (type != kI16 && type != kI32 && type != kI64) return reader.fail();
  *value = reader.zigzag();
  return !reader.failed() && *value >= low && *value <= high;
}

}  // namespace

// Fills a FrozenSchema from its compact encoding.
class SchemaParser {
 public:
  SchemaParser(FrozenSchema& schema, ConstByteView data) : schema_(schema), reader_(data) {}

  bool parse() {
    int16_t last = 0, id;
    uint8_t type;
    bool have_root = false;
    while (reader_.field(&last, &id, &type)) {
      int64_t value = 0;
      if (id == 4) {      // fileVersion: Frozen2 is at version 1; a newer layout scheme cannot be read
        if (!small_int(reader_, type, -0x7FFFFFFF, 0x7FFFFFFF, &value) || value > 1) return false;
      } else if (id == 2 && type == kMap) {
        if (!layouts()) return false;
      } else if (id == 3) {
        if (!small_int(reader_, type, 0, kMaxLayouts - 1, &value)) return false;
        schema_.root_ = static_cast<int16_t>(value);
        have_root = true;
      } else if (!reader_.skip(type, 1)) {
        return false;
      }
    }
    if (reader_.failed() || !have_root || !schema_.layout(schema_.root_)) return false;
    for (const FrozenField& f : schema_.fields_.view())
      if (!schema_.layout(f.layout)) return false;
    return true;
  }

 private:
  bool layouts() {
    uint64_t count;
    uint8_t key, value;
    if (!reader_.map(&count, &key, &value)) return false;
    if (count && (key != kI16 || value != kStruct)) return false;
    for (uint64_t i = 0; i < count; ++i) {
      int64_t id;
      if (!small_int(reader_, kI16, 0, kMaxLayouts - 1, &id)) return false;
      while (schema_.layouts_.count() <= uint64_t(id))
        if (!schema_.layouts_.push_back(FrozenLayout())) return false;
      FrozenLayout& layout = schema_.layouts_[id];
      if (layout.present) return false;
      if (!parse_layout(&layout)) return false;
    }
    return true;
  }

  bool parse_layout(FrozenLayout* layout) {
    layout->present = true;
    layout->first_field = static_cast<uint32_t>(schema_.fields_.count());
    int16_t last = 0, id;
    uint8_t type;
    while (reader_.field(&last, &id, &type)) {
      int64_t value = 0;
      if (id == 1) {
        if (!small_int(reader_, type, 0, 0x7FFFFFFF, &value)) return false;
        layout->size = static_cast<int32_t>(value);
      } else if (id == 2) {
        if (!small_int(reader_, type, 0, 0x7FFF, &value)) return false;
        layout->bits = static_cast<int16_t>(value);
      } else if (id == 3 && type == kMap) {
        uint64_t count;
        uint8_t key, item;
        if (!reader_.map(&count, &key, &item)) return false;
        if (count && (key != kI16 || item != kStruct)) return false;
        for (uint64_t i = 0; i < count; ++i) {
          FrozenField field;
          if (!small_int(reader_, kI16, -32768, 32767, &value)) return false;
          field.id = static_cast<int16_t>(value);
          if (!parse_field(&field)) return false;
          if (!schema_.fields_.push_back(field)) return false;
        }
      } else if (!reader_.skip(type, 2)) {
        return false;
      }
    }
    if (reader_.failed()) return false;
    layout->field_count = static_cast<uint32_t>(schema_.fields_.count() - layout->first_field);
    return true;
  }

  bool parse_field(FrozenField* field) {
    int16_t last = 0, id;
    uint8_t type;
    while (reader_.field(&last, &id, &type)) {
      int64_t value = 0;
      if (id == 1) {
        if (!small_int(reader_, type, 0, kMaxLayouts - 1, &value)) return false;
        field->layout = static_cast<int16_t>(value);
      } else if (id == 2) {
        if (!small_int(reader_, type, -32768, 32767, &value)) return false;
        field->offset = static_cast<int16_t>(value);
      } else if (!reader_.skip(type, 3)) {
        return false;
      }
    }
    return !reader_.failed();
  }

  FrozenSchema& schema_;
  CompactReader reader_;
};

bool FrozenSchema::parse(ConstByteView compact) {
  layouts_.clear();
  fields_.clear();
  SchemaParser parser(*this, compact);
  return parser.parse();
}

const FrozenLayout* FrozenSchema::layout(int32_t id) const {
  if (id < 0 || uint64_t(id) >= layouts_.count() || !layouts_[id].present) return nullptr;
  return &layouts_[id];
}

FrozenRef FrozenView::root() const {
  FrozenRef ref;
  ref.layout = schema_.root();
  return ref;
}

FrozenRef FrozenView::field(FrozenRef owner, int16_t id) const {
  const FrozenLayout* layout = schema_.layout(owner.layout);
  if (!layout) return FrozenRef();
  for (const FrozenField& f : schema_.fields(*layout)) {
    if (f.id != id) continue;
    FrozenRef ref;
    ref.layout = f.layout;
    ref.byte = owner.byte + (f.offset >= 0 ? uint64_t(f.offset) : 0);
    ref.bit = owner.bit + (f.offset < 0 ? uint64_t(-int32_t(f.offset)) : 0);
    return ref;
  }
  return FrozenRef();
}

bool FrozenView::read_uint(FrozenRef at, uint64_t* value) const {
  *value = 0;
  const FrozenLayout* layout = schema_.layout(at.layout);
  if (!layout) return true;
  const uint64_t width = layout->bits > 0 ? uint64_t(layout->bits) : uint64_t(layout->size) * 8;
  if (width == 0) return true;
  if (width > 64 || at.byte > data_.size()) return false;
  const uint64_t available = (data_.size() - at.byte) * 8;
  if (at.bit > available || width > available - at.bit) return false;
  const ConstByteView bytes = data_.subview(at.byte);
  uint64_t position = at.bit, result = 0;
  uint32_t got = 0;
  while (got < width) {
    const uint32_t shift = position & 7, take = static_cast<uint32_t>(8 - shift < width - got ? 8 - shift : width - got);
    const uint64_t piece = (bytes[position >> 3] >> shift) & ((1u << take) - 1);
    result |= piece << got;
    got += take;
    position += take;
  }
  *value = result;
  return true;
}

bool FrozenView::read_bool(FrozenRef at, bool* value) const {
  uint64_t raw = 0;
  if (!read_uint(at, &raw)) return false;
  *value = raw != 0;
  return true;
}

bool FrozenView::read_optional(FrozenRef at, bool* is_set, FrozenRef* value) const {
  if (!bool_field(at, 1, is_set)) return false;
  *value = *is_set ? field(at, 2) : FrozenRef();
  return true;
}

bool FrozenView::read_array(FrozenRef at, FrozenArray* array) const {
  *array = FrozenArray();
  uint64_t count = 0, distance = 0;
  if (!uint_field(at, 2, &count)) return false;
  if (count == 0) return true;
  if (!uint_field(at, 1, &distance)) return false;
  const FrozenRef item = field(at, 3);
  array->item_layout = item.layout;
  if (const FrozenLayout* layout = schema_.layout(item.layout)) {
    if (layout->size > 0) array->stride_bytes = uint64_t(layout->size);
    else array->stride_bits = uint64_t(layout->bits);
  }
  if (at.byte > data_.size() || distance > data_.size() - at.byte) return false;
  array->start = at.byte + distance;
  const uint64_t room = data_.size() - array->start;
  if (array->stride_bytes) {
    if (count > room / array->stride_bytes) return false;
  } else if (array->stride_bits) {
    if (count > (room * 8) / array->stride_bits) return false;
  } else if (count > kMinEmptyItems && count / 8 > data_.size()) {
    return false;
  }
  array->count = count;
  return true;
}

FrozenRef FrozenView::item(const FrozenArray& array, uint64_t index) const {
  FrozenRef ref;
  ref.layout = array.item_layout;
  ref.byte = array.start + array.stride_bytes * index;
  ref.bit = array.stride_bits * index;
  return ref;
}

bool FrozenView::read_string(FrozenRef at, ConstByteView* bytes) const {
  *bytes = ConstByteView();
  uint64_t count = 0, distance = 0;
  if (!uint_field(at, 2, &count)) return false;
  if (count == 0) return true;
  if (!uint_field(at, 1, &distance)) return false;
  if (at.byte > data_.size() || distance > data_.size() - at.byte) return false;
  const uint64_t start = at.byte + distance;
  if (count > data_.size() - start) return false;
  *bytes = data_.subview(start, count);
  return true;
}

// ---- string tables

bool StringTable::load_plain(const FrozenView& view, FrozenRef list) {
  FrozenArray array;
  if (!view.read_array(list, &array)) return false;
  data_.clear();
  index_.clear();
  if (!index_.reserve(array.count + 1) || !index_.push_back(0)) return false;
  for (uint64_t i = 0; i < array.count; ++i) {
    ConstByteView text;
    if (!view.read_string(view.item(array, i), &text) || !data_.append(text) || !index_.push_back(data_.size())) return false;
  }
  return true;
}

bool StringTable::load_symbols(ConstByteView symtab) {
  // fsst_import(): an 8-byte version word (FSST_VERSION 20190218 in the high half), a zero-terminated flag, the number of symbols of
  // each length 1 to 8, then the symbols, the ones of 2 to 8 bytes first and the single bytes last, given codes in that order.
  constexpr uint8_t kVersion[4] = {0x0A, 0x14, 0x34, 0x01};
  if (symtab.size() < 17) return false;
  for (int i = 0; i < 4; ++i)
    if (symtab[4 + i] != kVersion[i]) return false;
  const bool zero_terminated = symtab[8] & 1;
  uint8_t histogram[8];
  for (int i = 0; i < 8; ++i) histogram[i] = symtab[9 + i];
  uint32_t code = 0;
  if (zero_terminated) {
    if (histogram[0] == 0) return false;
    --histogram[0];
    symbol_length_[0] = 1;
    symbols_[0][0] = 0;
    code = 1;
  }
  uint64_t position = 17;
  for (uint32_t l = 1; l <= 8; ++l) {
    const uint32_t length = (l & 7) + 1;
    for (uint32_t i = 0; i < histogram[l & 7]; ++i, ++code) {
      if (code >= 255 || length > symtab.size() - position) return false;
      symbol_length_[code] = static_cast<uint8_t>(length);
      for (uint32_t j = 0; j < length; ++j) symbols_[code][j] = symtab[position++];
    }
  }
  return true;
}

bool StringTable::load_compact(const FrozenView& view, FrozenRef table) {
  ConstByteView buffer, symtab;
  bool has_symtab = false, packed = false;
  FrozenRef symtab_ref;
  FrozenArray index;
  if (!view.read_string(view.field(table, 1), &buffer) || !view.read_optional(view.field(table, 2), &has_symtab, &symtab_ref) ||
      !view.read_array(view.field(table, 3), &index) || !view.bool_field(table, 4, &packed))
    return false;
  if (has_symtab && !view.read_string(symtab_ref, &symtab)) return false;
  data_.clear();
  index_.clear();
  if (!data_.append(buffer) || !index_.reserve(index.count + 1)) return false;
  uint64_t total = 0;
  if (packed) {
    if (!index_.push_back(0)) return false;
    for (uint64_t i = 0; i < index.count; ++i) {
      uint64_t length;
      if (!view.read_uint(view.item(index, i), &length) || length > buffer.size() - total) return false;
      total += length;
      if (!index_.push_back(total)) return false;
    }
  } else {
    for (uint64_t i = 0; i < index.count; ++i) {
      uint64_t offset;
      if (!view.read_uint(view.item(index, i), &offset) || offset < total || offset > buffer.size() || (i == 0 && offset != 0)) return false;
      total = offset;
      if (!index_.push_back(offset)) return false;
    }
    if (index.count == 0 && !index_.push_back(0)) return false;
  }
  if (total != buffer.size()) return false;
  compressed_ = has_symtab;
  return !compressed_ || load_symbols(symtab);
}

bool StringTable::get(uint64_t index, ByteBuffer* out) const {
  out->clear();
  if (index + 1 >= index_.count()) return false;
  const uint64_t begin = index_[index], end = index_[index + 1];
  const ConstByteView in = data_.view().subview(begin, end - begin);
  if (!compressed_) return out->append(in);
  for (uint64_t i = 0; i < in.size(); ++i) {
    const uint8_t code = in[i];
    if (code == 255) {
      if (++i >= in.size() || !out->put8(in[i])) return false;
      continue;
    }
    const uint8_t length = symbol_length_[code];
    if (length == 0 || !out->append(ConstByteView(symbols_[code], length))) return false;
  }
  return true;
}

// ---- the metadata

namespace {

// metadata.thrift field ids.
namespace id {
constexpr int16_t kChunks = 1, kDirectories = 2, kInodes = 3, kChunkTable = 4, kSymlinkTable = 6, kModes = 9, kNames = 10, kSymlinks = 11,
                  kTimestampBase = 12, kBlockSize = 15, kTotalSize = 16, kOptions = 18, kDirEntries = 19, kSharedFiles = 20, kCompactNames = 24,
                  kCompactSymlinks = 25, kSeparator = 26, kFeatures = 27, kHoleBlock = 34, kLargeHoles = 35;
}

// Reads a list of unsigned numbers of at most 32 bits.
bool read_u32_list(const FrozenView& view, FrozenRef list, List<uint32_t>* out) {
  FrozenArray array;
  if (!view.read_array(list, &array) || !out->reserve(array.count)) return false;
  for (uint64_t i = 0; i < array.count; ++i) {
    uint64_t value;
    if (!view.read_uint(view.item(array, i), &value) || value > 0xFFFFFFFFu || !out->push_back(static_cast<uint32_t>(value))) return false;
  }
  return true;
}

// The bytes of a string from the image as text. Safe: char and uint8_t have the same size, alignment and representation, and the view
// keeps the bounds of the bytes it was made from.
StringView as_text(ConstByteView bytes) { return StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }

int type_order(uint32_t mode) {
  switch (mode & kTypeMask) {
    case kTypeDirectory: return 0;
    case kTypeLink: return 1;
    case kTypeRegular: return 2;
    default: return 3;
  }
}

}  // namespace

Status decode_metadata(allocators::IAllocator& allocator, ConstByteView schema_bytes, ConstByteView data, uint32_t blocks, Metadata* out) {
  FrozenSchema schema(allocator);
  if (!schema.parse(schema_bytes)) return Status::Corrupt;
  const FrozenView view(schema, data);
  const FrozenRef root = view.root();
  bool set = false;
  FrozenRef value;

  // Features this reader does not know change the meaning of the rest.
  if (!view.read_optional(view.field(root, id::kFeatures), &set, &value)) return Status::Corrupt;
  if (set) {
    FrozenArray features;
    if (!view.read_array(value, &features)) return Status::Corrupt;
    for (uint64_t i = 0; i < features.count; ++i) {
      ConstByteView name;
      if (!view.read_string(view.item(features, i), &name)) return Status::Corrupt;
      if (!as_text(name).equals("sparsefiles")) return Status::Unsupported;
    }
  }

  // Options.
  bool packed_chunks = false, packed_directories = false, packed_shared = false, have_links = false;
  uint64_t resolution = 1;
  if (!view.read_optional(view.field(root, id::kOptions), &set, &value)) return Status::Corrupt;
  if (set) {
    FrozenRef res;
    bool res_set = false;
    if (!view.bool_field(value, 3, &packed_chunks) || !view.bool_field(value, 4, &packed_directories) || !view.bool_field(value, 5, &packed_shared) ||
        !view.bool_field(value, 8, &have_links) || !view.read_optional(view.field(value, 2), &res_set, &res))
      return Status::Corrupt;
    if (res_set && (!view.read_uint(res, &resolution) || resolution == 0)) return Status::Corrupt;
  }

  uint64_t base = 0, block_size = 0;
  if (!view.uint_field(root, id::kTimestampBase, &base) || !view.uint_field(root, id::kBlockSize, &block_size) ||
      !view.uint_field(root, id::kTotalSize, &out->total_size))
    return Status::Corrupt;
  if (block_size == 0 || block_size > 0xFFFFFFFFu) return Status::Corrupt;
  out->block_size = static_cast<uint32_t>(block_size);

  if (!view.read_optional(view.field(root, id::kHoleBlock), &set, &value)) return Status::Corrupt;
  if (set) {
    uint64_t hole = 0;
    if (!view.read_uint(value, &hole) || hole > 0xFFFFFFFFu) return Status::Corrupt;
    out->has_holes = true;
    out->hole_block = static_cast<uint32_t>(hole);
  }
  if (!view.read_optional(view.field(root, id::kLargeHoles), &set, &value)) return Status::Corrupt;
  if (set) {
    FrozenArray holes;
    if (!view.read_array(value, &holes) || !out->large_holes.reserve(holes.count)) return Status::Corrupt;
    for (uint64_t i = 0; i < holes.count; ++i) {
      uint64_t size;
      if (!view.read_uint(view.item(holes, i), &size) || !out->large_holes.push_back(size)) return Status::Corrupt;
    }
  }

  if (!view.read_optional(view.field(root, id::kSeparator), &set, &value)) return Status::Corrupt;
  if (set) {
    uint64_t separator = 0;
    if (!view.read_uint(value, &separator)) return Status::Corrupt;
    out->backslash_links = separator == '\\';
  }

  // Directory entries: format 2.3 and later. Before that the inodes doubled as entries, which this reader does not handle.
  if (!view.read_optional(view.field(root, id::kDirEntries), &set, &value)) return Status::Corrupt;
  if (!set) return Status::Unsupported;
  {
    FrozenArray entries;
    if (!view.read_array(value, &entries) || entries.count == 0 || !out->entries.reserve(entries.count)) return Status::Corrupt;
    for (uint64_t i = 0; i < entries.count; ++i) {
      const FrozenRef e = view.item(entries, i);
      uint64_t name = 0, inode = 0;
      if (!view.uint_field(e, 1, &name) || !view.uint_field(e, 2, &inode) || name > 0xFFFFFFFFu || inode > 0xFFFFFFFFu) return Status::Corrupt;
      if (!out->entries.push_back(DirEntry{static_cast<uint32_t>(name), static_cast<uint32_t>(inode)})) return Status::OutOfMemory;
    }
  }

  // Inodes, with their mode looked up and the time made absolute.
  List<uint32_t> modes(allocator);
  if (!read_u32_list(view, view.field(root, id::kModes), &modes)) return Status::Corrupt;
  {
    FrozenArray inodes;
    if (!view.read_array(view.field(root, id::kInodes), &inodes) || inodes.count == 0 || inodes.count > 0xFFFFFFFFu) return Status::Corrupt;
    if (!out->inodes.reserve(inodes.count)) return Status::OutOfMemory;
    for (uint64_t i = 0; i < inodes.count; ++i) {
      const FrozenRef in = view.item(inodes, i);
      uint64_t mode_index = 0, mtime = 0, links = 0;
      if (!view.uint_field(in, 2, &mode_index) || !view.uint_field(in, 7, &mtime) || !view.uint_field(in, 14, &links)) return Status::Corrupt;
      if (mode_index >= modes.count() || links >= 0xFFFFFFFFu) return Status::Corrupt;
      InodeInfo info;
      info.mode = modes[mode_index];
      info.links = static_cast<uint32_t>(links + 1);
      info.modified = resolution * (base + mtime);
      if (!out->inodes.push_back(info)) return Status::OutOfMemory;
    }
  }

  // The order of the inodes gives each kind's range.
  int order = 0;
  for (const InodeInfo& info : out->inodes.view()) {
    const int kind = type_order(info.mode);
    if (kind < order) return Status::Corrupt;
    order = kind;
    if (kind == 0) ++out->directory_count;
    else if (kind == 1) ++out->link_count;
    else if (kind == 2) ++out->file_count;
  }
  if (type_order(out->inodes[0].mode) != 0) return Status::Corrupt;

  // Directories: first entries, delta-encoded when packed.
  {
    FrozenArray dirs;
    if (!view.read_array(view.field(root, id::kDirectories), &dirs) || dirs.count < uint64_t(out->directory_count) + 1) return Status::Corrupt;
    if (!out->directories.reserve(dirs.count)) return Status::OutOfMemory;
    uint64_t running = 0;
    for (uint64_t i = 0; i < dirs.count; ++i) {
      uint64_t first = 0;
      if (!view.uint_field(view.item(dirs, i), 2, &first)) return Status::Corrupt;
      if (packed_directories) {
        running += first;
        first = running;
      }
      if (first > out->entries.count() || (i > 0 && first < out->directories[i - 1])) return Status::Corrupt;
      if (!out->directories.push_back(static_cast<uint32_t>(first))) return Status::OutOfMemory;
    }
  }

  // Chunks and the table of each file's chunks (delta-encoded when packed).
  {
    FrozenArray chunks;
    if (!view.read_array(view.field(root, id::kChunks), &chunks) || chunks.count > 0xFFFFFFFFu) return Status::Corrupt;
    if (!out->chunks.reserve(chunks.count)) return Status::OutOfMemory;
    for (uint64_t i = 0; i < chunks.count; ++i) {
      const FrozenRef c = view.item(chunks, i);
      uint64_t block = 0, offset = 0, size = 0;
      if (!view.uint_field(c, 1, &block) || !view.uint_field(c, 2, &offset) || !view.uint_field(c, 3, &size)) return Status::Corrupt;
      if (block > 0xFFFFFFFFu || offset > 0xFFFFFFFFu || size > 0xFFFFFFFFu) return Status::Corrupt;
      const bool hole = out->has_holes && block == out->hole_block;
      // A data chunk lies inside its block (blocks are at most block_size bytes); a hole's fields encode its length instead.
      if (!hole && (block >= blocks || size > block_size || offset > block_size - size)) return Status::Corrupt;
      if (!out->chunks.push_back(Chunk{static_cast<uint32_t>(block), static_cast<uint32_t>(offset), static_cast<uint32_t>(size)})) return Status::OutOfMemory;
    }
  }
  if (!read_u32_list(view, view.field(root, id::kChunkTable), &out->chunk_table) || out->chunk_table.count() == 0) return Status::Corrupt;
  {
    uint64_t running = 0;
    for (uint64_t i = 0; i < out->chunk_table.count(); ++i) {
      uint64_t v = out->chunk_table[i];
      if (packed_chunks) v = running += v;
      if (v > out->chunks.count() || (i > 0 && v < out->chunk_table[i - 1])) return Status::Corrupt;
      out->chunk_table[i] = static_cast<uint32_t>(v);
    }
  }

  // Shared files: chunk_table indexes (less the unique files), run-length packed (count - 2 of each) when packed.
  if (!view.read_optional(view.field(root, id::kSharedFiles), &set, &value)) return Status::Corrupt;
  if (set) {
    List<uint32_t> table(allocator);
    if (!read_u32_list(view, value, &table)) return Status::Corrupt;
    if (packed_shared) {
      for (uint64_t i = 0; i < table.count(); ++i) {
        const uint64_t copies = uint64_t(table[i]) + 2;
        if (copies > out->file_count - out->shared_files.count()) return Status::Corrupt;
        for (uint64_t c = 0; c < copies; ++c)
          if (!out->shared_files.push_back(static_cast<uint32_t>(i))) return Status::OutOfMemory;
      }
    } else {
      for (uint32_t v : table.view())
        if (!out->shared_files.push_back(v)) return Status::OutOfMemory;
    }
  }
  if (out->shared_files.count() > out->file_count) return Status::Corrupt;
  out->unique_files = out->file_count - static_cast<uint32_t>(out->shared_files.count());
  if (uint64_t(out->unique_files) + 1 > out->chunk_table.count()) return Status::Corrupt;
  for (uint32_t shared : out->shared_files.view())
    if (uint64_t(shared) + out->unique_files + 1 >= out->chunk_table.count()) return Status::Corrupt;

  // Links.
  if (!read_u32_list(view, view.field(root, id::kSymlinkTable), &out->symlink_table) || out->symlink_table.count() < out->link_count) return Status::Corrupt;

  // Names and link targets: compact tables when present, plain lists otherwise.
  if (!view.read_optional(view.field(root, id::kCompactNames), &set, &value)) return Status::Corrupt;
  if (!(set ? out->names.load_compact(view, value) : out->names.load_plain(view, view.field(root, id::kNames)))) return Status::Corrupt;
  if (!view.read_optional(view.field(root, id::kCompactSymlinks), &set, &value)) return Status::Corrupt;
  if (!(set ? out->links.load_compact(view, value) : out->links.load_plain(view, view.field(root, id::kSymlinks)))) return Status::Corrupt;
  for (uint32_t i = 0; i < out->link_count; ++i)
    if (out->symlink_table[i] >= out->links.count()) return Status::Corrupt;

  // Every entry names an inode, and (but the root) has a name.
  const uint64_t inode_count = out->inodes.count();
  for (uint64_t i = 0; i < out->entries.count(); ++i) {
    const DirEntry& e = out->entries[i];
    if (e.inode >= inode_count || (i > 0 && e.name >= out->names.count())) return Status::Corrupt;
  }
  if (out->entries[0].inode != 0) return Status::Corrupt;

  // The directories form a tree. Their entry ranges are disjoint (first entries never decrease), so it is enough that the root is in no
  // directory and that no directory is named by more than one entry: then no walk from the root comes back to where it was.
  if (out->directories[0] == 0) return Status::Corrupt;
  {
    List<uint8_t> named(allocator);
    if (!named.reserve(out->directory_count)) return Status::OutOfMemory;
    for (uint32_t i = 0; i < out->directory_count; ++i)
      if (!named.push_back(0)) return Status::OutOfMemory;
    for (uint64_t i = 1; i < out->entries.count(); ++i) {
      const uint32_t inode = out->entries[i].inode;
      if (inode >= out->directory_count) continue;
      if (inode == 0 || named[inode]) return Status::Corrupt;
      named[inode] = 1;
    }
  }

  // Link counts, when the image does not keep them: how many entries name each inode.
  if (!have_links) {
    for (uint64_t i = 0; i < inode_count; ++i) out->inodes[i].links = 0;
    for (const DirEntry& e : out->entries.view()) ++out->inodes[e.inode].links;
  }
  return Status::Ok;
}

}  // namespace filesystem::dwarfs
