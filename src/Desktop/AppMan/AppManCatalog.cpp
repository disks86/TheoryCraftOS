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

#include "Desktop/AppMan/AppManCatalog.hpp"
#include "Utility/Move.hpp"

namespace desktop::appman {

namespace {

constexpr StringView kBullet = "\xE2\x97\x86";      // ◆

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }

// Does `text` contain `word`, without regard to ASCII case? *at gets where.
bool contains_folded(StringView text, StringView word, uint64_t* at = nullptr) {
  if (word.empty()) return true;
  for (uint64_t i = 0; i + word.size() <= text.size(); ++i) {
    uint64_t k = 0;
    while (k < word.size() && lower(text[i + k]) == lower(word[k])) ++k;
    if (k == word.size()) {
      if (at) *at = i;
      return true;
    }
  }
  return false;
}

}  // namespace

bool valid_app_name(StringView name) {
  if (name.empty() || name.size() > 64 || name[0] == '.' || name[0] == '-') return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-';
    if (!ok) return false;
  }
  return !name.equals("..");
}

bool Catalog::parse(StringView text) {
  entries_.clear();
  StringView rest = text, line;
  while (rest.next_token('\n', &line)) {
    line = line.trimmed();
    if (!line.starts_with(kBullet)) continue;
    line = line.substr(kBullet.size()).trimmed();
    const uint64_t colon = line.find(" : ");
    if (colon == StringView::npos) continue;
    const StringView name = line.first(colon).trimmed();
    StringView description = line.substr(colon + 3).trimmed();
    if (!valid_app_name(name)) continue;
    UniquePointer<CatalogEntry> entry = UniquePointer<CatalogEntry>::make(allocator_, allocator_);
    if (!entry) return false;
    // Trailing tags: "#itsdesktopapp", "#itscliapp".
    while (true) {
      const uint64_t hash = description.rfind('#');
      if (hash == StringView::npos || (hash > 0 && description[hash - 1] != ' ')) break;
      const StringView tag = description.substr(hash + 1);
      bool word = !tag.empty();
      for (char c : tag) word &= (c >= 'a' && c <= 'z');
      if (!word && !tag.empty()) break;
      if (tag.equals("itscliapp")) entry->command_line = true;
      description = description.first(hash).trimmed();
      if (tag.empty()) break;
    }
    if (!entry->name.assign(name) || !entry->description.assign(description) || !entries_.push_back(util::move(entry))) return false;
  }
  // Sorted by name (the file is already, nearly always: an insertion sort is cheap then).
  ArrayView<UniquePointer<CatalogEntry>> list = entries_.view();
  for (uint64_t i = 1; i < list.size(); ++i) {
    for (uint64_t j = i; j > 0 && list[j]->name.view() < list[j - 1]->name.view(); --j) {
      UniquePointer<CatalogEntry> t = util::move(list[j]);
      list[j] = util::move(list[j - 1]);
      list[j - 1] = util::move(t);
    }
  }
  return entries_.count() > 0;
}

bool Catalog::take(Catalog& other) {
  entries_.clear();
  bool ok = entries_.reserve(other.entries_.count());
  for (UniquePointer<CatalogEntry>& entry : other.entries_.view())
    ok = ok && entries_.push_back(util::move(entry));
  other.entries_.clear();
  if (!ok) entries_.clear();
  return ok;
}

const CatalogEntry* Catalog::find(StringView name) const {
  // (binary search: the list is sorted by name)
  uint64_t lo = 0, hi = entries_.count();
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    const int c = entries_.view()[mid]->name.view().compare(name);
    if (c == 0) return entries_.view()[mid].get();
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

bool Catalog::search(StringView query, uint64_t limit, List<uint64_t>* indices, uint64_t* total) const {
  indices->clear();
  *total = 0;
  query = query.trimmed();
  List<uint64_t> tiers[3] = {List<uint64_t>(allocator_), List<uint64_t>(allocator_), List<uint64_t>(allocator_)};
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    const CatalogEntry& e = *entries_.view()[i];
    bool all = true;
    StringView words = query, word;
    while (all && words.next_token(' ', &word)) {
      if (word.empty()) continue;
      all = contains_folded(e.name.view(), word) || contains_folded(e.description.view(), word);
    }
    if (!all) continue;
    ++*total;
    uint64_t at = 0;
    const int tier = query.empty() ? 0 : contains_folded(e.name.view(), query, &at) ? (at == 0 ? 0 : 1) : 2;
    if (!tiers[tier].push_back(i)) return false;
  }
  for (const List<uint64_t>& tier : tiers)
    for (const uint64_t i : tier.view()) {
      if (indices->count() >= limit) return true;
      if (!indices->push_back(i)) return false;
    }
  return true;
}

}  // namespace desktop::appman
