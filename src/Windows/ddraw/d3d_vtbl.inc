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
/* IDirect3D, IDirect3D2, IDirect3D3 and IDirect3D7 on the DirectDraw object, the vtables of the devices, and the registration of the Direct3D part with
 * DirectDraw (dd_d3d_register). */

#define D3D7O(i) D3D_IMPL(DDraw, vt_d3d7, i)
#define D3D3O(i) D3D_IMPL(DDraw, vt_d3d3, i)
#define D3D2O(i) D3D_IMPL(DDraw, vt_d3d2, i)
#define D3D1O(i) D3D_IMPL(DDraw, vt_d3d1, i)

/* What Windows' runtime gives in EnumDevices of Direct3D 1-3 besides the caps (Wine's test_enum_devices records it, and marks Wine's answers
 * todo): the software devices' hardware description is empty but for the sizes of its parts; their software description has these flags and
 * device caps; the HAL's software description has the flags of its colour model, caps, transform, lighting and clipping only, the caps of
 * transformed vertices in floats, no lights, no primitive caps, no depths and no texture or stipple sizes. */
static void enum_desc_windows(int kind, DlDeviceDesc *hal, DlDeviceDesc *hel) {
  if (kind != DL_DEVICE_HAL) {
    const uint32_t size = hal->dwSize;
    zero(hal, sizeof *hal);
    hal->dwSize = size;
    hal->dtcTransformCaps.dwSize = sizeof(D3DTRANSFORMCAPS);
    hal->dlcLightingCaps.dwSize = sizeof(D3DLIGHTINGCAPS);
    hal->dpcLineCaps.dwSize = sizeof(D3DPRIMCAPS);
    hal->dpcTriCaps.dwSize = sizeof(D3DPRIMCAPS);
    hel->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING | D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH |
                   D3DDD_DEVICEZBUFFERBITDEPTH | D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;
    hel->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_SORTINCREASINGZ | D3DDEVCAPS_SORTEXACT | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                     D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX;
    return;
  }
  hel->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING;
  hel->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX;
  hel->dlcLightingCaps.dwNumLights = 0;
  zero(&hel->dpcLineCaps, sizeof hel->dpcLineCaps);
  zero(&hel->dpcTriCaps, sizeof hel->dpcTriCaps);
  hel->dpcLineCaps.dwSize = hel->dpcTriCaps.dwSize = sizeof(D3DPRIMCAPS);
  hel->dwDeviceRenderBitDepth = hel->dwDeviceZBufferBitDepth = 0;
  hel->dwMaxVertexCount = hal->dwMaxVertexCount;
  if (hel->dwSize > DL_DEVICEDESC_V1_SIZE) {
    hel->dwMinTextureWidth = hel->dwMinTextureHeight = hel->dwMaxTextureWidth = hel->dwMaxTextureHeight = 0;
    hel->dwMinStippleWidth = hel->dwMinStippleHeight = hel->dwMaxStippleWidth = hel->dwMaxStippleHeight = 0;
  }
}

/* EnumDevices of Direct3D 1, 2 and 3: each device's GUID, description, name and its hardware and software descriptions (in the version's size). */
static HRESULT d3_enum_devices(DDraw *dd, UINT version, LPD3DENUMDEVICESCALLBACK cb, void *ctx) {
  if (!cb) return DDERR_INVALIDPARAMS;
  dd_lock();
  const DlCaps9 *caps = d3_caps(dd);
  dd_unlock();
  if (!caps) return DDERR_GENERIC;
  for (UINT k = 0;; ++k) {
    const int kind = dl_enum_device(version, k);
    if (kind == DL_DEVICE_NONE) break;
    DlDeviceDesc hal, hel;
    dl_device_descs(caps, kind, version, &hal, &hel);
    enum_desc_windows(kind, &hal, &hel);
    /* (writable copies: programs have written into the strings - Tomb Raider 3, Wine's note) */
    char name[64], description[128];
    lstrcpynA(name, dl_device_name(kind), sizeof name);
    lstrcpynA(description, dl_device_description(kind), sizeof description);
    GUID guid;
    memcpy(&guid, dl_device_guid(kind), sizeof guid);
    if (cb(&guid, description, name, (D3DDEVICEDESC *)&hal, (D3DDEVICEDESC *)&hel, ctx) != D3DENUMRET_OK) break;
  }
  return D3D_OK;
}
static HRESULT d3_find_device(DDraw *dd, UINT version, D3DFINDDEVICESEARCH *search, D3DFINDDEVICERESULT *result) {
  if (!search || !result) return DDERR_INVALIDPARAMS;
  dd_lock();
  const DlCaps9 *caps = d3_caps(dd);
  dd_unlock();
  if (!caps) return DDERR_GENERIC;
  if (search->dwSize != DL_FINDDEVICESEARCH_SIZE) return DDERR_INVALIDPARAMS;
  const DWORD size = result->dwSize;
  if (size != 20u + 2u * DL_DEVICEDESC_V1_SIZE && size != 20u + 2u * DL_DEVICEDESC_V2_SIZE && size != DL_FINDDEVICERESULT_SIZE) return DDERR_INVALIDPARAMS;
  return dl_find_device(caps, version, (const DlFindDeviceSearch *)search, dl_bytes(result, size));
}
static HRESULT d3_create_light(DDraw *dd, IDirect3DLight **out, IUnknown *outer) {
  if (outer) return CLASS_E_NOAGGREGATION;
  if (!out) return DDERR_INVALIDPARAMS;
  D3dLight *l = mem_alloc(sizeof *l);
  if (!l) return DDERR_OUTOFMEMORY;
  l->vt = &light_vtbl;
  l->ref = 1;
  l->dd = dd;
  *out = (IDirect3DLight *)l;
  return D3D_OK;
}
static HRESULT d3_create_material(DDraw *dd, UINT version, void **out, IUnknown *outer) {
  if (outer) return CLASS_E_NOAGGREGATION;
  if (!out) return DDERR_INVALIDPARAMS;
  D3dMaterial *m = mem_alloc(sizeof *m);
  if (!m) return DDERR_OUTOFMEMORY;
  m->vt3 = &mat3_vtbl;
  m->vt2 = &mat2_vtbl;
  m->vt1 = &mat1_vtbl;
  m->ref = 1;
  m->dd = dd;
  m->material.dwSize = DL_MATERIAL_SIZE;
  *out = version == 3 ? (void *)&m->vt3 : version == 2 ? (void *)&m->vt2 : (void *)&m->vt1;
  return D3D_OK;
}
static HRESULT d3_create_viewport(DDraw *dd, void **out, IUnknown *outer) {
  if (outer) return CLASS_E_NOAGGREGATION;
  if (!out) return DDERR_INVALIDPARAMS;
  D3dViewport *vp = mem_alloc(sizeof *vp);
  if (!vp) return DDERR_OUTOFMEMORY;
  vp->vt = &vp_vtbl;
  vp->ref = 1;
  vp->dd = dd;
  *out = vp;
  return D3D_OK;
}
/* EnumZBufferFormats: the depth formats Wine lists (16 before 24 and 32 bits: BattleZone II), and the 24-bit X8D24 once more with a bit depth of 24. */
static HRESULT d3_enum_zbuffer_formats(DDraw *dd, REFCLSID device, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) {
  (void)dd;
  if (!cb || !device) return DDERR_INVALIDPARAMS;
  if (dl_device_kind((const DlGuid *)device) == DL_DEVICE_NONE) return DDERR_INVALIDPARAMS;
  static const UINT formats[] = {DD_FMT_D15S1, DD_FMT_D16, DD_FMT_D24X8, DD_FMT_D24X4S4, DD_FMT_D24S8, DD_FMT_D32};
  for (UINT k = 0; k < sizeof formats / sizeof formats[0]; ++k) {
    DDPIXELFORMAT pf;
    ddpf_from_format(formats[k], &pf);
    if (cb(&pf, ctx) != DDENUMRET_OK) return D3D_OK;
  }
  DDPIXELFORMAT x8d24;
  zero(&x8d24, sizeof x8d24);
  x8d24.dwSize = sizeof x8d24;
  x8d24.dwFlags = DDPF_ZBUFFER;
  x8d24.dwZBufferBitDepth = 24;
  x8d24.dwZBitMask = 0x00FFFFFFu;
  cb(&x8d24, ctx);
  return D3D_OK;
}
/* EvictManagedTextures: the Direct3D copies of managed textures go (made again when they are used). */
static HRESULT d3_evict(DDraw *dd) {
  dd_lock();
  D3dDD *d = dd->d3d;
  if (d && d->dev) {
    for (Surface *s = dd->surfaces; s; s = s->list_next) {
      D3dRes *r = s->store.shadow.resource;
      if (!r || r->owner != d || r->kind != RES_TEXTURE || r->target_texture) continue;
      if (!(s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE))) continue;
      for (D3dDevice *dev = d->devices; dev; dev = dev->next)
        for (UINT st = 0; st < D3D_STAGES; ++st)
          if (dev->bound_res[st] == r) {
            if (d->active == dev) D3D_CALL(d->dev, SetTexture, st, NULL);
            dev->bound_res[st] = NULL;
          }
      res_free(r);
      s->store.shadow.resource = NULL;
    }
    ++d->tex_stamp;
  }
  dd_unlock();
  return D3D_OK;
}
static HRESULT d3_create_device_iface(DDraw *dd, REFCLSID guid, IUnknown *iface, UINT version, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  Surface *s = surface_from_any(iface);
  if (!s) return DDERR_INVALIDPARAMS;
  dd_lock();
  D3dDevice *dev;
  const HRESULT hr = d3_device_create(dd, guid, s, iface, version, NULL, &dev);
  dd_unlock();
  if (FAILED(hr)) return hr;
  *out = version == 7 ? (void *)DEVI7(dev) : version == 3 ? (void *)DEVI3(dev) : (void *)DEVI2(dev);
  return D3D_OK;
}

static HRESULT STDMETHODCALLTYPE d3d7_QueryInterface(IDirect3D7 *i, REFIID riid, void **out) { return IDirectDraw7_QueryInterface(DDI7(D3D7O(i)), riid, out); }
static ULONG STDMETHODCALLTYPE d3d7_AddRef(IDirect3D7 *i) { return IDirectDraw7_AddRef(DDI7(D3D7O(i))); }
static ULONG STDMETHODCALLTYPE d3d7_Release(IDirect3D7 *i) { return IDirectDraw7_Release(DDI7(D3D7O(i))); }
static HRESULT STDMETHODCALLTYPE d3d7_EnumDevices(IDirect3D7 *i, LPD3DENUMDEVICESCALLBACK7 cb, void *ctx) {
  DDraw *dd = D3D7O(i);
  if (!cb) return DDERR_INVALIDPARAMS;
  dd_lock();
  const DlCaps9 *caps = d3_caps(dd);
  dd_unlock();
  if (!caps) return DDERR_GENERIC;
  for (UINT k = 0;; ++k) {
    const int kind = dl_enum_device(7, k);
    if (kind == DL_DEVICE_NONE) break;
    D3DDEVICEDESC7 desc;
    dl_device_desc7(caps, kind, (DlDeviceDesc7 *)&desc);
    char name[64], description[128];
    lstrcpynA(name, dl_device_name(kind), sizeof name);
    lstrcpynA(description, dl_device_description(kind), sizeof description);
    if (cb(description, name, &desc, ctx) != D3DENUMRET_OK) break;
  }
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE d3d7_CreateDevice(IDirect3D7 *i, REFCLSID guid, IDirectDrawSurface7 *s, IDirect3DDevice7 **out) {
  return d3_create_device_iface(D3D7O(i), guid, (IUnknown *)s, 7, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d3d7_CreateVertexBuffer(IDirect3D7 *i, D3DVERTEXBUFFERDESC *desc, IDirect3DVertexBuffer7 **out, DWORD flags) {
  (void)flags;      /* (documented as 0, any value is taken: Wine's test_process_vertices) */
  return vb_create(D3D7O(i), 7, desc, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d3d7_EnumZBufferFormats(IDirect3D7 *i, REFCLSID device, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) {
  return d3_enum_zbuffer_formats(D3D7O(i), device, cb, ctx);
}
static HRESULT STDMETHODCALLTYPE d3d7_EvictManagedTextures(IDirect3D7 *i) { return d3_evict(D3D7O(i)); }
static const IDirect3D7Vtbl d3d7_vtbl = {d3d7_QueryInterface, d3d7_AddRef, d3d7_Release, d3d7_EnumDevices, d3d7_CreateDevice, d3d7_CreateVertexBuffer,
                                         d3d7_EnumZBufferFormats, d3d7_EvictManagedTextures};

static HRESULT STDMETHODCALLTYPE d3d3_QueryInterface(IDirect3D3 *i, REFIID riid, void **out) { return IDirectDraw7_QueryInterface(DDI7(D3D3O(i)), riid, out); }
static ULONG STDMETHODCALLTYPE d3d3_AddRef(IDirect3D3 *i) { return IDirectDraw_AddRef(DDI1(D3D3O(i))); }
static ULONG STDMETHODCALLTYPE d3d3_Release(IDirect3D3 *i) { return IDirectDraw_Release(DDI1(D3D3O(i))); }
static HRESULT STDMETHODCALLTYPE d3d3_EnumDevices(IDirect3D3 *i, LPD3DENUMDEVICESCALLBACK cb, void *ctx) { return d3_enum_devices(D3D3O(i), 3, cb, ctx); }
static HRESULT STDMETHODCALLTYPE d3d3_CreateLight(IDirect3D3 *i, IDirect3DLight **out, IUnknown *outer) { return d3_create_light(D3D3O(i), out, outer); }
static HRESULT STDMETHODCALLTYPE d3d3_CreateMaterial(IDirect3D3 *i, IDirect3DMaterial3 **out, IUnknown *outer) { return d3_create_material(D3D3O(i), 3, (void **)out, outer); }
static HRESULT STDMETHODCALLTYPE d3d3_CreateViewport(IDirect3D3 *i, IDirect3DViewport3 **out, IUnknown *outer) { return d3_create_viewport(D3D3O(i), (void **)out, outer); }
static HRESULT STDMETHODCALLTYPE d3d3_FindDevice(IDirect3D3 *i, D3DFINDDEVICESEARCH *s, D3DFINDDEVICERESULT *r) { return d3_find_device(D3D3O(i), 3, s, r); }
static HRESULT STDMETHODCALLTYPE d3d3_CreateDevice(IDirect3D3 *i, REFCLSID guid, IDirectDrawSurface4 *s, IDirect3DDevice3 **out, IUnknown *outer) {
  if (outer) return CLASS_E_NOAGGREGATION;
  return d3_create_device_iface(D3D3O(i), guid, (IUnknown *)s, 3, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d3d3_CreateVertexBuffer(IDirect3D3 *i, D3DVERTEXBUFFERDESC *desc, IDirect3DVertexBuffer **out, DWORD flags, IUnknown *outer) {
  if (outer) return CLASS_E_NOAGGREGATION;
  (void)flags;
  return vb_create(D3D3O(i), 3, desc, (void **)out);
}
static HRESULT STDMETHODCALLTYPE d3d3_EnumZBufferFormats(IDirect3D3 *i, REFCLSID device, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx) {
  return d3_enum_zbuffer_formats(D3D3O(i), device, cb, ctx);
}
static HRESULT STDMETHODCALLTYPE d3d3_EvictManagedTextures(IDirect3D3 *i) { return d3_evict(D3D3O(i)); }
static const IDirect3D3Vtbl d3d3_vtbl = {d3d3_QueryInterface, d3d3_AddRef, d3d3_Release, d3d3_EnumDevices, d3d3_CreateLight, d3d3_CreateMaterial,
                                         d3d3_CreateViewport, d3d3_FindDevice, d3d3_CreateDevice, d3d3_CreateVertexBuffer, d3d3_EnumZBufferFormats,
                                         d3d3_EvictManagedTextures};

static HRESULT STDMETHODCALLTYPE d3d2_QueryInterface(IDirect3D2 *i, REFIID riid, void **out) { return IDirectDraw7_QueryInterface(DDI7(D3D2O(i)), riid, out); }
static ULONG STDMETHODCALLTYPE d3d2_AddRef(IDirect3D2 *i) { return IDirectDraw_AddRef(DDI1(D3D2O(i))); }
static ULONG STDMETHODCALLTYPE d3d2_Release(IDirect3D2 *i) { return IDirectDraw_Release(DDI1(D3D2O(i))); }
static HRESULT STDMETHODCALLTYPE d3d2_EnumDevices(IDirect3D2 *i, LPD3DENUMDEVICESCALLBACK cb, void *ctx) { return d3_enum_devices(D3D2O(i), 2, cb, ctx); }
static HRESULT STDMETHODCALLTYPE d3d2_CreateLight(IDirect3D2 *i, IDirect3DLight **out, IUnknown *outer) { return d3_create_light(D3D2O(i), out, outer); }
static HRESULT STDMETHODCALLTYPE d3d2_CreateMaterial(IDirect3D2 *i, IDirect3DMaterial2 **out, IUnknown *outer) { return d3_create_material(D3D2O(i), 2, (void **)out, outer); }
static HRESULT STDMETHODCALLTYPE d3d2_CreateViewport(IDirect3D2 *i, IDirect3DViewport2 **out, IUnknown *outer) { return d3_create_viewport(D3D2O(i), (void **)out, outer); }
static HRESULT STDMETHODCALLTYPE d3d2_FindDevice(IDirect3D2 *i, D3DFINDDEVICESEARCH *s, D3DFINDDEVICERESULT *r) { return d3_find_device(D3D2O(i), 2, s, r); }
static HRESULT STDMETHODCALLTYPE d3d2_CreateDevice(IDirect3D2 *i, REFCLSID guid, IDirectDrawSurface *s, IDirect3DDevice2 **out) {
  return d3_create_device_iface(D3D2O(i), guid, (IUnknown *)s, 2, (void **)out);
}
static const IDirect3D2Vtbl d3d2_vtbl = {d3d2_QueryInterface, d3d2_AddRef, d3d2_Release, d3d2_EnumDevices, d3d2_CreateLight, d3d2_CreateMaterial,
                                         d3d2_CreateViewport, d3d2_FindDevice, d3d2_CreateDevice};

static HRESULT STDMETHODCALLTYPE d3d1_QueryInterface(IDirect3D *i, REFIID riid, void **out) { return IDirectDraw7_QueryInterface(DDI7(D3D1O(i)), riid, out); }
static ULONG STDMETHODCALLTYPE d3d1_AddRef(IDirect3D *i) { return IDirectDraw_AddRef(DDI1(D3D1O(i))); }
static ULONG STDMETHODCALLTYPE d3d1_Release(IDirect3D *i) { return IDirectDraw_Release(DDI1(D3D1O(i))); }
static HRESULT STDMETHODCALLTYPE d3d1_Initialize(IDirect3D *i, REFIID riid) { (void)i; (void)riid; return DDERR_ALREADYINITIALIZED; }
static HRESULT STDMETHODCALLTYPE d3d1_EnumDevices(IDirect3D *i, LPD3DENUMDEVICESCALLBACK cb, void *ctx) { return d3_enum_devices(D3D1O(i), 1, cb, ctx); }
static HRESULT STDMETHODCALLTYPE d3d1_CreateLight(IDirect3D *i, IDirect3DLight **out, IUnknown *outer) { return d3_create_light(D3D1O(i), out, outer); }
static HRESULT STDMETHODCALLTYPE d3d1_CreateMaterial(IDirect3D *i, IDirect3DMaterial **out, IUnknown *outer) { return d3_create_material(D3D1O(i), 1, (void **)out, outer); }
static HRESULT STDMETHODCALLTYPE d3d1_CreateViewport(IDirect3D *i, IDirect3DViewport **out, IUnknown *outer) { return d3_create_viewport(D3D1O(i), (void **)out, outer); }
static HRESULT STDMETHODCALLTYPE d3d1_FindDevice(IDirect3D *i, D3DFINDDEVICESEARCH *s, D3DFINDDEVICERESULT *r) { return d3_find_device(D3D1O(i), 1, s, r); }
static const IDirect3DVtbl d3d1_vtbl = {d3d1_QueryInterface, d3d1_AddRef, d3d1_Release, d3d1_Initialize, d3d1_EnumDevices, d3d1_CreateLight, d3d1_CreateMaterial,
                                        d3d1_CreateViewport, d3d1_FindDevice};

/* ---- the device vtables (filled by name: the compiler checks each method's type against d3d.h) -------------------------------------------------- */

static const IDirect3DDevice7Vtbl dev7_vtbl = {
  .QueryInterface = dev7_QueryInterface, .AddRef = dev7_AddRef, .Release = dev7_Release, .GetCaps = dev7_GetCaps, .EnumTextureFormats = dev7_EnumTextureFormats,
  .BeginScene = dev7_BeginScene, .EndScene = dev7_EndScene, .GetDirect3D = dev7_GetDirect3D, .SetRenderTarget = dev7_SetRenderTarget,
  .GetRenderTarget = dev7_GetRenderTarget, .Clear = dev7_Clear, .SetTransform = dev7_SetTransform, .GetTransform = dev7_GetTransform,
  .SetViewport = dev7_SetViewport, .MultiplyTransform = dev7_MultiplyTransform, .GetViewport = dev7_GetViewport, .SetMaterial = dev7_SetMaterial,
  .GetMaterial = dev7_GetMaterial, .SetLight = dev7_SetLight, .GetLight = dev7_GetLight, .SetRenderState = dev7_SetRenderState,
  .GetRenderState = dev7_GetRenderState, .BeginStateBlock = dev7_BeginStateBlock, .EndStateBlock = dev7_EndStateBlock, .PreLoad = dev7_PreLoad,
  .DrawPrimitive = dev7_DrawPrimitive, .DrawIndexedPrimitive = dev7_DrawIndexedPrimitive, .SetClipStatus = dev7_SetClipStatus,
  .GetClipStatus = dev7_GetClipStatus, .DrawPrimitiveStrided = dev7_DrawPrimitiveStrided, .DrawIndexedPrimitiveStrided = dev7_DrawIndexedPrimitiveStrided,
  .DrawPrimitiveVB = dev7_DrawPrimitiveVB, .DrawIndexedPrimitiveVB = dev7_DrawIndexedPrimitiveVB, .ComputeSphereVisibility = dev7_ComputeSphereVisibility,
  .GetTexture = dev7_GetTexture, .SetTexture = dev7_SetTexture, .GetTextureStageState = dev7_GetTextureStageState,
  .SetTextureStageState = dev7_SetTextureStageState, .ValidateDevice = dev7_ValidateDevice, .ApplyStateBlock = dev7_ApplyStateBlock,
  .CaptureStateBlock = dev7_CaptureStateBlock, .DeleteStateBlock = dev7_DeleteStateBlock, .CreateStateBlock = dev7_CreateStateBlock, .Load = dev7_Load,
  .LightEnable = dev7_LightEnable, .GetLightEnable = dev7_GetLightEnable, .SetClipPlane = dev7_SetClipPlane, .GetClipPlane = dev7_GetClipPlane,
  .GetInfo = dev7_GetInfo};
static const IDirect3DDevice3Vtbl dev3_vtbl = {
  .QueryInterface = dev3_QueryInterface, .AddRef = dev3_AddRef, .Release = dev3_Release, .GetCaps = dev3_GetCaps, .GetStats = dev3_GetStats,
  .AddViewport = dev3_AddViewport, .DeleteViewport = dev3_DeleteViewport, .NextViewport = dev3_NextViewport, .EnumTextureFormats = dev3_EnumTextureFormats,
  .BeginScene = dev3_BeginScene, .EndScene = dev3_EndScene, .GetDirect3D = dev3_GetDirect3D, .SetCurrentViewport = dev3_SetCurrentViewport,
  .GetCurrentViewport = dev3_GetCurrentViewport, .SetRenderTarget = dev3_SetRenderTarget, .GetRenderTarget = dev3_GetRenderTarget, .Begin = dev3_Begin,
  .BeginIndexed = dev3_BeginIndexed, .Vertex = dev3_Vertex, .Index = dev3_Index, .End = dev3_End, .GetRenderState = dev3_GetRenderState,
  .SetRenderState = dev3_SetRenderState, .GetLightState = dev3_GetLightState, .SetLightState = dev3_SetLightState, .SetTransform = dev3_SetTransform,
  .GetTransform = dev3_GetTransform, .MultiplyTransform = dev3_MultiplyTransform, .DrawPrimitive = dev3_DrawPrimitive,
  .DrawIndexedPrimitive = dev3_DrawIndexedPrimitive, .SetClipStatus = dev3_SetClipStatus, .GetClipStatus = dev3_GetClipStatus,
  .DrawPrimitiveStrided = dev3_DrawPrimitiveStrided, .DrawIndexedPrimitiveStrided = dev3_DrawIndexedPrimitiveStrided, .DrawPrimitiveVB = dev3_DrawPrimitiveVB,
  .DrawIndexedPrimitiveVB = dev3_DrawIndexedPrimitiveVB, .ComputeSphereVisibility = dev3_ComputeSphereVisibility, .GetTexture = dev3_GetTexture,
  .SetTexture = dev3_SetTexture, .GetTextureStageState = dev3_GetTextureStageState, .SetTextureStageState = dev3_SetTextureStageState,
  .ValidateDevice = dev3_ValidateDevice};
static const IDirect3DDevice2Vtbl dev2_vtbl = {
  .QueryInterface = dev2_QueryInterface, .AddRef = dev2_AddRef, .Release = dev2_Release, .GetCaps = dev2_GetCaps, .SwapTextureHandles = dev2_SwapTextureHandles,
  .GetStats = dev2_GetStats, .AddViewport = dev2_AddViewport, .DeleteViewport = dev2_DeleteViewport, .NextViewport = dev2_NextViewport,
  .EnumTextureFormats = dev2_EnumTextureFormats, .BeginScene = dev2_BeginScene, .EndScene = dev2_EndScene, .GetDirect3D = dev2_GetDirect3D,
  .SetCurrentViewport = dev2_SetCurrentViewport, .GetCurrentViewport = dev2_GetCurrentViewport, .SetRenderTarget = dev2_SetRenderTarget,
  .GetRenderTarget = dev2_GetRenderTarget, .Begin = dev2_Begin, .BeginIndexed = dev2_BeginIndexed, .Vertex = dev2_Vertex, .Index = dev2_Index, .End = dev2_End,
  .GetRenderState = dev2_GetRenderState, .SetRenderState = dev2_SetRenderState, .GetLightState = dev2_GetLightState, .SetLightState = dev2_SetLightState,
  .SetTransform = dev2_SetTransform, .GetTransform = dev2_GetTransform, .MultiplyTransform = dev2_MultiplyTransform, .DrawPrimitive = dev2_DrawPrimitive,
  .DrawIndexedPrimitive = dev2_DrawIndexedPrimitive, .SetClipStatus = dev2_SetClipStatus, .GetClipStatus = dev2_GetClipStatus};
static const IDirect3DDeviceVtbl dev1_vtbl = {
  .QueryInterface = dev1_QueryInterface, .AddRef = dev1_AddRef, .Release = dev1_Release, .Initialize = dev1_Initialize, .GetCaps = dev1_GetCaps,
  .SwapTextureHandles = dev1_SwapTextureHandles, .CreateExecuteBuffer = dev1_CreateExecuteBuffer, .GetStats = dev1_GetStats, .Execute = dev1_Execute,
  .AddViewport = dev1_AddViewport, .DeleteViewport = dev1_DeleteViewport, .NextViewport = dev1_NextViewport, .Pick = dev1_Pick,
  .GetPickRecords = dev1_GetPickRecords, .EnumTextureFormats = dev1_EnumTextureFormats, .CreateMatrix = dev1_CreateMatrix, .SetMatrix = dev1_SetMatrix,
  .GetMatrix = dev1_GetMatrix, .DeleteMatrix = dev1_DeleteMatrix, .BeginScene = dev1_BeginScene, .EndScene = dev1_EndScene, .GetDirect3D = dev1_GetDirect3D};

/* ---- the hooks ------------------------------------------------------------------------------------------------------------------------------------ */

/* QueryInterface of a version 1-4 surface for a device GUID: the surface's Direct3D 1 device, made the first time (aggregated: it counts on the
 * surface's IDirectDrawSurface). */
static HRESULT hook_surface_query(Surface *s, REFIID riid, void **out) {
  const int kind = dl_device_kind((const DlGuid *)riid);
  if (kind == DL_DEVICE_NONE) return E_NOINTERFACE;
  dd_lock();
  D3dSurf *ds = d3_surf(s);
  HRESULT hr = ds ? D3D_OK : DDERR_OUTOFMEMORY;
  if (ds && !ds->device1) {
    D3dDevice *dev;
    hr = d3_device_create(s->dd, riid, s, (IUnknown *)IFACE1(s), 1, (IUnknown *)IFACE1(s), &dev);
    if (SUCCEEDED(hr)) ds->device1 = dev;
  }
  if (SUCCEEDED(hr)) {
    IUnknown_AddRef(ds->device1->outer);
    *out = DEVI1(ds->device1);
  }
  dd_unlock();
  return hr;
}

static HRESULT hook_ddraw_query(DDraw *dd, REFIID riid, void **out) {
  (void)dd; (void)riid; (void)out;
  return E_NOINTERFACE;
}

void dd_d3d_register(DdD3dHooks *hooks) {
  hooks->d3d7_vtbl = &d3d7_vtbl;
  hooks->d3d3_vtbl = &d3d3_vtbl;
  hooks->d3d2_vtbl = &d3d2_vtbl;
  hooks->d3d1_vtbl = &d3d1_vtbl;
  hooks->texture2_vtbl = &tex2_vtbl;
  hooks->texture1_vtbl = &tex1_vtbl;
  hooks->ddraw_query = hook_ddraw_query;
  hooks->surface_query = hook_surface_query;
  hooks->pull = hook_pull;
  hooks->cpu_wrote = hook_cpu_wrote;
  hooks->surface_destroyed = hook_surface_destroyed;
  hooks->ddraw_destroyed = hook_ddraw_destroyed;
  hooks->flipped = hook_flipped;
}
