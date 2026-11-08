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

/* dhcpcsvc.dll for TheoryCraftOS: the DHCP client's API. The network stack keeps its DHCP state in the kernel and does not hand out options, so the requests of
 * programs (Chrome asks for option 252, the proxy auto-configuration URL) fail with ERROR_NOT_SUPPORTED and leave the caller's buffers alone; a browser then goes on
 * without a DHCP-supplied PAC file. Built freestanding over kernel32.dll and ntdll.dll. */
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

EXPORT DWORD WINAPI DhcpCApiInitialize(LPDWORD version) {
  if (version) *version = 0;
  return ERROR_NOT_SUPPORTED;
}
EXPORT void WINAPI DhcpCApiCleanup(void) {}

/* (DHCPCAPI_PARAMS_ARRAY is passed by value, which the x64 convention turns into a pointer.) */
EXPORT DWORD WINAPI DhcpRequestParams(DWORD flags, void *reserved, LPWSTR adapter, void *class_id, const void *send, const void *receive, LPBYTE buffer, LPDWORD size,
                                      LPWSTR request_id) {
  (void)flags; (void)reserved; (void)adapter; (void)class_id; (void)send; (void)receive; (void)buffer; (void)size; (void)request_id;
  return ERROR_NOT_SUPPORTED;
}
EXPORT DWORD WINAPI DhcpUndoRequestParams(DWORD flags, void *reserved, LPWSTR adapter, LPWSTR request_id) {
  (void)flags; (void)reserved; (void)adapter; (void)request_id;
  return ERROR_NOT_SUPPORTED;
}
