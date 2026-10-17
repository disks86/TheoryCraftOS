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

#include "Desktop/DirectoryListing.hpp"
#include "FileSystem/IDirectory.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop {

using namespace filesystem;

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

bool entry_less(const DirectoryEntry& a, const DirectoryEntry& b) {
  if (a.type != b.type) return a.type == NodeType::Directory;
  const StringView x = a.name.view(), y = b.name.view();
  const uint64_t n = x.size() < y.size() ? x.size() : y.size();
  for (uint64_t i = 0; i < n; ++i)
    if (lower(x[i]) != lower(y[i])) return lower(x[i]) < lower(y[i]);
  return x.size() < y.size();
}

}  // namespace

Status read_sorted_directory(IFileSystem& fs, StringView path, List<DirectoryEntry>* entries) {
  UniquePointer<IDirectory> directory;
  const Status opened = fs.open_directory(path, &directory);
  if (opened != Status::Ok) return opened;
  entries->clear();
  DirectoryEntry entry;
  while (directory->next(&entry) == Status::Ok) {
    uint64_t low = 0, high = entries->count();  // binary search for the insertion point
    while (low < high) {
      const uint64_t mid = (low + high) / 2;
      if (entry_less((*entries)[mid], entry)) low = mid + 1;
      else high = mid;
    }
    if (!entries->insert(low, entry)) break;
  }
  return Status::Ok;
}

bool describe_entry_type(const DirectoryEntry& entry, String* out) {
  out->clear();
  if (entry.type == NodeType::Directory) return out->assign("Folder");
  const StringView name = entry.name.view();
  const uint64_t dot = name.rfind('.');
  if (dot == StringView::npos || dot == 0 || dot + 1 == name.size()) return out->assign("File");
  for (uint64_t i = dot + 1; i < name.size() && i < dot + 6; ++i)
    if (!out->push_back(upper(name[i]))) return false;
  return out->append(" File");
}

bool name_has_extension(StringView name, StringView extensions) {
  if (extensions.empty() || extensions.equals("*")) return true;
  const uint64_t dot = name.rfind('.');
  if (dot == StringView::npos || dot + 1 >= name.size()) return false;
  const StringView ext = name.substr(dot + 1);
  StringView rest = extensions;
  while (!rest.empty()) {
    uint64_t semi = rest.find(';');
    const StringView one = semi == StringView::npos ? rest : rest.first(semi);
    rest = semi == StringView::npos ? StringView() : rest.without_prefix(semi + 1);
    if (one.size() != ext.size()) continue;
    bool same = true;
    for (uint64_t i = 0; i < one.size() && same; ++i) same = lower(one[i]) == lower(ext[i]);
    if (same) return true;
  }
  return false;
}

StringView first_extension(StringView extensions) {
  const uint64_t semi = extensions.find(';');
  const StringView first = semi == StringView::npos ? extensions : extensions.first(semi);
  return first.equals("*") ? StringView() : first;
}

}  // namespace desktop
