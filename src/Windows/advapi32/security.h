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
/* What the security part of advapi32.dll (security.c, token.c) needs on top of the mingw headers: the NT services and Rtl functions it calls, and the memory functions. */
#pragma once
#define _ADVAPI32_
#define _KERNEL32_
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>
#include <winsafer.h>
#include <ntsecapi.h>
#include <lmcons.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include "../ntsec.h"
void *NTAPI RtlAllocateHeap(void *heap, ULONG flags, SIZE_T size);
BOOLEAN NTAPI RtlFreeHeap(void *heap, ULONG flags, void *block);
void *NTAPI RtlReAllocateHeap(void *heap, ULONG flags, void *block, SIZE_T size);
void *NTAPI RtlProcessHeap(void);
#define malloc(n) RtlAllocateHeap(RtlProcessHeap(), 0, (n))
#define calloc(c, n) RtlAllocateHeap(RtlProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)(c) * (n))
#define free(p) RtlFreeHeap(RtlProcessHeap(), 0, (p))
#define wine_dbg_sprintf(...) ""
static inline WCHAR *strdupAW(const char *src)
{
    WCHAR *dst = NULL;
    if (src)
    {
        DWORD len = MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0);
        if ((dst = malloc(len * sizeof(WCHAR)))) MultiByteToWideChar(CP_ACP, 0, src, -1, dst, len);
    }
    return dst;
}
#define wcsicmp _wcsicmp
#define wcsnicmp _wcsnicmp
