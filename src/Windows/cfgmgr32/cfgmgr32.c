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

/* cfgmgr32.dll for TheoryCraftOS: the configuration manager's device tree. Programs see a tree of one node, the root ("HTREE\ROOT\0", devnode 1, started, no
 * children): CM_Locate_DevNode finds it for a NULL or empty instance ID or that name, and any other instance ID is CR_NO_SUCH_DEVNODE (the kernel's devices are not
 * offered as PnP device instances); walking up from the root or down into it finds nothing, and the root has no properties (CR_NO_SUCH_VALUE). A devnode other than
 * the root is CR_INVALID_DEVNODE. Outputs are zeroed on failure. The rest stays stubs (src/Windows/stubs/cfgmgr32.inc). Built freestanding over kernel32.dll and
 * ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#define _CFGMGR32_
#include <windows.h>
#include <cfgmgr32.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

#define ROOT_DEVNODE 1
static const WCHAR kRootId[] = L"HTREE\\ROOT\\0";
#define ROOT_ID_CHARS (sizeof kRootId / sizeof kRootId[0] - 1)

static WCHAR fold(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }
static BOOL is_root_id(const WCHAR *id) {
  if (!id || !*id) return TRUE;
  ULONG i = 0;
  for (; i < ROOT_ID_CHARS; ++i)
    if (fold(id[i]) != kRootId[i]) return FALSE;
  return id[i] == 0;
}

EXPORT CONFIGRET WINAPI CM_Locate_DevNodeW(PDEVINST devinst, DEVINSTID_W id, ULONG flags) {
  if (!devinst) return CR_INVALID_POINTER;
  *devinst = 0;
  if (flags & ~(ULONG)CM_LOCATE_DEVNODE_BITS) return CR_INVALID_FLAG;
  if (!is_root_id(id)) return CR_NO_SUCH_DEVNODE;
  *devinst = ROOT_DEVNODE;
  return CR_SUCCESS;
}

EXPORT CONFIGRET WINAPI CM_Locate_DevNodeA(PDEVINST devinst, DEVINSTID_A id, ULONG flags) {
  WCHAR wide[64];
  if (!id) return CM_Locate_DevNodeW(devinst, NULL, flags);
  ULONG i = 0;
  for (; id[i] && i < 63; ++i) wide[i] = (BYTE)id[i];
  wide[i] = 0;
  if (id[i]) { if (devinst) *devinst = 0; return devinst ? CR_NO_SUCH_DEVNODE : CR_INVALID_POINTER; }
  return CM_Locate_DevNodeW(devinst, wide, flags);
}

EXPORT CONFIGRET WINAPI CM_Get_Device_ID_Size(PULONG length, DEVINST devinst, ULONG flags) {
  (void)flags;
  if (!length) return CR_INVALID_POINTER;
  *length = 0;
  if (devinst != ROOT_DEVNODE) return CR_INVALID_DEVNODE;
  *length = ROOT_ID_CHARS;
  return CR_SUCCESS;
}

/* The instance ID; a buffer with room for the characters but not the terminator gets them without it (as on Windows), a smaller one CR_BUFFER_SMALL. */
EXPORT CONFIGRET WINAPI CM_Get_Device_IDW(DEVINST devinst, PWCHAR buffer, ULONG length, ULONG flags) {
  (void)flags;
  if (!buffer) return CR_INVALID_POINTER;
  if (length) buffer[0] = 0;
  if (devinst != ROOT_DEVNODE) return CR_INVALID_DEVNODE;
  if (length < ROOT_ID_CHARS) return CR_BUFFER_SMALL;
  for (ULONG i = 0; i < ROOT_ID_CHARS; ++i) buffer[i] = kRootId[i];
  if (length > ROOT_ID_CHARS) buffer[ROOT_ID_CHARS] = 0;
  return CR_SUCCESS;
}

static CONFIGRET relative(PDEVINST out, DEVINST devinst) {
  if (!out) return CR_INVALID_POINTER;
  *out = 0;
  return devinst == ROOT_DEVNODE ? CR_NO_SUCH_DEVNODE : CR_INVALID_DEVNODE;
}
EXPORT CONFIGRET WINAPI CM_Get_Parent(PDEVINST parent, DEVINST devinst, ULONG flags) { (void)flags; return relative(parent, devinst); }
EXPORT CONFIGRET WINAPI CM_Get_Child(PDEVINST child, DEVINST devinst, ULONG flags) { (void)flags; return relative(child, devinst); }
EXPORT CONFIGRET WINAPI CM_Get_Sibling(PDEVINST sibling, DEVINST devinst, ULONG flags) { (void)flags; return relative(sibling, devinst); }

EXPORT CONFIGRET WINAPI CM_Get_DevNode_Status(PULONG status, PULONG problem, DEVINST devinst, ULONG flags) {
  (void)flags;
  if (!status || !problem) return CR_INVALID_POINTER;
  *status = 0;
  *problem = 0;
  if (devinst != ROOT_DEVNODE) return CR_INVALID_DEVNODE;
  *status = DN_ROOT_ENUMERATED | DN_DRIVER_LOADED | DN_STARTED | DN_NT_ENUMERATOR | DN_NT_DRIVER;
  return CR_SUCCESS;
}

EXPORT CONFIGRET WINAPI CM_Get_DevNode_PropertyW(DEVINST devinst, const DEVPROPKEY *key, DEVPROPTYPE *type, PBYTE buffer, PULONG size, ULONG flags) {
  (void)buffer;
  if (!key || !type || !size) return CR_INVALID_POINTER;
  *type = 0;
  if (flags) { *size = 0; return CR_INVALID_FLAG; }
  *size = 0;
  if (devinst != ROOT_DEVNODE) return CR_INVALID_DEVNODE;
  return CR_NO_SUCH_VALUE;
}
