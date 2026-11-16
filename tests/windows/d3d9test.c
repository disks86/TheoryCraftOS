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

/* Direct3D 9 (d3d9.dll: the fixed-function pipeline, a layer on d3d12.dll) on the real kernel: the Direct3D object and what it says of the adapter,
 * a device on a window, clears, vertices that are pixels and vertices that go through the matrices, vertex and index buffers, culling, the
 * primitive types, textures of several formats and the texture stages, blending, the alpha test, depth, state blocks, the program's own vertex and
 * pixel shaders (with each other and with the fixed-function stages), drawing into a texture,
 * Present and Reset. Built for both widths; ends with exit code 42 if every check passed.
 *   x86_64-w64-mingw32-gcc -O1 -o d3d9test.exe tests/windows/d3d9test.c -ld3d9 -luser32 */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d9.h>
#include "d3d9test_shaders.h"

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "d3d9test: ok: %s\n" : "d3d9test: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

#define SIZE 64
static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_device;
static DWORD g_pixels[SIZE * SIZE];

typedef struct { float x, y, z, rhw; DWORD colour; } ScreenVertex;                 /* D3DFVF_XYZRHW | D3DFVF_DIFFUSE */
typedef struct { float x, y, z, rhw; DWORD colour; float u, v; } ScreenTexVertex;   /* ... | D3DFVF_TEX1 */
typedef struct { float x, y, z; DWORD colour; } WorldVertex;                        /* D3DFVF_XYZ | D3DFVF_DIFFUSE */
#define FVF_SCREEN (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define FVF_SCREEN_TEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define FVF_WORLD (D3DFVF_XYZ | D3DFVF_DIFFUSE)

/* The back buffer's pixels (X8R8G8B8: 0x00RRGGBB with whatever is in the top byte). */
static int read_back(void) {
  IDirect3DSurface9 *back = NULL;
  D3DLOCKED_RECT locked;
  int ok = 0;
  if (FAILED(IDirect3DDevice9_GetBackBuffer(g_device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return 0;
  if (SUCCEEDED(IDirect3DSurface9_LockRect(back, &locked, NULL, D3DLOCK_READONLY)) && locked.pBits) {
    for (int y = 0; y < SIZE; ++y) memcpy(g_pixels + y * SIZE, (const BYTE *)locked.pBits + (size_t)y * locked.Pitch, SIZE * 4);
    IDirect3DSurface9_UnlockRect(back);
    ok = 1;
  }
  IDirect3DSurface9_Release(back);
  return ok;
}
static DWORD pixel(int x, int y) { return g_pixels[y * SIZE + x] & 0x00FFFFFF; }
static int red(DWORD p) { return (int)((p >> 16) & 255); }
static int green(DWORD p) { return (int)((p >> 8) & 255); }
static int blue(DWORD p) { return (int)(p & 255); }
static void clear(DWORD colour) { IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, colour, 1.0f, 0); }
/* A rectangle of pixels in one colour, as two triangles of vertices that are pixels. */
static void screen_quad(float x0, float y0, float x1, float y1, float z, DWORD colour) {
  const ScreenVertex v[4] = {{x0, y0, z, 1, colour}, {x1, y0, z, 1, colour}, {x0, y1, z, 1, colour}, {x1, y1, z, 1, colour}};
  IDirect3DDevice9_SetFVF(g_device, FVF_SCREEN);
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
}
static void textured_quad(float x0, float y0, float x1, float y1, DWORD colour) {
  const ScreenTexVertex v[4] = {{x0, y0, 0.5f, 1, colour, 0, 0}, {x1, y0, 0.5f, 1, colour, 1, 0}, {x0, y1, 0.5f, 1, colour, 0, 1}, {x1, y1, 0.5f, 1, colour, 1, 1}};
  IDirect3DDevice9_SetFVF(g_device, FVF_SCREEN_TEX);
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
}

static void test_object(void) {
  g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
  check(g_d3d != NULL, "Direct3DCreate9");
  if (!g_d3d) return;
  D3DADAPTER_IDENTIFIER9 id;
  D3DDISPLAYMODE mode;
  D3DCAPS9 caps;
  memset(&id, 0, sizeof id);
  memset(&mode, 0, sizeof mode);
  memset(&caps, 0, sizeof caps);
  check(IDirect3D9_GetAdapterCount(g_d3d) == 1 && SUCCEEDED(IDirect3D9_GetAdapterIdentifier(g_d3d, 0, 0, &id)) && id.Description[0] != 0, "one adapter, with a description");
  check(SUCCEEDED(IDirect3D9_GetAdapterDisplayMode(g_d3d, 0, &mode)) && mode.Width >= 320 && mode.Height >= 200 && mode.Format == D3DFMT_X8R8G8B8, "the display mode");
  check(IDirect3D9_GetAdapterModeCount(g_d3d, 0, D3DFMT_X8R8G8B8) >= 1 && SUCCEEDED(IDirect3D9_EnumAdapterModes(g_d3d, 0, D3DFMT_X8R8G8B8, 0, &mode)), "the modes of the adapter");
  check(SUCCEEDED(IDirect3D9_GetDeviceCaps(g_d3d, 0, D3DDEVTYPE_HAL, &caps)) && caps.MaxSimultaneousTextures == 2 && caps.MaxTextureWidth >= 2048 &&
        caps.VertexShaderVersion == D3DVS_VERSION(3, 0) && caps.PixelShaderVersion == D3DPS_VERSION(3, 0), "the caps: two texture stages, shaders up to 3.0");
  check(IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8) == D3D_OK &&
        IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT1) == D3DERR_NOTAVAILABLE &&
        IDirect3D9_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8) == D3D_OK,
        "CheckDeviceFormat: a plain format and a depth format are there, a compressed one is not");
}

static HWND g_window;
static int make_device(void) {
  WNDCLASSW cls;
  memset(&cls, 0, sizeof cls);
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(NULL);
  cls.lpszClassName = L"d3d9test";
  RegisterClassW(&cls);
  g_window = CreateWindowExW(0, L"d3d9test", L"d3d9test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 30, 30, 200, 160, NULL, NULL, cls.hInstance, NULL);
  D3DPRESENT_PARAMETERS pp;
  memset(&pp, 0, sizeof pp);
  pp.BackBufferWidth = pp.BackBufferHeight = SIZE;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_window;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  const HRESULT hr = g_window ? IDirect3D9_CreateDevice(g_d3d, 0, D3DDEVTYPE_HAL, g_window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_device) : E_FAIL;
  check(g_window && hr == D3D_OK && g_device, "a window, and CreateDevice on it");
  if (!g_device) return 0;
  IDirect3D9 *parent = NULL;
  D3DVIEWPORT9 viewport;
  DWORD z = 0, cull = 0;
  memset(&viewport, 0, sizeof viewport);
  IDirect3DDevice9_GetDirect3D(g_device, &parent);
  IDirect3DDevice9_GetViewport(g_device, &viewport);
  IDirect3DDevice9_GetRenderState(g_device, D3DRS_ZENABLE, &z);
  IDirect3DDevice9_GetRenderState(g_device, D3DRS_CULLMODE, &cull);
  check(parent == g_d3d && viewport.Width == SIZE && viewport.Height == SIZE && viewport.MaxZ == 1.0f && z == D3DZB_TRUE && cull == D3DCULL_CCW,
        "the device's parent, its first viewport and render states");
  if (parent) IDirect3D9_Release(parent);
  IDirect3DVertexShader9 *shader = NULL;
  static const DWORD nothing[2] = {0xFFFE0101, 0x0000FFFF};
  check(IDirect3DDevice9_CreateVertexShader(g_device, nothing, &shader) == D3DERR_INVALIDCALL && shader == NULL && IDirect3DDevice9_SetVertexShader(g_device, NULL) == D3D_OK,
        "a vertex shader that writes no position cannot be made, and none can be set");
  /* (what the tests below do not want unless they say so: lighting is not there in any case) */
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_NONE);
  return 1;
}

static void test_clear_and_pixels(void) {
  check(IDirect3DDevice9_BeginScene(g_device) == D3D_OK && IDirect3DDevice9_BeginScene(g_device) == D3DERR_INVALIDCALL, "BeginScene, once");
  clear(0xFFFF0000);
  check(read_back() && pixel(0, 0) == 0xFF0000 && pixel(63, 63) == 0xFF0000, "Clear, and the back buffer locked to read it");
  /* vertices that are pixels: a rectangle from (16, 16) to (48, 48) covers exactly the pixels 16 .. 47 */
  clear(0xFF000000);
  screen_quad(16, 16, 48, 48, 0.5f, 0xFF00FF00);
  read_back();
  check(pixel(16, 16) == 0x00FF00 && pixel(47, 47) == 0x00FF00 && pixel(32, 32) == 0x00FF00, "a rectangle of pixel positions is filled");
  check(pixel(15, 16) == 0 && pixel(16, 15) == 0 && pixel(48, 47) == 0 && pixel(47, 48) == 0, "... to its edges and no further (Direct3D 9's pixel centres)");
  /* a triangle with a colour a corner */
  const ScreenVertex triangle[3] = {{32, 4, 0.5f, 1, 0xFFFF0000}, {60, 60, 0.5f, 1, 0xFF00FF00}, {4, 60, 0.5f, 1, 0xFF0000FF}};
  clear(0xFF000000);
  IDirect3DDevice9_SetFVF(g_device, FVF_SCREEN);
  check(IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLELIST, 1, triangle, sizeof triangle[0]) == D3D_OK, "DrawPrimitiveUP");
  read_back();
  check(red(pixel(32, 12)) > 150 && green(pixel(52, 56)) > 150 && blue(pixel(12, 56)) > 150 && pixel(2, 2) == 0, "a triangle with a colour a corner, shaded between them");
  check(IDirect3DDevice9_EndScene(g_device) == D3D_OK, "EndScene");
}

static void test_transforms(void) {
  /* vertices that go through the matrices: a triangle around the origin, then moved to the right by the world matrix */
  const WorldVertex v[3] = {{0.0f, 0.5f, 0.5f, 0xFFFFFFFF}, {0.5f, -0.5f, 0.5f, 0xFFFFFFFF}, {-0.5f, -0.5f, 0.5f, 0xFFFFFFFF}};
  D3DMATRIX world;
  memset(&world, 0, sizeof world);
  world.m[0][0] = world.m[1][1] = world.m[2][2] = world.m[3][3] = 1.0f;
  clear(0xFF000000);
  IDirect3DDevice9_SetFVF(g_device, FVF_WORLD);
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLELIST, 1, v, sizeof v[0]);
  read_back();
  check(pixel(32, 36) == 0xFFFFFF && pixel(6, 36) == 0 && pixel(58, 36) == 0 && pixel(32, 8) == 0, "a triangle through identity matrices is in the middle");
  world.m[3][0] = 0.5f;      /* (the translation is the fourth row) */
  clear(0xFF000000);
  IDirect3DDevice9_SetTransform(g_device, D3DTS_WORLD, &world);
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLELIST, 1, v, sizeof v[0]);
  read_back();
  check(pixel(48, 36) == 0xFFFFFF && pixel(24, 36) == 0, "the world matrix moves it to the right");
  D3DMATRIX got;
  memset(&got, 0, sizeof got);
  IDirect3DDevice9_GetTransform(g_device, D3DTS_WORLD, &got);
  check(got.m[3][0] == 0.5f && got.m[0][0] == 1.0f, "GetTransform gives the matrix back");
  world.m[3][0] = 0.0f;
  IDirect3DDevice9_SetTransform(g_device, D3DTS_WORLD, &world);
}

static void test_buffers_and_culling(void) {
  /* four corners of a square in a vertex buffer, two triangles by an index buffer: both wound clockwise as the screen shows them */
  const WorldVertex corners[4] = {{-0.5f, 0.5f, 0.5f, 0xFF0000FF}, {0.5f, 0.5f, 0.5f, 0xFF0000FF}, {0.5f, -0.5f, 0.5f, 0xFF0000FF}, {-0.5f, -0.5f, 0.5f, 0xFF0000FF}};
  const WORD clockwise[6] = {0, 1, 2, 0, 2, 3}, counter[6] = {0, 2, 1, 0, 3, 2};
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DIndexBuffer9 *ib = NULL;
  void *memory = NULL;
  HRESULT hr = IDirect3DDevice9_CreateVertexBuffer(g_device, sizeof corners, 0, FVF_WORLD, D3DPOOL_MANAGED, &vb, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateIndexBuffer(g_device, sizeof clockwise, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL);
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &memory, 0))) { memcpy(memory, corners, sizeof corners); IDirect3DVertexBuffer9_Unlock(vb); }
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DIndexBuffer9_Lock(ib, 0, 0, &memory, 0))) { memcpy(memory, clockwise, sizeof clockwise); IDirect3DIndexBuffer9_Unlock(ib); }
  D3DVERTEXBUFFER_DESC desc;
  memset(&desc, 0, sizeof desc);
  if (vb) IDirect3DVertexBuffer9_GetDesc(vb, &desc);
  check(SUCCEEDED(hr) && vb && ib && desc.Size == sizeof corners && desc.FVF == FVF_WORLD, "a vertex buffer and an index buffer, filled through Lock");
  if (FAILED(hr)) return;
  IDirect3DDevice9_SetFVF(g_device, FVF_WORLD);
  IDirect3DDevice9_SetStreamSource(g_device, 0, vb, 0, sizeof corners[0]);
  IDirect3DDevice9_SetIndices(g_device, ib);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_CCW);
  clear(0xFF000000);
  check(IDirect3DDevice9_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2) == D3D_OK, "DrawIndexedPrimitive");
  read_back();
  check(pixel(20, 20) == 0x0000FF && pixel(44, 44) == 0x0000FF && pixel(8, 8) == 0, "two clockwise triangles make a square (counter-clockwise ones are what is culled)");
  if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(ib, 0, 0, &memory, 0))) { memcpy(memory, counter, sizeof counter); IDirect3DIndexBuffer9_Unlock(ib); }
  clear(0xFF000000);
  IDirect3DDevice9_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
  read_back();
  check(pixel(32, 32) == 0, "the same square wound the other way is culled");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_CW);
  IDirect3DDevice9_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
  read_back();
  check(pixel(32, 32) == 0x0000FF, "... and drawn when clockwise is what is culled");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_NONE);
  /* the other primitive types, from the vertex buffer: a fan of the four corners, and a strip */
  clear(0xFF000000);
  check(IDirect3DDevice9_DrawPrimitive(g_device, D3DPT_TRIANGLEFAN, 0, 2) == D3D_OK, "DrawPrimitive of a triangle fan");
  read_back();
  check(pixel(20, 20) == 0x0000FF && pixel(44, 44) == 0x0000FF && pixel(20, 44) == 0x0000FF && pixel(44, 20) == 0x0000FF, "the fan of four corners is the square");
  IDirect3DVertexBuffer9 *bound = NULL;
  UINT offset = 9, stride = 0;
  IDirect3DDevice9_GetStreamSource(g_device, 0, &bound, &offset, &stride);
  check(bound == vb && offset == 0 && stride == sizeof corners[0], "GetStreamSource gives back what is set");
  if (bound) IDirect3DVertexBuffer9_Release(bound);
  IDirect3DDevice9_SetStreamSource(g_device, 0, NULL, 0, 0);
  IDirect3DDevice9_SetIndices(g_device, NULL);
  check(IDirect3DIndexBuffer9_Release(ib) == 0 && IDirect3DVertexBuffer9_Release(vb) == 0, "the buffers are gone with their last references");
}

static IDirect3DTexture9 *make_texture(D3DFORMAT format, const void *pixels, UINT bytes_a_pixel) {
  IDirect3DTexture9 *texture = NULL;
  D3DLOCKED_RECT locked;
  if (FAILED(IDirect3DDevice9_CreateTexture(g_device, 2, 2, 1, 0, format, D3DPOOL_MANAGED, &texture, NULL)) || !texture) return NULL;
  if (SUCCEEDED(IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0)) && locked.pBits) {
    for (UINT y = 0; y < 2; ++y) memcpy((BYTE *)locked.pBits + (size_t)y * locked.Pitch, (const BYTE *)pixels + y * 2 * bytes_a_pixel, 2 * bytes_a_pixel);
    IDirect3DTexture9_UnlockRect(texture, 0);
  }
  return texture;
}

static void test_textures(void) {
  /* 2 x 2: red, green / blue, white with no alpha */
  const DWORD texels[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0x00FFFFFF};
  IDirect3DTexture9 *texture = make_texture(D3DFMT_A8R8G8B8, texels, 4);
  D3DSURFACE_DESC desc;
  memset(&desc, 0, sizeof desc);
  if (texture) IDirect3DTexture9_GetLevelDesc(texture, 0, &desc);
  check(texture && desc.Width == 2 && desc.Height == 2 && desc.Format == D3DFMT_A8R8G8B8 && IDirect3DTexture9_GetLevelCount(texture) == 1, "a texture of 2 x 2, filled through LockRect");
  if (!texture) return;
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)texture);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  if (!(pixel(8, 8) == 0xFF0000 && pixel(56, 8) == 0x00FF00 && pixel(8, 56) == 0x0000FF && pixel(56, 56) == 0xFFFFFF))
    printf("d3d9test: (the four corners are %06lx %06lx %06lx %06lx)\n", (unsigned long)pixel(8, 8), (unsigned long)pixel(56, 8), (unsigned long)pixel(8, 56), (unsigned long)pixel(56, 56));
  check(pixel(8, 8) == 0xFF0000 && pixel(56, 8) == 0x00FF00 && pixel(8, 56) == 0x0000FF && pixel(56, 56) == 0xFFFFFF, "the texture's four texels on a rectangle (point sampling)");
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFF808080);
  read_back();
  check(red(pixel(8, 8)) > 120 && red(pixel(8, 8)) < 136 && green(pixel(8, 8)) == 0, "the first stage's default: the texture times the vertex colour");
  /* the stage's operations: the vertex colour alone, then the two added */
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFF808080);
  read_back();
  check(red(pixel(8, 8)) > 120 && red(pixel(8, 8)) < 136 && green(pixel(8, 8)) > 120, "SELECTARG2: the vertex colour alone");
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_ADD);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFF404040);
  read_back();
  check(red(pixel(8, 8)) == 255 && green(pixel(8, 8)) > 56 && green(pixel(8, 8)) < 72, "ADD: the texture plus the vertex colour");
  IDirect3DDevice9_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  /* the alpha test: the white texel has no alpha */
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHATESTENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHAREF, 0x80);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(pixel(8, 8) == 0xFF0000 && pixel(56, 56) == 0, "the alpha test leaves out the texel that has no alpha");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHATESTENABLE, FALSE);
  /* blending by the texture's alpha, over white */
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHABLENDENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(pixel(8, 8) == 0xFF0000 && pixel(56, 56) == 0, "blending by the source's alpha: the texel with none leaves what was there");
  IDirect3DDevice9_SetTexture(g_device, 0, NULL);
  clear(0xFF000000);
  screen_quad(0, 0, 64, 64, 0.5f, 0x80FFFFFF);
  read_back();
  check(red(pixel(32, 32)) > 120 && red(pixel(32, 32)) < 136, "... and half of white over black is grey (no texture: the vertex colour's alpha)");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DBaseTexture9 *bound = (IDirect3DBaseTexture9 *)1;
  IDirect3DDevice9_GetTexture(g_device, 0, &bound);
  check(bound == NULL && IDirect3DTexture9_Release(texture) == 0, "the texture is unset, and gone with its last reference");

  /* formats that are kept as 32 bits: 5-6-5, and alpha alone */
  const WORD packed[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  IDirect3DTexture9 *small = make_texture(D3DFMT_R5G6B5, packed, 2);
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)small);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(small && pixel(8, 8) == 0xFF0000 && pixel(56, 8) == 0x00FF00 && pixel(8, 56) == 0x0000FF && pixel(56, 56) == 0xFFFFFF, "a texture of 16 bits a pixel (R5G6B5)");
  IDirect3DDevice9_SetTexture(g_device, 0, NULL);
  if (small) IDirect3DTexture9_Release(small);
}

static void test_depth(void) {
  clear(0xFF000000);
  screen_quad(8, 8, 56, 56, 0.2f, 0xFFFF0000);
  screen_quad(8, 8, 56, 56, 0.8f, 0xFF00FF00);
  read_back();
  check(pixel(32, 32) == 0xFF0000, "the depth test: the nearer red rectangle stays in front of the green one drawn after it");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZENABLE, D3DZB_FALSE);
  screen_quad(8, 8, 56, 56, 0.8f, 0xFF00FF00);
  read_back();
  check(pixel(32, 32) == 0x00FF00, "without the depth test the green one is drawn over it");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZENABLE, D3DZB_TRUE);
  /* a scissor rectangle */
  const RECT left = {0, 0, 32, SIZE};
  clear(0xFF000000);
  IDirect3DDevice9_SetScissorRect(g_device, &left);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SCISSORTESTENABLE, TRUE);
  screen_quad(0, 0, 64, 64, 0.5f, 0xFF0000FF);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SCISSORTESTENABLE, FALSE);
  read_back();
  check(pixel(8, 32) == 0x0000FF && pixel(56, 32) == 0, "a scissor rectangle keeps the draw to the left half");
}

static void test_state_blocks(void) {
  IDirect3DStateBlock9 *block = NULL;
  DWORD value = 0;
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
  check(IDirect3DDevice9_CreateStateBlock(g_device, D3DSBT_ALL, &block) == D3D_OK && block, "CreateStateBlock");
  if (!block) return;
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SRCBLEND, D3DBLEND_ZERO);
  IDirect3DDevice9_SetFVF(g_device, FVF_WORLD);
  check(IDirect3DStateBlock9_Apply(block) == D3D_OK && SUCCEEDED(IDirect3DDevice9_GetRenderState(g_device, D3DRS_SRCBLEND, &value)) && value == D3DBLEND_DESTCOLOR,
        "applying the block sets a render state back");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SRCBLEND, D3DBLEND_SRCCOLOR);
  IDirect3DStateBlock9_Capture(block);
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SRCBLEND, D3DBLEND_ONE);
  IDirect3DStateBlock9_Apply(block);
  IDirect3DDevice9_GetRenderState(g_device, D3DRS_SRCBLEND, &value);
  check(value == D3DBLEND_SRCCOLOR, "Capture takes what is set now");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_SRCBLEND, D3DBLEND_ONE);
  check(IDirect3DStateBlock9_Release(block) == 0, "the block is gone with its last reference");
}

/* ---- the program's own shaders (compiled ahead from tests/data/d3d9: d3d9test_shaders.h) --------------------------------------------------------------- */

typedef struct { float x, y, z; DWORD colour; float u, v; } ShaderVertex;      /* D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 */
#define FVF_SHADER (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
static int near_colour(DWORD p, int r, int g, int b) {
  const int ok = red(p) >= r - 3 && red(p) <= r + 3 && green(p) >= g - 3 && green(p) <= g + 3 && blue(p) >= b - 3 && blue(p) <= b + 3;
  if (!ok) printf("d3d9test: (a pixel is %06lx, wanted about %02x%02x%02x)\n", (unsigned long)p, r, g, b);
  return ok;
}
/* The whole of clip space as two triangles, texture coordinates from the top left. */
static void shader_quad(DWORD colour) {
  const ShaderVertex v[4] = {{-1, 1, 0.5f, colour, 0, 0}, {1, 1, 0.5f, colour, 1, 0}, {-1, -1, 0.5f, colour, 0, 1}, {1, -1, 0.5f, colour, 1, 1}};
  IDirect3DDevice9_SetFVF(g_device, FVF_SHADER);
  IDirect3DDevice9_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
}
/* vs_basic's constants: the matrix (a column a register) that halves x, and k. */
static void basic_constants(float kx, float ky, float kz, float kw) {
  const float c[5][4] = {{0.5f, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}, {kx, ky, kz, kw}};
  IDirect3DDevice9_SetVertexShaderConstantF(g_device, 0, &c[0][0], 5);
}
static void test_shaders(void) {
  D3DCAPS9 caps;
  memset(&caps, 0, sizeof caps);
  IDirect3DDevice9_GetDeviceCaps(g_device, &caps);
  check(caps.VertexShaderVersion >= D3DVS_VERSION(2, 0) && caps.PixelShaderVersion >= D3DPS_VERSION(2, 0) && caps.MaxVertexShaderConst >= 256, "the caps have vertex and pixel shaders");

  IDirect3DVertexShader9 *vs = NULL, *vs11 = NULL, *vs30 = NULL, *wrong_vs = (IDirect3DVertexShader9 *)1;
  IDirect3DPixelShader9 *ps_math = NULL, *ps_math30 = NULL, *ps_tex = NULL, *wrong_ps = (IDirect3DPixelShader9 *)1;
  check(IDirect3DDevice9_CreateVertexShader(g_device, g_vs_basic_vs_2_0, &vs) == D3D_OK && vs, "CreateVertexShader (vs_2_0)");
  check(IDirect3DDevice9_CreateVertexShader(g_device, g_vs_basic_vs_1_1, &vs11) == D3D_OK && vs11 &&
        IDirect3DDevice9_CreateVertexShader(g_device, g_vs_basic_vs_3_0, &vs30) == D3D_OK && vs30, "... and vs_1_1 and vs_3_0");
  check(IDirect3DDevice9_CreatePixelShader(g_device, g_ps_math_ps_2_0, &ps_math) == D3D_OK && ps_math &&
        IDirect3DDevice9_CreatePixelShader(g_device, g_ps_math_ps_3_0, &ps_math30) == D3D_OK && ps_math30 &&
        IDirect3DDevice9_CreatePixelShader(g_device, g_ps_tex_ps_2_0, &ps_tex) == D3D_OK && ps_tex, "CreatePixelShader (ps_2_0, ps_3_0, one that samples)");
  if (!vs || !vs11 || !vs30 || !ps_math || !ps_math30 || !ps_tex) return;
  static const DWORD ps11[] = {0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000001, 0x800F0000, 0xB0E40000, 0x0000FFFF};      /* ps_1_1: tex t0; mov r0, t0 */
  check(FAILED(IDirect3DDevice9_CreateVertexShader(g_device, g_ps_math_ps_2_0, &wrong_vs)) && wrong_vs == NULL &&
        FAILED(IDirect3DDevice9_CreatePixelShader(g_device, g_vs_basic_vs_2_0, &wrong_ps)) && wrong_ps == NULL &&
        FAILED(IDirect3DDevice9_CreatePixelShader(g_device, ps11, &wrong_ps)) && FAILED(IDirect3DDevice9_CreateVertexShader(g_device, NULL, &wrong_vs)),
        "a shader of the other stage, a pixel shader 1.1 and no shader at all are refused");
  DWORD code[256];
  UINT size = 0;
  IDirect3DDevice9 *owner = NULL;
  check(IDirect3DVertexShader9_GetFunction(vs, NULL, &size) == D3D_OK && size == sizeof g_vs_basic_vs_2_0 && size <= sizeof code &&
        IDirect3DVertexShader9_GetFunction(vs, code, &size) == D3D_OK && memcmp(code, g_vs_basic_vs_2_0, size) == 0 &&
        IDirect3DVertexShader9_GetDevice(vs, &owner) == D3D_OK && owner == g_device, "GetFunction gives the shader's tokens back, GetDevice the device");
  if (owner) IDirect3DDevice9_Release(owner);

  /* constants are kept whatever is set */
  const float k[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
  float got[2][4];
  const BOOL flags[2] = {TRUE, FALSE};
  BOOL got_flags[2] = {FALSE, TRUE};
  const int counts[4] = {3, 0, 1, 0};
  int got_counts[4] = {0, 0, 0, 0};
  memset(got, 0, sizeof got);
  check(IDirect3DDevice9_SetVertexShaderConstantF(g_device, 254, &k[0][0], 2) == D3D_OK && IDirect3DDevice9_GetVertexShaderConstantF(g_device, 254, &got[0][0], 2) == D3D_OK &&
        memcmp(k, got, sizeof k) == 0 && FAILED(IDirect3DDevice9_SetVertexShaderConstantF(g_device, 255, &k[0][0], 2)), "the vertex shader's 256 float constants");
  memset(got, 0, sizeof got);
  check(IDirect3DDevice9_SetPixelShaderConstantF(g_device, 30, &k[0][0], 2) == D3D_OK && IDirect3DDevice9_GetPixelShaderConstantF(g_device, 30, &got[0][0], 2) == D3D_OK &&
        memcmp(k, got, sizeof k) == 0, "the pixel shader's float constants");
  check(IDirect3DDevice9_SetVertexShaderConstantB(g_device, 3, flags, 2) == D3D_OK && IDirect3DDevice9_GetVertexShaderConstantB(g_device, 3, got_flags, 2) == D3D_OK &&
        got_flags[0] && !got_flags[1] && IDirect3DDevice9_SetPixelShaderConstantI(g_device, 1, counts, 1) == D3D_OK &&
        IDirect3DDevice9_GetPixelShaderConstantI(g_device, 1, got_counts, 1) == D3D_OK && got_counts[0] == 3 && got_counts[2] == 1, "bool and integer constants");

  IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_NONE);
  /* the vertex shader with the fixed-function pixel stage: x halved, the colour scaled and biased */
  IDirect3DVertexShader9 *set = NULL;
  check(IDirect3DDevice9_SetVertexShader(g_device, vs) == D3D_OK && IDirect3DDevice9_GetVertexShader(g_device, &set) == D3D_OK && set == vs, "SetVertexShader, GetVertexShader");
  if (set) IDirect3DVertexShader9_Release(set);
  basic_constants(1.0f, 0.5f, 0.0f, 0.25f);
  clear(0xFF000000);
  shader_quad(0xFF808080);
  read_back();
  check(near_colour(pixel(32, 32), 191, 128, 64) && pixel(6, 32) == 0 && pixel(58, 32) == 0 && near_colour(pixel(20, 4), 191, 128, 64) && near_colour(pixel(44, 60), 191, 128, 64),
        "a vertex shader moves the vertices and works out their colour (the pixels by the fixed-function stage)");
  IDirect3DDevice9_SetVertexShader(g_device, vs11);
  clear(0xFF000000);
  shader_quad(0xFF808080);
  read_back();
  check(near_colour(pixel(32, 32), 191, 128, 64) && pixel(6, 32) == 0, "... and the same shader as vs_1_1");

  /* the same from a vertex buffer through a declaration, drawn by indices */
  static const D3DVERTEXELEMENT9 elements[4] = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0}, {0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
                                                {0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
  const ShaderVertex corners[4] = {{-1, 1, 0.5f, 0xFFFFFFFF, 0, 0}, {1, 1, 0.5f, 0xFFFFFFFF, 1, 0}, {-1, -1, 0.5f, 0xFFFFFFFF, 0, 1}, {1, -1, 0.5f, 0xFFFFFFFF, 1, 1}};
  const WORD order[6] = {0, 1, 2, 2, 1, 3};
  IDirect3DVertexDeclaration9 *declaration = NULL;
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DIndexBuffer9 *ib = NULL;
  void *memory = NULL;
  IDirect3DDevice9_CreateVertexDeclaration(g_device, elements, &declaration);
  IDirect3DDevice9_CreateVertexBuffer(g_device, sizeof corners, 0, 0, D3DPOOL_MANAGED, &vb, NULL);
  IDirect3DDevice9_CreateIndexBuffer(g_device, sizeof order, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL);
  if (vb && SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &memory, 0))) { memcpy(memory, corners, sizeof corners); IDirect3DVertexBuffer9_Unlock(vb); }
  if (ib && SUCCEEDED(IDirect3DIndexBuffer9_Lock(ib, 0, 0, &memory, 0))) { memcpy(memory, order, sizeof order); IDirect3DIndexBuffer9_Unlock(ib); }
  if (declaration && vb && ib) {
    /* ... with a pixel shader: its constants follow the vertex shader's in what a draw is given, and it takes one branch or the other by one */
    const float one[4] = {1, 0, 0, 0}, none[4] = {0, 0, 0, 0}, weights[4] = {0, 0, 0, 0};
    IDirect3DDevice9_SetVertexShader(g_device, vs);
    IDirect3DDevice9_SetPixelShader(g_device, (IDirect3DPixelShader9 *)ps_math);
    IDirect3DDevice9_SetVertexDeclaration(g_device, declaration);
    IDirect3DDevice9_SetStreamSource(g_device, 0, vb, 0, sizeof corners[0]);
    IDirect3DDevice9_SetIndices(g_device, ib);
    basic_constants(0.5f, 0.5f, 0.0f, 0.25f);      /* (white becomes 0.75 0.75 0.25) */
    IDirect3DDevice9_SetPixelShaderConstantF(g_device, 0, one, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(g_device, 1, weights, 1);
    clear(0xFF000000);
    const HRESULT drawn = IDirect3DDevice9_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    read_back();
    check(drawn == D3D_OK && near_colour(pixel(32, 32), 255, 255, 128) && pixel(6, 32) == 0, "a vertex and a pixel shader, from a vertex buffer through a declaration: twice the colour");
    IDirect3DDevice9_SetPixelShaderConstantF(g_device, 0, none, 1);
    clear(0xFF000000);
    IDirect3DDevice9_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    read_back();
    check(near_colour(pixel(32, 32), 64, 191, 191), "a constant of the pixel shader's takes its other branch: red and blue change places");
    /* Shader Model 3: outputs and inputs by what they are */
    IDirect3DDevice9_SetVertexShader(g_device, vs30);
    IDirect3DDevice9_SetPixelShader(g_device, ps_math30);
    clear(0xFF000000);
    IDirect3DDevice9_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    read_back();
    check(near_colour(pixel(32, 32), 64, 191, 191) && pixel(6, 32) == 0, "the same two as vs_3_0 and ps_3_0");
  } else check(0, "a declaration, a vertex buffer and an index buffer for the shaders");

  /* a pixel shader that samples: the vertex shader doubles the texture coordinates, so the 2 x 2 texture is there four times */
  const DWORD texels[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0x00FFFFFF};
  const float tint[4] = {1, 1, 1, 1}, half[4] = {0.5f, 1, 1, 1};
  IDirect3DTexture9 *texture = make_texture(D3DFMT_A8R8G8B8, texels, 4);
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)texture);
  IDirect3DDevice9_SetVertexShader(g_device, vs);
  IDirect3DDevice9_SetPixelShader(g_device, ps_tex);
  const float all[5][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};      /* (the colour stays white; the coordinates are doubled and moved by one) */
  IDirect3DDevice9_SetVertexShaderConstantF(g_device, 0, &all[0][0], 5);
  IDirect3DDevice9_SetPixelShaderConstantF(g_device, 0, tint, 1);
  clear(0xFF202020);
  shader_quad(0xFFFFFFFF);
  read_back();
  check(texture && pixel(8, 8) == 0xFF0000 && pixel(24, 8) == 0x00FF00 && pixel(40, 8) == 0xFF0000 && pixel(8, 24) == 0x0000FF && pixel(40, 56) == 0x0000FF,
        "a pixel shader samples the texture, at coordinates the vertex shader made");
  check(pixel(24, 24) == 0x202020 && pixel(56, 56) == 0x202020, "... and leaves a pixel out (texkill)");
  /* the fixed-function vertex stage with the pixel shader: vertices that are pixels already */
  IDirect3DDevice9_SetPixelShaderConstantF(g_device, 0, half, 1);
  clear(0xFF202020);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);      /* (XYZRHW: the vertex shader that is set is passed by) */
  read_back();
  check(near_colour(pixel(8, 8), 128, 0, 0) && near_colour(pixel(56, 8), 0, 255, 0) && near_colour(pixel(8, 56), 0, 0, 255) && pixel(56, 56) == 0x202020,
        "a pixel shader after the fixed-function vertex stage, with its constant as the tint");

  /* a state block keeps the shaders and their constants */
  IDirect3DStateBlock9 *block = NULL;
  IDirect3DPixelShader9 *now = NULL;
  float tint_now[4] = {0, 0, 0, 0};
  IDirect3DDevice9_CreateStateBlock(g_device, D3DSBT_ALL, &block);
  IDirect3DDevice9_SetPixelShader(g_device, NULL);
  IDirect3DDevice9_SetPixelShaderConstantF(g_device, 0, tint, 1);
  if (block) IDirect3DStateBlock9_Apply(block);
  IDirect3DDevice9_GetPixelShader(g_device, &now);
  IDirect3DDevice9_GetPixelShaderConstantF(g_device, 0, tint_now, 1);
  check(block && now == ps_tex && tint_now[0] == 0.5f, "a state block sets the pixel shader and its constants back");
  if (now) IDirect3DPixelShader9_Release(now);
  if (block) IDirect3DStateBlock9_Release(block);

  /* back to the fixed-function pipeline: it draws as before */
  IDirect3DDevice9_SetVertexShader(g_device, NULL);
  IDirect3DDevice9_SetPixelShader(g_device, NULL);
  IDirect3DDevice9_SetTexture(g_device, 0, NULL);
  IDirect3DDevice9_SetStreamSource(g_device, 0, NULL, 0, 0);
  IDirect3DDevice9_SetIndices(g_device, NULL);
  IDirect3DDevice9_SetVertexDeclaration(g_device, NULL);
  clear(0xFF000000);
  screen_quad(0, 0, 64, 64, 0.5f, 0xFF00FF00);
  read_back();
  check(pixel(32, 32) == 0x00FF00, "with no shaders set the fixed-function pipeline draws again");
  IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_CCW);
  int gone = 1;
  if (declaration) gone = gone && IDirect3DVertexDeclaration9_Release(declaration) == 0;
  if (vb) gone = gone && IDirect3DVertexBuffer9_Release(vb) == 0;
  if (ib) gone = gone && IDirect3DIndexBuffer9_Release(ib) == 0;
  if (texture) gone = gone && IDirect3DTexture9_Release(texture) == 0;
  gone = gone && IDirect3DVertexShader9_Release(vs) == 0 && IDirect3DVertexShader9_Release(vs11) == 0 && IDirect3DVertexShader9_Release(vs30) == 0;
  gone = gone && IDirect3DPixelShader9_Release(ps_math) == 0 && IDirect3DPixelShader9_Release(ps_math30) == 0 && IDirect3DPixelShader9_Release(ps_tex) == 0;
  check(gone, "the shaders and what they drew from are gone with their last references");
}

static void test_render_to_texture(void) {
  IDirect3DTexture9 *target = NULL;
  IDirect3DSurface9 *surface = NULL, *back = NULL, *depth = NULL;
  HRESULT hr = IDirect3DDevice9_CreateTexture(g_device, 16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target, NULL);
  if (SUCCEEDED(hr)) hr = IDirect3DTexture9_GetSurfaceLevel(target, 0, &surface);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTarget(g_device, 0, &back);
  if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetDepthStencilSurface(g_device, &depth);
  check(SUCCEEDED(hr) && target && surface && back && depth, "a texture to draw into, its surface, and the device's own target and depth buffer");
  if (FAILED(hr)) return;
  /* into the texture: all blue, with a red rectangle in its left half; no depth buffer (it has another size) */
  IDirect3DDevice9_SetDepthStencilSurface(g_device, NULL);
  check(IDirect3DDevice9_SetRenderTarget(g_device, 0, surface) == D3D_OK, "SetRenderTarget");
  D3DVIEWPORT9 viewport;
  memset(&viewport, 0, sizeof viewport);
  IDirect3DDevice9_GetViewport(g_device, &viewport);
  check(viewport.Width == 16 && viewport.Height == 16, "the viewport is the new target's size");
  IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0);
  screen_quad(0, 0, 8, 16, 0.5f, 0xFFFF0000);
  /* back to the back buffer, with the texture on a rectangle */
  IDirect3DDevice9_SetRenderTarget(g_device, 0, back);
  IDirect3DDevice9_SetDepthStencilSurface(g_device, depth);
  IDirect3DDevice9_SetTexture(g_device, 0, (IDirect3DBaseTexture9 *)target);
  clear(0xFF000000);
  textured_quad(0, 0, 64, 64, 0xFFFFFFFF);
  read_back();
  check(pixel(8, 32) == 0xFF0000 && pixel(56, 32) == 0x0000FF, "what was drawn into the texture is sampled from it");
  /* the texture's pixels fetched into a surface in memory */
  IDirect3DSurface9 *memory = NULL;
  D3DLOCKED_RECT locked;
  DWORD first = 0, last = 0;
  if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_device, 16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &memory, NULL)) && memory &&
      SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_device, surface, memory)) && SUCCEEDED(IDirect3DSurface9_LockRect(memory, &locked, NULL, D3DLOCK_READONLY))) {
    first = *(const DWORD *)locked.pBits;
    last = ((const DWORD *)((const BYTE *)locked.pBits + 15 * locked.Pitch))[15];
    IDirect3DSurface9_UnlockRect(memory);
  }
  check(first == 0xFFFF0000 && last == 0xFF0000FF, "GetRenderTargetData into a surface in memory");
  if (memory) IDirect3DSurface9_Release(memory);
  IDirect3DDevice9_SetTexture(g_device, 0, NULL);
  IDirect3DSurface9_Release(depth);
  IDirect3DSurface9_Release(back);
  IDirect3DSurface9_Release(surface);
  IDirect3DTexture9_Release(target);
}

static void test_present_and_reset(void) {
  clear(0xFF2040C0);
  check(IDirect3DDevice9_Present(g_device, NULL, NULL, NULL, NULL) == D3D_OK, "Present");
  D3DPRESENT_PARAMETERS pp;
  memset(&pp, 0, sizeof pp);
  pp.BackBufferWidth = 96;
  pp.BackBufferHeight = 48;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_window;
  pp.Windowed = TRUE;
  IDirect3DSurface9 *back = NULL;
  D3DSURFACE_DESC desc;
  DWORD z = 9;
  memset(&desc, 0, sizeof desc);
  const HRESULT hr = IDirect3DDevice9_Reset(g_device, &pp);
  if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DDevice9_GetBackBuffer(g_device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) IDirect3DSurface9_GetDesc(back, &desc);
  IDirect3DDevice9_GetRenderState(g_device, D3DRS_ZENABLE, &z);
  check(hr == D3D_OK && desc.Width == 96 && desc.Height == 48 && z == D3DZB_FALSE, "Reset: a back buffer of the new size, the states as a new device has them (no depth buffer now)");
  if (back) IDirect3DSurface9_Release(back);
  check(IDirect3DDevice9_GetDepthStencilSurface(g_device, &back) == D3DERR_NOTFOUND, "... and no depth buffer to get");
}

int main(void) {
  test_object();
  if (g_d3d && make_device()) {
    test_clear_and_pixels();
    test_transforms();
    test_buffers_and_culling();
    test_textures();
    test_depth();
    test_state_blocks();
    test_shaders();
    test_render_to_texture();
    test_present_and_reset();
    check(IDirect3DDevice9_Release(g_device) == 0, "the device is gone with its last reference");
  }
  if (g_d3d) IDirect3D9_Release(g_d3d);
  if (g_window) DestroyWindow(g_window);
  if (g_failed == 0) printf("d3d9test: all %d checks passed\n", g_passed);
  else printf("d3d9test: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
