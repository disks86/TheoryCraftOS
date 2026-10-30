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

/* ntdll.dll: Rtl* time fields, IPv4 text, the environment block, DOS path names, generic tables, performance counters, the thread error mode. Written from the documented
 * behaviour of the Windows functions and checked with Wine's tests (tests/windows/wine/ntdll: rtl, time, path, env). Included by ntdll.c after rtl.inc. */

#define STATUS_VARIABLE_NOT_FOUND_ 0xC0000100u
#define STATUS_INVALID_ADDRESS_ 0xC0000141u
#define STATUS_INVALID_NETWORK_RESPONSE_ 0xC00000C3u

/* ---- time ---------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday; } RTL_TIME_FIELDS;
static int is_leap_year(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in_month(int y, int m) {
  static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap_year(y) ? 29 : d[m - 1];
}
EXPORT void NTAPI RtlTimeToTimeFields(const LONGLONG *time, RTL_TIME_FIELDS *f) {
  LONGLONG t = *time;
  if (t < 0) t = 0;
  const LONGLONG ticks_per_day = 864000000000ll;
  ULONGLONG days = (ULONGLONG)(t / ticks_per_day);
  LONGLONG rest = t % ticks_per_day;
  f->Weekday = (SHORT)((days + 1) % 7);      /* 1601-01-01 was a Monday */
  f->Hour = (SHORT)(rest / 36000000000ll); rest %= 36000000000ll;
  f->Minute = (SHORT)(rest / 600000000ll); rest %= 600000000ll;
  f->Second = (SHORT)(rest / 10000000ll); rest %= 10000000ll;
  f->Milliseconds = (SHORT)(rest / 10000);
  int year = 1601 + (int)(days / 146097) * 400;
  days %= 146097;
  for (;;) {
    const int len = is_leap_year(year) ? 366 : 365;
    if (days < (ULONGLONG)len) break;
    days -= (ULONGLONG)len;
    ++year;
  }
  int month = 1;
  while (days >= (ULONGLONG)days_in_month(year, month)) { days -= (ULONGLONG)days_in_month(year, month); ++month; }
  f->Year = (SHORT)year;
  f->Month = (SHORT)month;
  f->Day = (SHORT)(days + 1);
}
EXPORT BOOLEAN NTAPI RtlTimeFieldsToTime(const RTL_TIME_FIELDS *f, LONGLONG *time) {
  if (f->Milliseconds < 0 || f->Milliseconds > 999 || f->Second < 0 || f->Second > 59 || f->Minute < 0 || f->Minute > 59 || f->Hour < 0 || f->Hour > 23 ||
      f->Month < 1 || f->Month > 12 || f->Day < 1 || f->Year < 1601 || f->Day > days_in_month(f->Year, f->Month))
    return FALSE;
  LONGLONG days = 0;
  const int years = f->Year - 1601;
  days += (LONGLONG)(years / 400) * 146097;
  for (int y = 1601 + (years / 400) * 400; y < f->Year; ++y) days += is_leap_year(y) ? 366 : 365;
  for (int m = 1; m < f->Month; ++m) days += days_in_month(f->Year, m);
  days += f->Day - 1;
  *time = days * 864000000000ll + (LONGLONG)f->Hour * 36000000000ll + (LONGLONG)f->Minute * 600000000ll + (LONGLONG)f->Second * 10000000ll + (LONGLONG)f->Milliseconds * 10000;
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlTimeToSecondsSince1970(const LONGLONG *time, PULONG seconds) {
  const LONGLONG s = *time / 10000000ll - 11644473600ll;
  if (s < 0 || s > 0xFFFFFFFFll) return FALSE;
  *seconds = (ULONG)s;
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlTimeToSecondsSince1980(const LONGLONG *time, PULONG seconds) {
  const LONGLONG s = *time / 10000000ll - 11960006400ll;
  if (s < 0 || s > 0xFFFFFFFFll) return FALSE;
  *seconds = (ULONG)s;
  return TRUE;
}
EXPORT void NTAPI RtlSecondsSince1970ToTime(ULONG seconds, LONGLONG *time) { *time = ((LONGLONG)seconds + 11644473600ll) * 10000000ll; }
EXPORT void NTAPI RtlSecondsSince1980ToTime(ULONG seconds, LONGLONG *time) { *time = ((LONGLONG)seconds + 11960006400ll) * 10000000ll; }

EXPORT BOOLEAN NTAPI RtlQueryPerformanceCounter(PLARGE_INTEGER counter) { LARGE_INTEGER f; NtQueryPerformanceCounter(counter, &f); return TRUE; }
EXPORT BOOLEAN NTAPI RtlQueryPerformanceFrequency(PLARGE_INTEGER frequency) { LARGE_INTEGER c; NtQueryPerformanceCounter(&c, frequency); return TRUE; }
EXPORT BOOLEAN NTAPI RtlQueryUnbiasedInterruptTime(ULONGLONG *time) {
  if (!time) { TEB_LAST_ERROR(NtCurrentTeb()) = 87; return FALSE; }      /* ERROR_INVALID_PARAMETER */
  LARGE_INTEGER c, f;
  NtQueryPerformanceCounter(&c, &f);
  const ULONGLONG hz = (ULONGLONG)f.QuadPart, ticks = (ULONGLONG)c.QuadPart;
  *time = hz ? (ticks / hz) * 10000000u + (ticks % hz) * 10000000u / hz : 0;
  return TRUE;
}
EXPORT ULONG NTAPI NtGetTickCount(void) {
  LARGE_INTEGER c, f;
  NtQueryPerformanceCounter(&c, &f);
  return f.QuadPart ? (ULONG)((ULONGLONG)c.QuadPart / ((ULONGLONG)f.QuadPart / 1000)) : 0;
}

/* ---- the thread error mode ------------------------------------------------------------------------------------------------------------------------------- */

static struct { PVOID teb; ULONG mode; } g_error_modes[64];
static ULONG *error_mode_of(void) {
  PVOID teb = NtCurrentTeb();
  for (unsigned i = 0; i < 64; ++i) {
    if (g_error_modes[i].teb == teb) return &g_error_modes[i].mode;
    if (!g_error_modes[i].teb) { g_error_modes[i].teb = teb; return &g_error_modes[i].mode; }
  }
  return &g_error_modes[63].mode;
}
EXPORT ULONG NTAPI RtlGetThreadErrorMode(void) { return *error_mode_of(); }
EXPORT NTSTATUS NTAPI RtlSetThreadErrorMode(ULONG mode, PULONG old) {
  if (mode & ~0x70u) return STATUS_INVALID_PARAMETER_;
  ULONG *p = error_mode_of();
  if (old) *old = *p;
  *p = mode;
  return STATUS_SUCCESS;
}

/* ---- IPv4 addresses as text ---------------------------------------------------------------------------------------------------------------------------- */

static char *ipv4_text(const BYTE *a, char *out) {
  for (int i = 0; i < 4; ++i) {
    char tmp[4];
    const unsigned n = format_integer(a[i], 10, tmp);
    for (unsigned k = 0; k < n; ++k) *out++ = tmp[k];
    if (i < 3) *out++ = '.';
  }
  *out = 0;
  return out;
}
EXPORT char *NTAPI RtlIpv4AddressToStringA(const BYTE *addr, char *s) { return ipv4_text(addr, s); }
EXPORT WCHAR *NTAPI RtlIpv4AddressToStringW(const BYTE *addr, WCHAR *s) {
  char t[16];
  const char *end = ipv4_text(addr, t);
  for (const char *p = t; p <= end; ++p) *s++ = (WCHAR)(BYTE)*p;
  return s - 1;
}
EXPORT NTSTATUS NTAPI RtlIpv4AddressToStringExA(const BYTE *addr, USHORT port, char *s, PULONG len) {
  if (!addr || !s || !len) return STATUS_INVALID_PARAMETER_;
  char t[24];
  char *end = ipv4_text(addr, t);
  if (port) { *end++ = ':'; end += format_integer((USHORT)((port >> 8) | (port << 8)), 10, end); }
  const ULONG need = (ULONG)(end - t) + 1;
  if (*len < need) { *len = need; return STATUS_INVALID_PARAMETER_; }
  memcpy(s, t, need);
  *len = need;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlIpv4AddressToStringExW(const BYTE *addr, USHORT port, WCHAR *s, PULONG len) {
  char t[24];
  ULONG n = sizeof(t);
  NTSTATUS st = RtlIpv4AddressToStringExA(addr, port, t, &n);
  if (st) return st;
  if (*len < n) { *len = n; return STATUS_INVALID_PARAMETER_; }
  for (ULONG i = 0; i < n; ++i) s[i] = (WCHAR)(BYTE)t[i];
  *len = n;
  return STATUS_SUCCESS;
}
/* One address: up to four numbers separated by dots (decimal, 0x hex, 0 octal when not strict); the last number fills the remaining bytes. */
static NTSTATUS ipv4_parse(const WCHAR *s, int wide, int strict, const void **end, BYTE *addr, USHORT *port, int allow_port) {
  ULONGLONG parts[4];
  int n = 0;
  const void *p = s;
#define CH(q) (wide ? ((const WCHAR *)(q))[0] : (WCHAR)((const BYTE *)(q))[0])
#define ADV(q, k) ((q) = (wide ? (const void *)((const WCHAR *)(q) + (k)) : (const void *)((const BYTE *)(q) + (k))))
  for (;;) {
    int base = 10, digits = 0;
    ULONGLONG v = 0;
    if (CH(p) == '0' && !strict) {
      const void *q = p;
      ADV(q, 1);
      if (CH(q) == 'x' || CH(q) == 'X') { base = 16; ADV(q, 1); p = q; }
      else { base = 8; }
    }
    for (;;) {
      const WCHAR c = CH(p);
      const int d = c < 128 ? rtl_digit(c, (ULONG)base) : -1;
      if (d < 0) break;
      if (strict && base == 10 && digits == 1 && v == 0) return STATUS_INVALID_PARAMETER_;      /* a leading zero */
      v = v * (ULONGLONG)base + (ULONGLONG)d;
      if (v > 0xFFFFFFFFull) return STATUS_INVALID_PARAMETER_;
      ++digits;
      ADV(p, 1);
    }
    if (!digits && !(base == 8)) return STATUS_INVALID_PARAMETER_;
    if (strict && v > 255) return STATUS_INVALID_PARAMETER_;
    parts[n++] = v;
    if (CH(p) == '.' && n < 4) { ADV(p, 1); continue; }
    break;
  }
  if (strict && n != 4) return STATUS_INVALID_PARAMETER_;
  for (int i = 0; i + 1 < n; ++i) if (parts[i] > 255) return STATUS_INVALID_PARAMETER_;
  if (parts[n - 1] >= (1ull << (8 * (5 - n))) && n < 4 + 1 && n != 4) return STATUS_INVALID_PARAMETER_;
  if (n == 4 && parts[3] > 255) return STATUS_INVALID_PARAMETER_;
  for (int i = 0; i + 1 < n; ++i) addr[i] = (BYTE)parts[i];
  ULONGLONG last = parts[n - 1];
  for (int i = 3; i >= n - 1; --i) { addr[i] = (BYTE)last; last >>= 8; }
  if (allow_port && CH(p) == ':') {
    ADV(p, 1);
    ULONG v = 0;
    int digits = 0;
    while (CH(p) >= '0' && CH(p) <= '9') { v = v * 10 + (ULONG)(CH(p) - '0'); if (v > 65535) return STATUS_INVALID_PARAMETER_; ++digits; ADV(p, 1); }
    if (!digits) return STATUS_INVALID_PARAMETER_;
    *port = (USHORT)((v >> 8) | (v << 8));
  } else if (port) {
    *port = 0;
  }
  *end = p;
  return STATUS_SUCCESS;
#undef CH
#undef ADV
}
EXPORT NTSTATUS NTAPI RtlIpv4StringToAddressA(const char *s, BOOLEAN strict, const char **term, BYTE *addr) {
  const void *end = s;
  NTSTATUS st = ipv4_parse((const WCHAR *)s, 0, strict, &end, addr, NULL, 0);
  if (term) *term = st ? s : (const char *)end;
  return st;
}
EXPORT NTSTATUS NTAPI RtlIpv4StringToAddressW(const WCHAR *s, BOOLEAN strict, const WCHAR **term, BYTE *addr) {
  const void *end = s;
  NTSTATUS st = ipv4_parse(s, 1, strict, &end, addr, NULL, 0);
  if (term) *term = st ? s : (const WCHAR *)end;
  return st;
}
EXPORT NTSTATUS NTAPI RtlIpv4StringToAddressExA(const char *s, BOOLEAN strict, BYTE *addr, USHORT *port) {
  if (!s || !addr || !port) return STATUS_INVALID_PARAMETER_;
  const void *end = s;
  NTSTATUS st = ipv4_parse((const WCHAR *)s, 0, strict, &end, addr, port, 1);
  if (st == STATUS_SUCCESS && *(const char *)end) return STATUS_INVALID_PARAMETER_;
  return st;
}
EXPORT NTSTATUS NTAPI RtlIpv4StringToAddressExW(const WCHAR *s, BOOLEAN strict, BYTE *addr, USHORT *port) {
  if (!s || !addr || !port) return STATUS_INVALID_PARAMETER_;
  const void *end = s;
  NTSTATUS st = ipv4_parse(s, 1, strict, &end, addr, port, 1);
  if (st == STATUS_SUCCESS && *(const WCHAR *)end) return STATUS_INVALID_PARAMETER_;
  return st;
}

/* ---- the environment block ------------------------------------------------------------------------------------------------------------------------------- */

static const WCHAR *process_environment(void) {
  const RTL_USER_PROCESS_PARAMETERS *p = NtCurrentPeb()->ProcessParameters;
  return p ? (const WCHAR *)p->Environment : NULL;
}
/* The value of a variable (a pointer into the block and its length in characters), or NULL. Entries that begin with '=' (=C:=...) have the '=' in their name. */
static const WCHAR *env_find(const WCHAR *env, const WCHAR *name, SIZE_T name_len, SIZE_T *value_len) {
  if (!env) return NULL;
  while (*env) {
    const WCHAR *eq = env + 1;
    while (*eq && *eq != '=') ++eq;
    const SIZE_T len = wcslen(env);
    if (*eq == '=' && (SIZE_T)(eq - env) == name_len) {
      SIZE_T k = 0;
      while (k < name_len && rtl_upcase(env[k]) == rtl_upcase(name[k])) ++k;
      if (k == name_len) { *value_len = len - name_len - 1; return eq + 1; }
    }
    env += len + 1;
  }
  return NULL;
}
EXPORT NTSTATUS NTAPI RtlQueryEnvironmentVariable_U(PWSTR env, const UNICODE_STRING *name, PUNICODE_STRING value) {
  if (!name || !value) return STATUS_INVALID_PARAMETER_;
  SIZE_T len;
  const WCHAR *v = env_find(env ? env : process_environment(), name->Buffer, name->Length / 2, &len);
  if (!v) return STATUS_VARIABLE_NOT_FOUND_;
  value->Length = (USHORT)(len * 2 > 0xFFFE ? 0xFFFE : len * 2);
  if (len * 2 > value->MaximumLength) return STATUS_BUFFER_TOO_SMALL_;
  memcpy(value->Buffer, v, len * 2);
  if (len * 2 + 2 <= value->MaximumLength) value->Buffer[len] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlQueryEnvironmentVariable(PWSTR env, PWSTR name, SIZE_T name_len, PWSTR value, SIZE_T value_len, SIZE_T *needed) {
  SIZE_T len;
  const WCHAR *v = env_find(env ? env : process_environment(), name, name_len, &len);
  if (!v) return STATUS_VARIABLE_NOT_FOUND_;
  *needed = len;
  if (len > value_len) return STATUS_BUFFER_TOO_SMALL_;
  memcpy(value, v, len * 2);
  if (len < value_len) value[len] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlExpandEnvironmentStrings(PWSTR env, PWSTR src, SIZE_T src_len, PWSTR dst, SIZE_T dst_len, SIZE_T *out_len) {
  if (!env) env = (PWSTR)process_environment();
  SIZE_T needed = 0;
  SIZE_T i = 0;
  while (i < src_len) {
    SIZE_T j = i + 1;
    if (src[i] == '%') {
      while (j < src_len && src[j] != '%') ++j;
      if (j < src_len && j > i + 1) {
        SIZE_T vlen;
        const WCHAR *v = env_find(env, src + i + 1, j - i - 1, &vlen);
        if (v) {
          if (needed + vlen <= dst_len && dst) memcpy(dst + needed, v, vlen * 2);
          needed += vlen;
          i = j + 1;
          continue;
        }
      }
    }
    if (needed < dst_len && dst) dst[needed] = src[i];
    ++needed;
    ++i;
  }
  if (needed < dst_len && dst) dst[needed] = 0;
  ++needed;      /* the terminator */
  if (out_len) *out_len = needed;
  return needed > dst_len ? STATUS_BUFFER_TOO_SMALL_ : STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlExpandEnvironmentStrings_U(PWSTR env, const UNICODE_STRING *src, PUNICODE_STRING dst, PULONG out_len) {
  SIZE_T needed = 0;
  const NTSTATUS st = RtlExpandEnvironmentStrings(env, src->Buffer, src->Length / 2, dst->Buffer, dst->MaximumLength / 2, &needed);
  if (out_len) *out_len = (ULONG)(needed * 2);
  if (!st) dst->Length = (USHORT)((needed - 1) * 2);
  return st;
}

/* ---- DOS path names ------------------------------------------------------------------------------------------------------------------------------------ */

#define IS_SEP(c) ((c) == '\\' || (c) == '/')
EXPORT ULONG NTAPI RtlDetermineDosPathNameType_U(PCWSTR path) {
  if (!path || !path[0]) return 0;      /* UNKNOWN */
  if (IS_SEP(path[0])) {
    if (!IS_SEP(path[1])) return 4;      /* ROOTED */
    if (path[2] != '.' && path[2] != '?') return 1;      /* UNC_ABSOLUTE */
    if (IS_SEP(path[3])) return 6;      /* LOCAL_DEVICE */
    if (!path[3]) return 7;      /* ROOT_LOCAL_DEVICE */
    return 1;
  }
  if (path[0] && path[1] == ':') return IS_SEP(path[2]) ? 2 : 3;      /* DRIVE_ABSOLUTE / DRIVE_RELATIVE */
  return 5;      /* RELATIVE */
}
EXPORT ULONG NTAPI RtlDetermineDosPathNameType_Ustr(const UNICODE_STRING *path) {
  WCHAR tmp[8] = {0};
  for (unsigned i = 0; i < 4 && i < path->Length / 2u; ++i) tmp[i] = path->Buffer[i];
  return RtlDetermineDosPathNameType_U(tmp);
}
static void current_directory(WCHAR *out, ULONG capacity) {      /* with a trailing backslash */
  const RTL_USER_PROCESS_PARAMETERS *p = NtCurrentPeb()->ProcessParameters;
  ULONG n = 0;
  if (p && p->CurrentDirectoryPath.Buffer) {
    n = p->CurrentDirectoryPath.Length / 2;
    if (n >= capacity) n = capacity - 1;
    memcpy(out, p->CurrentDirectoryPath.Buffer, n * 2);
  } else {
    static const WCHAR root[] = {'C', ':', '\\'};
    n = 3;
    memcpy(out, root, 6);
  }
  if (n && !IS_SEP(out[n - 1]) && n + 1 < capacity) out[n++] = '\\';
  out[n] = 0;
}
/* The canonical form of a DOS path in `out` (backslashes, no "." or ".." parts, no repeated separators); returns the length in characters. */
static ULONG canonical_path(const WCHAR *path, WCHAR *out, ULONG capacity, ULONG *file_part) {
  WCHAR work[1024];
  ULONG n = 0;
  const ULONG type = RtlDetermineDosPathNameType_U(path);
  ULONG prefix = 0;      /* characters of `work` that ".." must not remove */
  if (type == 6 || type == 7) {
    /* \\.\ and \\?\ names are taken as they are (with the slashes made into backslashes) */
    for (; path[n] && n < 1000; ++n) work[n] = path[n] == '/' ? '\\' : path[n];
    work[n] = 0;
    if (file_part) *file_part = 0;
    if (n + 1 > capacity) return n + 1;
    memcpy(out, work, (n + 1) * 2);
    return n;
  }
  const WCHAR *rest = path;
  if (type == 1) {
    work[n++] = '\\'; work[n++] = '\\';
    rest += 2;
    int slashes = 0;
    while (*rest && slashes < 2) {      /* server and share */
      if (IS_SEP(*rest)) { ++slashes; if (slashes < 2) work[n++] = '\\'; else break; ++rest; while (IS_SEP(*rest)) ++rest; continue; }
      work[n++] = *rest++;
    }
    prefix = n;
  } else if (type == 2) {
    work[n++] = path[0]; work[n++] = ':'; work[n++] = '\\';
    rest += 3;
    prefix = n;
  } else if (type == 3) {
    WCHAR cwd[260];
    current_directory(cwd, 260);
    if (rtl_upcase(cwd[0]) == rtl_upcase(path[0]) && cwd[1] == ':') {
      for (ULONG k = 0; cwd[k] && n < 600; ++k) work[n++] = cwd[k];
    } else {
      work[n++] = path[0]; work[n++] = ':'; work[n++] = '\\';
    }
    prefix = 3;
    rest += 2;
  } else if (type == 4) {
    WCHAR cwd[260];
    current_directory(cwd, 260);
    if (cwd[1] == ':') { work[n++] = cwd[0]; work[n++] = ':'; }
    work[n++] = '\\';
    prefix = n;
    rest += 1;
  } else {
    WCHAR cwd[260];
    current_directory(cwd, 260);
    for (ULONG k = 0; cwd[k] && n < 600; ++k) work[n++] = cwd[k];
    prefix = cwd[1] == ':' ? 3 : 1;
  }
  /* the rest: components */
  while (*rest) {
    while (IS_SEP(*rest)) ++rest;
    if (!*rest) break;
    const WCHAR *start = rest;
    while (*rest && !IS_SEP(*rest)) ++rest;
    const ULONG len = (ULONG)(rest - start);
    if (len == 1 && start[0] == '.') continue;
    if (len == 2 && start[0] == '.' && start[1] == '.') {
      if (n > prefix) {
        --n;      /* the trailing backslash */
        while (n > prefix && work[n - 1] != '\\') --n;
      }
      continue;
    }
    if (n && work[n - 1] != '\\' && n < 1000) work[n++] = '\\';
    for (ULONG k = 0; k < len && n < 1000; ++k) work[n++] = start[k];
    if (*rest && n < 1000) work[n++] = '\\';
  }
  /* a path that ends in a separator keeps it; a drive or share root is "X:\" */
  const WCHAR *last = path;
  while (*last) ++last;
  const int ends_in_sep = last > path && IS_SEP(last[-1]);
  if (ends_in_sep && n && work[n - 1] != '\\') work[n++] = '\\';
  if (!ends_in_sep && n > prefix && work[n - 1] == '\\') --n;
  work[n] = 0;
  if (file_part) {
    ULONG k = n;
    while (k > 0 && work[k - 1] != '\\') --k;
    *file_part = (n > prefix && !ends_in_sep && k < n) ? k : 0;
  }
  if (n + 1 > capacity) return n + 1;
  memcpy(out, work, (n + 1) * 2);
  return n;
}
EXPORT ULONG NTAPI RtlGetFullPathName_U(PCWSTR name, ULONG size, PWSTR buffer, PWSTR *file_part) {
  if (!name || !name[0]) return 0;
  ULONG fp = 0;
  const ULONG chars = canonical_path(name, buffer, size / 2, &fp);
  if (chars + 1 > size / 2) { if (file_part) *file_part = NULL; return chars * 2 + 2; }
  if (file_part) *file_part = fp ? buffer + fp : NULL;
  return chars * 2;
}
EXPORT NTSTATUS NTAPI RtlGetFullPathName_UEx(PWSTR name, ULONG size, PWSTR buffer, PWSTR *file_part, ULONG *type) {
  if (type) *type = RtlDetermineDosPathNameType_U(name);
  const ULONG r = RtlGetFullPathName_U(name, size, buffer, file_part);
  return r ? STATUS_SUCCESS : STATUS_OBJECT_NAME_INVALID;
}
EXPORT NTSTATUS NTAPI RtlDosPathNameToNtPathName_U_WithStatus(PCWSTR dos, PUNICODE_STRING nt, PWSTR *file_part, PVOID cd) {
  WCHAR full[1024];
  ULONG fp = 0;
  if (!dos || !dos[0]) return STATUS_OBJECT_NAME_INVALID;
  const ULONG chars = canonical_path(dos, full, 1024, &fp);
  if (chars + 1 > 1024) return STATUS_NAME_TOO_LONG_;
  static const WCHAR dev[] = {'\\', '?', '?', '\\'}, unc[] = {'U', 'N', 'C', '\\'};
  ULONG skip = 0, n = 0;
  WCHAR *out = rtl_alloc((chars + 12) * 2);
  if (!out) return STATUS_NO_MEMORY_;
  memcpy(out, dev, 8);
  n = 4;
  if (full[0] == '\\' && full[1] == '\\' && (full[2] == '.' || full[2] == '?') && full[3] == '\\') skip = 4;
  else if (full[0] == '\\' && full[1] == '\\') { memcpy(out + n, unc, 8); n += 4; skip = 2; }
  for (ULONG k = skip; k < chars; ++k) out[n++] = full[k];
  out[n] = 0;
  nt->Buffer = out;
  nt->Length = (USHORT)(n * 2);
  nt->MaximumLength = (USHORT)((chars + 12) * 2);
  if (file_part) *file_part = fp ? out + (fp - skip) + (n - (chars - skip)) : NULL;
  if (cd) memset(cd, 0, 16);
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlDosPathNameToNtPathName_U(PCWSTR dos, PUNICODE_STRING nt, PWSTR *file_part, PVOID cd) { return RtlDosPathNameToNtPathName_U_WithStatus(dos, nt, file_part, cd) == STATUS_SUCCESS; }
EXPORT ULONG NTAPI RtlIsDosDeviceName_U(PCWSTR name) {
  static const char *const base[] = {"CON", "PRN", "AUX", "NUL"};
  const WCHAR *p = name;
  if (IS_SEP(p[0]) && IS_SEP(p[1]) && (p[2] == '.' || p[2] == '?') && IS_SEP(p[3])) p += 4;
  else if (p[0] && p[1] == ':') p += 2;
  const WCHAR *start = p;
  for (const WCHAR *q = p; *q; ++q) if (IS_SEP(*q)) start = q + 1;
  ULONG len = 0;
  while (start[len] && start[len] != '.' && start[len] != ':') ++len;
  ULONG trimmed = len;
  while (trimmed && start[trimmed - 1] == ' ') --trimmed;
  int match = 0;
  for (int i = 0; i < 4 && !match; ++i) {
    unsigned k = 0;
    while (base[i][k] && k < trimmed && rtl_upcase(start[k]) == (WCHAR)base[i][k]) ++k;
    match = base[i][k] == 0 && k == trimmed;
  }
  if (!match && trimmed == 4 && start[3] >= '1' && start[3] <= '9') {
    const char *const n3[] = {"COM", "LPT"};
    for (int i = 0; i < 2 && !match; ++i) match = rtl_upcase(start[0]) == (WCHAR)n3[i][0] && rtl_upcase(start[1]) == (WCHAR)n3[i][1] && rtl_upcase(start[2]) == (WCHAR)n3[i][2];
  }
  if (!match) return 0;
  return ((ULONG)((start - name) * 2) << 16) | (trimmed * 2);
}

/* ---- generic tables (kept as lists in key order) ------------------------------------------------------------------------------------------------------------- */

typedef struct _RTL_GENERIC_TABLE_ RTL_GENERIC_TABLE_;
typedef struct GTNode { struct GTNode *prev, *next; BYTE pad[16]; } GTNode;      /* in front of the caller's data */
struct _RTL_GENERIC_TABLE_ {
  PVOID TableRoot;      /* the first node */
  PVOID unused[2];
  PVOID OrderedPointer;
  ULONG WhichOrderedElement, NumberGenericTableElements;
  ULONG (NTAPI *CompareRoutine)(RTL_GENERIC_TABLE_ *, PVOID, PVOID);
  PVOID (NTAPI *AllocateRoutine)(RTL_GENERIC_TABLE_ *, ULONG);
  void (NTAPI *FreeRoutine)(RTL_GENERIC_TABLE_ *, PVOID);
  PVOID TableContext;
};
#define GT_DATA(n) ((PVOID)((BYTE *)(n) + sizeof(GTNode)))
#define GT_NODE(d) ((GTNode *)((BYTE *)(d) - sizeof(GTNode)))
EXPORT void NTAPI RtlInitializeGenericTable(RTL_GENERIC_TABLE_ *t, ULONG (NTAPI *cmp)(RTL_GENERIC_TABLE_ *, PVOID, PVOID), PVOID (NTAPI *alloc)(RTL_GENERIC_TABLE_ *, ULONG),
                                            void (NTAPI *free_)(RTL_GENERIC_TABLE_ *, PVOID), PVOID context) {
  memset(t, 0, sizeof(*t));
  t->CompareRoutine = cmp;
  t->AllocateRoutine = alloc;
  t->FreeRoutine = free_;
  t->TableContext = context;
}
EXPORT BOOLEAN NTAPI RtlIsGenericTableEmpty(RTL_GENERIC_TABLE_ *t) { return t->NumberGenericTableElements == 0; }
EXPORT ULONG NTAPI RtlNumberGenericTableElements(RTL_GENERIC_TABLE_ *t) { return t->NumberGenericTableElements; }
/* The node equal to the key (or NULL), and the node after which the key would go (NULL = at the front). */
static GTNode *gt_find(RTL_GENERIC_TABLE_ *t, PVOID key, GTNode **before) {
  GTNode *prev = NULL;
  for (GTNode *n = t->TableRoot; n; n = n->next) {
    const ULONG c = t->CompareRoutine(t, key, GT_DATA(n));      /* 0 less, 1 greater, 2 equal */
    if (c == 2) { if (before) *before = prev; return n; }
    if (c == 0) break;
    prev = n;
  }
  if (before) *before = prev;
  return NULL;
}
EXPORT PVOID NTAPI RtlInsertElementGenericTable(RTL_GENERIC_TABLE_ *t, PVOID buffer, ULONG size, BOOLEAN *is_new) {
  GTNode *before;
  GTNode *found = gt_find(t, buffer, &before);
  if (found) { if (is_new) *is_new = FALSE; return GT_DATA(found); }
  GTNode *n = t->AllocateRoutine(t, size + (ULONG)sizeof(GTNode));
  if (!n) { if (is_new) *is_new = FALSE; return NULL; }
  memset(n, 0, sizeof(GTNode));
  memcpy(GT_DATA(n), buffer, size);
  if (before) { n->prev = before; n->next = before->next; if (before->next) before->next->prev = n; before->next = n; }
  else { n->next = t->TableRoot; if (n->next) n->next->prev = n; t->TableRoot = n; }
  ++t->NumberGenericTableElements;
  t->WhichOrderedElement = 0;
  t->OrderedPointer = NULL;
  if (is_new) *is_new = TRUE;
  return GT_DATA(n);
}
EXPORT PVOID NTAPI RtlInsertElementGenericTableFull(RTL_GENERIC_TABLE_ *t, PVOID buffer, ULONG size, BOOLEAN *is_new, PVOID node, ULONG result) { return RtlInsertElementGenericTable(t, buffer, size, is_new); }
EXPORT PVOID NTAPI RtlLookupElementGenericTable(RTL_GENERIC_TABLE_ *t, PVOID buffer) { GTNode *n = gt_find(t, buffer, NULL); return n ? GT_DATA(n) : NULL; }
EXPORT PVOID NTAPI RtlLookupElementGenericTableFull(RTL_GENERIC_TABLE_ *t, PVOID buffer, PVOID *node, ULONG *result) { PVOID r = RtlLookupElementGenericTable(t, buffer); if (node) *node = NULL; if (result) *result = r ? 2 : 1; return r; }
EXPORT BOOLEAN NTAPI RtlDeleteElementGenericTable(RTL_GENERIC_TABLE_ *t, PVOID buffer) {
  GTNode *n = gt_find(t, buffer, NULL);
  if (!n) return FALSE;
  if (n->prev) n->prev->next = n->next; else t->TableRoot = n->next;
  if (n->next) n->next->prev = n->prev;
  --t->NumberGenericTableElements;
  t->WhichOrderedElement = 0;
  t->OrderedPointer = NULL;
  t->FreeRoutine(t, n);
  return TRUE;
}
EXPORT PVOID NTAPI RtlGetElementGenericTable(RTL_GENERIC_TABLE_ *t, ULONG index) {
  GTNode *n = t->TableRoot;
  while (n && index--) n = n->next;
  return n ? GT_DATA(n) : NULL;
}
EXPORT PVOID NTAPI RtlEnumerateGenericTable(RTL_GENERIC_TABLE_ *t, BOOLEAN restart) {
  GTNode *n;
  if (restart || !t->OrderedPointer) n = t->TableRoot;
  else n = ((GTNode *)t->OrderedPointer)->next;
  t->OrderedPointer = n;
  return n ? GT_DATA(n) : NULL;
}
EXPORT PVOID NTAPI RtlEnumerateGenericTableWithoutSplaying(RTL_GENERIC_TABLE_ *t, PVOID *restart_key) {
  GTNode *n = *restart_key ? GT_NODE(*restart_key)->next : t->TableRoot;
  *restart_key = n ? GT_DATA(n) : *restart_key;
  return n ? GT_DATA(n) : NULL;
}

/* ---- locale names and numbers ------------------------------------------------------------------------------------------------------------------------------ */

#include "locale_names.inc"
#define NLS_STATUS_INVALID_PARAMETER_1 ((NTSTATUS)0xC00000EF)
#define NLS_STATUS_INVALID_PARAMETER_2 ((NTSTATUS)0xC00000F0)
#define NLS_STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define NLS_STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define NLS_RTL_ALLOW_NEUTRAL 2u
static WCHAR nls_low(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
/* Names are compared without regard to case, and an underscore is a hyphen. */
static BOOL nls_name_matches(const WCHAR *name, const char *text) {
  for (; *text; ++text, ++name) {
    WCHAR a = nls_low(*name), b = (WCHAR)nls_low((BYTE)*text);
    if (a == '_') a = '-';
    if (b == '_') b = '-';
    if (a != b) return FALSE;
  }
  return *name == 0;
}
/* The number of the locale with a name in the table, or -1. */
static int nls_find_name(const WCHAR *name) {
  if (!name) return -1;
  for (int i = 0; i < RTL_LOCALE_COUNT; ++i) if (nls_name_matches(name, k_rtl_locale_names + k_rtl_locale_name[i]) && *name) return i;
  return -1;
}
static int nls_find_lcid(ULONG lcid) {
  int lo = 0, hi = RTL_LOCALE_COUNT;
  while (lo < hi) {
    const int mid = lo + (hi - lo) / 2;
    if (k_rtl_locale_lcid[mid] < lcid) lo = mid + 1; else if (k_rtl_locale_lcid[mid] > lcid) hi = mid; else return mid;
  }
  return -1;
}
static BOOL nls_is_neutral_entry(int i) { return k_rtl_locale_default[i] != i; }
EXPORT BOOLEAN NTAPI RtlIsValidLocaleName(const WCHAR *name, ULONG flags) {
  if (!name) return FALSE;
  if (*name == 0) return TRUE;
  const int i = nls_find_name(name);
  return i >= 0 && (!nls_is_neutral_entry(i) || (flags & NLS_RTL_ALLOW_NEUTRAL));
}
EXPORT NTSTATUS NTAPI RtlLocaleNameToLcid(const WCHAR *name, ULONG *lcid, ULONG flags) {
  if (!name) return NLS_STATUS_INVALID_PARAMETER_1;
  if (*name == 0) { *lcid = 0x7F; return 0; }
  const int i = nls_find_name(name);
  if (i < 0 || (nls_is_neutral_entry(i) && !(flags & NLS_RTL_ALLOW_NEUTRAL))) return NLS_STATUS_INVALID_PARAMETER_1;
  *lcid = k_rtl_locale_lcid[i];
  return 0;
}
EXPORT NTSTATUS NTAPI RtlLcidToLocaleName(ULONG lcid, UNICODE_STRING *name, ULONG flags, BOOLEAN allocate) {
  const char *text;
  switch (lcid) {
    case 0x800: case 0x400: case 0xC00: case 0x1400: lcid = 0x409; break;
    case 0: case 0x1000: return NLS_STATUS_INVALID_PARAMETER_1;
    default: break;
  }
  const int i = nls_find_lcid(lcid);
  if (i < 0 || (nls_is_neutral_entry(i) && !(flags & NLS_RTL_ALLOW_NEUTRAL))) return NLS_STATUS_INVALID_PARAMETER_1;
  if (!name) return NLS_STATUS_INVALID_PARAMETER_2;
  text = k_rtl_locale_names + k_rtl_locale_name[i];
  USHORT length = 0;
  while (text[length]) ++length;
  const USHORT bytes = (USHORT)(length * 2);
  if (allocate) {
    name->Buffer = RtlAllocateHeap(RtlProcessHeap(), 0, (SIZE_T)bytes + 2);
    if (!name->Buffer) return STATUS_NO_MEMORY;
    name->MaximumLength = (USHORT)(bytes + 2);
  } else if (name->MaximumLength < bytes + 2) return NLS_STATUS_BUFFER_TOO_SMALL;
  for (USHORT k = 0; k <= length; ++k) name->Buffer[k] = (WCHAR)(BYTE)text[k];
  name->Length = bytes;
  return 0;
}

/* ---- interlocked singly linked lists --------------------------------------------------------------------------------------------------------------------- */

/* The header is the x64 one of Windows: the first quadword has the depth (16 bits) and a sequence number (48), the second the address of the first entry shifted
 * right by 4 (the entries are 16-aligned). One lock covers every list. */
static volatile LONG g_slist_lock;
static void slist_lock(void) { while (__sync_lock_test_and_set(&g_slist_lock, 1)) {} }
static void slist_unlock(void) { __sync_lock_release(&g_slist_lock); }
typedef struct { ULONGLONG depth_sequence, next; } SlistHeader;
static PSLIST_ENTRY slist_first(const SlistHeader *h) { return (PSLIST_ENTRY)(ULONG_PTR)(h->next & ~0xFull); }
static void slist_set(SlistHeader *h, PSLIST_ENTRY first, ULONGLONG depth) {
  h->depth_sequence = ((h->depth_sequence + 0x10000) & ~0xFFFFull) | (depth & 0xFFFF);
  h->next = (ULONGLONG)(ULONG_PTR)first & ~0xFull;
}
EXPORT void NTAPI RtlInitializeSListHead(PSLIST_HEADER head) { SlistHeader *h = (SlistHeader *)head; h->depth_sequence = 0; h->next = 0; }
EXPORT PSLIST_ENTRY NTAPI RtlFirstEntrySList(const SLIST_HEADER *head) { return slist_first((const SlistHeader *)head); }
EXPORT USHORT NTAPI RtlQueryDepthSList(PSLIST_HEADER head) { return (USHORT)(((SlistHeader *)head)->depth_sequence & 0xFFFF); }
EXPORT PSLIST_ENTRY NTAPI RtlInterlockedPushListSListEx(PSLIST_HEADER head, PSLIST_ENTRY first, PSLIST_ENTRY last, ULONG count) {
  SlistHeader *h = (SlistHeader *)head;
  slist_lock();
  PSLIST_ENTRY old = slist_first(h);
  last->Next = old;
  slist_set(h, first, (h->depth_sequence & 0xFFFF) + count);
  slist_unlock();
  return old;
}
EXPORT PSLIST_ENTRY NTAPI RtlInterlockedPushListSList(PSLIST_HEADER head, PSLIST_ENTRY first, PSLIST_ENTRY last, ULONG count) { return RtlInterlockedPushListSListEx(head, first, last, count); }
EXPORT PSLIST_ENTRY NTAPI RtlInterlockedPushEntrySList(PSLIST_HEADER head, PSLIST_ENTRY entry) { return RtlInterlockedPushListSListEx(head, entry, entry, 1); }
EXPORT PSLIST_ENTRY NTAPI RtlInterlockedPopEntrySList(PSLIST_HEADER head) {
  SlistHeader *h = (SlistHeader *)head;
  slist_lock();
  PSLIST_ENTRY top = slist_first(h);
  if (top) slist_set(h, top->Next, (h->depth_sequence & 0xFFFF) - 1);
  slist_unlock();
  return top;
}
EXPORT PSLIST_ENTRY NTAPI RtlInterlockedFlushSList(PSLIST_HEADER head) {
  SlistHeader *h = (SlistHeader *)head;
  slist_lock();
  PSLIST_ENTRY top = slist_first(h);
  if (top) slist_set(h, NULL, 0);
  slist_unlock();
  return top;
}

/* ---- PE image helpers ------------------------------------------------------------------------------------------------------------------------------------ */

/* The headers of an image mapped at `base` (a base with the low bit set is a file mapped as data). Raw offsets: the e_lfanew at 0x3C; the signature, file header (6 sections count,
 * 20 optional header size) and optional header (PE32+: data directories from 112) follow it. */
static BYTE *image_nt_headers(PVOID base) {
  BYTE *image = (BYTE *)((ULONG_PTR)base & ~(ULONG_PTR)1);
  if (!image || image[0] != 'M' || image[1] != 'Z') return NULL;
  const LONG lfanew = *(const LONG *)(image + 0x3C);
  if (lfanew < 0 || lfanew > 0x10000000) return NULL;
  BYTE *nt = image + lfanew;
  if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) return NULL;
  return nt;
}
EXPORT PVOID NTAPI RtlImageNtHeader(PVOID base) { return image_nt_headers(base); }
EXPORT NTSTATUS NTAPI RtlImageNtHeaderEx(ULONG flags, PVOID base, ULONGLONG size, PVOID *headers) {
  if (!headers) return STATUS_INVALID_PARAMETER;
  *headers = NULL;
  if (flags & ~(ULONG)1) return STATUS_INVALID_PARAMETER;
  if (!base || base == (PVOID)(ULONG_PTR)-1) return STATUS_INVALID_PARAMETER;
  BYTE *image = (BYTE *)((ULONG_PTR)base & ~(ULONG_PTR)1);
  if (!(flags & 1) && size < 0x40) return (NTSTATUS)0xC000007Bu;
  BYTE *nt = image_nt_headers(base);
  if (!nt) return (NTSTATUS)0xC000007Bu;
  if (!(flags & 1) && (ULONGLONG)(nt - image) + 0x108 > size) return (NTSTATUS)0xC000007Bu;
  *headers = nt;
  return STATUS_SUCCESS;
}
EXPORT PVOID NTAPI RtlImageRvaToSection(PVOID nt_headers, PVOID base, ULONG rva) {
  (void)base;
  BYTE *nt = nt_headers;
  const USHORT count = *(const USHORT *)(nt + 6), optional_size = *(const USHORT *)(nt + 20);
  BYTE *section = nt + 24 + optional_size;
  for (USHORT i = 0; i < count; ++i, section += 40) {
    const ULONG virtual_size = *(const ULONG *)(section + 8), raw_size = *(const ULONG *)(section + 16), address = *(const ULONG *)(section + 12);
    const ULONG span = virtual_size > raw_size ? virtual_size : raw_size;
    if (rva >= address && rva - address < span) return section;
  }
  return NULL;
}
EXPORT PVOID NTAPI RtlImageRvaToVa(PVOID nt_headers, PVOID base, ULONG rva, PVOID *last_section) {
  BYTE *section = NULL;
  if (last_section) section = *last_section;
  if (section) {      /* the section the last answer came from, if the address is in it too */
    const ULONG virtual_size = *(const ULONG *)(section + 8), raw_size = *(const ULONG *)(section + 16), address = *(const ULONG *)(section + 12);
    if (!(rva >= address && rva - address < (virtual_size > raw_size ? virtual_size : raw_size))) section = NULL;
  }
  if (!section) section = RtlImageRvaToSection(nt_headers, base, rva);
  if (!section) return NULL;
  if (last_section) *last_section = section;
  return (BYTE *)base + (rva - *(const ULONG *)(section + 12)) + *(const ULONG *)(section + 20);
}
EXPORT PVOID NTAPI RtlImageDirectoryEntryToData(PVOID base, BOOLEAN mapped_as_image, USHORT directory, PULONG size) {
  BYTE *nt = image_nt_headers(base);
  if (!nt) return NULL;
  const USHORT magic = *(const USHORT *)(nt + 24);
  if (magic != 0x20B) return NULL;      /* only PE32+ here */
  const ULONG count = *(const ULONG *)(nt + 24 + 108);
  if (directory >= count || directory >= 16) return NULL;
  const ULONG rva = *(const ULONG *)(nt + 24 + 112 + directory * 8), length = *(const ULONG *)(nt + 24 + 112 + directory * 8 + 4);
  if (!rva) return NULL;
  if (size) *size = length;
  BYTE *image = (BYTE *)((ULONG_PTR)base & ~(ULONG_PTR)1);
  if (mapped_as_image || rva < *(const ULONG *)(nt + 24 + 60)) return image + rva;      /* (inside the headers: the same either way) */
  return RtlImageRvaToVa(nt, image, rva, NULL);
}
