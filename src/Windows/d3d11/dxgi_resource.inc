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
 * Nothing is shared with another device or process here: a shared handle is refused. */

static Tex *tex_of_dxgi_res(IDXGIResource1 *p) { return (Tex *)((BYTE *)p - offsetof(Tex, dxgi_res)); }
static Tex *tex_of_dxgi_surf(IDXGISurface2 *p) { return (Tex *)((BYTE *)p - offsetof(Tex, dxgi_surf)); }

static HRESULT STDMETHODCALLTYPE tex_QueryInterface(Tex *t, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (guid_eq(riid, &IID_IDXGIObject) || guid_eq(riid, &IID_IDXGIDeviceSubObject) || guid_eq(riid, &IID_IDXGIResource) || guid_eq(riid, &IID_IDXGIResource1)) {
    *out = &t->dxgi_res;
  } else if (guid_eq(riid, &IID_IDXGISurface) || guid_eq(riid, &IID_IDXGISurface1) || guid_eq(riid, &IID_IDXGISurface2)) {
    if (t->desc.ArraySize != 1 || t->desc.MipLevels != 1 || t->kind != 0) { dx_no_interface("ID3D11Texture2D (a surface of several subresources)", riid); return E_NOINTERFACE; }
    *out = &t->dxgi_surf;
  } else {
    return dx_QueryInterface(OBJ(t), riid, out);
  }
  dx_AddRef(OBJ(t));
  return S_OK;
}

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
static HRESULT STDMETHODCALLTYPE dres_GetSharedHandle(IDXGIResource1 *p, HANDLE *handle) {
  (void)p;
  if (!handle) return E_INVALIDARG;
  *handle = NULL;
  return DXGI_ERROR_UNSUPPORTED;      /* (no texture is shared with another device here) */
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
  return ID3D12Device_CreateSharedHandle(t->c.dev->d12, (ID3D12DeviceChild *)t->res, attributes, GENERIC_ALL, name, handle);
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
static HRESULT STDMETHODCALLTYPE dsurf_GetDC(IDXGISurface2 *p, BOOL discard, HDC *dc) { (void)p; (void)discard; if (dc) *dc = NULL; return DXGI_ERROR_INVALID_CALL; }      /* (no GDI-compatible surfaces) */
static HRESULT STDMETHODCALLTYPE dsurf_ReleaseDC(IDXGISurface2 *p, RECT *dirty) { (void)p; (void)dirty; return DXGI_ERROR_INVALID_CALL; }
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
static void tex_dxgi_init(Tex *t) {
  t->dxgi_res.lpVtbl = &g_dxgi_res_vtbl;
  t->dxgi_surf.lpVtbl = &g_dxgi_surf_vtbl;
}
