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

/* ---- fields of the structures of the thunks, by offset --------------------------------------------------------------------------------------------------------------------- */

#define KPUT32(block, offset, value) (*(UINT32 *)((BYTE *)(block) + (offset)) = (UINT32)(value))
#define KPUT64(block, offset, value) (*(UINT64 *)((BYTE *)(block) + (offset)) = (UINT64)(value))
#define KGET32(block, offset) (*(UINT32 *)((BYTE *)(block) + (offset)))
#define KGET64(block, offset) (*(UINT64 *)((BYTE *)(block) + (offset)))

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
} KmtCaps;
#define KMT_CAPS_ACCELERATED 1u
#define KMT_CAPS_STENCIL 2u
#define KMT_CAPS_MIPMAPS 4u
#define KMT_CAPS_TEXTURES 8u
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

static NTSTATUS kmt_create_allocation(UINT device, const KmtAllocDesc *d, UINT *allocation, UINT *pitch, UINT64 *bytes) {
  UINT priv[KAW_WORDS] __attribute__((aligned(16))) = {0};
  BYTE block[72] __attribute__((aligned(16))) = {0};
  BYTE info[96] __attribute__((aligned(16))) = {0};
  priv[KAW_VERSION] = 1;
  priv[KAW_FLAGS] = d->flags;
  priv[KAW_FORMAT] = d->format;
  priv[KAW_WIDTH] = d->width;
  priv[KAW_HEIGHT] = d->height;
  priv[KAW_MIPS] = d->mips ? d->mips : 1;
  priv[KAW_SIZE_LO] = (UINT)d->size;
  priv[KAW_SIZE_HI] = (UINT)(d->size >> 32);
  priv[KAW_KIND] = d->kind;
  priv[KAW_SAMPLES] = d->samples ? d->samples : 1;
  priv[KAW_LAYERS] = d->layers ? d->layers : 1;
  priv[KAW_INITIAL_STATE] = d->initial_state;
  priv[KAW_HEAP] = d->heap;
  priv[KAW_HEAP_OFFSET_LO] = (UINT)d->heap_offset;
  priv[KAW_HEAP_OFFSET_HI] = (UINT)(d->heap_offset >> 32);
  KPUT64(info, 16, priv);
  KPUT32(info, 24, sizeof priv);
  KPUT32(block, 0, device);
  KPUT32(block, 44, 1);
  KPUT64(block, 48, info);
  NTSTATUS st = D3DKMTCreateAllocation(block);
  if (!KMT_OK(st)) return st;
  *allocation = KGET32(info, 0);
  if (pitch) *pitch = priv[KAW_PITCH];
  if (bytes) *bytes = priv[KAW_BYTES_LO] | ((UINT64)priv[KAW_BYTES_HI] << 32);
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
