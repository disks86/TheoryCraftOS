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

/* bthprops.dll (bthprops.cpl, which the loader folds to it) for TheoryCraftOS: the classic Bluetooth API. The Bluetooth stack of this OS does not reach user mode
 * yet, so there are no radios and no devices: the searches end at once with ERROR_NO_MORE_ITEMS and a NULL handle, the calls on a radio or search handle fail as for
 * a handle that does not exist, and the SDP record parsers have no record to read. Built freestanding over kernel32.dll and ntdll.dll. */
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

static BOOL fail(DWORD error) {
  SetLastError(error);
  return FALSE;
}

EXPORT HANDLE WINAPI BluetoothFindFirstRadio(const void *parameters, HANDLE *radio) {
  if (radio) *radio = NULL;
  SetLastError(parameters && radio ? ERROR_NO_MORE_ITEMS : ERROR_INVALID_PARAMETER);
  return NULL;
}
EXPORT BOOL WINAPI BluetoothFindNextRadio(HANDLE find, HANDLE *radio) { (void)find; if (radio) *radio = NULL; return fail(ERROR_INVALID_HANDLE); }
EXPORT BOOL WINAPI BluetoothFindRadioClose(HANDLE find) { (void)find; return fail(ERROR_INVALID_HANDLE); }

EXPORT HANDLE WINAPI BluetoothFindFirstDevice(const void *parameters, void *device_info) {
  (void)device_info;
  SetLastError(parameters ? ERROR_NO_MORE_ITEMS : ERROR_INVALID_PARAMETER);
  return NULL;
}
EXPORT BOOL WINAPI BluetoothFindNextDevice(HANDLE find, void *device_info) { (void)find; (void)device_info; return fail(ERROR_INVALID_HANDLE); }
EXPORT BOOL WINAPI BluetoothFindDeviceClose(HANDLE find) { (void)find; return fail(ERROR_INVALID_HANDLE); }

EXPORT DWORD WINAPI BluetoothGetRadioInfo(HANDLE radio, void *info) { (void)radio; (void)info; return ERROR_INVALID_PARAMETER; }
EXPORT BOOL WINAPI BluetoothIsConnectable(HANDLE radio) { (void)radio; return FALSE; }
EXPORT BOOL WINAPI BluetoothIsDiscoverable(HANDLE radio) { (void)radio; return FALSE; }
EXPORT BOOL WINAPI BluetoothEnableDiscovery(HANDLE radio, BOOL enabled) { (void)radio; (void)enabled; return FALSE; }
EXPORT BOOL WINAPI BluetoothEnableIncomingConnections(HANDLE radio, BOOL enabled) { (void)radio; (void)enabled; return FALSE; }

/* SDP: records only come from devices, of which there are none. */
EXPORT DWORD WINAPI BluetoothSdpGetAttributeValue(BYTE *record, ULONG record_size, USHORT attribute, void *data) {
  (void)record; (void)record_size; (void)attribute; (void)data;
  return ERROR_INVALID_PARAMETER;
}
EXPORT DWORD WINAPI BluetoothSdpGetContainerElementData(BYTE *container, ULONG container_size, HANDLE *element, void *data) {
  (void)container; (void)container_size; (void)data;
  if (element) *element = NULL;
  return ERROR_INVALID_PARAMETER;
}
