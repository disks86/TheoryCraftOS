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

#include "Network/Http/AltSvc.hpp"
#include "Utility/Move.hpp"

namespace network {

namespace {

StringView trim(StringView s) {
  while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s = s.substr(1);
  while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\t')) s = s.first(s.size() - 1);
  return s;
}

bool parse_number(StringView s, uint64_t* out) {
  if (s.empty() || s.size() > 12) return false;
  uint64_t v = 0;
  for (uint64_t i = 0; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
  }
  *out = v;
  return true;
}

}  // namespace

int64_t AltSvcCache::index_of(StringView host, uint16_t port) const {
  for (uint64_t i = 0; i < entries_.count(); ++i)
    if (entries_[i].origin_port == port && entries_[i].host.view().equals_ignore_case(host)) return static_cast<int64_t>(i);
  return -1;
}

void AltSvcCache::forget(StringView host, uint16_t port) {
  kernel::ScopedMutex lock(mutex_);
  forget_locked(host, port);
}

void AltSvcCache::forget_locked(StringView host, uint16_t port) {
  const int64_t i = index_of(host, port);
  if (i >= 0) entries_.remove_at(static_cast<uint64_t>(i));
}

void AltSvcCache::learn(StringView host, uint16_t port, StringView header, uint64_t now_ms) {
  kernel::ScopedMutex lock(mutex_);
  header = trim(header);
  if (header.equals_ignore_case("clear")) return forget_locked(host, port);
  // alt-value, alt-value ... : the first "h3" one that names this host (or none) is the one used
  for (uint64_t at = 0; at < header.size();) {
    uint64_t end = header.find(',', at);
    if (end == StringView::npos) end = header.size();
    const StringView value = trim(header.substr(at, end - at));
    at = end + 1;
    const uint64_t eq = value.find('=');
    if (eq == StringView::npos || !trim(value.first(eq)).equals("h3")) continue;
    StringView rest = trim(value.substr(eq + 1));
    if (rest.empty() || rest[0] != '"') continue;
    const uint64_t close = rest.find('"', 1);
    if (close == StringView::npos) continue;
    const StringView authority = rest.substr(1, close - 1);
    const uint64_t colon = authority.find(':');
    if (colon == StringView::npos) continue;
    if (colon > 0 && !authority.first(colon).equals_ignore_case(host)) continue;   // another host: not used
    uint64_t h3_port = 0;
    if (!parse_number(authority.substr(colon + 1), &h3_port) || h3_port == 0 || h3_port > 65535) continue;
    uint64_t max_age = kDefaultAgeSeconds;
    StringView params = rest.substr(close + 1);
    while (!params.empty()) {
      uint64_t semi = params.find(';');
      const StringView p = trim(semi == StringView::npos ? params : params.first(semi));
      params = semi == StringView::npos ? StringView() : params.substr(semi + 1);
      const uint64_t peq = p.find('=');
      if (peq != StringView::npos && trim(p.first(peq)).equals("ma")) {
        StringView v = trim(p.substr(peq + 1));
        if (v.size() >= 2 && v[0] == '"') v = v.substr(1, v.size() - 2);
        uint64_t n = 0;
        if (parse_number(v, &n)) max_age = n;
      }
    }
    if (max_age > kMaxAgeSeconds) max_age = kMaxAgeSeconds;
    if (max_age == 0) return forget_locked(host, port);
    const int64_t i = index_of(host, port);
    if (i >= 0) {
      Entry& e = entries_[static_cast<uint64_t>(i)];
      e.h3_port = static_cast<uint16_t>(h3_port);
      e.expires_ms = now_ms + max_age * 1000;
      return;
    }
    if (entries_.count() >= kMaxEntries) entries_.remove_at(0);   // (the oldest goes)
    Entry fresh(allocator_);
    if (!fresh.host.assign(host)) return;
    fresh.origin_port = port;
    fresh.h3_port = static_cast<uint16_t>(h3_port);
    fresh.expires_ms = now_ms + max_age * 1000;
    entries_.push_back(util::move(fresh));
    return;
  }
}

bool AltSvcCache::find(StringView host, uint16_t port, uint64_t now_ms, uint16_t* h3_port) const {
  kernel::ScopedMutex lock(mutex_);
  const int64_t i = index_of(host, port);
  if (i < 0) return false;
  const Entry& e = entries_[static_cast<uint64_t>(i)];
  if (now_ms >= e.expires_ms || now_ms < e.broken_until_ms) return false;
  *h3_port = e.h3_port;
  return true;
}

bool AltSvcCache::is_broken(StringView host, uint16_t port, uint64_t now_ms) const {
  kernel::ScopedMutex lock(mutex_);
  const int64_t i = index_of(host, port);
  return i >= 0 && now_ms < entries_[static_cast<uint64_t>(i)].broken_until_ms;
}

void AltSvcCache::mark_broken(StringView host, uint16_t port, uint64_t now_ms) {
  kernel::ScopedMutex lock(mutex_);
  const int64_t i = index_of(host, port);
  if (i >= 0) {
    entries_[static_cast<uint64_t>(i)].broken_until_ms = now_ms + kBrokenMs;
    return;
  }
  if (entries_.count() >= kMaxEntries) entries_.remove_at(0);
  Entry fresh(allocator_);
  if (!fresh.host.assign(host)) return;
  fresh.origin_port = port;
  fresh.broken_until_ms = now_ms + kBrokenMs;   // (expires_ms is 0: nothing is announced, only the failure is remembered)
  entries_.push_back(util::move(fresh));
}

}  // namespace network
