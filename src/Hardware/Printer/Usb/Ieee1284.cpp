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

#include "Hardware/Printer/Usb/Ieee1284.hpp"

namespace hardware {

namespace {

bool equals_ignore_case(const char* a, uint64_t alen, const char* b) {
  uint64_t i = 0;
  for (; i < alen && b[i]; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
    if (x != y) return false;
  }
  return i == alen && !b[i];
}

void copy_value(char* dst, uint32_t capacity, const char* v, uint64_t n) {
  uint64_t k = 0;
  for (; k < n && k + 1 < capacity; ++k) dst[k] = v[k];
  dst[k] = 0;
}

// Does the comma-separated list hold `word` (ignoring case, and a trailing revision such as "PCL5E")?
bool list_has(const char* v, uint64_t n, const char* word, bool prefix) {
  uint64_t start = 0;
  for (uint64_t i = 0; i <= n; ++i) {
    if (i == n || v[i] == ',') {
      uint64_t len = i - start;
      const char* item = v + start;
      uint64_t wl = 0;
      while (word[wl]) ++wl;
      if (prefix ? (len >= wl && equals_ignore_case(item, wl, word)) : equals_ignore_case(item, len, word)) return true;
      start = i + 1;
    }
  }
  return false;
}

}  // namespace

PrinterIdentity parse_device_id(ConstByteView id) {
  PrinterIdentity out;
  const char* d = reinterpret_cast<const char*>(id.data());
  const uint64_t n = id.size();
  uint64_t i = 0;
  while (i < n) {
    uint64_t key_start = i;
    while (i < n && d[i] != ':' && d[i] != ';') ++i;
    if (i >= n || d[i] != ':') {  // a stray separator or a key without a value
      ++i;
      continue;
    }
    uint64_t key_len = i - key_start;
    ++i;
    const uint64_t value_start = i;
    while (i < n && d[i] != ';') ++i;
    const char* v = d + value_start;
    const uint64_t vl = i - value_start;
    ++i;
    while (key_len > 0 && d[key_start] == ' ') {  // blanks after the previous separator are not part of the key
      ++key_start;
      --key_len;
    }
    const char* k = d + key_start;
    if (equals_ignore_case(k, key_len, "MFG") || equals_ignore_case(k, key_len, "MANUFACTURER")) copy_value(out.manufacturer, sizeof out.manufacturer, v, vl);
    else if (equals_ignore_case(k, key_len, "MDL") || equals_ignore_case(k, key_len, "MODEL")) copy_value(out.model, sizeof out.model, v, vl);
    else if (equals_ignore_case(k, key_len, "CMD") || equals_ignore_case(k, key_len, "COMMAND SET")) {
      if (list_has(v, vl, "PCL", true)) out.languages |= static_cast<uint8_t>(1u << static_cast<uint8_t>(PrinterLanguage::Pcl));
      if (list_has(v, vl, "POSTSCRIPT", true) || list_has(v, vl, "PS", false) || list_has(v, vl, "BRSCRIPT", true))
        out.languages |= static_cast<uint8_t>(1u << static_cast<uint8_t>(PrinterLanguage::PostScript));
      out.pjl = list_has(v, vl, "PJL", false);
      out.pcl_xl = list_has(v, vl, "PCLXL", false) || list_has(v, vl, "PCL6", false);
    }
  }
  return out;
}

}  // namespace hardware
