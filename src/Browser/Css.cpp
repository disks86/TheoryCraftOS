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

#include "Browser/Css.hpp"

namespace browser {

namespace {

constexpr uint32_t kMaxMatches = 192;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || static_cast<uint8_t>(c) >= 0x80; }
bool is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

// --- Colours ----------------------------------------------------------------------------------------

struct NamedColor {
  const char* name;
  uint32_t rgb;
};
const NamedColor kNamedColors[] = {
    {"black", 0x000000},   {"silver", 0xC0C0C0},  {"gray", 0x808080},    {"grey", 0x808080},   {"white", 0xFFFFFF},  {"maroon", 0x800000},  {"red", 0xFF0000},
    {"purple", 0x800080},  {"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF}, {"green", 0x008000},  {"lime", 0x00FF00},   {"olive", 0x808000},   {"yellow", 0xFFFF00},
    {"navy", 0x000080},    {"blue", 0x0000FF},    {"teal", 0x008080},    {"aqua", 0x00FFFF},   {"cyan", 0x00FFFF},   {"orange", 0xFFA500},  {"brown", 0xA52A2A},
    {"pink", 0xFFC0CB},    {"gold", 0xFFD700},    {"crimson", 0xDC143C}, {"coral", 0xFF7F50},  {"salmon", 0xFA8072}, {"tomato", 0xFF6347},  {"khaki", 0xF0E68C},
    {"indigo", 0x4B0082},  {"violet", 0xEE82EE},  {"turquoise", 0x40E0D0}, {"tan", 0xD2B48C},  {"beige", 0xF5F5DC}, {"ivory", 0xFFFFF0},   {"lavender", 0xE6E6FA},
    {"darkgray", 0xA9A9A9}, {"darkgrey", 0xA9A9A9}, {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"darkblue", 0x00008B}, {"darkgreen", 0x006400}, {"darkred", 0x8B0000}, {"lightblue", 0xADD8E6}, {"lightgreen", 0x90EE90}, {"royalblue", 0x4169E1},
    {"steelblue", 0x4682B4}, {"skyblue", 0x87CEEB}, {"whitesmoke", 0xF5F5F5}, {"gainsboro", 0xDCDCDC}, {"slategray", 0x708090}, {"slategrey", 0x708090},
    {"firebrick", 0xB22222}, {"forestgreen", 0x228B22}, {"orangered", 0xFF4500}, {"seagreen", 0x2E8B57}, {"chocolate", 0xD2691E}, {"hotpink", 0xFF69B4},
};

int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// Reads the numbers of "rgb(1, 2, 3)", "rgb(1 2 3 / 50%)", "hsl(...)": up to four, each with its unit ('%' or none), x100.
uint32_t read_numbers(StringView text, int64_t* values, bool* percent) {
  uint32_t count = 0;
  uint64_t i = 0;
  while (i < text.size() && count < 4) {
    while (i < text.size() && (is_space(text[i]) || text[i] == ',' || text[i] == '/')) ++i;
    if (i >= text.size()) break;
    bool negative = false;
    if (text[i] == '-' || text[i] == '+') negative = text[i++] == '-';
    int64_t whole = 0, fraction = 0, scale = 1;
    bool digits = false;
    while (i < text.size() && is_digit(text[i])) { whole = whole * 10 + (text[i++] - '0'); digits = true; if (whole > 100000) whole = 100000; }
    if (i < text.size() && text[i] == '.') {
      ++i;
      while (i < text.size() && is_digit(text[i])) {
        if (scale < 100) { fraction = fraction * 10 + (text[i] - '0'); scale *= 10; }
        ++i;
      }
    }
    if (!digits && scale == 1) return count;
    int64_t v = whole * 100 + fraction * 100 / scale;
    percent[count] = i < text.size() && text[i] == '%';
    if (percent[count]) ++i;
    while (i < text.size() && is_ident_char(text[i])) ++i;   // (units such as "deg")
    values[count++] = negative ? -v : v;
  }
  return count;
}

uint32_t channel(int64_t value_x100, bool percent) {
  int64_t v = percent ? value_x100 * 255 / 10000 : value_x100 / 100;
  return static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

uint32_t hsl_to_rgb(int64_t h_x100, int64_t s_x100, int64_t l_x100) {
  // all in 0..1000 fixed point
  int64_t h = ((h_x100 / 100) % 360 + 360) % 360, s = s_x100 / 10, l = l_x100 / 10;
  s = s < 0 ? 0 : s > 1000 ? 1000 : s;
  l = l < 0 ? 0 : l > 1000 ? 1000 : l;
  const int64_t c = (1000 - (2 * l - 1000 < 0 ? 1000 - 2 * l : 2 * l - 1000)) * s / 1000;
  const int64_t hp = h * 1000 / 60;   // 0..6000
  int64_t rem = hp % 2000;
  const int64_t x = c * (1000 - (rem > 1000 ? rem - 1000 : 1000 - rem)) / 1000;
  int64_t r = 0, g = 0, b = 0;
  const int64_t sector = hp / 1000;
  switch (sector) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
  }
  const int64_t m = l - c / 2;
  auto to8 = [&](int64_t v) { int64_t k = (v + m) * 255 / 1000; return static_cast<uint32_t>(k < 0 ? 0 : k > 255 ? 255 : k); };
  return (to8(r) << 16) | (to8(g) << 8) | to8(b);
}

// --- Lengths ----------------------------------------------------------------------------------------

enum Unit : uint8_t { kUnitNone, kUnitPx, kUnitEm, kUnitPt, kUnitPercent, kUnitOther };

// A number with a unit; value x100. False if the text is not a number (a keyword such as "auto").
bool parse_length(StringView text, int64_t* value, Unit* unit) {
  uint64_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
  int64_t whole = 0, fraction = 0, scale = 1;
  bool digits = false;
  while (i < text.size() && is_digit(text[i])) { whole = whole * 10 + (text[i++] - '0'); digits = true; if (whole > 1000000) whole = 1000000; }
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && is_digit(text[i])) {
      if (scale < 100) { fraction = fraction * 10 + (text[i] - '0'); scale *= 10; }
      ++i;
      digits = true;
    }
  }
  if (!digits) return false;
  *value = (whole * 100 + fraction * 100 / scale) * (negative ? -1 : 1);
  const StringView rest = text.substr(i);
  if (rest.empty()) *unit = kUnitNone;
  else if (rest.equals_ignore_case("px")) *unit = kUnitPx;
  else if (rest.equals_ignore_case("em") || rest.equals_ignore_case("rem") || rest.equals_ignore_case("ch")) *unit = kUnitEm;
  else if (rest.equals_ignore_case("pt")) *unit = kUnitPt;
  else if (rest.equals("%")) *unit = kUnitPercent;
  else *unit = kUnitOther;
  return true;
}

// A vertical space as blank lines (a line is 16 pixels or an em), at most 3.
uint8_t to_lines(StringView text) {
  int64_t v;
  Unit u;
  if (!parse_length(text, &v, &u) || v <= 0) return 0;
  int64_t lines = 0;
  if (u == kUnitPx) lines = (v + 800) / 1600;
  else if (u == kUnitPt) lines = (v * 4 / 3 + 800) / 1600;
  else if (u == kUnitEm) lines = (v + 50) / 100;
  return static_cast<uint8_t>(lines > 3 ? 3 : lines);
}

// A horizontal space as columns (a column is 8 pixels or half an em), at most 40.
uint8_t to_columns(StringView text) {
  int64_t v;
  Unit u;
  if (!parse_length(text, &v, &u) || v <= 0) return 0;
  int64_t columns = 0;
  if (u == kUnitPx) columns = (v + 400) / 800;
  else if (u == kUnitPt) columns = (v * 4 / 3 + 400) / 800;
  else if (u == kUnitEm) columns = (v * 2 + 50) / 100;
  return static_cast<uint8_t>(columns > 40 ? 40 : columns);
}

// --- Properties -------------------------------------------------------------------------------------

enum Property : uint8_t {
  kNone,
  kColor, kBackgroundColor, kBackground, kFontWeight, kTextDecoration, kTextTransform, kTextAlign, kDisplay, kVisibility, kWhiteSpace,
  kMargin, kMarginTop, kMarginBottom, kMarginLeft, kPadding, kPaddingTop, kPaddingBottom, kPaddingLeft, kListStyle,
};

struct PropertyName {
  const char* name;
  Property property;
};
const PropertyName kProperties[] = {
    {"color", kColor},
    {"background-color", kBackgroundColor},
    {"background", kBackground},
    {"font-weight", kFontWeight},
    {"text-decoration", kTextDecoration},
    {"text-decoration-line", kTextDecoration},
    {"text-transform", kTextTransform},
    {"text-align", kTextAlign},
    {"display", kDisplay},
    {"visibility", kVisibility},
    {"white-space", kWhiteSpace},
    {"margin", kMargin},
    {"margin-top", kMarginTop},
    {"margin-block-start", kMarginTop},
    {"margin-bottom", kMarginBottom},
    {"margin-block-end", kMarginBottom},
    {"margin-left", kMarginLeft},
    {"margin-inline-start", kMarginLeft},
    {"padding", kPadding},
    {"padding-top", kPaddingTop},
    {"padding-block-start", kPaddingTop},
    {"padding-bottom", kPaddingBottom},
    {"padding-block-end", kPaddingBottom},
    {"padding-left", kPaddingLeft},
    {"padding-inline-start", kPaddingLeft},
    {"list-style", kListStyle},
    {"list-style-type", kListStyle},
};

Property property_from_name(StringView name) {
  for (const PropertyName& p : kProperties)
    if (name.equals_ignore_case(p.name)) return p.property;
  return kNone;
}

// The first and following whitespace separated parts of a value (parentheses and quotes keep their insides together).
bool next_part(StringView text, uint64_t* at, StringView* part) {
  uint64_t i = *at;
  while (i < text.size() && is_space(text[i])) ++i;
  if (i >= text.size()) return false;
  const uint64_t start = i;
  int depth = 0;
  char quote = 0;
  while (i < text.size() && (depth > 0 || quote || !is_space(text[i]))) {
    const char c = text[i];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')' && depth > 0) {
      --depth;
    }
    ++i;
  }
  *part = text.substr(start, i - start);
  *at = i;
  return true;
}

// Up to four values of margin or padding: top, right, bottom, left -> top, bottom, left.
void box_values(StringView value, bool vertical_lines, uint8_t* top, uint8_t* bottom, uint8_t* left) {
  StringView parts[4];
  uint32_t count = 0;
  uint64_t at = 0;
  StringView part;
  while (count < 4 && next_part(value, &at, &part)) parts[count++] = part;
  if (count == 0) return;
  StringView t = parts[0], r = parts[count > 1 ? 1 : 0], b = parts[count > 2 ? 2 : 0], l = parts[count > 3 ? 3 : count > 1 ? 1 : 0];
  (void)r;
  (void)vertical_lines;
  *top = to_lines(t);
  *bottom = to_lines(b);
  *left = to_columns(l);
}

void apply_property(Property property, StringView value, Style* style) {
  value = value.trimmed();
  if (value.empty() || value.equals_ignore_case("inherit") || value.equals_ignore_case("initial") || value.equals_ignore_case("unset")) return;
  switch (property) {
    case kColor: {
      uint32_t c;
      if (parse_css_color(value, &c) && c != 0) style->color = c;
      break;
    }
    case kBackgroundColor:
    case kBackground: {
      uint64_t at = 0;
      StringView part;
      while (next_part(value, &at, &part)) {
        uint32_t c;
        if (parse_css_color(part, &c)) {
          style->own_background = true;
          style->own_background_color = c;
          break;
        }
      }
      if (property == kBackgroundColor && !style->own_background) break;
      break;
    }
    case kFontWeight: {
      int64_t v;
      Unit u;
      if (value.equals_ignore_case("bold") || value.equals_ignore_case("bolder")) style->bold = true;
      else if (value.equals_ignore_case("normal") || value.equals_ignore_case("lighter")) style->bold = false;
      else if (parse_length(value, &v, &u)) style->bold = v >= 60000;
      break;
    }
    case kTextDecoration: {
      if (value.equals_ignore_case("none")) {
        style->underline = style->strike = false;
        break;
      }
      uint64_t at = 0;
      StringView part;
      while (next_part(value, &at, &part)) {
        if (part.equals_ignore_case("underline")) style->underline = true;
        else if (part.equals_ignore_case("line-through")) style->strike = true;
      }
      break;
    }
    case kTextTransform:
      style->transform = value.equals_ignore_case("uppercase") ? 1 : value.equals_ignore_case("lowercase") ? 2 : 0;
      break;
    case kTextAlign:
      if (value.equals_ignore_case("center") || value.equals_ignore_case("-webkit-center")) style->align = 1;
      else if (value.equals_ignore_case("right") || value.equals_ignore_case("end")) style->align = 2;
      else if (value.equals_ignore_case("left") || value.equals_ignore_case("start") || value.equals_ignore_case("justify")) style->align = 0;
      break;
    case kDisplay:
      if (value.equals_ignore_case("none")) style->display = Display::None;
      else if (value.equals_ignore_case("list-item")) style->display = Display::ListItem;
      else if (value.equals_ignore_case("inline") || value.equals_ignore_case("inline-block") || value.equals_ignore_case("inline-flex") ||
               value.equals_ignore_case("inline-grid") || value.equals_ignore_case("inline-table") || value.equals_ignore_case("table-cell") ||
               value.equals_ignore_case("contents"))
        style->display = Display::Inline;
      else
        style->display = Display::Block;
      break;
    case kVisibility:
      if (value.equals_ignore_case("hidden") || value.equals_ignore_case("collapse")) style->hidden = true;
      else if (value.equals_ignore_case("visible")) style->hidden = false;
      break;
    case kWhiteSpace:
      if (value.equals_ignore_case("pre") || value.equals_ignore_case("pre-wrap") || value.equals_ignore_case("break-spaces")) style->pre = true;
      else if (value.equals_ignore_case("normal") || value.equals_ignore_case("nowrap") || value.equals_ignore_case("pre-line")) style->pre = false;
      break;
    case kMargin: {
      uint8_t top = 0, bottom = 0, left = 0;
      box_values(value, true, &top, &bottom, &left);
      style->margin_top = top;
      style->margin_bottom = bottom;
      style->margin_left = left;
      break;
    }
    case kPadding: {
      uint8_t top = 0, bottom = 0, left = 0;
      box_values(value, true, &top, &bottom, &left);
      style->padding_top = top;
      style->padding_bottom = bottom;
      style->padding_left = left;
      break;
    }
    case kMarginTop: style->margin_top = to_lines(value); break;
    case kMarginBottom: style->margin_bottom = to_lines(value); break;
    case kMarginLeft: style->margin_left = to_columns(value); break;
    case kPaddingTop: style->padding_top = to_lines(value); break;
    case kPaddingBottom: style->padding_bottom = to_lines(value); break;
    case kPaddingLeft: style->padding_left = to_columns(value); break;
    case kListStyle: {
      uint64_t at = 0;
      StringView part;
      while (next_part(value, &at, &part))
        if (part.equals_ignore_case("none")) style->list_none = true;
      if (!value.contains("none")) style->list_none = false;
      break;
    }
    default: break;
  }
}

// Calls `each(name, value, important)` for the declarations of a block ("a: b; c: d !important").
template <class F>
void for_each_declaration(StringView text, F each) {
  uint64_t i = 0;
  while (i < text.size()) {
    const uint64_t start = i;
    int depth = 0;
    char quote = 0;
    while (i < text.size() && (depth > 0 || quote || text[i] != ';')) {
      const char c = text[i];
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '(') {
        ++depth;
      } else if (c == ')' && depth > 0) {
        --depth;
      }
      ++i;
    }
    StringView declaration = text.substr(start, i - start);
    if (i < text.size()) ++i;
    const uint64_t colon = declaration.find(':');
    if (colon == StringView::npos) continue;
    StringView name = declaration.first(colon).trimmed();
    StringView value = declaration.substr(colon + 1).trimmed();
    bool important = false;
    const uint64_t bang = value.rfind('!');
    if (bang != StringView::npos && value.substr(bang + 1).trimmed().equals_ignore_case("important")) {
      important = true;
      value = value.first(bang).trimmed();
    }
    if (name.empty() || value.empty() || value.contains("var(")) continue;
    each(name, value, important);
  }
}

}  // namespace

bool parse_css_color(StringView text, uint32_t* out) {
  text = text.trimmed();
  if (text.empty()) return false;
  if (text.equals_ignore_case("transparent")) {
    *out = 0;
    return true;
  }
  if (text[0] == '#') {
    const StringView digits = text.substr(1);
    uint32_t v = 0;
    for (uint64_t i = 0; i < digits.size(); ++i) {
      const int d = hex_digit(digits[i]);
      if (d < 0) return false;
      v = (v << 4) | static_cast<uint32_t>(d);
    }
    uint32_t r, g, b, a = 255;
    if (digits.size() == 3 || digits.size() == 4) {
      uint32_t k = digits.size() == 4 ? v : (v << 4) | 0xF;
      r = ((k >> 12) & 0xF) * 17;
      g = ((k >> 8) & 0xF) * 17;
      b = ((k >> 4) & 0xF) * 17;
      a = (k & 0xF) * 17;
    } else if (digits.size() == 6) {
      r = (v >> 16) & 0xFF;
      g = (v >> 8) & 0xFF;
      b = v & 0xFF;
    } else if (digits.size() == 8) {
      r = (v >> 24) & 0xFF;
      g = (v >> 16) & 0xFF;
      b = (v >> 8) & 0xFF;
      a = v & 0xFF;
    } else {
      return false;
    }
    *out = a < 128 ? 0 : (0xFF000000u | (r << 16) | (g << 8) | b);   // (mostly transparent: not shown)
    return true;
  }
  const uint64_t open = text.find('(');
  if (open != StringView::npos && text.ends_with(')')) {
    const StringView function = text.first(open).trimmed();
    const bool rgb = function.equals_ignore_case("rgb") || function.equals_ignore_case("rgba"), hsl = function.equals_ignore_case("hsl") || function.equals_ignore_case("hsla");
    if (!rgb && !hsl) return false;
    int64_t v[4] = {};
    bool pct[4] = {};
    const uint32_t n = read_numbers(text.substr(open + 1, text.size() - open - 2), v, pct);
    if (n < 3) return false;
    if (n == 4 && v[3] < (pct[3] ? 5000 : 50)) {
      *out = 0;
      return true;
    }
    uint32_t rgb_value = rgb ? (channel(v[0], pct[0]) << 16) | (channel(v[1], pct[1]) << 8) | channel(v[2], pct[2]) : hsl_to_rgb(v[0], v[1], v[2]);
    *out = 0xFF000000u | rgb_value;
    return true;
  }
  for (const NamedColor& c : kNamedColors) {
    if (text.equals_ignore_case(c.name)) {
      *out = 0xFF000000u | c.rgb;
      return true;
    }
  }
  return false;
}

bool find_attribute(StringView attrs, StringView name, StringView* value) {
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
      *value = raw;
      return true;
    }
  }
  return false;
}

void StyleSheet::apply_declarations(StringView text, bool important, Style* style) {
  for_each_declaration(text, [&](StringView name, StringView value, bool is_important) {
    if (is_important != important) return;
    const Property p = property_from_name(name);
    if (p != kNone) apply_property(p, value, style);
  });
}

// --- Reading stylesheets ----------------------------------------------------------------------------

void StyleSheet::clear() {
  pool_.clear();
  tests_.clear();
  compounds_.clear();
  selectors_.clear();
  declarations_.clear();
  rules_.clear();
}

namespace {

// Moves `*i` past white space.
void skip_space(StringView css, uint64_t* i) {
  while (*i < css.size() && (is_space(css[*i]) || (css[*i] == '<' && css.substr(*i).starts_with("<!--")) || (css[*i] == '-' && css.substr(*i).starts_with("-->")))) {
    if (css[*i] == '<') *i += 4;
    else if (css[*i] == '-') *i += 3;
    else ++*i;
  }
}

// The text from `*i` up to (not including) the first of `stop1` / `stop2` that is outside brackets and quotes; `*i` ends on it (or at the end).
StringView scan_until(StringView css, uint64_t* i, char stop1, char stop2) {
  const uint64_t start = *i;
  int depth = 0;
  char quote = 0;
  while (*i < css.size()) {
    const char c = css[*i];
    if (quote) {
      if (c == '\\') ++*i;
      else if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if ((c == ')' || c == ']') && depth > 0) {
      --depth;
    } else if (depth == 0 && (c == stop1 || c == stop2)) {
      break;
    }
    ++*i;
  }
  if (*i > css.size()) *i = css.size();
  return css.substr(start, *i - start);
}

// With css[*i] == '{': the text inside the braces; `*i` ends after the closing brace.
StringView take_block(StringView css, uint64_t* i) {
  ++*i;
  const uint64_t start = *i;
  int depth = 1;
  char quote = 0;
  while (*i < css.size()) {
    const char c = css[*i];
    if (quote) {
      if (c == '\\') ++*i;
      else if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}' && --depth == 0) {
      const StringView body = css.substr(start, *i - start);
      ++*i;
      return body;
    }
    ++*i;
  }
  if (*i > css.size()) *i = css.size();
  return css.substr(start, *i - start);
}

// Does an @media query apply to a screen of this size and kind?
bool media_applies(StringView query) {
  static const char* const kNot[] = {"print", "speech", "max-width", "max-device-width", "max-height", "orientation", "prefers-", "hover: none", "hover:none", "pointer: coarse", "pointer:coarse", "-webkit-min-device-pixel-ratio"};
  for (const char* n : kNot) {
    const StringView needle(n);
    for (uint64_t i = 0; i + needle.size() <= query.size(); ++i)
      if (query.substr(i, needle.size()).equals_ignore_case(needle)) return false;
  }
  return true;
}

}  // namespace

bool StyleSheet::parse(StringView css) {
  String clean(allocator_);   // without the comments
  for (uint64_t i = 0; i < css.size();) {
    if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      const uint64_t end = css.find("*/", i + 2);
      i = end == StringView::npos ? css.size() : end + 2;
      if (!clean.push_back(' ')) return false;
    } else {
      if (!clean.push_back(css[i++])) return false;
    }
  }
  uint64_t at = 0;
  return parse_block(clean.view(), &at, false);
}

bool StyleSheet::parse_block(StringView css, uint64_t* at, bool nested) {
  uint64_t& i = *at;
  while (rules_.count() < kMaxRules) {
    skip_space(css, &i);
    if (i >= css.size()) return true;
    if (css[i] == '}') {
      ++i;
      if (nested) return true;
      continue;
    }
    if (css[i] == '@') {
      ++i;
      const uint64_t name_start = i;
      while (i < css.size() && is_ident_char(css[i])) ++i;
      const StringView name = css.substr(name_start, i - name_start);
      const StringView prelude = scan_until(css, &i, '{', ';');
      if (i >= css.size()) return true;
      if (css[i] == ';') {
        ++i;
        continue;
      }
      const bool container = name.equals_ignore_case("media") || name.equals_ignore_case("supports") || name.equals_ignore_case("layer") || name.equals_ignore_case("document");
      if (container && (!name.equals_ignore_case("media") || media_applies(prelude))) {
        ++i;   // (the rules inside are read as if they stood here)
        if (!parse_block(css, &i, true)) return false;
      } else {
        take_block(css, &i);
      }
      continue;
    }
    const StringView prelude = scan_until(css, &i, '{', '}');
    if (i >= css.size() || css[i] != '{') return true;
    const StringView body = take_block(css, &i);
    if (!parse_rule(prelude, body)) return false;
  }
  return true;
}

bool StyleSheet::parse_rule(StringView prelude, StringView body) {
  const uint32_t rule = static_cast<uint32_t>(rules_.count());
  const uint32_t first_selector = static_cast<uint32_t>(selectors_.count());
  if (!add_declarations(body, rule)) return false;
  if (rules_[rule].declaration_count == 0) {
    rules_.remove_at(rule);
    return true;
  }
  uint64_t i = 0;
  while (i <= prelude.size()) {
    uint64_t start = i;
    scan_until(prelude, &i, ',', ',');
    if (!parse_selector(prelude.substr(start, i - start).trimmed(), rule)) return false;
    ++i;
  }
  (void)first_selector;
  return true;
}

bool StyleSheet::add_declarations(StringView body, uint32_t rule) {
  Rule r;
  r.first_declaration = static_cast<uint32_t>(declarations_.count());
  r.declaration_count = 0;
  r.order = rule;
  bool ok = true;
  for_each_declaration(body, [&](StringView name, StringView value, bool important) {
    const Property p = property_from_name(name);
    if (p == kNone || !ok) return;
    Declaration d;
    d.property = p;
    d.important = important;
    d.value_length = static_cast<uint32_t>(value.size());
    ok = keep(value, &d.value_offset) && declarations_.push_back(d);
    ++r.declaration_count;
  });
  return ok && rules_.push_back(r);
}

namespace {

// Reads an identifier at *i.
StringView read_ident(StringView s, uint64_t* i) {
  const uint64_t start = *i;
  while (*i < s.size() && is_ident_char(s[*i])) ++*i;
  return s.substr(start, *i - start);
}

}  // namespace

// Adds the selector in `text` (one of a list) for `rule`. Selectors that cannot apply here (sibling combinators, pseudo-classes that need the pointer
// or the future of the page) are skipped without an error.
bool StyleSheet::parse_selector(StringView text, uint32_t rule) {
  if (text.empty()) return true;
  const uint32_t first_compound = static_cast<uint32_t>(compounds_.count());
  const uint32_t tests_before = static_cast<uint32_t>(tests_.count());
  uint32_t specificity = 0;
  Combinator comb = Combinator::None;
  uint64_t i = 0;
  const uint64_t n = text.size();
  bool ok = true, drop = false;
  while (ok && !drop) {
    bool space = false;
    while (i < n && is_space(text[i])) {
      ++i;
      space = true;
    }
    if (i >= n) break;
    const char c = text[i];
    if (c == '>') {
      if (compounds_.count() == first_compound || comb == Combinator::Child) drop = true;
      comb = Combinator::Child;
      ++i;
      continue;
    }
    if (c == '+' || c == '~') {
      drop = true;
      break;
    }
    if (space && compounds_.count() > first_compound && comb == Combinator::None) comb = Combinator::Descendant;
    Compound compound;
    compound.combinator = compounds_.count() == first_compound ? Combinator::None : comb;
    compound.first_test = static_cast<uint32_t>(tests_.count());
    compound.test_count = 0;
    bool any = false;
    while (i < n && !is_space(text[i]) && text[i] != '>' && text[i] != '+' && text[i] != '~' && ok && !drop) {
      const char k = text[i];
      Test t{};
      bool add = true;
      if (k == '*') {
        ++i;
        add = false;
      } else if (is_ident_start(k)) {
        const StringView name = read_ident(text, &i);
        t.kind = TestKind::Tag;
        ok = keep(name, &t.name_offset);
        t.name_length = static_cast<uint32_t>(name.size());
        specificity += 1;
      } else if (k == '#' || k == '.') {
        ++i;
        const StringView name = read_ident(text, &i);
        if (name.empty()) drop = true;
        t.kind = k == '#' ? TestKind::Id : TestKind::Class;
        ok = ok && keep(name, &t.name_offset);
        t.name_length = static_cast<uint32_t>(name.size());
        specificity += k == '#' ? 10000 : 100;
      } else if (k == '[') {
        const uint64_t close = text.find(']', i);
        if (close == StringView::npos) {
          drop = true;
          break;
        }
        const StringView inside = text.substr(i + 1, close - i - 1);
        i = close + 1;
        uint64_t p = 0;
        while (p < inside.size() && is_space(inside[p])) ++p;
        const StringView name = read_ident(inside, &p);
        while (p < inside.size() && is_space(inside[p])) ++p;
        t.kind = TestKind::Has;
        StringView value;
        if (p < inside.size()) {
          char op = inside[p];
          if (op == '=') {
            t.kind = TestKind::Equals;
            ++p;
          } else if (p + 1 < inside.size() && inside[p + 1] == '=' && (op == '~' || op == '^' || op == '$' || op == '*')) {
            t.kind = op == '~' ? TestKind::Includes : op == '^' ? TestKind::Prefix : op == '$' ? TestKind::Suffix : TestKind::Contains;
            p += 2;
          } else {
            drop = true;
          }
          while (p < inside.size() && is_space(inside[p])) ++p;
          if (p < inside.size() && (inside[p] == '"' || inside[p] == '\'')) {
            const char q = inside[p++];
            const uint64_t start = p;
            while (p < inside.size() && inside[p] != q) ++p;
            value = inside.substr(start, p - start);
          } else {
            const uint64_t start = p;
            while (p < inside.size() && !is_space(inside[p])) ++p;
            value = inside.substr(start, p - start);
          }
        }
        if (name.empty()) drop = true;
        ok = ok && keep(name, &t.name_offset) && keep(value, &t.value_offset);
        t.name_length = static_cast<uint32_t>(name.size());
        t.value_length = static_cast<uint32_t>(value.size());
        specificity += 100;
      } else if (k == ':') {
        ++i;
        if (i < n && text[i] == ':') {
          drop = true;   // (a pseudo-element)
          break;
        }
        const StringView name = read_ident(text, &i);
        if (i < n && text[i] == '(') {
          drop = true;
          break;
        }
        specificity += 100;
        if (name.equals_ignore_case("first-child")) {
          t.kind = TestKind::FirstChild;
        } else if (name.equals_ignore_case("link") || name.equals_ignore_case("visited") || name.equals_ignore_case("any-link")) {
          t.kind = TestKind::Link;
        } else if (name.equals_ignore_case("root")) {
          t.kind = TestKind::Tag;
          ok = keep("html", &t.name_offset);
          t.name_length = 4;
        } else {
          drop = true;
        }
      } else {
        drop = true;   // (an escape or something not understood)
        break;
      }
      if (add && !drop && ok) {
        ok = tests_.push_back(t);
        ++compound.test_count;
      }
      any = true;
    }
    if (!any) drop = true;
    if (compounds_.count() - first_compound >= 32) drop = true;   // (matching recurses once for each: a selector this long is not real)
    if (!drop && ok) ok = compounds_.push_back(compound);
    comb = Combinator::None;
  }
  if (compounds_.count() == first_compound || comb == Combinator::Child) drop = true;
  if (!ok || drop) {
    while (compounds_.count() > first_compound) compounds_.remove_at(compounds_.count() - 1);
    while (tests_.count() > tests_before) tests_.remove_at(tests_.count() - 1);
    return ok;
  }
  Selector selector;
  selector.first_compound = first_compound;
  selector.compound_count = static_cast<uint32_t>(compounds_.count()) - first_compound;
  selector.specificity = specificity;
  selector.rule = rule;
  return selectors_.push_back(selector);
}

namespace {

bool has_class(StringView classes, StringView name) {
  uint64_t i = 0;
  while (i < classes.size()) {
    while (i < classes.size() && is_space(classes[i])) ++i;
    const uint64_t start = i;
    while (i < classes.size() && !is_space(classes[i])) ++i;
    if (i > start && classes.substr(start, i - start).equals(name)) return true;
  }
  return false;
}

}  // namespace

bool StyleSheet::compound_matches(const Compound& compound, const ElementView& element) const {
  for (uint32_t k = 0; k < compound.test_count; ++k) {
    const Test& t = tests_[compound.first_test + k];
    const StringView name = text(t.name_offset, t.name_length), value = text(t.value_offset, t.value_length);
    StringView found;
    switch (t.kind) {
      case TestKind::Tag:
        if (!element.tag.equals_ignore_case(name)) return false;
        break;
      case TestKind::Id:
        if (!element.id.equals(name)) return false;
        break;
      case TestKind::Class:
        if (!has_class(element.classes, name)) return false;
        break;
      case TestKind::Has:
        if (!find_attribute(element.attrs, name, &found)) return false;
        break;
      case TestKind::Equals:
        if (!find_attribute(element.attrs, name, &found) || !found.equals(value)) return false;
        break;
      case TestKind::Includes:
        if (!find_attribute(element.attrs, name, &found) || !has_class(found, value)) return false;
        break;
      case TestKind::Prefix:
        if (!find_attribute(element.attrs, name, &found) || value.empty() || !found.starts_with(value)) return false;
        break;
      case TestKind::Suffix:
        if (!find_attribute(element.attrs, name, &found) || value.empty() || !found.ends_with(value)) return false;
        break;
      case TestKind::Contains:
        if (!find_attribute(element.attrs, name, &found) || value.empty() || !found.contains(value)) return false;
        break;
      case TestKind::FirstChild:
        if (element.index != 0) return false;
        break;
      case TestKind::Link:
        if (!element.tag.equals_ignore_case("a") || !find_attribute(element.attrs, "href", &found)) return false;
        break;
    }
  }
  return true;
}

// Does compound `k` of the selector (counting from its first) match path[index], and the ones before it match the elements before that?
bool StyleSheet::matches(const Selector& selector, uint32_t k, const ElementView* path, uint32_t index) const {
  const Compound& compound = compounds_[selector.first_compound + k];
  if (!compound_matches(compound, path[index])) return false;
  if (k == 0) return true;
  if (compound.combinator == Combinator::Child) return index > 0 && matches(selector, k - 1, path, index - 1);
  for (uint32_t j = index; j > 0; --j)
    if (matches(selector, k - 1, path, j - 1)) return true;
  return false;
}

void StyleSheet::apply(const ElementView* path, uint32_t depth, StringView inline_style, Style* style) const {
  uint32_t matched[kMaxMatches];
  uint32_t count = 0;
  if (depth > 0) {
    for (uint64_t s = 0; s < selectors_.count() && count < kMaxMatches; ++s) {
      const Selector& sel = selectors_[s];
      if (matches(sel, sel.compound_count - 1, path, depth - 1)) {
        uint32_t k = count++;   // (kept ordered by specificity, then by where it came from)
        while (k > 0) {
          const Selector& before = selectors_[matched[k - 1]];
          if (before.specificity < sel.specificity || (before.specificity == sel.specificity && before.rule <= sel.rule)) break;
          matched[k] = matched[k - 1];
          --k;
        }
        matched[k] = static_cast<uint32_t>(s);
      }
    }
  }
  for (int pass = 0; pass < 2; ++pass) {
    const bool important = pass == 1;
    for (uint32_t m = 0; m < count; ++m) {
      const Rule& rule = rules_[selectors_[matched[m]].rule];
      for (uint32_t d = 0; d < rule.declaration_count; ++d) {
        const Declaration& decl = declarations_[rule.first_declaration + d];
        if (decl.important == important) apply_property(static_cast<Property>(decl.property), text(decl.value_offset, decl.value_length), style);
      }
    }
    if (!inline_style.empty()) apply_declarations(inline_style, important, style);
  }
}

}  // namespace browser
