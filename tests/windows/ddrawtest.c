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
/* DirectDraw (ddraw.dll) on the real kernel, every interface version: the object's references and identities, the cooperative level, display
 * modes (8, 16 and 32 bits), surfaces of many formats read back through Lock, colour and depth fills, blits (stretched, mirrored, colour-keyed
 * from the source and the destination with Windows' version rules, converted between formats, a surface onto itself, clipped by a clip list),
 * BltFast, GetDC with GDI drawing, GetSurfaceFromDC, palettes, mipmaps and cube maps, z buffers, private data and uniqueness values, and a full-screen
 * flipping chain in an 8-bit and a 32-bit display mode whose front buffer is read back from the window it is shown in. Built for i386 (tests/
 * wow64_qemu.sh ddrawtest) and x86-64 (tests/d3d12_qemu.sh ddrawtest). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ddraw.h>

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "ddrawtest: ok: %s\n" : "ddrawtest: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}
static ULONG refs(void *object) {
  IUnknown *u = object;
  IUnknown_AddRef(u);
  return IUnknown_Release(u);
}

static HWND g_window;
static IDirectDraw7 *g_dd;

static IDirectDrawSurface7 *make_surface(DWORD w, DWORD h, DWORD caps, const DDPIXELFORMAT *pf) {
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (pf ? DDSD_PIXELFORMAT : 0);
  d.dwWidth = w;
  d.dwHeight = h;
  d.ddsCaps.dwCaps = caps;
  if (pf) d.ddpfPixelFormat = *pf;
  IDirectDrawSurface7 *s = NULL;
  if (FAILED(IDirectDraw7_CreateSurface(g_dd, &d, &s, NULL))) return NULL;
  return s;
}
static DDPIXELFORMAT rgb_format(DWORD bits, DWORD r, DWORD g, DWORD b, DWORD a) {
  DDPIXELFORMAT pf;
  memset(&pf, 0, sizeof pf);
  pf.dwSize = sizeof pf;
  pf.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
  pf.dwRGBBitCount = bits;
  pf.dwRBitMask = r;
  pf.dwGBitMask = g;
  pf.dwBBitMask = b;
  pf.dwRGBAlphaBitMask = a;
  return pf;
}
static DWORD get_pixel(IDirectDrawSurface7 *s, int x, int y) {
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  if (FAILED(IDirectDrawSurface7_Lock(s, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL))) return 0xDEADBEEF;
  DWORD v = 0;
  const DWORD bytes = d.ddpfPixelFormat.dwRGBBitCount / 8;
  memcpy(&v, (BYTE *)d.lpSurface + (SIZE_T)y * d.lPitch + (SIZE_T)x * bytes, bytes);
  IDirectDrawSurface7_Unlock(s, NULL);
  return v;
}
/* A texture: through IDirectDraw7 on 32-bit Windows; 64-bit Windows' IDirectDraw7 has no Direct3D and refuses textures (E_NOINTERFACE), so there
 * through IDirectDraw4, the surface then asked for its IDirectDrawSurface7. */
static HRESULT create_texture(DDSURFACEDESC2 *d, IDirectDrawSurface7 **out) {
  *out = NULL;
#ifdef _WIN64
  IDirectDrawSurface7 *t = NULL;
  HRESULT hr = IDirectDraw7_CreateSurface(g_dd, d, &t, NULL);
  check(hr == E_NOINTERFACE && !t, "64-bit IDirectDraw7 refuses textures with E_NOINTERFACE, as Windows");
  if (t) IDirectDrawSurface7_Release(t);
  IDirectDraw4 *dd4 = NULL;
  hr = IDirectDraw7_QueryInterface(g_dd, &IID_IDirectDraw4, (void **)&dd4);
  if (FAILED(hr)) return hr;
  IDirectDrawSurface4 *s4 = NULL;
  hr = IDirectDraw4_CreateSurface(dd4, d, &s4, NULL);
  if (SUCCEEDED(hr)) {
    hr = IDirectDrawSurface4_QueryInterface(s4, &IID_IDirectDrawSurface7, (void **)out);
    IDirectDrawSurface4_Release(s4);
  }
  IDirectDraw4_Release(dd4);
  return hr;
#else
  return IDirectDraw7_CreateSurface(g_dd, d, out, NULL);
#endif
}
static void fill(IDirectDrawSurface7 *s, RECT *r, DWORD colour) {
  DDBLTFX fx;
  memset(&fx, 0, sizeof fx);
  fx.dwSize = sizeof fx;
  fx.dwFillColor = colour;
  IDirectDrawSurface7_Blt(s, r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}

static void test_object(void) {
  HRESULT hr = DirectDrawCreateEx(NULL, (void **)&g_dd, &IID_IDirectDraw7, NULL);
  check(SUCCEEDED(hr) && g_dd, "DirectDrawCreateEx makes an IDirectDraw7");
  if (!g_dd) return;
  IDirectDraw *dd1 = NULL;
  IDirectDraw4 *dd4 = NULL;
  hr = IDirectDraw7_QueryInterface(g_dd, &IID_IDirectDraw, (void **)&dd1);
  check(SUCCEEDED(hr) && refs(dd1) == 1 && refs(g_dd) == 1, "each interface version has a reference count of its own");
  IDirectDraw_QueryInterface(dd1, &IID_IDirectDraw4, (void **)&dd4);
  IUnknown *u1 = NULL, *u2 = NULL;
  IDirectDraw4_QueryInterface(dd4, &IID_IUnknown, (void **)&u1);
  IDirectDraw_QueryInterface(dd1, &IID_IUnknown, (void **)&u2);
  check(u1 && u1 == u2 && u1 == (IUnknown *)g_dd, "IUnknown is the same pointer from every version (IDirectDraw7's)");
  if (u1) IUnknown_Release(u1);
  if (u2) IUnknown_Release(u2);
  void *dd3 = (void *)1;
  static const GUID iid_dd3 = {0x618f8ad4, 0x8b7a, 0x11d0, {0x8f, 0xcc, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
  check(IDirectDraw7_QueryInterface(g_dd, &iid_dd3, &dd3) == E_NOINTERFACE && !dd3, "IDirectDraw3 is not an interface of the object (as Windows)");
  check(IDirectDraw4_Release(dd4) == 0 && IDirectDraw_Release(dd1) == 0, "releasing the other versions to 0 leaves the object");
  check(IDirectDraw7_Initialize(g_dd, NULL) == DDERR_ALREADYINITIALIZED, "Initialize after DirectDrawCreateEx: DDERR_ALREADYINITIALIZED");
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS;
  d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
  IDirectDrawSurface7 *s = (void *)0x1234;
  check(IDirectDraw7_CreateSurface(g_dd, &d, &s, NULL) == DDERR_NOCOOPERATIVELEVELSET && s == (void *)0x1234, "no surface before a cooperative level");
  check(IDirectDraw7_SetCooperativeLevel(g_dd, g_window, DDSCL_NORMAL) == DD_OK, "SetCooperativeLevel normal");
  check(IDirectDraw7_SetCooperativeLevel(g_dd, g_window, DDSCL_EXCLUSIVE) == DDERR_INVALIDPARAMS, "exclusive mode needs DDSCL_FULLSCREEN");
  DDCAPS hal, hel;
  memset(&hal, 0, sizeof hal);
  memset(&hel, 0, sizeof hel);
  hal.dwSize = hel.dwSize = sizeof hal;
  hr = IDirectDraw7_GetCaps(g_dd, &hal, &hel);
  check(SUCCEEDED(hr) && (hal.dwCaps & DDCAPS_BLT) && (hal.dwCaps & DDCAPS_BLTSTRETCH) && (hal.dwCKeyCaps & DDCKEYCAPS_SRCBLT) && hal.dwVidMemTotal &&
            (hal.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) && !(hal.dwCaps & DDCAPS_OVERLAY), "GetCaps: blits, stretching, colour keys, video memory, no overlays");
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  hr = IDirectDraw7_GetDisplayMode(g_dd, &d);
  check(SUCCEEDED(hr) && d.dwWidth && d.dwHeight && d.ddpfPixelFormat.dwRGBBitCount == 32 && d.lPitch == (LONG)(d.dwWidth * 4),
        "GetDisplayMode: the desktop's 32-bit mode");
  DWORD n = 0;
  check(IDirectDraw7_GetFourCCCodes(g_dd, &n, NULL) == DD_OK && n >= 5, "GetFourCCCodes counts the FourCC formats");
  DWORD total = 0, free_ = 0;
  DDSCAPS2 vc = {DDSCAPS_VIDEOMEMORY, 0, 0, {0}};
  check(IDirectDraw7_GetAvailableVidMem(g_dd, &vc, &total, &free_) == DD_OK && total && free_ <= total, "GetAvailableVidMem");
  DDDEVICEIDENTIFIER2 id;
  memset(&id, 0, sizeof id);
  check(IDirectDraw7_GetDeviceIdentifier(g_dd, &id, 0) == DD_OK && strstr(id.szDriver, ".dll"), "GetDeviceIdentifier names the driver's library");
}

static int g_modes[3];
static HRESULT CALLBACK count_modes(DDSURFACEDESC2 *d, void *context) {
  (void)context;
  if (d->ddpfPixelFormat.dwRGBBitCount == 8) ++g_modes[0];
  if (d->ddpfPixelFormat.dwRGBBitCount == 16) ++g_modes[1];
  if (d->ddpfPixelFormat.dwRGBBitCount == 32) ++g_modes[2];
  return DDENUMRET_OK;
}
static void test_modes(void) {
  check(IDirectDraw7_EnumDisplayModes(g_dd, 0, NULL, NULL, count_modes) == DD_OK && g_modes[0] && g_modes[1] && g_modes[2],
        "EnumDisplayModes lists modes of 8, 16 and 32 bits");
  int has640 = 0;
  DDSURFACEDESC2 want;
  memset(&want, 0, sizeof want);
  want.dwSize = sizeof want;
  want.dwFlags = DDSD_WIDTH | DDSD_HEIGHT;
  want.dwWidth = 640;
  want.dwHeight = 480;
  memset(g_modes, 0, sizeof g_modes);
  IDirectDraw7_EnumDisplayModes(g_dd, 0, &want, NULL, count_modes);
  has640 = g_modes[0] && g_modes[2];
  check(has640, "640x480 is a mode at 8 and 32 bits");
}

static void test_surfaces(void) {
  DDPIXELFORMAT x8 = rgb_format(32, 0xFF0000, 0xFF00, 0xFF, 0);
  IDirectDrawSurface7 *s = make_surface(63, 7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, &x8);
  check(s != NULL, "an offscreen surface in system memory");
  if (!s) return;
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  IDirectDrawSurface7_GetSurfaceDesc(s, &d);
  check(d.lPitch == (sizeof(void *) == 8 ? 252 : 256) && (d.dwFlags & DDSD_PITCH) && (d.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY),
        "the pitch of a system memory surface (8-byte rows for 32-bit programs, 4 for 64-bit, as Windows)");
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  HRESULT hr = IDirectDrawSurface7_Lock(s, NULL, &d, DDLOCK_WAIT, NULL);
  check(hr == DD_OK && d.lpSurface, "Lock");
  if (hr == DD_OK) {
    for (int y = 0; y < 7; ++y) for (int x = 0; x < 63; ++x) ((DWORD *)((BYTE *)d.lpSurface + y * d.lPitch))[x] = (DWORD)(y << 8 | x);
    DDSURFACEDESC2 d2 = d;
    check(IDirectDrawSurface7_Lock(s, NULL, &d2, DDLOCK_WAIT, NULL) == DDERR_SURFACEBUSY, "a second Lock: DDERR_SURFACEBUSY");
    IDirectDrawSurface7_Unlock(s, NULL);
  }
  check(IDirectDrawSurface7_Unlock(s, NULL) == DDERR_NOTLOCKED, "Unlock of a surface that is not locked: DDERR_NOTLOCKED");
  RECT r = {10, 2, 20, 5};
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  hr = IDirectDrawSurface7_Lock(s, &r, &d, DDLOCK_READONLY, NULL);
  check(hr == DD_OK && *(DWORD *)d.lpSurface == (2 << 8 | 10), "Lock of a rectangle points at its first pixel");
  IDirectDrawSurface7_Unlock(s, NULL);
  RECT bad = {10, 2, 64, 5};
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  check(IDirectDrawSurface7_Lock(s, &bad, &d, 0, NULL) == DDERR_INVALIDPARAMS && !d.lpSurface, "Lock of a rectangle outside: DDERR_INVALIDPARAMS");
  /* uniqueness and private data */
  DWORD u1 = 0, u2 = 0;
  IDirectDrawSurface7_GetUniquenessValue(s, &u1);
  fill(s, NULL, 0);
  IDirectDrawSurface7_GetUniquenessValue(s, &u2);
  check(u1 != u2, "the uniqueness value changes when the contents do");
  static const GUID tag = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
  DWORD data = 0xC0FFEE, out = 0, size = sizeof out;
  check(IDirectDrawSurface7_SetPrivateData(s, &tag, &data, sizeof data, 0) == DD_OK && IDirectDrawSurface7_GetPrivateData(s, &tag, &out, &size) == DD_OK &&
            out == 0xC0FFEE && IDirectDrawSurface7_FreePrivateData(s, &tag) == DD_OK && IDirectDrawSurface7_GetPrivateData(s, &tag, &out, &size) == DDERR_NOTFOUND,
        "private data");
  /* the older interfaces see the same surface */
  IDirectDrawSurface *s1 = NULL;
  IDirectDrawSurface7_QueryInterface(s, &IID_IDirectDrawSurface, (void **)&s1);
  DDSURFACEDESC d1;
  memset(&d1, 0, sizeof d1);
  d1.dwSize = sizeof d1;
  check(s1 && IDirectDrawSurface_GetSurfaceDesc(s1, &d1) == DD_OK && d1.dwWidth == 63 && refs(s1) == 1 && refs(s) == 1, "IDirectDrawSurface of it: its own count");
  IDirectDrawSurface_Release(s1);
  check(IDirectDrawSurface7_Release(s) == 0, "the surface goes with its last reference");
  /* refusals */
  DDSURFACEDESC2 c;
  memset(&c, 0, sizeof c);
  c.dwSize = sizeof c;
  c.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
  c.dwWidth = c.dwHeight = 8;
  c.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_COMPLEX;
  IDirectDrawSurface7 *x = NULL;
  check(IDirectDraw7_CreateSurface(g_dd, &c, &x, NULL) == DDERR_INVALIDCAPS, "DDSCAPS_COMPLEX without flip, mipmap or cube: DDERR_INVALIDCAPS");
  c.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY;
  check(IDirectDraw7_CreateSurface(g_dd, &c, &x, NULL) == DDERR_NOOVERLAYHW, "no overlays: DDERR_NOOVERLAYHW");
}

static void test_blits(void) {
  DDPIXELFORMAT x8 = rgb_format(32, 0xFF0000, 0xFF00, 0xFF, 0);
  IDirectDrawSurface7 *a = make_surface(16, 16, DDSCAPS_OFFSCREENPLAIN, &x8), *b = make_surface(32, 32, DDSCAPS_OFFSCREENPLAIN, &x8);
  check(a && b, "two surfaces in video memory");
  if (!a || !b) return;
  fill(a, NULL, 0x0000FF);
  RECT q = {0, 0, 8, 8};
  fill(a, &q, 0xFF0000);
  check(get_pixel(a, 3, 3) == 0xFF0000 && get_pixel(a, 9, 9) == 0x0000FF, "colour fill of a rectangle and of all");
  HRESULT hr = IDirectDrawSurface7_Blt(b, NULL, a, NULL, DDBLT_WAIT, NULL);
  check(hr == DD_OK && get_pixel(b, 15, 15) == 0xFF0000 && get_pixel(b, 16, 16) == 0x0000FF, "a stretched blit (16x16 onto 32x32)");
  DDBLTFX fx;
  memset(&fx, 0, sizeof fx);
  fx.dwSize = sizeof fx;
  fx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
  hr = IDirectDrawSurface7_Blt(b, NULL, a, NULL, DDBLT_DDFX | DDBLT_WAIT, &fx);
  check(hr == DD_OK && get_pixel(b, 31, 0) == 0xFF0000 && get_pixel(b, 0, 0) == 0x0000FF, "DDBLTFX_MIRRORLEFTRIGHT");
  fx.dwROP = WHITENESS;
  hr = IDirectDrawSurface7_Blt(b, NULL, NULL, NULL, DDBLT_ROP, &fx);
  check(hr == DD_OK && get_pixel(b, 5, 5) == 0xFFFFFFFF, "the WHITENESS ROP (every bit set, the unused byte too, as Windows)");
  fx.dwROP = SRCAND;
  check(IDirectDrawSurface7_Blt(b, NULL, a, NULL, DDBLT_ROP, &fx) == DDERR_NORASTEROPHW, "other ROPs: DDERR_NORASTEROPHW");
  /* source colour key: blue of a is transparent */
  DDCOLORKEY key = {0x0000FF, 0x0000FF};
  IDirectDrawSurface7_SetColorKey(a, DDCKEY_SRCBLT, &key);
  fill(b, NULL, 0x00FF00);
  RECT d16 = {0, 0, 16, 16};
  hr = IDirectDrawSurface7_Blt(b, &d16, a, NULL, DDBLT_KEYSRC | DDBLT_WAIT, NULL);
  check(hr == DD_OK && get_pixel(b, 2, 2) == 0xFF0000 && get_pixel(b, 12, 12) == 0x00FF00, "DDBLT_KEYSRC leaves the key colour's pixels");
  /* destination colour key: versions 4 and 7 take the destination's key, 1 .. 3 the source's (as Windows) */
  DDCOLORKEY dkey = {0x00FF00, 0x00FF00}, skey_dest = {0xFF0000, 0xFF0000};
  IDirectDrawSurface7_SetColorKey(b, DDCKEY_DESTBLT, &dkey);
  IDirectDrawSurface7_SetColorKey(a, DDCKEY_DESTBLT, &skey_dest);
  hr = IDirectDrawSurface7_Blt(b, &d16, a, NULL, DDBLT_KEYDEST | DDBLT_WAIT, NULL);
  check(hr == DD_OK && get_pixel(b, 2, 2) == 0xFF0000 && get_pixel(b, 12, 12) == 0x0000FF, "IDirectDrawSurface7 DDBLT_KEYDEST: the destination's key (green replaced)");
  IDirectDrawSurface *a1 = NULL, *b1 = NULL;
  IDirectDrawSurface7_QueryInterface(a, &IID_IDirectDrawSurface, (void **)&a1);
  IDirectDrawSurface7_QueryInterface(b, &IID_IDirectDrawSurface, (void **)&b1);
  fill(b, NULL, 0xFF0000);
  RECT top = {0, 0, 16, 1};
  fill(b, &top, 0x00FF00);
  hr = IDirectDrawSurface_Blt(b1, &d16, a1, NULL, DDBLT_KEYDEST | DDBLT_WAIT, NULL);
  check(hr == DD_OK && get_pixel(b, 12, 12) == 0x0000FF && get_pixel(b, 12, 0) == 0x00FF00, "IDirectDrawSurface DDBLT_KEYDEST: the source's DDCKEY_DESTBLT (red replaced)");
  /* BltFast with the source key, a blit onto an overlapping part of the same surface */
  fill(b, NULL, 0x123456);
  hr = IDirectDrawSurface7_BltFast(b, 4, 4, a, NULL, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
  check(hr == DD_OK && get_pixel(b, 4, 4) == 0xFF0000 && get_pixel(b, 15, 15) == 0x123456, "BltFast with DDBLTFAST_SRCCOLORKEY");
  check(IDirectDrawSurface7_BltFast(b, 20, 20, a, NULL, 0) == DDERR_INVALIDRECT, "BltFast beyond the destination: DDERR_INVALIDRECT");
  RECT from = {4, 4, 12, 12}, to = {6, 6, 14, 14};
  hr = IDirectDrawSurface7_Blt(b, &to, b, &from, DDBLT_WAIT, NULL);
  check(hr == DD_OK && get_pixel(b, 6, 6) == 0xFF0000 && get_pixel(b, 13, 13) == 0xFF0000, "a blit onto an overlapping part of itself");
  /* format conversion: 5-6-5 into x8r8g8b8 */
  DDPIXELFORMAT r565 = rgb_format(16, 0xF800, 0x07E0, 0x001F, 0);
  IDirectDrawSurface7 *c = make_surface(4, 4, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, &r565);
  if (c) {
    fill(c, NULL, 0x07E0);
    hr = IDirectDrawSurface7_Blt(b, &q, c, NULL, DDBLT_WAIT, NULL);
    check(hr == DD_OK && get_pixel(b, 1, 1) == 0x00FF00 && (get_pixel(c, 0, 0) & 0xFFFF) == 0x07E0, "a blit from 5-6-5 into x8r8g8b8 converts");
    IDirectDrawSurface7_Release(c);
  }
  /* a clipper with a clip list */
  IDirectDrawClipper *clipper = NULL;
  IDirectDraw7_CreateClipper(g_dd, 0, &clipper, NULL);
  struct { RGNDATAHEADER h; RECT r[1]; } list;
  memset(&list, 0, sizeof list);
  list.h.dwSize = sizeof list.h;
  list.h.iType = RDH_RECTANGLES;
  list.h.nCount = 1;
  list.h.nRgnSize = sizeof(RECT);
  SetRect(&list.r[0], 0, 0, 8, 32);
  IDirectDrawClipper_SetClipList(clipper, (RGNDATA *)&list, 0);
  IDirectDrawSurface7_SetClipper(b, clipper);
  fill(b, NULL, 0xABCDEF);
  check(get_pixel(b, 4, 20) == 0xABCDEF && get_pixel(b, 20, 20) == 0x123456, "a clip list keeps a fill inside its rectangles");
  check(IDirectDrawSurface7_BltFast(b, 0, 0, a, NULL, 0) == DDERR_BLTFASTCANTCLIP, "BltFast into a surface with a clipper: DDERR_BLTFASTCANTCLIP");
  IDirectDrawSurface7_SetClipper(b, NULL);
  check(refs(clipper) == 1, "the surface let go of the clipper");
  IDirectDrawClipper_Release(clipper);
  IDirectDrawSurface_Release(a1);
  IDirectDrawSurface_Release(b1);
  IDirectDrawSurface7_Release(a);
  IDirectDrawSurface7_Release(b);
}

static void test_gdi(void) {
  DDPIXELFORMAT x8 = rgb_format(32, 0xFF0000, 0xFF00, 0xFF, 0);
  IDirectDrawSurface7 *s = make_surface(32, 32, DDSCAPS_OFFSCREENPLAIN, &x8);
  if (!s) { check(0, "a surface for GetDC"); return; }
  fill(s, NULL, 0x000000);
  HDC dc = NULL;
  HRESULT hr = IDirectDrawSurface7_GetDC(s, &dc);
  check(hr == DD_OK && dc, "GetDC");
  if (hr != DD_OK) { IDirectDrawSurface7_Release(s); return; }
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  check(IDirectDrawSurface7_Lock(s, NULL, &d, DDLOCK_WAIT, NULL) == DDERR_SURFACEBUSY, "Lock while the DC is out: DDERR_SURFACEBUSY");
  IDirectDrawSurface7 *found = NULL;
  check(IDirectDraw7_GetSurfaceFromDC(g_dd, dc, &found) == DD_OK && found == s, "GetSurfaceFromDC finds the surface of the DC");
  if (found) IDirectDrawSurface7_Release(found);
  RECT r = {4, 4, 12, 8};
  HBRUSH brush = CreateSolidBrush(RGB(255, 128, 0));
  FillRect(dc, &r, brush);
  DeleteObject(brush);
  SetPixel(dc, 20, 20, RGB(0, 0, 255));
  check(IDirectDrawSurface7_ReleaseDC(s, dc) == DD_OK, "ReleaseDC");
  check(get_pixel(s, 5, 5) == 0xFF8000 && get_pixel(s, 4, 8) == 0 && get_pixel(s, 20, 20) == 0x0000FF, "what GDI drew is in the surface, the right way up");
  check(IDirectDrawSurface7_ReleaseDC(s, dc) == DDERR_NODC, "a second ReleaseDC: DDERR_NODC");
  IDirectDrawSurface7_Release(s);
  /* a format GDI cannot draw */
  DDPIXELFORMAT l8;
  memset(&l8, 0, sizeof l8);
  l8.dwSize = sizeof l8;
  l8.dwFlags = DDPF_LUMINANCE;
  l8.dwLuminanceBitCount = 8;
  l8.dwLuminanceBitMask = 0xFF;
  s = make_surface(8, 8, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, &l8);
  if (s) {
    dc = (HDC)1;
    check(FAILED(IDirectDrawSurface7_GetDC(s, &dc)) && !dc, "GetDC of a luminance surface fails");
    IDirectDrawSurface7_Release(s);
  }
}

static void test_palettes_and_formats(void) {
  PALETTEENTRY e[256];
  for (int i = 0; i < 256; ++i) { e[i].peRed = (BYTE)i; e[i].peGreen = (BYTE)(255 - i); e[i].peBlue = 0; e[i].peFlags = 0; }
  IDirectDrawPalette *p = NULL;
  HRESULT hr = IDirectDraw7_CreatePalette(g_dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256, e, &p, NULL);
  check(hr == DD_OK && p && refs(g_dd) == 2, "CreatePalette (IDirectDraw7's palette holds the object)");
  if (!p) return;
  PALETTEENTRY got[2];
  check(IDirectDrawPalette_GetEntries(p, 0, 254, 2, got) == DD_OK && got[1].peRed == 255 && got[1].peGreen == 0, "GetEntries");
  check(IDirectDrawPalette_GetEntries(p, 0, 255, 2, got) == DDERR_INVALIDPARAMS, "GetEntries beyond the palette: DDERR_INVALIDPARAMS");
  DDPIXELFORMAT p8;
  memset(&p8, 0, sizeof p8);
  p8.dwSize = sizeof p8;
  p8.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
  p8.dwRGBBitCount = 8;
  IDirectDrawSurface7 *s8 = make_surface(4, 4, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, &p8);
  DDPIXELFORMAT x8 = rgb_format(32, 0xFF0000, 0xFF00, 0xFF, 0);
  IDirectDrawSurface7 *s32 = make_surface(4, 4, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, &x8);
  if (s8 && s32) {
    check(IDirectDrawSurface7_SetPalette(s32, p) == DDERR_INVALIDPIXELFORMAT, "a palette on a 32-bit surface: DDERR_INVALIDPIXELFORMAT");
    check(IDirectDrawSurface7_SetPalette(s8, p) == DD_OK && refs(p) == 2, "SetPalette holds the palette");
    fill(s8, NULL, 200);
    hr = IDirectDrawSurface7_Blt(s32, NULL, s8, NULL, DDBLT_WAIT, NULL);
    check(hr == DD_OK && get_pixel(s32, 1, 1) == 0xC83700, "a blit from an 8-bit surface goes through its palette");
  }
  if (s8) IDirectDrawSurface7_Release(s8);
  if (s32) IDirectDrawSurface7_Release(s32);
  check(IDirectDrawPalette_Release(p) == 0 && refs(g_dd) == 1, "the palette goes, and lets go of the object");
  /* mipmaps, cube maps, a z buffer */
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  d.dwWidth = 32;
  d.dwHeight = 8;
  d.ddpfPixelFormat = x8;
  d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY;
  IDirectDrawSurface7 *m = NULL;
  hr = create_texture(&d, &m);
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  if (m) IDirectDrawSurface7_GetSurfaceDesc(m, &d);
  #ifdef _WIN64
  check(hr == DD_OK && d.dwMipMapCount == 4, "a mipmap of IDirectDraw4 gets levels until one side is 1");
#else
  check(hr == DD_OK && d.dwMipMapCount == 6, "a mipmap of IDirectDraw7 gets levels until both sides are 1");
#endif
  if (m) {
    DDSCAPS2 caps = {DDSCAPS_TEXTURE | DDSCAPS_MIPMAP, 0, 0, {0}};
    IDirectDrawSurface7 *level = NULL;
    hr = IDirectDrawSurface7_GetAttachedSurface(m, &caps, &level);
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    if (level) IDirectDrawSurface7_GetSurfaceDesc(level, &d);
    check(hr == DD_OK && d.dwWidth == 16 && d.dwHeight == 4 && (d.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL), "the next level is half the size");
    DDCOLORKEY k = {0, 0};
#ifndef _WIN64
    if (level) check(IDirectDrawSurface7_SetColorKey(level, DDCKEY_SRCBLT, &k) == DDERR_NOTONMIPMAPSUBLEVEL, "no colour key on a sublevel (version 7)");
#else
    (void)k;
#endif
    check(IDirectDrawSurface7_Release(m) == 0, "the mipmap goes with its root's last reference");
    if (level) {
      check(get_pixel(level, 0, 0) != 0xDEADBEEF, "a level the program still holds stays usable after its root went");
      check(IDirectDrawSurface7_Release(level) == 0, "and goes with its own last reference");
    }
  }
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  d.dwWidth = d.dwHeight = 16;
  d.ddpfPixelFormat = x8;
  d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY;
  d.ddsCaps.dwCaps2 = DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES;
  IDirectDrawSurface7 *cube = NULL;
  hr = create_texture(&d, &cube);
  if (cube) {
    DDSCAPS2 caps = {0, DDSCAPS2_CUBEMAP_NEGATIVEZ, 0, {0}};
    IDirectDrawSurface7 *face = NULL;
    hr = IDirectDrawSurface7_GetAttachedSurface(cube, &caps, &face);
    check(hr == DD_OK && face, "a cube map has its faces attached");
    if (face) IDirectDrawSurface7_Release(face);
    IDirectDrawSurface7_Release(cube);
  } else {
    check(0, "a cube map");
  }
  DDPIXELFORMAT z;
  memset(&z, 0, sizeof z);
  z.dwSize = sizeof z;
  z.dwFlags = DDPF_ZBUFFER;
  z.dwZBufferBitDepth = 16;
  z.dwZBitMask = 0xFFFF;
  IDirectDrawSurface7 *zs = make_surface(8, 8, DDSCAPS_ZBUFFER, &z);
  if (zs) {
    DDBLTFX fx;
    memset(&fx, 0, sizeof fx);
    fx.dwSize = sizeof fx;
    fx.dwFillDepth = 0x8000;
    hr = IDirectDrawSurface7_Blt(zs, NULL, NULL, NULL, DDBLT_DEPTHFILL | DDBLT_WAIT, &fx);
    check(hr == DD_OK && (get_pixel(zs, 7, 7) & 0xFFFF) == 0x8000, "a depth fill");
    fx.dwFillColor = 0;
    check(IDirectDrawSurface7_Blt(zs, NULL, NULL, NULL, DDBLT_COLORFILL, &fx) == DDERR_INVALIDPARAMS, "a colour fill of a z buffer: DDERR_INVALIDPARAMS");
    IDirectDrawSurface7_Release(zs);
  } else {
    check(0, "a 16-bit z buffer");
  }
  /* YUY2 onto RGB */
  DDPIXELFORMAT yuy2;
  memset(&yuy2, 0, sizeof yuy2);
  yuy2.dwSize = sizeof yuy2;
  yuy2.dwFlags = DDPF_FOURCC;
  yuy2.dwFourCC = MAKEFOURCC('Y', 'U', 'Y', '2');
  IDirectDrawSurface7 *y = make_surface(8, 8, DDSCAPS_OFFSCREENPLAIN, &yuy2), *rgb = make_surface(8, 8, DDSCAPS_OFFSCREENPLAIN, &x8);
  if (y && rgb) {
    DDBLTFX fx;
    memset(&fx, 0, sizeof fx);
    fx.dwSize = sizeof fx;
    fx.dwFillColor = 0x80EB80EB;      /* (white: Y 235, U and V 128) */
    IDirectDrawSurface7_Blt(y, NULL, NULL, NULL, DDBLT_COLORFILL, &fx);
    hr = IDirectDrawSurface7_Blt(rgb, NULL, y, NULL, DDBLT_WAIT, NULL);
    check(hr == DD_OK && get_pixel(rgb, 3, 3) == 0xFFFFFF, "a YUY2 surface blitted onto an RGB one is converted");
  } else {
    check(0, "a YUY2 surface");
  }
  if (y) IDirectDrawSurface7_Release(y);
  if (rgb) IDirectDrawSurface7_Release(rgb);
}

/* The front buffer of a full-screen flipping chain, as GDI reads it back from the window it is shown in. */
static COLORREF screen_pixel(int x, int y) {
  HDC dc = GetWindowDC(g_window);
  COLORREF c = GetPixel(dc, x, y);
  ReleaseDC(g_window, dc);
  return c;
}
static void test_fullscreen(DWORD bpp) {
  char what[128];
  HRESULT hr = IDirectDraw7_SetCooperativeLevel(g_dd, g_window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
  snprintf(what, sizeof what, "exclusive full-screen mode (%lu bits)", (unsigned long)bpp);
  check(hr == DD_OK, what);
  hr = IDirectDraw7_SetDisplayMode(g_dd, 640, 480, bpp, 0, 0);
  snprintf(what, sizeof what, "SetDisplayMode 640x480x%lu", (unsigned long)bpp);
  check(hr == DD_OK, what);
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  IDirectDraw7_GetDisplayMode(g_dd, &d);
  snprintf(what, sizeof what, "GetDisplayMode says 640x480x%lu", (unsigned long)bpp);
  check(d.dwWidth == 640 && d.dwHeight == 480 && d.ddpfPixelFormat.dwRGBBitCount == bpp, what);
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
  d.dwBackBufferCount = 1;
  d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
  IDirectDrawSurface7 *front = NULL, *back = NULL;
  hr = IDirectDraw7_CreateSurface(g_dd, &d, &front, NULL);
  check(hr == DD_OK && front, "a primary surface with a back buffer");
  if (!front) { IDirectDraw7_RestoreDisplayMode(g_dd); IDirectDraw7_SetCooperativeLevel(g_dd, g_window, DDSCL_NORMAL); return; }
  DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
  IDirectDrawSurface7_GetAttachedSurface(front, &caps, &back);
  check(back != NULL, "its back buffer");
  IDirectDrawPalette *pal = NULL;
  DWORD red = 0xFF0000, green = 0x00FF00;
  if (bpp == 8) {
    PALETTEENTRY e[256];
    memset(e, 0, sizeof e);
    e[1].peRed = 255;
    e[2].peGreen = 255;
    IDirectDraw7_CreatePalette(g_dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256, e, &pal, NULL);
    check(IDirectDrawSurface7_SetPalette(front, pal) == DD_OK, "the primary's palette");
    red = 1;
    green = 2;
  }
  if (back) {
    fill(front, NULL, red);
    fill(back, NULL, green);
    hr = IDirectDrawSurface7_Flip(front, NULL, DDFLIP_WAIT);
    check(hr == DD_OK, "Flip");
    check(get_pixel(front, 100, 100) == green && get_pixel(back, 100, 100) == red, "the front and back buffers exchanged their pictures");
    Sleep(200);
    const COLORREF shown = screen_pixel(100, 100);
    snprintf(what, sizeof what, "the window shows the front buffer through the %s (%06lx)", bpp == 8 ? "palette" : "32-bit scanout", (unsigned long)shown);
    check(shown == RGB(0, 255, 0), what);
    if (pal) {
      PALETTEENTRY blue = {0, 0, 255, 0};
      IDirectDrawPalette_SetEntries(pal, 0, 2, 1, &blue);
      Sleep(200);
      check(screen_pixel(100, 100) == RGB(0, 0, 255), "changing the primary's palette shows at once");
    }
    IDirectDrawSurface7_Release(back);
  }
  /* a mode change loses the surfaces */
  hr = IDirectDraw7_SetDisplayMode(g_dd, 800, 600, 32, 0, 0);
  check(hr == DD_OK && IDirectDrawSurface7_IsLost(front) == DDERR_SURFACELOST, "a mode change loses the primary surface");
  check(IDirectDrawSurface7_Restore(front) == DDERR_WRONGMODE, "Restore in another mode: DDERR_WRONGMODE");
  check(IDirectDrawSurface7_Release(front) == 0, "the flipping chain goes with its front's last reference");
  if (pal) IDirectDrawPalette_Release(pal);
  check(IDirectDraw7_RestoreDisplayMode(g_dd) == DD_OK, "RestoreDisplayMode");
  check(IDirectDraw7_SetCooperativeLevel(g_dd, g_window, DDSCL_NORMAL) == DD_OK, "back to the normal cooperative level");
}

int main(void) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "ddrawtest";
  RegisterClassA(&wc);
  g_window = CreateWindowExA(0, "ddrawtest", "ddrawtest", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, NULL, NULL, wc.hInstance, NULL);
  SetForegroundWindow(g_window);
  test_object();
  if (g_dd) {
    test_modes();
    test_surfaces();
    test_blits();
    test_gdi();
    test_palettes_and_formats();
    test_fullscreen(32);
    test_fullscreen(8);
    check(IDirectDraw7_Release(g_dd) == 0, "the DirectDraw object goes with its last reference");
  }
  /* DirectDrawCreate and the older interfaces */
  IDirectDraw *dd1 = NULL;
  HRESULT hr = DirectDrawCreate(NULL, &dd1, NULL);
  check(SUCCEEDED(hr) && dd1 && refs(dd1) == 1, "DirectDrawCreate makes an IDirectDraw");
  if (dd1) {
    IDirectDraw_SetCooperativeLevel(dd1, g_window, DDSCL_NORMAL);
    DDSURFACEDESC d;
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = d.dwHeight = 4;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    IDirectDrawSurface *s = NULL;
    hr = IDirectDraw_CreateSurface(dd1, &d, &s, NULL);
    check(hr == DD_OK && s && refs(dd1) == 1, "a surface of IDirectDraw holds no reference of it (as Windows)");
    if (s) IDirectDrawSurface_Release(s);
    check(IDirectDraw_Release(dd1) == 0, "IDirectDraw goes with its last reference");
  }
  DestroyWindow(g_window);
  if (g_failed == 0) printf("ddrawtest: all %d checks passed\n", g_passed);
  else printf("ddrawtest: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
