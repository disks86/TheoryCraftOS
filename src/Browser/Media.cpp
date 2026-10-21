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

#include "Browser/Media.hpp"

namespace browser {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' || c == '_' || static_cast<uint8_t>(c) >= 0x80; }

bool equals_fold(StringView a, const char* b) {
  const StringView v(b);
  if (a.size() != v.size()) return false;
  for (uint64_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(v[i])) return false;
  return true;
}

enum class Tri : uint8_t { False, True, Unknown };

Tri tri(bool b) { return b ? Tri::True : Tri::False; }
Tri tri_not(Tri t) { return t == Tri::True ? Tri::False : t == Tri::False ? Tri::True : Tri::Unknown; }

// A value of a feature: a number with a unit kind, a ratio, or a keyword.
struct Value {
  enum Kind : uint8_t { None, Number, Length, Resolution, Ratio, Keyword } kind = None;
  int64_t number = 0;        // x1000 (a length in px, a resolution in dppx)
  int64_t denominator = 1;   // ratios: number / denominator
  StringView word;
};

class Evaluator {
 public:
  Evaluator(StringView text, const MediaEnvironment& env) : text_(text), env_(env) {}

  // The whole list: true if any query is true; *valid: false if the text is not a valid list.
  bool list(bool* valid) {
    bool any = false;
    skip();
    if (end()) return true;   // (an empty list applies)
    *valid = true;
    for (;;) {
      Tri t;
      if (!query(&t)) {
        *valid = false;
        // an invalid query is "not all"; the list goes on after the next comma
        skip_to_comma();
      } else if (t == Tri::True) {
        any = true;
      }
      skip();
      if (!end() && cur() == ',') {
        ++pos_;
        continue;
      }
      break;
    }
    if (!end()) *valid = false;
    return any;
  }

 private:
  bool end() const { return pos_ >= text_.size(); }
  char cur() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
  void skip() {
    while (!end()) {
      if (is_space(cur())) ++pos_;
      else if (cur() == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
        const uint64_t e = text_.find("*/", pos_ + 2);
        pos_ = e == StringView::npos ? text_.size() : e + 2;
      } else break;
    }
  }
  void skip_to_comma() {
    int depth = 0;
    while (!end()) {
      const char c = cur();
      if (c == '(') ++depth;
      else if (c == ')' && depth > 0) --depth;
      else if (c == ',' && depth == 0) return;
      ++pos_;
    }
  }
  StringView word() {
    const uint64_t start = pos_;
    while (!end() && is_ident_char(cur())) ++pos_;
    return text_.substr(start, pos_ - start);
  }
  bool accept_word(const char* w) {
    const uint64_t save = pos_;
    skip();
    const StringView got = word();
    if (equals_fold(got, w)) return true;
    pos_ = save;
    return false;
  }

  // media-query: [not|only]? type [and condition]? | condition
  bool query(Tri* out) {
    skip();
    if (end()) return false;
    if (cur() == '(' || (is_word_at("not") && next_is_paren_after("not"))) {
      return condition(out, true);
    }
    bool negate = false;
    if (accept_word("not")) negate = true;
    else accept_word("only");
    skip();
    const StringView type = word();
    if (type.empty()) return false;
    Tri t;
    if (equals_fold(type, "all")) t = Tri::True;
    else if (equals_fold(type, "screen")) t = tri(!env_.print);
    else if (equals_fold(type, "print")) t = tri(env_.print);
    else if (equals_fold(type, "speech") || equals_fold(type, "tty") || equals_fold(type, "tv") || equals_fold(type, "projection") || equals_fold(type, "handheld") ||
             equals_fold(type, "braille") || equals_fold(type, "embossed") || equals_fold(type, "aural"))
      t = Tri::False;
    else return false;   // not a media type (and `and` / `or` / `not` are not allowed as one)
    if (equals_fold(type, "and") || equals_fold(type, "or") || equals_fold(type, "not") || equals_fold(type, "only")) return false;
    skip();
    if (accept_word("and")) {
      Tri c;
      if (!condition(&c, false)) return false;
      if (t == Tri::True && c == Tri::True) t = Tri::True;
      else if (t == Tri::False || c == Tri::False) t = Tri::False;
      else t = Tri::Unknown;
    }
    *out = negate ? tri_not(t) : t;
    return true;
  }

  bool is_word_at(const char* w) {
    const uint64_t save = pos_;
    const StringView got = word();
    pos_ = save;
    return equals_fold(got, w);
  }
  bool next_is_paren_after(const char* w) {
    const uint64_t save = pos_;
    word();
    skip();
    const bool paren = cur() == '(';
    pos_ = save;
    (void)w;
    return paren;
  }

  // condition: not <in-parens> | <in-parens> [and <in-parens>]* | <in-parens> [or <in-parens>]*
  bool condition(Tri* out, bool allow_or) {
    skip();
    if (accept_word("not")) {
      Tri t;
      if (!in_parens(&t)) return false;
      *out = tri_not(t);
      return true;
    }
    Tri t;
    if (!in_parens(&t)) return false;
    skip();
    bool use_and = false, use_or = false;
    for (;;) {
      skip();
      if (accept_word("and")) {
        if (use_or) return false;
        use_and = true;
      } else if (allow_or && accept_word("or")) {
        if (use_and) return false;
        use_or = true;
      } else {
        break;
      }
      Tri next;
      if (!in_parens(&next)) return false;
      if (use_and) t = (t == Tri::True && next == Tri::True) ? Tri::True : (t == Tri::False || next == Tri::False) ? Tri::False : Tri::Unknown;
      else t = (t == Tri::True || next == Tri::True) ? Tri::True : (t == Tri::False && next == Tri::False) ? Tri::False : Tri::Unknown;
    }
    *out = t;
    return true;
  }

  bool in_parens(Tri* out) {
    skip();
    if (cur() != '(') return false;
    ++pos_;
    skip();
    // a nested condition?
    if (cur() == '(' || (is_word_at("not") && next_is_paren_after("not"))) {
      if (!condition(out, true)) return false;
      skip();
      if (cur() != ')') return false;
      ++pos_;
      return true;
    }
    // a feature: read to the matching ")"
    const uint64_t start = pos_;
    int depth = 1;
    while (!end() && depth > 0) {
      const char c = text_[pos_];
      if (c == '(') ++depth;
      else if (c == ')') --depth;
      if (depth > 0) ++pos_;
    }
    if (end()) return false;
    const StringView inside = text_.substr(start, pos_ - start);
    ++pos_;   // )
    *out = feature(inside);
    return true;
  }

  // --- Features
  static bool read_value(StringView s, uint64_t* at, Value* v) {
    *v = Value();
    uint64_t i = *at;
    while (i < s.size() && is_space(s[i])) ++i;
    if (i >= s.size()) return false;
    if (is_digit(s[i]) || s[i] == '.' || ((s[i] == '-' || s[i] == '+') && i + 1 < s.size() && (is_digit(s[i + 1]) || s[i + 1] == '.'))) {
      bool neg = false;
      if (s[i] == '-' || s[i] == '+') neg = s[i++] == '-';
      int64_t whole = 0, frac = 0, scale = 1;
      while (i < s.size() && is_digit(s[i])) whole = whole * 10 + (s[i++] - '0');
      if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && is_digit(s[i])) {
          if (scale < 1000) {
            frac = frac * 10 + (s[i] - '0');
            scale *= 10;
          }
          ++i;
        }
      }
      int64_t n = whole * 1000 + (scale > 1 ? frac * 1000 / scale : 0);
      if (neg) n = -n;
      const uint64_t ustart = i;
      while (i < s.size() && (is_ident_char(s[i]) || s[i] == '%')) ++i;
      const StringView unit = s.substr(ustart, i - ustart);
      v->denominator = 1;
      if (unit.empty()) {
        v->kind = Value::Number;
        v->number = n;
        // a ratio?
        uint64_t j = i;
        while (j < s.size() && is_space(s[j])) ++j;
        if (j < s.size() && s[j] == '/') {
          ++j;
          Value d;
          uint64_t k = j;
          if (!read_value(s, &k, &d) || d.kind != Value::Number || d.number == 0) return false;
          v->kind = Value::Ratio;
          v->denominator = d.number;
          i = k;
        }
      } else if (equals_fold(unit, "px")) { v->kind = Value::Length; v->number = n; }
      else if (equals_fold(unit, "em") || equals_fold(unit, "rem")) { v->kind = Value::Length; v->number = n * 16; }
      else if (equals_fold(unit, "ex") || equals_fold(unit, "ch")) { v->kind = Value::Length; v->number = n * 8; }
      else if (equals_fold(unit, "pt")) { v->kind = Value::Length; v->number = n * 4 / 3; }
      else if (equals_fold(unit, "pc")) { v->kind = Value::Length; v->number = n * 16; }
      else if (equals_fold(unit, "in")) { v->kind = Value::Length; v->number = n * 96; }
      else if (equals_fold(unit, "cm")) { v->kind = Value::Length; v->number = n * 96 * 100 / 254; }
      else if (equals_fold(unit, "mm")) { v->kind = Value::Length; v->number = n * 96 * 10 / 254; }
      else if (equals_fold(unit, "q")) { v->kind = Value::Length; v->number = n * 96 * 10 / 254 / 4; }
      else if (equals_fold(unit, "vw") || equals_fold(unit, "vh") || equals_fold(unit, "vmin") || equals_fold(unit, "vmax")) { v->kind = Value::Length; v->number = n; v->word = unit; }
      else if (equals_fold(unit, "dppx") || equals_fold(unit, "x")) { v->kind = Value::Resolution; v->number = n; }
      else if (equals_fold(unit, "dpi")) { v->kind = Value::Resolution; v->number = n / 96; }
      else if (equals_fold(unit, "dpcm")) { v->kind = Value::Resolution; v->number = n * 254 / 100 / 96; }
      else return false;
      *at = i;
      return true;
    }
    if (is_ident_char(s[i])) {
      const uint64_t start = i;
      while (i < s.size() && is_ident_char(s[i])) ++i;
      v->kind = Value::Keyword;
      v->word = s.substr(start, i - start);
      *at = i;
      return true;
    }
    return false;
  }

  // The comparison of the current value of a feature `have` with `want`: -1, 0, 1.
  static int cmp(int64_t have, int64_t want) { return have < want ? -1 : have > want ? 1 : 0; }

  // Resolves a value that is relative to the viewport into pixels x1000.
  int64_t px(const Value& v) const {
    if (v.word.empty()) return v.number;
    const int64_t w = env_.width, h = env_.height;
    const int64_t base = equals_fold(v.word, "vw") ? w : equals_fold(v.word, "vh") ? h : equals_fold(v.word, "vmin") ? (w < h ? w : h) : (w > h ? w : h);
    return v.number * base / 100;
  }

  // What the feature is now, as a number or a keyword.
  struct Have {
    enum Kind : uint8_t { Unknown, Number, Length, Resolution, Ratio, Keyword } kind = Unknown;
    int64_t number = 0, denominator = 1;
    const char* word = "";
  };

  Have have_of(StringView name) const {
    Have h;
    const int64_t sw = env_.screen_width > 0 ? env_.screen_width : env_.width, sh = env_.screen_height > 0 ? env_.screen_height : env_.height;
    auto length = [&](int64_t v) { h.kind = Have::Length; h.number = v * 1000; };
    auto keyword = [&](const char* w) { h.kind = Have::Keyword; h.word = w; };
    if (equals_fold(name, "width")) length(env_.width);
    else if (equals_fold(name, "height")) length(env_.height);
    else if (equals_fold(name, "device-width")) length(sw);
    else if (equals_fold(name, "device-height")) length(sh);
    else if (equals_fold(name, "aspect-ratio")) { h.kind = Have::Ratio; h.number = env_.width; h.denominator = env_.height > 0 ? env_.height : 1; }
    else if (equals_fold(name, "device-aspect-ratio")) { h.kind = Have::Ratio; h.number = sw; h.denominator = sh > 0 ? sh : 1; }
    else if (equals_fold(name, "orientation")) keyword(env_.height >= env_.width ? "portrait" : "landscape");
    else if (equals_fold(name, "resolution")) { h.kind = Have::Resolution; h.number = static_cast<int64_t>(env_.resolution_dpi) * 1000 / 96; }
    else if (equals_fold(name, "-webkit-device-pixel-ratio") || equals_fold(name, "device-pixel-ratio")) { h.kind = Have::Number; h.number = static_cast<int64_t>(env_.resolution_dpi) * 1000 / 96; }
    else if (equals_fold(name, "color")) { h.kind = Have::Number; h.number = env_.color_bits * 1000; }
    else if (equals_fold(name, "color-index") || equals_fold(name, "monochrome") || equals_fold(name, "grid")) { h.kind = Have::Number; h.number = 0; }
    else if (equals_fold(name, "hover") || equals_fold(name, "any-hover")) keyword("hover");
    else if (equals_fold(name, "pointer") || equals_fold(name, "any-pointer")) keyword("fine");
    else if (equals_fold(name, "prefers-color-scheme")) keyword(env_.dark ? "dark" : "light");
    else if (equals_fold(name, "prefers-reduced-motion")) keyword(env_.reduced_motion ? "reduce" : "no-preference");
    else if (equals_fold(name, "prefers-contrast") || equals_fold(name, "prefers-reduced-data") || equals_fold(name, "prefers-reduced-transparency")) keyword("no-preference");
    else if (equals_fold(name, "forced-colors")) keyword("none");
    else if (equals_fold(name, "inverted-colors")) keyword("none");
    else if (equals_fold(name, "scripting")) keyword(env_.scripting ? "enabled" : "none");
    else if (equals_fold(name, "update")) keyword("fast");
    else if (equals_fold(name, "overflow-block") || equals_fold(name, "overflow-inline")) keyword("scroll");
    else if (equals_fold(name, "scan")) keyword("progressive");
    else if (equals_fold(name, "display-mode")) keyword("browser");
    else if (equals_fold(name, "color-gamut")) keyword("srgb");
    else if (equals_fold(name, "dynamic-range") || equals_fold(name, "video-dynamic-range")) keyword("standard");
    return h;
  }

  // Compares the feature `have` with `want` the way an operator says; false if they cannot be compared.
  bool compare(const Have& have, const Value& want, int* order) const {
    switch (have.kind) {
      case Have::Length:
        if (want.kind == Value::Length) { *order = cmp(have.number, px(want)); return true; }
        if (want.kind == Value::Number && want.number == 0) { *order = cmp(have.number, 0); return true; }
        return false;
      case Have::Number:
        if (want.kind != Value::Number) return false;
        *order = cmp(have.number, want.number);
        return true;
      case Have::Resolution:
        if (want.kind != Value::Resolution) return false;
        *order = cmp(have.number, want.number);
        return true;
      case Have::Ratio:
        if (want.kind != Value::Ratio && !(want.kind == Value::Number)) return false;
        *order = cmp(have.number * want.denominator, want.number * have.denominator);
        return true;
      default: return false;
    }
  }

  // The text between the parentheses of a feature: "name", "name: value", "min-name: value", "a <= name <= b", "name > v".
  Tri feature(StringView s) {
    uint64_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    const uint64_t name_start = i;
    // a range with the value first, or an operator?
    bool has_op = false;
    for (uint64_t k = 0; k < s.size(); ++k)
      if (s[k] == '<' || s[k] == '>' || s[k] == '=') has_op = true;
    if (has_op) return range(s);
    while (i < s.size() && is_ident_char(s[i])) ++i;
    StringView name = s.substr(name_start, i - name_start);
    while (i < s.size() && is_space(s[i])) ++i;
    if (name.empty()) return Tri::Unknown;
    if (i >= s.size()) {   // boolean context: the feature has a value that is not zero / none
      const Have h = have_of(name);
      if (h.kind == Have::Unknown) return Tri::Unknown;
      if (h.kind == Have::Keyword) return tri(!equals_fold(StringView(h.word), "none"));
      return tri(h.number != 0);
    }
    if (s[i] != ':') return Tri::Unknown;
    ++i;
    Value want;
    if (!read_value(s, &i, &want)) return Tri::Unknown;
    while (i < s.size() && is_space(s[i])) ++i;
    if (i < s.size()) return Tri::Unknown;
    int mode = 0;   // 0 equals, -1 max, 1 min
    if (name.size() > 8 && equals_fold(name.first(8), "-webkit-")) name = name.substr(8);
    else if (name.size() > 5 && equals_fold(name.first(5), "-moz-")) name = name.substr(5);
    if (name.size() > 4 && equals_fold(name.first(4), "min-")) { mode = 1; name = name.substr(4); }
    else if (name.size() > 4 && equals_fold(name.first(4), "max-")) { mode = -1; name = name.substr(4); }
    const Have h = have_of(name);
    if (h.kind == Have::Unknown) return Tri::Unknown;
    if (h.kind == Have::Keyword) {
      if (mode != 0 || want.kind != Value::Keyword) return Tri::Unknown;
      if (equals_fold(name, "prefers-color-scheme") || equals_fold(name, "orientation") || equals_fold(name, "hover") || equals_fold(name, "any-hover") || equals_fold(name, "pointer") ||
          equals_fold(name, "any-pointer") || equals_fold(name, "prefers-reduced-motion") || equals_fold(name, "prefers-contrast") || equals_fold(name, "prefers-reduced-data") ||
          equals_fold(name, "prefers-reduced-transparency") || equals_fold(name, "forced-colors") || equals_fold(name, "inverted-colors") || equals_fold(name, "scripting") ||
          equals_fold(name, "update") || equals_fold(name, "overflow-block") || equals_fold(name, "overflow-inline") || equals_fold(name, "scan") || equals_fold(name, "display-mode") ||
          equals_fold(name, "color-gamut") || equals_fold(name, "dynamic-range") || equals_fold(name, "video-dynamic-range")) {
        // the keywords the feature has; pointer / hover have several, "none" is one too
        return tri(equals_fold(want.word, h.word) || (equals_fold(name, "any-hover") && equals_fold(want.word, "hover")) ||
                   ((equals_fold(name, "pointer") || equals_fold(name, "any-pointer")) && equals_fold(want.word, "fine") && equals_fold(StringView(h.word), "fine")));
      }
      return Tri::Unknown;
    }
    int order;
    if (!compare(h, want, &order)) return Tri::Unknown;
    return tri(mode == 0 ? order == 0 : mode > 0 ? order >= 0 : order <= 0);
  }

  // Level 4 ranges: "width >= 600px", "600px <= width", "400px <= width <= 800px".
  Tri range(StringView s) {
    // split into [value op] name [op value]
    struct Piece {
      StringView text;
      int op;   // the operator that follows: 1 <, 2 <=, 3 >, 4 >=, 5 =, 0 none
    } pieces[3];
    uint32_t n = 0;
    uint64_t start = 0;
    for (uint64_t k = 0; k <= s.size() && n < 3; ++k) {
      const bool at_end = k == s.size();
      if (!at_end && s[k] != '<' && s[k] != '>' && s[k] != '=') continue;
      pieces[n].text = s.substr(start, k - start).trimmed();
      int op = 0;
      if (!at_end) {
        const char c = s[k];
        const bool eq = k + 1 < s.size() && s[k + 1] == '=' && c != '=';
        op = c == '<' ? (eq ? 2 : 1) : c == '>' ? (eq ? 4 : 3) : 5;
        k += eq ? 1 : 0;
      }
      pieces[n].op = op;
      ++n;
      start = k + 1;
    }
    if (n < 2 || n > 3) return Tri::Unknown;
    auto check = [&](StringView name, int op, const Value& v, bool name_left) -> Tri {
      const Have h = have_of(name);
      if (h.kind == Have::Unknown || h.kind == Have::Keyword) return Tri::Unknown;
      int order;
      if (!compare(h, v, &order)) return Tri::Unknown;
      if (!name_left) order = -order;   // "value op name" reads the other way round
      switch (op) {
        case 1: return tri(order < 0);
        case 2: return tri(order <= 0);
        case 3: return tri(order > 0);
        case 4: return tri(order >= 0);
        default: return tri(order == 0);
      }
    };
    if (n == 2) {
      Value v;
      uint64_t at = 0;
      // "name op value" or "value op name"
      if (read_value(pieces[1].text, &at, &v) && at == pieces[1].text.size() && v.kind != Value::Keyword) return check(pieces[0].text, pieces[0].op, v, true);
      at = 0;
      if (read_value(pieces[0].text, &at, &v) && at == pieces[0].text.size() && v.kind != Value::Keyword) return check(pieces[1].text, pieces[0].op, v, false);
      return Tri::Unknown;
    }
    // value op name op value
    Value low, high;
    uint64_t a = 0, b = 0;
    if (!read_value(pieces[0].text, &a, &low) || a != pieces[0].text.size() || !read_value(pieces[2].text, &b, &high) || b != pieces[2].text.size()) return Tri::Unknown;
    const Tri x = check(pieces[1].text, pieces[0].op, low, false), y = check(pieces[1].text, pieces[1].op, high, true);
    if (x == Tri::Unknown || y == Tri::Unknown) return Tri::Unknown;
    return tri(x == Tri::True && y == Tri::True);
  }

  StringView text_;
  const MediaEnvironment& env_;
  uint64_t pos_ = 0;
};

}  // namespace

bool media_matches(StringView query, const MediaEnvironment& env) {
  Evaluator e(query, env);
  bool valid = true;
  return e.list(&valid);
}

bool media_valid(StringView query) {
  MediaEnvironment env;
  Evaluator e(query, env);
  bool valid = true;
  e.list(&valid);
  return valid;
}

}  // namespace browser
