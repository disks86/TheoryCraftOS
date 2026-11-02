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
/* Declarations for the security part of ntdll.dll (security.c) and its users, on top of the mingw headers. */
#pragma once
#define NTSEC_EXPORT __declspec(dllexport)

#include <windows.h>

#ifndef SE_SELF_RELATIVE
#define SE_SELF_RELATIVE 0x8000
#endif
void *NTAPI RtlProcessHeap(void);
void *NTAPI RtlAllocateHeap(void *heap, ULONG flags, SIZE_T size);
BOOLEAN NTAPI RtlFreeHeap(void *heap, ULONG flags, void *block);

#include "../ntsec.h"
#define NtCurrentProcess() ((HANDLE)(LONG_PTR)-1)
#define GetCurrentProcess() ((HANDLE)(LONG_PTR)-1)
#define GetCurrentThread() ((HANDLE)(LONG_PTR)-2)

