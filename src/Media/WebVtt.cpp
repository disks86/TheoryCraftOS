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
#include "Media/WebVtt.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {

constexpr uint32_t kMaxOps = 4096;        // pieces of text one layout may produce
constexpr uint32_t kMaxDepth = 64;        // nesting of cue text tags
constexpr uint32_t kDefaultBackground = 0xCC000000u;
constexpr uint32_t kDefaultColor = 0xFFFFFFFFu;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// The digits at the front of `s` (consumed), as a number; `count` gets how many there were. False if there were none or the number is too big.
bool take_digits(StringView* s, uint64_t* value, uint32_t* count) {
  uint64_t v = 0;
  uint32_t n = 0;
  while (n < s->size() && is_digit((*s)[n])) {
    if (v > 100000000000ull) return false;
    v = v * 10 + static_cast<uint64_t>((*s)[n] - '0');
    ++n;
  }
  if (n == 0) return false;
  *s = s->without_prefix(n);
  *value = v;
  *count = n;
  return true;
}

// [-]digits[.digits], all of `s`.
bool parse_decimal(StringView s, double* out) {
  bool negative = false;
  if (s.starts_with('-')) {
    negative = true;
    s = s.without_prefix(1);
  }
  uint64_t whole = 0;
  uint32_t n = 0;
  if (!take_digits(&s, &whole, &n)) return false;
  double value = static_cast<double>(whole);
  if (s.starts_with('.')) {
    s = s.without_prefix(1);
    double scale = 0.1;
    uint32_t digits = 0;
    while (!s.empty() && is_digit(s[0])) {
      value += static_cast<double>(s[0] - '0') * scale;
      scale /= 10;
      s = s.without_prefix(1);
      if (++digits > 12) {   // (more digits cannot change a double that matters here; keep scanning for the end)
        while (!s.empty() && is_digit(s[0])) s = s.without_prefix(1);
        break;
      }
    }
    if (digits == 0) return false;
  }
  if (!s.empty()) return false;
  *out = negative ? -value : value;
  return true;
}

// number%, within 0 .. 100.
bool parse_percent(StringView s, double* out) {
  if (!s.ends_with('%')) return false;
  double v;
  if (!parse_decimal(s.without_suffix(1), &v) || v < 0 || v > 100) return false;
  *out = v;
  return true;
}

bool parse_timestamp_prefix(StringView* s, double* seconds) {
  StringView r = *s;
  uint64_t v1, v2, v3 = 0, v4;
  uint32_t n1, n2, n3, n4;
  if (!take_digits(&r, &v1, &n1)) return false;
  const bool hours = n1 != 2 || v1 > 59;
  if (!r.starts_with(':')) return false;
  r = r.without_prefix(1);
  if (!take_digits(&r, &v2, &n2) || n2 != 2) return false;
  uint64_t h = 0, m, sec;
  if (hours || r.starts_with(':')) {
    if (!r.starts_with(':')) return false;
    r = r.without_prefix(1);
    if (!take_digits(&r, &v3, &n3) || n3 != 2) return false;
    h = v1;
    m = v2;
    sec = v3;
  } else {
    m = v1;
    sec = v2;
  }
  if (!r.starts_with('.')) return false;
  r = r.without_prefix(1);
  if (!take_digits(&r, &v4, &n4) || n4 != 3) return false;
  if (m > 59 || sec > 59) return false;
  *seconds = static_cast<double>(h) * 3600.0 + static_cast<double>(m) * 60.0 + static_cast<double>(sec) + static_cast<double>(v4) / 1000.0;
  *s = r;
  return true;
}

bool append_utf8(String* out, uint32_t cp) {
  char b[4];
  uint32_t n;
  if (cp < 0x80) { b[0] = static_cast<char>(cp); n = 1; }
  else if (cp < 0x800) { b[0] = static_cast<char>(0xC0 | (cp >> 6)); b[1] = static_cast<char>(0x80 | (cp & 0x3F)); n = 2; }
  else if (cp < 0x10000) { b[0] = static_cast<char>(0xE0 | (cp >> 12)); b[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); b[2] = static_cast<char>(0x80 | (cp & 0x3F)); n = 3; }
  else { b[0] = static_cast<char>(0xF0 | (cp >> 18)); b[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); b[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); b[3] = static_cast<char>(0x80 | (cp & 0x3F)); n = 4; }
  return out->append(StringView(b, n));
}

// ---------------------------------------------------------------------------------------------------------------- cue timings and settings

void split_setting(StringView token, StringView* name, StringView* value) {
  *name = StringView();
  *value = StringView();
  const uint64_t colon = token.find(':');
  if (colon == StringView::npos || colon == 0 || colon + 1 >= token.size()) return;
  *name = token.substr(0, colon);
  *value = token.substr(colon + 1);
}

bool parse_timings(StringView line, VttCue* cue) {
  auto skip = [&line]() { while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line = line.without_prefix(1); };
  skip();
  double start, end;
  if (!parse_timestamp_prefix(&line, &start)) return false;
  skip();
  if (!line.starts_with("-->")) return false;
  line = line.without_prefix(3);
  skip();
  if (!parse_timestamp_prefix(&line, &end)) return false;
  cue->start = start;
  cue->end = end;
  apply_vtt_settings(line, cue);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------- cue text

enum class Tag : uint8_t { C, I, B, U, V, Lang, Ruby, Rt };

const char* tag_name(Tag t) {
  switch (t) {
    case Tag::C: return "c";
    case Tag::I: return "i";
    case Tag::B: return "b";
    case Tag::U: return "u";
    case Tag::V: return "v";
    case Tag::Lang: return "lang";
    case Tag::Ruby: return "ruby";
    case Tag::Rt: return "rt";
  }
  return "";
}

bool tag_from_name(StringView name, Tag* out) {
  static const Tag kAll[] = {Tag::C, Tag::I, Tag::B, Tag::U, Tag::V, Tag::Lang, Tag::Ruby, Tag::Rt};
  for (Tag t : kAll)
    if (name.equals(tag_name(t))) {
      *out = t;
      return true;
    }
  return false;
}

struct TextVisitor {
  virtual bool open(Tag tag, StringView classes, StringView annotation) = 0;
  virtual bool close(Tag tag) = 0;
  virtual bool text(StringView decoded) = 0;
  virtual bool timestamp(double seconds) = 0;
};

// The character reference at the front of `s` (which starts with '&'): the text it stands for and how many bytes it takes. False if it is not one.
bool take_reference(StringView s, uint32_t* code_point, uint64_t* length) {
  const uint64_t semicolon = s.find(';');
  if (semicolon == StringView::npos || semicolon > 12) return false;
  const StringView name = s.substr(1, semicolon - 1);
  uint32_t cp = 0;
  if (name.equals("amp")) cp = '&';
  else if (name.equals("lt")) cp = '<';
  else if (name.equals("gt")) cp = '>';
  else if (name.equals("lrm")) cp = 0x200E;
  else if (name.equals("rlm")) cp = 0x200F;
  else if (name.equals("nbsp")) cp = 0xA0;
  else if (name.equals("quot")) cp = '"';
  else if (name.equals("apos")) cp = '\'';
  else if (name.size() >= 2 && name[0] == '#') {
    uint64_t v = 0;
    bool hex = name[1] == 'x' || name[1] == 'X';
    StringView digits = name.without_prefix(hex ? 2 : 1);
    if (digits.empty()) return false;
    for (char c : digits) {
      uint32_t d;
      if (is_digit(c)) d = static_cast<uint32_t>(c - '0');
      else if (hex && c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
      else if (hex && c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
      else return false;
      v = v * (hex ? 16 : 10) + d;
      if (v > 0x10FFFF) return false;
    }
    if (v == 0 || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
    cp = static_cast<uint32_t>(v);
  } else {
    return false;
  }
  *code_point = cp;
  *length = semicolon + 1;
  return true;
}

// The cue text parsing of the standard: a tokenizer for text, tags and timestamps, and the tree builder's rules about which end tag closes what.
bool walk_cue_text(allocators::IAllocator& allocator, StringView s, TextVisitor& v) {
  Tag stack[kMaxDepth];
  uint32_t depth = 0;
  String buffer(allocator);
  auto flush = [&]() {
    if (buffer.empty()) return true;
    const bool ok = v.text(buffer.view());
    buffer.clear();
    return ok;
  };
  uint64_t i = 0;
  const uint64_t n = s.size();
  while (i < n) {
    const char c = s[i];
    if (c == '&') {
      uint32_t cp;
      uint64_t length;
      if (take_reference(s.substr(i), &cp, &length)) {
        if (!append_utf8(&buffer, cp)) return false;
        i += length;
      } else {
        if (!buffer.push_back('&')) return false;
        ++i;
      }
      continue;
    }
    if (c != '<') {
      if (!buffer.push_back(c)) return false;
      ++i;
      continue;
    }
    if (!flush()) return false;
    uint64_t close = s.find('>', i + 1);
    const bool terminated = close != StringView::npos;
    if (!terminated) close = n;
    StringView body = s.substr(i + 1, close - i - 1);
    i = terminated ? close + 1 : n;
    if (body.starts_with('/')) {   // end tag
      body = body.without_prefix(1);
      uint64_t e = 0;
      while (e < body.size() && !is_space(body[e]) && body[e] != '.') ++e;
      Tag tag;
      if (!tag_from_name(body.substr(0, e), &tag) || depth == 0) continue;
      if (stack[depth - 1] == tag) {
        --depth;
        if (!v.close(tag)) return false;
      } else if (tag == Tag::Ruby && stack[depth - 1] == Tag::Rt && depth >= 2 && stack[depth - 2] == Tag::Ruby) {
        depth -= 2;
        if (!v.close(Tag::Rt) || !v.close(Tag::Ruby)) return false;
      }
      continue;
    }
    if (!body.empty() && is_digit(body[0])) {   // timestamp
      StringView t = body.trimmed();
      double seconds;
      if (parse_timestamp_prefix(&t, &seconds) && t.empty() && !v.timestamp(seconds)) return false;
      continue;
    }
    uint64_t e = 0;
    while (e < body.size() && !is_space(body[e]) && body[e] != '.') ++e;
    Tag tag;
    if (!tag_from_name(body.substr(0, e), &tag)) continue;
    if (tag == Tag::Rt && !(depth > 0 && stack[depth - 1] == Tag::Ruby)) continue;
    if (depth >= kMaxDepth) continue;
    StringView rest = body.substr(e);
    uint64_t class_end = 0;
    if (rest.starts_with('.')) {
      while (class_end < rest.size() && !is_space(rest[class_end])) ++class_end;
    }
    StringView classes = rest.substr(0, class_end);   // ".a.b"
    if (!classes.empty()) classes = classes.without_prefix(1);
    // the annotation: white space runs become one space, the ends are trimmed
    String annotation(allocator);
    bool pending_space = false;
    for (char a : rest.substr(class_end)) {
      if (is_space(a)) {
        pending_space = !annotation.empty();
        continue;
      }
      if (pending_space && !annotation.push_back(' ')) return false;
      pending_space = false;
      if (!annotation.push_back(a)) return false;
    }
    stack[depth++] = tag;
    if (!v.open(tag, classes, annotation.view())) return false;
  }
  if (!flush()) return false;
  while (depth > 0)
    if (!v.close(stack[--depth])) return false;
  return true;
}

bool append_escaped(String* out, StringView text, bool quote) {
  for (char c : text) {
    const char* r = c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : (quote && c == '"') ? "&quot;" : nullptr;
    if (r ? !out->append(r) : !out->push_back(c)) return false;
  }
  return true;
}

void append_seconds(String* out, double seconds) {
  if (!(seconds >= 0)) seconds = 0;
  if (seconds > 1e12) seconds = 1e12;
  const uint64_t ms = static_cast<uint64_t>(seconds * 1000.0 + 0.5);
  out->append_uint(ms / 1000);
  out->push_back('.');
  const uint64_t frac = ms % 1000;
  if (frac < 100) out->push_back('0');
  if (frac < 10) out->push_back('0');
  out->append_uint(frac);
}

struct HtmlVisitor final : TextVisitor {
  String* out;
  explicit HtmlVisitor(String* o) : out(o) {}
  bool open(Tag tag, StringView classes, StringView annotation) override {
    const bool span = tag == Tag::C || tag == Tag::V || tag == Tag::Lang;
    if (!out->push_back('<') || !out->append(span ? "span" : tag_name(tag))) return false;
    if (tag == Tag::V && (!out->append(" title=\"") || !append_escaped(out, annotation, true) || !out->push_back('"'))) return false;
    if (tag == Tag::Lang && (!out->append(" lang=\"") || !append_escaped(out, annotation, true) || !out->push_back('"'))) return false;
    if (!classes.empty()) {
      if (!out->append(" class=\"")) return false;
      for (char ch : classes)
        if (ch == '.' ? !out->push_back(' ') : !append_escaped(out, StringView(&ch, 1), true)) return false;
      if (!out->push_back('"')) return false;
    }
    return out->push_back('>');
  }
  bool close(Tag tag) override {
    const bool span = tag == Tag::C || tag == Tag::V || tag == Tag::Lang;
    return out->append("</") && out->append(span ? "span" : tag_name(tag)) && out->push_back('>');
  }
  bool text(StringView decoded) override { return append_escaped(out, decoded, false); }
  bool timestamp(double seconds) override {
    if (!out->append("<span data-timestamp=\"")) return false;
    append_seconds(out, seconds);
    return out->append("\"></span>");
  }
};

struct PlainVisitor final : TextVisitor {
  String* out;
  explicit PlainVisitor(String* o) : out(o) {}
  bool open(Tag, StringView, StringView) override { return true; }
  bool close(Tag) override { return true; }
  bool text(StringView decoded) override { return out->append(decoded); }
  bool timestamp(double) override { return true; }
};

// ---------------------------------------------------------------------------------------------------------------- layout

struct Style {
  uint8_t flags = 0;
  uint32_t color = kDefaultColor;
  uint32_t background = kDefaultBackground;
};

struct Run {
  explicit Run(allocators::IAllocator& a) : text(a) {}
  String text;
  Style style;
};

bool named_color(StringView name, uint32_t* rgb) {
  static const struct { const char* name; uint32_t rgb; } kColors[] = {{"white", 0xFFFFFF}, {"lime", 0x00FF00}, {"cyan", 0x00FFFF}, {"red", 0xFF0000},
                                                                       {"yellow", 0xFFFF00}, {"magenta", 0xFF00FF}, {"blue", 0x0000FF}, {"black", 0x000000}};
  for (const auto& c : kColors)
    if (name.equals(c.name)) {
      *rgb = c.rgb;
      return true;
    }
  return false;
}

struct RunVisitor final : TextVisitor {
  allocators::IAllocator& allocator;
  List<UniquePointer<Run>>* runs;
  Style stack[kMaxDepth + 1];
  uint32_t depth = 0;
  RunVisitor(allocators::IAllocator& a, List<UniquePointer<Run>>* r) : allocator(a), runs(r) {}
  bool open(Tag tag, StringView classes, StringView) override {
    Style s = stack[depth];
    if (tag == Tag::I) s.flags |= VttDrawOp::Italic;
    if (tag == Tag::B) s.flags |= VttDrawOp::Bold;
    if (tag == Tag::U) s.flags |= VttDrawOp::Underline;
    StringView rest = classes;
    while (!rest.empty()) {
      const uint64_t dot = rest.find('.');
      const StringView name = dot == StringView::npos ? rest : rest.substr(0, dot);
      rest = dot == StringView::npos ? StringView() : rest.substr(dot + 1);
      uint32_t rgb;
      if (named_color(name, &rgb)) s.color = 0xFF000000u | rgb;
      else if (name.starts_with("bg_") && named_color(name.without_prefix(3), &rgb)) s.background = 0xCC000000u | rgb;
    }
    if (depth + 1 > kMaxDepth) return true;
    stack[++depth] = s;
    return true;
  }
  bool close(Tag) override {
    if (depth > 0) --depth;
    return true;
  }
  bool text(StringView decoded) override {
    auto run = UniquePointer<Run>::make(allocator, allocator);
    if (!run || !run->text.assign(decoded)) return false;
    run->style = stack[depth];
    return runs->push_back(util::move(run));
  }
  bool timestamp(double) override { return true; }
};

struct Piece {
  explicit Piece(allocators::IAllocator& a) : text(a) {}
  String text;
  Style style;
  uint32_t cells = 0;
};

struct Line {
  uint32_t cells = 0;
  List<UniquePointer<Piece>> pieces;
  explicit Line(allocators::IAllocator& a) : pieces(a) {}
};

uint32_t cell_count(StringView s) {
  uint32_t n = 0;
  for (char c : s)
    if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) ++n;
  return n;
}

// The bytes at the front of `s` that make `cells` characters.
uint64_t bytes_for_cells(StringView s, uint32_t cells) {
  uint32_t seen = 0;
  uint64_t i = 0;
  while (i < s.size()) {
    if ((static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) {
      if (seen == cells) return i;
      ++seen;
    }
    ++i;
  }
  return s.size();
}

bool same_style(const Style& a, const Style& b) { return a.flags == b.flags && a.color == b.color && a.background == b.background; }

// Fills lines of at most `max_cells` characters from the runs: breaks at line feeds and between words, and inside a word that is longer than a line.
class Wrapper {
 public:
  Wrapper(allocators::IAllocator& a, uint32_t max_cells, List<UniquePointer<Line>>* lines)
      : allocator_(a), max_(max_cells ? max_cells : 1), lines_(lines), spaces_(a) {}

  bool run(const Run& r) {
    StringView s = r.text.view();
    while (!s.empty()) {
      if (s[0] == '\n') {
        if (!end_line()) return false;
        s = s.without_prefix(1);
        continue;
      }
      uint64_t e = 1;
      const bool space = s[0] == ' ' || s[0] == '\t';
      while (e < s.size() && s[e] != '\n' && ((s[e] == ' ' || s[e] == '\t') == space)) ++e;
      const StringView token = s.substr(0, e);
      s = s.substr(e);
      if (space) {
        if (!line_ || line_->cells == 0) continue;   // (no spaces at the start of a line)
        if (!spaces_.append(token)) return false;
        spaces_style_ = r.style;
        continue;
      }
      if (!word(token, r.style)) return false;
    }
    return true;
  }

  bool finish() { return line_ ? end_line() : true; }

 private:
  bool ensure_line() {
    if (line_) return true;
    auto l = UniquePointer<Line>::make(allocator_, allocator_);
    if (!l) return false;
    line_ = l.get();
    return lines_->push_back(util::move(l));
  }
  bool end_line() {
    if (!ensure_line()) return false;
    line_ = nullptr;
    spaces_.clear();
    return true;
  }
  bool add(StringView text, const Style& style) {
    if (!ensure_line()) return false;
    const uint32_t cells = cell_count(text);
    if (!line_->pieces.empty()) {
      Piece& last = *line_->pieces[line_->pieces.count() - 1];
      if (same_style(last.style, style)) {
        if (!last.text.append(text)) return false;
        last.cells += cells;
        line_->cells += cells;
        return true;
      }
    }
    auto p = UniquePointer<Piece>::make(allocator_, allocator_);
    if (!p || !p->text.assign(text)) return false;
    p->style = style;
    p->cells = cells;
    line_->cells += cells;
    return line_->pieces.push_back(util::move(p));
  }
  bool word(StringView w, const Style& style) {
    uint32_t cells = cell_count(w);
    const uint32_t pending = cell_count(spaces_.view());
    if (line_ && line_->cells != 0 && line_->cells + pending + cells > max_) {
      if (!end_line()) return false;
    }
    if (line_ && line_->cells != 0 && pending != 0) {
      if (!add(spaces_.view(), spaces_style_)) return false;
    }
    spaces_.clear();
    while (cells > max_) {   // longer than a line by itself
      if (line_ && line_->cells != 0 && !end_line()) return false;
      const uint64_t cut = bytes_for_cells(w, max_);
      if (!add(w.first(cut), style) || !end_line()) return false;
      w = w.without_prefix(cut);
      cells = cell_count(w);
    }
    if (cells == 0) return true;
    if (line_ && line_->cells != 0 && line_->cells + cells > max_ && !end_line()) return false;
    return add(w, style);
  }

  allocators::IAllocator& allocator_;
  uint32_t max_;
  List<UniquePointer<Line>>* lines_;
  Line* line_ = nullptr;
  String spaces_;
  Style spaces_style_;
};

struct Rect {
  int32_t x, y, w, h;
};
bool overlaps(const Rect& a, const Rect& b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

struct Prepared {
  explicit Prepared(allocators::IAllocator& a) : lines(a) {}
  const VttCue* cue = nullptr;
  bool placed = false;   // (laid out already, in its region)
  List<UniquePointer<Line>> lines;
};

VttAlign effective_align(VttAlign a) { return a == VttAlign::Start ? VttAlign::Left : a == VttAlign::End ? VttAlign::Right : a; }

bool add_op(allocators::IAllocator& allocator, List<UniquePointer<VttDrawOp>>* out, int32_t x, int32_t y, uint32_t w, uint32_t h, const Style& style, StringView text) {
  if (out->count() >= kMaxOps) return true;
  auto op = UniquePointer<VttDrawOp>::make(allocator, allocator);
  if (!op || !op->text.assign(text)) return false;
  op->x = x;
  op->y = y;
  op->width = w;
  op->height = h;
  op->background = style.background;
  op->color = style.color;
  op->flags = style.flags;
  return out->push_back(util::move(op));
}

// One line, horizontal: starting at `left`, in a box `box_width` pixels wide.
bool emit_line(allocators::IAllocator& allocator, const Line& line, int32_t left, int32_t top, int32_t box_width, VttAlign align, List<UniquePointer<VttDrawOp>>* out) {
  const int32_t line_width = static_cast<int32_t>(line.cells * kVttCellWidth);
  int32_t x = left;
  const VttAlign a = effective_align(align);
  if (a == VttAlign::Center) x += (box_width - line_width) / 2;
  else if (a == VttAlign::Right) x += box_width - line_width;
  for (const auto& p : line.pieces.view()) {
    const int32_t w = static_cast<int32_t>(p->cells * kVttCellWidth);
    if (!add_op(allocator, out, x, top, static_cast<uint32_t>(w), kVttCellHeight, p->style, p->text.view())) return false;
    x += w;
  }
  return true;
}

// One column of a vertical cue: the characters one under the other.
bool emit_column(allocators::IAllocator& allocator, const Line& line, int32_t left, int32_t top, int32_t box_height, VttAlign align, List<UniquePointer<VttDrawOp>>* out) {
  const int32_t length = static_cast<int32_t>(line.cells * kVttCellHeight);
  int32_t y = top;
  const VttAlign a = effective_align(align);
  if (a == VttAlign::Center) y += (box_height - length) / 2;
  else if (a == VttAlign::Right) y += box_height - length;
  for (const auto& p : line.pieces.view()) {
    StringView t = p->text.view();
    while (!t.empty()) {
      uint64_t len = 1;
      while (len < t.size() && (static_cast<uint8_t>(t[len]) & 0xC0) == 0x80) ++len;
      if (!add_op(allocator, out, left + static_cast<int32_t>((kVttCellHeight - kVttCellWidth) / 2), y, kVttCellWidth, kVttCellHeight, p->style, t.first(len))) return false;
      t = t.without_prefix(len);
      y += kVttCellHeight;
    }
  }
  return true;
}

int32_t clamp_i(int32_t v, int32_t lo, int32_t hi) { return hi < lo ? lo : v < lo ? lo : v > hi ? hi : v; }

}  // namespace

bool parse_vtt_timestamp(StringView text, double* seconds) {
  StringView r = text;
  double s;
  if (!parse_timestamp_prefix(&r, &s) || !r.empty()) return false;
  *seconds = s;
  return true;
}

void format_vtt_timestamp(double seconds, String* out) {
  if (!(seconds >= 0)) seconds = 0;
  if (seconds > 1e12) seconds = 1e12;
  const uint64_t ms = static_cast<uint64_t>(seconds * 1000.0 + 0.5);
  const uint64_t h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60, t = ms % 1000;
  auto two = [out](uint64_t v) {
    if (v < 10) out->push_back('0');
    out->append_uint(v);
  };
  if (h > 0) {
    two(h);
    out->push_back(':');
  }
  two(m);
  out->push_back(':');
  two(s);
  out->push_back('.');
  if (t < 100) out->push_back('0');
  if (t < 10) out->push_back('0');
  out->append_uint(t);
}

void apply_vtt_settings(StringView settings, VttCue* cue) {
  StringView rest = settings;
  while (true) {
    while (!rest.empty() && is_space(rest[0])) rest = rest.without_prefix(1);
    if (rest.empty()) break;
    uint64_t e = 0;
    while (e < rest.size() && !is_space(rest[e])) ++e;
    const StringView token = rest.substr(0, e);
    rest = rest.substr(e);
    StringView name, value;
    split_setting(token, &name, &value);
    if (name.empty()) continue;
    if (name.equals("vertical")) {
      if (value.equals("rl")) cue->vertical = VttVertical::GrowingLeft;
      else if (value.equals("lr")) cue->vertical = VttVertical::GrowingRight;
    } else if (name.equals("line")) {
      StringView position = value, align;
      const uint64_t comma = value.find(',');
      if (comma != StringView::npos) {
        position = value.substr(0, comma);
        align = value.substr(comma + 1);
      }
      VttLineAlign line_align = VttLineAlign::Start;
      if (comma != StringView::npos) {
        if (align.equals("start")) line_align = VttLineAlign::Start;
        else if (align.equals("center")) line_align = VttLineAlign::Center;
        else if (align.equals("end")) line_align = VttLineAlign::End;
        else continue;
      }
      double number;
      if (position.ends_with('%')) {
        if (!parse_percent(position, &number)) continue;
        cue->snap_to_lines = false;
      } else {
        if (!parse_decimal(position, &number)) continue;
        cue->snap_to_lines = true;
      }
      cue->line_auto = false;
      cue->line = number;
      cue->line_align = line_align;
    } else if (name.equals("position")) {
      StringView position = value, align;
      const uint64_t comma = value.find(',');
      if (comma != StringView::npos) {
        position = value.substr(0, comma);
        align = value.substr(comma + 1);
      }
      VttPositionAlign position_align = VttPositionAlign::Auto;
      if (comma != StringView::npos) {
        if (align.equals("line-left")) position_align = VttPositionAlign::LineLeft;
        else if (align.equals("center")) position_align = VttPositionAlign::Center;
        else if (align.equals("line-right")) position_align = VttPositionAlign::LineRight;
        else continue;
      }
      double number;
      if (!parse_percent(position, &number)) continue;
      cue->position_auto = false;
      cue->position = number;
      cue->position_align = position_align;
    } else if (name.equals("size")) {
      double number;
      if (parse_percent(value, &number)) cue->size = number;
    } else if (name.equals("align")) {
      if (value.equals("start")) cue->align = VttAlign::Start;
      else if (value.equals("center")) cue->align = VttAlign::Center;
      else if (value.equals("end")) cue->align = VttAlign::End;
      else if (value.equals("left")) cue->align = VttAlign::Left;
      else if (value.equals("right")) cue->align = VttAlign::Right;
    } else if (name.equals("region")) {
      cue->region_id.assign(value);
    }
  }
}

namespace {

void apply_region_settings(StringView text, VttRegion* region) {
  while (!text.empty()) {
    StringView line;
    const uint64_t nl = text.find('\n');
    line = nl == StringView::npos ? text : text.substr(0, nl);
    text = nl == StringView::npos ? StringView() : text.substr(nl + 1);
    StringView rest = line;
    while (true) {
      while (!rest.empty() && is_space(rest[0])) rest = rest.without_prefix(1);
      if (rest.empty()) break;
      uint64_t e = 0;
      while (e < rest.size() && !is_space(rest[e])) ++e;
      const StringView token = rest.substr(0, e);
      rest = rest.substr(e);
      StringView name, value;
      split_setting(token, &name, &value);
      if (name.empty()) continue;
      double a, b;
      if (name.equals("id")) {
        if (!value.contains("-->")) region->id.assign(value);
      } else if (name.equals("width")) {
        if (parse_percent(value, &a)) region->width = a;
      } else if (name.equals("lines")) {
        StringView d = value;
        uint64_t v;
        uint32_t n;
        if (take_digits(&d, &v, &n) && d.empty() && v <= 1000) region->lines = static_cast<uint32_t>(v);
      } else if (name.equals("regionanchor") || name.equals("viewportanchor")) {
        const uint64_t comma = value.find(',');
        if (comma == StringView::npos || !parse_percent(value.substr(0, comma), &a) || !parse_percent(value.substr(comma + 1), &b)) continue;
        if (name.equals("regionanchor")) {
          region->anchor_x = a;
          region->anchor_y = b;
        } else {
          region->viewport_x = a;
          region->viewport_y = b;
        }
      } else if (name.equals("scroll")) {
        if (value.equals("up")) region->scroll_up = true;
      }
    }
  }
}

}  // namespace

bool parse_webvtt(allocators::IAllocator& allocator, StringView text, VttFile* out) {
  // the byte order mark, and one kind of line ending
  if (text.starts_with("\xEF\xBB\xBF")) text = text.without_prefix(3);
  String norm(allocator);
  if (!norm.reserve(text.size())) return false;
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\0') {   // (U+FFFD for a NUL)
      if (!norm.append("\xEF\xBF\xBD")) return false;
    } else if (c == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
      if (!norm.push_back('\n')) return false;
    } else if (!norm.push_back(c)) {
      return false;
    }
  }
  const StringView all = norm.view();
  const uint64_t n = all.size();
  uint64_t pos = 0;
  auto read_line = [&]() {
    const uint64_t nl = all.find('\n', pos);
    const uint64_t end = nl == StringView::npos ? n : nl;
    const StringView line = all.substr(pos, end - pos);
    pos = nl == StringView::npos ? n : nl + 1;
    return line;
  };

  // the signature
  const StringView first = read_line();
  if (!first.starts_with("WEBVTT")) return false;
  if (first.size() > 6 && first[6] != ' ' && first[6] != '\t') return false;
  // the header: up to a blank line (or a cue that follows without one)
  while (pos < n) {
    const uint64_t before = pos;
    const StringView line = read_line();
    if (line.empty()) break;
    if (line.contains("-->")) {
      pos = before;
      break;
    }
  }

  bool seen_cue = false;
  while (pos < n) {
    String buffer(allocator);
    uint32_t line_count = 0;
    uint64_t previous = pos;
    bool seen_arrow = false, style = false, region = false;
    UniquePointer<VttCue> cue;
    for (;;) {
      const StringView line = read_line();
      ++line_count;
      const bool eof = pos >= n;
      if (line.contains("-->")) {
        if (line_count == 1 || (line_count == 2 && !seen_arrow)) {
          seen_arrow = true;
          previous = pos;
          cue = UniquePointer<VttCue>::make(allocator, allocator);
          if (!cue || !cue->id.assign(buffer.view())) return false;
          buffer.clear();
          if (!parse_timings(line, cue.get())) cue.reset();
          else seen_cue = true;
        } else {
          pos = previous;
          break;
        }
      } else if (line.empty()) {
        break;
      } else {
        if (line_count == 2) {
          if (!seen_cue && buffer.view().equals("STYLE")) style = true;
          else if (!seen_cue && buffer.view().equals("REGION")) region = true;
        }
        if (!buffer.empty() && !buffer.push_back('\n')) return false;
        if (!buffer.append(line)) return false;
      }
      previous = pos;
      if (eof) break;
    }
    if (cue) {
      if (!cue->text.assign(buffer.view()) || !out->cues.push_back(util::move(cue))) return false;
    } else if (style) {
      String css(allocator);
      const StringView b = buffer.view();
      if (!css.assign(b.without_prefix(6)) || !out->styles.push_back(util::move(css))) return false;
    } else if (region) {
      auto r = UniquePointer<VttRegion>::make(allocator, allocator);
      if (!r) return false;
      apply_region_settings(buffer.view().without_prefix(7), r.get());
      if (!r->id.empty()) {
        for (uint64_t i = 0; i < out->regions.count(); ++i)
          if (out->regions[i]->id.view().equals(r->id.view())) {
            out->regions.remove_at(i);
            break;
          }
        if (!out->regions.push_back(util::move(r))) return false;
      }
    }
  }
  return true;
}

bool vtt_cue_text_to_html(StringView cue_text, String* out) {
  HtmlVisitor v(out);
  return walk_cue_text(out->allocator(), cue_text, v);
}

bool vtt_cue_text_to_plain(StringView cue_text, String* out) {
  PlainVisitor v(out);
  return walk_cue_text(out->allocator(), cue_text, v);
}

bool layout_vtt_cues(allocators::IAllocator& allocator, ArrayView<const VttCue* const> cues, uint32_t width, uint32_t height, uint32_t bottom_inset,
                     List<UniquePointer<VttDrawOp>>* out) {
  if (width < kVttCellWidth * 4 || height < kVttCellHeight * 2) return true;   // (too small to show anything legible)
  const int32_t W = static_cast<int32_t>(width > 32768 ? 32768 : width);
  const int32_t H0 = static_cast<int32_t>(height > 32768 ? 32768 : height);
  const int32_t H = bottom_inset + kVttCellHeight * 2 > static_cast<uint32_t>(H0) ? H0 : H0 - static_cast<int32_t>(bottom_inset);
  const int32_t line_h = static_cast<int32_t>(kVttCellHeight);
  List<Rect> placed(allocator);

  // the lines of text of each cue, and the regions that cues are rolled up in
  List<UniquePointer<Prepared>> prepared(allocator);
  for (const VttCue* cue : cues) {
    auto p = UniquePointer<Prepared>::make(allocator, allocator);
    if (!p) return false;
    p->cue = cue;
    List<UniquePointer<Run>> runs(allocator);
    RunVisitor visitor(allocator, &runs);
    if (!walk_cue_text(allocator, cue->text.view(), visitor)) return false;
    const bool in_region = cue->has_region && cue->vertical == VttVertical::Horizontal && cue->snap_to_lines && cue->line_auto && cue->size >= 100;
    const uint32_t extent = cue->vertical == VttVertical::Horizontal ? static_cast<uint32_t>(W) : static_cast<uint32_t>(H);
    const double fraction = (in_region ? cue->region.width : cue->size) / 100.0;
    uint32_t max_cells = static_cast<uint32_t>(static_cast<double>(extent) * fraction) / (cue->vertical == VttVertical::Horizontal ? kVttCellWidth : kVttCellHeight);
    if (max_cells > 512) max_cells = 512;
    Wrapper wrapper(allocator, max_cells, &p->lines);
    for (const auto& r : runs.view())
      if (!wrapper.run(*r)) return false;
    if (!wrapper.finish()) return false;
    const uint64_t fits = static_cast<uint64_t>(H / line_h);   // (a cue taller than the video loses its last lines)
    while (p->lines.count() > fits && p->lines.remove_at(p->lines.count() - 1)) {}
    // (a cue of only blanks shows nothing)
    bool any = false;
    for (const auto& l : p->lines.view()) any |= l->cells != 0;
    if (!any) continue;
    if (!prepared.push_back(util::move(p))) return false;
  }

  // regions first: each takes the cues in it, rolled up from the bottom
  List<String> done(allocator);
  for (uint64_t i = 0; i < prepared.count(); ++i) {
    const VttCue& head = *prepared[i]->cue;
    if (prepared[i]->placed) continue;
    const bool in_region = head.has_region && head.vertical == VttVertical::Horizontal && head.snap_to_lines && head.line_auto && head.size >= 100;
    if (!in_region) continue;
    bool seen = false;
    for (const auto& d : done.view()) seen |= d.view().equals(head.region.id.view());
    if (seen) continue;
    String id(allocator);
    if (!id.assign(head.region.id.view()) || !done.push_back(util::move(id))) return false;
    const VttRegion& region = head.region;
    const int32_t rw = static_cast<int32_t>(static_cast<double>(W) * region.width / 100.0);
    const int32_t rh = static_cast<int32_t>(region.lines) * line_h;
    int32_t rx = static_cast<int32_t>(static_cast<double>(W) * region.viewport_x / 100.0 - static_cast<double>(rw) * region.anchor_x / 100.0);
    int32_t ry = static_cast<int32_t>(static_cast<double>(H) * region.viewport_y / 100.0 - static_cast<double>(rh) * region.anchor_y / 100.0);
    rx = clamp_i(rx, 0, W - rw);
    ry = clamp_i(ry, 0, H - rh);
    // the lines that fit, newest last
    struct Row {
      const Line* line;
      VttAlign align;
    };
    List<Row> rows(allocator);
    for (uint64_t j = 0; j < prepared.count(); ++j) {
      const VttCue& c = *prepared[j]->cue;
      if (!(c.has_region && c.region.id.view().equals(region.id.view()) && c.vertical == VttVertical::Horizontal && c.snap_to_lines && c.line_auto && c.size >= 100)) continue;
      for (const auto& l : prepared[j]->lines.view())
        if (!rows.push_back(Row{l.get(), c.align})) return false;
    }
    uint64_t skip = rows.count() > region.lines ? rows.count() - region.lines : 0;
    int32_t y = ry + rh - static_cast<int32_t>((rows.count() - skip) * kVttCellHeight);
    for (uint64_t j = skip; j < rows.count(); ++j) {
      if (!emit_line(allocator, *rows[j].line, rx, y, rw, rows[j].align, out)) return false;
      y += line_h;
    }
    const Rect box = {rx, ry + rh - static_cast<int32_t>((rows.count() - skip) * kVttCellHeight), rw, static_cast<int32_t>((rows.count() - skip) * kVttCellHeight)};
    if (!placed.push_back(box)) return false;
    // (those cues are done)
    for (uint64_t j = 0; j < prepared.count(); ++j) {
      const VttCue& c = *prepared[j]->cue;
      if (c.has_region && c.region.id.view().equals(region.id.view()) && c.vertical == VttVertical::Horizontal && c.snap_to_lines && c.line_auto && c.size >= 100)
        prepared[j]->placed = true;
    }
  }

  for (const auto& pr : prepared.view()) {
    const VttCue* cue = pr->cue;
    if (pr->placed) continue;
    const uint32_t line_count = static_cast<uint32_t>(pr->lines.count());
    VttPositionAlign pa = cue->position_align;
    const VttAlign ea = effective_align(cue->align);
    if (pa == VttPositionAlign::Auto) pa = ea == VttAlign::Left ? VttPositionAlign::LineLeft : ea == VttAlign::Right ? VttPositionAlign::LineRight : VttPositionAlign::Center;
    const double position = cue->position_auto ? (ea == VttAlign::Left ? 0.0 : ea == VttAlign::Right ? 100.0 : 50.0) : cue->position;
    if (cue->vertical == VttVertical::Horizontal) {
      const int32_t bw = static_cast<int32_t>(static_cast<double>(W) * cue->size / 100.0);
      const int32_t bh = static_cast<int32_t>(line_count) * line_h;
      int32_t anchor = static_cast<int32_t>(static_cast<double>(W) * position / 100.0);
      int32_t left = pa == VttPositionAlign::LineLeft ? anchor : pa == VttPositionAlign::LineRight ? anchor - bw : anchor - bw / 2;
      left = clamp_i(left, 0, W - bw);
      int32_t top;
      if (cue->snap_to_lines) {
        const double L = cue->line_auto ? -1.0 : cue->line;
        const int32_t li = static_cast<int32_t>(L);
        top = L >= 0 ? li * line_h : H + (li + 1) * line_h - bh;
        if (cue->line_align == VttLineAlign::Center) top += L >= 0 ? -bh / 2 : bh / 2;
        else if (cue->line_align == VttLineAlign::End) top += L >= 0 ? -bh : bh;
        const int32_t original = top;
        for (uint32_t guard = 0; guard < 64; ++guard) {
          bool moved = false;
          const Rect r = {left, top, bw, bh};
          for (const Rect& other : placed.view()) {
            if (!overlaps(r, other)) continue;
            top = L < 0 ? other.y - bh : other.y + other.h;
            moved = true;
            break;
          }
          if (!moved) break;
        }
        if (top < 0 || top + bh > H) top = original;
      } else {
        const int32_t y = static_cast<int32_t>(static_cast<double>(H) * (cue->line_auto ? 100.0 : cue->line) / 100.0);
        top = cue->line_align == VttLineAlign::Start ? y : cue->line_align == VttLineAlign::Center ? y - bh / 2 : y - bh;
      }
      top = clamp_i(top, 0, H - bh);
      if (!placed.push_back(Rect{left, top, bw, bh})) return false;
      for (uint32_t i = 0; i < line_count; ++i)
        if (!emit_line(allocator, *pr->lines[i], left, top + static_cast<int32_t>(i) * line_h, bw, cue->align, out)) return false;
      continue;
    }
    // vertical: lines are columns, the first at the right (growing left) or the left (growing right)
    const bool rl = cue->vertical == VttVertical::GrowingLeft;
    const int32_t bh = static_cast<int32_t>(static_cast<double>(H) * cue->size / 100.0);
    const int32_t bw = static_cast<int32_t>(line_count) * line_h;
    int32_t anchor = static_cast<int32_t>(static_cast<double>(H) * position / 100.0);
    int32_t top = pa == VttPositionAlign::LineLeft ? anchor : pa == VttPositionAlign::LineRight ? anchor - bh : anchor - bh / 2;
    top = clamp_i(top, 0, H - bh);
    const double L = cue->snap_to_lines ? (cue->line_auto ? -1.0 : cue->line) : 0.0;
    const int32_t li = static_cast<int32_t>(L);
    int32_t left;
    if (!cue->snap_to_lines) {
      const int32_t x = static_cast<int32_t>(static_cast<double>(W) * (cue->line_auto ? 0.0 : cue->line) / 100.0);
      left = rl ? x - bw : x;
    } else if (L >= 0) {
      left = rl ? W - li * line_h - bw : li * line_h;
    } else {
      left = rl ? (-li - 1) * line_h : W + (li + 1) * line_h - bw;
    }
    left = clamp_i(left, 0, W - bw);
    for (uint32_t i = 0; i < line_count; ++i) {
      const int32_t column = rl ? left + bw - static_cast<int32_t>(i + 1) * line_h : left + static_cast<int32_t>(i) * line_h;
      if (!emit_column(allocator, *pr->lines[i], column, top, bh, cue->align, out)) return false;
    }
  }
  return true;
}

}  // namespace media
