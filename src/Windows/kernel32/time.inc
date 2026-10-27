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

/* kernel32.dll: local time and time zones. A TIME_ZONE_INFORMATION has a bias (minutes to add to local time to get UTC) and, for zones with daylight saving, the
 * moment each period starts: DaylightDate in standard time, StandardDate in daylight time, either as an absolute date (wYear != 0) or as "the wDay-th wDayOfWeek of
 * wMonth" (wDay 5 is the last). The zone of the process is UTC until SetTimeZoneInformation. Included by kernel32.c. */

typedef struct {
  LONG Bias; WCHAR StandardName[32]; SYSTEMTIME StandardDate; LONG StandardBias; WCHAR DaylightName[32]; SYSTEMTIME DaylightDate; LONG DaylightBias;
  WCHAR TimeZoneKeyName[128]; BOOLEAN DynamicDaylightTimeDisabled;
} DYNAMIC_TZ_INFORMATION;

#define TZ_TICKS_PER_MINUTE 600000000ll
#define TZ_ID_UNKNOWN 0
#define TZ_ID_STANDARD 1
#define TZ_ID_DAYLIGHT 2

static TIME_ZONE_INFORMATION g_zone;
static BOOLEAN g_zone_disabled_dst;
static BOOL g_zone_set;

static void zone_default(TIME_ZONE_INFORMATION *tz) {
  memset(tz, 0, sizeof(*tz));
  static const WCHAR utc[] = {'U', 'T', 'C', 0};
  memcpy(tz->StandardName, utc, sizeof(utc));
  memcpy(tz->DaylightName, utc, sizeof(utc));
}
static const TIME_ZONE_INFORMATION *zone_current(void) {
  if (!g_zone_set) { zone_default(&g_zone); g_zone_set = TRUE; }
  return &g_zone;
}

static BOOL leap_year(LONGLONG y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int month_days(LONGLONG y, int m) {
  static const unsigned char days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leap_year(y) ? 29 : days[m - 1];
}

static ULONGLONG tz_ticks_of(const FILETIME *ft) { return ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime; }
static void tz_ticks_to(ULONGLONG t, FILETIME *ft) { ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32); }

/* The wall-clock moment (100 ns since 1601) at which a transition of `year` happens, from the date as the zone gives it. Zero if the zone has none. */
static LONGLONG tz_transition(const SYSTEMTIME *d, LONGLONG year) {
  if (d->wMonth < 1 || d->wMonth > 12) return -1;
  LONGLONG day;
  if (d->wYear != 0) {
    day = d->wDay;
    year = d->wYear;
  } else {
    const LONGLONG first = days_from_civil(year, d->wMonth, 1);      /* days since 1970; 1970-01-01 was a Thursday */
    const int first_weekday = (int)(((first % 7) + 7 + 4) % 7);
    int delta = (int)d->wDayOfWeek - first_weekday;
    if (delta < 0) delta += 7;
    day = 1 + delta + 7 * ((int)d->wDay - 1);
    while (day > month_days(year, d->wMonth)) day -= 7;
  }
  const LONGLONG days = days_from_civil(year, d->wMonth, day) + 134774;
  return ((days * 86400 + d->wHour * 3600 + d->wMinute * 60 + d->wSecond) * 1000 + d->wMilliseconds) * 10000;
}

static LONGLONG tz_year_of(ULONGLONG ticks) {
  FILETIME ft;
  SYSTEMTIME st;
  tz_ticks_to(ticks, &ft);
  if (!FileTimeToSystemTime(&ft, &st)) return 1601;
  return st.wYear;
}

/* Which period a moment is in. `local` says the moment is a wall-clock time of the zone, otherwise it is UTC. */
static int tz_period(const TIME_ZONE_INFORMATION *tz, ULONGLONG t, BOOL local) {
  if (!tz->DaylightDate.wMonth || !tz->StandardDate.wMonth) return TZ_ID_UNKNOWN;
  const LONGLONG year = tz_year_of(local ? t : t - (LONGLONG)(tz->Bias) * TZ_TICKS_PER_MINUTE);
  LONGLONG daylight = tz_transition(&tz->DaylightDate, year), standard = tz_transition(&tz->StandardDate, year);
  if (daylight < 0 || standard < 0) return TZ_ID_UNKNOWN;
  if (!local) {
    daylight += (LONGLONG)(tz->Bias + tz->StandardBias) * TZ_TICKS_PER_MINUTE;
    standard += (LONGLONG)(tz->Bias + tz->DaylightBias) * TZ_TICKS_PER_MINUTE;
  }
  const LONGLONG v = (LONGLONG)t;
  if (daylight < standard) return v >= daylight && v < standard ? TZ_ID_DAYLIGHT : TZ_ID_STANDARD;
  return v >= standard && v < daylight ? TZ_ID_STANDARD : TZ_ID_DAYLIGHT;
}
static LONG tz_bias_of(const TIME_ZONE_INFORMATION *tz, int period) {
  return tz->Bias + (period == TZ_ID_STANDARD ? tz->StandardBias : period == TZ_ID_DAYLIGHT ? tz->DaylightBias : 0);
}

static BOOL tz_convert(const TIME_ZONE_INFORMATION *tz_in, const SYSTEMTIME *in, SYSTEMTIME *out, BOOL to_local) {
  TIME_ZONE_INFORMATION current;
  const TIME_ZONE_INFORMATION *tz = tz_in;
  if (!tz) { current = *zone_current(); tz = &current; }
  FILETIME ft;
  if (!in || !out || !SystemTimeToFileTime(in, &ft)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const ULONGLONG t = tz_ticks_of(&ft);
  const LONG bias = tz_bias_of(tz, tz_period(tz, t, !to_local));
  const LONGLONG shifted = (LONGLONG)t + (to_local ? -1 : 1) * (LONGLONG)bias * TZ_TICKS_PER_MINUTE;
  if (shifted < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  tz_ticks_to((ULONGLONG)shifted, &ft);
  return FileTimeToSystemTime(&ft, out);
}

EXPORT BOOL WINAPI SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *utc, LPSYSTEMTIME local) { return tz_convert(tz, utc, local, TRUE); }
EXPORT BOOL WINAPI TzSpecificLocalTimeToSystemTime(const TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *local, LPSYSTEMTIME utc) { return tz_convert(tz, local, utc, FALSE); }
EXPORT BOOL WINAPI SystemTimeToTzSpecificLocalTimeEx(const DYNAMIC_TZ_INFORMATION *tz, const SYSTEMTIME *utc, LPSYSTEMTIME local) {
  return tz_convert(tz ? (const TIME_ZONE_INFORMATION *)tz : NULL, utc, local, TRUE);
}
EXPORT BOOL WINAPI TzSpecificLocalTimeToSystemTimeEx(const DYNAMIC_TZ_INFORMATION *tz, const SYSTEMTIME *local, LPSYSTEMTIME utc) {
  return tz_convert(tz ? (const TIME_ZONE_INFORMATION *)tz : NULL, local, utc, FALSE);
}

EXPORT void WINAPI GetLocalTime(LPSYSTEMTIME st) {
  SYSTEMTIME utc;
  GetSystemTime(&utc);
  if (!tz_convert(NULL, &utc, st, TRUE)) *st = utc;
}
EXPORT BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *in, LPFILETIME out) {
  if (!in || !out) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const ULONGLONG t = tz_ticks_of(in);
  const TIME_ZONE_INFORMATION *tz = zone_current();
  const LONGLONG v = (LONGLONG)t - (LONGLONG)tz_bias_of(tz, tz_period(tz, t, FALSE)) * TZ_TICKS_PER_MINUTE;
  if (t >= 0x8000000000000000ull || v < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  tz_ticks_to((ULONGLONG)v, out);
  return TRUE;
}
EXPORT BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *in, LPFILETIME out) {
  if (!in || !out) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const ULONGLONG t = tz_ticks_of(in);
  const TIME_ZONE_INFORMATION *tz = zone_current();
  const LONGLONG v = (LONGLONG)t + (LONGLONG)tz_bias_of(tz, tz_period(tz, t, TRUE)) * TZ_TICKS_PER_MINUTE;
  if (t >= 0x8000000000000000ull || v < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  tz_ticks_to((ULONGLONG)v, out);
  return TRUE;
}
EXPORT BOOL WINAPI LocalFileTimeToLocalSystemTime(const FILETIME *ft, LPSYSTEMTIME st) { return FileTimeToSystemTime(ft, st); }
EXPORT BOOL WINAPI LocalSystemTimeToLocalFileTime(const SYSTEMTIME *st, LPFILETIME ft) { return SystemTimeToFileTime(st, ft); }

static DWORD tz_id_now(const TIME_ZONE_INFORMATION *tz) {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  return (DWORD)tz_period(tz, tz_ticks_of(&now), FALSE);
}
EXPORT DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz) {
  if (!tz) { set_error(ERROR_INVALID_PARAMETER); return 0xFFFFFFFF; }
  *tz = *zone_current();
  return g_zone_disabled_dst ? TZ_ID_UNKNOWN : tz_id_now(tz);
}
EXPORT DWORD WINAPI GetDynamicTimeZoneInformation(DYNAMIC_TZ_INFORMATION *tz) {
  if (!tz) { set_error(ERROR_INVALID_PARAMETER); return 0xFFFFFFFF; }
  memset(tz, 0, sizeof(*tz));
  memcpy(tz, zone_current(), sizeof(TIME_ZONE_INFORMATION));
  memcpy(tz->TimeZoneKeyName, zone_current()->StandardName, sizeof(tz->StandardName));
  tz->DynamicDaylightTimeDisabled = g_zone_disabled_dst;
  return g_zone_disabled_dst ? TZ_ID_UNKNOWN : tz_id_now(zone_current());
}
EXPORT DWORD WINAPI GetTimeZoneInformationForYear(USHORT year, DYNAMIC_TZ_INFORMATION *dynamic, LPTIME_ZONE_INFORMATION tz) {
  (void)year; (void)dynamic;
  if (!tz) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *tz = *zone_current();
  return TRUE;
}
EXPORT BOOL WINAPI SetTimeZoneInformation(const TIME_ZONE_INFORMATION *tz) {
  if (!tz) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  g_zone = *tz;
  g_zone_set = TRUE;
  g_zone_disabled_dst = FALSE;
  return TRUE;
}
EXPORT BOOL WINAPI SetDynamicTimeZoneInformation(const DYNAMIC_TZ_INFORMATION *tz) {
  if (!tz) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  memcpy(&g_zone, tz, sizeof(g_zone));
  g_zone_set = TRUE;
  g_zone_disabled_dst = tz->DynamicDaylightTimeDisabled;
  return TRUE;
}
EXPORT BOOL WINAPI IsTimeZoneRedirectionEnabled(void) { return FALSE; }

/* The clock is the machine's (set by the firmware's real-time clock); a program may not change it. */
EXPORT BOOL WINAPI SetSystemTime(const SYSTEMTIME *st) { (void)st; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI SetLocalTime(const SYSTEMTIME *st) { (void)st; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI GetSystemTimeAdjustment(PDWORD adjustment, PDWORD increment, PBOOL disabled) {
  if (!adjustment || !increment || !disabled) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *adjustment = 156250;      /* 15.625 ms in 100 ns units, as Windows reports with no adjustment */
  *increment = 156250;
  *disabled = TRUE;
  return TRUE;
}
EXPORT BOOL WINAPI SetSystemTimeAdjustment(DWORD adjustment, BOOL disabled) { (void)adjustment; (void)disabled; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }

/* The processor times: the system's, from the performance information of every processor (NtQuerySystemInformation class 8, 48 bytes each: idle, kernel with idle
 * in it, user). */
EXPORT BOOL WINAPI GetSystemTimes(LPFILETIME idle, LPFILETIME kernel, LPFILETIME user) {
  ULONGLONG info[6 * 256] = {};
  ULONG length = 0;
  const NTSTATUS status = NtQuerySystemInformation(8, info, sizeof(info), &length);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  ULONGLONG idle_total = 0, kernel_total = 0, user_total = 0;
  for (ULONG i = 0; i + 48 <= length && i < sizeof(info); i += 48) {
    const ULONGLONG *entry = &info[i / 8];
    idle_total += entry[0]; kernel_total += entry[1]; user_total += entry[2];
  }
  if (idle) tz_ticks_to(idle_total, idle);
  if (kernel) tz_ticks_to(kernel_total, kernel);      /* (the kernel time has the idle time in it) */
  if (user) tz_ticks_to(user_total, user);
  return TRUE;
}

static ULONG cpu_count_now(void) {
  BYTE basic[64];
  if (!NT_SUCCESS(NtQuerySystemInformation(0, basic, sizeof(basic), NULL))) return 1;
  return basic[56] ? basic[56] : 1;
}
static BOOL idle_cycle_times(PULONG length, PULONG64 times) {
  if (!length) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const ULONG needed = cpu_count_now() * (ULONG)sizeof(ULONG64);
  if (!times) {
    if (*length == 0) *length = needed;
    return TRUE;
  }
  if (*length < needed) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  const ULONG64 now = __builtin_ia32_rdtsc();
  for (ULONG i = 0; i < needed / 8; ++i) times[i] = now;
  *length = needed;
  return TRUE;
}
EXPORT BOOL WINAPI QueryIdleProcessorCycleTime(PULONG length, PULONG64 times) { return idle_cycle_times(length, times); }
EXPORT BOOL WINAPI QueryIdleProcessorCycleTimeEx(USHORT group, PULONG length, PULONG64 times) {
  if (group != 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return idle_cycle_times(length, times);
}
