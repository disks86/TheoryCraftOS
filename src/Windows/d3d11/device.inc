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

/* Part of d3d11.c: the device itself - what it answers about itself, its two other faces (IDXGIDevice for programs that go from the device to its
 * adapter and factory, ITcosPresenter for dxgi.dll's swap chains), how it is made, and the entry points of the library. */

/* ---- the device's own methods ----------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dev_QueryInterface(Dev *d, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ID3D11Device)) *out = d;
  else if (guid_eq(riid, &IID_IDXGIObject) || guid_eq(riid, &IID_IDXGIDevice) || guid_eq(riid, &IID_IDXGIDevice1) || guid_eq(riid, &IID_IDXGIDevice2) ||
           guid_eq(riid, &IID_IDXGIDevice3)) *out = &d->dxgi;      /* (DXGI 1.2: ANGLE needs it to present to another process's window) */
  else if (guid_eq(riid, &IID_ITcosPresenter)) *out = &d->presenter;
  else { dx_no_interface("ID3D11Device", riid); return E_NOINTERFACE; }
  dx_AddRef(OBJ(d));
  return S_OK;
}
static D3D_FEATURE_LEVEL STDMETHODCALLTYPE dev_GetFeatureLevel(Dev *d) { return d->level; }
static UINT STDMETHODCALLTYPE dev_GetCreationFlags(Dev *d) { return d->flags; }
static HRESULT STDMETHODCALLTYPE dev_GetDeviceRemovedReason(Dev *d) { (void)d; return S_OK; }
static HRESULT STDMETHODCALLTYPE dev_SetExceptionMode(Dev *d, UINT flags) { (void)d; (void)flags; return S_OK; }
static UINT STDMETHODCALLTYPE dev_GetExceptionMode(Dev *d) { (void)d; return 0; }
static void STDMETHODCALLTYPE dev_GetImmediateContext(Dev *d, ID3D11DeviceContext **out) {
  if (!out) return;
  ctx_AddRef(d->ctx);
  *out = (ID3D11DeviceContext *)d->ctx;
}
/* The quality levels of a multisampled target of `format` with `samples` samples: what Direct3D 12 below says for the format the texture is stored in (one, the
 * standard pattern, for the counts the adapter draws with; 0 for the others). */
static UINT dev_ms_levels(Dev *d, DXGI_FORMAT format, UINT samples) {
  if (samples == 1) return 1;
  DXGI_FORMAT stored;
  UINT bpp, stored_bpp;
  int expand, depth;
  if (!format_of(format, D3D11_BIND_RENDER_TARGET, &stored, &bpp, &stored_bpp, &expand, &depth) &&
      !format_of(format, D3D11_BIND_DEPTH_STENCIL, &stored, &bpp, &stored_bpp, &expand, &depth)) return 0;
  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {stored, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
  if (FAILED(ID3D12Device_CheckFeatureSupport(d->d12, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof q))) return 0;
  return q.NumQualityLevels;
}
static HRESULT STDMETHODCALLTYPE dev_CheckFormatSupport(Dev *d, DXGI_FORMAT format, UINT *support) {
  DXGI_FORMAT stored;
  UINT bpp, stored_bpp;
  int expand, depth, convert;
  if (!support) return E_INVALIDARG;
  *support = 0;
  if (format_of(format, 0, &stored, &bpp, &stored_bpp, &expand, &depth)) {
    *support |= D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
    if (!expand) *support |= D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE | D3D11_FORMAT_SUPPORT_DISPLAY;
    else if (expand != 2) *support |= D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE;      /* (R8 and RG8: masked RGBA8) */
  } else if (format_of(format, D3D11_BIND_DEPTH_STENCIL, &stored, &bpp, &stored_bpp, &expand, &depth)) {
    *support |= D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
  }
  if (element_floats(format, &convert)) *support |= D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER;
  if (format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_R32_UINT) *support |= D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER;
  if ((*support & (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL)) && dev_ms_levels(d, format, 4))
    *support |= D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;      /* (drawn into and resolved, not read by shaders) */
  return *support ? S_OK : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE dev_CheckMultisampleQualityLevels(Dev *d, DXGI_FORMAT format, UINT samples, UINT *levels) {
  if (!levels) return E_INVALIDARG;
  *levels = samples >= 1 && samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT ? dev_ms_levels(d, format, samples) : 0;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dev_CheckFeatureSupport(Dev *d, D3D11_FEATURE feature, void *data, UINT size) {
  (void)d;
  if (!data) return E_INVALIDARG;
  switch (feature) {      /* (all "no": no concurrent creates or command lists of the driver, no doubles, no compute on a level below 11) */
    case D3D11_FEATURE_THREADING: if (size != sizeof(D3D11_FEATURE_DATA_THREADING)) return E_INVALIDARG; break;
    case D3D11_FEATURE_DOUBLES: if (size != sizeof(D3D11_FEATURE_DATA_DOUBLES)) return E_INVALIDARG; break;
    case D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS: if (size != sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS)) return E_INVALIDARG; break;
    default: return E_INVALIDARG;
  }
  memset(data, 0, size);
  return S_OK;
}

/* ---- IDXGIDevice -------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE dxgi_QueryInterface(IDXGIDevice1 *p, REFIID riid, void **out) { return dev_QueryInterface(dev_of_dxgi(p), riid, out); }
static ULONG STDMETHODCALLTYPE dxgi_AddRef(IDXGIDevice1 *p) { return dx_AddRef(OBJ(dev_of_dxgi(p))); }
static ULONG STDMETHODCALLTYPE dxgi_Release(IDXGIDevice1 *p) { return dx_Release(OBJ(dev_of_dxgi(p))); }
static HRESULT STDMETHODCALLTYPE dxgi_SetPrivateData(IDXGIDevice1 *p, REFGUID guid, UINT size, const void *data) { return dx_SetPrivateData(OBJ(dev_of_dxgi(p)), guid, size, data); }
static HRESULT STDMETHODCALLTYPE dxgi_SetPrivateDataInterface(IDXGIDevice1 *p, REFGUID guid, const IUnknown *object) { return dx_SetPrivateDataInterface(OBJ(dev_of_dxgi(p)), guid, object); }
static HRESULT STDMETHODCALLTYPE dxgi_GetPrivateData(IDXGIDevice1 *p, REFGUID guid, UINT *size, void *data) { return dx_GetPrivateData(OBJ(dev_of_dxgi(p)), guid, size, data); }
static HRESULT STDMETHODCALLTYPE dxgi_GetParent(IDXGIDevice1 *p, REFIID riid, void **out) { return IDXGIAdapter_QueryInterface(dev_of_dxgi(p)->adapter, riid, out); }
static HRESULT STDMETHODCALLTYPE dxgi_GetAdapter(IDXGIDevice1 *p, IDXGIAdapter **out) {
  if (!out) return E_INVALIDARG;
  *out = dev_of_dxgi(p)->adapter;
  IDXGIAdapter_AddRef(*out);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dxgi_CreateSurface(IDXGIDevice1 *p, const DXGI_SURFACE_DESC *desc, UINT count, DXGI_USAGE usage, const DXGI_SHARED_RESOURCE *shared, IDXGISurface **out) {
  (void)p; (void)desc; (void)count; (void)usage; (void)shared;
  if (out) *out = NULL;
  return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE dxgi_QueryResourceResidency(IDXGIDevice1 *p, IUnknown *const *resources, DXGI_RESIDENCY *status, UINT count) {
  (void)p; (void)resources;
  for (UINT i = 0; status && i < count; ++i) status[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dxgi_SetGPUThreadPriority(IDXGIDevice1 *p, INT priority) { (void)p; (void)priority; return S_OK; }
static HRESULT STDMETHODCALLTYPE dxgi_GetGPUThreadPriority(IDXGIDevice1 *p, INT *priority) { (void)p; if (priority) *priority = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE dxgi_SetMaximumFrameLatency(IDXGIDevice1 *p, UINT latency) { (void)p; (void)latency; return S_OK; }
static HRESULT STDMETHODCALLTYPE dxgi_GetMaximumFrameLatency(IDXGIDevice1 *p, UINT *latency) { (void)p; if (latency) *latency = 3; return S_OK; }
/* IDXGIDevice2 and 3: memory is never taken back from a resource, so an offered one is always reclaimed intact; the queue runs each list before it returns,
 * so the work before an EnqueueSetEvent is done when it is called. */
static HRESULT STDMETHODCALLTYPE dxgi_OfferResources(IDXGIDevice1 *p, UINT count, IDXGIResource *const *resources, DXGI_OFFER_RESOURCE_PRIORITY priority) {
  (void)p; (void)priority;
  return count && !resources ? E_INVALIDARG : S_OK;
}
static HRESULT STDMETHODCALLTYPE dxgi_ReclaimResources(IDXGIDevice1 *p, UINT count, IDXGIResource *const *resources, BOOL *discarded) {
  (void)p;
  if (count && !resources) return E_INVALIDARG;
  for (UINT i = 0; discarded && i < count; ++i) discarded[i] = FALSE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dxgi_EnqueueSetEvent(IDXGIDevice1 *p, HANDLE event) {
  Dev *d = dev_of_dxgi(p);
  if (!event) return E_INVALIDARG;
  EnterCriticalSection(&d->lock);
  flush(d);
  LeaveCriticalSection(&d->lock);
  return SetEvent(event) ? S_OK : E_INVALIDARG;
}
static void STDMETHODCALLTYPE dxgi_Trim(IDXGIDevice1 *p) { (void)p; }
#define DXGI_SLOT(name) .name = (__typeof__(((IDXGIDevice3Vtbl *)0)->name))dxgi_##name
static const IDXGIDevice3Vtbl g_dxgi_vtbl = {
    DXGI_SLOT(QueryInterface), DXGI_SLOT(AddRef), DXGI_SLOT(Release), DXGI_SLOT(SetPrivateData), DXGI_SLOT(SetPrivateDataInterface), DXGI_SLOT(GetPrivateData),
    DXGI_SLOT(GetParent), DXGI_SLOT(GetAdapter), DXGI_SLOT(CreateSurface), DXGI_SLOT(QueryResourceResidency), DXGI_SLOT(SetGPUThreadPriority),
    DXGI_SLOT(GetGPUThreadPriority), DXGI_SLOT(SetMaximumFrameLatency), DXGI_SLOT(GetMaximumFrameLatency), DXGI_SLOT(OfferResources), DXGI_SLOT(ReclaimResources),
    DXGI_SLOT(EnqueueSetEvent), DXGI_SLOT(Trim),
};
#undef DXGI_SLOT

/* ---- the swap chain's side (dxgi.dll) -------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE pres_QueryInterface(ITcosPresenter *p, REFIID riid, void **out) { return dev_QueryInterface(dev_of_presenter(p), riid, out); }
static ULONG STDMETHODCALLTYPE pres_AddRef(ITcosPresenter *p) { return dx_AddRef(OBJ(dev_of_presenter(p))); }
static ULONG STDMETHODCALLTYPE pres_Release(ITcosPresenter *p) { return dx_Release(OBJ(dev_of_presenter(p))); }
/* The back buffers are the Direct3D 12 queue's, each inside a texture of this runtime: what GetBuffer of the swap chain hands a program. */
static HRESULT STDMETHODCALLTYPE pres_CreateBackBuffers(ITcosPresenter *p, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT usage, IUnknown **out) {
  Dev *d = dev_of_presenter(p);
  ITcosPresenter *inner = NULL;
  IUnknown *made[16];
  if (count > 16 || FAILED(ID3D12CommandQueue_QueryInterface(d->queue, &IID_ITcosPresenter, (void **)&inner))) return E_FAIL;
  HRESULT hr = inner->lpVtbl->CreateBackBuffers(inner, count, width, height, format, usage, made);
  inner->lpVtbl->Release(inner);
  if (FAILED(hr)) return hr;
  D3D11_TEXTURE2D_DESC desc = {width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
  for (UINT i = 0; i < count; ++i) {
    Tex *t = NULL;
    if (SUCCEEDED(hr)) hr = tex_make(d, &desc, (ID3D12Resource *)made[i], &t);
    made[i]->lpVtbl->Release(made[i]);
    out[i] = (IUnknown *)t;
  }
  if (FAILED(hr)) for (UINT i = 0; i < count; ++i) if (out[i]) { out[i]->lpVtbl->Release(out[i]); out[i] = NULL; }
  return hr;
}
static HRESULT STDMETHODCALLTYPE pres_ReadBackBuffer(ITcosPresenter *p, IUnknown *buffer, void *dst, UINT pitch) {
  Dev *d = dev_of_presenter(p);
  Tex *t = (Tex *)buffer;
  ITcosPresenter *inner = NULL;
  if (!t || FAILED(ID3D12CommandQueue_QueryInterface(d->queue, &IID_ITcosPresenter, (void **)&inner))) return E_FAIL;
  EnterCriticalSection(&d->lock);
  flush(d);
  const HRESULT hr = inner->lpVtbl->ReadBackBuffer(inner, (IUnknown *)t->res, dst, pitch);
  LeaveCriticalSection(&d->lock);
  inner->lpVtbl->Release(inner);
  return hr;
}
static const ITcosPresenterVtbl g_presenter_vtbl = {pres_QueryInterface, pres_AddRef, pres_Release, pres_CreateBackBuffers, pres_ReadBackBuffer};

/* ---- making a device ---------------------------------------------------------------------------------------------------------------------------------- */

static void dev_destroy(DxObj *o) {
  Dev *d = (Dev *)o;
  InterlockedExchange(&d->o.ref, 16);      /* (the references given back below - the blank texture's, the bound state's - are not the last again) */
  if (d->list) flush(d);
  if (d->ctx) { ctx_unbind_all(d->ctx); mem_free(d->ctx); }
  if (d->blank) dx_Release(OBJ(d->blank));
  for (UINT i = 0; i < d->pipeline_count; ++i) if (d->pipelines[i].state) ID3D12PipelineState_Release(d->pipelines[i].state);
  if (d->ring) ID3D12Resource_Release(d->ring);
  if (d->view_heap) ID3D12DescriptorHeap_Release(d->view_heap);
  if (d->sampler_heap) ID3D12DescriptorHeap_Release(d->sampler_heap);
  if (d->rtv_heap) ID3D12DescriptorHeap_Release(d->rtv_heap);
  if (d->dsv_heap) ID3D12DescriptorHeap_Release(d->dsv_heap);
  if (d->root) ID3D12RootSignature_Release(d->root);
  if (d->list) ID3D12GraphicsCommandList_Release(d->list);
  if (d->allocator) ID3D12CommandAllocator_Release(d->allocator);
  if (d->queue) ID3D12CommandQueue_Release(d->queue);
  if (d->d12) ID3D12Device_Release(d->d12);
  if (d->adapter) IDXGIAdapter_Release(d->adapter);
  if (d->factory) IDXGIFactory1_Release(d->factory);
  DeleteCriticalSection(&d->lock);
}

static HRESULT make_heap(Dev *d, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, BOOL visible, ID3D12DescriptorHeap **out) {
  D3D12_DESCRIPTOR_HEAP_DESC desc = {type, count, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  return ID3D12Device_CreateDescriptorHeap(d->d12, &desc, &IID_ID3D12DescriptorHeap, (void **)out);
}

/* The Direct3D 12 half of a device: FALSE when some part of it could not be made. */
static HRESULT dev_start(Dev *d) {
  HRESULT hr = D3D12CreateDevice((IUnknown *)d->adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&d->d12);
  if (FAILED(hr) || !d->d12) return DXGI_ERROR_UNSUPPORTED;
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  hr = ID3D12Device_CreateCommandQueue(d->d12, &qd, &IID_ID3D12CommandQueue, (void **)&d->queue);
  if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(d->d12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&d->allocator);
  if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandList(d->d12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, d->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&d->list);
  if (FAILED(hr)) return hr;

  /* the one root signature: the block of constants (b0), the pixel shader's textures (t0 .. t7) and its samplers (s0 .. s7) */
  D3D12_DESCRIPTOR_RANGE views = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, TEXTURE_SLOTS, 0, 0, 0}, samplers = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, TEXTURE_SLOTS, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3];
  memset(params, 0, sizeof params);
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &views;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &samplers;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rsd = {3, params, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = NULL, *error = NULL;
  hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
  if (error) ID3D10Blob_Release(error);
  if (SUCCEEDED(hr) && blob)
    hr = ID3D12Device_CreateRootSignature(d->d12, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&d->root);
  if (blob) ID3D10Blob_Release(blob);
  if (FAILED(hr) || !d->root) return FAILED(hr) ? hr : E_FAIL;

  hr = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_SLOTS, FALSE, &d->rtv_heap);
  if (SUCCEEDED(hr)) hr = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_SLOTS, FALSE, &d->dsv_heap);
  if (SUCCEEDED(hr)) hr = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, VIEW_HEAP, TRUE, &d->view_heap);
  if (SUCCEEDED(hr)) hr = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, VIEW_HEAP, TRUE, &d->sampler_heap);
  if (FAILED(hr)) return hr;
  for (UINT i = 0; i < RTV_SLOTS; ++i) d->rtv_free[i] = RTV_SLOTS - 1 - i;
  for (UINT i = 0; i < DSV_SLOTS; ++i) d->dsv_free[i] = DSV_SLOTS - 1 - i;
  d->rtv_free_count = RTV_SLOTS;
  d->dsv_free_count = DSV_SLOTS;
  d->ring = make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, RING_BYTES);
  if (!d->ring || FAILED(ID3D12Resource_Map(d->ring, 0, NULL, (void **)&d->ring_data)) || !d->ring_data) return E_OUTOFMEMORY;
  d->epoch = 1;
  d->dirty = DIRTY_ALL;

  static const BYTE white[4] = {255, 255, 255, 255};
  D3D11_TEXTURE2D_DESC blank = {1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  hr = tex_make(d, &blank, NULL, &d->blank);
  if (FAILED(hr)) return hr;
  InterlockedDecrement(&d->o.ref);      /* (the device's own texture does not hold the device) */
  hr = tex_write(d, d->blank, 0, 0, 0, 1, 1, white, 4);
  if (FAILED(hr)) return hr;

  Ctx *c = mem_alloc(sizeof *c);
  if (!c) return E_OUTOFMEMORY;
  dx_init(&c->o, &ctx_vtbl, g_ctx_iids, NULL);
  c->dev = d;
  c->sample_mask = 0xFFFFFFFFu;
  for (UINT i = 0; i < 4; ++i) c->blend_factor[i] = 1.0f;
  d->ctx = c;
  return S_OK;
}

static HRESULT dev_create(IDXGIAdapter *adapter, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count, Dev **out) {
  *out = NULL;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (levels && level_count) {      /* the first the program asks for that is not above what there is */
    UINT i = 0;
    while (i < level_count && levels[i] > D3D_FEATURE_LEVEL_11_0) ++i;
    if (i == level_count) return DXGI_ERROR_UNSUPPORTED;
    level = levels[i];
  }
  Dev *d = mem_alloc(sizeof *d);
  if (!d) return E_OUTOFMEMORY;
  dx_init(&d->o, &dev_vtbl, g_dev_iids, dev_destroy);
  d->dxgi.lpVtbl = (const IDXGIDevice1Vtbl *)&g_dxgi_vtbl;      /* (IDXGIDevice3's table begins with IDXGIDevice1's) */
  d->presenter.lpVtbl = &g_presenter_vtbl;
  InitializeCriticalSection(&d->lock);
  d->level = level;
  d->flags = flags;
  HRESULT hr = S_OK;
  if (adapter) {
    IDXGIAdapter_AddRef(adapter);
    d->adapter = adapter;
  } else {
    /* (the factory is kept: dxgi.dll's adapters are the factory's, and close with it) */
    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&d->factory);
    if (SUCCEEDED(hr) && d->factory) hr = IDXGIFactory1_EnumAdapters(d->factory, 0, &d->adapter);
    if (SUCCEEDED(hr) && !d->adapter) hr = DXGI_ERROR_UNSUPPORTED;
  }
  if (SUCCEEDED(hr)) hr = dev_start(d);
  if (FAILED(hr)) { dx_Release(OBJ(d)); return hr; }
  *out = d;
  return S_OK;
}

/* ---- the entry points ------------------------------------------------------------------------------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_private_lock);
  return TRUE;
}

EXPORT HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
                                        ID3D11Device **device, D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context) {
  (void)sdk;
  if (device) *device = NULL;
  if (feature_level) *feature_level = 0;
  if (context) *context = NULL;
  if (adapter && driver_type != D3D_DRIVER_TYPE_UNKNOWN) return E_INVALIDARG;
  if (driver_type == D3D_DRIVER_TYPE_SOFTWARE && !software) return E_INVALIDARG;
  Dev *d = NULL;
  const HRESULT hr = dev_create(adapter, flags, levels, level_count, &d);
  if (FAILED(hr)) return hr;
  if (feature_level) *feature_level = d->level;
  if (context) { ctx_AddRef(d->ctx); *context = (ID3D11DeviceContext *)d->ctx; }
  if (device) *device = (ID3D11Device *)d;
  else dx_Release(OBJ(d));      /* (the context a program took keeps the device) */
  return device || context ? S_OK : S_FALSE;
}

EXPORT HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels,
                                                    UINT level_count, UINT sdk, const DXGI_SWAP_CHAIN_DESC *description, IDXGISwapChain **swap_chain, ID3D11Device **device,
                                                    D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context) {
  if (swap_chain) *swap_chain = NULL;
  if (!swap_chain || !description) return D3D11CreateDevice(adapter, driver_type, software, flags, levels, level_count, sdk, device, feature_level, context);
  ID3D11Device *made = NULL;
  ID3D11DeviceContext *made_context = NULL;
  HRESULT hr = D3D11CreateDevice(adapter, driver_type, software, flags, levels, level_count, sdk, &made, feature_level, &made_context);
  if (FAILED(hr)) return hr;
  IDXGIFactory *factory = NULL;
  DXGI_SWAP_CHAIN_DESC desc = *description;
  hr = IDXGIAdapter_GetParent(((Dev *)made)->adapter, &IID_IDXGIFactory, (void **)&factory);
  if (SUCCEEDED(hr) && factory) {
    hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)made, &desc, swap_chain);
    IDXGIFactory_Release(factory);
  }
  if (FAILED(hr)) {
    ID3D11DeviceContext_Release(made_context);
    ID3D11Device_Release(made);
    if (feature_level) *feature_level = 0;
    return hr;
  }
  if (context) *context = made_context; else ID3D11DeviceContext_Release(made_context);
  if (device) *device = made; else ID3D11Device_Release(made);
  return S_OK;
}

/* (a Direct3D 12 device that is not this runtime's own: not there) */
EXPORT HRESULT WINAPI D3D11On12CreateDevice(IUnknown *device12, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count, IUnknown *const *queues, UINT queue_count, UINT node_mask,
                                            ID3D11Device **device, ID3D11DeviceContext **context, D3D_FEATURE_LEVEL *feature_level) {
  (void)device12; (void)flags; (void)levels; (void)level_count; (void)queues; (void)queue_count; (void)node_mask;
  if (device) *device = NULL;
  if (context) *context = NULL;
  if (feature_level) *feature_level = 0;
  return DXGI_ERROR_UNSUPPORTED;
}
