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
/* IDirect3D .. IDirect3D7 on the DirectDraw object, and the objects they make: viewports, lights, materials, vertex buffers; execute buffers (made by
 * devices of Direct3D 1); IDirect3DTexture and IDirect3DTexture2 on texture surfaces (d3d.inc). */

/* ---- lights ------------------------------------------------------------------------------------------------------------------------------------- */

static const IDirect3DLightVtbl light_vtbl;
static D3dLight *light_from(const void *iface) { return iface && *(const void *const *)iface == &light_vtbl ? (D3dLight *)iface : NULL; }

static HRESULT STDMETHODCALLTYPE light_QueryInterface(IDirect3DLight *i, REFIID riid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDirect3DLight)) {
    IDirect3DLight_AddRef(i);
    *out = i;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE light_AddRef(IDirect3DLight *i) { return (ULONG)InterlockedIncrement(&((D3dLight *)i)->ref); }
static ULONG STDMETHODCALLTYPE light_Release(IDirect3DLight *i) {
  D3dLight *l = (D3dLight *)i;
  const LONG r = InterlockedDecrement(&l->ref);
  if (!r) mem_free(l);
  return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE light_Initialize(IDirect3DLight *i, IDirect3D *d3d) { (void)i; (void)d3d; return DDERR_ALREADYINITIALIZED; }
/* SetLight: a D3DLIGHT (76 bytes, always on) or D3DLIGHT2 (80, D3DLIGHT_ACTIVE); the type POINT .. PARALLELPOINT. */
static HRESULT STDMETHODCALLTYPE light_SetLight(IDirect3DLight *i, D3DLIGHT *data) {
  D3dLight *l = (D3dLight *)i;
  if (!data || (data->dwSize != DL_LIGHT_SIZE && data->dwSize != DL_LIGHT2_SIZE)) return DDERR_INVALIDPARAMS;
  if (!data->dltType || data->dltType > D3DLIGHT_PARALLELPOINT) return DDERR_INVALIDPARAMS;
  dd_lock();
  zero(&l->light, sizeof l->light);
  memcpy(&l->light, data, data->dwSize);
  if (data->dwSize == DL_LIGHT_SIZE) l->light.dwFlags = D3DLIGHT_ACTIVE;
  l->set = TRUE;
  dd_unlock();
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE light_GetLight(IDirect3DLight *i, D3DLIGHT *data) {
  D3dLight *l = (D3dLight *)i;
  if (!data || (data->dwSize != DL_LIGHT_SIZE && data->dwSize != DL_LIGHT2_SIZE)) return DDERR_INVALIDPARAMS;
  dd_lock();
  const DWORD size = data->dwSize;
  DlLight2 copy = l->light;
  copy.dwSize = size;
  memcpy(data, &copy, size);
  dd_unlock();
  return D3D_OK;
}
static const IDirect3DLightVtbl light_vtbl = {light_QueryInterface, light_AddRef, light_Release, light_Initialize, light_SetLight, light_GetLight};

/* ---- materials ------------------------------------------------------------------------------------------------------------------------------------ */

static const IDirect3DMaterial3Vtbl mat3_vtbl;
static const IDirect3DMaterial2Vtbl mat2_vtbl;
static const IDirect3DMaterialVtbl mat1_vtbl;
static D3dMaterial *material_from(const void *iface) {
  if (!iface) return NULL;
  const void *vt = *(const void *const *)iface;
  if (vt == &mat3_vtbl) return D3D_IMPL(D3dMaterial, vt3, iface);
  if (vt == &mat2_vtbl) return D3D_IMPL(D3dMaterial, vt2, iface);
  if (vt == &mat1_vtbl) return D3D_IMPL(D3dMaterial, vt1, iface);
  return NULL;
}
static HRESULT mat_query(D3dMaterial *m, REFIID riid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  void *iface = NULL;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDirect3DMaterial3)) iface = &m->vt3;
  else if (guid_eq(riid, &IID_IDirect3DMaterial2)) iface = &m->vt2;
  else if (guid_eq(riid, &IID_IDirect3DMaterial)) iface = &m->vt1;
  if (!iface) { *out = NULL; return E_NOINTERFACE; }
  InterlockedIncrement(&m->ref);
  *out = iface;
  return S_OK;
}
static ULONG mat_release(D3dMaterial *m) {
  const LONG r = InterlockedDecrement(&m->ref);
  if (!r) {
    dd_lock();
    if (m->handle) handle_free(&g_handles, m->handle, H_MATERIAL);
    dd_unlock();
    mem_free(m);
  }
  return (ULONG)r;
}
static HRESULT mat_set(D3dMaterial *m, const D3DMATERIAL *data) {
  if (!data || data->dwSize != DL_MATERIAL_SIZE) return DDERR_INVALIDPARAMS;
  dd_lock();
  memcpy(&m->material, data, DL_MATERIAL_SIZE);
  dd_unlock();
  return D3D_OK;
}
static HRESULT mat_get(D3dMaterial *m, D3DMATERIAL *data) {
  if (!data || data->dwSize != DL_MATERIAL_SIZE) return DDERR_INVALIDPARAMS;
  dd_lock();
  memcpy(data, &m->material, DL_MATERIAL_SIZE);
  dd_unlock();
  return D3D_OK;
}
static HRESULT mat_handle(D3dMaterial *m, const void *device, D3DMATERIALHANDLE *out) {
  if (!out || !device) return DDERR_INVALIDPARAMS;
  dd_lock();
  if (!m->handle) m->handle = handle_alloc(&g_handles, H_MATERIAL, m, NULL);
  const DWORD h = m->handle;
  dd_unlock();
  if (!h) return DDERR_OUTOFMEMORY;
  *out = h;
  return D3D_OK;
}
#define M3(i) D3D_IMPL(D3dMaterial, vt3, i)
#define M2(i) D3D_IMPL(D3dMaterial, vt2, i)
#define M1(i) D3D_IMPL(D3dMaterial, vt1, i)
static HRESULT STDMETHODCALLTYPE mat3_QueryInterface(IDirect3DMaterial3 *i, REFIID riid, void **out) { return mat_query(M3(i), riid, out); }
static ULONG STDMETHODCALLTYPE mat3_AddRef(IDirect3DMaterial3 *i) { return (ULONG)InterlockedIncrement(&M3(i)->ref); }
static ULONG STDMETHODCALLTYPE mat3_Release(IDirect3DMaterial3 *i) { return mat_release(M3(i)); }
static HRESULT STDMETHODCALLTYPE mat3_SetMaterial(IDirect3DMaterial3 *i, D3DMATERIAL *d) { return mat_set(M3(i), d); }
static HRESULT STDMETHODCALLTYPE mat3_GetMaterial(IDirect3DMaterial3 *i, D3DMATERIAL *d) { return mat_get(M3(i), d); }
static HRESULT STDMETHODCALLTYPE mat3_GetHandle(IDirect3DMaterial3 *i, IDirect3DDevice3 *dev, D3DMATERIALHANDLE *h) { return mat_handle(M3(i), dev, h); }
static HRESULT STDMETHODCALLTYPE mat2_QueryInterface(IDirect3DMaterial2 *i, REFIID riid, void **out) { return mat_query(M2(i), riid, out); }
static ULONG STDMETHODCALLTYPE mat2_AddRef(IDirect3DMaterial2 *i) { return (ULONG)InterlockedIncrement(&M2(i)->ref); }
static ULONG STDMETHODCALLTYPE mat2_Release(IDirect3DMaterial2 *i) { return mat_release(M2(i)); }
static HRESULT STDMETHODCALLTYPE mat2_SetMaterial(IDirect3DMaterial2 *i, D3DMATERIAL *d) { return mat_set(M2(i), d); }
static HRESULT STDMETHODCALLTYPE mat2_GetMaterial(IDirect3DMaterial2 *i, D3DMATERIAL *d) { return mat_get(M2(i), d); }
static HRESULT STDMETHODCALLTYPE mat2_GetHandle(IDirect3DMaterial2 *i, IDirect3DDevice2 *dev, D3DMATERIALHANDLE *h) { return mat_handle(M2(i), dev, h); }
static HRESULT STDMETHODCALLTYPE mat1_QueryInterface(IDirect3DMaterial *i, REFIID riid, void **out) { return mat_query(M1(i), riid, out); }
static ULONG STDMETHODCALLTYPE mat1_AddRef(IDirect3DMaterial *i) { return (ULONG)InterlockedIncrement(&M1(i)->ref); }
static ULONG STDMETHODCALLTYPE mat1_Release(IDirect3DMaterial *i) { return mat_release(M1(i)); }
static HRESULT STDMETHODCALLTYPE mat1_Initialize(IDirect3DMaterial *i, IDirect3D *d3d) { (void)i; (void)d3d; return DDERR_ALREADYINITIALIZED; }
static HRESULT STDMETHODCALLTYPE mat1_SetMaterial(IDirect3DMaterial *i, D3DMATERIAL *d) { return mat_set(M1(i), d); }
static HRESULT STDMETHODCALLTYPE mat1_GetMaterial(IDirect3DMaterial *i, D3DMATERIAL *d) { return mat_get(M1(i), d); }
static HRESULT STDMETHODCALLTYPE mat1_GetHandle(IDirect3DMaterial *i, IDirect3DDevice *dev, D3DMATERIALHANDLE *h) { return mat_handle(M1(i), dev, h); }
/* (Reserve and Unreserve are for the colour ramps of palette devices, which DirectX 5 and later do not have: DDERR_UNSUPPORTED, as Windows) */
static HRESULT STDMETHODCALLTYPE mat1_Reserve(IDirect3DMaterial *i) { (void)i; return DDERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE mat1_Unreserve(IDirect3DMaterial *i) { (void)i; return DDERR_UNSUPPORTED; }
static const IDirect3DMaterial3Vtbl mat3_vtbl = {mat3_QueryInterface, mat3_AddRef, mat3_Release, mat3_SetMaterial, mat3_GetMaterial, mat3_GetHandle};
static const IDirect3DMaterial2Vtbl mat2_vtbl = {mat2_QueryInterface, mat2_AddRef, mat2_Release, mat2_SetMaterial, mat2_GetMaterial, mat2_GetHandle};
static const IDirect3DMaterialVtbl mat1_vtbl = {mat1_QueryInterface, mat1_AddRef, mat1_Release, mat1_Initialize, mat1_SetMaterial, mat1_GetMaterial,
                                                mat1_GetHandle, mat1_Reserve, mat1_Unreserve};

/* ---- viewports ------------------------------------------------------------------------------------------------------------------------------------ */

static const IDirect3DViewport3Vtbl vp_vtbl;
static D3dViewport *viewport_from(const void *iface) { return iface && *(const void *const *)iface == &vp_vtbl ? (D3dViewport *)iface : NULL; }

/* The viewport becomes its device's: the rectangle and clip space (Direct3D 1-3 transform with it). */
static void viewport_activate(D3dViewport *vp) {
  D3dDevice *dev = vp->device;
  if (!dev || !vp->set) return;
  DlViewport7 v7;
  if (vp->set == 2) dl_viewport7_from_v2(&vp->vp2, &v7, &dev->clip);
  else dl_viewport7_from_v1(&vp->vp1, &v7, &dev->clip);
  D3DVIEWPORT7 v;
  memcpy(&v, &v7, sizeof v);
  d3_set_viewport(dev, &v);
}

static HRESULT STDMETHODCALLTYPE vp_QueryInterface(IDirect3DViewport3 *i, REFIID riid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDirect3DViewport) || guid_eq(riid, &IID_IDirect3DViewport2) || guid_eq(riid, &IID_IDirect3DViewport3)) {
    IDirect3DViewport3_AddRef(i);
    *out = i;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE vp_AddRef(IDirect3DViewport3 *i) { return (ULONG)InterlockedIncrement(&((D3dViewport *)i)->ref); }
static ULONG STDMETHODCALLTYPE vp_Release(IDirect3DViewport3 *i) {
  D3dViewport *vp = (D3dViewport *)i;
  const LONG r = InterlockedDecrement(&vp->ref);
  if (!r) {
    dd_lock();
    /* (its lights let go of it; a background depth surface it holds goes) */
    while (vp->lights) {
      D3dLight *l = vp->lights;
      vp->lights = l->next_in_viewport;
      l->viewport = NULL;
      l->next_in_viewport = NULL;
      IDirect3DLight_Release((IDirect3DLight *)l);
    }
    IUnknown *depth = vp->background_depth_iface;
    dd_unlock();
    if (depth) IUnknown_Release(depth);
    mem_free(vp);
  }
  return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE vp_Initialize(IDirect3DViewport3 *i, IDirect3D *d3d) { (void)i; (void)d3d; return DDERR_ALREADYINITIALIZED; }
static HRESULT STDMETHODCALLTYPE vp_GetViewport(IDirect3DViewport3 *i, D3DVIEWPORT *out) {
  D3dViewport *vp = (D3dViewport *)i;
  if (!out || out->dwSize != DL_VIEWPORT1_SIZE) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = D3D_OK;
  if (!vp->set) hr = D3DERR_VIEWPORTDATANOTSET;
  else {
    DlViewport1 v = vp->vp1;
    if (vp->set == 2) dl_viewport1_from_v2(&vp->vp2, &v);
    const DWORD size = out->dwSize < sizeof v ? out->dwSize : (DWORD)sizeof v;
    v.dwSize = out->dwSize;
    memcpy(out, &v, size);
  }
  dd_unlock();
  return hr;
}
static HRESULT vp_target_check(D3dViewport *vp, DWORD expected_size, DWORD size, DWORD x, DWORD y, DWORD w, DWORD h) {
  D3dDevice *dev = vp->device;
  if (size != expected_size) return DDERR_INVALIDPARAMS;
  if (!dev) return D3DERR_VIEWPORTHASNODEVICE;
  if (dev->version > 1 && !dev->target) return DDERR_INVALIDCAPS;
  const DWORD tw = dev->target ? dev->target->desc.dwWidth : 0, th = dev->target ? dev->target->desc.dwHeight : 0;
  return dl_viewport_check(size, expected_size, dev->version, x, y, w, h, tw, th);
}
static HRESULT STDMETHODCALLTYPE vp_SetViewport(IDirect3DViewport3 *i, D3DVIEWPORT *in) {
  D3dViewport *vp = (D3dViewport *)i;
  if (!in) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = vp_target_check(vp, DL_VIEWPORT1_SIZE, in->dwSize, in->dwX, in->dwY, in->dwWidth, in->dwHeight);
  if (SUCCEEDED(hr)) {
    memcpy(&vp->vp1, in, DL_VIEWPORT1_SIZE);
    vp->vp1.dvMinZ = 0.0f;      /* (what Windows does: Wine's tests of Direct3D 1 programs) */
    vp->vp1.dvMaxZ = 1.0f;
    vp->set = 1;
    if (vp->device->current_viewport == vp) viewport_activate(vp);
  }
  dd_unlock();
  return hr;
}
/* TransformVertices: the vertices through the viewport's device's world, view and projection and the viewport's own clip space and rectangle. */
static HRESULT STDMETHODCALLTYPE vp_TransformVertices(IDirect3DViewport3 *i, DWORD count, D3DTRANSFORMDATA *data, DWORD flags, DWORD *offscreen) {
  D3dViewport *vp = (D3dViewport *)i;
  if (!vp->device) return D3DERR_VIEWPORTHASNODEVICE;
  if (!data || data->dwSize != sizeof *data) return DDERR_INVALIDPARAMS;
  if (!(flags & (D3DTRANSFORM_CLIPPED | D3DTRANSFORM_UNCLIPPED))) return DDERR_INVALIDPARAMS;
  if (!data->lpIn || !data->lpOut) return DDERR_INVALIDPARAMS;
  dd_lock();
  D3dDevice *dev = vp->device;
  DlTransform t;
  d3_transform(dev, &t);
  if (vp->set) {
    DlViewport7 v7;
    if (vp->set == 2) dl_viewport7_from_v2(&vp->vp2, &v7, &t.clip);
    else dl_viewport7_from_v1(&vp->vp1, &v7, &t.clip);
    t.viewport = v7;
    dl_transform_prepare(&t);
  }
  const SIZE_T in_size = count ? (SIZE_T)(count - 1) * data->dwInSize + (data->dwInSize < 12 ? 12 : data->dwInSize) : 0;
  const SIZE_T out_size = count ? (SIZE_T)(count - 1) * data->dwOutSize + (data->dwOutSize < 16 ? 16 : data->dwOutSize) : 0;
  uint32_t off = 0;
  const HRESULT hr = dl_transform_vertices(&t, count, dl_const_bytes(data->lpIn, in_size), data->dwInSize, dl_bytes(data->lpOut, out_size), data->dwOutSize,
                                           (DlHVertex *)data->lpHOut, data->lpHOut ? count : 0, flags, &off);
  if (hr == DL_OK) {
    if (offscreen) *offscreen = off;
    data->dwClip = off;
  }
  dd_unlock();
  return hr;
}
/* LightElements: Windows never implemented it (DDERR_UNSUPPORTED, as Wine records). */
static HRESULT STDMETHODCALLTYPE vp_LightElements(IDirect3DViewport3 *i, DWORD count, D3DLIGHTDATA *data) { (void)i; (void)count; (void)data; return DDERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE vp_SetBackground(IDirect3DViewport3 *i, D3DMATERIALHANDLE h) {
  D3dViewport *vp = (D3dViewport *)i;
  dd_lock();
  const HRESULT hr = handle_get(&g_handles, h, H_MATERIAL) ? D3D_OK : DDERR_INVALIDPARAMS;
  if (SUCCEEDED(hr)) vp->background = h;
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE vp_GetBackground(IDirect3DViewport3 *i, D3DMATERIALHANDLE *h, BOOL *valid) {
  D3dViewport *vp = (D3dViewport *)i;
  dd_lock();
  const BOOL ok = vp->background && handle_get(&g_handles, vp->background, H_MATERIAL);
  if (valid) *valid = ok;
  if (h) *h = ok ? vp->background : 0;
  dd_unlock();
  return D3D_OK;
}
/* SetBackgroundDepth: a depth surface whose contents Clear copies into the depth buffer instead of a constant depth. */
static HRESULT vp_set_background_depth(D3dViewport *vp, IUnknown *iface) {
  Surface *s = surface_from_any(iface);
  if (iface && !s) return DDERR_INVALIDPARAMS;
  dd_lock();
  IUnknown *old = vp->background_depth_iface;
  if (iface) IUnknown_AddRef(iface);
  vp->background_depth_iface = iface;
  vp->background_depth = s;
  dd_unlock();
  if (old) IUnknown_Release(old);
  return D3D_OK;
}
static HRESULT vp_get_background_depth(D3dViewport *vp, REFIID iid, void **out, BOOL *valid) {
  if (!out) return DDERR_INVALIDPARAMS;
  dd_lock();
  IUnknown *iface = vp->background_depth_iface;
  if (valid) *valid = iface != NULL;
  *out = NULL;
  HRESULT hr = D3D_OK;
  if (iface) hr = IUnknown_QueryInterface(iface, iid, out);
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE vp_SetBackgroundDepth(IDirect3DViewport3 *i, IDirectDrawSurface *s) { return vp_set_background_depth((D3dViewport *)i, (IUnknown *)s); }
static HRESULT STDMETHODCALLTYPE vp_GetBackgroundDepth(IDirect3DViewport3 *i, IDirectDrawSurface **s, BOOL *valid) {
  return vp_get_background_depth((D3dViewport *)i, &IID_IDirectDrawSurface, (void **)s, valid);
}
static HRESULT STDMETHODCALLTYPE vp_SetBackgroundDepth2(IDirect3DViewport3 *i, IDirectDrawSurface4 *s) { return vp_set_background_depth((D3dViewport *)i, (IUnknown *)s); }
static HRESULT STDMETHODCALLTYPE vp_GetBackgroundDepth2(IDirect3DViewport3 *i, IDirectDrawSurface4 **s, BOOL *valid) {
  return vp_get_background_depth((D3dViewport *)i, &IID_IDirectDrawSurface4, (void **)s, valid);
}
/* Clear / Clear2 within the viewport's rectangle (Clear: the background material's diffuse colour, depth 1 or the background depth surface). */
static HRESULT vp_clear(D3dViewport *vp, DWORD count, D3DRECT *rects, DWORD flags, DWORD color, float z, DWORD stencil, BOOL legacy) {
  if (!rects || !count) return D3D_OK;
  dd_lock();
  D3dDevice *dev = vp->device;
  HRESULT hr;
  if (!dev) { dd_unlock(); return D3DERR_VIEWPORTHASNODEVICE; }
  if (!dev->dd3) { dd_unlock(); return DDERR_GENERIC; }
  if (legacy) {
    const D3dMaterial *m = handle_get(&g_handles, vp->background, H_MATERIAL);
    color = 0;
    if (m) {
      float c[4] = {m->material.diffuse.r, m->material.diffuse.g, m->material.diffuse.b, m->material.diffuse.a};
      color = dl_color_from_float(c);
    }
    flags &= D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER;
  }
  DlViewport7 v7;
  DlMatrix clip;
  if (vp->set == 2) dl_viewport7_from_v2(&vp->vp2, &v7, &clip);
  else if (vp->set == 1) dl_viewport7_from_v1(&vp->vp1, &v7, &clip);
  else { v7.dwX = v7.dwY = 0; v7.dwWidth = dev->target ? dev->target->desc.dwWidth : 0; v7.dwHeight = dev->target ? dev->target->desc.dwHeight : 0; }
  D3DVIEWPORT7 v;
  memcpy(&v, &v7, sizeof v);
  Surface *bg = legacy ? vp->background_depth : NULL;
  if (bg && (flags & D3DCLEAR_ZBUFFER)) {
    /* (the background depth surface goes into the depth buffer's rectangles) */
    Surface *zs = attached_depth(dev->target);
    flags &= ~(DWORD)D3DCLEAR_ZBUFFER;
    if (zs) {
      for (DWORD k = 0; k < count; ++k) {
        RECT r = {rects[k].x1, rects[k].y1, rects[k].x2, rects[k].y2}, vr = {(LONG)v.dwX, (LONG)v.dwY, (LONG)(v.dwX + v.dwWidth), (LONG)(v.dwY + v.dwHeight)};
        RECT c, b = {0, 0, (LONG)bg->desc.dwWidth, (LONG)bg->desc.dwHeight}, zr = {0, 0, (LONG)zs->desc.dwWidth, (LONG)zs->desc.dwHeight};
        if (!rect_intersect(&c, &r, &vr) || !rect_intersect(&c, &c, &b) || !rect_intersect(&c, &c, &zr)) continue;
        blit_rect(zs, &c, bg, &c, NULL);
      }
    }
  }
  hr = flags ? d3_clear(dev, count, rects, flags, color, z, stencil, &v) : D3D_OK;
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE vp_Clear(IDirect3DViewport3 *i, DWORD count, D3DRECT *rects, DWORD flags) {
  return vp_clear((D3dViewport *)i, count, rects, flags, 0, 1.0f, 0, TRUE);
}
static HRESULT STDMETHODCALLTYPE vp_Clear2(IDirect3DViewport3 *i, DWORD count, D3DRECT *rects, DWORD flags, DWORD color, D3DVALUE z, DWORD stencil) {
  return vp_clear((D3dViewport *)i, count, rects, flags, color, z, stencil, FALSE);
}
static HRESULT STDMETHODCALLTYPE vp_AddLight(IDirect3DViewport3 *i, IDirect3DLight *light) {
  D3dViewport *vp = (D3dViewport *)i;
  D3dLight *l = light_from(light);
  if (!l) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = D3D_OK;
  if (l->viewport) hr = D3DERR_LIGHTHASVIEWPORT;
  else {
    D3dLight **p = &vp->lights;
    while (*p) p = &(*p)->next_in_viewport;
    *p = l;
    l->viewport = vp;
    l->next_in_viewport = NULL;
    IDirect3DLight_AddRef(light);
  }
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE vp_DeleteLight(IDirect3DViewport3 *i, IDirect3DLight *light) {
  D3dViewport *vp = (D3dViewport *)i;
  D3dLight *l = light_from(light);
  if (!l) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = DDERR_INVALIDPARAMS;
  if (l->viewport == vp) {
    for (D3dLight **p = &vp->lights; *p; p = &(*p)->next_in_viewport)
      if (*p == l) { *p = l->next_in_viewport; break; }
    l->viewport = NULL;
    l->next_in_viewport = NULL;
    hr = D3D_OK;
  }
  dd_unlock();
  if (SUCCEEDED(hr)) IDirect3DLight_Release(light);
  return hr;
}
static HRESULT STDMETHODCALLTYPE vp_NextLight(IDirect3DViewport3 *i, IDirect3DLight *ref, IDirect3DLight **out, DWORD flags) {
  D3dViewport *vp = (D3dViewport *)i;
  if (!out) return DDERR_INVALIDPARAMS;
  dd_lock();
  D3dLight *l = light_from(ref), *found = NULL;
  switch (flags) {
  case D3DNEXT_NEXT: found = l && l->viewport == vp ? l->next_in_viewport : NULL; break;
  case D3DNEXT_HEAD: found = vp->lights; break;
  case D3DNEXT_TAIL: for (found = vp->lights; found && found->next_in_viewport; found = found->next_in_viewport) {} break;
  default: break;
  }
  if (found) IDirect3DLight_AddRef((IDirect3DLight *)found);
  *out = (IDirect3DLight *)found;
  dd_unlock();
  return found ? D3D_OK : DDERR_INVALIDPARAMS;
}
static HRESULT STDMETHODCALLTYPE vp_GetViewport2(IDirect3DViewport3 *i, D3DVIEWPORT2 *out) {
  D3dViewport *vp = (D3dViewport *)i;
  if (!out || out->dwSize != DL_VIEWPORT2_SIZE) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = D3D_OK;
  if (!vp->set) hr = D3DERR_VIEWPORTDATANOTSET;
  else {
    DlViewport2 v = vp->vp2;
    if (vp->set == 1) dl_viewport2_from_v1(&vp->vp1, &v);
    const DWORD size = out->dwSize < sizeof v ? out->dwSize : (DWORD)sizeof v;
    v.dwSize = out->dwSize;
    memcpy(out, &v, size);
  }
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE vp_SetViewport2(IDirect3DViewport3 *i, D3DVIEWPORT2 *in) {
  D3dViewport *vp = (D3dViewport *)i;
  if (!in) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = vp_target_check(vp, DL_VIEWPORT2_SIZE, in->dwSize, in->dwX, in->dwY, in->dwWidth, in->dwHeight);
  if (SUCCEEDED(hr)) {
    memcpy(&vp->vp2, in, DL_VIEWPORT2_SIZE);
    vp->set = 2;
    if (vp->device->current_viewport == vp) viewport_activate(vp);
  }
  dd_unlock();
  return hr;
}
static const IDirect3DViewport3Vtbl vp_vtbl = {
  vp_QueryInterface, vp_AddRef, vp_Release, vp_Initialize, vp_GetViewport, vp_SetViewport, vp_TransformVertices, vp_LightElements, vp_SetBackground,
  vp_GetBackground, vp_SetBackgroundDepth, vp_GetBackgroundDepth, vp_Clear, vp_AddLight, vp_DeleteLight, vp_NextLight, vp_GetViewport2, vp_SetViewport2,
  vp_SetBackgroundDepth2, vp_GetBackgroundDepth2, vp_Clear2};

/* ---- execute buffers ------------------------------------------------------------------------------------------------------------------------------ */

static const IDirect3DExecuteBufferVtbl eb_vtbl;
static D3dExecuteBuffer *execute_buffer_from(const void *iface) { return iface && *(const void *const *)iface == &eb_vtbl ? (D3dExecuteBuffer *)iface : NULL; }

static HRESULT STDMETHODCALLTYPE eb_QueryInterface(IDirect3DExecuteBuffer *i, REFIID riid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDirect3DExecuteBuffer)) {
    IDirect3DExecuteBuffer_AddRef(i);
    *out = i;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE eb_AddRef(IDirect3DExecuteBuffer *i) { return (ULONG)InterlockedIncrement(&((D3dExecuteBuffer *)i)->ref); }
static ULONG STDMETHODCALLTYPE eb_Release(IDirect3DExecuteBuffer *i) {
  D3dExecuteBuffer *eb = (D3dExecuteBuffer *)i;
  const LONG r = InterlockedDecrement(&eb->ref);
  if (!r) {
    if (eb->own) mem_free(eb->memory);
    mem_free(eb);
  }
  return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE eb_Initialize(IDirect3DExecuteBuffer *i, IDirect3DDevice *dev, D3DEXECUTEBUFFERDESC *desc) {
  (void)i; (void)dev; (void)desc;
  return DDERR_ALREADYINITIALIZED;
}
static HRESULT STDMETHODCALLTYPE eb_Lock(IDirect3DExecuteBuffer *i, D3DEXECUTEBUFFERDESC *desc) {
  D3dExecuteBuffer *eb = (D3dExecuteBuffer *)i;
  if (!desc || desc->dwSize < sizeof(DlExecuteBufferDesc)) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = D3D_OK;
  if (eb->locked) hr = D3DERR_EXECUTE_LOCKED;
  else {
    desc->dwFlags = D3DDEB_BUFSIZE | D3DDEB_CAPS | D3DDEB_LPDATA;
    desc->dwCaps = eb->caps;
    desc->dwBufferSize = eb->size;
    desc->lpData = eb->memory;
    eb->locked = TRUE;
  }
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE eb_Unlock(IDirect3DExecuteBuffer *i) {
  D3dExecuteBuffer *eb = (D3dExecuteBuffer *)i;
  dd_lock();
  const HRESULT hr = eb->locked ? D3D_OK : D3DERR_EXECUTE_NOT_LOCKED;
  eb->locked = FALSE;
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE eb_SetExecuteData(IDirect3DExecuteBuffer *i, D3DEXECUTEDATA *data) {
  D3dExecuteBuffer *eb = (D3dExecuteBuffer *)i;
  if (!data || data->dwSize != DL_EXECUTEDATA_SIZE) return DDERR_INVALIDPARAMS;
  /* (only the size is checked here: the offsets and counts are Execute's to check - Wine's test_execute_data) */
  dd_lock();
  memcpy(&eb->data, data, DL_EXECUTEDATA_SIZE);
  dd_unlock();
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE eb_GetExecuteData(IDirect3DExecuteBuffer *i, D3DEXECUTEDATA *data) {
  D3dExecuteBuffer *eb = (D3dExecuteBuffer *)i;
  if (!data) return DDERR_INVALIDPARAMS;
  dd_lock();
  memcpy(data, &eb->data, DL_EXECUTEDATA_SIZE);
  dd_unlock();
  return D3D_OK;
}
/* Validate and Optimize: Microsoft documents both as not implemented by the runtime (DDERR_UNSUPPORTED). */
static HRESULT STDMETHODCALLTYPE eb_Validate(IDirect3DExecuteBuffer *i, DWORD *offset, LPD3DVALIDATECALLBACK cb, void *ctx, DWORD reserved) {
  (void)i; (void)offset; (void)cb; (void)ctx; (void)reserved;
  return DDERR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE eb_Optimize(IDirect3DExecuteBuffer *i, DWORD reserved) { (void)i; (void)reserved; return DDERR_UNSUPPORTED; }
static const IDirect3DExecuteBufferVtbl eb_vtbl = {eb_QueryInterface, eb_AddRef, eb_Release, eb_Initialize, eb_Lock, eb_Unlock, eb_SetExecuteData, eb_GetExecuteData,
                                                   eb_Validate, eb_Optimize};

static HRESULT d3_execute_buffer_create(D3dDevice *dev, D3DEXECUTEBUFFERDESC *desc, IDirect3DExecuteBuffer **out) {
  if (!desc || !out || desc->dwSize < sizeof(DlExecuteBufferDesc)) return DDERR_INVALIDPARAMS;
  D3dExecuteBuffer *eb = mem_alloc(sizeof *eb);
  if (!eb) return DDERR_OUTOFMEMORY;
  eb->vt = &eb_vtbl;
  eb->ref = 1;
  eb->device = dev;
  eb->size = (desc->dwFlags & D3DDEB_BUFSIZE) ? desc->dwBufferSize : 0;
  eb->caps = (desc->dwFlags & D3DDEB_CAPS) ? desc->dwCaps : D3DDEBCAPS_SYSTEMMEMORY;
  if ((desc->dwFlags & D3DDEB_LPDATA) && desc->lpData) eb->memory = desc->lpData;
  else if (eb->size) {
    if (eb->size > 0x10000000u || !(eb->memory = mem_alloc(eb->size))) { mem_free(eb); return DDERR_OUTOFMEMORY; }
    eb->own = TRUE;
  }
  eb->data.dwSize = DL_EXECUTEDATA_SIZE;
  *out = (IDirect3DExecuteBuffer *)eb;
  return D3D_OK;
}

/* The processed vertices of a device: room for `count` (D3DTLVERTEX and their clip-space forms). */
static BOOL tl_room(D3dDevice *dev, UINT count) {
  if (dev->tl_count >= count) return TRUE;
  BYTE *tl = mem_grow(dev->tl, (SIZE_T)count * DL_VERTEX_SIZE);
  if (!tl) return FALSE;
  dev->tl = tl;
  DlClipVertex *cv = mem_grow(dev->tl_clip, (SIZE_T)count * sizeof *cv);
  if (!cv) return FALSE;
  dev->tl_clip = cv;
  dev->tl_count = count;
  return TRUE;
}

/* A point of the screen inside a processed triangle (for Pick): the depth there. */
static BOOL pick_hit(const DlTlVertex *a, const DlTlVertex *b, const DlTlVertex *c, float x, float y, float *z) {
  const float d = (b->sx - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (b->sy - a->sy);
  if (!(d > 0.0f || d < 0.0f)) return FALSE;
  const float u = ((x - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (y - a->sy)) / d;
  const float v = ((b->sx - a->sx) * (y - a->sy) - (x - a->sx) * (b->sy - a->sy)) / d;
  if (u < 0.0f || v < 0.0f || u + v > 1.0f) return FALSE;
  *z = a->sz + u * (b->sz - a->sz) + v * (c->sz - a->sz);
  return TRUE;
}

/* Carries out an execute buffer (Execute), or with `pick` only finds the triangles under a point (Pick). */
static HRESULT d3_run(D3dDevice *dev, D3dExecuteBuffer *eb, const D3DRECT *pick) {
  DlExecutor x;
  HRESULT hr = dl_execute_begin(&x, dl_const_bytes(eb->memory, eb->size), &eb->data, (DlStatus *)&eb->data.dsStatus);
  if (hr != DL_OK) return hr;
  if (!tl_room(dev, eb->data.dwVertexCount ? eb->data.dwVertexCount : 1)) return DDERR_OUTOFMEMORY;
  if (pick) { dev->pick_count = 0; }
  else if (FAILED(hr = d3_flush(dev))) return hr;
  const DlConstBytes buffer = dl_const_bytes(eb->memory, eb->size);
  const DlTlVertex *tl = (const DlTlVertex *)dev->tl;
  BOOL zvisible = FALSE;
  DlCommand cmd;
  int r;
  while ((r = dl_execute_next(&x, &cmd)) == DL_NEXT_COMMAND) {
    switch (cmd.opcode) {
    case DL_OP_POINT: case DL_OP_SPAN:
    case DL_OP_LINE: case DL_OP_TRIANGLE: {
      if (pick && cmd.opcode != DL_OP_TRIANGLE) break;
      DlTransform t;
      d3_transform(dev, &t);
      Assembly a;
      zero(&a, sizeof a);
      a.dev = dev;
      a.t = &t;
      a.fvf = D3DFVF_TLVERTEX;
      a.stride = DL_VERTEX_SIZE;
      const uint32_t planes = dl_clip_planes(&t);
      const BOOL wire = dev->st.rs[D3DRENDERSTATE_FILLMODE] == D3DFILL_WIREFRAME;
      DWORD out_type = cmd.opcode == DL_OP_TRIANGLE ? (wire ? D9_PT_LINELIST : D9_PT_TRIANGLELIST) : cmd.opcode == DL_OP_LINE ? D9_PT_LINELIST : D9_PT_POINTLIST;
      for (uint32_t k = 0; k < cmd.count; ++k) {
        if (cmd.opcode == DL_OP_TRIANGLE) {
          DlTriangle tri;
          if (dl_command_triangle(&x, &cmd, k, &tri) != DL_OK) continue;
          if (pick) {
            float z;
            if (pick_hit(&tl[tri.v1], &tl[tri.v2], &tl[tri.v3], (float)pick->x1, (float)pick->y1, &z)) {
              D3DPICKRECORD *grown = mem_grow(dev->picks, (SIZE_T)(dev->pick_count + 1) * sizeof *grown);
              if (grown) {
                dev->picks = grown;
                grown[dev->pick_count].bOpcode = D3DOP_TRIANGLE;
                grown[dev->pick_count].bPad = 0;
                grown[dev->pick_count].dwOffset = cmd.header - eb->data.dwInstructionOffset;
                grown[dev->pick_count].dvZ = z;
                ++dev->pick_count;
              }
            }
            continue;
          }
          const DlClipVertex *c[3] = {&dev->tl_clip[tri.v1], &dev->tl_clip[tri.v2], &dev->tl_clip[tri.v3]};
          if (wire) {
            uint8_t pairs[6];
            const unsigned n = dl_triangle_edges(tri.wFlags, pairs);
            for (unsigned e = 0; e < n; ++e) {
              DlClipVertex l[2] = {*c[pairs[2 * e]], *c[pairs[2 * e + 1]]};
              if (dl_clip_line(&l[0], &l[1], planes)) assembly_put(&a, l, 2);
            }
          } else {
            assembly_triangle(&a, c[0], c[1], c[2], planes, TRUE);
          }
        } else if (cmd.opcode == DL_OP_LINE) {
          DlLine ln;
          if (dl_command_line(&x, &cmd, k, &ln) != DL_OK) continue;
          DlClipVertex l[2] = {dev->tl_clip[ln.v1], dev->tl_clip[ln.v2]};
          if (dl_clip_line(&l[0], &l[1], planes)) assembly_put(&a, l, 2);
        } else {
          DlPoint p;
          if ((cmd.opcode == DL_OP_POINT ? dl_command_point(&x, &cmd, k, &p) : dl_command_span(&x, &cmd, k, (DlSpan *)&p)) != DL_OK) continue;
          for (uint32_t v = p.wFirst; v < (uint32_t)p.wFirst + p.wCount; ++v)
            if (!dev->tl_clip[v].clip) assembly_put(&a, &dev->tl_clip[v], 1);
        }
      }
      if (!pick && a.count && !a.failed) {
        IDirect3DDevice9 *d9 = dev->dd3->dev;
        IDirect3DVertexDeclaration9 *decl = d3_decl(dev->dd3, D3DFVF_TLVERTEX);
        const UINT prims = dl_primitive_count(out_type, a.count);
        if (decl && prims) {
          D3D_CALL(d9, SetVertexDeclaration, decl);
          if (dev->st.rs[D3DRENDERSTATE_ZVISIBLE]) {
            if (!dev->occlusion) D3D_CALL(d9, CreateQuery, D3DQUERYTYPE_OCCLUSION, &dev->occlusion);
            zvisible = TRUE;
          }
          if (zvisible && dev->occlusion) D3D_CALL(dev->occlusion, Issue, 2);
          const BYTE *drawn = transformed_z_clamped(dev, 2, a.out, a.count, DL_VERTEX_SIZE);
          if (drawn) D3D_CALL(d9, DrawPrimitiveUP, out_type, prims, drawn, DL_VERTEX_SIZE);
          if (zvisible && dev->occlusion) {
            D3D_CALL(dev->occlusion, Issue, 1);
            DWORD visible = 0;
            while (D3D_CALL(dev->occlusion, GetData, &visible, sizeof visible, 1 /* D3DGETDATA_FLUSH */) == S_FALSE) {}
            if (visible) eb->data.dsStatus.dwStatus &= ~(DWORD)D3DSTATUS_ZNOTVISIBLE;
          }
          d3_drawn(dev);
          if (cmd.opcode == DL_OP_TRIANGLE) dev->stats.dwTrianglesDrawn += cmd.count;
          else if (cmd.opcode == DL_OP_LINE) dev->stats.dwLinesDrawn += cmd.count;
          else if (cmd.opcode == DL_OP_SPAN) dev->stats.dwSpansDrawn += cmd.count;
          else dev->stats.dwPointsDrawn += cmd.count;
        }
      }
      mem_free(a.out);
      break;
    }
    case DL_OP_MATRIXLOAD:
      for (uint32_t k = 0; k < cmd.count; ++k) {
        DlMatrixLoad ml;
        if (dl_command_matrix_load(&x, &cmd, k, &ml) != DL_OK) continue;
        DlMatrix *d = handle_get(&g_handles, ml.hDestMatrix, H_MATRIX), *s = handle_get(&g_handles, ml.hSrcMatrix, H_MATRIX);
        if (d && s) *d = *s;
      }
      break;
    case DL_OP_MATRIXMULTIPLY:
      for (uint32_t k = 0; k < cmd.count; ++k) {
        DlMatrixMultiply mm;
        if (dl_command_matrix_multiply(&x, &cmd, k, &mm) != DL_OK) continue;
        DlMatrix *d = handle_get(&g_handles, mm.hDestMatrix, H_MATRIX), *s1 = handle_get(&g_handles, mm.hSrcMatrix1, H_MATRIX),
                 *s2 = handle_get(&g_handles, mm.hSrcMatrix2, H_MATRIX);
        if (d && s1 && s2) { DlMatrix m; dl_matrix_multiply(&m, s1, s2); *d = m; }
      }
      break;
    case DL_OP_STATETRANSFORM:
      for (uint32_t k = 0; k < cmd.count; ++k) {
        DlState st;
        if (dl_command_state(&x, &cmd, k, &st) != DL_OK) continue;
        const DlMatrix *m = handle_get(&g_handles, st.arg, H_MATRIX);
        if (!m || !dl_transform_state(1, st.type)) continue;
        d3_set_xf(dev, st.type, m);
        dev->matrix_handle[st.type] = st.arg;
      }
      break;
    case DL_OP_STATELIGHT:
      for (uint32_t k = 0; k < cmd.count; ++k) {
        DlState st;
        if (dl_command_state(&x, &cmd, k, &st) == DL_OK) dv_set_light_state(dev, st.type, st.arg);
      }
      break;
    case DL_OP_STATERENDER:
      for (uint32_t k = 0; k < cmd.count; ++k) {
        DlState st;
        if (dl_command_state(&x, &cmd, k, &st) == DL_OK) dv_set_render_state(dev, st.type, st.arg);
      }
      if (!pick) d3_flush(dev);
      break;
    case DL_OP_PROCESSVERTICES: {
      DlTransform t;
      d3_transform(dev, &t);
      DlLighting l;
      d3_lighting(dev, &l);
      for (uint32_t k = 0; k < cmd.count; ++k) {
        DlProcessVertices pv;
        if (dl_command_process_vertices(&x, &cmd, k, &pv) != DL_OK) continue;
        dl_execute_process(&pv, buffer, &eb->data, dl_bytes(dev->tl, (SIZE_T)dev->tl_count * DL_VERTEX_SIZE), dev->tl_clip, dev->tl_count, &t, &l,
                           (DlStatus *)&eb->data.dsStatus);
        dev->stats.dwVerticesProcessed += pv.dwCount;
      }
      break;
    }
    case DL_OP_TEXTURELOAD:
      if (pick) break;
      for (uint32_t k = 0; k < cmd.count; ++k) {
        DlTextureLoad tl_rec;
        if (dl_command_texture_load(&x, &cmd, k, &tl_rec) != DL_OK) continue;
        Surface *d = handle_get(&g_handles, tl_rec.hDestTexture, H_TEXTURE), *s = handle_get(&g_handles, tl_rec.hSrcTexture, H_TEXTURE);
        if (d && s) texture_load(d, s);
      }
      break;
    default: break;
    }
    if (!pick && cmd.opcode != DL_OP_PROCESSVERTICES && cmd.opcode != DL_OP_MATRIXLOAD && cmd.opcode != DL_OP_MATRIXMULTIPLY) d3_flush(dev);
  }
  if (r == DL_NEXT_ERROR) return x.error;
  return D3D_OK;
}
static HRESULT d3_execute(D3dDevice *dev, D3dExecuteBuffer *eb) { return d3_run(dev, eb, NULL); }
static HRESULT d3_pick(D3dDevice *dev, D3dExecuteBuffer *eb, D3dViewport *vp, const D3DRECT *rect) {
  HRESULT hr = dv_set_current_viewport(dev, vp);
  if (FAILED(hr)) return hr;
  return d3_run(dev, eb, rect);
}

/* ---- vertex buffers ------------------------------------------------------------------------------------------------------------------------------- */

static const IDirect3DVertexBuffer7Vtbl vb7_vtbl;
static const IDirect3DVertexBufferVtbl vb3_vtbl;
static D3dVertexBuffer *vb_from(const void *iface) {
  if (!iface) return NULL;
  const void *vt = *(const void *const *)iface;
  return vt == &vb7_vtbl || vt == &vb3_vtbl ? (D3dVertexBuffer *)iface : NULL;
}
#define VB(i) ((D3dVertexBuffer *)(i))

static HRESULT vb_query(D3dVertexBuffer *vb, REFIID riid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  if (guid_eq(riid, &IID_IUnknown) || (vb->version == 7 && guid_eq(riid, &IID_IDirect3DVertexBuffer7)) || (vb->version == 3 && guid_eq(riid, &IID_IDirect3DVertexBuffer))) {
    InterlockedIncrement(&vb->ref);
    *out = vb;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG vb_release(D3dVertexBuffer *vb) {
  const LONG r = InterlockedDecrement(&vb->ref);
  if (!r) {
    IUnknown *dd = vb->ddraw_iface;
    mem_free(vb->memory);
    mem_free(vb->clip_codes);
    mem_free(vb);
    if (dd) IUnknown_Release(dd);
  }
  return (ULONG)r;
}
/* Lock: the memory of the vertices (DDLOCK_DISCARDCONTENTS and NOOVERWRITE are Direct3D 7's; the memory stays where it is, so both keep it). */
static HRESULT vb_lock(D3dVertexBuffer *vb, DWORD flags, void **data, DWORD *size) {
  if (!data) return DDERR_INVALIDPARAMS;
  if (flags & ~(DWORD)(DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_WRITEONLY | DDLOCK_NOSYSLOCK | DDLOCK_NOOVERWRITE | DDLOCK_DISCARDCONTENTS | DDLOCK_DONOTWAIT)) {
    *data = NULL;
    return DDERR_INVALIDPARAMS;
  }
  dd_lock();
  ++vb->lock_count;
  *data = vb->memory;
  if (size) *size = (DWORD)vb->size;
  dd_unlock();
  return D3D_OK;
}
static HRESULT vb_unlock(D3dVertexBuffer *vb) {
  dd_lock();
  const HRESULT hr = vb->lock_count ? D3D_OK : D3DERR_VERTEXBUFFERUNLOCKFAILED;
  if (vb->lock_count) --vb->lock_count;
  dd_unlock();
  return hr;
}
/* Processed vertices written as the destination's FVF code (XYZRHW and what else it has). */
static void vb_write_processed(D3dVertexBuffer *dst, DWORD at, const DlTransform *t, const DlClipVertex *cv, DWORD count, BOOL copy_colours) {
  DlFvfLayout l;
  if (!dl_fvf_layout(dst->desc.dwFVF, &l)) return;
  const DlBytes out = dl_bytes(dst->memory, dst->size);
  for (DWORD k = 0; k < count; ++k) {
    const SIZE_T base = (SIZE_T)(at + k) * l.stride;
    float s[4];
    dl_to_screen(&t->viewport, cv[k].position, &s[0], &s[1], &s[2], &s[3]);
    /* (a destination without RHW gets it all the same, over the next vertex's first number - Windows does: Wine's test_process_vertices) */
    const SIZE_T room = dst->size - base;
    dl_write(out, base, s, room < 16 ? room : 16);
    if (copy_colours) {
      if (l.diffuse >= 0) { const uint32_t c = dl_color_from_float(cv[k].diffuse); dl_write(out, base + (SIZE_T)l.diffuse, &c, 4); }
      if (l.specular >= 0) { const uint32_t c = dl_color_from_float(cv[k].specular); dl_write(out, base + (SIZE_T)l.specular, &c, 4); }
    }
    for (unsigned set = 0; set < l.textures; ++set) dl_write(out, base + (SIZE_T)l.tex[set], cv[k].tex[set], 4 * l.tex_size[set]);
    if (dst->clip_codes) dst->clip_codes[at + k] = cv[k].clip;
  }
}
/* ProcessVertices (and ProcessVerticesStrided): `count` vertices of `fvf` through the device's transform (and lighting, with D3DVOP_LIGHT) into the
 * destination, which holds transformed vertices. */
static HRESULT vb_process(D3dVertexBuffer *dst, DWORD op, DWORD dst_index, DWORD count, DWORD fvf, const BYTE *src, const BYTE *src_end, D3dDevice *dev, DWORD flags) {
  if (!dev || !(op & D3DVOP_TRANSFORM)) return DDERR_INVALIDPARAMS;
  if (dst_index > dst->desc.dwNumVertices || count > dst->desc.dwNumVertices - dst_index) return DDERR_INVALIDPARAMS;
  if (!count) return D3D_OK;
  if (dst->lock_count) return D3DERR_VERTEXBUFFERLOCKED;
  DEV_READY(dev);
  DlClipVertex *cv = (DlClipVertex *)d3_scratch(dev->dd3, 3, (SIZE_T)count * sizeof *cv);
  if (!cv) return DDERR_OUTOFMEMORY;
  DlTransform t;
  d3_transform(dev, &t);
  /* (Direct3D 7's ProcessVertices lights no normal under a singular world matrix, where its draws use the matrix itself: Wine's test_lighting
   * of ddraw7 holds both of Windows' answers) */
  if (dev->version >= 7 && ffl_singular(t.world.m)) dl_zero(t.normal_matrix, sizeof t.normal_matrix);
  DlLighting l;
  d3_lighting(dev, &l);
  DlProcessResult result;
  dl_result_init(&result);
  DWORD stride = dl_fvf_stride(fvf);
  const BOOL light = (op & D3DVOP_LIGHT) != 0;
  const DWORD blend = dev->version >= 7 ? dev->st.rs[D3DRENDERSTATE_VERTEXBLEND] : 0;
  if (blend >= D3DVBLEND_1WEIGHT && blend <= D3DVBLEND_3WEIGHTS && (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) {
    /* Vertex blending (D3DRENDERSTATE_VERTEXBLEND): the position and normal are the weighted sum of what the world matrices 0 .. n make of them (the last
     * weight is 1 minus the others), then view and projection. The weights follow the position; a vertex without them gives the numbers after its
     * position (Windows: Wine's test_process_vertices). The blended vertices go on as XYZ vertices with an identity world. */
    DlFvfLayout lay;
    dl_fvf_layout(fvf, &lay);
    const DWORD out_fvf = (fvf & ~(DWORD)D3DFVF_POSITION_MASK) | D3DFVF_XYZ;
    DlFvfLayout ol;
    dl_fvf_layout(out_fvf, &ol);
    BYTE *blended = d3_scratch(dev->dd3, 1, (SIZE_T)count * ol.stride);
    if (!blended) return DDERR_OUTOFMEMORY;
    const DlMatrix *world[4] = {&dev->st.xf[D3DTRANSFORMSTATE_WORLD], &dev->st.xf[D3DTRANSFORMSTATE_WORLD1], &dev->st.xf[D3DTRANSFORMSTATE_WORLD2],
                                &dev->st.xf[D3DTRANSFORMSTATE_WORLD3]};
    const SIZE_T src_bytes = src_end > src ? (SIZE_T)(src_end - src) : 0;
    const DlConstBytes in = dl_const_bytes(src, src_bytes);
    for (DWORD k = 0; k < count; ++k) {
      const SIZE_T at = (SIZE_T)k * stride;
      float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, w[4] = {0, 0, 0, 0};
      dl_read(in, at, p, 12);
      for (DWORD j = 0; j < blend; ++j)
        if (!dl_read(in, at + 12 + 4 * j, &w[j], 4)) w[j] = 0.0f;
      float last = 1.0f;
      for (DWORD j = 0; j < blend; ++j) last -= w[j];
      w[blend] = last;
      if (lay.normal >= 0) dl_read(in, at + (SIZE_T)lay.normal, n, 12);
      float bp[3] = {0, 0, 0}, bn[3] = {0, 0, 0};
      for (DWORD j = 0; j <= blend; ++j) {
        float q[4];
        dl_transform_point(world[j], p, q);
        for (int c = 0; c < 3; ++c) {
          bp[c] += w[j] * q[c];
          bn[c] += w[j] * (n[0] * world[j]->m[0][c] + n[1] * world[j]->m[1][c] + n[2] * world[j]->m[2][c]);
        }
      }
      BYTE *o = blended + (SIZE_T)k * ol.stride;
      memcpy(o, bp, 12);
      /* (the rest of the vertex as it is: everything after the position and weights) */
      const SIZE_T rest = stride - lay.position_size;
      if (rest) dl_read(in, at + lay.position_size, o + 12, rest);
      if (ol.normal >= 0) memcpy(o + ol.normal, bn, 12);
    }
    dev->st.xf[0] = dev->st.xf[D3DTRANSFORMSTATE_WORLD];      /* (spare slot: the world is put back below) */
    dl_matrix_identity(&dev->st.xf[D3DTRANSFORMSTATE_WORLD]);
    DlTransform tb;
    d3_transform(dev, &tb);
    dev->st.xf[D3DTRANSFORMSTATE_WORLD] = dev->st.xf[0];
    t = tb;
    fvf = out_fvf;
    stride = ol.stride;
    src = blended;
  }
  if (dl_process_fvf(&t, &l, light, fvf, dl_const_bytes(src, (SIZE_T)count * stride), count, cv, count, &result) != DL_OK) return DDERR_INVALIDPARAMS;
  /* (D3DPV_DONOTCOPYDATA: the destination keeps its colours and texture coordinates where the source has none) */
  vb_write_processed(dst, dst_index, &t, cv, count, !(flags & D3DPV_DONOTCOPYDATA) || (fvf & (D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_NORMAL)));
  if (op & D3DVOP_EXTENTS) clip_status_add(dev, &result, 0);
  return D3D_OK;
}
static HRESULT vb_process_from(D3dVertexBuffer *dst, DWORD op, DWORD dst_index, DWORD count, D3dVertexBuffer *src, DWORD src_index, D3dDevice *dev, DWORD flags) {
  if (!src) return DDERR_INVALIDPARAMS;
  if (src_index > src->desc.dwNumVertices || count > src->desc.dwNumVertices - src_index) return DDERR_INVALIDPARAMS;
  return vb_process(dst, op, dst_index, count, src->desc.dwFVF, src->memory + (SIZE_T)src_index * src->stride, src->memory + src->size, dev, flags);
}
static HRESULT vb_get_desc(D3dVertexBuffer *vb, D3DVERTEXBUFFERDESC *desc) {
  if (!desc) return DDERR_INVALIDPARAMS;
  dd_lock();
  desc->dwCaps = vb->desc.dwCaps | (vb->optimized ? D3DVBCAPS_OPTIMIZED : 0);
  desc->dwFVF = vb->desc.dwFVF;
  desc->dwNumVertices = vb->desc.dwNumVertices;
  dd_unlock();
  return D3D_OK;
}
static HRESULT vb_optimize(D3dVertexBuffer *vb, DWORD flags) {
  if (flags) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = D3D_OK;
  if (vb->lock_count) hr = D3DERR_VERTEXBUFFERLOCKED;
  else if (vb->optimized) hr = D3DERR_VERTEXBUFFEROPTIMIZED;
  else vb->optimized = TRUE;
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE vb7_QueryInterface(IDirect3DVertexBuffer7 *i, REFIID riid, void **out) { return vb_query(VB(i), riid, out); }
static ULONG STDMETHODCALLTYPE vb7_AddRef(IDirect3DVertexBuffer7 *i) { return (ULONG)InterlockedIncrement(&VB(i)->ref); }
static ULONG STDMETHODCALLTYPE vb7_Release(IDirect3DVertexBuffer7 *i) { return vb_release(VB(i)); }
static HRESULT STDMETHODCALLTYPE vb7_Lock(IDirect3DVertexBuffer7 *i, DWORD flags, void **data, DWORD *size) { return vb_lock(VB(i), flags, data, size); }
static HRESULT STDMETHODCALLTYPE vb7_Unlock(IDirect3DVertexBuffer7 *i) { return vb_unlock(VB(i)); }
static HRESULT STDMETHODCALLTYPE vb7_ProcessVertices(IDirect3DVertexBuffer7 *i, DWORD op, DWORD dst_index, DWORD count, IDirect3DVertexBuffer7 *src, DWORD src_index,
                                                      IDirect3DDevice7 *device, DWORD flags) {
  dd_lock();
  const WORD cw = fpu_setup();
  D3dDevice *dev = device ? DEV7(device) : NULL;
  const HRESULT hr = vb_process_from(VB(i), op, dst_index, count, vb_from(src), src_index, dev, flags);
  fpu_set(cw);
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE vb7_GetVertexBufferDesc(IDirect3DVertexBuffer7 *i, D3DVERTEXBUFFERDESC *desc) { return vb_get_desc(VB(i), desc); }
static HRESULT STDMETHODCALLTYPE vb7_Optimize(IDirect3DVertexBuffer7 *i, IDirect3DDevice7 *device, DWORD flags) { (void)device; return vb_optimize(VB(i), flags); }
static HRESULT STDMETHODCALLTYPE vb7_ProcessVerticesStrided(IDirect3DVertexBuffer7 *i, DWORD op, DWORD dst_index, DWORD count, D3DDRAWPRIMITIVESTRIDEDDATA *data,
                                                             DWORD fvf, IDirect3DDevice7 *device, DWORD flags) {
  if (!data || !device || !dl_fvf_valid(fvf, 7)) return DDERR_INVALIDPARAMS;
  dd_lock();
  const WORD cw = fpu_setup();
  D3dDevice *dev = DEV7(device);
  HRESULT hr = DDERR_GENERIC;
  const DWORD stride = dl_fvf_stride(fvf);
  BYTE *buf = dev->dd3 ? d3_scratch(dev->dd3, 2, (SIZE_T)(count ? count : 1) * stride) : NULL;
  if (dev->dd3 && !buf) hr = DDERR_OUTOFMEMORY;
  else if (buf) {
    hr = dl_gather_strided(fvf, (const DlStridedData *)data, 0, count, count, dl_bytes(buf, (SIZE_T)count * stride));
    if (hr == DL_OK) hr = vb_process(VB(i), op, dst_index, count, fvf, buf, buf + (SIZE_T)count * stride, dev, flags);
  }
  fpu_set(cw);
  dd_unlock();
  return hr;
}
static const IDirect3DVertexBuffer7Vtbl vb7_vtbl = {vb7_QueryInterface, vb7_AddRef, vb7_Release, vb7_Lock, vb7_Unlock, vb7_ProcessVertices,
                                                    vb7_GetVertexBufferDesc, vb7_Optimize, vb7_ProcessVerticesStrided};
static HRESULT STDMETHODCALLTYPE vb3_QueryInterface(IDirect3DVertexBuffer *i, REFIID riid, void **out) { return vb_query(VB(i), riid, out); }
static ULONG STDMETHODCALLTYPE vb3_AddRef(IDirect3DVertexBuffer *i) { return (ULONG)InterlockedIncrement(&VB(i)->ref); }
static ULONG STDMETHODCALLTYPE vb3_Release(IDirect3DVertexBuffer *i) { return vb_release(VB(i)); }
static HRESULT STDMETHODCALLTYPE vb3_Lock(IDirect3DVertexBuffer *i, DWORD flags, void **data, DWORD *size) {
  return vb_lock(VB(i), flags & ~(DWORD)(DDLOCK_NOOVERWRITE | DDLOCK_DISCARDCONTENTS), data, size);
}
static HRESULT STDMETHODCALLTYPE vb3_Unlock(IDirect3DVertexBuffer *i) { return vb_unlock(VB(i)); }
static HRESULT STDMETHODCALLTYPE vb3_ProcessVertices(IDirect3DVertexBuffer *i, DWORD op, DWORD dst_index, DWORD count, IDirect3DVertexBuffer *src, DWORD src_index,
                                                      IDirect3DDevice3 *device, DWORD flags) {
  dd_lock();
  D3dDevice *dev = device ? DEV3(device) : NULL;
  const HRESULT hr = vb_process_from(VB(i), op, dst_index, count, vb_from(src), src_index, dev, flags);
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE vb3_GetVertexBufferDesc(IDirect3DVertexBuffer *i, D3DVERTEXBUFFERDESC *desc) { return vb_get_desc(VB(i), desc); }
static HRESULT STDMETHODCALLTYPE vb3_Optimize(IDirect3DVertexBuffer *i, IDirect3DDevice3 *device, DWORD flags) { (void)device; return vb_optimize(VB(i), flags); }
static const IDirect3DVertexBufferVtbl vb3_vtbl = {vb3_QueryInterface, vb3_AddRef, vb3_Release, vb3_Lock, vb3_Unlock, vb3_ProcessVertices, vb3_GetVertexBufferDesc,
                                                   vb3_Optimize};

static HRESULT vb_create(DDraw *dd, UINT version, const D3DVERTEXBUFFERDESC *desc, void **out) {
  if (!desc || !out) return DDERR_INVALIDPARAMS;
  if (desc->dwSize < sizeof(D3DVERTEXBUFFERDESC)) return DDERR_INVALIDPARAMS;
  if (!dl_fvf_valid(desc->dwFVF, version)) return DDERR_INVALIDPARAMS;
  if (desc->dwCaps & ~(DWORD)(D3DVBCAPS_SYSTEMMEMORY | D3DVBCAPS_WRITEONLY | D3DVBCAPS_OPTIMIZED | D3DVBCAPS_DONOTCLIP)) return DDERR_INVALIDPARAMS;
  const DWORD stride = dl_fvf_stride(desc->dwFVF);
  if (!desc->dwNumVertices || (UINT64)desc->dwNumVertices * stride > 0x10000000u) return DDERR_INVALIDPARAMS;
  D3dVertexBuffer *vb = mem_alloc(sizeof *vb);
  if (!vb) return DDERR_OUTOFMEMORY;
  vb->vt7 = version == 7 ? &vb7_vtbl : (const IDirect3DVertexBuffer7Vtbl *)(const void *)&vb3_vtbl;
  vb->ref = 1;
  vb->version = version;
  vb->dd = dd;
  vb->desc = *desc;
  vb->desc.dwCaps &= ~(DWORD)D3DVBCAPS_OPTIMIZED;
  vb->optimized = (desc->dwCaps & D3DVBCAPS_OPTIMIZED) != 0;
  vb->stride = stride;
  vb->size = (SIZE_T)desc->dwNumVertices * stride;
  vb->memory = mem_alloc(vb->size);
  if (!(desc->dwCaps & D3DVBCAPS_DONOTCLIP)) vb->clip_codes = mem_alloc((SIZE_T)desc->dwNumVertices * sizeof(DWORD));
  if (!vb->memory || (!(desc->dwCaps & D3DVBCAPS_DONOTCLIP) && !vb->clip_codes)) { mem_free(vb->memory); mem_free(vb->clip_codes); mem_free(vb); return DDERR_OUTOFMEMORY; }
  /* (a vertex buffer of IDirect3D7 holds a reference of IDirectDraw7, Wine's test_vb_refcount; one of IDirect3D3 none) */
  if (version == 7) { vb->ddraw_iface = (IUnknown *)DDI7(dd); IUnknown_AddRef(vb->ddraw_iface); }
  *out = vb;
  return D3D_OK;
}

/* ---- textures ------------------------------------------------------------------------------------------------------------------------------------- */

#define TEX2(i) D3D_IMPL(Surface, vt_tex2, i)
#define TEX1(i) D3D_IMPL(Surface, vt_tex1, i)
static const IDirect3DTexture2Vtbl tex2_vtbl;
static const IDirect3DTextureVtbl tex1_vtbl;
static Surface *surface_of_texture2(const void *iface) { return iface && *(const void *const *)iface == &tex2_vtbl ? TEX2(iface) : NULL; }
static Surface *surface_of_texture1(const void *iface) { return iface && *(const void *const *)iface == &tex1_vtbl ? TEX1(iface) : NULL; }

static HRESULT texture_handle(Surface *s, D3DTEXTUREHANDLE *out) {
  if (!out) return DDERR_INVALIDPARAMS;
  dd_lock();
  D3dSurf *ds = d3_surf(s);
  if (ds && !ds->handle) ds->handle = handle_alloc(&g_handles, H_TEXTURE, s, NULL);
  const DWORD h = ds ? ds->handle : 0;
  dd_unlock();
  if (!h) return DDERR_OUTOFMEMORY;
  *out = h;
  return D3D_OK;
}
/* Load (IDirect3DTexture(2), and the execute buffer's TEXTURELOAD): the source's pixels, palette entries and source colour key into the destination,
 * level by level (the sizes must match: D3DERR_TEXTURE_LOAD_FAILED). */
static HRESULT texture_load(Surface *dst, Surface *src) {
  if (dst == src) return D3D_OK;
  dd_lock();
  HRESULT hr = D3D_OK;
  Surface *d = dst, *s = src;
  while (d && s) {
    d->desc.ddsCaps.dwCaps &= ~(DWORD)DDSCAPS_ALLOCONLOAD;
    if (s->palette) {
      if (!d->palette) { hr = DDERR_NOPALETTEATTACHED; break; }
      memcpy(d->palette->entries, s->palette->entries, sizeof d->palette->entries);
      surfaces_palette_changed(d->palette);
    }
    if (s->desc.dwWidth != d->desc.dwWidth || s->desc.dwHeight != d->desc.dwHeight) { hr = D3DERR_TEXTURE_LOAD_FAILED; break; }
    if (s->desc.dwFlags & DDSD_CKSRCBLT) { d->desc.dwFlags |= DDSD_CKSRCBLT; d->desc.ddckCKSrcBlt = s->desc.ddckCKSrcBlt; }
    if (s->fmt == d->fmt) {
      cpu_access(s, NULL);
      cpu_access(d, NULL);
      const SIZE_T n = s->store.size < d->store.size ? s->store.size : d->store.size;
      memcpy(d->store.mem, s->store.mem, n);
      cpu_wrote(d, NULL);
    } else {
      const RECT r = {0, 0, (LONG)s->desc.dwWidth, (LONG)s->desc.dwHeight};
      if (FAILED(blit_rect(d, &r, s, &r, NULL))) { hr = D3DERR_TEXTURE_LOAD_FAILED; break; }
    }
    s = (s->desc.ddsCaps.dwCaps & DDSCAPS_MIPMAP) ? next_level(s) : NULL;
    d = (d->desc.ddsCaps.dwCaps & DDSCAPS_MIPMAP) ? next_level(d) : NULL;
  }
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE tex2_QueryInterface(IDirect3DTexture2 *i, REFIID riid, void **out) { return IUnknown_QueryInterface(TEX2(i)->texture_outer, riid, out); }
static ULONG STDMETHODCALLTYPE tex2_AddRef(IDirect3DTexture2 *i) { return IUnknown_AddRef(TEX2(i)->texture_outer); }
static ULONG STDMETHODCALLTYPE tex2_Release(IDirect3DTexture2 *i) { return IUnknown_Release(TEX2(i)->texture_outer); }
static HRESULT STDMETHODCALLTYPE tex2_GetHandle(IDirect3DTexture2 *i, IDirect3DDevice2 *dev, D3DTEXTUREHANDLE *h) { (void)dev; return texture_handle(TEX2(i), h); }
/* PaletteChanged: the palette's entries from `start` changed (the texture's copy is made again at its next use). */
static HRESULT texture_palette_changed(Surface *s, DWORD start, DWORD count) {
  (void)start; (void)count;
  dd_lock();
  cpu_wrote(s, NULL);
  dd_unlock();
  return D3D_OK;
}
static HRESULT STDMETHODCALLTYPE tex2_PaletteChanged(IDirect3DTexture2 *i, DWORD start, DWORD count) { return texture_palette_changed(TEX2(i), start, count); }
static HRESULT STDMETHODCALLTYPE tex2_Load(IDirect3DTexture2 *i, IDirect3DTexture2 *src) {
  Surface *s = surface_of_texture2(src);
  if (!s) return DDERR_INVALIDPARAMS;
  return texture_load(TEX2(i), s);
}
static const IDirect3DTexture2Vtbl tex2_vtbl = {tex2_QueryInterface, tex2_AddRef, tex2_Release, tex2_GetHandle, tex2_PaletteChanged, tex2_Load};
static HRESULT STDMETHODCALLTYPE tex1_QueryInterface(IDirect3DTexture *i, REFIID riid, void **out) { return IUnknown_QueryInterface(TEX1(i)->texture_outer, riid, out); }
static ULONG STDMETHODCALLTYPE tex1_AddRef(IDirect3DTexture *i) { return IUnknown_AddRef(TEX1(i)->texture_outer); }
static ULONG STDMETHODCALLTYPE tex1_Release(IDirect3DTexture *i) { return IUnknown_Release(TEX1(i)->texture_outer); }
static HRESULT STDMETHODCALLTYPE tex1_Initialize(IDirect3DTexture *i, IDirect3DDevice *dev, IDirectDrawSurface *s) { (void)i; (void)dev; (void)s; return DDERR_ALREADYINITIALIZED; }
static HRESULT STDMETHODCALLTYPE tex1_GetHandle(IDirect3DTexture *i, IDirect3DDevice *dev, D3DTEXTUREHANDLE *h) { (void)dev; return texture_handle(TEX1(i), h); }
static HRESULT STDMETHODCALLTYPE tex1_PaletteChanged(IDirect3DTexture *i, DWORD start, DWORD count) { return texture_palette_changed(TEX1(i), start, count); }
static HRESULT STDMETHODCALLTYPE tex1_Load(IDirect3DTexture *i, IDirect3DTexture *src) {
  Surface *s = surface_of_texture1(src);
  if (!s) return DDERR_INVALIDPARAMS;
  return texture_load(TEX1(i), s);
}
/* Unload: the Direct3D copy of the texture goes (made again at its next use). */
static HRESULT STDMETHODCALLTYPE tex1_Unload(IDirect3DTexture *i) {
  Surface *s = TEX1(i);
  dd_lock();
  D3dRes *r = s->store.shadow.resource;
  if (r && r->kind == RES_TEXTURE && !r->target_texture) {
    D3dDD *d = r->owner;
    for (D3dDevice *dev = d ? d->devices : NULL; dev; dev = dev->next)
      for (UINT st = 0; st < D3D_STAGES; ++st)
        if (dev->bound_res[st] == r) {
          if (d->active == dev) D3D_CALL(d->dev, SetTexture, st, NULL);
          dev->bound_res[st] = NULL;
        }
    res_free(r);
    s->store.shadow.resource = NULL;
    if (d) ++d->tex_stamp;
  }
  dd_unlock();
  return D3D_OK;
}
static const IDirect3DTextureVtbl tex1_vtbl = {tex1_QueryInterface, tex1_AddRef, tex1_Release, tex1_Initialize, tex1_GetHandle, tex1_PaletteChanged, tex1_Load, tex1_Unload};
