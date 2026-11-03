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

/* ole32.dll for TheoryCraftOS: COM (apartments, class registration and activation, in-process marshaling, monikers and the running object table), structured storage
 * (compound files), property sets, the OLE helpers (clipboard, drag and drop, data cache, advise holders) and the user-marshaling routines.
 *
 * Design. All of it runs inside one process: an object that a thread of the process creates is used by every other thread of the process through the same pointer, so the
 * "standard marshaler" turns an interface into an OBJREF (the Windows format) that names an entry of a table in this process, and unmarshaling finds the entry again. A
 * server in another process is not reachable: RPC is not part of this library. Behaviour checked with Wine's tests (tests/windows/wine/ole32). The Windows headers
 * of mingw supply the interface definitions; this file is the only one that defines the functions they declare, so they are not imports. */
#define INITGUID
#define _OLE32_
#define _KERNEL32_
#define _ADVAPI32_
#define _USER32_
#define _OLEAUT32_
#define _GDI32_
#define COBJMACROS
#define CONST_VTABLE
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define OEMRESOURCE
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <oleidl.h>
#include <propidl.h>
#include <oaidl.h>
#include <urlmon.h>
#include <docobj.h>
#include <ctxtcall.h>
#include <callobj.h>

#pragma GCC diagnostic ignored "-Wmisleading-indentation"
typedef int PROPVAR_CHANGE_FLAGS;

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define EXPORT __declspec(dllexport)

#include "core.inc"
#include "stream.inc"
#include "compobj.inc"
#include "marshal.inc"
#include "errorinfo.inc"
#include "propvariant.inc"
#include "storage.inc"
#include "propstg.inc"
#include "moniker.inc"
#include "ole.inc"
#include "olecreate.inc"
#include "usrmarshal.inc"
#include "builtin.inc"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module;
  (void)reserved;
  switch (reason) {
    case DLL_PROCESS_ATTACH: core_init(); break;
    case DLL_THREAD_DETACH: thread_detach(); break;
    default: break;
  }
  return TRUE;
}
