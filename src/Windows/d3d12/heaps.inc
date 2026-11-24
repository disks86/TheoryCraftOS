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
  UINT64 offset, size;     /* buffer views. A texture's view of planes (DK_SRV): `size` is its array mirror (alternate | SRV_ALTERNATE: the kernel's Descriptor::alternate), and
                            * `offset` has SRV_MIRRORS when it or the alternate is a mirror the kernel brings up to date (res_mirror) */
  UINT format;
  UINT mip;
  UINT sampler[4];         /* filter | address_u << 8 | address_v << 16 | mip_filter << 24, lod bias, min lod, max lod (floats as bits) */
  UINT dimension;
  UINT slice;
  UINT stride;             /* buffer unordered access views: the bytes of an element (ClearUnorderedAccessView* rectangles count elements) */
  UINT pad;
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

#define SRV_ALTERNATE ((UINT64)0xFFFFFFFFu << 32)      /* (the size word's high half that says the low half is an alternate: the kernel's WriteDescriptors) */
#define SRV_MIRRORS 1u
/* The allocation a view of a planar texture (NV12, P010, P016) names: its plane (the PlaneSlice; a view format of one channel is plane 0, of two plane 1) of its first slice
 * (a view of several slices gives the shaders that declare an array a mirror of them as well: plane_srv). Any other texture: its allocation. */
static UINT view_alloc(const Resource *r, DXGI_FORMAT view_format, UINT plane, UINT slice) {
  if (!r->planes) return r->alloc;
  /* (V208 / V408: three R8 planes, named by the PlaneSlice alone) */
  if (view_format != DXGI_FORMAT_UNKNOWN && r->planes == 2) {
    const FormatInfo *vf = format_info(view_format);
    if (vf && vf->narrow) plane = vf->narrow == NARROW_R8G8 || vf->narrow == NARROW_R16G16 ? 1 : 0;
  }
  if (plane >= r->planes || slice >= r->layers) return 0;
  return r->plane_alloc[plane * r->layers + slice];
}

/* A shader resource view of planes of a planar texture: plane `plane` of the slices from `first` (`count` of them; a TEXTURE2D view: slice 0). The view's image is the plane of
 * slice `first` - what a shader that declares a 2D texture reads -, or, for an integer view (R8_UINT, R8G8_UINT, R16_UINT, R16G16_UINT: plane_integer_view), a mirror of it in
 * the integer format of its bits; an array view also has a mirror of all its slices (the alternate a shader that declares a 2D array reads). 0 in d.kind when the view cannot be
 * made (a slice or plane the texture does not have, or a mirror the kernel refused). */
static void plane_srv(Resource *r, DXGI_FORMAT format, UINT plane, UINT first, UINT count, int array, SoftDesc *d) {
  UINT channels = 0;
  const int integer = format != DXGI_FORMAT_UNKNOWN && plane_integer_view(r, format, &plane, &channels);
  if (!integer) d->alloc = view_alloc(r, format, plane, first);
  else d->alloc = res_mirror(r, plane, first, 1, 0, MIRROR_INTEGER, 0);
  if (plane >= r->planes || first >= r->layers || count == 0 || count > r->layers - first) d->alloc = 0;
  UINT alternate = 0;
  if (d->alloc && array) {
    alternate = res_mirror(r, plane, first, count, 1, integer ? MIRROR_INTEGER : 0, 0);
    if (!alternate) d->alloc = 0;
  }
  if (!d->alloc) { d->kind = 0; return; }
  d->offset = integer || alternate ? SRV_MIRRORS : 0;
  d->size = alternate ? SRV_ALTERNATE | alternate : 0;
}

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
      /* (a packed YUV texture is seen through a view of its pixels' format only) */
      if (format_packed_yuv(r->desc.Format) && (!desc || !packed_yuv_view_ok(r->desc.Format, desc->Format, 0))) d.kind = 0;
      if (r->planes) {
        if (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY)
          plane_srv(r, desc->Format, desc->Texture2DArray.PlaneSlice, desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize, 1, &d);
        else if (!desc || desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
          plane_srv(r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, desc ? desc->Texture2D.PlaneSlice : 0, 0, 1, 0, &d);
        else d.kind = 0;      /* (a planar texture is 2D) */
      }
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
      if (!desc->Buffer.StructureByteStride && !(desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW)) d.format = desc->Format;      /* (typed: ClearUnorderedAccessView* converts to it) */
    }
    d.kind = DK_UAV;
    d.stride = stride;
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
    if (format_packed_yuv(r->desc.Format) && (!desc || !packed_yuv_view_ok(r->desc.Format, desc->Format, 1))) d.kind = 0;
    if (r->planes) {
      UINT plane = 0, slice = 0, channels = 0;
      if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) plane = desc->Texture2D.PlaneSlice;
      if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) { plane = desc->Texture2DArray.PlaneSlice; slice = desc->Texture2DArray.FirstArraySlice; }
      /* (an integer view: a mirror of the plane in the integer format of its bits, drawn into, whose channels go back to the plane after each pass; one of several slices
       * draws into the first, as any view of several slices does - the kernel takes no SV_RenderTargetArrayIndex) */
      if (desc && plane_integer_view(r, desc->Format, &plane, &channels))
        d.alloc = (r->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? res_mirror(r, plane, slice, 1, 0, MIRROR_INTEGER | MIRROR_WRITE_BACK, channels) : 0;
      else d.alloc = view_alloc(r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, plane, slice);
      if (!d.alloc) d.kind = 0;
    }
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
    /* (the size of the buffer, for the colour target a pass with this view alone is given: cmdlist.inc depth_companion) */
    d.size = (UINT64)(UINT)r->desc.Width | ((UINT64)r->desc.Height << 32);
    d.offset = r->desc.SampleDesc.Count ? r->desc.SampleDesc.Count : 1;
  }
  desc_write(handle, &d);
}

/* The words of a sampler (D3D12_FILTER / D3D12_TEXTURE_ADDRESS_MODE) the way the kernel has them: the first word packs filter, address modes and mip filter; rest[0..2] are the
 * LOD bias, minimum and maximum LOD as bits. An address byte is the kernel's mode (0 wrap, 1 clamp, 2 mirror, 3 border, 4 mirror once) and, in the byte of U, the border colour
 * in bits 4-5 (0 transparent black, 1 opaque black, 2 opaque white: the three a static sampler can have; another colour is the nearest of them). A device with wrap and
 * clamp alone uses the nearer of the two (the kernel does that). */
static float lod_within(float value, float limit) { return !(value == value) ? 0.0f : value > limit ? limit : value < -limit ? -limit : value; }
static UINT sampler_words(UINT filter, UINT au, UINT av, const float *border, float bias, float min_lod, float max_lod, UINT *rest) {
  const UINT linear = (filter & 0x14u) != 0 || filter == D3D12_FILTER_ANISOTROPIC;
  const UINT mip = (max_lod <= 0.0f) ? 0u : (((filter & 1u) || filter == D3D12_FILTER_ANISOTROPIC) ? 2u : 1u);
  UINT a[2] = {au, av};
  for (int i = 0; i < 2; ++i)
    a[i] = a[i] == D3D12_TEXTURE_ADDRESS_MODE_WRAP ? 0u : a[i] == D3D12_TEXTURE_ADDRESS_MODE_MIRROR ? 2u : a[i] == D3D12_TEXTURE_ADDRESS_MODE_BORDER ? 3u :
           a[i] == D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE ? 4u : 1u;
  if (border) a[0] |= (border[3] < 0.5f ? 0u : border[0] + border[1] + border[2] < 1.5f ? 1u : 2u) << 4;
  /* (the kernel takes a bias and a minimum within 100 and a maximum within 1000: "no limit" - FLT_MAX, the usual MaxLOD - is the most it takes; a
   * value that is not a number is 0) */
  rest[0] = bits_of(lod_within(bias, 100.0f));
  rest[1] = bits_of(lod_within(min_lod, 100.0f));
  rest[2] = bits_of(lod_within(max_lod, 1000.0f));
  return linear | (a[0] << 8) | (a[1] << 16) | (mip << 24);
}

static void STDMETHODCALLTYPE dev_CreateSampler(Dev *This, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  memset(&d, 0, sizeof d);
  if (desc) {
    d.kind = DK_SAMPLER;
    d.sampler[0] = sampler_words(desc->Filter, desc->AddressU, desc->AddressV, desc->BorderColor, desc->MipLODBias, desc->MinLOD, desc->MaxLOD, &d.sampler[1]);
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
      request[w++] = ((d->sampler[0] >> 8) & 0xF) | (((d->sampler[0] >> 12) & 0xF) << 8);      /* (the mode, and the border colour above it) */
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
