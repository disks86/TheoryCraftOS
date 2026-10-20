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
#include "Utility/Move.hpp"
#include <initializer_list>

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

// The value of attribute `name` in the text of a tag (entities decoded); false if it has none.
bool attribute(StringView attrs, StringView name, String* value) {
  StringView raw;
  if (!find_attribute(attrs, name, &raw)) return false;
  value->clear();
  for (uint64_t k = 0; k < raw.size();) {
    if (raw[k] == '&') k += decode_entity(raw, k, value);
    else value->push_back(raw[k++]);
  }
  return true;
}

bool name_is(StringView name, const char* a) { return name.equals(a); }
template <class... Rest>
bool name_is(StringView name, const char* a, Rest... rest) {
  return name.equals(a) || name_is(name, rest...);
}

}  // namespace

bool HtmlDocument::add_run(StringView text, uint8_t flags, uint8_t level, uint8_t indent, uint8_t breaks, int32_t link, uint8_t align, uint32_t color, uint32_t background) {
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
  run.align = align;
  run.link = link;
  run.color = color;
  run.background = background;
  return pool_.append(text) && runs_.push_back(run);
}

int32_t HtmlDocument::add_link(StringView target) {
  if (links_.count() >= kMaxRuns || pool_.size() + target.size() > kMaxText) return -1;
  LinkRef ref{static_cast<uint32_t>(pool_.size()), static_cast<uint32_t>(target.size())};
  if (!pool_.append(target) || !links_.push_back(ref)) return -1;
  return static_cast<int32_t>(links_.count() - 1);
}

bool HtmlDocument::add_stylesheet(StringView target) {
  if (sheets_.count() >= 16 || pool_.size() + target.size() > kMaxText) return true;
  LinkRef ref{static_cast<uint32_t>(pool_.size()), static_cast<uint32_t>(target.size())};
  return pool_.append(target) && sheets_.push_back(ref);
}

bool HtmlDocument::add_script_ref(StringView target) {
  if (scripts_.count() >= 32 || pool_.size() + target.size() > kMaxText) return true;
  LinkRef ref{static_cast<uint32_t>(pool_.size()), static_cast<uint32_t>(target.size())};
  return pool_.append(target) && scripts_.push_back(ref);
}

void HtmlDocument::clear() {
  page_background_ = 0;
  has_scripts_ = false;
  scripts_.clear();
  sheets_.clear();
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
  return add_run(shown.view(), kRunPre, 0, 0, 0, -1, 0, 0, 0);
}


// Reads HTML into runs. The state is the stack of open elements - each with the style it ended up with after the defaults for its kind, the page's
// stylesheets and its style attribute - plus the line breaks wanted before the next text.
class HtmlParser {
 public:
  static constexpr uint32_t kMaxDepth = 400;

  HtmlParser(HtmlDocument& doc, allocators::IAllocator& allocator, IScriptRunner* scripts) : doc_(doc), scripts_(scripts), written_(allocator), chunk_(allocator), title_(allocator), sheet_(allocator), stack_(allocator), path_(allocator) {}

  bool run(StringView html, StringView extra_css) {
    html_ = html;
    if (!extra_css.empty()) failed_ |= !sheet_.parse(extra_css);
    collect_styles(html);
    parse_chunk(html);
    flush();
    if (!doc_.title_.assign(title_.view().trimmed())) return false;
    return !failed_;
  }

 private:
  // Reads a piece of the page: the page itself, and what its scripts wrote.
  void parse_chunk(StringView html) {
    uint64_t i = 0;
    const uint64_t n = html.size();
    while (i < n && !doc_.truncated_ && !failed_) {
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
  }

  struct Element {
    Style style;
    int32_t link = -1;
    char tag[12] = {};
    uint32_t children = 0;   // elements opened inside it so far
    uint32_t items = 0;      // list items (ul, ol) or cells (tr) so far
    bool none = false;       // display none: its content is not shown
  };

  bool tag_is(const Element& e, const char* name) const { return StringView(e.tag).equals(name); }
  const Style& cur() const { return stack_.count() ? stack_[stack_.count() - 1].style : root_; }
  int32_t cur_link() const { return stack_.count() ? stack_[stack_.count() - 1].link : -1; }
  bool hidden() const { return none_ > 0 || cur().hidden; }
  uint8_t indent() const { return static_cast<uint8_t>(cur().indent < kMaxIndent ? cur().indent : kMaxIndent); }

  // One character of page text, with white space collapsed (unless preformatted).
  void put(char c) {
    if (head_open_ && !in_title_ && !is_space(c)) close_element("head");   // (text in the head starts the body)
    if (hidden() && !in_title_) return;
    if (skip_newline_) {
      skip_newline_ = false;
      if (c == '\n') return;
      if (c == '\r') {
        skip_newline_ = true;
        return;
      }
    }
    String& target = in_title_ ? title_ : chunk_;
    if (cur().pre && !in_title_) {
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
    String decoded(doc_.allocator());
    const uint64_t taken = decode_entity(html, at, &decoded);
    for (uint64_t k = 0; k < decoded.size(); ++k) put(decoded[k]);
    return taken;
  }

  uint32_t run_flags(const Style& s) const {
    uint32_t flags = 0;
    if (s.heading) flags |= kRunHeading;
    if (s.bold) flags |= kRunBold;
    if (s.code) flags |= kRunCode;
    if (s.pre) flags |= kRunPre;
    if (s.underline) flags |= kRunUnderline;
    if (s.strike) flags |= kRunStrike;
    if (s.background && s.background_block) flags |= kRunBlockBg;
    return flags;
  }

  // Turns the text collected since the last tag into a run in the current style.
  void flush() {
    if (chunk_.empty()) return;
    const Style& s = cur();
    StringView text = chunk_.view();
    const uint8_t breaks = doc_.runs_.count() == 0 ? 0 : pending_;
    if (!s.pre && breaks > 0 && text[0] == ' ') text = text.substr(1);   // (no blank at the start of a line)
    String transformed(doc_.allocator());
    if (s.transform && !text.empty()) {
      for (uint64_t k = 0; k < text.size(); ++k) {
        char c = text[k];
        if (s.transform == 1 && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (s.transform == 2 && c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        failed_ |= !transformed.push_back(c);
      }
      text = transformed.view();
    }
    if (!text.empty()) {
      failed_ |= !doc_.add_run(text, static_cast<uint8_t>(run_flags(s)), s.heading, indent(), breaks, cur_link(), s.align, s.color, s.background);
      pending_ = 0;
    }
    chunk_.clear();
  }

  void block(uint8_t breaks) {
    flush();
    if (breaks > pending_) pending_ = breaks;
    ended_space_ = true;
  }

  // Text that is not in the page: a list marker, a table divider, an image's description.
  void literal(StringView text, uint8_t at_indent) {
    flush();
    if (text.empty() || hidden()) return;
    const Style& s = cur();
    const uint8_t breaks = doc_.runs_.count() == 0 ? 0 : pending_;
    failed_ |= !doc_.add_run(text, static_cast<uint8_t>(run_flags(s) & ~kRunPre), 0, at_indent, breaks, cur_link(), s.align, s.color, s.background);
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
    const StringView written = html.substr(name_start, p - name_start);
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
      close_element(name);
      return after;
    }
    if (name_is(name, "style")) {   // (already read, before the page: a stylesheet applies to the whole page, wherever it is)
      const uint64_t end = find_end_tag(html, after, "</style");
      const uint64_t close = html.find('>', end);
      return end >= n || close == StringView::npos ? n : close + 1;
    }
    // Elements whose content is not shown: skipped up to their end tag.
    if (name_is(name, "script")) return script_element(html, attrs, after);
    if (name_is(name, "svg", "template", "iframe", "select", "textarea", "noscript", "object")) {
      flush();
      String end(doc_.allocator());
      end.append("</");
      end.append(name);
      const uint64_t found = find_end_tag(html, after, end.view());
      if (found >= n) return n;
      const uint64_t close = html.find('>', found);
      return close == StringView::npos ? n : close + 1;
    }
    open_element(name, written, attrs);
    return after;
  }

  // A <script>: counted, and run (when there is a runner) with what it wrote read in its place. Returns the position after it.
  uint64_t script_element(StringView html, StringView attrs, uint64_t after) {
    const uint64_t n = html.size();
    flush();
    const uint64_t end = find_end_tag(html, after, "</script");
    const uint64_t close = html.find('>', end);
    const uint64_t next = end >= n || close == StringView::npos ? n : close + 1;
    doc_.has_scripts_ = true;
    StringView type, src;
    const bool has_type = find_attribute(attrs, "type", &type);
    const bool has_src = find_attribute(attrs, "src", &src) && !src.empty();
    String target(doc_.allocator());
    uint32_t index = 0;
    if (has_src) {
      index = external_scripts_++;
      if (attribute(attrs, "src", &target)) failed_ |= !doc_.add_script_ref(target.view());
    }
    const bool javascript = !has_type || type.empty() || type.equals_ignore_case("text/javascript") || type.equals_ignore_case("application/javascript") ||
                            type.equals_ignore_case("text/ecmascript") || type.equals_ignore_case("application/ecmascript");
    if (!scripts_ || !javascript || script_depth_ > 3) return next;
    StringView source = has_src ? scripts_->external(index) : html.substr(after, (end < n ? end : n) - after);
    if (source.empty()) return next;
    String written(doc_.allocator());
    ++script_depth_;
    scripts_->run(source, &written);
    if (!written.empty()) {
      // (kept for as long as the parse goes on: the elements it opens point into it)
      if (written_.push_back(util::move(written))) {
        const StringView text = written_[written_.count() - 1].view();
        collect_styles(text);
        parse_chunk(text);
      } else {
        failed_ = true;
      }
    }
    --script_depth_;
    return next;
  }

  // Reads the <style> elements of the whole page (in order) before the page itself is read.
  void collect_styles(StringView html) {
    const uint64_t n = html.size();
    uint64_t i = 0;
    while (i < n && !failed_) {
      i = html.find('<', i);
      if (i == StringView::npos) return;
      if (html.substr(i).starts_with("<!--")) {
        const uint64_t end = html.find("-->", i + 4);
        i = end == StringView::npos ? n : end + 3;
        continue;
      }
      const bool style = i + 6 < n && html.substr(i, 6).equals_ignore_case("<style") && (is_space(html[i + 6]) || html[i + 6] == '>');
      const bool script = i + 7 < n && html.substr(i, 7).equals_ignore_case("<script") && (is_space(html[i + 7]) || html[i + 7] == '>');
      if (!style && !script) {
        ++i;
        continue;
      }
      const uint64_t open_end = html.find('>', i);
      if (open_end == StringView::npos) return;
      const uint64_t end = find_end_tag(html, open_end + 1, style ? "</style" : "</script");
      if (style) {
        StringView media;
        const StringView attrs = html.substr(i + 6, open_end - i - 6);
        if (!find_attribute(attrs, "media", &media) || !contains_word(media, "print")) failed_ |= !sheet_.parse(html.substr(open_end + 1, end - open_end - 1));
      }
      i = end;
      if (i < n) ++i;
    }
  }

  static bool contains_word(StringView text, const char* word) {
    const StringView w(word);
    for (uint64_t i = 0; i + w.size() <= text.size(); ++i)
      if (text.substr(i, w.size()).equals_ignore_case(w)) return true;
    return false;
  }

  // Where the end tag `end` ("</style") starts at or after `from`, or the end of the page.
  static uint64_t find_end_tag(StringView html, uint64_t from, StringView end) {
    for (uint64_t k = from; k + end.size() <= html.size(); ++k)
      if (html[k] == '<' && html.substr(k, end.size()).equals_ignore_case(end)) return k;
    return html.size();
  }

  // --- The stack of open elements

  void pop_element() {
    flush();
    const uint64_t last = stack_.count() - 1;
    const Element e = stack_[last];
    stack_.remove_at(last);
    path_.remove_at(last);
    if (e.none && none_ > 0) --none_;
    if (tag_is(e, "title")) in_title_ = false;
    if (tag_is(e, "head")) head_open_ = false;
    if (e.style.display != Display::Inline && !e.none && !hidden())
      block(static_cast<uint8_t>(1 + e.style.margin_bottom + e.style.padding_bottom));
  }

  void pop_to(uint64_t depth) {
    while (stack_.count() > depth) pop_element();
  }

  void close_element(StringView name) {
    for (uint64_t i = stack_.count(); i > 0; --i) {
      if (StringView(stack_[i - 1].tag).equals(name)) {
        pop_to(i - 1);
        return;
      }
    }
  }

  // Closes the innermost open element called one of `names`, if there is one inside the nearest element called one of `stops`.
  void close_open(std::initializer_list<const char*> names, std::initializer_list<const char*> stops) {
    for (uint64_t i = stack_.count(); i > 0; --i) {
      const Element& e = stack_[i - 1];
      for (const char* s : stops)
        if (tag_is(e, s)) return;
      for (const char* nm : names) {
        if (tag_is(e, nm)) {
          pop_to(i - 1);
          return;
        }
      }
    }
  }

  // The end tags HTML lets be left out: a new <li> ends the open one, a block ends a paragraph, and so on.
  void auto_close(StringView name) {
    if (head_open_ && !name_is(name, "title", "meta", "link", "script", "style", "base", "noscript", "head")) close_element("head");   // (anything else starts the body)
    if (name_is(name, "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "dl", "table", "pre", "blockquote", "form", "hr", "section", "article", "header",
                 "footer", "nav", "aside", "address", "figure", "fieldset")) {
      while (stack_.count() > 0 && tag_is(stack_[stack_.count() - 1], "p")) pop_element();
    }
    if (name_is(name, "li")) close_open({"li"}, {"ul", "ol"});
    else if (name_is(name, "dt", "dd")) close_open({"dt", "dd"}, {"dl"});
    else if (name_is(name, "tr")) close_open({"tr"}, {"table"});
    else if (name_is(name, "td", "th")) close_open({"td", "th"}, {"tr", "table"});
    else if (name_is(name, "thead", "tbody", "tfoot")) close_open({"thead", "tbody", "tfoot", "tr"}, {"table"});
  }

  // What an element looks like before the page's own styles: the defaults of its kind and what its attributes say.
  void defaults(StringView name, StringView attrs, Style* s) {
    StringView value;
    if (name_is(name, "head", "title", "meta", "link", "base")) s->display = Display::None;
    if (name_is(name, "div", "section", "article", "header", "footer", "nav", "main", "aside", "address", "center", "details", "summary", "caption", "thead",
                "tbody", "tfoot", "fieldset", "dt", "dd", "legend", "tr", "body", "html")) s->display = Display::Block;
    if (name_is(name, "p", "table", "form", "figure", "dl", "ul", "ol", "pre", "blockquote", "hr", "h1", "h2", "h3", "h4", "h5", "h6")) {
      s->display = Display::Block;
      s->margin_top = s->margin_bottom = 1;
    }
    if (name_is(name, "li")) s->display = Display::ListItem;
    if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
      s->heading = static_cast<uint8_t>(name[1] - '0');
      s->bold = true;
    }
    if (name_is(name, "ul", "ol")) {
      if (s->in_list) s->margin_top = s->margin_bottom = 0;
      s->in_list = true;
      s->padding_left = 3;
    }
    if (name_is(name, "blockquote", "dd")) s->margin_left = 4;
    if (name_is(name, "b", "strong", "th")) s->bold = true;
    if (name_is(name, "code", "tt", "kbd", "samp")) s->code = true;
    if (name_is(name, "pre")) s->pre = true;
    if (name_is(name, "u", "ins")) s->underline = true;
    if (name_is(name, "s", "strike", "del")) s->strike = true;
    if (name_is(name, "center")) s->align = 1;
    if (name_is(name, "a") && find_attribute(attrs, "href", &value)) s->underline = true;
    if (find_attribute(attrs, "hidden", &value)) s->display = Display::None;
    if (find_attribute(attrs, "align", &value)) {
      if (value.equals_ignore_case("center") || value.equals_ignore_case("middle")) s->align = 1;
      else if (value.equals_ignore_case("right")) s->align = 2;
      else if (value.equals_ignore_case("left")) s->align = 0;
    }
    uint32_t color;
    if (name_is(name, "font") && find_attribute(attrs, "color", &value) && parse_css_color(value, &color) && color) s->color = color;
    if (name_is(name, "body", "html", "table", "tr", "td", "th") && find_attribute(attrs, "bgcolor", &value) && parse_css_color(value, &color)) {
      s->own_background = true;
      s->own_background_color = color;
    }
  }

  void open_element(StringView name, StringView written, StringView attrs) {
    flush();
    auto_close(name);
    const bool is_void = name_is(name, "br", "hr", "img", "input", "meta", "link", "area", "base", "col", "embed", "source", "track", "wbr", "param");
    if (!is_void && stack_.count() >= kMaxDepth) return;   // (too deep: the element is left out)
    StringView id, classes, inline_style;
    find_attribute(attrs, "id", &id);
    find_attribute(attrs, "class", &classes);
    find_attribute(attrs, "style", &inline_style);
    ElementView view;
    view.tag = written;
    view.id = id;
    view.classes = classes;
    view.attrs = attrs;
    view.index = stack_.count() ? stack_[stack_.count() - 1].children++ : root_children_++;
    Style s = cur().inherit();
    defaults(name, attrs, &s);
    failed_ |= !path_.push_back(view);
    sheet_.apply(path_.view().data(), static_cast<uint32_t>(path_.count()), inline_style, &s);
    const bool is_body = name_is(name, "body", "html");
    if (s.own_background) {
      if (is_body) {
        if (s.own_background_color) doc_.page_background_ = s.own_background_color;
      } else {
        s.background = s.own_background_color;
        s.background_block = s.display != Display::Inline;
      }
    }
    const uint32_t indent_here = static_cast<uint32_t>(cur().indent) + s.margin_left + s.padding_left;
    s.indent = static_cast<uint8_t>(indent_here < kMaxIndent ? indent_here : kMaxIndent);
    Element e;
    e.style = s;
    e.link = cur_link();
    e.none = s.display == Display::None;
    for (uint32_t k = 0; k + 1 < sizeof e.tag && k < name.size(); ++k) e.tag[k] = name[k];
    if (name_is(name, "a")) {
      StringView href;
      String target(doc_.allocator());
      e.link = find_attribute(attrs, "href", &href) && attribute(attrs, "href", &target) && !target.empty() ? doc_.add_link(target.view()) : -1;
    }
    if (!hidden() && !e.none && s.display != Display::Inline) block(static_cast<uint8_t>(1 + s.margin_top + s.padding_top));
    failed_ |= !stack_.push_back(e);
    if (e.none) ++none_;
    if (name_is(name, "head")) head_open_ = true;
    if (name_is(name, "title")) {
      in_title_ = true;
    } else if (name_is(name, "pre")) {
      skip_newline_ = true;
    } else if (name_is(name, "tr")) {
      stack_[stack_.count() - 1].items = 0;
    } else if (name_is(name, "td", "th")) {
      flush();
      uint32_t cell = 0;
      for (uint64_t i = stack_.count() - 1; i > 0; --i)
        if (tag_is(stack_[i - 1], "tr")) {
          cell = stack_[i - 1].items++;
          break;
        }
      if (cell > 0 && !hidden()) {
        chunk_.append(" | ");
        ended_space_ = true;
      }
    } else if (name_is(name, "li") && s.display == Display::ListItem && !s.list_none && !hidden()) {
      block(1);
      String marker(doc_.allocator());
      uint32_t number = 0;
      bool ordered = false;
      for (uint64_t i = stack_.count() - 1; i > 0; --i) {
        const Element& list = stack_[i - 1];
        if (tag_is(list, "ul") || tag_is(list, "ol")) {
          ordered = tag_is(list, "ol");
          number = ++stack_[i - 1].items;
          break;
        }
      }
      if (ordered) {
        marker.append_uint(number);
        marker.append(". ");
      } else {
        marker.append("* ");
      }
      const uint32_t here = s.indent;
      literal(marker.view(), static_cast<uint8_t>(here >= 3 ? here - 3 : 0));
    }
    if (is_void) {
      void_action(name, attrs);
      pop_element();
    }
  }

  void void_action(StringView name, StringView attrs) {
    if (name_is(name, "link")) {
      StringView rel, href, media;
      if (find_attribute(attrs, "rel", &rel) && contains_word(rel, "stylesheet") && !contains_word(rel, "alternate") && find_attribute(attrs, "href", &href) && !href.empty() &&
          (!find_attribute(attrs, "media", &media) || !contains_word(media, "print"))) {
        String target(doc_.allocator());
        if (attribute(attrs, "href", &target)) failed_ |= !doc_.add_stylesheet(target.view());
      }
      return;
    }
    if (hidden()) return;
    if (name_is(name, "br")) {
      flush();
      if (pending_ < 2) ++pending_;
      ended_space_ = true;
    } else if (name_is(name, "hr")) {
      block(2);
      const Style& s = cur();
      failed_ |= !doc_.add_run(StringView(), kRunRule, 0, 0, doc_.runs_.count() == 0 ? 0 : pending_, -1, 0, s.color, 0);
      pending_ = 0;
      block(2);
    } else if (name_is(name, "img")) {
      String alt(doc_.allocator());
      if (!attribute(attrs, "alt", &alt) || alt.empty()) alt.assign("image");
      String shown(doc_.allocator());
      shown.push_back('[');
      for (uint64_t k = 0; k < alt.size(); ++k) shown.push_back(alt[k] >= 32 && alt[k] < 127 ? alt[k] : '?');
      shown.push_back(']');
      if (!ended_space_) literal(" ", indent());
      literal(shown.view(), indent());
    }
  }

  HtmlDocument& doc_;
  IScriptRunner* scripts_;
  uint32_t external_scripts_ = 0, script_depth_ = 0;
  List<String> written_;
  StringView html_;
  String chunk_, title_;
  StyleSheet sheet_;
  List<Element> stack_;
  List<ElementView> path_;
  Style root_;
  uint32_t root_children_ = 0, none_ = 0;
  bool failed_ = false, ended_space_ = true, in_title_ = false, skip_newline_ = false, head_open_ = false;
  uint8_t pending_ = 0;
};

bool HtmlDocument::parse_html(StringView html, StringView extra_css, IScriptRunner* scripts) {
  clear();
  HtmlParser parser(*this, allocator_, scripts);
  if (!parser.run(html, extra_css)) {
    clear();
    return false;
  }
  return true;
}

}  // namespace browser
