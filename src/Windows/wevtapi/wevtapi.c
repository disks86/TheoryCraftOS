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

/* wevtapi.dll for TheoryCraftOS: the Windows Event Log reader. There are no event log channels to read (the classic event log of advapi32 is separate), so a query finds
 * no channel (ERROR_EVT_CHANNEL_NOT_FOUND), and the calls on result sets, events and render contexts - none of which can exist - fail with ERROR_INVALID_HANDLE after
 * clearing their out-parameters. The rest is still stubs (src/Windows/stubs/wevtapi.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define ERROR_EVT_CHANNEL_NOT_FOUND_ 15007

typedef HANDLE EVT_HANDLE_;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

EXPORT EVT_HANDLE_ WINAPI EvtQuery(EVT_HANDLE_ session, LPCWSTR path, LPCWSTR query, DWORD flags) {
  (void)query; (void)flags;
  if (session) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }      /* (remote sessions: none can be opened) */
  (void)path;
  SetLastError(ERROR_EVT_CHANNEL_NOT_FOUND_);
  return NULL;
}

EXPORT BOOL WINAPI EvtNext(EVT_HANDLE_ results, DWORD count, EVT_HANDLE_ *events, DWORD timeout, DWORD flags, PDWORD returned) {
  (void)results; (void)timeout; (void)flags;
  if (returned) *returned = 0;
  if (events) for (DWORD i = 0; i < count; ++i) events[i] = NULL;
  SetLastError(ERROR_INVALID_HANDLE);
  return FALSE;
}

EXPORT BOOL WINAPI EvtRender(EVT_HANDLE_ context, EVT_HANDLE_ fragment, DWORD flags, DWORD size, PVOID buffer, PDWORD used, PDWORD properties) {
  (void)context; (void)fragment; (void)flags; (void)size; (void)buffer;
  if (used) *used = 0;
  if (properties) *properties = 0;
  SetLastError(ERROR_INVALID_HANDLE);
  return FALSE;
}

/* A render context needs the publisher metadata of the event system: not available. */
EXPORT EVT_HANDLE_ WINAPI EvtCreateRenderContext(DWORD count, LPCWSTR *paths, DWORD flags) {
  (void)count; (void)paths; (void)flags;
  SetLastError(ERROR_NOT_SUPPORTED);
  return NULL;
}

/* No handle of this library exists, so there is nothing to free; closing NULL is refused as on Windows. */
EXPORT BOOL WINAPI EvtClose(EVT_HANDLE_ object) {
  if (!object) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  return TRUE;
}
