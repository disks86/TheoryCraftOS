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

#include "Database/SqlText.hpp"

namespace database {

namespace {
bool is_word_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<unsigned char>(c) >= 0x80; }
bool is_word(char c) { return is_word_start(c) || (c >= '0' && c <= '9'); }
}  // namespace

bool translate_placeholders(StringView sql, PlaceholderStyle style, String* out, uint32_t* count) {
  out->clear();
  *count = 0;
  const uint64_t n = sql.size();
  uint64_t i = 0;
  auto copy = [&](uint64_t from, uint64_t to) { return out->append(sql.substr(from, to - from)); };
  while (i < n) {
    const char c = sql[i];
    if (c == '\'') {   // a string; E'...' also has backslash escapes
      const bool escapes = i > 0 && (sql[i - 1] == 'E' || sql[i - 1] == 'e') && (i < 2 || !is_word(sql[i - 2]));
      uint64_t j = i + 1;
      while (j < n) {
        if (escapes && sql[j] == '\\' && j + 1 < n) {
          j += 2;
          continue;
        }
        if (sql[j] == '\'') {
          if (j + 1 < n && sql[j + 1] == '\'') {
            j += 2;
            continue;
          }
          ++j;
          break;
        }
        ++j;
      }
      if (!copy(i, j)) return false;
      i = j;
    } else if (c == '"') {
      uint64_t j = i + 1;
      while (j < n) {
        if (sql[j] == '"') {
          if (j + 1 < n && sql[j + 1] == '"') {
            j += 2;
            continue;
          }
          ++j;
          break;
        }
        ++j;
      }
      if (!copy(i, j)) return false;
      i = j;
    } else if (c == '-' && i + 1 < n && sql[i + 1] == '-') {   // a comment to the end of the line
      uint64_t j = i;
      while (j < n && sql[j] != '\n') ++j;
      if (!copy(i, j)) return false;
      i = j;
    } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {   // a comment that may hold comments
      uint64_t j = i + 2;
      uint32_t depth = 1;
      while (j < n && depth) {
        if (sql[j] == '/' && j + 1 < n && sql[j + 1] == '*') {
          ++depth;
          j += 2;
        } else if (sql[j] == '*' && j + 1 < n && sql[j + 1] == '/') {
          --depth;
          j += 2;
        } else {
          ++j;
        }
      }
      if (!copy(i, j)) return false;
      i = j;
    } else if (c == '$' && (i == 0 || !is_word(sql[i - 1]))) {   // $tag$ ... $tag$ (a $ and a digit is a numbered marker)
      uint64_t j = i + 1;
      while (j < n && is_word(sql[j]) && !(j == i + 1 && sql[j] >= '0' && sql[j] <= '9')) ++j;
      if (j < n && sql[j] == '$') {
        const StringView tag = sql.substr(i, j + 1 - i);   // including both dollars
        uint64_t end = sql.find(tag, j + 1);
        end = end == StringView::npos ? n : end + tag.size();
        if (!copy(i, end)) return false;
        i = end;
      } else {
        if (!out->push_back(c)) return false;
        ++i;
      }
    } else if (c == '?') {
      if (i + 1 < n && sql[i + 1] == '?') {   // ?? is one question mark
        if (!out->push_back('?')) return false;
        i += 2;
        continue;
      }
      ++*count;
      if (style == PlaceholderStyle::Numbered) {
        if (!out->push_back('$') || !out->append_uint(*count)) return false;
      } else if (!out->push_back('?')) {
        return false;
      }
      ++i;
    } else {
      if (!out->push_back(c)) return false;
      ++i;
    }
  }
  return true;
}

}  // namespace database
