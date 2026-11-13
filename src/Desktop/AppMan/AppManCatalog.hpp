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
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

// The catalog of AppMan (https://github.com/ivan-hc/AM, the "am" / "appman" package manager for AppImages): one line per application,
//   ◆ obsidian : A powerful knowledge base, plain text Markdown files. #itsdesktopapp
// in programs/stats-appimages of the repository (programs/x86_64-appimages is the same without the tags), and for each application an install script,
// programs/x86_64/<name>, that finds the current download and puts it in /opt/<name> (AppManInstaller.hpp).
namespace desktop::appman {

inline constexpr StringView kRepository = "https://raw.githubusercontent.com/ivan-hc/AM/main";
inline constexpr StringView kCatalogUrl = "https://raw.githubusercontent.com/ivan-hc/AM/main/programs/stats-appimages";
inline constexpr StringView kScriptsUrl = "https://raw.githubusercontent.com/ivan-hc/AM/main/programs/x86_64/";
inline constexpr uint64_t kMaxCatalogBytes = 16ull << 20;

// The names AppMan gives applications (and the only ones this package manager will turn into paths): 1 to 64 of a-z A-Z 0-9 . _ + -, not starting
// with a dot or a dash.
bool valid_app_name(StringView name);

struct CatalogEntry {
  explicit CatalogEntry(allocators::IAllocator& allocator) : name(allocator), description(allocator) {}
  String name, description;
  bool command_line = false;      // #itscliapp: a terminal program
};

class Catalog {
 public:
  explicit Catalog(allocators::IAllocator& allocator) : allocator_(allocator), entries_(allocator) {}

  // Reads the list (either file); lines that are not entries, or name no valid application, are skipped. Sorted by name. False if memory ran out or
  // there is not a single entry.
  [[nodiscard]] bool parse(StringView text);
  uint64_t count() const { return entries_.count(); }
  // Takes the entries of `other` (which is left empty) in place of these. False if memory ran out (then this one is empty).
  [[nodiscard]] bool take(Catalog& other);
  const CatalogEntry& at(uint64_t index) const { return *entries_.view()[index]; }
  // The entry of that name, or nullptr.
  const CatalogEntry* find(StringView name) const;
  // The entries whose name or description contains every word of `query` (any case), in name order, best first: names that start with the query, then
  // names that contain it, then the rest. At most `limit` indices; *total gets how many matched in all.
  bool search(StringView query, uint64_t limit, List<uint64_t>* indices, uint64_t* total) const;

 private:
  allocators::IAllocator& allocator_;
  List<UniquePointer<CatalogEntry>> entries_;
};

}  // namespace desktop::appman
