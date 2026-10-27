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
#include "Process/DosWildcard.hpp"

namespace process {

namespace {

constexpr uint32_t kMaxExpression = 520, kMaxName = 300;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

struct Expression {
  char text[kMaxExpression];
  uint32_t length = 0;
  bool ok = false;
};

// Trailing run of dots and spaces: dropped if it ends with a space, else reduced to one dot.
uint32_t normalise(StringView pattern, char* out) {
  uint64_t end = pattern.size();
  while (end > 0 && (pattern[end - 1] == '.' || pattern[end - 1] == ' ')) --end;
  const bool run = end < pattern.size();
  const bool ends_with_space = run && pattern[pattern.size() - 1] == ' ';
  uint32_t n = 0;
  for (uint64_t i = 0; i < end && n < kMaxExpression - 2; ++i) out[n++] = pattern[i];
  if (run && !ends_with_space) out[n++] = '.';
  return n;
}

void convert(const char* in, uint32_t n, Expression* out) {
  uint32_t o = 0;
  for (uint32_t i = 0; i < n && o < kMaxExpression - 2; ++i) {
    const char c = in[i], prev = i > 0 ? in[i - 1] : 0;
    if (c == '?') out->text[o++] = '>';
    else if (c == '*' && i + 1 < n && in[i + 1] == '.') { out->text[o++] = '<'; out->text[o++] = '"'; ++i; }
    else if (c == '.' && (i == n - 1 || prev == '?' || (i + 2 == n && in[i + 1] == '*' && prev != '.'))) out->text[o++] = '"';
    else out->text[o++] = c;
  }
  out->length = o;
  out->ok = true;
}

// Matches `name` against an expression. With `dos` false the DOS wildcards are ordinary characters.
bool match(const char* expr, uint32_t elen, const char* name, uint32_t nlen, bool dos) {
  if (elen > kMaxExpression || nlen > kMaxName) return false;
  int64_t last_dot = -1;
  for (uint32_t i = 0; i < nlen; ++i) if (name[i] == '.') last_dot = i;
  // reach[pi] for the current name position; `until[pi]` is how far a '<' at pi that started earlier may still consume.
  bool cur[kMaxExpression + 2] = {}, next[kMaxExpression + 2] = {};
  int64_t until[kMaxExpression + 2];
  for (uint32_t i = 0; i < elen + 2; ++i) until[i] = -1;
  cur[0] = true;
  for (uint32_t ni = 0; ni <= nlen; ++ni) {
    for (uint32_t i = 0; i < elen + 2; ++i) next[i] = false;
    const bool at_end = ni >= nlen;
    const char c = at_end ? 0 : name[ni];
    for (uint32_t pi = 0; pi < elen; ++pi) {
      const char e = expr[pi];
      if (dos && e == '<' && until[pi] >= static_cast<int64_t>(ni)) cur[pi + 1] = true;      // an earlier '<' reaches here
      if (!cur[pi]) continue;
      if (e == '*') {
        cur[pi + 1] = true;
        if (!at_end) next[pi] = true;
      } else if (dos && e == '<') {
        cur[pi + 1] = true;
        const int64_t reach = last_dot >= static_cast<int64_t>(ni) ? last_dot + 1 : static_cast<int64_t>(nlen);
        if (reach > until[pi]) until[pi] = reach;
      } else if (dos && e == '>') {
        if (!at_end && c != '.') next[pi + 1] = true;
        else {
          uint32_t j = pi;
          while (j < elen && expr[j] == '>') ++j;
          cur[j] = true;
        }
      } else if (dos && e == '"') {
        if (!at_end && c == '.') next[pi + 1] = true;
        if (at_end) cur[pi + 1] = true;
      } else if (e == '?') {
        if (!at_end) next[pi + 1] = true;
      } else if (!at_end && lower(e) == lower(c)) {
        next[pi + 1] = true;
      }
    }
    if (ni == nlen) return cur[elen];
    for (uint32_t i = 0; i < elen + 2; ++i) cur[i] = next[i];
  }
  return false;
}

}  // namespace

uint32_t dos_short_name(StringView name, char* out, uint32_t capacity) {
  if (name.empty() || name.size() > 255 || capacity < 16) return 0;
  if (name.equals(".") || name.equals("..")) return 0;
  auto bad = [](char c) { return c == ' ' || c == '"' || c == '/' || c == '\\' || c == '[' || c == ']' || c == ':' || c == ';' || c == '=' || c == ',' || c == '+' || c == '*' ||
                                 c == '?' || c == '<' || c == '>' || c == '|' || static_cast<uint8_t>(c) >= 0x80; };
  for (uint64_t i = 0; i < name.size(); ++i) if (static_cast<uint8_t>(name[i]) >= 0x80) return 0;      // (no alias is made for names outside ASCII)
  uint64_t first = 0;
  while (first < name.size() && name[first] == '.') ++first;
  const bool leading_dot = first > 0;
  int64_t dot = -1;
  uint32_t dots = 0;
  for (uint64_t i = 0; i < name.size(); ++i) if (name[i] == '.') { ++dots; if (i >= first) dot = static_cast<int64_t>(i); }
  const uint64_t base_end = dot >= 0 ? static_cast<uint64_t>(dot) : name.size();
  const uint64_t base_len = base_end - first, ext_len = dot >= 0 ? name.size() - static_cast<uint64_t>(dot) - 1 : 0;
  bool clean = true;
  for (uint64_t i = 0; i < name.size(); ++i) if (bad(name[i])) clean = false;
  if (dots <= 1 && !leading_dot && base_len >= 1 && base_len <= 8 && ext_len <= 3 && clean) return 0;      // already a valid 8.3 name
  uint32_t n = 0;
  char base[8];
  uint32_t b = 0;
  const uint32_t base_limit = leading_dot ? 2 : 6;
  for (uint64_t i = first; i < base_end && b < base_limit; ++i) if (!bad(name[i]) && name[i] != '.') base[b++] = upper(name[i]);
  for (uint32_t i = 0; i < b; ++i) out[n++] = base[i];
  if (leading_dot) for (uint32_t i = 0; i < 4; ++i) out[n++] = '0';      // (the alias of such a name carries a hash)
  out[n++] = '~';
  out[n++] = '1';
  if (dot >= 0) {
    uint32_t e = 0;
    char ext[3];
    for (uint64_t i = static_cast<uint64_t>(dot) + 1; i < name.size() && e < 3; ++i) if (!bad(name[i]) && name[i] != '.') ext[e++] = upper(name[i]);
    if (e > 0) {
      out[n++] = '.';
      for (uint32_t i = 0; i < e; ++i) out[n++] = ext[i];
    }
  }
  return n;
}

bool dos_wildcard_matches(StringView pattern, StringView name) {
  if (pattern.size() > 255 || name.size() > 255) return false;
  char plain[kMaxExpression];
  const uint32_t plain_length = normalise(pattern, plain);
  Expression expression;
  convert(plain, plain_length, &expression);
  if (name.equals(".") || name.equals("..")) {
    return match(expression.text, expression.length, "", 0, true) || match(expression.text, expression.length, ".", 1, true) || match(plain, plain_length, "..", 2, false);
  }
  char buffer[kMaxName];
  for (uint64_t i = 0; i < name.size(); ++i) buffer[i] = name[i];
  if (match(expression.text, expression.length, buffer, static_cast<uint32_t>(name.size()), true)) return true;
  char alias[16];
  const uint32_t alias_length = dos_short_name(name, alias, sizeof(alias));
  return alias_length != 0 && match(expression.text, expression.length, alias, alias_length, true);
}

}  // namespace process
