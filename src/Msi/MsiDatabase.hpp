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
#include "Media/CompoundFile.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

// The database of a Windows Installer package (.msi): a compound file (Media/CompoundFile.hpp) whose streams are the tables of a small relational
// database, plus the streams the tables refer to (the cabinets of the files, the programs of custom actions, icons):
//
//   - stream names are packed (two characters of a 64-character alphabet per UTF-16 unit); a table's stream has a mark in front;
//   - every text of every table is in one pool ("_StringPool" has the lengths, "_StringData" the bytes, in the package's code page), and a cell
//     that holds text holds the number of its string;
//   - "_Tables" names the tables and "_Columns" their columns; a table's stream holds its rows column by column, integers offset so that zero
//     means "no value".
//
// This reads all of that into memory (the tables are small; the cabinets stay in the file) and answers by table, row and column name. Text comes out
// as UTF-8 (from code page 1252, the usual one, or UTF-8; of another code page only the ASCII characters are kept). The package is untrusted: sizes
// are limited and every cell, string number and column is checked.
namespace msi {

inline constexpr int32_t kNull = INT32_MIN;      // a cell with no value, as an integer
inline constexpr uint32_t kNoColumn = ~uint32_t{0};

class Database;

class Table {
 public:
  Table(allocators::IAllocator& allocator, const Database& database) : name_(allocator), columns_(allocator), data_(allocator), database_(database) {}
  StringView name() const { return name_.view(); }
  uint32_t rows() const { return rows_; }
  uint32_t columns() const { return static_cast<uint32_t>(columns_.count()); }
  // The number of the column called `name`, or kNoColumn.
  uint32_t column(StringView name) const;
  // The cell as a number (kNull if it has none, is text, or does not exist) and as text ("" if it has none, is a number, or does not exist).
  int32_t integer(uint32_t row, uint32_t column) const;
  StringView text(uint32_t row, uint32_t column) const;
  int32_t integer(uint32_t row, StringView column_name) const { return integer(row, column(column_name)); }
  StringView text(uint32_t row, StringView column_name) const { return text(row, column(column_name)); }
  // The first row whose column `column_name` holds exactly `value` (text), or kNoColumn.
  uint32_t find(StringView column_name, StringView value) const;

 private:
  friend class Database;
  struct Column {
    explicit Column(allocators::IAllocator& allocator) : name(allocator) {}
    Column(Column&&) = default;
    Column& operator=(Column&&) = default;
    String name;
    uint32_t number = 0;      // 1, 2, ...: its place
    uint32_t offset = 0;      // bytes of the columns before it, in one row
    uint32_t width = 0;       // bytes of a cell
    bool text = false;
  };
  bool raw(uint32_t row, uint32_t column, uint32_t* value, bool* text) const;

  String name_;
  List<Column> columns_;
  ByteBuffer data_;
  uint32_t rows_ = 0;
  const Database& database_;
};

class Database {
 public:
  static constexpr uint64_t kMaxPoolBytes = 256ull << 20, kMaxTableBytes = 256ull << 20;
  static constexpr uint32_t kMaxTables = 4096, kMaxColumns = 255;

  // `file` holds the package and must outlive this.
  Database(allocators::IAllocator& allocator, media::IZipSource& file)
      : allocator_(allocator), file_(allocator, file), names_(allocator), pool_(allocator), starts_(allocator), tables_(allocator), platform_(allocator), package_code_(allocator) {}

  // Reads the string pool, the tables and the summary. False if this is not a package this reader can read, or memory ran out.
  [[nodiscard]] bool open();

  // The table of that name, or nullptr (a table the package does not have: most are optional).
  const Table* table(StringView name) const;
  // String number `id` of the pool ("" for 0 and for a number the pool does not have).
  StringView string(uint32_t id) const;

  // A stream that is not a table, by its name as the tables give it: "Binary.Name" (the Binary table's data of row Name), "Icon.Name", or the name
  // of a cabinet without its '#'. False if there is none (or its chain is damaged).
  [[nodiscard]] bool open_stream(StringView name, media::CompoundStream* stream);
  [[nodiscard]] bool read_stream(StringView name, uint64_t max_size, ByteBuffer* out);

  // From the summary information: the platform the package is for ("x64", "Intel", "Arm64", ...; "" if it does not say), its package code, and
  // whether its files are packed in cabinets (else they lie beside it, which a download does not have).
  StringView platform() const { return platform_.view(); }
  StringView package_code() const { return package_code_.view(); }
  bool compressed() const { return (word_count_ & 2) != 0; }

 private:
  struct StreamName {
    explicit StreamName(allocators::IAllocator& allocator) : name(allocator) {}
    StreamName(StreamName&&) = default;
    StreamName& operator=(StreamName&&) = default;
    String name;      // unpacked, UTF-8
    bool table = false;
    uint32_t entry = 0;      // of the compound file
  };
  const media::CompoundEntry* stream_entry(StringView name, bool table) const;
  bool read_named(StringView name, bool table, uint64_t max_size, ByteBuffer* out);
  bool load_pool();
  bool load_tables();
  bool load_summary();

  allocators::IAllocator& allocator_;
  media::CompoundFile file_;
  List<StreamName> names_;
  String pool_;                    // every string, as UTF-8, one after the other
  List<uint32_t> starts_;          // where string n starts in pool_ (one more than there are strings: the end)
  uint32_t string_bytes_ = 2;      // of a cell that holds a string number
  List<UniquePointer<Table>> tables_;
  String platform_, package_code_;
  uint32_t word_count_ = 2;
};

}  // namespace msi
