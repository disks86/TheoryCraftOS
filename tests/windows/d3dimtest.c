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
/* Direct3D 1, 2, 3 and 7 immediate mode (ddraw.dll) on the real kernel: what is drawn is read back from the DirectDraw surfaces with Lock. Direct3D 7:
 * Clear, transformed and untransformed draws, the z buffer (and its contents read back), a colour-keyed texture, lighting, state blocks, vertex
 * buffers and ProcessVertices, a 3D flipping chain shown with Flip; Direct3D 3: viewports, legacy lights and material handles, Clear2, textures
 * through IDirect3DTexture2; Direct3D 2: D3DVERTEXTYPE draws, Begin .. End, TEXTUREHANDLE; Direct3D 1: an execute buffer (PROCESSVERTICES and
 * TRIANGLE, a triangle that is clipped). Built for i386 (tests/wow64_qemu.sh d3dimtest) and x86-64 (tests/d3d12_qemu.sh d3dimtest). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ddraw.h>
#include <d3d.h>

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "d3dimtest: ok: %s\n" : "d3dimtest: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}
static ULONG refs(void *object) {
  IUnknown *u = object;
  IUnknown_AddRef(u);
  return IUnknown_Release(u);
}
static HWND g_window;

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
static DDPIXELFORMAT z_format(DWORD bits, DWORD mask) {
  DDPIXELFORMAT pf;
  memset(&pf, 0, sizeof pf);
  pf.dwSize = sizeof pf;
  pf.dwFlags = DDPF_ZBUFFER;
  pf.dwZBufferBitDepth = bits;
  pf.dwZBitMask = mask;
  return pf;
}
static int near_colour(DWORD a, DWORD b, int tolerance) {
  for (int s = 0; s < 32; s += 8) {
    const int x = (int)((a >> s) & 0xFF), y = (int)((b >> s) & 0xFF);
    if (x - y > tolerance || y - x > tolerance) return 0;
  }
  return 1;
}

/* ---- Direct3D 7 ------------------------------------------------------------------------------------------------------------------------------------ */

static IDirectDraw7 *g_dd7;
static IDirectDrawSurface7 *surface7(DWORD w, DWORD h, DWORD caps, const DDPIXELFORMAT *pf) {
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (pf ? DDSD_PIXELFORMAT : 0);
  d.dwWidth = w;
  d.dwHeight = h;
  d.ddsCaps.dwCaps = caps;
  if (pf) d.ddpfPixelFormat = *pf;
  IDirectDrawSurface7 *s = NULL;
  if (FAILED(IDirectDraw7_CreateSurface(g_dd7, &d, &s, NULL))) return NULL;
  return s;
}
static DWORD pixel7(IDirectDrawSurface7 *s, int x, int y) {
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  if (FAILED(IDirectDrawSurface7_Lock(s, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL))) return 0xDEADBEEF;
  DWORD v = 0;
  const DWORD bytes = (d.ddpfPixelFormat.dwFlags & DDPF_ZBUFFER) ? d.ddpfPixelFormat.dwZBufferBitDepth / 8 : d.ddpfPixelFormat.dwRGBBitCount / 8;
  memcpy(&v, (BYTE *)d.lpSurface + (SIZE_T)y * d.lPitch + (SIZE_T)x * bytes, bytes);
  IDirectDrawSurface7_Unlock(s, NULL);
  return v;
}

typedef struct { float x, y, z, rhw; DWORD diffuse; } TlVertex;
typedef struct { float x, y, z; DWORD diffuse; } ColorVertex;
typedef struct { float x, y, z, nx, ny, nz; } NormalVertex;
typedef struct { float x, y, z, rhw; DWORD diffuse; float u, v; } TexVertex;
#define FVF_TL (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define FVF_COLOR (D3DFVF_XYZ | D3DFVF_DIFFUSE)
#define FVF_NORMAL (D3DFVF_XYZ | D3DFVF_NORMAL)
#define FVF_TEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void quad_tl(TlVertex *v, float x0, float y0, float x1, float y1, float z, DWORD c) {
  const TlVertex q[4] = {{x0, y0, z, 1.0f, c}, {x1, y0, z, 1.0f, c}, {x0, y1, z, 1.0f, c}, {x1, y1, z, 1.0f, c}};
  memcpy(v, q, sizeof q);
}

static void test_d3d7(void) {
  HRESULT hr = DirectDrawCreateEx(NULL, (void **)&g_dd7, &IID_IDirectDraw7, NULL);
  check(SUCCEEDED(hr), "DirectDrawCreateEx");
  if (FAILED(hr)) return;
  IDirectDraw7_SetCooperativeLevel(g_dd7, g_window, DDSCL_NORMAL);
  IDirect3D7 *d3d = NULL;
  hr = IDirectDraw7_QueryInterface(g_dd7, &IID_IDirect3D7, (void **)&d3d);
  check(hr == S_OK && d3d, "IDirectDraw7 answers IDirect3D7");
  if (!d3d) return;
  check(refs(g_dd7) == 2, "IDirect3D7 counts on IDirectDraw7");

  const DDPIXELFORMAT xrgb = rgb_format(32, 0xFF0000, 0xFF00, 0xFF, 0), zf = z_format(16, 0xFFFF);
  IDirectDrawSurface7 *rt = surface7(64, 64, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY, &xrgb);
  IDirectDrawSurface7 *z = surface7(64, 64, DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY, &zf);
  check(rt && z, "a render target and a z buffer");
  if (!rt || !z) return;
  check(SUCCEEDED(IDirectDrawSurface7_AddAttachedSurface(rt, z)), "the z buffer attaches to the target");
  IDirect3DDevice7 *dev = NULL;
  const ULONG rt_refs = refs(rt);
  hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, rt, &dev);
  check(hr == D3D_OK && dev, "IDirect3D7::CreateDevice (HAL)");
  if (!dev) return;
  check(refs(rt) == rt_refs + 1, "the device holds a reference of its target");
  IDirect3DDevice7 *dev2 = NULL;
  check(IDirect3DDevice7_QueryInterface(dev, &IID_IDirect3DDevice3, (void **)&dev2) == E_NOINTERFACE, "a Direct3D 7 device has no IDirect3DDevice3");
  D3DDEVICEDESC7 caps;
  memset(&caps, 0, sizeof caps);
  check(SUCCEEDED(IDirect3DDevice7_GetCaps(dev, &caps)) && (caps.dwDevCaps & D3DDEVCAPS_HWRASTERIZATION) && caps.wMaxTextureBlendStages >= 2, "GetCaps");
  DWORD zenable = 0;
  IDirect3DDevice7_GetRenderState(dev, D3DRENDERSTATE_ZENABLE, &zenable);
  check(zenable == D3DZB_TRUE, "ZENABLE starts on with an attached z buffer");

  /* Clear */
  hr = IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF0000FF, 0.5f, 0);
  check(hr == D3D_OK, "Clear");
  check(near_colour(pixel7(rt, 10, 10) & 0xFFFFFF, 0x0000FF, 1), "Clear's colour reads back with Lock");
  {
    const DWORD zv = pixel7(z, 5, 5);
    printf("d3dimtest: z after Clear(0.5) = %#lx\n", (unsigned long)zv);
    check(zv >= 0x7F00 && zv <= 0x8100, "the z buffer's Clear reads back (16 bits)");
  }

  /* transformed vertices */
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_LIGHTING, FALSE);
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
  TlVertex q[4];
  quad_tl(q, 0.0f, 0.0f, 32.0f, 64.0f, 0.0f, 0xFF00FF00);
  check(SUCCEEDED(IDirect3DDevice7_BeginScene(dev)), "BeginScene");
  hr = IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, FVF_TL, q, 4, 0);
  check(hr == D3D_OK, "DrawPrimitive of transformed vertices");
  check(IDirect3DDevice7_BeginScene(dev) == D3DERR_SCENE_IN_SCENE, "BeginScene twice: D3DERR_SCENE_IN_SCENE");
  check(SUCCEEDED(IDirect3DDevice7_EndScene(dev)), "EndScene");
  check(near_colour(pixel7(rt, 10, 10) & 0xFFFFFF, 0x00FF00, 1) && near_colour(pixel7(rt, 50, 10) & 0xFFFFFF, 0x0000FF, 1), "the left half is drawn green");

  /* untransformed vertices: identity transforms, the viewport maps -1 .. 1 to the target */
  D3DMATRIX id = {._11 = 1.0f, ._22 = 1.0f, ._33 = 1.0f, ._44 = 1.0f};
  IDirect3DDevice7_SetTransform(dev, D3DTRANSFORMSTATE_WORLD, &id);
  IDirect3DDevice7_SetTransform(dev, D3DTRANSFORMSTATE_VIEW, &id);
  IDirect3DDevice7_SetTransform(dev, D3DTRANSFORMSTATE_PROJECTION, &id);
  const ColorVertex cq[4] = {{-1.0f, 1.0f, 0.5f, 0xFFFF0000}, {1.0f, 1.0f, 0.5f, 0xFFFF0000}, {-1.0f, 0.0f, 0.5f, 0xFFFF0000}, {1.0f, 0.0f, 0.5f, 0xFFFF0000}};
  hr = IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, FVF_COLOR, (void *)cq, 4, 0);
  check(hr == D3D_OK && near_colour(pixel7(rt, 50, 10) & 0xFFFFFF, 0xFF0000, 1) && near_colour(pixel7(rt, 50, 50) & 0xFFFFFF, 0x0000FF, 1),
        "untransformed vertices: the top half red");

  /* the z buffer: a far quad after a near one stays hidden */
  IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
  quad_tl(q, 0.0f, 0.0f, 64.0f, 64.0f, 0.2f, 0xFF00FF00);
  IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, FVF_TL, q, 4, 0);
  quad_tl(q, 0.0f, 0.0f, 64.0f, 64.0f, 0.8f, 0xFFFF0000);
  IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, FVF_TL, q, 4, 0);
  check(near_colour(pixel7(rt, 32, 32) & 0xFFFFFF, 0x00FF00, 1), "the z test keeps the nearer quad");
  {
    const DWORD zv = pixel7(z, 32, 32);
    check(zv >= 0x3200 && zv <= 0x3500, "the z buffer has the near quad's depth (0.2)");
  }
  /* the CPU writes the z buffer (a depth fill): the GPU sees it */
  DDBLTFX fx;
  memset(&fx, 0, sizeof fx);
  fx.dwSize = sizeof fx;
  fx.dwFillDepth = 0;
  check(SUCCEEDED(IDirectDrawSurface7_Blt(z, NULL, NULL, NULL, DDBLT_DEPTHFILL | DDBLT_WAIT, &fx)), "a depth fill of the z buffer");
  quad_tl(q, 0.0f, 0.0f, 64.0f, 64.0f, 0.1f, 0xFF0000FF);
  IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, FVF_TL, q, 4, 0);
  check(near_colour(pixel7(rt, 32, 32) & 0xFFFFFF, 0x00FF00, 1), "after a depth fill of 0 nothing passes LESSEQUAL at 0.1");
  /* the CPU writes the target (a colour fill) and Direct3D draws over a part */
  fx.dwFillColor = 0x00FFFF00;
  IDirectDrawSurface7_Blt(rt, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
  quad_tl(q, 0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 0xFFFFFFFF);
  IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, FVF_TL, q, 4, 0);
  check(near_colour(pixel7(rt, 4, 4) & 0xFFFFFF, 0xFFFFFF, 1) && near_colour(pixel7(rt, 40, 40) & 0xFFFFFF, 0xFFFF00, 1),
        "a colour fill by the CPU and a draw over a part of it");

  /* a colour-keyed texture */
  DDSURFACEDESC2 td;
  memset(&td, 0, sizeof td);
  td.dwSize = sizeof td;
  td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CKSRCBLT;
  td.dwWidth = td.dwHeight = 4;
  td.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
  td.ddpfPixelFormat = rgb_format(16, 0xF800, 0x07E0, 0x001F, 0);
  td.ddckCKSrcBlt.dwColorSpaceLowValue = td.ddckCKSrcBlt.dwColorSpaceHighValue = 0xF800;      /* (red is the key) */
  IDirectDrawSurface7 *tex = NULL;
  hr = IDirectDraw7_CreateSurface(g_dd7, &td, &tex, NULL);
  check(hr == DD_OK && tex, "a 16-bit texture with a colour key");
  if (tex) {
    DDSURFACEDESC2 ld;
    memset(&ld, 0, sizeof ld);
    ld.dwSize = sizeof ld;
    if (SUCCEEDED(IDirectDrawSurface7_Lock(tex, NULL, &ld, DDLOCK_WAIT, NULL))) {
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) ((WORD *)((BYTE *)ld.lpSurface + y * ld.lPitch))[x] = x < 2 ? 0x001F : 0xF800;      /* (left blue, right the key) */
      IDirectDrawSurface7_Unlock(tex, NULL);
    }
    IDirectDrawSurface7_Blt(rt, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);      /* (yellow) */
    IDirect3DDevice7_SetTexture(dev, 0, tex);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_COLORKEYENABLE, TRUE);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTFG_POINT);
    const TexVertex tq[4] = {{0, 0, 0, 1, 0xFFFFFFFF, 0, 0}, {64, 0, 0, 1, 0xFFFFFFFF, 1, 0}, {0, 64, 0, 1, 0xFFFFFFFF, 0, 1}, {64, 64, 0, 1, 0xFFFFFFFF, 1, 1}};
    hr = IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, FVF_TEX, (void *)tq, 4, 0);
    check(hr == D3D_OK && near_colour(pixel7(rt, 10, 32) & 0xFFFFFF, 0x0000FF, 4), "the texture's blue half is drawn");
    check(near_colour(pixel7(rt, 50, 32) & 0xFFFFFF, 0xFFFF00, 4), "its colour-keyed half is not");
    /* the CPU changes the texture: the next draw sees it */
    memset(&ld, 0, sizeof ld);
    ld.dwSize = sizeof ld;
    if (SUCCEEDED(IDirectDrawSurface7_Lock(tex, NULL, &ld, DDLOCK_WAIT, NULL))) {
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) ((WORD *)((BYTE *)ld.lpSurface + y * ld.lPitch))[x] = 0x07E0;
      IDirectDrawSurface7_Unlock(tex, NULL);
    }
    IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, FVF_TEX, (void *)tq, 4, 0);
    check(near_colour(pixel7(rt, 50, 32) & 0xFFFFFF, 0x00FF00, 4), "a texture written by the CPU is sent again");
    IDirectDrawSurface7 *got = NULL;
    check(SUCCEEDED(IDirect3DDevice7_GetTexture(dev, 0, &got)) && got == tex, "GetTexture gives the texture");
    if (got) IDirectDrawSurface7_Release(got);
    IDirect3DDevice7_SetTexture(dev, 0, NULL);
    IDirect3DDevice7_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirectDrawSurface7_Release(tex);
  }

  /* lighting: a directional light straight onto a white material */
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_LIGHTING, TRUE);
  D3DMATERIAL7 mat;
  memset(&mat, 0, sizeof mat);
  mat.diffuse.r = 1.0f; mat.diffuse.g = 1.0f; mat.diffuse.b = 1.0f; mat.diffuse.a = 1.0f;
  IDirect3DDevice7_SetMaterial(dev, &mat);
  D3DLIGHT7 light;
  memset(&light, 0, sizeof light);
  light.dltType = D3DLIGHT_DIRECTIONAL;
  light.dcvDiffuse.r = 0.5f; light.dcvDiffuse.g = 0.25f; light.dcvDiffuse.b = 1.0f;
  light.dvDirection.z = 1.0f;
  check(SUCCEEDED(IDirect3DDevice7_SetLight(dev, 3, &light)) && SUCCEEDED(IDirect3DDevice7_LightEnable(dev, 3, TRUE)), "SetLight and LightEnable");
  BOOL on = FALSE;
  check(SUCCEEDED(IDirect3DDevice7_GetLightEnable(dev, 3, &on)) && on, "GetLightEnable");
  const NormalVertex nq[4] = {{-1, 1, 0.5f, 0, 0, -1}, {1, 1, 0.5f, 0, 0, -1}, {-1, -1, 0.5f, 0, 0, -1}, {1, -1, 0.5f, 0, 0, -1}};
  IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, FVF_NORMAL, (void *)nq, 4, 0);
  {
    const DWORD c = pixel7(rt, 32, 32) & 0xFFFFFF;
    printf("d3dimtest: lit colour %#lx\n", (unsigned long)c);
    check(near_colour(c, 0x7F3FFF, 3), "the light's colour on the white material");
  }
  IDirect3DDevice7_LightEnable(dev, 3, FALSE);
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_LIGHTING, FALSE);

  /* state blocks */
  DWORD block = 0, v = 0;
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_FOGCOLOR, 0x11);
  check(SUCCEEDED(IDirect3DDevice7_BeginStateBlock(dev)), "BeginStateBlock");
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_FOGCOLOR, 0x22);
  check(SUCCEEDED(IDirect3DDevice7_EndStateBlock(dev, &block)) && block, "EndStateBlock");
  IDirect3DDevice7_GetRenderState(dev, D3DRENDERSTATE_FOGCOLOR, &v);
  check(v == 0x11, "a recorded state is not set on the device");
  IDirect3DDevice7_ApplyStateBlock(dev, block);
  IDirect3DDevice7_GetRenderState(dev, D3DRENDERSTATE_FOGCOLOR, &v);
  check(v == 0x22, "ApplyStateBlock sets it");
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_FOGCOLOR, 0x33);
  IDirect3DDevice7_CaptureStateBlock(dev, block);
  IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_FOGCOLOR, 0x44);
  IDirect3DDevice7_ApplyStateBlock(dev, block);
  IDirect3DDevice7_GetRenderState(dev, D3DRENDERSTATE_FOGCOLOR, &v);
  check(v == 0x33, "CaptureStateBlock takes the current value");
  check(SUCCEEDED(IDirect3DDevice7_DeleteStateBlock(dev, block)) && IDirect3DDevice7_ApplyStateBlock(dev, block) == D3DERR_INVALIDSTATEBLOCK,
        "DeleteStateBlock");

  /* vertex buffers: ProcessVertices into a transformed buffer, then DrawPrimitiveVB */
  D3DVERTEXBUFFERDESC vd;
  memset(&vd, 0, sizeof vd);
  vd.dwSize = sizeof vd;
  vd.dwFVF = FVF_COLOR;
  vd.dwNumVertices = 4;
  IDirect3DVertexBuffer7 *src = NULL, *dst = NULL;
  const ULONG dd_refs = refs(g_dd7);
  check(SUCCEEDED(IDirect3D7_CreateVertexBuffer(d3d, &vd, &src, 0)) && refs(g_dd7) == dd_refs + 1, "a vertex buffer holds IDirectDraw7");
  vd.dwFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
  IDirect3D7_CreateVertexBuffer(d3d, &vd, &dst, 0);
  if (src && dst) {
    void *data = NULL;
    DWORD size = 0;
    IDirect3DVertexBuffer7_Lock(src, DDLOCK_WAIT, &data, &size);
    const ColorVertex bq[4] = {{-1.0f, 1.0f, 0.5f, 0xFF00FFFF}, {0.0f, 1.0f, 0.5f, 0xFF00FFFF}, {-1.0f, -1.0f, 0.5f, 0xFF00FFFF}, {0.0f, -1.0f, 0.5f, 0xFF00FFFF}};
    check(data && size == sizeof bq, "Lock of a vertex buffer");
    if (data) memcpy(data, bq, sizeof bq);
    IDirect3DVertexBuffer7_Unlock(src);
    hr = IDirect3DVertexBuffer7_ProcessVertices(dst, D3DVOP_TRANSFORM, 0, 4, src, 0, dev, 0);
    check(hr == D3D_OK, "ProcessVertices");
    IDirect3DVertexBuffer7_Lock(dst, DDLOCK_WAIT | DDLOCK_READONLY, &data, NULL);
    const TlVertex *out = data;
    check(out && out[0].x == 0.0f && out[0].y == 0.0f && out[3].x == 32.0f && out[3].y == 64.0f && out[0].z == 0.5f, "the processed positions");
    IDirect3DVertexBuffer7_Unlock(dst);
    IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    hr = IDirect3DDevice7_DrawPrimitiveVB(dev, D3DPT_TRIANGLESTRIP, dst, 0, 4, 0);
    check(hr == D3D_OK && near_colour(pixel7(rt, 10, 10) & 0xFFFFFF, 0x00FFFF, 1) && near_colour(pixel7(rt, 50, 10) & 0xFFFFFF, 0, 1), "DrawPrimitiveVB");
  }
  if (src) IDirect3DVertexBuffer7_Release(src);
  if (dst) IDirect3DVertexBuffer7_Release(dst);
  check(refs(g_dd7) == dd_refs, "the vertex buffers let go of IDirectDraw7");

  check(IDirect3DDevice7_Release(dev) == 0, "the device goes with its last reference");
  check(refs(rt) == rt_refs, "and lets go of its target");
  IDirectDrawSurface7_DeleteAttachedSurface(rt, 0, z);
  IDirectDrawSurface7_Release(z);
  IDirectDrawSurface7_Release(rt);
  IDirect3D7_Release(d3d);
}

/* A 3D flipping chain in exclusive mode: what is drawn into the back buffer is the front buffer after Flip. */
static void test_flip(void) {
  if (FAILED(IDirectDraw7_SetCooperativeLevel(g_dd7, g_window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN))) { check(0, "exclusive mode"); return; }
  IDirectDraw7_SetDisplayMode(g_dd7, 640, 480, 32, 0, 0);
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
  d.dwBackBufferCount = 1;
  d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
  IDirectDrawSurface7 *front = NULL, *back = NULL;
  HRESULT hr = IDirectDraw7_CreateSurface(g_dd7, &d, &front, NULL);
  check(hr == DD_OK && front, "a 3D flipping chain");
  if (!front) return;
  DDSCAPS2 bc = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
  IDirectDrawSurface7_GetAttachedSurface(front, &bc, &back);
  IDirect3D7 *d3d = NULL;
  IDirectDraw7_QueryInterface(g_dd7, &IID_IDirect3D7, (void **)&d3d);
  IDirect3DDevice7 *dev = NULL;
  hr = d3d && back ? IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, back, &dev) : E_FAIL;
  check(hr == D3D_OK && dev, "a device on the back buffer");
  if (dev) {
    for (int frame = 0; frame < 2; ++frame) {
      const DWORD colour = frame ? 0xFF00FF00 : 0xFFFF0000;
      IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET, colour, 1.0f, 0);
      hr = IDirectDrawSurface7_Flip(front, NULL, DDFLIP_WAIT);
      check(hr == DD_OK, "Flip");
      check(near_colour(pixel7(front, 100, 100) & 0xFFFFFF, colour & 0xFFFFFF, 1), frame ? "the second frame is the front buffer" : "the first frame is the front buffer");
    }
    IDirect3DDevice7_Release(dev);
  }
  if (d3d) IDirect3D7_Release(d3d);
  if (back) IDirectDrawSurface7_Release(back);
  IDirectDrawSurface7_Release(front);
  IDirectDraw7_RestoreDisplayMode(g_dd7);
  IDirectDraw7_SetCooperativeLevel(g_dd7, g_window, DDSCL_NORMAL);
}

/* ---- Direct3D 3 ------------------------------------------------------------------------------------------------------------------------------------ */

static DWORD pixel4(IDirectDrawSurface4 *s, int x, int y) {
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  if (FAILED(IDirectDrawSurface4_Lock(s, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL))) return 0xDEADBEEF;
  DWORD v = 0;
  memcpy(&v, (BYTE *)d.lpSurface + (SIZE_T)y * d.lPitch + (SIZE_T)x * (d.ddpfPixelFormat.dwRGBBitCount / 8), d.ddpfPixelFormat.dwRGBBitCount / 8);
  IDirectDrawSurface4_Unlock(s, NULL);
  return v;
}

static void test_d3d3(void) {
  IDirectDraw *dd1 = NULL;
  IDirectDraw4 *dd = NULL;
  if (FAILED(DirectDrawCreate(NULL, &dd1, NULL)) || FAILED(IDirectDraw_QueryInterface(dd1, &IID_IDirectDraw4, (void **)&dd))) { check(0, "IDirectDraw4"); return; }
  IDirectDraw_Release(dd1);
  IDirectDraw4_SetCooperativeLevel(dd, g_window, DDSCL_NORMAL);
  IDirect3D3 *d3d = NULL;
  check(SUCCEEDED(IDirectDraw4_QueryInterface(dd, &IID_IDirect3D3, (void **)&d3d)) && d3d, "IDirect3D3");
  if (!d3d) { IDirectDraw4_Release(dd); return; }
  DDSURFACEDESC2 d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  d.dwWidth = d.dwHeight = 64;
  d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
  d.ddpfPixelFormat = rgb_format(16, 0xF800, 0x07E0, 0x001F, 0);
  IDirectDrawSurface4 *rt = NULL;
  IDirectDraw4_CreateSurface(dd, &d, &rt, NULL);
  IDirect3DDevice3 *dev = NULL;
  HRESULT hr = rt ? IDirect3D3_CreateDevice(d3d, &IID_IDirect3DHALDevice, rt, &dev, NULL) : E_FAIL;
  check(hr == D3D_OK && dev, "IDirect3D3::CreateDevice on a 16-bit target");
  if (dev) {
    IDirect3DViewport3 *vp = NULL;
    IDirect3D3_CreateViewport(d3d, &vp, NULL);
    check(vp && SUCCEEDED(IDirect3DDevice3_AddViewport(dev, vp)), "a viewport");
    D3DVIEWPORT2 v2;
    memset(&v2, 0, sizeof v2);
    v2.dwSize = sizeof v2;
    v2.dwWidth = v2.dwHeight = 64;
    v2.dvClipX = -1.0f; v2.dvClipY = 1.0f; v2.dvClipWidth = 2.0f; v2.dvClipHeight = 2.0f; v2.dvMaxZ = 1.0f;
    check(SUCCEEDED(IDirect3DViewport3_SetViewport2(vp, &v2)) && SUCCEEDED(IDirect3DDevice3_SetCurrentViewport(dev, vp)), "SetViewport2, SetCurrentViewport");
    D3DRECT r = {{0}, {0}, {64}, {64}};
    hr = IDirect3DViewport3_Clear2(vp, 1, &r, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0);
    check(hr == D3D_OK && pixel4(rt, 5, 5) == 0x001F, "Clear2 of the viewport (blue in 5-6-5)");
    /* a legacy light and a material by handle */
    IDirect3DMaterial3 *mat = NULL;
    IDirect3D3_CreateMaterial(d3d, &mat, NULL);
    D3DMATERIAL m;
    memset(&m, 0, sizeof m);
    m.dwSize = sizeof m;
    m.diffuse.r = 1.0f; m.diffuse.g = 1.0f; m.diffuse.b = 1.0f; m.diffuse.a = 1.0f;
    D3DMATERIALHANDLE mh = 0;
    check(mat && SUCCEEDED(IDirect3DMaterial3_SetMaterial(mat, &m)) && SUCCEEDED(IDirect3DMaterial3_GetHandle(mat, dev, &mh)) && mh, "a material handle");
    IDirect3DDevice3_SetLightState(dev, D3DLIGHTSTATE_MATERIAL, mh);
    IDirect3DLight *light = NULL;
    IDirect3D3_CreateLight(d3d, &light, NULL);
    D3DLIGHT2 l;
    memset(&l, 0, sizeof l);
    l.dwSize = sizeof l;
    l.dltType = D3DLIGHT_DIRECTIONAL;
    l.dcvColor.r = 0.0f; l.dcvColor.g = 1.0f; l.dcvColor.b = 0.0f;
    l.dvDirection.z = 1.0f;
    l.dwFlags = D3DLIGHT_ACTIVE;
    check(light && SUCCEEDED(IDirect3DLight_SetLight(light, (D3DLIGHT *)&l)) && SUCCEEDED(IDirect3DViewport3_AddLight(vp, light)), "a light in the viewport");
    D3DMATRIX id = {._11 = 1.0f, ._22 = 1.0f, ._33 = 1.0f, ._44 = 1.0f};
    IDirect3DDevice3_SetTransform(dev, D3DTRANSFORMSTATE_WORLD, &id);
    IDirect3DDevice3_SetTransform(dev, D3DTRANSFORMSTATE_VIEW, &id);
    IDirect3DDevice3_SetTransform(dev, D3DTRANSFORMSTATE_PROJECTION, &id);
    IDirect3DDevice3_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    const D3DVERTEX tri[4] = {{{-1.0f}, {1.0f}, {0.5f}, {0}, {0}, {-1.0f}, {0}, {0}}, {{0.0f}, {1.0f}, {0.5f}, {0}, {0}, {-1.0f}, {0}, {0}},
                              {{-1.0f}, {-1.0f}, {0.5f}, {0}, {0}, {-1.0f}, {0}, {0}}, {{0.0f}, {-1.0f}, {0.5f}, {0}, {0}, {-1.0f}, {0}, {0}}};
    IDirect3DDevice3_BeginScene(dev);
    hr = IDirect3DDevice3_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, D3DFVF_VERTEX, (void *)tri, 4, 0);
    IDirect3DDevice3_EndScene(dev);
    check(hr == D3D_OK && pixel4(rt, 10, 32) == 0x07E0 && pixel4(rt, 50, 32) == 0x001F, "a lit D3DVERTEX quad (green light) on the left half");
    /* a texture through IDirect3DTexture2 */
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = 2;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    d.ddpfPixelFormat = rgb_format(32, 0xFF0000, 0xFF00, 0xFF, 0);
    IDirectDrawSurface4 *ts = NULL;
    IDirectDraw4_CreateSurface(dd, &d, &ts, NULL);
    IDirect3DTexture2 *t2 = NULL;
    if (ts) IDirectDrawSurface4_QueryInterface(ts, &IID_IDirect3DTexture2, (void **)&t2);
    check(t2 != NULL, "IDirect3DTexture2 of a texture surface");
    if (t2) {
      DDBLTFX fx;
      memset(&fx, 0, sizeof fx);
      fx.dwSize = sizeof fx;
      fx.dwFillColor = 0x00FF0000;
      IDirectDrawSurface4_Blt(ts, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
      IDirect3DDevice3_SetTexture(dev, 0, t2);
      IDirect3DDevice3_SetLightState(dev, D3DLIGHTSTATE_MATERIAL, 0);
      IDirect3DDevice3_SetRenderState(dev, D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_DECAL);
      const D3DTLVERTEX tl[4] = {{{0}, {0}, {0}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}}, {{64}, {0}, {0}, {1}, {0xFFFFFFFF}, {0}, {1}, {0}},
                                 {{0}, {64}, {0}, {1}, {0xFFFFFFFF}, {0}, {0}, {1}}, {{64}, {64}, {0}, {1}, {0xFFFFFFFF}, {0}, {1}, {1}}};
      hr = IDirect3DDevice3_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, (void *)tl, 4, 0);
      check(hr == D3D_OK && pixel4(rt, 32, 32) == 0xF800, "a texture (DECAL) through SetTexture of IDirect3DTexture2");
      IDirect3DDevice3_SetTexture(dev, 0, NULL);
      IDirect3DTexture2_Release(t2);
    }
    if (ts) IDirectDrawSurface4_Release(ts);
    if (light) { IDirect3DViewport3_DeleteLight(vp, light); IDirect3DLight_Release(light); }
    if (mat) IDirect3DMaterial3_Release(mat);
    if (vp) { IDirect3DDevice3_DeleteViewport(dev, vp); IDirect3DViewport3_Release(vp); }
    check(IDirect3DDevice3_Release(dev) == 0, "the Direct3D 3 device goes");
  }
  if (rt) IDirectDrawSurface4_Release(rt);
  IDirect3D3_Release(d3d);
  IDirectDraw4_Release(dd);
}

/* ---- Direct3D 2 and 1 ------------------------------------------------------------------------------------------------------------------------------ */

static DWORD pixel1(IDirectDrawSurface *s, int x, int y) {
  DDSURFACEDESC d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  if (FAILED(IDirectDrawSurface_Lock(s, NULL, &d, DDLOCK_READONLY | DDLOCK_WAIT, NULL))) return 0xDEADBEEF;
  DWORD v = 0;
  memcpy(&v, (BYTE *)d.lpSurface + (SIZE_T)y * d.lPitch + (SIZE_T)x * 4, 4);
  IDirectDrawSurface_Unlock(s, NULL);
  return v & 0xFFFFFF;
}
static IDirectDrawSurface *surface1(IDirectDraw *dd, DWORD w, DWORD h, DWORD caps) {
  DDSURFACEDESC d;
  memset(&d, 0, sizeof d);
  d.dwSize = sizeof d;
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  d.dwWidth = w;
  d.dwHeight = h;
  d.ddsCaps.dwCaps = caps;
  d.ddpfPixelFormat = rgb_format(32, 0xFF0000, 0xFF00, 0xFF, 0);
  IDirectDrawSurface *s = NULL;
  if (FAILED(IDirectDraw_CreateSurface(dd, &d, &s, NULL))) return NULL;
  return s;
}

static void test_d3d2(IDirectDraw *dd) {
  IDirect3D2 *d3d = NULL;
  check(SUCCEEDED(IDirectDraw_QueryInterface(dd, &IID_IDirect3D2, (void **)&d3d)) && d3d, "IDirect3D2");
  if (!d3d) return;
  IDirectDrawSurface *rt = surface1(dd, 64, 64, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY);
  IDirect3DDevice2 *dev = NULL;
  HRESULT hr = rt ? IDirect3D2_CreateDevice(d3d, &IID_IDirect3DHALDevice, rt, &dev) : E_FAIL;
  check(hr == D3D_OK && dev, "IDirect3D2::CreateDevice");
  if (dev) {
    IDirect3DViewport2 *vp = NULL;
    IDirect3D2_CreateViewport(d3d, &vp, NULL);
    IDirect3DDevice2_AddViewport(dev, vp);
    D3DVIEWPORT v;
    memset(&v, 0, sizeof v);
    v.dwSize = sizeof v;
    v.dwWidth = v.dwHeight = 64;
    v.dvScaleX = v.dvScaleY = 32.0f;
    v.dvMaxX = v.dvMaxY = 1.0f;
    check(SUCCEEDED(IDirect3DViewport2_SetViewport(vp, &v)) && SUCCEEDED(IDirect3DDevice2_SetCurrentViewport(dev, vp)), "a D3DVIEWPORT");
    D3DRECT r = {{0}, {0}, {64}, {64}};
    IDirect3DViewport2_Clear(vp, 1, &r, D3DCLEAR_TARGET);      /* (no background material: black) */
    check(pixel1(rt, 5, 5) == 0, "Clear of a viewport without a background is black");
    D3DMATRIX id = {._11 = 1.0f, ._22 = 1.0f, ._33 = 1.0f, ._44 = 1.0f};
    IDirect3DDevice2_SetTransform(dev, D3DTRANSFORMSTATE_WORLD, &id);
    IDirect3DDevice2_SetTransform(dev, D3DTRANSFORMSTATE_VIEW, &id);
    IDirect3DDevice2_SetTransform(dev, D3DTRANSFORMSTATE_PROJECTION, &id);
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    const D3DLVERTEX lv[4] = {{{-1.0f}, {1.0f}, {0.5f}, 0, {0xFFFF0000}, {0}, {0}, {0}}, {{1.0f}, {1.0f}, {0.5f}, 0, {0xFFFF0000}, {0}, {0}, {0}},
                              {{-1.0f}, {0.0f}, {0.5f}, 0, {0xFFFF0000}, {0}, {0}, {0}}, {{1.0f}, {0.0f}, {0.5f}, 0, {0xFFFF0000}, {0}, {0}, {0}}};
    IDirect3DDevice2_BeginScene(dev);
    hr = IDirect3DDevice2_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, D3DVT_LVERTEX, (void *)lv, 4, 0);
    check(hr == D3D_OK && pixel1(rt, 32, 10) == 0xFF0000 && pixel1(rt, 32, 50) == 0, "a D3DLVERTEX draw: the top half red");
    /* Begin .. End: the bottom half green, with a triangle that goes past the viewport (clipped) */
    const D3DLVERTEX gv[3] = {{{-1.0f}, {0.0f}, {0.5f}, 0, {0xFF00FF00}, {0}, {0}, {0}}, {{3.0f}, {0.0f}, {0.5f}, 0, {0xFF00FF00}, {0}, {0}, {0}},
                              {{-1.0f}, {-4.0f}, {0.5f}, 0, {0xFF00FF00}, {0}, {0}, {0}}};
    check(SUCCEEDED(IDirect3DDevice2_Begin(dev, D3DPT_TRIANGLELIST, D3DVT_LVERTEX, 0)), "Begin");
    for (int k = 0; k < 3; ++k) IDirect3DDevice2_Vertex(dev, (void *)&gv[k]);
    hr = IDirect3DDevice2_End(dev, 0);
    check(hr == D3D_OK && pixel1(rt, 10, 50) == 0x00FF00 && pixel1(rt, 50, 50) == 0x00FF00, "Begin .. End of a clipped triangle");
    D3DCLIPSTATUS cs;
    memset(&cs, 0, sizeof cs);
    check(SUCCEEDED(IDirect3DDevice2_GetClipStatus(dev, &cs)) && (cs.dwStatus & D3DSTATUS_CLIPUNIONRIGHT), "the clip status has the right plane");
    IDirect3DDevice2_EndScene(dev);
    IDirect3DDevice2_DeleteViewport(dev, vp);
    IDirect3DViewport2_Release(vp);
    check(IDirect3DDevice2_Release(dev) == 0, "the Direct3D 2 device goes");
  }
  if (rt) IDirectDrawSurface_Release(rt);
  IDirect3D2_Release(d3d);
}

/* An execute buffer: PROCESSVERTICES (D3DLVERTEX transformed) and two TRIANGLEs, one of them partly off the viewport. */
static void test_d3d1(IDirectDraw *dd) {
  IDirect3D *d3d = NULL;
  check(SUCCEEDED(IDirectDraw_QueryInterface(dd, &IID_IDirect3D, (void **)&d3d)) && d3d, "IDirect3D");
  if (!d3d) return;
  IDirectDrawSurface *rt = surface1(dd, 64, 64, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY);
  IDirect3DDevice *dev = NULL;
  const ULONG before = rt ? refs(rt) : 0;
  HRESULT hr = rt ? IDirectDrawSurface_QueryInterface(rt, &IID_IDirect3DHALDevice, (void **)&dev) : E_FAIL;
  check(hr == S_OK && dev, "a Direct3D 1 device by QueryInterface of the surface");
  if (!dev) { IDirect3D_Release(d3d); return; }
  check(refs(rt) == before + 1, "it counts on the surface");
  IDirect3DViewport *vp = NULL;
  IDirect3D_CreateViewport(d3d, &vp, NULL);
  IDirect3DDevice_AddViewport(dev, vp);
  D3DVIEWPORT v;
  memset(&v, 0, sizeof v);
  v.dwSize = sizeof v;
  v.dwWidth = v.dwHeight = 64;
  v.dvScaleX = v.dvScaleY = 32.0f;
  v.dvMaxX = v.dvMaxY = 1.0f;
  IDirect3DViewport_SetViewport(vp, &v);
  D3DMATRIXHANDLE mh = 0;
  check(SUCCEEDED(IDirect3DDevice_CreateMatrix(dev, &mh)) && mh, "CreateMatrix");
  D3DMATRIX id = {._11 = 1.0f, ._22 = 1.0f, ._33 = 1.0f, ._44 = 1.0f};
  IDirect3DDevice_SetMatrix(dev, mh, &id);
  D3DEXECUTEBUFFERDESC desc;
  memset(&desc, 0, sizeof desc);
  desc.dwSize = sizeof desc;
  desc.dwFlags = D3DDEB_BUFSIZE;
  desc.dwBufferSize = 1024;
  IDirect3DExecuteBuffer *eb = NULL;
  check(SUCCEEDED(IDirect3DDevice_CreateExecuteBuffer(dev, &desc, &eb, NULL)) && eb, "CreateExecuteBuffer");
  if (eb) {
    memset(&desc, 0, sizeof desc);
    desc.dwSize = sizeof desc;
    check(SUCCEEDED(IDirect3DExecuteBuffer_Lock(eb, &desc)) && desc.lpData && desc.dwBufferSize == 1024, "Lock of the execute buffer");
    BYTE *p = desc.lpData;
    const D3DLVERTEX verts[6] = {
      {{-1.0f}, {1.0f}, {0.5f}, 0, {0xFFFF0000}, {0}, {0}, {0}}, {{1.0f}, {1.0f}, {0.5f}, 0, {0xFFFF0000}, {0}, {0}, {0}}, {{-1.0f}, {-1.0f}, {0.5f}, 0, {0xFFFF0000}, {0}, {0}, {0}},
      {{1.0f}, {1.0f}, {0.5f}, 0, {0xFF0000FF}, {0}, {0}, {0}}, {{1.0f}, {-1.0f}, {0.5f}, 0, {0xFF0000FF}, {0}, {0}, {0}}, {{-3.0f}, {-1.0f}, {0.5f}, 0, {0xFF0000FF}, {0}, {0}, {0}}};
    memcpy(p, verts, sizeof verts);
    BYTE *ins = p + sizeof verts, *at = ins;
    D3DINSTRUCTION in;
    /* STATETRANSFORM world, view, projection */
    in.bOpcode = D3DOP_STATETRANSFORM; in.bSize = sizeof(D3DSTATE); in.wCount = 3;
    memcpy(at, &in, sizeof in); at += sizeof in;
    for (DWORD k = 1; k <= 3; ++k) { D3DSTATE s; s.dtstTransformStateType = (D3DTRANSFORMSTATETYPE)k; s.dwArg[0] = mh; memcpy(at, &s, sizeof s); at += sizeof s; }
    in.bOpcode = D3DOP_STATERENDER; in.bSize = sizeof(D3DSTATE); in.wCount = 1;
    memcpy(at, &in, sizeof in); at += sizeof in;
    { D3DSTATE s; s.drstRenderStateType = D3DRENDERSTATE_CULLMODE; s.dwArg[0] = D3DCULL_NONE; memcpy(at, &s, sizeof s); at += sizeof s; }
    in.bOpcode = D3DOP_PROCESSVERTICES; in.bSize = sizeof(D3DPROCESSVERTICES); in.wCount = 1;
    memcpy(at, &in, sizeof in); at += sizeof in;
    { D3DPROCESSVERTICES pv; memset(&pv, 0, sizeof pv); pv.dwFlags = D3DPROCESSVERTICES_TRANSFORM; pv.dwCount = 6; memcpy(at, &pv, sizeof pv); at += sizeof pv; }
    in.bOpcode = D3DOP_TRIANGLE; in.bSize = sizeof(D3DTRIANGLE); in.wCount = 2;
    memcpy(at, &in, sizeof in); at += sizeof in;
    { D3DTRIANGLE t; memset(&t, 0, sizeof t); t.v1 = 0; t.v2 = 1; t.v3 = 2; t.wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE; memcpy(at, &t, sizeof t); at += sizeof t; }
    { D3DTRIANGLE t; memset(&t, 0, sizeof t); t.v1 = 3; t.v2 = 4; t.v3 = 5; t.wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE; memcpy(at, &t, sizeof t); at += sizeof t; }
    in.bOpcode = D3DOP_EXIT; in.bSize = 0; in.wCount = 0;
    memcpy(at, &in, sizeof in); at += sizeof in;
    check(IDirect3DDevice_Execute(dev, eb, vp, 0) == D3DERR_EXECUTE_LOCKED, "Execute of a locked buffer: D3DERR_EXECUTE_LOCKED");
    IDirect3DExecuteBuffer_Unlock(eb);
    D3DEXECUTEDATA ed;
    memset(&ed, 0, sizeof ed);
    ed.dwSize = sizeof ed;
    ed.dwVertexCount = 6;
    ed.dwInstructionOffset = (DWORD)(ins - p);
    ed.dwInstructionLength = (DWORD)(at - ins);
    check(SUCCEEDED(IDirect3DExecuteBuffer_SetExecuteData(eb, &ed)), "SetExecuteData");
    IDirect3DDevice_BeginScene(dev);
    hr = IDirect3DDevice_Execute(dev, eb, vp, D3DEXECUTE_CLIPPED);
    IDirect3DDevice_EndScene(dev);
    check(hr == D3D_OK, "Execute");
    check(pixel1(rt, 5, 5) == 0xFF0000, "the first triangle (top left, red)");
    check(pixel1(rt, 60, 55) == 0x0000FF, "the clipped triangle (bottom right, blue)");
    memset(&ed, 0, sizeof ed);
    ed.dwSize = sizeof ed;
    IDirect3DExecuteBuffer_GetExecuteData(eb, &ed);
    check((ed.dsStatus.dwStatus & D3DSTATUS_CLIPUNIONLEFT) != 0, "the execute buffer's status has the left plane");
    IDirect3DExecuteBuffer_Release(eb);
  }
  IDirect3DDevice_DeleteMatrix(dev, mh);
  IDirect3DDevice_DeleteViewport(dev, vp);
  IDirect3DViewport_Release(vp);
  IDirect3DDevice_Release(dev);
  check(refs(rt) == before, "the device's reference goes back to the surface");
  IDirectDrawSurface_Release(rt);
  IDirect3D_Release(d3d);
}

int main(void) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "d3dimtest";
  RegisterClassA(&wc);
  g_window = CreateWindowExA(0, "d3dimtest", "d3dimtest", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, NULL, NULL, wc.hInstance, NULL);
  SetForegroundWindow(g_window);
  test_d3d7();
  if (g_dd7) {
    test_flip();
    check(IDirectDraw7_Release(g_dd7) == 0, "the DirectDraw object goes with its last reference");
  }
  test_d3d3();
  IDirectDraw *dd = NULL;
  if (SUCCEEDED(DirectDrawCreate(NULL, &dd, NULL))) {
    IDirectDraw_SetCooperativeLevel(dd, g_window, DDSCL_NORMAL);
    test_d3d2(dd);
    test_d3d1(dd);
    IDirectDraw_Release(dd);
  }
  DestroyWindow(g_window);
  if (g_failed == 0) printf("d3dimtest: all %d checks passed\n", g_passed);
  else printf("d3dimtest: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
