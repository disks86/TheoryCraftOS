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

#include "Desktop/AppMan/WingetCatalog.hpp"
#include "Desktop/AppMan/SqliteReader.hpp"
#include "Media/Zip.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Move.hpp"

namespace desktop::appman {

namespace {

constexpr StringView kListHeader = "winget-packages 1";
constexpr uint64_t kMaxField = 512;

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }

// A field of the list: text with no control characters in it (tabs and line ends separate the fields), of a sensible length.
bool plain_field(StringView text) {
  if (text.size() > kMaxField) return false;
  for (const char c : text)
    if (static_cast<uint8_t>(c) < 0x20 || c == 0x7F) return false;
  return true;
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

int compare_folded(StringView a, StringView b) {
  const uint64_t n = a.size() < b.size() ? a.size() : b.size();
  for (uint64_t i = 0; i < n; ++i) {
    const uint8_t x = static_cast<uint8_t>(lower(a[i])), y = static_cast<uint8_t>(lower(b[i]));
    if (x != y) return x < y ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

bool contains_folded(StringView text, StringView word, uint64_t* at) {
  if (word.empty()) {
    if (at) *at = 0;
    return true;
  }
  if (word.size() > text.size()) return false;
  for (uint64_t i = 0; i <= text.size() - word.size(); ++i) {
    uint64_t k = 0;
    while (k < word.size() && lower(text[i + k]) == lower(word[k])) ++k;
    if (k == word.size()) {
      if (at) *at = i;
      return true;
    }
  }
  return false;
}

bool valid_package_id(StringView id) {
  if (id.empty() || id.size() > 128 || id[0] == '.' || id[0] == ' ' || id.ends_with('.') || id.ends_with(' ')) return false;
  for (const char c : id) {
    if (static_cast<uint8_t>(c) < 0x20 || c == 0x7F) return false;
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
  }
  return !id.contains("..");
}

bool WingetCatalog::add(StringView id, StringView name, StringView moniker, StringView version, ConstByteView hash) {
  // (a row this package manager cannot use is skipped, not an error: the list is other people's data)
  if (!valid_package_id(id) || name.empty() || !plain_field(name) || !plain_field(moniker) || version.empty() || !plain_field(version) || hash.size() != 32) return true;
  if (entries_.count() >= kMaxWingetPackages) return false;
  UniquePointer<WingetPackage> entry = UniquePointer<WingetPackage>::make(allocator_, allocator_);
  if (!entry || !entry->id.assign(id) || !entry->name.assign(name) || !entry->moniker.assign(moniker) || !entry->version.assign(version)) return false;
  if (!ByteView(entry->hash).copy_from(hash)) return false;
  return entries_.push_back(util::move(entry));
}

// A row of the packages table: rowid, id, name, moniker, latest_version, arp_min_version, arp_max_version, hash.
bool WingetCatalog::add_row(void* context, int64_t, ArrayView<const SqliteValue> columns) {
  WingetCatalog& catalog = *static_cast<WingetCatalog*>(context);
  if (columns.size() < 8 || columns[7].kind != SqliteValue::Kind::Blob) return true;
  return catalog.add(columns[1].text(), columns[2].text(), columns[3].text(), columns[4].text(), columns[7].bytes);
}

void WingetCatalog::sort() {
  // By name, then identifier. (Shell sort: the index is in identifier order, which is far from name order, and there are thousands of rows.)
  ArrayView<UniquePointer<WingetPackage>> list = entries_.view();
  auto before = [](const WingetPackage& a, const WingetPackage& b) {
    const int c = compare_folded(a.name.view(), b.name.view());
    return c != 0 ? c < 0 : compare_folded(a.id.view(), b.id.view()) < 0;
  };
  uint64_t gap = 1;
  while (gap < list.size() / 3) gap = gap * 3 + 1;
  for (; gap > 0; gap /= 3) {
    for (uint64_t i = gap; i < list.size(); ++i) {
      UniquePointer<WingetPackage> moving = util::move(list[i]);
      uint64_t j = i;
      for (; j >= gap && before(*moving, *list[j - gap]); j -= gap) list[j] = util::move(list[j - gap]);
      list[j] = util::move(moving);
    }
  }
}

bool WingetCatalog::parse_index(ConstByteView msix) {
  entries_.clear();
  media::MemoryZipSource source(msix);
  media::ZipReader zip(allocator_, source);
  if (!zip.open()) return false;
  const media::ZipEntry* member = zip.find(kWingetIndexMember);
  ByteBuffer database(allocator_);
  if (!member || !zip.extract(*member, kMaxWingetDatabaseBytes, &database)) return false;
  return parse_database(database.view());
}

bool WingetCatalog::parse_database(ConstByteView database) {
  entries_.clear();
  SqliteReader reader(allocator_, database);
  uint32_t root = 0;
  if (!reader.open() || !reader.table_root("packages", &root) || !reader.scan_table(root, &WingetCatalog::add_row, this)) {
    entries_.clear();
    return false;
  }
  sort();
  return entries_.count() > 0;
}

bool WingetCatalog::write_list(String* out) const {
  out->clear();
  if (!out->append(kListHeader) || !out->push_back('\n')) return false;
  static constexpr char kHex[] = "0123456789abcdef";
  for (const UniquePointer<WingetPackage>& entry : entries_.view()) {
    if (!out->append(entry->id.view()) || !out->push_back('\t') || !out->append(entry->name.view()) || !out->push_back('\t') || !out->append(entry->moniker.view()) ||
        !out->push_back('\t') || !out->append(entry->version.view()) || !out->push_back('\t'))
      return false;
    for (const uint8_t byte : entry->hash)
      if (!out->push_back(kHex[byte >> 4]) || !out->push_back(kHex[byte & 15])) return false;
    if (!out->push_back('\n')) return false;
  }
  return true;
}

bool WingetCatalog::parse_list(StringView text) {
  entries_.clear();
  StringView rest = text, line;
  if (!rest.next_token('\n', &line) || !line.equals(kListHeader)) return false;
  while (rest.next_token('\n', &line)) {
    StringView fields[5];
    uint32_t count = 0;
    StringView field;
    while (count < 5 && line.next_token('\t', &field)) fields[count++] = field;
    if (count != 5 || !line.empty() || fields[4].size() != 64) continue;
    uint8_t hash[32];
    bool hex = true;
    for (uint32_t i = 0; i < 32 && hex; ++i) {
      const int high = hex_digit(fields[4][2 * i]), low = hex_digit(fields[4][2 * i + 1]);
      hex = high >= 0 && low >= 0;
      hash[i] = static_cast<uint8_t>((high << 4) | low);
    }
    if (!hex) continue;
    if (!add(fields[0], fields[1], fields[2], fields[3], ConstByteView(hash))) {
      entries_.clear();
      return false;
    }
  }
  sort();      // (it was written sorted: one pass)
  return entries_.count() > 0;
}

bool WingetCatalog::take(WingetCatalog& other) {
  entries_.clear();
  bool ok = entries_.reserve(other.entries_.count());
  for (UniquePointer<WingetPackage>& entry : other.entries_.view()) ok = ok && entries_.push_back(util::move(entry));
  other.entries_.clear();
  if (!ok) entries_.clear();
  return ok;
}

const WingetPackage* WingetCatalog::find(StringView id) const {
  for (const UniquePointer<WingetPackage>& entry : entries_.view())
    if (compare_folded(entry->id.view(), id) == 0) return entry.get();
  return nullptr;
}

bool WingetCatalog::search(StringView query, uint64_t limit, List<uint64_t>* indices, uint64_t* total, List<uint8_t>* tiers) const {
  indices->clear();
  if (tiers) tiers->clear();
  *total = 0;
  query = query.trimmed();
  List<uint64_t> found[3] = {List<uint64_t>(allocator_), List<uint64_t>(allocator_), List<uint64_t>(allocator_)};
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    const WingetPackage& e = *entries_.view()[i];
    bool all = true;
    StringView words = query, word;
    while (all && words.next_token(' ', &word)) {
      if (word.empty()) continue;
      all = contains_folded(e.name.view(), word) || contains_folded(e.id.view(), word) || contains_folded(e.moniker.view(), word);
    }
    if (!all) continue;
    ++*total;
    uint64_t at = 0;
    const int tier = query.empty() ? 0 : compare_folded(e.moniker.view(), query) == 0 ? 0 : contains_folded(e.name.view(), query, &at) ? (at == 0 ? 0 : 1) : 2;
    if (found[tier].count() < limit && !found[tier].push_back(i)) return false;
  }
  for (uint8_t tier = 0; tier < 3; ++tier)
    for (const uint64_t i : found[tier].view()) {
      if (indices->count() >= limit) return true;
      if (!indices->push_back(i) || (tiers && !tiers->push_back(tier))) return false;
    }
  return true;
}

}  // namespace desktop::appman
