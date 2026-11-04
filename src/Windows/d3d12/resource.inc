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

/* d3d12.dll: resources (ID3D12Resource), heaps (ID3D12Heap) and the footprints of subresources. Included by d3d12.c.
 *
 * A resource is one allocation of the kernel (src/Kernel/Dxgkrnl/VidMm.hpp). The kernel's allocations cannot be mapped for a long time (Lock gives a copy that Unlock writes back),
 * so a buffer in an upload or readback heap keeps its CPU copy here (`shadow`): Map hands that out; the copy goes to the kernel before the GPU work that may read it runs
 * (res_push, from ExecuteCommandLists), and a readback buffer is read from the kernel when it is mapped (res_pull). */

typedef struct HeapObj {
  DObj d;
  D3D12_HEAP_DESC desc;
  UINT alloc;
} HeapObj;

struct Resource {
  DObj d;
  D3D12_RESOURCE_DESC desc;
  D3D12_HEAP_PROPERTIES props;
  D3D12_HEAP_FLAGS heap_flags;
  UINT alloc;                  /* the kernel's allocation */
  UINT64 va;                   /* buffers: the made-up GPU address */
  UINT64 bytes;                /* the kernel's backing */
  const FormatInfo *fmt;       /* textures */
  UINT mips, layers, kind;     /* kind: the kernel's TextureKind (0 2D, 1 3D, 2 2D array, 3 cube) */
  BYTE *shadow;                /* the CPU's copy (upload / readback buffers) */
  int cpu_upload;              /* the CPU writes it, the GPU reads (shadow goes to the kernel) */
  int cpu_readback;            /* the GPU writes it, the CPU reads */
  LONG map_count;
  int dirty;
  UINT64 sum;                  /* of the shadow as the kernel has it */
  int pushed;
  HeapObj *heap;               /* placed: the heap (a reference) */
  struct Resource *next_cpu;   /* the list of the resources with a shadow */
};

static Resource *g_cpu_resources;

static const GUID *const g_resource_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12Resource, NULL};
static const GUID *const g_heap_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12Heap, NULL};

static UINT64 fnv(const BYTE *p, UINT64 n) {
  UINT64 h = 1469598103934665603ull;
  for (UINT64 i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

/* Sends the CPU's copy to the kernel when it changed. */
static void res_push(Resource *r) {
  if (!r->shadow || !r->alloc) return;
  const UINT64 n = r->desc.Width;
  const UINT64 sum = fnv(r->shadow, n);
  if (r->pushed && sum == r->sum) return;
  void *data = NULL;
  if (!KMT_OK(kmt_lock(r->d.dev->kdev, r->alloc, 0, 0, &data))) return;
  memcpy(data, r->shadow, (SIZE_T)(n < r->bytes ? n : r->bytes));
  kmt_unlock(r->d.dev->kdev, r->alloc);
  r->sum = sum;
  r->pushed = 1;
  r->dirty = 0;
}
/* Reads the kernel's copy into the CPU's. */
static void res_pull(Resource *r) {
  if (!r->shadow || !r->alloc) return;
  void *data = NULL;
  if (!KMT_OK(kmt_lock(r->d.dev->kdev, r->alloc, 0, KMT_LOCK_READ_ONLY, &data))) return;
  memcpy(r->shadow, data, (SIZE_T)(r->desc.Width < r->bytes ? r->desc.Width : r->bytes));
  kmt_unlock(r->d.dev->kdev, r->alloc);
}
static void res_push_all(void) {
  EnterCriticalSection(&g_lock);
  for (Resource *r = g_cpu_resources; r; r = r->next_cpu)
    if (r->cpu_upload && (r->map_count > 0 || r->dirty || !r->pushed)) res_push(r);
  LeaveCriticalSection(&g_lock);
}

static void resource_destroy(DxObj *o) {
  Resource *r = (Resource *)o;
  EnterCriticalSection(&g_lock);
  for (Resource **pp = &g_cpu_resources; *pp; pp = &(*pp)->next_cpu)
    if (*pp == r) { *pp = r->next_cpu; break; }
  LeaveCriticalSection(&g_lock);
  if (r->va) va_unregister(r->va);
  if (r->alloc) kmt_destroy_allocation(r->d.dev->kdev, r->alloc);
  mem_free(r->shadow);
  if (r->heap) dx_Release(OBJ(r->heap));
  dobj_child_destroy(o);
}

static UINT mip_count_for(UINT w, UINT h) {
  UINT n = 1, m = w > h ? w : h;
  while (m > 1) { m >>= 1; ++n; }
  return n;
}
static UINT mip_extent(UINT size, UINT level) { UINT v = size >> level; return v ? v : 1; }

/* ---- footprints ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The layout of subresources `first` .. `first + count - 1` of a resource described by `d` in a buffer from `base` on (the rows 256 apart, each subresource 512 aligned). */
static void get_footprints(const D3D12_RESOURCE_DESC *d, UINT first, UINT count, UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *row_sizes, UINT64 *total) {
  UINT64 offset = base;
  UINT64 end = base;
  if (d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    if (layouts && count) {
      memset(&layouts[0], 0, sizeof layouts[0]);
      layouts[0].Offset = base;
      layouts[0].Footprint.Format = DXGI_FORMAT_UNKNOWN;
      layouts[0].Footprint.Width = (UINT)d->Width;
      layouts[0].Footprint.Height = 1;
      layouts[0].Footprint.Depth = 1;
      layouts[0].Footprint.RowPitch = (UINT)d->Width;
    }
    if (rows && count) rows[0] = 1;
    if (row_sizes && count) row_sizes[0] = d->Width;
    if (total) *total = d->Width;
    return;
  }
  const UINT mips = d->MipLevels ? d->MipLevels : mip_count_for((UINT)d->Width, d->Height);
  const UINT depth_or_array = d->DepthOrArraySize ? d->DepthOrArraySize : 1;
  const UINT bytes = format_bytes(d->Format);
  for (UINT i = 0; i < count; ++i) {
    const UINT sub = first + i;
    const UINT level = sub % mips;
    const UINT w = mip_extent((UINT)d->Width, level), h = mip_extent(d->Height, level);
    const UINT depth = d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? mip_extent(depth_or_array, level) : 1;
    const UINT64 row_size = (UINT64)w * bytes;
    const UINT pitch = (UINT)KALIGN(row_size, 256);
    offset = KALIGN(offset, 512);
    if (layouts) {
      memset(&layouts[i], 0, sizeof layouts[i]);
      layouts[i].Offset = offset;
      layouts[i].Footprint.Format = d->Format;
      layouts[i].Footprint.Width = w;
      layouts[i].Footprint.Height = h;
      layouts[i].Footprint.Depth = depth;
      layouts[i].Footprint.RowPitch = pitch;
    }
    if (rows) rows[i] = h;
    if (row_sizes) row_sizes[i] = row_size;
    /* (the last row of the last slice is not padded) */
    end = offset + (UINT64)pitch * h * (depth - 1) + (UINT64)pitch * (h - 1) + row_size;
    offset += (UINT64)pitch * h * depth;
  }
  if (total) *total = end - base;
}

/* ---- creation ----------------------------------------------------------------------------------------------------------------------------------------------------------------- */

static int heap_cpu_access(const D3D12_HEAP_PROPERTIES *p, int *upload, int *readback) {
  *upload = *readback = 0;
  switch (p->Type) {
    case D3D12_HEAP_TYPE_UPLOAD: *upload = 1; return 1;
    case D3D12_HEAP_TYPE_READBACK: *readback = 1; return 1;
    case D3D12_HEAP_TYPE_CUSTOM:
      if (p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE) return 0;
      *upload = 1;
      *readback = p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
      return 1;
    default: return 0;
  }
}

static HRESULT resource_make(Dev *dev, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS hflags, const D3D12_RESOURCE_DESC *desc, HeapObj *heap, UINT64 heap_offset, int present,
                             Resource **out) {
  extern const ID3D12ResourceVtbl res_vtbl;
  KmtAllocDesc ad;
  memset(&ad, 0, sizeof ad);
  ad.mips = 1;
  ad.samples = 1;
  ad.layers = 1;
  if (desc->SampleDesc.Count > 1) return DXGI_ERROR_UNSUPPORTED;
  if (desc->Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN && desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) return E_INVALIDARG;
  Resource *r = dobj_new(sizeof *r, dev, &res_vtbl, g_resource_iids, resource_destroy);
  if (!r) return E_OUTOFMEMORY;
  r->desc = *desc;
  if (props) r->props = *props;
  r->heap_flags = hflags;
  int cpu = props ? heap_cpu_access(props, &r->cpu_upload, &r->cpu_readback) : 0;
  HRESULT hr = S_OK;
  if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    if (desc->Width == 0 || desc->Width > 256ull * 1024 * 1024) { hr = E_INVALIDARG; goto fail; }
    ad.flags = KA_VERTEX | KA_INDEX | ((desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? KA_UAV : 0);
    ad.size = KALIGN(desc->Width, 16);
    if (ad.size < 16) ad.size = 16;
  } else {
    const FormatInfo *fi = format_info(desc->Format);
    UINT depth_or_array = desc->DepthOrArraySize ? desc->DepthOrArraySize : 1;
    const int want_depth = (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    if (cpu || desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) { hr = cpu ? E_INVALIDARG : E_NOTIMPL; goto fail; }
    if (desc->Format == DXGI_FORMAT_R32_TYPELESS && want_depth) fi = format_info(DXGI_FORMAT_D32_FLOAT);
    if (desc->Format == DXGI_FORMAT_R24G8_TYPELESS && want_depth) fi = format_info(DXGI_FORMAT_D24_UNORM_S8_UINT);
    if (!fi || !fi->kernel || !!fi->depth != want_depth) { hr = fi && fi->kernel ? E_INVALIDARG : DXGI_ERROR_UNSUPPORTED; goto fail; }
    if (desc->Width == 0 || desc->Height == 0 || desc->Width > KMAX_TEXTURE_DIMENSION || desc->Height > KMAX_TEXTURE_DIMENSION) { hr = E_INVALIDARG; goto fail; }
    r->fmt = fi;
    r->mips = desc->MipLevels ? desc->MipLevels : mip_count_for((UINT)desc->Width, desc->Height);
    r->layers = 1;
    r->kind = 0;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) { r->kind = 1; r->layers = depth_or_array; }
    else if (depth_or_array > 1) { r->kind = 2; r->layers = depth_or_array; }
    const int target = (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    if ((r->kind != 0 || r->mips > 1) && (target || want_depth || (r->kind != 0 && r->mips > 1))) { hr = DXGI_ERROR_UNSUPPORTED; goto fail; }
    ad.flags = want_depth ? KA_DEPTH : (target ? (KA_RENDER_TARGET | KA_TEXTURE) : KA_TEXTURE);
    if (present) ad.flags |= KA_PRESENT;
    ad.format = fi->kernel;
    ad.width = (UINT)desc->Width;
    ad.height = desc->Height;
    ad.mips = r->mips;
    ad.kind = r->kind;
    ad.layers = r->layers;
    r->desc.MipLevels = (UINT16)r->mips;
  }
  if (heap) {
    ad.heap = heap->alloc;
    ad.heap_offset = heap_offset;
    r->heap = heap;
    dx_AddRef(OBJ(heap));
  }
  UINT pitch = 0;
  NTSTATUS st = kmt_create_allocation(dev->kdev, &ad, &r->alloc, &pitch, &r->bytes);
  if (!KMT_OK(st)) { hr = hr_from(st); goto fail; }
  if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    r->va = va_register(r, desc->Width);
    if (cpu) {
      r->shadow = mem_alloc((SIZE_T)desc->Width);
      if (!r->shadow) { hr = E_OUTOFMEMORY; goto fail; }
      EnterCriticalSection(&g_lock);
      r->next_cpu = g_cpu_resources;
      g_cpu_resources = r;
      LeaveCriticalSection(&g_lock);
    }
  }
  *out = r;
  return S_OK;
fail:
  dx_Release(OBJ(r));
  return hr;
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommittedResource(Dev *This, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc,
                                                              D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
  Resource *r = NULL;
  if (!props || !desc) return E_INVALIDARG;
  const HRESULT hr = resource_make(This, props, flags, desc, NULL, 0, 0, &r);
  if (FAILED(hr)) return hr;
  return query_out((IUnknown *)r, riid, out);
}

static HRESULT STDMETHODCALLTYPE dev_CreatePlacedResource(Dev *This, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
                                                           const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
  Resource *r = NULL;
  HeapObj *h = (HeapObj *)heap;
  if (!heap || !desc) return E_INVALIDARG;
  const HRESULT hr = resource_make(This, &h->desc.Properties, h->desc.Flags, desc, h, offset, 0, &r);
  if (FAILED(hr)) return hr;
  return query_out((IUnknown *)r, riid, out);
}

static HRESULT STDMETHODCALLTYPE dev_CreateReservedResource(Dev *This, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) {
  return DXGI_ERROR_UNSUPPORTED;
}

static void heap_destroy(DxObj *o) {
  HeapObj *h = (HeapObj *)o;
  if (h->alloc) kmt_destroy_allocation(h->d.dev->kdev, h->alloc);
  dobj_child_destroy(o);
}

static HRESULT STDMETHODCALLTYPE dev_CreateHeap(Dev *This, const D3D12_HEAP_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12HeapVtbl heap_vtbl;
  if (!desc || desc->SizeInBytes == 0 || desc->SizeInBytes > 256ull * 1024 * 1024) return E_INVALIDARG;
  HeapObj *h = dobj_new(sizeof *h, This, &heap_vtbl, g_heap_iids, heap_destroy);
  if (!h) return E_OUTOFMEMORY;
  h->desc = *desc;
  KmtAllocDesc ad;
  memset(&ad, 0, sizeof ad);
  ad.flags = KA_HEAP;
  ad.size = KALIGN(desc->SizeInBytes, 256);
  const NTSTATUS st = kmt_create_allocation(This->kdev, &ad, &h->alloc, NULL, NULL);
  if (!KMT_OK(st)) { dx_Release(OBJ(h)); return hr_from(st); }
  return query_out((IUnknown *)h, riid, out);
}
static D3D12_HEAP_DESC *STDMETHODCALLTYPE heap_GetDesc(HeapObj *This, D3D12_HEAP_DESC *ret) { *ret = This->desc; return ret; }

static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE dev_GetResourceAllocationInfo(Dev *This, D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT mask, UINT count,
                                                                                         const D3D12_RESOURCE_DESC *descs) {
  UINT64 size = 0;
  for (UINT i = 0; i < count; ++i) {
    UINT64 n = 0;
    if (descs[i].Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      n = descs[i].Width;
    } else {
      D3D12_RESOURCE_DESC d = descs[i];
      const UINT mips = d.MipLevels ? d.MipLevels : mip_count_for((UINT)d.Width, d.Height);
      const UINT subs = mips * (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : (d.DepthOrArraySize ? d.DepthOrArraySize : 1));
      get_footprints(&d, 0, subs, 0, NULL, NULL, NULL, &n);
    }
    size += KALIGN(n, KTILE_BYTES);
  }
  ret->SizeInBytes = size;
  ret->Alignment = KTILE_BYTES;
  return ret;
}

static void STDMETHODCALLTYPE dev_GetCopyableFootprints(Dev *This, const D3D12_RESOURCE_DESC *desc, UINT first, UINT count, UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
                                                        UINT *rows, UINT64 *row_sizes, UINT64 *total) {
  get_footprints(desc, first, count, base, layouts, rows, row_sizes, total);
}

static void STDMETHODCALLTYPE dev_GetResourceTiling(Dev *This, ID3D12Resource *resource, UINT *total_tiles, D3D12_PACKED_MIP_INFO *packed, D3D12_TILE_SHAPE *shape, UINT *subresource_count,
                                                    UINT first, D3D12_SUBRESOURCE_TILING *tilings) {
  if (total_tiles) *total_tiles = 0;
  if (packed) memset(packed, 0, sizeof *packed);
  if (shape) memset(shape, 0, sizeof *shape);
  if (subresource_count) *subresource_count = 0;
}

/* ---- ID3D12Resource ----------------------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE res_Map(Resource *This, UINT sub, const D3D12_RANGE *read, void **data) {
  if (sub != 0 || !This->shadow) return E_INVALIDARG;
  if (This->cpu_readback) res_pull(This);
  InterlockedIncrement(&This->map_count);
  This->dirty = 1;
  if (data) *data = This->shadow;
  return S_OK;
}
static void STDMETHODCALLTYPE res_Unmap(Resource *This, UINT sub, const D3D12_RANGE *written) {
  if (This->map_count > 0) InterlockedDecrement(&This->map_count);
  This->dirty = 1;
}
static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE res_GetDesc(Resource *This, D3D12_RESOURCE_DESC *ret) { *ret = This->desc; return ret; }
static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE res_GetGPUVirtualAddress(Resource *This) { return This->va; }
static HRESULT STDMETHODCALLTYPE res_GetHeapProperties(Resource *This, D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS *flags) {
  if (props) *props = This->heap ? This->heap->desc.Properties : This->props;
  if (flags) *flags = This->heap_flags;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE res_WriteToSubresource(Resource *This, UINT sub, const D3D12_BOX *box, const void *src, UINT row_pitch, UINT slice_pitch) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE res_ReadFromSubresource(Resource *This, void *dst, UINT row_pitch, UINT slice_pitch, UINT sub, const D3D12_BOX *box) { return E_NOTIMPL; }
