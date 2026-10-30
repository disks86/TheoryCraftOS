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
typedef struct { DWORD LowPart; LONG HighPart; } LUID, *PLUID;
/* Privileges: the names programs know them by and their numbers (the low part of the LUID). The user of this OS is a standard one: the privileges that a standard user's token holds
 * are the ones that can be enabled; asking for another one is answered as Windows does, with success and ERROR_NOT_ALL_ASSIGNED. */
static const struct { const char *name; DWORD value; BOOL held; } g_privileges[] = {
  {"SeCreateTokenPrivilege", 2, 0}, {"SeAssignPrimaryTokenPrivilege", 3, 0}, {"SeLockMemoryPrivilege", 4, 0}, {"SeIncreaseQuotaPrivilege", 5, 0}, {"SeMachineAccountPrivilege", 6, 0},
  {"SeTcbPrivilege", 7, 0}, {"SeSecurityPrivilege", 8, 0}, {"SeTakeOwnershipPrivilege", 9, 0}, {"SeLoadDriverPrivilege", 10, 0}, {"SeSystemProfilePrivilege", 11, 0},
  {"SeSystemtimePrivilege", 12, 0}, {"SeProfileSingleProcessPrivilege", 13, 0}, {"SeIncreaseBasePriorityPrivilege", 14, 0}, {"SeCreatePagefilePrivilege", 15, 0},
  {"SeCreatePermanentPrivilege", 16, 0}, {"SeBackupPrivilege", 17, 0}, {"SeRestorePrivilege", 18, 0}, {"SeShutdownPrivilege", 19, 1}, {"SeDebugPrivilege", 20, 0},
  {"SeAuditPrivilege", 21, 0}, {"SeSystemEnvironmentPrivilege", 22, 0}, {"SeChangeNotifyPrivilege", 23, 1}, {"SeRemoteShutdownPrivilege", 24, 0}, {"SeUndockPrivilege", 25, 1},
  {"SeSyncAgentPrivilege", 26, 0}, {"SeEnableDelegationPrivilege", 27, 0}, {"SeManageVolumePrivilege", 28, 0}, {"SeImpersonatePrivilege", 29, 0}, {"SeCreateGlobalPrivilege", 30, 0},
  {"SeTrustedCredManAccessPrivilege", 31, 0}, {"SeRelabelPrivilege", 32, 0}, {"SeIncreaseWorkingSetPrivilege", 33, 1}, {"SeTimeZonePrivilege", 34, 1},
  {"SeCreateSymbolicLinkPrivilege", 35, 1}, {"SeDelegateSessionUserImpersonatePrivilege", 36, 0},
};
#define PRIVILEGE_COUNT_ ((int)(sizeof(g_privileges) / sizeof(g_privileges[0])))
EXPORT BOOL WINAPI LookupPrivilegeValueA(LPCSTR system, LPCSTR name, PLUID luid) {
  if (!name || !luid || (system && *system)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  for (int i = 0; i < PRIVILEGE_COUNT_; ++i) {
    const char *a = name, *b = g_privileges[i].name;
    while (*a && ((*a | 0x20) == (*b | 0x20))) { ++a; ++b; }
    if (*a || *b) continue;
    luid->LowPart = g_privileges[i].value;
    luid->HighPart = 0;
    return TRUE;
  }
  SetLastError(1313 /* ERROR_NO_SUCH_PRIVILEGE */);
  return FALSE;
}
EXPORT BOOL WINAPI LookupPrivilegeValueW(LPCWSTR system, LPCWSTR name, PLUID luid) {
  char narrow[64];
  size_t_ n = 0;
  if (!name || (system && *system)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  for (; name[n] && n < sizeof(narrow) - 1; ++n) { if (name[n] > 0x7F) { SetLastError(1313); return FALSE; } narrow[n] = (char)name[n]; }
  if (name[n]) { SetLastError(1313); return FALSE; }
  narrow[n] = 0;
  return LookupPrivilegeValueA(NULL, narrow, luid);
}
EXPORT BOOL WINAPI LookupPrivilegeNameW(LPCWSTR system, PLUID luid, LPWSTR name, LPDWORD size) {
  if (!luid || !size || (system && *system)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  for (int i = 0; i < PRIVILEGE_COUNT_; ++i) {
    if (luid->HighPart != 0 || luid->LowPart != g_privileges[i].value) continue;
    const DWORD length = (DWORD)strlen(g_privileges[i].name);
    if (*size < length + 1) { *size = length + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (DWORD k = 0; k <= length; ++k) name[k] = (WCHAR)(unsigned char)g_privileges[i].name[k];
    *size = length;
    return TRUE;
  }
  SetLastError(1313);
  return FALSE;
}
EXPORT BOOL WINAPI LookupPrivilegeNameA(LPCSTR system, PLUID luid, LPSTR name, LPDWORD size) {
  if (!luid || !size || (system && *system)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  for (int i = 0; i < PRIVILEGE_COUNT_; ++i) {
    if (luid->HighPart != 0 || luid->LowPart != g_privileges[i].value) continue;
    const DWORD length = (DWORD)strlen(g_privileges[i].name);
    if (*size < length + 1) { *size = length + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(name, g_privileges[i].name, length + 1);
    *size = length;
    return TRUE;
  }
  SetLastError(1313);
  return FALSE;
}
typedef struct { LUID Luid; DWORD Attributes; } LUID_AND_ATTRIBUTES_;
typedef struct { DWORD PrivilegeCount; LUID_AND_ATTRIBUTES_ Privileges[1]; } TOKEN_PRIVILEGES_;
/* AdjustTokenPrivileges: enabling a privilege the token holds works; one it does not hold leaves the call a success with ERROR_NOT_ALL_ASSIGNED, as on Windows. */
EXPORT BOOL WINAPI AdjustTokenPrivileges(HANDLE token, BOOL disable_all, TOKEN_PRIVILEGES_ *state, DWORD length, TOKEN_PRIVILEGES_ *previous, LPDWORD returned) {
  (void)disable_all; (void)length;
  if (token != TOKEN_PSEUDO_HANDLE_) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  DWORD error = 0;
  if (state) {
    for (DWORD i = 0; i < state->PrivilegeCount; ++i) {
      BOOL held = FALSE;
      for (int k = 0; k < PRIVILEGE_COUNT_; ++k) if (state->Privileges[i].Luid.HighPart == 0 && state->Privileges[i].Luid.LowPart == g_privileges[k].value) held = g_privileges[k].held;
      if (!held && (state->Privileges[i].Attributes & 2 /* SE_PRIVILEGE_ENABLED */)) error = 1300 /* ERROR_NOT_ALL_ASSIGNED */;
    }
  }
  if (previous) previous->PrivilegeCount = 0;
  if (returned) *returned = 4;
  SetLastError(error);
  return TRUE;
}
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
    case 10: {      /* TokenStatistics: a primary token without privileges (see AdjustTokenPrivileges) */
      if (returned) *returned = 56;
      if (capacity < 56) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
      DWORD *words = info;
      memset(info, 0, 56);
      words[0] = 0x1000; words[2] = 0x3E7;      /* TokenId, AuthenticationId */
      words[4] = words[5] = 0x7FFFFFFF;         /* ExpirationTime: never */
      words[6] = 1;                             /* TokenType: TokenPrimary */
      words[7] = 0;                             /* ImpersonationLevel */
      words[12] = 0x1000;                       /* ModifiedId */
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

/* ---- access control lists ---------------------------------------------------------------------------------------------------------------------------------- */

IMPORT NTSTATUS NTAPI RtlCreateAcl(PVOID acl, DWORD size, DWORD revision);
EXPORT BOOL WINAPI InitializeAcl(PVOID acl, DWORD size, DWORD revision) {
  const NTSTATUS status = RtlCreateAcl(acl, size, revision);
  if (status) { SetLastError(RtlNtStatusToDosError(status)); return FALSE; }
  return TRUE;
}

/* ---- security identifiers as text ------------------------------------------------------------------------------------------------------------------------- */

IMPORT HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T bytes);
static WCHAR *sid_text_number(WCHAR *out, ULONGLONG v, int hex) {
  char digits[24];
  int n = 0;
  const unsigned base = hex ? 16 : 10;
  do { const unsigned d = (unsigned)(v % base); digits[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
  while (n) *out++ = (WCHAR)digits[--n];
  return out;
}
EXPORT BOOL WINAPI ConvertSidToStringSidW(PVOID sid_in, LPWSTR *text) {
  const BYTE *sid = sid_in;
  if (!sid || !text || sid[0] != 1 || sid[1] > 15) { SetLastError(87 /* ERROR_INVALID_PARAMETER */); return FALSE; }
  WCHAR buffer[16 * 12 + 40], *p = buffer;
  *p++ = 'S'; *p++ = '-';
  p = sid_text_number(p, sid[0], 0);
  *p++ = '-';
  if (sid[2] == 0 && sid[3] == 0) p = sid_text_number(p, (ULONGLONG)sid[4] << 24 | (ULONGLONG)sid[5] << 16 | (ULONGLONG)sid[6] << 8 | sid[7], 0);
  else { *p++ = '0'; *p++ = 'x'; for (int i = 2; i < 8; ++i) { *p++ = (WCHAR)"0123456789abcdef"[sid[i] >> 4]; *p++ = (WCHAR)"0123456789abcdef"[sid[i] & 15]; } }
  for (int i = 0; i < sid[1]; ++i) {
    const BYTE *sub = sid + 8 + 4 * i;
    *p++ = '-';
    p = sid_text_number(p, (ULONG)sub[0] | (ULONG)sub[1] << 8 | (ULONG)sub[2] << 16 | (ULONG)sub[3] << 24, 0);
  }
  *p = 0;
  const SIZE_T bytes = (SIZE_T)(p - buffer + 1) * sizeof(WCHAR);
  WCHAR *out = LocalAlloc(0, bytes);
  if (!out) { SetLastError(8 /* ERROR_NOT_ENOUGH_MEMORY */); return FALSE; }
  memcpy(out, buffer, bytes);
  *text = out;
  return TRUE;
}
