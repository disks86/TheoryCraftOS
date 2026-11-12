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

#include "DBus/Activation.hpp"
#include "DBus/Wire.hpp"
#include "Utility/Move.hpp"

namespace dbus {

bool split_exec(allocators::IAllocator& allocator, StringView line, List<String>* words) {
  words->clear();
  String word(allocator);
  bool in_word = false;
  char quote = 0;
  for (uint64_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quote == '\'') {
      if (c == '\'') quote = 0;
      else if (!word.push_back(c)) return false;
      continue;
    }
    if (quote == '"') {
      if (c == '"') { quote = 0; continue; }
      if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\' || line[i + 1] == '$' || line[i + 1] == '`')) {
        if (!word.push_back(line[++i])) return false;
        continue;
      }
      if (!word.push_back(c)) return false;
      continue;
    }
    if (c == ' ' || c == '\t') {
      if (in_word) {
        if (words->count() >= kMaxExecWords || !words->push_back(util::move(word))) return false;
        word = String(allocator);
        in_word = false;
      }
      continue;
    }
    in_word = true;
    if (c == '\'' || c == '"') {
      quote = c;
    } else if (c == '\\' && i + 1 < line.size()) {
      if (!word.push_back(line[++i])) return false;
    } else if (!word.push_back(c)) {
      return false;
    }
  }
  if (quote) return false;
  if (in_word && (words->count() >= kMaxExecWords || !words->push_back(util::move(word)))) return false;
  return words->count() > 0;
}

bool parse_service_file(StringView text, ServiceFile* out) {
  out->name.clear(); out->exec.clear(); out->user.clear(); out->systemd_service.clear();
  if (text.size() > kMaxServiceFileBytes) return false;
  bool in_group = false, seen_group = false;
  StringView rest = text, line;
  while (rest.next_token('\n', &line)) {
    line = line.trimmed();
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[') {
      in_group = line == "[D-BUS Service]";
      seen_group |= in_group;
      continue;
    }
    if (!in_group) continue;
    StringView key, value;
    if (!line.split_once('=', &key, &value)) continue;
    key = key.trimmed();
    value = value.trimmed();
    bool ok = true;
    if (key == "Name") ok = out->name.assign(value);
    else if (key == "Exec") ok = out->exec.assign(value);
    else if (key == "User") ok = out->user.assign(value);
    else if (key == "SystemdService") ok = out->systemd_service.assign(value);
    if (!ok) return false;
  }
  return seen_group && valid_bus_name(out->name.view()) && !is_unique_name(out->name.view()) && !out->exec.empty();
}

}  // namespace dbus
