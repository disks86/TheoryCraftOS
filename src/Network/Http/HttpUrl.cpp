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

#include "Network/Http/HttpUrl.hpp"
#include "Utility/Move.hpp"

namespace network {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool printable(StringView s) {
  for (char c : s)
    if (c < 0x21 || c > 0x7E) return false;
  return true;
}

bool starts_with_scheme(StringView text, const char* scheme) {
  uint64_t n = 0;
  while (scheme[n]) ++n;
  if (text.size() < n) return false;
  for (uint64_t i = 0; i < n; ++i)
    if (lower(text[i]) != scheme[i]) return false;
  return true;
}

// Replaces *out with `path_and_query` (the path starting with '/') minus its "." and ".." segments (RFC 3986 5.2.4); the query stays.
// `path_and_query` must not be (part of) *out.
bool normalize_path(StringView path_and_query, String* out) {
  const uint64_t q = path_and_query.find('?');
  const StringView path = q == StringView::npos ? path_and_query : path_and_query.first(q);
  const StringView query = q == StringView::npos ? StringView() : path_and_query.substr(q);
  constexpr uint32_t kMaxSegments = 128;
  StringView kept[kMaxSegments];
  uint32_t count = 0;
  bool directory = false;   // the path ends in a slash, or in "." or ".."
  uint64_t at = path.empty() ? 0 : 1;   // (past the leading slash)
  for (;;) {
    uint64_t end = at;
    while (end < path.size() && path[end] != '/') ++end;
    const StringView segment = path.substr(at, end - at);
    const bool last = end >= path.size();
    if (segment.equals("..")) {
      if (count) --count;
      directory = last;
    } else if (segment.equals(".")) {
      directory = last;
    } else if (last && segment.empty()) {
      directory = true;
    } else {
      if (count == kMaxSegments) return false;
      kept[count++] = segment;
      directory = false;
    }
    if (last) break;
    at = end + 1;
  }
  out->clear();
  for (uint32_t i = 0; i < count; ++i)
    if (!out->push_back('/') || !out->append(kept[i])) return false;
  if ((count == 0 || directory) && !out->push_back('/')) return false;
  return out->append(query);
}

}  // namespace

bool HttpUrl::assign_text(String* out) const {
  out->clear();
  if (!out->append(https ? "https://" : "http://") || !out->append(host.view())) return false;
  if (!default_port() && (!out->push_back(':') || !out->append_uint(port))) return false;
  return out->append(path.view());
}

bool parse_http_url(StringView text, HttpUrl* out) {
  uint64_t at;
  bool https;
  if (starts_with_scheme(text, "https://")) {
    https = true;
    at = 8;
  } else if (starts_with_scheme(text, "http://")) {
    https = false;
    at = 7;
  } else {
    return false;
  }
  if (!printable(text)) return false;
  uint64_t end = at;
  while (end < text.size() && text[end] != '/' && text[end] != '?' && text[end] != '#') ++end;
  const StringView authority = text.substr(at, end - at);
  if (authority.empty() || authority.contains('@') || authority.contains('[') || authority.contains(']')) return false;
  StringView host = authority;
  uint32_t port = https ? 443 : 80;
  const uint64_t colon = authority.rfind(':');
  if (colon != StringView::npos) {
    host = authority.first(colon);
    const StringView digits = authority.substr(colon + 1);
    if (digits.empty() || digits.size() > 5) return false;
    port = 0;
    for (char c : digits) {
      if (c < '0' || c > '9') return false;
      port = port * 10 + static_cast<uint32_t>(c - '0');
    }
    if (port == 0 || port > 65535) return false;
  }
  if (host.empty() || host.size() > 253 || host.contains(':')) return false;
  // The rest: the path and the query, up to a '#'.
  StringView rest = text.substr(end);
  const uint64_t hash = rest.find('#');
  if (hash != StringView::npos) rest = rest.first(hash);
  out->https = https;
  out->port = static_cast<uint16_t>(port);
  if (!out->host.assign(host)) return false;
  String raw(out->allocator());
  if (rest.empty() || rest[0] == '?') {
    if (!raw.push_back('/')) return false;
  }
  return raw.append(rest) && normalize_path(raw.view(), &out->path);
}

bool resolve_http_reference(const HttpUrl& base, StringView reference, HttpUrl* out) {
  while (!reference.empty() && (reference[0] == ' ' || reference[0] == '\t')) reference = reference.without_prefix(1);
  while (!reference.empty() && (reference[reference.size() - 1] == ' ' || reference[reference.size() - 1] == '\t')) reference = reference.first(reference.size() - 1);
  if (reference.empty()) return false;
  if (starts_with_scheme(reference, "http://") || starts_with_scheme(reference, "https://")) return parse_http_url(reference, out);
  if (!printable(reference)) return false;
  // A scheme ("ftp:", "mailto:", "data:") before any '/', '?' or '#' means another kind of URL: not one this client can go to.
  for (uint64_t i = 0; i < reference.size(); ++i) {
    const char c = reference[i];
    if (c == ':') {
      if (i > 0) return false;
      break;
    }
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool more = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    if (i == 0 ? !letter : !(letter || more)) break;
  }
  const uint64_t hash = reference.find('#');
  if (hash != StringView::npos) reference = reference.first(hash);
  if (reference.empty()) {   // only a fragment: the same document
    out->https = base.https;
    out->port = base.port;
    return out->host.assign(base.host.view()) && out->path.assign(base.path.view());
  }
  if (reference.starts_with(StringView("//"))) {   // a network-path reference: keeps the scheme
    String text(base.allocator());
    return text.append(base.https ? "https:" : "http:") && text.append(reference) && parse_http_url(text.view(), out);
  }
  out->https = base.https;
  out->port = base.port;
  if (!out->host.assign(base.host.view())) return false;
  String merged(base.allocator());
  if (reference[0] == '/') {
    if (!merged.append(reference)) return false;
  } else if (reference[0] == '?') {   // the same path with another query
    const uint64_t q = base.path.view().find('?');
    if (!merged.append(q == StringView::npos ? base.path.view() : base.path.view().first(q)) || !merged.append(reference)) return false;
  } else {   // relative to the folder of the base path
    const StringView base_path = base.path.view();
    const uint64_t q = base_path.find('?');
    const StringView without_query = q == StringView::npos ? base_path : base_path.first(q);
    const uint64_t slash = without_query.rfind('/');
    if (!merged.append(slash == StringView::npos ? StringView("/") : without_query.first(slash + 1)) || !merged.append(reference)) return false;
  }
  return normalize_path(merged.view(), &out->path);
}

}  // namespace network
