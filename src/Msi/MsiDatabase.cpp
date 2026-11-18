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

#include "Msi/MsiDatabase.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace msi {

namespace {

constexpr uint16_t kTypeText = 0x0800, kTypeNullable = 0x1000, kTypeValid = 0x0100;
constexpr uint32_t kCodePageUtf8 = 65001, kCodePage1252 = 1252;
constexpr uint64_t kMaxSummaryBytes = 1u << 20, kMaxSchemaBytes = 16u << 20;

bool append_code_point(String* out, uint32_t cp) {
  if (cp < 0x80) return out->push_back(static_cast<char>(cp));
  if (cp < 0x800) return out->push_back(static_cast<char>(0xC0 | (cp >> 6))) && out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  return out->push_back(static_cast<char>(0xE0 | (cp >> 12))) && out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))) && out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}

// One of the 64 characters a packed stream name is made of.
uint32_t name_character(uint32_t x) {
  if (x < 10) return '0' + x;
  if (x < 36) return 'A' + x - 10;
  if (x < 62) return 'a' + x - 36;
  return x == 62 ? '.' : '_';
}

// A stream's name as the tables spell it: units 0x3800..0x47FF hold two characters, 0x4800..0x483F one, 0x4840 in front marks a table.
bool unpack_name(ArrayView<const uint16_t> units, String* out, bool* table) {
  out->clear();
  *table = !units.empty() && units[0] == 0x4840;
  for (const uint16_t unit : *table ? units.subview(1) : units) {
    if (unit >= 0x3800 && unit < 0x4800) {
      const uint32_t packed = unit - 0x3800;
      if (!append_code_point(out, name_character(packed & 0x3F)) || !append_code_point(out, name_character(packed >> 6))) return false;
    } else if (unit >= 0x4800 && unit < 0x4840) {
      if (!append_code_point(out, name_character(unit - 0x4800))) return false;
    } else if (!append_code_point(out, unit)) {
      return false;
    }
  }
  return true;
}

// Code page 1252's characters 0x80..0x9F (0: none).
constexpr uint16_t k1252[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
                                0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

bool append_decoded(String* out, ConstByteView bytes, uint32_t code_page) {
  if (code_page == kCodePageUtf8) return out->append(StringView(bytes));
  for (const uint8_t byte : bytes) {
    uint32_t cp = byte;
    if (byte >= 0x80) {
      if (code_page != kCodePage1252 && code_page != 0) cp = 0xFFFD;
      else if (byte < 0xA0) cp = k1252[byte - 0x80] != 0 ? k1252[byte - 0x80] : 0xFFFD;
    }
    if (!append_code_point(out, cp)) return false;
  }
  return true;
}

uint32_t cell_at(ConstByteView data, uint64_t offset, uint32_t width) {
  ConstByteView bytes;
  if (!data.try_subview(offset, width, &bytes)) return 0;
  uint32_t value = 0;
  for (uint32_t i = 0; i < width; ++i) value |= uint32_t{bytes[i]} << (8 * i);
  return value;
}

}  // namespace

uint32_t Table::column(StringView name) const {
  for (uint32_t i = 0; i < columns_.count(); ++i)
    if (columns_[i].name.view().equals(name)) return i;
  return kNoColumn;
}

// The cell as stored. Rows are kept column by column: all of the first column, then all of the second, ...
bool Table::raw(uint32_t row, uint32_t column, uint32_t* value, bool* text) const {
  const Column* c = columns_.get(column);
  if (!c || row >= rows_) return false;
  *value = cell_at(data_.view(), uint64_t{c->offset} * rows_ + uint64_t{row} * c->width, c->width);
  *text = c->text;
  return true;
}

int32_t Table::integer(uint32_t row, uint32_t column) const {
  uint32_t value = 0;
  bool text = false;
  if (!raw(row, column, &value, &text) || text || value == 0) return kNull;
  const Column& c = columns_[column];
  return c.width == 4 ? static_cast<int32_t>(value ^ 0x80000000u) : static_cast<int32_t>(value) - 0x8000;
}

StringView Table::text(uint32_t row, uint32_t column) const {
  uint32_t value = 0;
  bool text = false;
  if (!raw(row, column, &value, &text) || !text) return StringView();
  return database_.string(value);
}

uint32_t Table::find(StringView column_name, StringView value) const {
  const uint32_t c = column(column_name);
  if (c == kNoColumn) return kNoColumn;
  for (uint32_t row = 0; row < rows_; ++row)
    if (text(row, c).equals(value)) return row;
  return kNoColumn;
}

const Table* Database::table(StringView name) const {
  for (const UniquePointer<Table>& t : tables_.view())
    if (t->name().equals(name)) return t.get();
  return nullptr;
}

StringView Database::string(uint32_t id) const {
  if (id == 0 || uint64_t{id} + 1 >= starts_.count()) return StringView();
  const uint32_t start = starts_[id], end = starts_[id + 1];
  StringView text;
  return end >= start && pool_.view().try_substr(start, end - start, &text) ? text : StringView();
}

const media::CompoundEntry* Database::stream_entry(StringView name, bool table) const {
  for (const StreamName& known : names_.view()) {
    if (known.table != table || !known.name.view().equals(name)) continue;
    ArrayView<const media::CompoundEntry> entries = file_.entries();
    return known.entry < entries.size() ? &entries[known.entry] : nullptr;
  }
  return nullptr;
}

bool Database::read_named(StringView name, bool table, uint64_t max_size, ByteBuffer* out) {
  const media::CompoundEntry* entry = stream_entry(name, table);
  return entry && file_.read_stream(*entry, max_size, out);
}

bool Database::open_stream(StringView name, media::CompoundStream* stream) {
  const media::CompoundEntry* entry = stream_entry(name, false);
  return entry && file_.open_stream(*entry, stream);
}

bool Database::read_stream(StringView name, uint64_t max_size, ByteBuffer* out) { return read_named(name, false, max_size, out); }

bool Database::open() {
  names_.clear();
  tables_.clear();
  if (!file_.open()) return false;
  uint32_t index = 0;
  for (const media::CompoundEntry& entry : file_.entries()) {
    StreamName name(allocator_);
    name.entry = index++;
    if (!unpack_name(entry.units(), &name.name, &name.table) || !names_.push_back(util::move(name))) return false;
  }
  return load_pool() && load_tables() && load_summary();
}

// "_StringPool" is a list of pairs of 16-bit numbers: first the code page (its top bit set: string numbers take three bytes in the tables), then a length
// and a count of uses for each string, whose bytes follow one another in "_StringData".
bool Database::load_pool() {
  ByteBuffer pool(allocator_), data(allocator_);
  if (!read_named("_StringPool", true, kMaxPoolBytes, &pool) || !read_named("_StringData", true, kMaxPoolBytes, &data) || pool.size() < 4) return false;
  ByteReader in(pool.view());
  const uint32_t low = in.le16(), high = in.le16();
  const uint32_t code_page = low | ((high & 0x7FFF) << 16);
  string_bytes_ = (high & 0x8000) != 0 ? 3 : 2;
  pool_.clear();
  starts_.clear();
  if (!starts_.push_back(0)) return false;      // (string 0 is "no string")
  uint64_t offset = 0;
  while (in.remaining() >= 4) {
    uint64_t length = in.le16();
    const uint32_t uses = in.le16();
    if (!starts_.push_back(static_cast<uint32_t>(pool_.size()))) return false;
    if (length == 0 && uses == 0) continue;      // (an unused number)
    if (length == 0) {      // a string of 64 KiB or more: its length is in the next pair
      if (in.remaining() < 4) return false;
      const uint32_t length_low = in.le16(), length_high = in.le16();
      length = (uint64_t{length_high} << 16) + length_low;
    }
    ConstByteView bytes;
    if (!data.view().try_subview(offset, length, &bytes)) return false;
    offset += length;
    if (!append_decoded(&pool_, bytes, code_page) || pool_.size() > kMaxPoolBytes) return false;
  }
  return starts_.push_back(static_cast<uint32_t>(pool_.size()));
}

// "_Tables" lists the tables (a string number each); "_Columns" has a row for every column of every table: table, number, name, type.
bool Database::load_tables() {
  ByteBuffer names(allocator_), columns(allocator_);
  if (!read_named("_Tables", true, kMaxSchemaBytes, &names) || !read_named("_Columns", true, kMaxSchemaBytes, &columns)) return false;
  const uint32_t sb = string_bytes_;
  const uint64_t table_count = names.size() / sb;
  if (table_count > kMaxTables) return false;
  // Which table a string number names (as its place in tables_ plus one; 0: none).
  List<uint32_t> owner(allocator_);
  if (!owner.reserve(starts_.count())) return false;
  for (uint64_t i = 0; i < starts_.count(); ++i)
    if (!owner.push_back(0)) return false;
  for (uint64_t i = 0; i < table_count; ++i) {
    const uint32_t id = cell_at(names.view(), i * sb, sb);
    UniquePointer<Table> table = UniquePointer<Table>::make(allocator_, allocator_, *this);
    if (!table || !table->name_.assign(string(id))) return false;
    if (id < owner.count() && owner[id] == 0) owner[id] = static_cast<uint32_t>(tables_.count() + 1);
    if (!tables_.push_back(util::move(table))) return false;
  }
  const uint64_t row_bytes = uint64_t{sb} * 2 + 4, rows = columns.size() / row_bytes;
  for (uint64_t row = 0; row < rows; ++row) {
    const uint32_t table_id = cell_at(columns.view(), row * sb, sb);
    const uint32_t number = cell_at(columns.view(), rows * sb + row * 2, 2) ^ 0x8000;
    const uint32_t name_id = cell_at(columns.view(), rows * (sb + 2) + row * sb, sb);
    const uint32_t type = cell_at(columns.view(), rows * (uint64_t{sb} * 2 + 2) + row * 2, 2) ^ 0x8000;
    if (table_id >= owner.count() || owner[table_id] == 0 || number == 0 || number > kMaxColumns) continue;
    Table& table = *tables_[owner[table_id] - 1];
    Table::Column column(allocator_);
    if (!column.name.assign(string(name_id))) return false;
    column.number = number;
    const bool binary = (type & ~uint32_t{kTypeNullable}) == (kTypeText | kTypeValid);      // (a stream: the cell only says whether there is one)
    column.text = (type & kTypeText) != 0 && !binary;
    column.width = column.text ? sb : (binary || (type & 0xFF) <= 2) ? 2 : 4;
    // In the order of their numbers.
    uint64_t at = table.columns_.count();
    while (at > 0 && table.columns_[at - 1].number > number) --at;
    if (table.columns_.count() >= kMaxColumns || !table.columns_.insert(at, util::move(column))) return false;
  }
  uint64_t total = 0;
  for (const UniquePointer<Table>& table : tables_.view()) {
    uint32_t row_width = 0;
    for (Table::Column& column : table->columns_.view()) {
      column.offset = row_width;
      row_width += column.width;
    }
    if (row_width == 0 || !stream_entry(table->name(), true)) continue;      // (a table without a stream has no rows)
    if (!read_named(table->name(), true, kMaxTableBytes - total, &table->data_)) return false;
    total += table->data_.size();
    table->rows_ = static_cast<uint32_t>(table->data_.size() / row_width);
  }
  return true;
}

// The summary information is a property set ([MS-OLEPS]): a header, one section, in it a list of (property, offset) and the values, each with its type.
bool Database::load_summary() {
  platform_.clear();
  package_code_.clear();
  word_count_ = 2;
  ByteBuffer raw(allocator_);
  if (!read_named("\x05SummaryInformation", false, kMaxSummaryBytes, &raw)) return true;      // (a package without one is taken as packed, for any processor)
  ByteReader in(raw.view());
  in.skip(2 + 2 + 4 + 16);
  const uint32_t sections = in.le32();
  in.skip(16);
  const uint32_t section = in.le32();
  if (!in.ok() || sections == 0 || !in.seek(section)) return true;
  in.skip(4);
  const uint32_t properties = in.le32();
  for (uint32_t i = 0; i < properties && i < 256 && in.ok(); ++i) {
    const uint32_t id = in.le32(), offset = in.le32();
    ByteReader value(raw.view());
    if (offset > raw.size() - section || !value.seek(uint64_t{section} + offset)) continue;
    const uint32_t type = value.le32();
    if (type == 30 && (id == 7 || id == 9)) {      // text: the template ("x64;1033") and the revision (the package code)
      const uint32_t length = value.le32();
      StringView text(value.take(length));
      if (!value.ok()) continue;
      const uint64_t end = text.find('\0');
      if (end != StringView::npos) text = text.first(end);
      if (id == 7) {
        const uint64_t semicolon = text.find(';');
        if (!platform_.assign((semicolon == StringView::npos ? text : text.first(semicolon)).trimmed())) return false;
      } else if (!package_code_.assign(text.trimmed())) {
        return false;
      }
    } else if ((type == 3 || type == 2) && id == 15) {
      word_count_ = type == 3 ? value.le32() : value.le16();
      if (!value.ok()) word_count_ = 2;
    }
  }
  return true;
}

}  // namespace msi
