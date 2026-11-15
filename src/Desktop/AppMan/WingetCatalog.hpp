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
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

// The catalog of winget, the Windows Package Manager (https://github.com/microsoft/winget-pkgs, the "winget" source): what the App Manager lists as
// applications for Windows.
//
// The source is published as https://cdn.winget.microsoft.com/cache/source2.msix: a zip archive (Media/Zip.hpp) holding Public/index.db, an SQLite
// database (SqliteReader.hpp) whose `packages` table has one row per package: its identifier ("Mozilla.Firefox"), name, moniker (the short name to type),
// latest version, and the SHA-256 of the package's version list. From there (WingetInstaller.hpp):
//
//   <cache>/packages/<identifier>/<first 8 hex digits of that hash>/versionData.mszyml    the versions, each with the path and hash of its manifest
//   <cache>/<that path>                                                                   the manifest (YAML): what to download and how to install it
//
// The App Manager keeps the list as text (one line per package: identifier, name, moniker, version and hash, separated by tabs), so that it is
// there at once next time without unpacking a database.
namespace desktop::appman {

inline constexpr StringView kWingetCache = "https://cdn.winget.microsoft.com/cache/";
inline constexpr StringView kWingetIndexUrl = "https://cdn.winget.microsoft.com/cache/source2.msix";
inline constexpr StringView kWingetIndexMember = "Public/index.db";
inline constexpr uint64_t kMaxWingetIndexBytes = 64ull << 20;       // the .msix (4 MB in 2026)
inline constexpr uint64_t kMaxWingetDatabaseBytes = 256ull << 20;   // the database in it (9 MB)
inline constexpr uint64_t kMaxWingetListBytes = 64ull << 20;        // the kept list
inline constexpr uint64_t kMaxWingetPackages = 1000000;

// The identifiers this package manager will turn into paths and addresses: 1 to 128 bytes, no control characters, none of \ / : * ? " < > |, not
// starting with a dot or a space and not ending in one. (winget's are "Publisher.Package"; a few have '&', '!', ',' or letters beyond ASCII.)
bool valid_package_id(StringView id);

struct SqliteValue;

struct WingetPackage {
  explicit WingetPackage(allocators::IAllocator& allocator) : id(allocator), name(allocator), moniker(allocator), version(allocator) {}
  [[nodiscard]] bool copy_from(const WingetPackage& other) {
    for (uint32_t i = 0; i < 32; ++i) hash[i] = other.hash[i];
    return id.assign(other.id.view()) && name.assign(other.name.view()) && moniker.assign(other.moniker.view()) && version.assign(other.version.view());
  }
  String id;           // "jqlang.jq"
  String name;         // "jq"
  String moniker;      // "jq" (may be empty)
  String version;      // the latest: "1.8.2"
  uint8_t hash[32] = {};      // SHA-256 of the package's versionData.mszyml
};

class WingetCatalog {
 public:
  explicit WingetCatalog(allocators::IAllocator& allocator) : allocator_(allocator), entries_(allocator) {}

  // Reads the packages out of the source's .msix. False if it is not one (not a zip, no index in it, not a database with a packages table), there is
  // not a single package, or memory ran out.
  [[nodiscard]] bool parse_index(ConstByteView msix);
  // The same from a database already taken out of the .msix.
  [[nodiscard]] bool parse_database(ConstByteView database);
  // The kept list: written by write_list(), read by parse_list() (lines that are not packages are skipped; false if there is none).
  [[nodiscard]] bool write_list(String* out) const;
  [[nodiscard]] bool parse_list(StringView text);

  uint64_t count() const { return entries_.count(); }
  [[nodiscard]] bool take(WingetCatalog& other);
  const WingetPackage& at(uint64_t index) const { return *entries_.view()[index]; }
  // The package with that identifier (any case, as winget has it), or nullptr.
  const WingetPackage* find(StringView id) const;
  // The packages whose name, identifier or moniker contains every word of `query` (any case), best first: those whose name starts with the query or
  // whose moniker is the query (tier 0), those whose name contains it (1), then the rest (2); by name within a tier. At most `limit` indices, with
  // the tier of each in `tiers` if that is not null; *total gets how many matched in all.
  [[nodiscard]] bool search(StringView query, uint64_t limit, List<uint64_t>* indices, uint64_t* total, List<uint8_t>* tiers = nullptr) const;

 private:
  [[nodiscard]] bool add(StringView id, StringView name, StringView moniker, StringView version, ConstByteView hash);
  void sort();
  static bool add_row(void* context, int64_t rowid, ArrayView<const SqliteValue> columns);

  allocators::IAllocator& allocator_;
  List<UniquePointer<WingetPackage>> entries_;      // by name (any case), then identifier
};

// Compares without regard to ASCII case: <0, 0 or >0.
int compare_folded(StringView a, StringView b);
// Does `text` contain `word`, without regard to ASCII case? *at (if not null) gets where.
bool contains_folded(StringView text, StringView word, uint64_t* at = nullptr);

}  // namespace desktop::appman
