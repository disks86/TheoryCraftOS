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

/* ndfapi.dll for TheoryCraftOS: the Network Diagnostics Framework. There are no diagnostics helpers here: an incident cannot be made (E_NOTIMPL, NULL handle), so
 * the "diagnose" button of a browser's error page does nothing. Built freestanding over kernel32.dll and ntdll.dll. */
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

EXPORT HRESULT WINAPI NdfCreateWebIncident(LPCWSTR url, void **handle) { (void)url; if (handle) *handle = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI NdfCreateWebIncidentEx(LPCWSTR url, BOOL use_winhttp, LPWSTR module, void **handle) { (void)url; (void)use_winhttp; (void)module; if (handle) *handle = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI NdfExecuteDiagnosis(void *handle, HWND window) { (void)handle; (void)window; return E_HANDLE; }
EXPORT HRESULT WINAPI NdfCloseIncident(void *handle) { (void)handle; return E_HANDLE; }
