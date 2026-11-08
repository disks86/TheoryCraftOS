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

/* dcomp.dll for TheoryCraftOS: DirectComposition. There is no composition engine here: a device cannot be made (E_NOTIMPL, NULL), so a program composes its windows
 * the plain way (Chrome falls back from DirectComposition to presenting through its swap chain). Built freestanding over kernel32.dll and ntdll.dll. */
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

EXPORT HRESULT WINAPI DCompositionCreateDevice(void *dxgi_device, const GUID *iid, void **device) { (void)dxgi_device; (void)iid; if (device) *device = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI DCompositionCreateDevice2(void *rendering_device, const GUID *iid, void **device) { (void)rendering_device; (void)iid; if (device) *device = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI DCompositionCreateDevice3(void *rendering_device, const GUID *iid, void **device) { (void)rendering_device; (void)iid; if (device) *device = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI DCompositionCreateSurfaceHandle(DWORD access, SECURITY_ATTRIBUTES *attributes, HANDLE *surface) {
  (void)access; (void)attributes;
  if (surface) *surface = NULL;
  return E_NOTIMPL;
}
