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

/* uxtheme.dll for TheoryCraftOS: visual styles are off. Windows with theming off answers OpenThemeData with NULL (the error E_PROP_ID_UNSUPPORTED), so programs draw the
 * classic look themselves; every call on a theme handle then fails with E_HANDLE and clears what it would have filled in. IsThemeActive / IsAppThemed say FALSE.
 * The rest of the library is still stubs (src/Windows/stubs/uxtheme.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define E_HANDLE_ ((HRESULT)0x80070006)
#define E_PROP_ID_UNSUPPORTED_ ((HRESULT)0x80070490)

typedef HANDLE HTHEME_;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

EXPORT BOOL WINAPI IsThemeActive(void) { return FALSE; }
EXPORT BOOL WINAPI IsAppThemed(void) { return FALSE; }

EXPORT HTHEME_ WINAPI OpenThemeData(HWND window, LPCWSTR classes) {
  (void)window;
  (void)classes;
  SetLastError((DWORD)E_PROP_ID_UNSUPPORTED_);
  return NULL;
}
EXPORT HTHEME_ WINAPI OpenThemeDataEx(HWND window, LPCWSTR classes, DWORD flags) {
  (void)flags;
  return OpenThemeData(window, classes);
}
EXPORT HTHEME_ WINAPI OpenThemeDataForDpi(HWND window, LPCWSTR classes, UINT dpi) {
  (void)dpi;
  return OpenThemeData(window, classes);
}

/* No theme handle is ever given out. */
EXPORT HRESULT WINAPI CloseThemeData(HTHEME_ theme) { (void)theme; return E_HANDLE_; }
EXPORT HRESULT WINAPI DrawThemeBackground(HTHEME_ theme, HDC dc, int part, int state, const RECT *rect, const RECT *clip) {
  (void)theme; (void)dc; (void)part; (void)state; (void)rect; (void)clip;
  return E_HANDLE_;
}
EXPORT HRESULT WINAPI GetThemeBackgroundContentRect(HTHEME_ theme, HDC dc, int part, int state, const RECT *bounds, RECT *content) {
  (void)theme; (void)dc; (void)part; (void)state; (void)bounds;
  if (content) content->left = content->top = content->right = content->bottom = 0;
  return E_HANDLE_;
}
EXPORT HRESULT WINAPI GetThemePartSize(HTHEME_ theme, HDC dc, int part, int state, const RECT *rect, int size_kind, SIZE *size) {
  (void)theme; (void)dc; (void)part; (void)state; (void)rect; (void)size_kind;
  if (size) size->cx = size->cy = 0;
  return E_HANDLE_;
}
