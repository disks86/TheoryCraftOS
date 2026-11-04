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

/* d3d12.dll: descriptor heaps (ID3D12DescriptorHeap) and the views and samplers written into them. Included by d3d12.c.
 *
 * A CPU descriptor handle is the address of a descriptor in the heap's memory here, as it is on hardware, so the descriptors are written by the CPU (CreateShaderResourceView ...,
 * CopyDescriptors) into `descs`. The kernel's copy of a shader-visible heap (the one the draws read) is brought up to date when a command list that may use it is executed. A
 * descriptor of a render target or depth view is only ever used by the command list's recording, so those heaps have no copy. */

#define DESCRIPTOR_STRIDE 64

enum { DK_NONE, DK_SRV, DK_SRV_BUFFER, DK_UAV, DK_CBV, DK_SAMPLER, DK_RTV, DK_DSV };

/* One descriptor: exactly DESCRIPTOR_STRIDE bytes, the increment size the device reports. */
typedef struct SoftDesc {
  UINT kind;
  UINT alloc;              /* the kernel's allocation */
  UINT64 offset, size;     /* buffer views */
  UINT format;
  UINT mip;
  UINT sampler[4];         /* filter | address_u << 8 | address_v << 16 | mip_filter << 24, lod bias, min lod, max lod (floats as bits) */
  UINT dimension;
  UINT slice;
  UINT pad[2];
} SoftDesc;
typedef char soft_desc_size_check[sizeof(SoftDesc) == DESCRIPTOR_STRIDE ? 1 : -1];

struct DescHeap {
  DObj d;
  D3D12_DESCRIPTOR_HEAP_DESC desc;
  SoftDesc *descs;
  UINT khandle;            /* the kernel's heap (shader-visible heaps of views and samplers) */
  LONG synced;             /* g_desc_epoch when the kernel's copy was written */
  UINT64 gpu_base;
};

static const GUID *const g_dheap_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12DescriptorHeap, NULL};
static UINT64 g_next_gpu_handle = (UINT64)1 << 44;

static void dheap_destroy(DxObj *o) {
  DescHeap *h = (DescHeap *)o;
  if (h->khandle) {
    UINT request[2] = {ESC_DESTROY_DESCRIPTOR_HEAP, h->khandle};
    dev_escape(h->d.dev, request, 2, NULL, 0);
  }
  mem_free(h->descs);
  dobj_child_destroy(o);
}

static HRESULT STDMETHODCALLTYPE dev_CreateDescriptorHeap(Dev *This, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12DescriptorHeapVtbl dheap_vtbl;
  if (!desc || desc->Type >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES || desc->NumDescriptors == 0 || desc->NumDescriptors > 1000000) return E_INVALIDARG;
  const int visible = (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
  const int kernel_side = visible && (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  if (visible && desc->Type > D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) return E_INVALIDARG;
  if (kernel_side && desc->NumDescriptors > 4096) return DXGI_ERROR_UNSUPPORTED;      /* (the kernel's heaps hold at most 4096 descriptors) */
  DescHeap *h = dobj_new(sizeof *h, This, &dheap_vtbl, g_dheap_iids, dheap_destroy);
  if (!h) return E_OUTOFMEMORY;
  h->desc = *desc;
  h->descs = mem_alloc((SIZE_T)desc->NumDescriptors * sizeof(SoftDesc));
  if (!h->descs) { dx_Release(OBJ(h)); return E_OUTOFMEMORY; }
  EnterCriticalSection(&g_lock);
  h->gpu_base = g_next_gpu_handle;
  g_next_gpu_handle += (UINT64)desc->NumDescriptors * DESCRIPTOR_STRIDE + 4096;
  LeaveCriticalSection(&g_lock);
  if (kernel_side) {
    UINT request[3] = {ESC_CREATE_DESCRIPTOR_HEAP, desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1u : 0u, desc->NumDescriptors};
    UINT reply[1] = {0};
    const NTSTATUS st = dev_escape(This, request, 3, reply, 1);
    if (!KMT_OK(st)) { dx_Release(OBJ(h)); return hr_from(st); }
    h->khandle = reply[0];
  }
  return query_out((IUnknown *)h, riid, out);
}

static D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE dheap_GetDesc(DescHeap *This, D3D12_DESCRIPTOR_HEAP_DESC *ret) { *ret = This->desc; return ret; }
static D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE dheap_GetCPUDescriptorHandleForHeapStart(DescHeap *This, D3D12_CPU_DESCRIPTOR_HANDLE *ret) {
  ret->ptr = (SIZE_T)(UINT_PTR)This->descs;
  return ret;
}
static D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE dheap_GetGPUDescriptorHandleForHeapStart(DescHeap *This, D3D12_GPU_DESCRIPTOR_HANDLE *ret) {
  ret->ptr = (This->desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? This->gpu_base : 0;
  return ret;
}

/* The descriptor a CPU handle names, with the heap it is in (0 / NULL when not one the caller may write). */
static SoftDesc *desc_at(D3D12_CPU_DESCRIPTOR_HANDLE handle) { return (SoftDesc *)(UINT_PTR)handle.ptr; }

static void desc_write(D3D12_CPU_DESCRIPTOR_HANDLE handle, const SoftDesc *d) {
  if (!handle.ptr) return;
  *desc_at(handle) = *d;
  InterlockedIncrement(&g_desc_epoch);
}

/* ---- the views ------------------------------------------------------------------------------------------------------------------------------------------------------------------ */

static void STDMETHODCALLTYPE dev_CreateConstantBufferView(Dev *This, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  memset(&d, 0, sizeof d);
  if (desc) {
    UINT64 offset = 0;
    Resource *r = va_find(desc->BufferLocation, &offset);
    if (r) {
      d.kind = DK_CBV;
      d.alloc = r->alloc;
      d.offset = offset;
      d.size = desc->SizeInBytes;
    }
  }
  desc_write(handle, &d);
}

static void STDMETHODCALLTYPE dev_CreateShaderResourceView(Dev *This, ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  Resource *r = (Resource *)resource;
  memset(&d, 0, sizeof d);
  if (r) {
    d.alloc = r->alloc;
    if (r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      UINT stride = 4;
      UINT64 first = 0, count = (r->desc.Width) / 4;
      if (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
        stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride : (desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != DXGI_FORMAT_R32_TYPELESS ? format_bytes(desc->Format) : 4);
        first = desc->Buffer.FirstElement;
        count = desc->Buffer.NumElements;
      }
      d.kind = DK_SRV_BUFFER;
      d.offset = first * stride;
      d.size = count * stride;
    } else {
      d.kind = DK_SRV;
    }
    d.format = desc ? desc->Format : r->desc.Format;
  }
  desc_write(handle, &d);
}

static void STDMETHODCALLTYPE dev_CreateUnorderedAccessView(Dev *This, ID3D12Resource *resource, ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  Resource *r = (Resource *)resource;
  memset(&d, 0, sizeof d);
  if (r && r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    UINT stride = 4;
    UINT64 first = 0, count = r->desc.Width / 4;
    if (desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
      stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride : (desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != DXGI_FORMAT_R32_TYPELESS ? format_bytes(desc->Format) : 4);
      first = desc->Buffer.FirstElement;
      count = desc->Buffer.NumElements;
    }
    d.kind = DK_UAV;
    d.alloc = r->alloc;
    d.offset = first * stride;
    d.size = count * stride;
  }
  desc_write(handle, &d);
}

static void STDMETHODCALLTYPE dev_CreateRenderTargetView(Dev *This, ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  Resource *r = (Resource *)resource;
  memset(&d, 0, sizeof d);
  if (r && r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
    d.kind = DK_RTV;
    d.alloc = r->alloc;
    d.format = desc ? desc->Format : r->desc.Format;
    if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) d.mip = desc->Texture2D.MipSlice;
  }
  desc_write(handle, &d);
}

static void STDMETHODCALLTYPE dev_CreateDepthStencilView(Dev *This, ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  Resource *r = (Resource *)resource;
  memset(&d, 0, sizeof d);
  if (r && r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
    d.kind = DK_DSV;
    d.alloc = r->alloc;
    d.format = desc ? desc->Format : r->desc.Format;
  }
  desc_write(handle, &d);
}

/* The words of a sampler (D3D12_FILTER / D3D12_TEXTURE_ADDRESS_MODE) the way the kernel has them: the first word packs filter, address modes and mip filter; rest[0..2] are the
 * LOD bias, minimum and maximum LOD as bits. The kernel has repeat and clamp only: mirrored addresses repeat, border and mirror-once clamp. */
static UINT sampler_words(UINT filter, UINT au, UINT av, float bias, float min_lod, float max_lod, UINT *rest) {
  const UINT linear = (filter & 0x14u) != 0 || filter == D3D12_FILTER_ANISOTROPIC;
  const UINT mip = (max_lod <= 0.0f) ? 0u : (((filter & 1u) || filter == D3D12_FILTER_ANISOTROPIC) ? 2u : 1u);
  UINT a[2] = {au, av};
  for (int i = 0; i < 2; ++i) a[i] = (a[i] == D3D12_TEXTURE_ADDRESS_MODE_WRAP || a[i] == D3D12_TEXTURE_ADDRESS_MODE_MIRROR) ? 0u : 1u;
  rest[0] = bits_of(bias);
  rest[1] = bits_of(min_lod);
  rest[2] = bits_of(max_lod);
  return linear | (a[0] << 8) | (a[1] << 16) | (mip << 24);
}

static void STDMETHODCALLTYPE dev_CreateSampler(Dev *This, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  memset(&d, 0, sizeof d);
  if (desc) {
    d.kind = DK_SAMPLER;
    d.sampler[0] = sampler_words(desc->Filter, desc->AddressU, desc->AddressV, desc->MipLODBias, desc->MinLOD, desc->MaxLOD, &d.sampler[1]);
  }
  desc_write(handle, &d);
}

static void STDMETHODCALLTYPE dev_CopyDescriptorsSimple(Dev *This, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  if (!dst.ptr || !src.ptr) return;
  memmove(desc_at(dst), desc_at(src), (SIZE_T)count * sizeof(SoftDesc));
  InterlockedIncrement(&g_desc_epoch);
}

static void STDMETHODCALLTYPE dev_CopyDescriptors(Dev *This, UINT dst_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dst, const UINT *dst_sizes, UINT src_count,
                                                  const D3D12_CPU_DESCRIPTOR_HANDLE *src, const UINT *src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  UINT si = 0, sused = 0;
  for (UINT di = 0; di < dst_count; ++di) {
    const UINT dsize = dst_sizes ? dst_sizes[di] : 1;
    for (UINT k = 0; k < dsize; ++k) {
      while (si < src_count && sused >= (src_sizes ? src_sizes[si] : 1)) { ++si; sused = 0; }
      if (si >= src_count) goto done;
      *desc_at((D3D12_CPU_DESCRIPTOR_HANDLE){dst[di].ptr + (SIZE_T)k * DESCRIPTOR_STRIDE}) = *desc_at((D3D12_CPU_DESCRIPTOR_HANDLE){src[si].ptr + (SIZE_T)sused * DESCRIPTOR_STRIDE});
      ++sused;
    }
  }
done:
  InterlockedIncrement(&g_desc_epoch);
}

/* ---- the kernel's copy of a shader-visible heap -------------------------------------------------------------------------------------------------------------------------- */

/* Writes the entries of the heap to the kernel. */
static void dheap_sync(DescHeap *h) {
  if (!h->khandle) return;
  const LONG epoch = g_desc_epoch;
  if (h->synced == epoch) return;
  const UINT count = h->desc.NumDescriptors;
  for (UINT first = 0; first < count; first += 1000) {
    const UINT n = count - first < 1000 ? count - first : 1000;
    UINT *request = mem_alloc((SIZE_T)(3 + n * 14) * 4);
    if (!request) return;
    UINT w = 0;
    request[w++] = ESC_WRITE_DESCRIPTORS;
    request[w++] = h->khandle;
    request[w++] = n;
    for (UINT i = 0; i < n; ++i) {
      const SoftDesc *d = &h->descs[first + i];
      UINT kind = 0;
      switch (d->kind) {
        case DK_SRV: case DK_SRV_BUFFER: kind = 1; break;
        case DK_UAV: kind = 2; break;
        case DK_CBV: kind = 3; break;
        case DK_SAMPLER: kind = 4; break;
        default: kind = 0; break;
      }
      request[w++] = first + i;
      request[w++] = kind;
      request[w++] = d->alloc;
      request[w++] = (UINT)d->offset;
      request[w++] = (UINT)(d->offset >> 32);
      request[w++] = (UINT)d->size;
      request[w++] = (UINT)(d->size >> 32);
      request[w++] = d->sampler[0] & 0xFF;
      request[w++] = (d->sampler[0] >> 8) & 0xFF;
      request[w++] = (d->sampler[0] >> 16) & 0xFF;
      request[w++] = (d->sampler[0] >> 24) & 0xFF;
      request[w++] = d->sampler[1];
      request[w++] = d->sampler[2];
      request[w++] = d->sampler[3];
    }
    NTSTATUS st = dev_escape(h->d.dev, request, w, NULL, 0);
    if (!KMT_OK(st)) {
      /* (a descriptor of a resource that is gone, or one the kernel does not take: the others still go, one by one) */
      for (UINT i = 0; i < n; ++i) {
        UINT one[17];
        memcpy(one, request, 3 * 4);
        one[2] = 1;
        memcpy(one + 3, request + 3 + i * 14, 14 * 4);
        dev_escape(h->d.dev, one, 17, NULL, 0);
      }
    }
    mem_free(request);
  }
  h->synced = epoch;
}
