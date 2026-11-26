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
/* The Direct3D devices (d3d.inc): one object answers IDirect3DDevice7, or IDirect3DDevice3, IDirect3DDevice2 and IDirect3DDevice as the version it was
 * made by allows (Windows' QueryInterface rules, Wine's tests). Its references are counted on `outer`: the surface it was made from for a device of
 * Direct3D 1 (QueryInterface of a surface makes it, aggregated), its own inner IUnknown otherwise. The device holds a reference of its render target's
 * interface (not in Direct3D 1). */

static D3dViewport *viewport_from(const void *iface);
static D3dMaterial *material_from(const void *iface);
static void viewport_activate(D3dViewport *vp);
static HRESULT d3_viewport_delete(D3dDevice *dev, D3dViewport *vp);

/* ---- objects ---------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT dv_query(D3dDevice *dev, REFIID riid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  if (!riid) return DDERR_INVALIDPARAMS;
  void *iface = NULL;
  if (guid_eq(riid, &IID_IUnknown)) iface = DEVI7(dev);
  else if (dev->version == 7) { if (guid_eq(riid, &IID_IDirect3DDevice7)) iface = DEVI7(dev); }
  else if (guid_eq(riid, &IID_IDirect3DDevice3) && dev->version == 3) iface = DEVI3(dev);
  else if (guid_eq(riid, &IID_IDirect3DDevice2) && dev->version >= 2) iface = DEVI2(dev);
  else if (guid_eq(riid, &IID_IDirect3DDevice)) iface = DEVI1(dev);
  if (!iface) return E_NOINTERFACE;
  IUnknown_AddRef(dev->outer);
  *out = iface;
  return S_OK;
}

static void dv_free_handles(D3dDevice *dev) {
  for (UINT i = 0; i < dev->blocks.count; ++i) {
    D3dStateBlock *b = handle_free(&dev->blocks, i + 1, H_STATEBLOCK);
    block_free(b);
  }
  mem_free(dev->blocks.entries);
  zero(&dev->blocks, sizeof dev->blocks);
  /* (the matrices it made) */
  for (UINT i = 0; i < g_handles.count; ++i) {
    D3dHandle *e = &g_handles.entries[i];
    if (e->type == H_MATRIX && e->owner == dev) {
      mem_free(e->object);
      e->type = H_FREE;
      e->object = NULL;
      e->owner = NULL;
    }
  }
}

/* The device lets go of its DirectDraw object's Direct3D 9 device (it goes, or the object goes before it). */
static void d3_device_detach(D3dDevice *dev) {
  D3dDD *d = dev->dd3;
  if (!d) return;
  for (D3dDevice **p = &d->devices; *p; p = &(*p)->next)
    if (*p == dev) { *p = dev->next; break; }
  if (d->active == dev) d->active = NULL;
  if (dev->occlusion) { D3D_CALL(dev->occlusion, Release); dev->occlusion = NULL; }
  dev->dd3 = NULL;
  for (UINT st = 0; st < D3D_STAGES; ++st) dev->bound_res[st] = NULL;
}

static void dv_destroy(D3dDevice *dev) {
  dd_lock();
  while (dev->viewports) d3_viewport_delete(dev, dev->viewports);
  dv_free_handles(dev);
  block_free(dev->recording);
  dev->recording = NULL;
  d3_device_detach(dev);
  mem_free(dev->st.lights);
  mem_free(dev->dirty.lights);
  mem_free(dev->begin_vertices);
  mem_free(dev->begin_indices);
  mem_free(dev->tl);
  mem_free(dev->tl_clip);
  mem_free(dev->picks);
  IUnknown *rt = dev->rt_iface;
  const UINT version = dev->version;
  dev->rt_iface = NULL;
  dev->target = NULL;
  mem_free(dev);
  dd_unlock();
  if (version != 1 && rt) IUnknown_Release(rt);
}

static HRESULT STDMETHODCALLTYPE inner_QueryInterface(IUnknown *i, REFIID riid, void **out) { return dv_query(D3D_IMPL(D3dDevice, vt_inner, i), riid, out); }
static ULONG STDMETHODCALLTYPE inner_AddRef(IUnknown *i) { return (ULONG)InterlockedIncrement(&D3D_IMPL(D3dDevice, vt_inner, i)->ref); }
static ULONG STDMETHODCALLTYPE inner_Release(IUnknown *i) {
  D3dDevice *dev = D3D_IMPL(D3dDevice, vt_inner, i);
  const LONG r = InterlockedDecrement(&dev->ref);
  if (!r) dv_destroy(dev);
  return (ULONG)r;
}
static const IUnknownVtbl inner_vtbl = {inner_QueryInterface, inner_AddRef, inner_Release};

/* Makes a device of `version` drawing into `target` (its interface `rt_iface`); `outer` the aggregating surface of Direct3D 1. */
static const IDirect3DDevice7Vtbl dev7_vtbl;
static const IDirect3DDevice3Vtbl dev3_vtbl;
static const IDirect3DDevice2Vtbl dev2_vtbl;
static const IDirect3DDeviceVtbl dev1_vtbl;
static HRESULT d3_device_create(DDraw *dd, REFCLSID guid, Surface *target, IUnknown *rt_iface, UINT version, IUnknown *outer, D3dDevice **out) {
  *out = NULL;
  if (!target) return DDERR_INVALIDPARAMS;
  const DWORD caps = target->desc.ddsCaps.dwCaps;
  if (!(caps & DDSCAPS_3DDEVICE) || (caps & DDSCAPS_ZBUFFER)) return DDERR_INVALIDCAPS;
  if (dd_format_is_palette(target->fmt) && !target->palette) return DDERR_NOPALETTEATTACHED;
  const int kind = guid ? dl_device_kind((const DlGuid *)guid) : DL_DEVICE_NONE;
  if (!dl_device_creatable(version, kind)) return version == 1 ? E_NOINTERFACE : DDERR_INVALIDPARAMS;
  if ((kind == DL_DEVICE_HAL || kind == DL_DEVICE_TNLHAL) && !(caps & DDSCAPS_VIDEOMEMORY)) return D3DERR_SURFACENOTINVIDMEM;
  D3dDD *d = d3_dd(dd);
  if (!d) return DDERR_OUTOFMEMORY;      /* (no Direct3D 9: what Wine answers without 3D) */
  D3dDevice *dev = mem_alloc(sizeof *dev);
  if (!dev) return DDERR_OUTOFMEMORY;
  dev->vt7 = &dev7_vtbl;
  dev->vt3 = &dev3_vtbl;
  dev->vt2 = &dev2_vtbl;
  dev->vt1 = &dev1_vtbl;
  dev->vt_inner = &inner_vtbl;
  dev->ref = 1;
  dev->version = version;
  dev->outer = outer ? outer : (IUnknown *)&dev->vt_inner;
  dev->dd = dd;
  dev->dd3 = d;
  dev->kind = kind;
  dev->target = target;
  dev->rt_iface = rt_iface;
  d3_state_defaults(dev);
  dev->st.rs[D3DRENDERSTATE_ZENABLE] = attached_depth(target) ? D3DZB_TRUE : D3DZB_FALSE;
  if (version != 1) IUnknown_AddRef(rt_iface);
  dev->next = d->devices;
  d->devices = dev;
  /* (without DDSCL_FPUPRESERVE the FPU is set up for Direct3D: single precision, exceptions masked) */
  dev->fpu_preserve = (dd->coop & DDSCL_FPUPRESERVE) != 0;
  if (!dev->fpu_preserve) fpu_setup();
  *out = dev;
  return D3D_OK;
}

/* The device's Direct3D 9 device (DDERR_GENERIC: its DirectDraw object is gone). */
#define DEV_READY(dev) do { if (!(dev)->dd3 || !(dev)->dd3->dev) return DDERR_GENERIC; } while (0)

/* ---- the methods every version has ------------------------------------------------------------------------------------------------------------------ */

static HRESULT dv_get_caps_legacy(D3dDevice *dev, D3DDEVICEDESC *hal, D3DDEVICEDESC *hel) {
  if (!hal || !hel) return DDERR_INVALIDPARAMS;
  const DWORD hs = hal->dwSize, es = hel->dwSize;
  if ((hs != DL_DEVICEDESC_V1_SIZE && hs != DL_DEVICEDESC_V2_SIZE && hs != DL_DEVICEDESC_V3_SIZE) ||
      (es != DL_DEVICEDESC_V1_SIZE && es != DL_DEVICEDESC_V2_SIZE && es != DL_DEVICEDESC_V3_SIZE))
    return DDERR_INVALIDPARAMS;
  DEV_READY(dev);
  DlDeviceDesc h, e;
  dl_device_descs(&dev->dd3->caps, dev->kind, 3, &h, &e);
  memcpy(hal, &h, hs);
  hal->dwSize = hs;
  memcpy(hel, &e, es);
  hel->dwSize = es;
  return D3D_OK;
}

static HRESULT dv_get_stats(D3dDevice *dev, D3DSTATS *stats) {
  if (!stats) return DDERR_INVALIDPARAMS;
  const DWORD size = stats->dwSize;
  if (size != sizeof(D3DSTATS)) return DDERR_INVALIDPARAMS;
  *stats = dev->stats;
  stats->dwSize = size;
  return D3D_OK;
}

/* The texture formats of EnumTextureFormats (Wine's list; Direct3D 1 and 2 without the FourCC and bump formats). */
static HRESULT dv_enum_texture_formats(D3dDevice *dev, void *callback, void *context, BOOL desc_form) {
  if (!callback) return DDERR_INVALIDPARAMS;
  static const UINT formats[] = {DD_FMT_X1R5G5B5, DD_FMT_A1R5G5B5, DD_FMT_A4R4G4B4, DD_FMT_R5G6B5, DD_FMT_X8R8G8B8, DD_FMT_A8R8G8B8, DD_FMT_R3G3B2, DD_FMT_P8,
                                 DD_FMT_DXT1, DD_FMT_DXT2, DD_FMT_DXT3, DD_FMT_DXT4, DD_FMT_DXT5, DD_FMT_V8U8, DD_FMT_L6V5U5, DD_FMT_X8L8V8U8};
  const UINT count = dev->version >= 3 ? sizeof formats / sizeof formats[0] : 8;
  for (UINT i = 0; i < count; ++i) {
    HRESULT r;
    if (desc_form) {
      DDSURFACEDESC sd;
      zero(&sd, sizeof sd);
      sd.dwSize = sizeof sd;
      sd.dwFlags = DDSD_PIXELFORMAT | DDSD_CAPS;
      sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
      ddpf_from_format(formats[i], &sd.ddpfPixelFormat);
      r = ((LPD3DENUMTEXTUREFORMATSCALLBACK)callback)(&sd, context);
    } else {
      DDPIXELFORMAT pf;
      ddpf_from_format(formats[i], &pf);
      r = ((LPD3DENUMPIXELFORMATSCALLBACK)callback)(&pf, context);
    }
    if (r != DDENUMRET_OK) break;
  }
  return D3D_OK;
}

static HRESULT dv_begin_scene(D3dDevice *dev) {
  if (dev->in_scene) return D3DERR_SCENE_IN_SCENE;
  dev->in_scene = TRUE;
  return D3D_OK;
}
static HRESULT dv_end_scene(D3dDevice *dev) {
  if (!dev->in_scene) return D3DERR_SCENE_NOT_IN_SCENE;
  dev->in_scene = FALSE;
  /* (a scene drawn straight into the primary surface goes to the screen at its end: the CPU's presentation of the primary does not see the GPU's
   * draws otherwise; a flipping chain is shown by Flip) */
  if (dev->target && (dev->target->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) && (dev->target->store.shadow.state & DD_SHADOW_GPU_NEWER))
    present_primary(dev->target, NULL);
  return D3D_OK;
}

static HRESULT dv_get_direct3d(D3dDevice *dev, UINT version, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  DDraw *dd = dev->dd;
  void *iface = version == 7 ? (void *)&dd->vt_d3d7 : version == 3 ? (void *)&dd->vt_d3d3 : version == 2 ? (void *)&dd->vt_d3d2 : (void *)&dd->vt_d3d1;
  IUnknown_AddRef((IUnknown *)iface);
  *out = iface;
  return D3D_OK;
}

/* SetRenderTarget (Wine's order of checks, which its tests hold against Windows: a depth buffer is taken and refused, a system memory target of a
 * hardware device is taken and ignored by Direct3D 2 and 3). */
static HRESULT dv_set_render_target(D3dDevice *dev, Surface *target, IUnknown *iface, UINT version) {
  if (!target) return DDERR_INVALIDPARAMS;
  if (dd_format_is_palette(target->fmt) && !target->palette) return DDERR_INVALIDCAPS;
  const DWORD caps = target->desc.ddsCaps.dwCaps;
  if (!(caps & DDSCAPS_3DDEVICE)) return DDERR_INVALIDCAPS;
  const BOOL hardware = dev->kind == DL_DEVICE_HAL || dev->kind == DL_DEVICE_TNLHAL;
  if (version == 7 && hardware && !(caps & DDSCAPS_VIDEOMEMORY)) return DDERR_INVALIDPARAMS;
  if (caps & DDSCAPS_ZBUFFER) {
    if (version != 2) IUnknown_AddRef(iface);
    IUnknown *old = dev->rt_iface;
    dev->rt_iface = iface;
    dev->target = NULL;
    if (old) IUnknown_Release(old);
    return DDERR_INVALIDPIXELFORMAT;
  }
  if (hardware && !(caps & DDSCAPS_VIDEOMEMORY)) {
    IUnknown_AddRef(iface);
    IUnknown *old = dev->rt_iface;
    dev->rt_iface = iface;
    dev->target = NULL;
    if (old) IUnknown_Release(old);
    return D3D_OK;
  }
  if (dev->rt_iface == iface) return D3D_OK;
  IUnknown_AddRef(iface);
  IUnknown *old = dev->rt_iface;
  dev->rt_iface = iface;
  dev->target = target;      /* (ZENABLE stays as the program set it: Wine binds the new target's depth buffer and leaves the state, test_viewport) */
  if (old) IUnknown_Release(old);
  return D3D_OK;
}
static HRESULT dv_get_render_target(D3dDevice *dev, REFIID iid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  if (!dev->rt_iface) { *out = NULL; return DDERR_NOTFOUND; }
  return IUnknown_QueryInterface(dev->rt_iface, iid, out);
}

/* GetRenderState / SetRenderState of Direct3D 2, 3 and 7. */
static HRESULT dv_get_render_state(D3dDevice *dev, DWORD state, DWORD *value) {
  if (!value) return DDERR_INVALIDPARAMS;
  if (dev->version >= 7) {
    if (state == D3DRENDERSTATE_TEXTUREHANDLE || state == D3DRENDERSTATE_TEXTUREMAPBLEND || !state || state >= 256) return DDERR_INVALIDPARAMS;
    *value = rs_sampler_value(dev, state);
    return D3D_OK;
  }
  if (!state || state >= 256) return DDERR_INVALIDPARAMS;
  if (dl_render_state_get_override(dev->version, state, (uint32_t *)value)) return D3D_OK;
  switch (state) {
  case D3DRENDERSTATE_TEXTUREHANDLE: {
    Surface *t = dev->st.texture[0];
    *value = t && t->d3d ? ((D3dSurf *)t->d3d)->handle : 0;
    return D3D_OK;
  }
  case D3DRENDERSTATE_TEXTUREMAPBLEND: *value = dev->texture_map_blend; return D3D_OK;
  default: *value = rs_sampler_value(dev, state); return D3D_OK;
  }
}
static HRESULT dv_set_render_state(D3dDevice *dev, DWORD state, DWORD value) {
  DlStateContext ctx;
  zero(&ctx, sizeof ctx);
  ctx.wrap0 = dev->st.rs[D3DRENDERSTATE_WRAP0];
  DlStateOps ops;
  const HRESULT check = dl_render_state(dev->version, state, value, &ctx, &ops);
  if (check != DL_OK) return check;
  switch (state) {
  case D3DRENDERSTATE_TEXTUREHANDLE: {
    Surface *t = NULL;
    if (value && !(t = handle_get(&g_handles, value, H_TEXTURE))) return DDERR_INVALIDPARAMS;
    d3_set_texture(dev, 0, t);
    dev->st.rs[state] = value;
    if (dev->legacy_blend) MASK_SET(dev->dirty.rs, D3DRENDERSTATE_TEXTUREMAPBLEND);
    return D3D_OK;
  }
  case D3DRENDERSTATE_TEXTUREMAPBLEND:
    if (value == dev->texture_map_blend && dev->legacy_blend) return D3D_OK;
    if (dev->texture_map_blend == D3DTBLEND_MODULATEMASK || !dev->legacy_blend) dev->dirty.tss[1] |= (1u << D3DTSS_COLOROP) | (1u << D3DTSS_ALPHAOP);
    dev->texture_map_blend = value;
    dev->legacy_blend = TRUE;
    MASK_SET(dev->dirty.rs, D3DRENDERSTATE_TEXTUREMAPBLEND);
    return D3D_OK;
  case D3DRENDERSTATE_WRAPU: case D3DRENDERSTATE_WRAPV: {
    const DWORD bit = state == D3DRENDERSTATE_WRAPU ? D3DWRAP_U : D3DWRAP_V;
    d3_set_rs(dev, state, value);
    d3_set_rs(dev, D3DRENDERSTATE_WRAP0, value ? (dev->st.rs[D3DRENDERSTATE_WRAP0] | bit) : (dev->st.rs[D3DRENDERSTATE_WRAP0] & ~bit));
    return D3D_OK;
  }
  default:
    d3_set_rs(dev, state, value);
    return D3D_OK;
  }
}

/* Light states of Direct3D 2 and 3. */
static HRESULT dv_set_light_state(D3dDevice *dev, DWORD state, DWORD value) {
  DlStateOps ops;
  const HRESULT hr = dl_light_state(state, value, &ops);
  if (hr != DL_OK) return hr;
  if (state == D3DLIGHTSTATE_MATERIAL && value && !handle_get(&g_handles, value, H_MATERIAL)) return DDERR_INVALIDPARAMS;
  dev->light_state[state] = value;
  return D3D_OK;
}
static HRESULT dv_get_light_state(D3dDevice *dev, DWORD state, DWORD *value) {
  if (!value) return DDERR_INVALIDPARAMS;
  if (!state || state > D3DLIGHTSTATE_COLORVERTEX) return DDERR_INVALIDPARAMS;
  *value = state == D3DLIGHTSTATE_COLORMODEL ? D3DCOLOR_RGB : dev->light_state[state];
  return D3D_OK;
}

/* Transforms (Direct3D 2 and 3 have only world, view and projection). */
static HRESULT dv_set_transform(D3dDevice *dev, DWORD state, const D3DMATRIX *matrix) {
  if (!matrix) return DDERR_INVALIDPARAMS;
  if (!dl_transform_state(dev->version, state) || state >= D3D_XF) return DDERR_INVALIDPARAMS;
  d3_set_xf(dev, state, (const DlMatrix *)matrix);
  return D3D_OK;
}
static HRESULT dv_get_transform(D3dDevice *dev, DWORD state, D3DMATRIX *matrix) {
  if (!matrix) return DDERR_INVALIDPARAMS;
  if (!dl_transform_state(dev->version, state) || state >= D3D_XF) return DDERR_INVALIDPARAMS;
  memcpy(matrix, &dev->st.xf[state], sizeof *matrix);
  return D3D_OK;
}
static HRESULT dv_multiply_transform(D3dDevice *dev, DWORD state, const D3DMATRIX *matrix) {
  if (!matrix) return DDERR_INVALIDPARAMS;
  if (!dl_transform_state(dev->version, state) || state >= D3D_XF) return DDERR_INVALIDPARAMS;
  /* (into the device's own states even while a state block is recorded - Wine's test_multiply_transform; the matrix times the current one) */
  DlMatrix out;
  dl_matrix_multiply(&out, (const DlMatrix *)matrix, &dev->st.xf[state]);
  dev->st.xf[state] = out;
  dev->dirty.xf |= 1u << state;
  return D3D_OK;
}

static HRESULT dv_set_clip_status(D3dDevice *dev, const D3DCLIPSTATUS *cs) {
  if (!cs) return DDERR_INVALIDPARAMS;
  if (cs->dwFlags & ~(DWORD)(D3DCLIPSTATUS_STATUS | D3DCLIPSTATUS_EXTENTS2 | D3DCLIPSTATUS_EXTENTS3)) return DDERR_INVALIDPARAMS;
  if (cs->dwFlags & D3DCLIPSTATUS_STATUS) dev->clip_status.dwStatus = cs->dwStatus;
  if (cs->dwFlags & (D3DCLIPSTATUS_EXTENTS2 | D3DCLIPSTATUS_EXTENTS3)) {
    dev->clip_status.minx = cs->minx; dev->clip_status.maxx = cs->maxx;
    dev->clip_status.miny = cs->miny; dev->clip_status.maxy = cs->maxy;
    dev->clip_status.minz = cs->minz; dev->clip_status.maxz = cs->maxz;
  }
  dev->clip_status.dwFlags = cs->dwFlags ? cs->dwFlags : dev->clip_status.dwFlags;
  return D3D_OK;
}
static HRESULT dv_get_clip_status(D3dDevice *dev, D3DCLIPSTATUS *cs) {
  if (!cs) return DDERR_INVALIDPARAMS;
  *cs = dev->clip_status;
  return D3D_OK;
}

/* ---- Direct3D 2 and 3: viewports of the device ------------------------------------------------------------------------------------------------------ */

static HRESULT dv_add_viewport(D3dDevice *dev, D3dViewport *vp) {
  if (!vp) return DDERR_INVALIDPARAMS;
  if (vp->device) return DDERR_INVALIDPARAMS;      /* (a viewport is of one device at a time) */
  IDirect3DViewport3_AddRef((IDirect3DViewport3 *)vp);
  vp->device = dev;
  vp->next_in_device = dev->viewports;
  dev->viewports = vp;
  return D3D_OK;
}
/* (called with a viewport of the device) */
static HRESULT d3_viewport_delete(D3dDevice *dev, D3dViewport *vp) {
  D3dViewport **p = &dev->viewports;
  while (*p && *p != vp) p = &(*p)->next_in_device;
  if (!*p) return DDERR_INVALIDPARAMS;
  *p = vp->next_in_device;
  vp->next_in_device = NULL;
  vp->device = NULL;
  if (dev->current_viewport == vp) {
    dev->current_viewport = NULL;
    IDirect3DViewport3_Release((IDirect3DViewport3 *)vp);      /* (the reference SetCurrentViewport took) */
  }
  IDirect3DViewport3_Release((IDirect3DViewport3 *)vp);
  return D3D_OK;
}
static HRESULT dv_delete_viewport(D3dDevice *dev, D3dViewport *vp) {
  if (!vp) return DDERR_INVALIDPARAMS;
  if (vp->device != dev) return DDERR_INVALIDPARAMS;
  return d3_viewport_delete(dev, vp);
}
/* NextViewport: D3DNEXT_HEAD, D3DNEXT_TAIL, D3DNEXT_NEXT (the list in the order they were added). */
static HRESULT dv_next_viewport(D3dDevice *dev, D3dViewport *ref, D3dViewport **out, DWORD flags) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  /* (the list is newest first: walk it backwards for the order of AddViewport) */
  D3dViewport *list[256];
  UINT n = 0;
  for (D3dViewport *v = dev->viewports; v && n < 256; v = v->next_in_device) list[n++] = v;
  D3dViewport *found = NULL;
  switch (flags) {
  case D3DNEXT_HEAD: found = n ? list[n - 1] : NULL; break;
  case D3DNEXT_TAIL: found = n ? list[0] : NULL; break;
  case D3DNEXT_NEXT:
    if (!ref) return DDERR_INVALIDPARAMS;
    for (UINT i = n; i-- > 0;)
      if (list[i] == ref) { found = i ? list[i - 1] : NULL; break; }
    break;
  default: return DDERR_INVALIDPARAMS;
  }
  if (!found) return D3DERR_NOVIEWPORTS;
  IDirect3DViewport3_AddRef((IDirect3DViewport3 *)found);
  *out = found;
  return D3D_OK;
}
static HRESULT dv_set_current_viewport(D3dDevice *dev, D3dViewport *vp) {
  if (!vp) return DDERR_INVALIDPARAMS;
  if (vp->device != dev) return DDERR_INVALIDPARAMS;
  if (dev->current_viewport == vp) return D3D_OK;
  IDirect3DViewport3_AddRef((IDirect3DViewport3 *)vp);
  if (dev->current_viewport) IDirect3DViewport3_Release((IDirect3DViewport3 *)dev->current_viewport);
  dev->current_viewport = vp;
  viewport_activate(vp);
  return D3D_OK;
}
static HRESULT dv_get_current_viewport(D3dDevice *dev, D3dViewport **out) {
  /* (no current viewport comes first, and leaves the pointer as it was: Wine's test_viewport_object of Direct3D 2 and 3) */
  if (!dev->current_viewport) return D3DERR_NOCURRENTVIEWPORT;
  if (!out) return DDERR_INVALIDPARAMS;
  *out = dev->current_viewport;
  IDirect3DViewport3_AddRef((IDirect3DViewport3 *)*out);
  return D3D_OK;
}

/* ---- Direct3D 2 and 3: Begin .. End ----------------------------------------------------------------------------------------------------------------- */

static HRESULT dv_begin(D3dDevice *dev, DWORD type, DWORD fvf, const void *vertices, DWORD vertex_count, DWORD flags, BOOL indexed) {
  if (dev->begun) return D3DERR_INBEGIN;
  if (!primitive_type_ok(type) || !dl_fvf_valid(fvf, 3)) return DDERR_INVALIDPARAMS;
  const DWORD stride = dl_fvf_stride(fvf);
  dev->begun = TRUE;
  dev->begun_indexed = indexed;
  dev->begin_type = type;
  dev->begin_fvf = fvf;
  dev->begin_flags = flags;
  dev->begin_vertex_count = 0;
  dev->begin_index_count = 0;
  if (indexed) {
    if (!vertices) { dev->begun = FALSE; return DDERR_INVALIDPARAMS; }
    if ((UINT64)vertex_count * stride > 0x7FFFFFFFu) { dev->begun = FALSE; return DDERR_INVALIDPARAMS; }
    if (vertex_count > dev->begin_vertex_capacity) {
      BYTE *grown = mem_grow(dev->begin_vertices, (SIZE_T)vertex_count * stride);
      if (!grown) { dev->begun = FALSE; return DDERR_OUTOFMEMORY; }
      dev->begin_vertices = grown;
      dev->begin_vertex_capacity = vertex_count;
    }
    memcpy(dev->begin_vertices, vertices, (SIZE_T)vertex_count * stride);
    dev->begin_vertex_count = vertex_count;
  }
  return D3D_OK;
}
static HRESULT dv_vertex(D3dDevice *dev, const void *vertex) {
  if (!vertex) return DDERR_INVALIDPARAMS;
  if (!dev->begun || dev->begun_indexed) return D3DERR_NOTINBEGIN;
  const DWORD stride = dl_fvf_stride(dev->begin_fvf);
  if (dev->begin_vertex_count == dev->begin_vertex_capacity) {
    const UINT capacity = dev->begin_vertex_capacity ? dev->begin_vertex_capacity * 2 : 64;
    if (capacity > 0x100000u) return DDERR_OUTOFMEMORY;
    BYTE *grown = mem_grow(dev->begin_vertices, (SIZE_T)capacity * stride);
    if (!grown) return DDERR_OUTOFMEMORY;
    dev->begin_vertices = grown;
    dev->begin_vertex_capacity = capacity;
  }
  memcpy(dev->begin_vertices + (SIZE_T)dev->begin_vertex_count++ * stride, vertex, stride);
  return D3D_OK;
}
static HRESULT dv_index(D3dDevice *dev, WORD index) {
  if (!dev->begun || !dev->begun_indexed) return D3DERR_NOTINBEGIN;
  if (index >= dev->begin_vertex_count) return DDERR_INVALIDPARAMS;
  if (dev->begin_index_count == dev->begin_index_capacity) {
    const UINT capacity = dev->begin_index_capacity ? dev->begin_index_capacity * 2 : 128;
    if (capacity > 0x100000u) return DDERR_OUTOFMEMORY;
    WORD *grown = mem_grow(dev->begin_indices, (SIZE_T)capacity * sizeof *grown);
    if (!grown) return DDERR_OUTOFMEMORY;
    dev->begin_indices = grown;
    dev->begin_index_capacity = capacity;
  }
  dev->begin_indices[dev->begin_index_count++] = index;
  return D3D_OK;
}
static HRESULT dv_end(D3dDevice *dev, DWORD flags) {
  (void)flags;
  if (!dev->begun) return D3DERR_NOTINBEGIN;
  dev->begun = FALSE;
  DEV_READY(dev);
  if (dev->begun_indexed)
    return d3_draw(dev, dev->begin_type, dev->begin_fvf, dev->begin_vertices, dev->begin_vertex_count, dev->begin_indices, dev->begin_index_count, dev->begin_flags);
  return d3_draw(dev, dev->begin_type, dev->begin_fvf, dev->begin_vertices, dev->begin_vertex_count, NULL, 0, dev->begin_flags);
}

/* ---- textures and texture stages -------------------------------------------------------------------------------------------------------------------- */

static HRESULT dv_set_texture(D3dDevice *dev, DWORD stage, Surface *s) {
  if (stage >= D3D_STAGES) return DDERR_INVALIDPARAMS;
  if (s && !(s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE)) s = NULL;      /* (Wine: a surface that is not a texture unbinds) */
  d3_set_texture(dev, stage, s);
  if (stage == 0 && dev->legacy_blend) MASK_SET(dev->dirty.rs, D3DRENDERSTATE_TEXTUREMAPBLEND);
  return D3D_OK;
}
static HRESULT dv_get_tss(D3dDevice *dev, DWORD stage, DWORD state, DWORD *value) {
  if (!value) return DDERR_INVALIDPARAMS;
  if (stage >= D3D_STAGES) return DDERR_INVALIDPARAMS;
  if (state > D3DTSS_TEXTURETRANSFORMFLAGS) { *value = 0; return D3D_OK; }
  *value = dev->st.tss[stage][state];
  return D3D_OK;
}
static HRESULT dv_set_tss(D3dDevice *dev, DWORD stage, DWORD state, DWORD value) {
  DlStateOps ops;
  const HRESULT hr = dl_texture_stage_state(stage, state, value, &ops);
  if (hr != DL_OK) return hr;
  if (!state || state > D3DTSS_TEXTURETRANSFORMFLAGS) return D3D_OK;
  if (dev->version < 7) {
    /* (the legacy texture blending ends with a change of a texture stage state, not with a value it already has - Wine) */
    if (dev->st.tss[stage][state] == value) return D3D_OK;
    if (dev->legacy_blend) {
      dev->legacy_blend = FALSE;
      dev->dirty.tss[0] = dev->dirty.tss[1] = 0xFFFFFFFFu;
    }
  }
  d3_set_tss(dev, stage, state, value);
  return D3D_OK;
}

/* ValidateDevice: one pass for what Direct3D 9 draws, its own answer for what it refuses. */
static HRESULT dv_validate(D3dDevice *dev, DWORD *passes) {
  DEV_READY(dev);
  HRESULT hr = d3_flush(dev);
  if (FAILED(hr)) return hr;
  DWORD p = 0;
  hr = D3D_CALL(dev->dd3->dev, ValidateDevice, &p);
  if (passes) *passes = SUCCEEDED(hr) ? p : 0;
  return SUCCEEDED(hr) ? D3D_OK : hr;
}

/* ComputeSphereVisibility of Direct3D 3 and 7. */
static HRESULT dv_sphere_visibility(D3dDevice *dev, const D3DVECTOR *centers, const float *radii, DWORD count, DWORD flags, DWORD *out) {
  if (!centers || !radii || !out || flags) return DDERR_INVALIDPARAMS;
  DlTransform t;
  d3_transform(dev, &t);
  for (DWORD i = 0; i < count; ++i) {
    const float c[3] = {centers[i].x, centers[i].y, centers[i].z};
    out[i] = dev->version >= 7 ? dl_sphere_visibility7(&t, c, radii[i]) : dl_sphere_visibility3(&t, c, radii[i]);
  }
  return D3D_OK;
}

/* Strided draws of Direct3D 3 and 7: the parts gathered into interleaved vertices. */
static HRESULT dv_draw_strided(D3dDevice *dev, DWORD type, DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA *data, DWORD vertex_count, const WORD *indices,
                               DWORD index_count, DWORD flags) {
  if (!data) return DDERR_INVALIDPARAMS;
  if (!dl_fvf_valid(fvf, dev->version >= 7 ? 7 : 3)) return DDERR_INVALIDPARAMS;
  const DWORD count = indices ? indexed_vertex_count(indices, index_count, vertex_count) : vertex_count;
  if (!count) return D3D_OK;
  const DWORD stride = dl_fvf_stride(fvf);
  DEV_READY(dev);
  BYTE *buf = d3_scratch(dev->dd3, 2, (SIZE_T)count * stride);
  if (!buf) return DDERR_OUTOFMEMORY;
  const HRESULT g = dl_gather_strided(fvf, (const DlStridedData *)data, 0, count, count, dl_bytes(buf, (SIZE_T)count * stride));
  if (g != DL_OK) return g;
  return d3_draw(dev, type, fvf, buf, count, indices, index_count, flags);
}

/* Draws from a vertex buffer (a processed buffer of Direct3D 3 or 7 is drawn as it is: its vertices are transformed). */
static HRESULT dv_draw_vb(D3dDevice *dev, DWORD type, D3dVertexBuffer *vb, DWORD start, DWORD vertex_count, const WORD *indices, DWORD index_count, DWORD flags) {
  if (!vb) return DDERR_INVALIDPARAMS;
  if (vb->lock_count) return D3DERR_VERTEXBUFFERLOCKED;
  if (start > vb->desc.dwNumVertices || vertex_count > vb->desc.dwNumVertices - start) return DDERR_INVALIDPARAMS;
  if (indices) {
    for (DWORD i = 0; i < index_count; ++i)
      if ((DWORD)indices[i] >= vb->desc.dwNumVertices - start) return DDERR_INVALIDPARAMS;
    return d3_draw(dev, type, vb->desc.dwFVF, vb->memory + (SIZE_T)start * vb->stride, vb->desc.dwNumVertices - start, indices, index_count, flags);
  }
  return d3_draw(dev, type, vb->desc.dwFVF, vb->memory + (SIZE_T)start * vb->stride, vertex_count, NULL, 0, flags);
}
