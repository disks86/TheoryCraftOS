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

/* pdh.dll for TheoryCraftOS: performance data queries. Queries and counters have real, checked handles; the counters that exist are the processor time ones (see
 * add_counter below), which is what Chromium's compute-pressure probe asks for. Other counter paths answer PDH_CSTATUS_NO_OBJECT. The rest is still stubs (src/Windows/stubs/pdh.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

typedef LONG PDH_STATUS_;
#define PDH_CSTATUS_NO_OBJECT_ ((PDH_STATUS_)0xC0000BB8)
#define PDH_INVALID_HANDLE_ ((PDH_STATUS_)0xC0000BBC)
#define PDH_INVALID_ARGUMENT_ ((PDH_STATUS_)0xC0000BBD)
#define PDH_NO_DATA_ ((PDH_STATUS_)0x800007D5)
#define PDH_MEMORY_ALLOCATION_FAILURE_ ((PDH_STATUS_)0xC0000BBB)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

/* The open queries: a handle is 0x5044'0000 + its slot, checked against the table (a handle the program made up is refused). */
#define MAX_QUERIES 256
#define QUERY_BASE 0x50440000u
static BYTE g_open[MAX_QUERIES];
static SRWLOCK g_lock = SRWLOCK_INIT;

static BOOL query_valid(HANDLE query) {
  const ULONG_PTR h = (ULONG_PTR)query;
  if (h < QUERY_BASE || h >= QUERY_BASE + MAX_QUERIES) return FALSE;
  AcquireSRWLockShared(&g_lock);
  const BOOL open = g_open[h - QUERY_BASE] != 0;
  ReleaseSRWLockShared(&g_lock);
  return open;
}

static void remove_counters_of_query_locked(WORD query);

static PDH_STATUS_ open_query(HANDLE *query) {
  if (!query) return PDH_INVALID_ARGUMENT_;
  *query = NULL;
  AcquireSRWLockExclusive(&g_lock);
  UINT slot = 0;
  while (slot < MAX_QUERIES && g_open[slot]) ++slot;
  if (slot < MAX_QUERIES) g_open[slot] = 1;
  ReleaseSRWLockExclusive(&g_lock);
  if (slot == MAX_QUERIES) return PDH_MEMORY_ALLOCATION_FAILURE_;
  *query = (HANDLE)(ULONG_PTR)(QUERY_BASE + slot);
  return ERROR_SUCCESS;
}

/* PdhOpenQuery(data source (NULL: the live counters; a log file is not supported), user data, query) */
EXPORT PDH_STATUS_ WINAPI PdhOpenQueryW(LPCWSTR source, DWORD_PTR user, HANDLE *query) {
  (void)user;
  if (query) *query = NULL;
  if (source && *source) return PDH_INVALID_ARGUMENT_;
  return open_query(query);
}
EXPORT PDH_STATUS_ WINAPI PdhOpenQueryA(LPCSTR source, DWORD_PTR user, HANDLE *query) {
  (void)user;
  if (query) *query = NULL;
  if (source && *source) return PDH_INVALID_ARGUMENT_;
  return open_query(query);
}
EXPORT PDH_STATUS_ WINAPI PdhOpenQuery(LPCWSTR source, DWORD_PTR user, HANDLE *query) { return PdhOpenQueryW(source, user, query); }

EXPORT PDH_STATUS_ WINAPI PdhCloseQuery(HANDLE query) {
  if (!query_valid(query)) return PDH_INVALID_HANDLE_;
  AcquireSRWLockExclusive(&g_lock);
  g_open[(ULONG_PTR)query - QUERY_BASE] = 0;
  remove_counters_of_query_locked((WORD)((ULONG_PTR)query - QUERY_BASE));
  ReleaseSRWLockExclusive(&g_lock);
  return ERROR_SUCCESS;
}

/* The counters. A handle is 0x5043'0000 + its slot. What exists:
 *   \Processor(_Total)\% Processor Time (also "Processor Information", and "% Processor Utility"): the busy share of all processors since the previous collection,
 *     from GetSystemTimes;
 *   \Hyper-V Hypervisor Logical Processor(_Total)\% Total Run Time: added, but it has no data (a virtual machine that is not Hyper-V; Chromium falls back to the
 *     processor counter when collecting it fails, as on Windows in such a machine).
 * Anything else names an object that is not there (PDH_CSTATUS_NO_OBJECT). */
#define PDH_INVALID_DATA_ ((PDH_STATUS_)0xC0000BC6)
#define PDH_CSTATUS_VALID_DATA_ 0u
#define PDH_CSTATUS_INVALID_DATA_ 0xC0000BBAu
#define PDH_CSTATUS_NO_INSTANCE_ 0x800007D1u
#define PDH_FMT_LONG_ 0x100u
#define PDH_FMT_DOUBLE_ 0x200u
#define PDH_FMT_LARGE_ 0x400u
#define PERF_100NSEC_TIMER_INV_ 0x21550500u
#define MAX_COUNTERS 1024
#define COUNTER_BASE 0x50430000u
enum { COUNTER_NONE, COUNTER_PROCESSOR_TIME, COUNTER_NO_DATA };
typedef struct {
  BYTE kind;
  WORD query;
  ULONGLONG idle[2], total[2];      /* [0] the collection before the last, [1] the last */
  BYTE samples;                     /* collections so far (stops at 2) */
} Counter;
static Counter g_counters[MAX_COUNTERS];

static WCHAR lower_char(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c - 'A' + 'a') : c; }

/* Does the path (UTF-16, or ANSI when `wide` is FALSE) equal `expected` (lower case ASCII), ignoring case? A leading "\\machine" naming this computer is not
 * supported: such a path names no object. */
static BOOL path_is(const void *path, BOOL wide, const char *expected) {
  for (SIZE_T i = 0;; ++i) {
    const WCHAR c = wide ? ((const WCHAR *)path)[i] : (WCHAR)(BYTE)((const char *)path)[i];
    if (lower_char(c) != (WCHAR)(BYTE)expected[i]) return FALSE;
    if (c == 0) return TRUE;
  }
}

static int counter_kind(const void *path, BOOL wide) {
  static const char *const processor[] = {
    "\\processor(_total)\\% processor time",
    "\\processor information(_total)\\% processor time",
    "\\processor information(_total)\\% processor utility",
  };
  for (UINT i = 0; i < sizeof(processor) / sizeof(processor[0]); ++i)
    if (path_is(path, wide, processor[i])) return COUNTER_PROCESSOR_TIME;
  if (path_is(path, wide, "\\hyper-v hypervisor logical processor(_total)\\% total run time")) return COUNTER_NO_DATA;
  return COUNTER_NONE;
}

/* (the lock must be held) */
static Counter *counter_at(HANDLE counter) {
  const ULONG_PTR h = (ULONG_PTR)counter;
  if (h < COUNTER_BASE || h >= COUNTER_BASE + MAX_COUNTERS) return NULL;
  Counter *c = &g_counters[h - COUNTER_BASE];
  return c->kind != COUNTER_NONE ? c : NULL;
}

static PDH_STATUS_ add_counter(HANDLE query, const void *path, BOOL wide, HANDLE *counter) {
  if (counter) *counter = NULL;
  if (!query_valid(query)) return PDH_INVALID_HANDLE_;
  if (!path || !counter) return PDH_INVALID_ARGUMENT_;
  const int kind = counter_kind(path, wide);
  if (kind == COUNTER_NONE) return PDH_CSTATUS_NO_OBJECT_;
  AcquireSRWLockExclusive(&g_lock);
  UINT slot = 0;
  while (slot < MAX_COUNTERS && g_counters[slot].kind != COUNTER_NONE) ++slot;
  if (slot < MAX_COUNTERS) {
    Counter *c = &g_counters[slot];
    ZeroMemory(c, sizeof(*c));
    c->kind = (BYTE)kind;
    c->query = (WORD)((ULONG_PTR)query - QUERY_BASE);
  }
  ReleaseSRWLockExclusive(&g_lock);
  if (slot == MAX_COUNTERS) return PDH_MEMORY_ALLOCATION_FAILURE_;
  *counter = (HANDLE)(ULONG_PTR)(COUNTER_BASE + slot);
  return ERROR_SUCCESS;
}
EXPORT PDH_STATUS_ WINAPI PdhAddEnglishCounterW(HANDLE query, LPCWSTR path, DWORD_PTR user, HANDLE *counter) { (void)user; return add_counter(query, path, TRUE, counter); }
EXPORT PDH_STATUS_ WINAPI PdhAddEnglishCounterA(HANDLE query, LPCSTR path, DWORD_PTR user, HANDLE *counter) { (void)user; return add_counter(query, path, FALSE, counter); }
EXPORT PDH_STATUS_ WINAPI PdhAddCounterW(HANDLE query, LPCWSTR path, DWORD_PTR user, HANDLE *counter) { (void)user; return add_counter(query, path, TRUE, counter); }
EXPORT PDH_STATUS_ WINAPI PdhAddCounterA(HANDLE query, LPCSTR path, DWORD_PTR user, HANDLE *counter) { (void)user; return add_counter(query, path, FALSE, counter); }

EXPORT PDH_STATUS_ WINAPI PdhRemoveCounter(HANDLE counter) {
  AcquireSRWLockExclusive(&g_lock);
  Counter *c = counter_at(counter);
  if (c) c->kind = COUNTER_NONE;
  ReleaseSRWLockExclusive(&g_lock);
  return c ? ERROR_SUCCESS : PDH_INVALID_HANDLE_;
}

/* A closed query takes its counters with it. (the lock must be held) */
static void remove_counters_of_query_locked(WORD query) {
  for (UINT i = 0; i < MAX_COUNTERS; ++i) if (g_counters[i].kind != COUNTER_NONE && g_counters[i].query == query) g_counters[i].kind = COUNTER_NONE;
}

static ULONGLONG file_time_value(FILETIME t) { return ((ULONGLONG)t.dwHighDateTime << 32) | t.dwLowDateTime; }

/* Takes a sample for every counter of the query. A query whose counters have no data (or that has none) answers PDH_NO_DATA. */
EXPORT PDH_STATUS_ WINAPI PdhCollectQueryData(HANDLE query) {
  if (!query_valid(query)) return PDH_INVALID_HANDLE_;
  FILETIME idle, kernel, user;
  const BOOL times = GetSystemTimes(&idle, &kernel, &user);
  const WORD slot = (WORD)((ULONG_PTR)query - QUERY_BASE);
  BOOL data = FALSE;
  AcquireSRWLockExclusive(&g_lock);
  for (UINT i = 0; i < MAX_COUNTERS; ++i) {
    Counter *c = &g_counters[i];
    if (c->kind != COUNTER_PROCESSOR_TIME || c->query != slot || !times) continue;
    c->idle[0] = c->idle[1];
    c->total[0] = c->total[1];
    c->idle[1] = file_time_value(idle);
    c->total[1] = file_time_value(kernel) + file_time_value(user);      /* (kernel time includes the idle time, as on Windows) */
    if (c->samples < 2) ++c->samples;
    data = TRUE;
  }
  ReleaseSRWLockExclusive(&g_lock);
  return data ? ERROR_SUCCESS : PDH_NO_DATA_;
}

/* PDH_FMT_COUNTERVALUE: the status and an 8-byte value (LONG, double or LONGLONG). */
typedef struct { DWORD status; DWORD padding; union { LONG long_value; double double_value; LONGLONG large_value; } value; } FormattedValue;
EXPORT PDH_STATUS_ WINAPI PdhGetFormattedCounterValue(HANDLE counter, DWORD format, LPDWORD type, FormattedValue *value) {
  if (!value) return PDH_INVALID_ARGUMENT_;
  ZeroMemory(value, sizeof(*value));
  if (type) *type = 0;
  const DWORD kinds = format & (PDH_FMT_LONG_ | PDH_FMT_DOUBLE_ | PDH_FMT_LARGE_);
  AcquireSRWLockShared(&g_lock);
  const Counter *c = counter_at(counter);
  Counter copy;
  if (c) copy = *c;
  ReleaseSRWLockShared(&g_lock);
  if (!c) { value->status = PDH_CSTATUS_INVALID_DATA_; return PDH_INVALID_HANDLE_; }
  if (kinds != PDH_FMT_LONG_ && kinds != PDH_FMT_DOUBLE_ && kinds != PDH_FMT_LARGE_) return PDH_INVALID_ARGUMENT_;
  if (type) *type = PERF_100NSEC_TIMER_INV_;
  /* A rate needs two collections; the counter of another hypervisor has none at all. */
  if (copy.kind != COUNTER_PROCESSOR_TIME || copy.samples < 2) {
    value->status = copy.kind == COUNTER_NO_DATA ? PDH_CSTATUS_NO_INSTANCE_ : PDH_CSTATUS_INVALID_DATA_;
    return PDH_INVALID_DATA_;
  }
  const ULONGLONG total = copy.total[1] - copy.total[0], idle = copy.idle[1] - copy.idle[0];
  double percent = 0.0;
  if (total != 0) percent = idle >= total ? 0.0 : 100.0 * (double)(total - idle) / (double)total;
  value->status = PDH_CSTATUS_VALID_DATA_;
  if (kinds == PDH_FMT_DOUBLE_) value->value.double_value = percent;
  else if (kinds == PDH_FMT_LONG_) value->value.long_value = (LONG)(percent + 0.5);
  else value->value.large_value = (LONGLONG)(percent + 0.5);
  return ERROR_SUCCESS;
}
