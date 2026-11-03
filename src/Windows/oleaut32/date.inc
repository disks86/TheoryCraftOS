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

/* Dates: DATE values (days since 30 December 1899, the time of day in the fraction; before the epoch the fraction counts forward from the day's midnight), the
 * conversions to and from calendar fields, MS-DOS date/time, parsing of date strings and the text of a date. */

#define DT_MIN (-657434LL)       /* 1 January 100 */
#define DT_MAX 2958465LL         /* 31 December 9999 */

static const BYTE dt_month_days[2][13] = {
  { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
  { 0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
};

static int dt_leap(long long y) { return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0); }
static long long dt_floordiv(long long a, long long b) { long long q = a / b; if (a % b != 0 && ((a < 0) != (b < 0))) q--; return q; }
static long long dt_floormod(long long a, long long b) { return a - dt_floordiv(a, b) * b; }

/* The DATE day number (0 = 30 December 1899) of a proleptic Gregorian date; the month must be 1..12. */
static long long dt_days_from_civil(long long y, int m, int d) {
  y -= m <= 2;
  const long long era = dt_floordiv(y, 400), yoe = y - era * 400;
  const long long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468 + 25569;
}
static void dt_civil_from_days(long long dn, long long *year, int *month, int *day) {
  const long long z = dn - 25569 + 719468;
  const long long era = dt_floordiv(z, 146097), doe = z - era * 146097;
  const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  *day = (int)(doy - (153 * mp + 2) / 5 + 1);
  *month = (int)(mp < 10 ? mp + 3 : mp - 9);
  *year = yoe + era * 400 + (*month <= 2);
}
static int dt_weekday(long long dn) { return (int)dt_floormod(dn + 6, 7); }      /* 0 = Sunday; 30 December 1899 was a Saturday */

/* The two-digit year rule of Windows 10: 0..49 are 2000..2049, 50..99 are 1950..1999. */
static long long dt_fix_year(long long year) {
  if (year < 0) return year + 2000;
  if (year < 100) return year + (year < 50 ? 2000 : 1900);
  return year;
}

EXPORT HRESULT WINAPI VarUdateFromDate(DATE date, ULONG flags, UDATE *udate) {
  (void)flags;
  if (!udate) return E_INVALIDARG;
  if (!(date > -657435.0 && date < 2958466.0)) return E_INVALIDARG;
  const double date_part = date < 0.0 ? oa_ceil(date) : oa_floor(date);
  double time_part = oa_fabs(date - date_part) + 0.00000000001;     /* (the epsilon compensates for the truncation of the fraction) */
  if (time_part >= 1.0) time_part -= 0.00000000001;
  long long dn = (long long)date_part, year;
  int month, day;
  dt_civil_from_days(dn, &year, &month, &day);
  time_part *= 24.0;
  int hour = (int)time_part;
  time_part = (time_part - hour) * 60.0;
  int minute = (int)time_part;
  time_part = (time_part - minute) * 60.0;
  int second = (int)time_part;
  time_part -= second;
  if (time_part > 0.5) {         /* round to whole seconds, carrying into the minute, hour and day */
    int next_day = 0;
    if (++second == 60) { second = 0; if (++minute == 60) { minute = 0; if (++hour == 24) { hour = 0; next_day = 1; } } }
    if (next_day) {
      if (dn + 1 > DT_MAX) { hour = 23; minute = 59; second = 59; }      /* (nothing after the last day: keep the last second) */
      else { dn++; dt_civil_from_days(dn, &year, &month, &day); }
    }
  }
  memset(udate, 0, sizeof(*udate));
  udate->st.wYear = (WORD)year;
  udate->st.wMonth = (WORD)month;
  udate->st.wDay = (WORD)day;
  udate->st.wDayOfWeek = (WORD)dt_weekday(dn);
  udate->st.wHour = (WORD)hour;
  udate->st.wMinute = (WORD)minute;
  udate->st.wSecond = (WORD)second;
  udate->wDayOfYear = (USHORT)(dn - dt_days_from_civil(year, 1, 1) + 1);
  return S_OK;
}

/* The DATE of calendar fields that may be out of range (they roll over, as in Windows): months and days count from the first of the year / month, a year below 100
 * is a two-digit year, and a negative year counts back from 2000. */
static HRESULT dt_from_fields(long long year, long long month, long long day, long long hour, long long minute, long long second, ULONG flags, DATE *out) {
  year = dt_fix_year(year);
  const long long months = year * 12 + (month - 1);
  year = dt_floordiv(months, 12);
  month = dt_floormod(months, 12) + 1;
  if (year < 100 || year > 9999) return E_INVALIDARG;
  long long dn = dt_days_from_civil(year, (int)month, 1) + (day - 1);
  long long seconds = hour * 3600 + minute * 60 + second;
  dn += dt_floordiv(seconds, 86400);
  seconds = dt_floormod(seconds, 86400);
  if (dn < DT_MIN || dn > DT_MAX) return E_INVALIDARG;
  double value = flags & VAR_TIMEVALUEONLY ? 0.0 : (double)dn;
  if (!(flags & VAR_DATEVALUEONLY) || (flags & VAR_TIMEVALUEONLY)) {
    const double fraction = (double)seconds / 86400.0;
    value += value < 0.0 ? -fraction : fraction;
  }
  *out = value;
  return S_OK;
}

EXPORT HRESULT WINAPI VarDateFromUdateEx(UDATE *udate, LCID lcid, ULONG flags, DATE *out) {
  (void)lcid;
  if (!udate || !out) return E_INVALIDARG;
  const SYSTEMTIME *st = &udate->st;
  return dt_from_fields((short)st->wYear, (short)st->wMonth, (short)st->wDay, (short)st->wHour, (short)st->wMinute, (short)st->wSecond, flags, out);
}
EXPORT HRESULT WINAPI VarDateFromUdate(UDATE *udate, ULONG flags, DATE *out) {
  return VarDateFromUdateEx(udate, MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT), flags, out);
}

EXPORT INT WINAPI SystemTimeToVariantTime(LPSYSTEMTIME st, double *out) {
  if (!st || !out) return FALSE;
  if (st->wMonth > 12 || st->wDay > 31 || (short)st->wYear < 0) return FALSE;
  UDATE udate;
  memset(&udate, 0, sizeof(udate));
  udate.st = *st;
  return VarDateFromUdate(&udate, 0, out) == S_OK;
}
EXPORT INT WINAPI VariantTimeToSystemTime(double time, LPSYSTEMTIME st) {
  UDATE udate;
  if (!st || FAILED(VarUdateFromDate(time, 0, &udate))) return FALSE;
  *st = udate.st;
  return TRUE;
}

EXPORT INT WINAPI DosDateTimeToVariantTime(USHORT dos_date, USHORT dos_time, double *out) {
  if (!out) return FALSE;
  UDATE udate;
  memset(&udate, 0, sizeof(udate));
  udate.st.wYear = (WORD)(1980 + ((dos_date >> 9) & 0x7f));
  udate.st.wMonth = (dos_date >> 5) & 0xf;
  udate.st.wDay = dos_date & 0x1f;
  udate.st.wHour = dos_time >> 11;
  udate.st.wMinute = (dos_time >> 5) & 0x3f;
  udate.st.wSecond = (WORD)((dos_time & 0x1f) << 1);
  if (udate.st.wYear > 2099 || udate.st.wMonth > 12 || udate.st.wHour > 23 || udate.st.wMinute > 59 || udate.st.wSecond > 59) return FALSE;
  return VarDateFromUdate(&udate, 0, out) == S_OK;
}
EXPORT INT WINAPI VariantTimeToDosDateTime(double time, USHORT *dos_date, USHORT *dos_time) {
  if (!dos_date || !dos_time) return FALSE;
  *dos_date = *dos_time = 0;
  UDATE udate;
  if (FAILED(VarUdateFromDate(time, 0, &udate))) return FALSE;
  if (udate.st.wYear < 1980 || udate.st.wYear > 2099) return FALSE;
  *dos_date = (USHORT)(udate.st.wDay | (udate.st.wMonth << 5) | ((udate.st.wYear - 1980) << 9));
  *dos_time = (USHORT)((udate.st.wSecond >> 1) | (udate.st.wMinute << 5) | (udate.st.wHour << 11));
  return TRUE;
}

/* ---- locale values -------------------------------------------------------------------------------------------------------------------------------------- */

static void dt_locale_string(LCID lcid, ULONG flags, LCTYPE type, WCHAR *buffer, int capacity) {
  if (GetLocaleInfoW(lcid, type | (flags & LOCALE_NOUSEROVERRIDE), buffer, capacity) <= 0) buffer[0] = 0;
}
static DWORD dt_locale_number(LCID lcid, ULONG flags, LCTYPE type, DWORD fallback) {
  DWORD value = 0;
  if (GetLocaleInfoW(lcid, type | LOCALE_RETURN_NUMBER | (flags & LOCALE_NOUSEROVERRIDE), (LPWSTR)&value, sizeof(value) / sizeof(WCHAR)) <= 0) return fallback;
  return value;
}

/* ---- parsing a date string -------------------------------------------------------------------------------------------------------------------------------- */

#define DP_TS    1      /* a time separator (':' or '.') follows the number */
#define DP_MONTH 2      /* the field is a month name; its value is the month */
#define DP_MAX_FIELDS 6

typedef struct {
  int count, ampm_field;
  BOOL pm;
  unsigned flags[DP_MAX_FIELDS];
  long values[DP_MAX_FIELDS];
} DateParse;

static BOOL dt_is_space(WCHAR c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x3000 || c == 0xA0; }
static BOOL dt_is_digit(WCHAR c) { return c >= '0' && c <= '9'; }
static BOOL dt_is_letter(WCHAR c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c > 127 && !dt_is_space(c)); }

static BOOL dt_word_equals(LCID lcid, const WCHAR *word, int length, const WCHAR *name) {
  return name[0] && CompareStringW(lcid, NORM_IGNORECASE, word, length, name, -1) == CSTR_EQUAL;
}

/* 1..12 if the word is a month name of the locale (full or abbreviated), else 0. */
static int dt_month_of_word(LCID lcid, ULONG flags, const WCHAR *word, int length) {
  WCHAR name[80];
  for (int month = 0; month < 12; month++) {
    dt_locale_string(lcid, flags, LOCALE_SMONTHNAME1 + month, name, 80);
    if (dt_word_equals(lcid, word, length, name)) return month + 1;
    dt_locale_string(lcid, flags, LOCALE_SABBREVMONTHNAME1 + month, name, 80);
    if (dt_word_equals(lcid, word, length, name)) return month + 1;
  }
  return 0;
}
/* 1 = AM, 2 = PM, 0 = neither */
static int dt_ampm_of_word(LCID lcid, ULONG flags, const WCHAR *word, int length) {
  WCHAR name[40];
  if (length == 1 || (length == 2 && oa_towupper(word[1]) == 'M')) {
    const WCHAR first = oa_towupper(word[0]);
    if (first == 'A') return 1;
    if (first == 'P') return 2;
  }
  dt_locale_string(lcid, flags, LOCALE_S1159, name, 40);
  if (dt_word_equals(lcid, word, length, name)) return 1;
  dt_locale_string(lcid, flags, LOCALE_S2359, name, 40);
  if (dt_word_equals(lcid, word, length, name)) return 2;
  return 0;
}

static BOOL dt_day_valid(long long day, long long month, long long year) {
  return month >= 1 && month <= 12 && day >= 1 && day <= dt_month_days[dt_leap(year)][month];
}

/* A date from two or three numbers (`month_at` is the position of a month name, or -1). The locale's order is tried first. */
static BOOL dt_make_date(const long *v, int count, int month_at, DWORD idate, long long current_year, long long *year, long long *month, long long *day) {
  if (count == 2) {
    if (month_at >= 0) {
      const long long other = v[1 - month_at], m = v[month_at];
      if (dt_day_valid(other, m, current_year)) { *year = current_year; *month = m; *day = other; return TRUE; }
      const long long y = dt_fix_year(other);
      if (y < 100 || y > 9999) return FALSE;
      *year = y; *month = m; *day = 1;
      return TRUE;
    }
    const long long a = v[0], b = v[1];
    const BOOL day_first = idate == 1;
    for (int pass = 0; pass < 2; pass++) {
      const BOOL as_day_month = (pass == 0) == day_first;
      const long long d = as_day_month ? a : b, m = as_day_month ? b : a;
      if (dt_day_valid(d, m, current_year)) { *year = current_year; *month = m; *day = d; return TRUE; }
    }
    /* neither is a day and a month: a year and a month */
    for (int pass = 0; pass < 2; pass++) {
      const long long y = dt_fix_year(pass == 0 ? a : b), m = pass == 0 ? b : a;
      if (m >= 1 && m <= 12 && y >= 100 && y <= 9999) { *year = y; *month = m; *day = 1; return TRUE; }
    }
    return FALSE;
  }
  static const char orders[6][4] = { "MDY", "YMD", "YDM", "DMY", "MYD", "DYM" };
  const int preferred = idate == 1 ? 3 : idate == 2 ? 1 : 0;
  for (int pass = 0; pass < 7; pass++) {
    const int index = pass == 0 ? preferred : pass - 1;
    if (pass > 0 && index == preferred) continue;
    const char *order = orders[index];
    if (month_at >= 0) { if (order[month_at] != 'M') continue; }
    else if (index >= 4) continue;
    long long y = 0, m = 0, d = 0;
    for (int k = 0; k < 3; k++) { if (order[k] == 'M') m = v[k]; else if (order[k] == 'D') d = v[k]; else y = v[k]; }
    y = dt_fix_year(y);
    if (y < 100 || y > 9999 || !dt_day_valid(d, m, y)) continue;
    *year = y; *month = m; *day = d;
    return TRUE;
  }
  return FALSE;
}

static HRESULT dt_parse_string(const WCHAR *text, LCID lcid, ULONG flags, DATE *out) {
  const HRESULT bad = DISP_E_TYPEMISMATCH;
  WCHAR separator[8];
  dt_locale_string(lcid, flags, LOCALE_SDATE, separator, 8);
  const WCHAR date_separator = separator[0] ? separator[0] : '/';
  DateParse dp;
  memset(&dp, 0, sizeof(dp));
  dp.ampm_field = -1;
  for (const WCHAR *p = text;;) {
    while (dt_is_space(*p)) p++;
    if (!*p) break;
    if (dp.count >= DP_MAX_FIELDS + 1) return bad;
    if (dt_is_digit(*p)) {
      long value = 0;
      int digits = 0;
      for (; dt_is_digit(*p); p++) { if (++digits > 9) return bad; value = value * 10 + (*p - '0'); }
      if (dp.count >= DP_MAX_FIELDS) return bad;
      dp.values[dp.count] = value;
      dp.flags[dp.count++] = 0;
    } else if (dt_is_letter(*p)) {
      const WCHAR *word = p;
      while (dt_is_letter(*p)) p++;
      const int length = (int)(p - word), month = dt_month_of_word(lcid, flags, word, length), ampm = month ? 0 : dt_ampm_of_word(lcid, flags, word, length);
      if (month) {
        if (dp.count >= DP_MAX_FIELDS) return bad;
        dp.values[dp.count] = month;
        dp.flags[dp.count++] = DP_MONTH;
      } else if (ampm) {
        if (dp.count == 0 || dp.ampm_field >= 0 || (dp.flags[dp.count - 1] & DP_MONTH)) return bad;
        dp.ampm_field = dp.count - 1;
        dp.pm = ampm == 2;
        continue;
      } else return bad;
    } else return bad;
    BOOL need_next = FALSE;
    if (*p == ':' || (*p == '.' && date_separator != '.')) { dp.flags[dp.count - 1] |= DP_TS; p++; need_next = TRUE; }
    else if (*p == '/' || *p == '-' || *p == date_separator) { p++; need_next = TRUE; }
    else if (*p == ',') p++;
    if (need_next) {
      while (dt_is_space(*p)) p++;
      if (!dt_is_digit(*p) && !dt_is_letter(*p)) return bad;
    }
  }
  const int n = dp.count;
  if (n == 0) return bad;
  /* the time of day: a chain of two or three numbers joined by time separators, or one hour with am/pm; it is at the start or the end */
  int t0 = -1, t1 = -1;
  for (int i = 0; i < n && t0 < 0; i++) if (dp.flags[i] & DP_TS) t0 = i;
  if (t0 >= 0) {
    t1 = t0;
    while (t1 < n && (dp.flags[t1] & DP_TS)) t1++;
    if (t1 >= n || t1 - t0 + 1 > 3) return bad;
    for (int k = t1 + 1; k < n; k++) if (dp.flags[k] & DP_TS) return bad;
  }
  if (dp.ampm_field >= 0) {
    if (t0 >= 0) { if (dp.ampm_field != t1) return bad; }
    else t0 = t1 = dp.ampm_field;
  }
  int time_count = 0, date_offset = 0;
  if (t0 >= 0) {
    for (int k = t0; k <= t1; k++) if (dp.flags[k] & DP_MONTH) return bad;
    if (t0 != 0 && t1 != n - 1) return bad;
    time_count = t1 - t0 + 1;
    if (t0 == 0) date_offset = t1 + 1;
  }
  const int date_count = n - time_count;
  if (date_count != 0 && date_count != 2 && date_count != 3) return bad;

  long long dn = 0;
  if (date_count) {
    int month_at = -1;
    for (int k = 0; k < date_count; k++) if (dp.flags[date_offset + k] & DP_MONTH) { if (month_at >= 0) return bad; month_at = k; }
    SYSTEMTIME now;
    GetSystemTime(&now);
    long long year, month, day;
    if (!dt_make_date(dp.values + date_offset, date_count, month_at, dt_locale_number(lcid, flags, LOCALE_IDATE, 0), now.wYear, &year, &month, &day)) return bad;
    dn = dt_days_from_civil(year, (int)month, (int)day);
  }
  long seconds = 0;
  if (time_count) {
    long hour = dp.values[t0], minute = time_count > 1 ? dp.values[t0 + 1] : 0, second = time_count > 2 ? dp.values[t0 + 2] : 0;
    if (dp.ampm_field >= 0 && hour <= 12) {
      if (dp.pm) { if (hour < 12) hour += 12; }
      else if (hour == 12) hour = 0;
    }
    if (hour > 23 || minute > 59 || second > 59) return bad;
    seconds = hour * 3600 + minute * 60 + second;
  }
  if (flags & VAR_TIMEVALUEONLY) dn = 0;
  else if (flags & VAR_DATEVALUEONLY) seconds = 0;
  const double fraction = (double)seconds / 86400.0;
  *out = (double)dn + (dn < 0 ? -fraction : fraction);
  return S_OK;
}

EXPORT HRESULT WINAPI VarDateFromStr(const OLECHAR *text, LCID lcid, ULONG flags, DATE *out) {
  if (!out) return E_INVALIDARG;
  if (!text) return DISP_E_TYPEMISMATCH;
  return dt_parse_string(text, lcid, flags, out);
}

/* A date with a Win32 date picture (d dd ddd dddd M MM MMM MMMM y yy yyy yyyy g gg, 'quoted text', other characters as they are), for the years that GetDateFormat
 * does not accept (before 1601). Returns the length; the text is cut to fit. */
static size_t dt_picture_date(LCID lcid, ULONG flags, const WCHAR *picture, const SYSTEMTIME *st, WCHAR *out, size_t capacity) {
  size_t length = 0;
#define DT_PUT(c) do { if (length + 1 < capacity) out[length] = (c); length++; } while (0)
  for (const WCHAR *p = picture; *p;) {
    const WCHAR c = *p;
    int run = 1;
    while (p[run] == c) run++;
    WCHAR text[80];
    text[0] = 0;
    BOOL numeric = FALSE;
    unsigned value = 0;
    int width = 0;
    if (c == '\'') {
      p++;
      for (; *p; p++) {
        if (*p == '\'') { if (p[1] == '\'') { DT_PUT('\''); p++; continue; } p++; break; }
        DT_PUT(*p);
      }
      continue;
    } else if (c == 'd') {
      if (run <= 2) { numeric = TRUE; value = st->wDay; width = run; }
      else dt_locale_string(lcid, flags, (run == 3 ? LOCALE_SABBREVDAYNAME1 : LOCALE_SDAYNAME1) + (st->wDayOfWeek + 6) % 7, text, 80);
      if (run > 4) run = 4;
    } else if (c == 'M') {
      if (run <= 2) { numeric = TRUE; value = st->wMonth; width = run; }
      else dt_locale_string(lcid, flags, (run == 3 ? LOCALE_SABBREVMONTHNAME1 : LOCALE_SMONTHNAME1) + st->wMonth - 1, text, 80);
      if (run > 4) run = 4;
    } else if (c == 'y') {
      numeric = TRUE;
      if (run == 1) { value = st->wYear % 100; width = 1; }
      else if (run == 2) { value = st->wYear % 100; width = 2; }
      else { value = st->wYear; width = 1; }
    } else if (c == 'g') {
      text[0] = 'A'; text[1] = '.'; text[2] = 'D'; text[3] = '.'; text[4] = 0;
    } else { DT_PUT(c); p++; continue; }
    if (numeric) {
      WCHAR digits[12];
      int count = 0;
      do { digits[count++] = (WCHAR)('0' + value % 10); value /= 10; } while (value);
      for (int pad = count; pad < width; pad++) DT_PUT('0');
      while (count) DT_PUT(digits[--count]);
    } else for (const WCHAR *t = text; *t; t++) DT_PUT(*t);
    p += run;
  }
  if (capacity) out[length < capacity ? length : capacity - 1] = 0;
#undef DT_PUT
  return length;
}

/* The short date of a locale as text. */
static BOOL dt_short_date(LCID lcid, ULONG flags, const SYSTEMTIME *st, WCHAR *out, int capacity) {
  if (st->wYear >= 1601 && GetDateFormatW(lcid, (flags & LOCALE_NOUSEROVERRIDE) | DATE_SHORTDATE, st, NULL, out, capacity)) return TRUE;
  WCHAR picture[80];
  dt_locale_string(lcid, flags, LOCALE_SSHORTDATE, picture, 80);
  if (!picture[0]) return FALSE;
  return dt_picture_date(lcid, flags, picture, st, out, (size_t)capacity) < (size_t)capacity;
}

/* ---- the text of a date ----------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI VarBstrFromDate(DATE date, LCID lcid, ULONG flags, BSTR *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  SYSTEMTIME st;
  if (!VariantTimeToSystemTime(date, &st)) return E_INVALIDARG;
  const DWORD format_flags = flags & LOCALE_NOUSEROVERRIDE;
  const BOOL has_day = (long long)(date < 0.0 ? oa_ceil(date) : oa_floor(date)) != 0, has_time = st.wHour || st.wMinute || st.wSecond;
  BOOL want_date = has_day, want_time = has_time || !has_day;
  if (flags & VAR_TIMEVALUEONLY) { want_date = FALSE; want_time = TRUE; }
  else if (flags & VAR_DATEVALUEONLY) { want_date = TRUE; want_time = FALSE; }
  else if (flags & (VARIANT_USE_NLS | LOCALE_USE_NLS)) want_date = want_time = TRUE;      /* (NLS formatting always gives both) */
  WCHAR date_text[128], time_text[128];
  date_text[0] = time_text[0] = 0;
  if (want_date && !dt_short_date(lcid, flags, &st, date_text, OA_ARRAY_SIZE(date_text))) return E_INVALIDARG;
  if (want_time) {
    SYSTEMTIME clock = st;
    clock.wYear = 2000; clock.wMonth = 1; clock.wDay = 1;          /* (only the time of day counts, and GetTimeFormat may not like old years) */
    if (!GetTimeFormatW(lcid, format_flags, &clock, NULL, time_text, OA_ARRAY_SIZE(time_text))) return E_INVALIDARG;
  }
  const size_t date_length = oa_wcslen(date_text), time_length = oa_wcslen(time_text);
  const size_t separator = date_length && time_length ? 1 : 0;
  BSTR result = SysAllocStringLen(NULL, (UINT)(date_length + separator + time_length));
  if (!result) return E_OUTOFMEMORY;
  memcpy(result, date_text, date_length * sizeof(WCHAR));
  if (separator) result[date_length] = ' ';
  memcpy(result + date_length + separator, time_text, time_length * sizeof(WCHAR));
  *out = result;
  return S_OK;
}

/* ---- month names that differ from the locale's (Arabic Hijri, and the genitives of Polish and Russian) ------------------------------------------------------ */

static LPOLESTR dt_hijri_months[13] = {
  L"محرم", L"صفر", L"ربيع الأول", L"ربيع الثاني",
  L"جمادى الأولى", L"جمادى الثانية", L"رجب", L"شعبان",
  L"رمضان", L"شوال", L"ذو القعدة", L"ذو الحجة", NULL };
static LPOLESTR dt_polish_months[13] = {
  L"stycznia", L"lutego", L"marca", L"kwietnia", L"maja", L"czerwca", L"lipca", L"sierpnia", L"września", L"października", L"listopada", L"grudnia", NULL };
static LPOLESTR dt_russian_months[13] = {
  L"января", L"февраля", L"марта", L"апреля", L"мая", L"июня",
  L"июля", L"августа", L"сентября", L"октября", L"ноября", L"декабря", NULL };

EXPORT HRESULT WINAPI GetAltMonthNames(LCID lcid, LPOLESTR **names) {
  if (!names) return E_INVALIDARG;
  switch (PRIMARYLANGID(LANGIDFROMLCID(lcid))) {
    case LANG_ARABIC: *names = dt_hijri_months; break;
    case LANG_POLISH: *names = dt_polish_months; break;
    case LANG_RUSSIAN: *names = dt_russian_months; break;
    default: *names = NULL; break;
  }
  return S_OK;
}
