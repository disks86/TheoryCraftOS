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
/* The vtables of every interface version: thin methods that find the object behind the interface pointer and call the shared code with the version's
 * structures (DDSURFACEDESC <-> DDSURFACEDESC2, DDSCAPS <-> DDSCAPS2) and the version's interface pointers. The vtables are filled by name, so the
 * compiler checks every method's type against mingw's ddraw.h. Included by ddraw.c.
 *
 * A surface pointer a program passes in may be any version's (Windows takes one version's pointer where another's is declared, and Wine's tests do
 * it): surface_from_any finds the surface by the vtable the pointer has. */

static Surface *surface_from_any(const void *iface) {
  if (!iface) return NULL;
  const void *vt = *(const void *const *)iface;
  if (vt == &surf7_vtbl) return S7(iface);
  if (vt == &surf4_vtbl) return S4(iface);
  if (vt == &surf3_vtbl) return S3(iface);
  if (vt == &surf2_vtbl) return S2(iface);
  if (vt == &surf1_vtbl) return S1(iface);
  return NULL;
}
#define BAD_SURFACE(iface, s) ((iface) && !(s))

/* ---- the surface interfaces ---------------------------------------------------------------------------------------------------------------------- */

/* Methods whose shape is the same in every version (only the interface types differ). */
#define SURF_COMMON(V, T)                                                                                                                        \
  static HRESULT STDMETHODCALLTYPE s##V##_QueryInterface(T *i, REFIID riid, void **out) { return surface_query(S##V(i), riid, out); }          \
  static HRESULT STDMETHODCALLTYPE s##V##_AddAttachedSurface(T *i, T *a) {                                                                      \
    Surface *as = surface_from_any(a);                                                                                                          \
    if (!as) return DDERR_INVALIDPARAMS;                                                                                                        \
    return surface_add_attached(S##V(i), as, (IUnknown *)a, V);                                                                                 \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_AddOverlayDirtyRect(T *i, RECT *r) { (void)i; (void)r; return DDERR_UNSUPPORTED; }                  \
  static HRESULT STDMETHODCALLTYPE s##V##_Blt(T *i, RECT *dr, T *src, RECT *sr, DWORD flags, DDBLTFX *fx) {                                    \
    Surface *ss = surface_from_any(src);                                                                                                        \
    if (BAD_SURFACE(src, ss)) return DDERR_INVALIDPARAMS;                                                                                       \
    return surface_blt(S##V(i), dr, ss, sr, flags, fx, V);                                                                                      \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_BltBatch(T *i, DDBLTBATCH *b, DWORD n, DWORD flags) { return surface_blt_batch(S##V(i), b, n, flags); } \
  static HRESULT STDMETHODCALLTYPE s##V##_BltFast(T *i, DWORD x, DWORD y, T *src, RECT *sr, DWORD trans) {                                     \
    Surface *ss = surface_from_any(src);                                                                                                        \
    if (!ss) return DDERR_INVALIDPARAMS;                                                                                                        \
    return surface_blt_fast(S##V(i), x, y, ss, sr, trans);                                                                                      \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_DeleteAttachedSurface(T *i, DWORD flags, T *a) {                                                      \
    return surface_delete_attached(S##V(i), flags, surface_from_any(a), (IUnknown *)a);                                                         \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_Flip(T *i, T *target, DWORD flags) {                                                                  \
    Surface *ts = surface_from_any(target);                                                                                                     \
    if (BAD_SURFACE(target, ts)) return DDERR_INVALIDPARAMS;                                                                                    \
    return surface_flip(S##V(i), ts, flags, V);                                                                                                 \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_GetBltStatus(T *i, DWORD flags) { return surface_get_blt_status(S##V(i), flags); }                  \
  static HRESULT STDMETHODCALLTYPE s##V##_GetClipper(T *i, IDirectDrawClipper **c) { return surface_get_clipper(S##V(i), c); }                 \
  static HRESULT STDMETHODCALLTYPE s##V##_GetColorKey(T *i, DWORD flags, DDCOLORKEY *k) { return surface_get_color_key(S##V(i), flags, k); }   \
  static HRESULT STDMETHODCALLTYPE s##V##_GetDC(T *i, HDC *dc) { return surface_get_dc(S##V(i), dc); }                                          \
  static HRESULT STDMETHODCALLTYPE s##V##_GetFlipStatus(T *i, DWORD flags) { return surface_get_flip_status(S##V(i), flags); }                \
  static HRESULT STDMETHODCALLTYPE s##V##_GetOverlayPosition(T *i, LONG *x, LONG *y) { (void)i; (void)x; (void)y; return DDERR_NOTAOVERLAYSURFACE; } \
  static HRESULT STDMETHODCALLTYPE s##V##_GetPalette(T *i, IDirectDrawPalette **p) { return surface_get_palette(S##V(i), p); }                 \
  static HRESULT STDMETHODCALLTYPE s##V##_GetPixelFormat(T *i, DDPIXELFORMAT *pf) { return surface_get_pixel_format(S##V(i), pf); }            \
  static HRESULT STDMETHODCALLTYPE s##V##_IsLost(T *i) { return surface_is_lost(S##V(i)) ? DDERR_SURFACELOST : DD_OK; }                         \
  static HRESULT STDMETHODCALLTYPE s##V##_ReleaseDC(T *i, HDC dc) { return surface_release_dc(S##V(i), dc); }                                   \
  static HRESULT STDMETHODCALLTYPE s##V##_Restore(T *i) { return surface_restore(S##V(i)); }                                                    \
  static HRESULT STDMETHODCALLTYPE s##V##_SetClipper(T *i, IDirectDrawClipper *c) { return surface_set_clipper(S##V(i), c); }                  \
  static HRESULT STDMETHODCALLTYPE s##V##_SetColorKey(T *i, DWORD flags, DDCOLORKEY *k) {                                                       \
    if (V == 7 && (S##V(i)->desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) return DDERR_NOTONMIPMAPSUBLEVEL;                                  \
    return surface_set_color_key(S##V(i), flags, k);                                                                                            \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_SetOverlayPosition(T *i, LONG x, LONG y) { (void)i; (void)x; (void)y; return DDERR_NOTAOVERLAYSURFACE; } \
  static HRESULT STDMETHODCALLTYPE s##V##_SetPalette(T *i, IDirectDrawPalette *p) { return surface_set_palette_checked(S##V(i), p, V == 7); }   \
  static HRESULT STDMETHODCALLTYPE s##V##_UpdateOverlay(T *i, RECT *sr, T *dst, RECT *dr, DWORD flags, DDOVERLAYFX *fx) {                       \
    (void)i; (void)sr; (void)dst; (void)dr; (void)flags; (void)fx;                                                                              \
    return DDERR_NOTAOVERLAYSURFACE;                                                                                                            \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_UpdateOverlayDisplay(T *i, DWORD flags) { (void)i; (void)flags; return DDERR_UNSUPPORTED; }          \
  static HRESULT STDMETHODCALLTYPE s##V##_UpdateOverlayZOrder(T *i, DWORD flags, T *ref) { (void)i; (void)flags; (void)ref; return DDERR_NOTAOVERLAYSURFACE; }

/* ... and those of the versions with DDSURFACEDESC and DDSCAPS (1, 2, 3). */
#define SURF_DESC1(V, T)                                                                                                                         \
  static HRESULT STDMETHODCALLTYPE s##V##_EnumAttachedSurfaces(T *i, void *ctx, LPDDENUMSURFACESCALLBACK cb) {                                 \
    return surface_enum_attached(S##V(i), V, ctx, (void *)cb);                                                                                  \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_EnumOverlayZOrders(T *i, DWORD flags, void *ctx, LPDDENUMSURFACESCALLBACK cb) {                      \
    (void)i; (void)flags; (void)ctx; (void)cb;                                                                                                  \
    return DDERR_NOTAOVERLAYSURFACE;                                                                                                            \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_GetAttachedSurface(T *i, DDSCAPS *caps, T **out) {                                                   \
    if (!out) return DDERR_INVALIDPARAMS;                                                                                                       \
    if (!caps) { *out = NULL; return DDERR_INVALIDPARAMS; }                                                                                     \
    DDSCAPS2 c2;                                                                                                                                \
    caps_to_caps2(caps, &c2);                                                                                                                   \
    Surface *a;                                                                                                                                 \
    HRESULT hr = surface_get_attached(S##V(i), &c2, V, &a);                                                                                     \
    if (FAILED(hr)) { *out = NULL; return hr; }                                                                                                 \
    s##V##_AddRef(IFACE##V(a));                                                                                                                 \
    *out = IFACE##V(a);                                                                                                                         \
    return DD_OK;                                                                                                                               \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_GetCaps(T *i, DDSCAPS *caps) { return surface_get_caps1(S##V(i), caps); }                            \
  static HRESULT STDMETHODCALLTYPE s##V##_GetSurfaceDesc(T *i, DDSURFACEDESC *d) { return surface_get_desc1(S##V(i), d); }                      \
  static HRESULT STDMETHODCALLTYPE s##V##_Initialize(T *i, IDirectDraw *dd, DDSURFACEDESC *d) { (void)i; (void)dd; (void)d; return DDERR_ALREADYINITIALIZED; } \
  static HRESULT STDMETHODCALLTYPE s##V##_Lock(T *i, RECT *r, DDSURFACEDESC *d, DWORD flags, HANDLE event) {                                    \
    Surface *s = S##V(i);                                                                                                                       \
    DWORD size;                                                                                                                                 \
    if (!lock_desc_size(s, d, &size)) return DDERR_INVALIDPARAMS;                                                                               \
    if (surface_is_lost(s)) return DDERR_SURFACELOST;                                                                                           \
    DDSURFACEDESC2 d2;                                                                                                                          \
    zero(&d2, sizeof d2);                                                                                                                       \
    d2.dwSize = d->dwSize;                                                                                                                      \
    HRESULT hr = surface_lock(s, r, &d2, sizeof d2, flags, event);                                                                             \
    if (FAILED(hr)) { d->lpSurface = NULL; return hr; }                                                                                         \
    DDSURFACEDESC d1;                                                                                                                           \
    ddsd2_to_ddsd(&d2, &d1);                                                                                                                    \
    const DWORD given_size = d->dwSize;                                                                                                         \
    copy_by_size(d, size, &d1, sizeof d1);                                                                                                      \
    d->dwSize = given_size;                    /* (a size Windows let through stays as the caller wrote it) */                                \
    if (size >= sizeof(DDSURFACEDESC2)) {      /* (a DDSURFACEDESC2 given to an old Lock: its newer fields come back 0, as Windows) */         \
      DDSURFACEDESC2 *full = (DDSURFACEDESC2 *)d;                                                                                              \
      full->ddsCaps.dwCaps2 = full->ddsCaps.dwCaps3 = full->ddsCaps.dwCaps4 = 0;                                                                \
      full->dwTextureStage = 0;                                                                                                                 \
    }                                                                                                                                           \
    return DD_OK;                                                                                                                               \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_Unlock(T *i, void *data) { (void)data; return surface_unlock(S##V(i), NULL); }

/* ... and those of the versions with DDSURFACEDESC2 and DDSCAPS2 (4, 7). */
#define SURF_DESC2(V, T, CB)                                                                                                                     \
  static HRESULT STDMETHODCALLTYPE s##V##_EnumAttachedSurfaces(T *i, void *ctx, CB cb) { return surface_enum_attached(S##V(i), V, ctx, (void *)cb); } \
  static HRESULT STDMETHODCALLTYPE s##V##_EnumOverlayZOrders(T *i, DWORD flags, void *ctx, CB cb) {                                            \
    (void)i; (void)flags; (void)ctx; (void)cb;                                                                                                  \
    return DDERR_NOTAOVERLAYSURFACE;                                                                                                            \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_GetAttachedSurface(T *i, DDSCAPS2 *caps, T **out) {                                                  \
    if (!out) return DDERR_INVALIDPARAMS;                                                                                                       \
    Surface *a;                                                                                                                                 \
    HRESULT hr = surface_get_attached(S##V(i), caps, V, &a);                                                                                    \
    if (FAILED(hr)) { *out = NULL; return hr; }                                                                                                 \
    s##V##_AddRef(IFACE##V(a));                                                                                                                 \
    *out = IFACE##V(a);                                                                                                                         \
    return DD_OK;                                                                                                                               \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_GetCaps(T *i, DDSCAPS2 *caps) { return surface_get_caps2(S##V(i), caps); }                           \
  static HRESULT STDMETHODCALLTYPE s##V##_GetSurfaceDesc(T *i, DDSURFACEDESC2 *d) { return surface_get_desc2(S##V(i), d); }                     \
  static HRESULT STDMETHODCALLTYPE s##V##_Initialize(T *i, IDirectDraw *dd, DDSURFACEDESC2 *d) { (void)i; (void)dd; (void)d; return DDERR_ALREADYINITIALIZED; } \
  static HRESULT STDMETHODCALLTYPE s##V##_Lock(T *i, RECT *r, DDSURFACEDESC2 *d, DWORD flags, HANDLE event) {                                   \
    Surface *s = S##V(i);                                                                                                                       \
    DWORD size;                                                                                                                                 \
    if (!lock_desc_size(s, (DDSURFACEDESC *)d, &size)) return DDERR_INVALIDPARAMS;                                                              \
    if (surface_is_lost(s)) return DDERR_SURFACELOST;                                                                                           \
    return surface_lock(s, r, d, size, flags, event);                                                                                           \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_Unlock(T *i, RECT *r) { return surface_unlock(S##V(i), r); }                                           \
  static HRESULT STDMETHODCALLTYPE s##V##_SetSurfaceDesc(T *i, DDSURFACEDESC2 *d, DWORD flags) { return surface_set_desc(S##V(i), d, flags); }  \
  static HRESULT STDMETHODCALLTYPE s##V##_SetPrivateData(T *i, REFGUID g, void *data, DWORD size, DWORD flags) {                                \
    return private_set(&S##V(i)->priv, g, data, size, flags);                                                                                   \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_GetPrivateData(T *i, REFGUID g, void *data, DWORD *size) { return private_get(&S##V(i)->priv, g, data, size); } \
  static HRESULT STDMETHODCALLTYPE s##V##_FreePrivateData(T *i, REFGUID g) { return private_free(&S##V(i)->priv, g); }                         \
  static HRESULT STDMETHODCALLTYPE s##V##_GetUniquenessValue(T *i, DWORD *v) {                                                                  \
    if (!v) return DDERR_INVALIDPARAMS;                                                                                                         \
    *v = S##V(i)->uniqueness;                                                                                                                   \
    return DD_OK;                                                                                                                               \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE s##V##_ChangeUniquenessValue(T *i) {                                                                         \
    Surface *s = S##V(i);                                                                                                                       \
    dd_lock();                                                                                                                                  \
    s->uniqueness = g_uniqueness++;                                                                                                             \
    if (!g_uniqueness) g_uniqueness = 1;                                                                                                        \
    dd_unlock();                                                                                                                                \
    return DD_OK;                                                                                                                               \
  }

/* (version 2 on: the DirectDraw interface, page locks; version 3 on: SetSurfaceDesc) */
static HRESULT surface_page_lock(Surface *s, DWORD flags) {
  if (flags) return DDERR_INVALIDPARAMS;
  dd_lock();
  if (s->desc.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ++s->page_locks;
  dd_unlock();
  return DD_OK;
}
static HRESULT surface_page_unlock(Surface *s, DWORD flags) {
  if (flags) return DDERR_INVALIDPARAMS;
  dd_lock();
  HRESULT hr = DD_OK;
  if (s->desc.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) { if (s->page_locks) --s->page_locks; else hr = DDERR_NOTPAGELOCKED; }
  dd_unlock();
  return hr;
}
#define SURF_V2(V, T)                                                                                                                            \
  static HRESULT STDMETHODCALLTYPE s##V##_GetDDInterface(T *i, void **out) { return surface_get_dd_interface(S##V(i), out); }                  \
  static HRESULT STDMETHODCALLTYPE s##V##_PageLock(T *i, DWORD flags) { return surface_page_lock(S##V(i), flags); }                            \
  static HRESULT STDMETHODCALLTYPE s##V##_PageUnlock(T *i, DWORD flags) { return surface_page_unlock(S##V(i), flags); }

SURF_COMMON(7, IDirectDrawSurface7)
SURF_COMMON(4, IDirectDrawSurface4)
SURF_COMMON(3, IDirectDrawSurface3)
SURF_COMMON(2, IDirectDrawSurface2)
SURF_COMMON(1, IDirectDrawSurface)
SURF_DESC2(7, IDirectDrawSurface7, LPDDENUMSURFACESCALLBACK7)
SURF_DESC2(4, IDirectDrawSurface4, LPDDENUMSURFACESCALLBACK2)
SURF_DESC1(3, IDirectDrawSurface3)
SURF_DESC1(2, IDirectDrawSurface2)
SURF_DESC1(1, IDirectDrawSurface)
SURF_V2(7, IDirectDrawSurface7)
SURF_V2(4, IDirectDrawSurface4)
SURF_V2(3, IDirectDrawSurface3)
SURF_V2(2, IDirectDrawSurface2)

static HRESULT STDMETHODCALLTYPE s3_SetSurfaceDesc(IDirectDrawSurface3 *i, DDSURFACEDESC *d, DWORD flags) {
  if (!d) return DDERR_INVALIDPARAMS;
  DDSURFACEDESC2 d2;
  ddsd_to_ddsd2(d, &d2);
  return surface_set_desc(S3(i), &d2, flags);
}
static HRESULT STDMETHODCALLTYPE s7_SetPriority(IDirectDrawSurface7 *i, DWORD p) { return surface_set_priority(S7(i), p); }
static HRESULT STDMETHODCALLTYPE s7_GetPriority(IDirectDrawSurface7 *i, DWORD *p) { return surface_get_priority(S7(i), p); }
static HRESULT STDMETHODCALLTYPE s7_SetLOD(IDirectDrawSurface7 *i, DWORD lod) { return surface_set_lod(S7(i), lod); }
static HRESULT STDMETHODCALLTYPE s7_GetLOD(IDirectDrawSurface7 *i, DWORD *lod) { return surface_get_lod(S7(i), lod); }

#define SURF_FIELDS_COMMON(V)                                                                                                                    \
  .QueryInterface = s##V##_QueryInterface, .AddRef = s##V##_AddRef, .Release = s##V##_Release, .AddAttachedSurface = s##V##_AddAttachedSurface, \
  .AddOverlayDirtyRect = s##V##_AddOverlayDirtyRect, .Blt = s##V##_Blt, .BltBatch = s##V##_BltBatch, .BltFast = s##V##_BltFast,                \
  .DeleteAttachedSurface = s##V##_DeleteAttachedSurface, .EnumAttachedSurfaces = s##V##_EnumAttachedSurfaces,                                 \
  .EnumOverlayZOrders = s##V##_EnumOverlayZOrders, .Flip = s##V##_Flip, .GetAttachedSurface = s##V##_GetAttachedSurface,                       \
  .GetBltStatus = s##V##_GetBltStatus, .GetCaps = s##V##_GetCaps, .GetClipper = s##V##_GetClipper, .GetColorKey = s##V##_GetColorKey,          \
  .GetDC = s##V##_GetDC, .GetFlipStatus = s##V##_GetFlipStatus, .GetOverlayPosition = s##V##_GetOverlayPosition,                               \
  .GetPalette = s##V##_GetPalette, .GetPixelFormat = s##V##_GetPixelFormat, .GetSurfaceDesc = s##V##_GetSurfaceDesc,                           \
  .Initialize = s##V##_Initialize, .IsLost = s##V##_IsLost, .Lock = s##V##_Lock, .ReleaseDC = s##V##_ReleaseDC, .Restore = s##V##_Restore,     \
  .SetClipper = s##V##_SetClipper, .SetColorKey = s##V##_SetColorKey, .SetOverlayPosition = s##V##_SetOverlayPosition,                         \
  .SetPalette = s##V##_SetPalette, .Unlock = s##V##_Unlock, .UpdateOverlay = s##V##_UpdateOverlay,                                             \
  .UpdateOverlayDisplay = s##V##_UpdateOverlayDisplay, .UpdateOverlayZOrder = s##V##_UpdateOverlayZOrder
#define SURF_FIELDS_V2(V) .GetDDInterface = s##V##_GetDDInterface, .PageLock = s##V##_PageLock, .PageUnlock = s##V##_PageUnlock
#define SURF_FIELDS_V4(V)                                                                                                                        \
  .SetSurfaceDesc = s##V##_SetSurfaceDesc, .SetPrivateData = s##V##_SetPrivateData, .GetPrivateData = s##V##_GetPrivateData,                    \
  .FreePrivateData = s##V##_FreePrivateData, .GetUniquenessValue = s##V##_GetUniquenessValue, .ChangeUniquenessValue = s##V##_ChangeUniquenessValue

IDirectDrawSurface7Vtbl surf7_vtbl = {SURF_FIELDS_COMMON(7), SURF_FIELDS_V2(7), SURF_FIELDS_V4(7), .SetPriority = s7_SetPriority,
                                            .GetPriority = s7_GetPriority, .SetLOD = s7_SetLOD, .GetLOD = s7_GetLOD};
IDirectDrawSurface4Vtbl surf4_vtbl = {SURF_FIELDS_COMMON(4), SURF_FIELDS_V2(4), SURF_FIELDS_V4(4)};
IDirectDrawSurface3Vtbl surf3_vtbl = {SURF_FIELDS_COMMON(3), SURF_FIELDS_V2(3), .SetSurfaceDesc = s3_SetSurfaceDesc};
IDirectDrawSurface2Vtbl surf2_vtbl = {SURF_FIELDS_COMMON(2), SURF_FIELDS_V2(2)};
IDirectDrawSurfaceVtbl surf1_vtbl = {SURF_FIELDS_COMMON(1)};

/* ---- the DirectDraw interfaces ------------------------------------------------------------------------------------------------------------------- */

#define DD_COMMON(V, T)                                                                                                                          \
  static HRESULT STDMETHODCALLTYPE dd##V##_QueryInterface(T *i, REFIID riid, void **out) { return dd_query(DD##V(i), riid, out); }             \
  static HRESULT STDMETHODCALLTYPE dd##V##_Compact(T *i) { return dd_compact(DD##V(i)); }                                                      \
  static HRESULT STDMETHODCALLTYPE dd##V##_CreateClipper(T *i, DWORD flags, IDirectDrawClipper **out, IUnknown *outer) {                       \
    return dd_create_clipper(DD##V(i), flags, out, outer);                                                                                      \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE dd##V##_CreatePalette(T *i, DWORD flags, PALETTEENTRY *e, IDirectDrawPalette **out, IUnknown *outer) {      \
    return dd_create_palette(DD##V(i), flags, e, out, outer, V);                                                                                \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE dd##V##_FlipToGDISurface(T *i) { return dd_flip_to_gdi_surface(DD##V(i)); }                                \
  static HRESULT STDMETHODCALLTYPE dd##V##_GetCaps(T *i, DDCAPS *d, DDCAPS *h) { return dd_get_caps(DD##V(i), d, h); }                         \
  static HRESULT STDMETHODCALLTYPE dd##V##_GetFourCCCodes(T *i, DWORD *n, DWORD *codes) { return dd_get_fourcc_codes(DD##V(i), n, codes); }   \
  static HRESULT STDMETHODCALLTYPE dd##V##_GetMonitorFrequency(T *i, DWORD *f) { return dd_get_monitor_frequency(DD##V(i), f); }              \
  static HRESULT STDMETHODCALLTYPE dd##V##_GetScanLine(T *i, DWORD *l) { return dd_get_scan_line(DD##V(i), l); }                              \
  static HRESULT STDMETHODCALLTYPE dd##V##_GetVerticalBlankStatus(T *i, BOOL *s) { return dd_get_vertical_blank_status(DD##V(i), s); }       \
  static HRESULT STDMETHODCALLTYPE dd##V##_Initialize(T *i, GUID *g) { return dd_initialize(DD##V(i), g); }                                   \
  static HRESULT STDMETHODCALLTYPE dd##V##_RestoreDisplayMode(T *i) {                                                                           \
    DD_READY(DD##V(i));                                                                                                                         \
    dd_lock();                                                                                                                                  \
    HRESULT hr = ddraw_restore_display_mode(DD##V(i));                                                                                          \
    dd_unlock();                                                                                                                                \
    return hr;                                                                                                                                  \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE dd##V##_SetCooperativeLevel(T *i, HWND w, DWORD flags) {                                                    \
    DDraw *dd = DD##V(i);                                                                                                                       \
    DD_READY(dd);                                                                                                                               \
    dd_lock();                                                                                                                                  \
    HRESULT hr = ddraw_set_cooperative_level(dd, w, flags, V == 1 ? FALSE : !(dd->flags & DD_SCL_DDRAW1));                                     \
    if (V == 1 && SUCCEEDED(hr)) dd->flags |= DD_SCL_DDRAW1;                                                                                    \
    dd_unlock();                                                                                                                                \
    return hr;                                                                                                                                  \
  }                                                                                                                                             \
  static HRESULT STDMETHODCALLTYPE dd##V##_WaitForVerticalBlank(T *i, DWORD flags, HANDLE e) { return dd_wait_for_vertical_blank(DD##V(i), flags, e); }

DD_COMMON(7, IDirectDraw7)
DD_COMMON(4, IDirectDraw4)
DD_COMMON(2, IDirectDraw2)
DD_COMMON(1, IDirectDraw)

/* (the surface methods in a version's own types) */
static HRESULT STDMETHODCALLTYPE dd7_CreateSurface(IDirectDraw7 *i, DDSURFACEDESC2 *d, IDirectDrawSurface7 **out, IUnknown *outer) {
  return dd_create_surface2(DD7(i), d, (void **)out, outer, 7);
}
static HRESULT STDMETHODCALLTYPE dd4_CreateSurface(IDirectDraw4 *i, DDSURFACEDESC2 *d, IDirectDrawSurface4 **out, IUnknown *outer) {
  return dd_create_surface2(DD4(i), d, (void **)out, outer, 4);
}
static HRESULT STDMETHODCALLTYPE dd2_CreateSurface(IDirectDraw2 *i, DDSURFACEDESC *d, IDirectDrawSurface **out, IUnknown *outer) {
  return dd_create_surface1(DD2(i), d, out, outer, 2);
}
static HRESULT STDMETHODCALLTYPE dd1_CreateSurface(IDirectDraw *i, DDSURFACEDESC *d, IDirectDrawSurface **out, IUnknown *outer) {
  return dd_create_surface1(DD1(i), d, out, outer, 1);
}
#define DD_DUPLICATE(V, T, SV)                                                                                                                   \
  static HRESULT STDMETHODCALLTYPE dd##V##_DuplicateSurface(T *i, SV *src, SV **out) {                                                         \
    if (!out) return DDERR_INVALIDPARAMS;                                                                                                       \
    Surface *s;                                                                                                                                 \
    HRESULT hr = dd_duplicate_surface(DD##V(i), surface_from_any(src), V, &s);                                                                  \
    *out = SUCCEEDED(hr) ? (SV *)(V == 7 ? (void *)IFACE7(s) : V == 4 ? (void *)IFACE4(s) : (void *)IFACE1(s)) : NULL;                         \
    return hr;                                                                                                                                  \
  }
DD_DUPLICATE(7, IDirectDraw7, IDirectDrawSurface7)
DD_DUPLICATE(4, IDirectDraw4, IDirectDrawSurface4)
DD_DUPLICATE(2, IDirectDraw2, IDirectDrawSurface)
DD_DUPLICATE(1, IDirectDraw, IDirectDrawSurface)

static HRESULT STDMETHODCALLTYPE dd7_EnumDisplayModes(IDirectDraw7 *i, DWORD flags, DDSURFACEDESC2 *d, void *ctx, LPDDENUMMODESCALLBACK2 cb) {
  return dd_enum_display_modes(DD7(i), flags, d, ctx, (void *)cb, TRUE);
}
static HRESULT STDMETHODCALLTYPE dd4_EnumDisplayModes(IDirectDraw4 *i, DWORD flags, DDSURFACEDESC2 *d, void *ctx, LPDDENUMMODESCALLBACK2 cb) {
  return dd_enum_display_modes(DD4(i), flags, d, ctx, (void *)cb, TRUE);
}
#define DD_ENUM_MODES1(V, T)                                                                                                                     \
  static HRESULT STDMETHODCALLTYPE dd##V##_EnumDisplayModes(T *i, DWORD flags, DDSURFACEDESC *d, void *ctx, LPDDENUMMODESCALLBACK cb) {         \
    DDSURFACEDESC2 d2;                                                                                                                          \
    if (d) ddsd_to_ddsd2(d, &d2);                                                                                                               \
    return dd_enum_display_modes(DD##V(i), flags, d ? &d2 : NULL, ctx, (void *)cb, FALSE);                                                      \
  }
DD_ENUM_MODES1(2, IDirectDraw2)
DD_ENUM_MODES1(1, IDirectDraw)

static HRESULT STDMETHODCALLTYPE dd7_EnumSurfaces(IDirectDraw7 *i, DWORD flags, DDSURFACEDESC2 *d, void *ctx, LPDDENUMSURFACESCALLBACK7 cb) {
  return dd_enum_surfaces(DD7(i), flags, d, ctx, (void *)cb, 7);
}
static HRESULT STDMETHODCALLTYPE dd4_EnumSurfaces(IDirectDraw4 *i, DWORD flags, DDSURFACEDESC2 *d, void *ctx, LPDDENUMSURFACESCALLBACK2 cb) {
  return dd_enum_surfaces(DD4(i), flags, d, ctx, (void *)cb, 4);
}
#define DD_ENUM_SURFACES1(V, T)                                                                                                                  \
  static HRESULT STDMETHODCALLTYPE dd##V##_EnumSurfaces(T *i, DWORD flags, DDSURFACEDESC *d, void *ctx, LPDDENUMSURFACESCALLBACK cb) {          \
    DDSURFACEDESC2 d2;                                                                                                                          \
    if (d) ddsd_to_ddsd2(d, &d2);                                                                                                               \
    return dd_enum_surfaces(DD##V(i), flags, d ? &d2 : NULL, ctx, (void *)cb, V);                                                               \
  }
DD_ENUM_SURFACES1(2, IDirectDraw2)
DD_ENUM_SURFACES1(1, IDirectDraw)

static HRESULT STDMETHODCALLTYPE dd7_GetDisplayMode(IDirectDraw7 *i, DDSURFACEDESC2 *d) { return dd_get_display_mode(DD7(i), d); }
static HRESULT STDMETHODCALLTYPE dd4_GetDisplayMode(IDirectDraw4 *i, DDSURFACEDESC2 *d) { return dd_get_display_mode(DD4(i), d); }
#define DD_GET_MODE1(V, T)                                                                                                                       \
  static HRESULT STDMETHODCALLTYPE dd##V##_GetDisplayMode(T *i, DDSURFACEDESC *d) {                                                            \
    HRESULT hr = dd_get_display_mode(DD##V(i), (DDSURFACEDESC2 *)d);                                                                            \
    if (SUCCEEDED(hr)) d->dwSize = sizeof *d;                                                                                                   \
    return hr;                                                                                                                                  \
  }
DD_GET_MODE1(2, IDirectDraw2)
DD_GET_MODE1(1, IDirectDraw)

#define DD_GDI_SURFACE(V, T, SV)                                                                                                                 \
  static HRESULT STDMETHODCALLTYPE dd##V##_GetGDISurface(T *i, SV **out) {                                                                     \
    if (!out) return DDERR_INVALIDPARAMS;                                                                                                       \
    Surface *s;                                                                                                                                 \
    HRESULT hr = dd_get_gdi_surface(DD##V(i), &s);                                                                                              \
    if (FAILED(hr)) { *out = NULL; return hr; }                                                                                                 \
    if (V == 7) { s7_AddRef(IFACE7(s)); *out = (SV *)IFACE7(s); }                                                                               \
    else if (V == 4) { s4_AddRef(IFACE4(s)); *out = (SV *)IFACE4(s); }                                                                          \
    else { s1_AddRef(IFACE1(s)); *out = (SV *)IFACE1(s); }                                                                                      \
    return DD_OK;                                                                                                                               \
  }
DD_GDI_SURFACE(7, IDirectDraw7, IDirectDrawSurface7)
DD_GDI_SURFACE(4, IDirectDraw4, IDirectDrawSurface4)
DD_GDI_SURFACE(2, IDirectDraw2, IDirectDrawSurface)
DD_GDI_SURFACE(1, IDirectDraw, IDirectDrawSurface)

static HRESULT STDMETHODCALLTYPE dd7_SetDisplayMode(IDirectDraw7 *i, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags) {
  DD_READY(DD7(i));
  dd_lock();
  HRESULT hr = ddraw_set_display_mode(DD7(i), w, h, bpp, refresh, flags);
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE dd4_SetDisplayMode(IDirectDraw4 *i, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags) {
  return dd7_SetDisplayMode(DDI7(DD4(i)), w, h, bpp, refresh, flags);
}
static HRESULT STDMETHODCALLTYPE dd2_SetDisplayMode(IDirectDraw2 *i, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags) {
  return dd7_SetDisplayMode(DDI7(DD2(i)), w, h, bpp, refresh, flags);
}
static HRESULT STDMETHODCALLTYPE dd1_SetDisplayMode(IDirectDraw *i, DWORD w, DWORD h, DWORD bpp) { return dd7_SetDisplayMode(DDI7(DD1(i)), w, h, bpp, 0, 0); }

static HRESULT STDMETHODCALLTYPE dd7_GetAvailableVidMem(IDirectDraw7 *i, DDSCAPS2 *c, DWORD *t, DWORD *f) { return dd_get_available_vid_mem(DD7(i), c, t, f); }
static HRESULT STDMETHODCALLTYPE dd4_GetAvailableVidMem(IDirectDraw4 *i, DDSCAPS2 *c, DWORD *t, DWORD *f) { return dd_get_available_vid_mem(DD4(i), c, t, f); }
static HRESULT STDMETHODCALLTYPE dd2_GetAvailableVidMem(IDirectDraw2 *i, DDSCAPS *c, DWORD *t, DWORD *f) {
  DDSCAPS2 c2;
  if (c) caps_to_caps2(c, &c2);
  return dd_get_available_vid_mem(DD2(i), c ? &c2 : NULL, t, f);
}

static HRESULT STDMETHODCALLTYPE dd7_GetSurfaceFromDC(IDirectDraw7 *i, HDC dc, IDirectDrawSurface7 **out) {
  if (!out) return E_INVALIDARG;
  Surface *s;
  HRESULT hr = dd_get_surface_from_dc(DD7(i), dc, &s);
  if (FAILED(hr)) { *out = NULL; return hr; }
  s7_AddRef(IFACE7(s));
  *out = IFACE7(s);
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE dd4_GetSurfaceFromDC(IDirectDraw4 *i, HDC dc, IDirectDrawSurface4 **out) {
  if (!out) return E_INVALIDARG;
  Surface *s;
  HRESULT hr = dd_get_surface_from_dc(DD4(i), dc, &s);
  if (FAILED(hr)) { *out = NULL; return hr; }
  /* (IDirectDraw4's gives the surface's IDirectDrawSurface interface, as Windows does) */
  s1_AddRef(IFACE1(s));
  *out = (IDirectDrawSurface4 *)IFACE1(s);
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE dd7_RestoreAllSurfaces(IDirectDraw7 *i) { return dd_restore_all_surfaces(DD7(i)); }
static HRESULT STDMETHODCALLTYPE dd4_RestoreAllSurfaces(IDirectDraw4 *i) { return dd_restore_all_surfaces(DD4(i)); }
static HRESULT STDMETHODCALLTYPE dd7_TestCooperativeLevel(IDirectDraw7 *i) { return dd_test_cooperative_level(DD7(i)); }
static HRESULT STDMETHODCALLTYPE dd4_TestCooperativeLevel(IDirectDraw4 *i) { return dd_test_cooperative_level(DD4(i)); }
static HRESULT STDMETHODCALLTYPE dd7_GetDeviceIdentifier(IDirectDraw7 *i, DDDEVICEIDENTIFIER2 *id, DWORD flags) { return dd_get_device_identifier(DD7(i), id, flags); }
static HRESULT STDMETHODCALLTYPE dd4_GetDeviceIdentifier(IDirectDraw4 *i, DDDEVICEIDENTIFIER *id, DWORD flags) {
  if (!id) return DDERR_INVALIDPARAMS;
  DDDEVICEIDENTIFIER2 id2;
  HRESULT hr = dd_get_device_identifier(DD4(i), &id2, flags);
  if (FAILED(hr)) return hr;
  memcpy(id->szDriver, id2.szDriver, sizeof id->szDriver);
  memcpy(id->szDescription, id2.szDescription, sizeof id->szDescription);
  id->liDriverVersion = id2.liDriverVersion;
  id->dwVendorId = id2.dwVendorId;
  id->dwDeviceId = id2.dwDeviceId;
  id->dwSubSysId = id2.dwSubSysId;
  id->dwRevision = id2.dwRevision;
  id->guidDeviceIdentifier = id2.guidDeviceIdentifier;
  return DD_OK;
}
static HRESULT STDMETHODCALLTYPE dd7_StartModeTest(IDirectDraw7 *i, SIZE *modes, DWORD n, DWORD flags) {
  dd_lock();
  HRESULT hr = dd_start_mode_test(DD7(i), modes, n, flags);
  dd_unlock();
  return hr;
}
static HRESULT STDMETHODCALLTYPE dd7_EvaluateMode(IDirectDraw7 *i, DWORD flags, DWORD *timeout) {
  dd_lock();
  HRESULT hr = dd_evaluate_mode(DD7(i), flags, timeout);
  dd_unlock();
  return hr;
}

#define DD_FIELDS_COMMON(V)                                                                                                                      \
  .QueryInterface = dd##V##_QueryInterface, .AddRef = dd##V##_AddRef, .Release = dd##V##_Release, .Compact = dd##V##_Compact,                  \
  .CreateClipper = dd##V##_CreateClipper, .CreatePalette = dd##V##_CreatePalette, .CreateSurface = dd##V##_CreateSurface,                      \
  .DuplicateSurface = dd##V##_DuplicateSurface, .EnumDisplayModes = dd##V##_EnumDisplayModes, .EnumSurfaces = dd##V##_EnumSurfaces,            \
  .FlipToGDISurface = dd##V##_FlipToGDISurface, .GetCaps = dd##V##_GetCaps, .GetDisplayMode = dd##V##_GetDisplayMode,                          \
  .GetFourCCCodes = dd##V##_GetFourCCCodes, .GetGDISurface = dd##V##_GetGDISurface, .GetMonitorFrequency = dd##V##_GetMonitorFrequency,       \
  .GetScanLine = dd##V##_GetScanLine, .GetVerticalBlankStatus = dd##V##_GetVerticalBlankStatus, .Initialize = dd##V##_Initialize,              \
  .RestoreDisplayMode = dd##V##_RestoreDisplayMode, .SetCooperativeLevel = dd##V##_SetCooperativeLevel, .SetDisplayMode = dd##V##_SetDisplayMode, \
  .WaitForVerticalBlank = dd##V##_WaitForVerticalBlank

IDirectDraw7Vtbl dd7_vtbl = {DD_FIELDS_COMMON(7), .GetAvailableVidMem = dd7_GetAvailableVidMem, .GetSurfaceFromDC = dd7_GetSurfaceFromDC,
                                   .RestoreAllSurfaces = dd7_RestoreAllSurfaces, .TestCooperativeLevel = dd7_TestCooperativeLevel,
                                   .GetDeviceIdentifier = dd7_GetDeviceIdentifier, .StartModeTest = dd7_StartModeTest, .EvaluateMode = dd7_EvaluateMode};
IDirectDraw4Vtbl dd4_vtbl = {DD_FIELDS_COMMON(4), .GetAvailableVidMem = dd4_GetAvailableVidMem, .GetSurfaceFromDC = dd4_GetSurfaceFromDC,
                                   .RestoreAllSurfaces = dd4_RestoreAllSurfaces, .TestCooperativeLevel = dd4_TestCooperativeLevel,
                                   .GetDeviceIdentifier = dd4_GetDeviceIdentifier};
IDirectDraw2Vtbl dd2_vtbl = {DD_FIELDS_COMMON(2), .GetAvailableVidMem = dd2_GetAvailableVidMem};
IDirectDrawVtbl dd1_vtbl = {DD_FIELDS_COMMON(1)};
