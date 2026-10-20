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

#include "Database/NumberText.hpp"

namespace database {

namespace {
bool is_digit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool same_word(StringView text, const char* word) {
  uint64_t i = 0;
  for (; word[i]; ++i)
    if (i >= text.size() || lower(text[i]) != word[i]) return false;
  return i == text.size();
}

double power_of_ten(int32_t n) {
  static const double kTable[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  const bool negative = n < 0;
  uint32_t k = static_cast<uint32_t>(negative ? -n : n);
  double result = 1;
  while (k > 22) {
    result *= 1e22;
    k -= 22;
    if (result > 1e300) break;
  }
  result *= kTable[k > 22 ? 22 : k];
  return negative ? 1.0 / result : result;
}

}  // namespace

bool parse_int64(StringView text, int64_t* out) {
  uint64_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
    negative = text[i] == '-';
    ++i;
  }
  if (i >= text.size()) return false;
  uint64_t value = 0;
  for (; i < text.size(); ++i) {
    if (!is_digit(text[i])) return false;
    const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
    if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10) return false;
    value = value * 10 + digit;
  }
  if (negative ? value > 0x8000000000000000ull : value > 0x7FFFFFFFFFFFFFFFull) return false;
  *out = negative ? static_cast<int64_t>(0 - value) : static_cast<int64_t>(value);
  return true;
}

bool parse_double(StringView text, double* out) {
  uint64_t n = text.size();
  uint64_t i = 0;
  while (i < n && text[i] == ' ') ++i;
  while (n > i && text[n - 1] == ' ') --n;
  text = text.substr(i, n - i);
  if (text.empty()) return false;
  if (same_word(text, "nan")) {
    *out = __builtin_nan("");
    return true;
  }
  if (same_word(text, "infinity") || same_word(text, "inf") || same_word(text, "+infinity")) {
    *out = __builtin_inf();
    return true;
  }
  if (same_word(text, "-infinity") || same_word(text, "-inf")) {
    *out = -__builtin_inf();
    return true;
  }
  i = 0;
  n = text.size();
  bool negative = false;
  if (text[i] == '-' || text[i] == '+') {
    negative = text[i] == '-';
    ++i;
  }
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
    bool exponent_negative = false;
    if (i < n && (text[i] == '-' || text[i] == '+')) {
      exponent_negative = text[i] == '-';
      ++i;
    }
    if (i >= n || !is_digit(text[i])) return false;
    int32_t e = 0;
    for (; i < n && is_digit(text[i]); ++i)
      if (e < 10000) e = e * 10 + (text[i] - '0');
    exponent += exponent_negative ? -e : e;
  }
  if (i != n) return false;
  double value = static_cast<double>(mantissa);
  if (value != 0) {
    if (exponent > 308) value = __builtin_inf();
    else if (exponent < -330) value = 0;
    else if (exponent < 0 && exponent >= -22) value /= power_of_ten(-exponent);
    else if (exponent != 0) value *= power_of_ten(exponent);
  }
  *out = negative ? -value : value;
  return true;
}

bool append_double(String* out, double v) {
  if (v != v) return out->append("NaN");
  if (v > 1.7976931348623157e308) return out->append("Infinity");
  if (v < -1.7976931348623157e308) return out->append("-Infinity");
  if (v == 0) return out->append("0");
  bool ok = true;
  if (v < 0) {
    ok = out->push_back('-');
    v = -v;
  }
  int32_t e = 0;   // v = m * 10^e with 1 <= m < 10
  double m = v;
  while (m >= 1e16) { m /= 1e16; e += 16; }
  while (m >= 10) { m /= 10; ++e; }
  while (m < 1e-15) { m *= 1e16; e -= 16; }
  while (m < 1) { m *= 10; --e; }
  uint64_t d = static_cast<uint64_t>(m * 1e16 + 0.5);   // 17 significant digits
  if (d >= 100000000000000000ull) {
    d /= 10;
    ++e;
  }
  char digits[17];
  for (int i = 16; i >= 0; --i) {
    digits[i] = static_cast<char>('0' + d % 10);
    d /= 10;
  }
  int32_t count = 17;
  while (count > 1 && digits[count - 1] == '0') --count;
  // Plain notation for moderate exponents, else d.dddE+x.
  if (e >= 21 || e < -6) {
    ok = ok && out->push_back(digits[0]);
    if (count > 1) ok = ok && out->push_back('.') && out->append(StringView(digits + 1, static_cast<uint64_t>(count - 1)));
    ok = ok && out->push_back('E') && out->push_back(e < 0 ? '-' : '+') && out->append_uint(static_cast<uint64_t>(e < 0 ? -e : e));
    return ok;
  }
  if (e >= 0) {
    for (int32_t i = 0; i <= e && ok; ++i) ok = out->push_back(i < count ? digits[i] : '0');
    if (count > e + 1) ok = ok && out->push_back('.') && out->append(StringView(digits + e + 1, static_cast<uint64_t>(count - e - 1)));
  } else {
    ok = ok && out->append("0.");
    for (int32_t i = 0; i < -e - 1 && ok; ++i) ok = out->push_back('0');
    ok = ok && out->append(StringView(digits, static_cast<uint64_t>(count)));
  }
  return ok;
}

}  // namespace database
