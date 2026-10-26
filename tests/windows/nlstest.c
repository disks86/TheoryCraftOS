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

/* Locale and string functions, atoms, completion ports, thread descriptions, process memory, version checks (mingw gcc; see tests/windows/build.sh).
 * Prints "nlstest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <tlhelp32.h>
BOOL WINAPI K32EnumProcesses(DWORD *, DWORD, DWORD *);

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

static DWORD WINAPI poster(LPVOID port) {
  Sleep(20);
  PostQueuedCompletionStatus((HANDLE)port, 7, 99, (LPOVERLAPPED)0x1000);
  return 0;
}

int main(void) {
  /* locale */
  WCHAR w[128];
  char a[128];
  check(GetLocaleInfoW(0x409, LOCALE_SDECIMAL, w, 128) == 2 && w[0] == '.', "decimal separator");
  check(GetLocaleInfoA(0x409, LOCALE_SMONTHNAME3, a, 128) == 6 && strcmp(a, "March") == 0, "month name");
  check(GetLocaleInfoA(0x409, LOCALE_SABBREVDAYNAME1, a, 128) == 4 && strcmp(a, "Mon") == 0, "abbreviated day");
  DWORD n = 0;
  check(GetLocaleInfoW(0x409, LOCALE_IDIGITS | LOCALE_RETURN_NUMBER, (LPWSTR)&n, 2) == 2 && n == 2, "number result");
  check(GetLocaleInfoW(0x409, LOCALE_SDECIMAL, w, 1) == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "small buffer");
  check(GetLocaleInfoW(0x409, LOCALE_SDECIMAL, NULL, 0) == 2, "size query");

  /* strings */
  check(CompareStringOrdinal(L"abc", -1, L"ABC", -1, TRUE) == CSTR_EQUAL, "ordinal ignore case");
  check(CompareStringOrdinal(L"abc", -1, L"abd", -1, FALSE) == CSTR_LESS_THAN, "ordinal less");
  check(FindStringOrdinal(0, L"hello world", -1, L"WORLD", -1, TRUE) == 6, "find ordinal");
  check(LCMapStringW(0x409, LCMAP_UPPERCASE, L"abc\xe9", 4, w, 128) == 4 && w[0] == 'A' && w[3] == 0xC9, "upper");
  check(LCMapStringW(0x409, LCMAP_LOWERCASE, L"ABC", -1, w, 128) == 4 && wcscmp(w, L"abc") == 0, "lower");
  WORD t[4];
  check(GetStringTypeW(CT_CTYPE1, L"a1 ,", 4, t) && (t[0] & C1_ALPHA) && (t[0] & C1_LOWER) && (t[1] & C1_DIGIT) && (t[2] & C1_SPACE) && (t[3] & C1_PUNCT), "string type");

  /* number, currency, date, time */
  check(GetNumberFormatW(0x409, 0, L"1234567.891", NULL, w, 128) > 0 && wcscmp(w, L"1,234,567.89") == 0, "number format");
  check(GetNumberFormatW(0x409, 0, L"-0.5", NULL, w, 128) > 0 && wcscmp(w, L"-0.50") == 0, "negative number");
  check(GetCurrencyFormatW(0x409, 0, L"1234.5", NULL, w, 128) > 0 && wcscmp(w, L"$1,234.50") == 0, "currency");
  SYSTEMTIME st = {2026, 3, 1, 15, 14, 5, 9, 0};      /* Monday, March 15, 2026, 14:05:09 */
  check(GetDateFormatW(0x409, DATE_LONGDATE, &st, NULL, w, 128) > 0 && wcscmp(w, L"Monday, March 15, 2026") == 0, "long date");
  check(GetDateFormatW(0x409, DATE_SHORTDATE, &st, NULL, w, 128) > 0 && wcscmp(w, L"3/15/2026") == 0, "short date");
  check(GetTimeFormatW(0x409, 0, &st, NULL, w, 128) > 0 && wcscmp(w, L"2:05:09 PM") == 0, "time");
  check(GetTimeFormatW(0x409, TIME_FORCE24HOURFORMAT | TIME_NOSECONDS, &st, NULL, w, 128) > 0 && wcscmp(w, L"14:05") == 0, "24 hour time");
  check(GetDateFormatW(0x409, 0, &st, L"yyyy-MM-dd", w, 128) > 0 && wcscmp(w, L"2026-03-15") == 0, "custom picture");

  /* atoms */
  ATOM x = AddAtomW(L"tcos.atom"), y = AddAtomW(L"TCOS.ATOM");
  check(x >= 0xC000 && x == y, "atom added twice is one atom");
  check(FindAtomW(L"tcos.atom") == x && GetAtomNameW(x, w, 128) == 9 && wcscmp(w, L"tcos.atom") == 0, "atom found and named");
  check(DeleteAtom(x) == 0 && DeleteAtom(x) == 0 && FindAtomW(L"tcos.atom") == 0, "atom deleted after both references");
  ATOM g = GlobalAddAtomA("global.one");
  check(g >= 0xC000 && GlobalFindAtomA("global.one") == g && FindAtomA("global.one") == 0, "global atoms are separate");
  check(GlobalGetAtomNameA(g, a, 128) == 10 && strcmp(a, "global.one") == 0, "global atom name");
  check(GlobalDeleteAtom(g) == 0, "global atom deleted");

  /* completion ports */
  HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
  check(port != NULL, "port created");
  DWORD bytes = 0; ULONG_PTR key = 0; LPOVERLAPPED ov = NULL;
  check(!GetQueuedCompletionStatus(port, &bytes, &key, &ov, 10) && GetLastError() == WAIT_TIMEOUT, "empty port times out");
  check(PostQueuedCompletionStatus(port, 3, 5, (LPOVERLAPPED)0x2000), "post");
  check(GetQueuedCompletionStatus(port, &bytes, &key, &ov, 1000) && bytes == 3 && key == 5 && ov == (LPOVERLAPPED)0x2000, "packet received");
  HANDLE th = CreateThread(NULL, 0, poster, port, 0, NULL);
  check(GetQueuedCompletionStatus(port, &bytes, &key, &ov, 5000) && bytes == 7 && key == 99, "packet from another thread");
  WaitForSingleObject(th, 2000); CloseHandle(th);
  CloseHandle(port);

  /* thread description, process memory, processors, versions */
  PWSTR d = NULL;
  check(SetThreadDescription(GetCurrentThread(), L"main thread") == S_OK && GetThreadDescription(GetCurrentThread(), &d) == S_OK && d && wcscmp(d, L"main thread") == 0, "thread description");
  if (d) LocalFree(d);
  int src = 1234, dst = 0;
  SIZE_T done = 0;
  check(ReadProcessMemory(GetCurrentProcess(), &src, &dst, sizeof(dst), &done) && dst == 1234 && done == sizeof(dst), "read own memory");
  dst = 77;
  check(WriteProcessMemory(GetCurrentProcess(), &src, &dst, sizeof(dst), &done) && src == 77, "write own memory");
  check(!ReadProcessMemory(GetCurrentProcess(), (void *)8, &dst, 4, &done), "bad address fails");
  DWORD len = 0;
  GetLogicalProcessorInformation(NULL, &len);
  check(len >= sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION), "processor information size");
  OSVERSIONINFOEXW need = {sizeof(need), 6, 0, 0, 0, {0}, 0, 0, 0, 0, 0};
  ULONGLONG mask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
  check(VerifyVersionInfoW(&need, VER_MAJORVERSION, mask), "at least version 6");
  need.dwMajorVersion = 99;
  check(!VerifyVersionInfoW(&need, VER_MAJORVERSION, mask), "not version 99");
  DWORD product = 0;
  check(GetProductInfo(10, 0, 0, 0, &product) && product != 0, "product info");

  /* process list */
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  check(snap != INVALID_HANDLE_VALUE, "snapshot");
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  int found = 0, entries = 0;
  for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
    ++entries;
    if (pe.th32ProcessID == GetCurrentProcessId() && pe.cntThreads >= 1) found = 1;
  }
  check(entries >= 1 && found, "this process is in the list");
  check(!Process32NextW(snap, &pe) && GetLastError() == ERROR_NO_MORE_FILES, "end of the list");
  CloseHandle(snap);
  DWORD ids[64], needed = 0, mine = 0;
  check(K32EnumProcesses(ids, sizeof(ids), &needed) && needed >= 4, "EnumProcesses");
  for (DWORD i = 0; i < needed / 4; ++i) if (ids[i] == GetCurrentProcessId()) mine = 1;
  check(mine, "EnumProcesses lists this process");

  printf("nlstest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
