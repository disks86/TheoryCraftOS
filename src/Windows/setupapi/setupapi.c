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

/* setupapi.dll for TheoryCraftOS: the device installer's enumeration functions. Programs here see no devices through the PnP manager (the kernel's devices are not
 * offered as device instances or interfaces), so a device information set is real - SetupDiGetClassDevs and SetupDiCreateDeviceInfoList give a handle that stays
 * valid until SetupDiDestroyDeviceInfoList - but always empty: enumerating ends at once with ERROR_NO_MORE_ITEMS, opening an instance or an interface by name fails
 * with ERROR_NO_SUCH_DEVINST / ERROR_NO_SUCH_DEVICE_INTERFACE, and asking about a device the set does not hold fails with ERROR_INVALID_PARAMETER. The rest of setupapi
 * (INF files, queues, class installers) stays stubs (src/Windows/stubs/setupapi.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#define _SETUPAPI_
#include <windows.h>
#include <setupapi.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

/* ------------------------------------------------------------------------------------------------------------------------- sets */

/* A set is HANDLE_BASE + its slot in this table; a made-up value or a destroyed set is refused. */
#define MAX_SETS 512
#define HANDLE_BASE 0x53440000u
static BYTE g_used[MAX_SETS];
static GUID g_class[MAX_SETS];
static SRWLOCK g_lock = SRWLOCK_INIT;

static HDEVINFO set_new(const GUID *class_guid) {
  AcquireSRWLockExclusive(&g_lock);
  UINT slot = 1;
  while (slot < MAX_SETS && g_used[slot]) ++slot;
  if (slot == MAX_SETS) { ReleaseSRWLockExclusive(&g_lock); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  g_used[slot] = 1;
  if (class_guid) g_class[slot] = *class_guid;
  else { static const GUID none; g_class[slot] = none; }
  ReleaseSRWLockExclusive(&g_lock);
  SetLastError(ERROR_SUCCESS);
  return (HDEVINFO)(ULONG_PTR)(HANDLE_BASE + slot);
}

/* The slot of a set, or 0 with ERROR_INVALID_HANDLE. */
static UINT set_slot(HDEVINFO set) {
  const ULONG_PTR h = (ULONG_PTR)set;
  if (h <= HANDLE_BASE || h >= HANDLE_BASE + MAX_SETS) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  const UINT slot = (UINT)(h - HANDLE_BASE);
  AcquireSRWLockShared(&g_lock);
  const BYTE used = g_used[slot];
  ReleaseSRWLockShared(&g_lock);
  if (!used) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  return slot;
}

#define FLAGS_KNOWN (DIGCF_DEFAULT | DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_PROFILE | DIGCF_DEVICEINTERFACE)

EXPORT HDEVINFO WINAPI SetupDiCreateDeviceInfoList(const GUID *class_guid, HWND parent) {
  (void)parent;
  return set_new(class_guid);
}

EXPORT HDEVINFO WINAPI SetupDiCreateDeviceInfoListExW(const GUID *class_guid, HWND parent, PCWSTR machine, PVOID reserved) {
  if (reserved) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if (machine && *machine) { SetLastError(ERROR_INVALID_MACHINENAME); return INVALID_HANDLE_VALUE; }
  return SetupDiCreateDeviceInfoList(class_guid, parent);
}

EXPORT HDEVINFO WINAPI SetupDiGetClassDevsExW(const GUID *class_guid, PCWSTR enumerator, HWND parent, DWORD flags, HDEVINFO set, PCWSTR machine, PVOID reserved) {
  (void)enumerator;
  (void)parent;
  if (reserved || (flags & ~(DWORD)FLAGS_KNOWN)) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if (!class_guid && !(flags & DIGCF_ALLCLASSES)) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if (machine && *machine) { SetLastError(ERROR_INVALID_MACHINENAME); return INVALID_HANDLE_VALUE; }
  if (set && set != INVALID_HANDLE_VALUE) {      /* (the devices found are added to this set: there are none) */
    if (!set_slot(set)) return INVALID_HANDLE_VALUE;
    SetLastError(ERROR_SUCCESS);
    return set;
  }
  return set_new(flags & DIGCF_DEVICEINTERFACE ? NULL : class_guid);
}

EXPORT HDEVINFO WINAPI SetupDiGetClassDevsW(const GUID *class_guid, PCWSTR enumerator, HWND parent, DWORD flags) {
  return SetupDiGetClassDevsExW(class_guid, enumerator, parent, flags, NULL, NULL, NULL);
}

EXPORT HDEVINFO WINAPI SetupDiGetClassDevsA(const GUID *class_guid, PCSTR enumerator, HWND parent, DWORD flags) {
  (void)enumerator;
  return SetupDiGetClassDevsExW(class_guid, NULL, parent, flags, NULL, NULL, NULL);
}

EXPORT WINBOOL WINAPI SetupDiDestroyDeviceInfoList(HDEVINFO set) {
  const UINT slot = set_slot(set);
  if (!slot) return FALSE;
  AcquireSRWLockExclusive(&g_lock);
  g_used[slot] = 0;
  ReleaseSRWLockExclusive(&g_lock);
  SetLastError(ERROR_SUCCESS);
  return TRUE;
}

/* ------------------------------------------------------------------------------------------------------------------------- the (empty) contents */

EXPORT WINBOOL WINAPI SetupDiEnumDeviceInfo(HDEVINFO set, DWORD index, PSP_DEVINFO_DATA info) {
  (void)index;
  if (!set_slot(set)) return FALSE;
  if (!info || info->cbSize != sizeof(SP_DEVINFO_DATA)) { SetLastError(ERROR_INVALID_USER_BUFFER); return FALSE; }
  SetLastError(ERROR_NO_MORE_ITEMS);
  return FALSE;
}

EXPORT WINBOOL WINAPI SetupDiEnumDeviceInterfaces(HDEVINFO set, PSP_DEVINFO_DATA info, const GUID *interface_class, DWORD index, PSP_DEVICE_INTERFACE_DATA data) {
  (void)index;
  if (!set_slot(set)) return FALSE;
  if (!interface_class) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!data || data->cbSize != sizeof(SP_DEVICE_INTERFACE_DATA)) { SetLastError(ERROR_INVALID_USER_BUFFER); return FALSE; }
  if (info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }      /* (a device of this set: there is none) */
  SetLastError(ERROR_NO_MORE_ITEMS);
  return FALSE;
}

/* An interface or device that a program passes in cannot be one of this (empty) set. */
static WINBOOL not_in_set(HDEVINFO set) {
  if (!set_slot(set)) return FALSE;
  SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}

EXPORT WINBOOL WINAPI SetupDiGetDeviceInterfaceDetailW(HDEVINFO set, PSP_DEVICE_INTERFACE_DATA data, PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail, DWORD size,
                                                       PDWORD required, PSP_DEVINFO_DATA info) {
  (void)data; (void)detail; (void)size; (void)info;
  if (required) *required = 0;
  return not_in_set(set);
}

EXPORT WINBOOL WINAPI SetupDiGetDeviceInterfaceDetailA(HDEVINFO set, PSP_DEVICE_INTERFACE_DATA data, PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail, DWORD size,
                                                       PDWORD required, PSP_DEVINFO_DATA info) {
  (void)data; (void)detail; (void)size; (void)info;
  if (required) *required = 0;
  return not_in_set(set);
}

EXPORT WINBOOL WINAPI SetupDiGetDevicePropertyW(HDEVINFO set, PSP_DEVINFO_DATA info, const DEVPROPKEY *key, DEVPROPTYPE *type, PBYTE buffer, DWORD size,
                                                PDWORD required, DWORD flags) {
  (void)info; (void)key; (void)buffer; (void)size; (void)flags;
  if (type) *type = 0;
  if (required) *required = 0;
  return not_in_set(set);
}

EXPORT WINBOOL WINAPI SetupDiGetDeviceRegistryPropertyW(HDEVINFO set, PSP_DEVINFO_DATA info, DWORD property, PDWORD type, PBYTE buffer, DWORD size, PDWORD required) {
  (void)info; (void)property; (void)buffer; (void)size;
  if (type) *type = 0;
  if (required) *required = 0;
  return not_in_set(set);
}

EXPORT WINBOOL WINAPI SetupDiGetDeviceRegistryPropertyA(HDEVINFO set, PSP_DEVINFO_DATA info, DWORD property, PDWORD type, PBYTE buffer, DWORD size, PDWORD required) {
  return SetupDiGetDeviceRegistryPropertyW(set, info, property, type, buffer, size, required);
}

EXPORT WINBOOL WINAPI SetupDiGetDeviceInstanceIdW(HDEVINFO set, PSP_DEVINFO_DATA info, PWSTR id, DWORD size, PDWORD required) {
  (void)info;
  if (id && size) id[0] = 0;
  if (required) *required = 0;
  return not_in_set(set);
}

EXPORT WINBOOL WINAPI SetupDiOpenDeviceInfoW(HDEVINFO set, PCWSTR instance_id, HWND parent, DWORD flags, PSP_DEVINFO_DATA info) {
  (void)parent; (void)flags; (void)info;
  if (!set_slot(set)) return FALSE;
  if (!instance_id) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  SetLastError(ERROR_NO_SUCH_DEVINST);
  return FALSE;
}

EXPORT WINBOOL WINAPI SetupDiOpenDeviceInterfaceW(HDEVINFO set, PCWSTR path, DWORD flags, PSP_DEVICE_INTERFACE_DATA data) {
  (void)flags; (void)data;
  if (!set_slot(set)) return FALSE;
  if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  SetLastError(ERROR_NO_SUCH_DEVICE_INTERFACE);
  return FALSE;
}

EXPORT HKEY WINAPI SetupDiOpenDevRegKey(HDEVINFO set, PSP_DEVINFO_DATA info, DWORD scope, DWORD profile, DWORD key_type, REGSAM access) {
  (void)info; (void)scope; (void)profile; (void)key_type; (void)access;
  if (set_slot(set)) SetLastError(ERROR_NO_SUCH_DEVINST);
  return (HKEY)INVALID_HANDLE_VALUE;
}
