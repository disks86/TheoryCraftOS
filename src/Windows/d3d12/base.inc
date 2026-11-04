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

/* d3d12.dll: the types the parts share, the formats, the GPU virtual addresses, the ID3D12Object / ID3D12DeviceChild methods. Included by d3d12.c. */

typedef struct Dev Dev;
typedef struct Resource Resource;
typedef struct DescHeap DescHeap;
typedef struct RootSig RootSig;
typedef struct Pso Pso;

#define KALIGN(value, alignment) (((value) + (alignment) - 1) / (alignment) * (alignment))

/* ---- the kernel's allocation flags and formats (src/Kernel/Dxgkrnl/DxgTypes.hpp) ------------------------------------------------------------------------------------------- */

enum {
  KA_VERTEX = 1u << 0, KA_INDEX = 1u << 1, KA_TEXTURE = 1u << 2, KA_RENDER_TARGET = 1u << 3, KA_DEPTH = 1u << 4, KA_PRESENT = 1u << 5, KA_STAGING = 1u << 6, KA_UAV = 1u << 9,
  KA_HEAP = 1u << 13, KA_RESERVED = 1u << 14,
};
enum {
  KF_UNKNOWN, KF_A8R8G8B8, KF_X8R8G8B8, KF_A8B8G8R8, KF_X8B8G8R8, KF_D32F, KF_D24S8, KF_RGBA16F, KF_RGBA32F, KF_BGRA8_SRGB, KF_RGBA8_SRGB, KF_RGBA16, KF_RGBA32UINT, KF_RGBA32SINT,
  KF_RGBA16UINT, KF_RGBA16SINT, KF_RGBA8UINT, KF_RGBA16SNORM, KF_RGB10A2,
};
#define KMAX_TEXTURE_DIMENSION 4096
#define KTILE_BYTES 65536

typedef struct FormatInfo {
  DXGI_FORMAT dxgi;
  UINT kernel;         /* the kernel's format (0: not one a texture can have) */
  UINT bytes;          /* of a pixel */
  UINT depth;          /* depth/stencil format */
} FormatInfo;

static const FormatInfo g_formats[] = {
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, KF_RGBA32F, 16, 0}, {DXGI_FORMAT_R32G32B32A32_FLOAT, KF_RGBA32F, 16, 0}, {DXGI_FORMAT_R32G32B32A32_UINT, KF_RGBA32UINT, 16, 0},
    {DXGI_FORMAT_R32G32B32A32_SINT, KF_RGBA32SINT, 16, 0}, {DXGI_FORMAT_R16G16B16A16_TYPELESS, KF_RGBA16F, 8, 0}, {DXGI_FORMAT_R16G16B16A16_FLOAT, KF_RGBA16F, 8, 0},
    {DXGI_FORMAT_R16G16B16A16_UNORM, KF_RGBA16, 8, 0}, {DXGI_FORMAT_R16G16B16A16_UINT, KF_RGBA16UINT, 8, 0}, {DXGI_FORMAT_R16G16B16A16_SNORM, KF_RGBA16SNORM, 8, 0},
    {DXGI_FORMAT_R16G16B16A16_SINT, KF_RGBA16SINT, 8, 0}, {DXGI_FORMAT_R10G10B10A2_TYPELESS, KF_RGB10A2, 4, 0}, {DXGI_FORMAT_R10G10B10A2_UNORM, KF_RGB10A2, 4, 0},
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, KF_A8B8G8R8, 4, 0}, {DXGI_FORMAT_R8G8B8A8_UNORM, KF_A8B8G8R8, 4, 0}, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, KF_RGBA8_SRGB, 4, 0},
    {DXGI_FORMAT_R8G8B8A8_UINT, KF_RGBA8UINT, 4, 0}, {DXGI_FORMAT_B8G8R8A8_UNORM, KF_A8R8G8B8, 4, 0}, {DXGI_FORMAT_B8G8R8X8_UNORM, KF_X8R8G8B8, 4, 0},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, KF_A8R8G8B8, 4, 0}, {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, KF_BGRA8_SRGB, 4, 0}, {DXGI_FORMAT_B8G8R8X8_TYPELESS, KF_X8R8G8B8, 4, 0},
    {DXGI_FORMAT_D32_FLOAT, KF_D32F, 4, 1}, {DXGI_FORMAT_D24_UNORM_S8_UINT, KF_D24S8, 4, 1},
};

static const FormatInfo *format_info(DXGI_FORMAT f) {
  for (UINT i = 0; i < sizeof g_formats / sizeof g_formats[0]; ++i)
    if (g_formats[i].dxgi == f) return &g_formats[i];
  return NULL;
}
/* The bytes of a pixel of any format a buffer view or a vertex attribute may name (not only the ones a texture can have). */
static UINT format_bytes(DXGI_FORMAT f) {
  const FormatInfo *i = format_info(f);
  if (i) return i->bytes;
  switch (f) {
    case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return 12;
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: return 4;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: return 2;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_A8_UNORM: return 1;
    default: return 0;
  }
}

/* ---- the objects' common part -------------------------------------------------------------------------------------------------------------------------------------------- */

/* Every object of the device: the head, then the device (a reference: a child keeps its device alive). */
typedef struct DObj {
  DxObj o;
  Dev *dev;
} DObj;

#define DOBJ(x) ((DObj *)(x))
#define OBJ(x) ((DxObj *)(x))

static CRITICAL_SECTION g_lock;      /* the table of GPU addresses and the resources that need their CPU copy sent to the kernel */
static LONG g_desc_epoch = 1;        /* bumped by every descriptor written: a shader-visible heap whose copy in the kernel is older is sent again */

#define obj_QueryInterface dx_QueryInterface
#define obj_AddRef dx_AddRef
#define obj_Release dx_Release
#define obj_SetPrivateData dx_SetPrivateData
#define obj_SetPrivateDataInterface dx_SetPrivateDataInterface
#define obj_GetPrivateData dx_GetPrivateData

static HRESULT STDMETHODCALLTYPE dobj_SetName(DxObj *o, LPCWSTR name) { (void)o; (void)name; return S_OK; }

/* A new object of `size` bytes with a reference on the device. */
static void dev_addref(Dev *d);
static void dev_release(Dev *d);

static void dobj_child_destroy(DxObj *o) {
  DObj *d = (DObj *)o;
  if (d->dev) dev_release(d->dev);
}

/* ---- GPU virtual addresses --------------------------------------------------------------------------------------------------------------------------------------------- */

/* A buffer's address is made up (the kernel has none): a range of 64 KiB aligned addresses per buffer, found again by a binary search. */
typedef struct VaEntry { UINT64 va, size; Resource *res; } VaEntry;
static VaEntry *g_va;
static UINT g_va_count, g_va_cap;
static UINT64 g_next_va = (UINT64)1 << 32;

static UINT64 va_register(Resource *r, UINT64 size) {
  EnterCriticalSection(&g_lock);
  if (g_va_count == g_va_cap) {
    const UINT cap = g_va_cap ? g_va_cap * 2 : 64;
    VaEntry *n = mem_grow(g_va, cap * sizeof *n);
    if (!n) { LeaveCriticalSection(&g_lock); return 0; }
    g_va = n;
    g_va_cap = cap;
  }
  const UINT64 va = g_next_va;
  g_next_va += KALIGN(size ? size : 1, KTILE_BYTES) + KTILE_BYTES;
  g_va[g_va_count].va = va;
  g_va[g_va_count].size = size;
  g_va[g_va_count].res = r;
  g_va_count++;
  LeaveCriticalSection(&g_lock);
  return va;
}
static void va_unregister(UINT64 va) {
  EnterCriticalSection(&g_lock);
  for (UINT i = 0; i < g_va_count; ++i)
    if (g_va[i].va == va) { memmove(&g_va[i], &g_va[i + 1], (g_va_count - i - 1) * sizeof g_va[0]); g_va_count--; break; }
  LeaveCriticalSection(&g_lock);
}
/* The buffer an address is in, and the offset in it. */
static Resource *va_find(UINT64 va, UINT64 *offset) {
  Resource *found = NULL;
  EnterCriticalSection(&g_lock);
  UINT lo = 0, hi = g_va_count;
  while (lo < hi) {
    const UINT mid = (lo + hi) / 2;
    if (g_va[mid].va <= va) lo = mid + 1;
    else hi = mid;
  }
  if (lo > 0 && va - g_va[lo - 1].va < g_va[lo - 1].size + 1) {
    found = g_va[lo - 1].res;
    *offset = va - g_va[lo - 1].va;
  }
  LeaveCriticalSection(&g_lock);
  return found;
}

/* ---- escape requests ---------------------------------------------------------------------------------------------------------------------------------------------------- */

enum {
  ESC_CREATE_SHADER = 1, ESC_CREATE_PIPELINE = 2, ESC_DESTROY_SHADER = 3, ESC_DESTROY_PIPELINE = 4, ESC_CREATE_COMPUTE_PIPELINE = 5, ESC_CREATE_ROOT_SIGNATURE = 6,
  ESC_DESTROY_ROOT_SIGNATURE = 7, ESC_CREATE_DESCRIPTOR_HEAP = 8, ESC_DESTROY_DESCRIPTOR_HEAP = 9, ESC_WRITE_DESCRIPTORS = 10, ESC_COPY_DESCRIPTORS = 11, ESC_UPDATE_TILE_MAPPINGS = 12,
  ESC_QUERY_SYNC = 13,
};

static UINT bits_of(float f) { UINT u; memcpy(&u, &f, 4); return u; }

/* ---- the device, as far as the other parts need it ---------------------------------------------------------------------------------------------------------------------- */

struct Dev {
  DxObj o;
  LUID luid;
  UINT adapter;            /* the kernel's handle of the adapter */
  UINT kdev;               /* ... and of the device made on it */
  KmtCaps caps;
  UINT64 segments[3];
  int software;
  LONG removed;
};

static const GUID *const g_device_iids[] = {&IID_ID3D12Object, &IID_ID3D12Device, NULL};

static void dev_addref(Dev *d) { dx_AddRef(OBJ(d)); }
static void dev_release(Dev *d) { dx_Release(OBJ(d)); }

static void *dobj_new(SIZE_T size, Dev *dev, const void *vtbl, const GUID *const *iids, void (*destroy)(DxObj *)) {
  DObj *o = mem_alloc(size);
  if (!o) return NULL;
  dx_init(&o->o, vtbl, iids, destroy);
  o->dev = dev;
  dev_addref(dev);
  return o;
}

static HRESULT STDMETHODCALLTYPE dobj_GetDevice(DObj *o, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  return dx_QueryInterface(OBJ(o->dev), riid, out);
}

/* ---- escapes ----------------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* Sends `in_words` words of request to the kernel's private escape, and takes `out_words` words of reply from the same buffer. */
static NTSTATUS dev_escape(Dev *d, const UINT *in, UINT in_words, UINT *out, UINT out_words) {
  const UINT words = in_words > out_words ? in_words : out_words;
  UINT *buffer = mem_alloc((SIZE_T)(words ? words : 1) * 4 + 16);
  if (!buffer) return (NTSTATUS)0xC0000017;
  memcpy(buffer, in, (SIZE_T)in_words * 4);
  const NTSTATUS st = kmt_escape(d->adapter, d->kdev, buffer, words * 4);
  if (KMT_OK(st) && out) memcpy(out, buffer, (SIZE_T)out_words * 4);
  mem_free(buffer);
  return st;
}

static HRESULT hr_from(NTSTATUS st) {
  if (KMT_OK(st)) return S_OK;
  switch ((UINT)st) {
    case 0xC0000017: case 0xC01E0100: case 0xC000009A: return E_OUTOFMEMORY;
    case 0xC00000BB: return DXGI_ERROR_UNSUPPORTED;
    case 0xC000000D: case 0xC0000008: return E_INVALIDARG;
    default: return E_FAIL;
  }
}

/* ---- the factory functions ---------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT query_out(IUnknown *object, REFIID riid, void **out) {
  const HRESULT hr = out ? object->lpVtbl->QueryInterface(object, riid, out) : S_FALSE;
  object->lpVtbl->Release(object);      /* (the creation's own reference: the output has its own, or there is none to give) */
  return hr;
}

