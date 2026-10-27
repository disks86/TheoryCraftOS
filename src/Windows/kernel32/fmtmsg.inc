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

/* kernel32.dll: FormatMessage. The message text is a template with inserts (%1 .. %99, optionally %1!printf-type!), a few escapes (%n new line, %t tab, %% ...) and
 * line breaks that are handled according to the width in the low byte of the flags. The arguments are an array of pointer-sized values (FORMAT_MESSAGE_ARGUMENT_ARRAY) or
 * the arguments of a variable argument list, which are read in order. Checked with Wine's tests (tests/windows/wine/kernel32/format_msg.c). Included by kernel32.c. */

#define FMT_ALLOCATE_BUFFER 0x100
#define FMT_IGNORE_INSERTS 0x200
#define FMT_FROM_STRING 0x400
#define FMT_FROM_HMODULE 0x800
#define FMT_FROM_SYSTEM 0x1000
#define FMT_ARGUMENT_ARRAY 0x2000
#define FMT_MAX_WIDTH_MASK 0xFF

typedef struct { WCHAR *text; size_t_ length, capacity; int failed; } FmtText;
static void fmt_put(FmtText *t, WCHAR c) {
  if (t->failed) return;
  if (t->length + 2 > t->capacity) {
    const size_t_ bigger = t->capacity ? t->capacity * 2 : 128;
    WCHAR *p = t->text ? RtlReAllocateHeap(heap(), 0, t->text, bigger * 2) : RtlAllocateHeap(heap(), 0, bigger * 2);
    if (!p) { t->failed = 1; return; }
    t->text = p;
    t->capacity = bigger;
  }
  t->text[t->length++] = c;
}
static void fmt_pad(FmtText *t, int count, WCHAR c) { while (count-- > 0) fmt_put(t, c); }

/* One insert with a printf-style specification ("-5d", "s", "*.*s", "I64x"): the arguments it uses start at `*next` (1-based), and *next moves past them. */
static void fmt_insert(FmtText *out, const WCHAR *spec, size_t_ spec_len, const ULONG_PTR *args, unsigned count, unsigned *next, int narrow_strings) {
  size_t_ i = 0;
  int minus = 0, plus = 0, space = 0, zero = 0, alt = 0, width = 0, precision = -1;
  for (; i < spec_len; ++i) {
    if (spec[i] == '-') minus = 1; else if (spec[i] == '+') plus = 1; else if (spec[i] == ' ') space = 1; else if (spec[i] == '0') zero = 1; else if (spec[i] == '#') alt = 1; else break;
  }
  const unsigned first = *next;
  unsigned used = 0;
  #define ARG() (first + used - 1 < count ? args[first + used++ - 1] : (used++, (ULONG_PTR)0))
  if (i < spec_len && spec[i] == '*') { width = (int)ARG(); if (width < 0) { minus = 1; width = -width; } ++i; }
  else while (i < spec_len && spec[i] >= '0' && spec[i] <= '9') width = width * 10 + (spec[i++] - '0');
  if (i < spec_len && spec[i] == '.') {
    ++i;
    precision = 0;
    if (i < spec_len && spec[i] == '*') { precision = (int)ARG(); ++i; }
    else while (i < spec_len && spec[i] >= '0' && spec[i] <= '9') precision = precision * 10 + (spec[i++] - '0');
  }
  int size = 0;      /* 0 default, 1 = long, 2 = 64 bit, -1 = short/narrow char, 3 = wide */
  for (; i < spec_len; ++i) {
    if (spec[i] == 'l') size = size == 1 ? 2 : 1;
    else if (spec[i] == 'h') size = -1;
    else if (spec[i] == 'w') size = 3;
    else if (spec[i] == 'I' && i + 2 < spec_len && spec[i + 1] == '6' && spec[i + 2] == '4') { size = 2; i += 2; }
    else if (spec[i] == 'I') size = 2;
    else break;
  }
  const WCHAR type = i < spec_len ? spec[i] : 's';
  ULONG_PTR value = ARG();
  WCHAR text[72];
  size_t_ n = 0;
  const WCHAR *string = NULL;
  const char *narrow = NULL;
  int is_string = 0;
  switch (type) {
    case 's': case 'S': {
      int wide_string = (type == 's') != narrow_strings;
      if (size == 3) wide_string = 1;
      if (size == -1) wide_string = 0;
      is_string = 1;
      if (!value) { static const WCHAR null_text[] = {'(', 'n', 'u', 'l', 'l', ')', 0}; string = null_text; }
      else if (wide_string) string = (const WCHAR *)value;
      else narrow = (const char *)value;
      break;
    }
    case 'c': case 'C': text[n++] = (WCHAR)(value & 0xFFFF); break;
    case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
      ULONGLONG v = value;
      const int is_signed = type == 'd' || type == 'i';
      int negative = 0;
      if (type == 'p') { v = value; size = 2; }
      if (size != 2) { v = (ULONG)value; if (is_signed && (LONG)v < 0) { negative = 1; v = (ULONGLONG)(0 - (ULONGLONG)(LONGLONG)(LONG)v); } }
      else if (is_signed && (LONGLONG)v < 0) { negative = 1; v = 0 - v; }
      const unsigned base = (type == 'x' || type == 'X' || type == 'p') ? 16 : type == 'o' ? 8 : 10;
      WCHAR digits[32];
      unsigned d = 0;
      do { const unsigned x = (unsigned)(v % base); digits[d++] = (WCHAR)(x < 10 ? '0' + x : (type == 'x' ? 'a' : 'A') + x - 10); v /= base; } while (v);
      if (precision >= 0) while ((int)d < precision) digits[d++] = '0';
      if (type == 'p') while (d < 16) digits[d++] = '0';
      if (negative) text[n++] = '-'; else if (is_signed && plus) text[n++] = '+'; else if (is_signed && space) text[n++] = ' ';
      if (alt && base == 16 && value) { text[n++] = '0'; text[n++] = type == 'x' ? 'x' : 'X'; }
      while (d) text[n++] = digits[--d];
      break;
    }
    default: text[n++] = type; break;
  }
  size_t_ len = n;
  if (is_string) {
    len = 0;
    if (string) while (string[len] && (precision < 0 || (int)len < precision)) ++len;
    else while (narrow[len] && (precision < 0 || (int)len < precision)) ++len;
  }
  const int padding = width > (int)len ? width - (int)len : 0;
  if (!minus && !(zero && !is_string)) fmt_pad(out, padding, ' ');
  if (!minus && zero && !is_string) {
    size_t_ k = 0;
    while (k < len && (text[k] == '-' || text[k] == '+' || text[k] == ' ')) fmt_put(out, text[k++]);
    fmt_pad(out, padding, '0');
    for (; k < len; ++k) fmt_put(out, text[k]);
  } else if (is_string) {
    for (size_t_ k = 0; k < len; ++k) fmt_put(out, string ? string[k] : (WCHAR)(BYTE)narrow[k]);
  } else {
    for (size_t_ k = 0; k < len; ++k) fmt_put(out, text[k]);
  }
  if (minus) fmt_pad(out, padding, ' ');
  *next = first + used;
  #undef ARG
}

static DWORD fmt_message(DWORD flags, LPCVOID source, DWORD id, void *buffer, DWORD size, PVOID arguments, int narrow) {
  const DWORD width = flags & FMT_MAX_WIDTH_MASK;
  if ((flags & FMT_FROM_STRING) && (flags & (FMT_FROM_HMODULE | FMT_FROM_SYSTEM))) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (!buffer || (!(flags & FMT_ALLOCATE_BUFFER) && !size && 0)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *template_text = NULL;
  WCHAR system_text[256];
  if (flags & FMT_FROM_STRING) {
    if (!source) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (narrow) {
      const int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)source, -1, NULL, 0);
      template_text = RtlAllocateHeap(heap(), 0, (SIZE_T)n * 2 + 2);
      if (!template_text) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
      MultiByteToWideChar(CP_UTF8, 0, (const char *)source, -1, template_text, n);
    } else {
      template_text = (WCHAR *)source;
    }
  } else if (flags & FMT_FROM_SYSTEM) {
    const char *text = error_text(id);
    if (!text) { set_error(317); return 0; }      /* ERROR_MR_MID_NOT_FOUND */
    size_t_ n = 0;
    while (text[n] && n < 250) { system_text[n] = (WCHAR)(BYTE)text[n]; ++n; }
    system_text[n++] = '\r'; system_text[n++] = '\n'; system_text[n] = 0;
    template_text = system_text;
  } else {
    set_error(flags & FMT_FROM_HMODULE ? 317 : ERROR_INVALID_PARAMETER);
    return 0;
  }
  /* the arguments: an array, or the slots of a variable argument list; the highest insert number says how many */
  ULONG_PTR slots[100];
  unsigned count = 0;
  if (!(flags & FMT_IGNORE_INSERTS)) {
    unsigned highest = 0;
    for (const WCHAR *p = template_text; *p; ++p) {
      if (*p != '%') continue;
      ++p;
      if (*p >= '1' && *p <= '9') {
        unsigned v = (unsigned)(*p - '0');
        if (p[1] >= '0' && p[1] <= '9') v = v * 10 + (unsigned)(*++p - '0');
        if (v > highest) highest = v;
      } else if (!*p) break;
    }
    if (highest) {
      if (!arguments) { if (narrow && (flags & FMT_FROM_STRING)) RtlFreeHeap(heap(), 0, template_text); set_error(ERROR_INVALID_PARAMETER); return 0; }
      count = highest < 99 ? highest + 4 : 99;      /* (starred widths and precisions use following slots) */
      if (flags & FMT_ARGUMENT_ARRAY) for (unsigned k = 0; k < count; ++k) slots[k] = ((ULONG_PTR *)arguments)[k];
      else { char *list = *(char **)arguments; for (unsigned k = 0; k < count; ++k) slots[k] = ((ULONG_PTR *)list)[k]; }
    }
  }
  FmtText out = {0};
  unsigned next_sequential = 1;
  size_t_ line_start = 0;
  for (const WCHAR *p = template_text; *p;) {
    const WCHAR c = *p++;
    if (c == '%' && (flags & FMT_IGNORE_INSERTS) && *p == '0') break;
    if (c == '%' && !*p) { if (out.text) RtlFreeHeap(heap(), 0, out.text); if (narrow && (flags & FMT_FROM_STRING)) RtlFreeHeap(heap(), 0, template_text); set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (c == '%' && !(flags & FMT_IGNORE_INSERTS)) {
      const WCHAR e = *p;
      if (e >= '1' && e <= '9') {
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
        unsigned arg = v;
        static const WCHAR default_spec[] = {'s'};
        fmt_insert(&out, spec ? spec : default_spec, spec ? spec_len : 1, slots, count, &arg, narrow);
        (void)next_sequential;
        continue;
      }
      if (e == '0') break;      /* ends the message, without a line break */
      ++p;
      if (e == 'n') { fmt_put(&out, '\r'); fmt_put(&out, '\n'); line_start = out.length; }
      else if (e == 'r') fmt_put(&out, '\r');
      else if (e == 't') fmt_put(&out, '\t');
      else if (e == '%' || e == ' ' || e == '.' || e == '!') fmt_put(&out, e);
      else if (e) fmt_put(&out, e);
      else break;
      continue;
    }
    if (c == '\r' && *p == '\n') ++p;      /* "\r\n" and "\n" alike are the same line break */
    if (c == '\r' || c == '\n') {
      if (width == FMT_MAX_WIDTH_MASK) fmt_put(&out, ' ');      /* a message with its line breaks removed */
      else { fmt_put(&out, '\r'); fmt_put(&out, '\n'); line_start = out.length; }
      continue;
    }
    fmt_put(&out, c);
    /* wrap long lines at a blank when a width is given */
    if (width && width != FMT_MAX_WIDTH_MASK && out.length - line_start > width) {
      size_t_ k = out.length;
      while (k > line_start && out.text[k - 1] != ' ') --k;
      if (k > line_start + 1) {
        out.text[k - 1] = '\r';
        fmt_put(&out, 0);
        memmove(out.text + k + 1, out.text + k, (out.length - k) * 2);
        out.text[k] = '\n';
        line_start = k + 1;
      }
    }
  }
  if (narrow && (flags & FMT_FROM_STRING)) RtlFreeHeap(heap(), 0, template_text);
  if (out.failed) { if (out.text) RtlFreeHeap(heap(), 0, out.text); set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  if (out.length == 0) {      /* nothing came out: no work was done */
    if (out.text) RtlFreeHeap(heap(), 0, out.text);
    set_error(1237);      /* ERROR_NO_WORK_DONE */
    return 0;
  }
  out.text[out.length] = 0;
  /* the result: wide as it is, or narrow */
  DWORD result_chars;
  char *narrow_text = NULL;
  if (narrow) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, out.text, (int)out.length + 1, NULL, 0, NULL, NULL);
    narrow_text = RtlAllocateHeap(heap(), 0, (SIZE_T)bytes);
    if (!narrow_text) { RtlFreeHeap(heap(), 0, out.text); set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    WideCharToMultiByte(CP_UTF8, 0, out.text, (int)out.length + 1, narrow_text, bytes, NULL, NULL);
    result_chars = (DWORD)bytes - 1;
  } else {
    result_chars = (DWORD)out.length;
  }
  DWORD returned = result_chars;
  const unsigned unit = narrow ? 1 : 2;
  if (flags & FMT_ALLOCATE_BUFFER) {
    void *memory = LocalAlloc(0, ((size_t_)(result_chars > size ? result_chars : size) + 1) * unit);
    if (!memory) { set_error(ERROR_NOT_ENOUGH_MEMORY); returned = 0; }
    else { memcpy(memory, narrow ? (void *)narrow_text : (void *)out.text, ((size_t_)result_chars + 1) * unit); *(void **)buffer = memory; }
  } else if (result_chars + 1 > size) {
    set_error(ERROR_INSUFFICIENT_BUFFER);
    returned = 0;
  } else {
    memcpy(buffer, narrow ? (void *)narrow_text : (void *)out.text, ((size_t_)result_chars + 1) * unit);
  }
  if (narrow_text) RtlFreeHeap(heap(), 0, narrow_text);
  RtlFreeHeap(heap(), 0, out.text);
  return returned;
}
EXPORT DWORD WINAPI FormatMessageA(DWORD flags, LPCVOID source, DWORD id, DWORD language, LPSTR buffer, DWORD capacity, PVOID arguments) { return fmt_message(flags, source, id, buffer, capacity, arguments, 1); }
EXPORT DWORD WINAPI FormatMessageW(DWORD flags, LPCVOID source, DWORD id, DWORD language, LPWSTR buffer, DWORD capacity, PVOID arguments) { return fmt_message(flags, source, id, buffer, capacity, arguments, 0); }
