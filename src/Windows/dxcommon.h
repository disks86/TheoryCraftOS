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

/* What dxgi.dll and d3d12.dll share: the C library functions a freestanding DLL has to bring along, memory, and the Direct3D kernel-mode thunks (D3DKMT*, gdi32.dll -> win32u.dll ->
 * src/Process/Win32k/Dxg) with the user-mode structures of the Windows x64 layout written by offset (the offsets are those of src/Process/Win32k/Dxg/DxgLayouts.hpp).
 *
 * Everything is static: each DLL has its own copy. */
#ifndef TCOS_DXCOMMON_H
#define TCOS_DXCOMMON_H

#include <windows.h>
#include <winternl.h>
#include <unknwn.h>

#define EXPORT __declspec(dllexport)

#ifndef TCOS_NO_LIBC
void *memcpy(void *dest, const void *source, size_t n) {
  unsigned char *d = dest;
  const unsigned char *s = source;
  while (n--) *d++ = *s++;
  return dest;
}
void *memmove(void *dest, const void *source, size_t n) {
  unsigned char *d = dest;
  const unsigned char *s = source;
  if (d < s) { while (n--) *d++ = *s++; }
  else { d += n; s += n; while (n--) *--d = *--s; }
  return dest;
}
void *memset(void *dest, int value, size_t n) {
  unsigned char *d = dest;
  while (n--) *d++ = (unsigned char)value;
  return dest;
}
int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (; n; --n, ++x, ++y)
    if (*x != *y) return *x < *y ? -1 : 1;
  return 0;
}
#endif

static void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static void *mem_grow(void *p, SIZE_T n) { return p ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n) : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static SIZE_T wlen(const WCHAR *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }
static SIZE_T alen(const char *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }
static int guid_eq(const GUID *a, const GUID *b) { return memcmp(a, b, sizeof(GUID)) == 0; }

/* What a program asked of these libraries that they do not have: a method nobody wrote, an interface no object answers. Said once each to the debug channel
 * (NtTcosDebugException with DBG_PRINTEXCEPTION_C, which the kernel's service trace prints as "debug text" whether or not a debugger is attached), so a program
 * that gives up, such as Chrome's GPU process deciding a device is not good enough, shows what it missed. */
static void dx_say(const char *text) {
  typedef LONG (NTAPI *DebugExceptionFn)(EXCEPTION_RECORD *, BOOLEAN, void *);
  static DebugExceptionFn report;
  if (!report) report = (DebugExceptionFn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtTcosDebugException");
  if (!report) return;
  EXCEPTION_RECORD record;
  memset(&record, 0, sizeof record);
  record.ExceptionCode = 0x40010006u;      /* DBG_PRINTEXCEPTION_C */
  record.NumberParameters = 2;
  SIZE_T n = 0;
  while (text[n]) ++n;
  record.ExceptionInformation[0] = n + 1;
  record.ExceptionInformation[1] = (ULONG_PTR)text;
  report(&record, TRUE, NULL);
}
static void dx_unimplemented(volatile LONG *said, const char *what) {
  if (InterlockedExchange((LONG *)said, 1)) return;
  char text[160] = "dx: not implemented: ";
  SIZE_T n = 21;
  for (SIZE_T i = 0; what[i] && n + 1 < sizeof text; ++i) text[n++] = what[i];
  text[n] = 0;
  dx_say(text);
}
/* A method that failed: its first few failures each, with the HRESULT. */
static void dx_failed(volatile LONG *said, const char *what, HRESULT hr) {
  if (InterlockedIncrement((LONG *)said) > 4) return;
  static const char hex[] = "0123456789abcdef";
  char text[160] = "dx: failed: ";
  SIZE_T n = 12;
  for (SIZE_T i = 0; what[i] && n + 12 < sizeof text; ++i) text[n++] = what[i];
  text[n++] = ' ';
  for (int shift = 28; shift >= 0; shift -= 4) text[n++] = hex[((ULONG)hr >> shift) & 15];
  text[n] = 0;
  dx_say(text);
}
static void dx_no_interface(const char *object, const GUID *g) {
  static const char hex[] = "0123456789abcdef";
  char text[160] = "dx: no interface ";
  SIZE_T n = 17;
  const BYTE *b = (const BYTE *)g;
  const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text[n++] = '-';
    text[n++] = hex[b[order[i]] >> 4];
    text[n++] = hex[b[order[i]] & 15];
  }
  text[n++] = ' ';
  for (SIZE_T i = 0; object[i] && n + 1 < sizeof text; ++i) text[n++] = object[i];
  text[n] = 0;
  dx_say(text);
}

/* ---- fields of the structures of the thunks, by offset --------------------------------------------------------------------------------------------------------------------- */

#define KPUT32(block, offset, value) (*(UINT32 *)((BYTE *)(block) + (offset)) = (UINT32)(value))
#define KPUT64(block, offset, value) (*(UINT64 *)((BYTE *)(block) + (offset)) = (UINT64)(value))
#define KGET32(block, offset) (*(UINT32 *)((BYTE *)(block) + (offset)))
#define KGET64(block, offset) (*(UINT64 *)((BYTE *)(block) + (offset)))

/* In a 32-bit program the D3DKMT functions of gdi32.dll take the structures as a 32-bit program lays them out (gdi32 converts the ones a program
 * calls itself, src/Windows/gdi32/wow64.inc). These libraries write the kernel's own layout - the same for both widths, by offset - so there they call the
 * services of win32u.dll, which take it as it is. */
#ifdef __i386__
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIEnumAdapters2(void *);
#define D3DKMTEnumAdapters2 NtGdiDdDDIEnumAdapters2
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIOpenAdapterFromLuid(void *);
#define D3DKMTOpenAdapterFromLuid NtGdiDdDDIOpenAdapterFromLuid
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDICloseAdapter(void *);
#define D3DKMTCloseAdapter NtGdiDdDDICloseAdapter
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIQueryAdapterInfo(void *);
#define D3DKMTQueryAdapterInfo NtGdiDdDDIQueryAdapterInfo
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIGetDisplayModeList(void *);
#define D3DKMTGetDisplayModeList NtGdiDdDDIGetDisplayModeList
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDICreateDevice(void *);
#define D3DKMTCreateDevice NtGdiDdDDICreateDevice
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIDestroyDevice(void *);
#define D3DKMTDestroyDevice NtGdiDdDDIDestroyDevice
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDICreateContext(void *);
#define D3DKMTCreateContext NtGdiDdDDICreateContext
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIDestroyContext(void *);
#define D3DKMTDestroyContext NtGdiDdDDIDestroyContext
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDICreateAllocation(void *);
#define D3DKMTCreateAllocation NtGdiDdDDICreateAllocation
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIDestroyAllocation(void *);
#define D3DKMTDestroyAllocation NtGdiDdDDIDestroyAllocation
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDILock(void *);
#define D3DKMTLock NtGdiDdDDILock
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIUnlock(void *);
#define D3DKMTUnlock NtGdiDdDDIUnlock
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIRender(void *);
#define D3DKMTRender NtGdiDdDDIRender
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIEscape(void *);
#define D3DKMTEscape NtGdiDdDDIEscape
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDICreateSynchronizationObject2(void *);
#define D3DKMTCreateSynchronizationObject2 NtGdiDdDDICreateSynchronizationObject2
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIDestroySynchronizationObject(void *);
#define D3DKMTDestroySynchronizationObject NtGdiDdDDIDestroySynchronizationObject
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDISignalSynchronizationObjectFromCpu(void *);
#define D3DKMTSignalSynchronizationObjectFromCpu NtGdiDdDDISignalSynchronizationObjectFromCpu
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIWaitForSynchronizationObjectFromCpu(void *);
#define D3DKMTWaitForSynchronizationObjectFromCpu NtGdiDdDDIWaitForSynchronizationObjectFromCpu
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDISignalSynchronizationObjectFromGpu(void *);
#define D3DKMTSignalSynchronizationObjectFromGpu NtGdiDdDDISignalSynchronizationObjectFromGpu
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIWaitForSynchronizationObjectFromGpu(void *);
#define D3DKMTWaitForSynchronizationObjectFromGpu NtGdiDdDDIWaitForSynchronizationObjectFromGpu
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIWaitForIdle(void *);
#define D3DKMTWaitForIdle NtGdiDdDDIWaitForIdle
/* (the NT-handle sharing: the handle comes back in 8 bytes and the OBJECT_ATTRIBUTES is the kernel's 48-byte layout, as kmt_attributes() writes it) */
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIShareObjects(UINT, const UINT *, void *, DWORD, UINT64 *);
#define D3DKMTShareObjects NtGdiDdDDIShareObjects
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIQueryResourceInfoFromNtHandle(void *);
#define D3DKMTQueryResourceInfoFromNtHandle NtGdiDdDDIQueryResourceInfoFromNtHandle
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIOpenResourceFromNtHandle(void *);
#define D3DKMTOpenResourceFromNtHandle NtGdiDdDDIOpenResourceFromNtHandle
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIOpenSyncObjectFromNtHandle2(void *);
#define D3DKMTOpenSyncObjectFromNtHandle2 NtGdiDdDDIOpenSyncObjectFromNtHandle2
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIOpenNtHandleFromName(void *);
#define D3DKMTOpenNtHandleFromName NtGdiDdDDIOpenNtHandleFromName
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIOpenSyncObjectNtHandleFromName(void *);
#define D3DKMTOpenSyncObjectNtHandleFromName NtGdiDdDDIOpenSyncObjectNtHandleFromName
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDICreateKeyedMutex2(void *);
#define D3DKMTCreateKeyedMutex2 NtGdiDdDDICreateKeyedMutex2
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIDestroyKeyedMutex(void *);
#define D3DKMTDestroyKeyedMutex NtGdiDdDDIDestroyKeyedMutex
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIAcquireKeyedMutex(void *);
#define D3DKMTAcquireKeyedMutex NtGdiDdDDIAcquireKeyedMutex
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIReleaseKeyedMutex(void *);
#define D3DKMTReleaseKeyedMutex NtGdiDdDDIReleaseKeyedMutex
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIOpenKeyedMutexFromNtHandle(void *);
#define D3DKMTOpenKeyedMutexFromNtHandle NtGdiDdDDIOpenKeyedMutexFromNtHandle
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIOpenKeyedMutex(void *);
#define D3DKMTOpenKeyedMutex NtGdiDdDDIOpenKeyedMutex
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIQueryResourceInfo(void *);
#define D3DKMTQueryResourceInfo NtGdiDdDDIQueryResourceInfo
__declspec(dllimport) NTSTATUS __cdecl NtGdiDdDDIOpenResource(void *);
#define D3DKMTOpenResource NtGdiDdDDIOpenResource
#else
NTSTATUS WINAPI D3DKMTEnumAdapters2(void *);
NTSTATUS WINAPI D3DKMTOpenAdapterFromLuid(void *);
NTSTATUS WINAPI D3DKMTCloseAdapter(void *);
NTSTATUS WINAPI D3DKMTQueryAdapterInfo(void *);
NTSTATUS WINAPI D3DKMTGetDisplayModeList(void *);
NTSTATUS WINAPI D3DKMTCreateDevice(void *);
NTSTATUS WINAPI D3DKMTDestroyDevice(void *);
NTSTATUS WINAPI D3DKMTCreateContext(void *);
NTSTATUS WINAPI D3DKMTDestroyContext(void *);
NTSTATUS WINAPI D3DKMTCreateAllocation(void *);
NTSTATUS WINAPI D3DKMTDestroyAllocation(void *);
NTSTATUS WINAPI D3DKMTLock(void *);
NTSTATUS WINAPI D3DKMTUnlock(void *);
NTSTATUS WINAPI D3DKMTRender(void *);
NTSTATUS WINAPI D3DKMTEscape(void *);
NTSTATUS WINAPI D3DKMTCreateSynchronizationObject2(void *);
NTSTATUS WINAPI D3DKMTDestroySynchronizationObject(void *);
NTSTATUS WINAPI D3DKMTSignalSynchronizationObjectFromCpu(void *);
NTSTATUS WINAPI D3DKMTWaitForSynchronizationObjectFromCpu(void *);
NTSTATUS WINAPI D3DKMTSignalSynchronizationObjectFromGpu(void *);
NTSTATUS WINAPI D3DKMTWaitForSynchronizationObjectFromGpu(void *);
NTSTATUS WINAPI D3DKMTWaitForIdle(void *);
NTSTATUS WINAPI D3DKMTShareObjects(UINT, const UINT *, void *, DWORD, UINT64 *);
NTSTATUS WINAPI D3DKMTQueryResourceInfoFromNtHandle(void *);
NTSTATUS WINAPI D3DKMTOpenResourceFromNtHandle(void *);
NTSTATUS WINAPI D3DKMTOpenSyncObjectFromNtHandle2(void *);
NTSTATUS WINAPI D3DKMTOpenNtHandleFromName(void *);
NTSTATUS WINAPI D3DKMTOpenSyncObjectNtHandleFromName(void *);
NTSTATUS WINAPI D3DKMTCreateKeyedMutex2(void *);
NTSTATUS WINAPI D3DKMTDestroyKeyedMutex(void *);
NTSTATUS WINAPI D3DKMTAcquireKeyedMutex(void *);
NTSTATUS WINAPI D3DKMTReleaseKeyedMutex(void *);
NTSTATUS WINAPI D3DKMTOpenKeyedMutexFromNtHandle(void *);
NTSTATUS WINAPI D3DKMTOpenKeyedMutex(void *);
NTSTATUS WINAPI D3DKMTQueryResourceInfo(void *);
NTSTATUS WINAPI D3DKMTOpenResource(void *);
#endif

#pragma GCC diagnostic ignored "-Wunused-function"

#define KMT_OK(status) ((NTSTATUS)(status) >= 0)

typedef struct KmtAdapter { UINT handle; LUID luid; UINT sources; } KmtAdapter;

/* All the adapters of the machine, each opened (the caller closes them). Returns how many were stored (at most `capacity`); the number there are goes to *total if given. */
static UINT kmt_enum_adapters(KmtAdapter *out, UINT capacity, UINT *total) {
  BYTE block[16] __attribute__((aligned(16))) = {0};
  BYTE entries[16 * 20] __attribute__((aligned(16)));
  UINT n = 0;
  if (total) *total = 0;
  if (!KMT_OK(D3DKMTEnumAdapters2(block))) return 0;
  n = KGET32(block, 0);
  if (total) *total = n;
  if (n == 0 || capacity == 0) return 0;
  if (n > 16) n = 16;
  KPUT32(block, 0, n);
  KPUT64(block, 8, entries);
  if (!KMT_OK(D3DKMTEnumAdapters2(block))) return 0;
  n = KGET32(block, 0);
  if (n > 16) n = 16;
  for (UINT i = 0; i < n; ++i) {
    BYTE *e = entries + i * 20;
    if (i < capacity) {
      out[i].handle = KGET32(e, 0);
      out[i].luid.LowPart = KGET32(e, 4);
      out[i].luid.HighPart = (LONG)KGET32(e, 8);
      out[i].sources = KGET32(e, 12);
    } else {
      BYTE c[4] __attribute__((aligned(8)));
      KPUT32(c, 0, KGET32(e, 0));
      D3DKMTCloseAdapter(c);
    }
  }
  return n < capacity ? n : capacity;
}

static NTSTATUS kmt_open_adapter(LUID luid, UINT *handle) {
  BYTE block[16] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, luid.LowPart);
  KPUT32(block, 4, luid.HighPart);
  NTSTATUS st = D3DKMTOpenAdapterFromLuid(block);
  if (KMT_OK(st)) *handle = KGET32(block, 8);
  return st;
}

static void kmt_close_adapter(UINT handle) {
  BYTE block[8] __attribute__((aligned(8))) = {0};
  KPUT32(block, 0, handle);
  D3DKMTCloseAdapter(block);
}

static NTSTATUS kmt_query_adapter(UINT adapter, UINT type, void *data, UINT size) {
  BYTE block[24] __attribute__((aligned(8))) = {0};
  KPUT32(block, 0, adapter);
  KPUT32(block, 4, type);
  KPUT64(block, 8, data);
  KPUT32(block, 16, size);
  return D3DKMTQueryAdapterInfo(block);
}

/* The private adapter data this kernel answers (KMTQAITYPE_UMDRIVERPRIVATE): the capabilities of the adapter (src/Process/Win32k/Dxg/DxgAdapters.cpp). */
typedef struct KmtCaps {
  UINT max_texture_dimension, max_vertex_attributes, max_varyings, max_texture_units, max_push_constant_bytes, max_vertex_stride, flags, features;
  char name[64];
  UINT max_color_targets, max_vertex_buffers;      /* (what the device takes: DeviceInfo::limits of the kernel's graphics device) */
  UINT sample_counts, device_kind, vendor_id, device_id;      /* bit n of sample_counts: images of n samples; the kind (0 the CPU renderer, 1 a virtual GPU, 2 a GPU) and PCI-style numbers */
} KmtCaps;
#define KMT_CAPS_ACCELERATED 1u
#define KMT_CAPS_STENCIL 2u
#define KMT_CAPS_MIPMAPS 4u
#define KMT_CAPS_TEXTURES 8u
#define KMT_CAPS_BLEND_CONSTANT 64u      /* (pipelines with the constant blend factors: graphics::DeviceInfo::blend_constant) */
static NTSTATUS kmt_adapter_caps(UINT adapter, KmtCaps *caps) { return kmt_query_adapter(adapter, 0, caps, sizeof(KmtCaps)); }

/* The memory of the adapter (KMTQAITYPE_GETSEGMENTSIZE): dedicated video memory, dedicated system memory, shared system memory. */
static NTSTATUS kmt_segment_sizes(UINT adapter, UINT64 sizes[3]) {
  UINT64 raw[3] = {0, 0, 0};
  NTSTATUS st = kmt_query_adapter(adapter, 3, raw, sizeof raw);
  if (KMT_OK(st)) { sizes[0] = raw[0]; sizes[1] = raw[1]; sizes[2] = raw[2]; }
  return st;
}

static NTSTATUS kmt_create_device(UINT adapter, UINT *device) {
  BYTE block[64] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, adapter);
  NTSTATUS st = D3DKMTCreateDevice(block);
  if (KMT_OK(st)) *device = KGET32(block, 12);
  return st;
}

static void kmt_destroy_device(UINT device) {
  BYTE block[8] __attribute__((aligned(8))) = {0};
  KPUT32(block, 0, device);
  D3DKMTDestroyDevice(block);
}

/* A context with the buffers the kernel maps for it: the command (DMA) buffer, and the allocation list of 8-byte entries. */
typedef struct KmtContext {
  UINT handle;
  BYTE *commands;
  UINT command_bytes;
  UINT *allocation_list;      /* entry n is allocation_list[2 * n] */
  UINT allocation_entries;
} KmtContext;

static NTSTATUS kmt_create_context(UINT device, KmtContext *context) {
  BYTE block[96] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, device);
  NTSTATUS st = D3DKMTCreateContext(block);
  if (!KMT_OK(st)) return st;
  context->handle = KGET32(block, 32);
  context->commands = (BYTE *)(UINT_PTR)KGET64(block, 40);
  context->command_bytes = KGET32(block, 48);
  context->allocation_list = (UINT *)(UINT_PTR)KGET64(block, 56);
  context->allocation_entries = KGET32(block, 64);
  return st;
}

static void kmt_destroy_context(UINT context) {
  BYTE block[8] __attribute__((aligned(8))) = {0};
  KPUT32(block, 0, context);
  D3DKMTDestroyContext(block);
}

/* Runs the first `bytes` of the context's command buffer with the first `allocations` entries of its allocation list. */
static NTSTATUS kmt_render(const KmtContext *context, UINT bytes, UINT allocations) {
  BYTE block[160] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, context->handle);
  KPUT32(block, 4, 0);
  KPUT32(block, 8, bytes);
  KPUT32(block, 12, allocations);
  return D3DKMTRender(block);
}

/* The private driver data of an allocation (src/Process/Win32k/Dxg/DxgLayouts.hpp AllocWord) */
enum { KAW_VERSION, KAW_FLAGS, KAW_FORMAT, KAW_WIDTH, KAW_HEIGHT, KAW_MIPS, KAW_SIZE_LO, KAW_SIZE_HI, KAW_KIND, KAW_SAMPLES, KAW_LAYERS, KAW_INITIAL_STATE, KAW_HEAP, KAW_HEAP_OFFSET_LO,
       KAW_HEAP_OFFSET_HI, KAW_PITCH, KAW_BYTES_LO, KAW_BYTES_HI, KAW_WORDS };

typedef struct KmtAllocDesc {
  UINT flags, format, width, height, mips, kind, samples, layers, initial_state, heap;
  UINT64 size, heap_offset;
} KmtAllocDesc;

static NTSTATUS kmt_create_allocation_shared(UINT device, const KmtAllocDesc *d, const void *runtime, UINT runtime_size, UINT *resource, UINT *global, UINT *allocation, UINT *pitch,
                                             UINT64 *bytes);
static NTSTATUS kmt_create_allocation(UINT device, const KmtAllocDesc *d, UINT *allocation, UINT *pitch, UINT64 *bytes) {
  return kmt_create_allocation_shared(device, d, NULL, 0, NULL, NULL, allocation, pitch, bytes);
}
/* `resource` non-NULL: the allocation is made as a resource of its own that is shared through NT handles (CreateResource | NtSecuritySharing), with `runtime` as
 * the data an opener gets; *resource is its handle (what kmt_share_objects shares). */
static NTSTATUS kmt_create_allocation_ex(UINT device, const KmtAllocDesc *d, const void *runtime, UINT runtime_size, UINT *resource, UINT *allocation, UINT *pitch, UINT64 *bytes) {
  return kmt_create_allocation_shared(device, d, runtime, runtime_size, resource, NULL, allocation, pitch, bytes);
}
/* ... and `global` non-NULL as well: shared through a global share handle instead (CreateResource | CreateShared: the legacy handles of
 * IDXGIResource::GetSharedHandle), *global being it. */
static NTSTATUS kmt_create_allocations_shared(UINT device, const KmtAllocDesc *descs, UINT count, const void *runtime, UINT runtime_size, UINT *resource, UINT *global,
                                              UINT *allocations, UINT *pitches, UINT64 *bytes);
static NTSTATUS kmt_create_allocation_shared(UINT device, const KmtAllocDesc *d, const void *runtime, UINT runtime_size, UINT *resource, UINT *global, UINT *allocation, UINT *pitch,
                                             UINT64 *bytes) {
  return kmt_create_allocations_shared(device, d, 1, runtime, runtime_size, resource, global, allocation, pitch, bytes);
}
/* `count` allocations (at most KMT_MAX_RESOURCE_ALLOCATIONS) made in one call - with `resource` non-NULL, one resource of all of them (Windows' resource of several
 * allocations, a planar texture's planes), shared as kmt_create_allocation_shared shares one: through NT handles, or through `global`. allocations[i], pitches[i] and
 * bytes[i] are the i-th's (pitches and bytes may be NULL). On a failure none of them is made. */
#define KMT_MAX_RESOURCE_ALLOCATIONS 256u      /* (win32k's kMaxAllocationsPerCall) */
static NTSTATUS kmt_create_allocations_shared(UINT device, const KmtAllocDesc *descs, UINT count, const void *runtime, UINT runtime_size, UINT *resource, UINT *global,
                                              UINT *allocations, UINT *pitches, UINT64 *bytes) {
  if (count == 0 || count > KMT_MAX_RESOURCE_ALLOCATIONS) return (NTSTATUS)0xC000000Du;      /* STATUS_INVALID_PARAMETER */
  UINT *priv = mem_alloc((SIZE_T)count * KAW_WORDS * 4);
  BYTE *info = mem_alloc((SIZE_T)count * 40);      /* (D3DDDI_ALLOCATIONINFO: 40 bytes each) */
  if (!priv || !info) { mem_free(priv); mem_free(info); return (NTSTATUS)0xC0000017u; }      /* STATUS_NO_MEMORY */
  BYTE block[72] __attribute__((aligned(16))) = {0};
  for (UINT i = 0; i < count; ++i) {
    const KmtAllocDesc *d = &descs[i];
    UINT *w = priv + (SIZE_T)i * KAW_WORDS;
    w[KAW_VERSION] = 1;
    w[KAW_FLAGS] = d->flags;
    w[KAW_FORMAT] = d->format;
    w[KAW_WIDTH] = d->width;
    w[KAW_HEIGHT] = d->height;
    w[KAW_MIPS] = d->mips ? d->mips : 1;
    w[KAW_SIZE_LO] = (UINT)d->size;
    w[KAW_SIZE_HI] = (UINT)(d->size >> 32);
    w[KAW_KIND] = d->kind;
    w[KAW_SAMPLES] = d->samples ? d->samples : 1;
    w[KAW_LAYERS] = d->layers ? d->layers : 1;
    w[KAW_INITIAL_STATE] = d->initial_state;
    w[KAW_HEAP] = d->heap;
    w[KAW_HEAP_OFFSET_LO] = (UINT)d->heap_offset;
    w[KAW_HEAP_OFFSET_HI] = (UINT)(d->heap_offset >> 32);
    KPUT64(info + (SIZE_T)i * 40, 16, (UINT_PTR)w);
    KPUT32(info + (SIZE_T)i * 40, 24, KAW_WORDS * 4);
  }
  KPUT32(block, 0, device);
  KPUT32(block, 44, count);
  KPUT64(block, 48, (UINT_PTR)info);
  if (resource) {
    KPUT64(block, 16, (UINT_PTR)runtime);
    KPUT32(block, 24, runtime_size);
    KPUT32(block, 56, global ? (1u | 2u) : (1u | 0x40u));      /* CreateResource | CreateShared, or CreateResource | NtSecuritySharing */
  }
  NTSTATUS st = D3DKMTCreateAllocation(block);
  if (KMT_OK(st)) {
    for (UINT i = 0; i < count; ++i) {
      const UINT *w = priv + (SIZE_T)i * KAW_WORDS;
      allocations[i] = KGET32(info + (SIZE_T)i * 40, 0);
      if (pitches) pitches[i] = w[KAW_PITCH];
      if (bytes) bytes[i] = w[KAW_BYTES_LO] | ((UINT64)w[KAW_BYTES_HI] << 32);
    }
    if (resource) *resource = KGET32(block, 4);
    if (resource && global) *global = KGET32(block, 8);
  }
  mem_free(priv);
  mem_free(info);
  return st;
}

static void kmt_destroy_allocation(UINT device, UINT allocation) {
  BYTE block[32] __attribute__((aligned(8))) = {0};
  UINT list[2] = {allocation, 0};
  KPUT32(block, 0, device);
  KPUT64(block, 8, list);
  KPUT32(block, 16, 1);
  D3DKMTDestroyAllocation(block);
}

#define KMT_LOCK_READ_ONLY 1u
#define KMT_LOCK_DISCARD 0x80u

/* The CPU's view of one subresource (the mip level; 0 for a buffer): a copy in this process's memory that unlock writes back unless the lock was read-only. */
static NTSTATUS kmt_lock(UINT device, UINT allocation, UINT subresource, UINT flags, void **data) {
  BYTE block[48] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, device);
  KPUT32(block, 4, allocation);
  KPUT32(block, 8, subresource);
  KPUT32(block, 32, flags);
  NTSTATUS st = D3DKMTLock(block);
  if (KMT_OK(st)) *data = (void *)(UINT_PTR)KGET64(block, 24);
  return st;
}

static NTSTATUS kmt_unlock(UINT device, UINT allocation) {
  BYTE block[16] __attribute__((aligned(16))) = {0};
  UINT list[2] = {allocation, 0};
  KPUT32(block, 0, device);
  KPUT32(block, 4, 1);
  KPUT64(block, 8, list);
  return D3DKMTUnlock(block);
}

/* The private escape (shaders, pipelines, root signatures, descriptor heaps ...: src/Kernel/Dxgkrnl/DxgKernel.hpp EscapeOp). `buffer` holds the request; the reply is written over it, so
 * it must be big enough for both. */
static NTSTATUS kmt_escape(UINT adapter, UINT device, void *buffer, UINT bytes) {
  BYTE block[32] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, adapter);
  KPUT32(block, 4, device);
  KPUT64(block, 16, buffer);
  KPUT32(block, 24, bytes);
  return D3DKMTEscape(block);
}

/* Synchronization objects: fences of 64-bit values. */
static NTSTATUS kmt_create_fence(UINT device, UINT64 initial, UINT *sync) {
  BYTE block[96] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, device);
  KPUT32(block, 8, 3);              /* D3DDDI_FENCE */
  KPUT64(block, 16, initial);
  NTSTATUS st = D3DKMTCreateSynchronizationObject2(block);
  if (KMT_OK(st)) *sync = KGET32(block, 88);
  return st;
}

/* A fence another process can open: made Shared | NtSecuritySharing, shared with kmt_share_objects. */
static NTSTATUS kmt_create_shared_fence(UINT device, UINT64 initial, UINT *sync) {
  BYTE block[96] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, device);
  KPUT32(block, 8, 3);              /* D3DDDI_FENCE */
  KPUT32(block, 12, 3);             /* D3DDDI_SYNCHRONIZATIONOBJECT_FLAGS: Shared | NtSecuritySharing */
  KPUT64(block, 16, initial);
  NTSTATUS st = D3DKMTCreateSynchronizationObject2(block);
  if (KMT_OK(st)) *sync = KGET32(block, 88);
  return st;
}

/* ---- NT-handle sharing (src/Process/Win32k/Dxg/DxgShares.cpp) ---------------------------------------------------------------------------------------------- */

/* An OBJECT_ATTRIBUTES of the kernel's layout { Length @0; RootDirectory @8; ObjectName @16; Attributes @24; SecurityDescriptor @32; SecurityQualityOfService @40 }
 * and its UNICODE_STRING { Length @0; MaximumLength @2; Buffer @8 } for the name a Direct3D runtime gives a shared object: the session's \BaseNamedObjects
 * directory (Global\ puts it in the system's, Local\ is the session's), as kernel32 names its objects. NULL name: an object without one. */
typedef struct KmtAttributes { BYTE attributes[48]; BYTE string[16]; WCHAR text[300]; } KmtAttributes;
static void *kmt_attributes(KmtAttributes *a, const WCHAR *name, BOOL inherit, const void *security) {
  static const WCHAR local_prefix[] = L"\\Sessions\\1\\BaseNamedObjects\\", global_prefix[] = L"\\BaseNamedObjects\\";
  memset(a, 0, sizeof(*a));
  KPUT32(a->attributes, 0, 48);
  KPUT32(a->attributes, 24, inherit ? 2u : 0u);      /* OBJ_INHERIT (names are case sensitive) */
  KPUT64(a->attributes, 32, (UINT_PTR)security);
  if (!name || !*name) return a->attributes;
  const WCHAR *prefix = local_prefix, *rest = name;
  if ((name[0] | 32) == 'l' && (name[1] | 32) == 'o' && (name[2] | 32) == 'c' && (name[3] | 32) == 'a' && (name[4] | 32) == 'l' && name[5] == '\\') rest = name + 6;
  else if ((name[0] | 32) == 'g' && (name[1] | 32) == 'l' && (name[2] | 32) == 'o' && (name[3] | 32) == 'b' && (name[4] | 32) == 'a' && (name[5] | 32) == 'l' && name[6] == '\\') { rest = name + 7; prefix = global_prefix; }
  UINT n = 0;
  for (; prefix[n]; ++n) a->text[n] = prefix[n];
  for (; *rest && n < 299; ++rest) a->text[n++] = *rest;
  if (*rest) return NULL;      /* (too long: refused by the caller) */
  a->text[n] = 0;
  KPUT32(a->string, 0, (n * 2) | ((n * 2 + 2) << 16));
  KPUT64(a->string, 8, (UINT_PTR)a->text);
  KPUT64(a->attributes, 16, (UINT_PTR)a->string);
  return a->attributes;
}
/* An NT handle (*nt, a HANDLE's value) for a resource handle (the win32k resource of an allocation made with NtSecuritySharing) or a shared fence - or, as a list, a
 * resource with its keyed mutex (and a fence). */
static NTSTATUS kmt_share_list(const UINT *objects, UINT count, void *attributes, DWORD access, UINT64 *nt) {
  *nt = 0;
  return D3DKMTShareObjects(count, objects, attributes, access, nt);
}
static NTSTATUS kmt_share_objects(UINT object, void *attributes, DWORD access, UINT64 *nt) {
  UINT list[1] = {object};
  return kmt_share_list(list, 1, attributes, access, nt);
}

/* ---- keyed mutexes (src/Process/Win32k/Dxg/DxgKeyedMutex.cpp) ---- */
/* A keyed mutex whose first key is `initial`, shared through its resource's NT handle (NtSecuritySharing) - or, `global` non-NULL, through the global share handle
 * that comes back there (the legacy handles). */
static NTSTATUS kmt_create_keyed_mutex_shared(UINT64 initial, UINT *mutex, UINT *global) {
  BYTE block[32] __attribute__((aligned(16))) = {0};
  KPUT64(block, 0, initial);
  KPUT32(block, 28, global ? 0u : 1u);      /* NtSecuritySharing */
  const NTSTATUS st = D3DKMTCreateKeyedMutex2(block);
  *mutex = KMT_OK(st) ? KGET32(block, 12) : 0;
  if (global) *global = KMT_OK(st) ? KGET32(block, 8) : 0;
  return st;
}
static NTSTATUS kmt_create_keyed_mutex(UINT64 initial, UINT *mutex) { return kmt_create_keyed_mutex_shared(initial, mutex, NULL); }
/* The keyed mutex of a global share handle. */
static NTSTATUS kmt_open_keyed_mutex_global(UINT global, UINT *mutex) {
  BYTE block[8] __attribute__((aligned(8))) = {0};
  KPUT32(block, 0, global);
  const NTSTATUS st = D3DKMTOpenKeyedMutex(block);
  *mutex = KMT_OK(st) ? KGET32(block, 4) : 0;
  return st;
}
/* What a resource's global share handle holds (D3DKMT_QUERYRESOURCEINFO), and opening it (D3DKMT_OPENRESOURCE) as kmt_query_resource_nt / kmt_open_resource_nt do
 * an NT handle's: one allocation, its private data into `driver` (the resource's comes first in that buffer: `resource_driver` bytes). */
static NTSTATUS kmt_query_resource_global(UINT device, UINT global, void *runtime, UINT capacity, UINT *runtime_size, UINT *total_driver, UINT *resource_driver, UINT *allocations) {
  BYTE block[32] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, device);
  KPUT32(block, 4, global);
  KPUT64(block, 8, (UINT_PTR)runtime);
  KPUT32(block, 16, capacity);
  const NTSTATUS st = D3DKMTQueryResourceInfo(block);
  *runtime_size = KGET32(block, 16);
  *total_driver = KGET32(block, 20);
  *resource_driver = KGET32(block, 24);
  *allocations = KGET32(block, 28);
  return st;
}
/* ... of `count` allocations: allocations[i] and, in `driver`, the i-th's private data at datas[i] (NULL: not wanted). */
static NTSTATUS kmt_open_resource_global_n(UINT device, UINT global, void *runtime, UINT runtime_size, void *driver, UINT driver_size, UINT count, UINT *allocations,
                                           void **datas, UINT *resource) {
  if (count == 0 || count > KMT_MAX_RESOURCE_ALLOCATIONS) return (NTSTATUS)0xC000000Du;
  BYTE block[72] __attribute__((aligned(16))) = {0};
  BYTE *info = mem_alloc((SIZE_T)count * 24);      /* (D3DDDI_OPENALLOCATIONINFO: 24 bytes each) */
  if (!info) return (NTSTATUS)0xC0000017u;
  KPUT32(block, 0, device);
  KPUT32(block, 4, global);
  KPUT32(block, 8, count);
  KPUT64(block, 16, (UINT_PTR)info);
  KPUT64(block, 24, (UINT_PTR)runtime);
  KPUT32(block, 32, runtime_size);
  KPUT64(block, 56, (UINT_PTR)driver);
  KPUT32(block, 64, driver_size);
  const NTSTATUS st = D3DKMTOpenResource(block);
  if (KMT_OK(st)) {
    for (UINT i = 0; i < count; ++i) {
      allocations[i] = KGET32(info + (SIZE_T)i * 24, 0);
      if (datas) datas[i] = (void *)(UINT_PTR)KGET64(info + (SIZE_T)i * 24, 8);
    }
    *resource = KGET32(block, 68);
  }
  mem_free(info);
  return st;
}
static NTSTATUS kmt_open_resource_global(UINT device, UINT global, void *runtime, UINT runtime_size, void *driver, UINT driver_size, UINT *allocation, UINT *resource,
                                         void **allocation_driver) {
  return kmt_open_resource_global_n(device, global, runtime, runtime_size, driver, driver_size, 1, allocation, allocation_driver, resource);
}
static void kmt_destroy_keyed_mutex(UINT mutex) {
  BYTE block[8] __attribute__((aligned(8))) = {0};
  KPUT32(block, 0, mutex);
  D3DKMTDestroyKeyedMutex(block);
}
/* Waits up to `ms` milliseconds (INFINITE: forever) for the mutex with `key`: STATUS_SUCCESS, STATUS_TIMEOUT (0x102), STATUS_ABANDONED (0x80) or a failure. */
static NTSTATUS kmt_acquire_keyed_mutex(UINT mutex, UINT64 key, DWORD ms) {
  BYTE block[32] __attribute__((aligned(16))) = {0};
  INT64 timeout = -(INT64)ms * 10000;
  KPUT32(block, 0, mutex);
  KPUT64(block, 8, key);
  KPUT64(block, 16, ms == INFINITE ? 0 : (UINT_PTR)&timeout);
  return D3DKMTAcquireKeyedMutex(block);
}
static NTSTATUS kmt_release_keyed_mutex(UINT mutex, UINT64 key) {
  BYTE block[24] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, mutex);
  KPUT64(block, 8, key);
  return D3DKMTReleaseKeyedMutex(block);
}
/* The keyed mutex shared with a resource's NT handle (STATUS_INVALID_PARAMETER: it was shared without one). */
static NTSTATUS kmt_open_keyed_mutex_nt(UINT64 nt, UINT *mutex) {
  BYTE block[32] __attribute__((aligned(16))) = {0};
  BYTE data[256];
  KPUT64(block, 0, nt);
  KPUT64(block, 16, (UINT_PTR)data);
  KPUT32(block, 24, sizeof data);
  const NTSTATUS st = D3DKMTOpenKeyedMutexFromNtHandle(block);
  *mutex = KMT_OK(st) ? KGET32(block, 8) : 0;
  return st;
}
/* What a shared resource's NT handle holds: its runtime data (up to `capacity` bytes into `runtime`), the bytes of its driver data, the number of allocations. */
static NTSTATUS kmt_query_resource_nt(UINT device, UINT64 nt, void *runtime, UINT capacity, UINT *runtime_size, UINT *total_driver, UINT *resource_driver, UINT *allocations) {
  BYTE block[40] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, device);
  KPUT64(block, 8, nt);
  KPUT64(block, 16, (UINT_PTR)runtime);
  KPUT32(block, 24, capacity);
  const NTSTATUS st = D3DKMTQueryResourceInfoFromNtHandle(block);
  *runtime_size = KGET32(block, 24);
  *total_driver = KGET32(block, 28);
  *resource_driver = KGET32(block, 32);
  *allocations = KGET32(block, 36);
  return st;
}
/* Opens a shared resource of `count` allocations (what kmt_query_resource_nt said it has): allocations[i] on `device` and, in `driver`, the i-th's private data at
 * datas[i] (NULL: not wanted), and the resource's handle; the resource's own private data goes to `resource_driver`. */
static void kmt_destroy_sync(UINT sync);
static NTSTATUS kmt_open_resource_nt_n(UINT device, UINT64 nt, void *runtime, UINT runtime_size, void *resource_driver, UINT resource_driver_size, void *driver, UINT driver_size,
                                       UINT count, UINT *allocations, void **datas, UINT *resource) {
  if (count == 0 || count > KMT_MAX_RESOURCE_ALLOCATIONS) return (NTSTATUS)0xC000000Du;
  BYTE block[104] __attribute__((aligned(16))) = {0};
  BYTE *info = mem_alloc((SIZE_T)count * 80);      /* (D3DDDI_OPENALLOCATIONINFO2: 80 bytes each) */
  if (!info) return (NTSTATUS)0xC0000017u;
  KPUT32(block, 0, device);
  KPUT64(block, 8, nt);
  KPUT32(block, 16, count);
  KPUT64(block, 24, (UINT_PTR)info);
  KPUT32(block, 32, runtime_size);
  KPUT64(block, 40, (UINT_PTR)runtime);
  KPUT32(block, 48, resource_driver_size);
  KPUT64(block, 56, (UINT_PTR)resource_driver);
  KPUT32(block, 64, driver_size);
  KPUT64(block, 72, (UINT_PTR)driver);
  BYTE mutex_data[256];      /* (a keyed mutex's data, when one was shared with it) */
  KPUT64(block, 88, (UINT_PTR)mutex_data);
  KPUT32(block, 96, sizeof mutex_data);
  const NTSTATUS st = D3DKMTOpenResourceFromNtHandle(block);
  if (KMT_OK(st)) {
    for (UINT i = 0; i < count; ++i) {
      allocations[i] = KGET32(info + (SIZE_T)i * 80, 0);
      if (datas) datas[i] = (void *)(UINT_PTR)KGET64(info + (SIZE_T)i * 80, 8);
    }
    *resource = KGET32(block, 80);
    /* (the keyed mutex and fence shared with it come with it: the runtime above opens the mutex itself - kmt_open_keyed_mutex_nt - so these go) */
    if (KGET32(block, 84)) kmt_destroy_keyed_mutex(KGET32(block, 84));
    if (KGET32(block, 100)) kmt_destroy_sync(KGET32(block, 100));
  }
  mem_free(info);
  return st;
}
/* ... of one allocation: its private data goes to `driver` (the allocation's) and `resource_driver`. */
static NTSTATUS kmt_open_resource_nt(UINT device, UINT64 nt, void *runtime, UINT runtime_size, void *resource_driver, UINT resource_driver_size, void *driver, UINT driver_size,
                                     UINT *allocation, UINT *resource) {
  return kmt_open_resource_nt_n(device, nt, runtime, runtime_size, resource_driver, resource_driver_size, driver, driver_size, 1, allocation, NULL, resource);
}
/* A synchronization object on `device` for a shared fence's NT handle. */
static NTSTATUS kmt_open_sync_nt(UINT device, UINT64 nt, UINT *sync) {
  BYTE block[88] __attribute__((aligned(16))) = {0};
  KPUT64(block, 0, nt);
  KPUT32(block, 8, device);
  const NTSTATUS st = D3DKMTOpenSyncObjectFromNtHandle2(block);
  if (KMT_OK(st)) *sync = KGET32(block, 16);
  return st;
}
/* An NT handle for the shared object of the name: a resource's (fence 0) or a fence's (fence 1). */
static NTSTATUS kmt_open_nt_by_name(void *attributes, DWORD access, int fence, UINT64 *nt) {
  BYTE block[24] __attribute__((aligned(16))) = {0};
  KPUT32(block, 0, access);
  KPUT64(block, 8, (UINT_PTR)attributes);
  const NTSTATUS st = fence ? D3DKMTOpenSyncObjectNtHandleFromName(block) : D3DKMTOpenNtHandleFromName(block);
  *nt = KMT_OK(st) ? KGET64(block, 16) : 0;
  return st;
}

static void kmt_destroy_sync(UINT sync) {
  BYTE block[8] __attribute__((aligned(8))) = {0};
  KPUT32(block, 0, sync);
  D3DKMTDestroySynchronizationObject(block);
}

static NTSTATUS kmt_signal_cpu(UINT device, UINT sync, UINT64 value) {
  BYTE block[32] __attribute__((aligned(16))) = {0};
  UINT64 v = value;
  UINT h = sync;
  KPUT32(block, 0, device);
  KPUT32(block, 4, 1);
  KPUT64(block, 8, &h);
  KPUT64(block, 16, &v);
  return D3DKMTSignalSynchronizationObjectFromCpu(block);
}

/* Blocks until the fence has reached `value`. */
static NTSTATUS kmt_wait_cpu(UINT device, UINT sync, UINT64 value) {
  BYTE block[40] __attribute__((aligned(16))) = {0};
  UINT64 v = value;
  UINT h = sync;
  KPUT32(block, 0, device);
  KPUT32(block, 4, 1);
  KPUT64(block, 8, &h);
  KPUT64(block, 16, &v);
  return D3DKMTWaitForSynchronizationObjectFromCpu(block);
}

/* On the context's timeline: the signal happens after everything queued before it; the wait holds back everything queued after it. */
static NTSTATUS kmt_signal_gpu(UINT context, UINT sync, UINT64 value) {
  BYTE block[24] __attribute__((aligned(16))) = {0};
  UINT64 v = value;
  UINT h = sync;
  KPUT32(block, 0, context);
  KPUT32(block, 4, 1);
  KPUT64(block, 8, &h);
  KPUT64(block, 16, &v);
  return D3DKMTSignalSynchronizationObjectFromGpu(block);
}

static NTSTATUS kmt_wait_gpu(UINT context, UINT sync, UINT64 value) {
  BYTE block[24] __attribute__((aligned(16))) = {0};
  UINT64 v = value;
  UINT h = sync;
  KPUT32(block, 0, context);
  KPUT32(block, 4, 1);
  KPUT64(block, 8, &h);
  KPUT64(block, 16, &v);
  return D3DKMTWaitForSynchronizationObjectFromGpu(block);
}



/* ---- COM objects ------------------------------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct DxPrivate {
  GUID guid;
  UINT size;
  void *data;              /* a copy of the bytes, or the interface (size == 0xFFFFFFFF) */
  struct DxPrivate *next;
} DxPrivate;

/* The head of every object: the vtable pointer comes first (the object IS its interface pointer), then the reference count, the interfaces QueryInterface answers (a NULL
 * ended list; IUnknown always), what to do when the last reference goes, and the private data set on it. */
typedef struct DxObj {
  const void *lpVtbl;
  LONG ref;
  const GUID *const *iids;
  void (*destroy)(struct DxObj *);
  CRITICAL_SECTION *private_lock;
  DxPrivate *priv;
} DxObj;

static CRITICAL_SECTION g_private_lock;

static void dx_init(DxObj *o, const void *vtbl, const GUID *const *iids, void (*destroy)(DxObj *)) {
  o->lpVtbl = vtbl;
  o->ref = 1;
  o->iids = iids;
  o->destroy = destroy;
}

static HRESULT STDMETHODCALLTYPE dx_QueryInterface(DxObj *o, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (guid_eq(riid, &IID_IUnknown)) goto found;
  for (const GUID *const *i = o->iids; i && *i; ++i)
    if (guid_eq(riid, *i)) goto found;
  dx_no_interface("(an object)", riid);
  return E_NOINTERFACE;
found:
  InterlockedIncrement(&o->ref);
  *out = o;
  return S_OK;
}
static ULONG STDMETHODCALLTYPE dx_AddRef(DxObj *o) { return (ULONG)InterlockedIncrement(&o->ref); }
static ULONG STDMETHODCALLTYPE dx_Release(DxObj *o) {
  const LONG n = InterlockedDecrement(&o->ref);
  if (n == 0) {
    DxPrivate *p = o->priv;
    while (p) {
      DxPrivate *next = p->next;
      if (p->size == 0xFFFFFFFFu) { if (p->data) ((IUnknown *)p->data)->lpVtbl->Release((IUnknown *)p->data); }
      else mem_free(p->data);
      mem_free(p);
      p = next;
    }
    if (o->destroy) o->destroy(o);
    mem_free(o);
  }
  return (ULONG)n;
}

static DxPrivate **dx_find_private(DxObj *o, const GUID *guid) {
  DxPrivate **pp = &o->priv;
  while (*pp && !guid_eq(&(*pp)->guid, guid)) pp = &(*pp)->next;
  return pp;
}
static void dx_drop_private(DxPrivate *p) {
  if (p->size == 0xFFFFFFFFu) { if (p->data) ((IUnknown *)p->data)->lpVtbl->Release((IUnknown *)p->data); }
  else mem_free(p->data);
  mem_free(p);
}
static HRESULT STDMETHODCALLTYPE dx_SetPrivateData(DxObj *o, REFGUID guid, UINT size, const void *data) {
  EnterCriticalSection(&g_private_lock);
  DxPrivate **pp = dx_find_private(o, guid);
  if (*pp) { DxPrivate *old = *pp; *pp = old->next; dx_drop_private(old); }
  if (data && size) {
    DxPrivate *p = mem_alloc(sizeof *p);
    void *copy = p ? mem_alloc(size) : NULL;
    if (!p || !copy) { mem_free(p); mem_free(copy); LeaveCriticalSection(&g_private_lock); return E_OUTOFMEMORY; }
    memcpy(copy, data, size);
    p->guid = *guid; p->size = size; p->data = copy; p->next = o->priv; o->priv = p;
  }
  LeaveCriticalSection(&g_private_lock);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dx_SetPrivateDataInterface(DxObj *o, REFGUID guid, const IUnknown *object) {
  EnterCriticalSection(&g_private_lock);
  DxPrivate **pp = dx_find_private(o, guid);
  if (*pp) { DxPrivate *old = *pp; *pp = old->next; dx_drop_private(old); }
  if (object) {
    DxPrivate *p = mem_alloc(sizeof *p);
    if (!p) { LeaveCriticalSection(&g_private_lock); return E_OUTOFMEMORY; }
    ((IUnknown *)object)->lpVtbl->AddRef((IUnknown *)object);
    p->guid = *guid; p->size = 0xFFFFFFFFu; p->data = (void *)object; p->next = o->priv; o->priv = p;
  }
  LeaveCriticalSection(&g_private_lock);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dx_GetPrivateData(DxObj *o, REFGUID guid, UINT *size, void *data) {
  HRESULT hr = ((HRESULT)0x887A0002);
  EnterCriticalSection(&g_private_lock);
  DxPrivate *p = *dx_find_private(o, guid);
  if (p && p->size == 0xFFFFFFFFu) {
    if (*size < sizeof(void *)) hr = 0x8007007A;      /* ERROR_INSUFFICIENT_BUFFER */
    else { *(void **)data = p->data; ((IUnknown *)p->data)->lpVtbl->AddRef((IUnknown *)p->data); *size = sizeof(void *); hr = S_OK; }
  } else if (p) {
    if (*size < p->size) { *size = p->size; hr = 0x8007007A; }
    else { memcpy(data, p->data, p->size); *size = p->size; hr = S_OK; }
  } else {
    *size = 0;
  }
  LeaveCriticalSection(&g_private_lock);
  return hr;
}

/* A method nobody implemented: E_NOTIMPL (the arguments are not looked at, so it serves any signature). */
static HRESULT STDMETHODCALLTYPE dx_not_implemented(void) { return E_NOTIMPL; }

#endif
