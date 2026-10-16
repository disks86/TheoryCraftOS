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
