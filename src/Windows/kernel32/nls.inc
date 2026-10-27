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

/* Which locale an LCID names: 0 none, 1 English (United States), 2 the invariant locale. */
static int nls_locale_of(LCID lcid) {
  switch (lcid) {
    case 0: case 0x400: case 0x800: case 0x1000: case 0x2000: case 0x409: case 0x0009: return 1;
    case 0x7F: case 0x1007F: return 2;
    default: break;
  }
  if ((lcid & 0xFFFF) == 0x409) return 1;
  return 0;
}
static WCHAR nls_lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
static BOOL nls_name_is(const WCHAR *name, const char *text) {
  for (; *text; ++text, ++name) if (nls_lower(*name) != (WCHAR)*text) return FALSE;
  return *name == 0;
}
/* The locale a name (NULL: the user's, "": invariant, "en-US", "en") picks: like nls_locale_of(). */
static int nls_locale_of_name(LPCWSTR name) {
  if (!name) return 1;
  if (*name == 0) return 2;
  if (nls_name_is(name, "en-us") || nls_name_is(name, "en") || nls_name_is(name, "!x-sys-default-locale") || nls_name_is(name, "en-us_tradnl") ) return 1;
  return 0;
}

/* The days of the week and months, in wide characters. */
static const WCHAR *nls_day_name(int weekday /* 0 = Sunday */, BOOL abbreviated, WCHAR *scratch) {
  const char *name = k_days[(weekday + 6) % 7];
  int n = 0;
  for (; name[n] && (!abbreviated || n < 3); ++n) scratch[n] = (WCHAR)name[n];
  scratch[n] = 0;
  return scratch;
}
static const WCHAR *nls_month_name(int month, BOOL abbreviated, WCHAR *scratch) {
  const char *name = k_months[(month - 1) % 12];
  int n = 0;
  for (; name[n] && (!abbreviated || n < 3); ++n) scratch[n] = (WCHAR)name[n];
  scratch[n] = 0;
  return scratch;
}

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

static void nls_date_picture(NlsOut *o, const WCHAR *p, const SYSTEMTIME *st) {
  WCHAR scratch[16];
  while (*p) {
    if (*p == '\'') { p = nls_quoted(o, p + 1); continue; }
    const WCHAR c = *p;
    int run = 1;
    while (p[run] == c) ++run;
    switch (c) {
      case 'd': {
        if (run > 4) run = 4;
        if (run <= 2) nls_put_number(o, st->wDay, run);
        else nls_put_text(o, nls_day_name(nls_weekday(st), run == 3, scratch));
        break;
      }
      case 'M': {
        if (run > 4) run = 4;
        if (run <= 2) nls_put_number(o, st->wMonth, run);
        else nls_put_text(o, nls_month_name(st->wMonth, run == 3, scratch));
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

static void nls_time_picture(NlsOut *o, const WCHAR *p, const SYSTEMTIME *st, DWORD flags) {
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
          nls_put(o, st->wHour < 12 ? 'A' : 'P');
          if (width == 2) nls_put(o, 'M');
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

static int nls_date_format(int locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) {
  if (locale == 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const DWORD shapes = flags & (NLS_DATE_SHORTDATE | NLS_DATE_LONGDATE | NLS_DATE_YEARMONTH | NLS_DATE_MONTHDAY);
  if ((flags & ~(NLS_NOUSEROVERRIDE | 0xFF)) || (shapes & (shapes - 1)) || (format && (shapes || (flags & NLS_NOUSEROVERRIDE)))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (capacity < 0 || (capacity > 0 && !out)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  SYSTEMTIME now;
  if (!time) { GetLocalTime(&now); time = &now; }
  if (!nls_valid_date(time)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR picture[40];
  if (!format) {
    nls_widen(picture, (shapes & NLS_DATE_LONGDATE) ? "dddd, MMMM d, yyyy" : (shapes & NLS_DATE_YEARMONTH) ? "MMMM yyyy" : (shapes & NLS_DATE_MONTHDAY) ? "MMMM d" : "M/d/yyyy");
    format = picture;
  }
  NlsOut o;
  o.length = 0; o.mark = 0; o.skipping = FALSE;
  nls_date_picture(&o, format, time);
  return nls_deliver(&o, out, capacity);
}
static int nls_time_format(int locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) {
  if (locale == 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if ((flags & ~(NLS_NOUSEROVERRIDE | 0xF)) || (format && (flags & NLS_NOUSEROVERRIDE))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (capacity < 0 || (capacity > 0 && !out)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  SYSTEMTIME now;
  if (!time) { GetLocalTime(&now); time = &now; }
  if (time->wHour > 23 || time->wMinute > 59 || time->wSecond > 59 || time->wMilliseconds > 999) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR picture[24];
  if (!format) {
    nls_widen(picture, (flags & NLS_TIME_FORCE24HOURFORMAT) ? "HH:mm:ss" : "h:mm:ss tt");
    format = picture;
  }
  NlsOut o;
  o.length = 0; o.mark = 0; o.skipping = FALSE;
  nls_time_picture(&o, format, time, flags);
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
static BOOL nls_info_text(int locale, DWORD type, BOOL user_override, WCHAR *out /* 80 */) {
  const DWORD base = type & 0xFFFF;
  if (user_override && locale == 1) {
    const WCHAR *o = nls_override(base);
    if (o) { int i = 0; for (; o[i] && i < 79; ++i) out[i] = o[i]; out[i] = 0; return TRUE; }
  }
  char scratch[8], number_text[16];
  DWORD number = (DWORD)-1;
  const char *text = locale_text(base, &number, scratch);
  if (!text && number != (DWORD)-1) {
    char *p = number_text + sizeof(number_text) - 1;
    *p = 0;
    DWORD v = number;
    do { *--p = (char)('0' + v % 10); v /= 10; } while (v);
    text = p;
  }
  if (!text) return FALSE;
  int i = 0;
  for (; text[i] && i < 79; ++i) out[i] = (WCHAR)(BYTE)text[i];
  out[i] = 0;
  return TRUE;
}
static BOOL nls_info_is_number(DWORD base) {
  switch (base) {
    case 0x0D: case 0x11: case 0x12: case 0x13: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x4A: case 0x1004: case 0x1007: case 0x100B: case 0x100C: case 0x1010: case 0x05: case 0x09: case 0x0B: case 0x01: case 0x2000 + 0: return TRUE;
    default: return FALSE;
  }
}
EXPORT int WINAPI GetLocaleInfoW(LCID locale, LCTYPE type, LPWSTR buffer, int capacity) {
  const int kind = nls_locale_of(locale);
  if (kind == 0 || capacity < 0 || (capacity > 0 && !buffer)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR text[80];
  const BOOL user = !(type & LCT_NOUSEROVERRIDE);
  if (!nls_info_text(kind, type, user, text)) { set_error(ERROR_INVALID_FLAGS); return 0; }
  const DWORD base = type & 0xFFFF;
  if (type & LCT_RETURN_NUMBER) {
    if (!nls_info_is_number(base)) { set_error(ERROR_INVALID_FLAGS); return 0; }
    DWORD number = 0;
    for (const WCHAR *c = text; *c >= '0' && *c <= '9'; ++c) number = number * 10 + (DWORD)(*c - '0');
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
EXPORT int WINAPI GetLocaleInfoA(LCID locale, LCTYPE type, LPSTR buffer, int capacity) {
  if (type & LCT_RETURN_NUMBER) {
    if (capacity < 0 || (capacity > 0 && !buffer)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (capacity == 0) { return GetLocaleInfoW(locale, type, NULL, 0) ? 4 : 0; }
    if (capacity < 4) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return GetLocaleInfoW(locale, type, (LPWSTR)buffer, 2) ? 4 : 0;
  }
  WCHAR wide[80];
  const int n = GetLocaleInfoW(locale, type, wide, 80);
  if (n == 0) return 0;
  return nls_narrow_result(n, wide, buffer, capacity);
}
EXPORT int WINAPI GetLocaleInfoEx(LPCWSTR name, LCTYPE type, LPWSTR buffer, int capacity) {
  const int kind = nls_locale_of_name(name);
  if (kind == 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  return GetLocaleInfoW(kind == 2 ? 0x7F : 0x409, type, buffer, capacity);
}
static BOOL nls_set_info(LCID locale, LCTYPE type, LPCWSTR value) {
  if (!value) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (nls_locale_of(locale) != 1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD base = type & 0xFFFF;
  switch (base) {
    case 0x1009 /* ICALENDARTYPE */: case 0x19: case 0x1B: case 0x11: case 0x1073 /* IDIGITSUBSTITUTION */: case 0x100C: case 0x100B: case 0x12: case 0x0D: case 0x1C: case 0x1010:
    case 0x100A /* IPAPERSIZE */: case 0x23: case 0x28: case 0x29: case 0x14: case 0x1D: case 0x0E: case 0x10: case 0x0C: case 0x20: case 0x16: case 0x18: case 0x17: case 0x13 + 0x1000:
    case 0x1012 /* SNATIVEDIGITS */: case 0x51: case 0x50: case 0x1F: case 0x1003: case 0x0F: case 0x1E:
      break;
    default: set_error(ERROR_INVALID_PARAMETER); return FALSE;
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

static const char k_sort_symbols[] = "\t\n\v\f\r _'-,;:!?.\"()[]{}@*/\\&#%`^+<=>|~$";
static const char k_latin1_base[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUY?s";
static const unsigned char k_latin1_accent[32] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 5, 1, 2, 3, 5, 9, 4, 1, 2, 3, 4, 5, 0, 10, 1, 2, 3, 5, 2, 0, 11};

static void sort_weight_of(WCHAR c, DWORD flags, SortWeight *w) {
  w->primary = 0; w->accent = 0; w->upper = 0; w->ignorable = FALSE; w->special = FALSE;
  if (c < 0x20 && !(c >= 9 && c <= 13)) { w->ignorable = TRUE; return; }
  if (c == 0x7F || c == 0xAD || (c >= 0x200B && c <= 0x200F) || c == 0xFEFF) { w->ignorable = TRUE; return; }
  if (c >= '0' && c <= '9') { w->primary = 0x1000 + (c - '0'); return; }
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
static BOOL sort_next(const WCHAR **p, const WCHAR *end, DWORD flags, SortWeight *w, BOOL include_special) {
  while (*p < end) {
    sort_weight_of(**p, flags, w);
    ++*p;
    if (w->ignorable) continue;
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

/* The checks and the conversion of the length that CompareString* have in common. */
static int nls_compare(DWORD flags, const WCHAR *a, int an, const WCHAR *b, int bn, BOOL ex) {
  const DWORD valid = CMP_IGNORECASE | CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS | CMP_LINGUISTIC_IGNORECASE | CMP_LINGUISTIC_IGNOREDIACRITIC | CMP_STRINGSORT | CMP_IGNOREKANATYPE | CMP_IGNOREWIDTH | CMP_LINGUISTIC_CASING | LCT_USE_CP_ACP | (ex ? CMP_DIGITSASNUMBERS : 0);
  if (flags & ~valid) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (!a || !b) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (an < 0) an = (int)wcslen(a);
  if (bn < 0) bn = (int)wcslen(b);
  return sort_compare(a, an, b, bn, flags);
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

static int nls_sort_key(const WCHAR *src, int count, DWORD flags, BYTE *out, int capacity) {
  int n = 0;
  #define KEY(b) do { if (out && n < capacity) out[n] = (BYTE)(b); ++n; } while (0)
  for (int pass = 0; pass < 4; ++pass) {
    if (pass == 1 && (flags & (CMP_IGNORENONSPACE | CMP_LINGUISTIC_IGNOREDIACRITIC))) continue;
    if (pass == 2 && (flags & (CMP_IGNORECASE | CMP_LINGUISTIC_IGNORECASE))) continue;
    if (pass == 3 && (flags & CMP_STRINGSORT)) continue;
    const WCHAR *p = src, *end = src + count;
    SortWeight w;
    while (sort_next(&p, end, flags, &w, pass == 3)) {
      if (pass == 0 || pass == 3) { KEY((w.primary + 0x100) >> 8); KEY((w.primary + 0x100) & 0xFF); }
      else if (pass == 1) KEY(w.accent + 2);
      else KEY(w.upper ? 3 : 2);
    }
    KEY(1);
  }
  KEY(0);
  #undef KEY
  return n;
}

static WCHAR nls_kana(WCHAR c, DWORD flags) {
  if ((flags & MAP_HIRAGANA) && c >= 0x30A1 && c <= 0x30F6) return (WCHAR)(c - 0x60);
  if ((flags & MAP_KATAKANA) && c >= 0x3041 && c <= 0x3096) return (WCHAR)(c + 0x60);
  return c;
}
static WCHAR nls_width(WCHAR c, DWORD flags) {
  if ((flags & MAP_FULLWIDTH)) { if (c >= 0x21 && c <= 0x7E) return (WCHAR)(c + 0xFEE0); if (c == ' ') return 0x3000; }
  if ((flags & MAP_HALFWIDTH)) { if (c >= 0xFF01 && c <= 0xFF5E) return (WCHAR)(c - 0xFEE0); if (c == 0x3000) return ' '; }
  return c;
}
static WCHAR nls_title_pending;

static int nls_lcmap(DWORD flags, const WCHAR *src, int count, WCHAR *dst, int capacity) {
  if (!src || count == 0 || capacity < 0 || (capacity > 0 && !dst && !(flags & MAP_SORTKEY) ) ) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (capacity > 0 && !dst) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const DWORD mapping = flags & MAP_ALL, norm = flags & (CMP_IGNORECASE | CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS | CMP_STRINGSORT | CMP_IGNOREKANATYPE | CMP_IGNOREWIDTH | CMP_LINGUISTIC_IGNORECASE | CMP_LINGUISTIC_IGNOREDIACRITIC);
  const DWORD known = MAP_ALL | MAP_HASH | MAP_LINGUISTIC_CASING | MAP_SORTHANDLE | CMP_IGNORECASE | CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS | CMP_STRINGSORT | CMP_IGNOREKANATYPE | CMP_IGNOREWIDTH | CMP_LINGUISTIC_IGNORECASE | CMP_LINGUISTIC_IGNOREDIACRITIC | LCT_USE_CP_ACP;
  const BOOL titlecase = (mapping & (MAP_LOWERCASE | MAP_UPPERCASE)) == (MAP_LOWERCASE | MAP_UPPERCASE);
  if ((flags & ~known) || flags == 0 || (mapping & MAP_SORTKEY && (mapping & ~MAP_SORTKEY))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (((mapping & MAP_HIRAGANA) && (mapping & MAP_KATAKANA)) || ((mapping & MAP_HALFWIDTH) && (mapping & MAP_FULLWIDTH)) || ((mapping & MAP_SIMPLIFIED) && (mapping & MAP_TRADITIONAL))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if ((mapping & (MAP_LOWERCASE | MAP_UPPERCASE)) && (flags & (CMP_STRINGSORT | CMP_IGNORESYMBOLS | CMP_IGNORENONSPACE))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if ((mapping & (MAP_HIRAGANA | MAP_KATAKANA | MAP_HALFWIDTH | MAP_FULLWIDTH)) && ((mapping & (MAP_SIMPLIFIED | MAP_TRADITIONAL)) || (flags & (CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS)))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (mapping == 0 && (norm & ~(CMP_IGNORENONSPACE | CMP_IGNORESYMBOLS))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (count < 0) count = (int)wcslen(src) + 1;
  if (dst && dst == src && !(mapping & (MAP_LOWERCASE | MAP_UPPERCASE)) ) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (dst && dst == src && (mapping & ~(MAP_LOWERCASE | MAP_UPPERCASE))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (mapping & MAP_SORTKEY) {
    BYTE *bytes = (BYTE *)dst;
    const int needed = nls_sort_key(src, count, flags, NULL, 0);
    if (capacity == 0) return needed;
    if (capacity < needed) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return nls_sort_key(src, count, flags, bytes, capacity);
  }
  int n = 0;
  BOOL word_start = TRUE;
  for (int i = 0; i < count; ++i) {
    WCHAR c = src[i];
    if ((flags & CMP_IGNORENONSPACE) && nls_nonspacing(c)) continue;
    if (flags & CMP_IGNORENONSPACE) c = nls_unaccent(c);
    if (flags & CMP_IGNORESYMBOLS) {
      SortWeight w;
      sort_weight_of(c, 0, &w);
      if (w.primary < 0x1000 && c != 0) continue;
    }
    if (titlecase) {
      const BOOL letter = wide_upper(c) != wide_lower(c);
      c = letter ? (word_start ? wide_upper(c) : wide_lower(c)) : c;
      word_start = !letter;
    } else if (mapping & MAP_LOWERCASE) c = wide_lower(c);
    else if (mapping & MAP_UPPERCASE) c = wide_upper(c);
    c = nls_kana(c, mapping);
    c = nls_width(c, mapping);
    if (mapping & MAP_BYTEREV) c = (WCHAR)((c << 8) | (c >> 8));
    if (capacity != 0 && n < capacity) dst[n] = c;
    ++n;
  }
  (void)nls_title_pending;
  if (capacity == 0) return n;
  if (n > capacity) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  return n;
}
EXPORT int WINAPI LCMapStringW(LCID locale, DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity) {
  if (nls_locale_of(locale) == 0 && locale != 0x40C && locale != 0x411) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  return nls_lcmap(flags, source, count, dest, capacity);
}
EXPORT int WINAPI LCMapStringEx(LPCWSTR name, DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity, LPVOID version, LPVOID reserved, LPARAM sort_handle) {
  if (reserved || (name && nls_locale_of_name(name) == 0 && !(name[0] && name[1] && name[2] == '-')) || (!(flags & MAP_SORTHANDLE) && sort_handle)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  (void)version;
  return nls_lcmap(flags & ~MAP_SORTHANDLE, source, count, dest, capacity);
}
EXPORT int WINAPI LCMapStringA(LCID locale, DWORD flags, LPCSTR source, int count, LPSTR dest, int capacity) {
  if (!source || count == 0 || capacity < 0 || (capacity > 0 && !dest)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (count < 0) count = (int)strlen(source) + 1;
  if (dest == source && !(flags & (MAP_LOWERCASE | MAP_UPPERCASE))) { set_error(ERROR_INVALID_FLAGS); return 0; }
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

IMPORT NTSTATUS NTAPI RtlLcidToLocaleName(ULONG, PVOID, ULONG, BOOLEAN);
EXPORT int WINAPI LCIDToLocaleName(LCID lcid, LPWSTR name, int capacity, DWORD flags) {
  (void)flags;
  if (capacity < 0 || (capacity > 0 && !name)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  static const WCHAR en_us[] = {'e', 'n', '-', 'U', 'S', 0}, en[] = {'e', 'n', 0}, invariant[] = {0};
  const WCHAR *text;
  switch (lcid) {
    case 0: case 0x400: case 0x800: case 0x1000: case 0x2000: case 0x409: text = en_us; break;
    case 0x7F: text = invariant; break;
    case 0x09: text = en; break;
    default: set_error(ERROR_INVALID_PARAMETER); return 0;
  }
  const int n = (int)wcslen(text) + 1;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  memcpy(name, text, (size_t_)n * 2);
  return n;
}
EXPORT LCID WINAPI LocaleNameToLCID(LPCWSTR name, DWORD flags) {
  if (!name) return 0x409;
  if (*name == 0) return 0x7F;
  if (nls_name_is(name, "en-us") || nls_name_is(name, "!x-sys-default-locale")) return 0x409;
  if (nls_name_is(name, "en")) return (flags & 0x08000000) ? 0x09 : 0x409;
  set_error(ERROR_INVALID_PARAMETER);
  return 0;
}
EXPORT BOOL WINAPI IsValidLocaleName(LPCWSTR name) { return name && (*name == 0 || nls_name_is(name, "en-us") || nls_name_is(name, "en")); }
EXPORT BOOL WINAPI IsValidLocale(LCID lcid, DWORD flags) {
  if (flags != 1 && flags != 2) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  return lcid == 0x409 || lcid == 0x7F;
}
EXPORT LCID WINAPI ConvertDefaultLocale(LCID lcid) {
  switch (lcid) {
    case 0: case 0x400: case 0x800: case 0x1000: case 0x2000: return 0x409;
    default: return lcid;
  }
}
EXPORT int WINAPI ResolveLocaleName(LPCWSTR name, LPWSTR out, int capacity) {
  static const WCHAR en_us[] = {'e', 'n', '-', 'U', 'S', 0}, invariant[] = {0};
  if (capacity < 0 || (capacity > 0 && !out)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const WCHAR *text = !name || (name[0] && nls_locale_of_name(name) == 0 && !(name[0] && name[1] && name[2] == '-')) ? en_us : (*name == 0 ? invariant : en_us);
  if (name && !*name) text = invariant;
  const int n = (int)wcslen(text) + 1;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
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
static BOOL nls_each_format(const char *const *list, nls_each_fn call, void *context) {
  for (; *list; ++list) {
    WCHAR text[40];
    nls_widen(text, *list);
    if (!call(text, context)) break;
  }
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
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (kind == 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!flags_ok) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI EnumDateFormatsA(DATEFMT_ENUMPROCA_ proc, LCID locale, DWORD flags) {
  if (!nls_enum_check(nls_locale_of(locale), (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 0, 1, 0};
  return nls_each_format(nls_date_list(flags), nls_each_a, &c);
}
EXPORT BOOL WINAPI EnumDateFormatsW(DATEFMT_ENUMPROCW_ proc, LCID locale, DWORD flags) {
  if (!nls_enum_check(nls_locale_of(locale), (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 0, 1, 0};
  return nls_each_format(nls_date_list(flags), nls_each_w, &c);
}
EXPORT BOOL WINAPI EnumDateFormatsExA(DATEFMT_ENUMPROCEXA_ proc, LCID locale, DWORD flags) {
  if (!nls_enum_check(nls_locale_of(locale), (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 1, 1, 0};
  return nls_each_format(nls_date_list(flags), nls_each_a, &c);
}
EXPORT BOOL WINAPI EnumDateFormatsExW(DATEFMT_ENUMPROCEXW_ proc, LCID locale, DWORD flags) {
  if (!nls_enum_check(nls_locale_of(locale), (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 1, 1, 0};
  return nls_each_format(nls_date_list(flags), nls_each_w, &c);
}
EXPORT BOOL WINAPI EnumDateFormatsExEx(DATEFMT_ENUMPROCEXEX_ proc, LPCWSTR name, DWORD flags, LPARAM param) {
  if (!nls_enum_check(nls_locale_of_name(name), (void *)proc, nls_date_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 2, 1, param};
  return nls_each_format(nls_date_list(flags), nls_each_w, &c);
}
static BOOL nls_time_flags_ok(DWORD flags) { flags &= ~(LCT_USE_CP_ACP | NLS_NOUSEROVERRIDE); return flags == 0 || flags == NLS_TIME_NOSECONDS; }
EXPORT BOOL WINAPI EnumTimeFormatsA(DATEFMT_ENUMPROCA_ proc, LCID locale, DWORD flags) {
  if (!nls_enum_check(nls_locale_of(locale), (void *)proc, nls_time_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 0, 0, 0};
  return nls_each_format((flags & NLS_TIME_NOSECONDS) ? k_times_short : k_times, nls_each_a, &c);
}
EXPORT BOOL WINAPI EnumTimeFormatsW(DATEFMT_ENUMPROCW_ proc, LCID locale, DWORD flags) {
  if (!nls_enum_check(nls_locale_of(locale), (void *)proc, nls_time_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 0, 0, 0};
  return nls_each_format((flags & NLS_TIME_NOSECONDS) ? k_times_short : k_times, nls_each_w, &c);
}
EXPORT BOOL WINAPI EnumTimeFormatsEx(TIMEFMT_ENUMPROCEX_ proc, LPCWSTR name, DWORD flags, LPARAM param) {
  if (!nls_enum_check(nls_locale_of_name(name), (void *)proc, nls_time_flags_ok(flags))) return FALSE;
  NlsEnumContext c = {(void *)proc, 3, 0, param};
  return nls_each_format((flags & NLS_TIME_NOSECONDS) ? k_times_short : k_times, nls_each_w, &c);
}

/* ---- the calendar ------------------------------------------------------------------------------------------------------------------------------------------- */

#define CALENDAR_RETURN_NUMBER 0x20000000u
#define CALENDAR_ALL 0xFFFFFFFFu
/* The text of a CAL_ value for the Gregorian calendars (1 and 2); the numbers are written as text. FALSE if the calendar has no such value. */
static BOOL nls_calendar_text(int kind, DWORD calendar, DWORD type, WCHAR *out /* 80 */) {
  const DWORD t = type & 0xFFFF;
  if (t == 0x30 /* CAL_ITWODIGITYEARMAX */) { nls_widen(out, "2029"); return TRUE; }
  if (calendar != 1 && calendar != 2) return FALSE;
  WCHAR scratch[16];
  const char *text = NULL;
  switch (t) {
    case 0x01: text = calendar == 2 ? "2" : "1"; break;
    case 0x02: text = calendar == 2 ? "Gregorian Calendar (U.S.)" : "Gregorian Calendar"; break;
    case 0x03: text = "0"; break;
    case 0x04: text = "A.D."; break;
    case 0x39: text = "AD"; break;
    case 0x05: text = kind == 2 ? "MM/dd/yyyy" : "M/d/yyyy"; break;
    case 0x06: text = kind == 2 ? "dddd, dd MMMM yyyy" : "dddd, MMMM d, yyyy"; break;
    case 0x2F: text = kind == 2 ? "yyyy MMMM" : "MMMM yyyy"; break;
    case 0x38: text = kind == 2 ? "MMMM dd" : "MMMM d"; break;
    case 0x3A: text = kind == 2 ? "dddd, dd MMMM yyyy" : "dddd, MMMM d, yyyy"; break;
    default: break;
  }
  if (text) { nls_widen(out, text); return TRUE; }
  if (t >= 0x07 && t <= 0x0D) { nls_day_name((int)((t - 0x07 + 1) % 7), FALSE, out); return TRUE; }
  if (t >= 0x0E && t <= 0x14) { nls_day_name((int)((t - 0x0E + 1) % 7), TRUE, out); return TRUE; }
  if (t >= 0x15 && t <= 0x20) { nls_month_name((int)(t - 0x15 + 1), FALSE, out); return TRUE; }
  if (t == 0x21) { out[0] = 0; return TRUE; }
  if (t >= 0x22 && t <= 0x2D) { nls_month_name((int)(t - 0x22 + 1), TRUE, out); return TRUE; }
  if (t == 0x2E) { out[0] = 0; return TRUE; }
  if (t >= 0x31 && t <= 0x37) { nls_day_name((int)((t - 0x31 + 1) % 7), TRUE, scratch); out[0] = scratch[0]; out[1] = scratch[1]; out[2] = 0; return TRUE; }
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
