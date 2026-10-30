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
  if (mode & ~0x70u) return 0xC00000EFu;      /* STATUS_INVALID_PARAMETER_1 */
  ULONG *p = error_mode_of();
  if (old) *old = *p;
  *p = mode;
  *(ULONG *)((BYTE *)NtCurrentTeb() + 0x16B0) = mode;      /* (TEB.HardErrorMode) */
  return STATUS_SUCCESS;
}

/* ---- IP addresses as text (as Wine's ntdll does them) ---------------------------------------------------------------------------------------------------- */

typedef struct { union { struct { BYTE s_b1, s_b2, s_b3, s_b4; } S_un_b; struct { USHORT s_w1, s_w2; } S_un_w; ULONG S_addr; } S_un; } IN_ADDR;
typedef struct { union { BYTE Byte[16]; USHORT Word[8]; } u; } IN6_ADDR;
#define s6_words u.Word
#define s6_bytes u.Byte
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
USHORT NTAPI RtlUshortByteSwap(USHORT v);
#undef htons
#undef ntohs

#define htons(x) RtlUshortByteSwap(x)
#define ntohs(x) RtlUshortByteSwap(x)

static const signed char hex_table[] =
{
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, /* 0x00-0x0F */
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, /* 0x10-0x1F */
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, /* 0x20-0x2F */
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1, /* 0x30-0x3F */
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, /* 0x40-0x4F */
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, /* 0x50-0x5F */
    -1, 10, 11, 12, 13, 14, 15                                      /* 0x60-0x66 */
};

static BOOL parse_ipv4_component(const WCHAR **str, BOOL strict, ULONG *value)
{
    int base = 10, d;
    WCHAR c;
    ULONG cur_value, prev_value = 0;
    BOOL success = FALSE;

    if (**str == '.')
    {
        *str += 1;
        return FALSE;
    }

    if ((*str)[0] == '0')
    {
        if ((*str)[1] == 'x' || (*str)[1] == 'X')
        {
            *str += 2;
            if (strict) return FALSE;
            base = 16;
        }
        else if ((*str)[1] >= '0' && (*str)[1] <= '9')
        {
            *str += 1;
            if (strict) return FALSE;
            base = 8;
        }
    }

    for (cur_value = 0; **str; *str += 1)
    {
        c = **str;
        if (c >= ARRAY_SIZE(hex_table)) break;
        d = hex_table[c];
        if (d == -1 || d >= base) break;
        cur_value = cur_value * base + d;
        success = TRUE;
        if (cur_value < prev_value) return FALSE; /* overflow */
        prev_value = cur_value;
    }

    if (success) *value = cur_value;
    return success;
}

static NTSTATUS ipv4_string_to_address(const WCHAR *str, BOOL strict,
                                       const WCHAR **terminator, IN_ADDR *address, USHORT *port)
{
    ULONG fields[4];
    int n = 0;

    for (;;)
    {
        if (!parse_ipv4_component(&str, strict, &fields[n]))
            goto error;
        n++;
        if (*str != '.')
            break;
        if (n == 4)
            goto error;
        str++;
    }

    if (strict && n < 4)
        goto error;

    switch (n)
    {
        case 4:
            if (fields[0] > 0xFF || fields[1] > 0xFF || fields[2] > 0xFF || fields[3] > 0xFF)
                goto error;
            address->S_un.S_un_b.s_b1 = fields[0];
            address->S_un.S_un_b.s_b2 = fields[1];
            address->S_un.S_un_b.s_b3 = fields[2];
            address->S_un.S_un_b.s_b4 = fields[3];
            break;
        case 3:
            if (fields[0] > 0xFF || fields[1] > 0xFF || fields[2] > 0xFFFF)
                goto error;
            address->S_un.S_un_b.s_b1 = fields[0];
            address->S_un.S_un_b.s_b2 = fields[1];
            address->S_un.S_un_b.s_b3 = (fields[2] & 0xFF00) >> 8;
            address->S_un.S_un_b.s_b4 = (fields[2] & 0x00FF);
            break;
        case 2:
            if (fields[0] > 0xFF || fields[1] > 0xFFFFFF)
                goto error;
            address->S_un.S_un_b.s_b1 = fields[0];
            address->S_un.S_un_b.s_b2 = (fields[1] & 0xFF0000) >> 16;
            address->S_un.S_un_b.s_b3 = (fields[1] & 0x00FF00) >> 8;
            address->S_un.S_un_b.s_b4 = (fields[1] & 0x0000FF);
            break;
        case 1:
            address->S_un.S_un_b.s_b1 = (fields[0] & 0xFF000000) >> 24;
            address->S_un.S_un_b.s_b2 = (fields[0] & 0x00FF0000) >> 16;
            address->S_un.S_un_b.s_b3 = (fields[0] & 0x0000FF00) >> 8;
            address->S_un.S_un_b.s_b4 = (fields[0] & 0x000000FF);
            break;
        default:
            goto error;
    }

    if (terminator) *terminator = str;

    if (*str == ':')
    {
        str++;
        if (!parse_ipv4_component(&str, FALSE, &fields[0]))
            goto error;
        if (!fields[0] || fields[0] > 0xFFFF || *str)
            goto error;
        if (port)
        {
            *port = htons(fields[0]);
            if (terminator) *terminator = str;
        }
    }

    if (!terminator && *str)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;

error:
    if (terminator) *terminator = str;
    return STATUS_INVALID_PARAMETER;
}

/***********************************************************************
 * RtlIpv4StringToAddressExW [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv4StringToAddressExW(const WCHAR *str, BOOLEAN strict, IN_ADDR *address, USHORT *port)
{
    if (!str || !address || !port) return STATUS_INVALID_PARAMETER;
    return ipv4_string_to_address(str, strict, NULL, address, port);
}

/***********************************************************************
 * RtlIpv4StringToAddressW [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv4StringToAddressW(const WCHAR *str, BOOLEAN strict, const WCHAR **terminator, IN_ADDR *address)
{
    return ipv4_string_to_address(str, strict, terminator, address, NULL);
}

/***********************************************************************
 * RtlIpv4StringToAddressExA [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv4StringToAddressExA(const char *str, BOOLEAN strict, IN_ADDR *address, USHORT *port)
{
    WCHAR wstr[32];

    if (!str) return STATUS_INVALID_PARAMETER;
    RtlMultiByteToUnicodeN(wstr, sizeof(wstr), NULL, str, strlen(str) + 1);
    wstr[ARRAY_SIZE(wstr) - 1] = 0;
    return RtlIpv4StringToAddressExW(wstr, strict, address, port);
}

/***********************************************************************
 * RtlIpv4StringToAddressA [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv4StringToAddressA(const char *str, BOOLEAN strict, const char **terminator, IN_ADDR *address)
{
    WCHAR wstr[32];
    const WCHAR *wterminator;
    NTSTATUS ret;

    RtlMultiByteToUnicodeN(wstr, sizeof(wstr), NULL, str, strlen(str) + 1);
    wstr[ARRAY_SIZE(wstr) - 1] = 0;
    ret = RtlIpv4StringToAddressW(wstr, strict, &wterminator, address);
    if (terminator) *terminator = str + (wterminator - wstr);
    return ret;
}

static BOOL parse_ipv6_component(const WCHAR **str, int base, ULONG *value)
{
    WCHAR *terminator;
    if (**str >= ARRAY_SIZE(hex_table) || hex_table[**str] == -1) return FALSE;
    *value = min(wcstoul(*str, &terminator, base), 0x7FFFFFFF);
    if (*terminator == '0') terminator++; /* "0x" but nothing valid after */
    else if (terminator == *str) return FALSE;
    *str = terminator;
    return TRUE;
}

static NTSTATUS ipv6_string_to_address(const WCHAR *str, BOOL ex,
                                       const WCHAR **terminator, IN6_ADDR *address, ULONG *scope, USHORT *port)
{
    BOOL expecting_port = FALSE, has_0x = FALSE, too_big = FALSE;
    int n_bytes = 0, n_ipv4_bytes = 0, gap = -1;
    ULONG ip_component, scope_component = 0, port_component = 0;
    const WCHAR *prev_str;

    if (str[0] == '[')
    {
        if (!ex) goto error;
        expecting_port = TRUE;
        str++;
    }

    if (str[0] == ':')
    {
        if (str[1] != ':') goto error;
        str++;
        address->u.Word[0] = 0;
    }

    for (;;)
    {
        if (!n_ipv4_bytes && *str == ':')
        {
            /* double colon */
            if (gap != -1) goto error;
            str++;
            prev_str = str;
            gap = n_bytes;
            if (n_bytes == 14 || !parse_ipv6_component(&str, 16, &ip_component)) break;
            str = prev_str;
        }
        else
        {
            prev_str = str;
        }

        if (!n_ipv4_bytes && n_bytes <= (gap != -1 ? 10 : 12))
        {
            if (parse_ipv6_component(&str, 10, &ip_component) && *str == '.')
                n_ipv4_bytes = 1;
            str = prev_str;
        }

        if (n_ipv4_bytes)
        {
            /* IPv4 component */
            if (!parse_ipv6_component(&str, 10, &ip_component)) goto error;
            if (str - prev_str > 3 || ip_component > 255)
            {
                too_big = TRUE;
            }
            else
            {
                if (*str != '.' && (n_ipv4_bytes < 4 || (n_bytes < 15 && gap == -1))) goto error;
                address->u.Byte[n_bytes] = ip_component;
                n_bytes++;
            }
            if (n_ipv4_bytes == 4 || *str != '.') break;
            n_ipv4_bytes++;
        }
        else
        {
            /* IPv6 component */
            if (!parse_ipv6_component(&str, 16, &ip_component)) goto error;
            if (prev_str[0] == '0' && (prev_str[1] == 'x' || prev_str[1] == 'X'))
            {
                /* Windows "feature": the last IPv6 component can start with "0x" and be longer than 4 digits */
                if (terminator) *terminator = prev_str + 1; /* Windows says that the "x" is the terminator */
                if (n_bytes < 14 && gap == -1) return STATUS_INVALID_PARAMETER;
                address->u.Word[n_bytes/2] = htons(ip_component);
                n_bytes += 2;
                has_0x = TRUE;
                goto fill_gap;
            }
            if (*str != ':' && n_bytes < 14 && gap == -1) goto error;
            if (str - prev_str > 4)
                too_big = TRUE;
            else
                address->u.Word[n_bytes/2] = htons(ip_component);
            n_bytes += 2;
            if (*str != ':' || (gap != -1 && str[1] == ':')) break;
        }
        if (n_bytes == (gap != -1 ? 14 : 16)) break;
        if (too_big) return STATUS_INVALID_PARAMETER;
        str++;
    }

    if (terminator) *terminator = str;
    if (too_big) return STATUS_INVALID_PARAMETER;

fill_gap:
    if (gap == -1)
    {
        if (n_bytes < 16) goto error;
    }
    else
    {
        memmove(address->u.Byte + 16 - (n_bytes - gap), address->u.Byte + gap, n_bytes - gap);
        memset(address->u.Byte + gap, 0, 16 - n_bytes);
    }

    if (ex)
    {
        if (has_0x) goto error;

        if (*str == '%')
        {
            str++;
            if (!parse_ipv4_component(&str, TRUE, &scope_component)) goto error;
        }

        if (expecting_port)
        {
            if (*str != ']') goto error;
            str++;
            if (*str == ':')
            {
                str++;
                if (!parse_ipv4_component(&str, FALSE, &port_component)) goto error;
                if (!port_component || port_component > 0xFFFF || *str) goto error;
                port_component = htons(port_component);
            }
        }
    }

    if (!terminator && *str) return STATUS_INVALID_PARAMETER;

    if (scope) *scope = scope_component;
    if (port) *port = port_component;

    return STATUS_SUCCESS;

error:
    if (terminator) *terminator = str;
    return STATUS_INVALID_PARAMETER;
}

/***********************************************************************
 * RtlIpv6StringToAddressExW [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv6StringToAddressExW(const WCHAR *str, IN6_ADDR *address, ULONG *scope, USHORT *port)
{
    if (!str || !address || !scope || !port) return STATUS_INVALID_PARAMETER;
    return ipv6_string_to_address(str, TRUE, NULL, address, scope, port);
}

/***********************************************************************
 * RtlIpv6StringToAddressW [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv6StringToAddressW(const WCHAR *str, const WCHAR **terminator, IN6_ADDR *address)
{
    return ipv6_string_to_address(str, FALSE, terminator, address, NULL, NULL);
}

/***********************************************************************
 * RtlIpv6StringToAddressExA [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv6StringToAddressExA(const char *str, IN6_ADDR *address, ULONG *scope, USHORT *port)
{
    WCHAR wstr[128];

    if (!str) return STATUS_INVALID_PARAMETER;
    RtlMultiByteToUnicodeN(wstr, sizeof(wstr), NULL, str, strlen(str) + 1);
    wstr[ARRAY_SIZE(wstr) - 1] = 0;
    return RtlIpv6StringToAddressExW(wstr, address, scope, port);
}

/***********************************************************************
 * RtlIpv6StringToAddressA [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv6StringToAddressA(const char *str, const char **terminator, IN6_ADDR *address)
{
    WCHAR wstr[128];
    const WCHAR *wterminator = NULL;
    NTSTATUS ret;

    RtlMultiByteToUnicodeN(wstr, sizeof(wstr), NULL, str, strlen(str) + 1);
    wstr[ARRAY_SIZE(wstr) - 1] = 0;
    ret = RtlIpv6StringToAddressW(wstr, &wterminator, address);
    if (terminator && wterminator) *terminator = str + (wterminator - wstr);
    return ret;
}

/***********************************************************************
 * RtlIpv4AddressToStringExW [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv4AddressToStringExW(const IN_ADDR *pin, USHORT port, LPWSTR buffer, PULONG psize)
{
    WCHAR tmp_ip[32];
    ULONG needed;

    if (!pin || !buffer || !psize)
        return STATUS_INVALID_PARAMETER;

    needed = swprintf(tmp_ip, ARRAY_SIZE(tmp_ip), L"%u.%u.%u.%u",
                      pin->S_un.S_un_b.s_b1, pin->S_un.S_un_b.s_b2,
                      pin->S_un.S_un_b.s_b3, pin->S_un.S_un_b.s_b4);

    if (port) needed += swprintf(tmp_ip + needed, ARRAY_SIZE(tmp_ip) - needed, L":%u", ntohs(port));

    if (*psize > needed) {
        *psize = needed + 1;
        wcscpy(buffer, tmp_ip);
        return STATUS_SUCCESS;
    }

    *psize = needed + 1;
    return STATUS_INVALID_PARAMETER;
}

/***********************************************************************
 * RtlIpv4AddressToStringExA [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv4AddressToStringExA(const IN_ADDR *pin, USHORT port, LPSTR buffer, PULONG psize)
{
    CHAR tmp_ip[32];
    ULONG needed;

    if (!pin || !buffer || !psize)
        return STATUS_INVALID_PARAMETER;

    needed = sprintf(tmp_ip, "%u.%u.%u.%u",
                     pin->S_un.S_un_b.s_b1, pin->S_un.S_un_b.s_b2,
                     pin->S_un.S_un_b.s_b3, pin->S_un.S_un_b.s_b4);

    if (port) needed += sprintf(tmp_ip + needed, ":%u", ntohs(port));

    if (*psize > needed) {
        *psize = needed + 1;
        strcpy(buffer, tmp_ip);
        return STATUS_SUCCESS;
    }

    *psize = needed + 1;
    return STATUS_INVALID_PARAMETER;
}

/***********************************************************************
 * RtlIpv4AddressToStringW [NTDLL.@]
 */
EXPORT WCHAR * NTAPI RtlIpv4AddressToStringW(const IN_ADDR *pin, LPWSTR buffer)
{
    ULONG size = 16;

    if (RtlIpv4AddressToStringExW(pin, 0, buffer, &size)) size = 0;
    return buffer + size - 1;
}

/***********************************************************************
 * RtlIpv4AddressToStringA [NTDLL.@]
 */
EXPORT CHAR * NTAPI RtlIpv4AddressToStringA(const IN_ADDR *pin, LPSTR buffer)
{
    ULONG size = 16;

    if (RtlIpv4AddressToStringExA(pin, 0, buffer, &size)) size = 0;
    return buffer + size - 1;
}

static BOOL is_ipv4_in_ipv6(const IN6_ADDR *address)
{
    if (address->s6_words[5] == htons(0x5efe) && (address->s6_words[4] & ~htons(0x200)) == 0)
        return TRUE;
    if (*(UINT64 *)address != 0)
        return FALSE;
    if (address->s6_words[4] != 0 && address->s6_words[4] != 0xffff)
        return FALSE;
    if (address->s6_words[4] == 0 && address->s6_words[5] != 0 && address->s6_words[5] != 0xffff)
        return FALSE;
    if (address->s6_words[4] == 0xffff && address->s6_words[5] != 0)
        return FALSE;
    if (address->s6_words[6] == 0)
        return FALSE;
    return TRUE;
}

/***********************************************************************
 * RtlIpv6AddressToStringExA [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv6AddressToStringExA(const IN6_ADDR *address, ULONG scope, USHORT port,
                                          char *str, ULONG *size)
{
    char buffer[64], *p = buffer;
    int i, len, gap = -1, gap_len = 1, ipv6_end = 8;
    ULONG needed;
    NTSTATUS ret;

    if (!address || !str || !size)
        return STATUS_INVALID_PARAMETER;

    if (is_ipv4_in_ipv6(address))
        ipv6_end = 6;

    for (i = 0; i < ipv6_end; i++)
    {
        len = 0;
        while (i < ipv6_end && !address->s6_words[i])
        {
            i++;
            len++;
        }
        if (len > gap_len)
        {
            gap = i - len;
            gap_len = len;
        }
    }

    if (port) p += sprintf(p, "[");

    i = 0;
    while (i < ipv6_end)
    {
        if (i == gap)
        {
            p += sprintf(p, ":");
            i += gap_len;
            if (i == ipv6_end) p += sprintf(p, ":");
            continue;
        }
        if (i > 0) p += sprintf(p, ":");
        p += sprintf(p, "%x", ntohs(address->s6_words[i]));
        i++;
    }

    if (ipv6_end == 6)
    {
        if (p[-1] != ':') p += sprintf(p, ":");
        p = RtlIpv4AddressToStringA((IN_ADDR *)(address->s6_words + 6), p);
    }

    if (scope) p += sprintf(p, "%%%lu", scope);

    if (port) p += sprintf(p, "]:%u", ntohs(port));

    needed = p - buffer + 1;

    if (*size >= needed)
    {
        strcpy(str, buffer);
        ret = STATUS_SUCCESS;
    }
    else
    {
        ret = STATUS_INVALID_PARAMETER;
    }

    *size = needed;
    return ret;
}

/***********************************************************************
 * RtlIpv6AddressToStringA [NTDLL.@]
 */
EXPORT char * NTAPI RtlIpv6AddressToStringA(const IN6_ADDR *address, char *str)
{
    ULONG size = 46;
    if (!address || !str) return str - 1;
    str[45] = 0; /* this byte is set even though the string is always shorter */
    RtlIpv6AddressToStringExA(address, 0, 0, str, &size);
    return str + size - 1;
}

/***********************************************************************
 * RtlIpv6AddressToStringExW [NTDLL.@]
 */
EXPORT NTSTATUS NTAPI RtlIpv6AddressToStringExW(const IN6_ADDR *address, ULONG scope, USHORT port,
                                          WCHAR *str, ULONG *size)
{
    char cstr[64];
    NTSTATUS ret = RtlIpv6AddressToStringExA(address, scope, port, cstr, size);
    if (ret == STATUS_SUCCESS) RtlMultiByteToUnicodeN(str, *size * sizeof(WCHAR), NULL, cstr, *size);
    return ret;
}

/***********************************************************************
 * RtlIpv6AddressToStringW [NTDLL.@]
 */
EXPORT WCHAR * NTAPI RtlIpv6AddressToStringW(const IN6_ADDR *address, WCHAR *str)
{
    ULONG size = 46;
    if (!address || !str) return str;
    str[45] = 0; /* this word is set even though the string is always shorter */
    if (RtlIpv6AddressToStringExW(address, 0, 0, str, &size) != STATUS_SUCCESS)
        return str;
    return str + size - 1;
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
  if (len > value_len) { *needed = len + 1; return STATUS_BUFFER_TOO_SMALL_; }
  memcpy(value, v, len * 2);
  if (len < value_len) value[len] = 0;
  return STATUS_SUCCESS;
}
/* What is written when the buffer is too small follows Windows: plain text goes in while there is room for it and one more character (and no end mark follows if it is cut); a
 * variable goes in whole with its end mark or not at all, and when it does not fit the end mark is written where it would have begun; nothing is written after that. */
EXPORT NTSTATUS NTAPI RtlExpandEnvironmentStrings(PWSTR env, PWSTR src, SIZE_T src_len, PWSTR dst, SIZE_T dst_len, SIZE_T *out_len) {
  if (!env) env = (PWSTR)process_environment();
  SIZE_T needed = 0, written = 0;
  int stopped = 0;
  SIZE_T i = 0;
  while (i < src_len) {
    SIZE_T j = i + 1;
    if (src[i] == '%') {
      while (j < src_len && src[j] != '%') ++j;
      if (j < src_len && j > i + 1) {
        SIZE_T vlen;
        const WCHAR *v = env_find(env, src + i + 1, j - i - 1, &vlen);
        if (v) {
          if (!stopped && dst) {
            if (written + vlen < dst_len) { memcpy(dst + written, v, vlen * 2); written += vlen; dst[written] = 0; }
            else { if (written < dst_len) dst[written] = 0; stopped = 1; }
          }
          needed += vlen;
          i = j + 1;
          continue;
        }
      }
    }
    if (!stopped && dst) { if (written + 1 < dst_len) dst[written++] = src[i]; else stopped = 1; }
    ++needed;
    ++i;
  }
  if (!stopped && dst && written < dst_len) dst[written] = 0;
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


/* ---- process parameters and the debug buffer -------------------------------------------------------------------------------------------------------------- */

#define PARAMS_HEADER 0x410
#define PARAMS_FLAG_NORMALIZED 1
/* The offsets of the strings in the parameters: current directory, DLL path, image, command line, window title, desktop, shell info, runtime info. */
static const ULONG g_params_strings[8] = {0x38, 0x50, 0x60, 0x70, 0xB0, 0xC0, 0xD0, 0xE0};
static ULONG_PTR params_align(ULONG_PTR v, ULONG to) { return (v + to - 1) & ~(ULONG_PTR)(to - 1); }

/* RtlCreateProcessParameters: one heap block with the header, the strings (kept as offsets from the block: not normalized) and then the environment. */
EXPORT NTSTATUS NTAPI RtlCreateProcessParametersEx(PVOID *result, const UNICODE_STRING *image, const UNICODE_STRING *dll_path, const UNICODE_STRING *current_directory, const UNICODE_STRING *command_line,
                                                   PWSTR environment, const UNICODE_STRING *title, const UNICODE_STRING *desktop, const UNICODE_STRING *shell, const UNICODE_STRING *runtime, ULONG flags) {
  static const UNICODE_STRING empty = {0, 2, NULL}, none = {0, 0, NULL};
  RTL_USER_PROCESS_PARAMETERS *current = NtCurrentPeb()->ProcessParameters;
  if (!result || !image) return STATUS_INVALID_PARAMETER_;
  const UNICODE_STRING *sources[8];
  UNICODE_STRING own_dir, own_dll;
  if (current_directory) sources[0] = current_directory; else if (current) { own_dir = current->CurrentDirectoryPath; sources[0] = &own_dir; } else sources[0] = &none;
  if (dll_path) sources[1] = dll_path; else if (current && current->DllPath.Buffer && current->DllPath.MaximumLength) { own_dll = current->DllPath; sources[1] = &own_dll; } else sources[1] = &none;
  sources[2] = image;
  sources[3] = command_line ? command_line : image;
  sources[4] = title ? title : &empty;
  sources[5] = desktop ? desktop : &empty;
  sources[6] = shell ? shell : &empty;
  sources[7] = runtime ? runtime : &none;
  ULONG_PTR pos = PARAMS_HEADER;
  USHORT maximum[8];
  for (int i = 0; i < 8; ++i) {
    USHORT m = sources[i]->MaximumLength;
    if (i == 0 && m < 520) m = 520;
    if (m != 0 && m < sources[i]->Length + 2) m = (USHORT)(sources[i]->Length + 2);
    maximum[i] = m;
    if (m) pos = params_align(pos, 8) + m;
  }
  const ULONG size = (ULONG)params_align(pos, 8);
  const PWSTR env = environment ? environment : (PWSTR)process_environment();
  SIZE_T env_chars = 0;
  { const WCHAR *e = env; while (*e) e += wcslen(e) + 1; env_chars = (SIZE_T)(e + 1 - env); }
  const SIZE_T total = (SIZE_T)params_align(size + env_chars * 2, 8);
  BYTE *block = RtlAllocateHeap(RtlProcessHeap(), 8, total);
  if (!block) return STATUS_NO_MEMORY_;
  *(ULONG *)(block + 0) = size;
  *(ULONG *)(block + 4) = size;
  *(ULONG *)(block + 8) = 0;
  pos = PARAMS_HEADER;
  for (int i = 0; i < 8; ++i) {
    UNICODE_STRING *dst = (UNICODE_STRING *)(block + g_params_strings[i]);
    dst->Length = sources[i]->Length;
    dst->MaximumLength = maximum[i];
    if (!maximum[i]) { dst->Buffer = NULL; continue; }
    pos = params_align(pos, 8);
    dst->Buffer = (PWSTR)pos;      /* (an offset: the parameters are not normalized) */
    if (sources[i]->Buffer) memcpy(block + pos, sources[i]->Buffer, sources[i]->Length);
    pos += maximum[i];
  }
  *(PVOID *)(block + 0x80) = block + size;
  memcpy(block + size, env, env_chars * 2);
  *(SIZE_T *)(block + 0x3F0) = total - size;
  (void)flags;
  *result = block;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlCreateProcessParameters(PVOID *result, const UNICODE_STRING *image, const UNICODE_STRING *dll_path, const UNICODE_STRING *current_directory, const UNICODE_STRING *command_line,
                                                 PWSTR environment, const UNICODE_STRING *title, const UNICODE_STRING *desktop, const UNICODE_STRING *shell, const UNICODE_STRING *runtime) {
  return RtlCreateProcessParametersEx(result, image, dll_path, current_directory, command_line, environment, title, desktop, shell, runtime, 0);
}
EXPORT NTSTATUS NTAPI RtlDestroyProcessParameters(PVOID params) { return RtlFreeHeap(RtlProcessHeap(), 0, params) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER_; }
/* The strings of normalized parameters hold addresses, the others offsets from the parameters. */
EXPORT PVOID NTAPI RtlNormalizeProcessParams(PVOID params) {
  BYTE *p = params;
  if (!p || (*(ULONG *)(p + 8) & PARAMS_FLAG_NORMALIZED)) return params;
  for (int i = 0; i < 8; ++i) { UNICODE_STRING *s = (UNICODE_STRING *)(p + g_params_strings[i]); if (s->Buffer) s->Buffer = (PWSTR)((BYTE *)s->Buffer + (ULONG_PTR)p); }
  *(ULONG *)(p + 8) |= PARAMS_FLAG_NORMALIZED;
  return params;
}
EXPORT PVOID NTAPI RtlDeNormalizeProcessParams(PVOID params) {
  BYTE *p = params;
  if (!p || !(*(ULONG *)(p + 8) & PARAMS_FLAG_NORMALIZED)) return params;
  for (int i = 0; i < 8; ++i) { UNICODE_STRING *s = (UNICODE_STRING *)(p + g_params_strings[i]); if (s->Buffer) s->Buffer = (PWSTR)((BYTE *)s->Buffer - (ULONG_PTR)p); }
  *(ULONG *)(p + 8) &= ~(ULONG)PARAMS_FLAG_NORMALIZED;
  return params;
}

/* The debug buffer of RtlQueryProcessDebugInformation: this process only; the heap and module lists are headers with a count. */
EXPORT PVOID NTAPI RtlCreateQueryDebugBuffer(ULONG size, BOOLEAN event_pair) {
  (void)size; (void)event_pair;
  return RtlAllocateHeap(RtlProcessHeap(), 8, 0xB8);
}
EXPORT NTSTATUS NTAPI RtlQueryProcessDebugInformation(ULONG pid, ULONG mask, PVOID buffer) {
  BYTE *b = buffer;
  if (!b) return STATUS_INVALID_PARAMETER_;
  if (pid != (ULONG)TEB_CLIENT_PROCESS(NtCurrentTeb())) return (NTSTATUS)0xC000000Bu;      /* STATUS_INVALID_CID */
  *(ULONG *)(b + 0x40) = mask;
  if (mask & 1) { if (!*(PVOID *)(b + 0x60)) *(PVOID *)(b + 0x60) = RtlAllocateHeap(RtlProcessHeap(), 8, 0x100); if (!*(PVOID *)(b + 0x60)) return STATUS_NO_MEMORY_; }
  if (mask & 4) {
    if (!*(PVOID *)(b + 0x70)) *(PVOID *)(b + 0x70) = RtlAllocateHeap(RtlProcessHeap(), 8, 0x100);
    if (!*(PVOID *)(b + 0x70)) return STATUS_NO_MEMORY_;
    PVOID heaps[MAX_HEAPS];
    *(ULONG *)*(PVOID *)(b + 0x70) = RtlGetProcessHeaps(MAX_HEAPS, heaps);
  }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDestroyQueryDebugBuffer(PVOID buffer) {
  BYTE *b = buffer;
  if (!b) return STATUS_INVALID_PARAMETER_;
  for (int off = 0x60; off <= 0x78; off += 8) if (*(PVOID *)(b + off)) RtlFreeHeap(RtlProcessHeap(), 0, *(PVOID *)(b + off));
  RtlFreeHeap(RtlProcessHeap(), 0, b);
  return STATUS_SUCCESS;
}

/* ---- environment blocks of the heap ------------------------------------------------------------------------------------------------------------------------ */

/* The characters of an environment block, with the final empty string. */
static SIZE_T env_chars(const WCHAR *env) { const WCHAR *e = env; while (*e) e += wcslen(e) + 1; return (SIZE_T)(e + 1 - env); }
EXPORT NTSTATUS NTAPI RtlCreateEnvironment(BOOLEAN inherit, PWSTR *environment) {
  if (!environment) return STATUS_INVALID_PARAMETER_;
  const WCHAR *source = inherit ? process_environment() : NULL;
  const SIZE_T n = source ? env_chars(source) : 2;
  WCHAR *block = RtlAllocateHeap(RtlProcessHeap(), 8, n * 2);
  if (!block) return STATUS_NO_MEMORY_;
  if (source) memcpy(block, source, n * 2);
  *environment = block;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDestroyEnvironment(PWSTR environment) { RtlFreeHeap(RtlProcessHeap(), 0, environment); return STATUS_SUCCESS; }
/* Sets (or, with no value, removes) a variable in a block of the heap (NULL: the process's own, whose size is kept in its parameters). The block is made again with exactly the size it needs. */
EXPORT NTSTATUS NTAPI RtlSetEnvironmentVar(PWSTR *penv, PCWSTR name, SIZE_T name_len, PCWSTR value, SIZE_T value_len) {
  RTL_USER_PROCESS_PARAMETERS *params = NtCurrentPeb()->ProcessParameters;
  if (!name || !name_len) return STATUS_INVALID_PARAMETER_;
  for (SIZE_T i = 0; i < name_len; ++i) if (name[i] == '=' && i > 0) return STATUS_INVALID_PARAMETER_;
  WCHAR *old = penv ? *penv : (WCHAR *)params->Environment;
  const SIZE_T old_chars = env_chars(old);
  SIZE_T found_len = 0;
  const WCHAR *found = env_find(old, name, name_len, &found_len);
  const SIZE_T removed = found ? name_len + 1 + found_len + 1 : 0, added = value ? name_len + 1 + value_len + 1 : 0;
  WCHAR *fresh = RtlAllocateHeap(RtlProcessHeap(), 0, (old_chars - removed + added) * 2);
  if (!fresh) return STATUS_NO_MEMORY_;
  SIZE_T out = 0;
  for (const WCHAR *e = old; *e; e += wcslen(e) + 1) {
    if (found && e + name_len + 1 == found) continue;      /* (the entry being replaced or removed) */
    const SIZE_T n = wcslen(e) + 1;
    memcpy(fresh + out, e, n * 2);
    out += n;
  }
  if (value) {
    memcpy(fresh + out, name, name_len * 2); out += name_len;
    fresh[out++] = '=';
    memcpy(fresh + out, value, value_len * 2); out += value_len;
    fresh[out++] = 0;
  }
  fresh[out++] = 0;
  if (out == 1) fresh[out++] = 0;
  const SIZE_T capacity = penv ? RtlSizeHeap(RtlProcessHeap(), 0, old) : *(SIZE_T *)((BYTE *)params + 0x3F0);
  if (out * 2 <= capacity) {      /* it fits where it is: the block keeps its size */
    memcpy(old, fresh, out * 2);
    RtlFreeHeap(RtlProcessHeap(), 0, fresh);
    return STATUS_SUCCESS;
  }
  if (penv) *penv = fresh; else { params->Environment = fresh; *(SIZE_T *)((BYTE *)params + 0x3F0) = out * 2; }
  RtlFreeHeap(RtlProcessHeap(), 0, old);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlSetEnvironmentVariable(PWSTR *penv, const UNICODE_STRING *name, const UNICODE_STRING *value) {
  if (!name) return STATUS_INVALID_PARAMETER_;
  return RtlSetEnvironmentVar(penv, name->Buffer, name->Length / 2, value ? value->Buffer : NULL, value ? value->Length / 2 : 0);
}
EXPORT void NTAPI RtlSetCurrentEnvironment(PWSTR environment, PWSTR *previous) {
  RTL_USER_PROCESS_PARAMETERS *params = NtCurrentPeb()->ProcessParameters;
  if (previous) *previous = (PWSTR)params->Environment;
  params->Environment = environment;
  *(SIZE_T *)((BYTE *)params + 0x3F0) = RtlSizeHeap(RtlProcessHeap(), 0, environment);
}
/* The process's parameters and environment become blocks of the heap, as they are on Windows (the strings follow the header; the current directory stays where it is). */
static void relocate_process_parameters(void) {
  PEB *peb = NtCurrentPeb();
  BYTE *old = (BYTE *)peb->ProcessParameters;
  if (!old) return;
  const ULONG index[8] = {0x38, 0x50, 0x60, 0x70, 0xB0, 0xC0, 0xD0, 0xE0};
  ULONG size = PARAMS_HEADER;
  for (int i = 1; i < 8; ++i) { const UNICODE_STRING *u = (const UNICODE_STRING *)(old + index[i]); if (u->Buffer) size += u->MaximumLength; }
  BYTE *fresh = RtlAllocateHeap(RtlProcessHeap(), 8, size);
  const SIZE_T chars = *(PVOID *)(old + 0x80) ? env_chars(*(const WCHAR **)(old + 0x80)) : 2;
  WCHAR *env = RtlAllocateHeap(RtlProcessHeap(), 8, chars * 2);
  if (!fresh || !env) return;
  memcpy(fresh, old, 0x3F0);
  if (*(PVOID *)(old + 0x80)) memcpy(env, *(const WCHAR **)(old + 0x80), chars * 2);
  *(PVOID *)(fresh + 0x80) = env;
  *(SIZE_T *)(fresh + 0x3F0) = chars * 2;
  *(ULONG *)fresh = *(ULONG *)(fresh + 4) = size;
  ULONG pos = PARAMS_HEADER;
  for (int i = 1; i < 8; ++i) {
    UNICODE_STRING *u = (UNICODE_STRING *)(fresh + index[i]);
    if (!u->Buffer) continue;
    memcpy(fresh + pos, u->Buffer, u->MaximumLength);
    u->Buffer = (PWSTR)(fresh + pos);
    pos += u->MaximumLength;
  }
  peb->ProcessParameters = (RTL_USER_PROCESS_PARAMETERS *)fresh;
}

/* ---- one character of an ANSI string ------------------------------------------------------------------------------------------------------------------- */

/* The character the ANSI string points at (the code page is UTF-8), and the pointer moved past it; a byte that starts no valid sequence gives U+FFFD. */
EXPORT WCHAR NTAPI RtlAnsiCharToUnicodeChar(PUCHAR *ansi) {
  const UCHAR *p = *ansi;
  const UCHAR lead = p[0];
  unsigned count = 1;
  ULONG cp = 0xFFFD;
  if (lead < 0x80) cp = lead;
  else if (lead >= 0xC2 && lead <= 0xF4) {
    const unsigned need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : 1;
    ULONG v = lead & (0x3Fu >> need);
    unsigned k = 0;
    for (; k < need && (p[1 + k] & 0xC0) == 0x80; ++k) v = (v << 6) | (p[1 + k] & 0x3F);
    const int ok = k == need && !(need == 2 && v < 0x800) && !(need == 3 && (v < 0x10000 || v > 0x10FFFF)) && !(v >= 0xD800 && v <= 0xDFFF);
    if (ok) { count = need + 1; cp = v >= 0x10000 ? 0xD800 + ((v - 0x10000) >> 10) : v; }
  }
  if (lead) *ansi = (PUCHAR)(p + count); else *ansi = (PUCHAR)(p + 1);
  return (WCHAR)cp;
}

/* ---- RtlIsTextUnicode ---------------------------------------------------------------------------------------------------------------------------------- */

#define TEXT_ASCII16 0x1u
#define TEXT_STATISTICS 0x2u
#define TEXT_CONTROLS 0x4u
#define TEXT_SIGNATURE 0x8u
#define TEXT_REVERSE_ASCII16 0x10u
#define TEXT_REVERSE_STATISTICS 0x20u
#define TEXT_REVERSE_CONTROLS 0x40u
#define TEXT_REVERSE_SIGNATURE 0x80u
#define TEXT_ILLEGAL_CHARS 0x100u
#define TEXT_ODD_LENGTH 0x200u
#define TEXT_NULL_BYTES 0x1000u
#define TEXT_UNICODE_MASK 0xFu
#define TEXT_REVERSE_MASK 0xF0u
#define TEXT_NOT_UNICODE_MASK 0xF00u
#define TEXT_NOT_ASCII_MASK 0xF000u
/* Whether a buffer looks like UTF-16 text; *flags says which tests to run and gets the ones that came out true. Only the first 256 characters count. */
EXPORT BOOLEAN NTAPI RtlIsTextUnicode(PVOID buffer, ULONG size, PULONG flags_in) {
  const WCHAR *s = buffer;
  unsigned flags = ~0u, out = 0;
  ULONG length = size;
  if (length < sizeof(WCHAR)) { if (flags_in) *flags_in = 0; return FALSE; }
  if (flags_in) flags = *flags_in;
  if (length & 1) out |= TEXT_ODD_LENGTH;
  if (((const char *)buffer)[length - 1] == 0) --length;      /* (a terminating NUL byte is not counted) */
  ULONG count = length / sizeof(WCHAR);
  if (count > 256) count = 256;
  if (s[0] == 0xFEFF) out |= TEXT_SIGNATURE;
  if (s[0] == 0xFFFE) out |= TEXT_REVERSE_SIGNATURE;
  if (!(out & (TEXT_SIGNATURE | TEXT_REVERSE_SIGNATURE))) {      /* (statistics are only taken when there is no byte order mark) */
    ULONG low = 0, high = 0;
    for (ULONG i = 0; i < count; ++i) { if (s[i] < 0x2000) ++low; if ((s[i] & 0xFF) < 0x20) ++high; }
    if ((flags & TEXT_STATISTICS) && low > count / 2) out |= TEXT_STATISTICS;
    if ((flags & TEXT_REVERSE_STATISTICS) && high > count / 2) out |= TEXT_REVERSE_STATISTICS;
    if ((flags & TEXT_REVERSE_ASCII16) && high == count) out |= TEXT_REVERSE_ASCII16;
  }
  if (flags & TEXT_NULL_BYTES) {
    for (ULONG i = 0; i < count; ++i) if (!(s[i] & 0xFF) || !(s[i] >> 8)) { out |= TEXT_NULL_BYTES; break; }
  }
  if (flags & TEXT_CONTROLS) {
    for (ULONG i = 0; i < count; ++i) if (s[i] == '\r' || s[i] == '\n' || s[i] == '\t' || s[i] == ' ' || s[i] == 0x3000) { out |= TEXT_CONTROLS; break; }
  }
  if (flags & TEXT_REVERSE_CONTROLS) {
    for (ULONG i = 0; i < count; ++i) if (s[i] == 0x0D00 || s[i] == 0x0A00 || s[i] == 0x0900 || s[i] == 0x2000) { out |= TEXT_REVERSE_CONTROLS; break; }
  }
  if (flags_in) { out &= *flags_in; *flags_in = out; }
  if (out & (TEXT_REVERSE_MASK | TEXT_NOT_UNICODE_MASK)) return FALSE;
  if (out & TEXT_NOT_ASCII_MASK) return TRUE;
  return (out & TEXT_UNICODE_MASK) != 0;
}

/* ---- RtlFormatMessage ---------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  int last;               /* the last argument used */
  ULONG_PTR *array;       /* the argument array */
  va_list_ *list;         /* the variable argument list */
  ULONGLONG arglist[102]; /* what has been fetched from the list */
} FormatMessageArgs;

static NTSTATUS fmt_add_chars(WCHAR **buffer, WCHAR *end, const WCHAR *str, ULONG len) {
  if (len > (ULONG)(end - *buffer)) return STATUS_BUFFER_OVERFLOW_;
  memcpy(*buffer, str, (size_t_)len * sizeof(WCHAR));
  *buffer += len;
  return STATUS_SUCCESS;
}

static ULONGLONG fmt_get_arg(int nr, FormatMessageArgs *args_data, BOOL is64) {
  if (nr == -1) nr = args_data->last + 1;
  if (nr < 1 || nr > 100) return 0;
  while (nr > args_data->last) {
    args_data->arglist[args_data->last++] = is64 ? __builtin_va_arg(*args_data->list, ULONGLONG) : __builtin_va_arg(*args_data->list, ULONG_PTR);
  }
  return args_data->arglist[nr - 1];
}

static const WCHAR *fmt_find(const WCHAR *set_or_text, WCHAR c, int in_set) {
  (void)in_set;
  for (; *set_or_text; ++set_or_text) if (*set_or_text == c) return set_or_text;
  return NULL;
}

static NTSTATUS fmt_add_format(WCHAR **buffer, WCHAR *end, const WCHAR **src, int insert, BOOLEAN ansi, FormatMessageArgs *args_data) {
  static const WCHAR flag_chars[] = {'0','1','2','3','4','5','6','7','8','9',' ','+','-','*','#','.',0};
  const WCHAR *format = *src;
  WCHAR *p, fmt[32];
  ULONG_PTR args[5] = {0};
  BOOL is_64 = FALSE;
  ULONGLONG val;
  int len, stars = 0, nb_args = 0;

  p = fmt;
  *p++ = '%';
  if (*format++ == '!') {
    const WCHAR *stop = fmt_find(format, '!', 0);
    if (!stop || stop - format > 30) return STATUS_INVALID_PARAMETER_;
    *src = stop + 1;
    while (*format && fmt_find(flag_chars, *format, 1)) {
      if (*format == '*') stars++;
      *p++ = *format++;
    }
    if (stars > 2) return STATUS_INVALID_PARAMETER_;
    switch (*format) {
      case 'c': case 'C': case 's': case 'S':
        if (ansi) *p++ = (WCHAR)(*format++ ^ ('s' - 'S'));
        break;
      default: break;
    }
    while (format != stop) *p++ = *format++;
  } else *p++ = ansi ? 'S' : 's';      /* a simple string */
  *p = 0;

  if (args_data->list) {
    fmt_get_arg(insert - 1, args_data, is_64);      /* make sure the previous arguments have been fetched */
    while (stars--) {
      args[nb_args++] = (ULONG_PTR)fmt_get_arg(insert, args_data, FALSE);
      insert = -1;
    }
    if (insert == -1) args_data->last--;      /* (the quirk of Windows: an argument is dropped when a width or precision came from the list) */
    val = fmt_get_arg(insert, args_data, is_64);
    args[nb_args++] = (ULONG_PTR)val;
    args[nb_args] = (ULONG_PTR)(val >> 32);
  } else if (args_data->array) {
    args[nb_args++] = args_data->array[insert - 1];
    if (args_data->last < insert) args_data->last = insert;
    if (is_64) nb_args++;
    while (stars--) args[nb_args++] = args_data->array[args_data->last++];
  } else return STATUS_INVALID_PARAMETER_;

  len = _snwprintf_s(*buffer, (size_t_)(end - *buffer), (size_t_)(end - *buffer - 1), fmt, args[0], args[1], args[2], args[3], args[4]);
  if (len == -1) return STATUS_BUFFER_OVERFLOW_;
  *buffer += len;
  return STATUS_SUCCESS;
}

/* Formats the template `src` (the message of FormatMessage) into `buffer` of `size` bytes; the arguments are an array or the address of a variable argument list. */
EXPORT NTSTATUS NTAPI RtlFormatMessageEx(const WCHAR *src, ULONG width, BOOLEAN ignore_inserts, BOOLEAN ansi, BOOLEAN is_array, void *args, WCHAR *buffer, ULONG size, ULONG *retsize, ULONG flags) {
  static const WCHAR spaceW = ' ', crW = '\r', tabW = '\t';
  static const WCHAR crlfW[] = {'\r', '\n'};
  FormatMessageArgs *args_data = RtlAllocateHeap(process_heap(), 0, sizeof(*args_data));
  if (!args_data) return 0xC0000017;
  NTSTATUS status = STATUS_SUCCESS;
  WCHAR *start = buffer, *end = buffer + size / sizeof(WCHAR), *line = buffer, *space = NULL;
  (void)flags;
  args_data->last = 0;
  args_data->array = is_array ? (ULONG_PTR *)args : NULL;
  args_data->list = is_array ? NULL : (va_list_ *)args;
  for (; *src; src++) {
    switch (*src) {
      case '\r':
        if (src[1] == '\n') src++;
        /* fall through */
      case '\n':
        if (!width) {
          status = fmt_add_chars(&buffer, end, crlfW, 2);
          line = buffer;
          space = NULL;
          break;
        }
        /* fall through */
      case ' ':
        space = buffer;
        status = fmt_add_chars(&buffer, end, &spaceW, 1);
        break;
      case '\t':
        if (space == buffer - 1) space = buffer;
        status = fmt_add_chars(&buffer, end, &tabW, 1);
        break;
      case '%':
        src++;
        switch (*src) {
          case 0:
            status = STATUS_INVALID_PARAMETER_;
            goto finish;
          case 't':
            if (!width) { status = fmt_add_chars(&buffer, end, &tabW, 1); break; }
            /* fall through */
          case 'n':
            status = fmt_add_chars(&buffer, end, crlfW, 2);
            line = buffer;
            space = NULL;
            break;
          case 'r':
            status = fmt_add_chars(&buffer, end, &crW, 1);
            line = buffer;
            space = NULL;
            break;
          case '0':
            while (src[1]) src++;
            break;
          case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            if (!ignore_inserts) {
              int nr = *src++ - '0';
              if (*src >= '0' && *src <= '9') nr = nr * 10 + *src++ - '0';
              status = fmt_add_format(&buffer, end, &src, nr, ansi, args_data);
              src--;
              break;
            }
            /* fall through */
          default:
            if (ignore_inserts) status = fmt_add_chars(&buffer, end, src - 1, 2);
            else status = fmt_add_chars(&buffer, end, src, 1);
            break;
        }
        break;
      default:
        status = fmt_add_chars(&buffer, end, src, 1);
        break;
    }
    if (status) goto finish;
    if (width && (ULONG)(buffer - line) >= width) {
      LONG_PTR diff = 2;
      WCHAR *next;
      if (space) {      /* split the line at the last space */
        next = space + 1;
        while (space > line && (space[-1] == ' ' || space[-1] == '\t')) space--;
        diff -= next - space;
      } else space = next = buffer;      /* split at the end of the buffer */
      if (diff > 0 && end - buffer < diff) { status = STATUS_BUFFER_OVERFLOW_; goto finish; }
      memmove(space + 2, next, (size_t_)(buffer - next) * sizeof(WCHAR));
      buffer += diff;
      memcpy(space, crlfW, sizeof(crlfW));
      line = space + 2;
      space = NULL;
    }
  }
  status = fmt_add_chars(&buffer, end, L"", 1);
  if (!status) *retsize = (ULONG)((buffer - start) * sizeof(WCHAR));
finish:
  RtlFreeHeap(process_heap(), 0, args_data);
  return status;
}
EXPORT NTSTATUS NTAPI RtlFormatMessage(const WCHAR *src, ULONG width, BOOLEAN ignore_inserts, BOOLEAN ansi, BOOLEAN is_array, void *args, WCHAR *buffer, ULONG size, ULONG *retsize) {
  return RtlFormatMessageEx(src, width, ignore_inserts, ansi, is_array, args, buffer, size, retsize, 0);
}
