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

/* Part of d3d11.c: what ID3D11Device2 .. 5 and ID3D11DeviceContext2 .. 4 add.
 *
 * - Fences (ID3D11Fence): each is a Direct3D 12 fence of the device below, so a fence is the kernel's synchronization object, a shared one is shared through an NT
 *   handle (D3DKMTShareObjects) like Direct3D 12's, and a handle of either runtime opens in the other. ID3D11DeviceContext4::Signal and Wait put the signal or the
 *   wait on the Direct3D 12 queue after what the immediate context has recorded.
 * - The *1 textures, views, rasterizer state and query (Device3): the descriptions with a plane slice, a texture layout, conservative rasterization and a context
 *   type. The formats here have one plane, textures have the driver's layout, and conservative rasterization is not there (CheckFeatureSupport says so), so the
 *   creators take what that leaves and refuse the rest with E_INVALIDARG, as Windows does on such a device.
 * - Tiled resources (Device2 / Context2 tile calls): the device has none (TiledResourcesTier NOT_SUPPORTED), so no resource is tiled and no buffer is a tile pool;
 *   the calls refuse what they are given, as on hardware without them.
 * - Device-removed events (Device4): the events are set when Direct3D 12 below loses the device. */

/* ---- fences ------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct Fence11 {
  Child c;
  ID3D12Fence *f;
  UINT flags;                  /* D3D11_FENCE_FLAG as made (or, opened from a handle, SHARED) */
} Fence11;

extern const ID3D11FenceVtbl fence_vtbl;
static const GUID *const g_fence_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11Fence, NULL};

static Fence11 *fence_of(ID3D11Fence *f) { return f && ((DxObj *)f)->lpVtbl == (const void *)&fence_vtbl ? (Fence11 *)f : NULL; }
static void fence_destroy(DxObj *o) {
  Fence11 *f = (Fence11 *)o;
  if (f->f) ID3D12Fence_Release(f->f);
  child_destroy(o);
}
static HRESULT fence_wrap(Dev *d, ID3D12Fence *f12, UINT flags, REFIID riid, void **out) {
  Fence11 *f = child_new(sizeof *f, d, &fence_vtbl, g_fence_iids, fence_destroy);
  if (!f) { ID3D12Fence_Release(f12); return E_OUTOFMEMORY; }
  f->f = f12;
  f->flags = flags;
  const HRESULT hr = dx_QueryInterface(OBJ(f), riid, out);
  dx_Release(OBJ(f));
  return hr;
}

/* D3D11_FENCE_FLAG_NONE is 1 in some headers and 0 in others: both mean none. SHARED is a fence CreateSharedHandle shares; NON_MONITORED changes nothing (no fence
 * here keeps its value in memory a process maps); one shared across adapters needs cross-adapter objects the kernel does not have. */
static HRESULT STDMETHODCALLTYPE dev_CreateFence(Dev *d, UINT64 initial, D3D11_FENCE_FLAG flags, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (!riid || !out) return E_INVALIDARG;
  if ((UINT)flags & ~(UINT)(D3D11_FENCE_FLAG_NONE | D3D11_FENCE_FLAG_SHARED | D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER | D3D11_FENCE_FLAG_NON_MONITORED)) return E_INVALIDARG;
  if ((UINT)flags & D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER) return DXGI_ERROR_UNSUPPORTED;
  D3D12_FENCE_FLAGS f12flags = D3D12_FENCE_FLAG_NONE;
  if ((UINT)flags & D3D11_FENCE_FLAG_SHARED) f12flags |= D3D12_FENCE_FLAG_SHARED;
  if ((UINT)flags & D3D11_FENCE_FLAG_NON_MONITORED) f12flags |= D3D12_FENCE_FLAG_NON_MONITORED;
  ID3D12Fence *f12 = NULL;
  const HRESULT hr = ID3D12Device_CreateFence(d->d12, initial, f12flags, &IID_ID3D12Fence, (void **)&f12);
  if (FAILED(hr)) return hr;
  return fence_wrap(d, f12, (UINT)flags & ~(UINT)D3D11_FENCE_FLAG_NONE, riid, out);
}
/* A fence's NT handle - of a fence this runtime or Direct3D 12 shared, in this process or another. A resource's handle is not a fence's (E_INVALIDARG). */
static HRESULT STDMETHODCALLTYPE dev_OpenSharedFence(Dev *d, HANDLE handle, REFIID riid, void **out) {
  if (out) *out = NULL;
  if (!handle || !riid || !out) return E_INVALIDARG;
  ID3D12Fence *f12 = NULL;
  HRESULT hr = ID3D12Device_OpenSharedHandle(d->d12, handle, &IID_ID3D12Fence, (void **)&f12);
  if (FAILED(hr)) return hr;
  return fence_wrap(d, f12, D3D11_FENCE_FLAG_SHARED, riid, out);
}
/* GENERIC_ALL is the one access a fence's handle has (Direct3D 12 below checks it); a fence not made SHARED has no handle. */
static HRESULT STDMETHODCALLTYPE fence_CreateSharedHandle(Fence11 *f, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle) {
  if (handle) *handle = NULL;
  if (!handle || !(f->flags & D3D11_FENCE_FLAG_SHARED)) return E_INVALIDARG;
  return ID3D12Device_CreateSharedHandle(f->c.dev->d12, (ID3D12DeviceChild *)f->f, attributes, access, name, handle);
}
static UINT64 STDMETHODCALLTYPE fence_GetCompletedValue(Fence11 *f) { return ID3D12Fence_GetCompletedValue(f->f); }
static HRESULT STDMETHODCALLTYPE fence_SetEventOnCompletion(Fence11 *f, UINT64 value, HANDLE event) {
  if (!event) return E_INVALIDARG;      /* (Direct3D 11 takes an event; Direct3D 12's NULL - block here - is not part of it) */
  return ID3D12Fence_SetEventOnCompletion(f->f, value, event);
}

/* ---- ID3D11DeviceContext4 ----------------------------------------------------------------------------------------------------------------------------------- */

/* The signal and the wait follow what the context has recorded, which goes to the queue first. A deferred context has no queue to put them on. */
static HRESULT STDMETHODCALLTYPE ctx_Signal(Ctx *c, ID3D11Fence *fence, UINT64 value) {
  Fence11 *f = fence_of(fence);
  if (!f) return E_INVALIDARG;
  if (c->deferred) return DXGI_ERROR_INVALID_CALL;
  LOCK(c);
  flush(c->dev);
  const HRESULT hr = ID3D12CommandQueue_Signal(c->dev->queue, f->f, value);
  UNLOCK(c);
  return hr;
}
static HRESULT STDMETHODCALLTYPE ctx_Wait(Ctx *c, ID3D11Fence *fence, UINT64 value) {
  Fence11 *f = fence_of(fence);
  if (!f) return E_INVALIDARG;
  if (c->deferred) return DXGI_ERROR_INVALID_CALL;
  LOCK(c);
  flush(c->dev);
  const HRESULT hr = ID3D12CommandQueue_Wait(c->dev->queue, f->f, value);
  UNLOCK(c);
  return hr;
}

/* ---- ID3D11DeviceContext3 ----------------------------------------------------------------------------------------------------------------------------------- */

/* Flush1: every context type is the one queue; the event is set once what was sent has run (the queue runs each list before it returns). */
static void STDMETHODCALLTYPE ctx_Flush1(Ctx *c, D3D11_CONTEXT_TYPE type, HANDLE event) {
  (void)type;
  if (c->deferred) return;
  LOCK(c);
  flush(c->dev);
  UNLOCK(c);
  if (event) SetEvent(event);
}
/* Hardware content protection: the state is kept and answered. No crypto session can be made on this device (no ID3D11VideoDevice content protection), so no
 * protected resource exists for it to change. */
static void STDMETHODCALLTYPE ctx_SetHardwareProtectionState(Ctx *c, BOOL enable) { c->hardware_protection = enable ? TRUE : FALSE; }
static void STDMETHODCALLTYPE ctx_GetHardwareProtectionState(Ctx *c, BOOL *enable) { if (enable) *enable = c->hardware_protection; }

/* ---- ID3D11DeviceContext2 ----------------------------------------------------------------------------------------------------------------------------------- */

/* The annotations go to a graphics debugger; none is attached to this runtime, so IsAnnotationEnabled is FALSE and the markers go nowhere (as on Windows without
 * one). */
static BOOL STDMETHODCALLTYPE ctx_IsAnnotationEnabled(Ctx *c) { (void)c; return FALSE; }
static void STDMETHODCALLTYPE ctx_SetMarkerInt(Ctx *c, const WCHAR *label, int data) { (void)c; (void)label; (void)data; }
static void STDMETHODCALLTYPE ctx_BeginEventInt(Ctx *c, const WCHAR *label, int data) { (void)c; (void)label; (void)data; }
static void STDMETHODCALLTYPE ctx_EndEvent(Ctx *c) { (void)c; }

/* Tiled resources: none can be made here (TiledResourcesTier NOT_SUPPORTED: CreateBuffer and CreateTexture2D refuse D3D11_RESOURCE_MISC_TILED and TILE_POOL), so
 * every resource given is not tiled and every buffer not a tile pool - what the runtime refuses. */
static HRESULT STDMETHODCALLTYPE ctx_UpdateTileMappings(Ctx *c, ID3D11Resource *resource, UINT regions, const D3D11_TILED_RESOURCE_COORDINATE *starts,
                                                        const D3D11_TILE_REGION_SIZE *sizes, ID3D11Buffer *pool, UINT ranges, const UINT *range_flags,
                                                        const UINT *pool_starts, const UINT *range_counts, UINT flags) {
  (void)c; (void)resource; (void)regions; (void)starts; (void)sizes; (void)pool; (void)ranges; (void)range_flags; (void)pool_starts; (void)range_counts; (void)flags;
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE ctx_CopyTileMappings(Ctx *c, ID3D11Resource *dst, const D3D11_TILED_RESOURCE_COORDINATE *dst_start, ID3D11Resource *src,
                                                      const D3D11_TILED_RESOURCE_COORDINATE *src_start, const D3D11_TILE_REGION_SIZE *size, UINT flags) {
  (void)c; (void)dst; (void)dst_start; (void)src; (void)src_start; (void)size; (void)flags;
  return E_INVALIDARG;
}
static void STDMETHODCALLTYPE ctx_CopyTiles(Ctx *c, ID3D11Resource *resource, const D3D11_TILED_RESOURCE_COORDINATE *start, const D3D11_TILE_REGION_SIZE *size,
                                            ID3D11Buffer *buffer, UINT64 offset, UINT flags) {
  (void)c; (void)resource; (void)start; (void)size; (void)buffer; (void)offset; (void)flags;      /* (no tiled resource: the call is invalid and does nothing) */
}
static void STDMETHODCALLTYPE ctx_UpdateTiles(Ctx *c, ID3D11Resource *dst, const D3D11_TILED_RESOURCE_COORDINATE *start, const D3D11_TILE_REGION_SIZE *size,
                                              const void *data, UINT flags) {
  (void)c; (void)dst; (void)start; (void)size; (void)data; (void)flags;      /* (no tiled resource: the call is invalid and does nothing) */
}
static HRESULT STDMETHODCALLTYPE ctx_ResizeTilePool(Ctx *c, ID3D11Buffer *pool, UINT64 size) { (void)c; (void)pool; (void)size; return E_INVALIDARG; }
/* (an ordering between uses of resources that alias tiles: the queue runs every command in order, so there is nothing to order) */
static void STDMETHODCALLTYPE ctx_TiledResourceBarrier(Ctx *c, ID3D11DeviceChild *before, ID3D11DeviceChild *after) { (void)c; (void)before; (void)after; }

/* ---- ID3D11Device2 ------------------------------------------------------------------------------------------------------------------------------------------ */

static void STDMETHODCALLTYPE dev_GetImmediateContext2(Dev *d, ID3D11DeviceContext2 **out) { dev_GetImmediateContext(d, (ID3D11DeviceContext **)out); }
static void STDMETHODCALLTYPE dev_GetImmediateContext3(Dev *d, ID3D11DeviceContext3 **out) { dev_GetImmediateContext(d, (ID3D11DeviceContext **)out); }
static HRESULT STDMETHODCALLTYPE dev_CreateDeferredContext2(Dev *d, UINT flags, ID3D11DeviceContext2 **out) { return dev_CreateDeferredContext(d, flags, (ID3D11DeviceContext **)out); }
static HRESULT STDMETHODCALLTYPE dev_CreateDeferredContext3(Dev *d, UINT flags, ID3D11DeviceContext3 **out) { return dev_CreateDeferredContext(d, flags, (ID3D11DeviceContext **)out); }

/* A resource that is not tiled has no tiles: the counts are 0 and the descriptions empty. */
static void STDMETHODCALLTYPE dev_GetResourceTiling(Dev *d, ID3D11Resource *resource, UINT *tile_count, D3D11_PACKED_MIP_DESC *mip_desc, D3D11_TILE_SHAPE *tile_shape,
                                                    UINT *subresource_count, UINT first_subresource, D3D11_SUBRESOURCE_TILING *tilings) {
  (void)d; (void)resource; (void)first_subresource; (void)tilings;
  if (tile_count) *tile_count = 0;
  if (mip_desc) memset(mip_desc, 0, sizeof *mip_desc);
  if (tile_shape) memset(tile_shape, 0, sizeof *tile_shape);
  if (subresource_count) *subresource_count = 0;
}
/* D3D11_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE asks about tiled resources, which have no quality levels here. */
static HRESULT STDMETHODCALLTYPE dev_CheckMultisampleQualityLevels1(Dev *d, DXGI_FORMAT format, UINT samples, UINT flags, UINT *levels) {
  if (!levels) return E_INVALIDARG;
  *levels = 0;
  if (flags & ~(UINT)D3D11_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE) return E_INVALIDARG;
  if (flags) return S_OK;
  return dev_CheckMultisampleQualityLevels(d, format, samples, levels);
}

/* ---- ID3D11Device3 ------------------------------------------------------------------------------------------------------------------------------------------ */

/* D3D11_TEXTURE_LAYOUT_ROW_MAJOR and 64K_STANDARD_SWIZZLE need MapOnDefaultTextures and StandardSwizzle, which the device does not have. */
static HRESULT STDMETHODCALLTYPE dev_CreateTexture2D1(Dev *d, const D3D11_TEXTURE2D_DESC1 *desc, const D3D11_SUBRESOURCE_DATA *initial, ID3D11Texture2D1 **out) {
  if (out) *out = NULL;
  if (!desc || desc->TextureLayout != D3D11_TEXTURE_LAYOUT_UNDEFINED) return E_INVALIDARG;
  const D3D11_TEXTURE2D_DESC plain = {desc->Width, desc->Height, desc->MipLevels, desc->ArraySize, desc->Format, desc->SampleDesc, desc->Usage, desc->BindFlags,
                                      desc->CPUAccessFlags, desc->MiscFlags};
  return dev_CreateTexture2D(d, &plain, initial, (ID3D11Texture2D **)out);
}
static HRESULT STDMETHODCALLTYPE dev_CreateTexture3D1(Dev *d, const D3D11_TEXTURE3D_DESC1 *desc, const D3D11_SUBRESOURCE_DATA *initial, ID3D11Texture3D1 **out) {
  if (out) *out = NULL;
  if (!desc || desc->TextureLayout != D3D11_TEXTURE_LAYOUT_UNDEFINED) return E_INVALIDARG;
  const D3D11_TEXTURE3D_DESC plain = {desc->Width, desc->Height, desc->Depth, desc->MipLevels, desc->Format, desc->Usage, desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags};
  return dev_CreateTexture3D(d, &plain, initial, (ID3D11Texture3D **)out);
}
static void STDMETHODCALLTYPE tex_GetDesc1(Tex *t, D3D11_TEXTURE2D_DESC1 *desc) {
  if (!desc) return;
  const D3D11_TEXTURE2D_DESC *s = &t->desc;
  const D3D11_TEXTURE2D_DESC1 d1 = {s->Width, s->Height, s->MipLevels, s->ArraySize, s->Format, s->SampleDesc, s->Usage, s->BindFlags, s->CPUAccessFlags, s->MiscFlags,
                                    D3D11_TEXTURE_LAYOUT_UNDEFINED};
  *desc = d1;
}
static void STDMETHODCALLTYPE tex3_GetDesc1(Tex *t, D3D11_TEXTURE3D_DESC1 *desc) {
  if (!desc) return;
  const D3D11_TEXTURE3D_DESC *s = &t->desc3;
  const D3D11_TEXTURE3D_DESC1 d1 = {s->Width, s->Height, s->Depth, s->MipLevels, s->Format, s->Usage, s->BindFlags, s->CPUAccessFlags, s->MiscFlags,
                                    D3D11_TEXTURE_LAYOUT_UNDEFINED};
  *desc = d1;
}

/* D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON needs a ConservativeRasterizationTier the device does not have. */
static HRESULT STDMETHODCALLTYPE dev_CreateRasterizerState2(Dev *d, const D3D11_RASTERIZER_DESC2 *desc, ID3D11RasterizerState2 **out) {
  if (out) *out = NULL;
  if (!desc || desc->ConservativeRaster != D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF) return E_INVALIDARG;
  const D3D11_RASTERIZER_DESC1 d1 = {desc->FillMode, desc->CullMode, desc->FrontCounterClockwise, desc->DepthBias, desc->DepthBiasClamp, desc->SlopeScaledDepthBias,
                                     desc->DepthClipEnable, desc->ScissorEnable, desc->MultisampleEnable, desc->AntialiasedLineEnable, desc->ForcedSampleCount};
  return dev_CreateRasterizerState1(d, &d1, (ID3D11RasterizerState1 **)out);
}
static void STDMETHODCALLTYPE raster_GetDesc2(Raster *s, D3D11_RASTERIZER_DESC2 *desc) {
  if (!desc) return;
  const D3D11_RASTERIZER_DESC1 *k = &s->desc1;
  const D3D11_RASTERIZER_DESC2 d2 = {k->FillMode, k->CullMode, k->FrontCounterClockwise, k->DepthBias, k->DepthBiasClamp, k->SlopeScaledDepthBias, k->DepthClipEnable,
                                     k->ScissorEnable, k->MultisampleEnable, k->AntialiasedLineEnable, k->ForcedSampleCount, D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF};
  *desc = d2;
}

static HRESULT STDMETHODCALLTYPE dev_CreateQuery1(Dev *d, const D3D11_QUERY_DESC1 *desc, ID3D11Query1 **out) {
  if (out) *out = NULL;
  if (!desc || (UINT)desc->ContextType > (UINT)D3D11_CONTEXT_TYPE_VIDEO) return E_INVALIDARG;
  const D3D11_QUERY_DESC plain = {desc->Query, desc->MiscFlags};
  Query *q = NULL;
  const HRESULT hr = query_make(d, &plain, FALSE, out ? (void **)&q : NULL);
  if (FAILED(hr) || !q) return hr;
  q->context_type = desc->ContextType;
  *out = (ID3D11Query1 *)q;
  return hr;
}
static void STDMETHODCALLTYPE query_GetDesc1(Query *q, D3D11_QUERY_DESC1 *desc) {
  if (!desc) return;
  desc->Query = q->desc.Query;
  desc->MiscFlags = q->desc.MiscFlags;
  desc->ContextType = q->context_type;
}

/* The *1 views: the descriptions of Direct3D 11.0 with a plane slice in the two-dimensional members. Every format here has one plane, so the slice is 0 (any other
 * is E_INVALIDARG, as for a format of one plane on Windows); the views are made, and kept, as their 11.0 descriptions. Every view has the *1 interface. */
static BOOL rtv_desc_plain(const D3D11_RENDER_TARGET_VIEW_DESC1 *in, D3D11_RENDER_TARGET_VIEW_DESC *out) {
  memset(out, 0, sizeof *out);
  out->Format = in->Format;
  out->ViewDimension = in->ViewDimension;
  switch (in->ViewDimension) {
    case D3D11_RTV_DIMENSION_BUFFER: out->Buffer = in->Buffer; return TRUE;
    case D3D11_RTV_DIMENSION_TEXTURE1D: out->Texture1D = in->Texture1D; return TRUE;
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: out->Texture1DArray = in->Texture1DArray; return TRUE;
    case D3D11_RTV_DIMENSION_TEXTURE2D:
      out->Texture2D.MipSlice = in->Texture2D.MipSlice;
      return in->Texture2D.PlaneSlice == 0;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
      out->Texture2DArray.MipSlice = in->Texture2DArray.MipSlice;
      out->Texture2DArray.FirstArraySlice = in->Texture2DArray.FirstArraySlice;
      out->Texture2DArray.ArraySize = in->Texture2DArray.ArraySize;
      return in->Texture2DArray.PlaneSlice == 0;
    case D3D11_RTV_DIMENSION_TEXTURE2DMS: out->Texture2DMS = in->Texture2DMS; return TRUE;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY: out->Texture2DMSArray = in->Texture2DMSArray; return TRUE;
    case D3D11_RTV_DIMENSION_TEXTURE3D: out->Texture3D = in->Texture3D; return TRUE;
    default: return FALSE;
  }
}
static void STDMETHODCALLTYPE rtv_GetDesc1(View *v, D3D11_RENDER_TARGET_VIEW_DESC1 *desc) {
  if (!desc) return;
  const D3D11_RENDER_TARGET_VIEW_DESC *in = &v->desc.rtv;
  memset(desc, 0, sizeof *desc);
  desc->Format = in->Format;
  desc->ViewDimension = in->ViewDimension;
  switch (in->ViewDimension) {
    case D3D11_RTV_DIMENSION_BUFFER: desc->Buffer = in->Buffer; break;
    case D3D11_RTV_DIMENSION_TEXTURE1D: desc->Texture1D = in->Texture1D; break;
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: desc->Texture1DArray = in->Texture1DArray; break;
    case D3D11_RTV_DIMENSION_TEXTURE2D: desc->Texture2D.MipSlice = in->Texture2D.MipSlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
      desc->Texture2DArray.MipSlice = in->Texture2DArray.MipSlice;
      desc->Texture2DArray.FirstArraySlice = in->Texture2DArray.FirstArraySlice;
      desc->Texture2DArray.ArraySize = in->Texture2DArray.ArraySize;
      break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMS: desc->Texture2DMS = in->Texture2DMS; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY: desc->Texture2DMSArray = in->Texture2DMSArray; break;
    case D3D11_RTV_DIMENSION_TEXTURE3D: desc->Texture3D = in->Texture3D; break;
    default: break;
  }
}
static HRESULT STDMETHODCALLTYPE dev_CreateRenderTargetView1(Dev *d, ID3D11Resource *resource, const D3D11_RENDER_TARGET_VIEW_DESC1 *desc, ID3D11RenderTargetView1 **out) {
  if (out) *out = NULL;
  D3D11_RENDER_TARGET_VIEW_DESC plain;
  if (desc && !rtv_desc_plain(desc, &plain)) return E_INVALIDARG;
  return dev_CreateRenderTargetView(d, resource, desc ? &plain : NULL, (ID3D11RenderTargetView **)out);
}

static BOOL srv_desc_plain(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *in, D3D11_SHADER_RESOURCE_VIEW_DESC *out) {
  memset(out, 0, sizeof *out);
  out->Format = in->Format;
  out->ViewDimension = in->ViewDimension;
  switch (in->ViewDimension) {
    case D3D11_SRV_DIMENSION_BUFFER: out->Buffer = in->Buffer; return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURE1D: out->Texture1D = in->Texture1D; return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY: out->Texture1DArray = in->Texture1DArray; return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURE2D:
      out->Texture2D.MostDetailedMip = in->Texture2D.MostDetailedMip;
      out->Texture2D.MipLevels = in->Texture2D.MipLevels;
      return in->Texture2D.PlaneSlice == 0;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
      out->Texture2DArray.MostDetailedMip = in->Texture2DArray.MostDetailedMip;
      out->Texture2DArray.MipLevels = in->Texture2DArray.MipLevels;
      out->Texture2DArray.FirstArraySlice = in->Texture2DArray.FirstArraySlice;
      out->Texture2DArray.ArraySize = in->Texture2DArray.ArraySize;
      return in->Texture2DArray.PlaneSlice == 0;
    case D3D11_SRV_DIMENSION_TEXTURE2DMS: out->Texture2DMS = in->Texture2DMS; return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY: out->Texture2DMSArray = in->Texture2DMSArray; return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURE3D: out->Texture3D = in->Texture3D; return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURECUBE: out->TextureCube = in->TextureCube; return TRUE;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY: out->TextureCubeArray = in->TextureCubeArray; return TRUE;
    case D3D11_SRV_DIMENSION_BUFFEREX: out->BufferEx = in->BufferEx; return TRUE;
    default: return FALSE;
  }
}
static void STDMETHODCALLTYPE srv_GetDesc1(View *v, D3D11_SHADER_RESOURCE_VIEW_DESC1 *desc) {
  if (!desc) return;
  const D3D11_SHADER_RESOURCE_VIEW_DESC *in = &v->desc.srv;
  memset(desc, 0, sizeof *desc);
  desc->Format = in->Format;
  desc->ViewDimension = in->ViewDimension;
  switch (in->ViewDimension) {
    case D3D11_SRV_DIMENSION_BUFFER: desc->Buffer = in->Buffer; break;
    case D3D11_SRV_DIMENSION_TEXTURE1D: desc->Texture1D = in->Texture1D; break;
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY: desc->Texture1DArray = in->Texture1DArray; break;
    case D3D11_SRV_DIMENSION_TEXTURE2D:
      desc->Texture2D.MostDetailedMip = in->Texture2D.MostDetailedMip;
      desc->Texture2D.MipLevels = in->Texture2D.MipLevels;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
      desc->Texture2DArray.MostDetailedMip = in->Texture2DArray.MostDetailedMip;
      desc->Texture2DArray.MipLevels = in->Texture2DArray.MipLevels;
      desc->Texture2DArray.FirstArraySlice = in->Texture2DArray.FirstArraySlice;
      desc->Texture2DArray.ArraySize = in->Texture2DArray.ArraySize;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMS: desc->Texture2DMS = in->Texture2DMS; break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY: desc->Texture2DMSArray = in->Texture2DMSArray; break;
    case D3D11_SRV_DIMENSION_TEXTURE3D: desc->Texture3D = in->Texture3D; break;
    case D3D11_SRV_DIMENSION_TEXTURECUBE: desc->TextureCube = in->TextureCube; break;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY: desc->TextureCubeArray = in->TextureCubeArray; break;
    case D3D11_SRV_DIMENSION_BUFFEREX: desc->BufferEx = in->BufferEx; break;
    default: break;
  }
}
static HRESULT STDMETHODCALLTYPE dev_CreateShaderResourceView1(Dev *d, ID3D11Resource *resource, const D3D11_SHADER_RESOURCE_VIEW_DESC1 *desc, ID3D11ShaderResourceView1 **out) {
  if (out) *out = NULL;
  D3D11_SHADER_RESOURCE_VIEW_DESC plain;
  if (desc && !srv_desc_plain(desc, &plain)) return E_INVALIDARG;
  return dev_CreateShaderResourceView(d, resource, desc ? &plain : NULL, (ID3D11ShaderResourceView **)out);
}

static BOOL uav_desc_plain(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *in, D3D11_UNORDERED_ACCESS_VIEW_DESC *out) {
  memset(out, 0, sizeof *out);
  out->Format = in->Format;
  out->ViewDimension = in->ViewDimension;
  switch (in->ViewDimension) {
    case D3D11_UAV_DIMENSION_BUFFER: out->Buffer = in->Buffer; return TRUE;
    case D3D11_UAV_DIMENSION_TEXTURE1D: out->Texture1D = in->Texture1D; return TRUE;
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY: out->Texture1DArray = in->Texture1DArray; return TRUE;
    case D3D11_UAV_DIMENSION_TEXTURE2D:
      out->Texture2D.MipSlice = in->Texture2D.MipSlice;
      return in->Texture2D.PlaneSlice == 0;
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
      out->Texture2DArray.MipSlice = in->Texture2DArray.MipSlice;
      out->Texture2DArray.FirstArraySlice = in->Texture2DArray.FirstArraySlice;
      out->Texture2DArray.ArraySize = in->Texture2DArray.ArraySize;
      return in->Texture2DArray.PlaneSlice == 0;
    case D3D11_UAV_DIMENSION_TEXTURE3D: out->Texture3D = in->Texture3D; return TRUE;
    default: return FALSE;
  }
}
static void STDMETHODCALLTYPE uav_GetDesc1(View *v, D3D11_UNORDERED_ACCESS_VIEW_DESC1 *desc) {
  if (!desc) return;
  const D3D11_UNORDERED_ACCESS_VIEW_DESC *in = &v->desc.uav;
  memset(desc, 0, sizeof *desc);
  desc->Format = in->Format;
  desc->ViewDimension = in->ViewDimension;
  switch (in->ViewDimension) {
    case D3D11_UAV_DIMENSION_BUFFER: desc->Buffer = in->Buffer; break;
    case D3D11_UAV_DIMENSION_TEXTURE1D: desc->Texture1D = in->Texture1D; break;
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY: desc->Texture1DArray = in->Texture1DArray; break;
    case D3D11_UAV_DIMENSION_TEXTURE2D: desc->Texture2D.MipSlice = in->Texture2D.MipSlice; break;
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
      desc->Texture2DArray.MipSlice = in->Texture2DArray.MipSlice;
      desc->Texture2DArray.FirstArraySlice = in->Texture2DArray.FirstArraySlice;
      desc->Texture2DArray.ArraySize = in->Texture2DArray.ArraySize;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE3D: desc->Texture3D = in->Texture3D; break;
    default: break;
  }
}
static HRESULT STDMETHODCALLTYPE dev_CreateUnorderedAccessView1(Dev *d, ID3D11Resource *resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *desc, ID3D11UnorderedAccessView1 **out) {
  if (out) *out = NULL;
  D3D11_UNORDERED_ACCESS_VIEW_DESC plain;
  if (desc && !uav_desc_plain(desc, &plain)) return E_INVALIDARG;
  return dev_CreateUnorderedAccessView(d, resource, desc ? &plain : NULL, (ID3D11UnorderedAccessView **)out);
}

/* WriteToSubresource and ReadFromSubresource reach a default-usage texture the CPU has mapped (Map with no D3D11_MAPPED_SUBRESOURCE), which needs
 * MapOnDefaultTextures: the device does not have it, so no texture can be made with CPU access and default usage, and no resource is one these calls take. The call
 * is invalid; nothing is written or read. */
static void STDMETHODCALLTYPE dev_WriteToSubresource(Dev *d, ID3D11Resource *dst, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch) {
  (void)d; (void)dst; (void)sub; (void)box; (void)data; (void)row_pitch; (void)depth_pitch;
}
static void STDMETHODCALLTYPE dev_ReadFromSubresource(Dev *d, void *data, UINT row_pitch, UINT depth_pitch, ID3D11Resource *src, UINT sub, const D3D11_BOX *box) {
  (void)d; (void)data; (void)row_pitch; (void)depth_pitch; (void)src; (void)sub; (void)box;
}

/* ---- ID3D11Device4: device-removed events --------------------------------------------------------------------------------------------------------------- */

/* Direct3D 12 below signals every fence of a device it loses with the largest value: a fence of the device's own is waited on for that by a thread, which sets the
 * registered events (each a duplicate of the program's handle, so the program may close its own). The fence is signalled from here too when the device goes, which
 * ends the thread. */
typedef struct RemovalWatch {
  ID3D12Fence *fence;
  HANDLE wake, thread;
  SRWLOCK lock;
  LONG closing, removed;
  DWORD next_cookie;
  struct Watched { DWORD cookie; HANDLE event; } *events;
  UINT count, capacity;
} RemovalWatch;

static DWORD WINAPI removal_thread(LPVOID param) {
  RemovalWatch *w = param;
  WaitForSingleObject(w->wake, INFINITE);
  if (InterlockedCompareExchange(&w->closing, 0, 0)) return 0;
  AcquireSRWLockExclusive(&w->lock);
  InterlockedExchange(&w->removed, 1);
  for (UINT i = 0; i < w->count; ++i) SetEvent(w->events[i].event);
  ReleaseSRWLockExclusive(&w->lock);
  return 0;
}
static HRESULT removal_start(Dev *d) {
  if (d->removal) return S_OK;
  RemovalWatch *w = mem_alloc(sizeof *w);
  if (!w) return E_OUTOFMEMORY;
  memset(w, 0, sizeof *w);
  InitializeSRWLock(&w->lock);
  w->next_cookie = 1;
  HRESULT hr = ID3D12Device_CreateFence(d->d12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&w->fence);
  if (SUCCEEDED(hr)) { w->wake = CreateEventW(NULL, TRUE, FALSE, NULL); if (!w->wake) hr = E_OUTOFMEMORY; }
  if (SUCCEEDED(hr)) hr = ID3D12Fence_SetEventOnCompletion(w->fence, ~(UINT64)0, w->wake);
  if (SUCCEEDED(hr)) { w->thread = CreateThread(NULL, 0, removal_thread, w, 0, NULL); if (!w->thread) hr = E_OUTOFMEMORY; }
  if (FAILED(hr)) {
    if (w->fence) { if (w->wake) ID3D12Fence_Signal(w->fence, ~(UINT64)0); ID3D12Fence_Release(w->fence); }      /* (ends the fence's own waiter) */
    if (w->wake) CloseHandle(w->wake);
    mem_free(w);
    return hr;
  }
  d->removal = w;
  return S_OK;
}
static void removal_shutdown(Dev *d) {
  RemovalWatch *w = d->removal;
  if (!w) return;
  d->removal = NULL;
  InterlockedExchange(&w->closing, 1);
  ID3D12Fence_Signal(w->fence, ~(UINT64)0);      /* (the waiter of Direct3D 12 sets `wake`, and the thread ends) */
  WaitForSingleObject(w->thread, INFINITE);
  CloseHandle(w->thread);
  CloseHandle(w->wake);
  ID3D12Fence_Release(w->fence);
  for (UINT i = 0; i < w->count; ++i) CloseHandle(w->events[i].event);
  mem_free(w->events);
  mem_free(w);
}
static HRESULT STDMETHODCALLTYPE dev_RegisterDeviceRemovedEvent(Dev *d, HANDLE event, DWORD *cookie) {
  if (cookie) *cookie = 0;
  if (!event || !cookie) return E_INVALIDARG;
  HANDLE mine = NULL;
  if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS)) return E_INVALIDARG;
  EnterCriticalSection(&d->lock);
  HRESULT hr = removal_start(d);
  RemovalWatch *w = d->removal;
  if (SUCCEEDED(hr)) {
    AcquireSRWLockExclusive(&w->lock);
    if (w->count == w->capacity) {
      const UINT capacity = w->capacity ? w->capacity * 2 : 4;
      struct Watched *grown = capacity > w->capacity ? mem_alloc(sizeof *grown * capacity) : NULL;
      if (!grown) hr = E_OUTOFMEMORY;
      else {
        for (UINT i = 0; i < w->count; ++i) grown[i] = w->events[i];
        mem_free(w->events);
        w->events = grown;
        w->capacity = capacity;
      }
    }
    if (SUCCEEDED(hr)) {
      DWORD c = w->next_cookie++;
      if (c == 0) c = w->next_cookie++;      /* (0 is no cookie) */
      w->events[w->count].cookie = c;
      w->events[w->count].event = mine;
      ++w->count;
      *cookie = c;
      mine = NULL;
      /* (a device lost already: set at once) */
      if (InterlockedCompareExchange(&w->removed, 0, 0) || FAILED(ID3D12Device_GetDeviceRemovedReason(d->d12))) SetEvent(w->events[w->count - 1].event);
    }
    ReleaseSRWLockExclusive(&w->lock);
  }
  LeaveCriticalSection(&d->lock);
  if (mine) CloseHandle(mine);
  return hr;
}
static void STDMETHODCALLTYPE dev_UnregisterDeviceRemoved(Dev *d, DWORD cookie) {
  EnterCriticalSection(&d->lock);
  RemovalWatch *w = d->removal;
  if (w) {
    AcquireSRWLockExclusive(&w->lock);
    for (UINT i = 0; i < w->count; ++i) {
      if (w->events[i].cookie != cookie) continue;
      CloseHandle(w->events[i].event);
      w->events[i] = w->events[--w->count];
      break;
    }
    ReleaseSRWLockExclusive(&w->lock);
  }
  LeaveCriticalSection(&d->lock);
}
