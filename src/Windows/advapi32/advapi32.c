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

/* advapi32.dll for TheoryCraftOS: the registry, and a few of the security functions programs link this library for without really using security. Written entirely over
 * kernel32.dll (no NT services of its own): a registry key is a directory under C:\Windows\System32\config\<hive>, and a value is one file in it holding a small
 * [type, length, data] header the program's bytes follow; the predefined keys (HKEY_LOCAL_MACHINE and so on) are fixed pseudo-handles that map to the hive directories, made
 * the first time something is created under them. This makes the registry an ordinary part of the file system: it is exactly as persistent as whatever is mounted there (the
 * RAM file system of the boot demo keeps nothing between boots, like everything else in it; a real disk would). There is one registry, shared by every process, matching how
 * the real one is machine-wide; this OS has no separate per-user hive, so HKEY_CURRENT_USER is just another fixed directory. Built freestanding with the OS's mingw compiler,
 * directly over kernel32.dll, the way a real advapi32.dll is layered over kernel32.dll's file functions for the registry. */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

typedef LONG LSTATUS;
typedef DWORD REGSAM;
typedef HKEY *PHKEY;

/* From kernel32.dll. */
IMPORT PVOID NTAPI RtlAllocateHeap(PVOID, ULONG, SIZE_T);
IMPORT BOOLEAN NTAPI RtlFreeHeap(PVOID, ULONG, PVOID);
IMPORT PVOID NTAPI RtlProcessHeap(void);
IMPORT void *memcpy(void *, const void *, size_t_);
IMPORT void *memset(void *, int, size_t_);
IMPORT int memcmp(const void *, const void *, size_t_);
IMPORT size_t_ wcslen(const WCHAR *);
IMPORT size_t_ strlen(const char *);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT void WINAPI SetLastError(DWORD error);
IMPORT HANDLE WINAPI CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file);
IMPORT BOOL WINAPI ReadFile(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPOVERLAPPED overlapped);
IMPORT BOOL WINAPI WriteFile(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, LPOVERLAPPED overlapped);
IMPORT BOOL WINAPI CloseHandle(HANDLE h);
IMPORT BOOL WINAPI DeleteFileW(LPCWSTR path);
IMPORT BOOL WINAPI CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa);
IMPORT BOOL WINAPI RemoveDirectoryW(LPCWSTR path);
IMPORT DWORD WINAPI GetFileAttributesW(LPCWSTR path);
IMPORT HANDLE WINAPI FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data);
IMPORT BOOL WINAPI FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data);
IMPORT BOOL WINAPI FindClose(HANDLE h);
IMPORT ULONGLONG WINAPI GetTickCount64(void);
IMPORT DWORD WINAPI GetCurrentProcessId(void);

static PVOID heap(void) { return RtlProcessHeap(); }
static LSTATUS fail(void) { return (LSTATUS)GetLastError(); }

/* ---- wide string helpers (advapi32 is its own small freestanding program: no CRT, so no wcscpy/wcscat/wcsrchr) ---------------------------------------------- */

static void wcopy(WCHAR *dest, const WCHAR *src) { while ((*dest++ = *src++) != 0) {} }
static void wappend(WCHAR *dest, const WCHAR *src) { while (*dest) ++dest; wcopy(dest, src); }
static BOOL is_dot_entry(const WCHAR *name) { return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)); }      /* "." and ".." (FindFirstFile lists them here, unlike the real one) */
static BOOL wequal_ignore_case(const WCHAR *a, const WCHAR *b) {
  for (;; ++a, ++b) {
    WCHAR x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (WCHAR)(x - 32);
    if (y >= 'a' && y <= 'z') y = (WCHAR)(y - 32);
    if (x != y) return FALSE;
    if (!x) return TRUE;
  }
}

#include "registry.inc"

/* ---- a little security: enough that a program only checking "am I an administrator" or using crypto randomness works -------------------------------------- */

EXPORT BOOL WINAPI GetUserNameW(LPWSTR buffer, LPDWORD capacity) {
  static const WCHAR name[] = L"User";
  const DWORD n = (DWORD)(sizeof(name) / 2);
  if (!capacity || *capacity < n) { if (capacity) *capacity = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memcpy(buffer, name, n * 2);
  *capacity = n;
  return TRUE;
}
EXPORT BOOL WINAPI GetUserNameA(LPSTR buffer, LPDWORD capacity) {
  if (!capacity || *capacity < 5) { if (capacity) *capacity = 5; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memcpy(buffer, "User", 5);
  *capacity = 5;
  return TRUE;
}
/* OpenProcessToken / GetTokenInformation: enough to answer "what user, and is it elevated" (always: yes). There is one user and no separate security principals. */
#define TOKEN_PSEUDO_HANDLE_ ((HANDLE)(ULONG_PTR)0x4B4F54)      /* "TOK" in ASCII, so it is never mistaken for a real one */
EXPORT BOOL WINAPI OpenProcessToken(HANDLE process, DWORD access, PHANDLE token) {
  (void)process; (void)access;
  if (!token) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *token = TOKEN_PSEUDO_HANDLE_;
  return TRUE;
}
EXPORT BOOL WINAPI OpenThreadToken(HANDLE thread, DWORD access, BOOL self, PHANDLE token) { (void)thread; (void)self; return OpenProcessToken(NULL, access, token); }
typedef struct { DWORD Attributes; } SID_AND_ATTRIBUTES_;
typedef struct { PVOID Sid; } TOKEN_USER_;
EXPORT BOOL WINAPI GetTokenInformation(HANDLE token, int class_, PVOID info, DWORD capacity, LPDWORD returned) {
  if (token != TOKEN_PSEUDO_HANDLE_) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  switch (class_) {
    case 2: {      /* TokenGroups: none beyond the default */
      if (returned) *returned = 4;
      if (capacity < 4) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
      memset(info, 0, 4);
      return TRUE;
    }
    case 18: case 20: {      /* TokenElevationType (1 = default, as if UAC had already allowed it), TokenElevation (1 = elevated: there is nothing this OS refuses a program) */
      if (returned) *returned = 4;
      if (capacity < 4) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
      *(DWORD *)info = 1;
      return TRUE;
    }
    default: SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
  }
}
/* CryptAcquireContext / CryptGenRandom / CryptReleaseContext: a program that only wants non-predictable bytes (not a cryptographic guarantee: there is no entropy
 * driver here yet, matching kernel32's own GetCurrentProcessId()-seeded sources elsewhere) gets them; anything about named key containers or providers is refused. */
#define PROV_RSA_FULL_ 1
EXPORT BOOL WINAPI CryptAcquireContextW(HANDLE *provider, LPCWSTR container, LPCWSTR provider_name, DWORD provider_type, DWORD flags) {
  (void)container; (void)provider_name;
  if (provider_type != 0 && provider_type != PROV_RSA_FULL_) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
  (void)flags;
  if (!provider) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *provider = (HANDLE)(ULONG_PTR)0x435259;      /* "CRY": a pseudo-handle, like the token above */
  return TRUE;
}
EXPORT BOOL WINAPI CryptAcquireContextA(HANDLE *provider, LPCSTR container, LPCSTR provider_name, DWORD provider_type, DWORD flags) {
  (void)container; (void)provider_name;
  return CryptAcquireContextW(provider, NULL, NULL, provider_type, flags);
}
EXPORT BOOL WINAPI CryptReleaseContext(HANDLE provider, DWORD flags) { (void)provider; (void)flags; return TRUE; }
EXPORT BOOL WINAPI CryptGenRandom(HANDLE provider, DWORD length, BYTE *buffer) {
  (void)provider;
  static ULONGLONG state;
  if (!state) state = GetTickCount64() ^ ((ULONGLONG)GetCurrentProcessId() << 32) ^ 0x9E3779B97F4A7C15ull;
  for (DWORD i = 0; i < length; ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    buffer[i] = (BYTE)(state >> 56);
  }
  return TRUE;
}

EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved; (void)reason;
  return TRUE;
}
