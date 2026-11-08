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

/* urlmon.dll for TheoryCraftOS: there are no Internet Explorer security zones, so the zone and security managers cannot be created (E_NOTIMPL, the out-pointer NULL;
 * a stub answered S_OK with the pointer unset). The rest is still stubs (src/Windows/stubs/urlmon.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define E_NOTIMPL_ ((HRESULT)0x80004001)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

EXPORT HRESULT WINAPI CoInternetCreateSecurityManager(void *provider, void **manager, DWORD reserved) {
  (void)provider; (void)reserved;
  if (manager) *manager = NULL;
  return E_NOTIMPL_;
}
EXPORT HRESULT WINAPI CoInternetCreateZoneManager(void *provider, void **manager, DWORD reserved) {
  (void)provider; (void)reserved;
  if (manager) *manager = NULL;
  return E_NOTIMPL_;
}
