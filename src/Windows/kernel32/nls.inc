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

/* kernel32.dll: dates and times written for a locale (GetDateFormat, GetTimeFormat, and the Ex forms), the calendar (GetCalendarInfo, EnumCalendarInfo) and the lists
 * of date and time formats. The locales are the invariant one and English (United States). Included by kernel32.c after extra3.inc. */

#define NLS_NOUSEROVERRIDE 0x80000000u
#define NLS_DATE_SHORTDATE 0x01
#define NLS_DATE_LONGDATE 0x02
#define NLS_DATE_USE_ALT_CALENDAR 0x04
#define NLS_DATE_YEARMONTH 0x08
#define NLS_DATE_LTRREADING 0x10
#define NLS_DATE_RTLREADING 0x20
#define NLS_DATE_AUTOLAYOUT 0x40
#define NLS_DATE_MONTHDAY 0x80
#define NLS_TIME_NOMINUTESORSECONDS 0x01
#define NLS_TIME_NOSECONDS 0x02
#define NLS_TIME_NOTIMEMARKER 0x04
#define NLS_TIME_FORCE24HOURFORMAT 0x08

static BOOL nls_valid_date(const SYSTEMTIME *st) {
  return st->wYear >= 1601 && st->wYear <= 30827 && st->wMonth >= 1 && st->wMonth <= 12 && st->wDay >= 1 && st->wDay <= month_days(st->wYear, st->wMonth);
}
static int nls_weekday(const SYSTEMTIME *st) {
  const LONGLONG days = days_from_civil(st->wYear, st->wMonth, st->wDay);      /* since 1970-01-01, a Thursday */
  return (int)(((days % 7) + 7 + 4) % 7);
}

/* The result of a picture, collected in a buffer of fixed size (a picture is at most a few hundred characters). */
#define NLS_PICTURE_MAX 520
typedef struct { WCHAR text[NLS_PICTURE_MAX]; int length; int mark; BOOL skipping; } NlsOut;
static void nls_put(NlsOut *o, WCHAR c) { if (o->length < NLS_PICTURE_MAX - 1) o->text[o->length++] = c; }
static void nls_put_text(NlsOut *o, const WCHAR *text) { for (; *text; ++text) nls_put(o, *text); }
static void nls_put_number(NlsOut *o, unsigned v, int width) {
  WCHAR tmp[12];
  int t = 0;
  do { tmp[t++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
  while (t < width) tmp[t++] = '0';
  while (t) nls_put(o, tmp[--t]);
}
/* A token that is written: what follows a token that was dropped is dropped up to the next white space. */
static void nls_keep(NlsOut *o) { o->mark = o->length; o->skipping = FALSE; }
/* A token that is not written takes the text since the last written token with it, and the punctuation after it. */
static void nls_drop(NlsOut *o) { o->length = o->mark; o->skipping = TRUE; }
static void nls_literal(NlsOut *o, WCHAR c) {
  if (o->skipping) {
    if (c != ' ' && c != '\t' && c != 0xA0) return;
    o->skipping = FALSE;
  }
  nls_put(o, c);
}
/* Copies a quoted string: the quote has been read; '' inside is a quote; returns the position after the closing quote. */
static const WCHAR *nls_quoted(NlsOut *o, const WCHAR *p) {
  while (*p) {
    if (*p == '\'') {
      if (p[1] == '\'') { nls_literal(o, '\''); p += 2; continue; }
      return p + 1;
    }
    nls_literal(o, *p++);
  }
  return p;
}

/* The day tokens of a picture (d and dd are the day of the month, ddd and dddd the name of the weekday), outside quotes, by position. */
typedef struct { int position; int kind; /* 1 number, 2 name */ } NlsDayToken;
static int nls_day_tokens(const WCHAR *p, NlsDayToken *tokens, int max) {
  int n = 0;
  const WCHAR *start = p;
  while (*p) {
    if (*p == '\'') { ++p; while (*p && *p != '\'') ++p; if (*p) ++p; continue; }
    if (*p == 'd') {
      int run = 0;
      while (p[run] == 'd') ++run;
      if (n < max) { tokens[n].position = (int)(p - start); tokens[n].kind = run <= 2 ? 1 : 2; ++n; }
      p += run;
      continue;
    }
    ++p;
  }
  return n;
}
/* Whether the month in a picture at a position is in the genitive form: when the day of the month follows, or when it comes before and no weekday name follows. */
static BOOL nls_month_is_genitive(const NlsDayToken *tokens, int count, int position) {
  int previous = 0, next = 0;
  for (int i = 0; i < count; ++i) {
    if (tokens[i].position < position) previous = tokens[i].kind;
    else if (!next) next = tokens[i].kind;
  }
  if (next == 1) return TRUE;
  return previous == 1;
}
static void nls_date_picture(NlsOut *o, int kind, const WCHAR *p, const SYSTEMTIME *st) {
  WCHAR scratch[80];
  NlsDayToken day_tokens[16];
  const WCHAR *picture_start = p;
  const int day_count = nls_day_tokens(p, day_tokens, 16);
  while (*p) {
    if (*p == '\'') { p = nls_quoted(o, p + 1); continue; }
    const WCHAR c = *p;
    int run = 1;
    while (p[run] == c) ++run;
    switch (c) {
      case 'd': {
        if (run > 4) run = 4;
        if (run <= 2) nls_put_number(o, st->wDay, run);
        else nls_put_text(o, nls_day_name(kind, nls_weekday(st), run == 3, scratch));
        break;
      }
      case 'M': {
        if (run > 4) run = 4;
        if (run <= 2) nls_put_number(o, st->wMonth, run);
        else {
          const DWORD base = (run == 3 ? 0x44u : 0x38u) + (DWORD)(st->wMonth - 1);
          if (nls_month_is_genitive(day_tokens, day_count, (int)(p - picture_start)) && nls_text_wide(kind, base | 0x8000u, scratch)) nls_put_text(o, scratch);
          else nls_put_text(o, nls_month_name(kind, st->wMonth, run == 3, scratch));
        }
        break;
      }
      case 'y': {
        if (run > 4) run = 4;
        if (run == 1) nls_put_number(o, st->wYear % 100, 1);
        else if (run == 2) nls_put_number(o, st->wYear % 100, 2);
        else nls_put_number(o, st->wYear, 4);
        break;
      }
      case 'g': {
        if (run > 2) run = 2;
        static const WCHAR era[] = {'A', '.', 'D', '.', 0};
        nls_put_text(o, era);
        break;
      }
      default: for (int k = 0; k < run; ++k) nls_literal(o, c); break;
    }
    p += run;
  }
}

static void nls_time_picture(NlsOut *o, int kind, const WCHAR *p, const SYSTEMTIME *st, DWORD flags) {
  while (*p) {
    if (*p == '\'') { p = nls_quoted(o, p + 1); continue; }
    const WCHAR c = *p;
    int run = 1;
    while (p[run] == c) ++run;
    const int width = run >= 2 ? 2 : 1;
    switch (c) {
      case 'h': case 'H': {
        unsigned hour = st->wHour;
        if (c == 'h' && !(flags & NLS_TIME_FORCE24HOURFORMAT)) { hour %= 12; if (hour == 0) hour = 12; }
        nls_put_number(o, hour, width);
        nls_keep(o);
        break;
      }
      case 'm':
        if (flags & NLS_TIME_NOMINUTESORSECONDS) nls_drop(o);
        else { nls_put_number(o, st->wMinute, width); nls_keep(o); }
        break;
      case 's':
        if (flags & (NLS_TIME_NOMINUTESORSECONDS | NLS_TIME_NOSECONDS)) nls_drop(o);
        else { nls_put_number(o, st->wSecond, width); nls_keep(o); }
        break;
      case 't':
        if (flags & NLS_TIME_NOTIMEMARKER) nls_drop(o);
        else {
          WCHAR marker[80];
          if (nls_text_wide(kind, st->wHour < 12 ? 0x28u : 0x29u, marker)) { if (width == 2) nls_put_text(o, marker); else if (marker[0]) nls_put(o, marker[0]); }
          nls_keep(o);
        }
        break;
      default: for (int k = 0; k < run; ++k) nls_literal(o, c); p += run; continue;
    }
    p += run;
  }
}

/* Hands the text to the caller's buffer: the length with the null (capacity 0 asks for it), or ERROR_INSUFFICIENT_BUFFER. */
static int nls_deliver(const NlsOut *o, LPWSTR out, int capacity) {
  const int needed = o->length + 1;
  if (capacity == 0) return needed;
  if (capacity < needed) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  memcpy(out, o->text, (size_t_)o->length * 2);
  out[o->length] = 0;
  return needed;
}
static void nls_widen(WCHAR *to, const char *from) { while ((*to++ = (WCHAR)(BYTE)*from++)) {} }

#define LOCALE_NAME_MAX_LENGTH_ 85
static BOOL nls_info_text(int kind, DWORD type, BOOL user_override, WCHAR *out /* 80 */);
static int nls_date_format(int locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) {
  if (locale == 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const DWORD shapes = flags & (NLS_DATE_SHORTDATE | NLS_DATE_LONGDATE | NLS_DATE_YEARMONTH | NLS_DATE_MONTHDAY);
  if ((flags & ~(NLS_NOUSEROVERRIDE | 0xFF)) || (shapes & (shapes - 1)) || (format && (shapes || (flags & NLS_NOUSEROVERRIDE)))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (capacity < 0 || (capacity > 0 && !out)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  SYSTEMTIME now;
  if (!time) { GetLocalTime(&now); time = &now; }
  if (!nls_valid_date(time)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR picture[80];
  if (!format) {
    nls_info_text(locale, (shapes & NLS_DATE_LONGDATE) ? 0x20u : (shapes & NLS_DATE_YEARMONTH) ? 0x1006u : (shapes & NLS_DATE_MONTHDAY) ? 0x78u : 0x1Fu, !(flags & NLS_NOUSEROVERRIDE), picture);
    format = picture;
  }
  NlsOut o;
  o.length = 0; o.mark = 0; o.skipping = FALSE;
  nls_date_picture(&o, locale, format, time);
  return nls_deliver(&o, out, capacity);
}
static int nls_time_format(int locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) {
  if (locale == 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if ((flags & ~(NLS_NOUSEROVERRIDE | 0xF)) || (format && (flags & NLS_NOUSEROVERRIDE))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (capacity < 0 || (capacity > 0 && !out)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  SYSTEMTIME now;
  if (!time) { GetLocalTime(&now); time = &now; }
  if (time->wHour > 23 || time->wMinute > 59 || time->wSecond > 59 || time->wMilliseconds > 999) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR picture[80];
  if (!format) {
    nls_info_text(locale, 0x1003u, !(flags & NLS_NOUSEROVERRIDE), picture);
    format = picture;
  }
  NlsOut o;
  o.length = 0; o.mark = 0; o.skipping = FALSE;
  nls_time_picture(&o, locale, format, time, flags);
  return nls_deliver(&o, out, capacity);
}

/* The narrow forms convert the picture and the result (the code page is UTF-8). */
static int nls_narrow_result(int n, const WCHAR *wide, LPSTR out, int capacity) {
  if (n == 0) return 0;
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if (capacity == 0) return needed;
  if (capacity < needed) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, capacity, NULL, NULL);
}

EXPORT int WINAPI GetDateFormatW(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) {
  return nls_date_format(nls_locale_of(locale), flags, time, format, out, capacity);
}
EXPORT int WINAPI GetDateFormatEx(LPCWSTR name, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity, LPCWSTR calendar) {
  if (calendar) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  return nls_date_format(nls_locale_of_name(name), flags, time, format, out, capacity);
}
EXPORT int WINAPI GetTimeFormatW(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) {
  return nls_time_format(nls_locale_of(locale), flags, time, format, out, capacity);
}
EXPORT int WINAPI GetTimeFormatEx(LPCWSTR name, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) {
  return nls_time_format(nls_locale_of_name(name), flags, time, format, out, capacity);
}
EXPORT int WINAPI GetDateFormatA(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCSTR format, LPSTR out, int capacity) {
  if (capacity < 0 || (capacity > 0 && !out)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *f = format ? to_wide(format) : NULL, wide[NLS_PICTURE_MAX];
  if (format && !f) return 0;
  const int n = nls_date_format(nls_locale_of(locale), flags, time, f, wide, NLS_PICTURE_MAX);
  free_wide(f);
  return nls_narrow_result(n, wide, out, capacity);
}
EXPORT int WINAPI GetTimeFormatA(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCSTR format, LPSTR out, int capacity) {
  if (capacity < 0 || (capacity > 0 && !out)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *f = format ? to_wide(format) : NULL, wide[NLS_PICTURE_MAX];
  if (format && !f) return 0;
  const int n = nls_time_format(nls_locale_of(locale), flags, time, f, wide, NLS_PICTURE_MAX);
  free_wide(f);
  return nls_narrow_result(n, wide, out, capacity);
}

/* ---- locale information ------------------------------------------------------------------------------------------------------------------------------------- */

#define LCT_NOUSEROVERRIDE 0x80000000u
#define LCT_USE_CP_ACP 0x40000000u
#define LCT_RETURN_NUMBER 0x20000000u
#define LCT_RETURN_GENITIVE_NAMES 0x10000000u

/* What the user changed with SetLocaleInfo (on Windows it is kept in the registry; here it lasts as long as the process). */
#define NLS_OVERRIDES 48
static struct { DWORD type; WCHAR value[80]; } g_overrides[NLS_OVERRIDES];
static int g_override_count;
static const WCHAR *nls_override(DWORD type) {
  for (int i = 0; i < g_override_count; ++i) if (g_overrides[i].type == type) return g_overrides[i].value;
  return NULL;
}

/* The text of a LOCALE_ value in wide characters, with the numbers written as text. FALSE if the type is not known. `invariant` picks the invariant locale. */
static BOOL nls_info_text(int kind, DWORD type, BOOL user_override, WCHAR *out /* 80 */) {
  const DWORD base = type & 0xFFFF;
  if (user_override && kind == nls_default_kind() && !(type & LCT_RETURN_GENITIVE_NAMES)) {
    const WCHAR *o = nls_override(base);
    if (o) { int i = 0; for (; o[i] && i < 79; ++i) out[i] = o[i]; out[i] = 0; return TRUE; }
  }
  /* the genitive forms of the month names are kept in the slots 0x8038.. of the table */
  if ((type & LCT_RETURN_GENITIVE_NAMES) && base >= 0x38 && base <= 0x4F && nls_text_wide(kind, base | 0x8000u, out)) return TRUE;
  return nls_text_wide(kind, base, out);
}
static BOOL nls_info_is_number(DWORD base) {
  switch (base) {
    case 0x0D: case 0x11: case 0x12: case 0x13: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x4A: case 0x1004: case 0x1007: case 0x100B: case 0x100C: case 0x1010: case 0x05: case 0x09: case 0x0B: case 0x01: case 0x2000 + 0: return TRUE;
    default: return FALSE;
  }
}
/* The worker of GetLocaleInfoW and GetLocaleInfoEx. LOCALE_RETURN_NUMBER hands the value over as a DWORD (the language identifiers are written in hexadecimal). */
static int nls_get_locale_info(int kind, LCTYPE type, LPWSTR buffer, int capacity) {
  if (kind == 0 || capacity < 0 || (capacity > 0 && !buffer)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR text[80];
  const BOOL user = !(type & LCT_NOUSEROVERRIDE);
  if (!nls_info_text(kind, type, user, text)) { set_error(ERROR_INVALID_FLAGS); return 0; }
  const DWORD base = type & 0xFFFF;
  if (type & LCT_RETURN_NUMBER) {
    if (!nls_info_is_number(base)) { set_error(ERROR_INVALID_FLAGS); return 0; }
    DWORD number = 0;
    const DWORD radix = base == 0x01 || base == 0x09 ? 16 : 10;
    for (const WCHAR *c = text; (*c >= '0' && *c <= '9') || (radix == 16 && ((*c | 0x20) >= 'a' && (*c | 0x20) <= 'f')); ++c) number = number * radix + (DWORD)(*c <= '9' ? *c - '0' : (*c | 0x20) - 'a' + 10);
    if (capacity == 0) return 2;
    if (capacity < 2) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buffer, &number, 4);
    return 2;
  }
  if ((type & LCT_RETURN_GENITIVE_NAMES) && !(base >= 0x38 && base <= 0x4F)) { set_error(ERROR_INVALID_FLAGS); return 0; }
  const int n = (int)wcslen(text) + 1;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  memcpy(buffer, text, (size_t_)n * 2);
  return n;
}
EXPORT int WINAPI GetLocaleInfoW(LCID locale, LCTYPE type, LPWSTR buffer, int capacity) { return nls_get_locale_info(nls_locale_of(locale), type, buffer, capacity); }
EXPORT int WINAPI GetLocaleInfoA(LCID locale, LCTYPE type, LPSTR buffer, int capacity) {
  if (type & LCT_RETURN_NUMBER) {
    if (capacity < 0 || (capacity > 0 && !buffer)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (capacity == 0) { return GetLocaleInfoW(locale, type, NULL, 0) ? 4 : 0; }
    if (capacity < 4) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return GetLocaleInfoW(locale, type, (LPWSTR)buffer, 2) ? 4 : 0;
  }
  if ((type & LCT_RETURN_GENITIVE_NAMES) || (type & 0xFFFF) == 0x79 /* SSHORTTIME */) { set_error(ERROR_INVALID_FLAGS); return 0; }
  WCHAR wide[80];
  const int n = GetLocaleInfoW(locale, type, wide, 80);
  if (n == 0) return 0;
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if (capacity > 0 && capacity < needed) {
    /* a buffer that is too short is filled as far as it goes (some programs rely on it) */
    BYTE tmp[400];
    const int got = WideCharToMultiByte(CP_UTF8, 0, wide, -1, (LPSTR)tmp, sizeof(tmp), NULL, NULL);
    for (int i = 0; i < capacity && i < got; ++i) buffer[i] = (char)tmp[i];
    set_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  return nls_narrow_result(n, wide, buffer, capacity);
}
EXPORT int WINAPI GetLocaleInfoEx(LPCWSTR name, LCTYPE type, LPWSTR buffer, int capacity) { return nls_get_locale_info(nls_locale_of_name(name), type, buffer, capacity); }
static BOOL nls_set_info(LCID locale, LCTYPE type, LPCWSTR value) {
  if (!value) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (nls_locale_of(locale) != nls_default_kind()) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD base = type & 0xFFFF;
  switch (base) {
    case 0x1009 /* ICALENDARTYPE */: case 0x19 /* ICURRDIGITS */: case 0x1B /* ICURRENCY */: case 0x11 /* IDIGITS */: case 0x1014 /* IDIGITSUBSTITUTION */: case 0x100C /* IFIRSTDAYOFWEEK */:
    case 0x100D /* IFIRSTWEEKOFYEAR */: case 0x12 /* ILZERO */: case 0x0D /* IMEASURE */: case 0x1C /* INEGCURR */: case 0x1010 /* INEGNUMBER */: case 0x100A /* IPAPERSIZE */:
    case 0x23 /* ITIME */: case 0x28 /* S1159 */: case 0x29 /* S2359 */: case 0x14 /* SCURRENCY */: case 0x1D /* SDATE */: case 0x0E /* SDECIMAL */: case 0x10 /* SGROUPING */:
    case 0x0C /* SLIST */: case 0x20 /* SLONGDATE */: case 0x16 /* SMONDECIMALSEP */: case 0x18 /* SMONGROUPING */: case 0x17 /* SMONTHOUSANDSEP */: case 0x13 /* SNATIVEDIGITS */:
    case 0x51 /* SNEGATIVESIGN */: case 0x50 /* SPOSITIVESIGN */: case 0x1F /* SSHORTDATE */: case 0x79 /* SSHORTTIME */: case 0x0F /* STHOUSAND */: case 0x1E /* STIME */:
    case 0x1003 /* STIMEFORMAT */: case 0x1006 /* SYEARMONTH */: case 0x15 /* SINTLSYMBOL */:
      break;
    default: set_error(ERROR_INVALID_FLAGS); return FALSE;
  }
  int length = 0;
  while (value[length]) ++length;
  if (length > 79) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  for (int i = 0; i < g_override_count; ++i) if (g_overrides[i].type == base) { memcpy(g_overrides[i].value, value, ((size_t_)length + 1) * 2); return TRUE; }
  if (g_override_count >= NLS_OVERRIDES) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  g_overrides[g_override_count].type = base;
  memcpy(g_overrides[g_override_count].value, value, ((size_t_)length + 1) * 2);
  ++g_override_count;
  return TRUE;
}
EXPORT BOOL WINAPI SetLocaleInfoW(LCID locale, LCTYPE type, LPCWSTR value) { return nls_set_info(locale, type, value); }
EXPORT BOOL WINAPI SetLocaleInfoA(LCID locale, LCTYPE type, LPCSTR value) {
  if (!value) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR *w = to_wide(value);
  if (!w) return FALSE;
  const BOOL ok = nls_set_info(locale, type, w);
  free_wide(w);
  return ok;
}

/* ---- numbers and currency ------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct { UINT NumDigits, LeadingZero, Grouping; LPWSTR lpDecimalSep, lpThousandSep; UINT NegativeOrder; } NUMBERFMTW_;
typedef struct { UINT NumDigits, LeadingZero, Grouping; LPWSTR lpDecimalSep, lpThousandSep; UINT NegativeOrder, PositiveOrder; LPWSTR lpCurrencySymbol; } CURRENCYFMTW_;
typedef struct { UINT NumDigits, LeadingZero, Grouping; LPSTR lpDecimalSep, lpThousandSep; UINT NegativeOrder; } NUMBERFMTA_;
typedef struct { UINT NumDigits, LeadingZero, Grouping; LPSTR lpDecimalSep, lpThousandSep; UINT NegativeOrder, PositiveOrder; LPSTR lpCurrencySymbol; } CURRENCYFMTA_;

/* The grouping of a NUMBERFMT is a number whose digits are the sizes of the groups from the right; a last digit of 0 ends the grouping, any other repeats. The
 * grouping of a locale is the string "3;0" (a last 0 repeats the group before, no 0 ends). This turns the string into the number. */
static ULONGLONG nls_grouping_number(const WCHAR *text) {
  WCHAR digits[40];
  int count = 0, last_zero = 0, tokens = 0;
  for (const WCHAR *c = text; *c;) {
    ++tokens;
    int length = 0;
    const WCHAR *start = c;
    while (*c && *c != ';') { ++c; ++length; }
    last_zero = length == 1 && *start == '0';
    for (int k = 0; k < length && count < 38; ++k) digits[count++] = start[k];
    if (*c == ';') ++c;
  }
  if (tokens > 1 && last_zero) --count;      /* "3;0": the 0 only says that the 3 repeats */
  else if (!last_zero || tokens == 1) digits[count++] = '0';      /* no 0 at the end: the grouping ends */
  ULONGLONG v = 0;
  for (int i = 0; i < count; ++i) v = v * 10 + (ULONGLONG)(digits[i] - '0');
  return v;
}

/* The number in `value` (digits with an optional minus and an optional point) written for the format; `symbol` is the currency symbol (NULL for a plain number). */
typedef struct {
  UINT digits, leading_zero; ULONGLONG grouping; const WCHAR *decimal, *thousand; UINT negative_order, positive_order; const WCHAR *symbol;
} NlsNumberFormat;

static BOOL nls_number_valid(const WCHAR *value) {
  const WCHAR *c = value;
  if (*c == '-') ++c;
  int digits = 0, dots = 0;
  for (; *c; ++c) {
    if (*c >= '0' && *c <= '9') ++digits;
    else if (*c == '.' && dots == 0) ++dots;
    else return FALSE;
  }
  return digits > 0;
}
static int nls_format_number(const WCHAR *value, const NlsNumberFormat *f, BOOL currency, WCHAR *out, int capacity) {
  WCHAR whole[512], frac[512];
  int w = 0, fr = 0;
  const BOOL negative_input = value[0] == '-';
  const WCHAR *c = negative_input ? value + 1 : value;
  while (*c >= '0' && *c <= '9' && w < 500) whole[w++] = *c++;
  if (*c == '.') { ++c; while (*c >= '0' && *c <= '9' && fr < 500) frac[fr++] = *c++; }
  int skip = 0;
  while (skip < w - 1 && whole[skip] == '0') ++skip;      /* leading zeros */
  if (skip) { memmove(whole, whole + skip, (size_t_)(w - skip) * 2); w -= skip; }
  if (w == 0) { whole[0] = '0'; w = 1; }
  const int want = (int)f->digits;
  if (fr > want) {      /* round half up at the digit after the last */
    const BOOL up = frac[want] >= '5';
    fr = want;
    if (up) {
      int i = fr - 1;
      while (i >= 0 && frac[i] == '9') frac[i--] = '0';
      if (i >= 0) ++frac[i];
      else {
        i = w - 1;
        while (i >= 0 && whole[i] == '9') whole[i--] = '0';
        if (i >= 0) ++whole[i];
        else { if (w < 500) { memmove(whole + 1, whole, (size_t_)w * 2); whole[0] = '1'; ++w; } }
      }
    }
  }
  while (fr < want && fr < 500) frac[fr++] = '0';
  BOOL zero = TRUE;
  for (int i = 0; i < w; ++i) if (whole[i] != '0') zero = FALSE;
  for (int i = 0; i < fr; ++i) if (frac[i] != '0') zero = FALSE;
  const BOOL negative = negative_input && !zero;

  /* the number, without sign or symbol */
  WCHAR body[1100];
  int n = 0;
  #define BODY(ch) do { if (n < 1090) body[n++] = (ch); } while (0)
  {
    UINT sizes[24];
    int size_count = 0;
    BOOL repeat = FALSE;
    ULONGLONG g = f->grouping;
    if (g != 0) {
      WCHAR digits[24];
      int d = 0;
      while (g && d < 22) { digits[d++] = (WCHAR)(g % 10); g /= 10; }
      for (int i = d - 1; i >= 0; --i) sizes[size_count++] = (UINT)digits[i];
      if (sizes[size_count - 1] == 0) --size_count; else repeat = TRUE;
    }
    int cuts[600];      /* the separators go before the digit at each of these places (twice at one place: an empty group) */
    int cut_count = 0, remaining = w;
    for (int index = 0;; ++index) {
      UINT size;
      if (index < size_count) size = sizes[index];
      else if (repeat) size = sizes[size_count - 1];
      else break;
      if (size != 0 && (int)size >= remaining) break;
      remaining -= (int)size;
      if (cut_count < 599) cuts[cut_count++] = remaining;
    }
    for (int i = 0; i <= w; ++i) {
      for (int k = 0; k < cut_count; ++k) if (cuts[k] == i) for (const WCHAR *t = f->thousand; *t; ++t) BODY(*t);
      if (i < w) BODY(whole[i]);
    }
  }
  const BOOL whole_is_zero = w == 1 && whole[0] == '0';
  if (whole_is_zero && !f->leading_zero && fr > 0) {      /* ".5" */
    for (int i = n - 1; i >= 0; --i) if (body[i] == '0') { memmove(body + i, body + i + 1, (size_t_)(n - i - 1) * 2); --n; break; }
  }
  if (fr > 0) {
    for (const WCHAR *d = f->decimal; *d; ++d) BODY(*d);
    for (int i = 0; i < fr; ++i) BODY(frac[i]);
  }
  #undef BODY
  body[n] = 0;

  WCHAR result[1200];
  int len = 0;
  #define RES(ch) do { if (len < 1190) result[len++] = (ch); } while (0)
  #define RES_BODY() do { for (int k = 0; k < n; ++k) RES(body[k]); } while (0)
  #define RES_SYMBOL() do { for (const WCHAR *s = f->symbol; *s; ++s) RES(*s); } while (0)
  if (!currency) {
    if (!negative) RES_BODY();
    else switch (f->negative_order) {
      case 0: RES('('); RES_BODY(); RES(')'); break;
      case 2: RES('-'); RES(' '); RES_BODY(); break;
      case 3: RES_BODY(); RES('-'); break;
      case 4: RES_BODY(); RES(' '); RES('-'); break;
      default: RES('-'); RES_BODY(); break;
    }
  } else if (!negative) {
    switch (f->positive_order) {
      case 1: RES_BODY(); RES_SYMBOL(); break;
      case 2: RES_SYMBOL(); RES(' '); RES_BODY(); break;
      case 3: RES_BODY(); RES(' '); RES_SYMBOL(); break;
      default: RES_SYMBOL(); RES_BODY(); break;
    }
  } else {
    switch (f->negative_order) {
      case 0: RES('('); RES_SYMBOL(); RES_BODY(); RES(')'); break;
      case 1: RES('-'); RES_SYMBOL(); RES_BODY(); break;
      case 2: RES_SYMBOL(); RES('-'); RES_BODY(); break;
      case 3: RES_SYMBOL(); RES_BODY(); RES('-'); break;
      case 4: RES('('); RES_BODY(); RES_SYMBOL(); RES(')'); break;
      case 5: RES('-'); RES_BODY(); RES_SYMBOL(); break;
      case 6: RES_BODY(); RES('-'); RES_SYMBOL(); break;
      case 7: RES_BODY(); RES_SYMBOL(); RES('-'); break;
      case 8: RES('-'); RES_BODY(); RES(' '); RES_SYMBOL(); break;
      case 9: RES('-'); RES_SYMBOL(); RES(' '); RES_BODY(); break;
      case 10: RES_BODY(); RES(' '); RES_SYMBOL(); RES('-'); break;
      case 11: RES_SYMBOL(); RES(' '); RES_BODY(); RES('-'); break;
      case 12: RES_SYMBOL(); RES(' '); RES('-'); RES_BODY(); break;
      case 13: RES_BODY(); RES('-'); RES(' '); RES_SYMBOL(); break;
      case 14: RES('('); RES_SYMBOL(); RES(' '); RES_BODY(); RES(')'); break;
      default: RES('('); RES_BODY(); RES(' '); RES_SYMBOL(); RES(')'); break;
    }
  }
  #undef RES
  #undef RES_BODY
  #undef RES_SYMBOL
  result[len] = 0;
  const int needed = len + 1;
  if (capacity == 0) return needed;
  if (capacity < needed) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  memcpy(out, result, (size_t_)needed * 2);
  return needed;
}

static UINT nls_info_uint(int kind, DWORD type, BOOL user, UINT fallback) {
  WCHAR text[80];
  if (!nls_info_text(kind, type, user, text)) return fallback;
  UINT v = 0;
  for (const WCHAR *c = text; *c >= '0' && *c <= '9'; ++c) v = v * 10 + (UINT)(*c - '0');
  return v;
}
/* Common to GetNumberFormat and GetCurrencyFormat. `fmt` is the caller's format (NULL: the locale's). */
static int nls_number_or_currency(int kind, DWORD flags, LPCWSTR value, const UINT *fmt, LPCWSTR fmt_decimal, LPCWSTR fmt_thousand, LPCWSTR fmt_symbol, BOOL have_fmt, BOOL currency, LPWSTR out, int capacity) {
  if (kind == 0 || !value || capacity < 0 || (capacity > 0 && !out)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if ((flags & ~LCT_NOUSEROVERRIDE) || (have_fmt && flags)) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (!nls_number_valid(value)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  NlsNumberFormat f;
  WCHAR decimal[80], thousand[80], symbol[80], grouping_text[80];
  if (have_fmt) {
    f.digits = fmt[0]; f.leading_zero = fmt[1]; f.grouping = fmt[2]; f.negative_order = fmt[3]; f.positive_order = currency ? fmt[4] : 0;
    if (f.digits > 9 || f.leading_zero > 1 || f.negative_order > (currency ? 15u : 4u) || (currency && f.positive_order > 3) || !fmt_decimal || !fmt_thousand || (currency && !fmt_symbol)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    f.decimal = fmt_decimal; f.thousand = fmt_thousand; f.symbol = fmt_symbol;
  } else {
    const BOOL user = !(flags & LCT_NOUSEROVERRIDE);
    nls_info_text(kind, currency ? 0x16 : 0x0E, user, decimal);
    nls_info_text(kind, currency ? 0x17 : 0x0F, user, thousand);
    nls_info_text(kind, currency ? 0x18 : 0x10, user, grouping_text);
    f.digits = nls_info_uint(kind, currency ? 0x19 : 0x11, user, 2);
    f.leading_zero = nls_info_uint(kind, 0x12, user, 1);
    f.grouping = nls_grouping_number(grouping_text);
    f.negative_order = nls_info_uint(kind, currency ? 0x1C : 0x1010, user, 1);
    f.positive_order = currency ? nls_info_uint(kind, 0x1B, user, 0) : 0;
    f.decimal = decimal; f.thousand = thousand;
    nls_info_text(kind, 0x14, user, symbol);
    f.symbol = symbol;
  }
  return nls_format_number(value, &f, currency, out, capacity);
}
EXPORT int WINAPI GetNumberFormatW(LCID locale, DWORD flags, LPCWSTR value, const NUMBERFMTW_ *format, LPWSTR out, int capacity) {
  UINT fmt[4] = {};
  if (format) { fmt[0] = format->NumDigits; fmt[1] = format->LeadingZero; fmt[2] = format->Grouping; fmt[3] = format->NegativeOrder; }
  return nls_number_or_currency(nls_locale_of(locale), flags, value, fmt, format ? format->lpDecimalSep : NULL, format ? format->lpThousandSep : NULL, NULL, format != NULL, FALSE, out, capacity);
}
EXPORT int WINAPI GetNumberFormatEx(LPCWSTR name, DWORD flags, LPCWSTR value, const NUMBERFMTW_ *format, LPWSTR out, int capacity) {
  UINT fmt[4] = {};
  if (format) { fmt[0] = format->NumDigits; fmt[1] = format->LeadingZero; fmt[2] = format->Grouping; fmt[3] = format->NegativeOrder; }
  return nls_number_or_currency(nls_locale_of_name(name), flags, value, fmt, format ? format->lpDecimalSep : NULL, format ? format->lpThousandSep : NULL, NULL, format != NULL, FALSE, out, capacity);
}
EXPORT int WINAPI GetCurrencyFormatW(LCID locale, DWORD flags, LPCWSTR value, const CURRENCYFMTW_ *format, LPWSTR out, int capacity) {
  UINT fmt[5] = {};
  if (format) { fmt[0] = format->NumDigits; fmt[1] = format->LeadingZero; fmt[2] = format->Grouping; fmt[3] = format->NegativeOrder; fmt[4] = format->PositiveOrder; }
  return nls_number_or_currency(nls_locale_of(locale), flags, value, fmt, format ? format->lpDecimalSep : NULL, format ? format->lpThousandSep : NULL, format ? format->lpCurrencySymbol : NULL, format != NULL, TRUE, out, capacity);
}
EXPORT int WINAPI GetCurrencyFormatEx(LPCWSTR name, DWORD flags, LPCWSTR value, const CURRENCYFMTW_ *format, LPWSTR out, int capacity) {
  UINT fmt[5] = {};
  if (format) { fmt[0] = format->NumDigits; fmt[1] = format->LeadingZero; fmt[2] = format->Grouping; fmt[3] = format->NegativeOrder; fmt[4] = format->PositiveOrder; }
  return nls_number_or_currency(nls_locale_of_name(name), flags, value, fmt, format ? format->lpDecimalSep : NULL, format ? format->lpThousandSep : NULL, format ? format->lpCurrencySymbol : NULL, format != NULL, TRUE, out, capacity);
}
EXPORT int WINAPI GetNumberFormatA(LCID locale, DWORD flags, LPCSTR value, const NUMBERFMTA_ *format, LPSTR out, int capacity) {
  if (capacity < 0 || (capacity > 0 && !out) || !value) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *v = to_wide(value), *d = format && format->lpDecimalSep ? to_wide(format->lpDecimalSep) : NULL, *t = format && format->lpThousandSep ? to_wide(format->lpThousandSep) : NULL, wide[1200];
  UINT fmt[4] = {};
  if (format) { fmt[0] = format->NumDigits; fmt[1] = format->LeadingZero; fmt[2] = format->Grouping; fmt[3] = format->NegativeOrder; }
  const int n = nls_number_or_currency(nls_locale_of(locale), flags, v, fmt, d, t, NULL, format != NULL, FALSE, wide, 1200);
  free_wide(v); free_wide(d); free_wide(t);
  return nls_narrow_result(n, wide, out, capacity);
}
EXPORT int WINAPI GetCurrencyFormatA(LCID locale, DWORD flags, LPCSTR value, const CURRENCYFMTA_ *format, LPSTR out, int capacity) {
  if (capacity < 0 || (capacity > 0 && !out) || !value) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *v = to_wide(value), *d = format && format->lpDecimalSep ? to_wide(format->lpDecimalSep) : NULL, *t = format && format->lpThousandSep ? to_wide(format->lpThousandSep) : NULL;
  WCHAR *s = format && format->lpCurrencySymbol ? to_wide(format->lpCurrencySymbol) : NULL, wide[1200];
  UINT fmt[5] = {};
  if (format) { fmt[0] = format->NumDigits; fmt[1] = format->LeadingZero; fmt[2] = format->Grouping; fmt[3] = format->NegativeOrder; fmt[4] = format->PositiveOrder; }
  const int n = nls_number_or_currency(nls_locale_of(locale), flags, v, fmt, d, t, s, format != NULL, TRUE, wide, 1200);
  free_wide(v); free_wide(d); free_wide(t); free_wide(s);
  return nls_narrow_result(n, wide, out, capacity);
}

/* ---- sorting -------------------------------------------------------------------------------------------------------------------------------------------------- */

#define CMP_IGNORECASE 0x00000001
#define CMP_IGNORENONSPACE 0x00000002
#define CMP_IGNORESYMBOLS 0x00000004
#define CMP_DIGITSASNUMBERS 0x00000008
#define CMP_LINGUISTIC_IGNORECASE 0x00000010
#define CMP_LINGUISTIC_IGNOREDIACRITIC 0x00000020
#define CMP_STRINGSORT 0x00001000
#define CMP_IGNOREKANATYPE 0x00010000
#define CMP_IGNOREWIDTH 0x00020000
#define CMP_LINGUISTIC_CASING 0x08000000

/* How a character sorts: a primary weight (symbols < digits < letters, letters without case or accent), then the accent, then the case (lower before upper). */
typedef struct { unsigned primary, accent, upper; BOOL ignorable, special; } SortWeight;

static const char k_sort_symbols[] = "\t\n\v\f\r '-,;:!?#._\"()[]{}@*/\\&%`^+<=>|~$";
static const char k_latin1_base[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUY?s";
static const unsigned char k_latin1_accent[32] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 5, 1, 2, 3, 5, 9, 4, 1, 2, 3, 4, 5, 0, 10, 1, 2, 3, 5, 2, 0, 11};

static void sort_weight_of(WCHAR c, DWORD flags, SortWeight *w) {
  w->primary = 0; w->accent = 0; w->upper = 0; w->ignorable = FALSE; w->special = FALSE;
  if (c == 0) { w->ignorable = TRUE; return; }
  if (c < 0x20 && !(c >= 9 && c <= 13)) { w->primary = 0x10u + c; return; }      /* (the controls sort before the symbols) */
  if (c == 0x7F || c == 0xAD || (c >= 0x200B && c <= 0x200F) || c == 0xFEFF) { w->ignorable = TRUE; return; }
  if (c >= '0' && c <= '9') { w->primary = 0x1000 + (c - '0'); return; }
  if (c == 0x131) { w->primary = 0x2000 + 8 * 4 + 1; return; }      /* the dotless i and the i with a dot sort after the i */
  if (c == 0xE130) { w->primary = 0x2000 + 8 * 4 + 2; w->upper = 1; return; }
  if (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7) {
    const unsigned i = (c & 0x1F) | 0;
    const unsigned idx = (c - 0xC0) & 0x1F;
    (void)i;
    w->upper = c < 0xE0;
    w->accent = k_latin1_accent[idx];
    const char base = k_latin1_base[idx];
    if (idx == 30) { w->primary = 0x2000 + 26 * 4; return; }      /* thorn: after z */
    w->primary = 0x2000 + (unsigned)((base | 0x20) - 'a') * 4;
    return;
  }
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { w->primary = 0x2000 + (unsigned)((c | 0x20) - 'a') * 4; w->upper = c < 'a'; return; }
  if (c < 0x80) {
    for (int i = 0; k_sort_symbols[i]; ++i) if ((unsigned char)k_sort_symbols[i] == c) {
      w->primary = 0x100 + (unsigned)i;
      w->special = (c == '-' || c == '\'') && !(flags & CMP_STRINGSORT);
      return;
    }
    w->primary = 0x100 + 40;
    return;
  }
  if (c == 0xD7 || c == 0xF7 || (c >= 0xA0 && c < 0xC0)) { w->primary = 0x100 + 50 + (c & 0x3F); return; }
  w->primary = 0x4000 + c;
  if (c >= 0x100 && c < 0x4000) w->upper = 0;
}
/* The next character of the string that counts (the others are skipped). */
static BOOL sort_is_mark(WCHAR c) {
  if (c < 0x300) return FALSE;
  WORD c1, c2, c3;
  ctype_of(c, &c1, &c2, &c3);
  return (c3 & 1 /* C3_NONSPACING */) != 0;
}
static BOOL sort_next(const WCHAR **p, const WCHAR *end, DWORD flags, SortWeight *w, BOOL include_special) {
  while (*p < end) {
    const WCHAR c = **p;
    sort_weight_of(c, flags, w);
    ++*p;
    if (w->ignorable || c == 0) continue;
    /* the combining marks after a letter are its accent */
    while (*p < end && sort_is_mark(**p) && !sort_is_mark(c)) { w->accent = (w->accent * 131u + (unsigned)(**p - 0x2FFu)) & 0xFFFFu; ++*p; }
    if ((flags & CMP_IGNORESYMBOLS) && w->primary < 0x1000) continue;
    if (w->special && !include_special) continue;
    return TRUE;
  }
  return FALSE;
}
static int sort_digit_run(const WCHAR **p, const WCHAR *end, ULONGLONG *value) {
  *value = 0;
  int count = 0;
  while (*p < end && **p >= '0' && **p <= '9') { if (count < 18) *value = *value * 10 + (ULONGLONG)(**p - '0'); ++*p; ++count; }
  return count;
}
/* 1 less, 2 equal, 3 greater. */
static int sort_compare(const WCHAR *a, int an, const WCHAR *b, int bn, DWORD flags) {
  const WCHAR *a_end = a + an, *b_end = b + bn;
  for (int pass = 0; pass < 4; ++pass) {
    if (pass == 1 && (flags & (CMP_IGNORENONSPACE | CMP_LINGUISTIC_IGNOREDIACRITIC))) continue;
    if (pass == 2 && (flags & (CMP_IGNORECASE | CMP_LINGUISTIC_IGNORECASE))) continue;
    if (pass == 3 && (flags & CMP_STRINGSORT)) continue;
    const WCHAR *pa = a, *pb = b;
    for (;;) {
      SortWeight x, y;
      if (pass == 0 && (flags & CMP_DIGITSASNUMBERS) && pa < a_end && pb < b_end && *pa >= '0' && *pa <= '9' && *pb >= '0' && *pb <= '9') {
        ULONGLONG va, vb;
        sort_digit_run(&pa, a_end, &va);
        sort_digit_run(&pb, b_end, &vb);
        if (va != vb) return va < vb ? 1 : 3;
        continue;
      }
      const BOOL ha = sort_next(&pa, a_end, flags, &x, pass == 3), hb = sort_next(&pb, b_end, flags, &y, pass == 3);
      if (!ha || !hb) {
        if (ha != hb) return ha ? 3 : 1;      /* the longer one is greater */
        break;
      }
      if (pass == 0 || pass == 3) { if (x.primary != y.primary) return x.primary < y.primary ? 1 : 3; }
      else if (pass == 1) { if (x.accent != y.accent) return x.accent < y.accent ? 1 : 3; }
      else if (x.upper != y.upper) return x.upper < y.upper ? 1 : 3;
    }
  }
  return 2;
}

/* How many of the units a string is said to have can be read: a count that is longer than the string stops where its memory does (the comparison ends at the first
 * difference, so it never needed the rest). */
static int nls_readable_units(const WCHAR *s, int n) {
  int ok = 0;
  while (ok < n) {
    const ULONG_PTR page_end = ((ULONG_PTR)(s + ok) | 0xFFF) + 1;
    int step = (int)((page_end - (ULONG_PTR)(s + ok)) / 2);
    if (step <= 0) step = 1;
    if (step > n - ok) step = n - ok;
    if (!range_allowed(s + ok, (SIZE_T)step * 2, FALSE)) break;
    ok += step;
  }
  return ok;
}
/* The decomposition of the capital I with a dot above would turn it into an I and a mark; it is a letter of its own in the sort order, kept as a private character. */
static void nls_protect_dotted_i(WCHAR *text, int count) { for (int i = 0; i < count; ++i) if (text[i] == 0x130) text[i] = 0xE130; }
/* The checks and the conversion of the length that CompareString* have in common. */
static int nls_compare(DWORD flags, const WCHAR *a, int an, const WCHAR *b, int bn, BOOL ex) {
  const DWORD valid = CMP_IGNORECASE | CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS | CMP_LINGUISTIC_IGNORECASE | CMP_LINGUISTIC_IGNOREDIACRITIC | CMP_STRINGSORT | CMP_IGNOREKANATYPE | CMP_IGNOREWIDTH | CMP_LINGUISTIC_CASING | LCT_USE_CP_ACP | (ex ? CMP_DIGITSASNUMBERS : 0);
  if (flags & ~valid) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (!a || !b) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (an < 0) an = (int)wcslen(a);
  if (bn < 0) bn = (int)wcslen(b);
  an = nls_readable_units(a, an);
  bn = nls_readable_units(b, bn);
  /* the strings are compared decomposed, so that a letter with its accent and the letter followed by the mark are the same */
  INT room_a = an * 18 + 8, room_b = bn * 18 + 8;
  WCHAR *da = RtlAllocateHeap(heap(), 0, ((SIZE_T)room_a + (SIZE_T)an) * 2), *db = RtlAllocateHeap(heap(), 0, ((SIZE_T)room_b + (SIZE_T)bn) * 2);
  int result;
  BOOL done = FALSE;
  if (da && db && an && bn) {
    WCHAR *ca = da + room_a, *cb = db + room_b;       /* (copies, with the dotted capital i set aside) */
    memcpy(ca, a, (SIZE_T)an * 2); memcpy(cb, b, (SIZE_T)bn * 2);
    nls_protect_dotted_i(ca, an); nls_protect_dotted_i(cb, bn);
    if (RtlNormalizeString(2, ca, an, da, &room_a) == STATUS_SUCCESS && RtlNormalizeString(2, cb, bn, db, &room_b) == STATUS_SUCCESS) {
      result = sort_compare(da, room_a, db, room_b, flags);
      done = TRUE;
    }
  }
  if (!done) result = sort_compare(a, an, b, bn, flags);
  if (da) RtlFreeHeap(heap(), 0, da);
  if (db) RtlFreeHeap(heap(), 0, db);
  return result;
}
EXPORT int WINAPI CompareStringW(DWORD locale, DWORD flags, LPCWSTR a, int an, LPCWSTR b, int bn) {
  (void)locale;
  return nls_compare(flags, a, an, b, bn, FALSE);
}
EXPORT int WINAPI CompareStringEx(LPCWSTR name, DWORD flags, LPCWSTR a, int an, LPCWSTR b, int bn, LPVOID version, LPVOID reserved, LPARAM sort_handle) {
  if (version || reserved || sort_handle) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (name && nls_locale_of_name(name) == 0 && !(name[0] && name[1] && name[2] == '-')) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  return nls_compare(flags, a, an, b, bn, TRUE);
}
EXPORT int WINAPI CompareStringA(DWORD locale, DWORD flags, LPCSTR a, int an, LPCSTR b, int bn) {
  (void)locale;
  if (!a || !b) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (an < 0) an = (int)strlen(a);
  if (bn < 0) bn = (int)strlen(b);
  WCHAR *wa = NULL, *wb = NULL;
  int result = 0;
  const int la = an ? MultiByteToWideChar(CP_UTF8, 0, a, an, NULL, 0) : 0, lb = bn ? MultiByteToWideChar(CP_UTF8, 0, b, bn, NULL, 0) : 0;
  wa = RtlAllocateHeap(heap(), 0, ((SIZE_T)la + 1) * 2);
  wb = RtlAllocateHeap(heap(), 0, ((SIZE_T)lb + 1) * 2);
  if (wa && wb) {
    if (la) MultiByteToWideChar(CP_UTF8, 0, a, an, wa, la);
    if (lb) MultiByteToWideChar(CP_UTF8, 0, b, bn, wb, lb);
    result = nls_compare(flags, wa, la, wb, lb, FALSE);
  } else set_error(ERROR_NOT_ENOUGH_MEMORY);
  RtlFreeHeap(heap(), 0, wa);
  RtlFreeHeap(heap(), 0, wb);
  return result;
}

/* ---- LCMapString ---------------------------------------------------------------------------------------------------------------------------------------------- */

#define MAP_LOWERCASE 0x00000100
#define MAP_UPPERCASE 0x00000200
#define MAP_SORTKEY 0x00000400
#define MAP_BYTEREV 0x00000800
#define MAP_HASH 0x00040000
#define MAP_HIRAGANA 0x00100000
#define MAP_KATAKANA 0x00200000
#define MAP_HALFWIDTH 0x00400000
#define MAP_FULLWIDTH 0x00800000
#define MAP_LINGUISTIC_CASING 0x01000000
#define MAP_SIMPLIFIED 0x02000000
#define MAP_TRADITIONAL 0x04000000
#define MAP_SORTHANDLE 0x20000000
#define MAP_ALL (MAP_LOWERCASE | MAP_UPPERCASE | MAP_SORTKEY | MAP_BYTEREV | MAP_HIRAGANA | MAP_KATAKANA | MAP_HALFWIDTH | MAP_FULLWIDTH | MAP_SIMPLIFIED | MAP_TRADITIONAL)

/* The letter without its accent, for the Latin-1 letters (0 if it has none). */
static WCHAR nls_unaccent(WCHAR c) {
  if (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7) {
    const WCHAR base = (WCHAR)k_latin1_base[(c - 0xC0) & 0x1F];
    if (base == '*' || base == '?') return c;
    return c >= 0xE0 ? (WCHAR)(base | 0x20) : base;
  }
  return c;
}
static BOOL nls_nonspacing(WCHAR c) { return c >= 0x300 && c <= 0x36F; }

static int nls_sort_key_raw(const WCHAR *src, int count, DWORD flags, BYTE *out, int capacity) {
  int n = 0;
  #define KEY(b) do { if (out && n < capacity) out[n] = (BYTE)(b); ++n; } while (0)
  for (int pass = 0; pass < 4; ++pass) {
    if (pass == 1 && (flags & (CMP_IGNORENONSPACE | CMP_LINGUISTIC_IGNOREDIACRITIC))) continue;
    if (pass == 3 && (flags & CMP_STRINGSORT)) continue;
    const WCHAR *p = src, *end = src + count;
    SortWeight w;
    while (sort_next(&p, end, flags, &w, pass == 3)) {
      if (pass == 0 || pass == 3) { KEY((w.primary + 0x100) >> 8); KEY((w.primary + 0x100) & 0xFF); }
      else if (pass == 1) { KEY((w.accent >> 8) + 2); KEY((w.accent & 0xFF) + 2); }
      else KEY((w.upper && !(flags & (CMP_IGNORECASE | CMP_LINGUISTIC_IGNORECASE))) ? 3 : 2);
    }
    KEY(1);
  }
  KEY(0);
  #undef KEY
  return n;
}
static int nls_sort_key(const WCHAR *src, int count, DWORD flags, BYTE *out, int capacity) {
  INT room = count * 18 + 8;
  WCHAR *decomposed = count > 0 ? RtlAllocateHeap(heap(), 0, (SIZE_T)room * 2) : NULL;
  int n;
  if (decomposed && RtlNormalizeString(2, src, count, decomposed, &room) == STATUS_SUCCESS) n = nls_sort_key_raw(decomposed, room, flags, out, capacity);
  else n = nls_sort_key_raw(src, count, flags, out, capacity);
  if (decomposed) RtlFreeHeap(heap(), 0, decomposed);
  return n;
}

/* ---- LCMapString ------------------------------------------------------------------------------------------------------------------------------------ */

/* The result of a mapping: the units are counted whatever the capacity, and only those that fit are written. */
typedef struct { WCHAR *dst; int capacity, length; } LcOut;
static void lc_put(LcOut *o, DWORD cp) {
  const int units = cp >= 0x10000 ? 2 : 1;
  if (o->dst && o->capacity && o->length + units <= o->capacity) {
    if (units == 2) { o->dst[o->length] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)); o->dst[o->length + 1] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF)); }
    else o->dst[o->length] = (WCHAR)cp;
  }
  o->length += units;
}

/* The simple case mappings, with the exceptions of the languages: Turkish and Azeri have the dotted and dotless i, and with LCMAP_LINGUISTIC_CASING the dotted capital and
 * the dotless small i map to the plain letters. */
static DWORD lc_case(DWORD cp, BOOL upper, BOOL linguistic, BOOL turkic) {
  if (linguistic) {
    if (turkic && upper && cp == 'i') return 0x130;
    if (turkic && !upper && cp == 'I') return 0x131;
    if (!upper && cp == 0x130) return 'i';
    if (upper && cp == 0x131) return 'I';
  }
  if (cp < 0x10000) return upper ? wide_upper((WCHAR)cp) : wide_lower((WCHAR)cp);
  SIZE_T lo = 0, hi = sizeof(k_case_supplementary) / sizeof(k_case_supplementary[0]);
  while (lo < hi) {
    const SIZE_T mid = lo + (hi - lo) / 2;
    if (k_case_supplementary[mid].cp < cp) lo = mid + 1; else if (k_case_supplementary[mid].cp > cp) hi = mid;
    else return upper ? k_case_supplementary[mid].upper : k_case_supplementary[mid].lower;
  }
  return cp;
}
static DWORD lc_hiragana_katakana(DWORD c, BOOL to_katakana) {
  if (to_katakana) { if ((c >= 0x3041 && c <= 0x3096) || c == 0x309D || c == 0x309E) return c + 0x60; }
  else if ((c >= 0x30A1 && c <= 0x30F6) || c == 0x30FD || c == 0x30FE) return c - 0x60;
  return c;
}
/* A half-width katakana (and a mark after it) as a full-width character; *used is the number of characters taken. */
static DWORD lc_to_fullwidth(const WCHAR *s, int left, int *used) {
  const DWORD c = s[0];
  *used = 1;
  if (c >= 0xFF61 && c <= 0xFF9F) {
    const DWORD f = k_hw_to_fw[c - 0xFF61];
    if (left > 1 && (s[1] == 0xFF9E || s[1] == 0xFF9F)) {
      for (SIZE_T i = 0; i < sizeof(k_kana_voiced) / sizeof(k_kana_voiced[0]); ++i)
        if (k_kana_voiced[i].base == f && k_kana_voiced[i].mark == s[1]) { *used = 2; return k_kana_voiced[i].result; }
    }
    return f;
  }
  if (c >= 0x21 && c <= 0x7E) return c + 0xFEE0;
  if (c == ' ') return 0x3000;
  if (c >= 0xFFE0 && c <= 0xFFE6) return c;
  switch (c) { case 0xA2: return 0xFFE0; case 0xA3: return 0xFFE1; case 0xAC: return 0xFFE2; case 0xAF: return 0xFFE3; case 0xA6: return 0xFFE4; case 0xA5: return 0xFFE5; case 0x20A9: return 0xFFE6; default: break; }
  return c;
}
/* The half-width form(s) of a character: up to two. */
static int lc_to_halfwidth(DWORD c, DWORD *out) {
  if (c >= 0xFF01 && c <= 0xFF5E) { out[0] = c - 0xFEE0; return 1; }
  if (c == 0x3000) { out[0] = ' '; return 1; }
  switch (c) { case 0xFFE0: out[0] = 0xA2; return 1; case 0xFFE1: out[0] = 0xA3; return 1; case 0xFFE2: out[0] = 0xAC; return 1; case 0xFFE3: out[0] = 0xAF; return 1; case 0xFFE4: out[0] = 0xA6; return 1; case 0xFFE5: out[0] = 0xA5; return 1; case 0xFFE6: out[0] = 0x20A9; return 1; default: break; }
  SIZE_T lo = 0, hi = sizeof(k_fw_to_hw) / sizeof(k_fw_to_hw[0]);
  while (lo < hi) {
    const SIZE_T mid = lo + (hi - lo) / 2;
    if (k_fw_to_hw[mid].full < c) lo = mid + 1; else if (k_fw_to_hw[mid].full > c) hi = mid;
    else { out[0] = k_fw_to_hw[mid].half; if (k_fw_to_hw[mid].mark) { out[1] = k_fw_to_hw[mid].mark; return 2; } return 1; }
  }
  out[0] = c;
  return 1;
}

/* Whether a language uses the Turkish casing of the i. */
static BOOL lc_turkic(int kind) {
  const char *iso = nls_text_of(kind, 0x59);
  return iso && ((iso[0] == 't' && iso[1] == 'r') || (iso[0] == 'a' && iso[1] == 'z')) && !iso[2];
}

static int nls_lcmap(int kind, BOOL ansi, DWORD flags, const WCHAR *src, int count, WCHAR *dst, int capacity) {
  if (!src || count == 0 || capacity < 0 || (capacity > 0 && !dst)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const DWORD mapping = flags & MAP_ALL, norm = flags & (CMP_IGNORECASE | CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS | CMP_STRINGSORT | CMP_IGNOREKANATYPE | CMP_IGNOREWIDTH | CMP_LINGUISTIC_IGNORECASE | CMP_LINGUISTIC_IGNOREDIACRITIC);
  const DWORD known = MAP_ALL | MAP_HASH | MAP_LINGUISTIC_CASING | MAP_SORTHANDLE | CMP_IGNORECASE | CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS | CMP_STRINGSORT | CMP_IGNOREKANATYPE | CMP_IGNOREWIDTH | CMP_LINGUISTIC_IGNORECASE | CMP_LINGUISTIC_IGNOREDIACRITIC | LCT_USE_CP_ACP;
  const BOOL titlecase = (mapping & (MAP_LOWERCASE | MAP_UPPERCASE)) == (MAP_LOWERCASE | MAP_UPPERCASE);
  if ((flags & ~known) || flags == 0 || (mapping & MAP_SORTKEY && (mapping & ~MAP_SORTKEY))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (ansi && titlecase) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (((mapping & MAP_HIRAGANA) && (mapping & MAP_KATAKANA)) || ((mapping & MAP_HALFWIDTH) && (mapping & MAP_FULLWIDTH)) || ((mapping & MAP_SIMPLIFIED) && (mapping & MAP_TRADITIONAL))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if ((mapping & (MAP_LOWERCASE | MAP_UPPERCASE)) && (flags & (CMP_STRINGSORT | CMP_IGNORESYMBOLS | CMP_IGNORENONSPACE))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if ((mapping & (MAP_HIRAGANA | MAP_KATAKANA | MAP_HALFWIDTH | MAP_FULLWIDTH)) && ((mapping & (MAP_SIMPLIFIED | MAP_TRADITIONAL)) || (flags & (CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS)))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (mapping == 0 && (norm & ~(CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (count < 0) count = (int)wcslen(src) + 1;
  if (dst && dst == src && !(mapping & (MAP_LOWERCASE | MAP_UPPERCASE))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (dst && dst == src && (mapping & ~(MAP_LOWERCASE | MAP_UPPERCASE))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (mapping & MAP_SORTKEY) {
    BYTE *bytes = (BYTE *)dst;
    const int needed = nls_sort_key(src, count, flags, NULL, 0);
    if (capacity == 0) return needed;
    if (capacity < needed) {
      /* the part of the key that fits is written, up to the last separator in it */
      BYTE full[1024];
      int upto = 0;
      if (needed <= (int)sizeof(full)) {
        nls_sort_key(src, count, flags, full, sizeof(full));
        for (int i = 0; i < capacity; ++i) if (full[i] == 1) upto = i + 1;
        for (int i = 0; i < upto; ++i) bytes[i] = full[i];
      }
      set_error(ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    return nls_sort_key(src, count, flags, bytes, capacity);
  }
  const BOOL linguistic = (flags & MAP_LINGUISTIC_CASING) != 0, turkic = lc_turkic(kind);
  LcOut o = { dst, capacity, 0 };
  BOOL word_start = TRUE;
  for (int i = 0; i < count;) {
    DWORD c = src[i];
    int used = 1;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < count && (src[i + 1] & 0xFC00) == 0xDC00) { c = 0x10000 + ((c - 0xD800) << 10) + (src[i + 1] - 0xDC00); used = 2; }
    if (c < 0x10000) {
      if ((flags & CMP_IGNORENONSPACE) && nls_nonspacing((WCHAR)c)) { i += used; continue; }
      if (flags & CMP_IGNORENONSPACE) c = nls_unaccent((WCHAR)c);
      if (flags & CMP_IGNORESYMBOLS) {
        SortWeight w;
        sort_weight_of((WCHAR)c, 0, &w);
        if (w.primary < 0x1000 && c != 0) { i += used; continue; }
      }
    }
    if (titlecase) {
      const DWORD up = lc_case(c, TRUE, linguistic, turkic), lo = lc_case(c, FALSE, linguistic, turkic);
      const BOOL letter = up != lo;
      c = letter ? (word_start ? up : lo) : c;
      word_start = !letter;
    } else if (mapping & MAP_LOWERCASE) c = lc_case(c, FALSE, linguistic, turkic);
    else if (mapping & MAP_UPPERCASE) c = lc_case(c, TRUE, linguistic, turkic);
    /* the full-width forms come first for FULLWIDTH, the kana first for HALFWIDTH: the order of the conversions of Windows */
    if (mapping & MAP_FULLWIDTH) {
      if (c < 0x10000) {
        int taken;
        WCHAR pair[2] = { (WCHAR)c, (WCHAR)(i + used < count ? src[i + used] : 0) };
        c = lc_to_fullwidth(pair, 1 + (pair[1] ? 1 : 0), &taken);
        used += taken - 1;
      } else {
        /* characters of the other planes that have a plain form (mathematical letters, compatibility ideographs) */
        WCHAR from[2] = { (WCHAR)(0xD800 + ((c - 0x10000) >> 10)), (WCHAR)(0xDC00 + ((c - 0x10000) & 0x3FF)) }, to[8];
        INT room = 8;
        if (RtlNormalizeString(5, from, 2, to, &room) == STATUS_SUCCESS && room == 1) c = to[0];
      }
    }
    if (mapping & MAP_HIRAGANA) c = lc_hiragana_katakana(c, FALSE);
    if (mapping & MAP_KATAKANA) c = lc_hiragana_katakana(c, TRUE);
    if (mapping & MAP_HALFWIDTH) {
      DWORD half[2];
      const int n = c < 0x10000 ? lc_to_halfwidth(c, half) : (half[0] = c, 1);
      for (int k = 0; k < n; ++k) {
        DWORD h = half[k];
        if (mapping & MAP_BYTEREV) h = (h << 8 | h >> 8) & 0xFFFF;
        lc_put(&o, h);
      }
      i += used;
      continue;
    }
    if (mapping & MAP_BYTEREV) c = (c << 8 | c >> 8) & 0xFFFF;
    lc_put(&o, c);
    i += used;
  }
  if (capacity == 0) return o.length;
  if (o.length > capacity) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  return o.length;
}
EXPORT int WINAPI LCMapStringW(LCID locale, DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity) {
  if (nls_locale_of(locale) == 0 && locale != 0x40C && locale != 0x411) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  return nls_lcmap(nls_locale_of(locale), FALSE, flags, source, count, dest, capacity);
}
EXPORT int WINAPI LCMapStringEx(LPCWSTR name, DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity, LPVOID version, LPVOID reserved, LPARAM sort_handle) {
  (void)version; (void)reserved; (void)sort_handle;
  int kind = nls_locale_of_name(name);
  /* a name that is no locale only matters where the locale is used: for the cases and the sort keys */
  if (kind == 0) {
    if (flags & (MAP_LOWERCASE | MAP_UPPERCASE | MAP_SORTKEY)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    kind = nls_default_kind();
  }
  return nls_lcmap(kind, FALSE, flags & ~MAP_SORTHANDLE, source, count, dest, capacity);
}
EXPORT int WINAPI LCMapStringA(LCID locale, DWORD flags, LPCSTR source, int count, LPSTR dest, int capacity) {
  if (!source || count == 0 || capacity < 0 || (capacity > 0 && !dest)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (count < 0) count = (int)strlen(source) + 1;
  if (dest == source && !(flags & (MAP_LOWERCASE | MAP_UPPERCASE))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if ((flags & (MAP_LOWERCASE | MAP_UPPERCASE)) == (MAP_LOWERCASE | MAP_UPPERCASE)) { set_error(ERROR_INVALID_FLAGS); return 0; }
  const int wide_count = MultiByteToWideChar(CP_UTF8, 0, source, count, NULL, 0);
  WCHAR *w = RtlAllocateHeap(heap(), 0, ((SIZE_T)wide_count + 1) * 2);
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  MultiByteToWideChar(CP_UTF8, 0, source, count, w, wide_count);
  int result = 0;
  if (flags & MAP_SORTKEY) {
    result = LCMapStringW(locale, flags, w, wide_count, (LPWSTR)dest, capacity);
  } else {
    const int cap_wide = wide_count * 2 + 2;
    WCHAR *mapped = RtlAllocateHeap(heap(), 0, (SIZE_T)cap_wide * 2);
    if (!mapped) set_error(ERROR_NOT_ENOUGH_MEMORY);
    else {
      const int m = LCMapStringW(locale, flags, w, wide_count, mapped, cap_wide);
      if (m > 0) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, mapped, m, NULL, 0, NULL, NULL);
        if (capacity == 0) result = needed;
        else if (capacity < needed) set_error(ERROR_INSUFFICIENT_BUFFER);
        else result = WideCharToMultiByte(CP_UTF8, 0, mapped, m, dest, capacity, NULL, NULL);
      }
      RtlFreeHeap(heap(), 0, mapped);
    }
  }
  RtlFreeHeap(heap(), 0, w);
  return result;
}

/* ---- locale names ------------------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT int WINAPI LCIDToLocaleName(LCID lcid, LPWSTR name, int capacity, DWORD flags) {
  (void)flags;
  if (capacity < 0 || (capacity > 0 && !name)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const int kind = nls_locale_of(lcid);
  WCHAR text[80];
  if (kind == 0 || !nls_text_wide(kind, 0x5C, text)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const int n = (int)wcslen(text) + 1;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  memcpy(name, text, (size_t_)n * 2);
  return n;
}
/* The identifier of the locale of a name; a neutral name gives the identifier of its specific locale unless LOCALE_ALLOW_NEUTRAL_NAMES is set (zh-Hant and zh-Hans are neutral). */
EXPORT LCID WINAPI LocaleNameToLCID(LPCWSTR name, DWORD flags) {
  if (flags & ~(0x08000000u | 0x80000000u | 0x2000u)) { set_error(ERROR_INVALID_FLAGS); return 0; }
  const int kind = nls_locale_of_name(name);
  if (kind == 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (nls_kind_is_neutral(kind) && !(flags & 0x08000000)) return k_locale_lcid[k_locale_default[kind - 1]];
  return k_locale_lcid[kind - 1];
}
EXPORT BOOL WINAPI IsValidLocaleName(LPCWSTR name) { return name && nls_locale_of_name(name) != 0 && !nls_name_is(name, "!x-sys-default-locale"); }
/* A locale identifier that is in the table as it is (the "default" identifiers are not locales). */
EXPORT BOOL WINAPI IsValidLocale(LCID lcid, DWORD flags) {
  if (flags > 2) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  return nls_index_of_lcid(lcid) >= 0;
}
EXPORT LCID WINAPI ConvertDefaultLocale(LCID lcid) {
  switch (lcid) {
    case 0: case 0x400: case 0x800: case 0xC00: case 0x1000: case 0x1400: return NLS_LCID_EN_US;
    default: break;
  }
  /* a neutral locale becomes the default specific one of its language */
  const int index = nls_index_of_lcid(lcid);
  if (index >= 0 && k_locale_default[index] != index) return k_locale_lcid[k_locale_default[index]];
  return lcid;
}
/* The locale a name stands for: the name as it is, or cut back at the last '-' or '_' until it names a locale; a neutral locale gives its specific one and the sort
 * order is dropped ("de-DE_phoneb" is "de-DE"). A name that has anything but letters, digits, hyphens and underscores is refused. */
EXPORT int WINAPI ResolveLocaleName(LPCWSTR name, LPWSTR out, int capacity) {
  if (capacity < 0 || (capacity > 0 && !out)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  int kind;
  if (name && nls_name_is(name, "!x-sys-default-locale")) kind = nls_default_kind();
  else if (!name) kind = nls_default_kind();
  else {
    WCHAR work[LOCALE_NAME_MAX_LENGTH_];
    int n = 0;
    for (; name[n]; ++n) {
      const WCHAR c = name[n];
      if (n >= LOCALE_NAME_MAX_LENGTH_ - 1 || !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_')) { set_error(ERROR_INVALID_PARAMETER); return 0; }
      work[n] = c;
    }
    work[n] = 0;
    kind = n == 0 ? nls_invariant_kind() : 0;
    while (kind == 0 && n > 0) {
      kind = nls_locale_of_name(work);
      if (kind) break;
      while (n > 0 && work[n - 1] != '-' && work[n - 1] != '_') --n;
      if (n > 0) --n;
      work[n] = 0;
    }
    if (kind == 0) kind = nls_invariant_kind();
    else if (nls_kind_is_neutral(kind)) kind = k_locale_default[kind - 1] + 1;
  }
  WCHAR text[80];
  nls_text_wide(kind, 0x5C, text);
  for (int i = 0; text[i]; ++i) if (text[i] == '_') text[i] = 0;
  const int n = (int)wcslen(text) + 1;
  if (capacity == 0) return n;
  if (capacity < n) { if (capacity > 0) { memcpy(out, text, (size_t_)(capacity - 1) * 2); out[capacity - 1] = 0; } set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  memcpy(out, text, (size_t_)n * 2);
  return n;
}

/* ---- lists of date and time formats ----------------------------------------------------------------------------------------------------------------------- */

static const char *const k_short_dates[] = {"M/d/yyyy", "M/d/yy", "MM/dd/yy", "MM/dd/yyyy", "yy/MM/dd", "yyyy-MM-dd", "dd-MMM-yy", NULL};
static const char *const k_long_dates[] = {"dddd, MMMM d, yyyy", "MMMM d, yyyy", "dddd, d MMMM, yyyy", "d MMMM, yyyy", NULL};
static const char *const k_year_months[] = {"MMMM yyyy", "MMMM, yyyy", NULL};
static const char *const k_times[] = {"h:mm:ss tt", "hh:mm:ss tt", "H:mm:ss", "HH:mm:ss", NULL};
static const char *const k_times_short[] = {"h:mm tt", "hh:mm tt", "H:mm", "HH:mm", NULL};

typedef BOOL (CALLBACK *DATEFMT_ENUMPROCA_)(LPSTR);
typedef BOOL (CALLBACK *DATEFMT_ENUMPROCW_)(LPWSTR);
typedef BOOL (CALLBACK *DATEFMT_ENUMPROCEXA_)(LPSTR, DWORD);
typedef BOOL (CALLBACK *DATEFMT_ENUMPROCEXW_)(LPWSTR, DWORD);
typedef BOOL (CALLBACK *DATEFMT_ENUMPROCEXEX_)(LPWSTR, DWORD, LPARAM);
typedef BOOL (CALLBACK *TIMEFMT_ENUMPROCEX_)(LPWSTR, LPARAM);

/* Calls `call(text)` (wide) for each of the formats until it answers FALSE. */
typedef BOOL (*nls_each_fn)(const WCHAR *text, void *context);
static BOOL nls_info_text(int kind, DWORD type, BOOL user_override, WCHAR *out /* 80 */);
static BOOL nls_each_format_for(int kind, const char *const *list, DWORD info_type, nls_each_fn call, void *context);
static BOOL nls_each_format(const char *const *list, nls_each_fn call, void *context) {
  for (; *list; ++list) {
    WCHAR text[40];
    nls_widen(text, *list);
    if (!call(text, context)) break;
  }
  return TRUE;
}
/* The formats of a locale: the English (United States) ones come as a list, any other locale has its own format. */
static BOOL nls_each_format_for(int kind, const char *const *list, DWORD info_type, nls_each_fn call, void *context) {
  if (kind == nls_default_kind()) return nls_each_format(list, call, context);
  WCHAR text[80];
  if (nls_info_text(kind, info_type, FALSE, text)) call(text, context);
  return TRUE;
}
static const char *const *nls_date_list(DWORD flags) {
  if (flags & NLS_DATE_LONGDATE) return k_long_dates;
  if (flags & NLS_DATE_YEARMONTH) return k_year_months;
  return k_short_dates;
}
static BOOL nls_date_flags_ok(DWORD flags) {
  flags &= ~(LCT_USE_CP_ACP | NLS_NOUSEROVERRIDE);
  return flags == 0 || flags == NLS_DATE_SHORTDATE || flags == NLS_DATE_LONGDATE || flags == NLS_DATE_YEARMONTH;
}
static BOOL nls_call_narrow(const WCHAR *text, int mode, void *proc, DWORD calendar, LPARAM param) {
  char narrow[80];
  WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof(narrow), NULL, NULL);
  if (mode == 0) return ((DATEFMT_ENUMPROCA_)proc)(narrow);
  return ((DATEFMT_ENUMPROCEXA_)proc)(narrow, calendar);
}
typedef struct { void *proc; int mode; DWORD calendar; LPARAM param; } NlsEnumContext;
static BOOL nls_each_a(const WCHAR *text, void *context) { NlsEnumContext *c = context; return nls_call_narrow(text, c->mode, c->proc, c->calendar, c->param); }
static BOOL nls_each_w(const WCHAR *text, void *context) {
  NlsEnumContext *c = context;
  switch (c->mode) {
    case 0: return ((DATEFMT_ENUMPROCW_)c->proc)((LPWSTR)text);
    case 1: return ((DATEFMT_ENUMPROCEXW_)c->proc)((LPWSTR)text, c->calendar);
    case 2: return ((DATEFMT_ENUMPROCEXEX_)c->proc)((LPWSTR)text, c->calendar, c->param);
    default: return ((TIMEFMT_ENUMPROCEX_)c->proc)((LPWSTR)text, c->param);
  }
}
static BOOL nls_enum_check(int kind, void *proc, BOOL flags_ok) {
  if (!flags_ok) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (kind == 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI EnumDateFormatsA(DATEFMT_ENUMPROCA_ proc, LCID locale, DWORD flags) {
  const int kind = nls_locale_of(locale);
  if (!nls_enum_check(kind, (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 0, 1, 0};
  return nls_each_format_for(kind, nls_date_list(flags), ((flags & NLS_DATE_LONGDATE) ? 0x20u : (flags & NLS_DATE_YEARMONTH) ? 0x1006u : 0x1Fu), nls_each_a, &c);
}
EXPORT BOOL WINAPI EnumDateFormatsW(DATEFMT_ENUMPROCW_ proc, LCID locale, DWORD flags) {
  const int kind = nls_locale_of(locale);
  if (!nls_enum_check(kind, (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 0, 1, 0};
  return nls_each_format_for(kind, nls_date_list(flags), ((flags & NLS_DATE_LONGDATE) ? 0x20u : (flags & NLS_DATE_YEARMONTH) ? 0x1006u : 0x1Fu), nls_each_w, &c);
}
EXPORT BOOL WINAPI EnumDateFormatsExA(DATEFMT_ENUMPROCEXA_ proc, LCID locale, DWORD flags) {
  const int kind = nls_locale_of(locale);
  if (!nls_enum_check(kind, (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 1, 1, 0};
  return nls_each_format_for(kind, nls_date_list(flags), ((flags & NLS_DATE_LONGDATE) ? 0x20u : (flags & NLS_DATE_YEARMONTH) ? 0x1006u : 0x1Fu), nls_each_a, &c);
}
EXPORT BOOL WINAPI EnumDateFormatsExW(DATEFMT_ENUMPROCEXW_ proc, LCID locale, DWORD flags) {
  const int kind = nls_locale_of(locale);
  if (!nls_enum_check(kind, (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 1, 1, 0};
  return nls_each_format_for(kind, nls_date_list(flags), ((flags & NLS_DATE_LONGDATE) ? 0x20u : (flags & NLS_DATE_YEARMONTH) ? 0x1006u : 0x1Fu), nls_each_w, &c);
}
EXPORT BOOL WINAPI EnumDateFormatsExEx(DATEFMT_ENUMPROCEXEX_ proc, LPCWSTR name, DWORD flags, LPARAM param) {
  const int kind = nls_locale_of_name(name);
  if (!nls_enum_check(kind, (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 2, 1, param};
  return nls_each_format_for(kind, nls_date_list(flags), ((flags & NLS_DATE_LONGDATE) ? 0x20u : (flags & NLS_DATE_YEARMONTH) ? 0x1006u : 0x1Fu), nls_each_w, &c);
}
static BOOL nls_time_flags_ok(DWORD flags) { flags &= ~(LCT_USE_CP_ACP | NLS_NOUSEROVERRIDE); return flags == 0 || flags == NLS_TIME_NOSECONDS; }
EXPORT BOOL WINAPI EnumTimeFormatsA(DATEFMT_ENUMPROCA_ proc, LCID locale, DWORD flags) {
  const int kind = nls_locale_of(locale);
  if (!nls_enum_check(kind, (void *)proc, nls_time_flags_ok(flags) && !(flags & NLS_TIME_NOSECONDS))) return FALSE;
  NlsEnumContext c = {(void *)proc, 0, 0, 0};
  return nls_each_format_for(kind, (flags & NLS_TIME_NOSECONDS) ? k_times_short : k_times, ((flags & NLS_TIME_NOSECONDS) ? 0x79u : 0x1003u), nls_each_a, &c);
}
EXPORT BOOL WINAPI EnumTimeFormatsW(DATEFMT_ENUMPROCW_ proc, LCID locale, DWORD flags) {
  const int kind = nls_locale_of(locale);
  if (!nls_enum_check(kind, (void *)proc, nls_time_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 0, 0, 0};
  return nls_each_format_for(kind, (flags & NLS_TIME_NOSECONDS) ? k_times_short : k_times, ((flags & NLS_TIME_NOSECONDS) ? 0x79u : 0x1003u), nls_each_w, &c);
}
EXPORT BOOL WINAPI EnumTimeFormatsEx(TIMEFMT_ENUMPROCEX_ proc, LPCWSTR name, DWORD flags, LPARAM param) {
  const int kind = nls_locale_of_name(name);
  if (!nls_enum_check(kind, (void *)proc, nls_time_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 3, 0, param};
  return nls_each_format_for(kind, (flags & NLS_TIME_NOSECONDS) ? k_times_short : k_times, ((flags & NLS_TIME_NOSECONDS) ? 0x79u : 0x1003u), nls_each_w, &c);
}

/* ---- the calendar ------------------------------------------------------------------------------------------------------------------------------------------- */

#define CALENDAR_RETURN_NUMBER 0x20000000u
#define CALENDAR_ALL 0xFFFFFFFFu
/* The text of a CAL_ value for the Gregorian calendars (1 and 2); the numbers are written as text. FALSE if the calendar has no such value. */
static BOOL nls_calendar_text(int kind, DWORD calendar, DWORD type, WCHAR *out /* 80 */) {
  const DWORD t = type & 0xFFFF;
  if (t == 0x30 /* CAL_ITWODIGITYEARMAX */) { nls_widen(out, "2029"); return TRUE; }
  if (calendar != 1 && calendar != 2) return FALSE;
  WCHAR scratch[80];
  const char *text = NULL;
  switch (t) {
    case 0x01: text = calendar == 2 ? "2" : "1"; break;
    case 0x02: text = calendar == 2 ? "Gregorian Calendar (U.S.)" : "Gregorian Calendar"; break;
    case 0x03: text = "0"; break;
    case 0x04: text = "A.D."; break;
    case 0x39: text = "AD"; break;
    default: break;
  }
  switch (t) {
    case 0x05: return nls_text_wide(kind, 0x1F, out);
    case 0x06: return nls_text_wide(kind, 0x20, out);
    case 0x2F: return nls_text_wide(kind, 0x1006, out);
    case 0x38: return nls_text_wide(kind, 0x78, out);
    case 0x3A: return nls_text_wide(kind, 0x7C, out);
    default: break;
  }
  if (text) { nls_widen(out, text); return TRUE; }
  if (t >= 0x07 && t <= 0x0D) { nls_day_name(kind, (int)((t - 0x07 + 1) % 7), FALSE, out); return TRUE; }
  if (t >= 0x0E && t <= 0x14) { nls_day_name(kind, (int)((t - 0x0E + 1) % 7), TRUE, out); return TRUE; }
  if (t >= 0x15 && t <= 0x20) { nls_month_name(kind, (int)(t - 0x15 + 1), FALSE, out); return TRUE; }
  if (t == 0x21) { out[0] = 0; return TRUE; }
  if (t >= 0x22 && t <= 0x2D) { nls_month_name(kind, (int)(t - 0x22 + 1), TRUE, out); return TRUE; }
  if (t == 0x2E) { out[0] = 0; return TRUE; }
  if (t >= 0x31 && t <= 0x37) { nls_day_name(kind, (int)((t - 0x31 + 1) % 7), TRUE, scratch); out[0] = scratch[0]; out[1] = scratch[1]; out[2] = 0; return TRUE; }
  return FALSE;
}
static int nls_calendar_info(int kind, DWORD calendar, DWORD type, WCHAR *buffer, int capacity, DWORD *value) {
  WCHAR text[80];
  if (kind == 0 || capacity < 0 || (capacity > 0 && !buffer)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (type & CALENDAR_RETURN_NUMBER) {
    if (!value) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!nls_calendar_text(kind, calendar, type, text)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD v = 0;
    for (const WCHAR *c = text; *c >= '0' && *c <= '9'; ++c) v = v * 10 + (DWORD)(*c - '0');
    *value = v;
    return 2;
  }
  if (!nls_calendar_text(kind, calendar, type, text)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const int n = (int)wcslen(text) + 1;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  memcpy(buffer, text, (size_t_)n * 2);
  return n;
}
EXPORT int WINAPI GetCalendarInfoW(LCID locale, DWORD calendar, DWORD type, LPWSTR buffer, int capacity, LPDWORD value) {
  return nls_calendar_info(nls_locale_of(locale), calendar, type, buffer, capacity, value);
}
EXPORT int WINAPI GetCalendarInfoEx(LPCWSTR name, DWORD calendar, LPCWSTR reserved, DWORD type, LPWSTR buffer, int capacity, LPDWORD value) {
  if (reserved) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  return nls_calendar_info(nls_locale_of_name(name), calendar, type, buffer, capacity, value);
}
EXPORT int WINAPI GetCalendarInfoA(LCID locale, DWORD calendar, DWORD type, LPSTR buffer, int capacity, LPDWORD value) {
  if (capacity < 0 || (capacity > 0 && !buffer)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (type & CALENDAR_RETURN_NUMBER) return nls_calendar_info(nls_locale_of(locale), calendar, type, NULL, 0, value) ? 4 : 0;
  WCHAR wide[80];
  const int n = nls_calendar_info(nls_locale_of(locale), calendar, type, wide, 80, value);
  return nls_narrow_result(n, wide, buffer, capacity);
}

typedef BOOL (CALLBACK *CALINFO_ENUMPROCA_)(LPSTR);
typedef BOOL (CALLBACK *CALINFO_ENUMPROCW_)(LPWSTR);
typedef BOOL (CALLBACK *CALINFO_ENUMPROCEXA_)(LPSTR, DWORD);
typedef BOOL (CALLBACK *CALINFO_ENUMPROCEXW_)(LPWSTR, DWORD);
typedef BOOL (CALLBACK *CALINFO_ENUMPROCEXEX_)(LPWSTR, DWORD, LPWSTR, LPARAM);
/* The values of one calendar value type: a list for the date formats, one value for the rest. mode: 0 narrow, 1 narrow+calendar, 2 wide, 3 wide+calendar, 4 wide+all. */
static BOOL nls_enum_calendar(int kind, DWORD calendar, DWORD type, int mode, void *proc, LPARAM param) {
  if (kind == 0 || !proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD calendars[2] = {1, 2};
  for (int i = 0; i < 2; ++i) {
    if (calendar != CALENDAR_ALL && calendar != calendars[i]) continue;
    if (calendar == CALENDAR_ALL && i == 1) break;      /* (the U.S. variant is not offered as an alternate here) */
    WCHAR one[80];
    const char *const *list = NULL;
    const DWORD t = type & 0xFFFF;
    if (t == 0x05) list = k_short_dates; else if (t == 0x06) list = k_long_dates; else if (t == 0x2F) list = k_year_months;
    WCHAR items[8][40];
    int count = 0;
    if (list) for (; list[count] && count < 8; ++count) nls_widen(items[count], list[count]);
    else {
      if (!nls_calendar_text(kind, calendars[i], type, one)) { if (calendar == CALENDAR_ALL) continue; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
      if (type & CALENDAR_RETURN_NUMBER) { DWORD v = 0; for (const WCHAR *c = one; *c >= '0' && *c <= '9'; ++c) v = v * 10 + (DWORD)(*c - '0'); WCHAR num[12]; int k = 0; WCHAR digits[12]; int d = 0; do { digits[d++] = (WCHAR)('0' + v % 10); v /= 10; } while (v); while (d) num[k++] = digits[--d]; num[k] = 0; memcpy(items[0], num, ((size_t_)k + 1) * 2); }
      else memcpy(items[0], one, ((size_t_)wcslen(one) + 1) * 2);
      count = 1;
    }
    for (int k = 0; k < count; ++k) {
      BOOL more;
      char narrow[80];
      switch (mode) {
        case 0: WideCharToMultiByte(CP_UTF8, 0, items[k], -1, narrow, sizeof(narrow), NULL, NULL); more = ((CALINFO_ENUMPROCA_)proc)(narrow); break;
        case 1: WideCharToMultiByte(CP_UTF8, 0, items[k], -1, narrow, sizeof(narrow), NULL, NULL); more = ((CALINFO_ENUMPROCEXA_)proc)(narrow, calendars[i]); break;
        case 2: more = ((CALINFO_ENUMPROCW_)proc)(items[k]); break;
        case 3: more = ((CALINFO_ENUMPROCEXW_)proc)(items[k], calendars[i]); break;
        default: more = ((CALINFO_ENUMPROCEXEX_)proc)(items[k], calendars[i], NULL, param); break;
      }
      if (!more) return TRUE;
    }
  }
  return TRUE;
}
EXPORT BOOL WINAPI EnumCalendarInfoA(CALINFO_ENUMPROCA_ proc, LCID locale, DWORD calendar, DWORD type) { return nls_enum_calendar(nls_locale_of(locale), calendar, type, 0, (void *)proc, 0); }
EXPORT BOOL WINAPI EnumCalendarInfoW(CALINFO_ENUMPROCW_ proc, LCID locale, DWORD calendar, DWORD type) { return nls_enum_calendar(nls_locale_of(locale), calendar, type, 2, (void *)proc, 0); }
EXPORT BOOL WINAPI EnumCalendarInfoExA(CALINFO_ENUMPROCEXA_ proc, LCID locale, DWORD calendar, DWORD type) { return nls_enum_calendar(nls_locale_of(locale), calendar, type, 1, (void *)proc, 0); }
EXPORT BOOL WINAPI EnumCalendarInfoExW(CALINFO_ENUMPROCEXW_ proc, LCID locale, DWORD calendar, DWORD type) { return nls_enum_calendar(nls_locale_of(locale), calendar, type, 3, (void *)proc, 0); }
EXPORT BOOL WINAPI EnumCalendarInfoExEx(CALINFO_ENUMPROCEXEX_ proc, LPCWSTR name, DWORD calendar, LPCWSTR reserved, DWORD type, LPARAM param) {
  if (reserved) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return nls_enum_calendar(nls_locale_of_name(name), calendar, type, 4, (void *)proc, param);
}
