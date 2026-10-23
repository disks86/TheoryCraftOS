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

// --- Maths (the image has no libm) ---------------------------------------------------------------------

constexpr double kPi = 3.14159265358979323846;
constexpr double kLn2 = 0.69314718055994530942;

bool is_finite(double x) { return x == x && x < 1e300 && x > -1e300; }
double floor_of(double x) {
  if (x > 9e15 || x < -9e15) return x;
  const double t = static_cast<double>(static_cast<int64_t>(x));
  return t > x ? t - 1 : t;
}
double trunc_of(double x) { return x < 0 ? -floor_of(-x) : floor_of(x); }
double ceil_of(double x) { return -floor_of(-x); }

double math_sqrt(double x) {
  if (!(x > 0)) return 0;
  double y = x > 1 ? x : 1;
  for (int i = 0; i < 120; ++i) {
    const double next = (y + x / y) * 0.5;
    if (next == y) break;
    y = next;
  }
  return y;
}

double math_exp(double x) {
  if (x > 700) return 1e300;
  if (x < -700) return 0;
  const double n = floor_of(x / kLn2 + 0.5);
  const double r = x - n * kLn2;
  double term = 1, sum = 1;
  for (int i = 1; i < 20; ++i) {
    term *= r / i;
    sum += term;
  }
  int k = static_cast<int>(n);
  while (k > 0) { sum *= 2; --k; }
  while (k < 0) { sum *= 0.5; ++k; }
  return sum;
}

double math_log(double x) {   // x > 0
  if (!(x > 0)) return 0;
  int e = 0;
  while (x >= 1.5) { x *= 0.5; ++e; }
  while (x < 0.75) { x *= 2; --e; }
  const double t = (x - 1) / (x + 1), t2 = t * t;
  double term = t, sum = 0;
  for (int i = 1; i < 60; i += 2) {
    sum += term / i;
    term *= t2;
  }
  return 2 * sum + e * kLn2;
}

double math_pow(double base, double exponent, bool* ok) {
  *ok = true;
  if (exponent == 0) return 1;
  const double whole = floor_of(exponent);
  if (whole == exponent && exponent > -4096 && exponent < 4096) {
    double result = 1, b = base;
    int64_t n = static_cast<int64_t>(exponent < 0 ? -exponent : exponent);
    while (n > 0) {
      if (n & 1) result *= b;
      b *= b;
      n >>= 1;
      if (!is_finite(b) && n > 0) { b = b > 0 ? 1e300 : -1e300; }
    }
    if (exponent < 0) {
      if (result == 0) { *ok = false; return 0; }
      return 1 / result;
    }
    return result;
  }
  if (!(base > 0)) {
    if (base == 0 && exponent > 0) return 0;
    *ok = false;
    return 0;
  }
  return math_exp(exponent * math_log(base));
}

double math_sin(double x) {   // radians
  x -= 2 * kPi * floor_of(x / (2 * kPi) + 0.5);   // -pi..pi
  double term = x, sum = x;
  const double x2 = x * x;
  for (int i = 1; i < 14; ++i) {
    term *= -x2 / ((2 * i) * (2 * i + 1));
    sum += term;
  }
  return sum;
}
double math_cos(double x) { return math_sin(x + kPi / 2); }

double math_atan(double x) {
  bool negative = false;
  if (x < 0) { negative = true; x = -x; }
  bool invert = false;
  if (x > 1) { invert = true; x = 1 / x; }
  for (int i = 0; i < 3; ++i) x = x / (1 + math_sqrt(1 + x * x));   // (the half angle, three times)
  double term = x, sum = 0;
  const double x2 = x * x;
  for (int i = 1; i < 30; i += 2) {
    sum += term / i * (((i - 1) / 2) % 2 ? -1 : 1);
    term *= x2;
  }
  sum *= 8;
  if (invert) sum = kPi / 2 - sum;
  return negative ? -sum : sum;
}

double math_atan2(double y, double x) {
  if (x > 0) return math_atan(y / x);
  if (x < 0) return y >= 0 ? math_atan(y / x) + kPi : math_atan(y / x) - kPi;
  return y > 0 ? kPi / 2 : y < 0 ? -kPi / 2 : 0;
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
    if (is_ident_start(c)) {
      uint64_t k = pos_;
      while (k < text_.size() && is_ident_char(text_[k])) ++k;
      if (k >= text_.size() || text_[k] != '(') {   // a constant
        const StringView ident = text_.substr(pos_, k - pos_);
        out->kind = Quantity::Number;
        if (ident.equals_ignore_case("pi")) out->v = kPi;
        else if (ident.equals_ignore_case("e")) out->v = 2.71828182845904523536;
        else if (ident.equals_ignore_case("infinity")) out->v = 1e300;
        else if (ident.equals_ignore_case("-infinity")) out->v = -1e300;
        else return false;
        pos_ = k;
        return true;
      }
      return function(out, depth);
    }
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
  static constexpr uint32_t kMaxArgs = 8;

  // An angle (or a plain number of radians) as radians.
  static bool radians_of(const Quantity& q, double* out) {
    if (q.kind == Quantity::Angle) { *out = q.v * kPi / 180; return true; }
    if (q.kind == Quantity::Number) { *out = q.v; return true; }
    return false;
  }
  static bool number_of(const Quantity& q, double* out) {
    if (q.kind != Quantity::Number) return false;
    *out = q.v;
    return true;
  }

  bool function(Quantity* out, int depth) {
    const uint64_t start = pos_;
    while (pos_ < text_.size() && (is_ident_char(text_[pos_]))) ++pos_;
    const StringView name = text_.substr(start, pos_ - start);
    if (pos_ >= text_.size() || text_[pos_] != '(') return false;
    ++pos_;
    if (name.equals_ignore_case("env")) {   // env(name, fallback): there is no safe area or title bar, so the names that are known are 0
      skip();
      const uint64_t ident_start = pos_;
      while (pos_ < text_.size() && is_ident_char(text_[pos_])) ++pos_;
      const StringView ident = text_.substr(ident_start, pos_ - ident_start);
      const bool known = ident.starts_with("safe-area-inset-") || ident.starts_with("titlebar-area-") || ident.starts_with("keyboard-inset-") || ident.starts_with("viewport-segment-");
      skip();
      Quantity fallback;
      bool has_fallback = false;
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        if (!sum(&fallback, depth + 1)) return false;
        has_fallback = true;
        skip();
      }
      if (pos_ >= text_.size() || text_[pos_] != ')') return false;
      ++pos_;
      if (known) {
        out->kind = Quantity::Length;
        out->v = 0;
        return true;
      }
      if (!has_fallback) return false;
      *out = fallback;
      return true;
    }
    const bool is_round = name.equals_ignore_case("round");
    int round_mode = 0;   // nearest, up, down, to-zero
    if (is_round) {
      skip();
      static const struct { const char* name; int mode; } kModes[] = {{"nearest", 0}, {"up", 1}, {"down", 2}, {"to-zero", 3}};
      for (const auto& m : kModes) {
        const uint64_t len = StringView(m.name).size();
        if (pos_ + len < text_.size() && text_.substr(pos_, len).equals_ignore_case(m.name)) {
          uint64_t k = pos_ + len;
          while (k < text_.size() && is_space(text_[k])) ++k;
          if (k < text_.size() && text_[k] == ',') {
            round_mode = m.mode;
            pos_ = k + 1;
          }
        }
      }
    }
    Quantity args[kMaxArgs];
    uint32_t count = 0;
    const bool calc = name.equals_ignore_case("calc") || name.equals_ignore_case("-webkit-calc") || name.equals_ignore_case("-moz-calc");
    for (;;) {
      if (count == kMaxArgs) return false;
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
    auto is = [&](const char* n) { return name.equals_ignore_case(n); };
    if (calc) {
      if (count != 1) return false;
      *out = args[0];
      return true;
    }
    if (is("min") || is("max")) {
      if (count < 1) return false;
      *out = args[0];
      for (uint32_t i = 1; i < count; ++i) {
        if (!unify(out, &args[i])) return false;
        if (is("min") ? args[i].v < out->v : args[i].v > out->v) out->v = args[i].v;
      }
      return true;
    }
    if (is("clamp")) {
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
    if (is("abs")) {
      if (count != 1) return false;
      *out = args[0];
      if (out->v < 0) out->v = -out->v;
      return true;
    }
    if (is("sign")) {
      if (count != 1) return false;
      out->kind = Quantity::Number;
      out->v = args[0].v > 0 ? 1 : args[0].v < 0 ? -1 : 0;
      return true;
    }
    if (is_round) {
      if (count < 1 || count > 2) return false;
      Quantity a = args[0], b;
      if (count == 2) b = args[1];
      else if (a.kind == Quantity::Number) { b.kind = Quantity::Number; b.v = 1; }
      else return false;
      if (!unify(&a, &b) || b.v == 0) return false;
      const double q = a.v / b.v;
      const double n = round_mode == 0 ? floor_of(q + 0.5) : round_mode == 1 ? ceil_of(q) : round_mode == 2 ? floor_of(q) : trunc_of(q);
      *out = a;
      out->v = n * b.v;
      return is_finite(out->v);
    }
    if (is("mod") || is("rem")) {
      if (count != 2) return false;
      Quantity a = args[0], b = args[1];
      if (!unify(&a, &b) || b.v == 0) return false;
      const double q = a.v / b.v;
      *out = a;
      out->v = a.v - b.v * (is("mod") ? floor_of(q) : trunc_of(q));
      return is_finite(out->v);
    }
    if (is("sin") || is("cos") || is("tan")) {
      double r;
      if (count != 1 || !radians_of(args[0], &r)) return false;
      out->kind = Quantity::Number;
      out->v = is("sin") ? math_sin(r) : is("cos") ? math_cos(r) : math_sin(r) / math_cos(r);
      return is_finite(out->v);
    }
    if (is("asin") || is("acos") || is("atan")) {
      double x;
      if (count != 1 || !number_of(args[0], &x)) return false;
      out->kind = Quantity::Angle;
      if (is("atan")) out->v = math_atan(x) * 180 / kPi;
      else {
        if (x > 1 || x < -1) return false;
        const double asin = math_atan2(x, math_sqrt(1 - x * x));
        out->v = (is("asin") ? asin : kPi / 2 - asin) * 180 / kPi;
      }
      return true;
    }
    if (is("atan2")) {
      Quantity y = args[0], x = args[1];
      if (count != 2 || !unify(&y, &x)) return false;
      out->kind = Quantity::Angle;
      out->v = math_atan2(y.v, x.v) * 180 / kPi;
      return true;
    }
    if (is("pow")) {
      double a, b;
      bool ok;
      if (count != 2 || !number_of(args[0], &a) || !number_of(args[1], &b)) return false;
      out->kind = Quantity::Number;
      out->v = math_pow(a, b, &ok);
      return ok && is_finite(out->v);
    }
    if (is("sqrt")) {
      double a;
      if (count != 1 || !number_of(args[0], &a) || a < 0) return false;
      out->kind = Quantity::Number;
      out->v = math_sqrt(a);
      return true;
    }
    if (is("hypot")) {
      if (count < 1) return false;
      *out = args[0];
      double total = args[0].v * args[0].v;
      for (uint32_t i = 1; i < count; ++i) {
        Quantity a = args[0], b = args[i];
        if (!unify(&a, &b)) return false;
        total += b.v * b.v;
        out->kind = a.kind;
      }
      out->v = math_sqrt(total);
      return is_finite(out->v);
    }
    if (is("log")) {
      double a, base = 0;
      if (count < 1 || count > 2 || !number_of(args[0], &a) || a <= 0) return false;
      if (count == 2 && (!number_of(args[1], &base) || base <= 0 || base == 1)) return false;
      out->kind = Quantity::Number;
      out->v = count == 2 ? math_log(a) / math_log(base) : math_log(a);
      return true;
    }
    if (is("exp")) {
      double a;
      if (count != 1 || !number_of(args[0], &a)) return false;
      out->kind = Quantity::Number;
      out->v = math_exp(a);
      return is_finite(out->v);
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

// --- Modern colours: lab(), lch(), oklab(), oklch(), color(), color-mix(), light-dark() ---------------------

struct ColorContext {
  bool dark = false;                // light-dark() takes its second colour
  uint32_t current = 0xFF000000;    // what currentcolor is
};

bool color_value(StringView text, const ColorContext& ctx, int depth, uint32_t* argb);

double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
double cube_root(double x) {
  if (x == 0) return 0;
  bool ok;
  const double r = math_pow(x < 0 ? -x : x, 1.0 / 3, &ok);
  return x < 0 ? -r : r;
}
double srgb_to_linear(double c) {
  const double a = c < 0 ? -c : c;
  bool ok;
  const double l = a <= 0.04045 ? a / 12.92 : math_pow((a + 0.055) / 1.055, 2.4, &ok);
  return c < 0 ? -l : l;
}
double linear_to_srgb(double c) {
  const double a = c < 0 ? -c : c;
  bool ok;
  const double g = a <= 0.0031308 ? a * 12.92 : 1.055 * math_pow(a, 1 / 2.4, &ok) - 0.055;
  return c < 0 ? -g : g;
}

struct Vec3 {
  double v[3];
};
Vec3 mul3(const double m[9], Vec3 a) {
  Vec3 r;
  for (int i = 0; i < 3; ++i) r.v[i] = m[i * 3] * a.v[0] + m[i * 3 + 1] * a.v[1] + m[i * 3 + 2] * a.v[2];
  return r;
}

const double kXyz65ToRgb[9] = {3.2409699419045226, -1.537383177570094, -0.4986107602930034, -0.9692436362808796, 1.8759675015077202, 0.04155505740717559,
                               0.05563007969699366, -0.20397695888897652, 1.0569715142428786};
const double kRgbToXyz65[9] = {0.41239079926595934, 0.357584339383878, 0.1804807884018343, 0.21263900587151027, 0.715168678767756, 0.07219231536073371,
                               0.01933081871559182, 0.11919477979462598, 0.9505321522496607};
const double kXyz50ToXyz65[9] = {0.9554734527042182, -0.023098536874261423, 0.0632593086610217, -0.028369706963208136, 1.0099954580058226, 0.021041398966943008,
                                 0.012314001688319899, -0.020507696433477912, 1.3303659366080753};
const double kXyz65ToXyz50[9] = {1.0479298208405488, 0.022946793341019088, -0.05019222954313557, 0.029627815688159344, 0.990434484573249, -0.01707382502938514,
                                 -0.009243058152591178, 0.015055144896577895, 0.7518742899580008};
const double kP3ToXyz[9] = {0.4865709486482162, 0.26566769316909306, 0.1982172852343625, 0.2289745640697488, 0.6917385218365064, 0.079286914093745,
                            0.0, 0.04511338185890264, 1.043944368900976};
const double kA98ToXyz[9] = {0.5766690429101305, 0.1855582379065463, 0.1882286462349947, 0.29734497525053605, 0.6273635662554661, 0.0752914584939978,
                             0.02703136138641234, 0.07068885253582723, 0.9913375368376388};
const double kRec2020ToXyz[9] = {0.6369580483012914, 0.14461690358620832, 0.1688809751641721, 0.2627002120112671, 0.6779980715188708, 0.05930171646986196,
                                 0.0, 0.028072693049087428, 1.0609850577107909};
const double kProPhotoToXyz50[9] = {0.7977604896723027, 0.13518583717574031, 0.0313493495815248, 0.2880711282292934, 0.7118432178101014, 0.00008565396060525902,
                                    0.0, 0.0, 0.8251046025104601};

uint32_t pack_srgb(double r, double g, double b, double a) {
  auto c8 = [](double v) { return static_cast<uint32_t>(clamp01(v) * 255 + 0.5); };
  return (c8(a) << 24) | (c8(r) << 16) | (c8(g) << 8) | c8(b);
}
uint32_t pack_linear(Vec3 lin, double a) { return pack_srgb(linear_to_srgb(lin.v[0]), linear_to_srgb(lin.v[1]), linear_to_srgb(lin.v[2]), a); }

Vec3 oklab_to_linear(double L, double a, double b) {
  const double l_ = L + 0.3963377773761749 * a + 0.2158037573099136 * b, m_ = L - 0.1055613458156586 * a - 0.0638541728258133 * b,
               s_ = L - 0.0894841775298119 * a - 1.2914855480194092 * b;
  const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
  Vec3 r;
  r.v[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
  r.v[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
  r.v[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
  return r;
}
Vec3 linear_to_oklab(Vec3 c) {
  const double l = cube_root(0.4122214708 * c.v[0] + 0.5363325363 * c.v[1] + 0.0514459929 * c.v[2]),
               m = cube_root(0.2119034982 * c.v[0] + 0.6806995451 * c.v[1] + 0.1073969566 * c.v[2]),
               s = cube_root(0.0883024619 * c.v[0] + 0.2817188376 * c.v[1] + 0.6299787005 * c.v[2]);
  Vec3 r;
  r.v[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
  r.v[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  r.v[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
  return r;
}

const double kD50[3] = {0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};
Vec3 lab_to_xyz50(double L, double a, double b) {
  const double kappa = 24389.0 / 27, eps = 216.0 / 24389;
  const double fy = (L + 16) / 116, fx = a / 500 + fy, fz = fy - b / 200;
  const double xr = fx * fx * fx > eps ? fx * fx * fx : (116 * fx - 16) / kappa, yr = L > kappa * eps ? fy * fy * fy : L / kappa,
               zr = fz * fz * fz > eps ? fz * fz * fz : (116 * fz - 16) / kappa;
  Vec3 r;
  r.v[0] = xr * kD50[0];
  r.v[1] = yr * kD50[1];
  r.v[2] = zr * kD50[2];
  return r;
}
Vec3 xyz50_to_lab(Vec3 xyz) {
  const double kappa = 24389.0 / 27, eps = 216.0 / 24389;
  double f[3];
  for (int i = 0; i < 3; ++i) {
    const double t = xyz.v[i] / kD50[i];
    f[i] = t > eps ? cube_root(t) : (kappa * t + 16) / 116;
  }
  Vec3 r;
  r.v[0] = 116 * f[1] - 16;
  r.v[1] = 500 * (f[0] - f[1]);
  r.v[2] = 200 * (f[1] - f[2]);
  return r;
}

uint32_t pack_xyz65(Vec3 xyz, double a) { return pack_linear(mul3(kXyz65ToRgb, xyz), a); }
uint32_t pack_xyz50(Vec3 xyz, double a) { return pack_xyz65(mul3(kXyz50ToXyz65, xyz), a); }
Vec3 unpack_linear(uint32_t argb) {
  Vec3 r;
  r.v[0] = srgb_to_linear(((argb >> 16) & 0xFF) / 255.0);
  r.v[1] = srgb_to_linear(((argb >> 8) & 0xFF) / 255.0);
  r.v[2] = srgb_to_linear((argb & 0xFF) / 255.0);
  return r;
}

// A number, percentage (of `percent_of`) or angle (degrees) of a colour function; `none` is 0.
bool color_component(StringView tok, double percent_of, double* out) {
  tok = tok.trimmed();
  if (tok.equals_ignore_case("none")) { *out = 0; return true; }
  uint64_t i = 0;
  double v;
  if (!read_double(tok, &i, &v)) return false;
  const StringView unit = tok.substr(i);
  if (unit.empty()) { *out = v; return true; }
  if (unit.equals("%")) { *out = v * percent_of / 100; return true; }
  if (unit.equals_ignore_case("deg")) { *out = v; return true; }
  if (unit.equals_ignore_case("rad")) { *out = v * 180 / kPi; return true; }
  if (unit.equals_ignore_case("grad")) { *out = v * 0.9; return true; }
  if (unit.equals_ignore_case("turn")) { *out = v * 360; return true; }
  return false;
}

// "a b c / alpha" (or with commas): three components and an alpha (1 if not given).
bool split_components(StringView inner, StringView comps[3], double* alpha) {
  uint64_t slash = StringView::npos;
  int depth = 0;
  for (uint64_t i = 0; i < inner.size(); ++i) {
    if (inner[i] == '(') ++depth;
    else if (inner[i] == ')') --depth;
    else if (inner[i] == '/' && depth == 0) { slash = i; break; }
  }
  StringView body = slash == StringView::npos ? inner : inner.first(slash);
  *alpha = 1;
  if (slash != StringView::npos) {
    if (!color_component(inner.substr(slash + 1), 1, alpha)) return false;
    *alpha = clamp01(*alpha);
  }
  uint32_t n = 0;
  uint64_t at = 0;
  StringView part;
  while (next_part(body, &at, &part)) {
    if (n == 3) return false;
    comps[n++] = part;
  }
  return n == 3;
}

double wrap_hue(double h) { return h - 360 * floor_of(h / 360); }

// An sRGB colour as the three numbers of an interpolation space; the hue (if the space has one) is index 2 for lch / oklch and 0 for hsl / hwb.
enum class Space : uint8_t { Srgb, SrgbLinear, Hsl, Hwb, Lab, Lch, Oklab, Oklch, Xyz65, Xyz50 };

bool space_by_name(StringView n, Space* out) {
  static const struct { const char* name; Space space; } kSpaces[] = {{"srgb", Space::Srgb}, {"srgb-linear", Space::SrgbLinear}, {"hsl", Space::Hsl}, {"hwb", Space::Hwb},
                                                                      {"lab", Space::Lab}, {"lch", Space::Lch}, {"oklab", Space::Oklab}, {"oklch", Space::Oklch},
                                                                      {"xyz", Space::Xyz65}, {"xyz-d65", Space::Xyz65}, {"xyz-d50", Space::Xyz50},
                                                                      {"display-p3", Space::Srgb}, {"a98-rgb", Space::Srgb}, {"prophoto-rgb", Space::Srgb}, {"rec2020", Space::Srgb}};
  for (const auto& s : kSpaces)
    if (n.equals_ignore_case(s.name)) { *out = s.space; return true; }
  return false;
}
int hue_index(Space s) { return s == Space::Hsl || s == Space::Hwb ? 0 : s == Space::Lch || s == Space::Oklch ? 2 : -1; }

void polar(double a, double b, double* c, double* h) {
  *c = math_sqrt(a * a + b * b);
  *h = *c < 1e-9 ? 0 : wrap_hue(math_atan2(b, a) * 180 / kPi);
}

// `achromatic` is set when the hue means nothing.
Vec3 to_space(uint32_t argb, Space space, bool* achromatic) {
  *achromatic = false;
  const double r = ((argb >> 16) & 0xFF) / 255.0, g = ((argb >> 8) & 0xFF) / 255.0, b = (argb & 0xFF) / 255.0;
  Vec3 out = {{0, 0, 0}};
  switch (space) {
    case Space::Srgb: out.v[0] = r; out.v[1] = g; out.v[2] = b; return out;
    case Space::SrgbLinear: return unpack_linear(argb);
    case Space::Hsl:
    case Space::Hwb: {
      const double mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b), d = mx - mn;
      double h = 0;
      if (d > 1e-9) {
        if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h *= 60;
      }
      *achromatic = d < 1e-4;
      if (space == Space::Hwb) { out.v[0] = h; out.v[1] = mn; out.v[2] = 1 - mx; return out; }
      const double l = (mx + mn) / 2;
      const double denom = 1 - (2 * l - 1 < 0 ? 1 - 2 * l : 2 * l - 1);
      out.v[0] = h;
      out.v[1] = denom < 1e-9 ? 0 : d / denom;
      out.v[2] = l;
      return out;
    }
    case Space::Xyz65: return mul3(kRgbToXyz65, unpack_linear(argb));
    case Space::Xyz50: return mul3(kXyz65ToXyz50, mul3(kRgbToXyz65, unpack_linear(argb)));
    case Space::Lab:
    case Space::Lch: {
      Vec3 lab = xyz50_to_lab(mul3(kXyz65ToXyz50, mul3(kRgbToXyz65, unpack_linear(argb))));
      if (space == Space::Lab) return lab;
      double c, h;
      polar(lab.v[1], lab.v[2], &c, &h);
      *achromatic = c < 0.15;
      out.v[0] = lab.v[0]; out.v[1] = c; out.v[2] = h;
      return out;
    }
    case Space::Oklab:
    case Space::Oklch: {
      Vec3 lab = linear_to_oklab(unpack_linear(argb));
      if (space == Space::Oklab) return lab;
      double c, h;
      polar(lab.v[1], lab.v[2], &c, &h);
      *achromatic = c < 0.0004;
      out.v[0] = lab.v[0]; out.v[1] = c; out.v[2] = h;
      return out;
    }
  }
  return out;
}

uint32_t from_space(Vec3 c, Space space, double alpha) {
  switch (space) {
    case Space::Srgb: return pack_srgb(c.v[0], c.v[1], c.v[2], alpha);
    case Space::SrgbLinear: return pack_linear(c, alpha);
    case Space::Hsl: {
      const int64_t h = static_cast<int64_t>(wrap_hue(c.v[0]) * 100), s = static_cast<int64_t>(clamp01(c.v[1]) * 10000), l = static_cast<int64_t>(clamp01(c.v[2]) * 10000);
      return (static_cast<uint32_t>(clamp01(alpha) * 255 + 0.5) << 24) | hsl_to_rgb(h, s, l);
    }
    case Space::Hwb: {
      const int64_t h = static_cast<int64_t>(wrap_hue(c.v[0]) * 100), w = static_cast<int64_t>(clamp01(c.v[1]) * 10000), bl = static_cast<int64_t>(clamp01(c.v[2]) * 10000);
      return (static_cast<uint32_t>(clamp01(alpha) * 255 + 0.5) << 24) | hwb_to_rgb(h, w, bl);
    }
    case Space::Xyz65: return pack_xyz65(c, alpha);
    case Space::Xyz50: return pack_xyz50(c, alpha);
    case Space::Lab: return pack_xyz50(lab_to_xyz50(c.v[0], c.v[1], c.v[2]), alpha);
    case Space::Lch: {
      const double h = c.v[2] * kPi / 180;
      return pack_xyz50(lab_to_xyz50(c.v[0], c.v[1] * math_cos(h), c.v[1] * math_sin(h)), alpha);
    }
    case Space::Oklab: return pack_linear(oklab_to_linear(c.v[0], c.v[1], c.v[2]), alpha);
    case Space::Oklch: {
      const double h = c.v[2] * kPi / 180;
      return pack_linear(oklab_to_linear(c.v[0], c.v[1] * math_cos(h), c.v[1] * math_sin(h)), alpha);
    }
  }
  return 0;
}

// color-mix(in <space> [<hue> hue], <color> [<p>%], <color> [<p>%])
bool color_mix(StringView inner, const ColorContext& ctx, int depth, uint32_t* out) {
  uint64_t at = 0;
  StringView head, c1, c2, extra;
  if (!next_comma_part(inner, &at, &head) || !next_comma_part(inner, &at, &c1) || !next_comma_part(inner, &at, &c2)) return false;
  if (next_comma_part(inner, &at, &extra)) return false;
  // "in oklch longer hue"
  uint64_t k = 0;
  StringView word;
  if (!next_part(head, &k, &word) || !word.equals_ignore_case("in")) return false;
  Space space;
  if (!next_part(head, &k, &word) || !space_by_name(word, &space)) return false;
  int hue_method = 0;   // shorter, longer, increasing, decreasing
  if (next_part(head, &k, &word)) {
    if (word.equals_ignore_case("shorter")) hue_method = 0;
    else if (word.equals_ignore_case("longer")) hue_method = 1;
    else if (word.equals_ignore_case("increasing")) hue_method = 2;
    else if (word.equals_ignore_case("decreasing")) hue_method = 3;
    else return false;
    if (hue_index(space) < 0 || !next_part(head, &k, &word) || !word.equals_ignore_case("hue") || next_part(head, &k, &word)) return false;
  }
  uint32_t colors[2];
  double pct[2] = {-1, -1};
  const StringView parts[2] = {c1, c2};
  for (int i = 0; i < 2; ++i) {
    uint64_t pa = 0;
    StringView piece;
    StringView color_part;
    bool have_color = false;
    while (next_part(parts[i], &pa, &piece)) {
      if (piece.ends_with('%')) {
        double p;
        uint64_t q = 0;
        if (!read_double(piece, &q, &p) || q + 1 != piece.size() || p < 0 || p > 100) return false;
        if (pct[i] >= 0) return false;
        pct[i] = p;
      } else {
        if (have_color) return false;   // (a colour is one part: functions keep their insides together)
        color_part = piece;
        have_color = true;
      }
    }
    if (!have_color || !color_value(color_part, ctx, depth + 1, &colors[i])) return false;
  }
  double p1 = pct[0], p2 = pct[1];
  if (p1 < 0 && p2 < 0) p1 = p2 = 50;
  else if (p1 < 0) p1 = 100 - p2;
  else if (p2 < 0) p2 = 100 - p1;
  const double total = p1 + p2;
  if (total <= 0) return false;
  const double w1 = p1 / total, w2 = p2 / total, alpha_scale = total < 100 ? total / 100 : 1;
  bool ach1, ach2;
  Vec3 a = to_space(colors[0], space, &ach1), b = to_space(colors[1], space, &ach2);
  const double al1 = (colors[0] >> 24) / 255.0, al2 = (colors[1] >> 24) / 255.0;
  const double alpha = (al1 * w1 + al2 * w2);
  const int hi = hue_index(space);
  Vec3 mixed;
  for (int i = 0; i < 3; ++i) {
    if (i == hi) continue;
    // (premultiplied by alpha, except for the hue)
    const double pa_ = a.v[i] * al1, pb_ = b.v[i] * al2;
    const double m = pa_ * w1 + pb_ * w2;
    mixed.v[i] = alpha > 1e-9 ? m / alpha : (a.v[i] * w1 + b.v[i] * w2);
  }
  if (hi >= 0) {
    double h1 = wrap_hue(a.v[hi]), h2 = wrap_hue(b.v[hi]);
    if (ach1 && !ach2) h1 = h2;
    else if (ach2 && !ach1) h2 = h1;
    const double d = h2 - h1;
    if (hue_method == 0) { if (d > 180) h1 += 360; else if (d < -180) h2 += 360; }
    else if (hue_method == 1) { if (d > 0 && d < 180) h1 += 360; else if (d > -180 && d <= 0) h2 += 360; }
    else if (hue_method == 2) { if (h2 < h1) h2 += 360; }
    else { if (h1 < h2) h1 += 360; }
    mixed.v[hi] = wrap_hue(h1 * w1 + h2 * w2);
  }
  *out = from_space(mixed, space, alpha * alpha_scale);
  return true;
}

bool is_modern_color_function(StringView text) {
  const uint64_t open = text.find('(');
  if (open == StringView::npos) return false;
  const StringView f = text.first(open).trimmed();
  return f.equals_ignore_case("lab") || f.equals_ignore_case("lch") || f.equals_ignore_case("oklab") || f.equals_ignore_case("oklch") || f.equals_ignore_case("color") ||
         f.equals_ignore_case("color-mix") || f.equals_ignore_case("light-dark");
}

bool modern_color(StringView text, const ColorContext& ctx, int depth, uint32_t* out) {
  if (depth > 8) return false;
  const uint64_t open = text.find('(');
  if (open == StringView::npos || !text.ends_with(')')) return false;
  const StringView f = text.first(open).trimmed();
  const StringView inner = text.substr(open + 1, text.size() - open - 2);
  if (f.equals_ignore_case("light-dark")) {
    uint64_t at = 0;
    StringView a, b, extra;
    if (!next_comma_part(inner, &at, &a) || !next_comma_part(inner, &at, &b) || next_comma_part(inner, &at, &extra)) return false;
    uint32_t first, second;   // (both have to be colours)
    if (!color_value(a, ctx, depth + 1, &first) || !color_value(b, ctx, depth + 1, &second)) return false;
    *out = ctx.dark ? second : first;
    return true;
  }
  if (f.equals_ignore_case("color-mix")) return color_mix(inner, ctx, depth, out);
  StringView comps[3];
  double alpha;
  if (f.equals_ignore_case("color")) {
    uint64_t k = 0;
    StringView space_name;
    if (!next_part(inner, &k, &space_name)) return false;
    if (!split_components(inner.substr(k), comps, &alpha)) return false;
    double c[3];
    for (int i = 0; i < 3; ++i)
      if (!color_component(comps[i], 1, &c[i])) return false;
    Vec3 v;
    v.v[0] = c[0]; v.v[1] = c[1]; v.v[2] = c[2];
    if (space_name.equals_ignore_case("srgb")) { *out = pack_srgb(c[0], c[1], c[2], alpha); return true; }
    if (space_name.equals_ignore_case("srgb-linear")) { *out = pack_linear(v, alpha); return true; }
    if (space_name.equals_ignore_case("xyz") || space_name.equals_ignore_case("xyz-d65")) { *out = pack_xyz65(v, alpha); return true; }
    if (space_name.equals_ignore_case("xyz-d50")) { *out = pack_xyz50(v, alpha); return true; }
    Vec3 lin;
    auto gamma_to_linear = [&](auto transfer) { for (int i = 0; i < 3; ++i) v.v[i] = transfer(v.v[i]); };
    if (space_name.equals_ignore_case("display-p3")) { gamma_to_linear(srgb_to_linear); *out = pack_xyz65(mul3(kP3ToXyz, v), alpha); return true; }
    if (space_name.equals_ignore_case("a98-rgb")) {
      gamma_to_linear([](double x) { bool ok; const double a = x < 0 ? -x : x; const double r = math_pow(a, 563.0 / 256, &ok); return x < 0 ? -r : r; });
      *out = pack_xyz65(mul3(kA98ToXyz, v), alpha);
      return true;
    }
    if (space_name.equals_ignore_case("rec2020")) {
      gamma_to_linear([](double x) {
        bool ok;
        const double a = x < 0 ? -x : x, alpha_ = 1.09929682680944, beta = 0.018053968510807;
        const double r = a < beta * 4.5 ? a / 4.5 : math_pow((a + alpha_ - 1) / alpha_, 1 / 0.45, &ok);
        return x < 0 ? -r : r;
      });
      *out = pack_xyz65(mul3(kRec2020ToXyz, v), alpha);
      return true;
    }
    if (space_name.equals_ignore_case("prophoto-rgb")) {
      gamma_to_linear([](double x) { bool ok; const double a = x < 0 ? -x : x; const double r = a <= 16.0 / 512 ? a / 16 : math_pow(a, 1.8, &ok); return x < 0 ? -r : r; });
      *out = pack_xyz50(mul3(kProPhotoToXyz50, v), alpha);
      return true;
    }
    (void)lin;
    return false;
  }
  const bool lab = f.equals_ignore_case("lab"), lch = f.equals_ignore_case("lch"), oklab = f.equals_ignore_case("oklab"), oklch = f.equals_ignore_case("oklch");
  if (!lab && !lch && !oklab && !oklch) return false;
  if (!split_components(inner, comps, &alpha)) return false;
  double c[3];
  if (lab) {
    if (!color_component(comps[0], 100, &c[0]) || !color_component(comps[1], 125, &c[1]) || !color_component(comps[2], 125, &c[2])) return false;
    *out = from_space(Vec3{{c[0] < 0 ? 0 : c[0] > 100 ? 100 : c[0], c[1], c[2]}}, Space::Lab, alpha);
  } else if (lch) {
    if (!color_component(comps[0], 100, &c[0]) || !color_component(comps[1], 150, &c[1]) || !color_component(comps[2], 360, &c[2])) return false;
    *out = from_space(Vec3{{c[0] < 0 ? 0 : c[0] > 100 ? 100 : c[0], c[1] < 0 ? 0 : c[1], c[2]}}, Space::Lch, alpha);
  } else if (oklab) {
    if (!color_component(comps[0], 1, &c[0]) || !color_component(comps[1], 0.4, &c[1]) || !color_component(comps[2], 0.4, &c[2])) return false;
    *out = from_space(Vec3{{clamp01(c[0]), c[1], c[2]}}, Space::Oklab, alpha);
  } else {
    if (!color_component(comps[0], 1, &c[0]) || !color_component(comps[1], 0.4, &c[1]) || !color_component(comps[2], 360, &c[2])) return false;
    *out = from_space(Vec3{{clamp01(c[0]), c[1] < 0 ? 0 : c[1], c[2]}}, Space::Oklch, alpha);
  }
  return true;
}

bool color_value(StringView text, const ColorContext& ctx, int depth, uint32_t* argb) {
  text = text.trimmed();
  if (text.equals_ignore_case("currentcolor")) { *argb = ctx.current; return true; }
  if (is_modern_color_function(text)) return modern_color(text, ctx, depth, argb);
  return parse_css_color_alpha(text, argb);
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
  // the pixel engine's longhands (the corners, the outline and the column rule keep their order: they are indexed from the first)
  pFontFamily, pFontStretch, pFontVariantCaps, pFontVariantNumeric, pFontVariantLigatures, pLetterSpacing, pWordSpacing,
  pBorderTopLeftRadius, pBorderTopRightRadius, pBorderBottomRightRadius, pBorderBottomLeftRadius, pBoxShadow, pTextShadow,
  pBackgroundImage, pBackgroundSize, pBackgroundPosition, pBackgroundRepeat, pBackgroundClip, pBackgroundOrigin, pBackgroundAttachment,
  pTransform, pTranslate, pRotate, pScale, pTransformOrigin, pOutlineWidth, pOutlineStyle, pOutlineColor, pOutlineOffset,
  pCursor, pPointerEvents, pUserSelect, pAspectRatio, pObjectFit, pObjectPosition, pListStyleImage, pColumnRuleWidth, pColumnRuleStyle, pColumnRuleColor,
  pTransitionProperty, pTransitionDuration, pTransitionTimingFunction, pTransitionDelay, pTransitionBehavior,
  pAnimationName, pAnimationDuration, pAnimationTimingFunction, pAnimationDelay, pAnimationIterationCount, pAnimationDirection, pAnimationFillMode, pAnimationPlayState,
  pFilter, pBackdropFilter, pClipPath, pWritingMode, pDirection, pColorScheme, pScrollSnapType, pScrollSnapAlign, pScrollSnapStop, pOverscrollBehaviorX, pOverscrollBehaviorY,
  pScrollBehavior, pContainerType, pContainerName,
  pLastLonghand,
  // shorthands
  pBackground, pFont, pTextDecoration, pListStyle, pMargin, pPadding, pBorder, pBorderTop, pBorderRight, pBorderBottom, pBorderLeft, pBorderWidthAll, pBorderStyleAll,
  pBorderColorAll, pOverflow, pInset, pFlex, pFlexFlow, pGap, pGridColumn, pGridRow, pGridArea, pGridTemplate, pGrid, pPlaceContent, pPlaceItems, pPlaceSelf, pColumns,
  pBorderRadius, pOutline, pColumnRule, pTransition, pAnimation, pOverscrollBehavior, pFontVariant, pContainer,
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
    {"font-family", pFontFamily}, {"font-stretch", pFontStretch}, {"font-width", pFontStretch}, {"font-variant", pFontVariant}, {"font-variant-caps", pFontVariantCaps},
    {"font-variant-numeric", pFontVariantNumeric}, {"font-variant-ligatures", pFontVariantLigatures}, {"letter-spacing", pLetterSpacing}, {"word-spacing", pWordSpacing},
    {"border-radius", pBorderRadius}, {"-webkit-border-radius", pBorderRadius}, {"border-top-left-radius", pBorderTopLeftRadius}, {"border-top-right-radius", pBorderTopRightRadius},
    {"border-bottom-right-radius", pBorderBottomRightRadius}, {"border-bottom-left-radius", pBorderBottomLeftRadius}, {"border-start-start-radius", pBorderTopLeftRadius},
    {"border-start-end-radius", pBorderTopRightRadius}, {"border-end-end-radius", pBorderBottomRightRadius}, {"border-end-start-radius", pBorderBottomLeftRadius},
    {"box-shadow", pBoxShadow}, {"-webkit-box-shadow", pBoxShadow}, {"text-shadow", pTextShadow},
    {"background-image", pBackgroundImage}, {"background-size", pBackgroundSize}, {"background-position", pBackgroundPosition}, {"background-repeat", pBackgroundRepeat},
    {"background-clip", pBackgroundClip}, {"-webkit-background-clip", pBackgroundClip}, {"background-origin", pBackgroundOrigin}, {"background-attachment", pBackgroundAttachment},
    {"transform", pTransform}, {"-webkit-transform", pTransform}, {"translate", pTranslate}, {"rotate", pRotate}, {"scale", pScale}, {"transform-origin", pTransformOrigin},
    {"-webkit-transform-origin", pTransformOrigin}, {"outline", pOutline}, {"outline-width", pOutlineWidth}, {"outline-style", pOutlineStyle}, {"outline-color", pOutlineColor},
    {"outline-offset", pOutlineOffset}, {"cursor", pCursor}, {"pointer-events", pPointerEvents}, {"user-select", pUserSelect}, {"-webkit-user-select", pUserSelect},
    {"-moz-user-select", pUserSelect}, {"aspect-ratio", pAspectRatio}, {"object-fit", pObjectFit}, {"object-position", pObjectPosition}, {"list-style-image", pListStyleImage},
    {"column-rule", pColumnRule}, {"column-rule-width", pColumnRuleWidth}, {"column-rule-style", pColumnRuleStyle}, {"column-rule-color", pColumnRuleColor},
    {"transition", pTransition}, {"-webkit-transition", pTransition}, {"transition-property", pTransitionProperty}, {"transition-duration", pTransitionDuration},
    {"transition-timing-function", pTransitionTimingFunction}, {"transition-delay", pTransitionDelay}, {"transition-behavior", pTransitionBehavior},
    {"animation", pAnimation}, {"-webkit-animation", pAnimation}, {"animation-name", pAnimationName}, {"animation-duration", pAnimationDuration},
    {"animation-timing-function", pAnimationTimingFunction}, {"animation-delay", pAnimationDelay}, {"animation-iteration-count", pAnimationIterationCount},
    {"animation-direction", pAnimationDirection}, {"animation-fill-mode", pAnimationFillMode}, {"animation-play-state", pAnimationPlayState},
    {"filter", pFilter}, {"-webkit-filter", pFilter}, {"backdrop-filter", pBackdropFilter}, {"-webkit-backdrop-filter", pBackdropFilter}, {"clip-path", pClipPath},
    {"-webkit-clip-path", pClipPath}, {"writing-mode", pWritingMode}, {"direction", pDirection}, {"color-scheme", pColorScheme}, {"scroll-snap-type", pScrollSnapType},
    {"scroll-snap-align", pScrollSnapAlign}, {"scroll-snap-stop", pScrollSnapStop}, {"overscroll-behavior", pOverscrollBehavior}, {"overscroll-behavior-x", pOverscrollBehaviorX},
    {"overscroll-behavior-y", pOverscrollBehaviorY}, {"scroll-behavior", pScrollBehavior}, {"container", pContainer}, {"container-type", pContainerType}, {"container-name", pContainerName},
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
    case pBackground:
      add(pBackgroundColor); add(pBackgroundImage); add(pBackgroundPosition); add(pBackgroundSize); add(pBackgroundRepeat); add(pBackgroundOrigin); add(pBackgroundClip);
      add(pBackgroundAttachment);
      break;
    case pFont:
      add(pFontStyle); add(pFontWeight); add(pFontSize); add(pLineHeight); add(pFontFamily); add(pFontStretch); add(pFontVariantCaps); add(pFontVariantNumeric);
      add(pFontVariantLigatures);
      break;
    case pFontVariant: add(pFontVariantCaps); add(pFontVariantNumeric); add(pFontVariantLigatures); break;
    case pBorderRadius: for (int i = 0; i < 4; ++i) add(static_cast<Prop>(pBorderTopLeftRadius + i)); break;
    case pOutline: add(pOutlineWidth); add(pOutlineStyle); add(pOutlineColor); break;
    case pColumnRule: add(pColumnRuleWidth); add(pColumnRuleStyle); add(pColumnRuleColor); break;
    case pTransition: for (int i = 0; i < 5; ++i) add(static_cast<Prop>(pTransitionProperty + i)); break;
    case pAnimation: for (int i = 0; i < 8; ++i) add(static_cast<Prop>(pAnimationName + i)); break;
    case pOverscrollBehavior: add(pOverscrollBehaviorX); add(pOverscrollBehaviorY); break;
    case pContainer: add(pContainerName); add(pContainerType); break;
    case pTextDecoration: add(pTextDecorationLine); break;
    case pListStyle: add(pListStyleType); add(pListStylePosition); add(pListStyleImage); break;
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
    case pBackgroundColor: to->own_background = from.own_background; to->own_background_color = from.own_background_color; to->background_argb = from.background_argb; break;
    case pFontSize: to->font_px = from.font_px; break;
    case pFontWeight: to->bold = from.bold; to->font_weight = from.font_weight; break;
    case pFontStyle: to->italic = from.italic; break;
    case pLineHeight: to->line_height_px100 = from.line_height_px100; break;
    case pTextAlign: to->align = from.align; to->justify = from.justify; break;
    case pTextIndent: to->text_indent_px100 = from.text_indent_px100; to->text_indent = from.text_indent; break;
    case pTextTransform: to->transform = from.transform; break;
    case pTextDecorationLine: to->underline = from.underline; to->strike = from.strike; break;
    case pWhiteSpace: to->white_space = from.white_space; to->pre = from.pre; break;
    case pWordBreak: case pOverflowWrap: to->word_break = from.word_break; break;
    case pTextOverflow: to->text_ellipsis = from.text_ellipsis; break;
    case pVerticalAlign: to->vertical_align = from.vertical_align; break;
    case pListStyleType: to->list_type = from.list_type; to->list_none = from.list_none; break;
    case pListStylePosition: to->list_inside = from.list_inside; break;
    case pVisibility: to->hidden = from.hidden; to->visibility = from.visibility; break;
    case pOpacity: to->opaque_zero = from.opaque_zero; to->opacity_x1000 = from.opacity_x1000; break;
    case pDisplay: to->display = from.display; to->layout = from.layout; to->inline_box = from.inline_box; to->contents = from.contents; break;
    case pPosition: to->position = from.position; break;
    case pTop: to->inset[0] = from.inset[0]; break;
    case pRight: to->inset[1] = from.inset[1]; break;
    case pBottom: to->inset[2] = from.inset[2]; break;
    case pLeft: to->inset[3] = from.inset[3]; break;
    case pZIndex: to->z_index = from.z_index; to->z_index_auto = from.z_index_auto; break;
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
    case pFontFamily: to->font_family = from.font_family; break;
    case pFontStretch: to->font_stretch_x10 = from.font_stretch_x10; break;
    case pFontVariantCaps: to->font_variant = static_cast<uint16_t>((to->font_variant & ~kVariantCapsMask) | (from.font_variant & kVariantCapsMask)); break;
    case pFontVariantNumeric: to->font_variant = static_cast<uint16_t>((to->font_variant & ~kVariantNumericMask) | (from.font_variant & kVariantNumericMask)); break;
    case pFontVariantLigatures: to->font_variant = static_cast<uint16_t>((to->font_variant & ~kVariantNoLigatures) | (from.font_variant & kVariantNoLigatures)); break;
    case pLetterSpacing: to->letter_spacing_px100 = from.letter_spacing_px100; break;
    case pWordSpacing: to->word_spacing_px100 = from.word_spacing_px100; break;
    case pBorderTopLeftRadius: case pBorderTopRightRadius: case pBorderBottomRightRadius: case pBorderBottomLeftRadius:
      to->radius[p - pBorderTopLeftRadius] = from.radius[p - pBorderTopLeftRadius];
      break;
    case pBoxShadow: to->box_shadow = from.box_shadow; break;
    case pTextShadow: to->text_shadow = from.text_shadow; break;
    case pBackgroundImage: to->background_image = from.background_image; break;
    case pBackgroundSize: to->background_size = from.background_size; break;
    case pBackgroundPosition: to->background_position = from.background_position; break;
    case pBackgroundRepeat: to->background_repeat = from.background_repeat; break;
    case pBackgroundClip: to->background_clip = from.background_clip; break;
    case pBackgroundOrigin: to->background_origin = from.background_origin; break;
    case pBackgroundAttachment: to->background_attachment = from.background_attachment; break;
    case pTransform: to->transform_value = from.transform_value; break;
    case pTranslate: to->translate = from.translate; break;
    case pRotate: to->rotate = from.rotate; break;
    case pScale: to->scale = from.scale; break;
    case pTransformOrigin: to->transform_origin_x = from.transform_origin_x; to->transform_origin_y = from.transform_origin_y; to->transform_origin_z_px100 = from.transform_origin_z_px100; break;
    case pOutlineWidth: to->outline_width_px100 = from.outline_width_px100; break;
    case pOutlineStyle: to->outline_style = from.outline_style; to->outline_auto = from.outline_auto; break;
    case pOutlineColor: to->outline_color = from.outline_color; break;
    case pOutlineOffset: to->outline_offset_px100 = from.outline_offset_px100; break;
    case pCursor: to->cursor = from.cursor; break;
    case pPointerEvents: to->pointer_events = from.pointer_events; break;
    case pUserSelect: to->user_select = from.user_select; break;
    case pAspectRatio: to->aspect_ratio_x1000 = from.aspect_ratio_x1000; to->aspect_auto = from.aspect_auto; break;
    case pObjectFit: to->object_fit = from.object_fit; break;
    case pObjectPosition: to->object_position_x = from.object_position_x; to->object_position_y = from.object_position_y; break;
    case pListStyleImage: to->list_style_image = from.list_style_image; break;
    case pColumnRuleWidth: to->column_rule_width_px100 = from.column_rule_width_px100; break;
    case pColumnRuleStyle: to->column_rule_style = from.column_rule_style; break;
    case pColumnRuleColor: to->column_rule_color = from.column_rule_color; break;
    case pTransitionProperty: to->transition_property = from.transition_property; break;
    case pTransitionDuration: to->transition_duration = from.transition_duration; break;
    case pTransitionTimingFunction: to->transition_timing = from.transition_timing; break;
    case pTransitionDelay: to->transition_delay = from.transition_delay; break;
    case pTransitionBehavior: to->transition_behavior = from.transition_behavior; break;
    case pAnimationName: to->animation_name = from.animation_name; break;
    case pAnimationDuration: to->animation_duration = from.animation_duration; break;
    case pAnimationTimingFunction: to->animation_timing = from.animation_timing; break;
    case pAnimationDelay: to->animation_delay = from.animation_delay; break;
    case pAnimationIterationCount: to->animation_iteration_count = from.animation_iteration_count; break;
    case pAnimationDirection: to->animation_direction = from.animation_direction; break;
    case pAnimationFillMode: to->animation_fill_mode = from.animation_fill_mode; break;
    case pAnimationPlayState: to->animation_play_state = from.animation_play_state; break;
    case pFilter: to->filter = from.filter; break;
    case pBackdropFilter: to->backdrop_filter = from.backdrop_filter; break;
    case pClipPath: to->clip_path = from.clip_path; break;
    case pWritingMode: to->writing_mode = from.writing_mode; break;
    case pDirection: to->direction = from.direction; break;
    case pColorScheme: to->color_scheme = from.color_scheme; break;
    case pScrollSnapType: to->scroll_snap_type = from.scroll_snap_type; break;
    case pScrollSnapAlign: to->scroll_snap_align = from.scroll_snap_align; break;
    case pScrollSnapStop: to->scroll_snap_stop_always = from.scroll_snap_stop_always; break;
    case pOverscrollBehaviorX: to->overscroll_x = from.overscroll_x; break;
    case pOverscrollBehaviorY: to->overscroll_y = from.overscroll_y; break;
    case pScrollBehavior: to->scroll_smooth = from.scroll_smooth; break;
    case pContainerType: to->container_type = from.container_type; break;
    case pContainerName: to->container_name = from.container_name; break;
    default: break;
  }
}

bool inherited_property(Prop p) {
  switch (p) {
    case pColor: case pFontSize: case pFontWeight: case pFontStyle: case pLineHeight: case pTextAlign: case pTextIndent: case pTextTransform: case pTextDecorationLine:
    case pWhiteSpace: case pWordBreak: case pOverflowWrap: case pListStyleType: case pListStylePosition: case pVisibility: case pBorderCollapse: case pBorderSpacing:
    case pCaptionSide: case pFontFamily: case pFontStretch: case pFontVariantCaps: case pFontVariantNumeric: case pFontVariantLigatures: case pLetterSpacing:
    case pWordSpacing: case pTextShadow: case pCursor: case pPointerEvents: case pUserSelect: case pWritingMode: case pDirection: case pColorScheme: case pListStyleImage:
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

// --- Values of the pixel engine's properties ---------------------------------------------------------------
//
// Each reads the text of one value (with var() worked out) against an EvalContext, so that the cascade can check a declaration and the readers of StyleSheet can
// give the structure back later.

namespace {

bool eq(StringView a, const char* b);

// "name(arguments)" -> name and the text between the brackets.
bool function_of(StringView t, StringView* name, StringView* inner) {
  t = t.trimmed();
  const uint64_t open = t.find('(');
  if (open == StringView::npos || !t.ends_with(')') || open == 0) return false;
  *name = t.first(open).trimmed();
  *inner = t.substr(open + 1, t.size() - open - 2);
  return true;
}

StringView unquote(StringView t) {
  t = t.trimmed();
  if (t.size() >= 2 && (t[0] == '"' || t[0] == '\'') && t[t.size() - 1] == t[0]) return t.substr(1, t.size() - 2);
  return t;
}

// A length in pixels with a unit (a bare number only when it is 0); percentages are not allowed.
bool length_px(StringView t, const EvalContext& ctx, double* px) {
  EvalContext c = ctx;
  c.has_percent = false;
  return evaluate_length(t, c, false, px);
}

bool dimension_token(StringView t, const EvalContext& ctx, Dimension* out, bool allow_auto) {
  t = t.trimmed();
  if (allow_auto && t.equals_ignore_case("auto")) { *out = Dimension::automatic(); return true; }
  double d;
  U u;
  if (parse_dimension(t, &d, &u) && u == U::Percent) { *out = Dimension::percent(to_px100(d)); return true; }
  double px;
  if (!length_px(t, ctx, &px)) return false;
  *out = Dimension::px(to_px100(px));
  return true;
}

bool angle_degrees(StringView t, const EvalContext& ctx, double* deg) {
  Quantity q;
  if (!Arithmetic(t.trimmed(), ctx).evaluate(&q)) return false;
  if (q.kind == Quantity::Angle || (q.kind == Quantity::Number && q.v == 0)) { *deg = q.v; return true; }
  return false;
}

bool time_ms(StringView t, const EvalContext& ctx, int32_t* ms) {
  Quantity q;
  if (!Arithmetic(t.trimmed(), ctx).evaluate(&q) || q.kind != Quantity::Time) return false;
  *ms = q.v > 2e9 ? 2000000000 : q.v < -2e9 ? -2000000000 : static_cast<int32_t>(q.v < 0 ? q.v - 0.5 : q.v + 0.5);
  return true;
}

// A number, or a percentage as a factor (50% -> 0.5).
bool number_or_percent(StringView t, const EvalContext& ctx, double* out) {
  Quantity q;
  if (!Arithmetic(t.trimmed(), ctx).evaluate(&q)) return false;
  if (q.kind == Quantity::Number) { *out = q.v; return true; }
  if (q.kind == Quantity::Percent) { *out = q.v / 100; return true; }
  return false;
}

bool is_css_wide(StringView t) { return eq(t, "inherit") || eq(t, "initial") || eq(t, "unset") || eq(t, "revert") || eq(t, "revert-layer"); }

// --- font-family

bool generic_family(StringView t, GenericFamily* out) {
  static const struct { const char* name; GenericFamily family; } kGenerics[] = {
      {"serif", GenericFamily::Serif}, {"sans-serif", GenericFamily::SansSerif}, {"monospace", GenericFamily::Monospace}, {"cursive", GenericFamily::Cursive},
      {"fantasy", GenericFamily::Fantasy}, {"system-ui", GenericFamily::SystemUi}, {"ui-serif", GenericFamily::UiSerif}, {"ui-sans-serif", GenericFamily::UiSansSerif},
      {"ui-monospace", GenericFamily::UiMonospace}, {"ui-rounded", GenericFamily::UiRounded}, {"math", GenericFamily::Math}, {"emoji", GenericFamily::Emoji},
      {"fangsong", GenericFamily::Fangsong}, {"-apple-system", GenericFamily::SystemUi}, {"blinkmacsystemfont", GenericFamily::SystemUi}};
  for (const auto& g : kGenerics)
    if (t.equals_ignore_case(g.name)) { *out = g.family; return true; }
  return false;
}

constexpr uint32_t kMaxFamilies = 32;

// font-family: a comma separated list of quoted names, unquoted names (identifiers separated by spaces) and generic families. Fills `out` when given.
bool parse_font_family_list(StringView value, List<FontFamily>* out) {
  uint64_t at = 0;
  StringView item;
  uint32_t count = 0;
  while (next_comma_part(value, &at, &item)) {
    if (item.empty() || count++ >= kMaxFamilies) return false;
    FontFamily family;
    if (item[0] == '"' || item[0] == '\'') {
      if (item.size() < 2 || item[item.size() - 1] != item[0]) return false;
      family.name = item.substr(1, item.size() - 2);
      if (family.name.empty()) return false;
    } else {
      uint64_t k = 0;
      StringView word;
      uint32_t words = 0;
      while (next_part(item, &k, &word)) {
        if (!is_ident_start(word[0]) || (word[0] == '-' && word.size() == 1)) return false;
        for (uint64_t j = 0; j < word.size(); ++j)
          if (!is_ident_char(word[j]) && word[j] != '\\') return false;
        ++words;
      }
      if (words == 0) return false;
      if (words == 1 && is_css_wide(item)) return false;
      if (words == 1 && generic_family(item, &family.generic)) family.name = StringView();
      else family.name = item;
    }
    if (out && !out->push_back(family)) return false;
  }
  return count > 0;
}

// --- positions

enum class PosKey : uint8_t { None, X, Y, Center };

PosKey position_keyword(StringView t, int32_t* percent_x100) {
  if (t.equals_ignore_case("left")) { *percent_x100 = 0; return PosKey::X; }
  if (t.equals_ignore_case("right")) { *percent_x100 = 10000; return PosKey::X; }
  if (t.equals_ignore_case("top")) { *percent_x100 = 0; return PosKey::Y; }
  if (t.equals_ignore_case("bottom")) { *percent_x100 = 10000; return PosKey::Y; }
  if (t.equals_ignore_case("center")) { *percent_x100 = 5000; return PosKey::Center; }
  return PosKey::None;
}

bool axis_from_value(StringView t, const EvalContext& ctx, AxisPos* out) {
  Dimension d;
  if (!dimension_token(t, ctx, &d, false)) return false;
  *out = d.kind == Dimension::Percent ? AxisPos{d.x100, 0} : AxisPos{0, d.x100};
  return true;
}

// background-position and object-position: 1 to 4 keywords and offsets.
bool parse_position(StringView value, const EvalContext& ctx, AxisPos* x, AxisPos* y) {
  StringView toks[4];
  uint32_t n = 0;
  uint64_t at = 0;
  StringView part;
  while (next_part(value, &at, &part)) {
    if (n == 4) return false;
    toks[n++] = part;
  }
  if (n == 0) return false;
  *x = AxisPos{5000, 0};
  *y = AxisPos{5000, 0};
  int32_t pct[4];
  PosKey key[4];
  for (uint32_t i = 0; i < n; ++i) key[i] = position_keyword(toks[i], &pct[i]);
  if (n <= 2) {
    if (n == 1) {
      if (key[0] == PosKey::Y) { *y = AxisPos{pct[0], 0}; return true; }
      if (key[0] != PosKey::None) { *x = AxisPos{pct[0], 0}; return true; }
      return axis_from_value(toks[0], ctx, x);
    }
    const bool swapped = key[0] == PosKey::Y || key[1] == PosKey::X;
    const uint32_t xi = swapped ? 1 : 0, yi = swapped ? 0 : 1;
    if (key[xi] == PosKey::Y || key[yi] == PosKey::X) return false;
    if (key[xi] != PosKey::None) *x = AxisPos{pct[xi], 0};
    else if (!axis_from_value(toks[xi], ctx, x)) return false;
    if (key[yi] != PosKey::None) *y = AxisPos{pct[yi], 0};
    else if (!axis_from_value(toks[yi], ctx, y)) return false;
    return true;
  }
  bool x_set = false, y_set = false;
  for (uint32_t i = 0; i < n;) {
    if (key[i] == PosKey::None) return false;
    AxisPos edge = {pct[i], 0};
    uint32_t used = 1;
    if (key[i] != PosKey::Center && i + 1 < n && key[i + 1] == PosKey::None) {   // an offset from that edge
      Dimension d;
      if (!dimension_token(toks[i + 1], ctx, &d, false)) return false;
      const bool far_edge = pct[i] == 10000;
      if (d.kind == Dimension::Percent) edge = AxisPos{far_edge ? 10000 - d.x100 : d.x100, 0};
      else edge = AxisPos{pct[i], far_edge ? -d.x100 : d.x100};
      used = 2;
    }
    const bool is_x = key[i] == PosKey::X || (key[i] == PosKey::Center && !x_set);
    if (is_x) {
      if (x_set) return false;
      *x = edge;
      x_set = true;
    } else {
      if (y_set) return false;
      *y = edge;
      y_set = true;
    }
    i += used;
  }
  return true;
}

// --- border-radius

bool parse_radius_lists(StringView value, const EvalContext& ctx, CornerRadius out[4]) {
  uint64_t at = 0;
  StringView halves[2];
  uint32_t halves_n = 0;
  StringView half;
  while (next_slash_part(value, &at, &half)) {
    if (halves_n == 2) return false;
    halves[halves_n++] = half;
  }
  if (halves_n == 0) return false;
  Dimension lists[2][4];
  for (uint32_t h = 0; h < halves_n; ++h) {
    Dimension v[4];
    uint32_t n = 0;
    uint64_t k = 0;
    StringView part;
    while (next_part(halves[h], &k, &part)) {
      if (n == 4 || !dimension_token(part, ctx, &v[n], false) || v[n].x100 < 0) return false;
      ++n;
    }
    if (n == 0) return false;
    lists[h][0] = v[0];
    lists[h][1] = n > 1 ? v[1] : v[0];
    lists[h][2] = n > 2 ? v[2] : v[0];
    lists[h][3] = n > 3 ? v[3] : (n > 1 ? v[1] : v[0]);
  }
  for (int i = 0; i < 4; ++i) {
    out[i].h = lists[0][i];
    out[i].v = halves_n == 2 ? lists[1][i] : lists[0][i];
  }
  return true;
}

// --- shadows

constexpr uint32_t kMaxShadows = 16;

bool parse_one_shadow(StringView item, const EvalContext& ctx, const ColorContext& colors, bool text_shadow, Shadow* out) {
  Shadow s;
  double lengths[4];
  uint32_t n = 0;
  bool colored = false, inset = false;
  uint64_t k = 0;
  StringView part;
  while (next_part(item, &k, &part)) {
    double px;
    if (part.equals_ignore_case("inset")) {
      if (text_shadow || inset) return false;
      inset = true;
    } else if (length_px(part, ctx, &px)) {
      if (n == 4) return false;
      lengths[n++] = px;
    } else {
      uint32_t c;
      if (colored || !color_value(part, colors, 0, &c)) return false;
      colored = true;
      s.color = c;
      s.current_color = part.trimmed().equals_ignore_case("currentcolor");
    }
  }
  if (n < 2 || (text_shadow && n > 3)) return false;
  if (n >= 3 && lengths[2] < 0) return false;
  s.x_px100 = to_px100(lengths[0]);
  s.y_px100 = to_px100(lengths[1]);
  s.blur_px100 = n >= 3 ? to_px100(lengths[2]) : 0;
  s.spread_px100 = n >= 4 ? to_px100(lengths[3]) : 0;
  s.inset = inset;
  if (!colored) s.current_color = true;
  *out = s;
  return true;
}

bool parse_shadow_list(StringView value, const EvalContext& ctx, const ColorContext& colors, bool text_shadow, List<Shadow>* out) {
  uint64_t at = 0;
  StringView item;
  uint32_t count = 0;
  while (next_comma_part(value, &at, &item)) {
    Shadow s;
    if (count++ >= kMaxShadows || !parse_one_shadow(item, ctx, colors, text_shadow, &s)) return false;
    if (out && !out->push_back(s)) return false;
  }
  return count > 0;
}

// --- gradients

bool gradient_function(StringView name, Gradient::Kind* kind, bool* repeating, bool* legacy) {
  *legacy = false;
  StringView n = name;
  if (n.starts_with("-webkit-") || n.starts_with("-moz-") || n.starts_with("-o-")) {
    *legacy = true;
    const uint64_t dash = n.find('-', 1);
    if (dash == StringView::npos) return false;
    n = n.substr(dash + 1);
  }
  *repeating = false;
  if (n.starts_with("repeating-")) { *repeating = true; n = n.substr(10); }
  if (n.equals_ignore_case("linear-gradient")) { *kind = Gradient::Linear; return true; }
  if (n.equals_ignore_case("radial-gradient")) { *kind = Gradient::Radial; *legacy = false; return true; }
  if (n.equals_ignore_case("conic-gradient")) { *kind = Gradient::Conic; *legacy = false; return true; }
  return false;
}

// A colour stop: "<color> [<position> [<position>]]", or a hint "<position>". Positions are lengths / percentages, or (conic) angles as a part of the turn.
bool parse_color_stops(StringView arg, const EvalContext& ctx, const ColorContext& colors, bool conic, Gradient* g) {
  uint64_t k = 0;
  StringView part, tokens[3];
  uint32_t n = 0;
  while (next_part(arg, &k, &part)) {
    if (n == 3) return false;
    tokens[n++] = part;
  }
  if (n == 0) return false;
  auto position = [&](StringView t, Dimension* d) {
    if (conic) {
      double deg;
      if (angle_degrees(t, ctx, &deg)) { *d = Dimension::percent(to_px100(deg / 3.6)); return true; }
    }
    return dimension_token(t, ctx, d, false);
  };
  uint32_t c;
  ColorStop stop;
  if (color_value(tokens[0], colors, 0, &c)) {   // colour first
    stop.color = c;
    stop.current_color = tokens[0].trimmed().equals_ignore_case("currentcolor");
    if (n >= 2 && !position(tokens[1], &stop.position)) return false;
    if (g->stop_count < kMaxGradientStops) g->stops[g->stop_count++] = stop;
    if (n == 3) {
      ColorStop second = stop;
      if (!position(tokens[2], &second.position)) return false;
      if (g->stop_count < kMaxGradientStops) g->stops[g->stop_count++] = second;
    }
    return true;
  }
  if (n != 1) return false;   // a hint
  stop.hint = true;
  if (!position(tokens[0], &stop.position)) return false;
  if (g->stop_count < kMaxGradientStops) g->stops[g->stop_count++] = stop;
  return true;
}

// The first argument of a radial or conic gradient when it is not a colour stop: shape, size, "from <angle>", "at <position>", "in <space>".
bool parse_gradient_config(StringView arg, const EvalContext& ctx, Gradient* g) {
  uint64_t k = 0;
  StringView t;
  Dimension radii[2];
  uint32_t radii_n = 0;
  bool shape_circle = false, shape_ellipse = false;
  while (next_part(arg, &k, &t)) {
    if (t.equals_ignore_case("in")) break;   // (the colour space for interpolation is not used)
    if (t.equals_ignore_case("at")) {
      uint64_t end = arg.size(), kk = k;
      StringView u;
      for (;;) {
        const uint64_t before = kk;
        if (!next_part(arg, &kk, &u)) break;
        if (u.equals_ignore_case("in")) { end = before; break; }
      }
      if (!parse_position(arg.substr(k, end - k), ctx, &g->center_x, &g->center_y)) return false;
      break;
    }
    if (g->kind == Gradient::Conic) {
      double deg;
      if (!t.equals_ignore_case("from") || !next_part(arg, &k, &t) || !angle_degrees(t, ctx, &deg)) return false;
      deg -= 360 * floor_of(deg / 360);
      g->angle_x100 = to_px100(deg);
      continue;
    }
    if (t.equals_ignore_case("circle")) shape_circle = true;
    else if (t.equals_ignore_case("ellipse")) shape_ellipse = true;
    else if (t.equals_ignore_case("closest-side")) g->extent = Gradient::ClosestSide;
    else if (t.equals_ignore_case("closest-corner")) g->extent = Gradient::ClosestCorner;
    else if (t.equals_ignore_case("farthest-side")) g->extent = Gradient::FarthestSide;
    else if (t.equals_ignore_case("farthest-corner")) g->extent = Gradient::FarthestCorner;
    else {
      if (radii_n == 2 || !dimension_token(t, ctx, &radii[radii_n], false)) return false;
      ++radii_n;
    }
  }
  if (g->kind == Gradient::Radial) {
    if (shape_circle && shape_ellipse) return false;
    g->circle = shape_circle || (!shape_ellipse && radii_n == 1);
    if (radii_n > 0) {
      if (g->circle && (radii_n != 1 || radii[0].kind == Dimension::Percent)) return false;
      if (!g->circle && radii_n != 2) return false;
      g->extent = Gradient::Explicit;
      g->radius_x = radii[0];
      g->radius_y = radii_n == 2 ? radii[1] : radii[0];
    }
  }
  return true;
}

bool parse_gradient(StringView text, const EvalContext& ctx, const ColorContext& colors, Gradient* g) {
  StringView name, inner;
  bool legacy;
  if (!function_of(text, &name, &inner)) return false;
  *g = Gradient();
  if (!gradient_function(name, &g->kind, &g->repeating, &legacy)) return false;
  uint64_t at = 0;
  StringView arg;
  bool first = true;
  while (next_comma_part(inner, &at, &arg)) {
    if (arg.empty()) return false;
    if (first) {
      first = false;
      uint32_t probe;
      uint64_t pk = 0;
      StringView p0;
      next_part(arg, &pk, &p0);
      if (!color_value(p0, colors, 0, &probe)) {   // not a colour stop: the direction / shape
        if (g->kind != Gradient::Linear) {
          if (!parse_gradient_config(arg, ctx, g)) return false;
          continue;
        }
        double deg;
        StringView toks[4];
        uint32_t n = 0;
        uint64_t k = 0;
        StringView part;
        while (next_part(arg, &k, &part)) {
          if (n == 4) return false;
          toks[n++] = part;
        }
        auto side_angle = [](uint8_t sides) { return sides == 1 ? 0 : sides == 2 ? 9000 : sides == 4 ? 18000 : sides == 8 ? 27000 : 18000; };
        if (!legacy && n >= 2 && toks[0].equals_ignore_case("to")) {
          uint8_t sides = 0;
          for (uint32_t i = 1; i < n; ++i) {
            if (toks[i].equals_ignore_case("top")) sides |= 1;
            else if (toks[i].equals_ignore_case("right")) sides |= 2;
            else if (toks[i].equals_ignore_case("bottom")) sides |= 4;
            else if (toks[i].equals_ignore_case("left")) sides |= 8;
            else return false;
          }
          if (((sides & 1) && (sides & 4)) || ((sides & 2) && (sides & 8))) return false;
          g->to_sides = sides;
          g->angle_x100 = side_angle(sides);
        } else if (legacy && (toks[0].equals_ignore_case("top") || toks[0].equals_ignore_case("bottom") || toks[0].equals_ignore_case("left") || toks[0].equals_ignore_case("right"))) {
          uint8_t sides = 0;   // (the side it starts at: the gradient goes to the opposite one)
          for (uint32_t i = 0; i < n; ++i) {
            if (toks[i].equals_ignore_case("top")) sides |= 4;
            else if (toks[i].equals_ignore_case("right")) sides |= 8;
            else if (toks[i].equals_ignore_case("bottom")) sides |= 1;
            else if (toks[i].equals_ignore_case("left")) sides |= 2;
            else return false;
          }
          g->to_sides = sides;
          g->angle_x100 = side_angle(sides);
        } else if (angle_degrees(toks[0], ctx, &deg)) {
          if (legacy) deg = 90 - deg;
          deg -= 360 * floor_of(deg / 360);
          g->angle_x100 = to_px100(deg);
          if (n > 1 && !(n == 3 && toks[1].equals_ignore_case("in"))) return false;
        } else {
          return false;
        }
        continue;
      }
    }
    if (!parse_color_stops(arg, ctx, colors, g->kind == Gradient::Conic, g)) return false;
  }
  return g->stop_count >= 2;
}

// --- images

// One item of background-image / list-style-image: none, url(), a gradient, or another image function.
bool parse_image_item(StringView t, const EvalContext& ctx, const ColorContext& colors, BackgroundLayer* layer) {
  t = t.trimmed();
  *layer = BackgroundLayer();
  if (t.equals_ignore_case("none")) return true;
  StringView name, inner;
  if (!function_of(t, &name, &inner)) return false;
  if (name.equals_ignore_case("url") || name.equals_ignore_case("src")) {
    layer->image = BackgroundLayer::Url;
    layer->url = unquote(inner);
    return true;
  }
  Gradient::Kind kind;
  bool repeating, legacy;
  if (gradient_function(name, &kind, &repeating, &legacy)) {
    layer->image = BackgroundLayer::GradientImage;
    return parse_gradient(t, ctx, colors, &layer->gradient);
  }
  if (name.equals_ignore_case("image-set") || name.equals_ignore_case("-webkit-image-set") || name.equals_ignore_case("cross-fade") || name.equals_ignore_case("element") ||
      name.equals_ignore_case("image") || name.equals_ignore_case("paint")) {
    layer->image = BackgroundLayer::Other;
    return true;
  }
  return false;
}

// --- animation timing

bool parse_timing_function(StringView t, const EvalContext& ctx, TimingFunction* out) {
  t = t.trimmed();
  *out = TimingFunction();
  struct Keyword { const char* name; double x1, y1, x2, y2; };
  static const Keyword kKeywords[] = {{"ease", 0.25, 0.1, 0.25, 1.0}, {"ease-in", 0.42, 0, 1, 1}, {"ease-out", 0, 0, 0.58, 1}, {"ease-in-out", 0.42, 0, 0.58, 1}};
  for (const Keyword& k : kKeywords)
    if (t.equals_ignore_case(k.name)) { out->kind = TimingFunction::CubicBezier; out->x1 = k.x1; out->y1 = k.y1; out->x2 = k.x2; out->y2 = k.y2; return true; }
  if (t.equals_ignore_case("linear")) { out->kind = TimingFunction::Linear; return true; }
  if (t.equals_ignore_case("step-start")) { out->kind = TimingFunction::Steps; out->steps = 1; out->jump_start = true; return true; }
  if (t.equals_ignore_case("step-end")) { out->kind = TimingFunction::Steps; out->steps = 1; out->jump_start = false; return true; }
  StringView name, inner;
  if (!function_of(t, &name, &inner)) return false;
  if (name.equals_ignore_case("linear")) { out->kind = TimingFunction::Linear; return !inner.trimmed().empty(); }
  uint64_t at = 0;
  StringView args[5];
  uint32_t n = 0;
  StringView a;
  while (next_comma_part(inner, &at, &a)) {
    if (n == 5) return false;
    args[n++] = a;
  }
  if (name.equals_ignore_case("cubic-bezier")) {
    if (n != 4) return false;
    double v[4];
    for (int i = 0; i < 4; ++i)
      if (!evaluate_number(args[i], ctx, &v[i])) return false;
    if (v[0] < 0 || v[0] > 1 || v[2] < 0 || v[2] > 1) return false;
    out->kind = TimingFunction::CubicBezier;
    out->x1 = v[0]; out->y1 = v[1]; out->x2 = v[2]; out->y2 = v[3];
    return true;
  }
  if (name.equals_ignore_case("steps")) {
    double steps;
    if (n < 1 || n > 2 || !evaluate_number(args[0], ctx, &steps) || steps < 1 || steps > 1000 || steps != floor_of(steps)) return false;
    out->kind = TimingFunction::Steps;
    out->steps = static_cast<int32_t>(steps);
    if (n == 2) {
      if (args[1].equals_ignore_case("jump-start") || args[1].equals_ignore_case("start")) out->jump_start = true;
      else if (args[1].equals_ignore_case("jump-end") || args[1].equals_ignore_case("end")) out->jump_start = false;
      else if (args[1].equals_ignore_case("jump-none")) { if (steps < 2) return false; }
      else if (args[1].equals_ignore_case("jump-both")) out->jump_start = true;
      else return false;
    }
    return true;
  }
  return false;
}

// --- transforms

constexpr uint32_t kMaxTransformOps = 32;

bool matrix_numbers(StringView* args, uint32_t n, const EvalContext& ctx, double* out) {
  for (uint32_t i = 0; i < n; ++i)
    if (!evaluate_number(args[i], ctx, &out[i])) return false;
  return true;
}

bool length_or_percent_dim(StringView t, const EvalContext& ctx, Dimension* d) { return dimension_token(t, ctx, d, false); }

bool parse_transform_function(StringView part, const EvalContext& ctx, TransformOp* op) {
  StringView name, inner;
  if (!function_of(part, &name, &inner)) return false;
  StringView args[16];
  uint32_t n = 0;
  uint64_t at = 0;
  StringView a;
  while (next_comma_part(inner, &at, &a)) {
    if (n == 16 || a.empty()) return false;
    args[n++] = a;
  }
  *op = TransformOp();
  auto is = [&](const char* s) { return name.equals_ignore_case(s); };
  double v[16];
  if (is("matrix")) {
    if (n != 6 || !matrix_numbers(args, 6, ctx, v)) return false;
    op->kind = TransformOp::Matrix;
    op->m[0] = v[0]; op->m[1] = v[1]; op->m[4] = v[2]; op->m[5] = v[3]; op->m[12] = v[4]; op->m[13] = v[5];
    return true;
  }
  if (is("matrix3d")) {
    if (n != 16 || !matrix_numbers(args, 16, ctx, v)) return false;
    op->kind = TransformOp::Matrix;
    for (int i = 0; i < 16; ++i) op->m[i] = v[i];
    return true;
  }
  if (is("translate") || is("translatex") || is("translatey") || is("translatez") || is("translate3d")) {
    op->kind = TransformOp::Translate;
    double px;
    if (is("translate")) {
      if (n < 1 || n > 2 || !length_or_percent_dim(args[0], ctx, &op->x)) return false;
      if (n == 2 && !length_or_percent_dim(args[1], ctx, &op->y)) return false;
    } else if (is("translatex")) {
      if (n != 1 || !length_or_percent_dim(args[0], ctx, &op->x)) return false;
    } else if (is("translatey")) {
      if (n != 1 || !length_or_percent_dim(args[0], ctx, &op->y)) return false;
    } else if (is("translatez")) {
      if (n != 1 || !length_px(args[0], ctx, &px)) return false;
      op->z_px = px;
    } else {
      if (n != 3 || !length_or_percent_dim(args[0], ctx, &op->x) || !length_or_percent_dim(args[1], ctx, &op->y) || !length_px(args[2], ctx, &px)) return false;
      op->z_px = px;
    }
    return true;
  }
  if (is("scale") || is("scalex") || is("scaley") || is("scalez") || is("scale3d")) {
    op->kind = TransformOp::Scale;
    double f[3];
    for (uint32_t i = 0; i < n && i < 3; ++i)
      if (!number_or_percent(args[i], ctx, &f[i])) return false;
    if (is("scale")) {
      if (n < 1 || n > 2) return false;
      op->sx = f[0];
      op->sy = n == 2 ? f[1] : f[0];
    } else if (is("scalex")) {
      if (n != 1) return false;
      op->sx = f[0];
    } else if (is("scaley")) {
      if (n != 1) return false;
      op->sy = f[0];
    } else if (is("scalez")) {
      if (n != 1) return false;
      op->sz = f[0];
    } else {
      if (n != 3) return false;
      op->sx = f[0]; op->sy = f[1]; op->sz = f[2];
    }
    return true;
  }
  if (is("rotate") || is("rotatex") || is("rotatey") || is("rotatez") || is("rotate3d")) {
    op->kind = TransformOp::Rotate;
    if (is("rotate3d")) {
      double ax[3];
      if (n != 4 || !matrix_numbers(args, 3, ctx, ax) || !angle_degrees(args[3], ctx, &op->angle_deg)) return false;
      op->ax = ax[0]; op->ay = ax[1]; op->az = ax[2];
      return true;
    }
    if (n != 1 || !angle_degrees(args[0], ctx, &op->angle_deg)) return false;
    op->ax = is("rotatex") ? 1 : 0;
    op->ay = is("rotatey") ? 1 : 0;
    op->az = is("rotatex") || is("rotatey") ? 0 : 1;
    return true;
  }
  if (is("skew") || is("skewx") || is("skewy")) {
    op->kind = TransformOp::Skew;
    if (is("skew")) {
      if (n < 1 || n > 2 || !angle_degrees(args[0], ctx, &op->angle_deg)) return false;
      if (n == 2 && !angle_degrees(args[1], ctx, &op->angle_y_deg)) return false;
    } else if (is("skewx")) {
      if (n != 1 || !angle_degrees(args[0], ctx, &op->angle_deg)) return false;
    } else {
      if (n != 1 || !angle_degrees(args[0], ctx, &op->angle_y_deg)) return false;
    }
    return true;
  }
  if (is("perspective")) {
    op->kind = TransformOp::Perspective;
    double px;
    if (n != 1) return false;
    if (args[0].equals_ignore_case("none")) { op->z_px = 0; return true; }
    if (!length_px(args[0], ctx, &px) || px < 0) return false;
    op->z_px = px;
    return true;
  }
  return false;
}

bool parse_transform_list(StringView value, const EvalContext& ctx, List<TransformOp>* out) {
  uint64_t at = 0;
  StringView part;
  uint32_t count = 0;
  while (next_part(value, &at, &part)) {
    TransformOp op;
    if (count++ >= kMaxTransformOps || !parse_transform_function(part, ctx, &op)) return false;
    if (out && !out->push_back(op)) return false;
  }
  return count > 0;
}

// translate: x [y [z]]
bool parse_translate_property(StringView value, const EvalContext& ctx, TransformOp* op) {
  StringView t[3];
  uint32_t n = 0;
  uint64_t at = 0;
  StringView part;
  while (next_part(value, &at, &part)) {
    if (n == 3) return false;
    t[n++] = part;
  }
  *op = TransformOp();
  op->kind = TransformOp::Translate;
  if (n == 0 || !length_or_percent_dim(t[0], ctx, &op->x)) return false;
  if (n >= 2 && !length_or_percent_dim(t[1], ctx, &op->y)) return false;
  if (n == 3) {
    double px;
    if (!length_px(t[2], ctx, &px)) return false;
    op->z_px = px;
  }
  return true;
}

// rotate: angle | x|y|z angle | x y z angle
bool parse_rotate_property(StringView value, const EvalContext& ctx, TransformOp* op) {
  StringView t[4];
  uint32_t n = 0;
  uint64_t at = 0;
  StringView part;
  while (next_part(value, &at, &part)) {
    if (n == 4) return false;
    t[n++] = part;
  }
  *op = TransformOp();
  op->kind = TransformOp::Rotate;
  if (n == 1) return angle_degrees(t[0], ctx, &op->angle_deg);
  if (n == 2) {
    if (!angle_degrees(t[1], ctx, &op->angle_deg)) return false;
    op->ax = t[0].equals_ignore_case("x") ? 1 : 0;
    op->ay = t[0].equals_ignore_case("y") ? 1 : 0;
    op->az = t[0].equals_ignore_case("z") ? 1 : 0;
    return op->ax + op->ay + op->az == 1;
  }
  if (n == 4) {
    double ax[3];
    if (!matrix_numbers(t, 3, ctx, ax) || !angle_degrees(t[3], ctx, &op->angle_deg)) return false;
    op->ax = ax[0]; op->ay = ax[1]; op->az = ax[2];
    return true;
  }
  return false;
}

// scale: x [y [z]]
bool parse_scale_property(StringView value, const EvalContext& ctx, TransformOp* op) {
  StringView t[3];
  uint32_t n = 0;
  uint64_t at = 0;
  StringView part;
  while (next_part(value, &at, &part)) {
    if (n == 3) return false;
    t[n++] = part;
  }
  *op = TransformOp();
  op->kind = TransformOp::Scale;
  if (n == 0 || !number_or_percent(t[0], ctx, &op->sx)) return false;
  op->sy = op->sx;
  if (n >= 2 && !number_or_percent(t[1], ctx, &op->sy)) return false;
  if (n == 3 && !number_or_percent(t[2], ctx, &op->sz)) return false;
  return true;
}

// --- filters

constexpr uint32_t kMaxFilters = 16;

bool parse_filter_list(StringView value, const EvalContext& ctx, const ColorContext& colors, List<FilterFunction>* out) {
  uint64_t at = 0;
  StringView part;
  uint32_t count = 0;
  while (next_part(value, &at, &part)) {
    StringView name, inner;
    if (count++ >= kMaxFilters || !function_of(part, &name, &inner)) return false;
    inner = inner.trimmed();
    FilterFunction f;
    auto is = [&](const char* s) { return name.equals_ignore_case(s); };
    double v;
    if (is("url")) {
      f.kind = FilterFunction::Url;
      f.url = unquote(inner);
    } else if (is("blur")) {
      f.kind = FilterFunction::Blur;
      if (!inner.empty()) {
        if (!length_px(inner, ctx, &v) || v < 0) return false;
        f.amount = v;
      }
    } else if (is("hue-rotate")) {
      f.kind = FilterFunction::HueRotate;
      if (!inner.empty() && !angle_degrees(inner, ctx, &f.amount)) return false;
    } else if (is("drop-shadow")) {
      f.kind = FilterFunction::DropShadow;
      if (!parse_one_shadow(inner, ctx, colors, true, &f.shadow)) return false;
    } else {
      static const struct { const char* name; FilterFunction::Kind kind; } kAmounts[] = {
          {"brightness", FilterFunction::Brightness}, {"contrast", FilterFunction::Contrast}, {"grayscale", FilterFunction::Grayscale}, {"invert", FilterFunction::Invert},
          {"opacity", FilterFunction::Opacity}, {"saturate", FilterFunction::Saturate}, {"sepia", FilterFunction::Sepia}};
      bool found = false;
      for (const auto& a : kAmounts) {
        if (!is(a.name)) continue;
        f.kind = a.kind;
        f.amount = 1;
        if (!inner.empty() && (!number_or_percent(inner, ctx, &f.amount) || f.amount < 0)) return false;
        found = true;
      }
      if (!found) return false;
    }
    if (out && !out->push_back(f)) return false;
  }
  return count > 0;
}

// --- clip-path

bool parse_clip_path_text(StringView value, const EvalContext& ctx, ClipPath* out) {
  *out = ClipPath();
  value = value.trimmed();
  if (value.equals_ignore_case("none")) return true;
  uint64_t at = 0;
  StringView part;
  bool have_shape = false, have_box = false;
  while (next_part(value, &at, &part)) {
    BackgroundBox box = BackgroundBox::BorderBox;
    bool is_box = true;
    if (part.equals_ignore_case("border-box") || part.equals_ignore_case("margin-box") || part.equals_ignore_case("stroke-box")) box = BackgroundBox::BorderBox;
    else if (part.equals_ignore_case("padding-box")) box = BackgroundBox::PaddingBox;
    else if (part.equals_ignore_case("content-box") || part.equals_ignore_case("fill-box") || part.equals_ignore_case("view-box")) box = BackgroundBox::ContentBox;
    else is_box = false;
    if (is_box) {
      if (have_box) return false;
      have_box = true;
      out->reference_box = box;
      if (!have_shape) out->kind = ClipPath::Box;
      continue;
    }
    StringView name, inner;
    if (have_shape || !function_of(part, &name, &inner)) return false;
    have_shape = true;
    inner = inner.trimmed();
    if (name.equals_ignore_case("url")) {
      out->kind = ClipPath::Url;
      out->url = unquote(inner);
    } else if (name.equals_ignore_case("inset")) {
      out->kind = ClipPath::Inset;
      uint64_t k = 0;
      StringView t;
      Dimension d[4];
      uint32_t n = 0;
      while (next_part(inner, &k, &t)) {
        if (t.equals_ignore_case("round")) {
          if (!parse_radius_lists(inner.substr(k).trimmed(), ctx, out->round)) return false;
          break;
        }
        if (n == 4 || !dimension_token(t, ctx, &d[n], false)) return false;
        ++n;
      }
      if (n == 0) return false;
      out->inset[0] = d[0];
      out->inset[1] = n > 1 ? d[1] : d[0];
      out->inset[2] = n > 2 ? d[2] : d[0];
      out->inset[3] = n > 3 ? d[3] : (n > 1 ? d[1] : d[0]);
    } else if (name.equals_ignore_case("circle") || name.equals_ignore_case("ellipse")) {
      const bool circle = name.equals_ignore_case("circle");
      out->kind = circle ? ClipPath::Circle : ClipPath::Ellipse;
      uint64_t k = 0;
      StringView t;
      uint32_t radii = 0;
      while (next_part(inner, &k, &t)) {
        if (t.equals_ignore_case("at")) {
          if (!parse_position(inner.substr(k).trimmed(), ctx, &out->center_x, &out->center_y)) return false;
          break;
        }
        Dimension d;
        if (t.equals_ignore_case("closest-side") || t.equals_ignore_case("farthest-side")) {
          d = Dimension::automatic();
          out->radius_far = t.equals_ignore_case("farthest-side");
        } else if (!dimension_token(t, ctx, &d, false) || d.x100 < 0) {
          return false;
        }
        if (radii == (circle ? 1u : 2u)) return false;
        (radii == 0 ? out->radius_x : out->radius_y) = d;
        ++radii;
      }
      if (!circle && radii == 1) return false;
      if (circle) out->radius_y = out->radius_x;
    } else if (name.equals_ignore_case("polygon")) {
      out->kind = ClipPath::Polygon;
      uint64_t k = 0;
      StringView item;
      bool first = true;
      while (next_comma_part(inner, &k, &item)) {
        if (first && (item.equals_ignore_case("evenodd") || item.equals_ignore_case("nonzero"))) {
          out->evenodd = item.equals_ignore_case("evenodd");
          first = false;
          continue;
        }
        first = false;
        uint64_t pk = 0;
        StringView xs, ys, extra;
        if (!next_part(item, &pk, &xs) || !next_part(item, &pk, &ys) || next_part(item, &pk, &extra)) return false;
        if (out->point_count == ClipPath::kMaxPoints) return false;
        if (!dimension_token(xs, ctx, &out->points_x[out->point_count], false) || !dimension_token(ys, ctx, &out->points_y[out->point_count], false)) return false;
        ++out->point_count;
      }
      if (out->point_count == 0) return false;
    } else if (name.equals_ignore_case("path")) {
      out->kind = ClipPath::Path;
      uint64_t k = 0;
      StringView item;
      while (next_comma_part(inner, &k, &item)) {
        if (item.equals_ignore_case("evenodd") || item.equals_ignore_case("nonzero")) out->evenodd = item.equals_ignore_case("evenodd");
        else if (item.size() >= 2 && (item[0] == '"' || item[0] == '\'')) out->path_data = unquote(item);
        else return false;
      }
      if (out->path_data.empty()) return false;
    } else {
      return false;
    }
  }
  return have_shape || have_box;
}

}  // namespace

// --- background layer pieces

namespace {

bool parse_bg_size_item(StringView t, const EvalContext& ctx, BackgroundLayer* layer) {
  t = t.trimmed();
  if (t.equals_ignore_case("cover")) { layer->size_mode = BackgroundLayer::Cover; return true; }
  if (t.equals_ignore_case("contain")) { layer->size_mode = BackgroundLayer::Contain; return true; }
  uint64_t at = 0;
  StringView a, b, extra;
  if (!next_part(t, &at, &a)) return false;
  Dimension w, h = Dimension::automatic();
  if (!dimension_token(a, ctx, &w, true) || (w.kind != Dimension::Auto && w.x100 < 0)) return false;
  if (next_part(t, &at, &b)) {
    if (!dimension_token(b, ctx, &h, true) || (h.kind != Dimension::Auto && h.x100 < 0) || next_part(t, &at, &extra)) return false;
  }
  layer->size_mode = w.is_auto() && h.is_auto() ? BackgroundLayer::Auto : BackgroundLayer::Explicit;
  layer->size_w = w;
  layer->size_h = h;
  return true;
}

bool repeat_keyword(StringView t, RepeatMode* out) {
  if (t.equals_ignore_case("repeat")) { *out = RepeatMode::Repeat; return true; }
  if (t.equals_ignore_case("space")) { *out = RepeatMode::Space; return true; }
  if (t.equals_ignore_case("round")) { *out = RepeatMode::Round; return true; }
  if (t.equals_ignore_case("no-repeat")) { *out = RepeatMode::NoRepeat; return true; }
  return false;
}

bool parse_bg_repeat_item(StringView t, BackgroundLayer* layer) {
  t = t.trimmed();
  if (t.equals_ignore_case("repeat-x")) { layer->repeat_x = RepeatMode::Repeat; layer->repeat_y = RepeatMode::NoRepeat; return true; }
  if (t.equals_ignore_case("repeat-y")) { layer->repeat_x = RepeatMode::NoRepeat; layer->repeat_y = RepeatMode::Repeat; return true; }
  uint64_t at = 0;
  StringView a, b, extra;
  if (!next_part(t, &at, &a)) return false;
  RepeatMode x, y;
  if (!repeat_keyword(a, &x)) return false;
  y = x;
  if (next_part(t, &at, &b)) {
    if (!repeat_keyword(b, &y) || next_part(t, &at, &extra)) return false;
  }
  layer->repeat_x = x;
  layer->repeat_y = y;
  return true;
}

bool parse_bg_box(StringView t, bool allow_text, BackgroundBox* out) {
  t = t.trimmed();
  if (t.equals_ignore_case("border-box")) { *out = BackgroundBox::BorderBox; return true; }
  if (t.equals_ignore_case("padding-box")) { *out = BackgroundBox::PaddingBox; return true; }
  if (t.equals_ignore_case("content-box")) { *out = BackgroundBox::ContentBox; return true; }
  if (allow_text && t.equals_ignore_case("text")) { *out = BackgroundBox::Text; return true; }
  return false;
}

bool parse_bg_attachment(StringView t, BackgroundAttachment* out) {
  t = t.trimmed();
  if (t.equals_ignore_case("scroll")) { *out = BackgroundAttachment::Scroll; return true; }
  if (t.equals_ignore_case("fixed")) { *out = BackgroundAttachment::Fixed; return true; }
  if (t.equals_ignore_case("local")) { *out = BackgroundAttachment::Local; return true; }
  return false;
}

// Is the token an image (so that the background shorthand knows where it goes)?
bool looks_like_image(StringView t) {
  StringView name, inner;
  if (t.equals_ignore_case("none")) return true;
  if (!function_of(t, &name, &inner)) return false;
  Gradient::Kind kind;
  bool repeating, legacy;
  return name.equals_ignore_case("url") || name.equals_ignore_case("image-set") || name.equals_ignore_case("-webkit-image-set") || name.equals_ignore_case("cross-fade") ||
         name.equals_ignore_case("element") || name.equals_ignore_case("image") || gradient_function(name, &kind, &repeating, &legacy);
}

bool is_time_token(StringView t, const EvalContext& ctx) {
  int32_t ms;
  return time_ms(t, ctx, &ms);
}

bool custom_ident(StringView t) {
  if (t.empty() || !is_ident_start(t[0]) || is_css_wide(t)) return false;
  for (uint64_t i = 0; i < t.size(); ++i)
    if (!is_ident_char(t[i]) && t[i] != '\\') return false;
  return true;
}

}  // namespace

// --- Applying declarations ---------------------------------------------------------------------------

// Works out declared values against one element's style (a friend of StyleSheet: it reads the custom property scopes).
struct CssApply {
  const StyleSheet& sheet;
  const Style& parent;
  Style* style;
  const dom::Node* element;
  EvalContext eval;
  const Style initial;   // the style with every property at its initial value
  uint32_t inherited_scope;   // the custom property scope the style started with (what the parent passed on)

  CssApply(const StyleSheet& s, const Style& p, Style* st, const dom::Node* el) : sheet(s), parent(p), style(st), element(el), inherited_scope(st->custom_scope) { refresh(); }

  void refresh() {
    eval.font_px = style->font_px;
    eval.root_px = style->root_font_px;
    eval.viewport_w = sheet.viewport_w_;
    eval.viewport_h = sheet.viewport_h_;
    eval.percent_px = style->content_px;
    eval.has_percent = true;
  }

  // --- custom properties
  bool lookup(StringView name, StringView* value) const {
    uint32_t at = style->custom_scope;
    bool inherits = true;
    const StyleSheet::Registration* registered = nullptr;
    for (uint64_t r = 0; r < sheet.registrations_.count(); ++r) {
      const StyleSheet::Registration& reg = sheet.registrations_[r];
      if (sheet.span_text(reg.name).equals(name)) { registered = &reg; inherits = reg.inherits; }
    }
    for (uint32_t guard = 0; at != 0 && guard < kMaxCustomScopes; ++guard) {
      if (!inherits && at == inherited_scope) break;   // (a registered property that does not inherit: only what this element set)
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
    if (registered && registered->has_initial) {
      *value = sheet.span_text(registered->initial);
      return true;
    }
    return false;
  }

  // The attribute `name` of the element as text.
  bool attribute_text(StringView name, StringView* value) const { return element && dom::get_attribute(element, name, value); }

  // The body of an env(...) or attr(...) call (what is between the brackets) replaced by its value; false when it has none.
  bool replace_env(StringView body, String* out, int depth) const {
    const uint64_t comma = body.find(',');
    const StringView name = (comma == StringView::npos ? body : body.first(comma)).trimmed();
    const bool known = name.starts_with("safe-area-inset-") || name.starts_with("titlebar-area-") || name.starts_with("keyboard-inset-") || name.starts_with("viewport-segment-");
    if (known) return out->append("0px");
    if (comma == StringView::npos) return false;
    return substitute(body.substr(comma + 1).trimmed(), out, depth + 1, true);
  }

  bool replace_attr(StringView body, String* out, int depth) const {
    uint64_t comma = StringView::npos;
    int nesting = 0;
    for (uint64_t j = 0; j < body.size(); ++j) {
      if (body[j] == '(') ++nesting;
      else if (body[j] == ')') --nesting;
      else if (body[j] == ',' && nesting == 0) { comma = j; break; }
    }
    const StringView head = (comma == StringView::npos ? body : body.first(comma)).trimmed();
    uint64_t at = 0;
    StringView name, type;
    if (!next_part(head, &at, &name)) return false;
    next_part(head, &at, &type);
    StringView value;
    bool ok = attribute_text(name, &value);
    String typed(sheet.allocator_);
    if (ok) {
      value = value.trimmed();
      if (type.empty() || eq(type, "string") || eq(type, "raw-string")) {
        typed.push_back('"');
        for (uint64_t j = 0; j < value.size(); ++j) {
          if (value[j] == '"' || value[j] == '\\') typed.push_back('\\');
          if (value[j] != '\n') typed.push_back(value[j]);
        }
        typed.push_back('"');
      } else if (eq(type, "color")) {
        uint32_t c;
        ok = parse_css_color_alpha(value, &c);
        if (ok) typed.append(value);
      } else if (eq(type, "number") || eq(type, "integer")) {
        uint64_t k = 0;
        double d;
        ok = read_double(value, &k, &d) && k == value.size();
        if (ok) typed.append(value);
      } else if (eq(type, "length") || eq(type, "angle") || eq(type, "time") || eq(type, "percentage")) {
        double d;
        U unit;
        ok = parse_dimension(value, &d, &unit) && unit != U::None;
        if (ok) typed.append(value);
      } else {   // a unit: attr(data-w px)
        uint64_t k = 0;
        double d;
        ok = read_double(value, &k, &d) && k == value.size() && unit_of(type) != U::Other && unit_of(type) != U::None;
        if (ok) {
          typed.append(value);
          typed.append(type);
        }
      }
    }
    if (ok) return out->append(typed.view());
    if (comma == StringView::npos) return false;
    return substitute(body.substr(comma + 1).trimmed(), out, depth + 1, true);
  }

  // `text` with every var(--name, fallback) (and env(), and attr() when `functions`) replaced; false if one has no value and no fallback.
  bool substitute(StringView text, String* out, int depth, bool functions = false) const {
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
      int call = 0;   // 1 var, 2 env, 3 attr
      if (i == 0 || !is_ident_char(text[i - 1])) {
        const uint64_t left = text.size() - i;
        if (left >= 4 && text.substr(i, 4).equals_ignore_case("var(")) call = 1;
        else if (functions && left >= 4 && text.substr(i, 4).equals_ignore_case("env(")) call = 2;
        else if (functions && left >= 5 && text.substr(i, 5).equals_ignore_case("attr(")) call = 3;
      }
      if (call != 0) {
        const uint64_t open_len = call == 3 ? 5 : 4;
        // find the matching ")"
        uint64_t k = i + open_len;
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
        const StringView body = text.substr(i + open_len, k - 1 - (i + open_len));
        if (call == 2) {
          if (!replace_env(body, out, depth)) return false;
          i = k;
          continue;
        }
        if (call == 3) {
          if (!replace_attr(body, out, depth)) return false;
          i = k;
          continue;
        }
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
          if (!substitute(value, &inner, depth + 1, functions)) return false;
          out->append(inner.view());
        } else if (comma != StringView::npos) {
          if (!substitute(body.substr(comma + 1).trimmed(), out, depth + 1, functions)) return false;
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
  // Is the calculation only sums, products and plain brackets (so that it is linear in the percentage)?
  static bool linear_calculation(StringView v) {
    for (uint64_t i = 0; i < v.size(); ++i) {
      if (v[i] != '(') continue;
      uint64_t k = i;
      while (k > 0 && is_ident_char(v[k - 1])) --k;
      const StringView name = v.substr(k, i - k);
      if (!name.empty() && !name.equals_ignore_case("calc") && !name.equals_ignore_case("-webkit-calc") && !name.equals_ignore_case("-moz-calc")) return false;
    }
    return true;
  }

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
    const bool percent = v.contains('%');
    if (!evaluate_length(v, c, lenient, &px)) {
      // a percentage of something that is not known (a height) is automatic for the old renderer
      if (vertical) {
        double dummy;
        U unit;
        if ((parse_dimension(v, &dummy, &unit) && unit == U::Percent) || percent) { *out = Length(); keep_percent_form(v, lenient, out); return true; }
      }
      return false;
    }
    *out = Length::px(to_px100(px));
    if (percent) keep_percent_form(v, lenient, out);
    return true;
  }

  static Length none_like(const Length& l) {   // (none for the old renderer; the percentage stays)
    Length n = Length::of(Length::None);
    n.percent100 = l.percent100;
    n.abs100 = l.abs100;
    n.expr = l.expr;
    return n;
  }

  // Adds the percentage part (linear form, or the expression) of a length written with a % to `out` (whose kind and px100 are the old renderer's).
  void keep_percent_form(StringView v, bool lenient, Length* out) const {
    EvalContext c = eval;
    c.has_percent = true;
    constexpr double kProbe = 100000;
    double at_zero, at_probe;
    c.percent_px = 0;
    if (!evaluate_length(v, c, lenient, &at_zero)) return;
    c.percent_px = kProbe;
    if (!evaluate_length(v, c, lenient, &at_probe)) return;
    out->abs100 = to_px100(at_zero);
    if (linear_calculation(v)) {
      const double percent100 = (at_probe - at_zero) / kProbe * 10000;
      out->percent100 = static_cast<int32_t>(percent100 < 0 ? percent100 - 0.5 : percent100 + 0.5);
      if (out->percent100 == 0) out->abs100 = 0;   // (the percentage cancelled out: it is just px100)
    } else {
      const TextRef ref = sheet.keep_text(v);
      if (!ref.empty()) out->expr = (ref.offset & 0x7FFFFFFFu) + 1;
    }
  }

  // A colour for a border: `transparent` is kept apart from the text colour (0).
  bool parse_border_color(StringView v, uint32_t* out) const {
    if (!parse_color(v, out)) return false;
    if (*out == 0 && !eq(v.trimmed(), "currentcolor")) *out = kTransparentBorder;
    return true;
  }

  ColorContext color_context() const {
    ColorContext ctx;
    ctx.dark = style->color_scheme == ColorScheme::Dark ? true : style->color_scheme == ColorScheme::Light ? false : sheet.dark_;
    ctx.current = style->color ? style->color : 0xFF000000u;
    return ctx;
  }

  // The colour with its alpha kept (for the pixel engine's backgrounds).
  bool parse_color_alpha(StringView v, uint32_t* out) const {
    if (eq(v.trimmed(), "currentcolor")) { *out = style->color; return true; }
    if (is_modern_color_function(v.trimmed())) return color_value(v, color_context(), 0, out);
    return parse_css_color_alpha(v, out);
  }

  bool parse_color(StringView v, uint32_t* out) const {
    if (eq(v.trimmed(), "currentcolor")) { *out = style->color; return true; }
    if (is_modern_color_function(v.trimmed())) {
      uint32_t argb;
      if (!color_value(v, color_context(), 0, &argb)) return false;
      *out = (argb >> 24) < 128 ? 0 : (0xFF000000u | argb);   // (as for the other colours: mostly transparent is not shown)
      return true;
    }
    return parse_css_color(v, out);
  }

  // A colour with its alpha (for shadows and outlines); false if it is not a colour. currentcolor gives *current = true.
  bool parse_color_alpha(StringView v, uint32_t* out, bool* current) const {
    v = v.trimmed();
    *current = eq(v, "currentcolor");
    if (*current) { *out = style->color; return true; }
    return color_value(v, color_context(), 0, out);
  }

  // Does the value call var() (or env() / attr() when `functions`)?
  static bool has_function(StringView value, bool functions) {
    for (uint64_t i = 0; i + 4 <= value.size(); ++i) {
      if (i > 0 && is_ident_char(value[i - 1])) continue;
      if (value.substr(i, 4).equals_ignore_case("var(")) return true;
      if (functions && value.substr(i, 4).equals_ignore_case("env(")) return true;
      if (functions && i + 5 <= value.size() && value.substr(i, 5).equals_ignore_case("attr(")) return true;
    }
    return false;
  }

  // --- applying
  void declare(Prop p, StringView value) {
    value = value.trimmed();
    if (value.empty() || p == pNone) return;
    String substituted(sheet.allocator_);
    if (has_function(value, p != pContent)) {
      if (!substitute(value, &substituted, 0, p != pContent)) {   // a variable without a value: the property is unset
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
      else if (parse_border_color(part, &c)) color = c;
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
    if (!type_only) style->list_style_image = TextRef();
    while (next_part(v, &at, &part)) {
      int k;
      BackgroundLayer image;
      if (part.contains('(') && !part.equals_ignore_case("none") && parse_image_item(part, eval, color_context(), &image) && image.image != BackgroundLayer::None) {
        keep_text(part, &style->list_style_image);
      } else if (KEYWORD(part, kTypes, &k)) {
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

  // --- the pixel engine's properties
  bool font_weight_number(StringView v, uint16_t* out) const;
  bool font_stretch_value(StringView v, uint16_t* out) const;
  bool parse_edge_color(StringView v, uint32_t* out) const;
  void set_outline_like(StringView value, int32_t* width, BorderStyle* border, bool* auto_style, uint32_t* color);
  void set_font_variant(StringView value, bool caps, bool numeric, bool ligatures);
  void set_background_shorthand(StringView value);
  void set_transition_shorthand(StringView value);
  void set_animation_shorthand(StringView value);
  void set_transform_origin(StringView value);
  void set_cursor(StringView value);
  void set_aspect_ratio(StringView value);
  void set_overscroll(StringView value, bool x, bool y);
  void keep_or_clear(StringView value, TextRef* ref) const {
    if (eq(value, "none")) *ref = TextRef();
    else keep_text(value, ref);
  }
  // Is every item of the comma separated list accepted by `ok`? Then it is kept.
  template <class F>
  bool keep_list(StringView value, TextRef* ref, F ok) const {
    uint64_t at = 0;
    StringView item;
    uint32_t n = 0;
    while (next_comma_part(value, &at, &item)) {
      if (item.empty() || ++n > 64 || !ok(item)) return false;
    }
    if (n == 0) return false;
    keep_text(value, ref);
    return true;
  }
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
        style->background_argb = parse_color_alpha(value, &c) ? c : style->own_background_color;
      }
      break;
    case pBackground: {
      set_background_shorthand(value);
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
          uint32_t argb;
          style->background_argb = parse_color_alpha(part, &argb) ? argb : c;
          found = true;
        }
      }
      if (!found && (eq(last, "none") || eq(last, "transparent"))) {
        // (no colour given: the background colour goes back to transparent)
        style->own_background = true;
        style->own_background_color = 0;
        style->background_argb = 0;
      } else if (!found) {
        style->own_background = true;
        style->own_background_color = 0;
        style->background_argb = 0;
      }
      break;
    }
    case pFontSize: set_font_size(value); break;
    case pFontWeight: {
      uint16_t weight;
      if (font_weight_number(value, &weight)) {
        style->font_weight = weight;
        style->bold = weight >= 600;
      }
      break;
    }
    case pFontStyle: style->italic = eq(value, "italic") || eq(value, "oblique") || value.starts_with("oblique "); break;
    case pFont: {
      // [style] [variant] [weight] [stretch] size[/line-height] family
      uint64_t at = 0;
      StringView part;
      bool italic = false, weight_set = false, small_caps = false;
      uint16_t weight = 400, stretch = 1000;
      StringView size_part, lh_part;
      while (next_part(value, &at, &part)) {
        if (eq(part, "italic")) { italic = true; continue; }
        if (eq(part, "oblique")) {
          italic = true;
          const uint64_t before = at;
          StringView angle;
          double deg;
          if (!next_part(value, &at, &angle) || !angle_degrees(angle, eval, &deg)) at = before;   // (an angle may follow)
          continue;
        }
        if (eq(part, "normal")) continue;
        if (eq(part, "small-caps")) { small_caps = true; continue; }
        uint16_t st;
        if (font_stretch_value(part, &st) && !part.contains('%')) { stretch = st; continue; }
        uint16_t w;
        if (!part.contains('/') && !part.contains('%') && font_weight_number(part, &w)) { weight = w; weight_set = true; continue; }
        // the size (possibly with /line-height)
        const uint64_t slash = part.find('/');
        size_part = slash == StringView::npos ? part : part.first(slash);
        if (slash != StringView::npos) lh_part = part.substr(slash + 1);
        break;
      }
      if (size_part.empty()) break;   // (not a font shorthand: a system font name)
      style->italic = italic;
      style->font_weight = weight_set ? weight : 400;
      style->bold = style->font_weight >= 600;
      style->font_stretch_x10 = stretch;
      style->font_variant = small_caps ? kVariantSmallCaps : 0;
      set_font_size(size_part);
      if (!lh_part.empty()) set_line_height(lh_part);
      else style->line_height_px100 = -1;
      const StringView family = at <= value.size() ? value.substr(at).trimmed() : StringView();
      if (!family.empty() && parse_font_family_list(family, nullptr)) keep_text(family, &style->font_family);
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
      if (parse_length(value, false, &l) && l.is_px()) {
        style->text_indent_px100 = l.px100;
        style->text_indent = l;
      }
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
      if (eq(value, "hidden") || eq(value, "collapse")) { style->hidden = true; style->visibility = eq(value, "hidden") ? Visibility::Hidden : Visibility::Collapse; }
      else if (eq(value, "visible")) { style->hidden = false; style->visibility = Visibility::Visible; }
      break;
    case pOpacity: {
      double n;
      EvalContext ec = eval;
      uint64_t k = 0;
      if (read_double(value, &k, &n) && k < value.size() && value[k] == '%') n /= 100;
      else if (!evaluate_number(value, ec, &n)) break;
      style->opaque_zero = n <= 0.0;
      style->opacity_x1000 = static_cast<uint16_t>(n <= 0 ? 0 : n >= 1 ? 1000 : static_cast<int32_t>(n * 1000 + 0.5));
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
      if (eq(value, "auto")) { style->z_index = 0; style->z_index_auto = true; }
      else if (evaluate_number(value, eval, &n)) { style->z_index = static_cast<int32_t>(n); style->z_index_auto = false; }
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
        if (p == pMaxWidth && l.kind == Length::Auto) l = none_like(l);
        (p == pWidth ? style->width : p == pMinWidth ? style->min_width : style->max_width) = l;
      }
      break;
    case pHeight: case pMinHeight: case pMaxHeight:
      if (parse_length(value, true, &l)) {
        if (l.kind == Length::Px && l.px100 < 0) break;
        if (p == pHeight && l.kind == Length::None) break;
        if (p == pMaxHeight && l.kind == Length::Auto) l = none_like(l);
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
      if (parse_border_color(value, &c)) style->border_color[p - pBorderTopColor] = c;
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
    case pFontFamily:
      if (parse_font_family_list(value, nullptr)) keep_text(value, &style->font_family);
      break;
    case pFontStretch: {
      uint16_t st;
      if (font_stretch_value(value, &st)) style->font_stretch_x10 = st;
      break;
    }
    case pFontVariantCaps: set_font_variant(value, true, false, false); break;
    case pFontVariantNumeric: set_font_variant(value, false, true, false); break;
    case pFontVariantLigatures: set_font_variant(value, false, false, true); break;
    case pFontVariant: set_font_variant(value, true, true, true); break;
    case pLetterSpacing:
      if (eq(value, "normal")) style->letter_spacing_px100 = 0;
      else if (parse_length(value, false, &l, false) && l.is_px()) style->letter_spacing_px100 = l.px100;
      break;
    case pWordSpacing:
      if (eq(value, "normal")) style->word_spacing_px100 = 0;
      else if (parse_length(value, false, &l, false) && l.is_px()) style->word_spacing_px100 = l.px100;
      break;
    case pBorderRadius: {
      CornerRadius r[4];
      if (parse_radius_lists(value, eval, r))
        for (int i = 0; i < 4; ++i) style->radius[i] = r[i];
      break;
    }
    case pBorderTopLeftRadius: case pBorderTopRightRadius: case pBorderBottomRightRadius: case pBorderBottomLeftRadius: {
      uint64_t at = 0;
      StringView a, b, extra;
      CornerRadius r;
      if (!next_part(value, &at, &a) || !dimension_token(a, eval, &r.h, false) || r.h.x100 < 0) break;
      r.v = r.h;
      if (next_part(value, &at, &b) && (!dimension_token(b, eval, &r.v, false) || r.v.x100 < 0 || next_part(value, &at, &extra))) break;
      style->radius[p - pBorderTopLeftRadius] = r;
      break;
    }
    case pBoxShadow: case pTextShadow: {
      if (eq(value, "none")) {
        (p == pBoxShadow ? style->box_shadow : style->text_shadow) = TextRef();
        break;
      }
      List<Shadow> shadows(sheet.allocator_);
      if (parse_shadow_list(value, eval, color_context(), p == pTextShadow, &shadows)) keep_text(value, p == pBoxShadow ? &style->box_shadow : &style->text_shadow);
      break;
    }
    case pBackgroundImage: {
      if (eq(value, "none")) { style->background_image = TextRef(); break; }
      keep_list(value, &style->background_image, [&](StringView item) {
        BackgroundLayer layer;
        return parse_image_item(item, eval, color_context(), &layer);
      });
      break;
    }
    case pBackgroundSize:
      keep_list(value, &style->background_size, [&](StringView item) {
        BackgroundLayer layer;
        return parse_bg_size_item(item, eval, &layer);
      });
      break;
    case pBackgroundPosition:
      keep_list(value, &style->background_position, [&](StringView item) {
        AxisPos x, y;
        return parse_position(item, eval, &x, &y);
      });
      break;
    case pBackgroundRepeat:
      keep_list(value, &style->background_repeat, [&](StringView item) {
        BackgroundLayer layer;
        return parse_bg_repeat_item(item, &layer);
      });
      break;
    case pBackgroundClip: case pBackgroundOrigin:
      keep_list(value, p == pBackgroundClip ? &style->background_clip : &style->background_origin, [&](StringView item) {
        BackgroundBox box;
        return parse_bg_box(item, p == pBackgroundClip, &box);
      });
      break;
    case pBackgroundAttachment:
      keep_list(value, &style->background_attachment, [](StringView item) {
        BackgroundAttachment a;
        return parse_bg_attachment(item, &a);
      });
      break;
    case pTransform: {
      if (eq(value, "none")) { style->transform_value = TextRef(); break; }
      List<TransformOp> ops(sheet.allocator_);
      if (parse_transform_list(value, eval, &ops)) keep_text(value, &style->transform_value);
      break;
    }
    case pTranslate: case pRotate: case pScale: {
      TextRef* ref = p == pTranslate ? &style->translate : p == pRotate ? &style->rotate : &style->scale;
      if (eq(value, "none")) { *ref = TextRef(); break; }
      TransformOp op;
      if (p == pTranslate ? parse_translate_property(value, eval, &op) : p == pRotate ? parse_rotate_property(value, eval, &op) : parse_scale_property(value, eval, &op)) keep_text(value, ref);
      break;
    }
    case pTransformOrigin: set_transform_origin(value); break;
    case pOutline: set_outline_like(value, &style->outline_width_px100, &style->outline_style, &style->outline_auto, &style->outline_color); break;
    case pOutlineWidth: {
      int32_t w;
      if (border_width_of(value, &w)) style->outline_width_px100 = w;
      break;
    }
    case pOutlineStyle: {
      BorderStyle bs;
      if (eq(value, "auto")) { style->outline_auto = true; style->outline_style = BorderStyle::Solid; }
      else if (border_style_of(value, &bs) && bs != BorderStyle::Hidden) { style->outline_auto = false; style->outline_style = bs; }
      break;
    }
    case pOutlineColor: {
      uint32_t color;
      if (parse_edge_color(value, &color)) style->outline_color = color;
      break;
    }
    case pOutlineOffset:
      if (parse_length(value, false, &l) && l.is_px()) style->outline_offset_px100 = l.px100;
      break;
    case pColumnRule: {
      bool unused = false;
      set_outline_like(value, &style->column_rule_width_px100, &style->column_rule_style, &unused, &style->column_rule_color);
      break;
    }
    case pColumnRuleWidth: {
      int32_t w;
      if (border_width_of(value, &w)) style->column_rule_width_px100 = w;
      break;
    }
    case pColumnRuleStyle: {
      BorderStyle bs;
      if (border_style_of(value, &bs)) style->column_rule_style = bs;
      break;
    }
    case pColumnRuleColor: {
      uint32_t color;
      if (parse_edge_color(value, &color)) style->column_rule_color = color;
      break;
    }
    case pCursor: set_cursor(value); break;
    case pPointerEvents:
      if (eq(value, "none")) style->pointer_events = PointerEvents::None;
      else if (eq(value, "auto") || eq(value, "all") || eq(value, "visible") || eq(value, "visiblepainted") || eq(value, "visiblefill") || eq(value, "visiblestroke") ||
               eq(value, "painted") || eq(value, "fill") || eq(value, "stroke"))
        style->pointer_events = PointerEvents::Auto;
      break;
    case pUserSelect: {
      static const KeywordValue kSelect[] = {{"auto", 0}, {"none", 1}, {"text", 2}, {"all", 3}, {"contain", 4}, {"-moz-none", 1}};
      int k;
      if (KEYWORD(value, kSelect, &k)) style->user_select = static_cast<UserSelect>(k);
      break;
    }
    case pAspectRatio: set_aspect_ratio(value); break;
    case pObjectFit: {
      static const KeywordValue kFit[] = {{"fill", 0}, {"contain", 1}, {"cover", 2}, {"none", 3}, {"scale-down", 4}};
      int k;
      if (KEYWORD(value, kFit, &k)) style->object_fit = static_cast<ObjectFit>(k);
      break;
    }
    case pObjectPosition: {
      AxisPos x, y;
      if (parse_position(value, eval, &x, &y)) { style->object_position_x = x; style->object_position_y = y; }
      break;
    }
    case pListStyleImage: {
      BackgroundLayer image;
      if (eq(value, "none")) style->list_style_image = TextRef();
      else if (parse_image_item(value, eval, color_context(), &image)) keep_text(value, &style->list_style_image);
      break;
    }
    case pTransition: set_transition_shorthand(value); break;
    case pTransitionProperty:
      keep_list(value, &style->transition_property, [](StringView item) { return eq(item, "none") || eq(item, "all") || custom_ident(item); });
      break;
    case pTransitionDuration: case pTransitionDelay: {
      const bool delay = p == pTransitionDelay;
      keep_list(value, delay ? &style->transition_delay : &style->transition_duration, [&](StringView item) {
        int32_t ms;
        return time_ms(item, eval, &ms) && (delay || ms >= 0);
      });
      break;
    }
    case pTransitionTimingFunction:
      keep_list(value, &style->transition_timing, [&](StringView item) {
        TimingFunction t;
        return parse_timing_function(item, eval, &t);
      });
      break;
    case pTransitionBehavior:
      keep_list(value, &style->transition_behavior, [](StringView item) { return eq(item, "normal") || eq(item, "allow-discrete"); });
      break;
    case pAnimation: set_animation_shorthand(value); break;
    case pAnimationName:
      keep_list(value, &style->animation_name, [](StringView item) { return eq(item, "none") || custom_ident(item) || (item.size() >= 2 && (item[0] == '"' || item[0] == '\'')); });
      break;
    case pAnimationDuration: case pAnimationDelay: {
      const bool delay = p == pAnimationDelay;
      keep_list(value, delay ? &style->animation_delay : &style->animation_duration, [&](StringView item) {
        int32_t ms;
        return (!delay && eq(item, "auto")) || (time_ms(item, eval, &ms) && (delay || ms >= 0));
      });
      break;
    }
    case pAnimationTimingFunction:
      keep_list(value, &style->animation_timing, [&](StringView item) {
        TimingFunction t;
        return parse_timing_function(item, eval, &t);
      });
      break;
    case pAnimationIterationCount:
      keep_list(value, &style->animation_iteration_count, [&](StringView item) {
        double n;
        return eq(item, "infinite") || (evaluate_number(item, eval, &n) && n >= 0);
      });
      break;
    case pAnimationDirection:
      keep_list(value, &style->animation_direction, [](StringView item) { return eq(item, "normal") || eq(item, "reverse") || eq(item, "alternate") || eq(item, "alternate-reverse"); });
      break;
    case pAnimationFillMode:
      keep_list(value, &style->animation_fill_mode, [](StringView item) { return eq(item, "none") || eq(item, "forwards") || eq(item, "backwards") || eq(item, "both"); });
      break;
    case pAnimationPlayState:
      keep_list(value, &style->animation_play_state, [](StringView item) { return eq(item, "running") || eq(item, "paused"); });
      break;
    case pFilter: case pBackdropFilter: {
      TextRef* ref = p == pFilter ? &style->filter : &style->backdrop_filter;
      if (eq(value, "none")) { *ref = TextRef(); break; }
      List<FilterFunction> filters(sheet.allocator_);
      if (parse_filter_list(value, eval, color_context(), &filters)) keep_text(value, ref);
      break;
    }
    case pClipPath: {
      ClipPath clip;
      if (eq(value, "none")) style->clip_path = TextRef();
      else if (parse_clip_path_text(value, eval, &clip)) keep_text(value, &style->clip_path);
      break;
    }
    case pWritingMode: {
      static const KeywordValue kModes[] = {{"horizontal-tb", 0}, {"vertical-rl", 1}, {"vertical-lr", 2}, {"sideways-rl", 3}, {"sideways-lr", 4}, {"lr", 0}, {"lr-tb", 0},
                                            {"rl", 0}, {"rl-tb", 0}, {"tb", 1}, {"tb-rl", 1}};
      int k;
      if (KEYWORD(value, kModes, &k)) style->writing_mode = static_cast<WritingMode>(k);
      break;
    }
    case pDirection:
      if (eq(value, "ltr")) style->direction = Direction::Ltr;
      else if (eq(value, "rtl")) style->direction = Direction::Rtl;
      break;
    case pColorScheme: {
      uint64_t at = 0;
      StringView part;
      bool light = false, dark = false, any = false;
      while (next_part(value, &at, &part)) {
        if (eq(part, "light")) light = any = true;
        else if (eq(part, "dark")) dark = any = true;
        else if (eq(part, "normal")) any = true;
        else if (eq(part, "only")) continue;
        else if (!custom_ident(part)) return;
      }
      if (any || value.trimmed().size()) style->color_scheme = light && dark ? ColorScheme::LightDark : dark ? ColorScheme::Dark : light ? ColorScheme::Light : ColorScheme::Normal;
      break;
    }
    case pScrollSnapType:
      keep_list(value, &style->scroll_snap_type, [](StringView item) {
        uint64_t at = 0;
        StringView word;
        uint32_t n = 0;
        while (next_part(item, &at, &word)) {
          if (++n > 2 || !(eq(word, "none") || eq(word, "x") || eq(word, "y") || eq(word, "block") || eq(word, "inline") || eq(word, "both") || eq(word, "mandatory") || eq(word, "proximity"))) return false;
        }
        return n > 0;
      });
      if (eq(value, "none")) style->scroll_snap_type = TextRef();
      break;
    case pScrollSnapAlign:
      keep_list(value, &style->scroll_snap_align, [](StringView item) {
        uint64_t at = 0;
        StringView word;
        uint32_t n = 0;
        while (next_part(item, &at, &word)) {
          if (++n > 2 || !(eq(word, "none") || eq(word, "start") || eq(word, "end") || eq(word, "center"))) return false;
        }
        return n > 0;
      });
      if (eq(value, "none")) style->scroll_snap_align = TextRef();
      break;
    case pScrollSnapStop: style->scroll_snap_stop_always = eq(value, "always"); break;
    case pOverscrollBehavior: set_overscroll(value, true, true); break;
    case pOverscrollBehaviorX: set_overscroll(value, true, false); break;
    case pOverscrollBehaviorY: set_overscroll(value, false, true); break;
    case pScrollBehavior: style->scroll_smooth = eq(value, "smooth"); break;
    case pContainerType:
      if (eq(value, "normal")) style->container_type = 0;
      else if (eq(value, "inline-size")) style->container_type = 1;
      else if (eq(value, "size")) style->container_type = 2;
      break;
    case pContainerName: keep_or_clear(value, &style->container_name); break;
    case pContainer: {
      const uint64_t slash = value.find('/');
      apply_value(pContainerName, slash == StringView::npos ? value : value.first(slash).trimmed());
      apply_value(pContainerType, slash == StringView::npos ? StringView("normal") : value.substr(slash + 1).trimmed());
      break;
    }
    default: break;
  }
}

// --- the pixel engine's properties: helpers

bool CssApply::font_weight_number(StringView v, uint16_t* out) const {
  v = v.trimmed();
  if (eq(v, "normal")) { *out = 400; return true; }
  if (eq(v, "bold")) { *out = 700; return true; }
  if (eq(v, "bolder")) {
    const uint16_t w = parent.font_weight;
    *out = w < 350 ? 400 : w < 550 ? 700 : 900;
    return true;
  }
  if (eq(v, "lighter")) {
    const uint16_t w = parent.font_weight;
    *out = w < 550 ? 100 : w < 750 ? 400 : 700;
    return true;
  }
  double n;
  if (evaluate_number(v, eval, &n) && n >= 1 && n <= 1000) {
    *out = static_cast<uint16_t>(n + 0.5);
    return true;
  }
  return false;
}

bool CssApply::font_stretch_value(StringView v, uint16_t* out) const {
  v = v.trimmed();
  static const struct { const char* name; uint16_t x10; } kStretch[] = {{"ultra-condensed", 500}, {"extra-condensed", 625}, {"condensed", 750}, {"semi-condensed", 875},
                                                                         {"normal", 1000}, {"semi-expanded", 1125}, {"expanded", 1250}, {"extra-expanded", 1500},
                                                                         {"ultra-expanded", 2000}};
  for (const auto& k : kStretch)
    if (eq(v, k.name)) { *out = k.x10; return true; }
  double d;
  U unit;
  if (parse_dimension(v, &d, &unit) && unit == U::Percent && d >= 0) {
    const double clamped = d < 50 ? 50 : d > 200 ? 200 : d;
    *out = static_cast<uint16_t>(clamped * 10 + 0.5);
    return true;
  }
  return false;
}

// A colour of an outline or a rule: currentcolor (and invert) follow the text colour (0), transparent is kTransparentBorder.
bool CssApply::parse_edge_color(StringView v, uint32_t* out) const {
  v = v.trimmed();
  if (eq(v, "currentcolor") || eq(v, "invert")) { *out = 0; return true; }
  return parse_border_color(v, out);
}

// outline / column-rule: width, style and colour in any order; what is not given goes back to its initial value.
void CssApply::set_outline_like(StringView value, int32_t* width, BorderStyle* border, bool* auto_style, uint32_t* color) {
  int32_t w = 300;
  BorderStyle bs = BorderStyle::None;
  bool is_auto = false;
  uint32_t c = 0;
  uint64_t at = 0;
  StringView part;
  while (next_part(value, &at, &part)) {
    BorderStyle b;
    int32_t pw;
    uint32_t pc;
    if (eq(part, "auto")) { is_auto = true; bs = BorderStyle::Solid; }
    else if (border_style_of(part, &b)) bs = b;
    else if (border_width_of(part, &pw)) w = pw;
    else if (parse_edge_color(part, &pc)) c = pc;
  }
  *width = w;
  *border = bs;
  *auto_style = is_auto;
  *color = c;
}

void CssApply::set_font_variant(StringView value, bool caps, bool numeric, bool ligatures) {
  uint16_t bits = 0;
  uint64_t at = 0;
  StringView part;
  bool valid = true;
  while (next_part(value, &at, &part)) {
    static const struct { const char* name; uint16_t bit; bool is_caps, is_numeric, is_ligatures; } kTokens[] = {
        {"small-caps", kVariantSmallCaps, true, false, false}, {"all-small-caps", kVariantAllSmallCaps, true, false, false}, {"petite-caps", kVariantPetiteCaps, true, false, false},
        {"all-petite-caps", kVariantAllPetiteCaps, true, false, false}, {"unicase", kVariantUnicase, true, false, false}, {"titling-caps", kVariantTitlingCaps, true, false, false},
        {"tabular-nums", kVariantTabularNums, false, true, false}, {"proportional-nums", kVariantProportionalNums, false, true, false},
        {"lining-nums", kVariantLiningNums, false, true, false}, {"oldstyle-nums", kVariantOldstyleNums, false, true, false}, {"slashed-zero", kVariantSlashedZero, false, true, false},
        {"ordinal", kVariantOrdinal, false, true, false}, {"diagonal-fractions", kVariantDiagonalFractions, false, true, false},
        {"stacked-fractions", kVariantStackedFractions, false, true, false}, {"none", kVariantNoLigatures, false, false, true}};
    bool found = false;
    if (eq(part, "normal")) continue;
    if (eq(part, "common-ligatures") || eq(part, "no-common-ligatures") || eq(part, "discretionary-ligatures") || eq(part, "no-discretionary-ligatures") ||
        eq(part, "historical-ligatures") || eq(part, "no-historical-ligatures") || eq(part, "contextual") || eq(part, "no-contextual")) {
      if (eq(part, "no-common-ligatures") && ligatures) bits |= kVariantNoLigatures;
      continue;
    }
    for (const auto& t : kTokens) {
      if (!eq(part, t.name)) continue;
      found = true;
      if ((t.is_caps && !caps) || (t.is_numeric && !numeric) || (t.is_ligatures && !ligatures)) valid = false;
      else bits = static_cast<uint16_t>(bits | t.bit);
    }
    if (!found && !(eq(part, "sub") || eq(part, "super") || eq(part, "traditional") || eq(part, "ruby") || eq(part, "jis78") || eq(part, "full-width") || eq(part, "proportional-width"))) valid = false;
  }
  if (!valid || value.trimmed().empty()) return;
  uint16_t clear = 0;
  if (caps) clear |= kVariantCapsMask;
  if (numeric) clear |= kVariantNumericMask;
  if (ligatures) clear |= kVariantNoLigatures;
  style->font_variant = static_cast<uint16_t>((style->font_variant & ~clear) | bits);
}

// transform-origin: x [y [z]]
void CssApply::set_transform_origin(StringView value) {
  uint64_t at = 0, end_of_second = 0;
  StringView part, tokens[3];
  uint32_t n = 0;
  while (next_part(value, &at, &part)) {
    if (n == 3) return;
    tokens[n++] = part;
    if (n == 2) end_of_second = at;
  }
  if (n == 0) return;
  AxisPos x, y;
  double z = 0;
  if (n == 3 && !length_px(tokens[2], eval, &z)) return;
  const StringView position = n == 3 ? value.first(end_of_second) : value;
  if (!parse_position(position, eval, &x, &y)) return;
  style->transform_origin_x = x;
  style->transform_origin_y = y;
  style->transform_origin_z_px100 = to_px100(z);
}

void CssApply::set_cursor(StringView value) {
  static const KeywordValue kCursors[] = {
      {"auto", 0}, {"default", 1}, {"none", 2}, {"context-menu", 3}, {"help", 4}, {"pointer", 5}, {"progress", 6}, {"wait", 7}, {"cell", 8}, {"crosshair", 9}, {"text", 10},
      {"vertical-text", 11}, {"alias", 12}, {"copy", 13}, {"move", 14}, {"no-drop", 15}, {"not-allowed", 16}, {"grab", 17}, {"grabbing", 18}, {"all-scroll", 19},
      {"col-resize", 20}, {"row-resize", 21}, {"n-resize", 22}, {"e-resize", 23}, {"s-resize", 24}, {"w-resize", 25}, {"ne-resize", 26}, {"nw-resize", 27}, {"se-resize", 28},
      {"sw-resize", 29}, {"ew-resize", 30}, {"ns-resize", 31}, {"nesw-resize", 32}, {"nwse-resize", 33}, {"zoom-in", 34}, {"zoom-out", 35}, {"-webkit-grab", 17},
      {"-webkit-grabbing", 18}, {"-webkit-zoom-in", 34}, {"-webkit-zoom-out", 35}};
  uint64_t at = 0;
  StringView item;
  int found = -1;
  while (next_comma_part(value, &at, &item)) {   // url(...) [x y], url(...), keyword: the keyword is the fallback that counts
    int k;
    if (KEYWORD(item, kCursors, &k)) found = k;
  }
  if (found >= 0) style->cursor = static_cast<Cursor>(found);
}

// aspect-ratio: auto | <ratio> | auto <ratio> | <ratio> auto
void CssApply::set_aspect_ratio(StringView value) {
  bool is_auto = false;
  double ratio = 0;
  bool have_ratio = false;
  uint64_t at = 0;
  StringView part;
  String ratio_text(sheet.allocator_);
  while (next_part(value, &at, &part)) {
    if (eq(part, "auto")) { if (is_auto) return; is_auto = true; continue; }
    if (!ratio_text.empty()) ratio_text.push_back(' ');
    ratio_text.append(part);
  }
  if (!ratio_text.empty()) {
    uint64_t k = 0;
    StringView w, h;
    next_slash_part(ratio_text.view(), &k, &w);
    double wn, hn = 1;
    if (!evaluate_number(w, eval, &wn) || wn < 0) return;
    if (k <= ratio_text.size() && next_slash_part(ratio_text.view(), &k, &h) && !h.empty()) {
      if (!evaluate_number(h, eval, &hn) || hn < 0) return;
      StringView extra;
      if (k <= ratio_text.size() && next_slash_part(ratio_text.view(), &k, &extra)) return;
    }
    if (wn == 0 || hn == 0) { ratio = 0; have_ratio = true; }   // (a degenerate ratio is no ratio)
    else { ratio = wn / hn; have_ratio = true; }
  } else if (!is_auto) {
    return;
  }
  style->aspect_auto = is_auto;
  style->aspect_ratio_x1000 = have_ratio ? static_cast<uint32_t>(ratio > 1e6 ? 1e9 : ratio * 1000 + 0.5) : 0;
}

void CssApply::set_overscroll(StringView value, bool x, bool y) {
  uint64_t at = 0;
  StringView a, b;
  if (!next_part(value, &at, &a)) return;
  if (!next_part(value, &at, &b)) b = a;
  auto parse = [](StringView v, OverscrollBehavior* out) {
    if (eq(v, "auto")) *out = OverscrollBehavior::Auto;
    else if (eq(v, "contain")) *out = OverscrollBehavior::Contain;
    else if (eq(v, "none")) *out = OverscrollBehavior::None;
    else return false;
    return true;
  };
  OverscrollBehavior ox, oy;
  if (!parse(a, &ox) || !parse(b, &oy)) return;
  if (x && y) { style->overscroll_x = ox; style->overscroll_y = oy; }
  else if (x) style->overscroll_x = ox;
  else style->overscroll_y = ox;
}

// background: the layers into the longhand lists (the colour is read separately).
void CssApply::set_background_shorthand(StringView value) {
  String images(sheet.allocator_), positions(sheet.allocator_), sizes(sheet.allocator_), repeats(sheet.allocator_), attachments(sheet.allocator_), origins(sheet.allocator_),
      clips(sheet.allocator_);
  uint64_t at = 0;
  StringView layer;
  uint32_t layers = 0;
  while (next_comma_part(value, &at, &layer)) {
    if (layers++ >= 16) break;
    String image(sheet.allocator_), position(sheet.allocator_), size(sheet.allocator_), repeat(sheet.allocator_), attachment(sheet.allocator_), origin(sheet.allocator_),
        clip(sheet.allocator_);
    bool after_slash = false;
    uint64_t k = 0;
    StringView part;
    while (next_part(layer, &k, &part)) {
      // "50%/cover" is three tokens
      StringView pieces[3];
      uint32_t pn = 0;
      const uint64_t slash = looks_like_image(part) ? StringView::npos : [&] {
        int depth = 0;
        for (uint64_t i = 0; i < part.size(); ++i) {
          if (part[i] == '(') ++depth;
          else if (part[i] == ')') --depth;
          else if (part[i] == '/' && depth == 0) return i;
        }
        return StringView::npos;
      }();
      if (slash == StringView::npos) pieces[pn++] = part;
      else {
        if (slash > 0) pieces[pn++] = part.first(slash);
        pieces[pn++] = StringView("/");
        if (slash + 1 < part.size()) pieces[pn++] = part.substr(slash + 1);
      }
      for (uint32_t pi = 0; pi < pn; ++pi) {
        const StringView t = pieces[pi];
        RepeatMode rm;
        BackgroundBox box;
        BackgroundAttachment att;
        uint32_t color;
        if (t.equals("/")) { after_slash = true; continue; }
        if (looks_like_image(t)) { image.assign(t); continue; }
        if (eq(t, "repeat-x") || eq(t, "repeat-y") || repeat_keyword(t, &rm)) {
          if (!repeat.empty()) repeat.push_back(' ');
          repeat.append(t);
        } else if (parse_bg_attachment(t, &att)) {
          attachment.assign(t);
        } else if (parse_bg_box(t, true, &box)) {
          if (origin.empty()) { origin.assign(t); if (box != BackgroundBox::Text) clip.assign(t); }
          else clip.assign(t);
        } else if (parse_color(t, &color) || eq(t, "currentcolor")) {
          continue;
        } else if (after_slash) {
          if (!size.empty()) size.push_back(' ');
          size.append(t);
        } else {
          if (!position.empty()) position.push_back(' ');
          position.append(t);
        }
      }
    }
    BackgroundLayer probe;
    AxisPos px, py;
    if (image.empty()) image.assign("none");
    if (position.empty() || !parse_position(position.view(), eval, &px, &py)) position.assign("0% 0%");
    if (size.empty() || !parse_bg_size_item(size.view(), eval, &probe)) size.assign("auto");
    if (repeat.empty() || !parse_bg_repeat_item(repeat.view(), &probe)) repeat.assign("repeat");
    if (attachment.empty()) attachment.assign("scroll");
    if (origin.empty()) origin.assign("padding-box");
    if (clip.empty()) clip.assign("border-box");
    auto add = [](String* list, const String& item) {
      if (!list->empty()) list->append(", ");
      list->append(item.view());
    };
    add(&images, image);
    add(&positions, position);
    add(&sizes, size);
    add(&repeats, repeat);
    add(&attachments, attachment);
    add(&origins, origin);
    add(&clips, clip);
  }
  if (layers == 0) return;
  if (eq(images.view(), "none")) style->background_image = TextRef();
  else keep_text(images.view(), &style->background_image);
  keep_text(positions.view(), &style->background_position);
  keep_text(sizes.view(), &style->background_size);
  keep_text(repeats.view(), &style->background_repeat);
  keep_text(attachments.view(), &style->background_attachment);
  keep_text(origins.view(), &style->background_origin);
  keep_text(clips.view(), &style->background_clip);
}

void CssApply::set_transition_shorthand(StringView value) {
  String props(sheet.allocator_), durations(sheet.allocator_), timings(sheet.allocator_), delays(sheet.allocator_), behaviors(sheet.allocator_);
  uint64_t at = 0;
  StringView item;
  uint32_t n = 0;
  while (next_comma_part(value, &at, &item)) {
    if (n++ >= 32) break;
    StringView property, timing, duration, delay, behavior;
    uint64_t k = 0;
    StringView part;
    while (next_part(item, &k, &part)) {
      TimingFunction tf;
      if (is_time_token(part, eval)) {
        if (duration.empty()) duration = part;
        else delay = part;
      } else if (parse_timing_function(part, eval, &tf)) {
        timing = part;
      } else if (eq(part, "allow-discrete") || eq(part, "normal")) {
        behavior = part;
      } else if (property.empty() && (eq(part, "none") || eq(part, "all") || custom_ident(part))) {
        property = part;
      } else {
        return;   // (not a transition)
      }
    }
    auto add = [](String* list, StringView v, const char* fallback) {
      if (!list->empty()) list->append(", ");
      list->append(v.empty() ? StringView(fallback) : v);
    };
    add(&props, property, "all");
    add(&durations, duration, "0s");
    add(&timings, timing, "ease");
    add(&delays, delay, "0s");
    add(&behaviors, behavior, "normal");
  }
  if (n == 0) return;
  keep_text(props.view(), &style->transition_property);
  keep_text(durations.view(), &style->transition_duration);
  keep_text(timings.view(), &style->transition_timing);
  keep_text(delays.view(), &style->transition_delay);
  keep_text(behaviors.view(), &style->transition_behavior);
}

void CssApply::set_animation_shorthand(StringView value) {
  String names(sheet.allocator_), durations(sheet.allocator_), timings(sheet.allocator_), delays(sheet.allocator_), counts(sheet.allocator_), directions(sheet.allocator_),
      fills(sheet.allocator_), states(sheet.allocator_);
  uint64_t at = 0;
  StringView item;
  uint32_t n = 0;
  while (next_comma_part(value, &at, &item)) {
    if (n++ >= 32) break;
    StringView name, timing, duration, delay, count, direction, fill, state;
    uint64_t k = 0;
    StringView part;
    while (next_part(item, &k, &part)) {
      TimingFunction tf;
      double number;
      if (is_time_token(part, eval)) {
        if (duration.empty()) duration = part;
        else delay = part;
      } else if (parse_timing_function(part, eval, &tf)) {
        timing = part;
      } else if (eq(part, "infinite") || (evaluate_number(part, eval, &number) && number >= 0)) {
        count = part;
      } else if (eq(part, "normal") || eq(part, "reverse") || eq(part, "alternate") || eq(part, "alternate-reverse")) {
        direction = part;
      } else if (eq(part, "forwards") || eq(part, "backwards") || eq(part, "both")) {
        fill = part;
      } else if (eq(part, "running") || eq(part, "paused")) {
        state = part;
      } else if (name.empty() && (eq(part, "none") || custom_ident(part) || (part.size() >= 2 && (part[0] == '"' || part[0] == '\'')))) {
        name = part;
      } else {
        return;
      }
    }
    auto add = [](String* list, StringView v, const char* fallback) {
      if (!list->empty()) list->append(", ");
      list->append(v.empty() ? StringView(fallback) : v);
    };
    add(&names, name, "none");
    add(&durations, duration, "0s");
    add(&timings, timing, "ease");
    add(&delays, delay, "0s");
    add(&counts, count, "1");
    add(&directions, direction, "normal");
    add(&fills, fill, "none");
    add(&states, state, "running");
  }
  if (n == 0) return;
  keep_text(names.view(), &style->animation_name);
  keep_text(durations.view(), &style->animation_duration);
  keep_text(timings.view(), &style->animation_timing);
  keep_text(delays.view(), &style->animation_delay);
  keep_text(counts.view(), &style->animation_iteration_count);
  keep_text(directions.view(), &style->animation_direction);
  keep_text(fills.view(), &style->animation_fill_mode);
  keep_text(states.view(), &style->animation_play_state);
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
    if (!rgb && !hsl && !hwb) return is_modern_color_function(text) && modern_color(text, ColorContext(), 0, out);
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
    if (!rgb && !hsl && !hwb) {
      uint32_t argb;
      if (!is_modern_color_function(text) || !modern_color(text, ColorContext(), 0, &argb)) return false;
      *out = (argb >> 24) < 128 ? 0 : (0xFF000000u | argb);
      return true;
    }
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
  imports_.clear();
  font_faces_.clear();
  unicode_ranges_.clear();
  keyframes_.clear();
  stops_.clear();
  registrations_.clear();
  layers_.clear();
  layer_rank_.clear();
  containers_.clear();
  for (uint32_t& slot : text_index_) slot = 0;
  seen_rule_ = false;
  current_layer_ = import_layer_ = current_container_ = 0;
  scope_depth_ = 0;
  anonymous_layers_ = 0;
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
  // the same text as before is shared (found by its hash among the texts kept so far)
  uint32_t hash = 2166136261u;
  for (uint64_t i = 0; i < s.size(); ++i) hash = (hash ^ static_cast<uint8_t>(s[i])) * 16777619u;
  uint32_t& slot = text_index_[hash & 1023];
  if (slot != 0 && slot <= kept_.count() && kept_[slot - 1].view().equals(s)) {
    ref.offset = 0x80000000u | (slot - 1);
    ref.length = static_cast<uint32_t>(s.size());
    return ref;
  }
  String copy(allocator_);
  if (!copy.assign(s) || !kept_.push_back(util::move(copy))) return ref;
  slot = static_cast<uint32_t>(kept_.count());
  ref.offset = 0x80000000u | static_cast<uint32_t>(kept_.count() - 1);
  ref.length = static_cast<uint32_t>(s.size());
  return ref;
}

StringView StyleSheet::length_expression(const Length& length) const {
  if (length.expr == 0 || length.expr > kept_.count()) return StringView();
  return kept_[length.expr - 1].view();
}

bool StyleSheet::resolve_length_in(const Length& length, const Style& style, int32_t percent_base_px100, int32_t* out_px100) const {
  if (length.expr == 0) {
    *out_px100 = length.resolve(percent_base_px100);
    return true;
  }
  const StringView text = length_expression(length);
  if (text.empty()) return false;
  EvalContext c;
  c.font_px = style.font_px;
  c.root_px = style.root_font_px;
  c.viewport_w = viewport_w_;
  c.viewport_h = viewport_h_;
  c.percent_px = percent_base_px100 / 100.0;
  c.has_percent = true;
  double px;
  if (!evaluate_length(text, c, true, &px)) return false;
  *out_px100 = to_px100(px);
  return true;
}

bool StyleSheet::resolve_length(StringView expression, const Style& style, int32_t percent_px, Length* out) const {
  EvalContext c;
  c.font_px = style.font_px;
  c.root_px = style.root_font_px;
  c.viewport_w = viewport_w_;
  c.viewport_h = viewport_h_;
  c.percent_px = percent_px;
  c.has_percent = percent_px >= 0;
  double px;
  if (!evaluate_length(expression, c, true, &px)) return false;
  *out = Length::px(to_px100(px));
  return true;
}

// --- Readers: the stored text of list valued properties as structures -------------------------------------------

namespace {

EvalContext eval_for(const Style& style, int32_t viewport_w, int32_t viewport_h) {
  EvalContext c;
  c.font_px = style.font_px;
  c.root_px = style.root_font_px;
  c.viewport_w = viewport_w;
  c.viewport_h = viewport_h;
  c.percent_px = 0;
  c.has_percent = false;
  return c;
}

// The item of a comma list at `index` (the list repeats when it is shorter); false for an empty list.
bool list_item(StringView list, uint64_t index, StringView* out) {
  uint64_t count = 0, at = 0;
  StringView item;
  while (next_comma_part(list, &at, &item)) ++count;
  if (count == 0) return false;
  index %= count;
  at = 0;
  for (uint64_t i = 0; next_comma_part(list, &at, &item); ++i) {
    if (i == index) { *out = item; return true; }
  }
  return false;
}

uint64_t list_count(StringView list) {
  uint64_t count = 0, at = 0;
  StringView item;
  while (next_comma_part(list, &at, &item)) ++count;
  return list.trimmed().empty() ? 0 : count;
}

// 4x4 matrices, column major (the layout of matrix3d()).
struct Mat4 {
  double m[16];
  static Mat4 identity() {
    Mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = (i % 5 == 0) ? 1 : 0;
    return r;
  }
};
Mat4 multiply(const Mat4& a, const Mat4& b) {   // a * b
  Mat4 r;
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row) {
      double sum = 0;
      for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
      r.m[col * 4 + row] = sum;
    }
  return r;
}
Mat4 translation(double x, double y, double z) {
  Mat4 r = Mat4::identity();
  r.m[12] = x; r.m[13] = y; r.m[14] = z;
  return r;
}

Mat4 op_matrix(const TransformOp& op, double w, double h) {
  Mat4 r = Mat4::identity();
  switch (op.kind) {
    case TransformOp::Matrix:
      for (int i = 0; i < 16; ++i) r.m[i] = op.m[i];
      break;
    case TransformOp::Translate:
      r = translation(op.x.resolve(to_px100(w)) / 100.0, op.y.resolve(to_px100(h)) / 100.0, op.z_px);
      break;
    case TransformOp::Scale:
      r.m[0] = op.sx; r.m[5] = op.sy; r.m[10] = op.sz;
      break;
    case TransformOp::Rotate: {
      double x = op.ax, y = op.ay, z = op.az;
      const double len = math_sqrt(x * x + y * y + z * z);
      if (len == 0) break;   // (no axis: no rotation)
      x /= len; y /= len; z /= len;
      const double a = op.angle_deg * kPi / 180, c = math_cos(a), s = math_sin(a), t = 1 - c;
      r.m[0] = t * x * x + c;      r.m[1] = t * x * y + s * z;  r.m[2] = t * x * z - s * y;
      r.m[4] = t * x * y - s * z;  r.m[5] = t * y * y + c;      r.m[6] = t * y * z + s * x;
      r.m[8] = t * x * z + s * y;  r.m[9] = t * y * z - s * x;  r.m[10] = t * z * z + c;
      break;
    }
    case TransformOp::Skew: {
      const double ax = op.angle_deg * kPi / 180, ay = op.angle_y_deg * kPi / 180;
      r.m[4] = math_sin(ax) / math_cos(ax);
      r.m[1] = math_sin(ay) / math_cos(ay);
      break;
    }
    case TransformOp::Perspective:
      if (op.z_px > 0) r.m[11] = -1 / op.z_px;
      break;
  }
  return r;
}

}  // namespace

bool StyleSheet::font_families(const Style& style, List<FontFamily>* out) const {
  const StringView t = text(style.font_family);
  if (t.empty()) return true;
  if (parse_font_family_list(t, out)) return true;
  out->clear();
  return false;
}

bool StyleSheet::shadows(TextRef list, const Style& style, List<Shadow>* out) const {
  const StringView t = text(list);
  if (t.empty()) return true;
  ColorContext colors;
  colors.dark = style.color_scheme == ColorScheme::Dark ? true : style.color_scheme == ColorScheme::Light ? false : dark_;
  colors.current = style.color ? style.color : 0xFF000000u;
  if (parse_shadow_list(t, eval_for(style, viewport_w_, viewport_h_), colors, false, out)) return true;
  out->clear();
  return false;
}

bool StyleSheet::background_layers(const Style& style, List<BackgroundLayer>* out) const {
  const EvalContext ctx = eval_for(style, viewport_w_, viewport_h_);
  ColorContext colors;
  colors.dark = style.color_scheme == ColorScheme::Dark ? true : style.color_scheme == ColorScheme::Light ? false : dark_;
  colors.current = style.color ? style.color : 0xFF000000u;
  const StringView images = text(style.background_image);
  const uint64_t n = list_count(images);
  for (uint64_t i = 0; i < n; ++i) {
    StringView item;
    BackgroundLayer layer;
    if (!list_item(images, i, &item) || !parse_image_item(item, ctx, colors, &layer)) {
      out->clear();
      return false;
    }
    if (list_item(text(style.background_size), i, &item)) parse_bg_size_item(item, ctx, &layer);
    if (list_item(text(style.background_position), i, &item)) {
      AxisPos x, y;
      if (parse_position(item, ctx, &x, &y)) { layer.position_x = x; layer.position_y = y; }
    }
    if (list_item(text(style.background_repeat), i, &item)) parse_bg_repeat_item(item, &layer);
    if (list_item(text(style.background_clip), i, &item)) parse_bg_box(item, true, &layer.clip);
    if (list_item(text(style.background_origin), i, &item)) parse_bg_box(item, false, &layer.origin);
    if (list_item(text(style.background_attachment), i, &item)) parse_bg_attachment(item, &layer.attachment);
    if (!out->push_back(layer)) return false;
  }
  return true;
}

bool StyleSheet::transform_ops(const Style& style, List<TransformOp>* out) const {
  const EvalContext ctx = eval_for(style, viewport_w_, viewport_h_);
  TransformOp op;
  if (!style.translate.empty() && parse_translate_property(text(style.translate), ctx, &op) && !out->push_back(op)) return false;
  if (!style.rotate.empty() && parse_rotate_property(text(style.rotate), ctx, &op) && !out->push_back(op)) return false;
  if (!style.scale.empty() && parse_scale_property(text(style.scale), ctx, &op) && !out->push_back(op)) return false;
  if (!style.transform_value.empty() && !parse_transform_list(text(style.transform_value), ctx, out)) return false;
  return true;
}

bool StyleSheet::transform_matrix(const Style& style, double box_width_px, double box_height_px, double out[16], bool around_origin) const {
  List<TransformOp> ops(allocator_);
  if (!transform_ops(style, &ops) || ops.count() == 0) return false;
  const double ox = around_origin ? style.transform_origin_x.resolve(to_px100(box_width_px)) / 100.0 : 0, oy = around_origin ? style.transform_origin_y.resolve(to_px100(box_height_px)) / 100.0 : 0,
               oz = around_origin ? style.transform_origin_z_px100 / 100.0 : 0;
  Mat4 m = translation(ox, oy, oz);
  for (uint64_t i = 0; i < ops.count(); ++i) m = multiply(m, op_matrix(ops[i], box_width_px, box_height_px));
  m = multiply(m, translation(-ox, -oy, -oz));
  for (int i = 0; i < 16; ++i) out[i] = m.m[i];
  return true;
}

bool StyleSheet::transitions(const Style& style, List<Transition>* out) const {
  const EvalContext ctx = eval_for(style, viewport_w_, viewport_h_);
  const StringView properties = text(style.transition_property), durations = text(style.transition_duration);
  uint64_t n = list_count(properties);
  if (n == 0 && list_count(durations) == 0) return true;
  if (n == 0) n = 1;   // (the property defaults to all)
  for (uint64_t i = 0; i < n; ++i) {
    Transition t;
    StringView item;
    t.property = list_item(properties, i, &item) ? item : StringView("all");
    if (list_item(durations, i, &item)) time_ms(item, ctx, &t.duration_ms);
    if (list_item(text(style.transition_delay), i, &item)) time_ms(item, ctx, &t.delay_ms);
    if (list_item(text(style.transition_timing), i, &item)) parse_timing_function(item, ctx, &t.timing);
    if (list_item(text(style.transition_behavior), i, &item)) t.allow_discrete = eq(item, "allow-discrete");
    if (t.duration_ms < 0) t.duration_ms = 0;
    if (!out->push_back(t)) return false;
  }
  return true;
}

bool StyleSheet::animations(const Style& style, List<Animation>* out) const {
  const EvalContext ctx = eval_for(style, viewport_w_, viewport_h_);
  const StringView names = text(style.animation_name);
  const uint64_t n = list_count(names);
  for (uint64_t i = 0; i < n; ++i) {
    Animation a;
    StringView item;
    list_item(names, i, &item);
    a.name = unquote(item);
    if (eq(a.name, "none")) a.name = StringView();
    if (list_item(text(style.animation_duration), i, &item) && !eq(item, "auto")) time_ms(item, ctx, &a.duration_ms);
    if (list_item(text(style.animation_delay), i, &item)) time_ms(item, ctx, &a.delay_ms);
    if (list_item(text(style.animation_timing), i, &item)) parse_timing_function(item, ctx, &a.timing);
    if (list_item(text(style.animation_iteration_count), i, &item)) {
      double count;
      if (eq(item, "infinite")) a.iterations = -1;
      else if (evaluate_number(item, ctx, &count)) a.iterations = count;
    }
    if (list_item(text(style.animation_direction), i, &item))
      a.direction = eq(item, "reverse") ? Animation::Reverse : eq(item, "alternate") ? Animation::Alternate : eq(item, "alternate-reverse") ? Animation::AlternateReverse : Animation::Normal;
    if (list_item(text(style.animation_fill_mode), i, &item))
      a.fill = eq(item, "forwards") ? Animation::Forwards : eq(item, "backwards") ? Animation::Backwards : eq(item, "both") ? Animation::Both : Animation::FillNone;
    if (list_item(text(style.animation_play_state), i, &item)) a.paused = eq(item, "paused");
    if (a.duration_ms < 0) a.duration_ms = 0;
    if (!out->push_back(a)) return false;
  }
  return true;
}

bool StyleSheet::filters(TextRef list, const Style& style, List<FilterFunction>* out) const {
  const StringView t = text(list);
  if (t.empty()) return true;
  ColorContext colors;
  colors.dark = style.color_scheme == ColorScheme::Dark ? true : style.color_scheme == ColorScheme::Light ? false : dark_;
  colors.current = style.color ? style.color : 0xFF000000u;
  if (parse_filter_list(t, eval_for(style, viewport_w_, viewport_h_), colors, out)) return true;
  out->clear();
  return false;
}

bool StyleSheet::clip_path(const Style& style, ClipPath* out) const {
  *out = ClipPath();
  const StringView t = text(style.clip_path);
  if (t.empty()) return true;
  if (parse_clip_path_text(t, eval_for(style, viewport_w_, viewport_h_), out)) return true;
  *out = ClipPath();
  return false;
}

bool StyleSheet::parse(StringView css, const MediaEnvironment& env) {
  env_ = &env;
  viewport_w_ = env.width > 0 ? env.width : 640;
  viewport_h_ = env.height > 0 ? env.height : 480;
  dark_ = env.dark;
  seen_rule_ = false;
  current_layer_ = import_layer_;
  current_container_ = 0;
  scope_depth_ = 0;
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

namespace {

constexpr uint32_t kMaxLayers = 256;
constexpr uint32_t kMaxLayerDepth = 12;
constexpr uint32_t kMaxContainers = 512;
constexpr uint32_t kMaxImports = 64;
constexpr uint32_t kMaxFontFaces = 256;
constexpr uint32_t kMaxKeyframes = 1024;
constexpr uint32_t kMaxKeyframeStops = 64;
constexpr uint32_t kMaxUnicodeRanges = 8192;

// "a.b.c" -> the names, trimmed (empty when a name is empty).
bool next_layer_name(StringView path, uint64_t* at, StringView* name) {
  if (*at > path.size()) return false;
  const uint64_t dot = path.find('.', *at);
  const uint64_t end = dot == StringView::npos ? path.size() : dot;
  *name = path.substr(*at, end - *at).trimmed();
  *at = end + 1;
  return true;
}

// Replaces ":scope" and "&" in a selector by ":where(root)", or puts ":where(root) " in front of it; adds the limit: the element is not inside it.
bool scope_selector(StringView selector, StringView root, StringView limit, String* out) {   // (out's allocator is used for the pieces)
  selector = selector.trimmed();
  if (selector.empty()) return true;
  String body(out->allocator());
  bool replaced = false;
  for (uint64_t i = 0; i < selector.size();) {
    if (selector[i] == '&' || (selector[i] == ':' && selector.substr(i).starts_with(":scope") && (i + 6 >= selector.size() || !is_ident_char(selector[i + 6])))) {
      body.append(":where(");
      body.append(root);
      body.push_back(')');
      i += selector[i] == '&' ? 1 : 6;
      replaced = true;
    } else {
      body.push_back(selector[i++]);
    }
  }
  String text(out->allocator());
  if (!replaced) {
    text.append(":where(");
    text.append(root);
    text.append(") ");
  }
  text.append(body.view());
  if (!limit.empty()) {   // (before a pseudo-element)
    uint64_t pseudo = text.view().find("::");
    String suffix(out->allocator());
    suffix.append(":not(:where(");
    suffix.append(limit);
    suffix.append(") *)");
    if (pseudo == StringView::npos) text.append(suffix.view());
    else text.insert(pseudo, suffix.view());
  }
  return out->append(text.view());
}

}  // namespace

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
      if (i >= css.size() || css[i] == ';') {   // a statement: @import, @layer a, b; @charset, @namespace
        if (i < css.size()) ++i;
        if (name.equals_ignore_case("import")) {
          if (!seen_rule_ && !parse_import(prelude)) return false;
        } else if (name.equals_ignore_case("layer")) {
          if (!parse_layer_statement(prelude)) return false;
        }
        if (i >= css.size()) return true;
        continue;
      }
      auto is = [&](const char* n) { return name.equals_ignore_case(n); };
      const bool conditional = is("media") || is("supports") || is("document") || is("-moz-document");
      if (conditional) {
        seen_rule_ = true;
        bool applies = false;
        if (is("media")) applies = media_matches(prelude, *env_);
        else if (is("supports")) applies = supports_condition(prelude);
        else applies = true;
        if (applies) {
          ++i;   // (the rules inside are read as if they stood here)
          if (!parse_block(css, &i, true)) return false;
        } else {
          take_block(css, &i);
        }
      } else if (is("layer")) {
        seen_rule_ = true;
        const uint32_t saved = current_layer_;
        const uint32_t layer = enter_layer(prelude);
        if (layer != 0) current_layer_ = layer;
        ++i;
        const bool ok = parse_block(css, &i, true);
        current_layer_ = saved;
        if (!ok) return false;
      } else if (is("container")) {
        seen_rule_ = true;
        // [name] (query)
        StringView p = prelude.trimmed();
        StringView container_name;
        if (!p.empty() && p[0] != '(' && !p.starts_with("not") && !p.starts_with("style(") && !p.starts_with("scroll-state(")) {
          const uint64_t sp = p.find(' ');
          container_name = sp == StringView::npos ? p : p.first(sp);
          p = sp == StringView::npos ? StringView() : p.substr(sp).trimmed();
        }
        if (containers_.count() >= kMaxContainers || p.empty()) {
          take_block(css, &i);
          continue;
        }
        Container c;
        c.parent = current_container_;
        if (!keep_span(container_name, &c.name) || !keep_span(p, &c.query) || !containers_.push_back(c)) return false;
        const uint32_t saved = current_container_;
        current_container_ = static_cast<uint32_t>(containers_.count());
        ++i;
        const bool ok = parse_block(css, &i, true);
        current_container_ = saved;
        if (!ok) return false;
      } else if (is("scope")) {
        seen_rule_ = true;
        // [(root)] [to (limit)]
        const StringView p = prelude.trimmed();
        StringView root, limit;
        uint64_t k = 0;
        StringView part;
        bool after_to = false;
        while (next_part(p, &k, &part)) {
          if (part.equals_ignore_case("to")) after_to = true;
          else if (part.size() >= 2 && part[0] == '(' && part[part.size() - 1] == ')') (after_to ? limit : root) = part.substr(1, part.size() - 2).trimmed();
        }
        const bool scoped = scope_depth_ < kMaxScopeDepth && !root.empty();
        if (scoped) {
          if (!keep_span(root, &scope_root_[scope_depth_]) || !keep_span(limit, &scope_limit_[scope_depth_])) return false;
          ++scope_depth_;
        }
        ++i;
        const bool ok = parse_block(css, &i, true);
        if (scoped) --scope_depth_;
        if (!ok) return false;
      } else if (is("font-face")) {
        seen_rule_ = true;
        if (!parse_font_face(take_block(css, &i))) return false;
      } else if (is("keyframes") || is("-webkit-keyframes")) {
        seen_rule_ = true;
        if (!parse_keyframes(prelude, take_block(css, &i))) return false;
      } else if (is("property")) {
        seen_rule_ = true;
        if (!parse_property_rule(prelude, take_block(css, &i))) return false;
      } else {   // @starting-style, @page, @font-feature-values ... are skipped
        take_block(css, &i);
      }
      continue;
    }
    const StringView prelude = scan_until(css, &i, '{', '}');
    if (i >= css.size() || css[i] != '{') return true;
    const StringView body = take_block(css, &i);
    seen_rule_ = true;
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
  if (scope_depth_ == 0) {
    selectors_.parse(prelude, rule, &added);   // (a selector that is wrong or can never match is left out)
    return true;
  }
  // inside @scope: every selector is relative to the scoping root (and stops at the limit)
  String text(allocator_);
  uint64_t at = 0;
  StringView selector;
  while (next_comma_part(prelude, &at, &selector)) {
    String scoped(allocator_);
    if (!scoped.assign(selector)) return false;
    for (uint32_t level = scope_depth_; level > 0; --level) {
      String next(allocator_);
      if (!scope_selector(scoped.view(), span_text(scope_root_[level - 1]), span_text(scope_limit_[level - 1]), &next)) return false;
      scoped = util::move(next);
    }
    if (!text.empty() && !text.push_back(',')) return false;
    if (!text.append(scoped.view())) return false;
  }
  selectors_.parse(text.view(), rule, &added);
  return true;
}

bool StyleSheet::add_declaration_run(StringView body, uint32_t* first, uint32_t* count) {
  *first = static_cast<uint32_t>(declarations_.count());
  *count = 0;
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
    ++*count;
  });
  return ok;
}

bool StyleSheet::add_declarations(StringView body, uint32_t rule) {
  Rule r;
  r.order = rule;
  r.scope = scope_;
  r.layer = current_layer_;
  r.container = current_container_;
  const bool ok = add_declaration_run(body, &r.first_declaration, &r.declaration_count);
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
  if ((s->font_weight >= 600) != s->bold) s->font_weight = s->bold ? 700 : 400;   // (the defaults of the old renderer set only `bold`)
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

StringView StyleSheet::span_text(Span span) const {
  if (span.length == 0 || span.offset > pool_.size() || span.length > pool_.size() - span.offset) return StringView();
  return StringView(pool_.view()).substr(span.offset, span.length);
}


bool StyleSheet::parse_layer_statement(StringView prelude) {
  uint64_t at = 0;
  StringView name;
  while (next_comma_part(prelude, &at, &name)) {
    if (name.empty()) continue;
    const uint32_t saved = current_layer_;
    enter_layer(name);
    current_layer_ = saved;
  }
  return true;
}

// The layer named by a (dotted) path inside the current one, made if it is new. 0 when the table is full or the name is not a name.
uint32_t StyleSheet::enter_layer(StringView path) {
  uint32_t parent = current_layer_;
  uint32_t depth = 0;
  for (uint32_t p = parent; p != 0 && depth < kMaxLayerDepth + 1; p = layers_[p - 1].parent) ++depth;
  if (path.trimmed().empty()) {   // an anonymous layer
    if (layers_.count() >= kMaxLayers || depth >= kMaxLayerDepth) return 0;
    String name(allocator_);
    name.append("\x01 anonymous ");
    name.append_uint(++anonymous_layers_);
    Layer layer;
    layer.parent = parent;
    if (!keep_span(name.view(), &layer.name)) return 0;
    uint32_t siblings = 0;
    for (uint64_t i = 0; i < layers_.count(); ++i)
      if (layers_[i].parent == parent) ++siblings;
    layer.ordinal = siblings;
    if (!layers_.push_back(layer)) return 0;
    rank_layers();
    return static_cast<uint32_t>(layers_.count());
  }
  uint64_t at = 0;
  StringView name;
  uint32_t result = 0;
  while (next_layer_name(path, &at, &name)) {
    if (name.empty() || !is_ident_start(name[0])) return 0;
    uint32_t found = 0, siblings = 0;
    for (uint64_t i = 0; i < layers_.count(); ++i) {
      if (layers_[i].parent != parent) continue;
      ++siblings;
      if (span_text(layers_[i].name).equals(name)) found = static_cast<uint32_t>(i + 1);
    }
    if (!found) {
      if (layers_.count() >= kMaxLayers || depth >= kMaxLayerDepth) return 0;
      Layer layer;
      layer.parent = parent;
      layer.ordinal = siblings;
      if (!keep_span(name, &layer.name) || !layers_.push_back(layer)) return 0;
      found = static_cast<uint32_t>(layers_.count());
      ++depth;
    }
    parent = found;
    result = found;
  }
  rank_layers();
  return result;
}

// Puts the layers in cascade order: by the position of each among its siblings, from the outermost down; a layer's own rules come after its sub-layers.
void StyleSheet::rank_layers() {
  const uint64_t n = layers_.count();
  layer_rank_.clear();
  for (uint64_t i = 0; i < n; ++i) {
    uint32_t path_i[kMaxLayerDepth + 2];
    uint32_t depth_i = 0;
    for (uint32_t p = static_cast<uint32_t>(i + 1); p != 0 && depth_i < kMaxLayerDepth + 2; p = layers_[p - 1].parent) path_i[depth_i++] = layers_[p - 1].ordinal;
    uint32_t rank = 1;
    for (uint64_t j = 0; j < n; ++j) {
      if (j == i) continue;
      uint32_t path_j[kMaxLayerDepth + 2];
      uint32_t depth_j = 0;
      for (uint32_t p = static_cast<uint32_t>(j + 1); p != 0 && depth_j < kMaxLayerDepth + 2; p = layers_[p - 1].parent) path_j[depth_j++] = layers_[p - 1].ordinal;
      // (the paths are stored from the layer up to the top: compare from the top)
      bool j_before = false, decided = false;
      uint32_t a = depth_i, b = depth_j;
      while (a > 0 && b > 0) {
        const uint32_t oi = path_i[a - 1], oj = path_j[b - 1];
        if (oi != oj) { j_before = oj < oi; decided = true; break; }
        --a;
        --b;
      }
      if (!decided) j_before = b > 0 ? true : (a > 0 ? false : j < i);   // (the one that is deeper comes first: its parent's own rules come last)
      if (j_before) ++rank;
    }
    layer_rank_.push_back(rank);
  }
}

void StyleSheet::set_layer(StringView layer_path) {
  const uint32_t saved = current_layer_;
  current_layer_ = 0;
  import_layer_ = layer_path.trimmed().empty() ? 0 : enter_layer(layer_path);
  current_layer_ = saved;
}

bool StyleSheet::container_applies(uint32_t container, const dom::Node* element) const {
  for (uint32_t guard = 0; container != 0 && guard < 16; ++guard) {
    const Container& c = containers_[container - 1];
    int32_t w = viewport_w_, h = viewport_h_;
    if (container_sizes_ && !container_sizes_->container_size(element, span_text(c.name), &w, &h)) return false;
    MediaEnvironment env;
    env.width = w;
    env.height = h;
    env.dark = dark_;
    const StringView query = span_text(c.query);
    if (query.contains("style(") || !media_matches(query, env)) return false;
    container = c.parent;
  }
  return true;
}

bool StyleSheet::parse_import(StringView prelude) {
  if (imports_.count() >= kMaxImports) return true;
  uint64_t at = 0;
  StringView part;
  ImportData data;
  data.has_layer = false;
  data.applies = true;
  if (!next_part(prelude, &at, &part)) return true;
  StringView name, inner;
  StringView url;
  if (function_of(part, &name, &inner) && name.equals_ignore_case("url")) url = unquote(inner);
  else if (part.size() >= 2 && (part[0] == '"' || part[0] == '\'')) url = unquote(part);
  else return true;
  if (url.empty() || !keep_span(url, &data.url)) return true;
  StringView supports, layer, media;
  uint64_t media_start = at;
  while (true) {
    const uint64_t before = at;
    if (!next_part(prelude, &at, &part)) { media_start = prelude.size(); break; }
    if (part.equals_ignore_case("layer")) { data.has_layer = true; layer = StringView(); media_start = at; continue; }
    if (function_of(part, &name, &inner) && name.equals_ignore_case("layer")) { data.has_layer = true; layer = inner.trimmed(); media_start = at; continue; }
    if (function_of(part, &name, &inner) && name.equals_ignore_case("supports")) { supports = inner.trimmed(); media_start = at; continue; }
    media_start = before;
    break;
  }
  media = media_start < prelude.size() ? prelude.substr(media_start).trimmed() : StringView();
  if (!keep_span(media, &data.media) || !keep_span(supports, &data.supports) || !keep_span(layer, &data.layer)) return true;
  if (env_ && !media.empty()) data.applies = media_matches(media, *env_);
  if (!supports.empty() && !supports_condition(supports)) data.applies = false;
  return imports_.push_back(data);
}

bool StyleSheet::import_at(uint32_t index, ImportRule* out) const {
  if (index >= imports_.count()) return false;
  const ImportData& d = imports_[index];
  out->url = span_text(d.url);
  out->media = span_text(d.media);
  out->supports = span_text(d.supports);
  out->layer = span_text(d.layer);
  out->has_layer = d.has_layer;
  out->applies = d.applies;
  return true;
}

namespace {

FontFormat font_format_of(StringView f) {
  f = unquote(f);
  if (f.equals_ignore_case("woff2") || f.equals_ignore_case("woff2-variations")) return FontFormat::Woff2;
  if (f.equals_ignore_case("woff") || f.equals_ignore_case("woff-variations")) return FontFormat::Woff;
  if (f.equals_ignore_case("truetype") || f.equals_ignore_case("truetype-variations")) return FontFormat::TrueType;
  if (f.equals_ignore_case("opentype") || f.equals_ignore_case("opentype-variations")) return FontFormat::OpenType;
  if (f.equals_ignore_case("embedded-opentype")) return FontFormat::EmbeddedOpenType;
  if (f.equals_ignore_case("svg")) return FontFormat::Svg;
  if (f.equals_ignore_case("collection")) return FontFormat::Collection;
  return FontFormat::Unknown;
}

// unicode-range: U+26, U+0025-00FF, U+4??
bool parse_unicode_range(StringView item, UnicodeRange* out) {
  item = item.trimmed();
  if (item.size() < 3 || (item[0] != 'U' && item[0] != 'u') || item[1] != '+') return false;
  item = item.substr(2);
  uint32_t lo = 0, hi = 0;
  uint64_t i = 0;
  uint32_t digits = 0, wildcards = 0;
  while (i < item.size() && (hex_digit(item[i]) >= 0 || item[i] == '?')) {
    if (item[i] == '?') { lo = lo << 4; hi = (hi << 4) | 0xF; ++wildcards; }
    else { if (wildcards) return false; lo = (lo << 4) | static_cast<uint32_t>(hex_digit(item[i])); hi = (hi << 4) | static_cast<uint32_t>(hex_digit(item[i])); }
    if (++digits > 6) return false;
    ++i;
  }
  if (digits == 0) return false;
  if (i < item.size()) {
    if (wildcards || item[i] != '-') return false;
    ++i;
    uint32_t end = 0, end_digits = 0;
    while (i < item.size() && hex_digit(item[i]) >= 0) {
      end = (end << 4) | static_cast<uint32_t>(hex_digit(item[i++]));
      if (++end_digits > 6) return false;
    }
    if (end_digits == 0 || i < item.size()) return false;
    hi = end;
  }
  if (lo > hi || hi > 0x10FFFF) return false;
  out->first = lo;
  out->last = hi;
  return true;
}

}  // namespace

bool StyleSheet::parse_font_face(StringView body) {
  if (font_faces_.count() >= kMaxFontFaces) return true;
  FontFaceData face;
  bool have_family = false;
  bool ok = true;
  List<UnicodeRange> ranges(allocator_);
  EvalContext ctx;
  for_each_declaration(body, [&](StringView name, StringView value, bool) {
    if (!ok) return;
    auto is = [&](const char* n) { return name.equals_ignore_case(n); };
    if (is("font-family")) {
      StringView family = unquote(value);
      if (family.empty() || !keep_span(family, &face.family)) return;
      have_family = true;
    } else if (is("src")) {
      uint64_t at = 0;
      StringView item;
      face.source_count = 0;
      while (next_comma_part(value, &at, &item)) {
        if (face.source_count == kMaxFontFaceSources) break;
        FontFaceData::Source src;
        uint64_t k = 0;
        StringView part;
        bool have_location = false, bad = false;
        while (next_part(item, &k, &part)) {
          StringView fn, inner;
          if (!function_of(part, &fn, &inner)) { bad = true; break; }
          if (fn.equals_ignore_case("url") && !have_location) {
            src.local = false;
            if (!keep_span(unquote(inner), &src.value)) { ok = false; return; }
            have_location = true;
          } else if (fn.equals_ignore_case("local") && !have_location) {
            src.local = true;
            if (!keep_span(unquote(inner), &src.value)) { ok = false; return; }
            have_location = true;
          } else if (fn.equals_ignore_case("format")) {
            StringView first;
            uint64_t fa = 0;
            next_comma_part(inner, &fa, &first);
            src.format = font_format_of(first);
            if (!keep_span(unquote(first), &src.format_text)) { ok = false; return; }
          } else if (fn.equals_ignore_case("tech")) {
            uint64_t ta = 0;
            StringView tech;
            while (next_comma_part(inner, &ta, &tech)) {
              if (!(tech.equals_ignore_case("variations") || tech.equals_ignore_case("features-opentype") || tech.equals_ignore_case("palettes") || tech.equals_ignore_case("incremental")))
                src.supported = false;
            }
          } else {
            bad = true;
            break;
          }
        }
        if (bad || !have_location) continue;
        face.sources[face.source_count++] = src;
      }
    } else if (is("font-weight")) {
      uint64_t at = 0;
      StringView a, b, extra;
      double w[2];
      uint32_t n = 0;
      while (next_part(value, &at, &a)) {
        if (n == 2) return;
        if (a.equals_ignore_case("normal")) w[n++] = 400;
        else if (a.equals_ignore_case("bold")) w[n++] = 700;
        else if (!evaluate_number(a, ctx, &w[n]) || w[n] < 1 || w[n] > 1000) return;
        else ++n;
      }
      if (n == 0) return;
      (void)b; (void)extra;
      face.weight_min = static_cast<uint16_t>(w[0] + 0.5);
      face.weight_max = static_cast<uint16_t>((n == 2 ? w[1] : w[0]) + 0.5);
      if (face.weight_min > face.weight_max) { const uint16_t t = face.weight_min; face.weight_min = face.weight_max; face.weight_max = t; }
    } else if (is("font-style")) {
      uint64_t at = 0;
      StringView a;
      if (!next_part(value, &at, &a)) return;
      if (a.equals_ignore_case("normal")) face.style = 0;
      else if (a.equals_ignore_case("italic")) face.style = 1;
      else if (a.equals_ignore_case("oblique")) {
        face.style = 2;
        double deg[2] = {14, 14};
        uint32_t n = 0;
        StringView angle;
        while (next_part(value, &at, &angle) && n < 2) {
          if (!angle_degrees(angle, ctx, &deg[n])) return;
          ++n;
        }
        face.oblique_min_x100 = to_px100(deg[0]);
        face.oblique_max_x100 = to_px100(n == 2 ? deg[1] : deg[0]);
      }
    } else if (is("font-stretch") || is("font-width")) {
      uint64_t at = 0;
      StringView a;
      uint16_t v[2];
      uint32_t n = 0;
      static const struct { const char* name; uint16_t x10; } kStretch[] = {{"ultra-condensed", 500}, {"extra-condensed", 625}, {"condensed", 750}, {"semi-condensed", 875},
                                                                             {"normal", 1000}, {"semi-expanded", 1125}, {"expanded", 1250}, {"extra-expanded", 1500},
                                                                             {"ultra-expanded", 2000}};
      while (next_part(value, &at, &a)) {
        if (n == 2) return;
        bool found = false;
        for (const auto& k : kStretch)
          if (a.equals_ignore_case(k.name)) { v[n++] = k.x10; found = true; break; }
        if (found) continue;
        double d;
        U unit;
        if (!parse_dimension(a, &d, &unit) || unit != U::Percent || d < 0) return;
        v[n++] = static_cast<uint16_t>((d < 50 ? 50 : d > 200 ? 200 : d) * 10 + 0.5);
      }
      if (n == 0) return;
      face.stretch_min = v[0] < (n == 2 ? v[1] : v[0]) ? v[0] : (n == 2 ? v[1] : v[0]);
      face.stretch_max = v[0] < (n == 2 ? v[1] : v[0]) ? (n == 2 ? v[1] : v[0]) : v[0];
    } else if (is("unicode-range")) {
      uint64_t at = 0;
      StringView item;
      List<UnicodeRange> parsed(allocator_);
      while (next_comma_part(value, &at, &item)) {
        UnicodeRange r;
        if (!parse_unicode_range(item, &r)) return;   // (one bad range: the descriptor is invalid)
        if (parsed.count() >= 256 || !parsed.push_back(r)) return;
      }
      ranges.clear();
      for (uint64_t i = 0; i < parsed.count(); ++i) ranges.push_back(parsed[i]);
    } else if (is("font-display")) {
      if (value.equals_ignore_case("auto")) face.display = FontDisplay::Auto;
      else if (value.equals_ignore_case("block")) face.display = FontDisplay::Block;
      else if (value.equals_ignore_case("swap")) face.display = FontDisplay::Swap;
      else if (value.equals_ignore_case("fallback")) face.display = FontDisplay::Fallback;
      else if (value.equals_ignore_case("optional")) face.display = FontDisplay::Optional;
    }
  });
  if (!ok) return false;
  if (!have_family || face.source_count == 0) return true;   // (not a usable face)
  if (unicode_ranges_.count() + ranges.count() > kMaxUnicodeRanges) return true;
  face.first_range = static_cast<uint32_t>(unicode_ranges_.count());
  face.range_count = static_cast<uint32_t>(ranges.count());
  for (uint64_t i = 0; i < ranges.count(); ++i)
    if (!unicode_ranges_.push_back(ranges[i])) return false;
  return font_faces_.push_back(face);
}

bool StyleSheet::font_face(uint32_t index, FontFace* out) const {
  if (index >= font_faces_.count()) return false;
  const FontFaceData& d = font_faces_[index];
  *out = FontFace();
  out->family = span_text(d.family);
  out->source_count = d.source_count;
  for (uint32_t i = 0; i < d.source_count; ++i) {
    out->sources[i].local = d.sources[i].local;
    out->sources[i].value = span_text(d.sources[i].value);
    out->sources[i].format = d.sources[i].format;
    out->sources[i].format_text = span_text(d.sources[i].format_text);
    out->sources[i].supported_tech = d.sources[i].supported;
  }
  out->weight_min = d.weight_min;
  out->weight_max = d.weight_max;
  out->style = d.style;
  out->oblique_min_x100 = d.oblique_min_x100;
  out->oblique_max_x100 = d.oblique_max_x100;
  out->stretch_min_x10 = d.stretch_min;
  out->stretch_max_x10 = d.stretch_max;
  out->display = d.display;
  if (d.range_count > 0 && d.first_range <= unicode_ranges_.count() && d.range_count <= unicode_ranges_.count() - d.first_range)
    out->unicode_ranges = ArrayView<const UnicodeRange>(unicode_ranges_.view()).subview(d.first_range, d.range_count);
  return true;
}

bool StyleSheet::parse_keyframes(StringView prelude, StringView body) {
  if (keyframes_.count() >= kMaxKeyframes) return true;
  StringView name = unquote(prelude.trimmed());
  if (name.empty() || eq(name, "none") || is_css_wide(name)) return true;
  KeyframesRule rule;
  rule.first_stop = static_cast<uint32_t>(stops_.count());
  rule.stop_count = 0;
  if (!keep_span(name, &rule.name)) return false;
  List<KeyframeStop> found(allocator_);
  uint64_t i = 0;
  while (i < body.size()) {
    skip_space(body, &i);
    if (i >= body.size()) break;
    const StringView selectors = scan_until(body, &i, '{', '}');
    if (i >= body.size() || body[i] != '{') break;
    const StringView block = take_block(body, &i);
    uint64_t at = 0;
    StringView sel;
    int32_t offsets[8];
    uint32_t n = 0;
    bool valid = true;
    while (next_comma_part(selectors, &at, &sel)) {
      if (n == 8) break;
      if (sel.equals_ignore_case("from")) offsets[n++] = 0;
      else if (sel.equals_ignore_case("to")) offsets[n++] = 1000;
      else {
        double d;
        U unit;
        if (!parse_dimension(sel, &d, &unit) || unit != U::Percent || d < 0 || d > 100) { valid = false; break; }
        offsets[n++] = static_cast<int32_t>(d * 10 + 0.5);
      }
    }
    if (!valid || n == 0) continue;
    uint32_t first, count;
    if (!add_declaration_run(block, &first, &count)) return false;
    for (uint32_t k = 0; k < n && found.count() < kMaxKeyframeStops; ++k) {
      KeyframeStop stop;
      stop.offset_x1000 = offsets[k];
      stop.first_declaration = first;
      stop.declaration_count = count;
      // (sorted by offset; stops at the same offset stay in the order they were written)
      uint64_t at_index = found.count();
      while (at_index > 0 && found[at_index - 1].offset_x1000 > stop.offset_x1000) --at_index;
      if (!found.insert(at_index, stop)) return false;
    }
  }
  for (uint64_t k = 0; k < found.count(); ++k)
    if (!stops_.push_back(found[k])) return false;
  rule.stop_count = static_cast<uint32_t>(found.count());
  return keyframes_.push_back(rule);
}

StringView StyleSheet::keyframes_name(uint32_t rule) const { return rule < keyframes_.count() ? span_text(keyframes_[rule].name) : StringView(); }

bool StyleSheet::find_keyframes(StringView name, uint32_t* rule) const {
  for (uint64_t i = keyframes_.count(); i > 0; --i)
    if (span_text(keyframes_[i - 1].name).equals(name)) { *rule = static_cast<uint32_t>(i - 1); return true; }
  return false;
}

uint32_t StyleSheet::keyframe_count(uint32_t rule) const { return rule < keyframes_.count() ? keyframes_[rule].stop_count : 0; }

int32_t StyleSheet::keyframe_offset_x1000(uint32_t rule, uint32_t index) const {
  if (rule >= keyframes_.count() || index >= keyframes_[rule].stop_count) return 0;
  return stops_[keyframes_[rule].first_stop + index].offset_x1000;
}

uint32_t StyleSheet::keyframe_declaration_count(uint32_t rule, uint32_t index) const {
  if (rule >= keyframes_.count() || index >= keyframes_[rule].stop_count) return 0;
  return stops_[keyframes_[rule].first_stop + index].declaration_count;
}

bool StyleSheet::keyframe_declaration(uint32_t rule, uint32_t index, uint32_t k, StringView* name, StringView* value) const {
  if (rule >= keyframes_.count() || index >= keyframes_[rule].stop_count) return false;
  const KeyframeStop& stop = stops_[keyframes_[rule].first_stop + index];
  if (k >= stop.declaration_count) return false;
  const Declaration& d = declarations_[stop.first_declaration + k];
  *value = span_text(Span{d.value_offset, d.value_length});
  if (d.property == pCustom) {
    *name = span_text(Span{d.name_offset, d.name_length});
    return true;
  }
  for (const PropertyName& p : kProperties)
    if (p.property == d.property) { *name = StringView(p.name); return true; }
  return false;
}

void StyleSheet::apply_keyframe(uint32_t rule, uint32_t index, const Style& parent, Style* style) const {
  if (rule >= keyframes_.count() || index >= keyframes_[rule].stop_count) return;
  const KeyframeStop& stop = stops_[keyframes_[rule].first_stop + index];
  CssApply a(*this, parent, style, nullptr);
  for (int round = 1; round <= 2; ++round) {
    for (uint32_t k = 0; k < stop.declaration_count; ++k) {
      const Declaration& d = declarations_[stop.first_declaration + k];
      const Prop p = static_cast<Prop>(d.property);
      const StringView value = StringView(pool_.view()).substr(d.value_offset, d.value_length);
      if (p == pCustom) continue;
      if (round == 1 ? (p == pFontSize || p == pColorScheme) : (p != pFontSize && p != pColorScheme)) a.declare(p, value);
    }
  }
  finish_style(style);
}

bool StyleSheet::parse_property_rule(StringView prelude, StringView body) {
  StringView name = prelude.trimmed();
  if (name.size() < 3 || name[0] != '-' || name[1] != '-' || registrations_.count() >= 256) return true;
  Registration reg;
  bool have_syntax = false, have_inherits = false;
  reg.inherits = true;
  reg.has_initial = false;
  bool ok = true;
  for_each_declaration(body, [&](StringView n, StringView v, bool) {
    if (!ok) return;
    if (n.equals_ignore_case("syntax")) {
      have_syntax = true;
      ok = keep_span(unquote(v), &reg.syntax);
    } else if (n.equals_ignore_case("inherits")) {
      if (v.equals_ignore_case("true")) { reg.inherits = true; have_inherits = true; }
      else if (v.equals_ignore_case("false")) { reg.inherits = false; have_inherits = true; }
    } else if (n.equals_ignore_case("initial-value")) {
      reg.has_initial = true;
      ok = keep_span(v, &reg.initial);
    }
  });
  if (!ok) return false;
  const bool universal = span_text(reg.syntax).trimmed().equals("*");
  if (!have_syntax || !have_inherits || (!reg.has_initial && !universal)) return true;
  if (!keep_span(name, &reg.name)) return false;
  return registrations_.push_back(reg);
}

bool StyleSheet::property_registration(uint32_t index, PropertyRegistration* out) const {
  if (index >= registrations_.count()) return false;
  const Registration& r = registrations_[index];
  out->name = span_text(r.name);
  out->syntax = span_text(r.syntax);
  out->inherits = r.inherits;
  out->has_initial = r.has_initial;
  out->initial_value = span_text(r.initial);
  return true;
}

void StyleSheet::apply(const dom::Node* element, StringView inline_style, const Style& parent, Style* style, PseudoElement pseudo) const {
  uint32_t matched[kMaxMatches];
  uint32_t count = 0;
  uint32_t element_scope, host_scope;
  element_scopes(element, &element_scope, &host_scope);
  for (uint32_t s = 0; s < selectors_.count() && count < kMaxMatches; ++s) {
    if (selectors_.pseudo(s) != static_cast<uint8_t>(pseudo) || !rule_applies(s, element_scope, host_scope) || !selectors_.matches(s, element, nullptr, true)) continue;
    const Rule& rule = rules_[selectors_.tag(s)];
    if (rule.container != 0 && !container_applies(rule.container, element)) continue;
    matched[count++] = s;
  }
  // The order the declarations are applied in (the last one wins): by layer, then specificity, then where it came from. Normal declarations: later layers win and
  // unlayered rules win over layers; !important ones go the other way round.
  auto rank_of = [&](uint32_t selector) {
    const uint32_t layer = rules_[selectors_.tag(selector)].layer;
    return layer == 0 || layer > layer_rank_.count() ? static_cast<uint32_t>(layers_.count() + 1) : layer_rank_[layer - 1];
  };
  uint32_t ordered[2][kMaxMatches];
  for (int which = 0; which < 2; ++which) {
    for (uint32_t m = 0; m < count; ++m) {
      uint32_t k = m;
      ordered[which][k] = matched[m];
      while (k > 0) {
        const uint32_t before = ordered[which][k - 1], now = ordered[which][k];
        const uint32_t rb = rank_of(before), rn = rank_of(now);
        const bool in_order = rb != rn ? (which == 0 ? rb < rn : rb > rn)
                                       : (selectors_.specificity(before) != selectors_.specificity(now) ? selectors_.specificity(before) < selectors_.specificity(now)
                                                                                                         : selectors_.tag(before) <= selectors_.tag(now));
        if (in_order) break;
        ordered[which][k - 1] = now;
        ordered[which][k] = before;
        --k;
      }
    }
  }
  CssApply a(*this, parent, style, element);
  const bool is_root = !element->parent || !element->parent->is_element();
  // Three rounds over the same declarations: custom properties first (so that var() has its values), then the font size and colour scheme (em, rem and light-dark()
  // mean them), then the rest. In each, the ordinary declarations come before the !important ones, the style attribute after the rules.
  for (int round = 0; round < 3; ++round) {
    for (int pass = 0; pass < 2; ++pass) {
      const bool important = pass == 1;
      for (uint32_t m = 0; m < count; ++m) {
        const Rule& rule = rules_[selectors_.tag(ordered[pass][m])];
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
            if (p == pFontSize || p == pColorScheme) a.declare(p, value);
          } else if (p != pCustom && p != pFontSize && p != pColorScheme) {
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
          if (p == pFontSize || p == pColorScheme) a.declare(p, value);
        } else if (p != pCustom && p != pFontSize && p != pColorScheme) {
          a.declare(p, value);
        }
      });
    }
    if (round == 1 && is_root) {   // rem is the size of the root element's font
      style->root_font_px = style->font_px;
      a.refresh();
    }
  }
  finish_style(style);
}

}  // namespace browser
