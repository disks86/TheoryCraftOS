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

// A quick look through a page's HTML, before it is read properly, for the resources it will ask for: <script src>, <link rel=stylesheet href> and <img src>.
// The browser starts downloading them at once (see ResourcePrefetcher), so they arrive while the page is read and its scripts run. It only has to be right most of
// the time: a resource that is named here and never used costs a download, one that is missed is fetched when the page asks for it.
//
//   scan_resources(html, [&](ResourceKind kind, StringView reference) { ... });   // `reference` is as written in the page (resolve it against the page's address)
#pragma once
#include "Utility/StringView.hpp"

namespace browser {

enum class ResourceKind : uint8_t { Script, Style, Image };

constexpr uint32_t kMaxScannedResources = 64;

namespace scan_detail {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Skips to the end tag `</name` (any case) at or after `from`; the end of the text if there is none.
inline uint64_t after_element(StringView html, uint64_t from, StringView name) {
  for (uint64_t at = html.find('<', from); at != StringView::npos; at = html.find('<', at + 1)) {
    if (at + 1 < html.size() && html[at + 1] == '/' && at + 2 + name.size() <= html.size() && html.substr(at + 2, name.size()).equals_ignore_case(name)) return at;
  }
  return html.size();
}

struct Attributes {
  StringView src, href, rel, type;
  bool nomodule = false;
};

// Reads the attributes of the tag whose name has just been read; `i` ends after the '>' (or at the end of the text).
inline void read_attributes(StringView html, uint64_t* i, Attributes* out) {
  const uint64_t n = html.size();
  while (*i < n && html[*i] != '>') {
    while (*i < n && (is_space(html[*i]) || html[*i] == '/')) ++*i;
    if (*i >= n || html[*i] == '>') break;
    const uint64_t name_start = *i;
    while (*i < n && !is_space(html[*i]) && html[*i] != '=' && html[*i] != '>' && html[*i] != '/') ++*i;
    const StringView name = html.substr(name_start, *i - name_start);
    while (*i < n && is_space(html[*i])) ++*i;
    StringView value;
    if (*i < n && html[*i] == '=') {
      ++*i;
      while (*i < n && is_space(html[*i])) ++*i;
      if (*i < n && (html[*i] == '"' || html[*i] == '\'')) {
        const char quote = html[*i];
        ++*i;
        const uint64_t end = html.find(quote, *i);
        const uint64_t stop = end == StringView::npos ? n : end;
        value = html.substr(*i, stop - *i);
        *i = end == StringView::npos ? n : end + 1;
      } else {
        const uint64_t start = *i;
        while (*i < n && !is_space(html[*i]) && html[*i] != '>') ++*i;
        value = html.substr(start, *i - start);
      }
    }
    if (name.equals_ignore_case("src")) out->src = value.trimmed();
    else if (name.equals_ignore_case("href")) out->href = value.trimmed();
    else if (name.equals_ignore_case("rel")) out->rel = value;
    else if (name.equals_ignore_case("type")) out->type = value.trimmed();
    else if (name.equals_ignore_case("nomodule")) out->nomodule = true;
  }
  if (*i < n) ++*i;   // the '>'
}

inline bool has_token(StringView list, StringView token) {
  uint64_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && is_space(list[i])) ++i;
    const uint64_t start = i;
    while (i < list.size() && !is_space(list[i])) ++i;
    if (i > start && list.substr(start, i - start).equals_ignore_case(token)) return true;
  }
  return false;
}

}  // namespace scan_detail

template <class Visit>
void scan_resources(StringView html, Visit&& visit) {
  using namespace scan_detail;
  const uint64_t n = html.size();
  uint32_t found = 0;
  uint64_t i = 0;
  while (i < n && found < kMaxScannedResources) {
    i = html.find('<', i);
    if (i == StringView::npos) return;
    ++i;
    if (i >= n) return;
    if (html[i] == '!') {   // a comment (or a doctype): skip it
      const uint64_t end = html.substr(i).starts_with("!--") ? html.find("-->", i + 3) : html.find('>', i);
      if (end == StringView::npos) return;
      i = end + 1;
      continue;
    }
    if (!is_letter(html[i])) continue;
    const uint64_t name_start = i;
    while (i < n && !is_space(html[i]) && html[i] != '>' && html[i] != '/') ++i;
    const StringView name = html.substr(name_start, i - name_start);
    Attributes attributes;
    read_attributes(html, &i, &attributes);
    if (name.equals_ignore_case("script")) {
      const bool javascript = attributes.type.empty() || !attributes.type.equals_ignore_case("module");   // (modules are not run by the engine)
      if (!attributes.src.empty() && !attributes.nomodule && javascript) {
        visit(ResourceKind::Script, attributes.src);
        ++found;
      }
      i = after_element(html, i, "script");
    } else if (name.equals_ignore_case("link")) {
      if (!attributes.href.empty() && has_token(attributes.rel, "stylesheet")) {
        visit(ResourceKind::Style, attributes.href);
        ++found;
      }
    } else if (name.equals_ignore_case("img")) {
      if (!attributes.src.empty()) {
        visit(ResourceKind::Image, attributes.src);
        ++found;
      }
    } else if (name.equals_ignore_case("style") || name.equals_ignore_case("textarea") || name.equals_ignore_case("title")) {
      i = after_element(html, i, name);
    }
  }
}

}  // namespace browser
