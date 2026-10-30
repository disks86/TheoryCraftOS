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
/* ntdll.dll: the printf formatter, included twice by ntdll.c: for a narrow result (FMT_CH char) and a wide one (FMT_CH WCHAR). */

typedef struct { FMT_CH *out; size_t_ capacity, length; int raw; } FMT_SINK;      /* (raw: the msvcrt _snprintf, which fills the whole buffer and keeps no room for the NUL) */
static void FMT_PUT(FMT_SINK *s, FMT_CH c) {
  if (s->length + (s->raw ? 0 : 1) < s->capacity) s->out[s->length] = c;
  ++s->length;
}
static void FMT_PAD(FMT_SINK *s, FMT_CH c, int n) { while (n-- > 0) FMT_PUT(s, c); }

static int FMT_FN(FMT_CH *buffer, size_t_ capacity, int raw, const FMT_CH *format, va_list_ args) {
  if (!buffer) capacity = 0;      /* (measuring only) */
  FMT_SINK sink = {buffer, capacity, 0, raw};
  for (const FMT_CH *f = format; *f; ++f) {
    if (*f != '%') { FMT_PUT(&sink, *f); continue; }
    ++f;
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, precision = -1, size = 0;
    for (;; ++f) {
      if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else break;
    }
    if (*f == '*') { width = __builtin_va_arg(args, int); if (width < 0) { left = 1; width = -width; } ++f; }
    else while ((*f >= '0' && *f <= '9')) width = width * 10 + (*f++ - '0');
    if (*f == '.') {
      ++f; precision = 0;
      if (*f == '*') { precision = __builtin_va_arg(args, int); ++f; }
      else while ((*f >= '0' && *f <= '9')) precision = precision * 10 + (*f++ - '0');
    }
    int has_h = 0, has_w = 0, ells = 0;
    for (;; ++f) {      /* size: 0 = 32 bit, 1 = 16 bit, 8 = 64 bit; a 64 bit size stays, "h" otherwise gives 16 bit */
      if (*f == 'h') { has_h = 1; if (size != 8) size = 1; }
      else if (*f == 'l') { ++ells; if (f[1] == 'l') { size = 8; ++ells; ++f; } }
      else if (*f == 'w') has_w = 1;
      else if (*f == 'z' || *f == 'j' || *f == 't') size = 8;
      else if (*f == 'I' && f[1] == '6' && f[2] == '4') { size = 8; f += 2; }
      else if (*f == 'I' && f[1] == '3' && f[2] == '2') { size = 0; f += 2; }
      else if (*f == 'I') size = 8;
      else if (*f == 'L') continue;      /* (a long double prefix changes nothing here) */
      else break;
    }
    const FMT_CH conversion = *f;
    if (!conversion) break;
    char text[80];
    const char *body = text;
    const WCHAR *wbody = NULL;      /* (a wide string to put out as it is) */
    WCHAR wc = 0;
    int length = 0;
    char sign = 0;
    const char *digits = "0123456789abcdef";
    switch (conversion) {
      case '%': FMT_PUT(&sink, '%'); continue;
      case 'c': case 'C': {
        const int wide_arg = fmt_wide_arg(FMT_WIDE, conversion == 'c', has_h, has_w, ells);
        const int v = __builtin_va_arg(args, int);
        if (wide_arg && FMT_WIDE) { wc = (WCHAR)v; wbody = &wc; }
        else text[0] = wide_arg && (WCHAR)v >= 0x100 ? '?' : (char)v;
        length = 1;
        break;
      }
      case 's': case 'S': {
        const int wide_arg = fmt_wide_arg(FMT_WIDE, conversion == 's', has_h, has_w, ells);
        if (wide_arg) {      /* a wide string: put out as it is in a wide result, narrowed in a narrow one */
          const WCHAR *w = __builtin_va_arg(args, const WCHAR *);
          if (!w) { body = "(null)"; while (body[length]) ++length; break; }
          if (FMT_WIDE) { wbody = w; while (w[length] && (precision < 0 || length < precision)) ++length; break; }
          while (w[length] && length < 79 && (precision < 0 || length < precision)) { text[length] = w[length] < 0x100 ? (char)w[length] : '?'; ++length; }
          break;
        }
        body = __builtin_va_arg(args, const char *);
        if (!body) body = "(null)";
        while (body[length] && (precision < 0 || length < precision)) ++length;
        break;
      }
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
        unsigned long long value;
        int is_signed = conversion == 'd' || conversion == 'i';
        unsigned base = conversion == 'o' ? 8 : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16 : 10;
        if (conversion == 'X') digits = "0123456789ABCDEF";
        if (conversion == 'p') { value = (unsigned long long)__builtin_va_arg(args, void *); is_signed = 0; if (precision < 0) precision = 16; digits = "0123456789ABCDEF"; }
        else if (is_signed) {
          long long v = size == 8 ? __builtin_va_arg(args, long long) : __builtin_va_arg(args, int);
          if (size == 1) v = (short)v; else if (size == 2) v = (signed char)v;
          if (v < 0) { sign = '-'; value = (unsigned long long)-v; } else { value = (unsigned long long)v; if (plus) sign = '+'; else if (space) sign = ' '; }
        } else {
          value = size == 8 ? __builtin_va_arg(args, unsigned long long) : __builtin_va_arg(args, unsigned int);
          if (size == 1) value &= 0xFFFF; else if (size == 2) value &= 0xFF;
        }
        char reversed[72];
        int n = 0;
        do { reversed[n++] = digits[value % base]; value /= base; } while (value);
        while (precision >= 0 && n < precision && n < 70) reversed[n++] = '0';
        if (alt && base == 16 && conversion != 'p' && (n > 1 || reversed[0] != '0')) { reversed[n++] = conversion; reversed[n++] = '0'; }
        for (int i = 0; i < n; ++i) text[i] = reversed[n - 1 - i];
        length = n;
        if (precision >= 0 && conversion != 'p') zero = 0;
        break;
      }
      default: FMT_PUT(&sink, conversion); continue;      /* (a conversion that is not known puts out the character alone) */
    }
    int pad = width - length - (sign ? 1 : 0);
    if (!left && !zero) FMT_PAD(&sink, ' ', pad);
    if (sign) FMT_PUT(&sink, sign);
    if (!left && zero && conversion != 's' && conversion != 'c') FMT_PAD(&sink, '0', pad);
    for (int i = 0; i < length; ++i) FMT_PUT(&sink, wbody ? wbody[i] : (FMT_CH)(BYTE)body[i]);
    if (left) FMT_PAD(&sink, ' ', pad);
  }
  if (raw) {
    if (sink.length < capacity) sink.out[sink.length] = 0;
    return !buffer || sink.length <= capacity ? (int)sink.length : -1;
  }
  if (capacity) sink.out[sink.length < capacity ? sink.length : capacity - 1] = 0;
  return (int)sink.length;
}
