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
/* Surfaces: creation (every DDSD/DDSCAPS combination Windows takes, and its refusals), references per interface, groups, attachments, Lock/Unlock,
 * GetDC/ReleaseDC (a GDI memory DC on a DIB section that has the surface's pixels while the DC is out), descriptions, colour keys, palettes and
 * clippers, private data, uniqueness values, priority and LOD of managed textures, lost surfaces, SetSurfaceDesc, the gamma ramp. Included by ddraw.c.
 *
 * Memory: a surface's pixels are one allocation of its own (or the program's, DDSD_LPSURFACE). The pitch of a row is the format's bytes rounded up to
 * 8 bytes (system memory of a 64-bit process: 4), as Windows' drivers give it; a block format's is its rows of blocks, which GetSurfaceDesc reports as
 * the linear size. */

extern IDirectDrawSurface7Vtbl surf7_vtbl;
extern IDirectDrawSurface4Vtbl surf4_vtbl;
extern IDirectDrawSurface3Vtbl surf3_vtbl;
extern IDirectDrawSurface2Vtbl surf2_vtbl;
extern IDirectDrawSurfaceVtbl surf1_vtbl;
extern IDirectDrawGammaControlVtbl gamma_vtbl;

/* ---- references ------------------------------------------------------------------------------------------------------------------------------------- */

static void surface_free(Surface *s);
static HRESULT surface_detach(Surface *s, Surface *attachment, IUnknown *detach_iface);

static void group_release(SurfaceGroup *g) {
  if (--g->ref > 0) return;
  DDraw *dd = g->dd;
  for (UINT i = 0; i < g->count; ++i) surface_free(g->members[i]);
  mem_free(g->members);
  mem_free(g);
  --dd->keep;
  InterlockedDecrement(&g_objects);
  ddraw_free_if_unused(dd);
}

/* A surface's interfaces got their first reference: it holds its DirectDraw interface and its group again. */
static void surface_add_iface(Surface *s) {
  if (InterlockedIncrement(&s->iface_count) == 1) {
    if (s->release_iface) IUnknown_AddRef(s->release_iface);
    ++s->group->ref;
  }
}
static void surface_set_palette(Surface *s, Palette *p);

/* The root of a group is destroyed (its last reference went): what is attached to it is let go, the surfaces made with it go with it (they stay in
 * memory while a program still has references of them). */
static void surface_destroy_root(Surface *s) {
  surface_set_palette(s, NULL);
  /* (a flipping chain that version 1 surfaces were attached to: those are detached) */
  for (UINT i = 0; i < MAX_COMPLEX; ++i) {
    Surface *c = s->complex[i];
    while (c && !c->implicit && !c->flip_chain_start && c->group != s->group) {
      if (FAILED(surface_detach(s, c, c->attached_iface))) break;
      c = s->complex[i];
    }
  }
  if ((s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) && s->dd && (s->dd->flags & DD_RESTORE_MODE) && s->dd->dest_window)
    RedrawWindow(s->dd->dest_window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
  if (s->dd && s->dd->primary == s) s->dd->primary = NULL;
  SurfaceGroup *g = s->group;
  g->root = NULL;
  /* (the reference each other surface of the group had from its creation goes; one the program still holds keeps that surface, and the group's
   * memory, until it is released too) */
  for (UINT i = 0; i < g->count; ++i) {
    Surface *m = g->members[i];
    if (m == s) continue;
    LONG *r = s->version == 7 ? &m->ref7 : s->version == 4 ? &m->ref4 : &m->ref1;
    if (*r > 0 && InterlockedDecrement(r) == 0 && InterlockedDecrement(&m->iface_count) == 0) --g->ref;
  }
  /* (the root's "alive" reference; its interfaces' one goes in the caller) */
  --g->ref;
}

static ULONG surface_release_iface(Surface *s) {
  /* (a surface that is attached to another is not destroyed by its last release: the attachment's reference keeps it, as Windows) */
  if (s->iface_count == 1 && s->attached_iface) IUnknown_AddRef(s->attached_iface);
  const LONG n = InterlockedDecrement(&s->iface_count);
  if (n == 0) {
    IUnknown *release = s->release_iface;
    SurfaceGroup *g = s->group;
    dd_lock();
    if (!s->implicit && g->root == s) surface_destroy_root(s);
    group_release(g);
    dd_unlock();
    if (release) IUnknown_Release(release);
  }
  return (ULONG)n;
}

#define SURF_REFS(V, field, IFACE)                                                                                                             \
  static ULONG STDMETHODCALLTYPE s##V##_AddRef(IFACE *i) {                                                                                       \
    Surface *s = S##V(i);                                                                                                                      \
    const LONG r = InterlockedIncrement(&s->field);                                                                                            \
    if (r == 1) surface_add_iface(s);                                                                                                          \
    return (ULONG)r;                                                                                                                           \
  }                                                                                                                                            \
  static ULONG STDMETHODCALLTYPE s##V##_Release(IFACE *i) {                                                                                      \
    Surface *s = S##V(i);                                                                                                                      \
    const LONG r = InterlockedDecrement(&s->field);                                                                                            \
    if (r == 0) surface_release_iface(s);                                                                                                      \
    return (ULONG)r;                                                                                                                           \
  }
SURF_REFS(7, ref7, IDirectDrawSurface7)
SURF_REFS(4, ref4, IDirectDrawSurface4)
SURF_REFS(3, ref3, IDirectDrawSurface3)
SURF_REFS(2, ref2, IDirectDrawSurface2)
SURF_REFS(1, ref1, IDirectDrawSurface)

#define SG(i) DD_IMPL(Surface, vt_gamma, i)
static ULONG STDMETHODCALLTYPE gamma_AddRef(IDirectDrawGammaControl *i) {
  Surface *s = SG(i);
  const LONG r = InterlockedIncrement(&s->ref_gamma);
  if (r == 1) surface_add_iface(s);
  return (ULONG)r;
}
static ULONG STDMETHODCALLTYPE gamma_Release(IDirectDrawGammaControl *i) {
  Surface *s = SG(i);
  const LONG r = InterlockedDecrement(&s->ref_gamma);
  if (r == 0) surface_release_iface(s);
  return (ULONG)r;
}

/* QueryInterface of every surface interface: the versions of IDirectDrawSurface and IDirectDrawGammaControl; IDirect3DTexture(2) and the Direct3D
 * devices of version 1-4 surfaces from the Direct3D part. Windows refuses what it does not know with E_NOINTERFACE on version 7 surfaces and
 * E_INVALIDARG on the older ones. */
static HRESULT surface_query(Surface *s, REFIID riid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = NULL;
  if (!riid) return DDERR_INVALIDPARAMS;
  if (guid_eq(riid, &IID_IDirectDrawSurface7)) { s7_AddRef(IFACE7(s)); *out = IFACE7(s); return S_OK; }
  if (guid_eq(riid, &IID_IDirectDrawSurface4)) { s4_AddRef(IFACE4(s)); *out = IFACE4(s); return S_OK; }
  if (guid_eq(riid, &IID_IDirectDrawSurface3)) { s3_AddRef(IFACE3(s)); *out = IFACE3(s); return S_OK; }
  if (guid_eq(riid, &IID_IDirectDrawSurface2)) { s2_AddRef(IFACE2(s)); *out = IFACE2(s); return S_OK; }
  if (guid_eq(riid, &IID_IDirectDrawSurface) || guid_eq(riid, &IID_IUnknown)) { s1_AddRef(IFACE1(s)); *out = IFACE1(s); return S_OK; }
  if (guid_eq(riid, &IID_IDirectDrawGammaControl)) {
    gamma_AddRef((IDirectDrawGammaControl *)&s->vt_gamma);
    *out = &s->vt_gamma;
    return S_OK;
  }
  if (guid_eq(riid, &IID_IDirectDrawColorControl)) return E_NOINTERFACE;      /* (no colour controls on this display) */
  if (s->version != 7) {
    if (guid_eq(riid, &IID_IDirect3DTexture2) || guid_eq(riid, &IID_IDirect3DTexture)) {
      const void *vtbl = guid_eq(riid, &IID_IDirect3DTexture2) ? (const void *)s->vt_tex2 : (const void *)s->vt_tex1;
      if (!vtbl) return E_NOINTERFACE;      /* (no Direct3D here) */
      IUnknown_AddRef(s->texture_outer);
      *out = guid_eq(riid, &IID_IDirect3DTexture2) ? (void *)&s->vt_tex2 : (void *)&s->vt_tex1;
      return S_OK;
    }
    if (g_d3d.surface_query) {
      HRESULT hr = g_d3d.surface_query(s, riid, out);
      if (hr != E_NOINTERFACE) return hr;
    }
    return E_INVALIDARG;
  }
  return E_NOINTERFACE;
}

/* ---- creation ---------------------------------------------------------------------------------------------------------------------------------------- */

/* The pitch alignment of a surface's rows: video memory and 32-bit processes 8 bytes, system memory of a 64-bit process 4 (as Windows). */
static UINT surface_pitch_align(DWORD caps) {
  if (sizeof(void *) == 8 && (caps & DDSCAPS_SYSTEMMEMORY)) return 4;
  return 8;
}

/* Is a format one a surface may be made of, in this kind of memory? */
static BOOL format_creatable(UINT fmt, DWORD caps) {
  const DdFormatInfo *f = dd_format(fmt);
  if (f->kind == DD_KIND_NONE) return FALSE;
  if ((caps & DDSCAPS_ZBUFFER) != (f->kind == DD_KIND_DEPTH ? (DWORD)DDSCAPS_ZBUFFER : 0)) return FALSE;
  if (caps & DDSCAPS_PRIMARYSURFACE)
    return fmt == DD_FMT_P8 || fmt == DD_FMT_X1R5G5B5 || fmt == DD_FMT_R5G6B5 || fmt == DD_FMT_R8G8B8 || fmt == DD_FMT_X8R8G8B8 || fmt == DD_FMT_A8R8G8B8;
  return TRUE;
}

static Surface *surface_alloc(DDraw *dd, SurfaceGroup *g, UINT version) {
  Surface *s = mem_alloc(sizeof *s);
  if (!s) return NULL;
  SurfaceGroup **unused = NULL;
  (void)unused;
  Surface **grown = mem_grow(g->members, (g->count + 1) * sizeof *grown);
  if (!grown) { mem_free(s); return NULL; }
  g->members = grown;
  g->members[g->count++] = s;
  s->vt7 = &surf7_vtbl;
  s->vt4 = &surf4_vtbl;
  s->vt3 = &surf3_vtbl;
  s->vt2 = &surf2_vtbl;
  s->vt1 = &surf1_vtbl;
  s->vt_gamma = &gamma_vtbl;
  s->vt_tex2 = g_d3d.texture2_vtbl;
  s->vt_tex1 = g_d3d.texture1_vtbl;
  s->version = version;
  s->dd = dd;
  s->group = g;
  s->implicit = TRUE;
  s->uniqueness = g_uniqueness++;
  if (!g_uniqueness) g_uniqueness = 1;
  if (version == 7) s->texture_outer = (IUnknown *)IFACE7(s);
  else if (version == 4) s->texture_outer = (IUnknown *)IFACE4(s);
  else s->texture_outer = (IUnknown *)IFACE1(s);
  for (UINT i = 0; i < 256; ++i) s->gamma.red[i] = s->gamma.green[i] = s->gamma.blue[i] = (WORD)(i * 257);
  /* (the DirectDraw object's list) */
  s->list_next = dd->surfaces;
  if (dd->surfaces) dd->surfaces->list_prev = s;
  dd->surfaces = s;
  return s;
}

/* The memory of a surface of the description (its width, height, format and pitch set; DDSD_LPSURFACE: the program's). */
static HRESULT surface_alloc_store(Surface *s, const DDSURFACEDESC2 *desc) {
  const UINT64 bytes = dd_image_bytes(s->fmt, s->desc.dwWidth, s->desc.dwHeight,
                                      dd_format_is_blocks(s->fmt) ? dd_min_pitch(s->fmt, s->desc.dwWidth) : (uint32_t)s->desc.lPitch);
  if (!bytes || bytes > 0x7FFFFFFFull) return DDERR_OUTOFMEMORY;
  if (desc && (desc->dwFlags & DDSD_LPSURFACE)) {
    s->store.mem = desc->lpSurface;
    s->store.size = (SIZE_T)bytes;
    s->store.own = FALSE;
    return DD_OK;
  }
  if (s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) {
    const UINT64 total = display_video_memory();
    if (g_vidmem_used + bytes > total) return DDERR_OUTOFVIDEOMEMORY;
  }
  s->store.mem = mem_alloc((SIZE_T)bytes);
  if (!s->store.mem) return (s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) ? DDERR_OUTOFVIDEOMEMORY : DDERR_OUTOFMEMORY;
  s->store.size = (SIZE_T)bytes;
  s->store.own = TRUE;
  if (s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) { s->store.vidmem = (SIZE_T)bytes; g_vidmem_used += bytes; }
  return DD_OK;
}

static void surface_unlink(Surface *s) {
  DDraw *dd = s->dd;
  if (s->list_prev) s->list_prev->list_next = s->list_next; else if (dd->surfaces == s) dd->surfaces = s->list_next;
  if (s->list_next) s->list_next->list_prev = s->list_prev;
  s->list_prev = s->list_next = NULL;
}
static void private_free_all(Private **list);
static void surface_dc_destroy(Surface *s);

static void surface_free(Surface *s) {
  /* (what was attached to it with AddAttachedSurface lets go of it) */
  while (s->attached)
    if (FAILED(surface_detach(s, s->attached->child, s->attached->iface))) break;
  surface_unlink(s);
  if (s->dd->primary == s) s->dd->primary = NULL;
  if (s->palette) { IDirectDrawPalette_Release((IDirectDrawPalette *)s->palette); s->palette = NULL; }
  if (s->clipper && clipper_alive(s->clipper)) IDirectDrawClipper_Release((IDirectDrawClipper *)s->clipper);
  s->clipper = NULL;
  surface_dc_destroy(s);
  private_free_all(&s->priv);
  if (g_d3d.surface_destroyed) g_d3d.surface_destroyed(s);
  g_vidmem_used -= s->store.vidmem;
  if (s->store.shared) {
    if (!--*s->store.shared) { mem_free(s->store.mem); mem_free(s->store.shared); }
  } else if (s->store.own) {
    mem_free(s->store.mem);
  }
  mem_free(s);
}

/* The levels a mipmap gets when the program names none: until both sides are 1 for version 7, until one side is 1 before (Windows). */
static UINT log2_floor(DWORD v) { UINT n = 0; while (v > 1) { v >>= 1; ++n; } return n; }

static HRESULT surface_init_level(Surface *s, const DDSURFACEDESC2 *base, DWORD width, DWORD height, UINT level, UINT levels, UINT face, UINT faces) {
  s->desc = *base;
  s->desc.dwWidth = width;
  s->desc.dwHeight = height;
  s->level = level;
  s->face = face;
  if (s->desc.ddsCaps.dwCaps & DDSCAPS_MIPMAP) s->desc.dwMipMapCount = levels - level;
  if (level) s->desc.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL; else s->desc.ddsCaps.dwCaps2 &= ~(DWORD)DDSCAPS2_MIPMAPSUBLEVEL;
  if (faces == 6) {
    static const DWORD face_caps[6] = {DDSCAPS2_CUBEMAP_POSITIVEX, DDSCAPS2_CUBEMAP_NEGATIVEX, DDSCAPS2_CUBEMAP_POSITIVEY, DDSCAPS2_CUBEMAP_NEGATIVEY,
                                       DDSCAPS2_CUBEMAP_POSITIVEZ, DDSCAPS2_CUBEMAP_NEGATIVEZ};
    s->desc.ddsCaps.dwCaps2 &= ~(DWORD)DDSCAPS2_CUBEMAP_ALLFACES;
    s->desc.ddsCaps.dwCaps2 |= face_caps[face];
  }
  if (dd_format_is_blocks(s->fmt)) {
    s->desc.dwFlags &= ~(DWORD)DDSD_PITCH;
    s->desc.dwFlags |= DDSD_LINEARSIZE;
    s->desc.dwLinearSize = (base->dwFlags & DDSD_LPSURFACE) ? ~0u : (DWORD)dd_image_bytes(s->fmt, width, height, dd_min_pitch(s->fmt, width));
  } else {
    if (!(base->dwFlags & DDSD_LPSURFACE)) {
      const UINT align = surface_pitch_align(s->desc.ddsCaps.dwCaps);
      const DWORD pitch = dd_format(s->fmt)->kind == DD_KIND_YUV_PLANAR ? ((dd_min_pitch(s->fmt, width) + align - 1) & ~(align - 1)) : dd_aligned_pitch(s->fmt, width, align);
      if (!pitch) return DDERR_INVALIDPARAMS;
      s->desc.lPitch = (LONG)pitch;
    }
    s->desc.dwFlags |= DDSD_PITCH;
    s->desc.dwFlags &= ~(DWORD)DDSD_LINEARSIZE;
  }
  HRESULT hr = surface_alloc_store(s, base);
  s->desc.dwFlags &= ~(DWORD)DDSD_LPSURFACE;
  s->desc.lpSurface = NULL;
  return hr;
}

/* Makes the surfaces of a CreateSurface (`desc` already a DDSURFACEDESC2; `version` the interface's: 1, 2, 4 or 7) after the checks the interface
 * versions do themselves. The root has no references yet: the caller gives it its first. */
static HRESULT surface_create(DDraw *dd, const DDSURFACEDESC2 *in, UINT version, Surface **out) {
  DDSURFACEDESC2 d = *in;
  d.dwFlags |= DDSD_CAPS;
  DWORD *caps = &d.ddsCaps.dwCaps, *caps2 = &d.ddsCaps.dwCaps2;
  if (version < 7) { d.ddsCaps.dwCaps3 = 0; d.ddsCaps.dwCaps4 = 0; }
  if (version < 4) d.ddsCaps.dwCaps2 = 0;
  if ((*caps & DDSCAPS_COMPLEX) && !(*caps & (DDSCAPS_FLIP | DDSCAPS_MIPMAP)) && !(*caps2 & DDSCAPS2_CUBEMAP)) return DDERR_INVALIDCAPS;
  if ((d.dwFlags & DDSD_MIPMAPCOUNT) && !(*caps & DDSCAPS_COMPLEX)) return DDERR_INVALIDCAPS;
  if (*caps & DDSCAPS_FLIP) {
    if (!(d.dwFlags & DDSD_BACKBUFFERCOUNT) || !d.dwBackBufferCount) return DDERR_INVALIDCAPS;
    if (!(*caps & DDSCAPS_COMPLEX)) return DDERR_INVALIDCAPS;
    if (*caps2 & DDSCAPS2_CUBEMAP) return DDERR_INVALIDPARAMS;      /* (a flipping chain of textures is made, as Windows) */
  } else if (d.dwFlags & DDSD_BACKBUFFERCOUNT) {
    return (*caps2 & DDSCAPS2_CUBEMAP) ? DDERR_INVALIDPARAMS : DDERR_INVALIDCAPS;
  }
  if (*caps & DDSCAPS_PRIMARYSURFACE) {
    if (*caps & DDSCAPS_TEXTURE) return DDERR_INVALIDCAPS;
    if ((*caps & DDSCAPS_COMPLEX) && !(*caps & DDSCAPS_FLIP)) return DDERR_INVALIDCAPS;
    if ((*caps & DDSCAPS_FLIP) && !(dd->coop & DDSCL_EXCLUSIVE)) return DDERR_NOEXCLUSIVEMODE;
    if (dd->primary) return DDERR_PRIMARYSURFACEALREADYEXISTS;
  }
  if ((*caps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY)) == (DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY)) return DDERR_INVALIDCAPS;
  if ((*caps & (DDSCAPS_ALLOCONLOAD | DDSCAPS_MIPMAP)) && !(*caps & DDSCAPS_TEXTURE)) return DDERR_INVALIDCAPS;
  if ((*caps2 & DDSCAPS2_CUBEMAP_ALLFACES) && !(*caps2 & DDSCAPS2_CUBEMAP)) return DDERR_INVALIDCAPS;
  if ((*caps2 & DDSCAPS2_CUBEMAP) && !(*caps2 & DDSCAPS2_CUBEMAP_ALLFACES)) return DDERR_INVALIDPARAMS;
  if ((*caps2 & DDSCAPS2_CUBEMAP) && (*caps2 & DDSCAPS2_CUBEMAP_ALLFACES) != DDSCAPS2_CUBEMAP_ALLFACES) return DDERR_INVALIDPARAMS;      /* (partial cube maps: no driver here has them) */
  if ((*caps2 & DDSCAPS2_CUBEMAP) && !(*caps & DDSCAPS_TEXTURE)) return DDERR_INVALIDCAPS;
  if (*caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) {
    if (!(*caps & DDSCAPS_TEXTURE)) return DDERR_INVALIDCAPS;
    if (*caps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY)) return DDERR_INVALIDCAPS;
  }
  if ((*caps & DDSCAPS_WRITEONLY) && !(*caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE))) return DDERR_INVALIDCAPS;
  if (*caps & DDSCAPS_OVERLAY) return DDERR_NOOVERLAYHW;      /* (no overlay hardware: the caps say none) */
  if (*caps & (DDSCAPS_VIDEOPORT | DDSCAPS_LIVEVIDEO | DDSCAPS_HWCODEC)) return DDERR_INVALIDCAPS;
  if (*caps2 & (DDSCAPS2_STEREOSURFACELEFT)) return DDERR_NOSTEREOHARDWARE;
  if (*caps2 & DDSCAPS2_VOLUME) return DDERR_INVALIDCAPS;
  DisplayMode mode;
  display_current(&mode);
  if (!(d.dwFlags & DDSD_PIXELFORMAT)) {
    d.dwFlags |= DDSD_PIXELFORMAT;
    ddpf_from_format(format_of_display_bpp(mode.bpp), &d.ddpfPixelFormat);
  }
  if (!(d.dwFlags & DDSD_WIDTH) || !(d.dwFlags & DDSD_HEIGHT)) {
    if (!(*caps & DDSCAPS_PRIMARYSURFACE)) return DDERR_INVALIDPARAMS;
    d.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = mode.width;
    d.dwHeight = mode.height;
  }
  if (!d.dwWidth || !d.dwHeight) return DDERR_INVALIDPARAMS;
  if (d.dwWidth > 16384 || d.dwHeight > 16384) return DDERR_INVALIDPARAMS;
  if (*caps & DDSCAPS_FLIP) *caps |= DDSCAPS_FRONTBUFFER;
  if (*caps & DDSCAPS_PRIMARYSURFACE) *caps |= DDSCAPS_VISIBLE;
  const UINT fmt = format_from_ddpf(&d.ddpfPixelFormat);
  /* (the program's memory is one surface: a complex mipmap or a cube map of it is DDERR_INVALIDCAPS, even of one level - Wine's test_user_memory;
   * DDSCAPS_MIPMAP alone makes one level in it) */
  if ((d.dwFlags & DDSD_LPSURFACE) && (*caps & DDSCAPS_COMPLEX) && ((*caps & DDSCAPS_MIPMAP) || (*caps2 & DDSCAPS2_CUBEMAP))) return DDERR_INVALIDCAPS;
  /* mipmap levels */
  UINT levels = 1;
  if (*caps & DDSCAPS_MIPMAP) {
    if (*caps & DDSCAPS_COMPLEX) {
      if (d.dwFlags & DDSD_MIPMAPCOUNT) {
        if (!d.dwMipMapCount) return DDERR_INVALIDPARAMS;
      } else {
        d.dwMipMapCount = (version == 7 ? log2_floor(d.dwWidth > d.dwHeight ? d.dwWidth : d.dwHeight) : log2_floor(d.dwWidth < d.dwHeight ? d.dwWidth : d.dwHeight)) + 1;
      }
    } else {
      d.dwMipMapCount = 1;
    }
    d.dwFlags |= DDSD_MIPMAPCOUNT;
    levels = d.dwMipMapCount;
    const UINT most = log2_floor(d.dwWidth > d.dwHeight ? d.dwWidth : d.dwHeight) + 1;
    if (levels > most) return DDERR_INVALIDPARAMS;
  }
  /* a DXTn surface is whole 4 x 4 blocks: another size is refused in any memory (Windows, Wine's test_block_formats_creation; its levels below 4 are
   * fine) */
  if (dd_format_is_blocks(fmt) && ((d.dwWidth & 3u) || (d.dwHeight & 3u))) return DDERR_INVALIDPARAMS;
  /* where the memory is: a format the "hardware" takes goes to video memory, the others to system memory (an object of the emulation only has no
   * video memory, and its z buffers are the HEL's: 16 and 32 bits) */
  BOOL sysmem_fallback = FALSE;
  if (dd->kind == DD_DRIVER_EMULATION) {
    if (*caps & DDSCAPS_VIDEOMEMORY) return DDERR_NODIRECTDRAWHW;
    if (fmt == DD_FMT_D24X8 || fmt == DD_FMT_D24X8H || fmt == DD_FMT_D24S8 || fmt == DD_FMT_D24X4S4) return DDERR_INVALIDPIXELFORMAT;
    *caps |= DDSCAPS_SYSTEMMEMORY;
  }
  if (!(*caps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY))) {
    if (!(*caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE))) {
      /* (a P8 texture is one the Direct3D part takes - EnumTextureFormats lists P8 - so it is in video memory as Windows puts the formats its
       * devices enumerate: Wine's test_unsupported_formats; other palette surfaces stay in system memory) */
      const BOOL hardware = fmt != DD_FMT_UNKNOWN && (!dd_format_is_palette(fmt) || (fmt == DD_FMT_P8 && (*caps & DDSCAPS_TEXTURE) && g_d3d.d3d7_vtbl)) &&
                            fmt != DD_FMT_R3G3B2 && fmt != DD_FMT_A8R3G3B2 && !((*caps & DDSCAPS_TEXTURE) && dd_format_is_yuv(fmt));
      if (hardware || (*caps & DDSCAPS_PRIMARYSURFACE)) *caps |= DDSCAPS_VIDEOMEMORY;
      else { *caps |= DDSCAPS_SYSTEMMEMORY; sysmem_fallback = TRUE; }
    } else if (!(*caps & DDSCAPS_TEXTURE)) {
      *caps |= DDSCAPS_LOCALVIDMEM | DDSCAPS_VIDEOMEMORY;
    }
  }
  if (*caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) *caps |= DDSCAPS_SYSTEMMEMORY;
  if (*caps & DDSCAPS_VIDEOMEMORY) *caps |= DDSCAPS_LOCALVIDMEM;
  if (d.dwFlags & DDSD_LPSURFACE) {
    if (!(*caps & DDSCAPS_SYSTEMMEMORY)) return DDERR_INVALIDCAPS;
    if (version < 4) return DDERR_INVALIDPARAMS;
    if (!d.lpSurface) return DDERR_INVALIDPARAMS;
    if (dd_format_is_blocks(fmt)) {
      if (version != 4 && (d.dwFlags & DDSD_PITCH)) return DDERR_INVALIDPARAMS;
      if (!(d.dwFlags & (DDSD_LINEARSIZE | DDSD_PITCH))) return DDERR_INVALIDPARAMS;
    } else if (!(d.dwFlags & DDSD_PITCH)) {
      return DDERR_INVALIDPARAMS;
    }
    if (levels > 1 || (*caps2 & DDSCAPS2_CUBEMAP) || (*caps & DDSCAPS_FLIP)) return DDERR_INVALIDPARAMS;      /* (one surface of the program's memory) */
  }
  if (((d.dwFlags & DDSD_CKDESTOVERLAY) && d.ddckCKDestOverlay.dwColorSpaceLowValue != d.ddckCKDestOverlay.dwColorSpaceHighValue) ||
      ((d.dwFlags & DDSD_CKDESTBLT) && d.ddckCKDestBlt.dwColorSpaceLowValue != d.ddckCKDestBlt.dwColorSpaceHighValue) ||
      ((d.dwFlags & DDSD_CKSRCOVERLAY) && d.ddckCKSrcOverlay.dwColorSpaceLowValue != d.ddckCKSrcOverlay.dwColorSpaceHighValue) ||
      ((d.dwFlags & DDSD_CKSRCBLT) && d.ddckCKSrcBlt.dwColorSpaceLowValue != d.ddckCKSrcBlt.dwColorSpaceHighValue))
    return DDERR_NOCOLORKEYHW;
  if (fmt == DD_FMT_UNKNOWN || !format_creatable(fmt, *caps)) return DDERR_INVALIDPIXELFORMAT;
  if (dd_format(fmt)->kind == DD_KIND_YUV_PLANAR && ((d.dwWidth & 1) || (d.dwHeight & 1))) return DDERR_INVALIDPARAMS;
  if ((d.dwFlags & DDSD_LPSURFACE) && !dd_format_is_blocks(fmt)) {
    if (d.lPitch < (LONG)dd_min_pitch(fmt, d.dwWidth) || (d.lPitch & 3)) return DDERR_INVALIDPARAMS;
  }
  if ((d.dwFlags & DDSD_LPSURFACE) && dd_format_is_blocks(fmt) && (d.dwFlags & DDSD_LINEARSIZE)) {
    if (d.dwLinearSize < dd_image_bytes(fmt, d.dwWidth, d.dwHeight, dd_min_pitch(fmt, d.dwWidth))) return DDERR_INVALIDPARAMS;
  }
#ifdef _WIN64
  /* (64-bit Windows has no Direct3D under DirectDraw: IDirectDraw7 refuses textures with E_NOINTERFACE, as its texture interface is missing.
   * With a Direct3D part registered here (g_d3d) there is one, and textures are made) */
  if (version == 7 && (*caps & DDSCAPS_TEXTURE) && !g_d3d.d3d7_vtbl) return E_NOINTERFACE;
#endif
  /* the surfaces */
  SurfaceGroup *g = mem_alloc(sizeof *g);
  if (!g) return DDERR_OUTOFMEMORY;
  g->dd = dd;
  g->ref = 1;      /* (the root alive) */
  ++dd->keep;
  InterlockedIncrement(&g_objects);
  HRESULT hr = DD_OK;
  const UINT faces = (*caps2 & DDSCAPS2_CUBEMAP) ? 6 : 1;
  Surface *root = NULL;
  for (UINT face = 0; face < faces && SUCCEEDED(hr); ++face) {
    Surface *prev = NULL;
    DWORD w = d.dwWidth, h = d.dwHeight;
    for (UINT level = 0; level < levels; ++level) {
      Surface *s = surface_alloc(dd, g, version);
      if (!s) { hr = DDERR_OUTOFMEMORY; break; }
      s->fmt = fmt;
      s->sysmem_fallback = sysmem_fallback;
      if (FAILED(hr = surface_init_level(s, &d, w, h, level, levels, face, faces))) break;
      s->desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
      if (!level) {
        if (!face) root = s;
        else root->complex[faces - 1 - face] = s;
      } else if (level == 1 && !face) {
        root->complex[faces - 1] = s;
      } else {
        prev->complex[0] = s;
      }
      prev = s;
      w = w > 1 ? w / 2 : 1;
      h = h > 1 ? h / 2 : 1;
    }
  }
  if (SUCCEEDED(hr) && (d.dwFlags & DDSD_BACKBUFFERCOUNT)) {
    root->flip_chain_start = TRUE;
    Surface *last = root;
    for (DWORD i = 0; i < d.dwBackBufferCount && SUCCEEDED(hr); ++i) {
      Surface *b = surface_alloc(dd, g, version);
      if (!b) { hr = DDERR_OUTOFMEMORY; break; }
      b->fmt = fmt;
      b->sysmem_fallback = sysmem_fallback;
      DDSURFACEDESC2 bd = root->desc;
      bd.ddsCaps.dwCaps &= ~(DWORD)(DDSCAPS_VISIBLE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER);
      if (!i) bd.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
      bd.dwBackBufferCount = 0;
      bd.dwFlags &= ~(DWORD)DDSD_LPSURFACE;
      b->desc = bd;
      if (FAILED(hr = surface_alloc_store(b, NULL))) break;
      last->complex[0] = b;
      last = b;
    }
    if (SUCCEEDED(hr)) last->complex[0] = root;
  }
  if (FAILED(hr)) {
    /* (nothing of it was given out: the group goes at once) */
    if (root && root->dd->primary == root) root->dd->primary = NULL;
    g->ref = 1;
    group_release(g);
    return hr;
  }
  root->implicit = FALSE;
  g->root = root;
  /* (the root's first reference: the caller sets the count of its interface; every other surface of the group has one reference of its own of the
   * creation version's interface, which the root holds and lets go when it is destroyed - as Windows: a back buffer from GetAttachedSurface has 2) */
  root->iface_count = 1;
  ++g->ref;
  for (UINT i = 0; i < g->count; ++i) {
    Surface *m = g->members[i];
    if (m == root) continue;
    if (version == 7) m->ref7 = 1; else if (version == 4) m->ref4 = 1; else m->ref1 = 1;
    m->iface_count = 1;
    ++g->ref;
  }
  if (*caps & DDSCAPS_PRIMARYSURFACE) {
    dd->primary = root;
    dd->gdi_mem = root->store.mem;
    for (UINT i = 0; i < g->count; ++i) g->members[i]->made_bpp = mode.bpp;
    if (!(dd->coop & DDSCL_EXCLUSIVE)) primary_read_screen(root);
  }
  /* (a new surface of an object whose surfaces are being lost is not lost) */
  ddraw_update_lost_surfaces(dd);
  for (UINT i = 0; i < g->count; ++i) g->members[i]->lost = FALSE;
  *out = root;
  return DD_OK;
}

/* The primary surface describes the display: after a mode change of another depth its flipping chain takes the new format (and new memory; it is
 * lost anyway, its size stays, as Windows' primary does). */
static void primary_follow_mode(DDraw *dd) {
  Surface *p = dd->primary;
  if (!p) return;
  DisplayMode m;
  display_current(&m);
  const UINT fmt = format_of_display_bpp(m.bpp);
  if (fmt == DD_FMT_UNKNOWN || fmt == p->fmt) return;
  Surface *x = p;
  do {
    if (x->lock_count || x->dc || x->store.shared || !x->store.own) break;
    const DWORD pitch = dd_aligned_pitch(fmt, x->desc.dwWidth, surface_pitch_align(x->desc.ddsCaps.dwCaps));
    const UINT64 bytes = dd_image_bytes(fmt, x->desc.dwWidth, x->desc.dwHeight, pitch);
    if (!bytes || bytes > 0x7FFFFFFFull) break;
    BYTE *mem = mem_alloc((SIZE_T)bytes);
    if (!mem) break;
    mem_free(x->store.mem);
    g_vidmem_used -= x->store.vidmem;
    x->store.mem = mem;
    x->store.size = (SIZE_T)bytes;
    x->store.vidmem = (x->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) ? (SIZE_T)bytes : 0;
    g_vidmem_used += x->store.vidmem;
    x->fmt = fmt;
    x->desc.lPitch = (LONG)pitch;
    ddpf_from_format(fmt, &x->desc.ddpfPixelFormat);
    cpu_wrote(x, NULL);
    x = x->complex[0];
  } while (x && x != p);
}

/* ---- descriptions ------------------------------------------------------------------------------------------------------------------------------------ */

static HRESULT surface_get_desc2(Surface *s, DDSURFACEDESC2 *out) {
  if (!out || out->dwSize != sizeof(DDSURFACEDESC2)) return DDERR_INVALIDPARAMS;
  dd_lock();
  *out = s->desc;
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_get_desc1(Surface *s, DDSURFACEDESC *out) {
  if (!out || out->dwSize != sizeof(DDSURFACEDESC)) return DDERR_INVALIDPARAMS;
  dd_lock();
  ddsd2_to_ddsd(&s->desc, out);
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_get_pixel_format(Surface *s, DDPIXELFORMAT *pf) {
  if (!pf) return DDERR_INVALIDPARAMS;
  dd_lock();
  copy_by_size(pf, pf->dwSize ? pf->dwSize : sizeof *pf, &s->desc.ddpfPixelFormat, sizeof(DDPIXELFORMAT));
  if (!pf->dwSize || pf->dwSize > sizeof *pf) pf->dwSize = sizeof *pf;
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_get_caps2(Surface *s, DDSCAPS2 *caps) {
  if (!caps) return DDERR_INVALIDPARAMS;
  *caps = s->desc.ddsCaps;
  return DD_OK;
}
static HRESULT surface_get_caps1(Surface *s, DDSCAPS *caps) {
  if (!caps) return DDERR_INVALIDPARAMS;
  caps->dwCaps = s->desc.ddsCaps.dwCaps;
  return DD_OK;
}

/* ---- attachments ------------------------------------------------------------------------------------------------------------------------------------- */

/* A version 1 surface that is not part of a complex surface may join a flipping chain (AddAttachedSurface of IDirectDrawSurface .. 3). */
static BOOL surface_attachable_for_flip(const Surface *s) {
  return s->version == 1 && !s->implicit && !(s->desc.ddsCaps.dwCaps & (DDSCAPS_ZBUFFER | DDSCAPS_TEXTURE | DDSCAPS_OVERLAY));
}
static void surface_backbuffer_count(Surface *s, int inc) {
  Surface *x = s;
  do {
    if (!x->implicit && (x->desc.dwFlags & DDSD_BACKBUFFERCOUNT)) { x->desc.dwBackBufferCount = (DWORD)((int)x->desc.dwBackBufferCount + inc); break; }
    x = x->complex[0];
  } while (x && x != s);
}
/* The AddAttachedSurface link from `s` to `a` (NULL: `a` is not attached to `s` that way). */
static AttachLink *attach_link_find(const Surface *s, const Surface *a) {
  for (AttachLink *l = s->attached; l; l = l->next)
    if (l->child == a) return l;
  return NULL;
}
static HRESULT surface_attach(Surface *s, Surface *a) {
  if (a == s) return DDERR_CANNOTATTACHSURFACE;
  if (surface_attachable_for_flip(a)) {
    if ((s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) && !(s->dd->coop & DDSCL_EXCLUSIVE)) return DDERR_NOEXCLUSIVEMODE;
    if (s->complex[0] == a) return DDERR_SURFACEALREADYATTACHED;
    if (a->complex[0] || a->desc.dwWidth != s->desc.dwWidth || a->desc.dwHeight != s->desc.dwHeight || a->fmt != s->fmt) return DDERR_CANNOTATTACHSURFACE;
    a->attach_saved_caps = a->desc.ddsCaps.dwCaps & ~(DWORD)DDSCAPS_FRONTBUFFER;
    a->flip_chain_start = FALSE;
    a->desc.ddsCaps.dwCaps |= DDSCAPS_FLIP;
    if (!s->complex[0]) {
      s->flip_chain_start = TRUE;
      s->complex[0] = a;
      a->complex[0] = s;
      s->attach_saved_caps = s->desc.ddsCaps.dwCaps & ~(DWORD)DDSCAPS_FRONTBUFFER;
      s->desc.ddsCaps.dwCaps |= DDSCAPS_FLIP;
      if (!(s->desc.ddsCaps.dwCaps & (DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER))) s->desc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
      if (s->desc.ddsCaps.dwCaps & DDSCAPS_BACKBUFFER) a->desc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
      else a->desc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
    } else {
      a->complex[0] = s->complex[0];
      s->complex[0] = a;
    }
    surface_backbuffer_count(s, 1);
    return DD_OK;
  }
  /* (a z buffer of another size than the surface's: DDERR_CANNOTATTACHSURFACE, as Windows - Wine's test_surface_attachment, smaller and larger) */
  if ((a->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) && !(s->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) &&
      (a->desc.dwWidth != s->desc.dwWidth || a->desc.dwHeight != s->desc.dwHeight))
    return DDERR_CANNOTATTACHSURFACE;
  /* (already attached here, or a surface that has attachments of its own - the reverse of an attachment among them: Wine's test_surface_attachment) */
  if (attach_link_find(s, a) || a->attached) return DDERR_SURFACEALREADYATTACHED;
  /* (a surface attached to another may be attached to this one too, as Windows: Wine's test_surface_attachment, a device's z buffer attached to a
   * second target in test_edge_antialiasing_blending) */
  AttachLink *link = mem_alloc(sizeof *link);
  if (!link) return DDERR_OUTOFMEMORY;
  link->child = a;
  link->iface = NULL;
  link->next = s->attached;
  s->attached = link;
  ++a->parents;
  return DD_OK;
}
/* AddAttachedSurface of each version: the attachment holds a reference of the interface it was given. */
static HRESULT surface_add_attached(Surface *s, Surface *a, IUnknown *iface, UINT call_version) {
  if (!a) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr;
  /* (an attachment that is there already, asked again through another interface version: DDERR_CANNOTATTACHSURFACE - ddraw7's
   * test_surface_attachment; through the same one DDERR_SURFACEALREADYATTACHED, below) */
  const AttachLink *there = attach_link_find(s, a);
  if (there && there->iface != iface) { dd_unlock(); return DDERR_CANNOTATTACHSURFACE; }
  if (call_version == 7) {
    hr = (a->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) ? surface_attach(s, a) : DDERR_CANNOTATTACHSURFACE;
  } else {
    const DWORD sc = s->desc.ddsCaps.dwCaps, ac = a->desc.ddsCaps.dwCaps;
    if ((sc & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN)) && (ac & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN))) {
      hr = (a->desc.dwWidth != s->desc.dwWidth || a->desc.dwHeight != s->desc.dwHeight) ? DDERR_CANNOTATTACHSURFACE : surface_attach(s, a);
    } else if ((!(sc & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_3DDEVICE)) || !(ac & DDSCAPS_ZBUFFER)) &&
               !(s->version == 1 && !(sc & (DDSCAPS_ZBUFFER | DDSCAPS_TEXTURE | DDSCAPS_OVERLAY)) && surface_attachable_for_flip(a))) {
      hr = DDERR_CANNOTATTACHSURFACE;
    } else {
      hr = surface_attach(s, a);
    }
  }
  if (SUCCEEDED(hr)) {
    AttachLink *link = attach_link_find(s, a);
    if (link) link->iface = iface;
    if (!link || !a->attached_iface) a->attached_iface = iface;
    IUnknown_AddRef(iface);
  }
  dd_unlock();
  return hr;
}
static HRESULT surface_detach(Surface *s, Surface *a, IUnknown *detach_iface) {
  if (!a) return DDERR_CANNOTDETACHSURFACE;
  if (surface_attachable_for_flip(a) && s->complex[0] == a && !(a->group == s->group)) {
    if (s->version != 1) return DDERR_SURFACENOTATTACHED;
    if (a->implicit || a->flip_chain_start) return DDERR_CANNOTDETACHSURFACE;
    surface_backbuffer_count(s, -1);
    Surface *next = s->complex[0] = a->complex[0];
    if (next == s) {
      s->flip_chain_start = FALSE;
      s->complex[0] = NULL;
      s->desc.ddsCaps.dwCaps = s->attach_saved_caps;
    } else {
      next->desc.ddsCaps.dwCaps &= ~(DWORD)(DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER);
      if (a->desc.ddsCaps.dwCaps & DDSCAPS_FRONTBUFFER) {
        s->desc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
        next->desc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
      } else {
        next->desc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
      }
    }
    a->desc.ddsCaps.dwCaps = a->attach_saved_caps;
    a->complex[0] = NULL;
    a->attached_iface = NULL;
    if (detach_iface) IUnknown_Release(detach_iface);
    return DD_OK;
  }
  AttachLink *link = attach_link_find(s, a);
  if (surface_attachable_for_flip(a) && s->complex[0] != a && !a->parents) return DDERR_SURFACENOTATTACHED;
  if (!link || a == s) return DDERR_CANNOTDETACHSURFACE;
  if (link->iface != detach_iface) return DDERR_SURFACENOTATTACHED;
  if (s->desc.ddsCaps.dwCaps & a->desc.ddsCaps.dwCaps & DDSCAPS_MIPMAP) a->desc.ddsCaps.dwCaps2 &= ~(DWORD)DDSCAPS2_MIPMAPSUBLEVEL;
  for (AttachLink **p = &s->attached; *p; p = &(*p)->next)
    if (*p == link) { *p = link->next; break; }
  --a->parents;
  /* (the interface that keeps the surface alive: another parent's, if it has one) */
  if (a->attached_iface == link->iface) {
    a->attached_iface = NULL;
    for (Surface *o = a->parents && a->dd ? a->dd->surfaces : NULL; o && !a->attached_iface; o = o->list_next)
      for (AttachLink *l = o->attached; l; l = l->next)
        if (l->child == a && l->iface) { a->attached_iface = l->iface; break; }
  }
  mem_free(link);
  if (detach_iface) IUnknown_Release(detach_iface);
  return DD_OK;
}
static HRESULT surface_delete_attached(Surface *s, DWORD flags, Surface *a, IUnknown *iface) {
  (void)flags;
  dd_lock();
  HRESULT hr = surface_detach(s, a, iface);
  dd_unlock();
  return hr;
}

/* The attached surface whose caps include `caps` (the complex ones first, then those attached with AddAttachedSurface), with a reference of its version
 * 7 interface (the callers turn it into theirs). Before version 7 only dwCaps counts. */
static HRESULT surface_get_attached(Surface *s, const DDSCAPS2 *in, UINT version, Surface **out) {
  *out = NULL;
  if (!in) return DDERR_INVALIDPARAMS;
  if (surface_is_lost(s)) return DDERR_SURFACELOST;
  DDSCAPS2 caps = *in;
  if (version < 7) caps.dwCaps2 = caps.dwCaps3 = caps.dwCaps4 = 0;
  dd_lock();
  Surface *found = NULL;
  for (UINT i = 0; i < MAX_COMPLEX && !found; ++i) {
    Surface *c = s->complex[i];
    if (!c) break;
    if ((c->desc.ddsCaps.dwCaps & caps.dwCaps) == caps.dwCaps && (c->desc.ddsCaps.dwCaps2 & caps.dwCaps2) == caps.dwCaps2) found = c;
  }
  for (AttachLink *l = s->attached; l && !found; l = l->next)
    if ((l->child->desc.ddsCaps.dwCaps & caps.dwCaps) == caps.dwCaps && (l->child->desc.ddsCaps.dwCaps2 & caps.dwCaps2) == caps.dwCaps2) found = l->child;
  dd_unlock();
  if (!found) return DDERR_NOTFOUND;
  *out = found;
  return DD_OK;
}

/* EnumAttachedSurfaces: the callback gets each attached surface's interface of the calling version with a reference, and its description. */
typedef HRESULT (CALLBACK *EnumAttached7)(IDirectDrawSurface7 *, DDSURFACEDESC2 *, void *);
static HRESULT surface_enum_attached(Surface *s, UINT version, void *context, void *callback) {
  if (!callback) return DDERR_INVALIDPARAMS;
  Surface *list[MAX_COMPLEX + 64];
  UINT n = 0;
  dd_lock();
  for (UINT i = 0; i < MAX_COMPLEX && s->complex[i]; ++i) list[n++] = s->complex[i];
  for (AttachLink *l = s->attached; l && n < sizeof list / sizeof list[0]; l = l->next) list[n++] = l->child;      /* (newest first) */
  dd_unlock();
  for (UINT i = 0; i < n; ++i) {
    Surface *c = list[i];
    HRESULT r;
    if (version >= 4) {
      DDSURFACEDESC2 d = c->desc;
      if (version == 7) { s7_AddRef(IFACE7(c)); r = ((EnumAttached7)callback)(IFACE7(c), &d, context); }
      else { s4_AddRef(IFACE4(c)); r = ((HRESULT (CALLBACK *)(IDirectDrawSurface4 *, DDSURFACEDESC2 *, void *))callback)(IFACE4(c), &d, context); }
    } else {
      DDSURFACEDESC d;
      ddsd2_to_ddsd(&c->desc, &d);
      void *iface;
      if (version == 3) { s3_AddRef(IFACE3(c)); iface = IFACE3(c); }
      else if (version == 2) { s2_AddRef(IFACE2(c)); iface = IFACE2(c); }
      else { s1_AddRef(IFACE1(c)); iface = IFACE1(c); }
      r = ((HRESULT (CALLBACK *)(IDirectDrawSurface *, DDSURFACEDESC *, void *))callback)((IDirectDrawSurface *)iface, &d, context);
    }
    if (r == DDENUMRET_CANCEL) break;
  }
  return DD_OK;
}

/* ---- Lock / Unlock ------------------------------------------------------------------------------------------------------------------------------------ */

static BOOL lock_desc_size(const Surface *s, const DDSURFACEDESC *d, DWORD *size) {
  if (!d) return FALSE;
  if (d->dwSize == sizeof(DDSURFACEDESC) || d->dwSize == sizeof(DDSURFACEDESC2)) { *size = d->dwSize; return TRUE; }
  /* (a version 7 texture in system memory takes any size, as Windows) */
  if (s->version == 7 && (s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE) && !(s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY)) {
    *size = d->dwSize >= sizeof(DDSURFACEDESC2) ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
    return TRUE;
  }
  return FALSE;
}
/* The address of a rectangle's first pixel (blocks: the block it is in). */
static BYTE *surface_address(const Surface *s, LONG x, LONG y) {
  const DdFormatInfo *f = dd_format(s->fmt);
  if (f->kind == DD_KIND_BLOCKS) return s->store.mem + (SIZE_T)(y / 4) * dd_min_pitch(s->fmt, s->desc.dwWidth) + (SIZE_T)(x / 4) * f->block_bytes;
  if (f->bytes) return s->store.mem + (SIZE_T)y * (SIZE_T)s->desc.lPitch + (SIZE_T)x * f->bytes;
  return s->store.mem + (SIZE_T)y * (SIZE_T)s->desc.lPitch + ((SIZE_T)x * f->bits) / 8;
}
/* Locks a surface: `out` gets its description with lpSurface pointing at the rectangle (`size`: a DDSURFACEDESC or a DDSURFACEDESC2 the caller
 * converts). */
static HRESULT surface_lock(Surface *s, RECT *rect, DDSURFACEDESC2 *out, DWORD size, DWORD flags, HANDLE event) {
  (void)event;
  dd_lock();
  out->lpSurface = NULL;
  if (rect) {
    if (rect->left < 0 || rect->top < 0 || rect->left > rect->right || rect->right > (LONG)s->desc.dwWidth || rect->top > rect->bottom ||
        rect->bottom > (LONG)s->desc.dwHeight) { dd_unlock(); return DDERR_INVALIDPARAMS; }
    /* (a rectangle of part of a block: system memory and managed surfaces take it, video memory ones do not - Windows Vista on, as d3d8/9; Wine's
     * dsurface test_partial_block_lock) */
    const BOOL vidmem = !(s->desc.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) && !(s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE));
    if (dd_format_is_blocks(s->fmt) && vidmem &&
        ((rect->left & 3) || (rect->top & 3) || ((rect->right & 3) && rect->right != (LONG)s->desc.dwWidth) ||
         ((rect->bottom & 3) && rect->bottom != (LONG)s->desc.dwHeight))) { dd_unlock(); return DDERR_INVALIDPARAMS; }
  }
  if (s->lock_count || s->dc) { dd_unlock(); return DDERR_SURFACEBUSY; }
  RECT r;
  if (rect) r = *rect; else rect_set(&r, 0, 0, (LONG)s->desc.dwWidth, (LONG)s->desc.dwHeight);
  cpu_access(s, &r);
  s->lock_count = 1;
  s->lock_rect = r;
  s->lock_whole = rect == NULL;
  s->lock_flags = flags;
  if (s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
    if (flags & DDLOCK_READONLY) rect_set(&s->dd->primary_lock, 0, 0, 0, 0);
    else s->dd->primary_lock = r;
  }
  const DWORD given_size = out->dwSize;
  copy_by_size(out, size, &s->desc, sizeof s->desc);
  out->dwSize = given_size; /* (a size Windows let through stays as the caller wrote it) */
  out->lpSurface = surface_address(s, r.left, r.top);
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_unlock(Surface *s, const RECT *rect) {
  (void)rect;
  dd_lock();
  if (!s->lock_count) { dd_unlock(); return DDERR_NOTLOCKED; }
  s->lock_count = 0;
  const RECT r = s->lock_rect;
  if (!(s->lock_flags & DDLOCK_READONLY)) {
    cpu_wrote(s, &r);
    if (surface_is_visible_front(s)) present_primary(s, &s->dd->primary_lock);
  }
  dd_unlock();
  return DD_OK;
}

/* ---- device contexts ------------------------------------------------------------------------------------------------------------------------------------
 * GetDC gives a GDI memory DC whose bitmap is a DIB on the surface's own memory (D3DKMTCreateDCFromMemory, as Windows' runtime makes it): what GDI draws
 * is in the surface at once, and the DIB's bits are the surface's. While the DC is out the surface is busy for Lock and Blt (a lock that came first
 * does not keep GetDC from working). Formats GDI can draw: RGB of 16, 24 and 32 bits (5-6-5, x1-5-5-5, 1-5-5-5, x4-4-4-4, 4-4-4-4, x8r8g8b8,
 * a8r8g8b8, a2r10g10b10) and 8-bit palette surfaces with the surface's palette, the primary's, or in an 8-bit display mode the system's; others
 * answer DDERR_CANTCREATEDC. */
/* (D3DKMT_CREATEDCFROMMEMORY / D3DKMT_DESTROYDCFROMMEMORY in the program's own layout: gdi32 converts a 32-bit one for the kernel) */
typedef struct { const void *pMemory; UINT Format, Width, Height, Pitch; HDC hDeviceDc; PALETTEENTRY *pColorTable; HDC hDc; HANDLE hBitmap; } KmtCreateDc;
typedef struct { HDC hDc; HANDLE hBitmap; } KmtDestroyDc;
NTSTATUS WINAPI D3DKMTCreateDCFromMemory(KmtCreateDc *);
NTSTATUS WINAPI D3DKMTDestroyDCFromMemory(const KmtDestroyDc *);

/* The D3DDDIFORMAT of a surface format GDI can draw (0: none). */
static UINT surface_kmt_format(UINT fmt) {
  switch (fmt) {
    case DD_FMT_R8G8B8: return 20;
    case DD_FMT_A8R8G8B8: return 21;
    case DD_FMT_X8R8G8B8: return 22;
    case DD_FMT_R5G6B5: return 23;
    case DD_FMT_X1R5G5B5: return 24;
    case DD_FMT_A1R5G5B5: return 25;
    case DD_FMT_A4R4G4B4: return 26;
    case DD_FMT_X4R4G4B4: return 30;
    case DD_FMT_A2R10G10B10: return 35;
    case DD_FMT_P8: return 41;
    default: return 0;
  }
}
static const Palette *surface_dc_palette(const Surface *s) {
  return s->palette ? s->palette : (s->dd->primary ? s->dd->primary->palette : NULL);
}
static void surface_dc_destroy(Surface *s) {
  if (s->dc) {
    const KmtDestroyDc d = {s->dc, s->dib};
    D3DKMTDestroyDCFromMemory(&d);
  }
  s->dc = NULL;
  s->dib = NULL;
}
static HRESULT surface_get_dc(Surface *s, HDC *out) {
  if (!out) return DDERR_INVALIDPARAMS;
  dd_lock();
  if (s->dc) { dd_unlock(); return DDERR_DCALREADYCREATED; }
  if (surface_is_lost(s)) { dd_unlock(); return DDERR_SURFACELOST; }
  const UINT format = surface_kmt_format(s->fmt);
  PALETTEENTRY table[256];
  BOOL ok = format != 0;
  if (ok && s->fmt == DD_FMT_P8) {
    const Palette *p = surface_dc_palette(s);
    if (p) {
      memcpy(table, p->entries, sizeof table);
    } else {
      /* (no palette: only in an 8-bit display mode, with the system's colours) */
      DisplayMode m;
      display_current(&m);
      HDC screen = m.bpp == 8 ? GetDC(NULL) : NULL;
      ok = screen && GetSystemPaletteEntries(screen, 0, 256, table) == 256;
      if (screen) ReleaseDC(NULL, screen);
    }
  }
  if (!ok) { dd_unlock(); *out = NULL; return DDERR_CANTCREATEDC; }
  cpu_access(s, NULL);
  KmtCreateDc c;
  zero(&c, sizeof c);
  c.pMemory = s->store.mem;
  c.Format = format;
  c.Width = s->desc.dwWidth;
  c.Height = s->desc.dwHeight;
  c.Pitch = (UINT)s->desc.lPitch;
  c.pColorTable = s->fmt == DD_FMT_P8 ? table : NULL;
  if (!KMT_OK(D3DKMTCreateDCFromMemory(&c))) { dd_unlock(); *out = NULL; return DDERR_CANTCREATEDC; }
  s->dc = c.hDc;
  s->dib = (HBITMAP)c.hBitmap;
  *out = s->dc;
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_release_dc(Surface *s, HDC dc) {
  dd_lock();
  if (!s->dc) { dd_unlock(); return DDERR_NODC; }
  if (dc != s->dc) { dd_unlock(); return DDERR_INVALIDPARAMS; }
  GdiFlush();
  surface_dc_destroy(s);
  cpu_wrote(s, NULL);
  if (surface_is_visible_front(s)) present_primary(s, NULL);
  dd_unlock();
  return DD_OK;
}

/* ---- colour keys --------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT surface_get_color_key(Surface *s, DWORD flags, DDCOLORKEY *key) {
  if (!key) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = DD_OK;
  switch (flags) {
    case DDCKEY_DESTBLT: if (s->desc.dwFlags & DDSD_CKDESTBLT) *key = s->desc.ddckCKDestBlt; else hr = DDERR_NOCOLORKEY; break;
    case DDCKEY_DESTOVERLAY: if (s->desc.dwFlags & DDSD_CKDESTOVERLAY) *key = s->desc.ddckCKDestOverlay; else hr = DDERR_NOCOLORKEY; break;
    case DDCKEY_SRCBLT: if (s->desc.dwFlags & DDSD_CKSRCBLT) *key = s->desc.ddckCKSrcBlt; else hr = DDERR_NOCOLORKEY; break;
    case DDCKEY_SRCOVERLAY: if (s->desc.dwFlags & DDSD_CKSRCOVERLAY) *key = s->desc.ddckCKSrcOverlay; else hr = DDERR_NOCOLORKEY; break;
    default: hr = DDERR_INVALIDPARAMS; break;
  }
  dd_unlock();
  return hr;
}
static HRESULT surface_set_color_key(Surface *s, DWORD flags, const DDCOLORKEY *key) {
  if (flags & DDCKEY_COLORSPACE) {
    if (key && key->dwColorSpaceLowValue != key->dwColorSpaceHighValue) return DDERR_NOCOLORKEYHW;
    flags &= ~(DWORD)DDCKEY_COLORSPACE;
  }
  DDCOLORKEY fixed;
  if (key) fixed.dwColorSpaceLowValue = fixed.dwColorSpaceHighValue = key->dwColorSpaceLowValue;
  dd_lock();
  DWORD bit;
  DDCOLORKEY *slot;
  switch (flags) {
    case DDCKEY_DESTBLT: bit = DDSD_CKDESTBLT; slot = &s->desc.ddckCKDestBlt; break;
    case DDCKEY_DESTOVERLAY: bit = DDSD_CKDESTOVERLAY; slot = &s->desc.ddckCKDestOverlay; break;
    case DDCKEY_SRCOVERLAY: bit = DDSD_CKSRCOVERLAY; slot = &s->desc.ddckCKSrcOverlay; break;
    case DDCKEY_SRCBLT: bit = DDSD_CKSRCBLT; slot = &s->desc.ddckCKSrcBlt; break;
    default: dd_unlock(); return DDERR_INVALIDPARAMS;
  }
  if (key) { *slot = fixed; s->desc.dwFlags |= bit; }
  else s->desc.dwFlags &= ~bit;
  /* (the Direct3D part sees the key change as a change of the texture's contents: colour keys become alpha there) */
  if (g_d3d.cpu_wrote) cpu_wrote(s, NULL);
  dd_unlock();
  return DD_OK;
}

/* ---- palettes and clippers --------------------------------------------------------------------------------------------------------------------------------- */

static void surface_set_palette(Surface *s, Palette *p) {
  Palette *prev = s->palette;
  if (s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
    if (prev) prev->flags &= ~(DWORD)DDPCAPS_PRIMARYSURFACE;
    if (p) p->flags |= DDPCAPS_PRIMARYSURFACE;
  }
  if (p) IDirectDrawPalette_AddRef((IDirectDrawPalette *)p);
  s->palette = p;
  if (prev) IDirectDrawPalette_Release((IDirectDrawPalette *)prev);
  if (p && (s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) present_primary(s, NULL);
}
static HRESULT surface_set_palette_checked(Surface *s, IDirectDrawPalette *iface, BOOL v7) {
  if (v7 && (s->desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) return DDERR_NOTONMIPMAPSUBLEVEL;
  if (surface_is_lost(s)) return DDERR_SURFACELOST;
  Palette *p = palette_from(iface);
  if (iface && !p) return DDERR_INVALIDPARAMS;
  if (p && (p->flags & DDPCAPS_ALPHA) && !(s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE)) return DDERR_INVALIDSURFACETYPE;
  if (!dd_format_is_palette(s->fmt)) return DDERR_INVALIDPIXELFORMAT;
  dd_lock();
  surface_set_palette(s, p);
  if (p) cpu_wrote(s, NULL);
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_get_palette(Surface *s, IDirectDrawPalette **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  if (surface_is_lost(s)) return DDERR_SURFACELOST;
  dd_lock();
  HRESULT hr = DD_OK;
  if (s->palette) { *out = (IDirectDrawPalette *)s->palette; IDirectDrawPalette_AddRef(*out); }
  else { *out = NULL; hr = DDERR_NOPALETTEATTACHED; }
  dd_unlock();
  return hr;
}
/* A palette's entries changed: the surfaces that use it (the primary on the screen, Direct3D's copies). A DC given out keeps the colours it had
 * at GetDC, as Windows: the next GetDC takes the new ones. */
static void surfaces_palette_changed(Palette *p) {
  for (DDraw *dd = g_ddraws; dd; dd = dd->next)
    for (Surface *s = dd->surfaces; s; s = s->list_next) {
      if (s->palette != p && !(s->fmt == DD_FMT_P8 && !s->palette && dd->primary && dd->primary->palette == p)) continue;
      if (s->palette == p) {
        if (g_d3d.cpu_wrote) cpu_wrote(s, NULL);
        if (s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) present_primary(s, NULL);
      }
    }
}
static HRESULT surface_set_clipper(Surface *s, IDirectDrawClipper *iface) {
  Clipper *c = iface ? (Clipper *)iface : NULL;
  if (iface && !clipper_alive(iface)) return DDERR_INVALIDPARAMS;
  dd_lock();
  if (c == s->clipper) { dd_unlock(); return DD_OK; }
  Clipper *old = s->clipper;
  s->clipper = c;
  if (c) IDirectDrawClipper_AddRef(iface);
  if (old && clipper_alive(old)) IDirectDrawClipper_Release((IDirectDrawClipper *)old);
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_get_clipper(Surface *s, IDirectDrawClipper **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  dd_lock();
  if (!s->clipper) { *out = NULL; dd_unlock(); return DDERR_NOCLIPPERATTACHED; }
  *out = (IDirectDrawClipper *)s->clipper;
  if (clipper_alive(s->clipper)) IDirectDrawClipper_AddRef(*out);
  dd_unlock();
  return DD_OK;
}

/* ---- private data -------------------------------------------------------------------------------------------------------------------------------------- */

static Private **private_find(Private **list, const GUID *g) {
  for (Private **p = list; *p; p = &(*p)->next) if (guid_eq(&(*p)->guid, g)) return p;
  return NULL;
}
static void private_drop(Private *p) {
  if (p->object) IUnknown_Release((IUnknown *)p->data); else mem_free(p->data);
  mem_free(p);
}
static void private_free_all(Private **list) {
  while (*list) { Private *p = *list; *list = p->next; private_drop(p); }
}
static HRESULT private_set(Private **list, REFGUID guid, const void *data, DWORD size, DWORD flags) {
  if (!guid) return DDERR_INVALIDPARAMS;
  if (!data) return DDERR_INVALIDPARAMS;
  if (flags & ~(DWORD)(DDSPD_IUNKNOWNPOINTER | DDSPD_VOLATILE)) return DDERR_INVALIDPARAMS;
  if ((flags & DDSPD_IUNKNOWNPOINTER) && size != sizeof(IUnknown *)) return DDERR_INVALIDPARAMS;
  Private *p = mem_alloc(sizeof *p);
  if (!p) return DDERR_OUTOFMEMORY;
  p->guid = *guid;
  if (flags & DDSPD_IUNKNOWNPOINTER) {
    p->object = TRUE;
    p->data = (void *)data;
    p->size = sizeof(IUnknown *);
    IUnknown_AddRef((IUnknown *)data);
  } else {
    p->data = mem_alloc(size);
    if (!p->data) { mem_free(p); return DDERR_OUTOFMEMORY; }
    memcpy(p->data, data, size);
    p->size = size;
  }
  dd_lock();
  Private **old = private_find(list, guid);
  if (old) { Private *dead = *old; *old = dead->next; private_drop(dead); }
  p->next = *list;
  *list = p;
  dd_unlock();
  return DD_OK;
}
static HRESULT private_get(Private **list, REFGUID guid, void *data, DWORD *size) {
  if (!guid) return DDERR_INVALIDPARAMS;
  dd_lock();
  Private **p = private_find(list, guid);
  HRESULT hr = DD_OK;
  if (!p) hr = DDERR_NOTFOUND;
  else if (!size) hr = DDERR_INVALIDPARAMS;
  else if (*size < (*p)->size) { *size = (*p)->size; hr = DDERR_MOREDATA; }
  else if (!data) hr = DDERR_INVALIDPARAMS;
  else {
    if ((*p)->object) memcpy(data, &(*p)->data, sizeof(void *));
    else memcpy(data, (*p)->data, (*p)->size);
    *size = (*p)->size;
  }
  dd_unlock();
  return hr;
}
static HRESULT private_free(Private **list, REFGUID guid) {
  if (!guid) return DDERR_INVALIDPARAMS;
  dd_lock();
  Private **p = private_find(list, guid);
  if (!p) { dd_unlock(); return DDERR_NOTFOUND; }
  Private *dead = *p;
  *p = dead->next;
  dd_unlock();
  private_drop(dead);
  return DD_OK;
}

/* ---- the rest ------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT surface_restore(Surface *s) {
  dd_lock();
  if (s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
    DisplayMode m;
    display_current(&m);
    if (m.width != s->desc.dwWidth || m.height != s->desc.dwHeight || format_of_display_bpp(m.bpp) != format_of_display_bpp(s->made_bpp)) { dd_unlock(); return DDERR_WRONGMODE; }
  }
  if (!surface_can_be_lost(s)) { dd_unlock(); return DD_OK; }
  ddraw_update_lost_surfaces(s->dd);
  if (s->dd->device_state == DD_STATE_LOST) { dd_unlock(); return DDERR_WRONGMODE; }
  /* (a restored surface's contents are undefined: its memory is kept; Direct3D's copy is out of date) */
  s->lost = FALSE;
  for (UINT i = 0; i < MAX_COMPLEX; ++i) {
    Surface *c = s->complex[i];
    if (!c || c == s) continue;
    if (c->group == s->group && c->implicit) c->lost = FALSE;
  }
  /* (the root restores every surface made with it: each back buffer of a flipping chain, not only the first - Wine's test_lost_device) */
  if (s->group && s->group->root == s)
    for (UINT i = 0; i < s->group->count; ++i) {
      Surface *c = s->group->members[i];
      if (c && c != s && c->implicit) c->lost = FALSE;
    }
  cpu_wrote(s, NULL);
  if (surface_is_visible_front(s)) present_primary(s, NULL);
  dd_unlock();
  return DD_OK;
}

/* The DirectDraw interface the surface was made with (its version's), with a reference. */
static HRESULT surface_get_dd_interface(Surface *s, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  DDraw *dd = s->dd;
  switch (s->version) {
    case 7: *out = DDI7(dd); break;
    case 4: *out = DDI4(dd); break;
    case 2: *out = DDI2(dd); break;
    default: *out = DDI1(dd); break;
  }
  IUnknown_AddRef((IUnknown *)*out);
  return DD_OK;
}

static HRESULT surface_set_priority(Surface *s, DWORD priority) {
  if (!(s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE))) return DDERR_INVALIDPARAMS;
  dd_lock();
  s->priority = priority;
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_get_priority(Surface *s, DWORD *priority) {
  if (s->desc.ddsCaps.dwCaps & DDSCAPS_OFFSCREENPLAIN) return DDERR_INVALIDOBJECT;
  if (!(s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) || s->implicit) return DDERR_INVALIDPARAMS;
  if (!priority) return DDERR_INVALIDPARAMS;
  *priority = s->priority;
  return DD_OK;
}
static HRESULT surface_set_lod(Surface *s, DWORD lod) {
  if (!(s->desc.ddsCaps.dwCaps2 & DDSCAPS2_TEXTUREMANAGE)) return DDERR_INVALIDOBJECT;
  if (lod >= s->desc.dwMipMapCount && s->desc.dwMipMapCount) return DDERR_INVALIDPARAMS;
  dd_lock();
  s->lod = lod;
  if (g_d3d.cpu_wrote) cpu_wrote(s, NULL);      /* (the Direct3D part reads s->lod when it binds the texture) */
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_get_lod(Surface *s, DWORD *lod) {
  if (!lod) return DDERR_INVALIDPARAMS;
  if (!(s->desc.ddsCaps.dwCaps2 & DDSCAPS2_TEXTUREMANAGE)) return DDERR_INVALIDOBJECT;
  *lod = s->lod;
  return DD_OK;
}

/* SetSurfaceDesc: a system-memory surface takes the program's memory (and with it a new size, pitch or format). */
static HRESULT surface_set_desc(Surface *s, const DDSURFACEDESC2 *d, DWORD flags) {
  if (!d) return DDERR_INVALIDPARAMS;
  if (flags) return DDERR_INVALIDPARAMS;
  const DWORD c = s->desc.ddsCaps.dwCaps;
  if (!(c & DDSCAPS_SYSTEMMEMORY) || (c & DDSCAPS_PRIMARYSURFACE) || (s->desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)))
    return DDERR_INVALIDSURFACETYPE;
  const DWORD allowed = DDSD_LPSURFACE | DDSD_PIXELFORMAT | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_CAPS;
  if (d->dwFlags & ~allowed) return DDERR_INVALIDPARAMS;
  if (!(d->dwFlags & DDSD_LPSURFACE) || !d->lpSurface) return DDERR_INVALIDPARAMS;
  if ((d->dwFlags & DDSD_CAPS) && d->ddsCaps.dwCaps) return DDERR_INVALIDCAPS;
  DWORD width = s->desc.dwWidth, height = s->desc.dwHeight;
  LONG pitch = s->desc.lPitch;
  if (d->dwFlags & DDSD_WIDTH) {
    if (!(d->dwFlags & DDSD_PITCH)) return DDERR_INVALIDPARAMS;
    if (!d->dwWidth || d->lPitch <= 0 || (d->lPitch & 3)) return DDERR_INVALIDPARAMS;
    width = d->dwWidth;
    pitch = d->lPitch;
  } else if (d->dwFlags & DDSD_PITCH) {
    return DDERR_INVALIDPARAMS;
  }
  if (d->dwFlags & DDSD_HEIGHT) {
    if (!d->dwHeight) return DDERR_INVALIDPARAMS;
    height = d->dwHeight;
  }
  UINT fmt = s->fmt;
  if (d->dwFlags & DDSD_PIXELFORMAT) {
    fmt = format_from_ddpf(&d->ddpfPixelFormat);
    if (fmt == DD_FMT_UNKNOWN) return DDERR_INVALIDPARAMS;
  }
  if (!dd_format_is_blocks(fmt) && (DWORD)pitch < dd_min_pitch(fmt, width)) return DDERR_INVALIDPARAMS;
  const UINT64 bytes = dd_image_bytes(fmt, width, height, dd_format_is_blocks(fmt) ? dd_min_pitch(fmt, width) : (uint32_t)pitch);
  if (!bytes || bytes > 0x7FFFFFFFull) return DDERR_INVALIDPARAMS;
  dd_lock();
  if (s->lock_count || s->dc) { dd_unlock(); return DDERR_SURFACEBUSY; }
  if (s->store.shared) {
    if (!--*s->store.shared) { mem_free(s->store.mem); mem_free(s->store.shared); }
    s->store.shared = NULL;
  } else if (s->store.own) {
    mem_free(s->store.mem);
  }
  s->store.mem = d->lpSurface;
  s->store.size = (SIZE_T)bytes;
  s->store.own = FALSE;
  s->desc.dwWidth = width;
  s->desc.dwHeight = height;
  s->desc.lPitch = pitch;
  if (d->dwFlags & DDSD_PIXELFORMAT) { s->fmt = fmt; s->desc.ddpfPixelFormat = d->ddpfPixelFormat; }
  cpu_wrote(s, NULL);
  dd_unlock();
  return DD_OK;
}

/* ---- the gamma ramp (the primary surface's: applied when it is shown) --------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE gamma_QueryInterface(IDirectDrawGammaControl *i, REFIID riid, void **out) { return surface_query(SG(i), riid, out); }
static HRESULT STDMETHODCALLTYPE gamma_GetGammaRamp(IDirectDrawGammaControl *i, DWORD flags, DDGAMMARAMP *ramp) {
  Surface *s = SG(i);
  (void)flags;
  if (!ramp) return DDERR_INVALIDPARAMS;
  if (!(s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) return DDERR_UNSUPPORTED;      /* (gamma of the primary only: DDCAPS2_PRIMARYGAMMA) */
  dd_lock();
  *ramp = s->gamma;
  dd_unlock();
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE gamma_SetGammaRamp(IDirectDrawGammaControl *i, DWORD flags, DDGAMMARAMP *ramp) {
  Surface *s = SG(i);
  if (!ramp) return DDERR_INVALIDPARAMS;
  if (flags & ~(DWORD)DDSGR_CALIBRATE) return DDERR_INVALIDPARAMS;
  if (!(s->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) return DDERR_UNSUPPORTED;
  dd_lock();
  s->gamma = *ramp;
  BOOL identity = TRUE;
  for (UINT k = 0; k < 256 && identity; ++k)
    if ((ramp->red[k] >> 8) != k || (ramp->green[k] >> 8) != k || (ramp->blue[k] >> 8) != k) identity = FALSE;
  s->gamma_set = !identity;
  present_primary(s, NULL);
  dd_unlock();
  return DD_OK;
}
IDirectDrawGammaControlVtbl gamma_vtbl = {
  .QueryInterface = gamma_QueryInterface, .AddRef = gamma_AddRef, .Release = gamma_Release, .GetGammaRamp = gamma_GetGammaRamp, .SetGammaRamp = gamma_SetGammaRamp,
};
