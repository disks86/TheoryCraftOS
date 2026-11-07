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

/* pdh.dll for TheoryCraftOS: performance data queries. A query can be opened and closed (the handles are real and checked), but there are no performance counters, so
 * adding one answers PDH_CSTATUS_NO_OBJECT, collecting a query answers PDH_NO_DATA and a counter handle is never valid (a stub answered ERROR_SUCCESS and left the
 * handles unset). The rest is still stubs (src/Windows/stubs/pdh.inc). Built freestanding over kernel32.dll and ntdll.dll. */
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
  ReleaseSRWLockExclusive(&g_lock);
  return ERROR_SUCCESS;
}

/* No counter exists: every counter path names an object that is not there. */
static PDH_STATUS_ add_counter(HANDLE query, const void *path, HANDLE *counter) {
  if (counter) *counter = NULL;
  if (!query_valid(query)) return PDH_INVALID_HANDLE_;
  if (!path || !counter) return PDH_INVALID_ARGUMENT_;
  return PDH_CSTATUS_NO_OBJECT_;
}
EXPORT PDH_STATUS_ WINAPI PdhAddEnglishCounterW(HANDLE query, LPCWSTR path, DWORD_PTR user, HANDLE *counter) { (void)user; return add_counter(query, path, counter); }
EXPORT PDH_STATUS_ WINAPI PdhAddEnglishCounterA(HANDLE query, LPCSTR path, DWORD_PTR user, HANDLE *counter) { (void)user; return add_counter(query, path, counter); }
EXPORT PDH_STATUS_ WINAPI PdhAddCounterW(HANDLE query, LPCWSTR path, DWORD_PTR user, HANDLE *counter) { (void)user; return add_counter(query, path, counter); }
EXPORT PDH_STATUS_ WINAPI PdhAddCounterA(HANDLE query, LPCSTR path, DWORD_PTR user, HANDLE *counter) { (void)user; return add_counter(query, path, counter); }
EXPORT PDH_STATUS_ WINAPI PdhRemoveCounter(HANDLE counter) { (void)counter; return PDH_INVALID_HANDLE_; }

/* A query with no counters has no data to collect. */
EXPORT PDH_STATUS_ WINAPI PdhCollectQueryData(HANDLE query) { return query_valid(query) ? PDH_NO_DATA_ : PDH_INVALID_HANDLE_; }

/* PDH_FMT_COUNTERVALUE: the status and an 8-byte value. */
typedef struct { DWORD status; DWORD padding; LONGLONG value; } FormattedValue;
EXPORT PDH_STATUS_ WINAPI PdhGetFormattedCounterValue(HANDLE counter, DWORD format, LPDWORD type, FormattedValue *value) {
  (void)counter; (void)format;
  if (type) *type = 0;
  if (value) { value->status = (DWORD)PDH_INVALID_HANDLE_; value->padding = 0; value->value = 0; }
  return PDH_INVALID_HANDLE_;
}
