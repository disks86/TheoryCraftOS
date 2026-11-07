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
#include <stdint.h>

// The memory functions every copy of the kernel goes through (ArrayView::copy_from, the user-memory copies, frame and buffer uploads): a byte at a time is far too slow. On x86-64
// they are the string instructions (fast strings make rep movsb / stosb as quick as wide moves); elsewhere 8-byte words where the two addresses line up, bytes otherwise. (The
// kernel is built with -O1, which does not turn these loops back into calls to themselves.)
namespace {
constexpr uintptr_t kWord = sizeof(uint64_t);
uintptr_t address_of(const void* p) { return reinterpret_cast<uintptr_t>(p); }
bool aligned_together(const void* a, const void* b) { return ((address_of(a) ^ address_of(b)) & (kWord - 1)) == 0; }
bool word_aligned(const void* p) { return (address_of(p) & (kWord - 1)) == 0; }
// A word of memory read or written as such: the callers pass only addresses aligned to a word (word_aligned), and the type may alias any other (the bytes belong to some other object).
using AliasWord = uint64_t __attribute__((__may_alias__));
uint64_t load_word(const unsigned char* p) { return *reinterpret_cast<const AliasWord*>(p); }
void store_word(unsigned char* p, uint64_t v) { *reinterpret_cast<AliasWord*>(p) = v; }
}  // namespace

extern "C" {
void* memset(void* dst, int c, size_t n) {
#if defined(__x86_64__)
  void* p = dst;
  __asm__ volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(c) : "memory");
#else
  auto* p = static_cast<unsigned char*>(dst);
  const unsigned char byte = static_cast<unsigned char>(c);
  for (; n && !word_aligned(p); --n) *p++ = byte;
  const uint64_t word = 0x0101010101010101ull * byte;
  for (; n >= kWord; n -= kWord, p += kWord) store_word(p, word);
  while (n--) *p++ = byte;
#endif
  return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
#if defined(__x86_64__)
  void* d = dst;
  __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
#else
  auto* d = static_cast<unsigned char*>(dst);
  auto* s = static_cast<const unsigned char*>(src);
  if (aligned_together(d, s)) {
    for (; n && !word_aligned(d); --n) *d++ = *s++;
    for (; n >= kWord; n -= kWord, d += kWord, s += kWord) store_word(d, load_word(s));   // (both aligned to a word here)
  }
  while (n--) *d++ = *s++;
#endif
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  auto* d = static_cast<unsigned char*>(dst);
  auto* s = static_cast<const unsigned char*>(src);
  if (d <= s || d >= s + n) return memcpy(dst, src, n);   // (forwards reads each byte before it is overwritten)
  d += n;
  s += n;
  if (aligned_together(d, s)) {
    for (; n && !word_aligned(d); --n) *--d = *--s;
    for (; n >= kWord; n -= kWord) {
      d -= kWord, s -= kWord;
      store_word(d, load_word(s));
    }
  }
  while (n--) *--d = *--s;
  return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
  auto* x = static_cast<const unsigned char*>(a);
  auto* y = static_cast<const unsigned char*>(b);
  if (aligned_together(x, y)) {
    for (; n && !word_aligned(x); --n, ++x, ++y)
      if (*x != *y) return *x - *y;
    for (; n >= kWord && load_word(x) == load_word(y); n -= kWord, x += kWord, y += kWord) {}
  }
  for (; n; --n, ++x, ++y)
    if (*x != *y) return *x - *y;
  return 0;
}

size_t strlen(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}
}

// Globals with destructors (the win32k modules' state pointers) register them with atexit; the kernel never exits, so there is nothing to run.
extern "C" int atexit(void (*)(void)) { return 0; }
