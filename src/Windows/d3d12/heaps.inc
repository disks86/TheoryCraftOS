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

enum { DK_NONE, DK_SRV, DK_SRV_BUFFER, DK_UAV, DK_CBV, DK_SAMPLER, DK_RTV, DK_DSV, DK_UAV_TEXTURE };
#define DESCRIPTOR_SAMPLER_EXTRAS (1u << 16)      /* the escape WriteDescriptors' kind word: six more words of a sampler follow (dxgk::kDescriptorSamplerExtras) */

/* Whether a format is one of the kernel's element formats of typed views of buffers (graphics::TexelFormat, which numbers them as DXGI does): 1 for a view that is read, 2 for one
 * that can also be an unordered access view (stored), 0 for none - typeless, sRGB, depth, block-compressed and video formats. */
static UINT texel_buffer_format(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: case DXGI_FORMAT_A8_UNORM: case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
      return 1;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
      return 2;
    default: return 0;
  }
}

/* One descriptor: exactly DESCRIPTOR_STRIDE bytes, the increment size the device reports. */
typedef struct SoftDesc {
  UINT kind;
  UINT alloc;              /* the kernel's allocation */
  UINT64 offset, size;     /* buffer views. A texture's view of planes (DK_SRV): `size` is its array mirror (alternate | SRV_ALTERNATE: the kernel's Descriptor::alternate), and
                            * `offset` has SRV_MIRRORS when it or the alternate is a mirror the kernel brings up to date (res_mirror) */
  UINT format;
  UINT mip;                /* the level of a render target, depth stencil or unordered access view; a texture's shader resource view: its first level */
  UINT sampler[4];         /* filter | address_u << 8 | address_v << 16 | mip_filter << 24, lod bias, min lod, max lod (floats as bits). A view of a buffer (DK_SRV_BUFFER, DK_UAV): 0, then
                            * the element format of a typed view (0: raw or structured), and an unordered access view's counter - the kernel's allocation and the word's offset (0: none).
                            * A texture's shader resource view (DK_SRV): 0, then its plane of a depth buffer (SRV_STENCIL: the stencil), 0, 0.
                            * A sampler (DK_SAMPLER) keeps its comparison in `offset` (sampler_comparison: the kernel's Descriptor offset low word), its anisotropy in `format` (1: none),
                            * in `pad` 1 when it has a border colour of its own (an address mode BORDER) and that colour's bits in `size` (red, green << 32), `slice` (blue) and `stride` (alpha),
                            * and in `dimension` 1 when its coordinates are in texels (D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES: the filter word's bit 8) */
  UINT dimension;
  UINT slice;              /* the first layer of a view of layers */
  UINT stride;             /* buffer unordered access views: the bytes of an element (ClearUnorderedAccessView* rectangles count elements); render target, depth stencil and
                            * texture unordered access views: how many layers from `slice` (an array's slices, a volume's depth slices at the level); a texture's shader resource
                            * view: how many levels from `mip` (0: to the last) */
  UINT pad;                /* a texture's unordered access view through a mirror: the plane and the first slice it is of, and the level of a narrow texture (plane | first << 8 |
                            * level << 16), what a clear writes; a texture's
                            * shader resource view: how many layers from `slice` (0: to the last; a cube's six, a cube array's six for each cube) */
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
#define SRV_STENCIL 1u      /* (DK_SRV's sampler[1]: the view is of a depth buffer's stencil plane - the lod-bias word 1 in the kernel's WriteDescriptors) */
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

/* The family of a narrow format: 1 R8, 2 R8G8, 3 R16, 4 R16G16 (each with its typeless, unorm, snorm, uint, sint and - 16-bit - float forms; D3D12's casting within a typeless
 * family), 0 for the others. */
static UINT narrow_family(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SINT: return 1;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SINT: return 2;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R16_FLOAT: return 3;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R16G16_FLOAT:
      return 4;
    default: return 0;
  }
}
/* How a view of `f` sees a typeless narrow texture the kernel keeps natively (format_native_narrow): 0 as its own image (the resource's format, UNKNOWN, or the unorm one of its
 * family), 1 through a mirror in the kernel format *kformat (the family's other kinds; the adapter samples it, and `need` - KFC_* - of it), -1 not at all (another family, a
 * typeless view format, or a kind the adapter lacks). Any other texture: 0. */
static int typed_view_kind(const Dev *dev, const Resource *r, DXGI_FORMAT f, UINT need, UINT *kformat) {
  *kformat = 0;
  if (!format_native_narrow(r->fmt) || f == DXGI_FORMAT_UNKNOWN || f == r->desc.Format) return 0;
  if (narrow_family(f) != narrow_family(r->desc.Format)) return -1;
  const FormatInfo *native = native_narrow_entry(f);
  if (native && native->kernel == r->fmt->kernel) return 0;      /* (the unorm view of the family's typeless texture) */
  const FormatInfo *vf = format_info(f);
  if (!vf || native || vf->narrow || !vf->kernel || vf->kernel < KF_FIRST_EXTRA) return -1;      /* (a typeless one: no view of that format) */
  const UINT caps = dev_format_caps(dev, vf->kernel);
  if ((caps & (KFC_SAMPLED | need)) != (KFC_SAMPLED | need)) return -1;
  *kformat = vf->kernel;
  return 1;
}
/* The channels of a narrow family's view as a write mask (red, or red and green). */
static UINT narrow_family_mask(DXGI_FORMAT f) { const UINT n = narrow_family(f); return n == 2 || n == 4 ? 0x3 : 0x1; }

static int view_layers_at(const Resource *r, UINT level, UINT first, UINT size, UINT *out_first, UINT *out_count);
static int view_layers(const Resource *r, UINT first, UINT size, UINT *out_first, UINT *out_count) { return view_layers_at(r, 0, first, size, out_first, out_count); }

/* The levels and layers a texture's shader resource view shows (SoftDesc mip / stride, slice / pad; the kernel's Descriptor::view): MostDetailedMip and MipLevels (-1: to the last), the
 * array slices of an array view (a TEXTURE2D / TEXTURE1D view: the first slice alone), a cube's six faces from the first, a cube array's six for each cube from First2DArrayFace.
 * 0 for levels or layers the resource does not have. */
static int srv_view(const Resource *r, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, SoftDesc *d) {
  d->mip = 0, d->stride = 0, d->slice = 0, d->pad = 0, d->sampler[2] = 0;
  if (!desc) return 1;
  UINT first_level = 0, levels = 0xFFFFFFFFu, first = 0, count = 0xFFFFFFFFu;
  FLOAT clamp = 0.0f;
  switch (desc->ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D: first_level = desc->Texture1D.MostDetailedMip, levels = desc->Texture1D.MipLevels, count = 1, clamp = desc->Texture1D.ResourceMinLODClamp; break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
      first_level = desc->Texture1DArray.MostDetailedMip, levels = desc->Texture1DArray.MipLevels, first = desc->Texture1DArray.FirstArraySlice, count = desc->Texture1DArray.ArraySize;
      clamp = desc->Texture1DArray.ResourceMinLODClamp;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2D: first_level = desc->Texture2D.MostDetailedMip, levels = desc->Texture2D.MipLevels, count = 1, clamp = desc->Texture2D.ResourceMinLODClamp; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
      first_level = desc->Texture2DArray.MostDetailedMip, levels = desc->Texture2DArray.MipLevels, first = desc->Texture2DArray.FirstArraySlice, count = desc->Texture2DArray.ArraySize;
      clamp = desc->Texture2DArray.ResourceMinLODClamp;
      break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS: count = 1; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: first = desc->Texture2DMSArray.FirstArraySlice, count = desc->Texture2DMSArray.ArraySize; break;
    case D3D12_SRV_DIMENSION_TEXTURE3D: first_level = desc->Texture3D.MostDetailedMip, levels = desc->Texture3D.MipLevels, clamp = desc->Texture3D.ResourceMinLODClamp; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
      first_level = desc->TextureCube.MostDetailedMip, levels = desc->TextureCube.MipLevels, count = 6, clamp = desc->TextureCube.ResourceMinLODClamp;
      break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
      first_level = desc->TextureCubeArray.MostDetailedMip, levels = desc->TextureCubeArray.MipLevels, first = desc->TextureCubeArray.First2DArrayFace;
      count = desc->TextureCubeArray.NumCubes > 0xFFFFFFFFu / 6 ? 0 : desc->TextureCubeArray.NumCubes * 6;
      clamp = desc->TextureCubeArray.ResourceMinLODClamp;
      break;
    default: return 1;      /* (UNKNOWN: the whole of it) */
  }
  /* (ResourceMinLODClamp: the most detailed level a lookup takes, counted in the resource's levels - not the view's -, which the kernel raises the sampler's MinLOD to (the
   * descriptor's min_lod word); below 0 or not a number is no clamp, as the debug layer's error leaves it on Windows, and a clamp past the view's last level reads its last) */
  if (clamp > 0.0f) d->sampler[2] = bits_of(clamp > 1000.0f ? 1000.0f : clamp);
  if (first_level >= r->mips || (levels != 0xFFFFFFFFu && (levels == 0 || levels > r->mips - first_level))) return 0;
  const UINT layers = r->kind == 0 || r->kind == 1 ? 1 : r->layers;
  if (count == 0xFFFFFFFFu) count = first < layers ? layers - first : 0;
  if (r->kind != 1 && (count == 0 || first >= layers || count > layers - first)) return 0;
  /* (the kernel's counts: 0 for "to the last") */
  d->mip = first_level;
  d->stride = levels == 0xFFFFFFFFu || levels == r->mips - first_level ? 0 : levels;
  d->slice = r->kind == 1 ? 0 : first;
  d->pad = r->kind == 1 || first + count == layers ? (first == 0 ? 0 : layers - first) : count;
  return 1;
}

/* A shader resource view of a depth buffer (one made without D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE, on a device that reads them: the kernel's KA_DEPTH | KA_TEXTURE). Its
 * format names the plane, as DXGI's are: the depth of D32_FLOAT / R32_TYPELESS through R32_FLOAT, of D16_UNORM / R16_TYPELESS through R16_UNORM, of D24_UNORM_S8_UINT /
 * R24G8_TYPELESS through R24_UNORM_X8_TYPELESS (PlaneSlice 0) and of D32_FLOAT_S8X24_UINT / R32G8X24_TYPELESS through R32_FLOAT_X8X24_TYPELESS; the stencil through
 * X24_TYPELESS_G8_UINT / X32_TYPELESS_G8X24_UINT (PlaneSlice 1), read as integers in green. The depth reads as (depth, 0, 0, 1) - every depth is a float in the kernel, so
 * a D16 or D24 buffer's is not rounded to its bits. A 1D or 2D view (of a slice of an array too), an array view of any of its slices, a cube or a cube array view of its faces,
 * of any of its levels from MostDetailedMip, with a ResourceMinLODClamp, as for a colour texture (srv_view). Any other format, a typed depth format, a dimension it cannot be seen
 * as (a volume, multisampled) or a buffer that cannot be read is a null view (the debug layer's error on Windows). */
static void depth_srv(const Resource *r, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, SoftDesc *d) {
  d->kind = 0;
  if (!desc || r->desc.SampleDesc.Count > 1) return;
  const DXGI_FORMAT res = r->desc.Format;
  const int d32 = res == DXGI_FORMAT_D32_FLOAT || res == DXGI_FORMAT_R32_TYPELESS, d16 = res == DXGI_FORMAT_D16_UNORM || res == DXGI_FORMAT_R16_TYPELESS;
  const int d24s8 = res == DXGI_FORMAT_D24_UNORM_S8_UINT || res == DXGI_FORMAT_R24G8_TYPELESS, d32s8 = res == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || res == DXGI_FORMAT_R32G8X24_TYPELESS;
  int stencil = 0;
  switch (desc->Format) {
    case DXGI_FORMAT_R32_FLOAT: if (!d32) return; break;
    case DXGI_FORMAT_R16_UNORM: if (!d16) return; break;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: if (!d24s8) return; break;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: if (!d32s8) return; break;
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: if (!d24s8) return; stencil = 1; break;
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: if (!d32s8) return; stencil = 1; break;
    default: return;
  }
  UINT plane = 0;
  const int cube = r->kind == 3 || r->kind == 5 || (r->kind == 2 && r->layers % 6 == 0);      /* (an array of whole cubes may be seen as cubes) */
  switch (desc->ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE2D: plane = desc->Texture2D.PlaneSlice; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: plane = desc->Texture2DArray.PlaneSlice; break;
    case D3D12_SRV_DIMENSION_TEXTURE1D: case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE: case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: if (!cube) return; break;
    default: return;
  }
  /* (the plane the format names: 1 the stencil's) */
  if (plane != (UINT)stencil || r->kind == 1) return;
  d->kind = DK_SRV;
  d->alloc = r->alloc;
  if (!srv_view(r, desc, d)) { d->kind = 0; return; }      /* (its levels and layers, and the clamp) */
  d->sampler[1] = stencil ? SRV_STENCIL : 0;
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
      int typed = 0;
      if (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
        stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride : (desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != DXGI_FORMAT_R32_TYPELESS ? format_bytes(desc->Format) : 4);
        first = desc->Buffer.FirstElement;
        count = desc->Buffer.NumElements;
        /* (a typed view - not structured, not raw - is of elements of its format: the kernel's typed view of a buffer, Buffer<>) */
        typed = !desc->Buffer.StructureByteStride && !(desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) && desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != DXGI_FORMAT_R32_TYPELESS;
      }
      d.kind = DK_SRV_BUFFER;
      if (typed) {
        if (texel_buffer_format(desc->Format)) d.sampler[1] = desc->Format;
        else d.kind = 0;      /* (no typed view of this format) */
      }
      d.offset = first * stride;
      d.size = count * stride;
    } else if (r->fmt && r->fmt->depth) {
      depth_srv(r, desc, &d);
    } else {
      d.kind = DK_SRV;
      /* (a packed YUV texture is seen through a view of its pixels' format only) */
      if (format_packed_yuv(r->desc.Format) && (!desc || !packed_yuv_view_ok(r->desc.Format, desc->Format, 0))) d.kind = 0;
      if (!r->planes && !srv_view(r, desc, &d)) d.kind = 0;      /* (the levels and layers it shows) */
      /* (a typeless narrow texture the kernel keeps natively, seen as another kind of its family: through a mirror of the view's levels and layers in the view's format, which the
       * kernel brings up to date when the view is used; the view is then all of the mirror - of a volume, a volume of the view's levels, every slice of each) */
      UINT kf = 0;
      const int typed = d.kind && desc ? typed_view_kind(This, r, desc->Format, 0, &kf) : 0;
      if (typed < 0) d.kind = 0;
      if (typed > 0) {
        const UINT layers = r->kind == 0 || r->kind == 1 ? 1 : r->layers;
        const UINT levels = d.stride ? d.stride : r->mips - d.mip, count = d.pad ? d.pad : layers - d.slice;
        const int array = desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY || desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE1DARRAY ||
                          desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE || desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        d.alloc = r->kind == 1 ? typed_mirror(r, kf, d.mip, levels, 0, mip_extent(r->layers, d.mip), 0, 0, 0) : typed_mirror(r, kf, d.mip, levels, d.slice, count, array, 0, 0);
        d.mip = d.stride = d.slice = d.pad = 0;
        d.offset = SRV_MIRRORS;
        if (!d.alloc) d.kind = 0;
      }
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

/* The layers a view of several slices names: `first` and `size` of an array view (an array of 1D textures is an array of rows), of a volume's FirstWSlice / WSize at level `level`
 * (-1: to the end of the level - a volume's level has half the slices of the one above it), within the resource's layers; 0 when they are not. */
static int view_layers_at(const Resource *r, UINT level, UINT first, UINT size, UINT *out_first, UINT *out_count) {
  const UINT layers = r->kind == 0 ? 1 : r->kind == 1 ? mip_extent(r->layers, level) : r->layers;
  if (size == 0xFFFFFFFFu) size = first < layers ? layers - first : 0;
  if (size == 0 || first >= layers || size > layers - first) return 0;
  *out_first = first;
  *out_count = size;
  return 1;
}

/* An unordered access view of a texture (a storage image of the kernel's, DK_UAV_TEXTURE): a level and layers of the resource's image, seen as the view's format - the kernel's
 * image of a typeless resource's views of the same pixel size. The narrow formats (R8, R8G8, R16, R16G16: the kernel keeps them widened) and the planes of a planar texture are
 * written through a mirror of the plane, or of the single image of a narrow texture, that gives its channels back after each dispatch or pass (MIRROR_WRITE_BACK; an
 * integer view's mirror in the integer format of its bits): so a store leaves the others as zeros and the maximum alpha, as the widened image keeps them. */
static void texture_uav(Dev *dev, Resource *r, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, SoftDesc *d) {
  d->kind = 0;
  if (!r->fmt || !(r->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !(dev->caps.features2 & DEV_FEATURE2_STORAGE_IMAGES)) return;
  const DXGI_FORMAT format = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : r->desc.Format;
  const FormatInfo *vf = format_info(format);
  UINT mip = 0, first = 0, count = 1, plane = 0;
  const D3D12_UAV_DIMENSION dim = desc ? desc->ViewDimension : r->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_UAV_DIMENSION_TEXTURE3D
                                  : r->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? (r->kind == 2 ? D3D12_UAV_DIMENSION_TEXTURE1DARRAY : D3D12_UAV_DIMENSION_TEXTURE1D)
                                  : r->kind == 2 || r->kind == 3 || r->kind == 5 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
  int ok = 1;
  switch (dim) {
    case D3D12_UAV_DIMENSION_TEXTURE1D: mip = desc ? desc->Texture1D.MipSlice : 0; ok = r->kind == 0; break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
      mip = desc ? desc->Texture1DArray.MipSlice : 0;
      ok = r->kind != 1 && (!desc || view_layers(r, desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize, &first, &count));
      if (!desc) count = r->kind == 0 ? 1 : r->layers;
      break;
    case D3D12_UAV_DIMENSION_TEXTURE2D: mip = desc ? desc->Texture2D.MipSlice : 0; plane = desc ? desc->Texture2D.PlaneSlice : 0; ok = r->kind == 0 || r->planes; break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
      mip = desc ? desc->Texture2DArray.MipSlice : 0;
      plane = desc ? desc->Texture2DArray.PlaneSlice : 0;
      if (r->planes) {
        first = desc ? desc->Texture2DArray.FirstArraySlice : 0;
        count = desc ? (desc->Texture2DArray.ArraySize == 0xFFFFFFFFu ? r->layers - (first < r->layers ? first : r->layers) : desc->Texture2DArray.ArraySize) : r->layers;
        ok = first < r->layers && count >= 1 && count <= r->layers - first;
      } else {
        ok = r->kind != 1 && (!desc || view_layers(r, desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize, &first, &count));
        if (!desc) count = r->kind == 0 ? 1 : r->layers;
      }
      break;
    case D3D12_UAV_DIMENSION_TEXTURE3D:
      mip = desc ? desc->Texture3D.MipSlice : 0;
      ok = r->kind == 1 && (!desc || view_layers_at(r, mip, desc->Texture3D.FirstWSlice, desc->Texture3D.WSize, &first, &count));
      if (!desc) count = r->layers;
      break;
    default: ok = 0; break;
  }
  /* (an integer view of a plane - R8_UINT, R8G8_UINT, R16_UINT, R16G16_UINT - has no format of its own here: its mirror's is the integer one of the plane's bits) */
  UINT probe_plane = plane, probe_channels = 0;
  const int plane_integer = r->planes && plane_integer_view(r, format, &probe_plane, &probe_channels);
  if (!ok || (!vf && !plane_integer) || (vf && (vf->depth || vf->compressed || vf->kernel == KF_BGRA8_SRGB || vf->kernel == KF_RGBA8_SRGB)) || mip >= r->mips) return;
  if (format_packed_yuv(r->desc.Format) && !packed_yuv_view_ok(r->desc.Format, format, 0)) return;      /* (a packed YUV texture: through a view of its pixels' format) */
  if (format_native_narrow(r->fmt)) {
    /* (a narrow texture the kernel keeps natively: its own format's view - and the unorm one of a typeless texture - is of its image; another kind of its family is a write-back
     * mirror of the view's level and layers in the view's format, its channels given back after each dispatch or pass) */
    UINT kf = 0;
    const int typed = typed_view_kind(dev, r, format, KFC_STORAGE, &kf);
    if (typed < 0) return;
    if (typed > 0) {
      const int array = dim == D3D12_UAV_DIMENSION_TEXTURE2DARRAY || dim == D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
      d->alloc = typed_mirror(r, kf, mip, 1, first, count, array, MIRROR_WRITE_BACK | MIRROR_UAV, narrow_family_mask(format));
      if (!d->alloc) return;
      d->mip = 0, d->slice = 0, d->stride = count;
      d->format = format;
      d->dimension = dim;
      d->offset = SRV_MIRRORS;
      d->pad = (first & 0xFF) << 8 | mip << 16;      /* (what a clear writes: the texture's level and first layer, the bytes as they are - no narrow copy) */
      d->size = (UINT64)r->alloc;
      d->kind = DK_UAV_TEXTURE;
      return;
    }
    vf = r->fmt;
  }
  d->mip = mip;
  d->slice = first;
  d->stride = count;
  d->format = format;
  d->dimension = dim;
  if (r->planes || (vf && vf->narrow)) {
    /* (through a write-back mirror: of the plane of a planar texture's slices, or of a narrow texture's one image; the view's channels go back) */
    UINT channels = 0;
    const int integer = r->planes && plane_integer_view(r, format, &plane, &channels);
    if (!integer) {
      if (!vf || !vf->narrow) return;
      if (r->planes && r->planes == 2) plane = vf->narrow == NARROW_R8G8 || vf->narrow == NARROW_R16G16 ? 1 : 0;
      channels = format_write_mask(format);
    }
    /* (a narrow texture's mirror is of its one image, or - on a device with levels of every image - of the view's level and layers of it, a volume's of the view's slices at its
     * level: narrow_mirror) */
    const int levels_mirror = !r->planes && (r->kind != 0 || r->mips != 1);
    if (r->planes ? plane >= r->planes : plane != 0) return;
    const UINT source = r->planes ? (first < r->layers ? r->plane_alloc[plane * r->layers + first] : 0) : r->alloc;
    const UINT mirror_mode = (integer ? MIRROR_INTEGER : 0) | MIRROR_WRITE_BACK | MIRROR_UAV;
    d->alloc = levels_mirror ? narrow_mirror(r, mip, first, count, dim == D3D12_UAV_DIMENSION_TEXTURE2DARRAY || dim == D3D12_UAV_DIMENSION_TEXTURE1DARRAY, mirror_mode, channels)
                             : res_mirror(r, plane, first, count, count > 1, mirror_mode, channels);
    if (!d->alloc || !source) return;
    d->offset = SRV_MIRRORS;
    d->pad = plane | (first & 0xFF) << 8 | mip << 16;      /* (what a clear writes: the plane, or the narrow texture's level, and the first slice) */
    d->slice = 0;      /* (the mirror's layers are the view's, of its one level) */
    d->mip = 0;
    const UINT narrow = !vf ? 0 : vf->narrow == NARROW_R8 ? 1 : vf->narrow == NARROW_R8G8 ? 2 : vf->narrow == NARROW_R16 ? 3 : vf->narrow == NARROW_R16G16 ? 4 : 0;
    d->size = (UINT64)source | ((UINT64)(integer ? (channels == 1 ? (r->fmt->planar == PLANAR_16 ? 3u : 1u) : (r->fmt->planar == PLANAR_16 ? 4u : 2u)) : narrow) << 32);
    d->kind = DK_UAV_TEXTURE;
    return;
  }
  /* (a view of the resource's own image: in its format, or another of the same pixel size the kernel can show its bits as) */
  if (vf->kernel != r->fmt->kernel && kernel_pixel_bytes(vf->kernel) != kernel_pixel_bytes(r->fmt->kernel)) return;
  d->alloc = r->alloc;
  d->sampler[0] = vf->kernel == r->fmt->kernel ? 0 : vf->kernel;
  d->size = (UINT64)r->alloc;      /* (what a clear writes) */
  d->kind = DK_UAV_TEXTURE;
}

static void STDMETHODCALLTYPE dev_CreateUnorderedAccessView(Dev *This, ID3D12Resource *resource, ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  Resource *r = (Resource *)resource;
  memset(&d, 0, sizeof d);
  if (r && r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) texture_uav(This, r, desc, &d);
  if (r && r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    UINT stride = 4;
    UINT64 first = 0, count = r->desc.Width / 4;
    if (desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
      stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride : (desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != DXGI_FORMAT_R32_TYPELESS ? format_bytes(desc->Format) : 4);
      first = desc->Buffer.FirstElement;
      count = desc->Buffer.NumElements;
      if (!desc->Buffer.StructureByteStride && !(desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW)) d.format = desc->Format;      /* (typed: ClearUnorderedAccessView* converts to it) */
    }
    d.kind = (r->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? DK_UAV : 0;      /* (no unordered access view of a buffer made without the flag) */
    d.stride = stride;
    d.alloc = r->alloc;
    d.offset = first * stride;
    d.size = count * stride;
    /* (a typed view: of elements of a format that can be stored, the kernel's typed view of a buffer, RWBuffer<>) */
    if (d.format != DXGI_FORMAT_UNKNOWN && d.format != DXGI_FORMAT_R32_TYPELESS) {
      if (texel_buffer_format(d.format) == 2) d.sampler[1] = d.format;
      else d.kind = 0;
    }
    /* (the counter of a structured view - an append, consume or counter buffer: the word at CounterOffsetInBytes of the counter resource, a buffer) */
    const Resource *c = (const Resource *)counter;
    if (c && desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER && desc->Buffer.StructureByteStride && c->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        desc->Buffer.CounterOffsetInBytes % 4 == 0 && desc->Buffer.CounterOffsetInBytes + 4 <= c->desc.Width && desc->Buffer.CounterOffsetInBytes <= 0xFFFFFFFFu) {
      d.sampler[2] = c->alloc;
      d.sampler[3] = (UINT)desc->Buffer.CounterOffsetInBytes;
    }
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
    d.stride = 1;
    /* (the layers it draws into: of an array (and a cube, which is one) or a volume; a primitive goes to the one its SV_RenderTargetArrayIndex names) */
    if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) d.mip = desc->Texture2D.MipSlice;
    if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY && !r->planes) {
      d.mip = desc->Texture2DArray.MipSlice;
      if (!view_layers(r, desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize, &d.slice, &d.stride)) d.kind = 0;
    }
    if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE1DARRAY) {
      d.mip = desc->Texture1DArray.MipSlice;
      if (!view_layers(r, desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize, &d.slice, &d.stride)) d.kind = 0;
    }
    if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE3D) {
      d.mip = desc->Texture3D.MipSlice;
      if (!view_layers_at(r, d.mip, desc->Texture3D.FirstWSlice, desc->Texture3D.WSize, &d.slice, &d.stride)) d.kind = 0;
    }
    if (!desc && r->kind != 0) d.stride = r->layers;      /* (no description: all of it) */
    /* (a level the resource has, on a device that draws into levels; a volume's slices of it) */
    if (d.mip >= r->mips || (d.mip && !(This->caps.features2 & DEV_FEATURE2_MIPMAPPED_LAYERS))) d.kind = 0;
    if (format_packed_yuv(r->desc.Format) && (!desc || !packed_yuv_view_ok(r->desc.Format, desc->Format, 1))) d.kind = 0;
    {
      /* (a typeless narrow texture the kernel keeps natively drawn into as another kind of its family: a write-back mirror of the view's level and layers, its channels given back
       * after each pass) */
      UINT kf = 0;
      const int typed = d.kind && desc && !r->planes ? typed_view_kind(This, r, desc->Format, KFC_RENDER_TARGET, &kf) : 0;
      if (typed < 0) d.kind = 0;
      if (typed > 0) {
        const int array = desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY || desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
        d.alloc = (r->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? typed_mirror(r, kf, d.mip, 1, d.slice, d.stride, array, MIRROR_WRITE_BACK, narrow_family_mask(desc->Format)) : 0;
        d.mip = 0, d.slice = 0;
        if (!d.alloc) d.kind = 0;
      }
    }
    if (r->planes) {
      UINT plane = 0, slice = 0, count = 1, channels = 0;
      if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) plane = desc->Texture2D.PlaneSlice;
      if (desc && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) {
        plane = desc->Texture2DArray.PlaneSlice;
        slice = desc->Texture2DArray.FirstArraySlice;
        count = desc->Texture2DArray.ArraySize == 0xFFFFFFFFu ? (slice < r->layers ? r->layers - slice : 0) : desc->Texture2DArray.ArraySize;
      }
      /* (an integer view: a mirror of the plane in the integer format of its bits, drawn into, whose channels go back to the plane after each pass. A view of several slices
       * draws into an array mirror of them that gives each layer back to its slice's plane - a primitive into the slice its SV_RenderTargetArrayIndex names) */
      const int integer = desc && plane_integer_view(r, desc->Format, &plane, &channels);
      if (count > 1 || integer) {
        if (!integer) {
          const FormatInfo *vf = format_info(desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
          if (vf && vf->narrow && r->planes == 2) plane = vf->narrow == NARROW_R8G8 || vf->narrow == NARROW_R16G16 ? 1 : 0;
          channels = desc ? format_write_mask(desc->Format) : 0xF;
        }
        d.alloc = (r->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? res_mirror(r, plane, slice, count, count > 1, (integer ? MIRROR_INTEGER : 0) | MIRROR_WRITE_BACK, channels) : 0;
        d.stride = count;
      } else {
        d.alloc = view_alloc(r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, plane, slice);
      }
      d.slice = 0;
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
    /* (the level it draws into - MipSlice, on a device that draws into levels -, and the size of that level, for the colour target a pass with this view alone is given: cmdlist.inc
     * depth_companion) */
    if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE1D) d.mip = desc->Texture1D.MipSlice;
    if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE1DARRAY) d.mip = desc->Texture1DArray.MipSlice;
    if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D) d.mip = desc->Texture2D.MipSlice;
    if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY) d.mip = desc->Texture2DArray.MipSlice;
    if (d.mip >= r->mips || (d.mip && !(This->caps.features2 & DEV_FEATURE2_MIPMAPPED_LAYERS))) d.kind = 0;
    d.size = (UINT64)mip_extent((UINT)r->desc.Width, d.mip) | ((UINT64)mip_extent(r->desc.Height, d.mip) << 32);
    d.offset = r->desc.SampleDesc.Count ? r->desc.SampleDesc.Count : 1;
    /* (the layers of an array (or cube) it draws into) */
    d.stride = desc ? 1 : r->kind == 2 || r->kind == 3 || r->kind == 5 ? r->layers : 1;
    if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY && !view_layers(r, desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize, &d.slice, &d.stride)) d.kind = 0;
    if (desc && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE1DARRAY && !view_layers(r, desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize, &d.slice, &d.stride)) d.kind = 0;
  }
  desc_write(handle, &d);
}

/* The words of a sampler (D3D12_FILTER / D3D12_TEXTURE_ADDRESS_MODE) the way the kernel has them: the first word packs the filter word, address modes and mip filter; rest[0..2] are
 * the LOD bias, minimum and maximum LOD as bits. The filter word (the low byte; dxgk::filter_word) is the minification filter in bit 0, in bit 1 that the magnification filter is
 * another, in bit 2 that one, in bits 4-5 the reduction (1 minimum, 2 maximum); its bit 8 - coordinates in texels - is the descriptor's `dimension`. An address byte is the kernel's
 * mode (0 wrap, 1 clamp, 2 mirror, 3 border, 4 mirror once) and, in the byte of U, the border colour in bits 4-5 (0 transparent black, 1 opaque black, 2 opaque white: the three a
 * static sampler can have; another colour is the nearest of them). A device with wrap and clamp alone uses the nearer of the two (the kernel does that). */
static float lod_within(float value, float limit) { return !(value == value) ? 0.0f : value > limit ? limit : value < -limit ? -limit : value; }
/* Whether `filter` is a D3D12_FILTER (bit 0 mip, bit 2 mag, bit 4 min - 1 linear -, bit 6 anisotropic, with min and mag linear and - AnisoFilterWithPointMipSupported - mip linear or
 * point, bits 7-8 the reduction: standard, comparison, minimum, maximum). */
static int d3d12_filter_valid(UINT filter) {
  if (filter & ~0x1D5u) return 0;
  return !(filter & 0x40u) || (filter & 0x14u) == 0x14u;
}
static UINT sampler_words(UINT filter, UINT au, UINT av, const float *border, float bias, float min_lod, float max_lod, UINT *rest) {
  const UINT min_linear = (filter & 0x10u) ? 1u : 0u, mag_linear = (filter & 0x04u) ? 1u : 0u;      /* (anisotropic: both linear, d3d12_filter_valid) */
  const UINT reduction = (filter >> 7) & 3u;
  const UINT word = min_linear | (min_linear != mag_linear ? 2u | (mag_linear << 2) : 0u) | ((reduction == 2 ? 1u : reduction == 3 ? 2u : 0u) << 4);
  const UINT mip = (max_lod <= 0.0f) ? 0u : ((filter & 1u) ? 2u : 1u);
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
  return word | (a[0] << 8) | (a[1] << 16) | (mip << 24);
}

/* The comparison a sampler's lookups (SampleCmp, GatherCmp) make, as the kernel has it (graphics::CompareOp: never 0 .. always 7): a comparison filter's function (D3D12_COMPARISON_FUNC
 * 1 .. 8), else none - never passes. */
static UINT sampler_comparison(UINT filter, UINT func) {
  if (((filter >> 7) & 3u) != 1u || func < D3D12_COMPARISON_FUNC_NEVER || func > D3D12_COMPARISON_FUNC_ALWAYS) return 0;
  return func - D3D12_COMPARISON_FUNC_NEVER;
}
/* Whether a non-normalized sampler (D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES) keeps Direct3D 12's rules: min and mag filter alike and a point mip filter, no anisotropy or
 * comparison (MIN_MAG_MIP_POINT, MIN_MAG_LINEAR_MIP_POINT and their MINIMUM_ / MAXIMUM_ forms), clamp or border addressing in U and V, MinLOD and MaxLOD 0. */
static int unnormalized_sampler_ok(const D3D12_SAMPLER_DESC2 *d) {
  const UINT f = (UINT)d->Filter & ~0x180u, reduction = ((UINT)d->Filter >> 7) & 3u;
  if ((f != 0x00u && f != 0x14u) || reduction == 1) return 0;
  if ((d->AddressU != D3D12_TEXTURE_ADDRESS_MODE_CLAMP && d->AddressU != D3D12_TEXTURE_ADDRESS_MODE_BORDER) ||
      (d->AddressV != D3D12_TEXTURE_ADDRESS_MODE_CLAMP && d->AddressV != D3D12_TEXTURE_ADDRESS_MODE_BORDER))
    return 0;
  return d->MinLOD == 0.0f && d->MaxLOD == 0.0f;
}
/* A sampler descriptor from D3D12_SAMPLER_DESC2 (CreateSampler's description is one with no flags). The border colour of a sampler with a BORDER address mode is the kernel's
 * colour of any value (on devices with kFeature2SamplerState; the nearest of transparent black, opaque black and white - sampler_words' - on the others): the floats as their bits,
 * or - D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR - the integers, which an integer texture's channels are in the kernel. Coordinates in texels (NON_NORMALIZED_COORDINATES) need the
 * device's kFeature2UnnormalizedCoordinates (NonNormalizedCoordinateSamplersSupported, OPTIONS17) and Direct3D 12's rules for them; a description that breaks the rules - a filter
 * that is not one, flags that do not exist - is an invalid call, and its descriptor a null one. */
static void sampler_descriptor(Dev *This, const D3D12_SAMPLER_DESC2 *desc, SoftDesc *d) {
  memset(d, 0, sizeof *d);
  if (!desc) return;
  const UINT flags = (UINT)desc->Flags;
  if (!d3d12_filter_valid((UINT)desc->Filter) || (flags & ~(UINT)(D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR | D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES))) return;
  if ((flags & D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES) && (!(This->caps.features2 & DEV_FEATURE2_UNNORMALIZED_COORDINATES) || !unnormalized_sampler_ok(desc))) return;
  const int uint_border = (flags & D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR) != 0;
  float nearest[4];
  for (UINT c = 0; c < 4; ++c) nearest[c] = uint_border ? (desc->UintBorderColor[c] ? 1.0f : 0.0f) : desc->FloatBorderColor[c];
  d->kind = DK_SAMPLER;
  d->sampler[0] = sampler_words(desc->Filter, desc->AddressU, desc->AddressV, nearest, desc->MipLODBias, desc->MinLOD, desc->MaxLOD, &d->sampler[1]);
  d->dimension = (flags & D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES) ? 1u : 0u;      /* (the filter word's bit 8) */
  d->offset = sampler_comparison(desc->Filter, desc->ComparisonFunc);
  /* (the anisotropic filters' MaxAnisotropy, 1 .. 16 - plain, comparing, minimum, maximum, with a linear or a point mip filter) */
  const UINT anisotropic = ((UINT)desc->Filter & 0x40u) != 0;
  d->format = anisotropic ? (desc->MaxAnisotropy < 1 ? 1 : desc->MaxAnisotropy > 16 ? 16 : desc->MaxAnisotropy) : 1;
  if (desc->AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER || desc->AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER || desc->AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER) {
    UINT bits[4];
    for (UINT c = 0; c < 4; ++c) bits[c] = uint_border ? desc->UintBorderColor[c] : bits_of(desc->FloatBorderColor[c]);
    d->pad = 1;
    d->size = (UINT64)bits[0] | (UINT64)bits[1] << 32;
    d->slice = bits[2];
    d->stride = bits[3];
  }
}
static void STDMETHODCALLTYPE dev_CreateSampler(Dev *This, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  SoftDesc d;
  if (desc) {
    D3D12_SAMPLER_DESC2 d2;
    memset(&d2, 0, sizeof d2);
    d2.Filter = desc->Filter;
    d2.AddressU = desc->AddressU, d2.AddressV = desc->AddressV, d2.AddressW = desc->AddressW;
    d2.MipLODBias = desc->MipLODBias;
    d2.MaxAnisotropy = desc->MaxAnisotropy;
    d2.ComparisonFunc = desc->ComparisonFunc;
    for (UINT c = 0; c < 4; ++c) d2.FloatBorderColor[c] = desc->BorderColor[c];
    d2.MinLOD = desc->MinLOD, d2.MaxLOD = desc->MaxLOD;
    sampler_descriptor(This, &d2, &d);
  } else {
    sampler_descriptor(This, NULL, &d);
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
    UINT *request = mem_alloc((SIZE_T)(3 + n * 20) * 4);      /* (an entry is 14 words, a sampler with its extras 20) */
    UINT starts[1000];
    if (!request) return;
    UINT w = 0;
    request[w++] = ESC_WRITE_DESCRIPTORS;
    request[w++] = h->khandle;
    request[w++] = n;
    for (UINT i = 0; i < n; ++i) {
      const SoftDesc *d = &h->descs[first + i];
      starts[i] = w;
      UINT kind = 0;
      switch (d->kind) {
        case DK_SRV: case DK_SRV_BUFFER: kind = 1; break;
        case DK_UAV: kind = 2; break;
        case DK_CBV: kind = 3; break;
        case DK_SAMPLER: kind = 4; break;
        case DK_UAV_TEXTURE: kind = 5; break;
        default: kind = 0; break;
      }
      if (kind == 5) {
        /* (a texture's unordered access view: the level, the first layer, the layer count, the kernel's format of the view (0: the image's own); no sampler) */
        const UINT words[13] = {kind, d->alloc, d->mip, d->slice, d->stride, d->sampler[0], 0, 0, 0, 0, 0, 0, 0};
        request[w++] = first + i;
        memcpy(request + w, words, sizeof words);
        w += 13;
        continue;
      }
      /* (a sampler with anisotropy or a border colour: the kernel's extended entry, kDescriptorSamplerExtras, with six more words) */
      const int extras = d->kind == DK_SAMPLER && (d->format > 1 || d->pad);
      request[w++] = first + i;
      request[w++] = kind | (extras ? DESCRIPTOR_SAMPLER_EXTRAS : 0u);
      request[w++] = d->kind == DK_SAMPLER ? 0 : d->alloc;
      if (d->kind == DK_SRV) {
        /* (a texture's view: its first level and count of levels, its first layer and count of layers - the kernel's Descriptor::view; SRV_MIRRORS is this DLL's own) */
        request[w++] = (d->mip & 0xFFFF) | (d->stride << 16);
        request[w++] = (d->slice & 0xFFFF) | (d->pad << 16);
      } else {
        request[w++] = (UINT)d->offset;      /* (a sampler's: its comparison) */
        request[w++] = (UINT)(d->offset >> 32);
      }
      request[w++] = d->kind == DK_SAMPLER ? 0 : (UINT)d->size;
      request[w++] = d->kind == DK_SAMPLER ? 0 : (UINT)(d->size >> 32);
      request[w++] = (d->sampler[0] & 0xFF) | (d->kind == DK_SAMPLER && d->dimension ? 0x100u : 0u);      /* (the filter word; a sampler's coordinates in texels: bit 8) */
      request[w++] = ((d->sampler[0] >> 8) & 0xF) | (((d->sampler[0] >> 12) & 0xF) << 8);      /* (the mode, and the border colour above it) */
      request[w++] = (d->sampler[0] >> 16) & 0xFF;
      request[w++] = (d->sampler[0] >> 24) & 0xFF;
      request[w++] = d->sampler[1];
      request[w++] = d->sampler[2];
      request[w++] = d->sampler[3];
      if (extras) {
        request[w++] = d->format > 1 ? d->format : 1;
        request[w++] = d->pad ? 1 : 0;
        request[w++] = (UINT)d->size;      /* (the border colour's red, green, blue, alpha) */
        request[w++] = (UINT)(d->size >> 32);
        request[w++] = d->slice;
        request[w++] = d->stride;
      }
    }
    NTSTATUS st = dev_escape(h->d.dev, request, w, NULL, 0);
    if (!KMT_OK(st)) {
      /* (a descriptor of a resource that is gone, or one the kernel does not take: the others still go, one by one) */
      for (UINT i = 0; i < n; ++i) {
        UINT one[23];
        const UINT words = (i + 1 < n ? starts[i + 1] : w) - starts[i];
        memcpy(one, request, 3 * 4);
        one[2] = 1;
        memcpy(one + 3, request + starts[i], (SIZE_T)words * 4);
        dev_escape(h->d.dev, one, 3 + words, NULL, 0);
      }
    }
    mem_free(request);
  }
  h->synced = epoch;
}
