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

#include "Browser/HtmlDocument.hpp"

namespace browser {

namespace {

constexpr uint32_t kMaxListDepth = 12;
constexpr uint32_t kMaxIndent = 60;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

struct Entity {
  const char* name;
  const char* text;
};
const Entity kEntities[] = {
    {"amp", "&"},     {"lt", "<"},      {"gt", ">"},       {"quot", "\""},   {"apos", "'"},    {"nbsp", " "},    {"copy", "(c)"},  {"reg", "(R)"},
    {"trade", "(TM)"}, {"mdash", "-"},  {"ndash", "-"},    {"hellip", "..."}, {"lsquo", "'"},  {"rsquo", "'"},   {"ldquo", "\""},  {"rdquo", "\""},
    {"bull", "*"},    {"middot", "."},  {"laquo", "<<"},   {"raquo", ">>"},  {"times", "x"},   {"euro", "EUR"},  {"pound", "GBP"}, {"deg", "deg"},
    {"hearts", "<3"}, {"larr", "<-"},   {"rarr", "->"},
};

// The text a character (or a few) of UTF-8 stands for among the characters that can be shown. *used is how many bytes it takes.
const char* utf8_look_alike(StringView s, uint64_t at, uint32_t* used) {
  const uint8_t c = static_cast<uint8_t>(s[at]);
  uint32_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
  if (at + length > s.size()) length = static_cast<uint32_t>(s.size() - at);
  for (uint32_t i = 1; i < length; ++i) {   // (a damaged sequence ends where the continuation bytes do)
    if ((static_cast<uint8_t>(s[at + i]) & 0xC0) != 0x80) {
      length = i;
      break;
    }
  }
  *used = length;
  if (length == 2 && c == 0xC2 && static_cast<uint8_t>(s[at + 1]) == 0xA0) return " ";
  if (length == 3 && c == 0xE2 && static_cast<uint8_t>(s[at + 1]) == 0x80) {
    switch (static_cast<uint8_t>(s[at + 2])) {
      case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: return "-";
      case 0x98: case 0x99: return "'";
      case 0x9C: case 0x9D: return "\"";
      case 0xA2: return "*";
      case 0xA6: return "...";
      default: break;
    }
  }
  return "?";
}

// Decodes the entity that starts at `at` (a '&'); appends what it stands for to *out and returns how many characters it takes (1 if it is no entity:
// the '&' then stands for itself).
uint64_t decode_entity(StringView s, uint64_t at, String* out) {
  uint64_t end = at + 1;
  while (end < s.size() && end - at < 12 && s[end] != ';' && s[end] != '&' && !is_space(s[end]) && s[end] != '<') ++end;
  if (end >= s.size() || s[end] != ';' || end == at + 1) {
    out->push_back('&');
    return 1;
  }
  const StringView name = s.substr(at + 1, end - at - 1);
  const uint64_t taken = end - at + 1;
  if (name[0] == '#') {
    uint64_t value = 0;
    bool ok = name.size() > 1;
    const bool hex = name.size() > 2 && (name[1] == 'x' || name[1] == 'X');
    for (uint64_t i = hex ? 2 : 1; i < name.size() && ok; ++i) {
      const char c = name[i];
      const int digit = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0) ok = false;
      else value = value * (hex ? 16 : 10) + static_cast<uint64_t>(digit);
      if (value > 0x10FFFF) ok = false;
    }
    if (!ok) {
      out->push_back('&');
      return 1;
    }
    if (value >= 32 && value < 127) out->push_back(static_cast<char>(value));
    else if (value == 9 || value == 10 || value == 13 || value == 0xA0) out->push_back(' ');
    else if (value == 0x2018 || value == 0x2019) out->push_back('\'');
    else if (value == 0x201C || value == 0x201D) out->push_back('"');
    else if (value >= 0x2010 && value <= 0x2015) out->push_back('-');
    else if (value >= 128) out->push_back('?');
    return taken;
  }
  for (const Entity& e : kEntities) {
    if (name.equals(e.name)) {
      out->append(e.text);
      return taken;
    }
  }
  out->push_back('?');
  return taken;
}

// The value of attribute `name` in the text of a tag (after its name); false if it has none.
bool attribute(StringView attrs, StringView name, String* value) {
  uint64_t i = 0;
  const uint64_t n = attrs.size();
  while (i < n) {
    while (i < n && (is_space(attrs[i]) || attrs[i] == '/')) ++i;
    const uint64_t name_start = i;
    while (i < n && !is_space(attrs[i]) && attrs[i] != '=' && attrs[i] != '/') ++i;
    const StringView found = attrs.substr(name_start, i - name_start);
    while (i < n && is_space(attrs[i])) ++i;
    StringView raw;
    if (i < n && attrs[i] == '=') {
      ++i;
      while (i < n && is_space(attrs[i])) ++i;
      if (i < n && (attrs[i] == '"' || attrs[i] == '\'')) {
        const char quote = attrs[i++];
        const uint64_t start = i;
        while (i < n && attrs[i] != quote) ++i;
        raw = attrs.substr(start, i - start);
        if (i < n) ++i;
      } else {
        const uint64_t start = i;
        while (i < n && !is_space(attrs[i])) ++i;
        raw = attrs.substr(start, i - start);
      }
    }
    if (!found.empty() && found.equals_ignore_case(name)) {
      value->clear();
      for (uint64_t k = 0; k < raw.size();) {
        if (raw[k] == '&') k += decode_entity(raw, k, value);
        else value->push_back(raw[k++]);
      }
      return true;
    }
  }
  return false;
}

bool name_is(StringView name, const char* a) { return name.equals(a); }
template <class... Rest>
bool name_is(StringView name, const char* a, Rest... rest) {
  return name.equals(a) || name_is(name, rest...);
}

}  // namespace

bool HtmlDocument::add_run(StringView text, uint8_t flags, uint8_t level, uint8_t indent, uint8_t breaks, int32_t link) {
  if (runs_.count() >= kMaxRuns || pool_.size() + text.size() > kMaxText) {
    truncated_ = true;
    return true;   // (dropped: not an error, the page is just cut off)
  }
  Run run;
  run.offset = static_cast<uint32_t>(pool_.size());
  run.length = static_cast<uint32_t>(text.size());
  run.flags = flags;
  run.level = level;
  run.indent = indent;
  run.breaks = breaks;
  run.link = link;
  return pool_.append(text) && runs_.push_back(run);
}

int32_t HtmlDocument::add_link(StringView target) {
  if (links_.count() >= kMaxRuns || pool_.size() + target.size() > kMaxText) return -1;
  LinkRef ref{static_cast<uint32_t>(pool_.size()), static_cast<uint32_t>(target.size())};
  if (!pool_.append(target) || !links_.push_back(ref)) return -1;
  return static_cast<int32_t>(links_.count() - 1);
}

void HtmlDocument::clear() {
  runs_.clear();
  links_.clear();
  pool_.clear();
  title_.clear();
  truncated_ = false;
}

bool HtmlDocument::parse_text(StringView text) {
  clear();
  String shown(allocator_);
  for (uint64_t i = 0; i < text.size();) {
    const char c = text[i];
    if (c == '\r') {
      ++i;
    } else if (c == '\t') {
      shown.append("    ");
      ++i;
    } else if (c == '\n' || (c >= 32 && c < 127)) {
      shown.push_back(c);
      ++i;
    } else if (static_cast<uint8_t>(c) >= 0x80) {
      uint32_t used = 1;
      shown.append(utf8_look_alike(text, i, &used));
      i += used;
    } else {
      ++i;   // (other control characters are dropped)
    }
  }
  if (shown.size() && shown[shown.size() - 1] == '\n') shown.truncate(shown.size() - 1);
  if (shown.empty()) return true;
  return add_run(shown.view(), kRunPre, 0, 0, 0, -1);
}

// Reads HTML into runs. The state is what the tags seen so far have switched on: bold, code, preformatted, the heading level, the link, the
// indentation and the lists, plus the line breaks wanted before the next text.
class HtmlParser {
 public:
  HtmlParser(HtmlDocument& doc, allocators::IAllocator& allocator) : doc_(doc), chunk_(allocator), title_(allocator) {}

  bool run(StringView html) {
    uint64_t i = 0;
    const uint64_t n = html.size();
    while (i < n && !doc_.truncated_) {
      const char c = html[i];
      if (c == '<') {
        i = tag(html, i);
      } else if (c == '&') {
        i += entity(html, i);
      } else if (static_cast<uint8_t>(c) >= 0x80) {
        uint32_t used = 1;
        const char* look = utf8_look_alike(html, i, &used);
        for (const char* p = look; *p; ++p) put(*p);
        i += used;
      } else {
        put(c);
        ++i;
      }
    }
    flush();
    if (!doc_.title_.assign(title_.view().trimmed())) return false;
    return !failed_;
  }

 private:
  // One character of page text, with white space collapsed (unless preformatted).
  void put(char c) {
    if (skip_newline_) {
      skip_newline_ = false;
      if (c == '\n') return;
      if (c == '\r') {
        skip_newline_ = true;
        return;
      }
    }
    String& target = in_title_ ? title_ : chunk_;
    if (pre_ > 0 && !in_title_) {
      if (c == '\r') return;
      if (c == '\t') {
        failed_ |= !target.append("    ");
        ended_space_ = true;
        return;
      }
      if (c != '\n' && (c < 32 || c > 126)) return;
      failed_ |= !target.push_back(c);
      ended_space_ = c == '\n' || c == ' ';
      return;
    }
    if (is_space(c)) {
      if (in_title_ ? (title_.empty() || title_[title_.size() - 1] == ' ') : ended_space_) return;
      failed_ |= !target.push_back(' ');
      if (!in_title_) ended_space_ = true;
      return;
    }
    if (c < 32 || c > 126) return;
    failed_ |= !target.push_back(c);
    if (!in_title_) ended_space_ = false;
  }

  uint64_t entity(StringView html, uint64_t at) {
    String& target = in_title_ ? title_ : chunk_;
    String decoded(doc_.allocator());
    const uint64_t taken = decode_entity(html, at, &decoded);
    for (uint64_t k = 0; k < decoded.size(); ++k) {
      if (decoded[k] == ' ') put(' ');   // (a no-break space still collapses: a plain, forgiving reading)
      else {
        failed_ |= !target.push_back(decoded[k]);
        if (!in_title_) ended_space_ = false;
      }
    }
    return taken;
  }

  // Turns the text collected since the last tag into a run in the current style.
  void flush() {
    if (chunk_.empty()) return;
    StringView text = chunk_.view();
    const uint8_t breaks = doc_.runs_.count() == 0 ? 0 : pending_;
    if (pre_ == 0 && breaks > 0 && text[0] == ' ') text = text.substr(1);   // (no blank at the start of a line)
    if (!text.empty()) {
      uint8_t flags = 0;
      if (heading_) flags |= kRunHeading | kRunBold;
      if (bold_ > 0) flags |= kRunBold;
      if (code_ > 0) flags |= kRunCode;
      if (pre_ > 0) flags |= kRunPre;
      failed_ |= !doc_.add_run(text, flags, heading_, indent(), breaks, link_);
      pending_ = 0;
    }
    chunk_.clear();
  }

  uint8_t indent() const { return static_cast<uint8_t>(indent_ < kMaxIndent ? indent_ : kMaxIndent); }

  void block(uint8_t breaks) {
    flush();
    if (breaks > pending_) pending_ = breaks;
    ended_space_ = true;
  }

  // Text that is not in the page: a list marker, a table divider, an image's description.
  void literal(StringView text, bool at_line_start_indent = false) {
    flush();
    if (text.empty()) return;
    const uint8_t breaks = doc_.runs_.count() == 0 ? 0 : pending_;
    failed_ |= !doc_.add_run(text, 0, 0, at_line_start_indent ? static_cast<uint8_t>(indent_ >= 3 ? (indent_ - 3 < kMaxIndent ? indent_ - 3 : kMaxIndent) : 0) : indent(), breaks, link_);
    pending_ = 0;
    ended_space_ = text[text.size() - 1] == ' ';
  }

  // Handles the tag that starts at html[at] == '<'; returns the position after it (or after the text it hides).
  uint64_t tag(StringView html, uint64_t at) {
    const uint64_t n = html.size();
    if (html.substr(at).starts_with("<!--")) {
      const uint64_t end = html.find("-->", at + 4);
      return end == StringView::npos ? n : end + 3;
    }
    if (at + 1 < n && (html[at + 1] == '!' || html[at + 1] == '?')) {
      const uint64_t end = html.find('>', at);
      return end == StringView::npos ? n : end + 1;
    }
    uint64_t p = at + 1;
    const bool closing = p < n && html[p] == '/';
    if (closing) ++p;
    if (p >= n || !is_letter(html[p])) {   // not a tag: a '<' in the text
      put('<');
      return at + 1;
    }
    const uint64_t name_start = p;
    while (p < n && !is_space(html[p]) && html[p] != '/' && html[p] != '>') ++p;
    char name_text[16];
    uint32_t name_length = 0;
    for (uint64_t k = name_start; k < p && name_length < sizeof name_text - 1; ++k) name_text[name_length++] = lower(html[k]);
    name_text[name_length] = 0;
    const StringView name(name_text, name_length);
    const uint64_t attrs_start = p;
    char quote = 0;
    while (p < n && (quote || html[p] != '>')) {
      if (quote) {
        if (html[p] == quote) quote = 0;
      } else if (html[p] == '"' || html[p] == '\'') {
        quote = html[p];
      }
      ++p;
    }
    const StringView attrs = html.substr(attrs_start, p - attrs_start);
    const uint64_t after = p < n ? p + 1 : n;
    if (closing) {
      close_tag(name);
      return after;
    }
    // Elements whose content is not shown: skipped up to their end tag.
    if (name_is(name, "script", "style", "svg", "template", "iframe", "select", "textarea", "noscript", "object")) {
      flush();
      String end(doc_.allocator());
      end.append("</");
      end.append(name);
      for (uint64_t k = after; k + end.size() <= n; ++k) {
        if (html[k] == '<' && html.substr(k, end.size()).equals_ignore_case(end.view())) {
          const uint64_t close = html.find('>', k);
          return close == StringView::npos ? n : close + 1;
        }
      }
      return n;
    }
    open_tag(name, attrs);
    return after;
  }

  void open_tag(StringView name, StringView attrs) {
    if (name_is(name, "br")) {
      flush();
      if (pending_ < 2) ++pending_;
      ended_space_ = true;
    } else if (name_is(name, "hr")) {
      block(2);
      failed_ |= !doc_.add_run(StringView(), kRunRule, 0, 0, doc_.runs_.count() == 0 ? 0 : pending_, -1);
      pending_ = 0;
      block(2);
    } else if (name_is(name, "img")) {
      String alt(doc_.allocator());
      if (!attribute(attrs, "alt", &alt) || alt.empty()) alt.assign("image");
      String shown(doc_.allocator());
      shown.push_back('[');
      for (uint64_t k = 0; k < alt.size(); ++k) shown.push_back(alt[k] >= 32 && alt[k] < 127 ? alt[k] : '?');
      shown.push_back(']');
      if (!ended_space_) literal(" ");
      literal(shown.view());
    } else if (name_is(name, "title")) {
      flush();
      in_title_ = true;
    } else if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
      block(2);
      heading_ = static_cast<uint8_t>(name[1] - '0');
    } else if (name_is(name, "p", "table", "form", "figure", "dl")) {
      block(2);
    } else if (name_is(name, "div", "section", "article", "header", "footer", "nav", "main", "aside", "address", "center", "details", "summary",
                       "caption", "thead", "tbody", "tfoot", "fieldset", "dt", "dd", "legend")) {
      block(1);
      if (name_is(name, "dd")) indent_ += 4;
    } else if (name_is(name, "tr")) {
      block(1);
      cell_ = 0;
    } else if (name_is(name, "td", "th")) {
      flush();
      if (cell_++ > 0) {
        chunk_.append(" | ");
        ended_space_ = true;
      }
      if (name_is(name, "th")) ++bold_;
    } else if (name_is(name, "ul", "ol")) {
      block(lists_ > 0 ? 1 : 2);
      if (lists_ < kMaxListDepth) {
        ordered_[lists_] = name_is(name, "ol");
        counter_[lists_] = 0;
      }
      ++lists_;
      indent_ += 3;
    } else if (name_is(name, "li")) {
      block(1);
      String marker(doc_.allocator());
      if (lists_ > 0 && lists_ <= kMaxListDepth && ordered_[lists_ - 1]) {
        marker.append_uint(++counter_[lists_ - 1]);
        marker.append(". ");
      } else {
        marker.append("* ");
      }
      literal(marker.view(), true);
    } else if (name_is(name, "blockquote")) {
      block(2);
      indent_ += 4;
    } else if (name_is(name, "pre")) {
      block(2);
      ++pre_;
      skip_newline_ = true;
    } else if (name_is(name, "a")) {
      flush();
      String href(doc_.allocator());
      link_ = attribute(attrs, "href", &href) && !href.empty() ? doc_.add_link(href.view()) : -1;
    } else if (name_is(name, "b", "strong")) {
      flush();
      ++bold_;
    } else if (name_is(name, "code", "tt", "kbd", "samp")) {
      flush();
      ++code_;
    }
  }

  void close_tag(StringView name) {
    if (name_is(name, "title")) {
      in_title_ = false;
    } else if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
      block(2);
      heading_ = 0;
    } else if (name_is(name, "p", "table", "form", "figure", "dl")) {
      block(2);
    } else if (name_is(name, "div", "section", "article", "header", "footer", "nav", "main", "aside", "address", "center", "details", "summary",
                       "caption", "thead", "tbody", "tfoot", "fieldset", "dt", "legend", "tr", "li")) {
      block(1);
    } else if (name_is(name, "dd")) {
      block(1);
      if (indent_ >= 4) indent_ -= 4;
    } else if (name_is(name, "th")) {
      flush();
      if (bold_ > 0) --bold_;
    } else if (name_is(name, "ul", "ol")) {
      block(lists_ > 1 ? 1 : 2);
      if (lists_ > 0) --lists_;
      if (indent_ >= 3) indent_ -= 3;
    } else if (name_is(name, "blockquote")) {
      block(2);
      if (indent_ >= 4) indent_ -= 4;
    } else if (name_is(name, "pre")) {
      block(2);
      if (pre_ > 0) --pre_;
    } else if (name_is(name, "a")) {
      flush();
      link_ = -1;
    } else if (name_is(name, "b", "strong")) {
      flush();
      if (bold_ > 0) --bold_;
    } else if (name_is(name, "code", "tt", "kbd", "samp")) {
      flush();
      if (code_ > 0) --code_;
    }
  }

  HtmlDocument& doc_;
  String chunk_, title_;
  bool failed_ = false, ended_space_ = true, in_title_ = false, skip_newline_ = false;
  uint8_t pending_ = 0, heading_ = 0;
  uint32_t bold_ = 0, code_ = 0, pre_ = 0, indent_ = 0, lists_ = 0, cell_ = 0;
  int32_t link_ = -1;
  bool ordered_[kMaxListDepth] = {};
  uint32_t counter_[kMaxListDepth] = {};
};

bool HtmlDocument::parse_html(StringView html) {
  clear();
  HtmlParser parser(*this, allocator_);
  if (!parser.run(html)) {
    clear();
    return false;
  }
  return true;
}

}  // namespace browser
