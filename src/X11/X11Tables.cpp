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

#include "X11/X11Protocol.hpp"

namespace x11 {

namespace {

struct Keysyms {
  uint8_t usage;
  uint32_t plain, shifted;
};

// USB HID keyboard usages and their keysyms (US layout).
constexpr Keysyms kKeys[] = {
    {0x28, 0xFF0D, 0xFF0D}, {0x29, 0xFF1B, 0xFF1B}, {0x2A, 0xFF08, 0xFF08}, {0x2B, 0xFF09, 0xFE20}, {0x2C, 0x20, 0x20},
    {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', '{'}, {0x30, ']', '}'}, {0x31, '\\', '|'}, {0x32, '\\', '|'}, {0x33, ';', ':'}, {0x34, '\'', '"'},
    {0x35, '`', '~'}, {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'}, {0x39, 0xFFE5, 0xFFE5},
    {0x46, 0xFF61, 0xFF61}, {0x47, 0xFF14, 0xFF14}, {0x48, 0xFF13, 0xFF13}, {0x49, 0xFF63, 0xFF63}, {0x4A, 0xFF50, 0xFF50}, {0x4B, 0xFF55, 0xFF55},
    {0x4C, 0xFFFF, 0xFFFF}, {0x4D, 0xFF57, 0xFF57}, {0x4E, 0xFF56, 0xFF56}, {0x4F, 0xFF53, 0xFF53}, {0x50, 0xFF51, 0xFF51}, {0x51, 0xFF54, 0xFF54},
    {0x52, 0xFF52, 0xFF52},
    {0x53, 0xFF7F, 0xFF7F}, {0x54, 0xFFAF, 0xFFAF}, {0x55, 0xFFAA, 0xFFAA}, {0x56, 0xFFAD, 0xFFAD}, {0x57, 0xFFAB, 0xFFAB}, {0x58, 0xFF8D, 0xFF8D},
    // keypad 1..9, 0, decimal: the navigation keysym, then the digit Num Lock gives
    {0x59, 0xFF9C, 0xFFB1}, {0x5A, 0xFF99, 0xFFB2}, {0x5B, 0xFF9B, 0xFFB3}, {0x5C, 0xFF96, 0xFFB4}, {0x5D, 0xFF9D, 0xFFB5}, {0x5E, 0xFF98, 0xFFB6},
    {0x5F, 0xFF95, 0xFFB7}, {0x60, 0xFF97, 0xFFB8}, {0x61, 0xFF9A, 0xFFB9}, {0x62, 0xFF9E, 0xFFB0}, {0x63, 0xFF9F, 0xFFAE},
    {0x64, '<', '>'}, {0x65, 0xFF67, 0xFF67},
    {0xE0, 0xFFE3, 0xFFE3}, {0xE1, 0xFFE1, 0xFFE1}, {0xE2, 0xFFE9, 0xFFE9}, {0xE3, 0xFFEB, 0xFFEB},
    {0xE4, 0xFFE4, 0xFFE4}, {0xE5, 0xFFE2, 0xFFE2}, {0xE6, 0xFFEA, 0xFFEA}, {0xE7, 0xFFEC, 0xFFEC},
};

constexpr char kShiftedDigits[] = ")!@#$%^&*(";

}  // namespace

bool keysyms_of_keycode(uint8_t keycode, uint32_t* plain, uint32_t* shifted) {
  if (keycode < kMinKeycode) return false;
  const uint32_t usage = keycode - 8u;
  if (usage >= 0x04 && usage <= 0x1D) {      // a .. z
    *plain = 'a' + (usage - 0x04);
    *shifted = 'A' + (usage - 0x04);
    return true;
  }
  if (usage >= 0x1E && usage <= 0x27) {      // 1 .. 9, 0
    const uint32_t digit = usage == 0x27 ? 0 : usage - 0x1E + 1;
    *plain = '0' + digit;
    *shifted = static_cast<uint8_t>(kShiftedDigits[digit]);
    return true;
  }
  if (usage >= 0x3A && usage <= 0x45) {      // F1 .. F12
    *plain = *shifted = 0xFFBE + (usage - 0x3A);
    return true;
  }
  if (usage >= 0x68 && usage <= 0x73) {      // F13 .. F24
    *plain = *shifted = 0xFFCA + (usage - 0x68);
    return true;
  }
  for (const Keysyms& k : kKeys) {
    if (k.usage != usage) continue;
    *plain = k.plain;
    *shifted = k.shifted;
    return true;
  }
  return false;
}

uint32_t system_cursor_of_glyph(uint32_t glyph) {
  switch (glyph) {
    case 152: return 32513;                                                    // xterm: I-beam
    case 150: return 32514;                                                    // watch
    case 30: case 34: case 90: case 130: return 32515;                         // cross, crosshair, plus, tcross
    case 134: case 14: return 32642;                                           // top_left / bottom_right corner
    case 136: case 12: return 32643;                                           // top_right / bottom_left corner
    case 108: case 70: case 96: return 32644;                                  // sb_h_double_arrow, left_side, right_side
    case 116: case 138: case 16: case 42: return 32645;                        // sb_v_double_arrow, top_side, bottom_side, double_arrow
    case 52: case 120: return 32646;                                           // fleur, sizing
    case 0: case 24: case 88: return 32648;                                    // X_cursor, circle, pirate
    case 58: case 60: return 32649;                                            // hand1, hand2
    default: return 32512;                                                     // left_ptr and the rest: the arrow
  }
}

namespace {

struct NamedColor {
  const char* name;      // lower case, no spaces
  uint8_t r, g, b;
};

constexpr NamedColor kColors[] = {
    {"black", 0, 0, 0}, {"white", 255, 255, 255}, {"red", 255, 0, 0}, {"green", 0, 255, 0}, {"blue", 0, 0, 255}, {"yellow", 255, 255, 0},
    {"cyan", 0, 255, 255}, {"magenta", 255, 0, 255}, {"gray", 190, 190, 190}, {"grey", 190, 190, 190}, {"darkgray", 169, 169, 169},
    {"darkgrey", 169, 169, 169}, {"lightgray", 211, 211, 211}, {"lightgrey", 211, 211, 211}, {"dimgray", 105, 105, 105}, {"dimgrey", 105, 105, 105},
    {"slategray", 112, 128, 144}, {"slategrey", 112, 128, 144}, {"lightslategray", 119, 136, 153}, {"darkslategray", 47, 79, 79},
    {"navy", 0, 0, 128}, {"navyblue", 0, 0, 128}, {"orange", 255, 165, 0}, {"darkorange", 255, 140, 0}, {"pink", 255, 192, 203},
    {"purple", 160, 32, 240}, {"brown", 165, 42, 42}, {"gold", 255, 215, 0}, {"maroon", 176, 48, 96}, {"beige", 245, 245, 220},
    {"wheat", 245, 222, 179}, {"tan", 210, 180, 140}, {"khaki", 240, 230, 140}, {"ivory", 255, 255, 240}, {"snow", 255, 250, 250},
    {"linen", 250, 240, 230}, {"lavender", 230, 230, 250}, {"salmon", 250, 128, 114}, {"coral", 255, 127, 80}, {"tomato", 255, 99, 71},
    {"orchid", 218, 112, 214}, {"violet", 238, 130, 238}, {"turquoise", 64, 224, 208}, {"darkgreen", 0, 100, 0}, {"forestgreen", 34, 139, 34},
    {"seagreen", 46, 139, 87}, {"limegreen", 50, 205, 50}, {"darkblue", 0, 0, 139}, {"lightblue", 173, 216, 230}, {"skyblue", 135, 206, 235},
    {"steelblue", 70, 130, 180}, {"royalblue", 65, 105, 225}, {"darkred", 139, 0, 0}, {"firebrick", 178, 34, 34}, {"indianred", 205, 92, 92},
    {"chocolate", 210, 105, 30}, {"sienna", 160, 82, 45}, {"peru", 205, 133, 63}, {"lightyellow", 255, 255, 224}, {"lightcyan", 224, 255, 255},
    {"darkcyan", 0, 139, 139}, {"darkmagenta", 139, 0, 139}, {"aquamarine", 127, 255, 212}, {"honeydew", 240, 255, 240}, {"mintcream", 245, 255, 250},
    {"azure", 240, 255, 255}, {"aliceblue", 240, 248, 255}, {"ghostwhite", 248, 248, 255}, {"whitesmoke", 245, 245, 245}, {"gainsboro", 220, 220, 220},
    {"midnightblue", 25, 25, 112}, {"cornflowerblue", 100, 149, 237}, {"dodgerblue", 30, 144, 255}, {"deepskyblue", 0, 191, 255},
    {"cadetblue", 95, 158, 160}, {"olivedrab", 107, 142, 35}, {"darkolivegreen", 85, 107, 47}, {"yellowgreen", 154, 205, 50},
    {"springgreen", 0, 255, 127}, {"lawngreen", 124, 252, 0}, {"chartreuse", 127, 255, 0}, {"greenyellow", 173, 255, 47}, {"goldenrod", 218, 165, 32},
    {"darkgoldenrod", 184, 134, 11}, {"rosybrown", 188, 143, 143}, {"saddlebrown", 139, 69, 19}, {"sandybrown", 244, 164, 96},
    {"burlywood", 222, 184, 135}, {"moccasin", 255, 228, 181}, {"peachpuff", 255, 218, 185}, {"mistyrose", 255, 228, 225},
    {"lavenderblush", 255, 240, 245}, {"hotpink", 255, 105, 180}, {"deeppink", 255, 20, 147}, {"lightpink", 255, 182, 193},
    {"palevioletred", 219, 112, 147}, {"mediumvioletred", 199, 21, 133}, {"blueviolet", 138, 43, 226}, {"darkviolet", 148, 0, 211},
    {"darkorchid", 153, 50, 204}, {"mediumpurple", 147, 112, 219}, {"slateblue", 106, 90, 205}, {"mediumslateblue", 123, 104, 238},
    {"darkslateblue", 72, 61, 139}, {"oldlace", 253, 245, 230}, {"papayawhip", 255, 239, 213}, {"blanchedalmond", 255, 235, 205},
    {"bisque", 255, 228, 196}, {"antiquewhite", 250, 235, 215}, {"floralwhite", 255, 250, 240}, {"cornsilk", 255, 248, 220},
    {"lemonchiffon", 255, 250, 205}, {"seashell", 255, 245, 238}, {"lightgoldenrod", 238, 221, 130}, {"palegoldenrod", 238, 232, 170},
    {"lightsalmon", 255, 160, 122}, {"darksalmon", 233, 150, 122}, {"orangered", 255, 69, 0}, {"lightcoral", 240, 128, 128},
    {"lightseagreen", 32, 178, 170}, {"mediumseagreen", 60, 179, 113}, {"palegreen", 152, 251, 152}, {"darkseagreen", 143, 188, 143},
    {"mediumaquamarine", 102, 205, 170}, {"darkturquoise", 0, 206, 209}, {"mediumturquoise", 72, 209, 204}, {"paleturquoise", 175, 238, 238},
    {"powderblue", 176, 224, 230}, {"lightsteelblue", 176, 196, 222}, {"lightskyblue", 135, 206, 250}, {"mediumblue", 0, 0, 205},
    {"lightslateblue", 132, 112, 255}, {"olive", 128, 128, 0}, {"teal", 0, 128, 128}, {"silver", 192, 192, 192}, {"lime", 0, 255, 0},
    {"aqua", 0, 255, 255}, {"fuchsia", 255, 0, 255}, {"crimson", 220, 20, 60}, {"indigo", 75, 0, 130},
};

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

int32_t hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = lower(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// `digits` hex digits as a 16-bit channel; `scale`: rgb:-style (spread over the range) or #-style (the digits are the high bits).
bool hex_channel(StringView text, bool scale, uint16_t* out) {
  if (text.empty() || text.size() > 4) return false;
  uint32_t v = 0;
  for (uint64_t i = 0; i < text.size(); ++i) {
    const int32_t d = hex_digit(text[i]);
    if (d < 0) return false;
    v = v * 16 + static_cast<uint32_t>(d);
  }
  const uint32_t bits = static_cast<uint32_t>(text.size()) * 4;
  if (scale) *out = static_cast<uint16_t>(v * 65535u / ((1u << bits) - 1));
  else *out = static_cast<uint16_t>(v << (16 - bits));
  return true;
}

}  // namespace

bool lookup_color(StringView name, uint16_t* red, uint16_t* green, uint16_t* blue) {
  if (name.empty()) return false;
  if (name[0] == '#') {
    const StringView hex = name.without_prefix(1);
    if (hex.size() == 0 || hex.size() % 3 != 0 || hex.size() > 12) return false;
    const uint64_t n = hex.size() / 3;
    return hex_channel(hex.substr(0, n), false, red) && hex_channel(hex.substr(n, n), false, green) && hex_channel(hex.substr(2 * n, n), false, blue);
  }
  if (name.size() > 4 && lower(name[0]) == 'r' && lower(name[1]) == 'g' && lower(name[2]) == 'b' && name[3] == ':') {
    StringView rest = name.without_prefix(4);
    StringView parts[3];
    for (uint32_t i = 0; i < 3; ++i) {
      const uint64_t slash = rest.find('/');
      if (i < 2 && slash == StringView::npos) return false;
      parts[i] = i < 2 ? rest.first(slash) : rest;
      if (i < 2) rest = rest.without_prefix(slash + 1);
    }
    return hex_channel(parts[0], true, red) && hex_channel(parts[1], true, green) && hex_channel(parts[2], true, blue);
  }
  // the name without spaces, lower case
  char key[48];
  uint32_t length = 0;
  for (uint64_t i = 0; i < name.size(); ++i) {
    if (name[i] == ' ') continue;
    if (length == sizeof key) return false;
    key[length++] = lower(name[i]);
  }
  StringView k(key, length);
  // grayN / greyN: N percent
  if (length > 4 && (k.starts_with("gray") || k.starts_with("grey"))) {
    uint32_t percent = 0;
    bool digits = true;
    for (uint32_t i = 4; i < length; ++i) {
      if (key[i] < '0' || key[i] > '9') { digits = false; break; }
      percent = percent * 10 + static_cast<uint32_t>(key[i] - '0');
      if (percent > 100) return false;
    }
    if (digits) {
      const uint16_t v = static_cast<uint16_t>((percent * 255 + 50) / 100 * 257);
      *red = *green = *blue = v;
      return true;
    }
  }
  // a numbered shade ("red3"): the colour darkened as rgb.txt's 1..4 are
  uint32_t shade = 0;
  if (length > 1 && key[length - 1] >= '1' && key[length - 1] <= '4') {
    shade = static_cast<uint32_t>(key[length - 1] - '0');
    k = k.first(length - 1);
  }
  constexpr uint32_t kShade[5] = {255, 255, 238, 205, 139};
  for (const NamedColor& c : kColors) {
    if (!k.equals(StringView(c.name))) continue;
    const uint32_t f = kShade[shade];
    *red = static_cast<uint16_t>(c.r * f / 255 * 257);
    *green = static_cast<uint16_t>(c.g * f / 255 * 257);
    *blue = static_cast<uint16_t>(c.b * f / 255 * 257);
    return true;
  }
  return false;
}

bool font_pattern_matches(StringView pattern, StringView name) {
  // iterative glob with one backtrack point (enough for '*')
  uint64_t p = 0, n = 0, star = StringView::npos, mark = 0;
  while (n < name.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(name[n]))) {
      ++p;
      ++n;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = n;
    } else if (star != StringView::npos) {
      p = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

}  // namespace x11
