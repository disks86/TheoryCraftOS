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

/* mf.dll for TheoryCraftOS: the Media Foundation pipeline. Only the capture-device calls a browser makes are here: there are no capture devices (MFEnumDeviceSources
 * gives an empty list) and no source can be made (MFCreateDeviceSource fails with E_NOTIMPL and a NULL source). Built freestanding over kernel32.dll and ntdll.dll. */
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

/* The capture devices: none. The list is NULL and the count 0, which CoTaskMemFree(list) accepts. */
EXPORT HRESULT WINAPI MFEnumDeviceSources(void *attributes, void ***sources, UINT32 *count) {
  (void)attributes;
  if (!sources || !count) return E_POINTER;
  *sources = NULL;
  *count = 0;
  return S_OK;
}

EXPORT HRESULT WINAPI MFCreateDeviceSource(void *attributes, void **source) {
  (void)attributes;
  if (source) *source = NULL;
  return E_NOTIMPL;
}
