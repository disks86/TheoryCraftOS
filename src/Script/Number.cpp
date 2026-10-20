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

#include "Script/Number.hpp"

namespace script {

namespace {
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
constexpr double kMax = 1.7976931348623157e308;
}  // namespace

double power_of_ten(int e) {
  static const double kTable[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  if (e < 0) return 1.0 / power_of_ten(-e);
  if (e <= 22) return kTable[e];
  double result = kTable[22];
  e -= 22;
  while (e >= 22) {
    result *= kTable[22];
    e -= 22;
    if (result > kMax) return result * 2;   // (infinity)
  }
  return result * kTable[e];
}

bool parse_decimal(StringView text, double* out) {
  uint64_t i = 0;
  const uint64_t n = text.size();
  uint64_t mantissa = 0;
  int32_t exponent = 0, digits = 0;
  bool any = false;
  for (; i < n && is_digit(text[i]); ++i) {
    any = true;
    if (digits < 19) {
      mantissa = mantissa * 10 + static_cast<uint64_t>(text[i] - '0');
      if (mantissa) ++digits;
    } else {
      ++exponent;
    }
  }
  if (i < n && text[i] == '.') {
    ++i;
    for (; i < n && is_digit(text[i]); ++i) {
      any = true;
      if (digits < 19) {
        mantissa = mantissa * 10 + static_cast<uint64_t>(text[i] - '0');
        if (mantissa) ++digits;
        --exponent;
      }
    }
  }
  if (!any) return false;
  if (i < n && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    bool negative = false;
    if (i < n && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
    if (i >= n || !is_digit(text[i])) return false;
    int32_t e = 0;
    for (; i < n && is_digit(text[i]); ++i)
      if (e < 100000) e = e * 10 + (text[i] - '0');
    exponent += negative ? -e : e;
  }
  if (i != n) return false;
  double value = static_cast<double>(mantissa);
  if (value == 0) {
    *out = 0;
    return true;
  }
  if (exponent > 400) value = kMax * 2;
  else if (exponent < -400) value = 0;
  else if (exponent < 0) value /= power_of_ten(-exponent);
  else value *= power_of_ten(exponent);
  *out = value;
  return true;
}

bool string_to_number(StringView text, double* out) {
  uint64_t from = 0, to = text.size();
  while (from < to && is_blank(text[from])) ++from;
  while (to > from && is_blank(text[to - 1])) --to;
  text = text.substr(from, to - from);
  if (text.empty()) {
    *out = 0;
    return true;
  }
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X' || text[1] == 'o' || text[1] == 'O' || text[1] == 'b' || text[1] == 'B')) {
    const uint32_t base = (text[1] == 'x' || text[1] == 'X') ? 16 : (text[1] == 'o' || text[1] == 'O') ? 8 : 2;
    double v = 0;
    for (uint64_t i = 2; i < text.size(); ++i) {
      const char c = text[i];
      const int d = is_digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 99;
      if (d >= static_cast<int>(base)) return false;
      v = v * base + d;
    }
    *out = v;
    return true;
  }
  bool negative = false;
  StringView body = text;
  if (body[0] == '-' || body[0] == '+') {
    negative = body[0] == '-';
    body = body.substr(1);
  }
  if (body.equals("Infinity")) {
    *out = negative ? -kMax * 2 : kMax * 2;
    return true;
  }
  double v;
  if (!parse_decimal(body, &v)) return false;
  *out = negative ? -v : v;
  return true;
}

namespace {

// The shortest decimal digits (at most 17) that read back as `v` (> 0): v = 0.DIGITS x 10^*point.
void shortest_digits(double v, char* digits, int* count, int* point) {
  int e = 0;   // v = m x 10^e with 1 <= m < 10
  double m = v;
  while (m >= 1e16) { m /= 1e16; e += 16; }
  while (m >= 10) { m /= 10; ++e; }
  while (m < 1e-15) { m *= 1e16; e -= 16; }
  while (m < 1) { m *= 10; --e; }
  for (int p = 1; p <= 17; ++p) {
    const int shift = e - p + 1;
    const double scaled = shift >= 0 ? v / power_of_ten(shift) : v * power_of_ten(-shift);
    uint64_t r = static_cast<uint64_t>(scaled + 0.5);
    int ee = e;
    uint64_t limit = 1;
    for (int k = 0; k < p; ++k) limit *= 10;
    if (r >= limit) {
      r /= 10;
      ++ee;
    }
    const int back_shift = ee - p + 1;
    const double back = back_shift >= 0 ? static_cast<double>(r) * power_of_ten(back_shift) : static_cast<double>(r) / power_of_ten(-back_shift);
    if (back == v || p == 17) {
      char buffer[24];
      int length = 0;
      uint64_t x = r;
      while (x > 0) {
        buffer[length++] = static_cast<char>('0' + x % 10);
        x /= 10;
      }
      int k = 0;
      for (int i = length - 1; i >= 0; --i) digits[k++] = buffer[i];
      while (k > 1 && digits[k - 1] == '0') --k;
      *count = k;
      *point = ee + 1;
      return;
    }
  }
}

}  // namespace

void number_to_string(double v, String* out) {
  out->clear();
  if (v != v) {
    out->assign("NaN");
    return;
  }
  if (v == 0) {
    out->assign("0");
    return;
  }
  if (v > kMax) {
    out->assign("Infinity");
    return;
  }
  if (v < -kMax) {
    out->assign("-Infinity");
    return;
  }
  if (v < 0) {
    out->push_back('-');
    v = -v;
  }
  if (v < 9007199254740992.0 && v == static_cast<double>(static_cast<uint64_t>(v))) {   // a whole number below 2^53: its digits
    char buffer[24];
    int length = 0;
    uint64_t x = static_cast<uint64_t>(v);
    while (x > 0) {
      buffer[length++] = static_cast<char>('0' + x % 10);
      x /= 10;
    }
    for (int i = length - 1; i >= 0; --i) out->push_back(buffer[i]);
    return;
  }
  char digits[24];
  int k, n;
  shortest_digits(v, digits, &k, &n);
  if (k <= n && n <= 21) {
    out->append(StringView(digits, static_cast<uint64_t>(k)));
    for (int i = k; i < n; ++i) out->push_back('0');
  } else if (0 < n && n <= 21) {
    out->append(StringView(digits, static_cast<uint64_t>(n)));
    out->push_back('.');
    out->append(StringView(digits + n, static_cast<uint64_t>(k - n)));
  } else if (-6 < n && n <= 0) {
    out->append("0.");
    for (int i = 0; i < -n; ++i) out->push_back('0');
    out->append(StringView(digits, static_cast<uint64_t>(k)));
  } else {
    out->push_back(digits[0]);
    if (k > 1) {
      out->push_back('.');
      out->append(StringView(digits + 1, static_cast<uint64_t>(k - 1)));
    }
    out->push_back('e');
    const int e = n - 1;
    out->push_back(e < 0 ? '-' : '+');
    out->append_uint(static_cast<uint64_t>(e < 0 ? -e : e));
  }
}

void number_to_radix(double v, int radix, String* out) {
  if (radix == 10 || radix < 2 || radix > 36 || v != v || v > kMax || v < -kMax) {
    number_to_string(v, out);
    return;
  }
  out->clear();
  if (v < 0) {
    out->push_back('-');
    v = -v;
  }
  double integer = v < 1.8e19 ? static_cast<double>(static_cast<uint64_t>(v)) : v;
  double fraction = v - integer;
  char buffer[80];
  int length = 0;
  if (integer < 1.8e19) {
    uint64_t x = static_cast<uint64_t>(integer);
    if (x == 0) buffer[length++] = '0';
    while (x > 0 && length < 70) {
      const int d = static_cast<int>(x % static_cast<uint64_t>(radix));
      buffer[length++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
      x /= static_cast<uint64_t>(radix);
    }
  } else {
    buffer[length++] = '0';
  }
  for (int i = length - 1; i >= 0; --i) out->push_back(buffer[i]);
  if (fraction > 0) {
    out->push_back('.');
    for (int i = 0; i < 20 && fraction > 0; ++i) {
      fraction *= radix;
      const int d = static_cast<int>(fraction);
      out->push_back(static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10));
      fraction -= d;
    }
  }
}

void number_to_fixed(double v, int digits, String* out) {
  if (digits < 0) digits = 0;
  if (digits > 100) digits = 100;
  if (v != v || v >= 1e21 || v <= -1e21) {
    number_to_string(v, out);
    return;
  }
  const bool negative = v < 0;
  if (negative) v = -v;
  const double scaled = v * power_of_ten(digits);
  if (scaled >= 1.8e19) {
    number_to_string(negative ? -v : v, out);
    return;
  }
  uint64_t r = static_cast<uint64_t>(scaled + 0.5);
  char buffer[40];
  int length = 0;
  while (r > 0) {
    buffer[length++] = static_cast<char>('0' + r % 10);
    r /= 10;
  }
  while (length < digits + 1) buffer[length++] = '0';
  out->clear();
  if (negative) out->push_back('-');
  for (int i = length - 1; i >= 0; --i) {
    out->push_back(buffer[i]);
    if (i == digits && digits > 0) out->push_back('.');
  }
}

}  // namespace script
