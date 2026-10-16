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

// Symbols the compiler may emit calls to even in freestanding code.
#include <stddef.h>

extern "C" {
void* memset(void* dst, int c, size_t n) {
  auto* p = static_cast<unsigned char*>(dst);
  while (n--) *p++ = static_cast<unsigned char>(c);
  return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
  auto* d = static_cast<unsigned char*>(dst);
  auto* s = static_cast<const unsigned char*>(src);
  while (n--) *d++ = *s++;
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  auto* d = static_cast<unsigned char*>(dst);
  auto* s = static_cast<const unsigned char*>(src);
  if (d < s) { while (n--) *d++ = *s++; }
  else       { d += n; s += n; while (n--) *--d = *--s; }
  return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
  auto* x = static_cast<const unsigned char*>(a);
  auto* y = static_cast<const unsigned char*>(b);
  for (; n; --n, ++x, ++y)
    if (*x != *y) return *x - *y;
  return 0;
}
}
