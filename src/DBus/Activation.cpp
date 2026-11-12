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

namespace {

// A unit file's assignments as systemd reads them: '#' and ';' lines are comments, a line that ends in '\\' goes on in the next one (comment lines in
// between are skipped). `f(group, key, value)` for each, with the group's header ("[Service]"); false when `f` is.
template <typename F>
bool for_each_assignment(allocators::IAllocator& allocator, StringView text, F&& f) {
  if (text.size() > kMaxServiceFileBytes) return false;
  String logical(allocator);
  StringView group, rest = text, line;
  bool going_on = false;
  const auto finish = [&]() {
    StringView key, value;
    if (!logical.view().split_once('=', &key, &value)) return true;
    return f(group, key.trimmed(), value.trimmed());
  };
  while (rest.next_token('\n', &line)) {
    line = line.trimmed();
    if (!line.empty() && (line[0] == '#' || line[0] == ';')) continue;
    if (!going_on) {
      if (line.empty()) continue;
      if (line[0] == '[') { group = line; continue; }
      logical.clear();
    }
    const bool more = line.ends_with('\\');
    if (!logical.append(more ? line.without_suffix(1) : line)) return false;
    if (more) {
      if (!logical.push_back(' ')) return false;
      going_on = true;
      continue;
    }
    going_on = false;
    if (!finish()) return false;
  }
  return !going_on || finish();
}

// `word` with "%%" made '%'; false when it has another specifier (%n, %i, ...: not expanded here).
bool expand_percent(StringView word, String* out) {
  out->clear();
  for (uint64_t i = 0; i < word.size(); ++i) {
    if (word[i] == '%') {
      if (i + 1 >= word.size() || word[i + 1] != '%') return false;
      ++i;
    }
    if (!out->push_back(word[i])) return false;
  }
  return true;
}

}  // namespace

bool parse_unit_service(allocators::IAllocator& allocator, StringView text, UnitService* out) {
  out->argv.clear();
  out->environment.clear();
  bool usable = false, have_command = false;
  List<String> words(allocator);
  String expanded(allocator);
  const bool read = for_each_assignment(allocator, text, [&](StringView group, StringView key, StringView value) {
    if (group != "[Service]") return true;
    if (key == "ExecStart") {
      if (value.empty()) {      // (an empty assignment drops the commands before it)
        out->argv.clear();
        have_command = usable = false;
        return true;
      }
      if (have_command) return true;      // (the first command: more than one is for Type=oneshot, run one after another)
      have_command = true;
      bool at = false, verbatim = false;
      while (!value.empty() && (value[0] == '-' || value[0] == '@' || value[0] == ':' || value[0] == '+' || value[0] == '!')) {
        at |= value[0] == '@';
        verbatim |= value[0] == ':';
        value = value.without_prefix(1);
      }
      if (!split_exec(allocator, value, &words)) return true;
      if (at) {
        if (words.count() < 2) return true;
        words.remove_at(1);
      }
      for (const String& w : words.view()) {
        if ((!verbatim && w.view().contains('$')) || !expand_percent(w.view(), &expanded)) {
          out->argv.clear();
          return true;
        }
        String copy(allocator);
        if (!copy.assign(expanded.view()) || !out->argv.push_back(util::move(copy))) return false;
      }
      usable = out->argv.count() > 0 && out->argv[0].view().starts_with('/');
      return true;
    }
    if (key == "Environment") {
      if (value.empty()) {
        out->environment.clear();
        return true;
      }
      if (!split_exec(allocator, value, &words)) return true;
      for (const String& w : words.view()) {
        StringView name, rest;
        if (!w.view().split_once('=', &name, &rest) || name.empty() || !expand_percent(w.view(), &expanded)) continue;
        bool replaced = false;      // (a later assignment of a name replaces the earlier one)
        for (String& e : out->environment.view())
          if (e.view().starts_with(name) && e.view().size() > name.size() && e.view()[name.size()] == '=') {
            if (!e.assign(expanded.view())) return false;
            replaced = true;
          }
        if (replaced) continue;
        if (out->environment.count() >= kMaxUnitEnvironment) continue;
        String copy(allocator);
        if (!copy.assign(expanded.view()) || !out->environment.push_back(util::move(copy))) return false;
      }
    }
    return true;
  });
  return read && usable;
}

bool unit_has_alias(StringView text, StringView alias) {
  // (no allocation needed: an Alias= line is matched as it stands; continued lines are rare in [Install] and are not looked at)
  bool in_install = false;
  StringView rest = text, line;
  if (text.size() > kMaxServiceFileBytes) return false;
  while (rest.next_token('\n', &line)) {
    line = line.trimmed();
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line[0] == '[') { in_install = line == "[Install]"; continue; }
    StringView key, value;
    if (!in_install || !line.split_once('=', &key, &value) || key.trimmed() != "Alias") continue;
    StringView names = value.trimmed(), one;
    while (names.next_token(' ', &one))
      if (one.trimmed() == alias) return true;
  }
  return false;
}

}  // namespace dbus
