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
#include "Utility/Move.hpp"

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
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4}, {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4},
    {"black", 0x000000}, {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A}, {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0},
    {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E}, {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C}, {"cyan", 0x00FFFF},
    {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B}, {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000},
    {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B}, {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F},
    {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22}, {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC},
    {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F}, {"grey", 0x808080},
    {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF},
    {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3}, {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE},
    {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32}, {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970},
    {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000},
    {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98},
    {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F}, {"pink", 0xFFC0CB},
    {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F},
    {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE},
    {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080}, {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0},
    {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
};

int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// Reads the numbers of "rgb(1, 2, 3)", "rgb(1 2 3 / 50%)", "hsl(...)": up to four, each with its unit ('%' or none), x100. `hue_degrees` gets the angle of a
// first number written with an angle unit (deg, grad, rad, turn) converted to degrees.
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
    const uint64_t unit_start = i;
    while (i < text.size() && is_ident_char(text[i])) ++i;   // (units such as "deg")
    const StringView unit = text.substr(unit_start, i - unit_start);
    if (count == 0) {   // an angle (the hue): to degrees
      if (unit.equals_ignore_case("turn")) v *= 360;
      else if (unit.equals_ignore_case("rad")) v = v * 180 * 1000 / 3141;
      else if (unit.equals_ignore_case("grad")) v = v * 9 / 10;
    }
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

// hwb(hue whiteness blackness): the hue's colour mixed with white and black.
uint32_t hwb_to_rgb(int64_t h_x100, int64_t w_x100, int64_t b_x100) {
  int64_t w = w_x100 / 10, b = b_x100 / 10;   // 0..1000
  w = w < 0 ? 0 : w > 1000 ? 1000 : w;
  b = b < 0 ? 0 : b > 1000 ? 1000 : b;
  if (w + b >= 1000) {
    const uint32_t grey = static_cast<uint32_t>(w * 255 / (w + b));
    return (grey << 16) | (grey << 8) | grey;
  }
  const uint32_t pure = hsl_to_rgb(h_x100, 10000, 5000);
  auto mix = [&](uint32_t ch) { return static_cast<uint32_t>((static_cast<int64_t>(ch) * (1000 - w - b) + 255 * w) / 1000); };
  return (mix((pure >> 16) & 0xFF) << 16) | (mix((pure >> 8) & 0xFF) << 8) | mix(pure & 0xFF);
}

// --- Numbers, units and arithmetic ---------------------------------------------------------------------

enum class U : uint8_t { None, Px, Em, Rem, Percent, Vw, Vh, Vmin, Vmax, Ch, Ex, Pt, Pc, In, Cm, Mm, Q, Deg, Rad, Grad, Turn, S, Ms, Fr, Other };

// [+-]digits[.digits][e[+-]digits] at *at.
bool read_double(StringView text, uint64_t* at, double* out) {
  uint64_t i = *at;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
  double value = 0;
  bool digits = false;
  while (i < text.size() && is_digit(text[i])) {
    value = value * 10 + (text[i++] - '0');
    digits = true;
    if (value > 1e15) value = 1e15;
  }
  if (i < text.size() && text[i] == '.') {
    ++i;
    double scale = 0.1;
    while (i < text.size() && is_digit(text[i])) {
      value += (text[i++] - '0') * scale;
      scale *= 0.1;
      digits = true;
    }
  }
  if (!digits) return false;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {   // an exponent (but not the start of the unit "em" / "ex")
    uint64_t k = i + 1;
    bool exp_negative = false;
    if (k < text.size() && (text[k] == '+' || text[k] == '-')) exp_negative = text[k++] == '-';
    if (k < text.size() && is_digit(text[k])) {
      int exponent = 0;
      while (k < text.size() && is_digit(text[k])) exponent = exponent * 10 + (text[k++] - '0'), exponent = exponent > 400 ? 400 : exponent;
      double factor = 1;
      for (int e = 0; e < exponent; ++e) factor *= 10;
      value = exp_negative ? value / factor : value * factor;
      i = k;
    }
  }
  *at = i;
  *out = negative ? -value : value;
  return true;
}

U unit_of(StringView s) {
  if (s.empty()) return U::None;
  if (s.equals("%")) return U::Percent;
  struct {
    const char* name;
    U unit;
  } static const kUnits[] = {{"px", U::Px}, {"em", U::Em}, {"rem", U::Rem}, {"vw", U::Vw}, {"vh", U::Vh}, {"vmin", U::Vmin}, {"vmax", U::Vmax}, {"ch", U::Ch},
                             {"ex", U::Ex}, {"pt", U::Pt}, {"pc", U::Pc}, {"in", U::In}, {"cm", U::Cm}, {"mm", U::Mm}, {"q", U::Q}, {"deg", U::Deg}, {"rad", U::Rad},
                             {"grad", U::Grad}, {"turn", U::Turn}, {"s", U::S}, {"ms", U::Ms}, {"fr", U::Fr}, {"svw", U::Vw}, {"dvw", U::Vw}, {"lvw", U::Vw},
                             {"svh", U::Vh}, {"dvh", U::Vh}, {"lvh", U::Vh}, {"cqw", U::Vw}, {"cqh", U::Vh}};
  for (const auto& u : kUnits)
    if (s.equals_ignore_case(u.name)) return u.unit;
  return U::Other;
}

// A dimension that fills the text: "12.5px", "50%", "1fr", "0".
bool parse_dimension(StringView text, double* v, U* unit) {
  text = text.trimmed();
  uint64_t i = 0;
  if (!read_double(text, &i, v)) return false;
  *unit = unit_of(text.substr(i));
  return *unit != U::Other;
}

// What the arithmetic needs to know to turn units into pixels.
struct EvalContext {
  double font_px = 16, root_px = 16, viewport_w = 640, viewport_h = 480;
  double percent_px = 0;     // what 100% is
  bool has_percent = false;
};

// A value in the middle of a calculation: a number, a length in pixels, a percentage, an angle in degrees, a time in milliseconds or a flex factor.
struct Quantity {
  double v = 0;
  enum Kind : uint8_t { Number, Length, Percent, Angle, Time, Flex } kind = Number;
};

class Arithmetic {
 public:
  Arithmetic(StringView text, const EvalContext& ctx) : text_(text), ctx_(ctx) {}

  // The whole text must be one expression (a calc() / min() / max() / clamp() or a plain dimension).
  bool evaluate(Quantity* out) {
    if (!sum(out, 0)) return false;
    skip();
    return pos_ == text_.size();
  }

 private:
  void skip() {
    while (pos_ < text_.size() && is_space(text_[pos_])) ++pos_;
  }
  bool length_of(Quantity* q) {   // percentages next to lengths become lengths
    if (q->kind != Quantity::Percent) return true;
    if (!ctx_.has_percent) return false;
    q->v = q->v * ctx_.percent_px / 100;
    q->kind = Quantity::Length;
    return true;
  }
  static bool same_kind(Quantity* a, Quantity* b) { return a->kind == b->kind; }
  bool unify(Quantity* a, Quantity* b) {
    if (a->kind != b->kind) {
      if ((a->kind == Quantity::Length && b->kind == Quantity::Percent) || (a->kind == Quantity::Percent && b->kind == Quantity::Length)) {
        if (!length_of(a) || !length_of(b)) return false;
      } else {
        return false;
      }
    }
    return same_kind(a, b);
  }

  bool sum(Quantity* out, int depth) {
    if (depth > 24 || !product(out, depth)) return false;
    for (;;) {
      skip();
      if (pos_ >= text_.size() || (text_[pos_] != '+' && text_[pos_] != '-')) return true;
      const bool add = text_[pos_] == '+';
      ++pos_;
      Quantity rhs;
      if (!product(&rhs, depth)) return false;
      if (!unify(out, &rhs)) return false;
      out->v = add ? out->v + rhs.v : out->v - rhs.v;
    }
  }
  bool product(Quantity* out, int depth) {
    if (!primary(out, depth)) return false;
    for (;;) {
      skip();
      if (pos_ >= text_.size() || (text_[pos_] != '*' && text_[pos_] != '/')) return true;
      const bool multiply = text_[pos_] == '*';
      ++pos_;
      Quantity rhs;
      if (!primary(&rhs, depth)) return false;
      if (multiply) {
        if (out->kind == Quantity::Number) {
          out->kind = rhs.kind;
          out->v *= rhs.v;
        } else if (rhs.kind == Quantity::Number) {
          out->v *= rhs.v;
        } else {
          return false;
        }
      } else {
        if (rhs.kind != Quantity::Number || rhs.v == 0) return false;
        out->v /= rhs.v;
      }
    }
  }
  bool primary(Quantity* out, int depth) {
    skip();
    if (pos_ >= text_.size()) return false;
    const char c = text_[pos_];
    if (c == '(') {
      ++pos_;
      if (!sum(out, depth + 1)) return false;
      skip();
      if (pos_ >= text_.size() || text_[pos_] != ')') return false;
      ++pos_;
      return true;
    }
    if (c == '-' || c == '+') {
      const bool negative = c == '-';
      if (pos_ + 1 < text_.size() && (is_digit(text_[pos_ + 1]) || text_[pos_ + 1] == '.')) return number(out);
      ++pos_;
      if (!primary(out, depth)) return false;
      if (negative) out->v = -out->v;
      return true;
    }
    if (is_digit(c) || c == '.') return number(out);
    if (is_ident_start(c)) return function(out, depth);
    return false;
  }
  bool number(Quantity* out) {
    double v;
    if (!read_double(text_, &pos_, &v)) return false;
    const uint64_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '%') ++pos_;
    else
      while (pos_ < text_.size() && ((text_[pos_] >= 'a' && text_[pos_] <= 'z') || (text_[pos_] >= 'A' && text_[pos_] <= 'Z'))) ++pos_;
    const U unit = unit_of(text_.substr(start, pos_ - start));
    out->v = v;
    switch (unit) {
      case U::None: out->kind = Quantity::Number; return true;
      case U::Px: out->kind = Quantity::Length; return true;
      case U::Em: out->kind = Quantity::Length; out->v = v * ctx_.font_px; return true;
      case U::Rem: out->kind = Quantity::Length; out->v = v * ctx_.root_px; return true;
      case U::Percent: out->kind = Quantity::Percent; return true;
      case U::Vw: out->kind = Quantity::Length; out->v = v * ctx_.viewport_w / 100; return true;
      case U::Vh: out->kind = Quantity::Length; out->v = v * ctx_.viewport_h / 100; return true;
      case U::Vmin: out->kind = Quantity::Length; out->v = v * (ctx_.viewport_w < ctx_.viewport_h ? ctx_.viewport_w : ctx_.viewport_h) / 100; return true;
      case U::Vmax: out->kind = Quantity::Length; out->v = v * (ctx_.viewport_w > ctx_.viewport_h ? ctx_.viewport_w : ctx_.viewport_h) / 100; return true;
      case U::Ch: out->kind = Quantity::Length; out->v = v * ctx_.font_px / 2; return true;   // (the screen's characters are half as wide as they are high)
      case U::Ex: out->kind = Quantity::Length; out->v = v * ctx_.font_px / 2; return true;
      case U::Pt: out->kind = Quantity::Length; out->v = v * 4 / 3; return true;
      case U::Pc: out->kind = Quantity::Length; out->v = v * 16; return true;
      case U::In: out->kind = Quantity::Length; out->v = v * 96; return true;
      case U::Cm: out->kind = Quantity::Length; out->v = v * 96 / 2.54; return true;
      case U::Mm: out->kind = Quantity::Length; out->v = v * 96 / 25.4; return true;
      case U::Q: out->kind = Quantity::Length; out->v = v * 96 / 101.6; return true;
      case U::Deg: out->kind = Quantity::Angle; return true;
      case U::Rad: out->kind = Quantity::Angle; out->v = v * 180 / 3.14159265358979; return true;
      case U::Grad: out->kind = Quantity::Angle; out->v = v * 0.9; return true;
      case U::Turn: out->kind = Quantity::Angle; out->v = v * 360; return true;
      case U::S: out->kind = Quantity::Time; out->v = v * 1000; return true;
      case U::Ms: out->kind = Quantity::Time; return true;
      case U::Fr: out->kind = Quantity::Flex; return true;
      default: return false;
    }
  }
  bool function(Quantity* out, int depth) {
    const uint64_t start = pos_;
    while (pos_ < text_.size() && (is_ident_char(text_[pos_]))) ++pos_;
    const StringView name = text_.substr(start, pos_ - start);
    if (pos_ >= text_.size() || text_[pos_] != '(') return false;
    ++pos_;
    Quantity args[3];
    uint32_t count = 0;
    const bool calc = name.equals_ignore_case("calc") || name.equals_ignore_case("-webkit-calc") || name.equals_ignore_case("-moz-calc");
    for (;;) {
      if (count == 3) return false;
      if (!sum(&args[count++], depth + 1)) return false;
      skip();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      break;
    }
    if (pos_ >= text_.size() || text_[pos_] != ')') return false;
    ++pos_;
    if (calc) {
      if (count != 1) return false;
      *out = args[0];
      return true;
    }
    if (name.equals_ignore_case("min") || name.equals_ignore_case("max")) {
      if (count < 1) return false;
      *out = args[0];
      for (uint32_t i = 1; i < count; ++i) {
        if (!unify(out, &args[i])) return false;
        if (name.equals_ignore_case("min") ? args[i].v < out->v : args[i].v > out->v) out->v = args[i].v;
      }
      return true;
    }
    if (name.equals_ignore_case("clamp")) {
      if (count != 3) return false;
      Quantity lo = args[0], value = args[1], hi = args[2];
      if (!unify(&lo, &value) || !unify(&lo, &hi) || !unify(&value, &hi)) return false;
      double v = value.v;
      if (v > hi.v) v = hi.v;
      if (v < lo.v) v = lo.v;
      *out = value;
      out->v = v;
      return true;
    }
    if (name.equals_ignore_case("abs")) {
      if (count != 1) return false;
      *out = args[0];
      if (out->v < 0) out->v = -out->v;
      return true;
    }
    return false;
  }

  StringView text_;
  const EvalContext& ctx_;
  uint64_t pos_ = 0;
};

// A length (as a number of pixels) from text such as "2em", "calc(100% - 10px)" or "min(50vw, 400px)". Unitless numbers other than 0 are pixels when `lenient`.
bool evaluate_length(StringView text, const EvalContext& ctx, bool lenient, double* px) {
  Quantity q;
  if (!Arithmetic(text.trimmed(), ctx).evaluate(&q)) return false;
  if (q.kind == Quantity::Percent) {
    if (!ctx.has_percent) return false;
    *px = q.v * ctx.percent_px / 100;
    return true;
  }
  if (q.kind == Quantity::Length) {
    *px = q.v;
    return true;
  }
  if (q.kind == Quantity::Number && (q.v == 0 || lenient)) {
    *px = q.v;
    return true;
  }
  return false;
}

// A plain number ("1.5", "calc(1 + 2)").
bool evaluate_number(StringView text, const EvalContext& ctx, double* out) {
  Quantity q;
  if (!Arithmetic(text.trimmed(), ctx).evaluate(&q) || q.kind != Quantity::Number) return false;
  *out = q.v;
  return true;
}

int32_t to_px100(double px) {
  const double v = px * 100;
  if (v > 2e9) return 2000000000;
  if (v < -2e9) return -2000000000;
  return static_cast<int32_t>(v < 0 ? v - 0.5 : v + 0.5);
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
      if (c == '\\') ++i;
      else if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if ((c == ')' || c == ']') && depth > 0) {
      --depth;
    }
    ++i;
  }
  *part = text.substr(start, i - start);
  *at = i;
  return true;
}

// The parts of a comma separated list (commas inside brackets and quotes do not split).
bool next_comma_part(StringView text, uint64_t* at, StringView* part) {
  if (*at > text.size()) return false;
  uint64_t i = *at;
  const uint64_t start = i;
  int depth = 0;
  char quote = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (quote) {
      if (c == '\\') ++i;
      else if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if ((c == ')' || c == ']') && depth > 0) {
      --depth;
    } else if (c == ',' && depth == 0) {
      break;
    }
    ++i;
  }
  *part = text.substr(start, i - start).trimmed();
  *at = i + 1;
  return true;
}

// The parts of a value separated by "/" outside brackets and quotes.
bool next_slash_part(StringView text, uint64_t* at, StringView* part) {
  if (*at > text.size()) return false;
  uint64_t i = *at;
  const uint64_t start = i;
  int depth = 0;
  char quote = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (quote) {
      if (c == '\\') ++i;
      else if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(' || c == '[') {
      ++depth;
    } else if ((c == ')' || c == ']') && depth > 0) {
      --depth;
    } else if (c == '/' && depth == 0) {
      break;
    }
    ++i;
  }
  *part = text.substr(start, i - start).trimmed();
  *at = i + 1;
  return true;
}

// --- Properties -------------------------------------------------------------------------------------

enum Prop : uint16_t {
  pNone, pCustom,
  // longhands
  pColor, pBackgroundColor, pFontSize, pFontWeight, pFontStyle, pLineHeight, pTextAlign, pTextIndent, pTextTransform, pTextDecorationLine, pWhiteSpace, pWordBreak,
  pOverflowWrap, pTextOverflow, pVerticalAlign, pListStyleType, pListStylePosition, pVisibility, pOpacity, pDisplay, pPosition, pTop, pRight, pBottom, pLeft, pZIndex,
  pFloat, pClear, pOverflowX, pOverflowY, pWidth, pHeight, pMinWidth, pMinHeight, pMaxWidth, pMaxHeight, pBoxSizing,
  pMarginTop, pMarginRight, pMarginBottom, pMarginLeft, pPaddingTop, pPaddingRight, pPaddingBottom, pPaddingLeft,
  pBorderTopWidth, pBorderRightWidth, pBorderBottomWidth, pBorderLeftWidth, pBorderTopStyle, pBorderRightStyle, pBorderBottomStyle, pBorderLeftStyle,
  pBorderTopColor, pBorderRightColor, pBorderBottomColor, pBorderLeftColor,
  pFlexGrow, pFlexShrink, pFlexBasis, pFlexDirection, pFlexWrap, pJustifyContent, pAlignItems, pAlignSelf, pAlignContent, pJustifyItems, pJustifySelf, pOrder,
  pRowGap, pColumnGap, pGridTemplateColumns, pGridTemplateRows, pGridTemplateAreas, pGridAutoColumns, pGridAutoRows, pGridAutoFlow, pGridColumnStart, pGridColumnEnd,
  pGridRowStart, pGridRowEnd, pContent, pCounterReset, pCounterIncrement, pColumnCount, pColumnWidth, pBorderCollapse, pBorderSpacing, pTableLayout, pCaptionSide,
  pLastLonghand,
  // shorthands
  pBackground, pFont, pTextDecoration, pListStyle, pMargin, pPadding, pBorder, pBorderTop, pBorderRight, pBorderBottom, pBorderLeft, pBorderWidthAll, pBorderStyleAll,
  pBorderColorAll, pOverflow, pInset, pFlex, pFlexFlow, pGap, pGridColumn, pGridRow, pGridArea, pGridTemplate, pGrid, pPlaceContent, pPlaceItems, pPlaceSelf, pColumns,
};

struct PropertyName {
  const char* name;
  Prop property;
};
const PropertyName kProperties[] = {
    {"color", pColor}, {"background-color", pBackgroundColor}, {"background", pBackground}, {"font-size", pFontSize}, {"font-weight", pFontWeight},
    {"font-style", pFontStyle}, {"font", pFont}, {"line-height", pLineHeight}, {"text-align", pTextAlign}, {"text-indent", pTextIndent},
    {"text-transform", pTextTransform}, {"text-decoration", pTextDecoration}, {"text-decoration-line", pTextDecorationLine}, {"white-space", pWhiteSpace},
    {"word-break", pWordBreak}, {"overflow-wrap", pOverflowWrap}, {"word-wrap", pOverflowWrap}, {"text-overflow", pTextOverflow}, {"vertical-align", pVerticalAlign},
    {"list-style", pListStyle}, {"list-style-type", pListStyleType}, {"list-style-position", pListStylePosition}, {"visibility", pVisibility}, {"opacity", pOpacity},
    {"display", pDisplay}, {"position", pPosition}, {"top", pTop}, {"right", pRight}, {"bottom", pBottom}, {"left", pLeft}, {"inset", pInset},
    {"inset-block-start", pTop}, {"inset-block-end", pBottom}, {"inset-inline-start", pLeft}, {"inset-inline-end", pRight}, {"z-index", pZIndex}, {"float", pFloat},
    {"clear", pClear}, {"overflow", pOverflow}, {"overflow-x", pOverflowX}, {"overflow-y", pOverflowY}, {"width", pWidth}, {"inline-size", pWidth}, {"height", pHeight},
    {"block-size", pHeight}, {"min-width", pMinWidth}, {"min-inline-size", pMinWidth}, {"min-height", pMinHeight}, {"min-block-size", pMinHeight}, {"max-width", pMaxWidth},
    {"max-inline-size", pMaxWidth}, {"max-height", pMaxHeight}, {"max-block-size", pMaxHeight}, {"box-sizing", pBoxSizing}, {"margin", pMargin},
    {"margin-top", pMarginTop}, {"margin-right", pMarginRight}, {"margin-bottom", pMarginBottom}, {"margin-left", pMarginLeft}, {"margin-block-start", pMarginTop},
    {"margin-block-end", pMarginBottom}, {"margin-inline-start", pMarginLeft}, {"margin-inline-end", pMarginRight}, {"padding", pPadding}, {"padding-top", pPaddingTop},
    {"padding-right", pPaddingRight}, {"padding-bottom", pPaddingBottom}, {"padding-left", pPaddingLeft}, {"padding-block-start", pPaddingTop},
    {"padding-block-end", pPaddingBottom}, {"padding-inline-start", pPaddingLeft}, {"padding-inline-end", pPaddingRight}, {"border", pBorder}, {"border-top", pBorderTop},
    {"border-right", pBorderRight}, {"border-bottom", pBorderBottom}, {"border-left", pBorderLeft}, {"border-block-start", pBorderTop}, {"border-block-end", pBorderBottom},
    {"border-inline-start", pBorderLeft}, {"border-inline-end", pBorderRight}, {"border-width", pBorderWidthAll}, {"border-style", pBorderStyleAll},
    {"border-color", pBorderColorAll}, {"border-top-width", pBorderTopWidth}, {"border-right-width", pBorderRightWidth}, {"border-bottom-width", pBorderBottomWidth},
    {"border-left-width", pBorderLeftWidth}, {"border-top-style", pBorderTopStyle}, {"border-right-style", pBorderRightStyle}, {"border-bottom-style", pBorderBottomStyle},
    {"border-left-style", pBorderLeftStyle}, {"border-top-color", pBorderTopColor}, {"border-right-color", pBorderRightColor}, {"border-bottom-color", pBorderBottomColor},
    {"border-left-color", pBorderLeftColor}, {"flex", pFlex}, {"flex-grow", pFlexGrow}, {"flex-shrink", pFlexShrink}, {"flex-basis", pFlexBasis},
    {"flex-direction", pFlexDirection}, {"flex-wrap", pFlexWrap}, {"flex-flow", pFlexFlow}, {"justify-content", pJustifyContent}, {"align-items", pAlignItems},
    {"align-self", pAlignSelf}, {"align-content", pAlignContent}, {"justify-items", pJustifyItems}, {"justify-self", pJustifySelf}, {"place-content", pPlaceContent},
    {"place-items", pPlaceItems}, {"place-self", pPlaceSelf}, {"order", pOrder}, {"gap", pGap}, {"grid-gap", pGap}, {"row-gap", pRowGap}, {"grid-row-gap", pRowGap},
    {"column-gap", pColumnGap}, {"grid-column-gap", pColumnGap}, {"grid-template-columns", pGridTemplateColumns}, {"grid-template-rows", pGridTemplateRows},
    {"grid-template-areas", pGridTemplateAreas}, {"grid-template", pGridTemplate}, {"grid", pGrid}, {"grid-auto-columns", pGridAutoColumns},
    {"grid-auto-rows", pGridAutoRows}, {"grid-auto-flow", pGridAutoFlow}, {"grid-column", pGridColumn}, {"grid-column-start", pGridColumnStart},
    {"grid-column-end", pGridColumnEnd}, {"grid-row", pGridRow}, {"grid-row-start", pGridRowStart}, {"grid-row-end", pGridRowEnd}, {"grid-area", pGridArea},
    {"content", pContent}, {"counter-reset", pCounterReset}, {"counter-increment", pCounterIncrement}, {"columns", pColumns}, {"column-count", pColumnCount},
    {"column-width", pColumnWidth}, {"border-collapse", pBorderCollapse}, {"border-spacing", pBorderSpacing}, {"table-layout", pTableLayout},
    {"caption-side", pCaptionSide},
};

Prop property_from_name(StringView name) {
  if (name.size() > 2 && name[0] == '-' && name[1] == '-') return pCustom;
  for (const PropertyName& p : kProperties)
    if (name.equals_ignore_case(p.name)) return p.property;
  return pNone;
}

// The longhands a shorthand sets (so that inherit / initial / unset can be applied to them).
uint32_t longhands_of(Prop p, Prop* out) {
  uint32_t n = 0;
  auto add = [&](Prop q) { out[n++] = q; };
  auto sides = [&](Prop first) { for (int i = 0; i < 4; ++i) add(static_cast<Prop>(first + i)); };
  switch (p) {
    case pBackground: add(pBackgroundColor); break;
    case pFont: add(pFontStyle); add(pFontWeight); add(pFontSize); add(pLineHeight); break;
    case pTextDecoration: add(pTextDecorationLine); break;
    case pListStyle: add(pListStyleType); add(pListStylePosition); break;
    case pMargin: sides(pMarginTop); break;
    case pPadding: sides(pPaddingTop); break;
    case pBorder: sides(pBorderTopWidth); sides(pBorderTopStyle); sides(pBorderTopColor); break;
    case pBorderTop: add(pBorderTopWidth); add(pBorderTopStyle); add(pBorderTopColor); break;
    case pBorderRight: add(pBorderRightWidth); add(pBorderRightStyle); add(pBorderRightColor); break;
    case pBorderBottom: add(pBorderBottomWidth); add(pBorderBottomStyle); add(pBorderBottomColor); break;
    case pBorderLeft: add(pBorderLeftWidth); add(pBorderLeftStyle); add(pBorderLeftColor); break;
    case pBorderWidthAll: sides(pBorderTopWidth); break;
    case pBorderStyleAll: sides(pBorderTopStyle); break;
    case pBorderColorAll: sides(pBorderTopColor); break;
    case pOverflow: add(pOverflowX); add(pOverflowY); break;
    case pInset: sides(pTop); break;
    case pFlex: add(pFlexGrow); add(pFlexShrink); add(pFlexBasis); break;
    case pFlexFlow: add(pFlexDirection); add(pFlexWrap); break;
    case pGap: add(pRowGap); add(pColumnGap); break;
    case pGridColumn: add(pGridColumnStart); add(pGridColumnEnd); break;
    case pGridRow: add(pGridRowStart); add(pGridRowEnd); break;
    case pGridArea: add(pGridRowStart); add(pGridColumnStart); add(pGridRowEnd); add(pGridColumnEnd); break;
    case pGridTemplate: add(pGridTemplateRows); add(pGridTemplateColumns); add(pGridTemplateAreas); break;
    case pGrid: add(pGridTemplateRows); add(pGridTemplateColumns); add(pGridTemplateAreas); add(pGridAutoRows); add(pGridAutoColumns); add(pGridAutoFlow); break;
    case pPlaceContent: add(pAlignContent); add(pJustifyContent); break;
    case pPlaceItems: add(pAlignItems); add(pJustifyItems); break;
    case pPlaceSelf: add(pAlignSelf); add(pJustifySelf); break;
    case pColumns: add(pColumnCount); add(pColumnWidth); break;
    default: if (p < pLastLonghand) add(p); break;
  }
  return n;
}

// Copies one longhand from another style: what inherit (from the parent) and initial (from a style with the defaults) do.
void copy_property(Prop p, const Style& from, Style* to) {
  switch (p) {
    case pColor: to->color = from.color; break;
    case pBackgroundColor: to->own_background = from.own_background; to->own_background_color = from.own_background_color; break;
    case pFontSize: to->font_px = from.font_px; break;
    case pFontWeight: to->bold = from.bold; break;
    case pFontStyle: to->italic = from.italic; break;
    case pLineHeight: to->line_height_px100 = from.line_height_px100; break;
    case pTextAlign: to->align = from.align; to->justify = from.justify; break;
    case pTextIndent: to->text_indent_px100 = from.text_indent_px100; break;
    case pTextTransform: to->transform = from.transform; break;
    case pTextDecorationLine: to->underline = from.underline; to->strike = from.strike; break;
    case pWhiteSpace: to->white_space = from.white_space; to->pre = from.pre; break;
    case pWordBreak: case pOverflowWrap: to->word_break = from.word_break; break;
    case pTextOverflow: to->text_ellipsis = from.text_ellipsis; break;
    case pVerticalAlign: to->vertical_align = from.vertical_align; break;
    case pListStyleType: to->list_type = from.list_type; to->list_none = from.list_none; break;
    case pListStylePosition: to->list_inside = from.list_inside; break;
    case pVisibility: to->hidden = from.hidden; break;
    case pOpacity: to->opaque_zero = from.opaque_zero; break;
    case pDisplay: to->display = from.display; to->layout = from.layout; to->inline_box = from.inline_box; to->contents = from.contents; break;
    case pPosition: to->position = from.position; break;
    case pTop: to->inset[0] = from.inset[0]; break;
    case pRight: to->inset[1] = from.inset[1]; break;
    case pBottom: to->inset[2] = from.inset[2]; break;
    case pLeft: to->inset[3] = from.inset[3]; break;
    case pZIndex: to->z_index = from.z_index; break;
    case pFloat: to->float_side = from.float_side; break;
    case pClear: to->clear = from.clear; break;
    case pOverflowX: to->overflow_x = from.overflow_x; break;
    case pOverflowY: to->overflow_y = from.overflow_y; break;
    case pWidth: to->width = from.width; break;
    case pHeight: to->height = from.height; break;
    case pMinWidth: to->min_width = from.min_width; break;
    case pMinHeight: to->min_height = from.min_height; break;
    case pMaxWidth: to->max_width = from.max_width; break;
    case pMaxHeight: to->max_height = from.max_height; break;
    case pBoxSizing: to->sizing = from.sizing; break;
    case pMarginTop: case pMarginRight: case pMarginBottom: case pMarginLeft: to->margin[p - pMarginTop] = from.margin[p - pMarginTop]; break;
    case pPaddingTop: case pPaddingRight: case pPaddingBottom: case pPaddingLeft: to->padding[p - pPaddingTop] = from.padding[p - pPaddingTop]; break;
    case pBorderTopWidth: case pBorderRightWidth: case pBorderBottomWidth: case pBorderLeftWidth: to->border_px100[p - pBorderTopWidth] = from.border_px100[p - pBorderTopWidth]; break;
    case pBorderTopStyle: case pBorderRightStyle: case pBorderBottomStyle: case pBorderLeftStyle: to->border_style[p - pBorderTopStyle] = from.border_style[p - pBorderTopStyle]; break;
    case pBorderTopColor: case pBorderRightColor: case pBorderBottomColor: case pBorderLeftColor: to->border_color[p - pBorderTopColor] = from.border_color[p - pBorderTopColor]; break;
    case pFlexGrow: to->flex_grow_x100 = from.flex_grow_x100; break;
    case pFlexShrink: to->flex_shrink_x100 = from.flex_shrink_x100; break;
    case pFlexBasis: to->flex_basis = from.flex_basis; break;
    case pFlexDirection: to->flex_direction = from.flex_direction; break;
    case pFlexWrap: to->flex_wrap = from.flex_wrap; break;
    case pJustifyContent: to->justify_content = from.justify_content; break;
    case pAlignItems: to->align_items = from.align_items; break;
    case pAlignSelf: to->align_self = from.align_self; break;
    case pAlignContent: to->align_content = from.align_content; break;
    case pJustifyItems: to->justify_items = from.justify_items; break;
    case pJustifySelf: to->justify_self = from.justify_self; break;
    case pOrder: to->order = from.order; break;
    case pRowGap: to->row_gap = from.row_gap; break;
    case pColumnGap: to->column_gap = from.column_gap; break;
    case pGridTemplateColumns: to->grid_template_columns = from.grid_template_columns; break;
    case pGridTemplateRows: to->grid_template_rows = from.grid_template_rows; break;
    case pGridTemplateAreas: to->grid_template_areas = from.grid_template_areas; break;
    case pGridAutoColumns: to->grid_auto_columns = from.grid_auto_columns; break;
    case pGridAutoRows: to->grid_auto_rows = from.grid_auto_rows; break;
    case pGridAutoFlow: to->grid_dense = from.grid_dense; to->grid_column_flow = from.grid_column_flow; break;
    case pGridColumnStart: to->grid_column_start = from.grid_column_start; break;
    case pGridColumnEnd: to->grid_column_end = from.grid_column_end; break;
    case pGridRowStart: to->grid_row_start = from.grid_row_start; break;
    case pGridRowEnd: to->grid_row_end = from.grid_row_end; break;
    case pContent: to->content = from.content; to->has_content = from.has_content; break;
    case pCounterReset: to->counter_reset = from.counter_reset; break;
    case pCounterIncrement: to->counter_increment = from.counter_increment; break;
    case pColumnCount: to->column_count = from.column_count; break;
    case pColumnWidth: to->column_width = from.column_width; break;
    case pBorderCollapse: to->border_collapse = from.border_collapse; break;
    case pBorderSpacing: to->border_spacing = from.border_spacing; break;
    case pTableLayout: to->table_fixed = from.table_fixed; break;
    case pCaptionSide: to->caption_bottom = from.caption_bottom; break;
    default: break;
  }
}

bool inherited_property(Prop p) {
  switch (p) {
    case pColor: case pFontSize: case pFontWeight: case pFontStyle: case pLineHeight: case pTextAlign: case pTextIndent: case pTextTransform: case pTextDecorationLine:
    case pWhiteSpace: case pWordBreak: case pOverflowWrap: case pListStyleType: case pListStylePosition: case pVisibility: case pBorderCollapse: case pBorderSpacing:
    case pCaptionSide:
      return true;
    default: return false;
  }
}

bool eq(StringView a, const char* b) { return a.equals_ignore_case(StringView(b)); }

struct KeywordValue {
  const char* name;
  int value;
};
bool keyword(StringView text, const KeywordValue* table, uint32_t count, int* out) {
  for (uint32_t i = 0; i < count; ++i)
    if (eq(text, table[i].name)) {
      *out = table[i].value;
      return true;
    }
  return false;
}
#define KEYWORD(text, table, out) keyword(text, table, sizeof(table) / sizeof(table[0]), out)

const KeywordValue kPlaceKeywords[] = {
    {"normal", static_cast<int>(Place::Normal)}, {"start", static_cast<int>(Place::Start)}, {"end", static_cast<int>(Place::End)}, {"center", static_cast<int>(Place::Center)},
    {"stretch", static_cast<int>(Place::Stretch)}, {"baseline", static_cast<int>(Place::Baseline)}, {"first", static_cast<int>(Place::Baseline)},
    {"last", static_cast<int>(Place::Baseline)}, {"space-between", static_cast<int>(Place::SpaceBetween)}, {"space-around", static_cast<int>(Place::SpaceAround)},
    {"space-evenly", static_cast<int>(Place::SpaceEvenly)}, {"flex-start", static_cast<int>(Place::FlexStart)}, {"flex-end", static_cast<int>(Place::FlexEnd)},
    {"left", static_cast<int>(Place::Left)}, {"right", static_cast<int>(Place::Right)}, {"self-start", static_cast<int>(Place::Start)},
    {"self-end", static_cast<int>(Place::End)}, {"auto", static_cast<int>(Place::Auto)}, {"legacy", static_cast<int>(Place::Normal)}};

// The parts of a value that is a list of keywords, one of which is the place (skipping "safe", "unsafe", "first", "last" qualifiers).
bool place_value(StringView value, Place* out) {
  uint64_t at = 0;
  StringView part;
  bool found = false;
  while (next_part(value, &at, &part)) {
    if (eq(part, "safe") || eq(part, "unsafe")) continue;
    int v;
    if (!KEYWORD(part, kPlaceKeywords, &v)) return false;
    *out = static_cast<Place>(v);
    found = true;
    if (!(eq(part, "first") || eq(part, "last"))) break;   // ("first baseline": the baseline)
  }
  return found;
}

}  // namespace

// --- Applying declarations ---------------------------------------------------------------------------

// Works out declared values against one element's style (a friend of StyleSheet: it reads the custom property scopes).
struct CssApply {
  const StyleSheet& sheet;
  const Style& parent;
  Style* style;
  EvalContext eval;
  const Style initial;   // the style with every property at its initial value

  CssApply(const StyleSheet& s, const Style& p, Style* st) : sheet(s), parent(p), style(st) { refresh(); }

  void refresh() {
    eval.font_px = style->font_px;
    eval.root_px = 16;
    eval.viewport_w = sheet.viewport_w_;
    eval.viewport_h = sheet.viewport_h_;
    eval.percent_px = style->content_px;
    eval.has_percent = true;
  }

  // --- custom properties
  bool lookup(StringView name, StringView* value) const {
    uint32_t at = style->custom_scope;
    for (uint32_t guard = 0; at != 0 && guard < kMaxCustomScopes; ++guard) {
      const StyleSheet::Scope& scope = sheet.scopes_[at - 1];
      TextRef name_ref, value_ref;
      name_ref.offset = scope.name_offset;
      name_ref.length = scope.name_length;
      value_ref.offset = scope.value_offset;
      value_ref.length = scope.value_length;
      if (sheet.text(name_ref).equals(name)) {
        *value = sheet.text(value_ref);
        return true;
      }
      at = scope.parent;
    }
    return false;
  }

  // `text` with every var(--name, fallback) replaced; false if one has no value and no fallback.
  bool substitute(StringView text, String* out, int depth) const {
    if (depth > 12) return false;
    uint64_t i = 0;
    char quote = 0;
    while (i < text.size()) {
      const char c = text[i];
      if (quote) {
        out->push_back(c);
        if (c == '\\' && i + 1 < text.size()) out->push_back(text[++i]);
        else if (c == quote) quote = 0;
        ++i;
        continue;
      }
      if (c == '"' || c == '\'') {
        quote = c;
        out->push_back(c);
        ++i;
        continue;
      }
      if ((c == 'v' || c == 'V') && text.substr(i).size() >= 4 && text.substr(i, 4).equals_ignore_case("var(") && (i == 0 || !is_ident_char(text[i - 1]))) {
        // find the matching ")"
        uint64_t k = i + 4;
        int nesting = 1;
        char inner_quote = 0;
        while (k < text.size() && nesting > 0) {
          const char d = text[k];
          if (inner_quote) {
            if (d == '\\') ++k;
            else if (d == inner_quote) inner_quote = 0;
          } else if (d == '"' || d == '\'') {
            inner_quote = d;
          } else if (d == '(') {
            ++nesting;
          } else if (d == ')') {
            --nesting;
          }
          ++k;
        }
        if (nesting != 0) return false;
        const StringView body = text.substr(i + 4, k - 1 - (i + 4));
        const uint64_t comma = [&] {
          int n = 0;
          for (uint64_t j = 0; j < body.size(); ++j) {
            if (body[j] == '(') ++n;
            else if (body[j] == ')') --n;
            else if (body[j] == ',' && n == 0) return j;
          }
          return StringView::npos;
        }();
        const StringView name = (comma == StringView::npos ? body : body.first(comma)).trimmed();
        StringView value;
        if (lookup(name, &value)) {
          String inner(sheet.allocator_);
          if (!substitute(value, &inner, depth + 1)) return false;
          out->append(inner.view());
        } else if (comma != StringView::npos) {
          if (!substitute(body.substr(comma + 1).trimmed(), out, depth + 1)) return false;
        } else {
          return false;
        }
        i = k;
        continue;
      }
      out->push_back(c);
      ++i;
    }
    return true;
  }

  // --- values
  bool parse_length(StringView v, bool vertical, Length* out, bool lenient = true) const {
    v = v.trimmed();
    if (eq(v, "auto")) { *out = Length(); return true; }
    if (eq(v, "none")) { *out = Length::of(Length::None); return true; }
    if (eq(v, "min-content") || eq(v, "-webkit-min-content")) { *out = Length::of(Length::MinContent); return true; }
    if (eq(v, "max-content") || eq(v, "-webkit-max-content")) { *out = Length::of(Length::MaxContent); return true; }
    if (eq(v, "fit-content") || eq(v, "-webkit-fit-content") || eq(v, "stretch") || eq(v, "-webkit-fill-available") || eq(v, "-moz-available")) {
      *out = eq(v, "fit-content") || eq(v, "-webkit-fit-content") ? Length::of(Length::FitContent) : Length();
      return true;
    }
    if (v.size() > 12 && v.first(12).equals_ignore_case("fit-content(")) { *out = Length::of(Length::FitContent); return true; }
    EvalContext c = eval;
    if (vertical) c.has_percent = false;
    double px;
    if (!evaluate_length(v, c, lenient, &px)) {
      // a percentage of something that is not known (a height) is automatic
      if (vertical) {
        double dummy;
        U unit;
        if ((parse_dimension(v, &dummy, &unit) && unit == U::Percent) || v.contains('%')) { *out = Length(); return true; }
      }
      return false;
    }
    *out = Length::px(to_px100(px));
    return true;
  }

  bool parse_color(StringView v, uint32_t* out) const {
    if (eq(v.trimmed(), "currentcolor")) { *out = style->color; return true; }
    return parse_css_color(v, out);
  }

  // --- applying
  void declare(Prop p, StringView value) {
    value = value.trimmed();
    if (value.empty() || p == pNone) return;
    String substituted(sheet.allocator_);
    if (value.contains("var(") || value.contains("VAR(")) {
      if (!substitute(value, &substituted, 0)) {   // a variable without a value: the property is unset
        apply_wide(p, 2);
        return;
      }
      value = substituted.view().trimmed();
      if (value.empty()) { apply_wide(p, 2); return; }
    }
    if (eq(value, "inherit")) apply_wide(p, 0);
    else if (eq(value, "initial")) apply_wide(p, 1);
    else if (eq(value, "unset")) apply_wide(p, 2);
    else if (eq(value, "revert") || eq(value, "revert-layer")) apply_wide(p, 2);
    else apply_value(p, value);
    if (p == pFontSize || p == pFont || p == pDisplay) refresh();
  }

  // inherit (0), initial (1) and unset (2) for a property or all the longhands of a shorthand.
  void apply_wide(Prop p, int which) {
    Prop list[16];
    const uint32_t n = longhands_of(p, list);
    for (uint32_t i = 0; i < n; ++i) {
      const bool from_parent = which == 0 || (which == 2 && inherited_property(list[i]));
      copy_property(list[i], from_parent ? parent : initial, style);
      if (list[i] == pFontSize) {
        if (from_parent) style->font_px = parent.font_px;
        else style->font_px = 16;
      }
    }
    if (p == pDisplay) finish_display_defaults();
    refresh();
  }

  void finish_display_defaults() {}

  // Sets a side of margin / padding / inset from a shorthand's list (1 to 4 values).
  void four_sides(StringView value, Prop first, bool vertical_percent_ok) {
    (void)vertical_percent_ok;
    StringView parts[4];
    uint32_t n = 0;
    uint64_t at = 0;
    StringView part;
    while (n < 4 && next_part(value, &at, &part)) parts[n++] = part;
    if (n == 0) return;
    const StringView t = parts[0], r = n > 1 ? parts[1] : parts[0], b = n > 2 ? parts[2] : parts[0], l = n > 3 ? parts[3] : (n > 1 ? parts[1] : parts[0]);
    apply_value(static_cast<Prop>(first + 0), t);
    apply_value(static_cast<Prop>(first + 1), r);
    apply_value(static_cast<Prop>(first + 2), b);
    apply_value(static_cast<Prop>(first + 3), l);
  }

  bool border_style_of(StringView v, BorderStyle* out) const {
    static const KeywordValue kStyles[] = {{"none", static_cast<int>(BorderStyle::None)}, {"hidden", static_cast<int>(BorderStyle::Hidden)}, {"dotted", static_cast<int>(BorderStyle::Dotted)},
                                           {"dashed", static_cast<int>(BorderStyle::Dashed)}, {"solid", static_cast<int>(BorderStyle::Solid)}, {"double", static_cast<int>(BorderStyle::Double)},
                                           {"groove", static_cast<int>(BorderStyle::Groove)}, {"ridge", static_cast<int>(BorderStyle::Ridge)}, {"inset", static_cast<int>(BorderStyle::Inset)},
                                           {"outset", static_cast<int>(BorderStyle::Outset)}};
    int k;
    if (!KEYWORD(v, kStyles, &k)) return false;
    *out = static_cast<BorderStyle>(k);
    return true;
  }

  bool border_width_of(StringView v, int32_t* px100) const {
    if (eq(v, "thin")) { *px100 = 100; return true; }
    if (eq(v, "medium")) { *px100 = 300; return true; }
    if (eq(v, "thick")) { *px100 = 500; return true; }
    Length l;
    EvalContext c = eval;
    c.has_percent = false;
    double px;
    if (!evaluate_length(v, c, true, &px) || px < 0) return false;
    *px100 = to_px100(px);
    return true;
  }

  // border / border-top ...: width, style and colour in any order; what is not given goes back to its initial value.
  void border_shorthand(StringView value, int side_first, int side_count) {
    int32_t width = 300;
    BorderStyle bstyle = BorderStyle::None;
    uint32_t color = 0;
    uint64_t at = 0;
    StringView part;
    while (next_part(value, &at, &part)) {
      BorderStyle bs;
      int32_t w;
      uint32_t c;
      if (border_style_of(part, &bs)) bstyle = bs;
      else if (border_width_of(part, &w)) width = w;
      else if (parse_color(part, &c)) color = c;
    }
    for (int i = side_first; i < side_first + side_count; ++i) {
      style->border_px100[i] = width;
      style->border_style[i] = bstyle;
      style->border_color[i] = color;
    }
  }

  void set_display(StringView v) {
    uint64_t at = 0;
    StringView a, b;
    next_part(v, &at, &a);
    const bool two = next_part(v, &at, &b);
    if (two) {   // "inline flex", "block grid", "inline flow-root" ...
      const bool a_outside = eq(a, "block") || eq(a, "inline") || eq(a, "run-in");
      const StringView outside = a_outside ? a : b, inside = a_outside ? b : a;
      const bool inline_outside = eq(outside, "inline");
      if (eq(inside, "flex")) return display_kind(inline_outside ? "inline-flex" : "flex");
      if (eq(inside, "grid")) return display_kind(inline_outside ? "inline-grid" : "grid");
      if (eq(inside, "table")) return display_kind(inline_outside ? "inline-table" : "table");
      if (eq(inside, "flow-root")) return display_kind(inline_outside ? "inline-block" : "flow-root");
      return display_kind(inline_outside ? "inline" : "block");
    }
    display_kind(a);
  }

  void display_kind(StringView v) {
    style->contents = false;
    style->inline_box = false;
    style->layout = Layout::Flow;
    if (eq(v, "none")) style->display = Display::None;
    else if (eq(v, "contents")) { style->display = Display::Inline; style->contents = true; }
    else if (eq(v, "block") || eq(v, "flow-root") || eq(v, "flow")) style->display = Display::Block;
    else if (eq(v, "inline") || eq(v, "run-in") || eq(v, "ruby") || eq(v, "ruby-text") || eq(v, "ruby-base")) style->display = Display::Inline;
    else if (eq(v, "list-item")) style->display = Display::ListItem;
    else if (eq(v, "inline-block")) { style->display = Display::Inline; style->inline_box = true; }
    else if (eq(v, "flex") || eq(v, "-webkit-box") || eq(v, "-webkit-flex") || eq(v, "-ms-flexbox")) { style->display = Display::Block; style->layout = Layout::Flex; }
    else if (eq(v, "inline-flex") || eq(v, "-webkit-inline-box") || eq(v, "-webkit-inline-flex") || eq(v, "-ms-inline-flexbox")) { style->display = Display::Inline; style->layout = Layout::Flex; style->inline_box = true; }
    else if (eq(v, "grid") || eq(v, "-ms-grid")) { style->display = Display::Block; style->layout = Layout::Grid; }
    else if (eq(v, "inline-grid")) { style->display = Display::Inline; style->layout = Layout::Grid; style->inline_box = true; }
    else if (eq(v, "table")) { style->display = Display::Block; style->layout = Layout::Table; }
    else if (eq(v, "inline-table")) { style->display = Display::Inline; style->layout = Layout::Table; style->inline_box = true; }
    else if (eq(v, "table-row-group") || eq(v, "table-header-group") || eq(v, "table-footer-group")) { style->display = Display::Block; style->layout = Layout::TableRowGroup; }
    else if (eq(v, "table-row")) { style->display = Display::Block; style->layout = Layout::TableRow; }
    else if (eq(v, "table-cell")) { style->display = Display::Block; style->layout = Layout::TableCell; }
    else if (eq(v, "table-caption")) { style->display = Display::Block; style->layout = Layout::TableCaption; }
    else if (eq(v, "table-column") || eq(v, "table-column-group")) style->display = Display::None;
    else style->display = Display::Block;
  }

  void set_font_size(StringView v) {
    static const KeywordValue kSizes[] = {{"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
    int k;
    double px;
    if (KEYWORD(v, kSizes, &k)) px = k;
    else if (eq(v, "smaller")) px = parent.font_px * 5.0 / 6;
    else if (eq(v, "larger")) px = parent.font_px * 1.2;
    else {
      EvalContext c = eval;
      c.font_px = parent.font_px;   // (em in font-size is relative to the parent's size)
      c.percent_px = parent.font_px;
      c.has_percent = true;
      if (!evaluate_length(v, c, false, &px) || px < 0) return;
    }
    if (px < 1) px = 1;
    if (px > 2000) px = 2000;
    style->font_px = static_cast<uint16_t>(px + 0.5);
  }

  bool font_weight_of(StringView v, bool* bold) const {
    double n;
    if (eq(v, "bold") || eq(v, "bolder")) { *bold = true; return true; }
    if (eq(v, "normal") || eq(v, "lighter")) { *bold = false; return true; }
    if (evaluate_number(v, eval, &n)) { *bold = n >= 600; return true; }
    return false;
  }

  void set_line_height(StringView v) {
    double n;
    if (eq(v, "normal")) { style->line_height_px100 = -1; return; }
    if (evaluate_number(v, eval, &n) && n >= 0) { style->line_height_px100 = to_px100(n * style->font_px); return; }
    EvalContext c = eval;
    c.percent_px = style->font_px;
    double px;
    if (evaluate_length(v, c, false, &px) && px >= 0) style->line_height_px100 = to_px100(px);
  }

  void set_text_decoration(StringView v) {
    if (eq(v, "none")) { style->underline = style->strike = false; return; }
    uint64_t at = 0;
    StringView part;
    bool any = false;
    bool underline = false, strike = false;
    while (next_part(v, &at, &part)) {
      if (eq(part, "underline")) { underline = true; any = true; }
      else if (eq(part, "line-through")) { strike = true; any = true; }
      else if (eq(part, "overline") || eq(part, "blink")) any = true;
    }
    if (any) { style->underline = underline; style->strike = strike; }
  }

  void set_list_style(StringView v, bool type_only) {
    static const KeywordValue kTypes[] = {{"disc", 0}, {"circle", 1}, {"square", 2}, {"decimal", 3}, {"lower-alpha", 4}, {"lower-latin", 4}, {"upper-alpha", 5}, {"upper-latin", 5},
                                          {"lower-roman", 6}, {"upper-roman", 7}, {"none", 8}, {"decimal-leading-zero", 9}, {"lower-greek", 4}, {"disclosure-open", 0},
                                          {"disclosure-closed", 0}};
    uint64_t at = 0;
    StringView part;
    bool type_set = false;
    while (next_part(v, &at, &part)) {
      int k;
      if (KEYWORD(part, kTypes, &k)) {
        style->list_type = static_cast<uint8_t>(k);
        type_set = true;
      } else if (!type_only && eq(part, "inside")) style->list_inside = true;
      else if (!type_only && eq(part, "outside")) style->list_inside = false;
      else if (part.contains('"') || part.contains('\'')) { style->list_type = 0; type_set = true; }   // (a string marker: shown as a bullet)
    }
    if (!type_only && !type_set) style->list_type = style->list_type == 8 ? 0 : style->list_type;
    style->list_none = style->list_type == 8;
  }

  void keep_text(StringView v, TextRef* ref) const { *ref = sheet.keep_text(v); }

  // grid-column: 1 / 3, grid-row: span 2, grid-area: a / b / c / d
  void grid_lines(StringView v, TextRef* start, TextRef* end) const {
    const uint64_t slash = [&] {
      int depth = 0;
      for (uint64_t i = 0; i < v.size(); ++i) {
        if (v[i] == '(') ++depth;
        else if (v[i] == ')') --depth;
        else if (v[i] == '/' && depth == 0) return i;
      }
      return StringView::npos;
    }();
    if (slash == StringView::npos) {
      keep_text(v, start);
      // a single ident is both the start and the end line of a named area; a number or span only sets the start
      *end = TextRef();
      return;
    }
    keep_text(v.first(slash).trimmed(), start);
    keep_text(v.substr(slash + 1).trimmed(), end);
  }

  void apply_value(Prop p, StringView v);
};

void CssApply::apply_value(Prop p, StringView value) {
  Length l;
  uint32_t c;
  switch (p) {
    case pColor:
      if (parse_color(value, &c) && c != 0) style->color = c;
      break;
    case pBackgroundColor:
      if (parse_color(value, &c)) {
        style->own_background = true;
        style->own_background_color = c;
      }
      break;
    case pBackground: {
      // the colour of the last layer (a gradient or url is not a colour and is skipped)
      uint64_t at = 0;
      StringView layer, last;
      while (next_comma_part(value, &at, &layer)) last = layer;
      uint64_t k = 0;
      StringView part;
      bool found = false;
      while (next_part(last, &k, &part)) {
        if (parse_color(part, &c)) {
          style->own_background = true;
          style->own_background_color = c;
          found = true;
        }
      }
      if (!found && (eq(last, "none") || eq(last, "transparent"))) {
        // (no colour given: the background colour goes back to transparent)
        style->own_background = true;
        style->own_background_color = 0;
      } else if (!found) {
        style->own_background = true;
        style->own_background_color = 0;
      }
      break;
    }
    case pFontSize: set_font_size(value); break;
    case pFontWeight: {
      bool bold;
      if (font_weight_of(value, &bold)) style->bold = bold;
      break;
    }
    case pFontStyle: style->italic = eq(value, "italic") || eq(value, "oblique") || value.starts_with("oblique "); break;
    case pFont: {
      // [style] [variant] [weight] [stretch] size[/line-height] family
      uint64_t at = 0;
      StringView part;
      bool bold = false, italic = false, bold_set = false;
      StringView size_part, lh_part;
      while (next_part(value, &at, &part)) {
        if (eq(part, "italic") || eq(part, "oblique")) { italic = true; continue; }
        if (eq(part, "normal") || eq(part, "small-caps") || eq(part, "ultra-condensed") || eq(part, "condensed") || eq(part, "semi-condensed") || eq(part, "expanded") ||
            eq(part, "semi-expanded") || eq(part, "extra-expanded") || eq(part, "ultra-expanded") || eq(part, "extra-condensed")) continue;
        bool b;
        double number;
        if (!part.contains('/') && !part.contains('%') && font_weight_of(part, &b) && (!evaluate_number(part, eval, &number) || number >= 1)) {
          if (eq(part, "bold") || eq(part, "bolder") || eq(part, "lighter") || (evaluate_number(part, eval, &number) && number >= 100 && number <= 1000)) { bold = b; bold_set = true; continue; }
        }
        // the size (possibly with /line-height)
        const uint64_t slash = part.find('/');
        size_part = slash == StringView::npos ? part : part.first(slash);
        if (slash != StringView::npos) lh_part = part.substr(slash + 1);
        break;
      }
      if (size_part.empty()) break;   // (not a font shorthand: a system font name)
      style->italic = italic;
      if (bold_set) style->bold = bold;
      else style->bold = false;
      set_font_size(size_part);
      if (!lh_part.empty()) set_line_height(lh_part);
      else style->line_height_px100 = -1;
      break;
    }
    case pLineHeight: set_line_height(value); break;
    case pTextAlign: {
      if (eq(value, "center") || eq(value, "-webkit-center") || eq(value, "-moz-center")) { style->align = 1; style->justify = false; }
      else if (eq(value, "right") || eq(value, "end") || eq(value, "-webkit-right")) { style->align = 2; style->justify = false; }
      else if (eq(value, "left") || eq(value, "start") || eq(value, "-webkit-left") || eq(value, "match-parent")) { style->align = 0; style->justify = false; }
      else if (eq(value, "justify") || eq(value, "justify-all")) { style->align = 0; style->justify = true; }
      break;
    }
    case pTextIndent:
      if (parse_length(value, false, &l) && l.is_px()) style->text_indent_px100 = l.px100;
      break;
    case pTextTransform:
      style->transform = eq(value, "uppercase") ? 1 : eq(value, "lowercase") ? 2 : eq(value, "capitalize") ? 3 : 0;
      break;
    case pTextDecoration:
    case pTextDecorationLine: set_text_decoration(value); break;
    case pWhiteSpace: {
      static const KeywordValue kModes[] = {{"normal", 0}, {"nowrap", 1}, {"pre", 2}, {"pre-wrap", 3}, {"pre-line", 4}, {"break-spaces", 5}};
      int k;
      if (KEYWORD(value, kModes, &k)) {
        style->white_space = static_cast<uint8_t>(k);
        style->pre = k == 2 || k == 3 || k == 5;
      }
      break;
    }
    case pWordBreak: style->word_break = eq(value, "break-all") || eq(value, "break-word"); break;
    case pOverflowWrap: style->word_break = eq(value, "anywhere") || eq(value, "break-word"); break;
    case pTextOverflow: style->text_ellipsis = eq(value, "ellipsis") || value.starts_with("ellipsis ") || value.contains("ellipsis"); break;
    case pVerticalAlign:
      style->vertical_align = eq(value, "top") || eq(value, "text-top") ? 1 : eq(value, "middle") ? 2 : eq(value, "bottom") || eq(value, "text-bottom") ? 3 : 0;
      break;
    case pListStyle: set_list_style(value, false); break;
    case pListStyleType: set_list_style(value, true); break;
    case pListStylePosition: style->list_inside = eq(value, "inside"); break;
    case pVisibility:
      if (eq(value, "hidden") || eq(value, "collapse")) style->hidden = true;
      else if (eq(value, "visible")) style->hidden = false;
      break;
    case pOpacity: {
      double n;
      EvalContext ec = eval;
      uint64_t k = 0;
      if (read_double(value, &k, &n) && k < value.size() && value[k] == '%') n /= 100;
      else if (!evaluate_number(value, ec, &n)) break;
      style->opaque_zero = n <= 0.0;
      if (style->opaque_zero) style->hidden = true;
      else if (parent.hidden == false) style->hidden = false;
      break;
    }
    case pDisplay: set_display(value); break;
    case pPosition: {
      static const KeywordValue kPositions[] = {{"static", 0}, {"relative", 1}, {"absolute", 2}, {"fixed", 3}, {"sticky", 4}, {"-webkit-sticky", 4}};
      int k;
      if (KEYWORD(value, kPositions, &k)) style->position = static_cast<Position>(k);
      break;
    }
    case pTop: case pRight: case pBottom: case pLeft:
      if (parse_length(value, p == pTop || p == pBottom, &l)) style->inset[p - pTop] = l;
      break;
    case pInset: four_sides(value, pTop, true); break;
    case pZIndex: {
      double n;
      if (eq(value, "auto")) style->z_index = 0;
      else if (evaluate_number(value, eval, &n)) style->z_index = static_cast<int32_t>(n);
      break;
    }
    case pFloat:
      style->float_side = eq(value, "left") || eq(value, "inline-start") ? Float::Left : eq(value, "right") || eq(value, "inline-end") ? Float::Right : Float::None;
      break;
    case pClear:
      style->clear = eq(value, "left") || eq(value, "inline-start") ? Clear::Left : eq(value, "right") || eq(value, "inline-end") ? Clear::Right : eq(value, "both") ? Clear::Both : Clear::None;
      break;
    case pOverflow: {
      uint64_t at = 0;
      StringView a, b;
      next_part(value, &at, &a);
      if (!next_part(value, &at, &b)) b = a;
      apply_value(pOverflowX, a);
      apply_value(pOverflowY, b);
      break;
    }
    case pOverflowX:
    case pOverflowY: {
      Overflow o = Overflow::Visible;
      if (eq(value, "hidden") || eq(value, "clip")) o = Overflow::Hidden;
      else if (eq(value, "scroll")) o = Overflow::Scroll;
      else if (eq(value, "auto") || eq(value, "overlay")) o = Overflow::Auto;
      (p == pOverflowX ? style->overflow_x : style->overflow_y) = o;
      break;
    }
    case pWidth: case pMinWidth: case pMaxWidth:
      if (parse_length(value, false, &l)) {
        if (l.kind == Length::Px && l.px100 < 0) break;
        if (p == pWidth && l.kind == Length::None) break;
        if (p == pMaxWidth && l.kind == Length::Auto) l = Length::of(Length::None);
        (p == pWidth ? style->width : p == pMinWidth ? style->min_width : style->max_width) = l;
      }
      break;
    case pHeight: case pMinHeight: case pMaxHeight:
      if (parse_length(value, true, &l)) {
        if (l.kind == Length::Px && l.px100 < 0) break;
        if (p == pHeight && l.kind == Length::None) break;
        if (p == pMaxHeight && l.kind == Length::Auto) l = Length::of(Length::None);
        (p == pHeight ? style->height : p == pMinHeight ? style->min_height : style->max_height) = l;
      }
      break;
    case pBoxSizing: style->sizing = eq(value, "border-box") ? BoxSizing::BorderBox : BoxSizing::ContentBox; break;
    case pMargin: four_sides(value, pMarginTop, true); break;
    case pPadding: four_sides(value, pPaddingTop, true); break;
    case pMarginTop: case pMarginRight: case pMarginBottom: case pMarginLeft:
      if (parse_length(value, false, &l) && (l.kind == Length::Auto || l.kind == Length::Px)) style->margin[p - pMarginTop] = l;
      break;
    case pPaddingTop: case pPaddingRight: case pPaddingBottom: case pPaddingLeft:
      if (parse_length(value, false, &l) && l.kind == Length::Px && l.px100 >= 0) style->padding[p - pPaddingTop] = l;
      break;
    case pBorder: border_shorthand(value, 0, 4); break;
    case pBorderTop: border_shorthand(value, 0, 1); break;
    case pBorderRight: border_shorthand(value, 1, 1); break;
    case pBorderBottom: border_shorthand(value, 2, 1); break;
    case pBorderLeft: border_shorthand(value, 3, 1); break;
    case pBorderWidthAll: four_sides(value, pBorderTopWidth, false); break;
    case pBorderStyleAll: four_sides(value, pBorderTopStyle, false); break;
    case pBorderColorAll: four_sides(value, pBorderTopColor, false); break;
    case pBorderTopWidth: case pBorderRightWidth: case pBorderBottomWidth: case pBorderLeftWidth: {
      int32_t w;
      if (border_width_of(value, &w)) style->border_px100[p - pBorderTopWidth] = w;
      break;
    }
    case pBorderTopStyle: case pBorderRightStyle: case pBorderBottomStyle: case pBorderLeftStyle: {
      BorderStyle bs;
      if (border_style_of(value, &bs)) style->border_style[p - pBorderTopStyle] = bs;
      break;
    }
    case pBorderTopColor: case pBorderRightColor: case pBorderBottomColor: case pBorderLeftColor:
      if (parse_color(value, &c)) style->border_color[p - pBorderTopColor] = c;
      break;
    case pFlex: {
      if (eq(value, "none")) { style->flex_grow_x100 = 0; style->flex_shrink_x100 = 0; style->flex_basis = Length(); break; }
      if (eq(value, "auto")) { style->flex_grow_x100 = 100; style->flex_shrink_x100 = 100; style->flex_basis = Length(); break; }
      if (eq(value, "initial")) { style->flex_grow_x100 = 0; style->flex_shrink_x100 = 100; style->flex_basis = Length(); break; }
      // <grow> [<shrink>] [<basis>], or just a <basis>
      double numbers[2] = {1, 1};
      int count = 0;
      Length basis = Length::px(0);
      bool have_basis = false;
      uint64_t at = 0;
      StringView part;
      while (next_part(value, &at, &part)) {
        double n;
        if (count < 2 && !have_basis && evaluate_number(part, eval, &n) && n >= 0) {
          numbers[count++] = n;
        } else if (eq(part, "content")) {
          basis = Length();
          have_basis = true;
        } else if (parse_length(part, false, &l) && l.kind != Length::None) {
          basis = l;
          have_basis = true;
        }
      }
      if (count == 0 && !have_basis) break;
      style->flex_grow_x100 = to_px100(count >= 1 ? numbers[0] : 1.0);
      style->flex_shrink_x100 = to_px100(count >= 2 ? numbers[1] : 1.0);
      style->flex_basis = basis;
      break;
    }
    case pFlexGrow: case pFlexShrink: {
      double n;
      if (evaluate_number(value, eval, &n) && n >= 0) (p == pFlexGrow ? style->flex_grow_x100 : style->flex_shrink_x100) = to_px100(n);
      break;
    }
    case pFlexBasis:
      if (parse_length(value, false, &l) && l.kind != Length::None) style->flex_basis = l;
      break;
    case pFlexDirection: {
      static const KeywordValue kDirs[] = {{"row", 0}, {"row-reverse", 1}, {"column", 2}, {"column-reverse", 3}};
      int k;
      if (KEYWORD(value, kDirs, &k)) style->flex_direction = static_cast<FlexDirection>(k);
      break;
    }
    case pFlexWrap: {
      static const KeywordValue kWraps[] = {{"nowrap", 0}, {"wrap", 1}, {"wrap-reverse", 2}};
      int k;
      if (KEYWORD(value, kWraps, &k)) style->flex_wrap = static_cast<FlexWrap>(k);
      break;
    }
    case pFlexFlow: {
      uint64_t at = 0;
      StringView part;
      while (next_part(value, &at, &part)) {
        apply_value(pFlexDirection, part);
        apply_value(pFlexWrap, part);
      }
      break;
    }
    case pJustifyContent: case pAlignItems: case pAlignSelf: case pAlignContent: case pJustifyItems: case pJustifySelf: {
      Place place;
      if (!place_value(value, &place)) break;
      switch (p) {
        case pJustifyContent: style->justify_content = place; break;
        case pAlignItems: style->align_items = place; break;
        case pAlignSelf: style->align_self = place; break;
        case pAlignContent: style->align_content = place; break;
        case pJustifyItems: style->justify_items = place; break;
        default: style->justify_self = place; break;
      }
      break;
    }
    case pPlaceContent: case pPlaceItems: case pPlaceSelf: {
      uint64_t at = 0;
      StringView a, b;
      next_part(value, &at, &a);
      if (!next_part(value, &at, &b)) b = a;
      apply_value(p == pPlaceContent ? pAlignContent : p == pPlaceItems ? pAlignItems : pAlignSelf, a);
      apply_value(p == pPlaceContent ? pJustifyContent : p == pPlaceItems ? pJustifyItems : pJustifySelf, b);
      break;
    }
    case pOrder: {
      double n;
      if (evaluate_number(value, eval, &n)) style->order = static_cast<int32_t>(n);
      break;
    }
    case pGap: {
      uint64_t at = 0;
      StringView a, b;
      next_part(value, &at, &a);
      if (!next_part(value, &at, &b)) b = a;
      apply_value(pRowGap, a);
      apply_value(pColumnGap, b);
      break;
    }
    case pRowGap:
      if (eq(value, "normal")) style->row_gap = Length();
      else if (parse_length(value, false, &l) && l.is_px() && l.px100 >= 0) style->row_gap = l;
      break;
    case pColumnGap:
      if (eq(value, "normal")) style->column_gap = Length();
      else if (parse_length(value, false, &l) && l.is_px() && l.px100 >= 0) style->column_gap = l;
      break;
    case pGridTemplateColumns: if (eq(value, "none")) style->grid_template_columns = TextRef(); else keep_text(value, &style->grid_template_columns); break;
    case pGridTemplateRows: if (eq(value, "none")) style->grid_template_rows = TextRef(); else keep_text(value, &style->grid_template_rows); break;
    case pGridTemplateAreas: if (eq(value, "none")) style->grid_template_areas = TextRef(); else keep_text(value, &style->grid_template_areas); break;
    case pGridAutoColumns: keep_text(value, &style->grid_auto_columns); break;
    case pGridAutoRows: keep_text(value, &style->grid_auto_rows); break;
    case pGridAutoFlow: {
      uint64_t at = 0;
      StringView part;
      bool column = false, dense = false;
      while (next_part(value, &at, &part)) {
        if (eq(part, "column")) column = true;
        else if (eq(part, "dense")) dense = true;
      }
      style->grid_column_flow = column;
      style->grid_dense = dense;
      break;
    }
    case pGridColumnStart: keep_text(value, &style->grid_column_start); break;
    case pGridColumnEnd: keep_text(value, &style->grid_column_end); break;
    case pGridRowStart: keep_text(value, &style->grid_row_start); break;
    case pGridRowEnd: keep_text(value, &style->grid_row_end); break;
    case pGridColumn: grid_lines(value, &style->grid_column_start, &style->grid_column_end); break;
    case pGridRow: grid_lines(value, &style->grid_row_start, &style->grid_row_end); break;
    case pGridArea: {
      // row-start / column-start / row-end / column-end  (a single name names an area)
      StringView parts[4];
      uint32_t n = 0;
      uint64_t at = 0;
      StringView part;
      while (n < 4 && next_slash_part(value, &at, &part)) parts[n++] = part;
      if (n == 0) break;
      keep_text(parts[0], &style->grid_row_start);
      keep_text(n > 1 ? parts[1] : parts[0], &style->grid_column_start);
      if (n > 2) keep_text(parts[2], &style->grid_row_end);
      else if (n == 1) keep_text(parts[0], &style->grid_row_end);
      else style->grid_row_end = TextRef();
      if (n > 3) keep_text(parts[3], &style->grid_column_end);
      else if (n == 1) keep_text(parts[0], &style->grid_column_end);
      else if (n == 2) keep_text(parts[1], &style->grid_column_end);
      else style->grid_column_end = TextRef();
      break;
    }
    case pGridTemplate:
    case pGrid: {
      // "rows / columns", where rows may be "area-strings with sizes"; for `grid`, also "auto-flow ..." forms
      int depth = 0;
      uint64_t slash = StringView::npos;
      char q = 0;
      for (uint64_t i = 0; i < value.size() && slash == StringView::npos; ++i) {
        const char ch = value[i];
        if (q) { if (ch == q) q = 0; }
        else if (ch == '"' || ch == '\'') q = ch;
        else if (ch == '(') ++depth;
        else if (ch == ')') --depth;
        else if (ch == '/' && depth == 0) slash = i;
      }
      StringView rows = slash == StringView::npos ? value : value.first(slash).trimmed(), columns = slash == StringView::npos ? StringView() : value.substr(slash + 1).trimmed();
      if (p == pGrid && (rows.contains("auto-flow") || columns.contains("auto-flow"))) {
        const bool rows_flow = rows.contains("auto-flow");   // "auto-flow [dense] <auto-rows> / <columns>"
        StringView flow = rows_flow ? rows : columns, tracks = rows_flow ? columns : rows;
        style->grid_column_flow = !rows_flow;
        style->grid_dense = flow.contains("dense");
        String auto_text(sheet.allocator_);
        uint64_t k = 0;
        StringView fp;
        while (next_part(flow, &k, &fp))
          if (!eq(fp, "auto-flow") && !eq(fp, "dense")) {
            if (!auto_text.empty()) auto_text.push_back(' ');
            auto_text.append(fp);
          }
        if (rows_flow) {
          keep_text(auto_text.view(), &style->grid_auto_rows);
          keep_text(tracks, &style->grid_template_columns);
        } else {
          keep_text(auto_text.view(), &style->grid_auto_columns);
          keep_text(tracks, &style->grid_template_rows);
        }
        break;
      }
      if (rows.contains('"') || rows.contains('\'')) {   // areas with row sizes
        String areas(sheet.allocator_), sizes(sheet.allocator_);
        uint64_t k = 0;
        StringView part;
        bool pending_size = false;
        while (next_part(rows, &k, &part)) {
          if (part[0] == '"' || part[0] == '\'') {
            if (pending_size) sizes.append(" auto");
            if (!areas.empty()) areas.push_back(' ');
            areas.append(part);
            pending_size = true;
          } else if (pending_size) {
            sizes.push_back(' ');
            sizes.append(part);
            pending_size = false;
          }
        }
        if (pending_size) sizes.append(" auto");
        keep_text(areas.view(), &style->grid_template_areas);
        keep_text(sizes.view().trimmed(), &style->grid_template_rows);
      } else {
        style->grid_template_areas = TextRef();
        if (eq(rows, "none")) style->grid_template_rows = TextRef(); else keep_text(rows, &style->grid_template_rows);
      }
      if (eq(columns, "none") || columns.empty()) style->grid_template_columns = TextRef(); else keep_text(columns, &style->grid_template_columns);
      break;
    }
    case pContent: {
      if (eq(value, "none") || eq(value, "normal")) {
        style->has_content = false;
        style->content = TextRef();
      } else {
        style->has_content = true;
        keep_text(value, &style->content);
      }
      break;
    }
    case pCounterReset: if (eq(value, "none")) style->counter_reset = TextRef(); else keep_text(value, &style->counter_reset); break;
    case pCounterIncrement: if (eq(value, "none")) style->counter_increment = TextRef(); else keep_text(value, &style->counter_increment); break;
    case pColumns: {
      uint64_t at = 0;
      StringView part;
      while (next_part(value, &at, &part)) {
        apply_value(pColumnCount, part);
        apply_value(pColumnWidth, part);
      }
      break;
    }
    case pColumnCount: {
      double n;
      if (eq(value, "auto")) style->column_count = 0;
      else if (evaluate_number(value, eval, &n) && n >= 1 && n <= 32) style->column_count = static_cast<uint16_t>(n);
      break;
    }
    case pColumnWidth:
      if (parse_length(value, false, &l, false)) style->column_width = l.kind == Length::Px ? l : Length();
      break;
    case pBorderCollapse: style->border_collapse = eq(value, "collapse"); break;
    case pBorderSpacing:
      if (parse_length(value.first(value.find(' ') == StringView::npos ? value.size() : value.find(' ')), false, &l) && l.is_px()) style->border_spacing = l;
      break;
    case pTableLayout: style->table_fixed = eq(value, "fixed"); break;
    case pCaptionSide: style->caption_bottom = eq(value, "bottom"); break;
    default: break;
  }
}


bool parse_css_color_alpha(StringView text, uint32_t* out) {
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
    *out = (a << 24) | (r << 16) | (g << 8) | b;
    return true;
  }
  const uint64_t open = text.find('(');
  if (open != StringView::npos && text.ends_with(')')) {
    const StringView function = text.first(open).trimmed();
    const bool rgb = function.equals_ignore_case("rgb") || function.equals_ignore_case("rgba"), hsl = function.equals_ignore_case("hsl") || function.equals_ignore_case("hsla"),
               hwb = function.equals_ignore_case("hwb");
    if (!rgb && !hsl && !hwb) return false;
    int64_t v[4] = {};
    bool pct[4] = {};
    const uint32_t n = read_numbers(text.substr(open + 1, text.size() - open - 2), v, pct);
    if (n < 3) return false;
    uint32_t rgb_value = rgb ? (channel(v[0], pct[0]) << 16) | (channel(v[1], pct[1]) << 8) | channel(v[2], pct[2]) : hwb ? hwb_to_rgb(v[0], v[1], v[2]) : hsl_to_rgb(v[0], v[1], v[2]);
    int64_t alpha = 255;
    if (n >= 4) {
      alpha = pct[3] ? v[3] * 255 / 10000 : v[3] * 255 / 100;
      alpha = alpha < 0 ? 0 : alpha > 255 ? 255 : alpha;
    }
    *out = (static_cast<uint32_t>(alpha) << 24) | rgb_value;
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
    const bool rgb = function.equals_ignore_case("rgb") || function.equals_ignore_case("rgba"), hsl = function.equals_ignore_case("hsl") || function.equals_ignore_case("hsla"),
               hwb = function.equals_ignore_case("hwb");
    if (!rgb && !hsl && !hwb) return false;
    int64_t v[4] = {};
    bool pct[4] = {};
    const uint32_t n = read_numbers(text.substr(open + 1, text.size() - open - 2), v, pct);
    if (n < 3) return false;
    if (n == 4 && v[3] < (pct[3] ? 5000 : 50)) {
      *out = 0;
      return true;
    }
    uint32_t rgb_value = rgb ? (channel(v[0], pct[0]) << 16) | (channel(v[1], pct[1]) << 8) | channel(v[2], pct[2]) : hwb ? hwb_to_rgb(v[0], v[1], v[2]) : hsl_to_rgb(v[0], v[1], v[2]);
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


// --- Reading stylesheets ----------------------------------------------------------------------------

namespace {

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
        if (c == '\\') ++i;
        else if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '(' || c == '[') {
        ++depth;
      } else if ((c == ')' || c == ']') && depth > 0) {
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
    if (name.empty() || value.empty()) continue;
    each(name, value, important);
  }
}

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

// Do the conditions of an @supports rule hold? (property: value) pairs are checked against the properties known here; not / and / or are understood.
bool supports_condition(StringView text) {
  text = text.trimmed();
  if (text.empty()) return false;
  if (text.starts_with("not ") || text.starts_with("not(")) return !supports_condition(text.substr(3));
  // split on top-level and / or
  int depth = 0;
  for (uint64_t i = 0; i < text.size(); ++i) {
    if (text[i] == '(') ++depth;
    else if (text[i] == ')') --depth;
    else if (depth == 0 && i + 4 < text.size() && is_space(text[i]) && (text.substr(i + 1, 3).equals_ignore_case("and") || text.substr(i + 1, 2).equals_ignore_case("or")) &&
             is_space(text[i + (text.substr(i + 1, 3).equals_ignore_case("and") ? 4 : 3)])) {
      const bool is_and = text.substr(i + 1, 3).equals_ignore_case("and");
      const StringView left = text.first(i), right = text.substr(i + (is_and ? 5 : 4));
      return is_and ? supports_condition(left) && supports_condition(right) : supports_condition(left) || supports_condition(right);
    }
  }
  if (text[0] == '(' && text.ends_with(')')) {
    const StringView inner = text.substr(1, text.size() - 2).trimmed();
    const uint64_t colon = inner.find(':');
    if (colon == StringView::npos || inner.starts_with("(") || inner.starts_with("not ")) return supports_condition(inner);
    return property_from_name(inner.first(colon).trimmed()) != pNone;
  }
  if (text.starts_with("selector(")) return true;
  const uint64_t colon = text.find(':');
  return colon != StringView::npos && property_from_name(text.first(colon).trimmed()) != pNone;
}

}  // namespace

void StyleSheet::clear() {
  pointer_state_ = false;
  scope_ = 0;
  pool_.clear();
  selectors_.clear();
  declarations_.clear();
  rules_.clear();
  scopes_.clear();
  kept_.clear();
}

StringView StyleSheet::text(TextRef ref) const {
  if (ref.length == 0) return StringView();
  if (ref.offset & 0x80000000u) {
    const uint32_t index = ref.offset & 0x7FFFFFFFu;
    return index < kept_.count() ? kept_[index].view() : StringView();
  }
  return StringView(pool_.view()).substr(ref.offset, ref.length);
}

TextRef StyleSheet::keep_text(StringView s) const {
  TextRef ref;
  if (s.empty() || kept_.count() >= 50000) return ref;
  // the same text as a moment ago is shared
  for (uint64_t i = kept_.count(); i > 0 && i + 8 > kept_.count(); --i) {
    if (kept_[i - 1].view().equals(s)) {
      ref.offset = 0x80000000u | static_cast<uint32_t>(i - 1);
      ref.length = static_cast<uint32_t>(s.size());
      return ref;
    }
  }
  String copy(allocator_);
  if (!copy.assign(s) || !kept_.push_back(util::move(copy))) return ref;
  ref.offset = 0x80000000u | static_cast<uint32_t>(kept_.count() - 1);
  ref.length = static_cast<uint32_t>(s.size());
  return ref;
}

bool StyleSheet::resolve_length(StringView expression, const Style& style, int32_t percent_px, Length* out) const {
  EvalContext c;
  c.font_px = style.font_px;
  c.viewport_w = viewport_w_;
  c.viewport_h = viewport_h_;
  c.percent_px = percent_px;
  c.has_percent = percent_px >= 0;
  double px;
  if (!evaluate_length(expression, c, true, &px)) return false;
  *out = Length::px(to_px100(px));
  return true;
}

bool StyleSheet::parse(StringView css, const MediaEnvironment& env) {
  env_ = &env;
  viewport_w_ = env.width > 0 ? env.width : 640;
  viewport_h_ = env.height > 0 ? env.height : 480;
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
  const bool ok = parse_block(clean.view(), &at, false);
  env_ = nullptr;
  return ok;
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
      const bool container = name.equals_ignore_case("media") || name.equals_ignore_case("supports") || name.equals_ignore_case("layer") || name.equals_ignore_case("document") ||
                             name.equals_ignore_case("container") || name.equals_ignore_case("scope") || name.equals_ignore_case("starting-style");
      bool applies = false;
      if (container) {
        if (name.equals_ignore_case("media")) applies = media_matches(prelude, *env_);
        else if (name.equals_ignore_case("supports")) applies = supports_condition(prelude);
        else applies = true;
      }
      if (applies) {
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
  if (prelude.contains(":hover") || prelude.contains(":active") || prelude.contains(":focus-within")) pointer_state_ = true;
  const uint32_t rule = static_cast<uint32_t>(rules_.count());
  if (!add_declarations(body, rule)) return false;
  if (rules_[rule].declaration_count == 0) {
    rules_.remove_at(rule);
    return true;
  }
  uint32_t added = 0;
  selectors_.parse(prelude, rule, &added);   // (a selector that is wrong or can never match is left out)
  return true;
}

bool StyleSheet::add_declarations(StringView body, uint32_t rule) {
  Rule r;
  r.first_declaration = static_cast<uint32_t>(declarations_.count());
  r.declaration_count = 0;
  r.order = rule;
  r.scope = scope_;
  bool ok = true;
  for_each_declaration(body, [&](StringView name, StringView value, bool important) {
    const Prop p = property_from_name(name);
    if (p == pNone || !ok) return;
    Declaration d;
    d.property = p;
    d.important = important;
    d.value_length = static_cast<uint32_t>(value.size());
    d.name_offset = d.name_length = 0;
    if (p == pCustom) {
      d.name_length = static_cast<uint32_t>(name.size());
      ok = keep(name, &d.name_offset);
    }
    ok = ok && keep(value, &d.value_offset) && declarations_.push_back(d);
    ++r.declaration_count;
  });
  return ok && rules_.push_back(r);
}

// The shadow root an element is in (0: the document's tree) and the one it hosts.
static void element_scopes(const dom::Node* element, uint32_t* in_scope, uint32_t* hosted) {
  const dom::Node* top = element;
  while (top->parent) top = top->parent;
  *in_scope = top->shadow_host ? top->serial : 0;
  *hosted = element->shadow ? element->shadow->serial : 0;
}

// A rule applies to what is in its own tree (the document's rules to the document's elements, a shadow tree's to that shadow tree's), and the rules of a shadow
// tree written with :host to its host.
bool StyleSheet::rule_applies(uint32_t selector, uint32_t element_scope, uint32_t host_scope) const {
  const uint32_t scope = rules_[selectors_.tag(selector)].scope;
  return scope == element_scope || (scope != 0 && scope == host_scope && selectors_.uses_host(selector));
}

bool StyleSheet::has_pseudo(const dom::Node* element, PseudoElement pseudo) const {
  uint32_t element_scope, host_scope;
  element_scopes(element, &element_scope, &host_scope);
  for (uint32_t s = 0; s < selectors_.count(); ++s)
    if (selectors_.pseudo(s) == static_cast<uint8_t>(pseudo) && rule_applies(s, element_scope, host_scope) && selectors_.matches(s, element, nullptr, true)) return true;
  return false;
}

namespace {

// Keeps the lines, columns and sizes the flow layout works with in step with the lengths of the box.
void finish_style(Style* s) {
  auto lines = [](const Length& l, int cap) {
    int32_t v = l.kind == Length::Px ? l.lines() : 0;
    return static_cast<uint8_t>(v < 0 ? 0 : v > cap ? cap : v);
  };
  auto columns = [](const Length& l, int cap) {
    int32_t v = l.kind == Length::Px ? l.columns() : 0;
    return static_cast<uint8_t>(v < 0 ? 0 : v > cap ? cap : v);
  };
  s->margin_top = lines(s->margin[0], 3);
  s->margin_bottom = lines(s->margin[2], 3);
  s->margin_left = columns(s->margin[3], 40);
  s->padding_top = lines(s->padding[0], 3);
  s->padding_bottom = lines(s->padding[2], 3);
  s->padding_left = columns(s->padding[3], 40);
  const int32_t w = s->width.kind == Length::Px && s->width.px100 > 0 ? s->width.px100 / 100 : 0;
  const int32_t h = s->height.kind == Length::Px && s->height.px100 > 0 ? s->height.px100 / 100 : 0;
  s->width_px = static_cast<uint16_t>(w > 8192 ? 8192 : w);
  s->height_px = static_cast<uint16_t>(h > 8192 ? 8192 : h);
  s->width_pct = 0;
  s->list_none = s->list_type == 8;
}

}  // namespace

// Adds a scope (or finds the same one among the latest): returns the index to put in a style.
uint32_t StyleSheet::add_scope(const Scope& scope, uint32_t fallback) const {
  const uint64_t n = scopes_.count();
  for (uint64_t i = n; i > 0 && i + 64 > n; --i) {
    const Scope& s = scopes_[i - 1];
    if (s.parent == scope.parent && s.name_offset == scope.name_offset && s.name_length == scope.name_length && s.value_offset == scope.value_offset && s.value_length == scope.value_length)
      return static_cast<uint32_t>(i);
  }
  if (!scopes_.push_back(scope)) return fallback;
  return static_cast<uint32_t>(scopes_.count());
}

void StyleSheet::apply(const dom::Node* element, StringView inline_style, const Style& parent, Style* style, PseudoElement pseudo) const {
  uint32_t matched[kMaxMatches];
  uint32_t count = 0;
  uint32_t element_scope, host_scope;
  element_scopes(element, &element_scope, &host_scope);
  for (uint32_t s = 0; s < selectors_.count() && count < kMaxMatches; ++s) {
    if (selectors_.pseudo(s) != static_cast<uint8_t>(pseudo) || !rule_applies(s, element_scope, host_scope) || !selectors_.matches(s, element, nullptr, true)) continue;
    uint32_t k = count++;   // (kept ordered by specificity, then by where it came from)
    while (k > 0) {
      const uint32_t before = matched[k - 1];
      if (selectors_.specificity(before) < selectors_.specificity(s) || (selectors_.specificity(before) == selectors_.specificity(s) && selectors_.tag(before) <= selectors_.tag(s))) break;
      matched[k] = matched[k - 1];
      --k;
    }
    matched[k] = s;
  }
  CssApply a(*this, parent, style);
  // Three rounds over the same declarations: custom properties first (so that var() has its values), then the font size (em means it), then the rest. In each, the
  // ordinary declarations come before the !important ones, the style attribute after the rules.
  for (int round = 0; round < 3; ++round) {
    for (int pass = 0; pass < 2; ++pass) {
      const bool important = pass == 1;
      for (uint32_t m = 0; m < count; ++m) {
        const Rule& rule = rules_[selectors_.tag(matched[m])];
        for (uint32_t d = 0; d < rule.declaration_count; ++d) {
          const Declaration& decl = declarations_[rule.first_declaration + d];
          if (decl.important != important) continue;
          const Prop p = static_cast<Prop>(decl.property);
          const StringView value = StringView(pool_.view()).substr(decl.value_offset, decl.value_length);
          if (round == 0) {
            if (p == pCustom && scopes_.count() < kMaxCustomScopes) {
              Scope scope;
              scope.parent = style->custom_scope;
              scope.name_offset = decl.name_offset;
              scope.name_length = decl.name_length;
              scope.value_offset = decl.value_offset;
              scope.value_length = decl.value_length;
              style->custom_scope = add_scope(scope, style->custom_scope);
            }
          } else if (round == 1) {
            if (p == pFontSize) a.declare(p, value);
          } else if (p != pCustom && p != pFontSize) {
            a.declare(p, value);
          }
        }
      }
      if (inline_style.empty()) continue;
      for_each_declaration(inline_style, [&](StringView name, StringView value, bool is_important) {
        if (is_important != important) return;
        const Prop p = property_from_name(name);
        if (p == pNone) return;
        if (round == 0) {
          if (p == pCustom && scopes_.count() < kMaxCustomScopes) {
            const TextRef n = keep_text(name), v = keep_text(value);
            Scope scope;
            scope.parent = style->custom_scope;
            scope.name_offset = n.offset;
            scope.name_length = n.length;
            scope.value_offset = v.offset;
            scope.value_length = v.length;
            style->custom_scope = add_scope(scope, style->custom_scope);
          }
        } else if (round == 1) {
          if (p == pFontSize) a.declare(p, value);
        } else if (p != pCustom && p != pFontSize) {
          a.declare(p, value);
        }
      });
    }
  }
  finish_style(style);
}

}  // namespace browser
