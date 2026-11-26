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

/* Part of d3d11.c. A texture as DXGI sees it: every Direct3D 11 texture is also an IDXGIResource1 and an IDXGISurface2 (programs ask for them to share it, to learn its usage,
 * or to wrap it - Chrome's video frames and shared images do). They are parts of the texture object (Tex.dxgi_res, Tex.dxgi_surf) and share its count.
 * A texture made D3D11_RESOURCE_MISC_SHARED_NTHANDLE is shared through NT handles (CreateSharedHandle; interop.inc opens them), with its keyed mutex (Tex.dxgi_mutex,
 * IDXGIKeyedMutex) when it was made SHARED_KEYEDMUTEX. */

static Tex *tex_of_dxgi_res(IDXGIResource1 *p) { return (Tex *)((BYTE *)p - offsetof(Tex, dxgi_res)); }
static Tex *tex_of_dxgi_surf(IDXGISurface2 *p) { return (Tex *)((BYTE *)p - offsetof(Tex, dxgi_surf)); }

static HRESULT STDMETHODCALLTYPE tex_QueryInterface(Tex *t, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (guid_eq(riid, &IID_IDXGIObject) || guid_eq(riid, &IID_IDXGIDeviceSubObject) || guid_eq(riid, &IID_IDXGIResource) || guid_eq(riid, &IID_IDXGIResource1)) {
    *out = &t->dxgi_res;
  } else if (guid_eq(riid, &IID_IDXGIKeyedMutex)) {
    if (!t->keyed_mutex) return E_NOINTERFACE;      /* (made without SHARED_KEYEDMUTEX) */
    *out = &t->dxgi_mutex;
  } else if (guid_eq(riid, &IID_IDXGISurface) || guid_eq(riid, &IID_IDXGISurface1) || guid_eq(riid, &IID_IDXGISurface2)) {
    if (t->desc.ArraySize != 1 || t->desc.MipLevels != 1 || t->kind != 0) { dx_no_interface("ID3D11Texture2D (a surface of several subresources)", riid); return E_NOINTERFACE; }
    *out = &t->dxgi_surf;
  } else {
    return child_QueryInterface(&t->c, riid, out);      /* (its Direct3D 10 interface, then the Direct3D 11 ones) */
  }
  dx_AddRef(OBJ(t));
  return S_OK;
}

/* ---- a buffer as DXGI sees it: IDXGIResource1 (Buf.dxgi_res), with the buffer's count ---- */
static Buf *buf_of_dxgi_res(IDXGIResource1 *p) { return (Buf *)((BYTE *)p - offsetof(Buf, dxgi_res)); }
static const IDXGIResource1Vtbl g_buf_dxgi_vtbl;
static HRESULT STDMETHODCALLTYPE buf_QueryInterface(Buf *b, REFIID riid, void **out) {
  if (out && riid && (guid_eq(riid, &IID_IDXGIObject) || guid_eq(riid, &IID_IDXGIDeviceSubObject) || guid_eq(riid, &IID_IDXGIResource) || guid_eq(riid, &IID_IDXGIResource1))) {
    b->dxgi_res.lpVtbl = &g_buf_dxgi_vtbl;
    dx_AddRef(OBJ(b));
    *out = &b->dxgi_res;
    return S_OK;
  }
  return child_QueryInterface(&b->c, riid, out);
}
static HRESULT STDMETHODCALLTYPE bres_QueryInterface(IDXGIResource1 *p, REFIID riid, void **out) { return buf_QueryInterface(buf_of_dxgi_res(p), riid, out); }
static ULONG STDMETHODCALLTYPE bres_AddRef(IDXGIResource1 *p) { return child_AddRef(&buf_of_dxgi_res(p)->c); }
static ULONG STDMETHODCALLTYPE bres_Release(IDXGIResource1 *p) { return child_Release(&buf_of_dxgi_res(p)->c); }
static HRESULT STDMETHODCALLTYPE bres_SetPrivateData(IDXGIResource1 *p, REFGUID g, UINT size, const void *data) { return dx_SetPrivateData(OBJ(buf_of_dxgi_res(p)), g, size, data); }
static HRESULT STDMETHODCALLTYPE bres_SetPrivateDataInterface(IDXGIResource1 *p, REFGUID g, const IUnknown *o) { return dx_SetPrivateDataInterface(OBJ(buf_of_dxgi_res(p)), g, o); }
static HRESULT STDMETHODCALLTYPE bres_GetPrivateData(IDXGIResource1 *p, REFGUID g, UINT *size, void *data) { return dx_GetPrivateData(OBJ(buf_of_dxgi_res(p)), g, size, data); }
static HRESULT STDMETHODCALLTYPE bres_GetParent(IDXGIResource1 *p, REFIID riid, void **out) { return dev_QueryInterface(buf_of_dxgi_res(p)->c.dev, riid, out); }
static HRESULT STDMETHODCALLTYPE bres_GetDevice(IDXGIResource1 *p, REFIID riid, void **out) { return dev_QueryInterface(buf_of_dxgi_res(p)->c.dev, riid, out); }
/* (buffers are not shared by this runtime: a buffer has no shared handle - DXGI_ERROR_INVALID_CALL, as for a resource made without sharing - and no NT one) */
static HRESULT STDMETHODCALLTYPE bres_GetSharedHandle(IDXGIResource1 *p, HANDLE *handle) { (void)p; if (!handle) return E_INVALIDARG; *handle = NULL; return DXGI_ERROR_INVALID_CALL; }
static HRESULT STDMETHODCALLTYPE bres_GetUsage(IDXGIResource1 *p, DXGI_USAGE *usage) {
  const Buf *b = buf_of_dxgi_res(p);
  if (!usage) return E_INVALIDARG;
  *usage = 0;
  if (b->desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) *usage |= DXGI_USAGE_SHADER_INPUT;
  if (b->desc.BindFlags & D3D11_BIND_RENDER_TARGET) *usage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
  if (b->desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) *usage |= DXGI_USAGE_UNORDERED_ACCESS;
  if (b->desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) *usage |= DXGI_CPU_ACCESS_READ_WRITE;
  else if (b->desc.Usage == D3D11_USAGE_DYNAMIC) *usage |= DXGI_CPU_ACCESS_DYNAMIC;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE bres_SetEvictionPriority(IDXGIResource1 *p, UINT priority) { buf_SetEvictionPriority(buf_of_dxgi_res(p), priority); return S_OK; }
static HRESULT STDMETHODCALLTYPE bres_GetEvictionPriority(IDXGIResource1 *p, UINT *priority) {
  if (!priority) return E_INVALIDARG;
  *priority = buf_GetEvictionPriority(buf_of_dxgi_res(p));
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE bres_CreateSubresourceSurface(IDXGIResource1 *p, UINT index, IDXGISurface2 **out) { (void)p; (void)index; if (out) *out = NULL; return E_INVALIDARG; }      /* (no surface of a buffer) */
static HRESULT STDMETHODCALLTYPE bres_CreateSharedHandle(IDXGIResource1 *p, const SECURITY_ATTRIBUTES *attributes, DWORD access, LPCWSTR name, HANDLE *handle) {
  (void)p; (void)attributes; (void)access; (void)name;
  if (handle) *handle = NULL;
  return E_INVALIDARG;      /* (made without SHARED_NTHANDLE: this runtime makes no shared buffers) */
}
#define BRES_SLOT(name) .name = (__typeof__(((IDXGIResource1Vtbl *)0)->name))bres_##name
static const IDXGIResource1Vtbl g_buf_dxgi_vtbl = {
    BRES_SLOT(QueryInterface), BRES_SLOT(AddRef), BRES_SLOT(Release), BRES_SLOT(SetPrivateData), BRES_SLOT(SetPrivateDataInterface), BRES_SLOT(GetPrivateData),
    BRES_SLOT(GetParent), BRES_SLOT(GetDevice), BRES_SLOT(GetSharedHandle), BRES_SLOT(GetUsage), BRES_SLOT(SetEvictionPriority), BRES_SLOT(GetEvictionPriority),
    BRES_SLOT(CreateSubresourceSurface), BRES_SLOT(CreateSharedHandle),
};
#undef BRES_SLOT

static UINT dxgi_usage_of(const Tex *t) {
  UINT usage = 0;
  if (t->desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) usage |= DXGI_USAGE_SHADER_INPUT;
  if (t->desc.BindFlags & D3D11_BIND_RENDER_TARGET) usage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
  if (t->desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) usage |= DXGI_USAGE_UNORDERED_ACCESS;
  if (t->desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) usage |= DXGI_CPU_ACCESS_READ_WRITE;
  return usage;
}

/* ---- IDXGIResource1 ---- */
static HRESULT STDMETHODCALLTYPE dres_QueryInterface(IDXGIResource1 *p, REFIID riid, void **out) { return tex_QueryInterface(tex_of_dxgi_res(p), riid, out); }
static ULONG STDMETHODCALLTYPE dres_AddRef(IDXGIResource1 *p) { return dx_AddRef(OBJ(tex_of_dxgi_res(p))); }
static ULONG STDMETHODCALLTYPE dres_Release(IDXGIResource1 *p) { return dx_Release(OBJ(tex_of_dxgi_res(p))); }
static HRESULT STDMETHODCALLTYPE dres_SetPrivateData(IDXGIResource1 *p, REFGUID g, UINT size, const void *data) { return dx_SetPrivateData(OBJ(tex_of_dxgi_res(p)), g, size, data); }
static HRESULT STDMETHODCALLTYPE dres_SetPrivateDataInterface(IDXGIResource1 *p, REFGUID g, const IUnknown *o) { return dx_SetPrivateDataInterface(OBJ(tex_of_dxgi_res(p)), g, o); }
static HRESULT STDMETHODCALLTYPE dres_GetPrivateData(IDXGIResource1 *p, REFGUID g, UINT *size, void *data) { return dx_GetPrivateData(OBJ(tex_of_dxgi_res(p)), g, size, data); }
static HRESULT STDMETHODCALLTYPE dres_GetParent(IDXGIResource1 *p, REFIID riid, void **out) { return dev_QueryInterface(tex_of_dxgi_res(p)->c.dev, riid, out); }
static HRESULT STDMETHODCALLTYPE dres_GetDevice(IDXGIResource1 *p, REFIID riid, void **out) { return dev_QueryInterface(tex_of_dxgi_res(p)->c.dev, riid, out); }
/* The legacy shared handle of a texture made SHARED or SHARED_KEYEDMUTEX without NTHANDLE (or opened by one): its global share handle, which OpenSharedResource of any
 * device - in any process - opens. Any other texture has none. */
static HRESULT STDMETHODCALLTYPE dres_GetSharedHandle(IDXGIResource1 *p, HANDLE *handle) {
  Tex *t = tex_of_dxgi_res(p);
  if (!handle) return E_INVALIDARG;
  *handle = NULL;
  if (!t->global_share) return DXGI_ERROR_INVALID_CALL;
  *handle = (HANDLE)(UINT_PTR)t->global_share;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dres_GetUsage(IDXGIResource1 *p, DXGI_USAGE *usage) {
  if (!usage) return E_INVALIDARG;
  *usage = dxgi_usage_of(tex_of_dxgi_res(p));
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dres_SetEvictionPriority(IDXGIResource1 *p, UINT priority) { (void)p; (void)priority; return S_OK; }
static HRESULT STDMETHODCALLTYPE dres_GetEvictionPriority(IDXGIResource1 *p, UINT *priority) {
  (void)p;
  if (!priority) return E_INVALIDARG;
  *priority = DXGI_RESOURCE_PRIORITY_NORMAL;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dres_CreateSubresourceSurface(IDXGIResource1 *p, UINT index, IDXGISurface2 **out) {
  Tex *t = tex_of_dxgi_res(p);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (index != 0 || t->desc.ArraySize != 1 || t->desc.MipLevels != 1) return E_INVALIDARG;
  dx_AddRef(OBJ(t));
  *out = &t->dxgi_surf;
  return S_OK;
}
/* An NT handle of a texture made D3D11_RESOURCE_MISC_SHARED_NTHANDLE (its Direct3D 12 resource's, which d3d12.dll shares: interop.inc). `access` is what
 * DXGI_SHARED_RESOURCE_READ / _WRITE ask for; the handle carries both. */
static HRESULT STDMETHODCALLTYPE dres_CreateSharedHandle(IDXGIResource1 *p, const SECURITY_ATTRIBUTES *attributes, DWORD access, LPCWSTR name, HANDLE *handle) {
  Tex *t = tex_of_dxgi_res(p);
  if (!handle) return E_INVALIDARG;
  *handle = NULL;
  if (!(t->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) || !access || (access & ~SHARED_ACCESS_ALL)) return E_INVALIDARG;
  /* (the resource, and its keyed mutex: one NT handle for both, as D3DKMTShareObjects takes them) */
  const UINT list[2] = {TcosD3D12SharedResource(t->res), t->keyed_mutex};
  if (!list[0]) return E_INVALIDARG;
  KmtAttributes a;
  void *oa = kmt_attributes(&a, name, attributes && attributes->bInheritHandle, attributes ? attributes->lpSecurityDescriptor : NULL);
  if (!oa) return E_INVALIDARG;
  UINT64 nt = 0;
  const NTSTATUS st = kmt_share_list(list, t->keyed_mutex ? 2 : 1, oa, GENERIC_ALL, &nt);
  if ((UINT)st == 0xC0000035u) return DXGI_ERROR_NAME_ALREADY_EXISTS;      /* STATUS_OBJECT_NAME_COLLISION */
  if (!KMT_OK(st)) return (UINT)st == 0xC000000Du || (UINT)st == 0xC0000008u ? E_INVALIDARG : E_FAIL;
  *handle = (HANDLE)(UINT_PTR)nt;
  return S_OK;
}
#define DRES_SLOT(name) .name = (__typeof__(((IDXGIResource1Vtbl *)0)->name))dres_##name
static const IDXGIResource1Vtbl g_dxgi_res_vtbl = {
    DRES_SLOT(QueryInterface), DRES_SLOT(AddRef), DRES_SLOT(Release), DRES_SLOT(SetPrivateData), DRES_SLOT(SetPrivateDataInterface), DRES_SLOT(GetPrivateData),
    DRES_SLOT(GetParent), DRES_SLOT(GetDevice), DRES_SLOT(GetSharedHandle), DRES_SLOT(GetUsage), DRES_SLOT(SetEvictionPriority), DRES_SLOT(GetEvictionPriority),
    DRES_SLOT(CreateSubresourceSurface), DRES_SLOT(CreateSharedHandle),
};
#undef DRES_SLOT

/* ---- IDXGISurface2 (a texture of one level and one layer) ---- */
static HRESULT STDMETHODCALLTYPE dsurf_QueryInterface(IDXGISurface2 *p, REFIID riid, void **out) { return tex_QueryInterface(tex_of_dxgi_surf(p), riid, out); }
static ULONG STDMETHODCALLTYPE dsurf_AddRef(IDXGISurface2 *p) { return dx_AddRef(OBJ(tex_of_dxgi_surf(p))); }
static ULONG STDMETHODCALLTYPE dsurf_Release(IDXGISurface2 *p) { return dx_Release(OBJ(tex_of_dxgi_surf(p))); }
static HRESULT STDMETHODCALLTYPE dsurf_SetPrivateData(IDXGISurface2 *p, REFGUID g, UINT size, const void *data) { return dx_SetPrivateData(OBJ(tex_of_dxgi_surf(p)), g, size, data); }
static HRESULT STDMETHODCALLTYPE dsurf_SetPrivateDataInterface(IDXGISurface2 *p, REFGUID g, const IUnknown *o) { return dx_SetPrivateDataInterface(OBJ(tex_of_dxgi_surf(p)), g, o); }
static HRESULT STDMETHODCALLTYPE dsurf_GetPrivateData(IDXGISurface2 *p, REFGUID g, UINT *size, void *data) { return dx_GetPrivateData(OBJ(tex_of_dxgi_surf(p)), g, size, data); }
static HRESULT STDMETHODCALLTYPE dsurf_GetParent(IDXGISurface2 *p, REFIID riid, void **out) { return dev_QueryInterface(tex_of_dxgi_surf(p)->c.dev, riid, out); }
static HRESULT STDMETHODCALLTYPE dsurf_GetDevice(IDXGISurface2 *p, REFIID riid, void **out) { return dev_QueryInterface(tex_of_dxgi_surf(p)->c.dev, riid, out); }
static HRESULT STDMETHODCALLTYPE dsurf_GetDesc(IDXGISurface2 *p, DXGI_SURFACE_DESC *desc) {
  const Tex *t = tex_of_dxgi_surf(p);
  if (!desc) return E_INVALIDARG;
  desc->Width = t->desc.Width;
  desc->Height = t->desc.Height;
  desc->Format = t->desc.Format;
  desc->SampleDesc = t->desc.SampleDesc;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dsurf_Map(IDXGISurface2 *p, DXGI_MAPPED_RECT *rect, UINT flags) {
  Tex *t = tex_of_dxgi_surf(p);
  if (!rect) return E_INVALIDARG;
  if (!(t->desc.CPUAccessFlags & (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE))) return DXGI_ERROR_INVALID_CALL;      /* (as on Windows: a surface the CPU may not reach) */
  const D3D11_MAP how = (flags & DXGI_MAP_READ) && (flags & DXGI_MAP_WRITE) ? D3D11_MAP_READ_WRITE : (flags & DXGI_MAP_WRITE) ? D3D11_MAP_WRITE : D3D11_MAP_READ;
  D3D11_MAPPED_SUBRESOURCE m;
  const HRESULT hr = ctx_Map(t->c.dev->ctx, (ID3D11Resource *)t, 0, how, 0, &m);
  if (FAILED(hr)) return hr;
  rect->Pitch = (INT)m.RowPitch;
  rect->pBits = m.pData;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dsurf_Unmap(IDXGISurface2 *p) {
  Tex *t = tex_of_dxgi_surf(p);
  ctx_Unmap(t->c.dev->ctx, (ID3D11Resource *)t, 0);
  return S_OK;
}
/* GDI on a texture made GDI_COMPATIBLE (B8G8R8A8, one level, one sample): GetDC hands out a memory device context with a DIB section of the texels - the texture's own, or
 * none with `discard` -, which GDI draws into; ReleaseDC puts them back into the texture (all of them: a dirty rectangle only says what changed) and the DC goes. One DC at
 * a time; GetDC again, or ReleaseDC without one, is DXGI_ERROR_INVALID_CALL, as is a texture that is not GDI-compatible. */
static HRESULT STDMETHODCALLTYPE dsurf_GetDC(IDXGISurface2 *p, BOOL discard, HDC *dc) {
  Tex *t = tex_of_dxgi_surf(p);
  Dev *d = t->c.dev;
  if (!dc) return E_INVALIDARG;
  if (!(t->desc.MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE)) return DXGI_ERROR_INVALID_CALL;
  EnterCriticalSection(&d->lock);
  if (t->gdi_dc) { LeaveCriticalSection(&d->lock); return DXGI_ERROR_INVALID_CALL; }
  BITMAPINFO info;
  memset(&info, 0, sizeof info);
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = (LONG)t->desc.Width;
  info.bmiHeader.biHeight = -(LONG)t->desc.Height;      /* (top-down, as the texture's rows) */
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void *bits = NULL;
  HDC memory = CreateCompatibleDC(NULL);
  HBITMAP bitmap = memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
  HRESULT hr = bitmap && bits ? S_OK : E_OUTOFMEMORY;
  if (SUCCEEDED(hr) && !discard) hr = tex_read(d, t, 0, 0, bits);      /* (rows of Width * 4 bytes: a 32-bit DIB's own pitch) */
  if (FAILED(hr)) {
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    LeaveCriticalSection(&d->lock);
    return hr;
  }
  t->gdi_previous = SelectObject(memory, bitmap);
  t->gdi_dc = memory;
  t->gdi_bitmap = bitmap;
  t->gdi_bits = bits;
  *dc = memory;
  LeaveCriticalSection(&d->lock);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dsurf_ReleaseDC(IDXGISurface2 *p, RECT *dirty) {
  Tex *t = tex_of_dxgi_surf(p);
  Dev *d = t->c.dev;
  EnterCriticalSection(&d->lock);
  if (!t->gdi_dc) { LeaveCriticalSection(&d->lock); return DXGI_ERROR_INVALID_CALL; }
  (void)dirty;
  GdiFlush();
  const HRESULT hr = tex_write(d, t, 0, 0, 0, 0, t->desc.Width, t->desc.Height, t->gdi_bits, t->desc.Width * 4);
  SelectObject(t->gdi_dc, t->gdi_previous);
  DeleteObject(t->gdi_bitmap);
  DeleteDC(t->gdi_dc);
  t->gdi_dc = NULL;
  t->gdi_bitmap = t->gdi_previous = NULL;
  t->gdi_bits = NULL;
  LeaveCriticalSection(&d->lock);
  return hr;
}
static HRESULT STDMETHODCALLTYPE dsurf_GetResource(IDXGISurface2 *p, REFIID riid, void **out, UINT *subresource) {
  if (subresource) *subresource = 0;
  return tex_QueryInterface(tex_of_dxgi_surf(p), riid, out);
}
#define DSURF_SLOT(name) .name = (__typeof__(((IDXGISurface2Vtbl *)0)->name))dsurf_##name
static const IDXGISurface2Vtbl g_dxgi_surf_vtbl = {
    DSURF_SLOT(QueryInterface), DSURF_SLOT(AddRef), DSURF_SLOT(Release), DSURF_SLOT(SetPrivateData), DSURF_SLOT(SetPrivateDataInterface), DSURF_SLOT(GetPrivateData),
    DSURF_SLOT(GetParent), DSURF_SLOT(GetDevice), DSURF_SLOT(GetDesc), DSURF_SLOT(Map), DSURF_SLOT(Unmap), DSURF_SLOT(GetDC), DSURF_SLOT(ReleaseDC),
    DSURF_SLOT(GetResource),
};
#undef DSURF_SLOT

/* (every texture made gets the two) */
/* ---- IDXGIKeyedMutex (a texture made SHARED_KEYEDMUTEX) ---- */
/* AcquireSync waits up to `ms` for the mutex with `key` (WAIT_TIMEOUT when the time runs out, WAIT_ABANDONED when its holder went without releasing it); ReleaseSync
 * sends what the device recorded first - the texture's contents are there for the next holder - and frees it with `key`. Either without holding it as it should:
 * DXGI_ERROR_INVALID_CALL. */
static Tex *tex_of_dxgi_mutex(IDXGIKeyedMutex *p) { return (Tex *)((BYTE *)p - offsetof(Tex, dxgi_mutex)); }
static HRESULT STDMETHODCALLTYPE dkm_QueryInterface(IDXGIKeyedMutex *p, REFIID riid, void **out) { return tex_QueryInterface(tex_of_dxgi_mutex(p), riid, out); }
static ULONG STDMETHODCALLTYPE dkm_AddRef(IDXGIKeyedMutex *p) { return dx_AddRef(OBJ(tex_of_dxgi_mutex(p))); }
static ULONG STDMETHODCALLTYPE dkm_Release(IDXGIKeyedMutex *p) { return dx_Release(OBJ(tex_of_dxgi_mutex(p))); }
static HRESULT STDMETHODCALLTYPE dkm_SetPrivateData(IDXGIKeyedMutex *p, REFGUID g, UINT size, const void *data) { return dx_SetPrivateData(OBJ(tex_of_dxgi_mutex(p)), g, size, data); }
static HRESULT STDMETHODCALLTYPE dkm_SetPrivateDataInterface(IDXGIKeyedMutex *p, REFGUID g, const IUnknown *o) { return dx_SetPrivateDataInterface(OBJ(tex_of_dxgi_mutex(p)), g, o); }
static HRESULT STDMETHODCALLTYPE dkm_GetPrivateData(IDXGIKeyedMutex *p, REFGUID g, UINT *size, void *data) { return dx_GetPrivateData(OBJ(tex_of_dxgi_mutex(p)), g, size, data); }
static HRESULT STDMETHODCALLTYPE dkm_GetParent(IDXGIKeyedMutex *p, REFIID riid, void **out) { return dev_QueryInterface(tex_of_dxgi_mutex(p)->c.dev, riid, out); }
static HRESULT STDMETHODCALLTYPE dkm_GetDevice(IDXGIKeyedMutex *p, REFIID riid, void **out) { return dev_QueryInterface(tex_of_dxgi_mutex(p)->c.dev, riid, out); }
static HRESULT keyed_mutex_result(NTSTATUS st) {
  switch ((UINT)st) {
    case 0: return S_OK;
    case 0x102u: return WAIT_TIMEOUT;           /* STATUS_TIMEOUT */
    case 0x80u: return WAIT_ABANDONED;          /* STATUS_ABANDONED */
    case 0xC0000184u: return DXGI_ERROR_INVALID_CALL;      /* STATUS_INVALID_DEVICE_STATE: held by it already, or not held by it */
    default: return E_FAIL;
  }
}
static HRESULT STDMETHODCALLTYPE dkm_AcquireSync(IDXGIKeyedMutex *p, UINT64 key, DWORD ms) {
  return keyed_mutex_result(kmt_acquire_keyed_mutex(tex_of_dxgi_mutex(p)->keyed_mutex, key, ms));
}
static HRESULT STDMETHODCALLTYPE dkm_ReleaseSync(IDXGIKeyedMutex *p, UINT64 key) {
  Tex *t = tex_of_dxgi_mutex(p);
  Dev *d = t->c.dev;
  EnterCriticalSection(&d->lock);
  flush(d);      /* (what the device recorded runs before the next holder looks: the queue runs it as it is sent) */
  LeaveCriticalSection(&d->lock);
  return keyed_mutex_result(kmt_release_keyed_mutex(t->keyed_mutex, key));
}
#define DKM_SLOT(name) .name = (__typeof__(((IDXGIKeyedMutexVtbl *)0)->name))dkm_##name
static const IDXGIKeyedMutexVtbl g_dxgi_mutex_vtbl = {
    DKM_SLOT(QueryInterface), DKM_SLOT(AddRef), DKM_SLOT(Release), DKM_SLOT(SetPrivateData), DKM_SLOT(SetPrivateDataInterface), DKM_SLOT(GetPrivateData),
    DKM_SLOT(GetParent), DKM_SLOT(GetDevice), DKM_SLOT(AcquireSync), DKM_SLOT(ReleaseSync),
};
#undef DKM_SLOT

static void tex_dxgi_init(Tex *t) {
  t->dxgi_res.lpVtbl = &g_dxgi_res_vtbl;
  t->dxgi_surf.lpVtbl = &g_dxgi_surf_vtbl;
  t->dxgi_mutex.lpVtbl = &g_dxgi_mutex_vtbl;
}
