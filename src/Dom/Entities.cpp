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
  uint16_t code_point;
};

// The named references that pages use: the Latin-1 letters and signs, Greek, punctuation, arrows, mathematics and cards.
const Entity kEntities[] = {
    {"quot", 34}, {"amp", 38}, {"apos", 39}, {"lt", 60}, {"gt", 62}, {"nbsp", 160}, {"iexcl", 161}, {"cent", 162}, {"pound", 163}, {"curren", 164},
    {"yen", 165}, {"brvbar", 166}, {"sect", 167}, {"uml", 168}, {"copy", 169}, {"ordf", 170}, {"laquo", 171}, {"not", 172}, {"shy", 173}, {"reg", 174},
    {"macr", 175}, {"deg", 176}, {"plusmn", 177}, {"sup2", 178}, {"sup3", 179}, {"acute", 180}, {"micro", 181}, {"para", 182}, {"middot", 183},
    {"cedil", 184}, {"sup1", 185}, {"ordm", 186}, {"raquo", 187}, {"frac14", 188}, {"frac12", 189}, {"frac34", 190}, {"iquest", 191},
    {"Agrave", 192}, {"Aacute", 193}, {"Acirc", 194}, {"Atilde", 195}, {"Auml", 196}, {"Aring", 197}, {"AElig", 198}, {"Ccedil", 199}, {"Egrave", 200},
    {"Eacute", 201}, {"Ecirc", 202}, {"Euml", 203}, {"Igrave", 204}, {"Iacute", 205}, {"Icirc", 206}, {"Iuml", 207}, {"ETH", 208}, {"Ntilde", 209},
    {"Ograve", 210}, {"Oacute", 211}, {"Ocirc", 212}, {"Otilde", 213}, {"Ouml", 214}, {"times", 215}, {"Oslash", 216}, {"Ugrave", 217}, {"Uacute", 218},
    {"Ucirc", 219}, {"Uuml", 220}, {"Yacute", 221}, {"THORN", 222}, {"szlig", 223}, {"agrave", 224}, {"aacute", 225}, {"acirc", 226}, {"atilde", 227},
    {"auml", 228}, {"aring", 229}, {"aelig", 230}, {"ccedil", 231}, {"egrave", 232}, {"eacute", 233}, {"ecirc", 234}, {"euml", 235}, {"igrave", 236},
    {"iacute", 237}, {"icirc", 238}, {"iuml", 239}, {"eth", 240}, {"ntilde", 241}, {"ograve", 242}, {"oacute", 243}, {"ocirc", 244}, {"otilde", 245},
    {"ouml", 246}, {"divide", 247}, {"oslash", 248}, {"ugrave", 249}, {"uacute", 250}, {"ucirc", 251}, {"uuml", 252}, {"yacute", 253}, {"thorn", 254},
    {"yuml", 255}, {"OElig", 338}, {"oelig", 339}, {"Scaron", 352}, {"scaron", 353}, {"Yuml", 376}, {"fnof", 402}, {"circ", 710}, {"tilde", 732},
    {"Alpha", 913}, {"Beta", 914}, {"Gamma", 915}, {"Delta", 916}, {"Epsilon", 917}, {"Zeta", 918}, {"Eta", 919}, {"Theta", 920}, {"Iota", 921},
    {"Kappa", 922}, {"Lambda", 923}, {"Mu", 924}, {"Nu", 925}, {"Xi", 926}, {"Omicron", 927}, {"Pi", 928}, {"Rho", 929}, {"Sigma", 931}, {"Tau", 932},
    {"Upsilon", 933}, {"Phi", 934}, {"Chi", 935}, {"Psi", 936}, {"Omega", 937}, {"alpha", 945}, {"beta", 946}, {"gamma", 947}, {"delta", 948},
    {"epsilon", 949}, {"zeta", 950}, {"eta", 951}, {"theta", 952}, {"iota", 953}, {"kappa", 954}, {"lambda", 955}, {"mu", 956}, {"nu", 957}, {"xi", 958},
    {"omicron", 959}, {"pi", 960}, {"rho", 961}, {"sigmaf", 962}, {"sigma", 963}, {"tau", 964}, {"upsilon", 965}, {"phi", 966}, {"chi", 967}, {"psi", 968},
    {"omega", 969}, {"ensp", 8194}, {"emsp", 8195}, {"thinsp", 8201}, {"zwnj", 8204}, {"zwj", 8205}, {"lrm", 8206}, {"rlm", 8207}, {"ndash", 8211},
    {"mdash", 8212}, {"lsquo", 8216}, {"rsquo", 8217}, {"sbquo", 8218}, {"ldquo", 8220}, {"rdquo", 8221}, {"bdquo", 8222}, {"dagger", 8224},
    {"Dagger", 8225}, {"bull", 8226}, {"hellip", 8230}, {"permil", 8240}, {"prime", 8242}, {"Prime", 8243}, {"lsaquo", 8249}, {"rsaquo", 8250},
    {"oline", 8254}, {"euro", 8364}, {"trade", 8482}, {"larr", 8592}, {"uarr", 8593}, {"rarr", 8594}, {"darr", 8595}, {"harr", 8596}, {"lArr", 8656},
    {"rArr", 8658}, {"hArr", 8660}, {"forall", 8704}, {"part", 8706}, {"exist", 8707}, {"empty", 8709}, {"nabla", 8711}, {"isin", 8712}, {"notin", 8713},
    {"ni", 8715}, {"prod", 8719}, {"sum", 8721}, {"minus", 8722}, {"lowast", 8727}, {"radic", 8730}, {"prop", 8733}, {"infin", 8734}, {"ang", 8736},
    {"and", 8743}, {"or", 8744}, {"cap", 8745}, {"cup", 8746}, {"int", 8747}, {"there4", 8756}, {"sim", 8764}, {"cong", 8773}, {"asymp", 8776},
    {"ne", 8800}, {"equiv", 8801}, {"le", 8804}, {"ge", 8805}, {"sub", 8834}, {"sup", 8835}, {"nsub", 8836}, {"sube", 8838}, {"supe", 8839},
    {"oplus", 8853}, {"otimes", 8855}, {"perp", 8869}, {"sdot", 8901}, {"lceil", 8968}, {"rceil", 8969}, {"lfloor", 8970}, {"rfloor", 8971},
    {"lang", 9001}, {"rang", 9002}, {"loz", 9674}, {"spades", 9824}, {"clubs", 9827}, {"hearts", 9829}, {"diams", 9830}, {"check", 10003},
};

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
    append_utf8(static_cast<uint32_t>(value), out);
    return end - at;
  }
  const uint64_t name_start = end;
  while (end < s.size() && ((s[end] >= 'a' && s[end] <= 'z') || (s[end] >= 'A' && s[end] <= 'Z') || (s[end] >= '0' && s[end] <= '9')) && end - name_start < 12) ++end;
  const StringView name = s.substr(name_start, end - name_start);
  const bool semicolon = end < s.size() && s[end] == ';';
  // the longest name that matches (references without ";" are allowed for the few old ones, in text)
  for (uint64_t length = name.size(); length > 0; --length) {
    const StringView candidate = name.first(length);
    for (const Entity& e : kEntities) {
      if (!candidate.equals(e.name)) continue;
      const bool whole = length == name.size() && semicolon;
      if (!whole) {
        // without a ";" only the legacy references count, and not before more name characters in an attribute
        const bool legacy = e.code_point <= 255 || candidate.equals("amp") || candidate.equals("lt") || candidate.equals("gt") || candidate.equals("quot");
        if (!legacy) continue;
        if (in_attribute && (length < name.size() || (end < s.size() && s[end] == '='))) continue;
      }
      append_utf8(e.code_point, out);
      return 1 + length + (whole ? 1 : 0);
    }
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
