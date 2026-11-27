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

/* Part of d3d9test.c: the device's API beyond drawing - clears of rectangles, ColorFill, StretchRect, UpdateSurface, UpdateTexture, GetDC, swap chains and their
 * buffers, the front buffer, the raster status, gamma, palettes, the cursor, the levels of textures as objects and their references, private data, the kinds of
 * state blocks and recording, queries, and Direct3D 9Ex (devices, shared textures, the Ex methods). Each check reads the pixels back where it can. */

/* A pixel of a surface in memory (A8R8G8B8 or X8R8G8B8), read with LockRect. */
static DWORD surface_pixel(IDirect3DSurface9 *s, int x, int y) {
  D3DLOCKED_RECT locked;
  DWORD p = 0xDEADBEEF;
  if (SUCCEEDED(IDirect3DSurface9_LockRect(s, &locked, NULL, D3DLOCK_READONLY))) {
    p = ((const DWORD *)((const BYTE *)locked.pBits + (size_t)y * locked.Pitch))[x];
    IDirect3DSurface9_UnlockRect(s);
  }
  return p;
}
/* A target's pixel through a surface in system memory (GetRenderTargetData), as programs read targets that are not lockable. */
static DWORD target_pixel(IDirect3DSurface9 *target, int x, int y) {
  D3DSURFACE_DESC desc;
  IDirect3DSurface9 *memory = NULL;
  DWORD p = 0xDEADBEEF;
  if (FAILED(IDirect3DSurface9_GetDesc(target, &desc))) return p;
  if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &memory, NULL)) &&
      SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_device, target, memory)))
    p = surface_pixel(memory, x, y);
  if (memory) IDirect3DSurface9_Release(memory);
  return p;
}
static ULONG refs(void *object) {
  IUnknown *u = object;
  u->lpVtbl->AddRef(u);
  return u->lpVtbl->Release(u);
}

static void test_clear_rectangles(void) {
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  clear(0xFF000000);
  const D3DRECT rect = {8, 8, 24, 24};
  check(IDirect3DDevice9_Clear(g_device, 1, &rect, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0) == D3D_OK, "Clear of a rectangle");
  read_back();
  check(pixel(8, 8) == 0xFF0000 && pixel(23, 23) == 0xFF0000 && pixel(7, 8) == 0 && pixel(24, 23) == 0 && pixel(40, 40) == 0, "... clears that rectangle and nothing else");
  const D3DRECT upside_down = {40, 40, 30, 30}, two[2] = {{0, 0, 4, 4}, {60, 60, 64, 64}};
  check(IDirect3DDevice9_Clear(g_device, 1, &upside_down, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0) == D3D_OK &&
        IDirect3DDevice9_Clear(g_device, 0, two, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0) == D3D_OK && read_back() && pixel(35, 35) == 0 && pixel(1, 1) == 0,
        "a rectangle upside down, and rectangles with no count, clear nothing");
  check(IDirect3DDevice9_Clear(g_device, 2, two, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0) == D3D_OK && read_back() && pixel(1, 1) == 0x0000FF && pixel(62, 62) == 0x0000FF &&
        pixel(30, 2) == 0, "two rectangles at once");
  /* the viewport limits a clear, and the scissor rectangle when its test is on */
  D3DVIEWPORT9 old, vp = {32, 32, 16, 16, 0.0f, 1.0f};
  IDirect3DDevice9_GetViewport(g_device, &old);
  clear(0xFF000000);
  IDirect3DDevice9_SetViewport(g_device, &vp);
  IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);
  IDirect3DDevice9_SetViewport(g_device, &old);
  read_back();
  check(pixel(32, 32) == 0xFFFFFF && pixel(47, 47) == 0xFFFFFF && pixel(31, 32) == 0 && pixel(48, 40) == 0, "a clear with no rectangles is of the viewport");
  const RECT scissor = {0, 0, 10, 64};
  clear(0xFF000000);
  IDirect3DDevice9_SetScissorRect(g_device, &scissor);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SCISSORTESTENABLE, TRUE);
  IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SCISSORTESTENABLE, FALSE);
  read_back();
  check(pixel(5, 30) == 0x00FF00 && pixel(12, 30) == 0, "... and of the scissor rectangle when its test is on");
  /* the depth buffer of a rectangle only: a rectangle drawn at depth 0.5 shows where the depth was cleared to 1, not where it is 0 */
  IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 0.0f, 0);
  const D3DRECT left = {0, 0, 32, 64};
  check(IDirect3DDevice9_Clear(g_device, 1, &left, D3DCLEAR_ZBUFFER, 0, 1.0f, 0) == D3D_OK, "Clear of the depth of a rectangle");
  screen_quad(0, 0, 64, 64, 0.5f, 0xFFFFFFFF);
  read_back();
  check(pixel(10, 30) == 0xFFFFFF && pixel(50, 30) == 0, "... clears its depth and no other (and not its colour)");
  /* the stencil of a rectangle, then a stencil test that keeps 5 */
  IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.0f, 0);
  const D3DRECT top = {0, 0, 64, 20};
  check(IDirect3DDevice9_Clear(g_device, 1, &top, D3DCLEAR_STENCIL, 0, 1.0f, 5) == D3D_OK, "Clear of the stencil of a rectangle");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_STENCILENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_STENCILREF, 5);
  screen_quad(0, 0, 64, 64, 0.5f, 0xFF00FF00);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_STENCILENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
  read_back();
  check(pixel(30, 10) == 0x00FF00 && pixel(30, 40) == 0, "... which a stencil test then reads");
  IDirect3DSurface9 *depth = NULL;
  IDirect3DDevice9_GetDepthStencilSurface(g_device, &depth);
  IDirect3DDevice9_SetDepthStencilSurface(g_device, NULL);
  check(IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0) == D3DERR_INVALIDCALL, "a clear of depth with no depth buffer is refused");
  IDirect3DDevice9_SetDepthStencilSurface(g_device, depth);
  if (depth) IDirect3DSurface9_Release(depth);
}

static void test_fills_and_copies(void) {
  IDirect3DSurface9 *back = NULL, *rt = NULL, *rt2 = NULL, *plain = NULL, *memory = NULL, *small = NULL;
  IDirect3DDevice9_GetBackBuffer(g_device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
  HRESULT hr = IDirect3DDevice9_CreateRenderTarget(g_device, 32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateRenderTarget(g_device, 16, 16, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt2, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 32, 32, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &plain, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &memory, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 8, 8, D3DFMT_R5G6B5, D3DPOOL_SYSTEMMEM, &small, NULL);
  check(SUCCEEDED(hr) && back && rt && rt2 && plain && memory && small, "render targets and off-screen plain surfaces of the default pool and of memory");
  if (FAILED(hr)) goto done;
  D3DLOCKED_RECT locked;
  check(IDirect3DSurface9_LockRect(rt, &locked, NULL, 0) == D3DERR_INVALIDCALL, "a render target made not lockable cannot be locked");
  /* ColorFill: all of a target, a rectangle of it, and a plain surface of the default pool */
  const RECT corner = {0, 0, 8, 8};
  check(IDirect3DDevice9_ColorFill(g_device, rt, NULL, 0xFF00FF00) == D3D_OK && IDirect3DDevice9_ColorFill(g_device, rt, &corner, 0xFFFF0000) == D3D_OK,
        "ColorFill of a render target, all of it and a rectangle");
  check((target_pixel(rt, 2, 2) & 0xFFFFFF) == 0xFF0000 && (target_pixel(rt, 20, 20) & 0xFFFFFF) == 0x00FF00 && (target_pixel(rt, 8, 2) & 0xFFFFFF) == 0x00FF00,
        "... the rectangle in its colour, the rest in the other (GetRenderTargetData)");
  check(IDirect3DDevice9_ColorFill(g_device, plain, &corner, 0x80123456) == D3D_OK && surface_pixel(plain, 3, 3) == 0x80123456, "ColorFill of an off-screen plain surface");
  check(IDirect3DDevice9_ColorFill(g_device, memory, NULL, 0) == D3DERR_INVALIDCALL, "... not of a surface in system memory");
  /* StretchRect: a part of a target into a part of another, of the same size; scaled by two; from a plain surface; to the back buffer */
  const RECT from = {0, 0, 16, 16}, to = {0, 0, 16, 16};
  check(IDirect3DDevice9_StretchRect(g_device, rt, &from, rt2, &to, D3DTEXF_NONE) == D3D_OK && (target_pixel(rt2, 3, 3) & 0xFFFFFF) == 0xFF0000 &&
        (target_pixel(rt2, 12, 12) & 0xFFFFFF) == 0x00FF00, "StretchRect of a part of one size");
  const RECT quarter = {0, 0, 8, 8}, whole = {0, 0, 16, 16};
  check(IDirect3DDevice9_StretchRect(g_device, rt, &quarter, rt2, &whole, D3DTEXF_POINT) == D3D_OK && (target_pixel(rt2, 15, 15) & 0xFFFFFF) == 0xFF0000 &&
        (target_pixel(rt2, 0, 0) & 0xFFFFFF) == 0xFF0000, "StretchRect that scales by two (D3DTEXF_POINT)");
  clear(0xFF000000);
  const RECT strip = {4, 0, 12, 16}, wide = {0, 0, 64, 32};
  check(IDirect3DDevice9_StretchRect(g_device, rt, &strip, back, &wide, D3DTEXF_LINEAR) == D3D_OK && read_back() && pixel(10, 4) == 0xFF0000 && pixel(60, 4) == 0x00FF00 &&
        pixel(30, 40) == 0, "StretchRect into the back buffer, scaled each way (D3DTEXF_LINEAR)");
  clear(0xFF000000);
  const RECT spot = {40, 40, 50, 50};
  check(IDirect3DDevice9_StretchRect(g_device, plain, &corner, back, &spot, D3DTEXF_NONE) == D3D_OK && read_back() && pixel(45, 45) == 0x123456 && pixel(39, 45) == 0,
        "StretchRect of an off-screen plain surface into a target");
  check(IDirect3DDevice9_StretchRect(g_device, rt, NULL, memory, NULL, D3DTEXF_NONE) == D3DERR_INVALIDCALL &&
        IDirect3DDevice9_StretchRect(g_device, rt, NULL, rt2, NULL, D3DTEXF_ANISOTROPIC) == D3DERR_INVALIDCALL,
        "StretchRect refuses a destination in memory and the filters it has not");
  /* UpdateSurface: a part of a surface in memory to a place of one of the default pool */
  memset(&locked, 0, sizeof locked);
  if (SUCCEEDED(IDirect3DSurface9_LockRect(memory, &locked, NULL, 0))) {
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x) ((DWORD *)((BYTE *)locked.pBits + (size_t)y * locked.Pitch))[x] = x < 16 ? 0xFF0000FF : 0xFFFFFF00;
    IDirect3DSurface9_UnlockRect(memory);
  }
  const RECT part = {12, 0, 20, 8};
  const POINT at = {0, 16};
  check(IDirect3DDevice9_UpdateSurface(g_device, memory, &part, rt, &at) == D3D_OK && (target_pixel(rt, 1, 18) & 0xFFFFFF) == 0x0000FF && (target_pixel(rt, 6, 18) & 0xFFFFFF) == 0xFFFF00 &&
        (target_pixel(rt, 10, 18) & 0xFFFFFF) == 0x00FF00, "UpdateSurface of a part to a point");
  check(IDirect3DDevice9_UpdateSurface(g_device, small, NULL, rt, NULL) == D3DERR_INVALIDCALL && IDirect3DDevice9_UpdateSurface(g_device, rt, NULL, rt2, NULL) == D3DERR_INVALIDCALL,
        "UpdateSurface refuses another format and a source that is not in memory");
  /* GetRenderTargetData of the back buffer */
  clear(0xFF336699);
  IDirect3DSurface9 *screen = NULL;
  IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, SIZE, SIZE, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &screen, NULL);
  check(screen && IDirect3DDevice9_GetRenderTargetData(g_device, back, screen) == D3D_OK && (surface_pixel(screen, 5, 5) & 0xFFFFFF) == 0x336699,
        "GetRenderTargetData of the back buffer");
  if (screen) IDirect3DSurface9_Release(screen);
done:
  if (back) IDirect3DSurface9_Release(back);
  if (rt) IDirect3DSurface9_Release(rt);
  if (rt2) IDirect3DSurface9_Release(rt2);
  if (plain) IDirect3DSurface9_Release(plain);
  if (memory) IDirect3DSurface9_Release(memory);
  if (small) IDirect3DSurface9_Release(small);
}

static void test_update_texture(void) {
  IDirect3DTexture9 *system = NULL, *video = NULL;
  HRESULT hr = IDirect3DDevice9_CreateTexture(g_device, 8, 8, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &system, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateTexture(g_device, 8, 8, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &video, NULL);
  check(SUCCEEDED(hr) && system && video, "a texture in memory and one of the default pool");
  if (FAILED(hr)) goto done;
  D3DLOCKED_RECT locked;
  check(IDirect3DTexture9_LockRect(video, 0, &locked, NULL, 0) == D3DERR_INVALIDCALL, "a texture of the default pool that is not dynamic cannot be locked");
  for (UINT level = 0; level < 2; ++level)
    if (SUCCEEDED(IDirect3DTexture9_LockRect(system, level, &locked, NULL, 0))) {
      for (int y = 0; y < 8 >> level; ++y)
        for (int x = 0; x < 8 >> level; ++x) ((DWORD *)((BYTE *)locked.pBits + (size_t)y * locked.Pitch))[x] = level ? 0xFF00FF00 : 0xFFFF0000;
      IDirect3DTexture9_UnlockRect(system, level);
    }
  check(IDirect3DDevice9_UpdateTexture(g_device, (IDirect3DBaseTexture9 *)system, (IDirect3DBaseTexture9 *)video) == D3D_OK, "UpdateTexture");
  /* (the default pool's texture drawn with point sampling: its top level) */
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)video);
  IDirect3DDevice9_SetSamplerState(g_device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(pixel(32, 32) == 0xFF0000, "... the top level is copied");
  /* a change that is not marked dirty (D3DLOCK_NO_DIRTY_UPDATE) is not copied; one marked with AddDirtyRect is */
  if (SUCCEEDED(IDirect3DTexture9_LockRect(system, 0, &locked, NULL, D3DLOCK_NO_DIRTY_UPDATE))) {
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) ((DWORD *)((BYTE *)locked.pBits + (size_t)y * locked.Pitch))[x] = 0xFF0000FF;
    IDirect3DTexture9_UnlockRect(system, 0);
  }
  IDirect3DDevice9_UpdateTexture(g_device, (IDirect3DBaseTexture9 *)system, (IDirect3DBaseTexture9 *)video);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  const DWORD unmarked = pixel(32, 32);
  const RECT left_half = {0, 0, 4, 8};
  IDirect3DTexture9_AddDirtyRect(system, &left_half);
  IDirect3DDevice9_UpdateTexture(g_device, (IDirect3DBaseTexture9 *)system, (IDirect3DBaseTexture9 *)video);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(unmarked == 0xFF0000 && pixel(8, 32) == 0x0000FF && pixel(56, 32) == 0xFF0000, "... what changed is copied as far as it is marked dirty");
  IDirect3DDevice9_SetTexture(g_device, 0, NULL);
  check(IDirect3DDevice9_UpdateTexture(g_device, (IDirect3DBaseTexture9 *)video, (IDirect3DBaseTexture9 *)system) == D3DERR_INVALIDCALL, "UpdateTexture the other way is refused");
done:
  if (system) IDirect3DTexture9_Release(system);
  if (video) IDirect3DTexture9_Release(video);
}

static void test_getdc(void) {
  IDirect3DSurface9 *s = NULL, *l8 = NULL;
  HDC dc = NULL;
  HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 16, 16, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &s, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 16, 16, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &l8, NULL);
  if (FAILED(hr)) { check(0, "surfaces for GetDC"); goto done; }
  IDirect3DDevice9_ColorFill(g_device, s, NULL, 0);      /* (refused in memory: the surface is zeros) */
  check(IDirect3DSurface9_GetDC(s, &dc) == D3D_OK && dc && GetObjectType(dc) == OBJ_MEMDC, "GetDC of an X8R8G8B8 surface: a memory DC");
  if (dc) {
    HDC again = (HDC)1;
    D3DLOCKED_RECT locked;
    check(IDirect3DSurface9_GetDC(s, &again) == D3DERR_INVALIDCALL && again == (HDC)1 && IDirect3DSurface9_LockRect(s, &locked, NULL, 0) == D3DERR_INVALIDCALL,
          "... a second GetDC and LockRect are refused while it is out");
    const RECT box = {4, 4, 8, 8};
    HBRUSH brush = CreateSolidBrush(RGB(0x10, 0x20, 0x30));
    FillRect(dc, &box, brush);
    DeleteObject(brush);
    check(IDirect3DSurface9_ReleaseDC(s, dc) == D3D_OK && IDirect3DSurface9_ReleaseDC(s, dc) == D3DERR_INVALIDCALL, "ReleaseDC, once");
    check((surface_pixel(s, 5, 5) & 0xFFFFFF) == 0x102030 && (surface_pixel(s, 1, 1) & 0xFFFFFF) == 0, "what GDI drew is in the surface");
  }
  dc = (HDC)1;
  check(IDirect3DSurface9_GetDC(l8, &dc) == D3DERR_INVALIDCALL && dc == (HDC)1, "GetDC of a format GDI has not is refused");
done:
  if (s) IDirect3DSurface9_Release(s);
  if (l8) IDirect3DSurface9_Release(l8);
}

static void test_swap_chains(void) {
  IDirect3DSwapChain9 *implicit = NULL, *extra = NULL;
  IDirect3DSurface9 *back = NULL, *back2 = NULL, *b0 = NULL, *b1 = NULL, *saved = NULL;
  D3DPRESENT_PARAMETERS pp;
  check(IDirect3DDevice9_GetNumberOfSwapChains(g_device) == 1 && IDirect3DDevice9_GetSwapChain(g_device, 0, &implicit) == D3D_OK && implicit &&
        IDirect3DDevice9_GetSwapChain(g_device, 1, &extra) == D3DERR_INVALIDCALL && !extra, "the implicit swap chain, and no other");
  if (!implicit) return;
  memset(&pp, 0, sizeof pp);
  check(IDirect3DSwapChain9_GetPresentParameters(implicit, &pp) == D3D_OK && pp.BackBufferWidth == SIZE && pp.BackBufferCount == 1 && pp.hDeviceWindow == g_window,
        "its present parameters");
  IDirect3DSwapChain9_GetBackBuffer(implicit, 0, D3DBACKBUFFER_TYPE_MONO, &back);
  IDirect3DDevice9_GetBackBuffer(g_device, 0, 0, D3DBACKBUFFER_TYPE_LEFT, &back2);
  IDirect3DSwapChain9 *container = NULL;
  if (back) IDirect3DSurface9_GetContainer(back, &IID_IDirect3DSwapChain9, (void **)&container);
  check(back && back == back2 && container == implicit, "its back buffer is the device's, and the swap chain is its container");
  if (container) IDirect3DSwapChain9_Release(container);
  const ULONG device_refs = refs(g_device);
  if (back2) IDirect3DSurface9_Release(back2);
  check(back && IDirect3DSurface9_Release(back) == 0 && refs(g_device) == device_refs - 1, "the back buffer counts from 0 and holds the device while it has references");
  back = back2 = NULL;
  D3DRASTER_STATUS raster;
  D3DDISPLAYMODE mode;
  memset(&raster, 0xFF, sizeof raster);
  check(IDirect3DSwapChain9_GetRasterStatus(implicit, &raster) == D3D_OK && IDirect3DSwapChain9_GetDisplayMode(implicit, &mode) == D3D_OK &&
        (raster.InVBlank ? raster.ScanLine == 0 : raster.ScanLine < mode.Height), "the raster status: a scan line of the display, 0 in the vertical blank");
  /* an additional swap chain of two back buffers that flip: a window's size and the device's format */
  memset(&pp, 0, sizeof pp);
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_FLIP;
  pp.BackBufferCount = 2;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  RECT client;
  GetClientRect(g_window, &client);
  check(IDirect3DDevice9_CreateAdditionalSwapChain(g_device, &pp, &extra) == D3D_OK && extra && pp.BackBufferWidth == (UINT)client.right && pp.hDeviceWindow == NULL,
        "CreateAdditionalSwapChain: the window's size");
  if (!extra) goto done;
  memset(&pp, 0, sizeof pp);
  IDirect3DSwapChain9_GetPresentParameters(extra, &pp);
  check(pp.hDeviceWindow == g_window && pp.BackBufferCount == 2, "... on the device's window, with its two back buffers");
  check(IDirect3DSwapChain9_GetBackBuffer(extra, 0, 0, &b0) == D3D_OK && IDirect3DSwapChain9_GetBackBuffer(extra, 1, 0, &b1) == D3D_OK &&
        IDirect3DSwapChain9_GetBackBuffer(extra, 2, 0, &back) == D3DERR_INVALIDCALL && !back, "its back buffers, and no third");
  if (!b0 || !b1) goto done;
  IDirect3DDevice9_ColorFill(g_device, b0, NULL, 0xFFFF0000);
  IDirect3DDevice9_ColorFill(g_device, b1, NULL, 0xFF00FF00);
  check(IDirect3DSwapChain9_Present(extra, NULL, NULL, NULL, NULL, 0) == D3D_OK, "Present of the additional swap chain");
  check((target_pixel(b0, 1, 1) & 0xFFFFFF) == 0x00FF00, "... the buffers flip: the first back buffer has the second's pixels now");
  IDirect3DDevice9_GetRenderTarget(g_device, 0, &saved);
  check(IDirect3DDevice9_SetRenderTarget(g_device, 0, b1) == D3D_OK && IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0) == D3D_OK &&
        (target_pixel(b1, 2, 2) & 0xFFFFFF) == 0x0000FF, "a back buffer of the additional swap chain as the render target");
  IDirect3DDevice9_SetRenderTarget(g_device, 0, saved);
done:
  if (saved) IDirect3DSurface9_Release(saved);
  if (b0) IDirect3DSurface9_Release(b0);
  if (b1) IDirect3DSurface9_Release(b1);
  if (extra) check(IDirect3DSwapChain9_Release(extra) == 0, "the additional swap chain is gone with its last reference");
  IDirect3DSwapChain9_Release(implicit);
}

static void test_front_buffer_gamma_cursor(void) {
  D3DDISPLAYMODE mode;
  IDirect3DDevice9_GetDisplayMode(g_device, 0, &mode);
  IDirect3DSurface9 *front = NULL;
  clear(0xFF4080C0);
  IDirect3DDevice9_Present(g_device, NULL, NULL, NULL, NULL);
  POINT corner = {0, 0};
  ClientToScreen(g_window, &corner);
  check(SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, mode.Width, mode.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &front, NULL)) &&
        IDirect3DDevice9_GetFrontBufferData(g_device, 0, front) == D3D_OK, "GetFrontBufferData into a surface of the display's size");
  if (front) {
    const DWORD p = surface_pixel(front, corner.x + 8, corner.y + 8);
    if ((p & 0xFFFFFF) != 0x4080C0 || (p >> 24) != 0xFF) printf("d3d9test: the front buffer at (%ld, %ld) of %ux%u: %08lx\n", corner.x + 8, corner.y + 8, mode.Width, mode.Height, p);
    check((p & 0xFFFFFF) == 0x4080C0 && (p >> 24) == 0xFF, "... the picture shown is where the window is");
    IDirect3DSurface9_Release(front);
  }
  IDirect3DSurface9 *small = NULL;
  IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 8, 8, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &small, NULL);
  check(small && IDirect3DDevice9_GetFrontBufferData(g_device, 0, small) == D3DERR_INVALIDCALL, "... not into one of another size");
  /* the gamma ramp: what is set is given back */
  D3DGAMMARAMP ramp, got;
  for (int i = 0; i < 256; ++i) ramp.red[i] = ramp.green[i] = ramp.blue[i] = (WORD)((255 - i) * 257);
  IDirect3DDevice9_SetGammaRamp(g_device, 0, D3DSGR_NO_CALIBRATION, &ramp);
  memset(&got, 0, sizeof got);
  IDirect3DDevice9_GetGammaRamp(g_device, 0, &got);
  check(memcmp(&ramp, &got, sizeof ramp) == 0, "SetGammaRamp and GetGammaRamp");
  for (int i = 0; i < 256; ++i) ramp.red[i] = ramp.green[i] = ramp.blue[i] = (WORD)(i * 257);
  IDirect3DDevice9_SetGammaRamp(g_device, 0, D3DSGR_NO_CALIBRATION, &ramp);
  /* the cursor: an A8R8G8B8 surface of sides that are powers of two */
  IDirect3DSurface9 *cursor = NULL, *odd = NULL;
  IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &cursor, NULL);
  IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 3, 2, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &odd, NULL);
  check(cursor && odd && IDirect3DDevice9_SetCursorProperties(g_device, 1, 1, cursor) == D3D_OK && IDirect3DDevice9_SetCursorProperties(g_device, 0, 0, odd) == D3DERR_INVALIDCALL &&
        IDirect3DDevice9_SetCursorProperties(g_device, 40, 0, cursor) == D3DERR_INVALIDCALL, "SetCursorProperties: powers of two, the hot spot inside");
  check(IDirect3DDevice9_ShowCursor(g_device, TRUE) == FALSE && IDirect3DDevice9_ShowCursor(g_device, FALSE) == TRUE, "ShowCursor answers what was shown before");
  if (cursor) IDirect3DSurface9_Release(cursor);
  if (odd) IDirect3DSurface9_Release(odd);
  if (small) IDirect3DSurface9_Release(small);
  check(IDirect3DDevice9_SetDialogBoxMode(g_device, TRUE) == D3DERR_INVALIDCALL && IDirect3DDevice9_SetDialogBoxMode(g_device, FALSE) == D3D_OK,
        "SetDialogBoxMode: not of a windowed device");
}

static void test_palettes(void) {
  PALETTEENTRY entries[256], got[256];
  memset(entries, 0, sizeof entries);
  entries[1].peRed = 0xFF; entries[1].peFlags = 0xFF;
  entries[2].peBlue = 0xFF; entries[2].peFlags = 0xFF;
  UINT current = 7;
  check(IDirect3DDevice9_SetCurrentTexturePalette(g_device, 3) == D3DERR_INVALIDCALL, "a palette that was never set cannot be current");
  check(IDirect3DDevice9_SetPaletteEntries(g_device, 3, entries) == D3D_OK && IDirect3DDevice9_SetCurrentTexturePalette(g_device, 3) == D3D_OK &&
        IDirect3DDevice9_GetCurrentTexturePalette(g_device, &current) == D3D_OK && current == 3 && IDirect3DDevice9_GetPaletteEntries(g_device, 3, got) == D3D_OK &&
        got[1].peRed == 0xFF && got[2].peBlue == 0xFF, "SetPaletteEntries, SetCurrentTexturePalette and the Get methods");
  IDirect3DTexture9 *texture = NULL;
  const HRESULT hr = IDirect3DDevice9_CreateTexture(g_device, 2, 2, 1, 0, D3DFMT_P8, D3DPOOL_MANAGED, &texture, NULL);
  check(hr == D3D_OK && texture, "a P8 texture");
  if (!texture) return;
  D3DLOCKED_RECT locked;
  if (SUCCEEDED(IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0))) {
    memset(locked.pBits, 1, 2);
    memset((BYTE *)locked.pBits + locked.Pitch, 1, 2);
    IDirect3DTexture9_UnlockRect(texture, 0);
  }
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)texture);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  const DWORD with_red = pixel(32, 32);
  entries[1].peRed = 0; entries[1].peGreen = 0xFF;
  IDirect3DDevice9_SetPaletteEntries(g_device, 3, entries);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(with_red == 0xFF0000 && pixel(32, 32) == 0x00FF00, "the P8 texture is drawn through the current palette, as it is when it is drawn");
  IDirect3DDevice9_SetTexture(g_device, 0, NULL);
  IDirect3DTexture9_Release(texture);
}

static void test_levels_and_references(void) {
  IDirect3DTexture9 *texture = NULL;
  IDirect3DVolumeTexture9 *volume_texture = NULL;
  IDirect3DSurface9 *level = NULL, *again = NULL;
  IDirect3DVolume9 *volume = NULL, *volume2 = NULL;
  IDirect3DDevice9_CreateTexture(g_device, 8, 8, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL);
  IDirect3DDevice9_CreateVolumeTexture(g_device, 4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume_texture, NULL);
  if (!texture || !volume_texture) { check(0, "textures for the levels"); goto done; }
  const ULONG device_refs = refs(g_device), texture_refs = refs(texture);
  IDirect3DTexture9_GetSurfaceLevel(texture, 1, &level);
  IDirect3DTexture9_GetSurfaceLevel(texture, 1, &again);
  check(level && level == again && refs(texture) == texture_refs + 2 && refs(level) == texture_refs + 2 && refs(g_device) == device_refs,
        "a texture's level is one surface, which counts with its texture and not with the device");
  IDirect3DTexture9 *container = NULL;
  if (level) IDirect3DSurface9_GetContainer(level, &IID_IDirect3DTexture9, (void **)&container);
  check(container == texture, "... and the texture is its container");
  if (container) IDirect3DTexture9_Release(container);
  check(IDirect3DVolumeTexture9_GetVolumeLevel(volume_texture, 0, &volume) == D3D_OK && IDirect3DVolumeTexture9_GetVolumeLevel(volume_texture, 0, &volume2) == D3D_OK &&
        volume && volume == volume2 && refs(volume) == refs(volume_texture), "a volume texture's level is an IDirect3DVolume9 that counts with its texture");
  if (volume) {
    D3DLOCKED_BOX box;
    D3DVOLUME_DESC desc;
    const D3DBOX inside = {1, 1, 3, 3, 2, 4};
    check(IDirect3DVolume9_GetDesc(volume, &desc) == D3D_OK && desc.Depth == 4 && desc.Width == 4 && IDirect3DVolume9_LockBox(volume, &box, &inside, 0) == D3D_OK &&
          box.RowPitch == 16 && box.SlicePitch == 64 && IDirect3DVolume9_UnlockBox(volume) == D3D_OK, "LockBox of a volume, a box inside it");
    IDirect3DVolumeTexture9 *vc = NULL;
    IDirect3DVolume9_GetContainer(volume, &IID_IDirect3DVolumeTexture9, (void **)&vc);
    check(vc == volume_texture, "... the volume texture is its container");
    if (vc) IDirect3DVolumeTexture9_Release(vc);
  }
  /* private data: bytes, and an interface that is held */
  static const GUID tag = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
  DWORD data = 0x11223344, size = sizeof data, back_data = 0;
  check(level && IDirect3DSurface9_SetPrivateData(level, &tag, &data, sizeof data, 0) == D3D_OK && IDirect3DSurface9_GetPrivateData(level, &tag, &back_data, &size) == D3D_OK &&
        back_data == data, "SetPrivateData and GetPrivateData");
  size = 1;
  check(level && IDirect3DSurface9_GetPrivateData(level, &tag, &back_data, &size) == D3DERR_MOREDATA && size == sizeof data && IDirect3DSurface9_FreePrivateData(level, &tag) == D3D_OK &&
        IDirect3DSurface9_FreePrivateData(level, &tag) == D3DERR_NOTFOUND, "... too small a buffer, FreePrivateData");
  IUnknown *unknown = (IUnknown *)g_device;
  const ULONG before = refs(g_device);
  check(level && IDirect3DSurface9_SetPrivateData(level, &tag, unknown, sizeof unknown, D3DSPD_IUNKNOWN) == D3D_OK && refs(g_device) == before + 1 &&
        IDirect3DSurface9_FreePrivateData(level, &tag) == D3D_OK && refs(g_device) == before, "an interface as private data is held while it is there");
  check(IDirect3DTexture9_SetPriority(texture, 5) == 0 && IDirect3DTexture9_GetPriority(texture) == 5 && IDirect3DSurface9_GetPriority(level) == 0,
        "a managed texture keeps a priority (its surfaces do not)");
done:
  if (volume) IDirect3DVolume9_Release(volume);
  if (volume2) IDirect3DVolume9_Release(volume2);
  if (level) IDirect3DSurface9_Release(level);
  if (again) IDirect3DSurface9_Release(again);
  if (texture) check(IDirect3DTexture9_Release(texture) == 0, "the texture is gone with its last reference (its levels' too)");
  if (volume_texture) IDirect3DVolumeTexture9_Release(volume_texture);
}

static void test_state_block_kinds(void) {
  IDirect3DStateBlock9 *pixel_block = NULL, *vertex_block = NULL, *recorded = NULL;
  DWORD value = 0;
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHAREF, 10);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_CW);
  IDirect3DDevice9_CreateStateBlock(g_device, D3DSBT_PIXELSTATE, &pixel_block);
  IDirect3DDevice9_CreateStateBlock(g_device, D3DSBT_VERTEXSTATE, &vertex_block);
  check(pixel_block && vertex_block, "state blocks of the pixel and the vertex states");
  if (!pixel_block || !vertex_block) goto done;
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHAREF, 20);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_CCW);
  IDirect3DStateBlock9_Apply(pixel_block);
  DWORD cull = 0;
  IDirect3DDevice9_GetRenderState(g_device, D3DRS_ALPHAREF, &value);
  IDirect3DDevice9_GetRenderState(g_device, D3DRS_CULLMODE, &cull);
  check(value == 10 && cull == D3DCULL_CCW, "D3DSBT_PIXELSTATE has the alpha reference and not the cull mode");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHAREF, 20);
  IDirect3DStateBlock9_Apply(vertex_block);
  IDirect3DDevice9_GetRenderState(g_device, D3DRS_ALPHAREF, &value);
  IDirect3DDevice9_GetRenderState(g_device, D3DRS_CULLMODE, &cull);
  check(value == 20 && cull == D3DCULL_CW, "D3DSBT_VERTEXSTATE has the cull mode and not the alpha reference");
  /* recording: what is set goes into the block, not into the device; the block applies only that */
  check(IDirect3DDevice9_BeginStateBlock(g_device) == D3D_OK && IDirect3DDevice9_BeginStateBlock(g_device) == D3DERR_INVALIDCALL, "BeginStateBlock, once");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHAREF, 30);
  IDirect3DDevice9_GetRenderState(g_device, D3DRS_ALPHAREF, &value);
  check(value == 20, "... a state set while recording does not change the device");
  check(IDirect3DDevice9_EndStateBlock(g_device, &recorded) == D3D_OK && recorded && IDirect3DDevice9_EndStateBlock(g_device, &recorded) == D3DERR_INVALIDCALL, "EndStateBlock");
  if (recorded) {
    IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DStateBlock9_Apply(recorded);
    IDirect3DDevice9_GetRenderState(g_device, D3DRS_ALPHAREF, &value);
    IDirect3DDevice9_GetRenderState(g_device, D3DRS_CULLMODE, &cull);
    check(value == 30 && cull == D3DCULL_NONE, "a recorded block applies what was recorded, and nothing else");
    IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHAREF, 40);
    IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_CW);
    IDirect3DStateBlock9_Capture(recorded);
    IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHAREF, 0);
    IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DStateBlock9_Apply(recorded);
    IDirect3DDevice9_GetRenderState(g_device, D3DRS_ALPHAREF, &value);
    IDirect3DDevice9_GetRenderState(g_device, D3DRS_CULLMODE, &cull);
    check(value == 40 && cull == D3DCULL_NONE, "... and Capture takes only that");
  }
  /* lights by any number, in a block */
  D3DLIGHT9 light;
  memset(&light, 0, sizeof light);
  light.Type = D3DLIGHT_POINT;
  light.Range = 10.0f;
  BOOL on = FALSE;
  check(IDirect3DDevice9_SetLight(g_device, 1000, &light) == D3D_OK && IDirect3DDevice9_LightEnable(g_device, 1000, TRUE) == D3D_OK &&
        IDirect3DDevice9_GetLightEnable(g_device, 1000, &on) == D3D_OK && on, "a light of number 1000");
  IDirect3DDevice9_LightEnable(g_device, 1000, FALSE);
done:
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHAREF, 0);
  if (pixel_block) IDirect3DStateBlock9_Release(pixel_block);
  if (vertex_block) IDirect3DStateBlock9_Release(vertex_block);
  if (recorded) IDirect3DStateBlock9_Release(recorded);
}

static void test_queries(void) {
  IDirect3DQuery9 *event = NULL, *occlusion = NULL, *stamp = NULL, *freq = NULL;
  BOOL done_ = FALSE;
  check(IDirect3DDevice9_CreateQuery(g_device, D3DQUERYTYPE_EVENT, NULL) == D3D_OK && IDirect3DDevice9_CreateQuery(g_device, D3DQUERYTYPE_RESOURCEMANAGER, NULL) == D3DERR_NOTAVAILABLE &&
        IDirect3DDevice9_CreateQuery(g_device, (D3DQUERYTYPE)30, NULL) == D3DERR_NOTAVAILABLE, "CreateQuery with NULL asks whether a type is there");
  IDirect3DDevice9_CreateQuery(g_device, D3DQUERYTYPE_EVENT, &event);
  check(event && IDirect3DQuery9_GetDataSize(event) == sizeof(BOOL), "an event query");
  if (event) {
    screen_quad(0, 0, 8, 8, 0.5f, 0xFFFFFFFF);
    IDirect3DQuery9_Issue(event, D3DISSUE_END);
    check(IDirect3DQuery9_GetData(event, &done_, sizeof done_, D3DGETDATA_FLUSH) == S_OK && done_, "... done once the commands before it have run");
  }
  IDirect3DDevice9_CreateQuery(g_device, D3DQUERYTYPE_OCCLUSION, &occlusion);
  if (occlusion) {
    DWORD samples = 0xDEADBEEF;
    clear(0xFF000000);
    check(IDirect3DQuery9_Issue(occlusion, D3DISSUE_BEGIN) == D3D_OK, "an occlusion query begun");
    screen_quad(0, 0, 16, 16, 0.5f, 0xFFFFFFFF);
    screen_quad(20, 20, 24, 24, 0.5f, 0xFFFFFFFF);
    IDirect3DQuery9_Issue(occlusion, D3DISSUE_END);
    HRESULT hr;
    while ((hr = IDirect3DQuery9_GetData(occlusion, &samples, sizeof samples, D3DGETDATA_FLUSH)) == S_FALSE) {}
    check(hr == S_OK && samples == 16 * 16 + 4 * 4, "... counts the samples that passed (two rectangles of 16 x 16 and 4 x 4)");
    /* across a submission of the list: a lock that reads sends it between the draws */
    IDirect3DQuery9_Issue(occlusion, D3DISSUE_BEGIN);
    screen_quad(0, 0, 10, 10, 0.5f, 0xFFFFFFFF);
    read_back();
    screen_quad(0, 0, 10, 10, 0.5f, 0xFFFFFFFF);
    IDirect3DQuery9_Issue(occlusion, D3DISSUE_END);
    while ((hr = IDirect3DQuery9_GetData(occlusion, &samples, sizeof samples, D3DGETDATA_FLUSH)) == S_FALSE) {}
    check(hr == S_OK && samples == 200, "... across lists that were sent in between");
    IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZFUNC, D3DCMP_NEVER);
    IDirect3DQuery9_Issue(occlusion, D3DISSUE_BEGIN);
    screen_quad(0, 0, 10, 10, 0.5f, 0xFFFFFFFF);
    IDirect3DQuery9_Issue(occlusion, D3DISSUE_END);
    while ((hr = IDirect3DQuery9_GetData(occlusion, &samples, sizeof samples, D3DGETDATA_FLUSH)) == S_FALSE) {}
    IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    check(hr == S_OK && samples == 0, "... and none that the depth test drops");
  } else check(0, "an occlusion query");
  IDirect3DDevice9_CreateQuery(g_device, D3DQUERYTYPE_TIMESTAMP, &stamp);
  IDirect3DDevice9_CreateQuery(g_device, D3DQUERYTYPE_TIMESTAMPFREQ, &freq);
  if (stamp && freq) {
    UINT64 t0 = 0, t1 = 0, f = 0;
    IDirect3DQuery9_Issue(stamp, D3DISSUE_END);
    while (IDirect3DQuery9_GetData(stamp, &t0, sizeof t0, D3DGETDATA_FLUSH) == S_FALSE) {}
    screen_quad(0, 0, 64, 64, 0.5f, 0xFFFFFFFF);
    IDirect3DQuery9_Issue(stamp, D3DISSUE_END);
    while (IDirect3DQuery9_GetData(stamp, &t1, sizeof t1, D3DGETDATA_FLUSH) == S_FALSE) {}
    IDirect3DQuery9_Issue(freq, D3DISSUE_END);
    check(IDirect3DQuery9_GetData(freq, &f, sizeof f, D3DGETDATA_FLUSH) == S_OK && f != 0 && t1 > t0 && IDirect3DQuery9_Issue(stamp, D3DISSUE_BEGIN) == D3DERR_INVALIDCALL,
          "timestamps that go up, their frequency; a timestamp has no begin");
  }
  if (event) IDirect3DQuery9_Release(event);
  if (occlusion) IDirect3DQuery9_Release(occlusion);
  if (stamp) IDirect3DQuery9_Release(stamp);
  if (freq) IDirect3DQuery9_Release(freq);
}

static void test_device_misc(void) {
  check(IDirect3DDevice9_TestCooperativeLevel(g_device) == D3D_OK, "TestCooperativeLevel of a windowed device");
  const UINT before = IDirect3DDevice9_GetAvailableTextureMem(g_device);
  IDirect3DTexture9 *big = NULL;
  IDirect3DDevice9_CreateTexture(g_device, 1024, 1024, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &big, NULL);
  const UINT after = IDirect3DDevice9_GetAvailableTextureMem(g_device);
  /* (less by what the texture takes - unless there is more than a UINT holds: then both are the 0xFFF00000 that Direct3D 9 caps the figure at, as on a machine
   * whose adapter shares half of a large memory) */
  const BOOL capped = before == 0xFFF00000u && after == 0xFFF00000u;
  if (!(before > (64u << 20) && (before & 0xFFFFF) == 0 && (capped || after <= before - (4u << 20)))) printf("d3d9test: GetAvailableTextureMem %u then %u\n", before, after);
  check(before > (64u << 20) && (before & 0xFFFFF) == 0 && (capped || after <= before - (4u << 20)), "GetAvailableTextureMem: in megabytes, less by what a texture takes (or at the cap)");
  if (big) IDirect3DTexture9_Release(big);
  check(IDirect3DDevice9_EvictManagedResources(g_device) == D3D_OK, "EvictManagedResources");
  DEVMODEW dm;
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  D3DDISPLAYMODE mode;
  EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm);
  check(IDirect3D9_GetAdapterDisplayMode(g_d3d, 0, &mode) == D3D_OK && mode.Width == dm.dmPelsWidth && mode.Height == dm.dmPelsHeight, "the display mode is the display's");
  const UINT count = IDirect3D9_GetAdapterModeCount(g_d3d, 0, D3DFMT_X8R8G8B8);
  int found = 0;
  for (UINT i = 0; i < count; ++i)
    if (SUCCEEDED(IDirect3D9_EnumAdapterModes(g_d3d, 0, D3DFMT_X8R8G8B8, i, &mode)) && mode.Width == dm.dmPelsWidth && mode.Height == dm.dmPelsHeight) found = 1;
  check(count >= 1 && found && IDirect3D9_EnumAdapterModes(g_d3d, 0, D3DFMT_X8R8G8B8, count, &mode) == D3DERR_INVALIDCALL, "the adapter's modes are the display's, the current one among them");
  IDirect3DVertexDeclaration9 *a = NULL, *b = NULL;
  DWORD fvf = 0;
  IDirect3DDevice9_SetFVF(g_device, D3DFVF_XYZ | D3DFVF_NORMAL);
  IDirect3DDevice9_GetVertexDeclaration(g_device, &a);
  IDirect3DDevice9_SetFVF(g_device, FVF_SCREEN);
  IDirect3DDevice9_SetFVF(g_device, D3DFVF_XYZ | D3DFVF_NORMAL);
  IDirect3DDevice9_GetVertexDeclaration(g_device, &b);
  check(a && a == b, "SetFVF has one declaration for each code");
  if (a) IDirect3DVertexDeclaration9_Release(a);
  if (b) IDirect3DVertexDeclaration9_Release(b);
  static const D3DVERTEXELEMENT9 position[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITIONT, 0}, {0, 16, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  IDirect3DDevice9_CreateVertexDeclaration(g_device, position, &decl);
  IDirect3DDevice9_SetVertexDeclaration(g_device, decl);
  IDirect3DDevice9_GetFVF(g_device, &fvf);
  check(decl && fvf == D3DFVF_XYZRHW, "GetFVF of a declaration of a transformed position: D3DFVF_XYZRHW");
  IDirect3DDevice9_SetVertexDeclaration(g_device, NULL);
  if (decl) IDirect3DVertexDeclaration9_Release(decl);
}

/* ---- Direct3D 9Ex ---------------------------------------------------------------------------------------------------------------------------------------------------- */

static void test_ex(void) {
  IDirect3D9Ex *d3d = NULL;
  IDirect3DDevice9Ex *dev = NULL, *dev2 = NULL;
  HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d);
  check(hr == D3D_OK && d3d, "Direct3DCreate9Ex");
  if (!d3d) return;
  IDirect3D9 *plain = NULL;
  IDirect3D9Ex_QueryInterface(d3d, &IID_IDirect3D9, (void **)&plain);
  check(plain == (IDirect3D9 *)d3d, "... it is an IDirect3D9 too");
  if (plain) IDirect3D9_Release(plain);
  LUID luid = {0, 0};
  D3DDISPLAYMODEFILTER filter = {sizeof filter, D3DFMT_X8R8G8B8, D3DSCANLINEORDERING_PROGRESSIVE};
  D3DDISPLAYMODEEX mode;
  memset(&mode, 0, sizeof mode);
  mode.Size = sizeof mode;
  check(IDirect3D9Ex_GetAdapterLUID(d3d, 0, &luid) == D3D_OK && (luid.LowPart || luid.HighPart) && IDirect3D9Ex_GetAdapterModeCountEx(d3d, 0, &filter) >= 1 &&
        IDirect3D9Ex_EnumAdapterModesEx(d3d, 0, &filter, 0, &mode) == D3D_OK && mode.Width > 0 && mode.ScanLineOrdering == D3DSCANLINEORDERING_PROGRESSIVE,
        "GetAdapterLUID, GetAdapterModeCountEx, EnumAdapterModesEx");
  D3DPRESENT_PARAMETERS pp;
  memset(&pp, 0, sizeof pp);
  pp.BackBufferWidth = pp.BackBufferHeight = 32;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_window;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  D3DPRESENT_PARAMETERS pp2 = pp;
  hr = IDirect3D9Ex_CreateDeviceEx(d3d, 0, D3DDEVTYPE_HAL, g_window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, NULL, &dev);
  check(hr == D3D_OK && dev, "CreateDeviceEx of a windowed device");
  if (!dev) goto done;
  IDirect3DDevice9 *base = NULL;
  IDirect3DDevice9Ex_QueryInterface(dev, &IID_IDirect3DDevice9, (void **)&base);
  check(base == (IDirect3DDevice9 *)dev, "... it is an IDirect3DDevice9 too");
  if (base) IDirect3DDevice9_Release(base);
  UINT latency = 0;
  INT priority = 9;
  check(IDirect3DDevice9Ex_GetMaximumFrameLatency(dev, &latency) == D3D_OK && latency == 3 && IDirect3DDevice9Ex_SetMaximumFrameLatency(dev, 31) == D3DERR_INVALIDCALL &&
        IDirect3DDevice9Ex_SetGPUThreadPriority(dev, 3) == D3D_OK && IDirect3DDevice9Ex_GetGPUThreadPriority(dev, &priority) == D3D_OK && priority == 3 &&
        IDirect3DDevice9Ex_SetGPUThreadPriority(dev, 8) == D3DERR_INVALIDCALL, "the frame latency and the GPU priority");
  check(IDirect3DDevice9Ex_CheckDeviceState(dev, g_window) == D3D_OK && IDirect3DDevice9Ex_WaitForVBlank(dev, 0) == D3D_OK && IDirect3DDevice9Ex_TestCooperativeLevel(dev) == D3D_OK,
        "CheckDeviceState, WaitForVBlank, TestCooperativeLevel");
  IDirect3DTexture9 *managed = (IDirect3DTexture9 *)1;
  check(IDirect3DDevice9Ex_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managed, NULL) == D3DERR_INVALIDCALL, "a 9Ex device has no managed pool");
  /* a shared render target texture: made on one device, opened by its handle on another, read there */
  HANDLE handle = NULL;
  IDirect3DTexture9 *shared = NULL, *opened = NULL;
  hr = IDirect3DDevice9Ex_CreateTexture(dev, 16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &shared, &handle);
  check(hr == D3D_OK && shared && handle, "a texture shared: its handle");
  hr = IDirect3D9Ex_CreateDeviceEx(d3d, 0, D3DDEVTYPE_HAL, g_window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp2, NULL, &dev2);
  if (shared && handle && dev2) {
    IDirect3DSurface9 *level = NULL, *level2 = NULL, *memory = NULL;
    IDirect3DTexture9_GetSurfaceLevel(shared, 0, &level);
    IDirect3DDevice9Ex_ColorFill(dev, level, NULL, 0xFF8040C0);
    /* (what the first device did is done before the other reads it: an event query) */
    IDirect3DQuery9 *event = NULL;
    BOOL done_ = FALSE;
    IDirect3DDevice9Ex_CreateQuery(dev, D3DQUERYTYPE_EVENT, &event);
    if (event) {
      IDirect3DQuery9_Issue(event, D3DISSUE_END);
      while (IDirect3DQuery9_GetData(event, &done_, sizeof done_, D3DGETDATA_FLUSH) == S_FALSE) {}
      IDirect3DQuery9_Release(event);
    }
    HANDLE same = handle;
    hr = IDirect3DDevice9Ex_CreateTexture(dev2, 16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &opened, &same);
    check(hr == D3D_OK && opened, "... opened on another device");
    HANDLE wrong = handle;
    IDirect3DTexture9 *nothing = NULL;
    check(IDirect3DDevice9Ex_CreateTexture(dev2, 8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &nothing, &wrong) == D3DERR_INVALIDCALL && !nothing,
          "... not with another size");
    if (opened) {
      IDirect3DTexture9_GetSurfaceLevel(opened, 0, &level2);
      IDirect3DDevice9Ex_CreateOffscreenPlainSurface(dev2, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &memory, NULL);
      DWORD p = 0;
      if (level2 && memory && SUCCEEDED(IDirect3DDevice9Ex_GetRenderTargetData(dev2, level2, memory))) p = surface_pixel(memory, 3, 3);
      check((p & 0xFFFFFF) == 0x8040C0, "... the other device reads what the first one put into it");
    }
    if (memory) IDirect3DSurface9_Release(memory);
    if (level2) IDirect3DSurface9_Release(level2);
    if (level) IDirect3DSurface9_Release(level);
  }
  /* a texture in system memory on the program's memory */
  DWORD *pixels = calloc(16 * 16, 4);
  IDirect3DTexture9 *user = NULL;
  D3DLOCKED_RECT locked;
  HANDLE memory_handle = pixels;
  check(pixels && IDirect3DDevice9Ex_CreateTexture(dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &user, &memory_handle) == D3D_OK && user &&
        SUCCEEDED(IDirect3DTexture9_LockRect(user, 0, &locked, NULL, 0)) && locked.pBits == pixels && locked.Pitch == 64, "9Ex: a texture in memory on the program's memory");
  if (user) { IDirect3DTexture9_UnlockRect(user, 0); IDirect3DTexture9_Release(user); }
  free(pixels);
  /* PresentEx, and the Ex swap chain */
  IDirect3DSwapChain9 *chain = NULL;
  IDirect3DSwapChain9Ex *chain_ex = NULL;
  UINT presents = 7;
  IDirect3DDevice9Ex_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
  check(IDirect3DDevice9Ex_PresentEx(dev, NULL, NULL, NULL, NULL, 0) == D3D_OK && IDirect3DDevice9Ex_GetSwapChain(dev, 0, &chain) == D3D_OK && chain &&
        SUCCEEDED(IDirect3DSwapChain9_QueryInterface(chain, &IID_IDirect3DSwapChain9Ex, (void **)&chain_ex)) && chain_ex &&
        IDirect3DSwapChain9Ex_GetLastPresentCount(chain_ex, &presents) == D3D_OK && presents == 1, "PresentEx, and the swap chain's count of Presents (IDirect3DSwapChain9Ex)");
  if (chain_ex) IDirect3DSwapChain9Ex_Release(chain_ex);
  if (chain) IDirect3DSwapChain9_Release(chain);
  /* ResetEx keeps the resources of the default pool (a device that is not Ex would fail) */
  memset(&pp, 0, sizeof pp);
  pp.BackBufferWidth = pp.BackBufferHeight = 48;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_window;
  pp.Windowed = TRUE;
  check(IDirect3DDevice9Ex_ResetEx(dev, &pp, NULL) == D3D_OK, "ResetEx with a texture of the default pool there");
done:
  if (opened) IDirect3DTexture9_Release(opened);
  if (shared) IDirect3DTexture9_Release(shared);
  if (dev2) check(IDirect3DDevice9Ex_Release(dev2) == 0, "the second 9Ex device is gone with its last reference");
  if (dev) check(IDirect3DDevice9Ex_Release(dev) == 0, "the 9Ex device is gone with its last reference");
  IDirect3D9Ex_Release(d3d);
}

/* Vertex blending, tweening, more than eight lights, and a program's vertex shader: half a pixel, and the white of an oD0 it does not write. */
static D3DMATRIX translation(float x, float y) {
  D3DMATRIX m;
  memset(&m, 0, sizeof m);
  m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
  m.m[3][0] = x;
  m.m[3][1] = y;
  return m;
}
static void test_vertex_processing(void) {
  const D3DMATRIX identity = translation(0, 0), right = translation(1.0f, 0), up = translation(0, 0.5f);
  /* two matrices by one weight: half of the second's move (a rectangle of -0.25 .. 0.25 at x + 0.5) */
  typedef struct { float x, y, z, weight; DWORD colour; } BlendVertex;
  const BlendVertex b[4] = {{-0.25f, 0.25f, 0.5f, 0.5f, 0xFFFFFFFF}, {0.25f, 0.25f, 0.5f, 0.5f, 0xFFFFFFFF}, {-0.25f, -0.25f, 0.5f, 0.5f, 0xFFFFFFFF}, {0.25f, -0.25f, 0.5f, 0.5f, 0xFFFFFFFF}};
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetTransform(g_device, D3DTS_WORLDMATRIX(0), &identity);
  IDirect3DDevice9_SetTransform(g_device, D3DTS_WORLDMATRIX(1), &right);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
  clear(0xFF000000);
  IDirect3DDevice9_SetFVF(g_device, D3DFVF_XYZB1 | D3DFVF_DIFFUSE);
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, b, sizeof b[0]);
  read_back();
  check(pixel(48, 32) == 0xFFFFFF && pixel(30, 32) == 0, "vertex blending of two world matrices by a weight");
  /* indexed: one matrix of a vertex's own (index 5: up by a quarter of the target) */
  typedef struct { float x, y, z; BYTE index[4]; DWORD colour; } IndexedVertex;
  const IndexedVertex iv[4] = {{-0.25f, 0.25f, 0.5f, {5, 0, 0, 0}, 0xFFFFFFFF}, {0.25f, 0.25f, 0.5f, {5, 0, 0, 0}, 0xFFFFFFFF}, {-0.25f, -0.25f, 0.5f, {5, 0, 0, 0}, 0xFFFFFFFF},
                                {0.25f, -0.25f, 0.5f, {5, 0, 0, 0}, 0xFFFFFFFF}};
  IDirect3DDevice9_SetTransform(g_device, D3DTS_WORLDMATRIX(5), &up);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_VERTEXBLEND, D3DVBF_0WEIGHTS);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
  clear(0xFF000000);
  IDirect3DDevice9_SetFVF(g_device, D3DFVF_XYZB1 | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_DIFFUSE);
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, iv, sizeof iv[0]);
  read_back();
  check(pixel(32, 20) == 0xFFFFFF && pixel(32, 34) == 0, "indexed vertex blending: the matrix of the vertex's index");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
  /* tweening: two positions, half way */
  static const D3DVERTEXELEMENT9 tween_elements[] = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0}, {0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 1},
                                                     {0, 24, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0}, D3DDECL_END()};
  typedef struct { float x, y, z, x1, y1, z1; DWORD colour; } TweenVertex;
  const TweenVertex tv[4] = {{-0.75f, 0.25f, 0.5f, 0.25f, 0.25f, 0.5f, 0xFFFFFFFF}, {-0.25f, 0.25f, 0.5f, 0.75f, 0.25f, 0.5f, 0xFFFFFFFF},
                             {-0.75f, -0.25f, 0.5f, 0.25f, -0.25f, 0.5f, 0xFFFFFFFF}, {-0.25f, -0.25f, 0.5f, 0.75f, -0.25f, 0.5f, 0xFFFFFFFF}};
  IDirect3DVertexDeclaration9 *decl = NULL;
  IDirect3DDevice9_CreateVertexDeclaration(g_device, tween_elements, &decl);
  const float half = 0.5f;
  DWORD half_bits;
  memcpy(&half_bits, &half, 4);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_VERTEXBLEND, D3DVBF_TWEENING);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_TWEENFACTOR, half_bits);
  IDirect3DDevice9_SetVertexDeclaration(g_device, decl);
  clear(0xFF000000);
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, tv, sizeof tv[0]);
  read_back();
  check(decl && pixel(32, 32) == 0xFFFFFF && pixel(14, 32) == 0 && pixel(50, 32) == 0, "tweening: the two positions half way");
  IDirect3DDevice9_SetVertexDeclaration(g_device, NULL);
  if (decl) IDirect3DVertexDeclaration9_Release(decl);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
  /* sixteen lights that shine at once, each a sixteenth of the light */
  D3DCAPS9 caps;
  IDirect3DDevice9_GetDeviceCaps(g_device, &caps);
  D3DMATERIAL9 material;
  memset(&material, 0, sizeof material);
  material.Diffuse.r = material.Diffuse.g = material.Diffuse.b = material.Diffuse.a = 1.0f;
  IDirect3DDevice9_SetMaterial(g_device, &material);
  D3DLIGHT9 light;
  memset(&light, 0, sizeof light);
  light.Type = D3DLIGHT_DIRECTIONAL;
  light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f / 16.0f;
  light.Direction.z = 1.0f;
  for (DWORD i = 0; i < 16; ++i) { IDirect3DDevice9_SetLight(g_device, 100 + i, &light); IDirect3DDevice9_LightEnable(g_device, 100 + i, TRUE); }
  typedef struct { float x, y, z, nx, ny, nz; } NormalVertex;
  const NormalVertex nv[4] = {{-0.5f, 0.5f, 0.5f, 0, 0, -1}, {0.5f, 0.5f, 0.5f, 0, 0, -1}, {-0.5f, -0.5f, 0.5f, 0, 0, -1}, {0.5f, -0.5f, 0.5f, 0, 0, -1}};
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_LIGHTING, TRUE);
  clear(0xFF000000);
  IDirect3DDevice9_SetFVF(g_device, D3DFVF_XYZ | D3DFVF_NORMAL);
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, nv, sizeof nv[0]);
  read_back();
  check(caps.MaxActiveLights >= 16 && near_colour(pixel(32, 32), 255, 255, 255), "sixteen lights that shine at once all light the vertices");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_LIGHTING, FALSE);
  for (DWORD i = 0; i < 16; ++i) IDirect3DDevice9_LightEnable(g_device, 100 + i, FALSE);
  /* a program's vertex shader (vs_2_0: mov oPos, v0): its oD0 is white, and its rectangle's edge at 31.25 pixels covers pixel 31 as Direct3D 9's centres have it */
  static const DWORD vs_position[] = {0xFFFE0200, 0x0200001F, 0x80000000, 0x900F0000, 0x02000001, 0xC00F0000, 0x90E40000, 0x0000FFFF};
  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DDevice9_CreateVertexShader(g_device, vs_position, &vs);
  check(vs != NULL, "a vertex shader that writes only its position");
  if (vs) {
    const float edge = 31.25f / 32.0f - 1.0f;
    const WorldVertex q[4] = {{-1.0f, 1.0f, 0.5f, 0xFF000000}, {edge, 1.0f, 0.5f, 0xFF000000}, {-1.0f, -1.0f, 0.5f, 0xFF000000}, {edge, -1.0f, 0.5f, 0xFF000000}};
    IDirect3DDevice9_SetVertexShader(g_device, vs);
    IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    clear(0xFF000000);
    IDirect3DDevice9_SetFVF(g_device, FVF_WORLD);
    IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    IDirect3DDevice9_SetVertexShader(g_device, NULL);
    IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    read_back();
    check(pixel(10, 32) == 0xFFFFFF, "... the colour it does not write is white");
    check(pixel(31, 32) == 0xFFFFFF && pixel(32, 32) == 0, "... its pixels' centres are Direct3D 9's (half a pixel)");
    IDirect3DVertexShader9_Release(vs);
  }
}

/* The wide formats: a float target that keeps what is above 1, read back by GetRenderTargetData and sampled; textures of R32F, G16R16 (the channels they have
 * not read 1, as Direct3D 9 has them) and A2R10G10B10. */
static void test_wide_formats(void) {
  static const DWORD ps_two[] = {0xFFFF0200, 0x05000051, 0xA00F0000, 0x40000000, 0x40000000, 0x40000000, 0x40000000, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF};
  static const DWORD ps_quarter[] = {0xFFFF0200, 0x05000051, 0xA00F0001, 0x3E800000, 0x3E800000, 0x3E800000, 0x3E800000, 0x0200001F, 0x80000000, 0xB00F0000,
                                     0x0200001F, 0x90000000, 0xA00F0800, 0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800, 0x03000005, 0x800F0000, 0x80E40000,
                                     0xA0E40001, 0x02000001, 0x800F0800, 0x80E40000, 0x0000FFFF};
  IDirect3DTexture9 *target = NULL, *r32 = NULL, *g16 = NULL, *a2 = NULL;
  IDirect3DSurface9 *level = NULL, *memory = NULL, *back = NULL;
  IDirect3DPixelShader9 *two = NULL, *quarter = NULL;
  HRESULT hr = IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F);
  check(hr == D3D_OK && IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_R32F) == D3D_OK, "float formats: textures and targets");
  hr = IDirect3DDevice9_CreateTexture(g_device, 16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &target, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DTexture9_GetSurfaceLevel(target, 0, &level);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 16, 16, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &memory, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreatePixelShader(g_device, ps_two, &two);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreatePixelShader(g_device, ps_quarter, &quarter);
  IDirect3DDevice9_GetRenderTarget(g_device, 0, &back);
  check(SUCCEEDED(hr) && target && level && memory && two && quarter && back, "an A16B16G16R16F target, a surface of it in memory, and two pixel shaders");
  if (FAILED(hr) || !back) goto done;
  /* 2.0 into the float target, kept as it is */
  IDirect3DDevice9_SetRenderTarget(g_device, 0, level);
  IDirect3DDevice9_SetPixelShader(g_device, two);
  screen_quad(0, 0, 16, 16, 0.5f, 0xFFFFFFFF);
  IDirect3DDevice9_SetPixelShader(g_device, NULL);
  IDirect3DDevice9_SetRenderTarget(g_device, 0, back);
  D3DLOCKED_RECT locked;
  WORD half[4] = {0, 0, 0, 0};
  if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_device, level, memory)) && SUCCEEDED(IDirect3DSurface9_LockRect(memory, &locked, NULL, D3DLOCK_READONLY))) {
    memcpy(half, (const BYTE *)locked.pBits + 5 * locked.Pitch + 5 * 8, 8);
    IDirect3DSurface9_UnlockRect(memory);
  }
  check(half[0] == 0x4000 && half[3] == 0x4000, "a pixel shader's 2.0 is kept in a float target (GetRenderTargetData)");
  /* ... and sampled as 2.0: a quarter of it is half the light */
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)target);
  IDirect3DDevice9_SetPixelShader(g_device, quarter);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  IDirect3DDevice9_SetPixelShader(g_device, NULL);
  read_back();
  check(near_colour(pixel(32, 32), 128, 128, 128), "... and sampled as 2.0");
  /* an R32F texture through the stages: (r, 1, 1, 1) */
  hr = IDirect3DDevice9_CreateTexture(g_device, 2, 2, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &r32, NULL);
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DTexture9_LockRect(r32, 0, &locked, NULL, 0))) {
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) ((float *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0.25f;
    IDirect3DTexture9_UnlockRect(r32, 0);
  }
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)r32);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(r32 && near_colour(pixel(32, 32), 64, 255, 255), "an R32F texture samples as (r, 1, 1, 1), as Direct3D 9 has it");
  /* a G16R16 texture: (r, g, 1, 1) */
  hr = IDirect3DDevice9_CreateTexture(g_device, 2, 2, 1, 0, D3DFMT_G16R16, D3DPOOL_MANAGED, &g16, NULL);
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DTexture9_LockRect(g16, 0, &locked, NULL, 0))) {
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) ((DWORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0x0000FFFFu;      /* (red 1, green 0) */
    IDirect3DTexture9_UnlockRect(g16, 0);
  }
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)g16);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(g16 && pixel(32, 32) == 0xFF00FF, "a G16R16 texture samples as (r, g, 1, 1)");
  /* A2R10G10B10: red in the top bits */
  hr = IDirect3DDevice9_CreateTexture(g_device, 2, 2, 1, 0, D3DFMT_A2R10G10B10, D3DPOOL_MANAGED, &a2, NULL);
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DTexture9_LockRect(a2, 0, &locked, NULL, 0))) {
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) ((DWORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0xC0000000u | (1023u << 20);
    IDirect3DTexture9_UnlockRect(a2, 0);
  }
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)a2);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(a2 && pixel(32, 32) == 0xFF0000, "an A2R10G10B10 texture: red in its top bits");
  IDirect3DDevice9_SetTexture(g_device, 0, NULL);
done:
  if (back) IDirect3DSurface9_Release(back);
  if (level) IDirect3DSurface9_Release(level);
  if (memory) IDirect3DSurface9_Release(memory);
  if (target) IDirect3DTexture9_Release(target);
  if (r32) IDirect3DTexture9_Release(r32);
  if (g16) IDirect3DTexture9_Release(g16);
  if (a2) IDirect3DTexture9_Release(a2);
  if (two) IDirect3DPixelShader9_Release(two);
  if (quarter) IDirect3DPixelShader9_Release(quarter);
}

/* ProcessVertices: the vertices made pixels, lit, and clipped as Direct3D 9 does it; what the draws leave of stream 0 and an indexed draw with no indices; a small
 * texture target drawn into with the device's larger depth buffer. */
static void test_process_vertices(void) {
  typedef struct { float x, y, z; DWORD colour; } In;
  typedef struct { float x, y, z, rhw; DWORD colour; } Out;
  static const In in[4] = {{-0.5f, -0.5f, 0.0f, 0xFFFF0000}, {-0.5f, 0.5f, 0.5f, 0xFF00FF00}, {0.5f, -0.5f, 0.75f, 0xFF0000FF}, {2.0f, 0.0f, 0.0f, 0xFFFFFFFF}};
  IDirect3DVertexBuffer9 *source = NULL, *dest = NULL, *got = NULL;
  void *p = NULL;
  HRESULT hr = IDirect3DDevice9_CreateVertexBuffer(g_device, sizeof in, 0, D3DFVF_XYZ | D3DFVF_DIFFUSE, D3DPOOL_SYSTEMMEM, &source, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateVertexBuffer(g_device, 4 * sizeof(Out), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_SYSTEMMEM, &dest, NULL);
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DVertexBuffer9_Lock(source, 0, 0, &p, 0))) {
    memcpy(p, in, sizeof in);
    IDirect3DVertexBuffer9_Unlock(source);
  }
  check(SUCCEEDED(hr), "a source and a destination vertex buffer");
  if (FAILED(hr)) goto done;
  D3DMATRIX identity;
  memset(&identity, 0, sizeof identity);
  identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
  IDirect3DDevice9_SetTransform(g_device, D3DTS_WORLD, &identity);
  IDirect3DDevice9_SetTransform(g_device, D3DTS_VIEW, &identity);
  IDirect3DDevice9_SetTransform(g_device, D3DTS_PROJECTION, &identity);
  IDirect3DDevice9_SetFVF(g_device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
  IDirect3DDevice9_SetStreamSource(g_device, 0, source, 0, sizeof(In));
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_LIGHTING, FALSE);
  D3DCLIPSTATUS9 status = {0, 0};
  IDirect3DDevice9_SetClipStatus(g_device, &status);
  hr = IDirect3DDevice9_ProcessVertices(g_device, 0, 0, 4, dest, NULL, 0);
  Out out[4];
  memset(out, 0, sizeof out);
  if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(dest, 0, 0, &p, D3DLOCK_READONLY))) {
    memcpy(out, p, sizeof out);
    IDirect3DVertexBuffer9_Unlock(dest);
  }
  check(hr == D3D_OK && out[0].x == 16.0f && out[0].y == 48.0f && out[0].z == 0.0f && out[0].rhw == 1.0f && out[0].colour == 0xFFFF0000 &&
        out[1].x == 16.0f && out[1].y == 16.0f && out[1].z == 0.5f && out[2].x == 48.0f && out[2].z == 0.75f && out[2].colour == 0xFF0000FF,
        "ProcessVertices: positions in pixels by the matrices and the viewport, the colours copied");
  check(out[3].x == 1.5f && out[3].y == 0.5f && out[3].z == 0.0f && out[3].rhw == 1.0f, "... a vertex outside the clip volume is (x + w) / 2, (y + w) / 2, z, w");
  IDirect3DDevice9_GetClipStatus(g_device, &status);
  check(status.ClipUnion == D3DCS_RIGHT && status.ClipIntersection == 0, "... and the clip status has its clip codes");
  /* lit: the global ambient light on the material's ambient colour, the vertex's alpha */
  D3DMATERIAL9 material;
  memset(&material, 0, sizeof material);
  material.Ambient.r = material.Ambient.g = material.Ambient.b = material.Ambient.a = 1.0f;
  IDirect3DDevice9_SetMaterial(g_device, &material);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_LIGHTING, TRUE);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_AMBIENT, 0xFF404040);
  hr = IDirect3DDevice9_ProcessVertices(g_device, 1, 2, 1, dest, NULL, D3DPV_DONOTCOPYDATA);
  DWORD kept = 0;
  hr = SUCCEEDED(hr) ? IDirect3DDevice9_ProcessVertices(g_device, 1, 1, 1, dest, NULL, 0) : hr;
  if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(dest, 0, 0, &p, D3DLOCK_READONLY))) {
    memcpy(out, p, sizeof out);
    IDirect3DVertexBuffer9_Unlock(dest);
    kept = out[2].colour;
  }
  check(hr == D3D_OK && out[1].colour == 0xFF404040 && out[2].x == 16.0f && out[2].y == 16.0f && kept == 0xFF0000FF,
        "... lit by the ambient light; D3DPV_DONOTCOPYDATA writes the position alone");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_AMBIENT, 0);
  check(IDirect3DDevice9_ProcessVertices(g_device, 0, 3, 2, dest, NULL, 0) == D3DERR_INVALIDCALL && IDirect3DDevice9_ProcessVertices(g_device, 3, 0, 2, dest, NULL, 0) == D3DERR_INVALIDCALL &&
        IDirect3DDevice9_ProcessVertices(g_device, 0, 0, 1, NULL, NULL, 0) == D3DERR_INVALIDCALL,
        "... not past the end of the destination nor of the source, not into no buffer");
  /* an indexed draw with no indices draws nothing and succeeds; the UP draws leave stream 0 unset with no offset and no stride */
  IDirect3DDevice9_SetIndices(g_device, NULL);
  check(IDirect3DDevice9_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1) == D3D_OK, "DrawIndexedPrimitive with no index buffer: D3D_OK");
  IDirect3DDevice9_SetStreamSource(g_device, 0, NULL, 0, 0);
  UINT offset = 7, stride = 7;
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLELIST, 1, in, sizeof(In));
  IDirect3DDevice9_GetStreamSource(g_device, 0, &got, &offset, &stride);
  check(got == NULL && offset == 0 && stride == 0, "after DrawPrimitiveUP stream 0 is NULL, 0, 0");
  if (got) IDirect3DVertexBuffer9_Release(got);
  /* a 32 x 32 texture target with the 64 x 64 depth buffer: its top left part */
  IDirect3DTexture9 *small = NULL;
  IDirect3DSurface9 *level = NULL, *back = NULL, *memory = NULL;
  hr = IDirect3DDevice9_CreateTexture(g_device, 32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &small, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DTexture9_GetSurfaceLevel(small, 0, &level);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &memory, NULL);
  IDirect3DDevice9_GetRenderTarget(g_device, 0, &back);
  DWORD front = 0, behind = 0;
  if (SUCCEEDED(hr) && back) {
    IDirect3DDevice9_SetRenderTarget(g_device, 0, level);
    IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
    screen_quad(0, 0, 32, 32, 0.25f, 0xFFFFFF00);
    screen_quad(0, 0, 16, 32, 0.75f, 0xFF0000FF);      /* (behind: not drawn) */
    IDirect3DDevice9_SetRenderTarget(g_device, 0, back);
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_device, level, memory)) && SUCCEEDED(IDirect3DSurface9_LockRect(memory, &locked, NULL, D3DLOCK_READONLY))) {
      behind = ((const DWORD *)((const BYTE *)locked.pBits + 8 * locked.Pitch))[8] & 0xFFFFFF;
      front = ((const DWORD *)((const BYTE *)locked.pBits + 8 * locked.Pitch))[24] & 0xFFFFFF;
      IDirect3DSurface9_UnlockRect(memory);
    }
  }
  check(front == 0xFFFF00 && behind == 0xFFFF00, "a texture target smaller than the depth buffer is drawn into, with the depth test");
  if (back) IDirect3DSurface9_Release(back);
  if (memory) IDirect3DSurface9_Release(memory);
  if (level) IDirect3DSurface9_Release(level);
  if (small) IDirect3DTexture9_Release(small);
done:
  IDirect3DDevice9_SetStreamSource(g_device, 0, NULL, 0, 0);
  if (source) IDirect3DVertexBuffer9_Release(source);
  if (dest) IDirect3DVertexBuffer9_Release(dest);
}

/* The levels of a target texture and the faces of a cube target are targets of their own: drawn into, cleared and read back, the others untouched. */
static void test_level_targets(void) {
  IDirect3DTexture9 *texture = NULL;
  IDirect3DCubeTexture9 *cube = NULL;
  IDirect3DSurface9 *level0 = NULL, *level1 = NULL, *face2 = NULL, *face3 = NULL, *back = NULL;
  HRESULT hr = IDirect3DDevice9_CreateTexture(g_device, 32, 32, 2, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &level0);
  if (SUCCEEDED(hr)) hr = IDirect3DTexture9_GetSurfaceLevel(texture, 1, &level1);
  check(SUCCEEDED(hr) && IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_CUBETEXTURE, D3DFMT_A8R8G8B8) == D3D_OK,
        "a target texture of two levels; cube targets are there");
  IDirect3DDevice9_GetRenderTarget(g_device, 0, &back);
  if (FAILED(hr) || !back) goto done;
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetRenderTarget(g_device, 0, level0);
  IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0);
  IDirect3DDevice9_SetRenderTarget(g_device, 0, level1);
  D3DVIEWPORT9 viewport;
  IDirect3DDevice9_GetViewport(g_device, &viewport);
  IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
  screen_quad(0, 0, 8, 16, 0.5f, 0xFF00FF00);
  IDirect3DDevice9_SetRenderTarget(g_device, 0, back);
  check(viewport.Width == 16 && viewport.Height == 16 && target_pixel(level1, 4, 4) == 0xFF00FF00 && target_pixel(level1, 12, 4) == 0xFF000000 && target_pixel(level0, 20, 20) == 0xFF0000FF,
        "level 1 of a target texture is drawn into and cleared as a target of its own (its size; level 0 keeps its blue)");
  /* a cube's faces */
  hr = IDirect3DDevice9_CreateCubeTexture(g_device, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DCubeTexture9_GetCubeMapSurface(cube, D3DCUBEMAP_FACE_POSITIVE_Y, 0, &face2);
  if (SUCCEEDED(hr)) hr = IDirect3DCubeTexture9_GetCubeMapSurface(cube, D3DCUBEMAP_FACE_NEGATIVE_Y, 0, &face3);
  if (SUCCEEDED(hr)) {
    IDirect3DDevice9_SetRenderTarget(g_device, 0, face3);
    IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFFFFFF00, 1.0f, 0);
    hr = IDirect3DDevice9_SetRenderTarget(g_device, 0, face2);
    IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    screen_quad(0, 0, 16, 8, 0.5f, 0xFFFF0000);
    IDirect3DDevice9_SetRenderTarget(g_device, 0, back);
  }
  check(SUCCEEDED(hr) && target_pixel(face2, 4, 4) == 0xFFFF0000 && target_pixel(face2, 4, 12) == 0xFF000000 && target_pixel(face3, 8, 8) == 0xFFFFFF00,
        "the faces of a cube target are targets of their own");
  /* the write mask with no blending: white over the blue of level 0 through D3DRS_COLORWRITEENABLE of red alone */
  IDirect3DDevice9_SetRenderTarget(g_device, 0, level0);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
  screen_quad(0, 0, 4, 4, 0.5f, 0xFFFFFFFF);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_COLORWRITEENABLE, 0xF);
  IDirect3DDevice9_SetRenderTarget(g_device, 0, back);
  check(target_pixel(level0, 1, 1) == 0xFFFF00FF && target_pixel(level0, 6, 6) == 0xFF0000FF, "D3DRS_COLORWRITEENABLE with no blending writes its channels alone");
  /* two targets at once: a pixel shader's oC0 and oC1, both cleared by Clear; COLORWRITEENABLE1 masks the second */
  D3DCAPS9 caps;
  IDirect3DDevice9_GetDeviceCaps(g_device, &caps);
  if (caps.NumSimultaneousRTs >= 2) {
    static const DWORD ps_two_targets[] = {0xFFFF0200, 0x05000051, 0xA00F0000, 0x3F800000, 0, 0, 0x3F800000, 0x05000051, 0xA00F0001, 0, 0x3F800000, 0, 0x3F800000,
                                           0x02000001, 0x800F0800, 0xA0E40000, 0x02000001, 0x800F0801, 0xA0E40001, 0x0000FFFF};
    IDirect3DTexture9 *second = NULL;
    IDirect3DSurface9 *second0 = NULL;
    IDirect3DPixelShader9 *shader = NULL;
    hr = IDirect3DDevice9_CreateTexture(g_device, 32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &second, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DTexture9_GetSurfaceLevel(second, 0, &second0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreatePixelShader(g_device, ps_two_targets, &shader);
    DWORD cleared0 = 0, cleared1 = 0;
    if (SUCCEEDED(hr)) {
      IDirect3DDevice9_SetRenderTarget(g_device, 0, level0);
      hr = IDirect3DDevice9_SetRenderTarget(g_device, 1, second0);
      IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFF102030, 1.0f, 0);
      IDirect3DDevice9_SetRenderTarget(g_device, 0, back);
      cleared0 = target_pixel(level0, 3, 3);
      cleared1 = target_pixel(second0, 3, 3);
      IDirect3DDevice9_SetRenderTarget(g_device, 0, level0);
      IDirect3DDevice9_SetPixelShader(g_device, shader);
      screen_quad(0, 0, 16, 32, 0.5f, 0xFFFFFFFF);
      IDirect3DDevice9_SetRenderState(g_device, D3DRS_COLORWRITEENABLE1, D3DCOLORWRITEENABLE_ALPHA);
      screen_quad(16, 0, 32, 32, 0.5f, 0xFFFFFFFF);
      IDirect3DDevice9_SetRenderState(g_device, D3DRS_COLORWRITEENABLE1, 0xF);
      IDirect3DDevice9_SetPixelShader(g_device, NULL);
      IDirect3DDevice9_SetRenderTarget(g_device, 1, NULL);
      IDirect3DDevice9_SetRenderTarget(g_device, 0, back);
    }
    IDirect3DSurface9 *none = (IDirect3DSurface9 *)0x1;
    const HRESULT unset = IDirect3DDevice9_GetRenderTarget(g_device, 1, &none);
    check(SUCCEEDED(hr) && cleared0 == 0xFF102030 && cleared1 == 0xFF102030 && target_pixel(level0, 8, 8) == 0xFFFF0000 && target_pixel(second0, 8, 8) == 0xFF00FF00 &&
          target_pixel(level0, 24, 8) == 0xFFFF0000 && target_pixel(second0, 24, 8) == 0xFF102030 && unset == D3DERR_NOTFOUND && none == NULL,
          "two render targets: Clear fills both, oC0 and oC1 each its own, COLORWRITEENABLE1 masks the second; target 1 unset again");
    if (shader) IDirect3DPixelShader9_Release(shader);
    if (second0) IDirect3DSurface9_Release(second0);
    if (second) IDirect3DTexture9_Release(second);
  }
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZENABLE, D3DZB_TRUE);
done:
  if (back) IDirect3DSurface9_Release(back);
  if (level0) IDirect3DSurface9_Release(level0);
  if (level1) IDirect3DSurface9_Release(level1);
  if (face2) IDirect3DSurface9_Release(face2);
  if (face3) IDirect3DSurface9_Release(face3);
  if (texture) IDirect3DTexture9_Release(texture);
  if (cube) IDirect3DCubeTexture9_Release(cube);
}

/* The signed (bump map) formats and the bump environment operations of the stages; a depth buffer made lockable with d3d9.dll's own usage (ddraw.dll's). */
static void test_bump_and_depth_locks(void) {
  check(IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_V8U8) == D3D_OK &&
            IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_LEGACYBUMPMAP, D3DRTYPE_TEXTURE, D3DFMT_L6V5U5) == D3D_OK &&
            IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_V8U8) == D3DERR_NOTAVAILABLE &&
            IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_LEGACYBUMPMAP, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8) == D3DERR_NOTAVAILABLE,
        "the signed formats are textures (and bump maps), not targets");
  D3DCAPS9 caps;
  IDirect3DDevice9_GetDeviceCaps(g_device, &caps);
  check((caps.TextureOpCaps & (D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE)) == (D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE),
        "the caps have the bump environment operations");
  IDirect3DTexture9 *bump = NULL, *x8 = NULL, *colours = NULL;
  D3DLOCKED_RECT locked;
  /* V8U8 u -1, v 1 through BLENDFACTORALPHA with a factor of half: (u, v, 1) * 0.5 + 0.5 */
  HRESULT hr = IDirect3DDevice9_CreateTexture(g_device, 2, 2, 1, 0, D3DFMT_V8U8, D3DPOOL_MANAGED, &bump, NULL);
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DTexture9_LockRect(bump, 0, &locked, NULL, 0))) {
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) ((WORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0x7F81;      /* (u -127, v 127) */
    IDirect3DTexture9_UnlockRect(bump, 0);
  }
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)bump);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_TEXTUREFACTOR, 0x80FFFFFF);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_BLENDFACTORALPHA);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(bump && near_colour(pixel(32, 32), 0, 255, 255), "a V8U8 texture samples as signed numbers, (u, v, 1)");
  /* X8L8V8U8: u 0, v -1, an unsigned luminance of 0x40 */
  hr = IDirect3DDevice9_CreateTexture(g_device, 2, 2, 1, 0, D3DFMT_X8L8V8U8, D3DPOOL_MANAGED, &x8, NULL);
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DTexture9_LockRect(x8, 0, &locked, NULL, 0))) {
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) ((DWORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0x00408100u;
    IDirect3DTexture9_UnlockRect(x8, 0);
  }
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)x8);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(x8 && near_colour(pixel(32, 32), 128, 0, 160), "an X8L8V8U8 texture: signed u, v and an unsigned luminance");
  /* BUMPENVMAP: stage 0's du of half moves stage 1's u by M00 * du - from the red half of a two-texel texture to the green one */
  hr = IDirect3DDevice9_CreateTexture(g_device, 2, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &colours, NULL);
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DTexture9_LockRect(colours, 0, &locked, NULL, 0))) {
    ((DWORD *)locked.pBits)[0] = 0xFFFF0000u;
    ((DWORD *)locked.pBits)[1] = 0xFF00FF00u;
    IDirect3DTexture9_UnlockRect(colours, 0);
  }
  if (bump && SUCCEEDED(IDirect3DTexture9_LockRect(bump, 0, &locked, NULL, 0))) {
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) ((WORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0x0040;      /* (du 64/127, dv 0) */
    IDirect3DTexture9_UnlockRect(bump, 0);
  }
  const float one = 1.0f, zero = 0.0f, half = 0.5f;
  DWORD one_bits, zero_bits, half_bits;
  memcpy(&one_bits, &one, 4);
  memcpy(&zero_bits, &zero, 4);
  memcpy(&half_bits, &half, 4);
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)bump);
  IDirect3DDevice9_SetTexture(g_device, 1, (IDirect3DBaseTexture9 *)colours);
  IDirect3DDevice9_SetSamplerState(g_device, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(g_device, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(g_device, 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_BUMPENVMAP);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_BUMPENVMAT00, one_bits);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_BUMPENVMAT01, zero_bits);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_BUMPENVMAT10, zero_bits);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_BUMPENVMAT11, zero_bits);
  IDirect3DDevice9_SetTextureStageState(g_device, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(g_device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  IDirect3DDevice9_SetTextureStageState(g_device, 1, D3DTSS_TEXCOORDINDEX, 0);
  DWORD passes = 0;
  check(IDirect3DDevice9_ValidateDevice(g_device, &passes) == D3D_OK && passes == 1, "ValidateDevice takes the bump environment operations");
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(colours && near_colour(pixel(10, 32), 0, 255, 0) && near_colour(pixel(54, 32), 0, 255, 0), "BUMPENVMAP moves the next stage's coordinates by the matrix");
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_BUMPENVMAT00, zero_bits);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(near_colour(pixel(10, 32), 255, 0, 0) && near_colour(pixel(54, 32), 0, 255, 0), "... and by nothing with a matrix of 0");
  /* BUMPENVMAPLUMINANCE with V8U8 (luminance 1): the texel times saturate(1 * 0.5 + 0) */
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_BUMPENVMAPLUMINANCE);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_BUMPENVLSCALE, half_bits);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_BUMPENVLOFFSET, zero_bits);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(near_colour(pixel(10, 32), 128, 0, 0), "BUMPENVMAPLUMINANCE scales the next stage's texel");
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_BUMPENVLSCALE, zero_bits);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLORARG2, D3DTA_CURRENT);
  IDirect3DDevice9_SetTextureStageState(g_device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  IDirect3DDevice9_SetTextureStageState(g_device, 1, D3DTSS_TEXCOORDINDEX, 1);
  IDirect3DDevice9_SetSamplerState(g_device, 1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
  IDirect3DDevice9_SetTexture(g_device, 0, NULL);
  IDirect3DDevice9_SetTexture(g_device, 1, NULL);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
  if (bump) IDirect3DTexture9_Release(bump);
  if (x8) IDirect3DTexture9_Release(x8);
  if (colours) IDirect3DTexture9_Release(colours);
  /* depth buffers: D24S8 locks only when made with TCOS_USAGE_LOCKABLE_DEPTH (d3d9.dll's private TcosD3D9CreateLockableDepthStencil, ddraw.dll's), depth in the low 24 bits
   * and stencil above; what was written comes back */
  IDirect3DSurface9 *plain = NULL, *lockable = NULL, *d16 = NULL;
  typedef HRESULT(WINAPI * LockableDepth)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **);
  LockableDepth make_lockable = (LockableDepth)(void *)GetProcAddress(GetModuleHandleA("d3d9.dll"), "TcosD3D9CreateLockableDepthStencil");
  IDirect3DDevice9_CreateDepthStencilSurface(g_device, 8, 8, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &plain, NULL);
  check(make_lockable != NULL, "d3d9.dll's lockable depth buffers");
  if (make_lockable) {
    make_lockable(g_device, SIZE, SIZE, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &lockable);
    make_lockable(g_device, 8, 8, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, &d16);
  }
  check(plain && IDirect3DSurface9_LockRect(plain, &locked, NULL, 0) == D3DERR_INVALIDCALL, "a D24S8 depth buffer cannot be locked");
  DWORD back24 = 0;
  WORD back16 = 0;
  if (lockable && SUCCEEDED(IDirect3DSurface9_LockRect(lockable, &locked, NULL, 0))) {
    for (int y = 0; y < SIZE; ++y) for (int x = 0; x < SIZE; ++x) ((DWORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0x5A123456u;
    IDirect3DSurface9_UnlockRect(lockable);
    if (SUCCEEDED(IDirect3DSurface9_LockRect(lockable, &locked, NULL, D3DLOCK_READONLY))) {
      back24 = ((const DWORD *)((const BYTE *)locked.pBits + 3 * locked.Pitch))[5];
      IDirect3DSurface9_UnlockRect(lockable);
    }
  }
  check(back24 == 0x5A123456u, "... made lockable it can, and what was written comes back from the device");
  if (lockable) {      /* (cleared by the device: depth 0.5, stencil 7) */
    IDirect3DSurface9 *old = NULL;
    IDirect3DDevice9_GetDepthStencilSurface(g_device, &old);
    IDirect3DDevice9_SetDepthStencilSurface(g_device, lockable);
    D3DVIEWPORT9 vp = {0, 0, SIZE, SIZE, 0.0f, 1.0f}, was;      /* (a buffer as large as the target: a smaller one is not drawn with) */
    IDirect3DDevice9_GetViewport(g_device, &was);
    IDirect3DDevice9_SetViewport(g_device, &vp);
    IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.5f, 7);
    IDirect3DDevice9_SetDepthStencilSurface(g_device, old);
    IDirect3DDevice9_SetViewport(g_device, &was);
    if (old) IDirect3DSurface9_Release(old);
    if (SUCCEEDED(IDirect3DSurface9_LockRect(lockable, &locked, NULL, D3DLOCK_READONLY))) {
      back24 = ((const DWORD *)locked.pBits)[0];
      IDirect3DSurface9_UnlockRect(lockable);
    }
    check(back24 >> 24 == 7 && (back24 & 0xFFFFFF) >= 0x7FFFFE && (back24 & 0xFFFFFF) <= 0x800001, "... and what the device cleared is read back");
  }
  if (d16 && SUCCEEDED(IDirect3DSurface9_LockRect(d16, &locked, NULL, 0))) {
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) ((WORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0x1234;
    IDirect3DSurface9_UnlockRect(d16);
    if (SUCCEEDED(IDirect3DSurface9_LockRect(d16, &locked, NULL, D3DLOCK_READONLY))) {
      back16 = ((const WORD *)locked.pBits)[9];
      IDirect3DSurface9_UnlockRect(d16);
    }
  }
  check(back16 == 0x1234, "a D16 made lockable keeps 16 bits exactly");
  if (plain) IDirect3DSurface9_Release(plain);
  if (lockable) IDirect3DSurface9_Release(lockable);
  if (d16) IDirect3DSurface9_Release(d16);
}

/* Each part leaves the device's references as they were: what it made is gone with its last reference. */
static void api_part(void (*part)(void), const char *name) {
  const ULONG before = refs(g_device);
  part();
  char what[160];
  snprintf(what, sizeof what, "%s: the device's references are as they were after it", name);
  check(refs(g_device) == before, what);
}
static void test_api(void) {
  api_part(test_clear_rectangles, "clears of rectangles");
  api_part(test_fills_and_copies, "fills and copies");
  api_part(test_update_texture, "UpdateTexture");
  api_part(test_getdc, "GetDC");
  api_part(test_swap_chains, "swap chains");
  api_part(test_front_buffer_gamma_cursor, "the front buffer, gamma, the cursor");
  api_part(test_palettes, "palettes");
  api_part(test_levels_and_references, "levels and references");
  api_part(test_state_block_kinds, "state blocks");
  api_part(test_queries, "queries");
  api_part(test_device_misc, "the device's other methods");
  api_part(test_vertex_processing, "vertex processing");
  api_part(test_wide_formats, "wide formats");
  api_part(test_bump_and_depth_locks, "bump maps and depth locks");
  api_part(test_level_targets, "levels and cube faces as targets");
  api_part(test_process_vertices, "ProcessVertices, draws with what is not set, a smaller target than the depth buffer");
  api_part(test_ex, "Direct3D 9Ex");
}
