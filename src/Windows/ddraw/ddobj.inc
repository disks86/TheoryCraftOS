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
/* The methods of the DirectDraw object, for IDirectDraw, IDirectDraw2, IDirectDraw4 and IDirectDraw7 (vtbl.inc wraps them per version). Included by
 * ddraw.c.
 *
 * References: each interface counts its own; the object goes when all four are 0. A surface made through IDirectDraw4 or IDirectDraw7 holds a
 * reference of that interface, one made through IDirectDraw or IDirectDraw2 none; palettes the same. The Direct3D interfaces share IDirectDraw's
 * count (IDirect3D7 IDirectDraw7's). An object made by CoCreateInstance answers DDERR_NOTINITIALIZED until Initialize. */

static HRESULT dd_query(DDraw *dd, REFIID riid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  if (!riid) return DDERR_INVALIDPARAMS;
  void *iface = NULL;
  if (guid_eq(riid, &IID_IDirectDraw7) || guid_eq(riid, &IID_IUnknown)) iface = DDI7(dd);
  else if (guid_eq(riid, &IID_IDirectDraw4)) iface = DDI4(dd);
  else if (guid_eq(riid, &IID_IDirectDraw2)) iface = DDI2(dd);
  else if (guid_eq(riid, &IID_IDirectDraw)) iface = DDI1(dd);
  else if (guid_eq(riid, &IID_IDirect3D7)) { if (dd->vt_d3d7) { dd->d3d_version = 7; iface = &dd->vt_d3d7; } }
  else if (guid_eq(riid, &IID_IDirect3D3)) { if (dd->vt_d3d3) { dd->d3d_version = 3; iface = &dd->vt_d3d3; } }
  else if (guid_eq(riid, &IID_IDirect3D2)) { if (dd->vt_d3d2) { dd->d3d_version = 2; iface = &dd->vt_d3d2; } }
  else if (guid_eq(riid, &IID_IDirect3D)) { if (dd->vt_d3d1) { dd->d3d_version = 1; iface = &dd->vt_d3d1; } }
  else if (g_d3d.ddraw_query) return g_d3d.ddraw_query(dd, riid, out);
  if (!iface) return E_NOINTERFACE;
  IUnknown_AddRef((IUnknown *)iface);
  *out = iface;
  return S_OK;
}

static void dd_destroy(DDraw *dd) {
  /* (as Windows: the cooperative level goes back to normal, the display mode to the desktop's, a device window made for it goes) */
  ddraw_set_cooperative_level(dd, NULL, DDSCL_NORMAL, TRUE);
  ddraw_restore_display_mode(dd);
  if (dd->device_window) { DestroyWindow(dd->device_window); dd->device_window = NULL; }
  focus_window_unhook(dd);
  if (g_exclusive == dd) g_exclusive = NULL;
  if (g_d3d.ddraw_destroyed) g_d3d.ddraw_destroyed(dd);
  dd_lock();
  dd->flags |= DD_DEAD;
  dd_unlock();
  ddraw_free_if_unused(dd);
}

#define DD_REFS(V, field, IFACE)                                                                                                               \
  static ULONG STDMETHODCALLTYPE dd##V##_AddRef(IFACE *i) {                                                                                    \
    DDraw *dd = DD##V(i);                                                                                                                      \
    const LONG r = InterlockedIncrement(&dd->field);                                                                                           \
    if (r == 1) InterlockedIncrement(&dd->ifaces);                                                                                             \
    return (ULONG)r;                                                                                                                           \
  }                                                                                                                                            \
  static ULONG STDMETHODCALLTYPE dd##V##_Release(IFACE *i) {                                                                                   \
    DDraw *dd = DD##V(i);                                                                                                                      \
    const LONG r = InterlockedDecrement(&dd->field);                                                                                           \
    if (!r && !InterlockedDecrement(&dd->ifaces)) dd_destroy(dd);                                                                            \
    return (ULONG)r;                                                                                                                           \
  }
DD_REFS(7, ref7, IDirectDraw7)
DD_REFS(4, ref4, IDirectDraw4)
DD_REFS(2, ref2, IDirectDraw2)
DD_REFS(1, ref1, IDirectDraw)

#define DD_READY(dd) do { if (!((dd)->flags & DD_INITIALIZED)) return DDERR_NOTINITIALIZED; } while (0)

/* ---- surfaces ---------------------------------------------------------------------------------------------------------------------------------- */

/* CreateSurface of the four versions: IDirectDraw7/4 take a DDSURFACEDESC2 and refuse explicit front and back buffers; IDirectDraw2/1 a
 * DDSURFACEDESC, and IDirectDraw refuses a primary back buffer or a flipping front buffer. */
static HRESULT dd_create_surface(DDraw *dd, const DDSURFACEDESC2 *desc, UINT version, Surface **out) {
  *out = NULL;
  dd_lock();
  Surface *s = NULL;
  HRESULT hr = surface_create(dd, desc, version, &s);
  if (SUCCEEDED(hr)) {
    if (version == 7) { s->ref7 = 1; s->release_iface = (IUnknown *)DDI7(dd); }
    else if (version == 4) { s->ref4 = 1; s->release_iface = (IUnknown *)DDI4(dd); }
    else s->ref1 = 1;
    if (s->release_iface) IUnknown_AddRef(s->release_iface);
    *out = s;
  }
  dd_unlock();
  return hr;
}
static HRESULT dd_create_surface2(DDraw *dd, DDSURFACEDESC2 *desc, void **out, IUnknown *outer, UINT version) {
  DD_READY(dd);
  if (!(dd->coop & (DDSCL_NORMAL | DDSCL_EXCLUSIVE))) return DDERR_NOCOOPERATIVELEVELSET;
  if (!desc || desc->dwSize != sizeof(DDSURFACEDESC2)) return DDERR_INVALIDPARAMS;
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  if (desc->ddsCaps.dwCaps & (DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) return DDERR_INVALIDCAPS;
  if (outer) return CLASS_E_NOAGGREGATION;
  Surface *s;
  HRESULT hr = dd_create_surface(dd, desc, version, &s);
  if (SUCCEEDED(hr)) *out = version == 7 ? (void *)IFACE7(s) : (void *)IFACE4(s);
  return hr;
}
static HRESULT dd_create_surface1(DDraw *dd, DDSURFACEDESC *desc, IDirectDrawSurface **out, IUnknown *outer, UINT version) {
  DD_READY(dd);
  if (!(dd->coop & (DDSCL_NORMAL | DDSCL_EXCLUSIVE))) return DDERR_NOCOOPERATIVELEVELSET;
  if (!desc || desc->dwSize != sizeof(DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  const DWORD c = desc->ddsCaps.dwCaps;
  if (version == 2) {
    if (c & (DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) return DDERR_INVALIDCAPS;
  } else if ((c & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) == (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER) ||
             (c & (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER)) == (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER)) {
    return DDERR_INVALIDCAPS;
  }
  if (outer) return CLASS_E_NOAGGREGATION;
  DDSURFACEDESC2 d2;
  ddsd_to_ddsd2(desc, &d2);
  Surface *s;
  HRESULT hr = dd_create_surface(dd, &d2, version, &s);
  if (SUCCEEDED(hr)) *out = IFACE1(s);
  return hr;
}

/* DuplicateSurface: a new surface object that shares the source's memory (freed with the last of them). The primary surface, complex surfaces and the
 * surfaces of one cannot be duplicated (DDERR_CANTDUPLICATE). */
static HRESULT dd_duplicate_surface(DDraw *dd, Surface *src, UINT version, Surface **out) {
  *out = NULL;
  DD_READY(dd);
  if (!src) return DDERR_INVALIDPARAMS;
  const DWORD c = src->desc.ddsCaps.dwCaps;
  if ((c & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_3DDEVICE)) || src->implicit || src->complex[0] ||
      (src->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) || !src->store.mem)
    return DDERR_CANTDUPLICATE;
  if (surface_is_lost(src)) return DDERR_SURFACELOST;
  dd_lock();
  SurfaceGroup *g = mem_alloc(sizeof *g);
  if (!g) { dd_unlock(); return DDERR_OUTOFMEMORY; }
  g->dd = dd;
  g->ref = 1;
  ++dd->keep;
  InterlockedIncrement(&g_objects);
  Surface *s = surface_alloc(dd, g, version);
  if (!s) { group_release(g); dd_unlock(); return DDERR_OUTOFMEMORY; }
  if (!src->store.shared) {
    if (src->store.own) {
      src->store.shared = mem_alloc(sizeof(LONG));
      if (!src->store.shared) { group_release(g); dd_unlock(); return DDERR_OUTOFMEMORY; }
      *src->store.shared = 1;
      src->store.own = FALSE;
    }
  }
  s->desc = src->desc;
  s->fmt = src->fmt;
  s->store.mem = src->store.mem;
  s->store.size = src->store.size;
  s->store.shared = src->store.shared;
  if (s->store.shared) ++*s->store.shared;
  s->sysmem_fallback = src->sysmem_fallback;
  s->implicit = FALSE;
  g->root = s;
  s->iface_count = 1;
  ++g->ref;
  if (version == 7) { s->ref7 = 1; s->release_iface = (IUnknown *)DDI7(dd); }
  else if (version == 4) { s->ref4 = 1; s->release_iface = (IUnknown *)DDI4(dd); }
  else s->ref1 = 1;
  if (s->release_iface) IUnknown_AddRef(s->release_iface);
  dd_unlock();
  *out = s;
  return DD_OK;
}

/* Does a surface match what EnumSurfaces asks for (the fields the description's flags name; the pixel format by Windows' rules)? */
static BOOL ddpf_match(const DDPIXELFORMAT *req, const DDPIXELFORMAT *have) {
  const DWORD must = DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 | DDPF_PALETTEINDEXED8 | DDPF_FOURCC | DDPF_ZBUFFER | DDPF_STENCILBUFFER;
  if ((req->dwFlags & have->dwFlags) != req->dwFlags) return FALSE;
  if ((req->dwFlags & must) != (have->dwFlags & must)) return FALSE;
  if ((req->dwFlags & DDPF_FOURCC) && req->dwFourCC != have->dwFourCC) return FALSE;
  if ((req->dwFlags & (DDPF_RGB | DDPF_YUV | DDPF_ZBUFFER | DDPF_ALPHA | DDPF_LUMINANCE | DDPF_BUMPDUDV)) && req->dwRGBBitCount != have->dwRGBBitCount) return FALSE;
  if ((req->dwFlags & (DDPF_RGB | DDPF_YUV | DDPF_STENCILBUFFER | DDPF_LUMINANCE | DDPF_BUMPDUDV)) && req->dwRBitMask != have->dwRBitMask) return FALSE;
  if ((req->dwFlags & (DDPF_RGB | DDPF_YUV | DDPF_ZBUFFER | DDPF_BUMPDUDV)) && req->dwGBitMask != have->dwGBitMask) return FALSE;
  if ((req->dwFlags & (DDPF_RGB | DDPF_YUV | DDPF_STENCILBUFFER | DDPF_BUMPDUDV)) && req->dwBBitMask != have->dwBBitMask) return FALSE;
  if ((req->dwFlags & (DDPF_ALPHAPIXELS | DDPF_ZPIXELS)) && req->dwRGBAlphaBitMask != have->dwRGBAlphaBitMask) return FALSE;
  return TRUE;
}
static BOOL ddsd_match(const DDSURFACEDESC2 *req, const DDSURFACEDESC2 *have) {
  if (!req) return TRUE;
  if ((req->dwFlags & have->dwFlags) != req->dwFlags) return FALSE;
  #define DDM(flag, field) if ((req->dwFlags & (flag)) && memcmp(&req->field, &have->field, sizeof req->field)) return FALSE
  DDM(DDSD_ALPHABITDEPTH, dwAlphaBitDepth); DDM(DDSD_BACKBUFFERCOUNT, dwBackBufferCount); DDM(DDSD_CAPS, ddsCaps);
  DDM(DDSD_CKDESTBLT, ddckCKDestBlt); DDM(DDSD_CKDESTOVERLAY, ddckCKDestOverlay); DDM(DDSD_CKSRCBLT, ddckCKSrcBlt); DDM(DDSD_CKSRCOVERLAY, ddckCKSrcOverlay);
  DDM(DDSD_HEIGHT, dwHeight); DDM(DDSD_LINEARSIZE, dwLinearSize); DDM(DDSD_LPSURFACE, lpSurface); DDM(DDSD_MIPMAPCOUNT, dwMipMapCount);
  DDM(DDSD_PITCH, lPitch); DDM(DDSD_REFRESHRATE, dwRefreshRate); DDM(DDSD_TEXTURESTAGE, dwTextureStage); DDM(DDSD_WIDTH, dwWidth);
  #undef DDM
  if ((req->dwFlags & DDSD_PIXELFORMAT) && !ddpf_match(&req->ddpfPixelFormat, &have->ddpfPixelFormat)) return FALSE;
  return TRUE;
}

typedef HRESULT (CALLBACK *EnumSurf7)(IDirectDrawSurface7 *, DDSURFACEDESC2 *, void *);
typedef HRESULT (CALLBACK *EnumSurf4)(IDirectDrawSurface4 *, DDSURFACEDESC2 *, void *);
typedef HRESULT (CALLBACK *EnumSurf1)(IDirectDrawSurface *, DDSURFACEDESC *, void *);
/* EnumSurfaces: the surfaces that exist (DDENUMSURFACES_DOESEXIST: all, those that match, those that do not) or those that could be made
 * (DDENUMSURFACES_CANBECREATED | MATCH: with a size, that one; without, one for each display mode's size), each with the calling version's interface
 * and description. */
static HRESULT dd_enum_surfaces(DDraw *dd, DWORD flags, const DDSURFACEDESC2 *desc, void *context, void *callback, UINT version) {
  DD_READY(dd);
  if (!callback) return DDERR_INVALIDPARAMS;
  if (flags & DDENUMSURFACES_CANBECREATED) {
    if ((flags & (DDENUMSURFACES_ALL | DDENUMSURFACES_MATCH | DDENUMSURFACES_NOMATCH)) != DDENUMSURFACES_MATCH) return DDERR_INVALIDPARAMS;
    if (!desc) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 tries[64];
    UINT n = 0;
    if (desc->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) {
      tries[n++] = *desc;
    } else {
      DisplayMode cur, m;
      display_current(&cur);
      for (DWORD i = 0; display_mode_at(i, &m) && n < 64; ++i) {
        if (m.bpp != cur.bpp) continue;
        BOOL seen = FALSE;
        for (UINT k = 0; k < n; ++k) if (tries[k].dwWidth == m.width && tries[k].dwHeight == m.height) seen = TRUE;
        if (seen) continue;
        DDSURFACEDESC2 d = *desc;
        d.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
        d.dwWidth = m.width;
        d.dwHeight = m.height;
        ddpf_from_format(format_of_display_bpp(m.bpp), &d.ddpfPixelFormat);
        d.lPitch = (LONG)(((m.width * (m.bpp / 8)) + 3) & ~3u);
        tries[n++] = d;
      }
    }
    for (UINT k = 0; k < n; ++k) {
      Surface *s;
      DDSURFACEDESC2 d = tries[k];
      d.dwSize = sizeof d;
      d.dwFlags &= ~(DWORD)DDSD_PITCH;
      if (FAILED(dd_create_surface(dd, &d, version, &s))) continue;
      DDSURFACEDESC2 got = (desc->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) ? s->desc : tries[k];
      if (version == 7) s7_Release(IFACE7(s)); else if (version == 4) s4_Release(IFACE4(s)); else s1_Release(IFACE1(s));
      HRESULT r;
      if (version >= 4) r = ((EnumSurf7)callback)(NULL, &got, context);
      else { DDSURFACEDESC d1; ddsd2_to_ddsd(&got, &d1); r = ((EnumSurf1)callback)(NULL, &d1, context); }
      if (r != DDENUMRET_OK) break;
    }
    return DD_OK;
  }
  if (!(flags & DDENUMSURFACES_DOESEXIST)) return DDERR_INVALIDPARAMS;
  if (!(flags & DDENUMSURFACES_ALL) && !desc) return DDERR_INVALIDPARAMS;
  const BOOL nomatch = (flags & DDENUMSURFACES_NOMATCH) != 0;
  /* (a snapshot: the callback may release surfaces) */
  dd_lock();
  UINT n = 0;
  for (Surface *s = dd->surfaces; s; s = s->list_next) if (s->iface_count) ++n;
  Surface **list = mem_alloc((n ? n : 1) * sizeof *list);
  if (!list) { dd_unlock(); return DDERR_OUTOFMEMORY; }
  n = 0;
  for (Surface *s = dd->surfaces; s; s = s->list_next) {
    if (!s->iface_count) continue;
    if (!(flags & DDENUMSURFACES_ALL) && nomatch == ddsd_match(desc, &s->desc)) continue;
    list[n++] = s;
    if (version == 7) s7_AddRef(IFACE7(s)); else if (version == 4) s4_AddRef(IFACE4(s)); else s1_AddRef(IFACE1(s));
  }
  dd_unlock();
  UINT i = 0;
  for (; i < n; ++i) {
    Surface *s = list[i];
    HRESULT r;
    if (version == 7) { DDSURFACEDESC2 d = s->desc; r = ((EnumSurf7)callback)(IFACE7(s), &d, context); }
    else if (version == 4) { DDSURFACEDESC2 d = s->desc; r = ((EnumSurf4)callback)(IFACE4(s), &d, context); }
    else { DDSURFACEDESC d; ddsd2_to_ddsd(&s->desc, &d); r = ((EnumSurf1)callback)(IFACE1(s), &d, context); }
    if (r != DDENUMRET_OK) { ++i; break; }
  }
  /* (the surfaces the program was not called with do not keep the reference taken for them) */
  for (; i < n; ++i) {
    Surface *s = list[i];
    if (version == 7) s7_Release(IFACE7(s)); else if (version == 4) s4_Release(IFACE4(s)); else s1_Release(IFACE1(s));
  }
  mem_free(list);
  return DD_OK;
}

/* ---- display ------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT dd_get_display_mode(DDraw *dd, DDSURFACEDESC2 *out) {
  DD_READY(dd);
  if (!out || (out->dwSize != sizeof(DDSURFACEDESC) && out->dwSize != sizeof(DDSURFACEDESC2))) return DDERR_INVALIDPARAMS;
  DisplayMode m;
  display_current(&m);
  const DWORD size = out->dwSize;
  zero(out, size);
  out->dwSize = sizeof(DDSURFACEDESC2);
  out->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH | DDSD_REFRESHRATE;
  out->dwWidth = m.width;
  out->dwHeight = m.height;
  out->dwRefreshRate = m.refresh;
  ddpf_from_format(format_of_display_bpp(m.bpp), &out->ddpfPixelFormat);
  out->lPitch = (LONG)(m.width * out->ddpfPixelFormat.dwRGBBitCount / 8);
  return DD_OK;
}

typedef HRESULT (CALLBACK *EnumModes2)(DDSURFACEDESC2 *, void *);
typedef HRESULT (CALLBACK *EnumModes1)(DDSURFACEDESC *, void *);
static HRESULT dd_enum_display_modes(DDraw *dd, DWORD flags, const DDSURFACEDESC2 *desc, void *context, void *callback, BOOL v2desc) {
  DD_READY(dd);
  if (!callback) return DDERR_INVALIDPARAMS;
  if (flags & ~(DWORD)(DDEDM_REFRESHRATES | DDEDM_STANDARDVGAMODES)) return DDERR_INVALIDPARAMS;
  DisplayMode seen[256];
  UINT n = 0;
  DisplayMode m;
  for (DWORD i = 0; display_mode_at(i, &m); ++i) {
    if (m.bpp != 8 && m.bpp != 16 && m.bpp != 24 && m.bpp != 32) continue;
    if (desc) {
      if ((desc->dwFlags & DDSD_WIDTH) && m.width != desc->dwWidth) continue;
      if ((desc->dwFlags & DDSD_HEIGHT) && m.height != desc->dwHeight) continue;
      if ((desc->dwFlags & DDSD_REFRESHRATE) && m.refresh != desc->dwRefreshRate) continue;
      if ((desc->dwFlags & DDSD_PIXELFORMAT) && m.bpp != desc->ddpfPixelFormat.dwRGBBitCount) continue;
    }
    BOOL again = FALSE;
    for (UINT k = 0; k < n && !again; ++k)
      if (seen[k].width == m.width && seen[k].height == m.height && seen[k].bpp == m.bpp && (seen[k].refresh == m.refresh || !(flags & DDEDM_REFRESHRATES))) again = TRUE;
    if (again) continue;
    if (n < sizeof seen / sizeof seen[0]) seen[n++] = m;
    DDSURFACEDESC2 d;
    zero(&d, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH | DDSD_REFRESHRATE;
    if (flags & DDEDM_REFRESHRATES) d.dwRefreshRate = m.refresh;
    d.dwWidth = m.width;
    d.dwHeight = m.height;
    ddpf_from_format(format_of_display_bpp(m.bpp), &d.ddpfPixelFormat);
    d.lPitch = (LONG)((((m.bpp / 8) * m.width) + 3) & ~3u);
    HRESULT r;
    if (v2desc) r = ((EnumModes2)callback)(&d, context);
    else { DDSURFACEDESC d1; ddsd2_to_ddsd(&d, &d1); r = ((EnumModes1)callback)(&d1, context); }
    if (r == DDENUMRET_CANCEL) break;
  }
  return DD_OK;
}

static HRESULT dd_get_caps(DDraw *dd, DDCAPS *driver, DDCAPS *hel) {
  DD_READY(dd);
  if (!driver && !hel) return DDERR_INVALIDPARAMS;
  /* (a DDCAPS of any DirectX version: the DX3, DX5, DX6 and DX7 sizes) */
  const DWORD sizes[] = {sizeof(DDCAPS_DX3), sizeof(DDCAPS_DX5), sizeof(DDCAPS_DX6), sizeof(DDCAPS_DX7)};
  for (int k = 0; k < 2; ++k) {
    DDCAPS *c = k ? hel : driver;
    if (!c) continue;
    BOOL ok = FALSE;
    for (UINT i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) if (c->dwSize == sizes[i]) ok = TRUE;
    if (!ok) return DDERR_INVALIDPARAMS;
  }
  DDCAPS caps;
  dd_lock();
  if (driver) {
    ddraw_caps(dd, &caps, FALSE);
    caps.dwVidMemFree = g_vidmem_used >= caps.dwVidMemTotal ? 0 : caps.dwVidMemTotal - (DWORD)g_vidmem_used;
    copy_by_size(driver, driver->dwSize, &caps, sizeof caps);
  }
  if (hel) {
    ddraw_caps(dd, &caps, TRUE);
    copy_by_size(hel, hel->dwSize, &caps, sizeof caps);
  }
  dd_unlock();
  return DD_OK;
}

static HRESULT dd_get_fourcc_codes(DDraw *dd, DWORD *count, DWORD *codes) {
  DD_READY(dd);
  if (!count) return DDERR_INVALIDPARAMS;
  const DWORD n = fourcc_codes(NULL, 0);
  if (codes) fourcc_codes(codes, *count);
  *count = n;
  return DD_OK;
}
static HRESULT dd_get_monitor_frequency(DDraw *dd, DWORD *frequency) {
  DD_READY(dd);
  if (!frequency) return DDERR_INVALIDPARAMS;
  DisplayMode m;
  display_current(&m);
  *frequency = m.refresh;
  return DD_OK;
}
static HRESULT dd_get_scan_line(DDraw *dd, DWORD *line) {
  DD_READY(dd);
  if (!line) return DDERR_INVALIDPARAMS;
  BOOL blank = FALSE;
  DWORD l = 0;
  if (!display_scanline(&l, &blank)) return DDERR_UNSUPPORTED;
  *line = l;
  return blank ? DDERR_VERTICALBLANKINPROGRESS : DD_OK;
}
static HRESULT dd_get_vertical_blank_status(DDraw *dd, BOOL *status) {
  DD_READY(dd);
  if (!status) return DDERR_INVALIDPARAMS;
  BOOL blank = FALSE;
  DWORD l = 0;
  if (!display_scanline(&l, &blank)) return DDERR_UNSUPPORTED;
  *status = blank;
  return DD_OK;
}
static HRESULT dd_wait_for_vertical_blank(DDraw *dd, DWORD flags, HANDLE event) {
  DD_READY(dd);
  (void)event;
  if (flags == DDWAITVB_BLOCKBEGINEVENT) return DDERR_UNSUPPORTED;      /* (as Windows: an event is not signalled at the blank) */
  if (flags != DDWAITVB_BLOCKBEGIN && flags != DDWAITVB_BLOCKEND) return DDERR_INVALIDPARAMS;
  if (!display_wait_vblank()) return DDERR_UNSUPPORTED;
  if (flags == DDWAITVB_BLOCKEND) {
    /* (until the beam leaves the blank) */
    for (int i = 0; i < 2000; ++i) {
      BOOL blank = FALSE;
      DWORD l = 0;
      if (!display_scanline(&l, &blank) || !blank) break;
      Sleep(0);
    }
  }
  return DD_OK;
}
static HRESULT dd_get_available_vid_mem(DDraw *dd, const DDSCAPS2 *caps, DWORD *total, DWORD *free_) {
  DD_READY(dd);
  if (!total && !free_) return DDERR_INVALIDPARAMS;
  if (caps && (caps->dwCaps & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_PRIMARYSURFACE)) ) {
    /* (system memory is not video memory, and the primary surface is the frame buffer) */
    if (total) *total = 0;
    if (free_) *free_ = 0;
    return DD_OK;
  }
  dd_lock();
  DisplayMode m;
  display_current(&m);
  const UINT64 mem = display_video_memory();
  const UINT64 fb = (UINT64)m.width * m.height * ((m.bpp + 7) / 8);
  UINT64 t = mem > fb ? mem - fb : 0;
  if (t > 0xFFFFFFFFull) t = 0xFFFFFFFFull;
  UINT64 f = t > g_vidmem_used ? t - g_vidmem_used : 0;
  if (total) *total = (DWORD)t;
  if (free_) *free_ = (DWORD)f;
  dd_unlock();
  return DD_OK;
}

static HRESULT dd_initialize(DDraw *dd, GUID *guid) {
  if (dd->flags & DD_INITIALIZED) return DDERR_ALREADYINITIALIZED;
  if (!driver_guid_ok(guid)) return DDERR_INVALIDDIRECTDRAWGUID;
  dd->kind = driver_kind(guid);
  dd->flags |= DD_INITIALIZED;
  return DD_OK;
}

static HRESULT dd_get_gdi_surface(DDraw *dd, Surface **out) {
  *out = NULL;
  DD_READY(dd);
  if (!dd->primary) return DDERR_NOTFOUND;
  /* (the surface of the flipping chain whose memory is the GDI's: the front buffer, the primary surface itself) */
  *out = dd->primary;
  return DD_OK;
}
static HRESULT dd_flip_to_gdi_surface(DDraw *dd) {
  DD_READY(dd);
  Surface *gdi;
  HRESULT hr = dd_get_gdi_surface(dd, &gdi);
  if (FAILED(hr)) return hr;
  /* (the primary surface keeps the GDI's memory in its front buffer here: flipping chains pass memory, not the front's identity) */
  present_primary(gdi, NULL);
  return DD_OK;
}

static HRESULT dd_test_cooperative_level(DDraw *dd) {
  DD_READY(dd);
  if (dd->device_state == DD_STATE_LOST) return DDERR_NOEXCLUSIVEMODE;
  if ((dd->coop & DDSCL_EXCLUSIVE) && g_exclusive != dd) return DDERR_NOEXCLUSIVEMODE;
  if (!(dd->coop & DDSCL_EXCLUSIVE) && g_exclusive && g_exclusive != dd) return DDERR_EXCLUSIVEMODEALREADYSET;
  return DD_OK;
}

static HRESULT dd_get_device_identifier(DDraw *dd, DDDEVICEIDENTIFIER2 *out, DWORD flags) {
  DD_READY(dd);
  if (!out) return DDERR_INVALIDPARAMS;
  if (flags & ~(DWORD)DDGDI_GETHOSTIDENTIFIER) return DDERR_INVALIDPARAMS;
  DDDEVICEIDENTIFIER2 id;
  ddraw_identifier(&id);
  /* (up to dwWHQLLevel: the structure may be padded to 8 bytes, and Windows writes nothing after the field) */
  memcpy(out, &id, offsetof(DDDEVICEIDENTIFIER2, dwWHQLLevel) + sizeof(DWORD));
  return DD_OK;
}

static HRESULT dd_get_surface_from_dc(DDraw *dd, HDC dc, Surface **out) {
  *out = NULL;
  DD_READY(dd);
  if (!dc) return DDERR_NOTFOUND;
  dd_lock();
  for (Surface *s = dd->surfaces; s; s = s->list_next)
    if (s->dc == dc && s->iface_count) { *out = s; dd_unlock(); return DD_OK; }
  dd_unlock();
  return DDERR_NOTFOUND;
}

static HRESULT dd_restore_all_surfaces(DDraw *dd) {
  DD_READY(dd);
  dd_lock();
  for (Surface *s = dd->surfaces; s; s = s->list_next) if (s->iface_count || s->implicit) surface_restore(s);
  dd_unlock();
  return DD_OK;
}

/* StartModeTest / EvaluateMode: the program tests display modes for the monitor; each is set for it to look at, EvaluateMode passes or fails it (or
 * after 15 seconds it fails), the next one follows, and at the end the desktop's mode comes back (DDERR_TESTFINISHED). */
static HRESULT dd_set_test_mode(DDraw *dd) {
  while (dd->test_index < dd->test_count) {
    const SIZE z = dd->test_modes[dd->test_index];
    DisplayMode cur;
    display_current(&cur);
    if (SUCCEEDED(display_set((DWORD)z.cx, (DWORD)z.cy, cur.bpp, 0))) {
      dd->test_start_tick = GetTickCount();
      return DD_OK;
    }
    ++dd->test_index;
  }
  return DDERR_TESTFINISHED;
}
static HRESULT dd_start_mode_test(DDraw *dd, SIZE *modes, DWORD count, DWORD flags) {
  DD_READY(dd);
  if (flags & ~(DWORD)DDSMT_ISTESTREQUIRED) return DDERR_INVALIDPARAMS;
  if (!(flags & DDSMT_ISTESTREQUIRED) && (!(dd->coop & DDSCL_EXCLUSIVE) || g_exclusive != dd)) return DDERR_NOEXCLUSIVEMODE;
  if (!modes || !count) {
    if (count || modes) return DDERR_INVALIDPARAMS;
    /* (no modes: a test going on is stopped) */
    if (dd->test_modes) { mem_free(dd->test_modes); dd->test_modes = NULL; dd->test_count = dd->test_index = 0; display_restore(); }
    return DD_OK;
  }
  for (DWORD i = 0; i < count; ++i) if (modes[i].cx <= 0 || modes[i].cy <= 0) return DDERR_INVALIDPARAMS;
  if (flags & DDSMT_ISTESTREQUIRED) {
    /* (a mode needs testing if the display does not list it already) */
    DisplayMode cur;
    display_current(&cur);
    for (DWORD i = 0; i < count; ++i) if (!display_has_mode((DWORD)modes[i].cx, (DWORD)modes[i].cy, cur.bpp, 0)) return DDERR_NEWMODE;
    return DDERR_TESTFINISHED;
  }
  SIZE *copy = mem_alloc(count * sizeof *copy);
  if (!copy) return DDERR_OUTOFMEMORY;
  memcpy(copy, modes, count * sizeof *copy);
  mem_free(dd->test_modes);
  dd->test_modes = copy;
  dd->test_count = count;
  dd->test_index = 0;
  HRESULT hr = dd_set_test_mode(dd);
  if (hr == DDERR_TESTFINISHED) { mem_free(dd->test_modes); dd->test_modes = NULL; dd->test_count = 0; display_restore(); }
  return hr == DDERR_TESTFINISHED ? DDERR_TESTFINISHED : DD_OK;
}
static HRESULT dd_evaluate_mode(DDraw *dd, DWORD flags, DWORD *timeout) {
  DD_READY(dd);
  if (!dd->test_modes) return DDERR_NOTFOUND;
  if (flags & ~(DWORD)(DDEM_MODEPASSED | DDEM_MODEFAILED)) return DDERR_INVALIDPARAMS;
  if ((flags & DDEM_MODEPASSED) && (flags & DDEM_MODEFAILED)) return DDERR_INVALIDPARAMS;
  const DWORD elapsed = GetTickCount() - dd->test_start_tick;
  if (flags || elapsed >= 15000) {
    ++dd->test_index;
    HRESULT hr = dd_set_test_mode(dd);
    if (hr == DDERR_TESTFINISHED) {
      mem_free(dd->test_modes);
      dd->test_modes = NULL;
      dd->test_count = dd->test_index = 0;
      display_restore();
      if (timeout) *timeout = 0;
      return DDERR_TESTFINISHED;
    }
    if (timeout) *timeout = 15;
    return elapsed >= 15000 && !flags ? DDERR_EXPIRED : DD_OK;
  }
  if (timeout) *timeout = (15000 - elapsed + 999) / 1000;
  return DD_OK;
}

/* Compact: Windows' answer is DD_OK for a program in exclusive mode (it has nothing to move), DDERR_NOEXCLUSIVEMODE otherwise. */
static HRESULT dd_compact(DDraw *dd) {
  DD_READY(dd);
  if (!(dd->coop & DDSCL_EXCLUSIVE)) return DDERR_NOEXCLUSIVEMODE;
  return DD_OK;
}

static HRESULT dd_create_palette(DDraw *dd, DWORD flags, PALETTEENTRY *entries, IDirectDrawPalette **out, IUnknown *outer, UINT version) {
  DD_READY(dd);
  if (outer) return CLASS_E_NOAGGREGATION;
  if (!out) return DDERR_INVALIDPARAMS;
  if (!dd->coop) return DDERR_NOCOOPERATIVELEVELSET;
  dd_lock();
  Palette *p;
  HRESULT hr = palette_create(dd, flags, entries, &p);
  if (SUCCEEDED(hr)) {
    if (version == 7) p->release_iface = (IUnknown *)DDI7(dd);
    else if (version == 4) p->release_iface = (IUnknown *)DDI4(dd);
    if (p->release_iface) IUnknown_AddRef(p->release_iface);
    *out = (IDirectDrawPalette *)p;
  }
  dd_unlock();
  return hr;
}
static HRESULT dd_create_clipper(DDraw *dd, DWORD flags, IDirectDrawClipper **out, IUnknown *outer) {
  DD_READY(dd);
  if (outer) return CLASS_E_NOAGGREGATION;
  if (!out) return DDERR_INVALIDPARAMS;
  Clipper *c;
  HRESULT hr = clipper_create(flags, &c);
  if (SUCCEEDED(hr)) *out = (IDirectDrawClipper *)c;
  return hr;
}
