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

#include "Dom/Entities.hpp"

namespace dom {

namespace {

struct Entity {
  const char* name;
  uint32_t code_point, second;   // second: 0 when the name stands for one character
  bool legacy;                   // also a reference without the closing ";"
};

const Entity kEntities[] = {
#include "Dom/EntityTable.inc"
};
constexpr uint64_t kLongestEntity = 31;

const Entity* find_entity(StringView name) {   // binary search over the sorted table
  uint64_t low = 0, high = sizeof(kEntities) / sizeof(kEntities[0]);
  while (low < high) {
    const uint64_t mid = low + (high - low) / 2;
    const StringView n(kEntities[mid].name);
    int order = 0;
    const uint64_t common = n.size() < name.size() ? n.size() : name.size();
    for (uint64_t i = 0; i < common && order == 0; ++i) order = static_cast<uint8_t>(n[i]) < static_cast<uint8_t>(name[i]) ? -1 : static_cast<uint8_t>(n[i]) > static_cast<uint8_t>(name[i]) ? 1 : 0;
    if (order == 0) order = n.size() < name.size() ? -1 : n.size() > name.size() ? 1 : 0;
    if (order == 0) return &kEntities[mid];
    if (order < 0) low = mid + 1;
    else high = mid;
  }
  return nullptr;
}

int hex_value(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

}  // namespace

void append_utf8(uint32_t cp, String* out) {
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

uint32_t next_code_point(StringView s, uint64_t* at) {
  const uint8_t c = static_cast<uint8_t>(s[*at]);
  uint32_t length = c >= 0xF0 && c < 0xF8 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
  if (c >= 0x80 && c < 0xC0) length = 1;
  if (length == 1) {
    ++*at;
    return c < 0x80 ? c : 0xFFFD;
  }
  if (*at + length > s.size()) {
    ++*at;
    return 0xFFFD;
  }
  uint32_t cp = c & (0xFF >> (length + 1));
  for (uint32_t i = 1; i < length; ++i) {
    const uint8_t b = static_cast<uint8_t>(s[*at + i]);
    if ((b & 0xC0) != 0x80) {
      ++*at;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  *at += length;
  return cp;
}

uint64_t decode_reference(StringView s, uint64_t at, bool in_attribute, String* out) {
  uint64_t end = at + 1;
  if (end < s.size() && s[end] == '#') {
    ++end;
    const bool hex = end < s.size() && (s[end] == 'x' || s[end] == 'X');
    if (hex) ++end;
    const uint64_t digits_start = end;
    uint64_t value = 0;
    while (end < s.size() && (hex ? hex_value(s[end]) >= 0 : (s[end] >= '0' && s[end] <= '9'))) {
      value = value * (hex ? 16 : 10) + static_cast<uint64_t>(hex ? hex_value(s[end]) : s[end] - '0');
      if (value > 0x10FFFF) value = 0x110000;
      ++end;
    }
    if (end == digits_start) {
      out->push_back('&');
      return 1;
    }
    if (end < s.size() && s[end] == ';') ++end;
    // the old Windows-1252 range
    static const uint16_t kWin[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
                                      0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
    if (value >= 0x80 && value <= 0x9F) value = kWin[value - 0x80];
    if (value == 0 || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) value = 0xFFFD;
    append_utf8(static_cast<uint32_t>(value), out);
    return end - at;
  }
  const uint64_t name_start = end;
  while (end < s.size() && ((s[end] >= 'a' && s[end] <= 'z') || (s[end] >= 'A' && s[end] <= 'Z') || (s[end] >= '0' && s[end] <= '9')) && end - name_start < kLongestEntity) ++end;
  const StringView name = s.substr(name_start, end - name_start);
  const bool semicolon = end < s.size() && s[end] == ';';
  // the longest name that matches (references without ";" are allowed for the legacy ones)
  for (uint64_t length = name.size(); length > 0; --length) {
    const Entity* e = find_entity(name.first(length));
    if (!e) continue;
    const bool whole = length == name.size() && semicolon;
    if (!whole) {
      if (!e->legacy) continue;
      if (in_attribute && (length < name.size() || (end < s.size() && s[end] == '='))) continue;
    }
    append_utf8(e->code_point, out);
    if (e->second) append_utf8(e->second, out);
    return 1 + length + (whole ? 1 : 0);
  }
  out->push_back('&');
  return 1;
}

const char* ascii_look_alike(uint32_t cp) {
  switch (cp) {
    case 0xA0: case 0x2002: case 0x2003: case 0x2009: return " ";
    case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0xFEFF: return "";
    case 0xA1: return "!";
    case 0xA2: return "c";
    case 0xA3: return "GBP";
    case 0xA5: return "JPY";
    case 0xA7: return "S";
    case 0xA9: return "(c)";
    case 0xAB: return "<<";
    case 0xAC: return "!";
    case 0xAE: return "(R)";
    case 0xB0: return "deg";
    case 0xB1: return "+/-";
    case 0xB2: return "2";
    case 0xB3: return "3";
    case 0xB5: return "u";
    case 0xB6: return "P";
    case 0xB7: case 0x2022: case 0x2219: case 0x22C5: return "*";
    case 0xB9: return "1";
    case 0xBB: return ">>";
    case 0xBC: return "1/4";
    case 0xBD: return "1/2";
    case 0xBE: return "3/4";
    case 0xBF: return "?";
    case 0xC6: return "AE";
    case 0xD0: return "D";
    case 0xD7: return "x";
    case 0xD8: return "O";
    case 0xDE: return "Th";
    case 0xDF: return "ss";
    case 0xE6: return "ae";
    case 0xF0: return "d";
    case 0xF7: return "/";
    case 0xF8: return "o";
    case 0xFE: return "th";
    case 0x152: return "OE";
    case 0x153: return "oe";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2020: return "+";
    case 0x2026: return "...";
    case 0x2030: return "%";
    case 0x2039: return "<";
    case 0x203A: return ">";
    case 0x20AC: return "EUR";
    case 0x2122: return "(TM)";
    case 0x2190: case 0x21D0: return "<-";
    case 0x2191: return "^";
    case 0x2192: case 0x21D2: return "->";
    case 0x2193: return "v";
    case 0x2194: return "<->";
    case 0x2260: return "!=";
    case 0x2264: return "<=";
    case 0x2265: return ">=";
    case 0x221E: return "inf";
    case 0x2713: case 0x2714: return "v";
    case 0x2665: return "<3";
    default: break;
  }
  // accented Latin letters: the letter they are made from
  static const char kLatin1[] = "AAAAAA?CEEEEIIII?NOOOOO?OUUUUY??aaaaaa?ceeeeiiii?nooooo?ouuuuy?y";   // U+00C0 .. U+00FF
  if (cp >= 0xC0 && cp <= 0xFF) {
    static char one[2];
    one[0] = kLatin1[cp - 0xC0];
    one[1] = 0;
    return one[0] == '?' ? nullptr : one;
  }
  if (cp >= 0x100 && cp <= 0x17F) {   // Latin Extended-A: pairs of capital and small letters of the same base
    static const char kExtA[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    static char one[2];
    one[0] = kExtA[cp - 0x100 < sizeof kExtA - 1 ? cp - 0x100 : 0];
    one[1] = 0;
    return one;
  }
  if (cp >= 0x391 && cp <= 0x3C9) {   // Greek letters by sound
    static const char* const kGreek[] = {"A", "B", "G", "D", "E", "Z", "E", "Th", "I", "K", "L", "M", "N", "X", "O", "P", "R", "", "S", "T", "U", "Ph", "Ch", "Ps", "O", "", "", "", "", "", "", "",
                                          "a", "b", "g", "d", "e", "z", "e", "th", "i", "k", "l", "m", "n", "x", "o", "p", "r", "s", "s", "t", "u", "ph", "ch", "ps", "o"};
    const char* g = kGreek[cp - 0x391];
    return g[0] ? g : nullptr;
  }
  return nullptr;
}

}  // namespace dom
