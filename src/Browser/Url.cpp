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

#include "Browser/Url.hpp"
#include "Network/Http/HttpUrl.hpp"

namespace browser {

namespace {

bool starts_with_ignore_case(StringView text, const char* prefix) {
  const StringView p(prefix);
  return text.size() >= p.size() && text.substr(0, p.size()).equals_ignore_case(p);
}

// Does the text start with a scheme ("mailto:", "http:")?
bool has_scheme(StringView text) {
  if (text.empty() || !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z'))) return false;
  for (uint64_t i = 1; i < text.size(); ++i) {
    const char c = text[i];
    if (c == ':') return true;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return false;
}

StringView without_fragment(StringView text) {
  const uint64_t hash = text.find('#');
  return hash == StringView::npos ? text : text.first(hash);
}

// A path with its "." and ".." parts worked out.
bool clean_path(StringView path, String* out) {
  out->clear();
  uint64_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    uint64_t end = i;
    while (end < path.size() && path[end] != '/') ++end;
    const StringView part = path.substr(i, end - i);
    if (part.empty() || part.equals(".")) {
      // nothing
    } else if (part.equals("..")) {
      const uint64_t slash = out->view().rfind('/');
      if (slash != StringView::npos) out->truncate(slash);
    } else if (!out->push_back('/') || !out->append(part)) {
      return false;
    }
    i = end;
  }
  if (out->empty()) return out->push_back('/');
  if (path.ends_with('/') && !out->view().ends_with('/')) return out->push_back('/');
  return true;
}

}  // namespace

bool is_web_address(StringView address) { return starts_with_ignore_case(address, "http://") || starts_with_ignore_case(address, "https://"); }

bool is_file_address(StringView address) { return address.starts_with('/') || starts_with_ignore_case(address, "file://"); }

bool normalize_address(StringView typed, String* out) {
  const StringView text = typed.trimmed();
  if (text.empty()) return false;
  if (starts_with_ignore_case(text, "file://")) return out->assign(text.substr(7));
  if (text.starts_with('/') || has_scheme(text)) return out->assign(text);
  out->clear();
  return out->append("http://") && out->append(text);
}

bool resolve_reference(allocators::IAllocator& allocator, StringView base, StringView reference, String* out) {
  const StringView ref = reference.trimmed();
  if (ref.empty()) return false;
  if (ref.starts_with('#')) return out->assign(base);
  if (starts_with_ignore_case(ref, "file://")) return out->assign(without_fragment(ref.substr(7)));
  if (is_web_address(ref)) return out->assign(without_fragment(ref));
  if (has_scheme(ref)) return false;
  const StringView target = without_fragment(ref);
  if (is_web_address(base)) {
    network::HttpUrl page(allocator), resolved(allocator);
    if (!network::parse_http_url(base, &page) || !network::resolve_http_reference(page, target, &resolved)) return false;
    return resolved.assign_text(out);
  }
  // A file: the reference is a path, from the root or from the folder the page is in (a query makes no sense for a file).
  const uint64_t query = target.find('?');
  const StringView path = query == StringView::npos ? target : target.first(query);
  String joined(allocator);
  if (path.starts_with('/')) {
    joined.assign(path);
  } else {
    const uint64_t slash = base.rfind('/');
    joined.assign(slash == StringView::npos ? StringView("/") : base.first(slash + 1));
    joined.append(path);
  }
  return clean_path(joined.view(), out);
}

StringView address_name(StringView address) {
  StringView text = without_fragment(address);
  if (is_web_address(text)) {
    text = text.substr(text.find("//") + 2);
    const uint64_t slash = text.find('/');
    const StringView host = slash == StringView::npos ? text : text.first(slash);
    const bool bare = slash == StringView::npos || slash + 1 == text.size();
    if (bare) return host;
  }
  while (text.size() > 1 && text.ends_with('/')) text = text.without_suffix(1);
  const uint64_t slash = text.rfind('/');
  return slash == StringView::npos || slash + 1 >= text.size() ? text : text.substr(slash + 1);
}

}  // namespace browser
