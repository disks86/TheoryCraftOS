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

#include "Desktop/AppMan/SqliteReader.hpp"
#include "Utility/ByteStream.hpp"

namespace desktop::appman {

namespace {

constexpr uint8_t kMagic[16] = {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f', 'o', 'r', 'm', 'a', 't', ' ', '3', 0};
constexpr uint8_t kInteriorTable = 0x05, kLeafTable = 0x0D;
constexpr uint32_t kMaxDepth = 24;      // (a b-tree of 2^32 pages is far shallower)

// A variable-length integer: big-endian, seven bits a byte, the ninth byte whole.
uint64_t varint(ByteReader& in) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    const uint8_t byte = in.u8();
    value = (value << 7) | (byte & 0x7F);
    if (!(byte & 0x80)) return value;
  }
  return (value << 8) | in.u8();
}

// How many bytes of the body a column of this serial type takes.
uint64_t serial_size(uint64_t type) {
  static constexpr uint8_t kFixed[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  return type < 12 ? kFixed[type] : (type - 12) / 2;
}

struct SchemaSearch {
  StringView name;
  uint32_t root = 0;
  bool found = false;
};

// A row of the schema table: type, name, tbl_name, rootpage, sql.
bool schema_row(void* context, int64_t, ArrayView<const SqliteValue> columns) {
  SchemaSearch& search = *static_cast<SchemaSearch*>(context);
  if (columns.size() < 4 || !columns[0].text().equals("table") || !columns[1].text().equals(search.name)) return true;
  if (columns[3].kind != SqliteValue::Kind::Integer || columns[3].integer < 1 || columns[3].integer > 0xFFFFFFFFll) return true;
  search.root = static_cast<uint32_t>(columns[3].integer);
  search.found = true;
  return false;      // (found: stop)
}

}  // namespace

bool SqliteReader::open() {
  ByteReader in(file_);
  if (!in.take(16).equals(ConstByteView(kMagic))) return false;
  const uint32_t size = in.be16();
  in.skip(2);      // the file format versions
  const uint8_t reserved = in.u8();
  in.seek(56);
  const uint32_t encoding = in.be32();
  if (!in.ok()) return false;
  page_size_ = size == 1 ? 65536 : size;
  if (page_size_ < 512 || (page_size_ & (page_size_ - 1)) != 0) return false;
  if (reserved > page_size_ - 480) return false;      // (the format's own limit: at least 480 usable bytes)
  usable_ = page_size_ - reserved;
  if (encoding != 1) return false;                    // UTF-8 only
  const uint64_t pages = file_.size() / page_size_;
  if (pages == 0 || pages > 0xFFFFFFFFull) return false;
  page_count_ = static_cast<uint32_t>(pages);
  return true;
}

bool SqliteReader::page(uint32_t number, ConstByteView* out) const {
  if (number == 0 || number > page_count_) return false;
  return file_.try_subview(uint64_t{number - 1} * page_size_, page_size_, out);
}

bool SqliteReader::table_root(StringView name, uint32_t* root_page) {
  SchemaSearch search{name};
  pages_walked_ = 0;
  (void)walk(1, 0, &schema_row, &search);      // (false when found: the row stops the walk)
  if (search.found) *root_page = search.root;
  return search.found;
}

bool SqliteReader::scan_table(uint32_t root_page, RowSink sink, void* context) {
  pages_walked_ = 0;
  return walk(root_page, 0, sink, context);
}

bool SqliteReader::walk(uint32_t page_number, uint32_t depth, RowSink sink, void* context) {
  ConstByteView bytes;
  if (depth > kMaxDepth || ++pages_walked_ > page_count_ || !page(page_number, &bytes)) return false;
  ByteReader in(bytes);
  in.skip(page_number == 1 ? 100 : 0);      // (the first page starts with the file header)
  const uint8_t type = in.u8();
  in.skip(2);                               // the first free block
  const uint32_t cells = in.be16();
  in.skip(2 + 1);                           // where the cells start, the fragmented bytes
  const uint32_t right_most = type == kInteriorTable ? in.be32() : 0;
  if (!in.ok() || (type != kInteriorTable && type != kLeafTable)) return false;
  ByteReader pointers(in.take(uint64_t{cells} * 2));
  if (!in.ok()) return false;
  for (uint32_t i = 0; i < cells; ++i) {
    const uint64_t cell_at = pointers.be16();
    if (type == kLeafTable) {
      if (!row(bytes, cell_at, sink, context)) return false;
    } else {
      ByteReader cell(bytes);
      cell.seek(cell_at);
      const uint32_t child = cell.be32();
      if (!cell.ok() || !walk(child, depth + 1, sink, context)) return false;
    }
  }
  return type == kLeafTable || walk(right_most, depth + 1, sink, context);
}

bool SqliteReader::row(ConstByteView page_bytes, uint64_t cell_at, RowSink sink, void* context) {
  ByteReader cell(page_bytes.first(usable_));
  cell.seek(cell_at);
  const uint64_t payload_size = varint(cell);
  const int64_t rowid = static_cast<int64_t>(varint(cell));
  if (!cell.ok() || payload_size > kMaxPayloadBytes) return false;

  // The payload: all of it in the cell, or its start there and the rest in a chain of overflow pages.
  ConstByteView payload;
  const uint64_t max_local = usable_ - 35;
  if (payload_size <= max_local) {
    payload = cell.take(payload_size);
    if (!cell.ok()) return false;
  } else {
    const uint64_t min_local = (uint64_t{usable_} - 12) * 32 / 255 - 23;
    const uint64_t spill = min_local + (payload_size - min_local) % (usable_ - 4);
    const uint64_t local = spill <= max_local ? spill : min_local;
    const ConstByteView start = cell.take(local);
    uint32_t next = cell.be32();
    scratch_.clear();
    if (!cell.ok() || !scratch_.append(start)) return false;
    while (scratch_.size() < payload_size) {
      ConstByteView overflow;
      if (++pages_walked_ > uint64_t{page_count_} * 2 || !page(next, &overflow)) return false;
      ByteReader in(overflow.first(usable_));
      next = in.be32();
      const uint64_t left = payload_size - scratch_.size();
      if (!scratch_.append(in.take(left < in.remaining() ? left : in.remaining()))) return false;
    }
    payload = scratch_.view();
  }

  // The record: a header of serial types, then the values in the same order.
  ByteReader header(payload);
  const uint64_t header_size = varint(header);
  if (!header.ok() || header_size > payload.size() || header_size < header.position()) return false;
  ByteReader types(payload.first(header_size));
  types.skip(header.position());
  ByteReader body(payload.subview(header_size));
  SqliteValue values[kMaxColumns];
  uint32_t count = 0;
  while (!types.at_end() && count < kMaxColumns) {
    const uint64_t type = varint(types);
    if (!types.ok()) return false;
    SqliteValue& value = values[count++];
    const ConstByteView bytes = body.take(serial_size(type));
    if (!body.ok()) return false;
    if (type == 0) {
      value.kind = SqliteValue::Kind::Null;
    } else if (type <= 6) {
      int64_t n = (bytes[0] & 0x80) ? -1 : 0;      // (sign extended)
      for (const uint8_t byte : bytes) n = static_cast<int64_t>((static_cast<uint64_t>(n) << 8) | byte);
      value.kind = SqliteValue::Kind::Integer;
      value.integer = n;
    } else if (type == 7) {
      value.kind = SqliteValue::Kind::Float;
    } else if (type == 8 || type == 9) {
      value.kind = SqliteValue::Kind::Integer;
      value.integer = type == 9;
    } else if (type < 12) {
      return false;      // (reserved)
    } else {
      value.kind = (type & 1) ? SqliteValue::Kind::Text : SqliteValue::Kind::Blob;
      value.bytes = bytes;
    }
  }
  return sink(context, rowid, ArrayView<const SqliteValue>(values, count));
}

}  // namespace desktop::appman
