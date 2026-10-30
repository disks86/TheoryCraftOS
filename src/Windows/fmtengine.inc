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
/* The message formatting engine shared by kernel32's FormatMessage and ntdll's RtlFormatMessage (see kernel32/fmtmsg.inc for what it does). The includer
   defines FMT_HEAP() as the heap to allocate from. */

#define FMT_ALLOCATE_BUFFER 0x100
#define FMT_IGNORE_INSERTS 0x200
#define FMT_FROM_STRING 0x400
#define FMT_FROM_HMODULE 0x800
#define FMT_FROM_SYSTEM 0x1000
#define FMT_ARGUMENT_ARRAY 0x2000
#define FMT_MAX_WIDTH_MASK 0xFF
#define FMT_SLOTS 200
#define FMT_ANSI_LIMIT 32766      /* (the ANSI version works through a counted string of 64 KiB) */

typedef struct {
  WCHAR *text;
  size_t_ length, capacity;
  int failed;            /* out of memory */
  int overflow;          /* the caller's buffer is full */
  int invalid;           /* a bad template or no arguments for an insert */
  int stop;
  int limited;           /* the caller's buffer holds `size` characters */
  int rtl;               /* RtlFormatMessage: a literal may fill the buffer to its last character, an insert leaves room for the NUL, a line break is whole or not at all */
  int in_insert;
  int overflow_in_insert;
  size_t_ size;
  DWORD width;
  size_t_ line_start;
} FmtText;

typedef struct {
  const ULONG_PTR *array;      /* FORMAT_MESSAGE_ARGUMENT_ARRAY */
  char *list;                  /* the next slot of a variable argument list */
  int have;                    /* there are arguments at all */
  unsigned last;               /* the highest argument number read */
  unsigned maxreq;             /* the highest argument number an insert asked for */
  unsigned fetched;
  ULONG_PTR slots[FMT_SLOTS];
} FmtArgs;

/* Makes room for `more` characters beyond the length (and one for the NUL). */
static int fmt_reserve(FmtText *t, size_t_ more) {
  if (t->failed) return 0;
  if (more > ((size_t_)1 << 28) || t->length > ((size_t_)1 << 28)) { t->failed = 1; t->stop = 1; return 0; }
  if (t->length + more + 1 <= t->capacity) return 1;
  size_t_ bigger = t->capacity ? t->capacity : 128;
  while (bigger < t->length + more + 1) bigger *= 2;
  WCHAR *p = t->text ? RtlReAllocateHeap(FMT_HEAP(), 0, t->text, bigger * 2) : RtlAllocateHeap(FMT_HEAP(), 0, bigger * 2);
  if (!p) { t->failed = 1; t->stop = 1; return 0; }
  t->text = p;
  t->capacity = bigger;
  return 1;
}
static void fmt_put(FmtText *t, WCHAR c) {
  if (t->stop) return;
  if (t->limited) {
    const size_t_ room = t->rtl && !t->in_insert ? t->size : (t->size ? t->size - 1 : 0);
    if (t->length >= room) { t->overflow = 1; t->overflow_in_insert = t->in_insert; t->stop = 1; return; }
  }
  if (!fmt_reserve(t, 1)) return;
  t->text[t->length++] = c;
}
static void fmt_pad(FmtText *t, int count, WCHAR c) { while (count-- > 0 && !t->stop) fmt_put(t, c); }
static void fmt_newline_break(FmtText *t) {
  if (t->limited && t->rtl && !t->stop && t->length + 2 > (t->in_insert ? t->size - 1 : t->size)) { t->overflow = 1; t->stop = 1; return; }
  fmt_put(t, '\r');
  fmt_put(t, '\n');
  t->line_start = t->length;
}

/* The line has reached the width: it breaks at its last blank (the blanks before that one go with it), or, without a blank, where it is. */
static void fmt_wrap(FmtText *t) {
  size_t_ space = t->length;
  while (space > t->line_start && t->text[space - 1] != ' ') --space;
  if (space == t->line_start) { fmt_newline_break(t); return; }
  --space;      /* the blank */
  size_t_ cut = space;
  while (cut > t->line_start && t->text[cut - 1] == ' ') --cut;
  const size_t_ rest = t->length - (space + 1);
  if (!fmt_reserve(t, 2 + rest)) return;
  memmove(t->text + cut + 2, t->text + space + 1, rest * 2);
  t->text[cut] = '\r';
  t->text[cut + 1] = '\n';
  t->length = cut + 2 + rest;
  t->line_start = cut + 2;
  if (t->limited && t->length + 1 > t->size) { t->length = t->size ? t->size - 1 : 0; t->overflow = 1; t->stop = 1; }
}
/* A character of the message, or of an insert: the width is looked after. */
static void fmt_char(FmtText *t, WCHAR c) {
  fmt_put(t, c);
  if (!t->stop && t->width && t->width != FMT_MAX_WIDTH_MASK && t->length - t->line_start >= t->width) fmt_wrap(t);
}

/* Argument number `nr` (0: the one after the highest so far). An array is read where it is; a list is read up to there and the slots are kept. */
static ULONG_PTR fmt_arg(FmtArgs *a, unsigned nr) {
  if (!a->have) return 0;
  if (nr == 0) nr = a->last + 1;
  if (nr > FMT_SLOTS) return 0;
  if (nr > a->last) a->last = nr;
  if (a->array) return a->array[nr - 1];
  while (a->fetched < nr) {
    a->slots[a->fetched++] = *(const ULONG_PTR *)a->list;
    a->list += sizeof(ULONG_PTR);
  }
  return a->slots[nr - 1];
}

/* One insert, %n or %n!spec!: the printf-style specification ("-5d", "s", "*.*s", "I64x"). The arguments it uses start at number n; the width and precision stars come first. */
static void fmt_insert_body(FmtText *out, const WCHAR *spec, size_t_ spec_len, FmtArgs *args, unsigned n, int narrow_strings) {
  size_t_ i = 0;
  int minus = 0, plus = 0, space = 0, zero = 0, alt = 0, width = 0, precision = -1, starred = 0;
  for (; i < spec_len; ++i) {
    if (spec[i] == '-') minus = 1; else if (spec[i] == '+') plus = 1; else if (spec[i] == ' ') space = 1; else if (spec[i] == '0') zero = 1; else if (spec[i] == '#') alt = 1; else break;
  }
  /* (the first star reads argument n, but argument n of a repeated insert whose arguments were the last read ones goes on with the next) */
  #define STAR() (starred ? fmt_arg(args, 0) : (starred = 1, fmt_arg(args, n == args->last ? n + 1 : n)))
  if (i < spec_len && spec[i] == '*') { width = (int)(LONG)STAR(); if (width < 0) { minus = 1; width = -width; } ++i; }
  else while (i < spec_len && spec[i] >= '0' && spec[i] <= '9') { if (width < 100000) width = width * 10 + (spec[i] - '0'); ++i; }
  if (i < spec_len && spec[i] == '.') {
    ++i;
    precision = 0;
    if (i < spec_len && spec[i] == '*') { precision = (int)(LONG)STAR(); if (precision < 0) precision = -1; ++i; }
    else while (i < spec_len && spec[i] >= '0' && spec[i] <= '9') { if (precision < 100000) precision = precision * 10 + (spec[i] - '0'); ++i; }
  }
  if (width > 100000) width = 100000;
  int size = 0;      /* 0 default, 1 = long, 2 = 64 bit, -1 = short/narrow char, 3 = wide */
  for (; i < spec_len; ++i) {
    if (spec[i] == 'l') size = size == 1 ? 2 : 1;
    else if (spec[i] == 'h') size = -1;
    else if (spec[i] == 'w') size = 3;
    else if (spec[i] == 'I' && i + 2 < spec_len && spec[i + 1] == '6' && spec[i + 2] == '4') { size = 2; i += 2; }
    else if (spec[i] == 'I') size = 2;
    else break;
  }
  if (spec_len == 0) return;      /* ("%1!!": no conversion, no text) */
  const WCHAR type = i < spec_len ? spec[i] : 's';
  const ULONG_PTR value = starred ? fmt_arg(args, 0) : fmt_arg(args, n == args->last && args->last > args->maxreq ? n + 1 : n);
  if (n > args->maxreq) args->maxreq = n;
  #undef STAR
  WCHAR prefix[4], digits[24];
  unsigned prefix_length = 0, digit_count = 0, zeros = 0;
  const WCHAR *string = NULL;
  const char *narrow = NULL;
  size_t_ len = 0;
  int is_string = 0, is_number = 0;
  switch (type) {
    case 's': case 'S': {
      int wide_string = (type == 's') != narrow_strings;
      if (size == 3) wide_string = 1;
      if (size == -1) wide_string = 0;
      if (size == 1) wide_string = 1;
      is_string = 1;
      if (!value) { static const WCHAR null_text[] = {'(', 'n', 'u', 'l', 'l', ')', 0}; string = null_text; }
      else if (wide_string) string = (const WCHAR *)value;
      else narrow = (const char *)value;
      if (string) while (string[len] && (precision < 0 || (int)len < precision)) ++len;
      else while (narrow[len] && (precision < 0 || (int)len < precision)) ++len;
      break;
    }
    case 'c': case 'C': digits[0] = (WCHAR)(value & 0xFFFF); digit_count = 1; break;
    case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
      is_number = 1;
      ULONGLONG v = value;
      const int is_signed = type == 'd' || type == 'i';
      int negative = 0;
      if (type == 'p') size = 2;
      if (size == -1) { v = (USHORT)value; if (is_signed && (SHORT)v < 0) { negative = 1; v = (ULONGLONG)(0 - (LONG)(SHORT)v); } }
      else if (size != 2) { v = (ULONG)value; if (is_signed && (LONG)v < 0) { negative = 1; v = (ULONGLONG)(0 - (ULONGLONG)(LONGLONG)(LONG)v); } }
      else if (is_signed && (LONGLONG)v < 0) { negative = 1; v = 0 - v; }
      const unsigned base = (type == 'x' || type == 'X' || type == 'p') ? 16 : type == 'o' ? 8 : 10;
      do { const unsigned x = (unsigned)(v % base); digits[digit_count++] = (WCHAR)(x < 10 ? '0' + x : (type == 'x' ? 'a' : 'A') + x - 10); v /= base; } while (v);
      int wanted = precision >= 0 ? precision : 0;
      if (type == 'p' && wanted < 16) wanted = 16;
      zeros = wanted > (int)digit_count ? (unsigned)wanted - digit_count : 0;
      if (negative) prefix[prefix_length++] = '-'; else if (is_signed && plus) prefix[prefix_length++] = '+'; else if (is_signed && space) prefix[prefix_length++] = ' ';
      if (alt && base == 16 && value) { prefix[prefix_length++] = '0'; prefix[prefix_length++] = type == 'x' ? 'x' : 'X'; }
      break;
    }
    default: digits[0] = type; digit_count = 1; break;
  }
  if (!is_string) len = prefix_length + zeros + digit_count;
  const int padding = width > (int)len ? width - (int)len : 0;
  const int zero_pad = zero && is_number && precision < 0;
  if (!minus && !zero_pad) fmt_pad(out, padding, ' ');
  if (is_string) {
    for (size_t_ k = 0; k < len && !out->stop; ++k) fmt_char(out, string ? string[k] : (WCHAR)(BYTE)narrow[k]);
  } else {
    for (unsigned k = 0; k < prefix_length; ++k) fmt_char(out, prefix[k]);
    if (!minus && zero_pad) fmt_pad(out, padding, '0');
    for (unsigned k = 0; k < zeros && !out->stop; ++k) fmt_char(out, '0');
    for (unsigned k = digit_count; k > 0 && !out->stop; --k) fmt_char(out, digits[k - 1]);
  }
  if (minus) fmt_pad(out, padding, ' ');
  for (size_t_ k = i + 1; k < spec_len && !out->stop; ++k) fmt_char(out, spec[k]);      /* (what follows the type in the specification is text) */
}
static void fmt_insert(FmtText *out, const WCHAR *spec, size_t_ spec_len, FmtArgs *args, unsigned n, int narrow_strings) {
  const DWORD width = out->width;
  out->in_insert = 1;
  out->width = 0;      /* (an insert is not broken up; the line breaks after it when it has become long enough) */
  fmt_insert_body(out, spec, spec_len, args, n, narrow_strings);
  out->width = width;
  out->in_insert = 0;
  if (!out->stop && width && width != FMT_MAX_WIDTH_MASK && out->length - out->line_start >= width) fmt_newline_break(out);
}

/* Runs a template through the engine: the output text and its state are in `out`. */
static void fmt_template(const WCHAR *template_text, FmtText *out, FmtArgs *args, int ignore, int narrow) {
  for (const WCHAR *p = template_text; *p && !out->stop;) {
    const WCHAR c = *p++;
    if (c == '%') {
      const WCHAR e = *p;
      if (ignore) {
        if (e == '0') break;
        if (e != 'n' && e != 'r' && e != 't') { fmt_char(out, '%'); continue; }
      } else if (!e) {
        out->invalid = 1;
        break;
      } else if (e >= '1' && e <= '9') {
        unsigned v = (unsigned)(e - '0');
        ++p;
        if (*p >= '0' && *p <= '9') v = v * 10 + (unsigned)(*p++ - '0');
        const WCHAR *spec = NULL;
        size_t_ spec_len = 0;
        if (*p == '!') {
          spec = p + 1;
          const WCHAR *q = spec;
          while (*q && *q != '!') ++q;
          spec_len = (size_t_)(q - spec);
          p = *q ? q + 1 : q;
        }
        if (!args->have) { out->invalid = 1; break; }
        static const WCHAR default_spec[] = {'s'};
        fmt_insert(out, spec ? spec : default_spec, spec ? spec_len : 1, args, v, narrow);
        continue;
      } else if (e == '0') {
        break;      /* ends the message, without a line break */
      }
      ++p;
      if (e == 'n') fmt_newline_break(out);
      else if (e == 'r') { fmt_put(out, '\r'); out->line_start = out->length; }
      else if (e == 't') fmt_char(out, '\t');
      else fmt_char(out, e);      /* %% %. %! "% " and the other escapes give the character */
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (c == '\r' && *p == '\n') ++p;      /* "\r\n" and "\n" alike are one line break */
      if (out->width) fmt_char(out, ' ');      /* (a width: the text runs on) */
      else fmt_newline_break(out);
      continue;
    }
    fmt_char(out, c);
  }
}
