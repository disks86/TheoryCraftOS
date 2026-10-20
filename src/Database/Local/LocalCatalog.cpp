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

#include "Database/Local/LocalCatalog.hpp"
#include "Utility/Crc32.hpp"
#include "Utility/Move.hpp"

namespace database::local {

namespace {
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxName = 1024;
constexpr uint32_t kMaxColumns = 1024;

bool put_u64(ByteBuffer* out, uint64_t v) { return out->put32(static_cast<uint32_t>(v)) && out->put32(static_cast<uint32_t>(v >> 32)); }

bool put_string(ByteBuffer* out, StringView s) { return s.size() <= 0xFFFFFFFFu && out->put32(static_cast<uint32_t>(s.size())) && out->append(s.bytes()); }

bool put_value(ByteBuffer* out, const Value& v) {
  if (!out->put8(static_cast<uint8_t>(v.kind))) return false;
  switch (v.kind) {
    case Value::Kind::Null: return true;
    case Value::Kind::Integer: return put_u64(out, static_cast<uint64_t>(v.integer));
    case Value::Kind::Real: {
      uint64_t bits = 0;
      __builtin_memcpy(&bits, &v.real, sizeof bits);
      return put_u64(out, bits);
    }
    case Value::Kind::Text:
    case Value::Kind::Bytes: return put_string(out, v.text.view());
  }
  return false;
}

// Reads what put_* wrote; after the first thing that does not fit, every read fails.
class Reader {
 public:
  explicit Reader(ConstByteView data) : data_(data) {}
  bool ok() const { return ok_; }
  uint64_t remaining() const { return ok_ ? data_.size() - pos_ : 0; }
  uint8_t u8() {
    if (!need(1)) return 0;
    return data_[pos_++];
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    const uint32_t v = static_cast<uint32_t>(data_[pos_]) | static_cast<uint32_t>(data_[pos_ + 1]) << 8 | static_cast<uint32_t>(data_[pos_ + 2]) << 16 |
                       static_cast<uint32_t>(data_[pos_ + 3]) << 24;
    pos_ += 4;
    return v;
  }
  uint64_t u64() {
    const uint64_t lo = u32();
    const uint64_t hi = u32();
    return lo | hi << 32;
  }
  StringView bytes(uint64_t n) {
    if (!need(n)) return StringView();
    const StringView s(reinterpret_cast<const char*>(data_.data()) + pos_, n);
    pos_ += n;
    return s;
  }
  StringView string(uint32_t limit) {
    const uint32_t n = u32();
    if (n > limit) {
      ok_ = false;
      return StringView();
    }
    return bytes(n);
  }
  bool value(Value* out) {
    const uint8_t kind = u8();
    switch (kind) {
      case 0: out->set_null(); break;
      case 1: out->set_int(static_cast<int64_t>(u64())); break;
      case 2: {
        const uint64_t bits = u64();
        double d = 0;
        __builtin_memcpy(&d, &bits, sizeof d);
        out->set_real(d);
        break;
      }
      case 3:
      case 4: {
        const StringView s = string(0x7FFFFFFFu);
        if (!ok_) return false;
        if (kind == 3 ? !out->set_text(s) : !out->set_bytes(s)) ok_ = false;
        break;
      }
      default: ok_ = false;
    }
    return ok_;
  }

 private:
  bool need(uint64_t n) {
    if (!ok_ || n > data_.size() - pos_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  ConstByteView data_;
  uint64_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace

int32_t Table::find_column(StringView column_name) const {
  for (uint64_t i = 0; i < columns.count(); ++i)
    if (columns[i].name.view().equals_ignore_case(column_name)) return static_cast<int32_t>(i);
  return -1;
}

bool Table::has_key() const {
  for (uint64_t i = 0; i < columns.count(); ++i)
    if (columns[i].primary_key) return true;
  return false;
}

bool Table::add_row(ArrayView<Value> row) {
  if (row.size() != columns.count() || !cells.reserve(cells.count() + row.size())) return false;
  for (uint64_t i = 0; i < row.size(); ++i) cells.push_back(util::move(row[i]));
  return true;
}

void Table::erase_rows(const List<uint8_t>& flags) {
  const uint64_t n = columns.count();
  const uint64_t rows = row_count();
  uint64_t kept = 0;
  for (uint64_t r = 0; r < rows && r < flags.count(); ++r) {
    if (flags[r]) continue;
    if (kept != r)
      for (uint64_t c = 0; c < n; ++c) cells[kept * n + c] = util::move(cells[r * n + c]);
    ++kept;
  }
  while (cells.count() > kept * n) cells.remove_at(cells.count() - 1);
}

Table* Catalog::find(StringView name) const {
  for (uint64_t i = 0; i < tables_.count(); ++i)
    if (tables_[i]->name.view().equals_ignore_case(name)) return tables_[i].get();
  return nullptr;
}

bool Catalog::drop(StringView name) {
  for (uint64_t i = 0; i < tables_.count(); ++i)
    if (tables_[i]->name.view().equals_ignore_case(name)) return tables_.remove_at(i);
  return false;
}

bool Catalog::serialize(ByteBuffer* out) const {
  out->clear();
  if (!out->put_tag("TCDB") || !out->put32(kVersion) || !out->put32(static_cast<uint32_t>(tables_.count()))) return false;
  for (uint64_t t = 0; t < tables_.count(); ++t) {
    const Table& table = *tables_[t];
    if (!put_string(out, table.name.view()) || !out->put32(static_cast<uint32_t>(table.columns.count()))) return false;
    for (uint64_t c = 0; c < table.columns.count(); ++c) {
      const ColumnDef& column = table.columns[c];
      const uint8_t flags = static_cast<uint8_t>((column.not_null ? 1 : 0) | (column.primary_key ? 2 : 0) | (column.has_default ? 4 : 0));
      if (!put_string(out, column.name.view()) || !out->put8(static_cast<uint8_t>(column.type)) || !out->put8(flags)) return false;
      if (column.has_default && !put_value(out, column.default_value)) return false;
    }
    if (!put_u64(out, table.row_count())) return false;
    for (uint64_t i = 0; i < table.cells.count(); ++i)
      if (!put_value(out, table.cells[i])) return false;
  }
  return out->put32(crc32(out->view()));
}

DbStatus Catalog::load(ConstByteView data, DbError* error) {
  tables_.clear();
  auto corrupt = [&](const char* why) {
    tables_.clear();
    if (error) error->set(DbStatus::Protocol, why);
    return DbStatus::Protocol;
  };
  if (data.size() < 16) return corrupt("the database file is too short");
  if (crc32(data.first(data.size() - 4)) != (static_cast<uint32_t>(data[data.size() - 4]) | static_cast<uint32_t>(data[data.size() - 3]) << 8 |
                                              static_cast<uint32_t>(data[data.size() - 2]) << 16 | static_cast<uint32_t>(data[data.size() - 1]) << 24))
    return corrupt("the database file does not match its checksum");
  Reader in(data.first(data.size() - 4));
  const StringView magic = in.bytes(4);
  if (!in.ok() || !magic.equals("TCDB")) return corrupt("not a TheoryCraftOS database file");
  if (in.u32() != kVersion) return corrupt("the database file is from a newer version");
  const uint32_t table_count = in.u32();
  for (uint32_t t = 0; t < table_count; ++t) {
    const StringView name = in.string(kMaxName);
    const uint32_t column_count = in.u32();
    if (!in.ok() || column_count == 0 || column_count > kMaxColumns || name.empty() || find(name)) return corrupt("the database file is damaged");
    UniquePointer<Table> table = UniquePointer<Table>::make(allocator_, allocator_, name);
    if (!table) return corrupt("out of memory");
    for (uint32_t c = 0; c < column_count; ++c) {
      ColumnDef column(allocator_);
      column.name.assign(in.string(kMaxName));
      const uint8_t type = in.u8();
      const uint8_t flags = in.u8();
      if (!in.ok() || type > static_cast<uint8_t>(DbType::Uuid)) return corrupt("the database file is damaged");
      column.type = static_cast<DbType>(type);
      column.not_null = flags & 1;
      column.primary_key = flags & 2;
      column.has_default = flags & 4;
      if (column.has_default && !in.value(&column.default_value)) return corrupt("the database file is damaged");
      if (!table->columns.push_back(util::move(column))) return corrupt("out of memory");
    }
    const uint64_t rows = in.u64();
    if (!in.ok() || rows > in.remaining() / column_count) return corrupt("the database file is damaged");
    for (uint64_t i = 0; i < rows * column_count; ++i) {
      Value v(allocator_);
      if (!in.value(&v) || !table->cells.push_back(util::move(v))) return corrupt("the database file is damaged");
    }
    if (!add(util::move(table))) return corrupt("out of memory");
  }
  if (in.remaining() != 0) return corrupt("the database file is damaged");
  return DbStatus::Ok;
}

}  // namespace database::local
