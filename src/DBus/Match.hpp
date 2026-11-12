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

// Match rules (AddMatch, RemoveMatch, BecomeMonitor): "type='signal',interface='org.example.Foo',arg0='bar'".
//
// Keys: type, sender, interface, member, path, path_namespace, destination, arg0..arg63, arg0path..arg63path, arg0namespace and eavesdrop. A value is
// quoted with ' (nothing is escaped inside the quotes; outside them \' is a quote), each key appears at most once, and path with path_namespace is an error.
// A rule matches a message when every key it has matches (see matches()). Two rules are the same rule (RemoveMatch) when they have the same keys and values,
// whatever their order and quoting.
#pragma once
#include <stdint.h>
#include "DBus/Message.hpp"
#include "DBus/Wire.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace dbus {

class MatchRule {
 public:
  static constexpr uint32_t kMaxRuleBytes = 1024;
  static constexpr uint32_t kMaxArgs = 64;

  explicit MatchRule(allocators::IAllocator& allocator)
      : allocator_(allocator), sender(allocator), interface(allocator), member(allocator), path(allocator), path_namespace(allocator), destination(allocator), arg0namespace(allocator),
        args(allocator) {}
  MatchRule(const MatchRule&) = delete;
  MatchRule& operator=(const MatchRule&) = delete;

  // False (and the rule is left half made) when `text` is not a valid rule.
  bool parse(StringView text);
  bool same_as(const MatchRule& other) const;

  // Whether the message matches. `sender_owns(name)` answers whether the message's sender is the primary owner of the well-known name the rule's sender
  // key gives (so a rule for "org.example.Service" follows the service from owner to owner). `args` are the message's string arguments (string_args()),
  // looked up only when the rule has arg keys (args_wanted() says how many).
  template <class SenderOwns>
  bool matches(const Message& m, const StringArgs* args, SenderOwns sender_owns) const;
  uint32_t args_wanted() const { return args_wanted_; }
  allocators::IAllocator& allocator_;
  bool eavesdrop = false;

  uint8_t type = 0;      // 0: any
  String sender, interface, member, path, path_namespace, destination, arg0namespace;
  struct Arg {
    explicit Arg(allocators::IAllocator& allocator) : value(allocator) {}
    Arg(Arg&&) = default;
    Arg& operator=(Arg&&) = default;
    uint32_t index = 0;
    bool is_path = false;
    String value;
  };
  List<Arg> args;

 private:
  bool set(StringView key, StringView value);
  uint32_t seen_ = 0;      // which of the single keys have been given
  uint32_t args_wanted_ = 0;
};

bool arg_path_matches(StringView rule, StringView arg);

template <class SenderOwns>
bool MatchRule::matches(const Message& m, const StringArgs* found, SenderOwns sender_owns) const {
  if (type && m.type != type) return false;
  if (!sender.empty() && sender != m.sender.view() && (is_unique_name(sender.view()) || !sender_owns(sender.view()))) return false;
  if (!interface.empty() && interface != m.interface.view()) return false;
  if (!member.empty() && member != m.member.view()) return false;
  if (!path.empty() && path != m.path.view()) return false;
  if (!path_namespace.empty()) {
    const StringView ns = path_namespace.view(), p = m.path.view();
    if (m.path.empty()) return false;
    if (!(ns == "/" || p == ns || (p.starts_with(ns) && p.size() > ns.size() && p[ns.size()] == '/'))) return false;
  }
  if (!destination.empty() && destination != m.destination.view()) return false;
  if (args_wanted_ == 0) return true;
  if (!found) return false;
  if (!arg0namespace.empty()) {
    if (found->count < 1 || found->kind[0] != 's') return false;
    const StringView ns = arg0namespace.view(), a = found->value[0];
    if (!(a == ns || (a.starts_with(ns) && a.size() > ns.size() && a[ns.size()] == '.'))) return false;
  }
  for (const Arg& rule : args.view()) {
    if (rule.index >= found->count || found->kind[rule.index] == 0) return false;
    const StringView a = found->value[rule.index];
    if (rule.is_path) {
      if (!arg_path_matches(rule.value.view(), a)) return false;
    } else {
      if (found->kind[rule.index] != 's' || rule.value != a) return false;
    }
  }
  return true;
}

}  // namespace dbus
