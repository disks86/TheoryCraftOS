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

/* rpcrt4.dll for TheoryCraftOS: UUIDs. UuidCreate makes random (version 4) UUIDs from the system's random source (NtQuerySystemInformation class 0x1000, the one
 * BCryptGenRandom reads), as Windows does since 2000; UuidCreateSequential gives the same kind, since there is no network card address to build a version 1 UUID from
 * (Windows answers RPC_S_UUID_LOCAL_ONLY then: here too). The nil / compare helpers are here as well. RPC itself is still stubs (src/Windows/stubs/rpcrt4.inc).
 * Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define RPC_S_OK_ 0
#define RPC_S_UUID_LOCAL_ONLY_ 1824
#define RPC_S_UUID_NO_ADDRESS_ 1739
#define RPC_S_INVALID_ARG_ 87

typedef GUID Uuid;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

static LONG random_uuid(Uuid *uuid) {
  if (!uuid) return RPC_S_INVALID_ARG_;
  if (NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)0x1000, uuid, sizeof *uuid, NULL) < 0) {
    BYTE *p = (BYTE *)uuid;
    for (SIZE_T i = 0; i < sizeof *uuid; ++i) p[i] = 0;
    return RPC_S_UUID_NO_ADDRESS_;
  }
  uuid->Data3 = (WORD)((uuid->Data3 & 0x0FFF) | 0x4000);      /* version 4 */
  uuid->Data4[0] = (BYTE)((uuid->Data4[0] & 0x3F) | 0x80);      /* the RFC 4122 variant */
  return RPC_S_OK_;
}

EXPORT LONG WINAPI UuidCreate(Uuid *uuid) { return random_uuid(uuid); }
EXPORT LONG WINAPI UuidCreateSequential(Uuid *uuid) {
  const LONG r = random_uuid(uuid);
  return r == RPC_S_OK_ ? RPC_S_UUID_LOCAL_ONLY_ : r;
}

EXPORT LONG WINAPI UuidCreateNil(Uuid *uuid) {
  if (!uuid) return RPC_S_INVALID_ARG_;
  BYTE *p = (BYTE *)uuid;
  for (SIZE_T i = 0; i < sizeof *uuid; ++i) p[i] = 0;
  return RPC_S_OK_;
}

/* UuidCompare: -1, 0, 1 by the fields in order (a NULL UUID is the nil UUID). */
EXPORT int WINAPI UuidCompare(const Uuid *a, const Uuid *b, LONG *status) {
  static const Uuid nil;
  if (status) *status = RPC_S_OK_;
  if (!a) a = &nil;
  if (!b) b = &nil;
  if (a->Data1 != b->Data1) return a->Data1 < b->Data1 ? -1 : 1;
  if (a->Data2 != b->Data2) return a->Data2 < b->Data2 ? -1 : 1;
  if (a->Data3 != b->Data3) return a->Data3 < b->Data3 ? -1 : 1;
  for (int i = 0; i < 8; ++i)
    if (a->Data4[i] != b->Data4[i]) return a->Data4[i] < b->Data4[i] ? -1 : 1;
  return 0;
}
EXPORT int WINAPI UuidEqual(const Uuid *a, const Uuid *b, LONG *status) { return UuidCompare(a, b, status) == 0; }
EXPORT int WINAPI UuidIsNil(const Uuid *uuid, LONG *status) { return UuidCompare(uuid, NULL, status) == 0; }
