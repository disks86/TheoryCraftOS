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
/* IDirectDrawPalette and IDirectDrawClipper. Included by ddraw.c.
 *
 * A palette has 2, 4, 16 or 256 entries (DDPCAPS_1BIT .. 8BIT); without DDPCAPS_ALLOW256 a 256-entry palette keeps entry 0 black and entry 255 white
 * (the system colours); DDPCAPS_8BITENTRIES palettes take and give one byte an entry (an index into another palette); DDPCAPS_ALPHA ones keep alpha in
 * peFlags. Setting the entries of the primary surface's palette shows the primary again (an 8-bit display mode is drawn through it), and a surface
 * whose device context is out gets the new colours in its DIB.
 *
 * A clipper clips blits into a surface: to a window (SetHWnd: what of its client area is visible) or to a list of rectangles (SetClipList). Like
 * Windows' it checks that it was handed a live clipper (a stale or made-up pointer gets DDERR_INVALIDPARAMS, or 0 from AddRef and Release). */

/* ---- palettes ---------------------------------------------------------------------------------------------------------------------------------------- */

extern IDirectDrawPaletteVtbl palette_vtbl;

static UINT palette_size(DWORD flags) {
  switch (flags & (DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT)) {
    case DDPCAPS_1BIT: return 2;
    case DDPCAPS_2BIT: return 4;
    case DDPCAPS_4BIT: return 16;
    case DDPCAPS_8BIT: return 256;
    default: return 0;
  }
}
static void palette_store(Palette *p, DWORD start, DWORD count, const PALETTEENTRY *entries) {
  if (p->flags & DDPCAPS_8BITENTRIES) {
    const BYTE *e = (const BYTE *)entries;
    for (DWORD i = 0; i < count; ++i) { p->entries[start + i].peRed = e[i]; p->entries[start + i].peGreen = 0; p->entries[start + i].peBlue = 0; p->entries[start + i].peFlags = 0; }
  } else {
    for (DWORD i = 0; i < count; ++i) p->entries[start + i] = entries[i];
  }
  if (p->count == 256 && !(p->flags & DDPCAPS_ALLOW256)) {
    p->entries[0].peRed = p->entries[0].peGreen = p->entries[0].peBlue = 0;
    p->entries[255].peRed = p->entries[255].peGreen = p->entries[255].peBlue = 0xFF;
  }
}
static void surfaces_palette_changed(Palette *p);

static HRESULT palette_create(DDraw *dd, DWORD flags, const PALETTEENTRY *entries, Palette **out) {
  const UINT n = palette_size(flags);
  if (!n) return DDERR_INVALIDPARAMS;
  if (flags & ~(DWORD)(DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT | DDPCAPS_8BITENTRIES | DDPCAPS_ALLOW256 | DDPCAPS_PRIMARYSURFACE |
                       DDPCAPS_PRIMARYSURFACELEFT | DDPCAPS_ALPHA | DDPCAPS_INITIALIZE | DDPCAPS_VSYNC)) return DDERR_INVALIDPARAMS;
  if ((flags & DDPCAPS_8BITENTRIES) && n == 256) return DDERR_INVALIDPARAMS;      /* (indices into another palette: a palette of 1, 2 or 4 bits) */
  if (!entries) return DDERR_INVALIDPARAMS;
  Palette *p = mem_alloc(sizeof *p);
  if (!p) return DDERR_OUTOFMEMORY;
  p->lpVtbl = &palette_vtbl;
  p->ref = 1;
  p->flags = flags & ~(DWORD)(DDPCAPS_PRIMARYSURFACE | DDPCAPS_INITIALIZE);
  p->count = n;
  p->dd = dd;
  palette_store(p, 0, n, entries);
  ++dd->keep;
  InterlockedIncrement(&g_objects);
  *out = p;
  return DD_OK;
}

static HRESULT STDMETHODCALLTYPE pal_QueryInterface(IDirectDrawPalette *iface, REFIID riid, void **out) {
  if (!out) return DDERR_INVALIDPARAMS;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDirectDrawPalette)) {
    IDirectDrawPalette_AddRef(iface);
    *out = iface;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE pal_AddRef(IDirectDrawPalette *iface) { return (ULONG)InterlockedIncrement(&((Palette *)iface)->ref); }
static ULONG STDMETHODCALLTYPE pal_Release(IDirectDrawPalette *iface) {
  Palette *p = (Palette *)iface;
  const LONG r = InterlockedDecrement(&p->ref);
  if (!r) {
    dd_lock();
    DDraw *dd = p->dd;
    if ((p->flags & DDPCAPS_PRIMARYSURFACE) && dd->primary && dd->primary->palette == p) dd->primary->palette = NULL;
    IUnknown *release = p->release_iface;
    mem_free(p);
    InterlockedDecrement(&g_objects);
    --dd->keep;
    dd_unlock();
    if (release) IUnknown_Release(release);
    ddraw_free_if_unused(dd);
  }
  return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE pal_GetCaps(IDirectDrawPalette *iface, DWORD *caps) {
  if (!caps) return DDERR_INVALIDPARAMS;
  dd_lock();
  *caps = ((Palette *)iface)->flags;
  dd_unlock();
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE pal_GetEntries(IDirectDrawPalette *iface, DWORD flags, DWORD start, DWORD count, PALETTEENTRY *entries) {
  Palette *p = (Palette *)iface;
  if (!entries || flags) return DDERR_INVALIDPARAMS;
  if (start > p->count || count > p->count - start) return DDERR_INVALIDPARAMS;
  dd_lock();
  if (p->flags & DDPCAPS_8BITENTRIES) {
    BYTE *e = (BYTE *)entries;
    for (DWORD i = 0; i < count; ++i) e[i] = p->entries[start + i].peRed;
  } else {
    for (DWORD i = 0; i < count; ++i) entries[i] = p->entries[start + i];
  }
  dd_unlock();
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE pal_SetEntries(IDirectDrawPalette *iface, DWORD flags, DWORD start, DWORD count, PALETTEENTRY *entries) {
  Palette *p = (Palette *)iface;
  if (!entries || flags) return DDERR_INVALIDPARAMS;
  if (start > p->count || count > p->count - start) return DDERR_INVALIDPARAMS;
  dd_lock();
  palette_store(p, start, count, entries);
  surfaces_palette_changed(p);
  dd_unlock();
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE pal_Initialize(IDirectDrawPalette *iface, IDirectDraw *dd, DWORD flags, PALETTEENTRY *entries) {
  (void)iface; (void)dd; (void)flags; (void)entries;
  return DDERR_ALREADYINITIALIZED;
}
IDirectDrawPaletteVtbl palette_vtbl = {
  .QueryInterface = pal_QueryInterface, .AddRef = pal_AddRef, .Release = pal_Release, .GetCaps = pal_GetCaps, .GetEntries = pal_GetEntries,
  .Initialize = pal_Initialize, .SetEntries = pal_SetEntries,
};
static Palette *palette_from(IDirectDrawPalette *iface) {
  return iface && iface->lpVtbl == &palette_vtbl ? (Palette *)iface : NULL;
}

/* ---- clippers ---------------------------------------------------------------------------------------------------------------------------------------- */

extern IDirectDrawClipperVtbl clipper_vtbl;

typedef struct ClipperNode { Clipper *c; struct ClipperNode *next; } ClipperNode;
static ClipperNode *g_clippers;      /* every live clipper (the check of a clipper's pointer) */

static BOOL clipper_alive(const void *p) {
  BOOL ok = FALSE;
  dd_lock();
  for (ClipperNode *n = g_clippers; n; n = n->next) if ((const void *)n->c == p) { ok = TRUE; break; }
  dd_unlock();
  return ok;
}
static HRESULT clipper_create(DWORD flags, Clipper **out) {
  if (flags) return DDERR_INVALIDPARAMS;
  Clipper *c = mem_alloc(sizeof *c);
  ClipperNode *n = mem_alloc(sizeof *n);
  if (!c || !n) { mem_free(c); mem_free(n); return DDERR_OUTOFMEMORY; }
  c->lpVtbl = &clipper_vtbl;
  c->ref = 1;
  c->initialized = TRUE;
  n->c = c;
  dd_lock();
  n->next = g_clippers;
  g_clippers = n;
  dd_unlock();
  InterlockedIncrement(&g_objects);
  *out = c;
  return DD_OK;
}

/* The region a clipper clips to, in screen coordinates for a window (what of the window's client area is visible) and as given for a clip list. NULL
 * with *hr set: none. The caller deletes the region. */
static HRGN clipper_region(Clipper *c, HRESULT *hr) {
  if (c->window) {
    if (!IsWindow(c->window)) { *hr = E_FAIL; return NULL; }      /* (the window is gone: Windows answers E_FAIL) */
    HDC dc = GetDC(c->window);
    if (!dc) { *hr = E_FAIL; return NULL; }
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    if (rgn && GetRandomRgn(dc, rgn, SYSRGN) != 1) {
      /* (no visible region from GDI: the client area on the screen) */
      RECT r;
      POINT o = {0, 0};
      GetClientRect(c->window, &r);
      ClientToScreen(c->window, &o);
      OffsetRect(&r, o.x, o.y);
      SetRectRgn(rgn, r.left, r.top, r.right, r.bottom);
    }
    ReleaseDC(c->window, dc);
    if (!rgn) *hr = E_FAIL;
    return rgn;
  }
  if (!c->region) { *hr = DDERR_NOCLIPLIST; return NULL; }
  HRGN copy = CreateRectRgn(0, 0, 0, 0);
  if (!copy || CombineRgn(copy, c->region, NULL, RGN_COPY) == ERROR) { if (copy) DeleteObject(copy); *hr = E_FAIL; return NULL; }
  return copy;
}

static HRESULT STDMETHODCALLTYPE clip_QueryInterface(IDirectDrawClipper *iface, REFIID riid, void **out) {
  if (!clipper_alive(iface)) return DDERR_INVALIDPARAMS;
  if (!out) return DDERR_INVALIDPARAMS;
  if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IDirectDrawClipper)) {
    InterlockedIncrement(&((Clipper *)iface)->ref);
    *out = iface;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE clip_AddRef(IDirectDrawClipper *iface) {
  if (!clipper_alive(iface)) return 0;
  return (ULONG)InterlockedIncrement(&((Clipper *)iface)->ref);
}
static ULONG STDMETHODCALLTYPE clip_Release(IDirectDrawClipper *iface) {
  /* (Release, unlike the other methods, also wants the clipper's own vtable: with a broken one it answers 0 and changes nothing, as Windows) */
  if (!clipper_alive(iface) || iface->lpVtbl != &clipper_vtbl) return 0;
  Clipper *c = (Clipper *)iface;
  const LONG r = InterlockedDecrement(&c->ref);
  if (!r) {
    dd_lock();
    for (ClipperNode **n = &g_clippers; *n; n = &(*n)->next)
      if ((*n)->c == c) { ClipperNode *dead = *n; *n = dead->next; mem_free(dead); break; }
    dd_unlock();
    if (c->region) DeleteObject(c->region);
    mem_free(c);
    InterlockedDecrement(&g_objects);
  }
  return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE clip_GetClipList(IDirectDrawClipper *iface, RECT *rect, RGNDATA *data, DWORD *size) {
  if (!clipper_alive(iface)) return DDERR_INVALIDPARAMS;
  Clipper *c = (Clipper *)iface;
  if (!size) return DDERR_INVALIDPARAMS;
  HRESULT hr = DD_OK;
  dd_lock();
  HRGN rgn = clipper_region(c, &hr);
  dd_unlock();
  if (!rgn) return hr;
  if (rect) {
    HRGN r = CreateRectRgnIndirect(rect);
    if (!r || CombineRgn(rgn, rgn, r, RGN_AND) == ERROR) { if (r) DeleteObject(r); DeleteObject(rgn); return E_FAIL; }
    DeleteObject(r);
  }
  const DWORD needed = GetRegionData(rgn, 0, NULL);
  if (data) {
    if (*size < needed) { *size = needed; DeleteObject(rgn); return DDERR_REGIONTOOSMALL; }
    *size = GetRegionData(rgn, *size, data);
  } else {
    *size = needed;
  }
  DeleteObject(rgn);
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE clip_GetHWnd(IDirectDrawClipper *iface, HWND *window) {
  if (!clipper_alive(iface) || !window) return DDERR_INVALIDPARAMS;
  dd_lock();
  *window = ((Clipper *)iface)->window;
  dd_unlock();
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE clip_Initialize(IDirectDrawClipper *iface, IDirectDraw *dd, DWORD flags) {
  if (!clipper_alive(iface)) return DDERR_INVALIDPARAMS;
  (void)dd;
  if (flags) return DDERR_INVALIDPARAMS;
  Clipper *c = (Clipper *)iface;
  dd_lock();
  const BOOL was = c->initialized;
  c->initialized = TRUE;
  dd_unlock();
  return was ? DDERR_ALREADYINITIALIZED : DD_OK;
}
/* Has what of the window is visible changed since the program last asked? (A clip list never changes by itself.) */
static HRESULT STDMETHODCALLTYPE clip_IsClipListChanged(IDirectDrawClipper *iface, BOOL *changed) {
  if (!clipper_alive(iface) || !changed) return DDERR_INVALIDPARAMS;
  Clipper *c = (Clipper *)iface;
  dd_lock();
  BOOL now = FALSE;
  if (c->window && IsWindow(c->window)) {
    HRESULT hr = DD_OK;
    HRGN rgn = clipper_region(c, &hr);
    RECT box = {0, 0, 0, 0};
    if (rgn) { GetRgnBox(rgn, &box); DeleteObject(rgn); }
    now = !c->last_valid || !EqualRect(&box, &c->last_window_rect);
    c->last_window_rect = box;
    c->last_valid = TRUE;
  }
  dd_unlock();
  *changed = now;
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE clip_SetClipList(IDirectDrawClipper *iface, RGNDATA *data, DWORD flags) {
  if (!clipper_alive(iface)) return DDERR_INVALIDPARAMS;
  Clipper *c = (Clipper *)iface;
  if (flags) return DDERR_INVALIDPARAMS;
  dd_lock();
  if (c->window) { dd_unlock(); return DDERR_CLIPPERISUSINGHWND; }
  HRGN rgn = NULL;
  if (data) {
    if (data->rdh.dwSize < sizeof(RGNDATAHEADER) || data->rdh.iType != RDH_RECTANGLES) { dd_unlock(); return DDERR_INVALIDCLIPLIST; }
    rgn = ExtCreateRegion(NULL, (DWORD)(sizeof(RGNDATAHEADER) + data->rdh.nCount * sizeof(RECT)), data);
    if (!rgn) { dd_unlock(); return DDERR_INVALIDCLIPLIST; }
  }
  if (c->region) DeleteObject(c->region);
  c->region = rgn;
  dd_unlock();
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE clip_SetHWnd(IDirectDrawClipper *iface, DWORD flags, HWND window) {
  if (!clipper_alive(iface)) return DDERR_INVALIDPARAMS;
  if (flags) return DDERR_INVALIDPARAMS;
  if (window && !IsWindow(window)) return DDERR_INVALIDPARAMS;
  Clipper *c = (Clipper *)iface;
  dd_lock();
  c->window = window;
  c->last_valid = FALSE;
  dd_unlock();
  return DD_OK;
}
IDirectDrawClipperVtbl clipper_vtbl = {
  .QueryInterface = clip_QueryInterface, .AddRef = clip_AddRef, .Release = clip_Release, .GetClipList = clip_GetClipList, .GetHWnd = clip_GetHWnd,
  .Initialize = clip_Initialize, .IsClipListChanged = clip_IsClipListChanged, .SetClipList = clip_SetClipList, .SetHWnd = clip_SetHWnd,
};
static Clipper *clipper_from(IDirectDrawClipper *iface) { return iface && clipper_alive(iface) ? (Clipper *)iface : NULL; }
