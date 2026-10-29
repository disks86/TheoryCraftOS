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
  if (!token || !size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
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
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!GetUserProfileDirectoryW(token, wide, &n)) return FALSE;
  if (!buffer || *size < n) { *size = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (DWORD i = 0; i < n; i++) buffer[i] = (char)wide[i];      /* (ASCII) */
  *size = n;
  return TRUE;
}
