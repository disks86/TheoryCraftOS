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

#include "Browser/WebUrl.hpp"
#include "ObjectContainer/List.hpp"

namespace browser {

namespace {

enum class Encode : uint8_t { C0, Fragment, Query, SpecialQuery, Path, Userinfo };

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int hex_value(char c) { return is_digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool in_set(uint8_t b, Encode set) {
  if (b < 0x20 || b > 0x7E) return true;   // (C0 controls, DEL and everything that is not ASCII)
  if (set == Encode::C0) return false;
  if (set == Encode::Fragment) return b == ' ' || b == '"' || b == '<' || b == '>' || b == '`';
  if (b == ' ' || b == '"' || b == '#' || b == '<' || b == '>') return true;
  if (set == Encode::Query) return false;
  if (set == Encode::SpecialQuery) return b == '\'';
  if (b == '?' || b == '^' || b == '`' || b == '{' || b == '}') return true;
  if (set == Encode::Path) return false;
  return b == '/' || b == ':' || b == ';' || b == '=' || b == '@' || b == '[' || b == '\\' || b == ']' || b == '|';
}

void append_encoded(StringView text, Encode set, String* out) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const char c : text) {
    const uint8_t b = static_cast<uint8_t>(c);
    if (in_set(b, set)) {
      out->push_back('%');
      out->push_back(kHex[b >> 4]);
      out->push_back(kHex[b & 15]);
    } else {
      out->push_back(c);
    }
  }
}

bool is_special_scheme(StringView s) {
  return s.equals("http") || s.equals("https") || s.equals("ws") || s.equals("wss") || s.equals("ftp") || s.equals("file");
}

int32_t special_default_port(StringView s) {
  if (s.equals("http") || s.equals("ws")) return 80;
  if (s.equals("https") || s.equals("wss")) return 443;
  if (s.equals("ftp")) return 21;
  return -1;
}

bool is_slash(char c, bool special) { return c == '/' || (special && c == '\\'); }

// --- Hosts ---------------------------------------------------------------------------------------------------------------------------------

// Punycode (RFC 3492) of one label given as code points.
bool punycode_encode(const List<uint32_t>& input, String* out) {
  constexpr uint32_t kBase = 36, kTmin = 1, kTmax = 26, kSkew = 38, kDamp = 700;
  auto digit = [](uint32_t d) { return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26)); };
  auto adapt = [&](uint32_t delta, uint32_t points, bool first) {
    delta = first ? delta / kDamp : delta / 2;
    delta += delta / points;
    uint32_t k = 0;
    while (delta > ((kBase - kTmin) * kTmax) / 2) {
      delta /= kBase - kTmin;
      k += kBase;
    }
    return k + (kBase - kTmin + 1) * delta / (delta + kSkew);
  };
  uint32_t n = 128, delta = 0, bias = 72, basic = 0;
  for (uint64_t i = 0; i < input.count(); ++i) {
    if (input[i] < 0x80) {
      out->push_back(static_cast<char>(input[i]));
      ++basic;
    }
  }
  uint32_t handled = basic;
  if (basic > 0) out->push_back('-');
  while (handled < input.count()) {
    uint32_t m = 0xFFFFFFFFu;
    for (uint64_t i = 0; i < input.count(); ++i)
      if (input[i] >= n && input[i] < m) m = input[i];
    if (m - n > (0xFFFFFFFFu - delta) / (handled + 1)) return false;
    delta += (m - n) * (handled + 1);
    n = m;
    for (uint64_t i = 0; i < input.count(); ++i) {
      if (input[i] < n && ++delta == 0) return false;
      if (input[i] != n) continue;
      uint32_t q = delta;
      for (uint32_t k = kBase;; k += kBase) {
        const uint32_t t = k <= bias ? kTmin : k >= bias + kTmax ? kTmax : k - bias;
        if (q < t) break;
        out->push_back(digit(t + (q - t) % (kBase - t)));
        q = (q - t) / (kBase - t);
      }
      out->push_back(digit(q));
      bias = adapt(delta, handled + 1, handled == basic);
      delta = 0;
      ++handled;
    }
    ++delta;
    ++n;
  }
  return true;
}

// UTF-8 to code points; false for text that is not valid UTF-8.
bool decode_utf8(StringView text, List<uint32_t>* out) {
  uint64_t i = 0;
  while (i < text.size()) {
    const uint8_t b = static_cast<uint8_t>(text[i]);
    uint32_t cp, extra;
    if (b < 0x80) { cp = b; extra = 0; }
    else if ((b & 0xE0) == 0xC0) { cp = b & 0x1Fu; extra = 1; }
    else if ((b & 0xF0) == 0xE0) { cp = b & 0x0Fu; extra = 2; }
    else if ((b & 0xF8) == 0xF0) { cp = b & 0x07u; extra = 3; }
    else return false;
    if (extra > text.size() - i - 1) return false;
    for (uint32_t k = 1; k <= extra; ++k) {
      const uint8_t cont = static_cast<uint8_t>(text[i + k]);
      if ((cont & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cont & 0x3Fu);
    }
    if (!out->push_back(cp)) return false;
    i += extra + 1;
  }
  return true;
}

// The ASCII form of a domain name: ASCII lower-cased, other labels as "xn--" punycode.
bool domain_to_ascii(allocators::IAllocator& allocator, StringView domain, String* out) {
  uint64_t start = 0;
  for (;;) {
    uint64_t end = start;
    while (end < domain.size() && domain[end] != '.') ++end;
    const StringView label = domain.substr(start, end - start);
    bool ascii = true;
    for (const char c : label) ascii = ascii && static_cast<uint8_t>(c) < 0x80;
    if (ascii) {
      for (const char c : label) out->push_back(lower(c));
    } else {
      List<uint32_t> points(allocator);
      if (!decode_utf8(label, &points)) return false;
      for (uint64_t i = 0; i < points.count(); ++i)
        if (points[i] >= 'A' && points[i] <= 'Z') points[i] += 'a' - 'A';
      out->append("xn--");
      if (!punycode_encode(points, out)) return false;
    }
    if (end >= domain.size()) return true;
    out->push_back('.');
    start = end + 1;
  }
}

// One part of an IPv4 address in decimal, 0x hex or leading-zero octal.
bool ipv4_number(StringView part, uint64_t* out) {
  if (part.empty()) return false;
  uint32_t radix = 10;
  if (part.size() >= 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X')) {
    part = part.without_prefix(2);
    radix = 16;
  } else if (part.size() >= 2 && part[0] == '0') {
    part = part.without_prefix(1);
    radix = 8;
  }
  uint64_t value = 0;
  for (const char c : part) {
    if (radix == 16 ? !is_hex(c) : !(is_digit(c) && (radix == 10 || c < '8'))) return false;
    value = value * radix + static_cast<uint64_t>(hex_value(c));
    if (value > 0xFFFFFFFFFull) return false;
  }
  *out = value;
  return true;
}

// Does the last label of the host look like a number (then the host must be an IPv4 address)?
bool ends_in_number(StringView host) {
  if (host.ends_with('.')) host = host.without_suffix(1);
  const uint64_t dot = host.rfind('.');
  const StringView last = dot == StringView::npos ? host : host.substr(dot + 1);
  if (last.empty()) return false;
  bool digits = true;
  for (const char c : last) digits = digits && is_digit(c);
  if (digits) return true;
  uint64_t ignored;
  return last.size() >= 2 && last[0] == '0' && (last[1] == 'x' || last[1] == 'X') && (last.size() == 2 || ipv4_number(last, &ignored));
}

bool parse_ipv4(StringView host, String* out) {
  uint64_t numbers[5];
  uint32_t count = 0;
  StringView rest = host, part;
  while (rest.next_token('.', &part)) {
    if (count == 5) return false;
    // (a single trailing dot gives no last empty part: next_token stops there)
    if (!ipv4_number(part, &numbers[count++])) return false;
  }
  if (count == 0 || count > 4) return false;
  for (uint32_t i = 0; i + 1 < count; ++i)
    if (numbers[i] > 255) return false;
  uint64_t limit = 1;
  for (uint32_t i = 0; i < 5 - count; ++i) limit *= 256;
  if (numbers[count - 1] >= limit) return false;
  uint64_t address = numbers[count - 1];
  uint64_t scale = 1;
  for (uint32_t i = 0; i + 1 < count; ++i) {
    scale = 1;
    for (uint32_t k = 0; k < 3 - i; ++k) scale *= 256;
    address += numbers[i] * scale;
  }
  for (int shift = 24; shift >= 0; shift -= 8) {
    out->append_uint((address >> shift) & 255);
    if (shift) out->push_back('.');
  }
  return true;
}

// [..] without the brackets: eight 16-bit pieces, "::" for a run of zeros, an IPv4 address at the end.
bool parse_ipv6(StringView text, String* out) {
  uint16_t pieces[8] = {};
  int compress = -1;
  uint32_t piece = 0;
  uint64_t i = 0;
  if (text.starts_with("::")) {
    i = 2;
    compress = 0;
    piece = 0;
    if (text.size() == 2) {
      out->append("[::]");
      return true;
    }
  } else if (text.starts_with(':')) {
    return false;
  }
  while (i < text.size()) {
    if (piece == 8) return false;
    if (text[i] == ':') {
      if (compress >= 0) return false;
      ++i;
      ++piece;
      compress = static_cast<int>(piece);
      continue;
    }
    uint32_t value = 0, length = 0;
    while (length < 4 && i < text.size() && is_hex(text[i])) {
      value = value * 16 + static_cast<uint32_t>(hex_value(text[i]));
      ++i;
      ++length;
    }
    if (i < text.size() && text[i] == '.') {
      if (length == 0 || piece > 6) return false;
      i -= length;
      uint32_t seen = 0;
      while (i < text.size()) {
        uint32_t v4 = 0;
        if (seen > 0) {
          if (text[i] == '.' && seen < 4) ++i;
          else return false;
        }
        if (i >= text.size() || !is_digit(text[i])) return false;
        while (i < text.size() && is_digit(text[i])) {
          const uint32_t d = static_cast<uint32_t>(text[i] - '0');
          v4 = v4 * 10 + d;
          if (v4 > 255) return false;
          ++i;
        }
        pieces[piece] = static_cast<uint16_t>(pieces[piece] * 256 + v4);
        ++seen;
        if (seen == 2 || seen == 4) ++piece;
      }
      if (seen != 4) return false;
      break;
    }
    if (i < text.size()) {
      if (text[i] != ':') return false;
      ++i;
      if (i >= text.size()) return false;
    }
    pieces[piece++] = static_cast<uint16_t>(value);
  }
  if (compress >= 0) {
    int swaps = static_cast<int>(piece) - compress;
    piece = 7;
    while (piece != 0 && swaps > 0) {
      const uint16_t t = pieces[compress + swaps - 1];
      pieces[compress + swaps - 1] = pieces[piece];
      pieces[piece] = t;
      --piece;
      --swaps;
    }
  } else if (piece != 8) {
    return false;
  }
  // serialize, with the longest run of zeros (at least two) as "::"
  int best = -1, best_length = 0;
  for (int s = 0; s < 8;) {
    if (pieces[s] != 0) { ++s; continue; }
    int e = s;
    while (e < 8 && pieces[e] == 0) ++e;
    if (e - s > best_length) { best = s; best_length = e - s; }
    s = e;
  }
  if (best_length < 2) best = -1;
  out->push_back('[');
  static const char kHex[] = "0123456789abcdef";
  bool ignore0 = false;
  for (int s = 0; s < 8; ++s) {
    if (ignore0 && pieces[s] == 0) continue;
    ignore0 = false;
    if (best == s) {
      out->append(s == 0 ? "::" : ":");
      ignore0 = true;
      continue;
    }
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
      const uint32_t d = (pieces[s] >> shift) & 15;
      if (d || started || shift == 0) {
        out->push_back(kHex[d]);
        started = true;
      }
    }
    if (s != 7) out->push_back(':');
  }
  out->push_back(']');
  return true;
}

// The host part of an authority, as the standard serializes it. `special`: a domain or address; otherwise an opaque host.
bool parse_host(allocators::IAllocator& allocator, StringView input, bool special, String* out) {
  if (input.starts_with('[')) {
    if (!input.ends_with(']')) return false;
    return parse_ipv6(input.substr(1, input.size() - 2), out);
  }
  if (!special) {
    for (const char c : input) {
      switch (c) {
        case '\0': case '\t': case '\n': case '\r': case ' ': case '#': case '/': case ':': case '<': case '>': case '?': case '@': case '[': case '\\': case ']': case '^': case '|': return false;
        default: break;
      }
    }
    append_encoded(input, Encode::C0, out);
    return true;
  }
  String decoded(allocator);
  if (!WebUrl::percent_decode(input, &decoded)) return false;
  String ascii(allocator);
  if (!domain_to_ascii(allocator, decoded.view(), &ascii)) return false;
  if (ascii.empty()) return false;
  for (const char c : ascii.view()) {
    const uint8_t b = static_cast<uint8_t>(c);
    if (b <= 0x20 || b == 0x7F) return false;
    switch (c) {
      case '#': case '/': case ':': case '<': case '>': case '?': case '@': case '[': case '\\': case ']': case '^': case '|': case '%': return false;
      default: break;
    }
  }
  if (ends_in_number(ascii.view())) return parse_ipv4(ascii.view(), out);
  return out->append(ascii.view());
}

// --- Paths ---------------------------------------------------------------------------------------------------------------------------------

bool is_single_dot(StringView s) { return s.equals(".") || s.equals_ignore_case("%2e"); }
bool is_double_dot(StringView s) {
  return s.equals("..") || s.equals_ignore_case(".%2e") || s.equals_ignore_case("%2e.") || s.equals_ignore_case("%2e%2e");
}

// Removes the last segment of "/a/b/c" (not a lone Windows drive letter of a file URL).
void shorten_path(const WebUrl& url, String* path) {
  if (url.is_file() && path->view().size() == 3 && is_alpha(path->view()[1]) && path->view()[2] == ':') return;
  const uint64_t slash = path->view().rfind('/');
  path->truncate(slash == StringView::npos ? 0 : slash);
}

// Adds the segments of `input` (separated by "/" and, for special URLs, "\") to url.path.
void add_path_segments(WebUrl* url, StringView input) {
  const bool special = url->is_special();
  uint64_t start = 0;
  for (;;) {
    uint64_t end = start;
    while (end < input.size() && !is_slash(input[end], special)) ++end;
    const StringView segment = input.substr(start, end - start);
    const bool last = end >= input.size();
    if (is_double_dot(segment)) {
      shorten_path(*url, &url->path);
      if (last) url->path.push_back('/');
    } else if (is_single_dot(segment)) {
      if (last) url->path.push_back('/');
    } else {
      url->path.push_back('/');
      append_encoded(segment, Encode::Path, &url->path);
    }
    if (last) return;
    start = end + 1;
  }
}

// Reads "user:pass@host:port" (the text between "//" and the next "/", "?" or "#"). `file`: no credentials or port.
bool parse_authority(allocators::IAllocator& allocator, StringView authority, bool special, bool file, WebUrl* url) {
  const uint64_t at = authority.rfind('@');
  StringView hostport = authority;
  if (at != StringView::npos) {
    const StringView userinfo = authority.first(at);
    hostport = authority.substr(at + 1);
    StringView user = userinfo, pass;
    const uint64_t colon = userinfo.find(':');
    if (colon != StringView::npos) {
      user = userinfo.first(colon);
      pass = userinfo.substr(colon + 1);
    }
    url->username.clear();
    url->password.clear();
    append_encoded(user, Encode::Userinfo, &url->username);
    append_encoded(pass, Encode::Userinfo, &url->password);
    if (hostport.empty()) return false;   // "http://user@/"
  }
  // the port: after the last ':' that is not inside [..]
  StringView host = hostport;
  StringView port_text;
  bool has_port = false;
  const uint64_t close = hostport.rfind(']');
  const uint64_t colon = hostport.rfind(':');
  if (colon != StringView::npos && (close == StringView::npos || colon > close)) {
    host = hostport.first(colon);
    port_text = hostport.substr(colon + 1);
    has_port = true;
  }
  if (host.empty()) {
    if (special && !file) return false;
    if (has_port || at != StringView::npos) return false;
  }
  url->host.clear();
  if (!host.empty() && !parse_host(allocator, host, special, &url->host)) return false;
  if (file && url->host.equals("localhost")) url->host.clear();
  if (has_port && !port_text.empty()) {
    uint32_t port = 0;
    for (const char c : port_text) {
      if (!is_digit(c)) return false;
      port = port * 10 + static_cast<uint32_t>(c - '0');
      if (port > 65535) return false;
    }
    url->port = static_cast<int32_t>(port);
    if (url->port == special_default_port(url->scheme.view())) url->port = -1;
  }
  url->has_host = true;
  return true;
}

// The text after the scheme's colon (and slashes), up to the first of / ? # (\ too for special URLs).
uint64_t authority_end(StringView text, bool special) {
  uint64_t i = 0;
  while (i < text.size() && !is_slash(text[i], special) && text[i] != '?' && text[i] != '#') ++i;
  return i;
}

// Splits "path?query#fragment" and stores each part.
void parse_rest(WebUrl* url, StringView rest, bool path_from_base, bool opaque) {
  const bool special = url->is_special();
  const uint64_t hash = rest.find('#');
  StringView before = hash == StringView::npos ? rest : rest.first(hash);
  if (hash != StringView::npos) {
    url->has_fragment = true;
    url->fragment.clear();
    append_encoded(rest.substr(hash + 1), Encode::Fragment, &url->fragment);
  }
  const uint64_t question = before.find('?');
  StringView path_text = question == StringView::npos ? before : before.first(question);
  if (question != StringView::npos) {
    url->has_query = true;
    url->query.clear();
    append_encoded(before.substr(question + 1), special ? Encode::SpecialQuery : Encode::Query, &url->query);
  }
  if (path_from_base) return;
  if (opaque) {
    url->path.clear();
    append_encoded(path_text, Encode::C0, &url->path);
    return;
  }
  url->path.clear();
  if (path_text.empty()) {
    if (special) url->path.assign("/");
    return;
  }
  // path_text starts with a slash for absolute paths; "a/b" for relative ones was joined with the base before this
  add_path_segments(url, is_slash(path_text[0], special) ? path_text.without_prefix(1) : path_text);
}

bool drive_letter(StringView s) { return s.size() >= 2 && is_alpha(s[0]) && (s[1] == ':' || s[1] == '|') && (s.size() == 2 || s[2] == '/' || s[2] == '\\' || s[2] == '?' || s[2] == '#'); }

}  // namespace

// --- WebUrl ----------------------------------------------------------------------------------------------------------------------------

bool WebUrl::is_special() const { return is_special_scheme(scheme.view()); }
uint16_t WebUrl::default_port() const {
  const int32_t p = special_default_port(scheme.view());
  return p < 0 ? 0 : static_cast<uint16_t>(p);
}

bool WebUrl::percent_decode(StringView text, String* out) {
  for (uint64_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && is_hex(text[i + 1]) && is_hex(text[i + 2])) {
      if (!out->push_back(static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2])))) return false;
      i += 2;
    } else if (!out->push_back(text[i])) {
      return false;
    }
  }
  return true;
}

void WebUrl::form_encode(StringView text, String* out) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const char c : text) {
    const uint8_t b = static_cast<uint8_t>(c);
    if (is_alpha(c) || is_digit(c) || c == '*' || c == '-' || c == '.' || c == '_') out->push_back(c);
    else if (c == ' ') out->push_back('+');
    else {
      out->push_back('%');
      out->push_back(kHex[b >> 4]);
      out->push_back(kHex[b & 15]);
    }
  }
}

bool WebUrl::form_decode(StringView text, String* out) {
  for (uint64_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+') out->push_back(' ');
    else if (text[i] == '%' && i + 2 < text.size() && is_hex(text[i + 1]) && is_hex(text[i + 2])) {
      out->push_back(static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2])));
      i += 2;
    } else {
      out->push_back(text[i]);
    }
  }
  return true;
}

bool WebUrl::can_parse(allocators::IAllocator& allocator, StringView input, const WebUrl* base) {
  WebUrl scratch(allocator);
  return parse(allocator, input, base, &scratch);
}

bool WebUrl::parse(allocators::IAllocator& allocator, StringView input, const WebUrl* base, WebUrl* out) {
  // leading and trailing C0 controls and spaces go, and tabs and newlines everywhere
  String cleaned(allocator);
  uint64_t a = 0, b = input.size();
  while (a < b && static_cast<uint8_t>(input[a]) <= 0x20) ++a;
  while (b > a && static_cast<uint8_t>(input[b - 1]) <= 0x20) --b;
  for (uint64_t i = a; i < b; ++i)
    if (input[i] != '\t' && input[i] != '\n' && input[i] != '\r') cleaned.push_back(input[i]);
  const StringView text = cleaned.view();

  WebUrl url(allocator);
  // a scheme?
  uint64_t colon = 0;
  bool has_scheme = false;
  if (!text.empty() && is_alpha(text[0])) {
    colon = 1;
    while (colon < text.size() && (is_alpha(text[colon]) || is_digit(text[colon]) || text[colon] == '+' || text[colon] == '-' || text[colon] == '.')) ++colon;
    has_scheme = colon < text.size() && text[colon] == ':';
  }
  StringView rest;
  bool relative = false;
  if (has_scheme) {
    for (const char c : text.first(colon)) url.scheme.push_back(lower(c));
    rest = text.substr(colon + 1);
    if (base && base->scheme.equals(url.scheme.view()) && url.is_special() && !(!rest.empty() && is_slash(rest[0], true))) relative = true;
  } else {
    if (!base) return false;
    if (base->opaque_path) {
      if (text.empty() || text[0] != '#') return false;
      url = WebUrl(allocator);
      url.scheme.assign(base->scheme.view());
      url.opaque_path = true;
      url.path.assign(base->path.view());
      url.has_query = base->has_query;
      url.query.assign(base->query.view());
      url.has_fragment = true;
      append_encoded(text.substr(1), Encode::Fragment, &url.fragment);
      *out = static_cast<WebUrl&&>(url);
      return true;
    }
    url.scheme.assign(base->scheme.view());
    rest = text;
    relative = true;
  }
  const bool special = url.is_special();

  if (relative && base) {
    // copy the base, then change what the reference says
    url.username.assign(base->username.view());
    url.password.assign(base->password.view());
    url.has_host = base->has_host;
    url.host.assign(base->host.view());
    url.port = base->port;
    if (!rest.empty() && is_slash(rest[0], special) && rest.size() >= 2 && is_slash(rest[1], special)) {   // "//host/path"
      url.username.clear();
      url.password.clear();
      url.port = -1;
      const StringView after = rest.substr(2);
      const uint64_t end = authority_end(after, special);
      if (!parse_authority(allocator, after.first(end), special, url.is_file(), &url)) return false;
      parse_rest(&url, after.substr(end), false, false);
    } else if (!rest.empty() && is_slash(rest[0], special)) {   // "/path"
      parse_rest(&url, rest, false, false);
    } else if (rest.empty() || rest[0] == '?' || rest[0] == '#') {
      url.path.assign(base->path.view());
      url.opaque_path = false;
      if (rest.empty() || rest[0] == '#') {
        url.has_query = base->has_query;
        url.query.assign(base->query.view());
      }
      parse_rest(&url, rest, true, false);
    } else {   // "a/b": relative to the base's directory
      url.path.assign(base->path.view());
      shorten_path(url, &url.path);
      // path text up to ? or #, then query and fragment
      const uint64_t hash = rest.find('#');
      StringView before = hash == StringView::npos ? rest : rest.first(hash);
      if (hash != StringView::npos) {
        url.has_fragment = true;
        append_encoded(rest.substr(hash + 1), Encode::Fragment, &url.fragment);
      }
      const uint64_t question = before.find('?');
      if (question != StringView::npos) {
        url.has_query = true;
        append_encoded(before.substr(question + 1), special ? Encode::SpecialQuery : Encode::Query, &url.query);
        before = before.first(question);
      }
      add_path_segments(&url, before);
    }
    if (special && url.path.empty()) url.path.assign("/");
    *out = static_cast<WebUrl&&>(url);
    return true;
  }

  // an absolute URL with a scheme
  if (url.is_file()) {
    url.has_host = true;
    if (!rest.empty() && is_slash(rest[0], true) && rest.size() >= 2 && is_slash(rest[1], true)) {   // file://host/path
      const StringView after = rest.substr(2);
      const uint64_t end = authority_end(after, true);
      StringView host_text = after.first(end);
      if (drive_letter(host_text)) {   // "file://C:/x": the drive is part of the path
        parse_rest(&url, rest.substr(1), false, false);
      } else {
        if (!parse_authority(allocator, host_text, true, true, &url)) return false;
        parse_rest(&url, after.substr(end), false, false);
      }
    } else {   // "file:/path", or "file:path" with no file URL to be relative to
      String slashed(allocator);
      if (rest.empty() || !is_slash(rest[0], true)) slashed.push_back('/');
      slashed.append(rest);
      parse_rest(&url, slashed.view(), false, false);
    }
    if (url.path.empty()) url.path.assign("/");
    *out = static_cast<WebUrl&&>(url);
    return true;
  }
  if (special) {
    uint64_t skip = 0;
    while (skip < rest.size() && is_slash(rest[skip], true)) ++skip;
    const StringView after = rest.substr(skip);
    const uint64_t end = authority_end(after, true);
    if (!parse_authority(allocator, after.first(end), true, false, &url)) return false;
    parse_rest(&url, after.substr(end), false, false);
    if (url.path.empty()) url.path.assign("/");
    *out = static_cast<WebUrl&&>(url);
    return true;
  }
  // other schemes
  if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
    const StringView after = rest.substr(2);
    const uint64_t end = authority_end(after, false);
    if (!parse_authority(allocator, after.first(end), false, false, &url)) return false;
    parse_rest(&url, after.substr(end), false, false);
  } else if (!rest.empty() && rest[0] == '/') {
    parse_rest(&url, rest, false, false);
  } else {
    url.opaque_path = true;
    parse_rest(&url, rest, false, true);
  }
  *out = static_cast<WebUrl&&>(url);
  return true;
}

void WebUrl::serialize(String* out, bool with_fragment) const {
  out->append(scheme.view());
  out->push_back(':');
  if (has_host) {
    out->append("//");
    if (!username.empty() || !password.empty()) {
      out->append(username.view());
      if (!password.empty()) {
        out->push_back(':');
        out->append(password.view());
      }
      out->push_back('@');
    }
    out->append(host.view());
    if (port >= 0) {
      out->push_back(':');
      out->append_uint(static_cast<uint32_t>(port));
    }
  } else if (!opaque_path && path.view().starts_with("//")) {
    out->append("/.");
  }
  out->append(path.view());
  if (has_query) {
    out->push_back('?');
    out->append(query.view());
  }
  if (with_fragment && has_fragment) {
    out->push_back('#');
    out->append(fragment.view());
  }
}

void WebUrl::origin(String* out) const {
  if (is_special() && !is_file()) {
    out->append(scheme.view());
    out->append("://");
    out->append(host.view());
    if (port >= 0) {
      out->push_back(':');
      out->append_uint(static_cast<uint32_t>(port));
    }
    return;
  }
  if (scheme.equals("blob") && opaque_path) {   // blob:https://example.com/uuid has the origin of the URL inside it
    WebUrl inner(*allocator_);
    if (parse(*allocator_, path.view(), nullptr, &inner) && (inner.scheme.equals("http") || inner.scheme.equals("https"))) {
      inner.origin(out);
      return;
    }
  }
  out->append("null");
}

bool WebUrl::same_origin(const WebUrl& other) const {
  if (!is_special() || is_file() || !other.is_special() || other.is_file()) return false;
  return scheme.view().equals(other.scheme.view()) && host.view().equals(other.host.view()) && port == other.port;
}

void WebUrl::get(Part part, String* out) const {
  switch (part) {
    case Part::Href: serialize(out); break;
    case Part::Protocol:
      out->append(scheme.view());
      out->push_back(':');
      break;
    case Part::Username: out->append(username.view()); break;
    case Part::Password: out->append(password.view()); break;
    case Part::Host:
      out->append(host.view());
      if (port >= 0) {
        out->push_back(':');
        out->append_uint(static_cast<uint32_t>(port));
      }
      break;
    case Part::Hostname: out->append(host.view()); break;
    case Part::Port:
      if (port >= 0) out->append_uint(static_cast<uint32_t>(port));
      break;
    case Part::Pathname: out->append(path.view()); break;
    case Part::Search:
      if (has_query && !query.empty()) {
        out->push_back('?');
        out->append(query.view());
      }
      break;
    case Part::Hash:
      if (has_fragment && !fragment.empty()) {
        out->push_back('#');
        out->append(fragment.view());
      }
      break;
  }
}

bool WebUrl::set(Part part, StringView value) {
  allocators::IAllocator& allocator = *allocator_;
  const bool special = is_special();
  switch (part) {
    case Part::Href: {
      WebUrl fresh(allocator);
      if (!parse(allocator, value, nullptr, &fresh)) return false;
      *this = static_cast<WebUrl&&>(fresh);
      return true;
    }
    case Part::Protocol: {
      const uint64_t colon = value.find(':');
      const StringView name = colon == StringView::npos ? value : value.first(colon);
      if (name.empty() || !is_alpha(name[0])) return false;
      String fresh(allocator);
      for (const char c : name) {
        if (!(is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.')) return false;
        fresh.push_back(lower(c));
      }
      if (is_special_scheme(fresh.view()) != special) return false;
      if (fresh.equals("file") && (!username.empty() || !password.empty() || port >= 0)) return false;
      if (is_file() && host.empty()) return false;
      scheme = static_cast<String&&>(fresh);
      if (port == special_default_port(scheme.view())) port = -1;
      return true;
    }
    case Part::Username:
    case Part::Password: {
      if (!has_host || host.empty() || is_file()) return false;
      String& target = part == Part::Username ? username : password;
      target.clear();
      append_encoded(value, Encode::Userinfo, &target);
      return true;
    }
    case Part::Host:
    case Part::Hostname: {
      if (opaque_path) return false;
      StringView text = value.first(authority_end(value, special));
      StringView port_text;
      bool has_port = false;
      const uint64_t close = text.rfind(']');
      const uint64_t colon = text.rfind(':');
      if (colon != StringView::npos && (close == StringView::npos || colon > close)) {
        if (part == Part::Hostname) return false;
        port_text = text.substr(colon + 1);
        text = text.first(colon);
        has_port = true;
      }
      String fresh(allocator);
      if (text.empty()) {
        if (special) return false;
        if (has_port) return false;
      } else if (!parse_host(allocator, text, special, &fresh)) {
        return false;
      }
      if (is_file() && fresh.equals("localhost")) fresh.clear();
      host = static_cast<String&&>(fresh);
      has_host = true;
      if (has_port && !port_text.empty()) {
        uint32_t number = 0;
        uint64_t used = 0;
        while (used < port_text.size() && is_digit(port_text[used]) && number <= 65535) number = number * 10 + static_cast<uint32_t>(port_text[used++] - '0');
        if (used > 0 && number <= 65535) port = static_cast<int32_t>(number) == special_default_port(scheme.view()) ? -1 : static_cast<int32_t>(number);
      }
      return true;
    }
    case Part::Port: {
      if (!has_host || host.empty() || is_file()) return false;
      if (value.empty()) {
        port = -1;
        return true;
      }
      uint32_t number = 0;
      uint64_t used = 0;
      while (used < value.size() && is_digit(value[used])) {
        number = number * 10 + static_cast<uint32_t>(value[used++] - '0');
        if (number > 65535) return false;
      }
      if (used == 0) return false;
      port = static_cast<int32_t>(number) == special_default_port(scheme.view()) ? -1 : static_cast<int32_t>(number);
      return true;
    }
    case Part::Pathname: {
      if (opaque_path) return false;
      path.clear();
      add_path_segments(this, !value.empty() && is_slash(value[0], special) ? value.without_prefix(1) : value);
      return true;
    }
    case Part::Search: {
      query.clear();
      if (value.empty()) {
        has_query = false;
        return true;
      }
      has_query = true;
      append_encoded(value.starts_with('?') ? value.without_prefix(1) : value, special ? Encode::SpecialQuery : Encode::Query, &query);
      return true;
    }
    case Part::Hash: {
      fragment.clear();
      if (value.empty()) {
        has_fragment = false;
        return true;
      }
      has_fragment = true;
      append_encoded(value.starts_with('#') ? value.without_prefix(1) : value, Encode::Fragment, &fragment);
      return true;
    }
  }
  return false;
}

}  // namespace browser
