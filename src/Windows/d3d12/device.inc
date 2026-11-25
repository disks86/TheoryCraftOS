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

/* d3d12.dll: the device (ID3D12Device) and the factory functions. Included by d3d12.c after the other parts. */

/* The private fence handles and the like are in the other parts; the device is made here. */
static void dev_destroy(DxObj *o) {
  Dev *d = (Dev *)o;
  mem_free(d->shared);      /* (empty: every pipeline has gone, and each gave its shaders back) */
  if (d->null_target) kmt_destroy_allocation(d->kdev, d->null_target);
  for (UINT i = 0; i < d->companion_count; ++i) kmt_destroy_allocation(d->kdev, d->companions[i].color);
  mem_free(d->companions);
  for (UINT i = 0; i < d->uav_target_count; ++i) kmt_destroy_allocation(d->kdev, d->uav_targets[i].color);
  mem_free(d->uav_targets);
  if (d->kdev) kmt_destroy_device(d->kdev);
  if (d->adapter) kmt_close_adapter(d->adapter);
}

/* ---- ID3D12Device ------------------------------------------------------------------------------------------------------------------------------------------------------------ */

static UINT STDMETHODCALLTYPE dev_GetNodeCount(Dev *This) { return 1; }
static LUID *STDMETHODCALLTYPE dev_GetAdapterLuid(Dev *This, LUID *ret) { *ret = This->luid; return ret; }
static HRESULT STDMETHODCALLTYPE dev_GetDeviceRemovedReason(Dev *This) { return This->removed ? DXGI_ERROR_DEVICE_REMOVED : S_OK; }
static HRESULT STDMETHODCALLTYPE dev_SetStablePowerState(Dev *This, BOOL enable) { return S_OK; }
static HRESULT STDMETHODCALLTYPE dev_MakeResident(Dev *This, UINT count, ID3D12Pageable *const *objects) { return S_OK; }
static HRESULT STDMETHODCALLTYPE dev_Evict(Dev *This, UINT count, ID3D12Pageable *const *objects) { return S_OK; }
/* ---- shared handles ----------------------------------------------------------------------------------------------------------------------------------------- */
/* A resource or heap made D3D12_HEAP_FLAG_SHARED, or a fence made D3D12_FENCE_FLAG_SHARED, gets an NT handle (D3DKMTShareObjects: win32k makes a kernel object
 * "DxgkSharedResource" / "DxgkSharedSyncObject" holding the kernel's allocation or fence, src/Process/Win32k/Dxg/DxgShares.cpp). Another device - of this process or of
 * any process the handle reaches by DuplicateHandle, inheritance or the name - opens it as a resource, heap or fence of its own over the same memory or fence, on the
 * same adapter. A resource's description travels in the share (resource.inc ShareDesc). */
static HRESULT STDMETHODCALLTYPE dev_CreateSharedHandle(Dev *This, ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle) {
  extern const ID3D12Resource2Vtbl res_vtbl;
  extern const ID3D12Heap1Vtbl heap_vtbl;
  extern const ID3D12Fence1Vtbl fence_vtbl;
  if (handle) *handle = NULL;
  if (!object || !handle) return E_INVALIDARG;
  if (access != GENERIC_ALL) return E_INVALIDARG;      /* (the one value Direct3D 12 takes) */
  const void *vtbl = ((IUnknown *)object)->lpVtbl;
  UINT shared = 0;
  if (vtbl == &res_vtbl) shared = ((Resource *)object)->kresource;
  else if (vtbl == &heap_vtbl) shared = ((HeapObj *)object)->kresource;
  else if (vtbl == &fence_vtbl && (((Fence *)object)->flags & D3D12_FENCE_FLAG_SHARED)) shared = ((Fence *)object)->sync;
  else if (vtbl != &fence_vtbl) return E_INVALIDARG;      /* (no other object is shared) */
  if (!shared || DOBJ(object)->dev != This) return E_INVALIDARG;      /* (not made shared, or of another device) */
  KmtAttributes a;
  void *oa = kmt_attributes(&a, name, attributes && attributes->bInheritHandle, attributes ? attributes->lpSecurityDescriptor : NULL);
  if (!oa) return E_INVALIDARG;
  UINT64 nt = 0;
  const NTSTATUS st = kmt_share_objects(shared, oa, access, &nt);
  if ((UINT)st == 0xC0000035u) return DXGI_ERROR_NAME_ALREADY_EXISTS;      /* STATUS_OBJECT_NAME_COLLISION */
  if (!KMT_OK(st)) return hr_from(st);
  *handle = (HANDLE)(UINT_PTR)nt;
  return S_OK;
}

/* The object a shared handle names, made on this device: a resource or a heap (what its share says it is) or a fence. `mode`: SHARED_NT an NT handle only (of a resource,
 * heap or fence), SHARED_GLOBAL a global share handle only (the legacy handles of IDXGIResource::GetSharedHandle, a resource's), SHARED_ANY either - an NT handle
 * first. */
enum { SHARED_ANY, SHARED_NT, SHARED_GLOBAL };
static HRESULT open_shared(Dev *This, UINT64 handle, int mode, IUnknown **out) {
  extern const ID3D12Heap1Vtbl heap_vtbl;
  *out = NULL;
  BYTE runtime[SHARE_DESC_BYTES];
  UINT runtime_size = 0, total = 0, resource_driver = 0, allocations = 0;
  int global = mode == SHARED_GLOBAL;
  NTSTATUS st = 0xC0000008u;
  if (!global) {
    st = kmt_query_resource_nt(This->kdev, handle, runtime, sizeof runtime, &runtime_size, &total, &resource_driver, &allocations);
    if ((UINT)st == 0xC0000024u) {      /* STATUS_OBJECT_TYPE_MISMATCH: not a resource's handle - a fence's? */
      UINT sync = 0;
      st = kmt_open_sync_nt(This->kdev, handle, &sync);
      if (!KMT_OK(st)) return (UINT)st == 0xC0000024u || (UINT)st == 0xC0000008u ? E_INVALIDARG : hr_from(st);
      Fence *f = NULL;
      const HRESULT hr = fence_make(This, 0, D3D12_FENCE_FLAG_SHARED, sync, &f);
      if (FAILED(hr)) { kmt_destroy_sync(sync); return hr; }
      *out = (IUnknown *)f;
      return S_OK;
    }
    /* (no NT handle of the process's: a global share handle, if that may be one - they are 32-bit numbers) */
    if ((UINT)st == 0xC0000008u && mode == SHARED_ANY && handle <= 0xFFFFFFFFu) global = 1;
  }
  if (global) {
    if (handle == 0 || handle > 0xFFFFFFFFu) return E_INVALIDARG;
    st = kmt_query_resource_global(This->kdev, (UINT)handle, runtime, sizeof runtime, &runtime_size, &total, &resource_driver, &allocations);
  }
  if ((UINT)st == 0xC0000023u) return E_INVALIDARG;      /* (runtime data of another runtime's: bigger than ours) */
  if (!KMT_OK(st)) return (UINT)st == 0xC0000008u ? E_INVALIDARG : hr_from(st);
  ShareDesc sd;
  /* (our description, and the allocations it says there are: one, or two for each slice of a planar texture - its planes; a resource another Direct3D runtime made
   * would be laid out otherwise) */
  if (!share_desc_read(runtime, runtime_size, &sd) || total > 4096 * KMT_MAX_RESOURCE_ALLOCATIONS || resource_driver > 4096) return E_INVALIDARG;
  const FormatInfo *sfi = sd.kind == 0 && sd.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ? format_info(sd.desc.Format) : NULL;
  const UINT shared_layers = sd.desc.DepthOrArraySize ? sd.desc.DepthOrArraySize : 1u;
  /* (a planar texture: an allocation for every plane of every slice; a packed YUV array: one for every slice) */
  const UINT expected = sfi && sfi->planar ? format_planes(sfi) * shared_layers : sfi && format_packed_yuv(sfi->dxgi) && shared_layers > 1 ? shared_layers : 1u;
  if (allocations != expected || expected > KMT_MAX_RESOURCE_ALLOCATIONS) return E_INVALIDARG;
  BYTE *driver = mem_alloc(total ? total : 1), *resource_data = mem_alloc(resource_driver ? resource_driver : 1);
  UINT *opened = mem_alloc(sizeof(UINT) * expected);
  void **datas = mem_alloc(sizeof(void *) * expected);
  if (!driver || !resource_data || !opened || !datas) { mem_free(driver); mem_free(resource_data); mem_free(opened); mem_free(datas); return E_OUTOFMEMORY; }
  UINT kresource = 0;
  if (global) st = kmt_open_resource_global_n(This->kdev, (UINT)handle, runtime, runtime_size, driver, total, expected, opened, datas, &kresource);
  else st = kmt_open_resource_nt_n(This->kdev, handle, runtime, runtime_size, resource_data, resource_driver, driver, total, expected, opened, datas, &kresource);
  /* (the backing of all of them: each allocation's data, where the kernel says it put it in the buffer, has its bytes) */
  UINT64 bytes = 0;
  for (UINT i = 0; KMT_OK(st) && i < expected; ++i) {
    const BYTE *at = datas[i];
    if (!at || at < driver || at > driver + total || (SIZE_T)(driver + total - at) < KAW_WORDS * 4) continue;
    UINT words[KAW_WORDS];
    memcpy(words, at, sizeof words);
    bytes += words[KAW_BYTES_LO] | ((UINT64)words[KAW_BYTES_HI] << 32);
  }
  mem_free(driver);
  mem_free(resource_data);
  mem_free(datas);
  if (!KMT_OK(st)) { mem_free(opened); return (UINT)st == 0xC0000008u ? E_INVALIDARG : hr_from(st); }
  const UINT allocation = opened[0];
  if (sd.kind == 1) {
    mem_free(opened);
    HeapObj *h = dobj_new(sizeof *h, This, &heap_vtbl, g_heap_iids, heap_destroy);
    if (!h) { kmt_destroy_allocation(This->kdev, allocation); return E_OUTOFMEMORY; }
    memset(&h->desc, 0, sizeof h->desc);
    h->desc.SizeInBytes = sd.heap_size;
    h->desc.Properties = sd.props;
    h->desc.Alignment = sd.heap_alignment;
    h->desc.Flags = sd.flags;
    h->alloc = allocation;
    h->kresource = kresource;
    *out = (IUnknown *)h;
    return S_OK;
  }
  ResourceSource source = {allocation, kresource, bytes, opened, expected};
  if (sd.cube) This->next_resource_flags = TCOS_RESOURCE_CUBE;
  Resource *r = NULL;
  const HRESULT hr = resource_make_from(This, &sd.props, sd.flags, &sd.desc, NULL, 0, 0, &source, &r);
  if (FAILED(hr)) {
    for (UINT i = 0; i < expected; ++i) kmt_destroy_allocation(This->kdev, opened[i]);      /* (a description this device cannot make: the opened allocations go) */
    mem_free(opened);
    return hr;
  }
  mem_free(opened);
  memcpy(r->share_extra, sd.extra, sd.extra_size);
  r->share_extra_size = sd.extra_size;
  if (global) r->global_share = (UINT)handle;      /* (what GetSharedHandle of the runtime above answers for it) */
  *out = (IUnknown *)r;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_OpenSharedHandle(Dev *This, HANDLE handle, REFIID riid, void **object) {
  if (object) *object = NULL;
  if (!handle || !riid || !object) return E_INVALIDARG;
  IUnknown *made = NULL;
  const HRESULT hr = open_shared(This, (UINT64)(UINT_PTR)handle, SHARED_ANY, &made);
  if (FAILED(hr)) return hr;
  return query_out(made, riid, object);      /* (an interface the object does not have: E_NOINTERFACE, and the object goes) */
}
/* The NT handle of a shared object by the name it was shared with (in the session's \BaseNamedObjects, as CreateSharedHandle put it). */
static HRESULT STDMETHODCALLTYPE dev_OpenSharedHandleByName(Dev *This, const WCHAR *name, DWORD access, HANDLE *handle) {
  if (handle) *handle = NULL;
  if (!name || !*name || !handle) return E_INVALIDARG;
  if (access != GENERIC_ALL) return E_INVALIDARG;
  KmtAttributes a;
  void *oa = kmt_attributes(&a, name, FALSE, NULL);
  if (!oa) return E_INVALIDARG;
  UINT64 nt = 0;
  NTSTATUS st = kmt_open_nt_by_name(oa, access, 0, &nt);
  if ((UINT)st == 0xC0000024u) st = kmt_open_nt_by_name(oa, access, 1, &nt);      /* (a fence's name) */
  if ((UINT)st == 0xC0000034u || (UINT)st == 0xC000003Au) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);      /* STATUS_OBJECT_NAME_NOT_FOUND / PATH_NOT_FOUND */
  if ((UINT)st == 0xC0000024u) return E_INVALIDARG;      /* (an object of another kind has the name) */
  if (!KMT_OK(st)) return hr_from(st);
  *handle = (HANDLE)(UINT_PTR)nt;
  return S_OK;
}

static UINT STDMETHODCALLTYPE dev_GetDescriptorHandleIncrementSize(Dev *This, D3D12_DESCRIPTOR_HEAP_TYPE type) { return type < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES ? DESCRIPTOR_STRIDE : 0; }

static D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE dev_GetCustomHeapProperties(Dev *This, D3D12_HEAP_PROPERTIES *ret, UINT node, D3D12_HEAP_TYPE type) {
  memset(ret, 0, sizeof *ret);
  ret->Type = D3D12_HEAP_TYPE_CUSTOM;
  ret->CreationNodeMask = ret->VisibleNodeMask = 1;
  switch (type) {
    case D3D12_HEAP_TYPE_DEFAULT: ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE; ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L1; break;
    case D3D12_HEAP_TYPE_UPLOAD: ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE; ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0; break;
    default: ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0; break;
  }
  return ret;
}

/* The feature level the adapter's limits (what the kernel's graphics device takes, DeviceInfo::limits) allow: 11_0 needs 16 vertex inputs, 16 interpolants, 16 texture units and 8 render targets,
 * 10_0 the same but fewer interpolants (the ones it needs are 16 as well), and below that 9_3 (4 render targets, 16 textures) or 9_1. */
static D3D_FEATURE_LEVEL dev_feature_level(const Dev *d) {
  const KmtCaps *c = &d->caps;
  /* (11_0: sixteen vertex inputs and texture units, eight render targets. The level also names 32 interpolants; a GPU reached through OpenGL has 14 or 15 for the shaders'
   * own - GL_MAX_VARYING_COMPONENTS less the position - and a pipeline whose shaders pass more than the device takes is refused when it is made) */
  if (c->max_vertex_attributes >= 16 && c->max_varyings >= 14 && c->max_texture_units >= 16 && c->max_color_targets >= 8) return D3D_FEATURE_LEVEL_11_0;
  if (c->max_texture_units >= 16 && c->max_color_targets >= 4 && c->max_varyings >= 8) return D3D_FEATURE_LEVEL_9_3;
  return D3D_FEATURE_LEVEL_9_1;
}

/* Whether a format holds integers (no filtering, no blending). */
static int format_integer(DXGI_FORMAT f) {
  const FormatInfo *i = format_info(f);
  return i && (i->kernel == KF_RGBA32UINT || i->kernel == KF_RGBA32SINT || i->kernel == KF_RGBA16UINT || i->kernel == KF_RGBA16SINT || i->kernel == KF_RGBA8UINT || i->kernel == KF_R32UINT ||
               i->kernel == KF_R32SINT || f == DXGI_FORMAT_R16G16_SINT);
}
/* Unordered access to textures of the format (the kernel's storage images, DEV_FEATURE2_STORAGE_IMAGES): typed loads and stores of every format a texture's view can be
 * made of - the colour formats that are not sRGB or block-compressed, the narrow ones and the planes of the planar ones through mirrors (texture_uav) -, and the atomic
 * operations on R32_UINT and R32_SINT. (A cube has none: the kernel's cube is not an array.) */
static void format_uav_support(const Dev *dev, DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 *s1, D3D12_FORMAT_SUPPORT2 *s2) {
  const FormatInfo *i = format_info(format);
  if (!(dev->caps.features2 & DEV_FEATURE2_STORAGE_IMAGES) || !i || !i->kernel || i->depth || i->compressed || i->kernel == KF_BGRA8_SRGB || i->kernel == KF_RGBA8_SRGB) return;
  if (i->narrow && i->kernel != KF_A8B8G8R8 && i->kernel != KF_RGBA16) return;      /* (a mirror is of 8- or 16-bit RGBA: not of R16G16_SINT's image) */
  *s1 |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
  *s2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
  if (format == DXGI_FORMAT_R32_UINT || format == DXGI_FORMAT_R32_SINT)
    *s2 |= D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
           D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
}

/* What the device can do: Direct3D 12 on feature level 11_0 with the tiers at their lowest. The limits that are lower than the levels' (texture units, vertex attributes, ...) are
 * those of the kernel's pipeline model: docs/win32k/Dxg.md and d3d12.c say which. */
static HRESULT STDMETHODCALLTYPE dev_CheckFeatureSupport(Dev *This, D3D12_FEATURE feature, void *data, UINT size) {
  if (!data) return E_INVALIDARG;
  switch (feature) {
    case D3D12_FEATURE_D3D12_OPTIONS: {
      D3D12_FEATURE_DATA_D3D12_OPTIONS *o = data;
      if (size != sizeof *o) return E_INVALIDARG;
      memset(o, 0, sizeof *o);
      o->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
      /* (typed loads of every format a typed unordered access view can have: the kernel's storage images and typed views of buffers read them all) */
      o->TypedUAVLoadAdditionalFormats = (This->caps.features2 & (DEV_FEATURE2_STORAGE_IMAGES | DEV_FEATURE2_TEXEL_BUFFERS)) == (DEV_FEATURE2_STORAGE_IMAGES | DEV_FEATURE2_TEXEL_BUFFERS);
      /* (the CPU renderer covers every pixel a triangle's area touches - within tier 1's half-pixel uncertainty, degenerate triangles culled, no inner coverage -; virgl's
       * pipe state has no conservative mode) */
      o->ConservativeRasterizationTier = (This->caps.features2 & DEV_FEATURE2_CONSERVATIVE_RASTER) ? D3D12_CONSERVATIVE_RASTERIZATION_TIER_1 : D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
      o->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_1;
      o->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
      o->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      o->StandardSwizzle64KBSupported = TRUE;      /* (resource.inc: CPU-visible textures of the standard swizzle are mapped in it) */
      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE: {
      D3D12_FEATURE_DATA_ARCHITECTURE *a = data;
      if (size != sizeof *a || a->NodeIndex != 0) return E_INVALIDARG;
      a->TileBasedRenderer = FALSE;
      a->UMA = This->software ? TRUE : FALSE;
      a->CacheCoherentUMA = a->UMA;
      return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
      D3D12_FEATURE_DATA_FEATURE_LEVELS *f = data;
      if (size != sizeof *f) return E_INVALIDARG;
      D3D_FEATURE_LEVEL best = (D3D_FEATURE_LEVEL)0;
      const D3D_FEATURE_LEVEL top = dev_feature_level(This);
      for (UINT i = 0; i < f->NumFeatureLevels; ++i)
        if (f->pFeatureLevelsRequested[i] <= top && f->pFeatureLevelsRequested[i] > best) best = f->pFeatureLevelsRequested[i];
      f->MaxSupportedFeatureLevel = best ? best : top;
      return best ? S_OK : E_INVALIDARG;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
      D3D12_FEATURE_DATA_FORMAT_SUPPORT *f = data;
      if (size != sizeof *f) return E_INVALIDARG;
      const FormatInfo *i = format_info(f->Format);
      f->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
      f->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
      if (i && i->planar) {
        /* (NV12, P010, P016: 2D textures and arrays, sampled and drawn into by plane views, what the video decoder writes and the video processor reads and writes; P208 the
         * same but no decoder writes it; V208 and V408 sampled and drawn into only) */
        f->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
        if (i->planar == PLANAR_8 || i->planar == PLANAR_16) f->Support1 |= D3D12_FORMAT_SUPPORT1_DECODER_OUTPUT;
        if (i->planar != PLANAR_V208 && i->planar != PLANAR_V408) f->Support1 |= D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_OUTPUT | D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_INPUT;
      } else if (i && format_packed_yuv(f->Format)) {
        /* (AYUV, Y410, Y416, YUY2, Y210, Y216: 2D textures and arrays, sampled through views of their pixels' format - AYUV drawn into too -, what the AV1 decoder's profiles
         * 1 and 2 write, the video processor's input and output; no texture UAVs: the kernel has none) */
        f->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_DECODER_OUTPUT |
                      D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_OUTPUT | D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_INPUT;
        if (f->Format == DXGI_FORMAT_AYUV) f->Support1 |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
        /* (AYUV and Y416 have mip levels - Y416's where the kernel has levels of its 16-bit images; the others have none in DXGI's tables) */
        if (format_packed_yuv_mips(f->Format) && device_format_levels(This, i->kernel)) f->Support1 |= D3D12_FORMAT_SUPPORT1_MIP;
      } else if (i) {
        f->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D;
        /* (levels: the kernel's 8-bit and block-compressed images have them; its wider ones - 16 and 32-bit channels, 10-bit, integer - and depth buffers on a device with levels of
         * every image, DEV_FEATURE2_MIPMAPPED_LAYERS) */
        if (device_format_levels(This, i->kernel) && (!i->depth || (This->caps.features2 & DEV_FEATURE2_MIPMAPPED_LAYERS))) f->Support1 |= D3D12_FORMAT_SUPPORT1_MIP;
        if (format_tiled(i)) f->Support2 |= D3D12_FORMAT_SUPPORT2_TILED;      /* (the kernel's reserved textures: tile_layout) */
        if (i->depth) f->Support1 |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
        else if (format_integer(f->Format)) f->Support1 |= D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_RENDER_TARGET;      /* (integers: loaded, not filtered or blended) */
        else f->Support1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE;
        if (i->narrow || i->kernel == KF_A8R8G8B8 || i->kernel == KF_A8B8G8R8 || i->kernel == KF_X8R8G8B8 || i->kernel == KF_RGBA16 || i->kernel == KF_RGB10A2)
          f->Support1 |= D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_OUTPUT | D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_INPUT;      /* (the video processor's RGB) */
      } else if (format_bytes(f->Format)) {
        f->Support1 = D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER;
      }
      if (f->Format == DXGI_FORMAT_R16_UINT || f->Format == DXGI_FORMAT_R32_UINT) f->Support1 |= D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER;
      /* (the views of depth buffers: their depth sampled, compared and gathered, their stencil loaded - on a device that reads depth buffers -; R32_FLOAT and R16_UNORM textures
       * are compared too) */
      if (This->caps.features2 & DEV_FEATURE2_DEPTH_TEXTURES) {
        const UINT depth_reads = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE | D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
                                 D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON | D3D12_FORMAT_SUPPORT1_SHADER_GATHER | D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON;
        switch (f->Format) {
          case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: f->Support1 = depth_reads; break;
          case DXGI_FORMAT_X24_TYPELESS_G8_UINT: case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: f->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD; break;
          case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R16_UNORM: f->Support1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON | D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON; break;
          default: break;
        }
      }
      format_uav_support(This, f->Format, &f->Support1, &f->Support2);
      if (This->caps.features2 & DEV_FEATURE2_TEXEL_BUFFERS) {
        /* (typed views of buffers: the kernel's element formats - shader resource views of all of them, unordered access views of those that can be stored, with typed loads, and the
         * atomic operations on the 32-bit integers) */
        const UINT texel = texel_buffer_format(f->Format);
        if (texel) {
          f->Support1 |= D3D12_FORMAT_SUPPORT1_BUFFER | D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
          if (texel == 2) {
            f->Support1 |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
            f->Support2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
          }
          if (f->Format == DXGI_FORMAT_R32_UINT || f->Format == DXGI_FORMAT_R32_SINT)
            f->Support2 |= D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
                           D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
        }
      }
      return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *m = data;
      if (size != sizeof *m) return E_INVALIDARG;
      /* (one quality level - the standard sample pattern - for the counts the adapter has, on the formats a render target or depth buffer can have) */
      m->NumQualityLevels = m->SampleCount == 1 || (m->SampleCount < 32 && ((This->caps.sample_counts >> m->SampleCount) & 1) && format_info(m->Format) && format_info(m->Format)->bytes == 4) ? 1 : 0;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_INFO: {
      D3D12_FEATURE_DATA_FORMAT_INFO *f = data;
      if (size != sizeof *f) return E_INVALIDARG;
      /* (a format with depth and stencil has two planes; a format the device does not know is not one) */
      if (!format_info(f->Format) && !format_bytes(f->Format)) return E_INVALIDARG;
      const FormatInfo *fi = format_info(f->Format);
      f->PlaneCount = f->Format == DXGI_FORMAT_D24_UNORM_S8_UINT || f->Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 2 : fi && fi->planar ? (UINT8)format_planes(fi) : 1;
      return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
      D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *g = data;
      if (size != sizeof *g) return E_INVALIDARG;
      g->MaxGPUVirtualAddressBitsPerResource = 40;
      g->MaxGPUVirtualAddressBitsPerProcess = 40;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
      D3D12_FEATURE_DATA_SHADER_MODEL *s = data;
      if (size != sizeof *s) return E_INVALIDARG;
      if (s->HighestShaderModel > D3D_SHADER_MODEL_5_1) s->HighestShaderModel = D3D_SHADER_MODEL_5_1;
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      D3D12_FEATURE_DATA_ROOT_SIGNATURE *r = data;
      if (size != sizeof *r) return E_INVALIDARG;
      /* (the highest of the asked-for version and 1.1: 1.2's static sampler descriptions are not read) */
      if (r->HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_0 || r->HighestVersion > (D3D_ROOT_SIGNATURE_VERSION)3) return E_INVALIDARG;      /* (3: version 1.2) */
      if (r->HighestVersion > D3D_ROOT_SIGNATURE_VERSION_1_1) r->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      D3D12_FEATURE_DATA_D3D12_OPTIONS1 *o = data;
      if (size != sizeof *o) return E_INVALIDARG;
      memset(o, 0, sizeof *o);
      o->TotalLaneCount = 1;
      return S_OK;
    }
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
      D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY *p = data;
      if (size != sizeof *p) return E_INVALIDARG;
      p->PriorityForTypeIsSupported = p->CommandListType == D3D12_COMMAND_LIST_TYPE_DIRECT && p->Priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
      return S_OK;
    }
    case D3D12_FEATURE_EXISTING_HEAPS: {
      D3D12_FEATURE_DATA_EXISTING_HEAPS *e = data;
      if (size != sizeof *e) return E_INVALIDARG;
      e->Supported = FALSE;
      return S_OK;
    }
    /* The features of the later interfaces: what device1.inc, library.inc and cmdlist1.inc do, the rest at the "not supported" tier (those calls are left out). */
#define FEATURE(type, name) type *name = data; if (size != sizeof *name) return E_INVALIDARG
    case D3D12_FEATURE_ARCHITECTURE1: {
      FEATURE(D3D12_FEATURE_DATA_ARCHITECTURE1, a);
      if (a->NodeIndex != 0) return E_INVALIDARG;
      a->TileBasedRenderer = FALSE;
      a->UMA = This->software ? TRUE : FALSE;
      a->CacheCoherentUMA = a->UMA;
      a->IsolatedMMU = TRUE;      /* (the device's addresses are its own: the kernel's allocations, not the process's) */
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS2, o); memset(o, 0, sizeof *o); return S_OK; }
    case D3D12_FEATURE_SHADER_CACHE: {
      FEATURE(D3D12_FEATURE_DATA_SHADER_CACHE, c);
      /* (CachedPSO blobs, pipeline libraries, and the sessions' ShaderCacheControl clear and SetDeleteOnDestroy) */
      c->SupportFlags = D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO | D3D12_SHADER_CACHE_SUPPORT_LIBRARY | D3D12_SHADER_CACHE_SUPPORT_SHADER_CONTROL_CLEAR |
                        D3D12_SHADER_CACHE_SUPPORT_SHADER_SESSION_DELETE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS3: {
      FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS3, o);
      memset(o, 0, sizeof *o);
      o->WriteBufferImmediateSupportFlags = D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT | D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE | D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY;
      o->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
      FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS4, o);
      memset(o, 0, sizeof *o);
      o->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_SERIALIZATION: {
      FEATURE(D3D12_FEATURE_DATA_SERIALIZATION, z);
      if (z->NodeIndex != 0) return E_INVALIDARG;
      z->HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_CROSS_NODE: {
      FEATURE(D3D12_FEATURE_DATA_CROSS_NODE, x);
      x->SharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      x->AtomicShaderInstructions = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
      FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS5, o);
      memset(o, 0, sizeof *o);
      o->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
      o->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_DISPLAYABLE: {
      FEATURE(D3D12_FEATURE_DATA_DISPLAYABLE, d);
      d->DisplayableTexture = FALSE;      /* (D3D12_RESOURCE_FLAG_ALLOW_DISPLAY needs shared resources) */
      d->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS6: {
      FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS6, o);
      memset(o, 0, sizeof *o);
      o->VariableShadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_QUERY_META_COMMAND: {
      FEATURE(D3D12_FEATURE_DATA_QUERY_META_COMMAND, q);
      (void)q;
      return E_INVALIDARG;      /* (EnumerateMetaCommands lists none: no command id is one of the device's) */
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
      FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS7, o);
      o->MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
      o->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT: {
      FEATURE(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT, p);
      if (p->NodeIndex != 0) return E_INVALIDARG;
      p->Support = D3D12_PROTECTED_RESOURCE_SESSION_SUPPORT_FLAG_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT: {
      FEATURE(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT, p);
      if (p->NodeIndex != 0) return E_INVALIDARG;
      p->Count = 0;
      return S_OK;
    }
    case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES: {
      FEATURE(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES, p);
      if (p->NodeIndex != 0 || p->Count != 0) return E_INVALIDARG;      /* (the count must be the one TYPE_COUNT gave: none) */
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS8: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS8, o); o->UnalignedBlockTexturesSupported = FALSE; return S_OK; }
    case D3D12_FEATURE_D3D12_OPTIONS9: {
      FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS9, o);
      memset(o, 0, sizeof *o);
      o->WaveMMATier = D3D12_WAVE_MMA_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS10: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS10, o); memset(o, 0, sizeof *o); return S_OK; }
    case D3D12_FEATURE_D3D12_OPTIONS11: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS11, o); memset(o, 0, sizeof *o); return S_OK; }
    case D3D12_FEATURE_D3D12_OPTIONS12: {
      FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS12, o);
      o->MSPrimitivesPipelineStatisticIncludesCulledPrimitives = D3D12_TRI_STATE_UNKNOWN;      /* (no mesh shaders) */
      o->EnhancedBarriersSupported = TRUE;      /* (ID3D12GraphicsCommandList7::Barrier: cmdlist1.inc) */
      o->RelaxedFormatCastingSupported = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS13: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS13, o); memset(o, 0, sizeof *o); return S_OK; }
    case D3D12_FEATURE_D3D12_OPTIONS14: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS14, o); memset(o, 0, sizeof *o); return S_OK; }
    case D3D12_FEATURE_D3D12_OPTIONS15: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS15, o); memset(o, 0, sizeof *o); return S_OK; }
    case D3D12_FEATURE_D3D12_OPTIONS16: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS16, o); memset(o, 0, sizeof *o); return S_OK; }
    case D3D12_FEATURE_D3D12_OPTIONS17: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS17, o); memset(o, 0, sizeof *o); return S_OK; }
    case D3D12_FEATURE_D3D12_OPTIONS18: { FEATURE(D3D12_FEATURE_DATA_D3D12_OPTIONS18, o); o->RenderPassesValid = TRUE; return S_OK; }
#undef FEATURE
    default: return E_INVALIDARG;
  }
}

/* ---- the whole table is in the other parts: the creation methods ------------------------------------------------------------------------------------------------------------ */

static Dev *dev_create(LUID luid, NTSTATUS *status_out) {
  Dev *d = mem_alloc(sizeof *d);
  if (!d) return NULL;
  extern const ID3D12Device10Vtbl dev_vtbl;
  dx_init(OBJ(d), &dev_vtbl, g_device_iids, dev_destroy);
  {
    extern const ID3D12VideoDevice4Vtbl vdev_vtbl;
    d->video.lpVtbl = &vdev_vtbl;
  }
  d->luid = luid;
  NTSTATUS st = kmt_open_adapter(luid, &d->adapter);
  if (KMT_OK(st)) st = kmt_create_device(d->adapter, &d->kdev);
  if (KMT_OK(st) && !KMT_OK(kmt_adapter_caps(d->adapter, &d->caps))) memset(&d->caps, 0, sizeof d->caps);
  kmt_segment_sizes(d->adapter, d->segments);
  d->software = !(d->caps.flags & KMT_CAPS_ACCELERATED);
  if (!KMT_OK(st)) {
    *status_out = st;
    dx_Release(OBJ(d));
    return NULL;
  }
  *status_out = st;
  return d;
}
