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

/* mmdevapi.dll for TheoryCraftOS: the audio endpoint API. Only ActivateAudioInterfaceAsync is here (programs import it by its ordinal, 17: see mmdevapi.def), and it
 * fails with E_NOTIMPL and no operation: the endpoints are reached through the device enumerator of COM instead. Built freestanding over kernel32.dll and ntdll.dll. */
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

EXPORT HRESULT WINAPI ActivateAudioInterfaceAsync(LPCWSTR device, const GUID *iid, const void *parameters, void *handler, void **operation) {
  (void)device; (void)iid; (void)parameters; (void)handler;
  if (operation) *operation = NULL;
  return E_NOTIMPL;
}
