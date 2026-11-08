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

/* cryptui.dll for TheoryCraftOS: the certificate dialogs. There are no such dialogs here: they fail with ERROR_NOT_SUPPORTED (and nothing was changed). Built
 * freestanding over kernel32.dll and ntdll.dll. */
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

EXPORT BOOL WINAPI CryptUIDlgCertMgr(const void *info) {
  (void)info;
  SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}
EXPORT BOOL WINAPI CryptUIDlgViewCertificateW(const void *info, BOOL *properties_changed) {
  (void)info;
  if (properties_changed) *properties_changed = FALSE;
  SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}
EXPORT BOOL WINAPI CryptUIDlgViewCertificateA(const void *info, BOOL *properties_changed) { return CryptUIDlgViewCertificateW(info, properties_changed); }
