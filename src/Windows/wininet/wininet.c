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

/* wininet.dll for TheoryCraftOS: the URL cache is empty (there is no Internet Explorer cache), so a lookup in it finds nothing: ERROR_FILE_NOT_FOUND, as Windows answers
 * for a URL that is not cached (a stub answered TRUE with the entry unwritten). The rest is still stubs (src/Windows/stubs/wininet.inc). Built freestanding over
 * kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

static BOOL not_cached(const void *url) {
  SetLastError(url ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_PARAMETER);
  return FALSE;
}

EXPORT BOOL WINAPI GetUrlCacheEntryInfoExW(LPCWSTR url, void *info, LPDWORD size, LPWSTR redirect, LPDWORD redirect_size, LPVOID reserved, DWORD flags) {
  (void)info; (void)size; (void)redirect; (void)redirect_size; (void)reserved; (void)flags;
  return not_cached(url);
}
EXPORT BOOL WINAPI GetUrlCacheEntryInfoExA(LPCSTR url, void *info, LPDWORD size, LPSTR redirect, LPDWORD redirect_size, LPVOID reserved, DWORD flags) {
  (void)info; (void)size; (void)redirect; (void)redirect_size; (void)reserved; (void)flags;
  return not_cached(url);
}
EXPORT BOOL WINAPI GetUrlCacheEntryInfoW(LPCWSTR url, void *info, LPDWORD size) {
  (void)info; (void)size;
  return not_cached(url);
}
EXPORT BOOL WINAPI GetUrlCacheEntryInfoA(LPCSTR url, void *info, LPDWORD size) {
  (void)info; (void)size;
  return not_cached(url);
}
