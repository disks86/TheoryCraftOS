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

/* userenv.dll for TheoryCraftOS: the profile folders. There is one user; the profile is C:\Users\Default (the USERPROFILE of the environment). */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

IMPORT void WINAPI SetLastError(DWORD error);
IMPORT DWORD WINAPI GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD capacity);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int length, LPSTR dest, int capacity, LPCSTR default_char, LPBOOL used_default);

static const WCHAR kVariable[] = {'U', 'S', 'E', 'R', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0};
static const WCHAR kDefault[] = {'C', ':', '\\', 'U', 's', 'e', 'r', 's', '\\', 'D', 'e', 'f', 'a', 'u', 'l', 't', 0};

/* The folder in `buffer` (size in characters, with the end mark); on a buffer too small the size needed is stored and the error is ERROR_INSUFFICIENT_BUFFER. */
EXPORT BOOL WINAPI GetUserProfileDirectoryW(HANDLE token, LPWSTR buffer, LPDWORD size) {
  WCHAR folder[260];
  if (!token) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  DWORD n = GetEnvironmentVariableW(kVariable, folder, 260);
  if (n == 0 || n >= 260) { for (n = 0; kDefault[n]; n++) folder[n] = kDefault[n]; folder[n] = 0; }
  if (!buffer || *size < n + 1) { *size = n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (DWORD i = 0; i <= n; i++) buffer[i] = folder[i];
  *size = n + 1;
  return TRUE;
}
EXPORT BOOL WINAPI GetUserProfileDirectoryA(HANDLE token, LPSTR buffer, LPDWORD size) {
  WCHAR wide[260];
  DWORD n = 260;
  if (!size || !buffer) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!GetUserProfileDirectoryW(token, wide, &n)) return FALSE;
  if (*size < n) { *size = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (DWORD i = 0; i < n; i++) buffer[i] = (char)wide[i];      /* (ASCII) */
  *size = n;
  return TRUE;
}

/* ---- environment blocks ---------------------------------------------------------------------------------------------------------------------------------- */

IMPORT WCHAR *WINAPI GetEnvironmentStringsW(void);
IMPORT BOOL WINAPI FreeEnvironmentStringsW(WCHAR *block);
IMPORT HANDLE WINAPI GetProcessHeap(void);
IMPORT LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
IMPORT BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID memory);
IMPORT size_t_ wcslen(const WCHAR *);

#define W(...) (const WCHAR *)(const WCHAR[]){__VA_ARGS__, 0}
static const struct { const char *name, *value; } kSystemVariables[] = {
  {"ALLUSERSPROFILE", "C:\\ProgramData"}, {"APPDATA", "C:\\Users\\Default\\AppData\\Roaming"}, {"CommonProgramFiles", "C:\\Program Files\\Common Files"},
  {"CommonProgramFiles(x86)", "C:\\Program Files (x86)\\Common Files"}, {"CommonProgramW6432", "C:\\Program Files\\Common Files"}, {"COMPUTERNAME", "TCOS"},
  {"ComSpec", "C:\\Windows\\System32\\cmd.exe"}, {"LOCALAPPDATA", "C:\\Users\\Default\\AppData\\Local"}, {"OS", "Windows_NT"}, {"PATH", "C:\\Windows\\System32;C:\\Windows"},
  {"PROCESSOR_ARCHITECTURE", "AMD64"}, {"PROCESSOR_IDENTIFIER", "Intel64 Family 6 Model 1 Stepping 1, GenuineIntel"}, {"PROCESSOR_LEVEL", "6"}, {"PROCESSOR_REVISION", "0101"},
  {"ProgramData", "C:\\ProgramData"}, {"ProgramFiles", "C:\\Program Files"}, {"ProgramFiles(x86)", "C:\\Program Files (x86)"}, {"ProgramW6432", "C:\\Program Files"},
  {"SystemDrive", "C:"}, {"TEMP", "C:\\Temp"}, {"TMP", "C:\\Temp"}, {"USERNAME", "Default"}, {"USERPROFILE", "C:\\Users\\Default"}, {"windir", "C:\\Windows"},
};

static WCHAR upcase(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }
/* Does the entry `entry` ("NAME=value") start with `name`= ? */
static BOOL has_name(const WCHAR *entry, const char *name) {
  size_t_ i = 0;
  for (; name[i]; i++) if (upcase(entry[i]) != upcase((WCHAR)name[i])) return FALSE;
  return entry[i] == '=';
}

/* A new environment block for a user: the system variables fresh, SystemRoot and the number of processors as this process has them and, with `inherit`, everything else of this
   process's environment. DestroyEnvironmentBlock gives it back. */
EXPORT BOOL WINAPI CreateEnvironmentBlock(LPVOID *environment, HANDLE token, BOOL inherit) {
  if (!environment) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }      /* (no token: the default user) */
  WCHAR *current = GetEnvironmentStringsW();
  if (!current) return FALSE;
  size_t_ total = 2;
  for (size_t_ k = 0; k < sizeof(kSystemVariables) / sizeof(kSystemVariables[0]); k++) {
    size_t_ n = 0;
    while (kSystemVariables[k].name[n]) n++;
    size_t_ v = 0;
    while (kSystemVariables[k].value[v]) v++;
    total += n + 1 + v + 1;
  }
  total += 2 * 512;      /* SystemRoot and NUMBER_OF_PROCESSORS from this process */
  if (inherit) for (const WCHAR *e = current; *e; e += wcslen(e) + 1) total += wcslen(e) + 1;
  WCHAR *block = HeapAlloc(GetProcessHeap(), 0, total * 2);
  if (!block) { FreeEnvironmentStringsW(current); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  size_t_ out = 0;
  for (size_t_ k = 0; k < sizeof(kSystemVariables) / sizeof(kSystemVariables[0]); k++) {
    for (const char *c = kSystemVariables[k].name; *c; c++) block[out++] = (unsigned char)*c;
    block[out++] = '=';
    for (const char *c = kSystemVariables[k].value; *c; c++) block[out++] = (unsigned char)*c;
    block[out++] = 0;
  }
  BOOL got_processors = FALSE;
  for (const WCHAR *e = current; *e; e += wcslen(e) + 1) {
    const size_t_ n = wcslen(e);
    const BOOL taken = has_name(e, "SystemRoot") || has_name(e, "NUMBER_OF_PROCESSORS");
    if (has_name(e, "NUMBER_OF_PROCESSORS")) got_processors = TRUE;
    BOOL known = FALSE;
    if (!taken) for (size_t_ k = 0; k < sizeof(kSystemVariables) / sizeof(kSystemVariables[0]); k++) if (has_name(e, kSystemVariables[k].name)) known = TRUE;
    if (!(taken || (inherit && !known))) continue;
    if (out + n + 1 > total - 2) break;
    for (size_t_ i = 0; i <= n; i++) block[out++] = e[i];
  }
  if (!got_processors) { const WCHAR *one = W('N','U','M','B','E','R','_','O','F','_','P','R','O','C','E','S','S','O','R','S','=','1'); size_t_ n = wcslen(one); for (size_t_ i = 0; i <= n; i++) block[out++] = one[i]; }
  block[out++] = 0;
  FreeEnvironmentStringsW(current);
  *environment = block;
  return TRUE;
}
EXPORT BOOL WINAPI DestroyEnvironmentBlock(LPVOID environment) {
  if (!environment) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return HeapFree(GetProcessHeap(), 0, environment);
}

/* ---- profile folders -------------------------------------------------------------------------------------------------------------------------------- */

/* The folder (from the environment, expanded: %SystemDrive%\Users) into `buffer`. A buffer too small gets the size needed in characters, with the end mark, and
   ERROR_INSUFFICIENT_BUFFER; GetProfilesDirectoryA's size on that error leaves out the end mark, and its result is the number of characters stored with the end mark. */
static BOOL folder_w(const WCHAR *text, LPWSTR buffer, LPDWORD size) {
  size_t_ n = wcslen(text);
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!buffer || *size < n + 1) { *size = (DWORD)n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (size_t_ i = 0; i <= n; i++) buffer[i] = text[i];
  *size = (DWORD)n + 1;
  return TRUE;
}
EXPORT BOOL WINAPI GetProfilesDirectoryW(LPWSTR buffer, LPDWORD size) { return folder_w(W('C',':','\\','U','s','e','r','s'), buffer, size); }
EXPORT BOOL WINAPI GetDefaultUserProfileDirectoryW(LPWSTR buffer, LPDWORD size) { return folder_w(W('C',':','\\','U','s','e','r','s','\\','D','e','f','a','u','l','t'), buffer, size); }
EXPORT BOOL WINAPI GetAllUsersProfileDirectoryW(LPWSTR buffer, LPDWORD size) { return folder_w(W('C',':','\\','P','r','o','g','r','a','m','D','a','t','a'), buffer, size); }
EXPORT DWORD WINAPI GetProfilesDirectoryA(LPSTR buffer, LPDWORD size) {
  WCHAR wide[64];
  DWORD n = 64;
  if (!size || (!buffer && *size == 0 && 0)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (!GetProfilesDirectoryW(wide, &n)) return 0;
  if (!buffer) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (*size < n) { *size = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (DWORD i = 0; i < n; i++) buffer[i] = (char)wide[i];
  *size = n;
  return n;
}
EXPORT BOOL WINAPI GetDefaultUserProfileDirectoryA(LPSTR buffer, LPDWORD size) {
  WCHAR wide[64];
  DWORD n = 64;
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!GetDefaultUserProfileDirectoryW(wide, &n)) return FALSE;
  if (!buffer || *size < n) { *size = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (DWORD i = 0; i < n; i++) buffer[i] = (char)wide[i];
  *size = n;
  return TRUE;
}
EXPORT BOOL WINAPI GetAllUsersProfileDirectoryA(LPSTR buffer, LPDWORD size) {
  WCHAR wide[64];
  DWORD n = 64;
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!GetAllUsersProfileDirectoryW(wide, &n)) return FALSE;
  if (!buffer || *size < n) { *size = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (DWORD i = 0; i < n; i++) buffer[i] = (char)wide[i];
  *size = n;
  return TRUE;
}
