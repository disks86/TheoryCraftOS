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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"

// Reads the rows of a table out of an SQLite 3 database file held in memory (https://www.sqlite.org/fileformat2.html) - what winget's package index
// is (WingetCatalog.hpp). Read only, and only what that takes: ordinary (rowid) tables, found by name in the schema table and walked leaf by leaf, with
// payloads that spill into overflow pages. There is no SQL, no index, no WITHOUT ROWID table and no write-ahead log. The file is untrusted: every page
// number, cell offset and length is checked, and a file whose pages point at each other in a circle ends the walk instead of looping.
namespace desktop::appman {

struct SqliteValue {
  enum class Kind : uint8_t { Null, Integer, Float, Text, Blob };
  Kind kind = Kind::Null;
  int64_t integer = 0;        // Integer
  ConstByteView bytes;        // Text (UTF-8) and Blob: valid until the row's callback returns
  StringView text() const { return kind == Kind::Text ? StringView(bytes) : StringView(); }
};

class SqliteReader {
 public:
  static constexpr uint32_t kMaxColumns = 32;
  static constexpr uint64_t kMaxPayloadBytes = 16ull << 20;

  SqliteReader(allocators::IAllocator& allocator, ConstByteView file) : file_(file), scratch_(allocator) {}

  // Checks the header (the format, the page size, UTF-8 text). False if this is not a database this reader can read.
  [[nodiscard]] bool open();
  // The root page of the table called `name`. False if the schema has no such table.
  [[nodiscard]] bool table_root(StringView name, uint32_t* root_page);
  // Called for every row, in rowid order, with its columns (at most kMaxColumns; a column added to the table after the row was written is absent).
  // Returning false stops the walk.
  using RowSink = bool (*)(void* context, int64_t rowid, ArrayView<const SqliteValue> columns);
  // Walks the table whose root page is `root_page`. False if the file is damaged or the sink stopped the walk.
  [[nodiscard]] bool scan_table(uint32_t root_page, RowSink sink, void* context);

 private:
  bool page(uint32_t number, ConstByteView* out) const;
  bool walk(uint32_t page_number, uint32_t depth, RowSink sink, void* context);
  bool row(ConstByteView page_bytes, uint64_t cell_at, RowSink sink, void* context);

  ConstByteView file_;
  ByteBuffer scratch_;          // a payload put together from its overflow pages
  uint32_t page_size_ = 0;
  uint32_t usable_ = 0;         // of each page (the page size less the reserved bytes at its end)
  uint32_t page_count_ = 0;
  uint64_t pages_walked_ = 0;   // (a guard: a walk never visits more pages than the file has)
};

}  // namespace desktop::appman
