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

#include "Database/TextResultSet.hpp"
#include "Database/NumberText.hpp"
#include "Utility/Move.hpp"

namespace database {

namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool same_ignoring_case(StringView a, StringView b) {
  if (a.size() != b.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

bool TextResultSet::add_column(StringView name, DbType type) {
  if (cells_.count()) return false;   // the columns come first
  Column column{String(allocator_), type};
  return column.name.assign(name) && columns_.push_back(util::move(column));
}

bool TextResultSet::begin_row() {
  if (columns_.count() == 0) {
    ++rows_without_columns_;
    return true;
  }
  return complete();   // the row before it must have all its cells
}

bool TextResultSet::add_cell(StringView text) {
  if (columns_.count() == 0 || text.size() > 0xFFFFFFFFu) return false;
  const Cell c{data_.size(), static_cast<uint32_t>(text.size()), false};
  return data_.append(text.bytes()) && cells_.push_back(c);
}

bool TextResultSet::add_null() {
  if (columns_.count() == 0) return false;
  return cells_.push_back(Cell{data_.size(), 0, true});
}

int32_t TextResultSet::find_column(StringView name) const {
  for (uint64_t i = 0; i < columns_.count(); ++i)
    if (same_ignoring_case(columns_[i].name.view(), name)) return static_cast<int32_t>(i);
  return -1;
}

bool TextResultSet::is_null(uint64_t row, uint32_t column) const {
  const Cell* c = cell(row, column);
  return !c || c->null;
}

StringView TextResultSet::text(uint64_t row, uint32_t column) const {
  const Cell* c = cell(row, column);
  return c && !c->null ? view_of(*c) : StringView();
}

bool TextResultSet::get_bool(uint64_t row, uint32_t column, bool* out) const {
  const Cell* c = cell(row, column);
  if (!c || c->null) return false;
  const StringView t = view_of(*c);
  static const char* const kTrue[] = {"t", "true", "1", "y", "yes", "on"};
  static const char* const kFalse[] = {"f", "false", "0", "n", "no", "off"};
  for (const char* word : kTrue)
    if (same_ignoring_case(t, word)) {
      *out = true;
      return true;
    }
  for (const char* word : kFalse)
    if (same_ignoring_case(t, word)) {
      *out = false;
      return true;
    }
  return false;
}

bool TextResultSet::get_int(uint64_t row, uint32_t column, int64_t* out) const {
  const Cell* c = cell(row, column);
  if (!c || c->null) return false;
  const StringView t = view_of(*c);
  if (parse_int64(t, out)) return true;
  // A boolean column reads as 0 and 1, the way most databases let programs treat it.
  bool b;
  if (columns_[column].type == DbType::Boolean && get_bool(row, column, &b)) {
    *out = b ? 1 : 0;
    return true;
  }
  return false;
}

bool TextResultSet::get_real(uint64_t row, uint32_t column, double* out) const {
  const Cell* c = cell(row, column);
  if (!c || c->null) return false;
  return parse_double(view_of(*c), out);
}

bool TextResultSet::get_bytes(uint64_t row, uint32_t column, ByteBuffer* out) const {
  const Cell* c = cell(row, column);
  if (!c || c->null) return false;
  const StringView t = view_of(*c);
  out->clear();
  if (columns_[column].type == DbType::Bytes && t.size() >= 2 && t[0] == '\\' && t[1] == 'x') {   // the hex form: \x48656c6c6f
    if (t.size() % 2 != 0) return false;
    for (uint64_t i = 2; i < t.size(); i += 2) {
      const int hi = hex_value(t[i]), lo = hex_value(t[i + 1]);
      if (hi < 0 || lo < 0 || !out->put8(static_cast<uint8_t>(hi * 16 + lo))) return false;
    }
    return true;
  }
  return out->append(t.bytes());
}

}  // namespace database
