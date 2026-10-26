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

/* printf's engine, written once for narrow and wide formats (included twice by msvcrt.c with CH and FORMAT_NAME defined).
 * `put` receives each output character; the engine returns the number of characters it produced (or -1 for a bad format). */

#ifndef FORMAT_NAME
#error "FORMAT_NAME and CH must be defined"
#endif

#define IS_WIDE_FORMAT (sizeof(CH) == 2)

typedef struct {
  void (*put)(void *context, CH c);
  void *context;
  long long count;
} CONCAT(Sink, CH_TAG);

static void CONCAT(emit, CH_TAG)(CONCAT(Sink, CH_TAG) *s, CH c) { s->put(s->context, c); ++s->count; }
static void CONCAT(pad, CH_TAG)(CONCAT(Sink, CH_TAG) *s, CH c, int n) { while (n-- > 0) CONCAT(emit, CH_TAG)(s, c); }

/* One character of the other width as output code units of this width: narrow text is UTF-8, wide text UTF-16. */
static int CONCAT(emit_wide_char, CH_TAG)(CONCAT(Sink, CH_TAG) *s, unsigned cp) {
  int n = 0;
  if (IS_WIDE_FORMAT) {
    if (cp >= 0x10000) { CONCAT(emit, CH_TAG)(s, (CH)(0xD800 + ((cp - 0x10000) >> 10))); CONCAT(emit, CH_TAG)(s, (CH)(0xDC00 + ((cp - 0x10000) & 0x3FF))); n = 2; }
    else { CONCAT(emit, CH_TAG)(s, (CH)cp); n = 1; }
  } else if (cp < 0x80) { CONCAT(emit, CH_TAG)(s, (CH)cp); n = 1; }
  else if (cp < 0x800) { CONCAT(emit, CH_TAG)(s, (CH)(0xC0 | (cp >> 6))); CONCAT(emit, CH_TAG)(s, (CH)(0x80 | (cp & 0x3F))); n = 2; }
  else if (cp < 0x10000) { CONCAT(emit, CH_TAG)(s, (CH)(0xE0 | (cp >> 12))); CONCAT(emit, CH_TAG)(s, (CH)(0x80 | ((cp >> 6) & 0x3F))); CONCAT(emit, CH_TAG)(s, (CH)(0x80 | (cp & 0x3F))); n = 3; }
  else { CONCAT(emit, CH_TAG)(s, (CH)(0xF0 | (cp >> 18))); CONCAT(emit, CH_TAG)(s, (CH)(0x80 | ((cp >> 12) & 0x3F))); CONCAT(emit, CH_TAG)(s, (CH)(0x80 | ((cp >> 6) & 0x3F))); CONCAT(emit, CH_TAG)(s, (CH)(0x80 | (cp & 0x3F))); n = 4; }
  return n;
}

static int FORMAT_NAME(void (*put)(void *, CH), void *context, const CH *format, va_list_ args) {
  CONCAT(Sink, CH_TAG) sink = {put, context, 0};
  CONCAT(Sink, CH_TAG) *s = &sink;
  for (const CH *f = format; *f; ++f) {
    if (*f != '%') { CONCAT(emit, CH_TAG)(s, *f); continue; }
    ++f;
    int flags = 0, width = 0, precision = -1, size = 0;
    for (;; ++f) {
      if (*f == '-') flags |= FMT_LEFT; else if (*f == '+') flags |= FMT_PLUS; else if (*f == ' ') flags |= FMT_SPACE; else if (*f == '#') flags |= FMT_ALT; else if (*f == '0') flags |= FMT_ZERO; else break;
    }
    if (*f == '*') { width = va_arg_(args, int); if (width < 0) { flags |= FMT_LEFT; width = -width; } ++f; }
    else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    if (*f == '.') {
      ++f; precision = 0;
      if (*f == '*') { precision = va_arg_(args, int); ++f; }
      else while (*f >= '0' && *f <= '9') precision = precision * 10 + (*f++ - '0');
    }
    /* size: 'h' 1, 'H' hh 2, 'l' 4 (a long is 32 bits), 'q' ll/I64 8, 'z' 8 (size_t), 'w' wide string, 'L' long double */
    for (;; ++f) {
      if (*f == 'h') size = size == 1 ? 2 : 1;
      else if (*f == 'l') size = size == 4 ? 8 : 4;
      else if (*f == 'L') size = 16;
      else if (*f == 'z' || *f == 'j' || *f == 't') size = 8;
      else if (*f == 'w') size = 5;
      else if (*f == 'I' && f[1] == '6' && f[2] == '4') { size = 8; f += 2; }
      else if (*f == 'I' && f[1] == '3' && f[2] == '2') { size = 0; f += 2; }
      else if (*f == 'I') size = 8;
      else break;
    }
    const CH conv = *f;
    if (!conv) break;
    char text[400];
    int length = 0;
    char sign = 0;
    const char *prefix = "";
    int prefix_length = 0;
    switch (conv) {
      case '%': CONCAT(emit, CH_TAG)(s, '%'); continue;
      case 'n': { int *p = va_arg_(args, int *); if (p) *p = (int)s->count; continue; }
      case 'c': case 'C': {
        const int arg_is_wide = size == 4 || size == 5 ? 1 : size == 1 ? 0 : (conv == 'C') ? !IS_WIDE_FORMAT : IS_WIDE_FORMAT;
        unsigned cp = (unsigned)va_arg_(args, int);
        if (!arg_is_wide) cp &= 0xFF;
        const int pad = width - 1;
        if (!(flags & FMT_LEFT)) CONCAT(pad, CH_TAG)(s, ' ', pad);
        if (!arg_is_wide && !IS_WIDE_FORMAT) CONCAT(emit, CH_TAG)(s, (CH)cp); else CONCAT(emit_wide_char, CH_TAG)(s, cp);
        if (flags & FMT_LEFT) CONCAT(pad, CH_TAG)(s, ' ', pad);
        continue;
      }
      case 's': case 'S': {
        const int arg_is_wide = size == 4 || size == 5 ? 1 : size == 1 ? 0 : (conv == 'S') ? !IS_WIDE_FORMAT : IS_WIDE_FORMAT;
        const void *arg = va_arg_(args, const void *);
        if (!arg) arg = arg_is_wide ? (const void *)L"(null)" : (const void *)"(null)";
        /* count the characters first (for padding), then write them */
        int units = 0;
        if (arg_is_wide) { const wchar_t *w = arg; while (w[units] && (precision < 0 || units < precision)) ++units; }
        else { const char *n = arg; while (n[units] && (precision < 0 || units < precision)) ++units; }
        const int pad = width - units;
        if (!(flags & FMT_LEFT)) CONCAT(pad, CH_TAG)(s, (flags & FMT_ZERO) ? '0' : ' ', pad);
        if (arg_is_wide == IS_WIDE_FORMAT) {
          for (int i = 0; i < units; ++i) CONCAT(emit, CH_TAG)(s, ((const CH *)arg)[i]);
        } else if (arg_is_wide) {      /* wide argument, narrow output: UTF-16 to UTF-8 */
          const wchar_t *w = arg;
          for (int i = 0; i < units; ++i) {
            unsigned cp = w[i];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < units && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00); ++i; }
            CONCAT(emit_wide_char, CH_TAG)(s, cp);
          }
        } else {                       /* narrow argument, wide output: UTF-8 to UTF-16 */
          const unsigned char *n = arg;
          for (int i = 0; i < units;) {
            unsigned cp = n[i], extra = 0;
            if (cp >= 0xF0) { cp &= 7; extra = 3; } else if (cp >= 0xE0) { cp &= 15; extra = 2; } else if (cp >= 0xC0) { cp &= 31; extra = 1; }
            ++i;
            while (extra-- && i < units && (n[i] & 0xC0) == 0x80) cp = (cp << 6) | (n[i++] & 0x3F);
            CONCAT(emit_wide_char, CH_TAG)(s, cp);
          }
        }
        if (flags & FMT_LEFT) CONCAT(pad, CH_TAG)(s, ' ', pad);
        continue;
      }
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
        unsigned long long value;
        int is_signed = conv == 'd' || conv == 'i';
        unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
        const char *digits = conv == 'X' || conv == 'p' ? "0123456789ABCDEF" : "0123456789abcdef";
        if (conv == 'p') { value = (unsigned long long)va_arg_(args, void *); is_signed = 0; if (precision < 0) precision = 16; }
        else if (is_signed) {
          long long v = size == 8 ? va_arg_(args, long long) : va_arg_(args, int);
          if (size == 1) v = (short)v; else if (size == 2) v = (signed char)v;
          if (v < 0) { sign = '-'; value = 0 - (unsigned long long)v; } else { value = (unsigned long long)v; if (flags & FMT_PLUS) sign = '+'; else if (flags & FMT_SPACE) sign = ' '; }
        } else {
          value = size == 8 ? va_arg_(args, unsigned long long) : va_arg_(args, unsigned int);
          if (size == 1) value &= 0xFFFF; else if (size == 2) value &= 0xFF;
        }
        char reversed[70];
        int n = 0;
        if (value == 0 && precision == 0) n = 0;
        else do { reversed[n++] = digits[value % base]; value /= base; } while (value);
        while (precision >= 0 && n < precision && n < 69) reversed[n++] = '0';
        if ((flags & FMT_ALT) && base == 8 && (n == 0 || reversed[n - 1] != '0')) reversed[n++] = '0';
        if ((flags & FMT_ALT) && base == 16 && conv != 'p' && n > 0 && !(n == 1 && reversed[0] == '0')) { prefix = conv == 'X' ? "0X" : "0x"; prefix_length = 2; }
        for (int i = 0; i < n; ++i) text[i] = reversed[n - 1 - i];
        length = n;
        if (precision >= 0 && conv != 'p') flags &= ~FMT_ZERO;
        break;
      }
      case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
        long double v = size == 16 ? va_arg_(args, long double) : (long double)va_arg_(args, double);
        const int upper = conv == 'E' || conv == 'F' || conv == 'G' || conv == 'A';
        char lower = (char)(conv | 32);
        if (lower == 'a') lower = 'e';
        length = format_float(text, v, lower, precision, flags, upper);
        if (text[0] == '-' || text[0] == '+' || text[0] == ' ') { sign = text[0]; for (int i = 1; i < length; ++i) text[i - 1] = text[i]; --length; }
        if (ld_isnan(v) || ld_isinf(v)) flags &= ~FMT_ZERO;
        precision = -1;
        break;
      }
      default: CONCAT(emit, CH_TAG)(s, '%'); CONCAT(emit, CH_TAG)(s, conv); continue;
    }
    const int pad = width - length - (sign ? 1 : 0) - prefix_length;
    if (!(flags & FMT_LEFT) && !(flags & FMT_ZERO)) CONCAT(pad, CH_TAG)(s, ' ', pad);
    if (sign) CONCAT(emit, CH_TAG)(s, (CH)sign);
    for (int i = 0; i < prefix_length; ++i) CONCAT(emit, CH_TAG)(s, (CH)prefix[i]);
    if (!(flags & FMT_LEFT) && (flags & FMT_ZERO)) CONCAT(pad, CH_TAG)(s, '0', pad);
    for (int i = 0; i < length; ++i) CONCAT(emit, CH_TAG)(s, (CH)text[i]);
    if (flags & FMT_LEFT) CONCAT(pad, CH_TAG)(s, ' ', pad);
  }
  return (int)sink.count;
}
