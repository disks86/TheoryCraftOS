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
#include "Media/Dash.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {

constexpr uint64_t kMaxXmlBytes = 16u << 20;
constexpr uint32_t kMaxXmlDepth = 64;
constexpr uint32_t kMaxXmlNodes = 200000;
constexpr uint64_t kMaxSegments = 200000;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_name_char(char c) { return !(is_space(c) || c == '=' || c == '>' || c == '/' || c == '<' || c == '"' || c == '\'' || c == '?' || c == '!'); }

bool append_utf8(String* out, uint32_t cp) {
  char b[4];
  uint32_t n;
  if (cp < 0x80) { b[0] = static_cast<char>(cp); n = 1; }
  else if (cp < 0x800) { b[0] = static_cast<char>(0xC0 | (cp >> 6)); b[1] = static_cast<char>(0x80 | (cp & 0x3F)); n = 2; }
  else if (cp < 0x10000) { b[0] = static_cast<char>(0xE0 | (cp >> 12)); b[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); b[2] = static_cast<char>(0x80 | (cp & 0x3F)); n = 3; }
  else { b[0] = static_cast<char>(0xF0 | (cp >> 18)); b[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); b[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); b[3] = static_cast<char>(0x80 | (cp & 0x3F)); n = 4; }
  return out->append(StringView(b, n));
}

// Appends `text` with the entities decoded.
bool append_decoded(String* out, StringView text) {
  uint64_t i = 0;
  while (i < text.size()) {
    if (text[i] != '&') {
      uint64_t e = i;
      while (e < text.size() && text[e] != '&') ++e;
      if (!out->append(text.substr(i, e - i))) return false;
      i = e;
      continue;
    }
    const uint64_t semi = text.find(';', i);
    if (semi == StringView::npos || semi - i > 10) {
      if (!out->push_back('&')) return false;
      ++i;
      continue;
    }
    const StringView name = text.substr(i + 1, semi - i - 1);
    uint32_t cp = 0;
    if (name.equals("amp")) cp = '&';
    else if (name.equals("lt")) cp = '<';
    else if (name.equals("gt")) cp = '>';
    else if (name.equals("quot")) cp = '"';
    else if (name.equals("apos")) cp = '\'';
    else if (name.size() >= 2 && name[0] == '#') {
      const bool hex = name[1] == 'x' || name[1] == 'X';
      StringView digits = name.without_prefix(hex ? 2 : 1);
      uint64_t v = 0;
      bool ok = !digits.empty();
      for (char c : digits) {
        uint32_t d;
        if (is_digit(c)) d = static_cast<uint32_t>(c - '0');
        else if (hex && c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else { ok = false; break; }
        v = v * (hex ? 16 : 10) + d;
        if (v > 0x10FFFF) { ok = false; break; }
      }
      if (ok && v != 0) cp = static_cast<uint32_t>(v);
    }
    if (cp == 0) {
      if (!out->push_back('&')) return false;
      ++i;
      continue;
    }
    if (!append_utf8(out, cp)) return false;
    i = semi + 1;
  }
  return true;
}

StringView local_name(StringView name) {
  const uint64_t colon = name.rfind(':');
  return colon == StringView::npos ? name : name.substr(colon + 1);
}

// --- numbers

bool parse_uint(StringView s, uint64_t* out) {
  s = s.trimmed();
  if (s.empty() || s.size() > 19) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (!is_digit(c)) return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  *out = v;
  return true;
}

bool parse_int(StringView s, int64_t* out) {
  s = s.trimmed();
  bool neg = false;
  if (s.starts_with('-')) {
    neg = true;
    s = s.without_prefix(1);
  }
  uint64_t v;
  if (!parse_uint(s, &v) || v > static_cast<uint64_t>(INT64_MAX)) return false;
  *out = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
  return true;
}

bool parse_decimal(StringView s, double* out) {
  s = s.trimmed();
  if (s.empty()) return false;
  double v = 0;
  uint64_t i = 0;
  bool any = false;
  while (i < s.size() && is_digit(s[i])) {
    v = v * 10 + (s[i] - '0');
    ++i;
    any = true;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    double scale = 0.1;
    while (i < s.size() && is_digit(s[i])) {
      v += (s[i] - '0') * scale;
      scale /= 10;
      ++i;
      any = true;
    }
  }
  if (!any || i != s.size()) return false;
  *out = v;
  return true;
}

bool parse_range(StringView s, DashRange* out) {
  s = s.trimmed();
  const uint64_t dash = s.find('-');
  if (dash == StringView::npos) return false;
  uint64_t a, b;
  if (!parse_uint(s.substr(0, dash), &a) || !parse_uint(s.substr(dash + 1), &b) || b < a) return false;
  out->present = true;
  out->first = a;
  out->last = b;
  return true;
}

int64_t scale_us(uint64_t value, uint64_t timescale) {
  if (timescale == 0) timescale = 1;
  // value * 1e6 / timescale without overflowing for large values
  const uint64_t whole = value / timescale, rest = value % timescale;
  const uint64_t cap = static_cast<uint64_t>(INT64_MAX / 2000000);
  if (whole > cap) return INT64_MAX / 2;
  return static_cast<int64_t>(whole * 1000000 + rest * 1000000 / timescale);
}

int64_t scale_us_signed(int64_t value, uint64_t timescale) { return value < 0 ? -scale_us(static_cast<uint64_t>(-value), timescale) : scale_us(static_cast<uint64_t>(value), timescale); }

// --- XML reading

class XmlReader {
 public:
  XmlReader(allocators::IAllocator& a, StringView text) : allocator_(a), s_(text) {}

  UniquePointer<XmlNode> run() {
    if (s_.size() > kMaxXmlBytes) return UniquePointer<XmlNode>();
    UniquePointer<XmlNode> root;
    XmlNode* stack[kMaxXmlDepth];
    uint32_t depth = 0;
    uint32_t nodes = 0;
    if (s_.starts_with("\xEF\xBB\xBF")) i_ = 3;
    while (i_ < s_.size()) {
      if (s_[i_] != '<') {   // character data
        uint64_t e = i_;
        while (e < s_.size() && s_[e] != '<') ++e;
        const StringView text = s_.substr(i_, e - i_);
        i_ = e;
        if (depth > 0) {
          if (!append_decoded(&stack[depth - 1]->text, text)) return UniquePointer<XmlNode>();
        } else if (!text.trimmed().empty()) {
          return UniquePointer<XmlNode>();
        }
        continue;
      }
      if (s_.substr(i_).starts_with("<!--")) {
        const uint64_t e = s_.find("-->", i_ + 4);
        if (e == StringView::npos) return UniquePointer<XmlNode>();
        i_ = e + 3;
      } else if (s_.substr(i_).starts_with("<![CDATA[")) {
        const uint64_t e = s_.find("]]>", i_ + 9);
        if (e == StringView::npos || depth == 0) return UniquePointer<XmlNode>();
        if (!stack[depth - 1]->text.append(s_.substr(i_ + 9, e - i_ - 9))) return UniquePointer<XmlNode>();
        i_ = e + 3;
      } else if (s_.substr(i_).starts_with("<?")) {
        const uint64_t e = s_.find("?>", i_ + 2);
        if (e == StringView::npos) return UniquePointer<XmlNode>();
        i_ = e + 2;
      } else if (s_.substr(i_).starts_with("<!")) {   // DOCTYPE (without an internal subset worth reading)
        uint64_t e = i_ + 2;
        uint32_t nest = 1;
        while (e < s_.size() && nest > 0) {
          if (s_[e] == '<') ++nest;
          else if (s_[e] == '>') --nest;
          ++e;
        }
        if (nest != 0) return UniquePointer<XmlNode>();
        i_ = e;
      } else if (s_.substr(i_).starts_with("</")) {
        if (depth == 0) return UniquePointer<XmlNode>();
        const uint64_t e = s_.find('>', i_);
        if (e == StringView::npos) return UniquePointer<XmlNode>();
        const StringView name = local_name(s_.substr(i_ + 2, e - i_ - 2).trimmed());
        if (!stack[depth - 1]->name.view().equals(name)) return UniquePointer<XmlNode>();
        --depth;
        i_ = e + 1;
        if (depth == 0) {
          while (i_ < s_.size() && (is_space(s_[i_]))) ++i_;
          if (i_ < s_.size() && s_[i_] == '<' && !s_.substr(i_).starts_with("<!--") && !s_.substr(i_).starts_with("<?")) return UniquePointer<XmlNode>();
        }
      } else {
        if (++nodes > kMaxXmlNodes || depth >= kMaxXmlDepth) return UniquePointer<XmlNode>();
        auto node = UniquePointer<XmlNode>::make(allocator_, allocator_);
        if (!node) return UniquePointer<XmlNode>();
        bool empty = false;
        if (!read_tag(node.get(), &empty)) return UniquePointer<XmlNode>();
        XmlNode* raw = node.get();
        if (depth == 0) {
          if (root) return UniquePointer<XmlNode>();
          root = util::move(node);
        } else if (!stack[depth - 1]->children.push_back(util::move(node))) {
          return UniquePointer<XmlNode>();
        }
        if (!empty) stack[depth++] = raw;
      }
    }
    if (depth != 0) return UniquePointer<XmlNode>();
    return root;
  }

 private:
  void skip_space() {
    while (i_ < s_.size() && is_space(s_[i_])) ++i_;
  }
  bool read_tag(XmlNode* node, bool* empty) {
    ++i_;   // <
    uint64_t e = i_;
    while (e < s_.size() && is_name_char(s_[e])) ++e;
    if (e == i_) return false;
    if (!node->name.assign(local_name(s_.substr(i_, e - i_)))) return false;
    i_ = e;
    for (;;) {
      skip_space();
      if (i_ >= s_.size()) return false;
      if (s_[i_] == '>') {
        ++i_;
        *empty = false;
        return true;
      }
      if (s_[i_] == '/') {
        if (i_ + 1 >= s_.size() || s_[i_ + 1] != '>') return false;
        i_ += 2;
        *empty = true;
        return true;
      }
      uint64_t n = i_;
      while (n < s_.size() && is_name_char(s_[n])) ++n;
      if (n == i_) return false;
      auto attribute = UniquePointer<XmlAttribute>::make(allocator_, allocator_);
      if (!attribute || !attribute->name.assign(local_name(s_.substr(i_, n - i_)))) return false;
      i_ = n;
      skip_space();
      if (i_ >= s_.size() || s_[i_] != '=') return false;
      ++i_;
      skip_space();
      if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) return false;
      const char quote = s_[i_++];
      const uint64_t close = s_.find(quote, i_);
      if (close == StringView::npos) return false;
      if (!append_decoded(&attribute->value, s_.substr(i_, close - i_))) return false;
      i_ = close + 1;
      if (!node->attributes.push_back(util::move(attribute))) return false;
    }
  }

  allocators::IAllocator& allocator_;
  StringView s_;
  uint64_t i_ = 0;
};

// --- MPD reading

struct Chain {   // Period, AdaptationSet, Representation elements, outermost first
  const XmlNode* node[3] = {nullptr, nullptr, nullptr};
};

void read_timeline(const XmlNode* st, bool* has, List<DashTimelineEntry>* out) {
  const XmlNode* timeline = st->child("SegmentTimeline");
  if (!timeline) return;
  *has = true;
  out->clear();
  for (const auto& s : timeline->children.view()) {
    if (!s->name.view().equals("S")) continue;
    DashTimelineEntry e;
    uint64_t v;
    int64_t r = 0;
    if (!parse_uint(s->attribute("d"), &e.duration) || e.duration == 0) continue;
    if (parse_uint(s->attribute("t"), &v)) e.time = v;
    else e.time = UINT64_MAX;   // continues from the previous entry
    if (parse_int(s->attribute("r"), &r)) {
      if (r < 0) e.open_ended = true;
      else e.repeat = r > 100000 ? 100000 : static_cast<uint32_t>(r);
    }
    out->push_back(e);
  }
}

void read_template(const XmlNode* st, DashTemplate* t) {
  t->present = true;
  uint64_t v;
  int64_t sv;
  if (st->has_attribute("media")) t->media.assign(st->attribute("media"));
  if (st->has_attribute("initialization")) t->initialization.assign(st->attribute("initialization"));
  if (parse_uint(st->attribute("timescale"), &v) && v > 0) t->timescale = v;
  if (parse_uint(st->attribute("duration"), &v)) t->duration = v;
  if (parse_uint(st->attribute("startNumber"), &v)) t->start_number = v;
  if (parse_int(st->attribute("presentationTimeOffset"), &sv)) t->presentation_time_offset = sv;
  read_timeline(st, &t->has_timeline, &t->timeline);
}

void read_list(allocators::IAllocator& a, const XmlNode* sl, DashList* l) {
  l->present = true;
  uint64_t v;
  int64_t sv;
  if (parse_uint(sl->attribute("timescale"), &v) && v > 0) l->timescale = v;
  if (parse_uint(sl->attribute("duration"), &v)) l->duration = v;
  if (parse_uint(sl->attribute("startNumber"), &v)) l->start_number = v;
  if (parse_int(sl->attribute("presentationTimeOffset"), &sv)) l->presentation_time_offset = sv;
  read_timeline(sl, &l->has_timeline, &l->timeline);
  if (const XmlNode* init = sl->child("Initialization")) {
    l->initialization.assign(init->attribute("sourceURL"));
    parse_range(init->attribute("range"), &l->initialization_range);
  }
  bool any = false;
  for (const auto& c : sl->children.view()) {
    if (!c->name.view().equals("SegmentURL")) continue;
    if (!any) l->items.clear();
    any = true;
    auto item = UniquePointer<DashListItem>::make(a, a);
    if (!item) return;
    item->media.assign(c->attribute("media"));
    parse_range(c->attribute("mediaRange"), &item->range);
    l->items.push_back(util::move(item));
  }
}

void read_base(const XmlNode* sb, DashBase* b) {
  b->present = true;
  uint64_t v;
  int64_t sv;
  if (parse_uint(sb->attribute("timescale"), &v) && v > 0) b->timescale = v;
  if (parse_int(sb->attribute("presentationTimeOffset"), &sv)) b->presentation_time_offset = sv;
  parse_range(sb->attribute("indexRange"), &b->index_range);
  if (const XmlNode* init = sb->child("Initialization")) parse_range(init->attribute("range"), &b->initialization_range);
}

// The attribute of the chain's elements that is closest to the representation.
StringView chain_attribute(const Chain& c, StringView name) {
  for (int i = 2; i >= 0; --i)
    if (c.node[i] && c.node[i]->has_attribute(name)) return c.node[i]->attribute(name);
  return StringView();
}

// RFC 3986 reference resolution (what the addresses of a manifest need: absolute, //authority, /path, relative path, ?query).
bool resolve(allocators::IAllocator& a, StringView base, StringView reference, String* out) {
  if (reference.empty()) return out->assign(base);
  auto has_scheme = [](StringView s) {
    for (uint64_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (c == ':') return i > 0;
      const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      if (!(alpha || (i > 0 && (is_digit(c) || c == '+' || c == '-' || c == '.')))) return false;
    }
    return false;
  };
  if (has_scheme(reference)) return out->assign(reference);
  // the parts of the base
  uint64_t scheme_end = base.find(':');
  if (scheme_end == StringView::npos) return out->assign(reference);
  const StringView scheme = base.substr(0, scheme_end + 1);
  StringView rest = base.substr(scheme_end + 1);
  StringView authority;
  if (rest.starts_with("//")) {
    uint64_t e = 2;
    while (e < rest.size() && rest[e] != '/' && rest[e] != '?' && rest[e] != '#') ++e;
    authority = rest.substr(0, e);
    rest = rest.substr(e);
  }
  uint64_t cut = rest.find('#');
  if (cut != StringView::npos) rest = rest.substr(0, cut);
  StringView base_query;
  cut = rest.find('?');
  StringView base_path = rest;
  if (cut != StringView::npos) {
    base_path = rest.substr(0, cut);
    base_query = rest.substr(cut);
  }
  if (reference.starts_with("//")) {
    String r(a);
    return r.append(scheme) && r.append(reference) && out->assign(r.view());
  }
  String path(a);
  StringView query;
  if (reference.starts_with('#')) {
    return out->assign(scheme) && out->append(authority) && out->append(base_path) && out->append(base_query) && out->append(reference);
  }
  if (reference.starts_with('?')) {
    return out->assign(scheme) && out->append(authority) && out->append(base_path) && out->append(reference);
  }
  StringView ref_path = reference;
  cut = reference.find('?');
  const uint64_t hash = reference.find('#');
  StringView tail;
  if (cut != StringView::npos || hash != StringView::npos) {
    const uint64_t t = cut != StringView::npos && (hash == StringView::npos || cut < hash) ? cut : hash;
    ref_path = reference.substr(0, t);
    tail = reference.substr(t);
  }
  String merged(a);
  if (ref_path.starts_with('/')) {
    if (!merged.assign(ref_path)) return false;
  } else {
    const uint64_t slash = base_path.rfind('/');
    if (!merged.assign(slash == StringView::npos ? StringView(authority.empty() ? "" : "/") : base_path.substr(0, slash + 1)) || !merged.append(ref_path)) return false;
  }
  // remove . and .. segments
  String clean(a);
  StringView m = merged.view();
  List<StringView> segs(a);
  const bool absolute = m.starts_with('/');
  uint64_t i = absolute ? 1 : 0;
  bool trailing = m.ends_with('/');
  while (i <= m.size()) {
    uint64_t e = i;
    while (e < m.size() && m[e] != '/') ++e;
    const StringView seg = m.substr(i, e - i);
    if (seg.equals("..")) {
      if (segs.count() > 0) segs.remove_at(segs.count() - 1);
      if (e >= m.size()) trailing = true;
    } else if (seg.equals(".")) {
      if (e >= m.size()) trailing = true;
    } else if (e < m.size() || !seg.empty()) {
      if (!segs.push_back(seg)) return false;
    }
    if (e >= m.size()) break;
    i = e + 1;
  }
  if (absolute && !clean.push_back('/')) return false;
  for (uint64_t k = 0; k < segs.count(); ++k) {
    if (k > 0 && !clean.push_back('/')) return false;
    if (!clean.append(segs[k])) return false;
  }
  if (trailing && segs.count() > 0 && !clean.push_back('/')) return false;
  return out->assign(scheme) && out->append(authority) && out->append(clean.view()) && out->append(tail);
}

bool read_representation(allocators::IAllocator& a, const Chain& chain, StringView base, DashRepresentation* rep) {
  const XmlNode* r = chain.node[2];
  rep->id.assign(r->attribute("id"));
  uint64_t v;
  if (parse_uint(chain_attribute(chain, "bandwidth"), &v)) rep->bandwidth = v;
  if (parse_uint(chain_attribute(chain, "width"), &v)) rep->width = static_cast<uint32_t>(v);
  if (parse_uint(chain_attribute(chain, "height"), &v)) rep->height = static_cast<uint32_t>(v);
  rep->codecs.assign(chain_attribute(chain, "codecs"));
  rep->mime_type.assign(chain_attribute(chain, "mimeType"));
  // the base address: BaseURL elements nest from the Period down; `base` already includes the MPD's and Period's
  String current(a);
  current.assign(base);
  for (int i = 1; i < 3; ++i) {
    const XmlNode* b = chain.node[i] ? chain.node[i]->child("BaseURL") : nullptr;
    if (!b) continue;
    String next(a);
    if (!resolve(a, current.view(), b->text.view().trimmed(), &next)) return false;
    current = util::move(next);
  }
  rep->base_url = util::move(current);
  for (int i = 0; i < 3; ++i) {
    if (!chain.node[i]) continue;
    if (const XmlNode* st = chain.node[i]->child("SegmentTemplate")) read_template(st, &rep->segment_template);
    if (const XmlNode* sl = chain.node[i]->child("SegmentList")) read_list(a, sl, &rep->segment_list);
    if (const XmlNode* sb = chain.node[i]->child("SegmentBase")) read_base(sb, &rep->segment_base);
  }
  return true;
}

}  // namespace

// ----------------------------------------------------------------------------------------------------------------------------------------- XmlNode

StringView XmlNode::attribute(StringView n, StringView fallback) const {
  for (const auto& a : attributes.view())
    if (a->name.view().equals(n)) return a->value.view();
  return fallback;
}
bool XmlNode::has_attribute(StringView n) const {
  for (const auto& a : attributes.view())
    if (a->name.view().equals(n)) return true;
  return false;
}
const XmlNode* XmlNode::child(StringView n) const {
  for (const auto& c : children.view())
    if (c->name.view().equals(n)) return c.get();
  return nullptr;
}

UniquePointer<XmlNode> parse_xml(allocators::IAllocator& allocator, StringView text) {
  XmlReader reader(allocator, text);
  return reader.run();
}

// ----------------------------------------------------------------------------------------------------------------------------------------- dates

bool parse_iso_duration(StringView s, int64_t* out) {
  s = s.trimmed();
  bool neg = false;
  if (s.starts_with('-')) {
    neg = true;
    s = s.without_prefix(1);
  }
  if (!s.starts_with('P')) return false;
  s = s.without_prefix(1);
  bool time = false, any = false;
  double total = 0;
  while (!s.empty()) {
    if (s[0] == 'T') {
      time = true;
      s = s.without_prefix(1);
      continue;
    }
    uint64_t n = 0;
    while (n < s.size() && (is_digit(s[n]) || s[n] == '.')) ++n;
    if (n == 0 || n >= s.size()) return false;
    double v;
    if (!parse_decimal(s.substr(0, n), &v)) return false;
    const char unit = s[n];
    s = s.without_prefix(n + 1);
    double seconds;
    if (!time) seconds = unit == 'Y' ? 365.0 * 86400 : unit == 'M' ? 30.0 * 86400 : unit == 'W' ? 7.0 * 86400 : unit == 'D' ? 86400.0 : -1;
    else seconds = unit == 'H' ? 3600.0 : unit == 'M' ? 60.0 : unit == 'S' ? 1.0 : -1;
    if (seconds < 0) return false;
    total += v * seconds;
    any = true;
  }
  if (!any || total > 1e12) return false;
  *out = static_cast<int64_t>(total * 1e6 + 0.5) * (neg ? -1 : 1);
  return true;
}

bool parse_iso_datetime(StringView s, int64_t* out) {
  s = s.trimmed();
  auto num = [&](uint64_t at, uint64_t len, int64_t* v) {
    if (at + len > s.size()) return false;
    int64_t r = 0;
    for (uint64_t i = 0; i < len; ++i) {
      if (!is_digit(s[at + i])) return false;
      r = r * 10 + (s[at + i] - '0');
    }
    *v = r;
    return true;
  };
  int64_t y, mo, d, h = 0, mi = 0, sec = 0;
  if (!num(0, 4, &y) || s.size() < 10 || s[4] != '-' || !num(5, 2, &mo) || s[7] != '-' || !num(8, 2, &d)) return false;
  uint64_t i = 10;
  if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) {
    if (!num(i + 1, 2, &h) || i + 3 >= s.size() || s[i + 3] != ':' || !num(i + 4, 2, &mi)) return false;
    i += 6;
    if (i < s.size() && s[i] == ':') {
      if (!num(i + 1, 2, &sec)) return false;
      i += 3;
      if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && is_digit(s[i])) ++i;
      }
    }
  }
  int64_t offset = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    int64_t oh, om = 0;
    if (!num(i + 1, 2, &oh)) return false;
    if (i + 3 < s.size() && s[i + 3] == ':') num(i + 4, 2, &om);
    else num(i + 3, 2, &om);
    offset = (oh * 3600 + om * 60) * (s[i] == '-' ? -1 : 1);
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || sec > 60) return false;
  // days from civil (Howard Hinnant)
  y -= mo <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + doe - 719468;
  *out = days * 86400 + h * 3600 + mi * 60 + sec - offset;
  return true;
}

// ----------------------------------------------------------------------------------------------------------------------------------------- the MPD

bool parse_mpd(allocators::IAllocator& a, StringView mpd, StringView manifest_url, DashManifest* out) {
  UniquePointer<XmlNode> root = parse_xml(a, mpd);
  if (!root || !root->name.view().equals("MPD")) return false;
  int64_t us;
  out->dynamic = root->attribute("type").equals("dynamic");
  if (parse_iso_duration(root->attribute("mediaPresentationDuration"), &us)) out->duration_us = us;
  if (parse_iso_duration(root->attribute("minBufferTime"), &us)) out->min_buffer_us = us;
  if (parse_iso_duration(root->attribute("minimumUpdatePeriod"), &us)) out->min_update_us = us;
  if (parse_iso_duration(root->attribute("timeShiftBufferDepth"), &us)) out->time_shift_depth_us = us;
  if (parse_iso_duration(root->attribute("suggestedPresentationDelay"), &us)) out->suggested_delay_us = us;
  int64_t secs;
  if (parse_iso_datetime(root->attribute("availabilityStartTime"), &secs)) out->availability_start_s = secs;
  if (const XmlNode* loc = root->child("Location")) out->location.assign(loc->text.view().trimmed());

  String mpd_base(a);
  mpd_base.assign(manifest_url);
  if (const XmlNode* b = root->child("BaseURL")) {
    String next(a);
    if (!resolve(a, manifest_url, b->text.view().trimmed(), &next)) return false;
    mpd_base = util::move(next);
  }

  int64_t previous_end = 0;
  for (const auto& pn : root->children.view()) {
    if (!pn->name.view().equals("Period")) continue;
    auto period = UniquePointer<DashPeriod>::make(a, a);
    if (!period) return false;
    period->id.assign(pn->attribute("id"));
    period->start_us = parse_iso_duration(pn->attribute("start"), &us) ? us : previous_end;
    period->duration_us = parse_iso_duration(pn->attribute("duration"), &us) ? us : -1;
    if (period->duration_us >= 0) previous_end = period->start_us + period->duration_us;
    String period_base(a);
    period_base.assign(mpd_base.view());
    if (const XmlNode* b = pn->child("BaseURL")) {
      String next(a);
      if (!resolve(a, mpd_base.view(), b->text.view().trimmed(), &next)) return false;
      period_base = util::move(next);
    }
    for (const auto& an : pn->children.view()) {
      if (!an->name.view().equals("AdaptationSet")) continue;
      auto set = UniquePointer<DashAdaptationSet>::make(a, a);
      if (!set) return false;
      StringView type = an->attribute("contentType");
      StringView mime = an->attribute("mimeType");
      set->language.assign(an->attribute("lang"));
      for (const auto& cp : an->children.view()) {
        if (!cp->name.view().equals("ContentProtection")) continue;
        auto prot = UniquePointer<DashProtection>::make(a, a);
        if (!prot) return false;
        for (char c : cp->attribute("schemeIdUri")) prot->scheme.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
        prot->default_kid.assign(cp->attribute("default_KID"));
        set->protection.push_back(util::move(prot));
      }
      Chain chain;
      chain.node[0] = pn.get();
      chain.node[1] = an.get();
      for (const auto& rn : an->children.view()) {
        if (!rn->name.view().equals("Representation")) continue;
        chain.node[2] = rn.get();
        auto rep = UniquePointer<DashRepresentation>::make(a, a);
        if (!rep) return false;
        // (the Period's BaseURL is in `period_base`; the chain adds the AdaptationSet's and the Representation's)
        if (!read_representation(a, chain, period_base.view(), rep.get())) return false;
        if (type.empty()) {
          const StringView m = rep->mime_type.view();
          if (mime.empty() && !m.empty()) mime = m;
        }
        // lowest bandwidth first
        uint64_t at = set->representations.count();
        while (at > 0 && set->representations[at - 1]->bandwidth > rep->bandwidth) --at;
        if (!set->representations.insert(at, util::move(rep))) return false;
      }
      if (type.empty()) type = mime.starts_with("video") ? StringView("video") : mime.starts_with("audio") ? StringView("audio") : mime.starts_with("text") || mime.starts_with("application") ? StringView("text") : StringView();
      set->content_type.assign(type);
      if (!period->sets.push_back(util::move(set))) return false;
    }
    if (!out->periods.push_back(util::move(period))) return false;
  }
  return out->periods.count() != 0;
}

// ----------------------------------------------------------------------------------------------------------------------------------------- templates

bool dash_expand(StringView p, StringView id, uint64_t bandwidth, uint64_t number, uint64_t time, String* out) {
  uint64_t i = 0;
  while (i < p.size()) {
    if (p[i] != '$') {
      uint64_t e = i;
      while (e < p.size() && p[e] != '$') ++e;
      if (!out->append(p.substr(i, e - i))) return false;
      i = e;
      continue;
    }
    const uint64_t close = p.find('$', i + 1);
    if (close == StringView::npos) return false;
    StringView name = p.substr(i + 1, close - i - 1);
    i = close + 1;
    if (name.empty()) {
      if (!out->push_back('$')) return false;
      continue;
    }
    uint64_t width = 1;
    const uint64_t pct = name.find('%');
    if (pct != StringView::npos) {
      StringView f = name.substr(pct);   // %0Nd
      name = name.substr(0, pct);
      if (f.size() < 3 || f[f.size() - 1] != 'd') return false;
      StringView digits = f.substr(1, f.size() - 2);
      if (digits.starts_with('0')) digits = digits.without_prefix(1);
      if (!digits.empty() && !parse_uint(digits, &width)) return false;
      if (width > 20) return false;
    }
    uint64_t value;
    if (name.equals("RepresentationID")) {
      if (!out->append(id)) return false;
      continue;
    } else if (name.equals("Number")) value = number;
    else if (name.equals("Bandwidth")) value = bandwidth;
    else if (name.equals("Time")) value = time;
    else return false;
    String digits(out->allocator());
    digits.append_uint(value);
    for (uint64_t k = digits.size(); k < width; ++k)
      if (!out->push_back('0')) return false;
    if (!out->append(digits.view())) return false;
  }
  return true;
}

bool dash_media_url(allocators::IAllocator& a, const DashRepresentation& rep, const DashSegment& seg, String* out) {
  String relative(a);
  if (rep.segment_template.present && !rep.segment_template.media.empty()) {
    if (!dash_expand(rep.segment_template.media.view(), rep.id.view(), rep.bandwidth, seg.number, seg.time, &relative)) return false;
  } else if (rep.segment_list.present && seg.item >= 0 && static_cast<uint64_t>(seg.item) < rep.segment_list.items.count()) {
    relative.assign(rep.segment_list.items[static_cast<uint64_t>(seg.item)]->media.view());
  }
  return resolve(a, rep.base_url.view(), relative.view(), out);
}

bool parse_sidx(allocators::IAllocator& a, ConstByteView bytes, uint64_t anchor, uint64_t* timescale, uint64_t* earliest, List<DashSidxEntry>* out) {
  (void)a;
  ByteReader r(bytes);
  uint64_t size = r.be32();
  const uint32_t type = r.be32();
  if (!r.ok() || type != 0x73696478u) return false;   // 'sidx'
  if (size == 1) {
    size = (static_cast<uint64_t>(r.be32()) << 32) | r.be32();
  }
  const uint32_t version_flags = r.be32();
  const uint8_t version = static_cast<uint8_t>(version_flags >> 24);
  r.be32();   // reference_ID
  *timescale = r.be32();
  uint64_t first_offset;
  if (version == 0) {
    *earliest = r.be32();
    first_offset = r.be32();
  } else {
    *earliest = (static_cast<uint64_t>(r.be32()) << 32) | r.be32();
    first_offset = (static_cast<uint64_t>(r.be32()) << 32) | r.be32();
  }
  r.be16();   // reserved
  const uint16_t count = r.be16();
  if (!r.ok() || *timescale == 0 || size > bytes.size()) return false;
  uint64_t offset = anchor + first_offset;
  for (uint16_t i = 0; i < count; ++i) {
    const uint32_t ref = r.be32();
    const uint32_t duration = r.be32();
    const uint32_t sap = r.be32();
    if (!r.ok()) return false;
    if (ref & 0x80000000u) continue;   // a reference to another index: not followed
    DashSidxEntry e;
    e.offset = offset;
    e.size = ref & 0x7FFFFFFFu;
    e.duration = duration;
    e.sap = (sap & 0x80000000u) != 0;
    if (!out->push_back(e)) return false;
    offset += e.size;
  }
  return true;
}

// ----------------------------------------------------------------------------------------------------------------------------------------- segments

namespace {

// The entries of a timeline as segments (times in timescale units), until `limit_time` (open ended repeats) .
bool timeline_segments(const List<DashTimelineEntry>& timeline, uint64_t timescale, int64_t pto, int64_t period_start_us, int64_t period_end_us, uint64_t start_number,
                       List<DashSegment>* out) {
  uint64_t t = 0, number = start_number;
  bool first = true;
  for (const DashTimelineEntry& e : timeline.view()) {
    if (e.time != UINT64_MAX) t = e.time;
    else if (first) t = 0;
    first = false;
    uint64_t count = static_cast<uint64_t>(e.repeat) + 1;
    if (e.open_ended) {
      const int64_t end_media = period_end_us < 0 ? INT64_MAX / 4 : period_end_us - period_start_us;
      const uint64_t span = e.duration;
      const uint64_t from_pto = t >= static_cast<uint64_t>(pto > 0 ? pto : 0) ? t - static_cast<uint64_t>(pto > 0 ? pto : 0) : 0;
      const int64_t start_us = scale_us(from_pto, timescale);
      count = end_media > start_us ? static_cast<uint64_t>((end_media - start_us) * static_cast<int64_t>(timescale) / 1000000 / static_cast<int64_t>(span)) + 1 : 1;
    }
    for (uint64_t k = 0; k < count; ++k) {
      if (out->count() >= kMaxSegments) return false;
      DashSegment s;
      s.number = number++;
      s.time = t;
      s.start_us = period_start_us + scale_us_signed(static_cast<int64_t>(t) - pto, timescale);
      s.duration_us = scale_us(e.duration, timescale);
      if (!out->push_back(s)) return false;
      t += e.duration;
    }
  }
  return true;
}

}  // namespace

bool dash_segments(allocators::IAllocator& a, const DashManifest& m, const DashPeriod& period, int64_t period_end_us, const DashRepresentation& rep, int64_t live_edge_us,
                   IDashRangeSource* source, DashSegments* out) {
  out->list.clear();
  out->init_url.clear();
  out->init_range = DashRange();
  const DashTemplate& tp = rep.segment_template;
  const DashList& sl = rep.segment_list;
  const DashBase& sb = rep.segment_base;
  const int64_t edge = m.dynamic ? live_edge_us : -1;
  if (tp.present && (tp.has_timeline || tp.duration > 0)) {
    out->timestamp_offset_us = period.start_us - scale_us_signed(tp.presentation_time_offset, tp.timescale);
    if (!tp.initialization.empty()) {
      String rel(a);
      if (!dash_expand(tp.initialization.view(), rep.id.view(), rep.bandwidth, 0, 0, &rel) || !resolve(a, rep.base_url.view(), rel.view(), &out->init_url)) return false;
    }
    if (tp.has_timeline) {
      if (!timeline_segments(tp.timeline, tp.timescale, tp.presentation_time_offset, period.start_us, period_end_us, tp.start_number, &out->list)) return false;
    } else {
      const int64_t seg_us = scale_us(tp.duration, tp.timescale);
      if (seg_us <= 0) return false;
      int64_t last_end = period_end_us >= 0 ? period_end_us : (edge >= 0 ? edge : -1);
      if (last_end < 0) return false;
      if (m.dynamic && edge >= 0 && edge < last_end) last_end = edge;
      uint64_t n = 0;
      int64_t from = 0;
      if (m.dynamic && m.time_shift_depth_us > 0 && last_end - period.start_us > m.time_shift_depth_us) {
        from = last_end - period.start_us - m.time_shift_depth_us;
        n = static_cast<uint64_t>(from / seg_us);
      }
      for (;; ++n) {
        const int64_t start = period.start_us + static_cast<int64_t>(n) * seg_us;
        if (start >= last_end || (m.dynamic && start + seg_us > last_end)) break;
        if (out->list.count() >= kMaxSegments) return false;
        DashSegment s;
        s.number = tp.start_number + n;
        s.time = n * tp.duration + static_cast<uint64_t>(tp.presentation_time_offset > 0 ? tp.presentation_time_offset : 0);
        s.start_us = start;
        s.duration_us = start + seg_us > last_end && !m.dynamic ? last_end - start : seg_us;
        if (!out->list.push_back(s)) return false;
      }
    }
    return true;
  }
  if (sl.present && (sl.items.count() != 0)) {
    out->timestamp_offset_us = period.start_us - scale_us_signed(sl.presentation_time_offset, sl.timescale);
    if (!sl.initialization.empty() || sl.initialization_range.present) {   // (no sourceURL: the range is of the representation's file)
      if (!resolve(a, rep.base_url.view(), sl.initialization.view(), &out->init_url)) return false;
      out->init_range = sl.initialization_range;
    }
    List<DashSegment> timed(a);
    if (sl.has_timeline) timeline_segments(sl.timeline, sl.timescale, sl.presentation_time_offset, period.start_us, period_end_us, sl.start_number, &timed);
    int64_t t = period.start_us;
    for (uint64_t i = 0; i < sl.items.count() && i < kMaxSegments; ++i) {
      DashSegment s;
      s.item = static_cast<int32_t>(i);
      s.number = sl.start_number + i;
      s.range = sl.items[i]->range;
      if (i < timed.count()) {
        s.start_us = timed[i].start_us;
        s.duration_us = timed[i].duration_us;
        s.time = timed[i].time;
      } else {
        s.start_us = t;
        s.duration_us = scale_us(sl.duration, sl.timescale);
      }
      t = s.start_us + s.duration_us;
      if (!out->list.push_back(s)) return false;
    }
    return true;
  }
  if (sb.present && sb.index_range.present && source) {   // one file with an index
    out->timestamp_offset_us = period.start_us - scale_us_signed(sb.presentation_time_offset, sb.timescale);
    out->init_url.assign(rep.base_url.view());
    out->init_range = sb.initialization_range;
    if (!out->init_range.present) {
      out->init_range.present = true;
      out->init_range.first = 0;
      out->init_range.last = sb.index_range.first > 0 ? sb.index_range.first - 1 : 0;
    }
    ByteBuffer index(a);
    if (!source->fetch(rep.base_url.view(), sb.index_range, &index)) return false;
    uint64_t timescale, earliest;
    List<DashSidxEntry> entries(a);
    if (!parse_sidx(a, index.view(), sb.index_range.last + 1, &timescale, &earliest, &entries)) return false;
    uint64_t t = earliest;
    for (const DashSidxEntry& e : entries.view()) {
      if (e.size == 0) continue;
      DashSegment s;
      s.time = t;
      s.number = out->list.count() + 1;
      s.start_us = period.start_us + scale_us_signed(static_cast<int64_t>(t) - sb.presentation_time_offset, timescale);
      s.duration_us = scale_us(e.duration, timescale);
      s.range.present = true;
      s.range.first = e.offset;
      s.range.last = e.offset + e.size - 1;
      if (!out->list.push_back(s)) return false;
      t += e.duration;
    }
    return out->list.count() != 0;
  }
  if (!rep.base_url.empty()) {   // the whole file is the one segment
    out->timestamp_offset_us = period.start_us;
    DashSegment s;
    s.number = 1;
    s.start_us = period.start_us;
    s.duration_us = period_end_us >= 0 ? period_end_us - period.start_us : 0;
    return out->list.push_back(s);
  }
  return false;
}

}  // namespace media
