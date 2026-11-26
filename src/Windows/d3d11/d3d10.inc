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

/* Part of d3d11.c: Direct3D 10. A Direct3D 10 device is a Direct3D 11 device that also answers ID3D10Device1 (Dev.d10), as on Windows 7 and later, and every object it
 * makes that Direct3D 10 has a counterpart of is also that interface - the same object, with the same count: ID3D10Buffer, ID3D10Texture1D/2D/3D, the views, the shaders,
 * the input layout, the state objects and the queries. The interface is a pointer in the object's head (Child.face10): QueryInterface goes from one to the other both ways.
 *
 * Direct3D 10 has no device context: its device's methods are the immediate context's, with Direct3D 10's structures turned into Direct3D 11's (d3d10core.dll,
 * d3d10.dll and d3d10_1.dll make the device with TcosD3D10CoreCreateDevice). An object Direct3D 11 makes that Direct3D 10 cannot describe - a structured buffer, a
 * resource with a clamp or tiles, an extended buffer view, a hull, domain or compute shader, an unordered access view - has no Direct3D 10 interface, as on Windows;
 * and the Direct3D 10 objects of a device Direct3D 11 made have no Direct3D 10 device (GetDevice gives NULL). */

#define DEV10(p) ((Dev *)((BYTE *)(p) - offsetof(Dev, d10)))
#define DEVMT10(p) ((Dev *)((BYTE *)(p) - offsetof(Dev, d10mt)))
#define D10(p) ((Child *)((BYTE *)(p) - offsetof(Child, face10)))      /* (an object's Direct3D 10 interface -> the object, in its own methods) */

extern const ID3D10Device1Vtbl d10dev_vtbl;
extern const ID3D10BufferVtbl d10buf_vtbl;
extern const ID3D10Texture1DVtbl d10tex1d_vtbl;
extern const ID3D10Texture2DVtbl d10tex_vtbl;
extern const ID3D10Texture3DVtbl d10tex3_vtbl;
extern const ID3D10RenderTargetViewVtbl d10rtv_vtbl;
extern const ID3D10DepthStencilViewVtbl d10dsv_vtbl;
extern const ID3D10ShaderResourceView1Vtbl d10srv_vtbl;
extern const ID3D10VertexShaderVtbl d10vs_vtbl;
extern const ID3D10GeometryShaderVtbl d10gs_vtbl;
extern const ID3D10PixelShaderVtbl d10ps_vtbl;
extern const ID3D10InputLayoutVtbl d10layout_vtbl;
extern const ID3D10BlendState1Vtbl d10blend_vtbl;
extern const ID3D10DepthStencilStateVtbl d10depth_vtbl;
extern const ID3D10RasterizerStateVtbl d10raster_vtbl;
extern const ID3D10SamplerStateVtbl d10sampler_vtbl;
extern const ID3D10QueryVtbl d10query_vtbl;      /* (a predicate's too: ID3D10Predicate adds no methods) */
extern const ID3D11Texture1DVtbl tex1d_vtbl;
extern const ID3D11Texture3D1Vtbl tex3_vtbl;
extern const ID3D11PredicateVtbl pred_vtbl;

/* The misc flags of a resource that Direct3D 10 has no word for: such a resource has no Direct3D 10 interface (a structured buffer, RESOURCE_CLAMP, tiles, a tile pool,
 * the protected kinds). DRAWINDIRECT_ARGS and ALLOW_RAW_VIEWS are only left out of the Direct3D 10 description. */
#define D11_ONLY_MISC (D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_RESOURCE_CLAMP | D3D11_RESOURCE_MISC_TILED | D3D11_RESOURCE_MISC_TILE_POOL | \
                       D3D11_RESOURCE_MISC_HW_PROTECTED | D3D11_RESOURCE_MISC_RESTRICTED_CONTENT | D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE | \
                       D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE_DRIVER | D3D11_RESOURCE_MISC_GUARDED)
#define D10_BIND_ALL (D3D10_BIND_VERTEX_BUFFER | D3D10_BIND_INDEX_BUFFER | D3D10_BIND_CONSTANT_BUFFER | D3D10_BIND_SHADER_RESOURCE | D3D10_BIND_STREAM_OUTPUT | \
                      D3D10_BIND_RENDER_TARGET | D3D10_BIND_DEPTH_STENCIL)
#define D10_MISC_ALL (D3D10_RESOURCE_MISC_GENERATE_MIPS | D3D10_RESOURCE_MISC_SHARED | D3D10_RESOURCE_MISC_TEXTURECUBE | D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX | \
                      D3D10_RESOURCE_MISC_GDI_COMPATIBLE)

static UINT misc11_of_10(UINT misc) {
  UINT out = misc & (D3D10_RESOURCE_MISC_GENERATE_MIPS | D3D10_RESOURCE_MISC_SHARED | D3D10_RESOURCE_MISC_TEXTURECUBE);
  if (misc & D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX) out |= D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (misc & D3D10_RESOURCE_MISC_GDI_COMPATIBLE) out |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
  return out;
}
static UINT misc10_of_11(UINT misc) {
  UINT out = misc & (D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_TEXTURECUBE);
  if (misc & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) out |= D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (misc & D3D11_RESOURCE_MISC_GDI_COMPATIBLE) out |= D3D10_RESOURCE_MISC_GDI_COMPATIBLE;
  return out;
}

/* ---- an object's Direct3D 10 interface ------------------------------------------------------------------------------------------------------------------------------ */

/* The table of the Direct3D 10 interface an object has (NULL: none). Worked out from what the object is now: a texture's Direct3D 11 table is set after it is made. */
static const void *d10_table(const Child *c) {
  const void *v = c->o.lpVtbl;
  if (v == (const void *)&buf_vtbl) return (((const Buf *)c)->desc.MiscFlags & D11_ONLY_MISC) ? NULL : (const void *)&d10buf_vtbl;
  if (v == (const void *)&tex_vtbl || v == (const void *)&tex1d_vtbl || v == (const void *)&tex3_vtbl) {
    const Tex *t = (const Tex *)c;
    const UINT misc = v == (const void *)&tex1d_vtbl ? t->desc1.MiscFlags : v == (const void *)&tex3_vtbl ? t->desc3.MiscFlags : t->desc.MiscFlags;
    if (misc & D11_ONLY_MISC) return NULL;
    return v == (const void *)&tex_vtbl ? (const void *)&d10tex_vtbl : v == (const void *)&tex1d_vtbl ? (const void *)&d10tex1d_vtbl : (const void *)&d10tex3_vtbl;
  }
  if (v == (const void *)&rtv_vtbl) return &d10rtv_vtbl;
  if (v == (const void *)&dsv_vtbl) return &d10dsv_vtbl;
  if (v == (const void *)&srv_vtbl) return ((const View *)c)->desc.srv.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX ? NULL : (const void *)&d10srv_vtbl;
  if (v == (const void *)&vs_vtbl) return &d10vs_vtbl;
  if (v == (const void *)&gs_vtbl) return &d10gs_vtbl;
  if (v == (const void *)&ps_vtbl) return &d10ps_vtbl;
  if (v == (const void *)&layout_vtbl) return &d10layout_vtbl;
  if (v == (const void *)&blend_vtbl) return &d10blend_vtbl;
  if (v == (const void *)&depth_vtbl) return &d10depth_vtbl;
  if (v == (const void *)&raster_vtbl) return &d10raster_vtbl;
  if (v == (const void *)&sampler_vtbl) return &d10sampler_vtbl;
  if (v == (const void *)&query_vtbl || v == (const void *)&pred_vtbl)
    return ((const Query *)c)->desc.Query <= D3D11_QUERY_SO_OVERFLOW_PREDICATE ? (const void *)&d10query_vtbl : NULL;      /* (the per-stream queries are Direct3D 11's) */
  return NULL;
}
/* Whether `riid` names the Direct3D 10 interface of an object of that table (or one it derives from). */
static BOOL d10_answers(const Child *c, const void *table, REFIID riid) {
  if (guid_eq(riid, &IID_ID3D10DeviceChild)) return TRUE;
  if (table == &d10buf_vtbl) return guid_eq(riid, &IID_ID3D10Resource) || guid_eq(riid, &IID_ID3D10Buffer);
  if (table == &d10tex1d_vtbl) return guid_eq(riid, &IID_ID3D10Resource) || guid_eq(riid, &IID_ID3D10Texture1D);
  if (table == &d10tex_vtbl) return guid_eq(riid, &IID_ID3D10Resource) || guid_eq(riid, &IID_ID3D10Texture2D);
  if (table == &d10tex3_vtbl) return guid_eq(riid, &IID_ID3D10Resource) || guid_eq(riid, &IID_ID3D10Texture3D);
  if (table == &d10rtv_vtbl) return guid_eq(riid, &IID_ID3D10View) || guid_eq(riid, &IID_ID3D10RenderTargetView);
  if (table == &d10dsv_vtbl) return guid_eq(riid, &IID_ID3D10View) || guid_eq(riid, &IID_ID3D10DepthStencilView);
  if (table == &d10srv_vtbl) return guid_eq(riid, &IID_ID3D10View) || guid_eq(riid, &IID_ID3D10ShaderResourceView) || guid_eq(riid, &IID_ID3D10ShaderResourceView1);
  if (table == &d10vs_vtbl) return guid_eq(riid, &IID_ID3D10VertexShader);
  if (table == &d10gs_vtbl) return guid_eq(riid, &IID_ID3D10GeometryShader);
  if (table == &d10ps_vtbl) return guid_eq(riid, &IID_ID3D10PixelShader);
  if (table == &d10layout_vtbl) return guid_eq(riid, &IID_ID3D10InputLayout);
  if (table == &d10blend_vtbl) return guid_eq(riid, &IID_ID3D10BlendState) || guid_eq(riid, &IID_ID3D10BlendState1);
  if (table == &d10depth_vtbl) return guid_eq(riid, &IID_ID3D10DepthStencilState);
  if (table == &d10raster_vtbl) return guid_eq(riid, &IID_ID3D10RasterizerState);
  if (table == &d10sampler_vtbl) return guid_eq(riid, &IID_ID3D10SamplerState);
  if (table == &d10query_vtbl)
    return guid_eq(riid, &IID_ID3D10Asynchronous) || guid_eq(riid, &IID_ID3D10Query) ||
           (guid_eq(riid, &IID_ID3D10Predicate) && query_is_predicate(((const Query *)c)->desc.Query));
  return FALSE;
}
/* The object's Direct3D 10 interface (NULL: none, or no object). */
static void *d10_face(void *object) {
  Child *c = object;
  if (!c) return NULL;
  const void *table = d10_table(c);
  if (!table) return NULL;
  c->face10 = table;      /* (always the same table for the object: written again by whoever asks) */
  return &c->face10;
}
/* A Direct3D 10 interface a program hands in -> the object (NULL: none, or not one of this library's Direct3D 10 interfaces). */
static Child *d10_child(const void *p) {
  if (!p) return NULL;
  const void *t = *(const void *const *)p;
  if (t != &d10buf_vtbl && t != &d10tex1d_vtbl && t != &d10tex_vtbl && t != &d10tex3_vtbl && t != &d10rtv_vtbl && t != &d10dsv_vtbl && t != &d10srv_vtbl &&
      t != &d10vs_vtbl && t != &d10gs_vtbl && t != &d10ps_vtbl && t != &d10layout_vtbl && t != &d10blend_vtbl && t != &d10depth_vtbl && t != &d10raster_vtbl &&
      t != &d10sampler_vtbl && t != &d10query_vtbl)
    return NULL;
  return D10(p);
}
/* ... of the kind a method takes (NULL for another kind: Direct3D 10 calls with an object of the wrong kind bind nothing). */
static Child *d10_child_of(const void *p, const void *table) {
  Child *c = d10_child(p);
  return c && c->face10 == table ? c : NULL;
}
static Child *d10_resource(const void *p) {
  Child *c = d10_child(p);
  if (!c || (c->face10 != &d10buf_vtbl && c->face10 != &d10tex1d_vtbl && c->face10 != &d10tex_vtbl && c->face10 != &d10tex3_vtbl)) return NULL;
  return c;
}

/* Every Direct3D 11 object: its Direct3D 10 interface, then the others. */
static HRESULT STDMETHODCALLTYPE child_QueryInterface(Child *c, REFIID riid, void **out) {
  if (out && riid) {
    const void *table = d10_table(c);
    if (table && d10_answers(c, table, riid)) {
      *out = d10_face(c);
      dx_AddRef(OBJ(c));
      return S_OK;
    }
  }
  return dx_QueryInterface(OBJ(c), riid, out);
}

/* What every Direct3D 10 interface has (ID3D10DeviceChild): the object's QueryInterface (IUnknown is the object's Direct3D 11 interface, as on Windows), its count, its
 * device's Direct3D 10 interface and its private data. */
static HRESULT STDMETHODCALLTYPE d10c_QueryInterface(void *p, REFIID riid, void **out) {
  IUnknown *u = (IUnknown *)D10(p);
  return u->lpVtbl->QueryInterface(u, riid, out);
}
static ULONG STDMETHODCALLTYPE d10c_AddRef(void *p) { return child_AddRef(D10(p)); }
static ULONG STDMETHODCALLTYPE d10c_Release(void *p) { return child_Release(D10(p)); }
static void STDMETHODCALLTYPE d10c_GetDevice(void *p, ID3D10Device **out) {
  if (!out) return;
  Dev *d = D10(p)->dev;
  *out = NULL;
  if (!d->d10.lpVtbl) return;      /* (a Direct3D 11 device has no Direct3D 10 interface) */
  dx_AddRef(OBJ(d));
  *out = (ID3D10Device *)&d->d10;
}
static HRESULT STDMETHODCALLTYPE d10c_GetPrivateData(void *p, REFGUID guid, UINT *size, void *data) {
  if (!guid || !size) return E_INVALIDARG;
  return dx_GetPrivateData(OBJ(D10(p)), guid, size, data);
}
static HRESULT STDMETHODCALLTYPE d10c_SetPrivateData(void *p, REFGUID guid, UINT size, const void *data) {
  if (!guid) return E_INVALIDARG;
  return dx_SetPrivateData(OBJ(D10(p)), guid, size, data);
}
static HRESULT STDMETHODCALLTYPE d10c_SetPrivateDataInterface(void *p, REFGUID guid, const IUnknown *object) {
  if (!guid) return E_INVALIDARG;
  return dx_SetPrivateDataInterface(OBJ(D10(p)), guid, object);
}

/* ---- resources ---------------------------------------------------------------------------------------------------------------------------------------------------- */

static void STDMETHODCALLTYPE d10buf_GetType(ID3D10Buffer *p, D3D10_RESOURCE_DIMENSION *type) { (void)p; if (type) *type = D3D10_RESOURCE_DIMENSION_BUFFER; }
static void STDMETHODCALLTYPE d10tex1d_GetType(ID3D10Texture1D *p, D3D10_RESOURCE_DIMENSION *type) { (void)p; if (type) *type = D3D10_RESOURCE_DIMENSION_TEXTURE1D; }
static void STDMETHODCALLTYPE d10tex_GetType(ID3D10Texture2D *p, D3D10_RESOURCE_DIMENSION *type) { (void)p; if (type) *type = D3D10_RESOURCE_DIMENSION_TEXTURE2D; }
static void STDMETHODCALLTYPE d10tex3_GetType(ID3D10Texture3D *p, D3D10_RESOURCE_DIMENSION *type) { (void)p; if (type) *type = D3D10_RESOURCE_DIMENSION_TEXTURE3D; }
static void STDMETHODCALLTYPE d10res_SetEvictionPriority(void *p, UINT priority) {
  Child *c = D10(p);
  if (c->face10 == &d10buf_vtbl) buf_SetEvictionPriority((Buf *)c, priority);
  else tex_SetEvictionPriority((Tex *)c, priority);
}
static UINT STDMETHODCALLTYPE d10res_GetEvictionPriority(void *p) {
  Child *c = D10(p);
  return c->face10 == &d10buf_vtbl ? buf_GetEvictionPriority((Buf *)c) : tex_GetEvictionPriority((Tex *)c);
}
/* Map and Unmap of a Direct3D 10 resource: the immediate context's (D3D10_MAP and its flags are D3D11_MAP's). */
static HRESULT d10_map(Child *c, UINT sub, D3D10_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *m) {
  memset(m, 0, sizeof *m);
  if (flags & ~(UINT)D3D10_MAP_FLAG_DO_NOT_WAIT) return E_INVALIDARG;
  /* (Direct3D 10 maps what has CPU access only: a default or immutable resource is E_INVALIDARG, Direct3D 11.3's mapping of default textures is not Direct3D 10's) */
  const UINT cpu = c->face10 == &d10buf_vtbl ? ((Buf *)c)->desc.CPUAccessFlags : c->face10 == &d10tex1d_vtbl ? ((Tex *)c)->desc1.CPUAccessFlags :
                   c->face10 == &d10tex3_vtbl ? ((Tex *)c)->desc3.CPUAccessFlags : ((Tex *)c)->desc.CPUAccessFlags;
  const UINT need = type == D3D10_MAP_READ ? D3D10_CPU_ACCESS_READ : type == D3D10_MAP_READ_WRITE ? D3D10_CPU_ACCESS_READ | D3D10_CPU_ACCESS_WRITE : D3D10_CPU_ACCESS_WRITE;
  if ((cpu & need) != need) return E_INVALIDARG;
  return ctx_Map(c->dev->ctx, (ID3D11Resource *)c, sub, (D3D11_MAP)type, flags, m);
}
static HRESULT STDMETHODCALLTYPE d10buf_Map(ID3D10Buffer *p, D3D10_MAP type, UINT flags, void **data) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (!data) return E_INVALIDARG;
  const HRESULT hr = d10_map(D10(p), 0, type, flags, &m);
  *data = SUCCEEDED(hr) ? m.pData : NULL;
  return hr;
}
static void STDMETHODCALLTYPE d10buf_Unmap(ID3D10Buffer *p) { Child *c = D10(p); ctx_Unmap(c->dev->ctx, (ID3D11Resource *)c, 0); }
static void STDMETHODCALLTYPE d10buf_GetDesc(ID3D10Buffer *p, D3D10_BUFFER_DESC *desc) {
  if (!desc) return;
  const D3D11_BUFFER_DESC *b = &((Buf *)D10(p))->desc;
  desc->ByteWidth = b->ByteWidth;
  desc->Usage = (D3D10_USAGE)b->Usage;
  desc->BindFlags = b->BindFlags;      /* (as Direct3D 11 has them, D3D11_BIND_UNORDERED_ACCESS too: Windows does not take it out) */
  desc->CPUAccessFlags = b->CPUAccessFlags;
  desc->MiscFlags = misc10_of_11(b->MiscFlags);
}
static HRESULT STDMETHODCALLTYPE d10tex1d_Map(ID3D10Texture1D *p, UINT sub, D3D10_MAP type, UINT flags, void **data) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (!data) return E_INVALIDARG;
  const HRESULT hr = d10_map(D10(p), sub, type, flags, &m);
  *data = SUCCEEDED(hr) ? m.pData : NULL;
  return hr;
}
static void STDMETHODCALLTYPE d10tex1d_Unmap(ID3D10Texture1D *p, UINT sub) { Child *c = D10(p); ctx_Unmap(c->dev->ctx, (ID3D11Resource *)c, sub); }
static void STDMETHODCALLTYPE d10tex1d_GetDesc(ID3D10Texture1D *p, D3D10_TEXTURE1D_DESC *desc) {
  if (!desc) return;
  const D3D11_TEXTURE1D_DESC *t = &((Tex *)D10(p))->desc1;
  desc->Width = t->Width;
  desc->MipLevels = t->MipLevels;
  desc->ArraySize = t->ArraySize;
  desc->Format = t->Format;
  desc->Usage = (D3D10_USAGE)t->Usage;
  desc->BindFlags = t->BindFlags;
  desc->CPUAccessFlags = t->CPUAccessFlags;
  desc->MiscFlags = misc10_of_11(t->MiscFlags);
}
static HRESULT STDMETHODCALLTYPE d10tex_Map(ID3D10Texture2D *p, UINT sub, D3D10_MAP type, UINT flags, D3D10_MAPPED_TEXTURE2D *out) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (!out) return E_INVALIDARG;
  const HRESULT hr = d10_map(D10(p), sub, type, flags, &m);
  out->pData = SUCCEEDED(hr) ? m.pData : NULL;
  out->RowPitch = SUCCEEDED(hr) ? m.RowPitch : 0;
  return hr;
}
static void STDMETHODCALLTYPE d10tex_Unmap(ID3D10Texture2D *p, UINT sub) { Child *c = D10(p); ctx_Unmap(c->dev->ctx, (ID3D11Resource *)c, sub); }
static void STDMETHODCALLTYPE d10tex_GetDesc(ID3D10Texture2D *p, D3D10_TEXTURE2D_DESC *desc) {
  if (!desc) return;
  const D3D11_TEXTURE2D_DESC *t = &((Tex *)D10(p))->desc;
  desc->Width = t->Width;
  desc->Height = t->Height;
  desc->MipLevels = t->MipLevels;
  desc->ArraySize = t->ArraySize;
  desc->Format = t->Format;
  desc->SampleDesc = t->SampleDesc;
  desc->Usage = (D3D10_USAGE)t->Usage;
  desc->BindFlags = t->BindFlags;
  desc->CPUAccessFlags = t->CPUAccessFlags;
  desc->MiscFlags = misc10_of_11(t->MiscFlags);
}
static HRESULT STDMETHODCALLTYPE d10tex3_Map(ID3D10Texture3D *p, UINT sub, D3D10_MAP type, UINT flags, D3D10_MAPPED_TEXTURE3D *out) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (!out) return E_INVALIDARG;
  const HRESULT hr = d10_map(D10(p), sub, type, flags, &m);
  out->pData = SUCCEEDED(hr) ? m.pData : NULL;
  out->RowPitch = SUCCEEDED(hr) ? m.RowPitch : 0;
  out->DepthPitch = SUCCEEDED(hr) ? m.DepthPitch : 0;
  return hr;
}
static void STDMETHODCALLTYPE d10tex3_Unmap(ID3D10Texture3D *p, UINT sub) { Child *c = D10(p); ctx_Unmap(c->dev->ctx, (ID3D11Resource *)c, sub); }
static void STDMETHODCALLTYPE d10tex3_GetDesc(ID3D10Texture3D *p, D3D10_TEXTURE3D_DESC *desc) {
  if (!desc) return;
  const D3D11_TEXTURE3D_DESC *t = &((Tex *)D10(p))->desc3;
  desc->Width = t->Width;
  desc->Height = t->Height;
  desc->Depth = t->Depth;
  desc->MipLevels = t->MipLevels;
  desc->Format = t->Format;
  desc->Usage = (D3D10_USAGE)t->Usage;
  desc->BindFlags = t->BindFlags;
  desc->CPUAccessFlags = t->CPUAccessFlags;
  desc->MiscFlags = misc10_of_11(t->MiscFlags);
}

/* ---- views ------------------------------------------------------------------------------------------------------------------------------------------------------------ */

static void STDMETHODCALLTYPE d10view_GetResource(void *p, ID3D10Resource **out) {
  if (!out) return;
  View *v = (View *)D10(p);
  Child *r = v->tex ? &v->tex->c : &v->buf->c;
  *out = d10_face(r);
  if (*out) child_ref(r);
}
static void STDMETHODCALLTYPE d10rtv_GetDesc(ID3D10RenderTargetView *p, D3D10_RENDER_TARGET_VIEW_DESC *desc) {
  /* (the layouts are the same: Direct3D 11 added no kind of render target view) */
  if (desc) memcpy(desc, &((View *)D10(p))->desc.rtv, sizeof *desc);
}
static void STDMETHODCALLTYPE d10dsv_GetDesc(ID3D10DepthStencilView *p, D3D10_DEPTH_STENCIL_VIEW_DESC *desc) {
  if (!desc) return;
  const D3D11_DEPTH_STENCIL_VIEW_DESC *v = &((View *)D10(p))->desc.dsv;
  desc->Format = v->Format;
  desc->ViewDimension = (D3D10_DSV_DIMENSION)v->ViewDimension;
  memcpy(&desc->Texture1D, &v->Texture1D, sizeof *desc - offsetof(D3D10_DEPTH_STENCIL_VIEW_DESC, Texture1D));      /* (Direct3D 11's has Flags before the union) */
}
static void STDMETHODCALLTYPE d10srv_GetDesc(ID3D10ShaderResourceView1 *p, D3D10_SHADER_RESOURCE_VIEW_DESC *desc) {
  if (desc) memcpy(desc, &((View *)D10(p))->desc.srv, sizeof *desc);
}
static void STDMETHODCALLTYPE d10srv_GetDesc1(ID3D10ShaderResourceView1 *p, D3D10_SHADER_RESOURCE_VIEW_DESC1 *desc) {
  if (desc) memcpy(desc, &((View *)D10(p))->desc.srv, sizeof *desc);      /* (D3D10_1_SRV_DIMENSION has Direct3D 11's numbers up to cube arrays) */
}

/* ---- state objects ------------------------------------------------------------------------------------------------------------------------------------------------- */

/* Direct3D 10's blend description has one blend of every target: that of the first target that blends (the others are in their usual form). */
static void STDMETHODCALLTYPE d10blend_GetDesc(ID3D10BlendState1 *p, D3D10_BLEND_DESC *desc) {
  if (!desc) return;
  const D3D11_BLEND_DESC *b = &((Blend *)D10(p))->desc;
  UINT first = 0;
  while (first < 8 && !b->RenderTarget[first].BlendEnable) ++first;
  const D3D11_RENDER_TARGET_BLEND_DESC *t = &b->RenderTarget[first < 8 ? first : 0];
  desc->AlphaToCoverageEnable = b->AlphaToCoverageEnable;
  desc->SrcBlend = (D3D10_BLEND)t->SrcBlend;
  desc->DestBlend = (D3D10_BLEND)t->DestBlend;
  desc->BlendOp = (D3D10_BLEND_OP)t->BlendOp;
  desc->SrcBlendAlpha = (D3D10_BLEND)t->SrcBlendAlpha;
  desc->DestBlendAlpha = (D3D10_BLEND)t->DestBlendAlpha;
  desc->BlendOpAlpha = (D3D10_BLEND_OP)t->BlendOpAlpha;
  for (UINT i = 0; i < 8; ++i) {
    desc->BlendEnable[i] = b->RenderTarget[i].BlendEnable;
    desc->RenderTargetWriteMask[i] = b->RenderTarget[i].RenderTargetWriteMask;
  }
}
static void STDMETHODCALLTYPE d10blend_GetDesc1(ID3D10BlendState1 *p, D3D10_BLEND_DESC1 *desc) {
  if (desc) memcpy(desc, &((Blend *)D10(p))->desc, sizeof *desc);      /* (D3D10_BLEND_DESC1 is D3D11_BLEND_DESC) */
}
static void STDMETHODCALLTYPE d10depth_GetDesc(ID3D10DepthStencilState *p, D3D10_DEPTH_STENCIL_DESC *desc) {
  if (desc) memcpy(desc, &((Depth *)D10(p))->desc, sizeof *desc);
}
static void STDMETHODCALLTYPE d10raster_GetDesc(ID3D10RasterizerState *p, D3D10_RASTERIZER_DESC *desc) {
  if (desc) memcpy(desc, &((Raster *)D10(p))->desc, sizeof *desc);
}
static void STDMETHODCALLTYPE d10sampler_GetDesc(ID3D10SamplerState *p, D3D10_SAMPLER_DESC *desc) {
  if (desc) memcpy(desc, &((Sampler *)D10(p))->desc, sizeof *desc);
}

/* ---- queries ---------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* Direct3D 10 begins and ends a query on the query itself (there is no context); its pipeline statistics are the first eight counts of Direct3D 11's. */

static void STDMETHODCALLTYPE d10query_Begin(ID3D10Query *p) { Child *c = D10(p); ctx_Begin(c->dev->ctx, (ID3D11Asynchronous *)c); }
static void STDMETHODCALLTYPE d10query_End(ID3D10Query *p) { Child *c = D10(p); ctx_End(c->dev->ctx, (ID3D11Asynchronous *)c); }
static UINT d10_query_size(const Query *q) { return q->desc.Query == D3D11_QUERY_PIPELINE_STATISTICS ? sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS) : query_data_size(q->desc.Query); }
static HRESULT STDMETHODCALLTYPE d10query_GetData(ID3D10Query *p, void *data, UINT size, UINT flags) {
  Query *q = (Query *)D10(p);
  Ctx *c = q->c.dev->ctx;
  if (q->desc.Query != D3D11_QUERY_PIPELINE_STATISTICS) return ctx_GetData(c, (ID3D11Asynchronous *)q, data, size, flags);
  if (data && size != 0 && size != sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS)) return E_INVALIDARG;
  D3D11_QUERY_DATA_PIPELINE_STATISTICS stats;
  const BOOL want = data && size != 0;
  const HRESULT hr = ctx_GetData(c, (ID3D11Asynchronous *)q, want ? &stats : NULL, want ? sizeof stats : 0, flags);
  if (hr == S_OK && want) memcpy(data, &stats, sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS));      /* (IA vertices .. PS invocations: the same order) */
  return hr;
}
static UINT STDMETHODCALLTYPE d10query_GetDataSize(ID3D10Query *p) { return d10_query_size((Query *)D10(p)); }
static void STDMETHODCALLTYPE d10query_GetDesc(ID3D10Query *p, D3D10_QUERY_DESC *desc) {
  if (!desc) return;
  const Query *q = (Query *)D10(p);
  desc->Query = (D3D10_QUERY)q->desc.Query;
  desc->MiscFlags = q->desc.MiscFlags;
}

/* ---- the device ----------------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE d10dev_QueryInterface(ID3D10Device1 *p, REFIID riid, void **out) { return dev_QueryInterface(DEV10(p), riid, out); }
static ULONG STDMETHODCALLTYPE d10dev_AddRef(ID3D10Device1 *p) { return dx_AddRef(OBJ(DEV10(p))); }
static ULONG STDMETHODCALLTYPE d10dev_Release(ID3D10Device1 *p) { return dx_Release(OBJ(DEV10(p))); }

/* Direct3D 10's interfaces handed in -> the objects, for a Direct3D 11 call (`count` of them, at most `capacity`: a call beyond the slots binds nothing, as Direct3D 11's). */
static BOOL d10_in(void *const *in, UINT count, UINT capacity, const void *table, void **out) {
  if (count > capacity) return FALSE;
  for (UINT i = 0; i < count; ++i) out[i] = in ? (void *)d10_child_of(in[i], table) : NULL;
  return TRUE;
}
/* Objects a Direct3D 11 getter gave (references) -> their Direct3D 10 interfaces, in place: one with none is given back and is NULL. */
static void d10_out(void **objects, UINT count) {
  for (UINT i = 0; objects && i < count; ++i) {
    if (!objects[i]) continue;
    void *face = d10_face(objects[i]);
    if (!face) dx_Release(OBJ(objects[i]));
    objects[i] = face;
  }
}
typedef void (STDMETHODCALLTYPE *RangeGetter)(Ctx *, UINT, UINT, void **);
/* A getter of a range of slots, for any count: in pieces, so the objects fit on the stack. */
static void d10_get_range(Ctx *c, RangeGetter get, UINT start, UINT count, void **out) {
  for (UINT done = 0; out && done < count;) {
    void *piece[16];
    const UINT n = count - done < 16 ? count - done : 16;
    if (start > ~0u - done) { for (UINT i = 0; i < n; ++i) out[done + i] = NULL; }
    else { get(c, start + done, n, piece); d10_out(piece, n); for (UINT i = 0; i < n; ++i) out[done + i] = piece[i]; }
    done += n;
  }
}

#define D10_CB_SLOTS D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
#define D10_SRV_SLOTS D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
#define D10_SAMPLER_SLOTS D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT
static void STDMETHODCALLTYPE d10dev_VSSetConstantBuffers(ID3D10Device1 *p, UINT start, UINT count, ID3D10Buffer *const *buffers) {
  void *o[D10_CB_SLOTS];
  if (d10_in((void *const *)buffers, count, D10_CB_SLOTS, &d10buf_vtbl, o)) ctx_VSSetConstantBuffers(DEV10(p)->ctx, start, count, (ID3D11Buffer *const *)o);
}
static void STDMETHODCALLTYPE d10dev_PSSetConstantBuffers(ID3D10Device1 *p, UINT start, UINT count, ID3D10Buffer *const *buffers) {
  void *o[D10_CB_SLOTS];
  if (d10_in((void *const *)buffers, count, D10_CB_SLOTS, &d10buf_vtbl, o)) ctx_PSSetConstantBuffers(DEV10(p)->ctx, start, count, (ID3D11Buffer *const *)o);
}
static void STDMETHODCALLTYPE d10dev_GSSetConstantBuffers(ID3D10Device1 *p, UINT start, UINT count, ID3D10Buffer *const *buffers) {
  void *o[D10_CB_SLOTS];
  if (d10_in((void *const *)buffers, count, D10_CB_SLOTS, &d10buf_vtbl, o)) ctx_GSSetConstantBuffers(DEV10(p)->ctx, start, count, (ID3D11Buffer *const *)o);
}
static void STDMETHODCALLTYPE d10dev_VSSetShaderResources(ID3D10Device1 *p, UINT start, UINT count, ID3D10ShaderResourceView *const *views) {
  void *o[D10_SRV_SLOTS];
  if (d10_in((void *const *)views, count, D10_SRV_SLOTS, &d10srv_vtbl, o)) ctx_VSSetShaderResources(DEV10(p)->ctx, start, count, (ID3D11ShaderResourceView *const *)o);
}
static void STDMETHODCALLTYPE d10dev_PSSetShaderResources(ID3D10Device1 *p, UINT start, UINT count, ID3D10ShaderResourceView *const *views) {
  void *o[D10_SRV_SLOTS];
  if (d10_in((void *const *)views, count, D10_SRV_SLOTS, &d10srv_vtbl, o)) ctx_PSSetShaderResources(DEV10(p)->ctx, start, count, (ID3D11ShaderResourceView *const *)o);
}
static void STDMETHODCALLTYPE d10dev_GSSetShaderResources(ID3D10Device1 *p, UINT start, UINT count, ID3D10ShaderResourceView *const *views) {
  void *o[D10_SRV_SLOTS];
  if (d10_in((void *const *)views, count, D10_SRV_SLOTS, &d10srv_vtbl, o)) ctx_GSSetShaderResources(DEV10(p)->ctx, start, count, (ID3D11ShaderResourceView *const *)o);
}
static void STDMETHODCALLTYPE d10dev_VSSetSamplers(ID3D10Device1 *p, UINT start, UINT count, ID3D10SamplerState *const *samplers) {
  void *o[D10_SAMPLER_SLOTS];
  if (d10_in((void *const *)samplers, count, D10_SAMPLER_SLOTS, &d10sampler_vtbl, o)) ctx_VSSetSamplers(DEV10(p)->ctx, start, count, (ID3D11SamplerState *const *)o);
}
static void STDMETHODCALLTYPE d10dev_PSSetSamplers(ID3D10Device1 *p, UINT start, UINT count, ID3D10SamplerState *const *samplers) {
  void *o[D10_SAMPLER_SLOTS];
  if (d10_in((void *const *)samplers, count, D10_SAMPLER_SLOTS, &d10sampler_vtbl, o)) ctx_PSSetSamplers(DEV10(p)->ctx, start, count, (ID3D11SamplerState *const *)o);
}
static void STDMETHODCALLTYPE d10dev_GSSetSamplers(ID3D10Device1 *p, UINT start, UINT count, ID3D10SamplerState *const *samplers) {
  void *o[D10_SAMPLER_SLOTS];
  if (d10_in((void *const *)samplers, count, D10_SAMPLER_SLOTS, &d10sampler_vtbl, o)) ctx_GSSetSamplers(DEV10(p)->ctx, start, count, (ID3D11SamplerState *const *)o);
}
static void STDMETHODCALLTYPE d10dev_VSSetShader(ID3D10Device1 *p, ID3D10VertexShader *shader) {
  ctx_VSSetShader(DEV10(p)->ctx, (ID3D11VertexShader *)d10_child_of(shader, &d10vs_vtbl), NULL, 0);
}
static void STDMETHODCALLTYPE d10dev_PSSetShader(ID3D10Device1 *p, ID3D10PixelShader *shader) {
  ctx_PSSetShader(DEV10(p)->ctx, (ID3D11PixelShader *)d10_child_of(shader, &d10ps_vtbl), NULL, 0);
}
static void STDMETHODCALLTYPE d10dev_GSSetShader(ID3D10Device1 *p, ID3D10GeometryShader *shader) {
  ctx_GSSetShader(DEV10(p)->ctx, (ID3D11GeometryShader *)d10_child_of(shader, &d10gs_vtbl), NULL, 0);
}
static void STDMETHODCALLTYPE d10dev_DrawIndexed(ID3D10Device1 *p, UINT count, UINT start, INT base) { ctx_DrawIndexed(DEV10(p)->ctx, count, start, base); }
static void STDMETHODCALLTYPE d10dev_Draw(ID3D10Device1 *p, UINT count, UINT start) { ctx_Draw(DEV10(p)->ctx, count, start); }
static void STDMETHODCALLTYPE d10dev_DrawIndexedInstanced(ID3D10Device1 *p, UINT count, UINT instances, UINT start, INT base, UINT start_instance) {
  ctx_DrawIndexedInstanced(DEV10(p)->ctx, count, instances, start, base, start_instance);
}
static void STDMETHODCALLTYPE d10dev_DrawInstanced(ID3D10Device1 *p, UINT count, UINT instances, UINT start, UINT start_instance) {
  ctx_DrawInstanced(DEV10(p)->ctx, count, instances, start, start_instance);
}
static void STDMETHODCALLTYPE d10dev_DrawAuto(ID3D10Device1 *p) { ctx_DrawAuto(DEV10(p)->ctx); }
static void STDMETHODCALLTYPE d10dev_IASetInputLayout(ID3D10Device1 *p, ID3D10InputLayout *layout) {
  ctx_IASetInputLayout(DEV10(p)->ctx, (ID3D11InputLayout *)d10_child_of(layout, &d10layout_vtbl));
}
static void STDMETHODCALLTYPE d10dev_IASetVertexBuffers(ID3D10Device1 *p, UINT start, UINT count, ID3D10Buffer *const *buffers, const UINT *strides, const UINT *offsets) {
  void *o[D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
  if (start > D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT || count > D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - start) return;
  if (d10_in((void *const *)buffers, count, D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, &d10buf_vtbl, o))
    ctx_IASetVertexBuffers(DEV10(p)->ctx, start, count, (ID3D11Buffer *const *)o, strides, offsets);
}
static void STDMETHODCALLTYPE d10dev_IASetIndexBuffer(ID3D10Device1 *p, ID3D10Buffer *buffer, DXGI_FORMAT format, UINT offset) {
  ctx_IASetIndexBuffer(DEV10(p)->ctx, (ID3D11Buffer *)d10_child_of(buffer, &d10buf_vtbl), format, offset);
}
static void STDMETHODCALLTYPE d10dev_IASetPrimitiveTopology(ID3D10Device1 *p, D3D10_PRIMITIVE_TOPOLOGY topology) {
  if ((UINT)topology > D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) return;      /* (Direct3D 11's patch lists are not Direct3D 10's) */
  ctx_IASetPrimitiveTopology(DEV10(p)->ctx, (D3D11_PRIMITIVE_TOPOLOGY)topology);
}
static void STDMETHODCALLTYPE d10dev_SetPredication(ID3D10Device1 *p, ID3D10Predicate *predicate, BOOL value) {
  ctx_SetPredication(DEV10(p)->ctx, (ID3D11Predicate *)d10_child_of(predicate, &d10query_vtbl), value);
}
static void STDMETHODCALLTYPE d10dev_OMSetRenderTargets(ID3D10Device1 *p, UINT count, ID3D10RenderTargetView *const *views, ID3D10DepthStencilView *depth) {
  void *o[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT];
  if (!d10_in((void *const *)views, count, D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, &d10rtv_vtbl, o)) return;
  ctx_OMSetRenderTargets(DEV10(p)->ctx, count, (ID3D11RenderTargetView *const *)o, (ID3D11DepthStencilView *)d10_child_of(depth, &d10dsv_vtbl));
}
static void STDMETHODCALLTYPE d10dev_OMSetBlendState(ID3D10Device1 *p, ID3D10BlendState *state, const FLOAT factor[4], UINT mask) {
  ctx_OMSetBlendState(DEV10(p)->ctx, (ID3D11BlendState *)d10_child_of(state, &d10blend_vtbl), factor, mask);
}
static void STDMETHODCALLTYPE d10dev_OMSetDepthStencilState(ID3D10Device1 *p, ID3D10DepthStencilState *state, UINT ref) {
  ctx_OMSetDepthStencilState(DEV10(p)->ctx, (ID3D11DepthStencilState *)d10_child_of(state, &d10depth_vtbl), ref);
}
static void STDMETHODCALLTYPE d10dev_SOSetTargets(ID3D10Device1 *p, UINT count, ID3D10Buffer *const *buffers, const UINT *offsets) {
  void *o[D3D10_SO_BUFFER_SLOT_COUNT];
  if (d10_in((void *const *)buffers, count, D3D10_SO_BUFFER_SLOT_COUNT, &d10buf_vtbl, o)) ctx_SOSetTargets(DEV10(p)->ctx, count, (ID3D11Buffer *const *)o, offsets);
}
static void STDMETHODCALLTYPE d10dev_RSSetState(ID3D10Device1 *p, ID3D10RasterizerState *state) {
  ctx_RSSetState(DEV10(p)->ctx, (ID3D11RasterizerState *)d10_child_of(state, &d10raster_vtbl));
}
/* (Direct3D 10's viewports are whole pixels) */
static void STDMETHODCALLTYPE d10dev_RSSetViewports(ID3D10Device1 *p, UINT count, const D3D10_VIEWPORT *viewports) {
  D3D11_VIEWPORT v[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  if (count > D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE || (count && !viewports)) return;
  for (UINT i = 0; i < count; ++i) {
    v[i].TopLeftX = (FLOAT)viewports[i].TopLeftX;
    v[i].TopLeftY = (FLOAT)viewports[i].TopLeftY;
    v[i].Width = (FLOAT)viewports[i].Width;
    v[i].Height = (FLOAT)viewports[i].Height;
    v[i].MinDepth = viewports[i].MinDepth;
    v[i].MaxDepth = viewports[i].MaxDepth;
  }
  ctx_RSSetViewports(DEV10(p)->ctx, count, v);
}
static void STDMETHODCALLTYPE d10dev_RSSetScissorRects(ID3D10Device1 *p, UINT count, const D3D10_RECT *rects) { ctx_RSSetScissorRects(DEV10(p)->ctx, count, rects); }
static void STDMETHODCALLTYPE d10dev_CopySubresourceRegion(ID3D10Device1 *p, ID3D10Resource *to, UINT to_sub, UINT x, UINT y, UINT z, ID3D10Resource *from, UINT from_sub,
                                                          const D3D10_BOX *box) {
  Child *t = d10_resource(to), *f = d10_resource(from);
  if (!t || !f) return;
  ctx_CopySubresourceRegion(DEV10(p)->ctx, (ID3D11Resource *)t, to_sub, x, y, z, (ID3D11Resource *)f, from_sub, (const D3D11_BOX *)box);
}
static void STDMETHODCALLTYPE d10dev_CopyResource(ID3D10Device1 *p, ID3D10Resource *to, ID3D10Resource *from) {
  Child *t = d10_resource(to), *f = d10_resource(from);
  if (t && f) ctx_CopyResource(DEV10(p)->ctx, (ID3D11Resource *)t, (ID3D11Resource *)f);
}
static void STDMETHODCALLTYPE d10dev_UpdateSubresource(ID3D10Device1 *p, ID3D10Resource *resource, UINT sub, const D3D10_BOX *box, const void *data, UINT row_pitch,
                                                      UINT depth_pitch) {
  Child *r = d10_resource(resource);
  if (r) ctx_UpdateSubresource(DEV10(p)->ctx, (ID3D11Resource *)r, sub, (const D3D11_BOX *)box, data, row_pitch, depth_pitch);
}
static void STDMETHODCALLTYPE d10dev_ClearRenderTargetView(ID3D10Device1 *p, ID3D10RenderTargetView *view, const FLOAT colour[4]) {
  Child *v = d10_child_of(view, &d10rtv_vtbl);
  if (v) ctx_ClearRenderTargetView(DEV10(p)->ctx, (ID3D11RenderTargetView *)v, colour);
}
static void STDMETHODCALLTYPE d10dev_ClearDepthStencilView(ID3D10Device1 *p, ID3D10DepthStencilView *view, UINT flags, FLOAT depth, UINT8 stencil) {
  Child *v = d10_child_of(view, &d10dsv_vtbl);
  if (v) ctx_ClearDepthStencilView(DEV10(p)->ctx, (ID3D11DepthStencilView *)v, flags, depth, stencil);
}
static void STDMETHODCALLTYPE d10dev_GenerateMips(ID3D10Device1 *p, ID3D10ShaderResourceView *view) {
  Child *v = d10_child_of(view, &d10srv_vtbl);
  if (v) ctx_GenerateMips(DEV10(p)->ctx, (ID3D11ShaderResourceView *)v);
}
static void STDMETHODCALLTYPE d10dev_ResolveSubresource(ID3D10Device1 *p, ID3D10Resource *to, UINT to_sub, ID3D10Resource *from, UINT from_sub, DXGI_FORMAT format) {
  Child *t = d10_resource(to), *f = d10_resource(from);
  if (t && f) ctx_ResolveSubresource(DEV10(p)->ctx, (ID3D11Resource *)t, to_sub, (ID3D11Resource *)f, from_sub, format);
}

/* (the getters: Direct3D 11's, with Direct3D 10's interfaces given out) */
static void STDMETHODCALLTYPE d10dev_VSGetConstantBuffers(ID3D10Device1 *p, UINT start, UINT count, ID3D10Buffer **out) {
  d10_get_range(DEV10(p)->ctx, (RangeGetter)ctx_VSGetConstantBuffers, start, count, (void **)out);
}
static void STDMETHODCALLTYPE d10dev_PSGetConstantBuffers(ID3D10Device1 *p, UINT start, UINT count, ID3D10Buffer **out) {
  d10_get_range(DEV10(p)->ctx, (RangeGetter)ctx_PSGetConstantBuffers, start, count, (void **)out);
}
static void STDMETHODCALLTYPE d10dev_GSGetConstantBuffers(ID3D10Device1 *p, UINT start, UINT count, ID3D10Buffer **out) {
  d10_get_range(DEV10(p)->ctx, (RangeGetter)ctx_GSGetConstantBuffers, start, count, (void **)out);
}
static void STDMETHODCALLTYPE d10dev_VSGetShaderResources(ID3D10Device1 *p, UINT start, UINT count, ID3D10ShaderResourceView **out) {
  d10_get_range(DEV10(p)->ctx, (RangeGetter)ctx_VSGetShaderResources, start, count, (void **)out);
}
static void STDMETHODCALLTYPE d10dev_PSGetShaderResources(ID3D10Device1 *p, UINT start, UINT count, ID3D10ShaderResourceView **out) {
  d10_get_range(DEV10(p)->ctx, (RangeGetter)ctx_PSGetShaderResources, start, count, (void **)out);
}
static void STDMETHODCALLTYPE d10dev_GSGetShaderResources(ID3D10Device1 *p, UINT start, UINT count, ID3D10ShaderResourceView **out) {
  d10_get_range(DEV10(p)->ctx, (RangeGetter)ctx_GSGetShaderResources, start, count, (void **)out);
}
static void STDMETHODCALLTYPE d10dev_VSGetSamplers(ID3D10Device1 *p, UINT start, UINT count, ID3D10SamplerState **out) {
  d10_get_range(DEV10(p)->ctx, (RangeGetter)ctx_VSGetSamplers, start, count, (void **)out);
}
static void STDMETHODCALLTYPE d10dev_PSGetSamplers(ID3D10Device1 *p, UINT start, UINT count, ID3D10SamplerState **out) {
  d10_get_range(DEV10(p)->ctx, (RangeGetter)ctx_PSGetSamplers, start, count, (void **)out);
}
static void STDMETHODCALLTYPE d10dev_GSGetSamplers(ID3D10Device1 *p, UINT start, UINT count, ID3D10SamplerState **out) {
  d10_get_range(DEV10(p)->ctx, (RangeGetter)ctx_GSGetSamplers, start, count, (void **)out);
}
static void STDMETHODCALLTYPE d10dev_VSGetShader(ID3D10Device1 *p, ID3D10VertexShader **out) {
  if (!out) return;
  ctx_VSGetShader(DEV10(p)->ctx, (ID3D11VertexShader **)out, NULL, NULL);
  d10_out((void **)out, 1);
}
static void STDMETHODCALLTYPE d10dev_PSGetShader(ID3D10Device1 *p, ID3D10PixelShader **out) {
  if (!out) return;
  ctx_PSGetShader(DEV10(p)->ctx, (ID3D11PixelShader **)out, NULL, NULL);
  d10_out((void **)out, 1);
}
static void STDMETHODCALLTYPE d10dev_GSGetShader(ID3D10Device1 *p, ID3D10GeometryShader **out) {
  if (!out) return;
  ctx_GSGetShader(DEV10(p)->ctx, (ID3D11GeometryShader **)out, NULL, NULL);
  d10_out((void **)out, 1);
}
static void STDMETHODCALLTYPE d10dev_IAGetInputLayout(ID3D10Device1 *p, ID3D10InputLayout **out) {
  if (!out) return;
  ctx_IAGetInputLayout(DEV10(p)->ctx, (ID3D11InputLayout **)out);
  d10_out((void **)out, 1);
}
static void STDMETHODCALLTYPE d10dev_IAGetVertexBuffers(ID3D10Device1 *p, UINT start, UINT count, ID3D10Buffer **buffers, UINT *strides, UINT *offsets) {
  Ctx *c = DEV10(p)->ctx;
  for (UINT done = 0; done < count;) {
    ID3D11Buffer *piece[16];
    const UINT n = count - done < 16 ? count - done : 16;
    const UINT at = start > ~0u - done ? ~0u : start + done;      /* (past every slot: nothing there) */
    ctx_IAGetVertexBuffers(c, at, n, buffers ? piece : NULL, strides ? strides + done : NULL, offsets ? offsets + done : NULL);
    if (buffers) { d10_out((void **)piece, n); for (UINT i = 0; i < n; ++i) buffers[done + i] = (ID3D10Buffer *)piece[i]; }
    done += n;
  }
}
static void STDMETHODCALLTYPE d10dev_IAGetIndexBuffer(ID3D10Device1 *p, ID3D10Buffer **buffer, DXGI_FORMAT *format, UINT *offset) {
  ctx_IAGetIndexBuffer(DEV10(p)->ctx, (ID3D11Buffer **)buffer, format, offset);
  if (buffer) d10_out((void **)buffer, 1);
}
static void STDMETHODCALLTYPE d10dev_IAGetPrimitiveTopology(ID3D10Device1 *p, D3D10_PRIMITIVE_TOPOLOGY *topology) {
  ctx_IAGetPrimitiveTopology(DEV10(p)->ctx, (D3D11_PRIMITIVE_TOPOLOGY *)topology);
}
static void STDMETHODCALLTYPE d10dev_GetPredication(ID3D10Device1 *p, ID3D10Predicate **predicate, BOOL *value) {
  ctx_GetPredication(DEV10(p)->ctx, (ID3D11Predicate **)predicate, value);
  if (predicate) d10_out((void **)predicate, 1);
}
static void STDMETHODCALLTYPE ctx_d10_targets(Ctx *c, UINT start, UINT count, void **out) { get_range((void **)c->rtv, 8, start, count, out); }
static void STDMETHODCALLTYPE d10dev_OMGetRenderTargets(ID3D10Device1 *p, UINT count, ID3D10RenderTargetView **views, ID3D10DepthStencilView **depth) {
  Ctx *c = DEV10(p)->ctx;
  if (views) d10_get_range(c, (RangeGetter)ctx_d10_targets, 0, count, (void **)views);
  if (depth) { ctx_OMGetRenderTargets(c, 0, NULL, (ID3D11DepthStencilView **)depth); d10_out((void **)depth, 1); }
}
static void STDMETHODCALLTYPE d10dev_OMGetBlendState(ID3D10Device1 *p, ID3D10BlendState **state, FLOAT factor[4], UINT *mask) {
  ctx_OMGetBlendState(DEV10(p)->ctx, (ID3D11BlendState **)state, factor, mask);
  if (state) d10_out((void **)state, 1);
}
static void STDMETHODCALLTYPE d10dev_OMGetDepthStencilState(ID3D10Device1 *p, ID3D10DepthStencilState **state, UINT *ref) {
  ctx_OMGetDepthStencilState(DEV10(p)->ctx, (ID3D11DepthStencilState **)state, ref);
  if (state) d10_out((void **)state, 1);
}
/* (the offsets: Windows gives ~0 for a bound buffer - its records go on from where they end - and 0 for an empty slot) */
static void STDMETHODCALLTYPE d10dev_SOGetTargets(ID3D10Device1 *p, UINT count, ID3D10Buffer **buffers, UINT *offsets) {
  Ctx *c = DEV10(p)->ctx;
  for (UINT i = 0; i < count; ++i) {
    void *object = i < D3D10_SO_BUFFER_SLOT_COUNT ? (void *)c->so_buf[i] : NULL;
    if (offsets) offsets[i] = object ? ~0u : 0;
    if (buffers) {
      void *face = d10_face(object);
      if (face) child_ref(object);
      buffers[i] = face;
    }
  }
}
static void STDMETHODCALLTYPE d10dev_RSGetState(ID3D10Device1 *p, ID3D10RasterizerState **state) {
  if (!state) return;
  ctx_RSGetState(DEV10(p)->ctx, (ID3D11RasterizerState **)state);
  d10_out((void **)state, 1);
}
static void STDMETHODCALLTYPE d10dev_RSGetViewports(ID3D10Device1 *p, UINT *count, D3D10_VIEWPORT *viewports) {
  D3D11_VIEWPORT v[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  Ctx *c = DEV10(p)->ctx;
  if (!count) return;
  if (!viewports) { ctx_RSGetViewports(c, count, NULL); return; }
  UINT n = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  ctx_RSGetViewports(c, &n, v);
  for (UINT i = 0; i < *count; ++i) {
    if (i < c->viewport_count && i < n) {
      viewports[i].TopLeftX = (INT)v[i].TopLeftX;
      viewports[i].TopLeftY = (INT)v[i].TopLeftY;
      viewports[i].Width = (UINT)v[i].Width;
      viewports[i].Height = (UINT)v[i].Height;
      viewports[i].MinDepth = v[i].MinDepth;
      viewports[i].MaxDepth = v[i].MaxDepth;
    } else memset(&viewports[i], 0, sizeof viewports[i]);
  }
  if (*count > c->viewport_count) *count = c->viewport_count;      /* (how many there are of those asked for) */
}
static void STDMETHODCALLTYPE d10dev_RSGetScissorRects(ID3D10Device1 *p, UINT *count, D3D10_RECT *rects) {
  Ctx *c = DEV10(p)->ctx;
  ctx_RSGetScissorRects(c, count, rects);
  if (count && rects && *count > c->scissor_count) *count = c->scissor_count;
}

static HRESULT STDMETHODCALLTYPE d10dev_GetDeviceRemovedReason(ID3D10Device1 *p) { return dev_GetDeviceRemovedReason(DEV10(p)); }
/* (D3D10_RAISE_FLAG_DRIVER_INTERNAL_ERROR is the only flag; the runtime keeps it and answers it) */
static HRESULT STDMETHODCALLTYPE d10dev_SetExceptionMode(ID3D10Device1 *p, UINT flags) {
  if (flags & ~(UINT)D3D10_RAISE_FLAG_DRIVER_INTERNAL_ERROR) return E_INVALIDARG;
  DEV10(p)->d10_exceptions = flags;
  return S_OK;
}
static UINT STDMETHODCALLTYPE d10dev_GetExceptionMode(ID3D10Device1 *p) { return DEV10(p)->d10_exceptions; }
static HRESULT STDMETHODCALLTYPE d10dev_GetPrivateData(ID3D10Device1 *p, REFGUID guid, UINT *size, void *data) {
  if (!guid || !size) return E_INVALIDARG;
  return dx_GetPrivateData(OBJ(DEV10(p)), guid, size, data);
}
static HRESULT STDMETHODCALLTYPE d10dev_SetPrivateData(ID3D10Device1 *p, REFGUID guid, UINT size, const void *data) {
  if (!guid) return E_INVALIDARG;
  return dx_SetPrivateData(OBJ(DEV10(p)), guid, size, data);
}
static HRESULT STDMETHODCALLTYPE d10dev_SetPrivateDataInterface(ID3D10Device1 *p, REFGUID guid, const IUnknown *object) {
  if (!guid) return E_INVALIDARG;
  return dx_SetPrivateDataInterface(OBJ(DEV10(p)), guid, object);
}
static void STDMETHODCALLTYPE d10dev_ClearState(ID3D10Device1 *p) { ctx_ClearState(DEV10(p)->ctx); }
static void STDMETHODCALLTYPE d10dev_Flush(ID3D10Device1 *p) { ctx_Flush(DEV10(p)->ctx); }

/* ---- making objects --------------------------------------------------------------------------------------------------------------------------------------------- */

/* What Direct3D 11 made -> its Direct3D 10 interface (the reference goes with it; S_FALSE and no object when the program asked for none). */
static HRESULT d10_give(HRESULT hr, void *object, void **out) {
  if (FAILED(hr) || !out) return hr;
  *out = d10_face(object);
  if (!*out && object) { dx_Release(OBJ(object)); return E_FAIL; }
  return hr;
}
static BOOL d10_resource_flags_valid(UINT bind, UINT cpu, UINT misc) {
  return !(bind & ~(UINT)D10_BIND_ALL) && !(cpu & ~(UINT)(D3D10_CPU_ACCESS_READ | D3D10_CPU_ACCESS_WRITE)) && !(misc & ~(UINT)D10_MISC_ALL);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateBuffer(ID3D10Device1 *p, const D3D10_BUFFER_DESC *desc, const D3D10_SUBRESOURCE_DATA *data, ID3D10Buffer **out) {
  if (out) *out = NULL;
  if (!desc || !d10_resource_flags_valid(desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags)) return E_INVALIDARG;
  /* (what is a texture's alone: mipmaps, a cube, a GDI device context) */
  if (desc->MiscFlags & (D3D10_RESOURCE_MISC_GENERATE_MIPS | D3D10_RESOURCE_MISC_TEXTURECUBE | D3D10_RESOURCE_MISC_GDI_COMPATIBLE)) return E_INVALIDARG;
  const D3D11_BUFFER_DESC d11 = {desc->ByteWidth, (D3D11_USAGE)desc->Usage, desc->BindFlags, desc->CPUAccessFlags, misc11_of_10(desc->MiscFlags), 0};
  ID3D11Buffer *b = NULL;
  const HRESULT hr = dev_CreateBuffer(DEV10(p), &d11, (const D3D11_SUBRESOURCE_DATA *)data, out ? &b : NULL);
  return d10_give(hr, b, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateTexture1D(ID3D10Device1 *p, const D3D10_TEXTURE1D_DESC *desc, const D3D10_SUBRESOURCE_DATA *data, ID3D10Texture1D **out) {
  if (out) *out = NULL;
  if (!desc || !d10_resource_flags_valid(desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags)) return E_INVALIDARG;
  const D3D11_TEXTURE1D_DESC d11 = {desc->Width, desc->MipLevels, desc->ArraySize, desc->Format, (D3D11_USAGE)desc->Usage, desc->BindFlags, desc->CPUAccessFlags,
                                    misc11_of_10(desc->MiscFlags)};
  ID3D11Texture1D *t = NULL;
  const HRESULT hr = dev_CreateTexture1D(DEV10(p), &d11, (const D3D11_SUBRESOURCE_DATA *)data, out ? &t : NULL);
  return d10_give(hr, t, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateTexture2D(ID3D10Device1 *p, const D3D10_TEXTURE2D_DESC *desc, const D3D10_SUBRESOURCE_DATA *data, ID3D10Texture2D **out) {
  if (out) *out = NULL;
  if (!desc || !d10_resource_flags_valid(desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags)) return E_INVALIDARG;
  /* (a cube is six slices; several cubes - an array of them - are Direct3D 10.1's) */
  if ((desc->MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE) && desc->ArraySize != 6 && DEV10(p)->level < D3D_FEATURE_LEVEL_10_1) return E_INVALIDARG;
  const D3D11_TEXTURE2D_DESC d11 = {desc->Width, desc->Height, desc->MipLevels, desc->ArraySize, desc->Format, desc->SampleDesc, (D3D11_USAGE)desc->Usage, desc->BindFlags,
                                    desc->CPUAccessFlags, misc11_of_10(desc->MiscFlags)};
  ID3D11Texture2D *t = NULL;
  const HRESULT hr = dev_CreateTexture2D(DEV10(p), &d11, (const D3D11_SUBRESOURCE_DATA *)data, out ? &t : NULL);
  return d10_give(hr, t, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateTexture3D(ID3D10Device1 *p, const D3D10_TEXTURE3D_DESC *desc, const D3D10_SUBRESOURCE_DATA *data, ID3D10Texture3D **out) {
  if (out) *out = NULL;
  if (!desc || !d10_resource_flags_valid(desc->BindFlags, desc->CPUAccessFlags, desc->MiscFlags)) return E_INVALIDARG;
  const D3D11_TEXTURE3D_DESC d11 = {desc->Width, desc->Height, desc->Depth, desc->MipLevels, desc->Format, (D3D11_USAGE)desc->Usage, desc->BindFlags, desc->CPUAccessFlags,
                                    misc11_of_10(desc->MiscFlags)};
  ID3D11Texture3D *t = NULL;
  const HRESULT hr = dev_CreateTexture3D(DEV10(p), &d11, (const D3D11_SUBRESOURCE_DATA *)data, out ? &t : NULL);
  return d10_give(hr, t, (void **)out);
}
/* A shader resource view of Direct3D 10.1's description (cube arrays from feature level 10_1 on: Direct3D 10's has none). */
static HRESULT d10_srv(Dev *d, ID3D10Resource *resource, const D3D10_SHADER_RESOURCE_VIEW_DESC1 *desc, UINT top_dimension, void **out) {
  if (out) *out = NULL;
  Child *r = d10_resource(resource);
  if (!r) return E_INVALIDARG;
  D3D11_SHADER_RESOURCE_VIEW_DESC d11;
  if (desc) {
    if ((UINT)desc->ViewDimension > top_dimension) return E_INVALIDARG;
    if (desc->ViewDimension == D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY && d->level < D3D_FEATURE_LEVEL_10_1) return E_INVALIDARG;
    memset(&d11, 0, sizeof d11);
    memcpy(&d11, desc, sizeof *desc);
  }
  ID3D11ShaderResourceView *v = NULL;
  const HRESULT hr = dev_CreateShaderResourceView(d, (ID3D11Resource *)r, desc ? &d11 : NULL, out ? &v : NULL);
  return d10_give(hr, v, out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateShaderResourceView(ID3D10Device1 *p, ID3D10Resource *resource, const D3D10_SHADER_RESOURCE_VIEW_DESC *desc,
                                                                 ID3D10ShaderResourceView **out) {
  D3D10_SHADER_RESOURCE_VIEW_DESC1 d1;
  if (desc) { memset(&d1, 0, sizeof d1); memcpy(&d1, desc, sizeof *desc); }
  return d10_srv(DEV10(p), resource, desc ? &d1 : NULL, D3D10_SRV_DIMENSION_TEXTURECUBE, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateShaderResourceView1(ID3D10Device1 *p, ID3D10Resource *resource, const D3D10_SHADER_RESOURCE_VIEW_DESC1 *desc,
                                                                  ID3D10ShaderResourceView1 **out) {
  return d10_srv(DEV10(p), resource, desc, D3D10_1_SRV_DIMENSION_TEXTURECUBEARRAY, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateRenderTargetView(ID3D10Device1 *p, ID3D10Resource *resource, const D3D10_RENDER_TARGET_VIEW_DESC *desc,
                                                               ID3D10RenderTargetView **out) {
  if (out) *out = NULL;
  Child *r = d10_resource(resource);
  if (!r) return E_INVALIDARG;
  D3D11_RENDER_TARGET_VIEW_DESC d11;
  if (desc) { memset(&d11, 0, sizeof d11); memcpy(&d11, desc, sizeof *desc); }
  ID3D11RenderTargetView *v = NULL;
  const HRESULT hr = dev_CreateRenderTargetView(DEV10(p), (ID3D11Resource *)r, desc ? &d11 : NULL, out ? &v : NULL);
  return d10_give(hr, v, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateDepthStencilView(ID3D10Device1 *p, ID3D10Resource *resource, const D3D10_DEPTH_STENCIL_VIEW_DESC *desc,
                                                               ID3D10DepthStencilView **out) {
  if (out) *out = NULL;
  Child *r = d10_resource(resource);
  if (!r) return E_INVALIDARG;
  D3D11_DEPTH_STENCIL_VIEW_DESC d11;
  if (desc) {
    memset(&d11, 0, sizeof d11);
    d11.Format = desc->Format;
    d11.ViewDimension = (D3D11_DSV_DIMENSION)desc->ViewDimension;
    d11.Flags = 0;      /* (read-only depth views are Direct3D 11's) */
    memcpy(&d11.Texture1D, &desc->Texture1D, sizeof *desc - offsetof(D3D10_DEPTH_STENCIL_VIEW_DESC, Texture1D));
  }
  ID3D11DepthStencilView *v = NULL;
  const HRESULT hr = dev_CreateDepthStencilView(DEV10(p), (ID3D11Resource *)r, desc ? &d11 : NULL, out ? &v : NULL);
  return d10_give(hr, v, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateInputLayout(ID3D10Device1 *p, const D3D10_INPUT_ELEMENT_DESC *elements, UINT count, const void *code, SIZE_T bytes,
                                                          ID3D10InputLayout **out) {
  if (out) *out = NULL;
  if (count > D3D10_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) return E_INVALIDARG;
  for (UINT i = 0; elements && i < count; ++i) if (elements[i].InputSlot >= D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return E_INVALIDARG;
  ID3D11InputLayout *l = NULL;
  const HRESULT hr = dev_CreateInputLayout(DEV10(p), (const D3D11_INPUT_ELEMENT_DESC *)elements, count, code, bytes, out ? &l : NULL);      /* (the same layout) */
  return d10_give(hr, l, (void **)out);
}

/* Direct3D 10 takes shader model 4.0 code (4.1 on a 10.1 device) of the stage asked for, in a SHDR part - SHEX (shader model 5) is Direct3D 11's: E_INVALIDARG for
 * anything else, a Direct3D 9 shader too. `type` is the stage's D3D10_SB_TOKENIZED_PROGRAM_TYPE (0 pixel, 1 vertex, 2 geometry); `any_of` a second one it may be. */
static HRESULT d10_shader_check(const Dev *d, const void *code, SIZE_T bytes, UINT type, UINT any_of) {
  const BYTE *b = code;
  if (!b || bytes < 32 || memcmp(b, "DXBC", 4) != 0) return E_INVALIDARG;
  UINT size, parts;
  memcpy(&size, b + 24, 4);
  memcpy(&parts, b + 28, 4);
  if (size > bytes || parts > (bytes - 32) / 4) return E_INVALIDARG;
  for (UINT i = 0; i < parts; ++i) {
    UINT at, length;
    memcpy(&at, b + 32 + 4 * (SIZE_T)i, 4);
    if (at > bytes || bytes - at < 16) return E_INVALIDARG;
    memcpy(&length, b + at + 4, 4);
    if (length > bytes - at - 8) return E_INVALIDARG;
    if (memcmp(b + at, "SHEX", 4) == 0) return E_INVALIDARG;
    if (memcmp(b + at, "SHDR", 4) != 0) continue;
    UINT version;
    memcpy(&version, b + at + 8, 4);
    const UINT program = version >> 16, major = (version >> 4) & 0xF, minor = version & 0xF;
    if (program != type && program != any_of) return E_INVALIDARG;
    if (major != 4 || minor > 1 || (minor == 1 && d->level < D3D_FEATURE_LEVEL_10_1)) return E_INVALIDARG;
    return S_OK;
  }
  return E_INVALIDARG;      /* (no code: a Direct3D 9 shader, or no shader) */
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateVertexShader(ID3D10Device1 *p, const void *code, SIZE_T bytes, ID3D10VertexShader **out) {
  if (out) *out = NULL;
  HRESULT hr = d10_shader_check(DEV10(p), code, bytes, 1, 1);
  if (FAILED(hr)) return hr;
  ID3D11VertexShader *s = NULL;
  hr = dev_CreateVertexShader(DEV10(p), code, bytes, NULL, out ? &s : NULL);
  return d10_give(hr, s, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateGeometryShader(ID3D10Device1 *p, const void *code, SIZE_T bytes, ID3D10GeometryShader **out) {
  if (out) *out = NULL;
  HRESULT hr = d10_shader_check(DEV10(p), code, bytes, 2, 2);
  if (FAILED(hr)) return hr;
  ID3D11GeometryShader *s = NULL;
  hr = dev_CreateGeometryShader(DEV10(p), code, bytes, NULL, out ? &s : NULL);
  return d10_give(hr, s, (void **)out);
}
/* Whether two semantic names are the same (Direct3D compares them without regard to case). */
static BOOL d10_same_name(const char *a, const char *b) {
  for (SIZE_T j = 0;; ++j) {
    char x = a[j], y = b[j];
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return FALSE;
    if (!x) return TRUE;
  }
}
/* An output of a shader's signature (its OSGN part): the components the shader writes of semantic `name` / `index`. FALSE when the code has no such output. */
static BOOL d10_output_of(const void *code, SIZE_T bytes, const char *name, UINT index, UINT *mask) {
  const BYTE *b = code;
  UINT parts;
  memcpy(&parts, b + 28, 4);      /* (the container's parts were checked by d10_shader_check) */
  for (UINT i = 0; i < parts; ++i) {
    UINT at, length, count;
    memcpy(&at, b + 32 + 4 * (SIZE_T)i, 4);
    memcpy(&length, b + at + 4, 4);
    if (memcmp(b + at, "OSGN", 4) != 0 || length < 8) continue;
    const BYTE *chunk = b + at + 8;
    memcpy(&count, chunk, 4);
    if (count > (length - 8) / 24) return FALSE;
    for (UINT k = 0; k < count; ++k) {
      const BYTE *e = chunk + 8 + 24 * (SIZE_T)k;
      UINT name_at, semantic_index;
      memcpy(&name_at, e, 4);
      memcpy(&semantic_index, e + 4, 4);
      if (name_at >= length || semantic_index != index) continue;
      SIZE_T end = name_at;
      while (end < length && chunk[end]) ++end;
      if (end == length) continue;      /* (a name that does not end in the part) */
      if (!d10_same_name((const char *)chunk + name_at, name)) continue;
      *mask = e[20] & 0xF;
      return TRUE;
    }
  }
  return FALSE;
}
/* Direct3D 10's stream output: one stream, in one buffer (all the elements, OutputStreamStride the record's size - not 0 -) or in a buffer each (OutputSlot 0 .. 3 of
 * them, one element in each, the stride 0). An element is components of an output the shader writes (each component once in a buffer), or a gap - no semantic, index and
 * first component 0 - of 1 or more components that are left as they are. At most 64 components a record. The code may be a vertex shader's (stream output with no
 * geometry stage), unless there are no elements: then it is a geometry shader like any other, with no stride. */
static HRESULT STDMETHODCALLTYPE d10dev_CreateGeometryShaderWithStreamOutput(ID3D10Device1 *p, const void *code, SIZE_T bytes, const D3D10_SO_DECLARATION_ENTRY *entries,
                                                                            UINT count, UINT stride, ID3D10GeometryShader **out) {
  Dev *d = DEV10(p);
  if (out) *out = NULL;
  HRESULT hr = d10_shader_check(d, code, bytes, 2, 1);
  if (FAILED(hr)) return hr;
  if (!count) {
    if (stride || d10_shader_check(d, code, bytes, 2, 2) != S_OK) return E_INVALIDARG;
    ID3D11GeometryShader *s = NULL;
    hr = dev_CreateGeometryShader(d, code, bytes, NULL, out ? &s : NULL);
    return d10_give(hr, s, (void **)out);
  }
  if (!entries || count > D3D10_SO_SINGLE_BUFFER_COMPONENT_LIMIT) return E_INVALIDARG;
  /* (Direct3D 11's elements: a gap of more than four components is several of Direct3D 11's, which have at most four) */
  D3D11_SO_DECLARATION_ENTRY e[2 * D3D10_SO_SINGLE_BUFFER_COMPONENT_LIMIT];
  UINT e_count = 0, per_slot[D3D10_SO_BUFFER_SLOT_COUNT] = {0, 0, 0, 0}, top_slot = 0, components = 0;
  for (UINT i = 0; i < count; ++i) {
    const D3D10_SO_DECLARATION_ENTRY *x = &entries[i];
    if (x->OutputSlot >= D3D10_SO_BUFFER_SLOT_COUNT || x->ComponentCount == 0 || x->ComponentCount > D3D10_SO_SINGLE_BUFFER_COMPONENT_LIMIT) return E_INVALIDARG;
    if (x->OutputSlot > top_slot) top_slot = x->OutputSlot;
    ++per_slot[x->OutputSlot];
    components += x->ComponentCount;
    if (!x->SemanticName) {
      if (x->SemanticIndex || x->StartComponent) return E_INVALIDARG;
      for (UINT left = x->ComponentCount; left;) {
        const UINT n = left < 4 ? left : 4;
        e[e_count++] = (D3D11_SO_DECLARATION_ENTRY){0, NULL, 0, 0, (BYTE)n, x->OutputSlot};
        left -= n;
      }
      continue;
    }
    UINT mask = 0;
    if (x->StartComponent > 3 || x->ComponentCount > 4u - x->StartComponent || !d10_output_of(code, bytes, x->SemanticName, x->SemanticIndex, &mask)) return E_INVALIDARG;
    const UINT want = ((1u << x->ComponentCount) - 1) << x->StartComponent;
    if ((want & mask) != want) return E_INVALIDARG;      /* (components the output has not) */
    for (UINT k = 0; k < i; ++k) {      /* (a component of an output once in each buffer) */
      const D3D10_SO_DECLARATION_ENTRY *y = &entries[k];
      if (!y->SemanticName || y->OutputSlot != x->OutputSlot || y->SemanticIndex != x->SemanticIndex || y->StartComponent > 3) continue;
      const UINT theirs = ((1u << y->ComponentCount) - 1) << y->StartComponent;
      if ((theirs & want) && d10_same_name(x->SemanticName, y->SemanticName)) return E_INVALIDARG;
    }
    e[e_count++] = (D3D11_SO_DECLARATION_ENTRY){0, x->SemanticName, x->SemanticIndex, x->StartComponent, x->ComponentCount, x->OutputSlot};
  }
  if (components > D3D10_SO_SINGLE_BUFFER_COMPONENT_LIMIT) return E_INVALIDARG;
  if (top_slot) {
    if (stride) return E_INVALIDARG;
    for (UINT slot = 0; slot < D3D10_SO_BUFFER_SLOT_COUNT; ++slot) if (per_slot[slot] > D3D10_SO_MULTIPLE_BUFFER_ELEMENTS_PER_BUFFER) return E_INVALIDARG;
  } else if (!stride || stride % 4 || stride > 2048 || stride < components * 4) {
    return E_INVALIDARG;
  }
  ID3D11GeometryShader *s = NULL;
  hr = dev_CreateGeometryShaderWithStreamOutput(d, code, bytes, e, e_count, top_slot ? NULL : &stride, top_slot ? 0 : 1, 0, NULL, out ? &s : NULL);
  return d10_give(hr, s, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreatePixelShader(ID3D10Device1 *p, const void *code, SIZE_T bytes, ID3D10PixelShader **out) {
  if (out) *out = NULL;
  HRESULT hr = d10_shader_check(DEV10(p), code, bytes, 0, 0);
  if (FAILED(hr)) return hr;
  ID3D11PixelShader *s = NULL;
  hr = dev_CreatePixelShader(DEV10(p), code, bytes, NULL, out ? &s : NULL);
  return d10_give(hr, s, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateBlendState1(ID3D10Device1 *p, const D3D10_BLEND_DESC1 *desc, ID3D10BlendState1 **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  ID3D11BlendState *s = NULL;
  const HRESULT hr = dev_CreateBlendState(DEV10(p), (const D3D11_BLEND_DESC *)desc, out ? &s : NULL);      /* (D3D10_BLEND_DESC1 is D3D11_BLEND_DESC) */
  return d10_give(hr, s, (void **)out);
}
/* Direct3D 10's blend: one blend for every target, enabled target by target - Direct3D 11's with the blend copied into every target (independent when the targets
 * differ in what is enabled or written). */
static HRESULT STDMETHODCALLTYPE d10dev_CreateBlendState(ID3D10Device1 *p, const D3D10_BLEND_DESC *desc, ID3D10BlendState **out) {
  if (out) *out = NULL;
  if (!desc) return E_INVALIDARG;
  D3D10_BLEND_DESC1 d1;
  memset(&d1, 0, sizeof d1);
  d1.AlphaToCoverageEnable = desc->AlphaToCoverageEnable;
  for (UINT i = 0; i + 1 < 8; ++i)
    if (!desc->BlendEnable[i] != !desc->BlendEnable[i + 1] || desc->RenderTargetWriteMask[i] != desc->RenderTargetWriteMask[i + 1]) d1.IndependentBlendEnable = TRUE;
  for (UINT i = 0; i < 8; ++i) {
    D3D10_RENDER_TARGET_BLEND_DESC1 *t = &d1.RenderTarget[i];
    t->BlendEnable = desc->BlendEnable[i];
    t->SrcBlend = desc->SrcBlend;
    t->DestBlend = desc->DestBlend;
    t->BlendOp = desc->BlendOp;
    t->SrcBlendAlpha = desc->SrcBlendAlpha;
    t->DestBlendAlpha = desc->DestBlendAlpha;
    t->BlendOpAlpha = desc->BlendOpAlpha;
    t->RenderTargetWriteMask = desc->RenderTargetWriteMask[i];
  }
  return d10dev_CreateBlendState1(p, &d1, (ID3D10BlendState1 **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateDepthStencilState(ID3D10Device1 *p, const D3D10_DEPTH_STENCIL_DESC *desc, ID3D10DepthStencilState **out) {
  if (out) *out = NULL;
  ID3D11DepthStencilState *s = NULL;
  const HRESULT hr = dev_CreateDepthStencilState(DEV10(p), (const D3D11_DEPTH_STENCIL_DESC *)desc, out ? &s : NULL);
  return d10_give(hr, s, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateRasterizerState(ID3D10Device1 *p, const D3D10_RASTERIZER_DESC *desc, ID3D10RasterizerState **out) {
  if (out) *out = NULL;
  ID3D11RasterizerState *s = NULL;
  const HRESULT hr = dev_CreateRasterizerState(DEV10(p), (const D3D11_RASTERIZER_DESC *)desc, out ? &s : NULL);
  return d10_give(hr, s, (void **)out);
}
/* (the MINIMUM_ and MAXIMUM_ filters are Direct3D 11.1's. D3D10_FILTER_TEXT_1BIT, the filter of Direct3D 10's own text drawing for R1_UNORM glyph textures, is refused:
 * the sampler below has no one-bit filter - docs/Direct3D10.md) */
static HRESULT STDMETHODCALLTYPE d10dev_CreateSamplerState(ID3D10Device1 *p, const D3D10_SAMPLER_DESC *desc, ID3D10SamplerState **out) {
  if (out) *out = NULL;
  if (desc && ((UINT)desc->Filter & ~(UINT)0xD5)) return E_INVALIDARG;
  ID3D11SamplerState *s = NULL;
  const HRESULT hr = dev_CreateSamplerState(DEV10(p), (const D3D11_SAMPLER_DESC *)desc, out ? &s : NULL);
  return d10_give(hr, s, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateQuery(ID3D10Device1 *p, const D3D10_QUERY_DESC *desc, ID3D10Query **out) {
  if (out) *out = NULL;
  if (!desc || (UINT)desc->Query > D3D10_QUERY_SO_OVERFLOW_PREDICATE) return E_INVALIDARG;
  ID3D11Query *q = NULL;
  const HRESULT hr = dev_CreateQuery(DEV10(p), (const D3D11_QUERY_DESC *)desc, out ? &q : NULL);
  return d10_give(hr, q, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d10dev_CreatePredicate(ID3D10Device1 *p, const D3D10_QUERY_DESC *desc, ID3D10Predicate **out) {
  if (out) *out = NULL;
  if (!desc || (UINT)desc->Query > D3D10_QUERY_SO_OVERFLOW_PREDICATE) return E_INVALIDARG;
  ID3D11Predicate *q = NULL;
  const HRESULT hr = dev_CreatePredicate(DEV10(p), (const D3D11_QUERY_DESC *)desc, out ? &q : NULL);
  return d10_give(hr, q, (void **)out);
}
/* Counters. Direct3D 10 names counters (D3D10_COUNTER_GPU_IDLE .. TEXTURE_CACHE_HIT_RATE) that a device may or may not have, and those a driver adds from
 * DEVICE_DEPENDENT_0 on; this device has none of either (CheckCounterInfo says so): DXGI_ERROR_UNSUPPORTED for those, E_INVALIDARG for a number that is no counter. */
static HRESULT d10_counter_known(const D3D10_COUNTER_DESC *desc) {
  if (!desc || desc->MiscFlags) return E_INVALIDARG;
  if ((UINT)desc->Counter <= D3D10_COUNTER_TEXTURE_CACHE_HIT_RATE || (UINT)desc->Counter >= D3D10_COUNTER_DEVICE_DEPENDENT_0) return DXGI_ERROR_UNSUPPORTED;
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE d10dev_CreateCounter(ID3D10Device1 *p, const D3D10_COUNTER_DESC *desc, ID3D10Counter **out) {
  (void)p;
  if (out) *out = NULL;
  return d10_counter_known(desc);
}
static void STDMETHODCALLTYPE d10dev_CheckCounterInfo(ID3D10Device1 *p, D3D10_COUNTER_INFO *info) { dev_CheckCounterInfo(DEV10(p), (D3D11_COUNTER_INFO *)info); }
static HRESULT STDMETHODCALLTYPE d10dev_CheckCounter(ID3D10Device1 *p, const D3D10_COUNTER_DESC *desc, D3D10_COUNTER_TYPE *type, UINT *active, LPSTR name, UINT *name_length,
                                                     LPSTR units, UINT *units_length, LPSTR description, UINT *description_length) {
  (void)p; (void)type; (void)active; (void)name; (void)name_length; (void)units; (void)units_length; (void)description; (void)description_length;
  return d10_counter_known(desc);
}
/* Direct3D 11's answer, in Direct3D 10's bits: no unordered access, typed or not, and no gather below feature level 10_1. */
static HRESULT STDMETHODCALLTYPE d10dev_CheckFormatSupport(ID3D10Device1 *p, DXGI_FORMAT format, UINT *support) {
  Dev *d = DEV10(p);
  const HRESULT hr = dev_CheckFormatSupport(d, format, support);
  if (SUCCEEDED(hr) && support) {
    *support &= 0x1FFFFFFu;      /* (D3D10_FORMAT_SUPPORT_BUFFER .. D3D10_FORMAT_SUPPORT_BACK_BUFFER_CAST) */
    if (d->level < D3D_FEATURE_LEVEL_10_1) *support &= ~(UINT)D3D10_FORMAT_SUPPORT_SHADER_GATHER;
    if (!*support) return E_FAIL;      /* (a format the device has no use for, as Direct3D 11 answers it) */
  }
  return hr;
}
static HRESULT STDMETHODCALLTYPE d10dev_CheckMultisampleQualityLevels(ID3D10Device1 *p, DXGI_FORMAT format, UINT samples, UINT *levels) {
  return dev_CheckMultisampleQualityLevels(DEV10(p), format, samples, levels);
}
static UINT STDMETHODCALLTYPE d10dev_GetCreationFlags(ID3D10Device1 *p) { return DEV10(p)->d10_flags; }
static HRESULT STDMETHODCALLTYPE d10dev_OpenSharedResource(ID3D10Device1 *p, HANDLE handle, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  IUnknown *r = NULL;
  HRESULT hr = dev_OpenSharedResource(DEV10(p), handle, &IID_ID3D11Resource, (void **)&r);
  if (FAILED(hr) || !r) return FAILED(hr) ? hr : E_INVALIDARG;
  hr = r->lpVtbl->QueryInterface(r, riid, out);      /* (a Direct3D 10 interface or a DXGI one, of the same object) */
  r->lpVtbl->Release(r);
  return hr;
}
static void STDMETHODCALLTYPE d10dev_SetTextFilterSize(ID3D10Device1 *p, UINT width, UINT height) {
  Dev *d = DEV10(p);
  d->d10_text_filter[0] = width;
  d->d10_text_filter[1] = height;
}
static void STDMETHODCALLTYPE d10dev_GetTextFilterSize(ID3D10Device1 *p, UINT *width, UINT *height) {
  const Dev *d = DEV10(p);
  if (width) *width = d->d10_text_filter[0];
  if (height) *height = d->d10_text_filter[1];
}
static D3D10_FEATURE_LEVEL1 STDMETHODCALLTYPE d10dev_GetFeatureLevel(ID3D10Device1 *p) { return (D3D10_FEATURE_LEVEL1)DEV10(p)->level; }

/* ---- ID3D10Multithread: the immediate context's ID3D11Multithread -------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE d10mt_QueryInterface(ID3D10Multithread *p, REFIID riid, void **out) { return dev_QueryInterface(DEVMT10(p), riid, out); }
static ULONG STDMETHODCALLTYPE d10mt_AddRef(ID3D10Multithread *p) { return dx_AddRef(OBJ(DEVMT10(p))); }
static ULONG STDMETHODCALLTYPE d10mt_Release(ID3D10Multithread *p) { return dx_Release(OBJ(DEVMT10(p))); }
static void STDMETHODCALLTYPE d10mt_Enter(ID3D10Multithread *p) { mt_Enter(&DEVMT10(p)->ctx->multithread); }
static void STDMETHODCALLTYPE d10mt_Leave(ID3D10Multithread *p) { mt_Leave(&DEVMT10(p)->ctx->multithread); }
static BOOL STDMETHODCALLTYPE d10mt_SetMultithreadProtected(ID3D10Multithread *p, BOOL on) { return mt_SetMultithreadProtected(&DEVMT10(p)->ctx->multithread, on); }
static BOOL STDMETHODCALLTYPE d10mt_GetMultithreadProtected(ID3D10Multithread *p) { return mt_GetMultithreadProtected(&DEVMT10(p)->ctx->multithread); }
static const ID3D10MultithreadVtbl g_d10mt_vtbl = {d10mt_QueryInterface, d10mt_AddRef, d10mt_Release, d10mt_Enter, d10mt_Leave, d10mt_SetMultithreadProtected,
                                                   d10mt_GetMultithreadProtected};

/* ---- making a Direct3D 10 device -------------------------------------------------------------------------------------------------------------------------------------- */

#define D10_CREATE_FLAGS (D3D10_CREATE_DEVICE_SINGLETHREADED | D3D10_CREATE_DEVICE_DEBUG | D3D10_CREATE_DEVICE_SWITCH_TO_REF | \
                          D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS | D3D10_CREATE_DEVICE_ALLOW_NULL_FROM_MAP | D3D10_CREATE_DEVICE_BGRA_SUPPORT | \
                          D3D10_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY | D3D10_CREATE_DEVICE_STRICT_VALIDATION | D3D10_CREATE_DEVICE_DEBUGGABLE)
/* What d3d10core.dll's D3D10CoreCreateDevice is (its .def forwards to this): a Direct3D 10 device of `level` (10_0, 10_1, or 9_1 .. 9_3 for Direct3D 10.1) on the
 * adapter, or DXGI_ERROR_UNSUPPORTED when the adapter has not that level. The debug layer and the switch to the reference rasterizer are the SDK's layers, which this system
 * does not have: DXGI_ERROR_SDK_COMPONENT_MISSING, as Windows answers without them. */
EXPORT HRESULT WINAPI TcosD3D10CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags, D3D_FEATURE_LEVEL level, ID3D10Device **out) {
  (void)factory;      /* (the adapter is the factory's) */
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (flags & ~(UINT)D10_CREATE_FLAGS) return E_INVALIDARG;
  if (level != D3D_FEATURE_LEVEL_10_0 && level != D3D_FEATURE_LEVEL_10_1 && level != D3D_FEATURE_LEVEL_9_1 && level != D3D_FEATURE_LEVEL_9_2 && level != D3D_FEATURE_LEVEL_9_3)
    return E_INVALIDARG;
  if (flags & (D3D10_CREATE_DEVICE_DEBUG | D3D10_CREATE_DEVICE_SWITCH_TO_REF)) return DXGI_ERROR_SDK_COMPONENT_MISSING;
  const UINT flags11 = flags & (D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS | D3D11_CREATE_DEVICE_BGRA_SUPPORT);
  Dev *d = NULL;
  const HRESULT hr = dev_create(adapter, flags11, &level, 1, &d);
  if (FAILED(hr)) return hr;
  d->level = level;      /* (EnableFeatureLevelUpgrade is Direct3D 11's) */
  d->d10.lpVtbl = &d10dev_vtbl;
  d->d10_flags = flags;
  d->ctx->protected_ = !(flags & D3D10_CREATE_DEVICE_SINGLETHREADED);      /* (Direct3D 10 devices are thread-safe unless made single-threaded) */
  *out = (ID3D10Device *)&d->d10;
  return S_OK;
}
