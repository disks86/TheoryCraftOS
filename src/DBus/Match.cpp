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

#include "DBus/Match.hpp"

namespace dbus {

namespace {

enum SingleKey : uint32_t { kType = 1, kSender = 2, kInterface = 4, kMember = 8, kPath = 16, kPathNamespace = 32, kDestination = 64, kArg0Namespace = 128,
                            kEavesdrop = 256 };

bool valid_namespace(StringView ns) {
  if (ns.empty() || ns.size() > kMaxNameBytes || ns[0] == '.' || ns.back() == '.') return false;
  char previous = '.';
  for (char c : ns) {
    const bool word = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (c == '.' ? previous == '.' : !word) return false;
    previous = c;
  }
  return true;
}

}  // namespace

bool arg_path_matches(StringView rule, StringView arg) {
  if (rule == arg) return true;
  if (rule.ends_with('/') && arg.starts_with(rule)) return true;
  if (arg.ends_with('/') && rule.starts_with(arg)) return true;
  return false;
}

bool MatchRule::set(StringView key, StringView value) {
  auto once = [&](uint32_t bit) {
    if (seen_ & bit) return false;
    seen_ |= bit;
    return true;
  };
  if (key == "type") {
    if (!once(kType)) return false;
    if (value == "signal") type = kSignal;
    else if (value == "method_call") type = kMethodCall;
    else if (value == "method_return") type = kMethodReturn;
    else if (value == "error") type = kError;
    else return false;
    return true;
  }
  if (key == "sender") return once(kSender) && valid_bus_name(value) && sender.assign(value);
  if (key == "interface") return once(kInterface) && valid_interface_name(value) && interface.assign(value);
  if (key == "member") return once(kMember) && valid_member_name(value) && member.assign(value);
  if (key == "path") return once(kPath) && !(seen_ & kPathNamespace) && valid_object_path(value) && path.assign(value);
  if (key == "path_namespace") return once(kPathNamespace) && !(seen_ & kPath) && valid_object_path(value) && path_namespace.assign(value);
  if (key == "destination") return once(kDestination) && valid_bus_name(value) && destination.assign(value);
  if (key == "eavesdrop") {
    if (!once(kEavesdrop)) return false;
    if (value == "true") eavesdrop = true;
    else if (value == "false") eavesdrop = false;
    else return false;
    return true;
  }
  if (key == "arg0namespace") {
    if (!once(kArg0Namespace) || !valid_namespace(value) || !arg0namespace.assign(value)) return false;
    if (args_wanted_ < 1) args_wanted_ = 1;
    return true;
  }
  if (key.starts_with("arg")) {
    StringView digits = key.without_prefix(3);
    bool is_path = false;
    if (digits.ends_with("path")) {
      digits = digits.without_suffix(4);
      is_path = true;
    }
    uint64_t index = 0;
    if (digits.empty() || digits.size() > 2 || !digits.parse_uint(&index) || index >= kMaxArgs) return false;
    if (digits.size() == 2 && digits[0] == '0') return false;
    uint64_t at = 0;
    while (at < args.count() && args[at].index < index) ++at;
    if (at < args.count() && args[at].index == index) return false;      // (the same argument twice)
    Arg arg(allocator_);
    arg.index = uint32_t(index);
    arg.is_path = is_path;
    if (!arg.value.assign(value) || !args.insert(at, util::move(arg))) return false;
    if (args_wanted_ < index + 1) args_wanted_ = uint32_t(index + 1);
    return true;
  }
  return false;
}

bool MatchRule::parse(StringView text) {
  if (text.size() > kMaxRuleBytes) return false;
  String value(allocator_);
  uint64_t i = 0;
  const uint64_t n = text.size();
  while (i < n) {
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i == n) break;
    const uint64_t key_start = i;
    while (i < n && text[i] != '=') ++i;
    if (i == n) return false;
    const StringView key = text.substr(key_start, i - key_start).trimmed();
    ++i;      // '='
    value.clear();
    bool quoted = false;
    while (i < n) {
      const char c = text[i];
      if (quoted) {
        if (c == '\'') quoted = false;
        else if (!value.push_back(c)) return false;
        ++i;
        continue;
      }
      if (c == ',') break;
      if (c == '\'') {
        quoted = true;
        ++i;
        continue;
      }
      if (c == '\\' && i + 1 < n && text[i + 1] == '\'') {
        if (!value.push_back('\'')) return false;
        i += 2;
        continue;
      }
      if (!value.push_back(c)) return false;
      ++i;
    }
    if (quoted || key.empty() || !set(key, value.view())) return false;
    if (i < n) ++i;      // ','
  }
  return true;
}

bool MatchRule::same_as(const MatchRule& o) const {
  if (type != o.type || eavesdrop != o.eavesdrop || sender != o.sender.view() || interface != o.interface.view() || member != o.member.view() ||
      path != o.path.view() || path_namespace != o.path_namespace.view() || destination != o.destination.view() || arg0namespace != o.arg0namespace.view() ||
      args.count() != o.args.count())
    return false;
  for (uint64_t i = 0; i < args.count(); ++i)
    if (args[i].index != o.args[i].index || args[i].is_path != o.args[i].is_path || args[i].value != o.args[i].value.view()) return false;
  return true;
}

}  // namespace dbus
