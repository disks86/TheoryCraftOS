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

/* Direct3D 8 (d3d8.dll, a layer on d3d9.dll) on the real kernel: the Direct3D object and its caps, a device on a window with Direct3D 8's
 * presentation parameters, the references and identities of Direct3D 8 (implicit surfaces at 0, levels with their texture's references, the same
 * object from every Get), private data, draws read back by pixel - FVF vertices, vertex and index buffers with SetIndices' base vertex index, a
 * vertex shader of D3DVSD tokens with constants its declaration loads, pixel shaders 1.0 and 1.1, a texture through stage and sampler states -,
 * render states Direct3D 9 has not, state blocks by handle, CopyRects, the sizes in the descriptions, Reset, ValidateVertexShader and
 * ValidatePixelShader. Built for both widths; ends with exit code 42 if every check passed.
 *   i686-w64-mingw32-gcc -O1 -o d3d8test.exe tests/windows/d3d8test.c -ld3d8 -luser32
 *   (x86-64: mingw-w64 has no libd3d8.a for it - link against the built d3d8.dll) */
#define COBJMACROS
#define INITGUID      /* (the interfaces' identifiers are defined here: mingw-w64 has no library of them for Direct3D 8) */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d8.h>

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "d3d8test: ok: %s\n" : "d3d8test: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

#define SIZE 64
static IDirect3D8 *g_d3d;
static IDirect3DDevice8 *g_device;
static HWND g_window;
static DWORD g_pixels[SIZE * SIZE];

typedef struct { float x, y, z, rhw; DWORD colour; } ScreenVertex;
typedef struct { float x, y, z, rhw; DWORD colour; float u, v; } ScreenTexVertex;
typedef struct { float x, y, z; DWORD colour; } ClipVertex;      /* (positions a vertex shader passes on as they are) */
#define FVF_SCREEN (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define FVF_SCREEN_TEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static ULONG refs(void *object) {
  IUnknown *u = object;
  IUnknown_AddRef(u);
  return IUnknown_Release(u);
}
static int read_back(void) {
  IDirect3DSurface8 *back = NULL;
  D3DLOCKED_RECT locked;
  int ok = 0;
  if (FAILED(IDirect3DDevice8_GetBackBuffer(g_device, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return 0;
  if (SUCCEEDED(IDirect3DSurface8_LockRect(back, &locked, NULL, D3DLOCK_READONLY)) && locked.pBits) {
    for (int y = 0; y < SIZE; ++y) memcpy(g_pixels + y * SIZE, (const BYTE *)locked.pBits + (size_t)y * locked.Pitch, SIZE * 4);
    IDirect3DSurface8_UnlockRect(back);
    ok = 1;
  }
  IDirect3DSurface8_Release(back);
  return ok;
}
static DWORD pixel(int x, int y) { return g_pixels[y * SIZE + x] & 0x00FFFFFF; }
static int near_colour(DWORD p, int r, int g, int b) {
  const int dr = (int)((p >> 16) & 255) - r, dg = (int)((p >> 8) & 255) - g, db = (int)(p & 255) - b;
  return dr >= -3 && dr <= 3 && dg >= -3 && dg <= 3 && db >= -3 && db <= 3;
}
static void clear(DWORD colour) { IDirect3DDevice8_Clear(g_device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, colour, 1.0f, 0); }
static void screen_quad(float x0, float y0, float x1, float y1, DWORD colour) {
  const ScreenVertex v[4] = {{x0, y0, 0.5f, 1, colour}, {x1, y0, 0.5f, 1, colour}, {x0, y1, 0.5f, 1, colour}, {x1, y1, 0.5f, 1, colour}};
  IDirect3DDevice8_SetVertexShader(g_device, FVF_SCREEN);
  IDirect3DDevice8_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
}

static void test_object(void) {
  g_d3d = Direct3DCreate8(D3D_SDK_VERSION);
  check(g_d3d != NULL, "Direct3DCreate8");
  if (!g_d3d) return;
  D3DADAPTER_IDENTIFIER8 id;
  D3DDISPLAYMODE mode, first;
  D3DCAPS8 caps;
  memset(&id, 0, sizeof id);
  memset(&caps, 0, sizeof caps);
  check(IDirect3D8_GetAdapterCount(g_d3d) == 1 && SUCCEEDED(IDirect3D8_GetAdapterIdentifier(g_d3d, 0, D3DENUM_NO_WHQL_LEVEL, &id)) && id.Description[0] != 0 &&
        IDirect3D8_GetAdapterIdentifier(g_d3d, 0, 0x100, &id) == D3DERR_INVALIDCALL, "one adapter with a description; an unknown flag is refused");
  const UINT modes = IDirect3D8_GetAdapterModeCount(g_d3d, 0);
  check(modes >= 1 && SUCCEEDED(IDirect3D8_EnumAdapterModes(g_d3d, 0, 0, &first)) && first.Width >= 320 && first.Format == D3DFMT_X8R8G8B8 &&
        IDirect3D8_EnumAdapterModes(g_d3d, 0, modes, &mode) == D3DERR_INVALIDCALL && SUCCEEDED(IDirect3D8_GetAdapterDisplayMode(g_d3d, 0, &mode)),
        "the modes of every format, one list (EnumAdapterModes has no format in Direct3D 8)");
  check(SUCCEEDED(IDirect3D8_GetDeviceCaps(g_d3d, 0, D3DDEVTYPE_HAL, &caps)) && caps.VertexShaderVersion == D3DVS_VERSION(1, 1) && caps.PixelShaderVersion == D3DPS_VERSION(1, 4) &&
        caps.MaxVertexShaderConst >= 96 && caps.MaxTextureBlendStages == 8 && (caps.Caps2 & D3DCAPS2_CANRENDERWINDOWED) && (caps.RasterCaps & D3DPRASTERCAPS_ZBIAS),
        "the caps of Direct3D 8: vertex shaders 1.1, pixel shaders 1.4, eight stages, a depth bias of D3DRS_ZBIAS");
  check(IDirect3D8_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8) == D3D_OK &&
        IDirect3D8_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8) == D3D_OK &&
        IDirect3D8_CheckDeviceType(g_d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE) == D3D_OK, "CheckDeviceFormat, CheckDeviceType");
}

static void present_parameters(D3DPRESENT_PARAMETERS *pp, UINT size) {
  memset(pp, 0, sizeof *pp);
  pp->BackBufferWidth = pp->BackBufferHeight = size;
  pp->BackBufferFormat = D3DFMT_X8R8G8B8;
  pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp->hDeviceWindow = g_window;
  pp->Windowed = TRUE;
  pp->EnableAutoDepthStencil = TRUE;
  pp->Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;      /* (read_back locks the back buffer) */
  pp->AutoDepthStencilFormat = D3DFMT_D24S8;
}
static int make_device(void) {
  WNDCLASSW cls;
  memset(&cls, 0, sizeof cls);
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(NULL);
  cls.lpszClassName = L"d3d8test";
  RegisterClassW(&cls);
  g_window = CreateWindowExW(0, L"d3d8test", L"d3d8test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 30, 30, 200, 160, NULL, NULL, cls.hInstance, NULL);
  D3DPRESENT_PARAMETERS pp;
  IDirect3DDevice8 *bad = (IDirect3DDevice8 *)1;
  present_parameters(&pp, SIZE);
  pp.SwapEffect = 0;
  const HRESULT no_effect = IDirect3D8_CreateDevice(g_d3d, 0, D3DDEVTYPE_HAL, g_window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &bad);
  present_parameters(&pp, SIZE);
  pp.SwapEffect = D3DSWAPEFFECT_COPY;
  pp.BackBufferCount = 2;
  const HRESULT copy_two = IDirect3D8_CreateDevice(g_d3d, 0, D3DDEVTYPE_HAL, g_window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &bad);
  check(no_effect == D3DERR_INVALIDCALL && copy_two == D3DERR_INVALIDCALL && bad == NULL, "presentation parameters Direct3D 8 refuses: no swap effect, two back buffers to copy");
  present_parameters(&pp, SIZE);
  pp.SwapEffect = D3DSWAPEFFECT_COPY_VSYNC;
  const HRESULT hr = g_window ? IDirect3D8_CreateDevice(g_d3d, 0, D3DDEVTYPE_HAL, g_window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_device) : E_FAIL;
  check(g_window && hr == D3D_OK && g_device && pp.BackBufferCount == 1, "a window, and CreateDevice on it (D3DSWAPEFFECT_COPY_VSYNC)");
  if (!g_device) return 0;
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_CULLMODE, D3DCULL_NONE);
  return 1;
}

static void test_references(void) {
  IDirect3D8 *parent = NULL;
  check(refs(g_device) == 1 && refs(g_d3d) == 2 && SUCCEEDED(IDirect3DDevice8_GetDirect3D(g_device, &parent)) && parent == g_d3d && refs(g_d3d) == 3,
        "the device holds the Direct3D object, GetDirect3D gives the program's");
  if (parent) IDirect3D8_Release(parent);
  /* The back buffer: the render target, at 0 references of its own, the device's while it has some, never freed before the device. */
  IDirect3DSurface8 *rt = NULL, *back = NULL, *depth = NULL;
  IDirect3DDevice8 *owner = NULL;
  IDirect3DDevice8_GetRenderTarget(g_device, &rt);
  IDirect3DDevice8_GetBackBuffer(g_device, 0, D3DBACKBUFFER_TYPE_MONO, &back);
  check(rt && rt == back && refs(rt) == 2 && refs(g_device) == 2, "GetRenderTarget and GetBackBuffer give the same surface; it holds the device");
  if (rt) {
    IDirect3DSurface8_Release(back);
    check(IDirect3DSurface8_Release(rt) == 0 && refs(g_device) == 1 && IDirect3DSurface8_Release(rt) == 0, "... at 0 references it lets the device go and stays");
    check(IDirect3DSurface8_AddRef(rt) == 1 && refs(g_device) == 2 && SUCCEEDED(IDirect3DSurface8_GetContainer(rt, &IID_IDirect3DDevice8, (void **)&owner)) && owner == g_device,
          "... and can be taken again; its container is the device");
    if (owner) IDirect3DDevice8_Release(owner);
    IDirect3DSurface8_Release(rt);
  }
  check(SUCCEEDED(IDirect3DDevice8_GetDepthStencilSurface(g_device, &depth)) && depth && refs(depth) == 1, "the depth buffer made with the device");
  if (depth) IDirect3DSurface8_Release(depth);

  /* A level of a texture: the same object each time, with the texture's references; its container is the texture. */
  IDirect3DTexture8 *texture = NULL;
  IDirect3DSurface8 *level = NULL, *again = NULL;
  IDirect3DBaseTexture8 *base = NULL, *got = NULL;
  IUnknown *container = NULL;
  const ULONG device_refs = refs(g_device);
  check(SUCCEEDED(IDirect3DDevice8_CreateTexture(g_device, 16, 16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture)) && texture && refs(g_device) == device_refs + 1,
        "a texture holds its device");
  if (!texture) return;
  IDirect3DTexture8_GetSurfaceLevel(texture, 1, &level);
  IDirect3DTexture8_GetSurfaceLevel(texture, 1, &again);
  check(level && level == again && refs(texture) == 3 && refs(level) == 3 && refs(g_device) == device_refs + 1, "a level is one object with its texture's references");
  check(level && SUCCEEDED(IDirect3DSurface8_GetContainer(level, &IID_IDirect3DTexture8, (void **)&container)) && container == (IUnknown *)texture &&
        IDirect3DSurface8_GetContainer(level, &IID_IDirect3DSurface8, (void **)&owner) == E_NOINTERFACE, "the level's container is its texture");
  if (container) IUnknown_Release(container);
  if (again) IDirect3DSurface8_Release(again);
  check(SUCCEEDED(IDirect3DTexture8_QueryInterface(texture, &IID_IDirect3DBaseTexture8, (void **)&base)) && base == (IDirect3DBaseTexture8 *)texture &&
        IDirect3DTexture8_QueryInterface(texture, &IID_IDirect3DVolumeTexture8, (void **)&got) == E_NOINTERFACE, "QueryInterface of a texture");
  if (base) IDirect3DBaseTexture8_Release(base);
  const ULONG before = refs(texture);
  IDirect3DDevice8_SetTexture(g_device, 0, (IDirect3DBaseTexture8 *)texture);
  check(refs(texture) == before && SUCCEEDED(IDirect3DDevice8_GetTexture(g_device, 0, &got)) && got == (IDirect3DBaseTexture8 *)texture && refs(texture) == before + 1,
        "SetTexture takes no reference of the program's; GetTexture gives the same object");
  if (got) IDirect3DBaseTexture8_Release(got);
  IDirect3DDevice8_SetTexture(g_device, 0, NULL);
  /* Private data: bytes, an interface it holds, sizes. */
  static const GUID tag = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
  DWORD data[2] = {7, 9}, out[2] = {0, 0}, size = 4;
  check(IDirect3DSurface8_SetPrivateData(level, &tag, data, sizeof data, 0) == D3D_OK && IDirect3DSurface8_GetPrivateData(level, &tag, out, &size) == D3DERR_MOREDATA &&
        size == 8 && IDirect3DSurface8_GetPrivateData(level, &tag, out, &size) == D3D_OK && out[0] == 7 && out[1] == 9 &&
        IDirect3DTexture8_GetPrivateData(texture, &tag, out, &size) == D3DERR_NOTFOUND, "private data of a level is its own");
  const ULONG with = refs(g_device);
  check(IDirect3DSurface8_SetPrivateData(level, &tag, g_device, sizeof(IUnknown *), D3DSPD_IUNKNOWN) == D3D_OK && refs(g_device) == with + 1 &&
        IDirect3DSurface8_FreePrivateData(level, &tag) == D3D_OK && refs(g_device) == with && IDirect3DSurface8_FreePrivateData(level, &tag) == D3DERR_NOTFOUND,
        "private data that is an interface holds it while it is kept");
  IDirect3DSurface8_Release(level);
  check(IDirect3DTexture8_Release(texture) == 0 && refs(g_device) == device_refs, "the texture goes with its last reference, and lets the device go");

  /* Buffers: GetStreamSource and GetIndices give the program's objects; a stream set to none keeps its stride. */
  IDirect3DVertexBuffer8 *vb = NULL, *vb_got = NULL;
  IDirect3DIndexBuffer8 *ib = NULL, *ib_got = NULL;
  UINT stride = 0, base_vertex = 0;
  IDirect3DDevice8_CreateVertexBuffer(g_device, 64, 0, 0, D3DPOOL_MANAGED, &vb);
  IDirect3DDevice8_CreateIndexBuffer(g_device, 16, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
  if (vb && ib) {
    IDirect3DDevice8_SetStreamSource(g_device, 1, vb, 12);
    IDirect3DDevice8_SetIndices(g_device, ib, 5);
    check(SUCCEEDED(IDirect3DDevice8_GetStreamSource(g_device, 1, &vb_got, &stride)) && vb_got == vb && stride == 12 &&
          SUCCEEDED(IDirect3DDevice8_GetIndices(g_device, &ib_got, &base_vertex)) && ib_got == ib && base_vertex == 5, "GetStreamSource, GetIndices (with the base vertex index)");
    if (vb_got) IDirect3DVertexBuffer8_Release(vb_got);
    if (ib_got) IDirect3DIndexBuffer8_Release(ib_got);
    IDirect3DDevice8_SetStreamSource(g_device, 1, NULL, 0);
    vb_got = (IDirect3DVertexBuffer8 *)1;
    check(SUCCEEDED(IDirect3DDevice8_GetStreamSource(g_device, 1, &vb_got, &stride)) && vb_got == NULL && stride == 12, "a stream set to no buffer keeps its stride");
    IDirect3DDevice8_SetIndices(g_device, NULL, 0);
  } else {
    check(0, "CreateVertexBuffer, CreateIndexBuffer");
  }
  if (vb) IDirect3DVertexBuffer8_Release(vb);
  if (ib) IDirect3DIndexBuffer8_Release(ib);
}

static void test_draws(void) {
  clear(0xFF000000);
  screen_quad(0, 0, 32, 32, 0xFFFF0000);
  read_back();
  check(pixel(4, 4) == 0xFF0000 && pixel(40, 40) == 0, "a quad of XYZRHW vertices by an FVF handle");
  DWORD handle = 0;
  check(SUCCEEDED(IDirect3DDevice8_GetVertexShader(g_device, &handle)) && handle == FVF_SCREEN, "GetVertexShader gives the FVF code");

  /* Eight vertices in a buffer, four indices 0 .. 3; with a base vertex index of 4 the second quad is drawn. */
  const ScreenVertex v[8] = {{0, 0, 0.5f, 1, 0xFFFF0000}, {32, 0, 0.5f, 1, 0xFFFF0000}, {0, 32, 0.5f, 1, 0xFFFF0000}, {32, 32, 0.5f, 1, 0xFFFF0000},
                             {32, 32, 0.5f, 1, 0xFF00FF00}, {64, 32, 0.5f, 1, 0xFF00FF00}, {32, 64, 0.5f, 1, 0xFF00FF00}, {64, 64, 0.5f, 1, 0xFF00FF00}};
  const WORD indices[4] = {0, 1, 2, 3};
  IDirect3DVertexBuffer8 *vb = NULL;
  IDirect3DIndexBuffer8 *ib = NULL;
  BYTE *memory = NULL;
  IDirect3DDevice8_CreateVertexBuffer(g_device, sizeof v, D3DUSAGE_WRITEONLY, FVF_SCREEN, D3DPOOL_MANAGED, &vb);
  IDirect3DDevice8_CreateIndexBuffer(g_device, sizeof indices, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
  if (vb && SUCCEEDED(IDirect3DVertexBuffer8_Lock(vb, 0, 0, &memory, 0))) { memcpy(memory, v, sizeof v); IDirect3DVertexBuffer8_Unlock(vb); }
  if (ib && SUCCEEDED(IDirect3DIndexBuffer8_Lock(ib, 0, 0, &memory, 0))) { memcpy(memory, indices, sizeof indices); IDirect3DIndexBuffer8_Unlock(ib); }
  if (vb && ib) {
    IDirect3DDevice8_SetVertexShader(g_device, FVF_SCREEN);
    IDirect3DDevice8_SetStreamSource(g_device, 0, vb, sizeof v[0]);
    IDirect3DDevice8_SetIndices(g_device, ib, 4);
    clear(0xFF000000);
    check(IDirect3DDevice8_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLESTRIP, 0, 4, 0, 2) == D3D_OK, "DrawIndexedPrimitive");
    read_back();
    check(pixel(4, 4) == 0 && pixel(40, 40) == 0x00FF00, "SetIndices' base vertex index counts the indices from vertex 4");
    IDirect3DDevice8_SetIndices(g_device, ib, 0);
    clear(0xFF000000);
    IDirect3DDevice8_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLESTRIP, 0, 4, 0, 2);
    IDirect3DDevice8_DrawPrimitive(g_device, D3DPT_TRIANGLESTRIP, 4, 2);
    read_back();
    check(pixel(4, 4) == 0xFF0000 && pixel(40, 40) == 0x00FF00, "... from 0, and DrawPrimitive from vertex 4");
    IDirect3DDevice8_SetStreamSource(g_device, 0, NULL, 0);
    IDirect3DDevice8_SetIndices(g_device, NULL, 0);
  } else {
    check(0, "the buffers to draw from");
  }
  if (vb) IDirect3DVertexBuffer8_Release(vb);
  if (ib) IDirect3DIndexBuffer8_Release(ib);
}

/* vs.1.1: mov oPos, v0; add oD0, v5, c1 (the colour plus a constant the program sets) */
static const DWORD g_vs_colour[] = {0xFFFE0101, 0x00000001, 0xC00F0000, 0x90E40000, 0x00000002, 0xD00F0000, 0x90E40005, 0xA0E40001, 0x0000FFFF};
/* vs.1.0: mov oPos, v0; mov oD0, c4 (a constant the declaration loads) */
static const DWORD g_vs_loaded[] = {0xFFFE0100, 0x00000001, 0xC00F0000, 0x90E40000, 0x00000001, 0xD00F0000, 0xA0E40004, 0x0000FFFF};

static void clip_quad(float x0, float y0, float x1, float y1, DWORD colour) {
  const ClipVertex v[4] = {{x0, y0, 0.5f, colour}, {x1, y0, 0.5f, colour}, {x0, y1, 0.5f, colour}, {x1, y1, 0.5f, colour}};
  IDirect3DDevice8_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
}
static void test_shaders(void) {
  const DWORD declaration[] = {D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END()};
  const DWORD loading[] = {D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3), D3DVSD_SKIP(1), D3DVSD_CONST(4, 1), 0x00000000, 0x3F800000, 0x3F800000, 0x3F800000,
                           D3DVSD_END()};      /* (c4 = 0, 1, 1, 1: cyan) */
  DWORD vs = 0, vs_loaded = 0, vs_fixed = 0, ps = 0, ps10 = 0, ps_tex = 0, current = 0;
  check(IDirect3DDevice8_CreateVertexShader(g_device, declaration, g_vs_colour, &vs, 0) == D3D_OK && vs > 0xF0000000u, "CreateVertexShader: a handle above the FVF codes");
  check(IDirect3DDevice8_CreateVertexShader(g_device, loading, g_vs_loaded, &vs_loaded, 0) == D3D_OK && vs_loaded != vs, "... a vs.1.0 whose declaration loads a constant");
  check(IDirect3DDevice8_CreateVertexShader(g_device, declaration, NULL, &vs_fixed, 0) == D3D_OK, "... the fixed-function pipeline with a declaration");
  const DWORD bad_normal[] = {D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3), D3DVSD_REG(D3DVSDE_NORMAL, D3DVSDT_FLOAT2), D3DVSD_END()};
  const DWORD bad_register[] = {D3DVSD_STREAM(0), D3DVSD_REG(17, D3DVSDT_FLOAT3), D3DVSD_END()};
  static const DWORD vs12[] = {0xFFFE0102, 0x0000FFFF};
  DWORD unused = 0;
  check(IDirect3DDevice8_CreateVertexShader(g_device, bad_normal, NULL, &unused, 0) == D3DERR_INVALIDCALL &&
        IDirect3DDevice8_CreateVertexShader(g_device, bad_register, g_vs_colour, &unused, 0) == D3DERR_INVALIDCALL &&
        IDirect3DDevice8_CreateVertexShader(g_device, declaration, vs12, &unused, 0) == D3DERR_INVALIDCALL,
        "refused: a normal of two floats for the fixed-function pipeline, a register 17, a vs.1.2");
  DWORD size = 0, tokens[16];
  check(IDirect3DDevice8_GetVertexShaderDeclaration(g_device, vs, NULL, &size) == D3D_OK && size == sizeof declaration &&
        (size = 4, IDirect3DDevice8_GetVertexShaderDeclaration(g_device, vs, tokens, &size) == D3DERR_INVALIDCALL && size == 4) &&
        (size = sizeof tokens, IDirect3DDevice8_GetVertexShaderDeclaration(g_device, vs, tokens, &size) == D3D_OK && size == sizeof declaration &&
                               memcmp(tokens, declaration, size) == 0) &&
        (size = sizeof tokens, IDirect3DDevice8_GetVertexShaderFunction(g_device, vs, tokens, &size) == D3D_OK && size == sizeof g_vs_colour && memcmp(tokens, g_vs_colour, size) == 0) &&
        IDirect3DDevice8_GetVertexShaderFunction(g_device, vs_fixed, NULL, &size) == D3D_OK && size == 0,
        "GetVertexShaderDeclaration and GetVertexShaderFunction give the program's tokens (with Direct3D 8's sizing)");

  const float add[4] = {0.0f, 0.0f, 0.5f, 0.0f};
  float back_k[4] = {0, 0, 0, 0};
  check(IDirect3DDevice8_SetVertexShaderConstant(g_device, 1, add, 1) == D3D_OK && IDirect3DDevice8_GetVertexShaderConstant(g_device, 1, back_k, 1) == D3D_OK && back_k[2] == 0.5f,
        "SetVertexShaderConstant, GetVertexShaderConstant");
  check(IDirect3DDevice8_SetVertexShader(g_device, vs) == D3D_OK && IDirect3DDevice8_GetVertexShader(g_device, &current) == D3D_OK && current == vs, "SetVertexShader of a handle");
  clear(0xFF000000);
  clip_quad(-1.0f, 1.0f, 0.0f, 0.0f, 0xFFFF0000);
  read_back();
  check(near_colour(pixel(16, 16), 255, 0, 128) && pixel(48, 48) == 0, "the vertex shader of D3DVSD tokens draws (v5 is the diffuse colour, c1 added)");
  IDirect3DDevice8_SetVertexShader(g_device, vs_loaded);
  clear(0xFF000000);
  {
    const struct { float x, y, z; DWORD skipped; } q[4] = {{0, 0, 0.5f, 0}, {1, 0, 0.5f, 0}, {0, -1, 0.5f, 0}, {1, -1, 0.5f, 0}};
    IDirect3DDevice8_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
  }
  read_back();
  check(near_colour(pixel(48, 48), 0, 255, 255) && pixel(16, 16) == 0, "the constant the declaration loads when its shader is set (and a skipped word)");
  IDirect3DDevice8_SetVertexShader(g_device, vs_fixed);
  static const D3DMATRIX identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  IDirect3DDevice8_SetTransform(g_device, D3DTS_WORLD, &identity);
  IDirect3DDevice8_SetTransform(g_device, D3DTS_VIEW, &identity);
  IDirect3DDevice8_SetTransform(g_device, D3DTS_PROJECTION, &identity);
  clear(0xFF000000);
  clip_quad(0.0f, 1.0f, 1.0f, 0.0f, 0xFF0000FF);
  read_back();
  check(pixel(48, 16) == 0x0000FF && pixel(16, 48) == 0, "the fixed-function pipeline through a declaration handle");

  /* Pixel shaders: ps.1.1 mov r0, c2; ps.1.0 mul r0, v0, c3; ps.1.1 tex t0; mov r0, t0 */
  static const DWORD ps11[] = {0xFFFF0101, 0x00000001, 0x800F0000, 0xA0E40002, 0x0000FFFF};
  static const DWORD ps10_code[] = {0xFFFF0100, 0x00000005, 0x800F0000, 0x90E40000, 0xA0E40003, 0x0000FFFF};
  static const DWORD ps_tex_code[] = {0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000001, 0x800F0000, 0xB0E40000, 0x0000FFFF};
  static const DWORD ps20[] = {0xFFFF0200, 0x0000FFFF};
  check(IDirect3DDevice8_CreatePixelShader(g_device, ps11, &ps) == D3D_OK && IDirect3DDevice8_CreatePixelShader(g_device, ps10_code, &ps10) == D3D_OK &&
        IDirect3DDevice8_CreatePixelShader(g_device, ps_tex_code, &ps_tex) == D3D_OK && IDirect3DDevice8_CreatePixelShader(g_device, ps20, &unused) == D3DERR_INVALIDCALL &&
        IDirect3DDevice8_GetPixelShader(g_device, &current) == D3D_OK && current == 0, "CreatePixelShader of ps.1.1 and ps.1.0 (not ps_2_0); none is set by it");
  const float colours[2][4] = {{0.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.5f, 1.0f, 1.0f}};
  float back_c[2][4];
  check(IDirect3DDevice8_SetPixelShaderConstant(g_device, 2, colours, 2) == D3D_OK && IDirect3DDevice8_GetPixelShaderConstant(g_device, 2, back_c, 2) == D3D_OK &&
        back_c[1][1] == 0.5f && IDirect3DDevice8_SetPixelShaderConstant(g_device, 7, colours, 2) == D3DERR_INVALIDCALL, "the pixel shader's eight constants");
  IDirect3DDevice8_SetVertexShader(g_device, FVF_SCREEN);
  IDirect3DDevice8_SetPixelShader(g_device, ps);
  clear(0xFF000000);
  screen_quad(0, 0, 32, 32, 0xFFFF0000);
  IDirect3DDevice8_SetPixelShader(g_device, ps10);
  screen_quad(32, 32, 64, 64, 0xFFFFFFFF);
  read_back();
  check(pixel(16, 16) == 0x00FF00 && near_colour(pixel(48, 48), 255, 128, 255), "a ps.1.1 of a constant, and a ps.1.0 that scales the colour");
  IDirect3DTexture8 *texture = NULL;
  D3DLOCKED_RECT locked;
  IDirect3DDevice8_CreateTexture(g_device, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture);
  if (texture && SUCCEEDED(IDirect3DTexture8_LockRect(texture, 0, &locked, NULL, 0))) {
    DWORD *row0 = locked.pBits, *row1 = (DWORD *)((BYTE *)locked.pBits + locked.Pitch);
    row0[0] = 0xFFFF0000; row0[1] = 0xFF00FF00; row1[0] = 0xFF0000FF; row1[1] = 0xFFFFFFFF;
    IDirect3DTexture8_UnlockRect(texture, 0);
  }
  /* (the sampler states are texture stage states in Direct3D 8) */
  DWORD filter = 0;
  check(IDirect3DDevice8_SetTextureStageState(g_device, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT) == D3D_OK && IDirect3DDevice8_SetTextureStageState(g_device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT) == D3D_OK &&
        IDirect3DDevice8_GetTextureStageState(g_device, 0, D3DTSS_MAGFILTER, &filter) == D3D_OK && filter == D3DTEXF_POINT &&
        IDirect3DDevice8_SetTextureStageState(g_device, 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP) == D3D_OK &&
        IDirect3DDevice8_GetTextureStageState(g_device, 0, D3DTSS_ADDRESSU, &filter) == D3D_OK && filter == D3DTADDRESS_CLAMP &&
        IDirect3DDevice8_SetTextureStageState(g_device, 8, D3DTSS_COLOROP, D3DTOP_DISABLE) == D3DERR_INVALIDCALL, "sampler states as texture stage states");
  IDirect3DDevice8_SetTexture(g_device, 0, (IDirect3DBaseTexture8 *)texture);
  IDirect3DDevice8_SetPixelShader(g_device, ps_tex);
  IDirect3DDevice8_SetVertexShader(g_device, FVF_SCREEN_TEX);
  clear(0xFF000000);
  {
    const ScreenTexVertex q[4] = {{0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {64, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0}, {0, 64, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {64, 64, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
    IDirect3DDevice8_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
  }
  read_back();
  check(pixel(10, 10) == 0xFF0000 && pixel(54, 10) == 0x00FF00 && pixel(10, 54) == 0x0000FF && pixel(54, 54) == 0xFFFFFF, "a ps.1.1 that samples a texture");
  IDirect3DDevice8_SetPixelShader(g_device, 0);
  clear(0xFF000000);
  {
    const ScreenTexVertex q[4] = {{0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {64, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0}, {0, 64, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {64, 64, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
    IDirect3DDevice8_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
  }
  read_back();
  check(pixel(10, 10) == 0xFF0000 && pixel(54, 54) == 0xFFFFFF, "... and the fixed-function stages with the same texture");
  IDirect3DDevice8_SetTexture(g_device, 0, NULL);
  if (texture) IDirect3DTexture8_Release(texture);

  /* Deleting: the shader that is set leaves none set; a handle deleted twice is refused. */
  IDirect3DDevice8_SetVertexShader(g_device, vs);
  IDirect3DDevice8_SetPixelShader(g_device, ps);
  check(IDirect3DDevice8_DeleteVertexShader(g_device, vs) == D3D_OK && IDirect3DDevice8_GetVertexShader(g_device, &current) == D3D_OK && current == 0 &&
        IDirect3DDevice8_DeleteVertexShader(g_device, vs) == D3DERR_INVALIDCALL && IDirect3DDevice8_DeleteVertexShader(g_device, FVF_SCREEN) == D3DERR_INVALIDCALL &&
        IDirect3DDevice8_DeletePixelShader(g_device, ps) == D3D_OK && IDirect3DDevice8_GetPixelShader(g_device, &current) == D3D_OK && current == 0 &&
        IDirect3DDevice8_DeletePixelShader(g_device, ps) == D3DERR_INVALIDCALL && IDirect3DDevice8_SetVertexShader(g_device, vs) == D3DERR_INVALIDCALL,
        "DeleteVertexShader and DeletePixelShader");
  IDirect3DDevice8_DeleteVertexShader(g_device, vs_loaded);
  IDirect3DDevice8_DeleteVertexShader(g_device, vs_fixed);
  IDirect3DDevice8_DeletePixelShader(g_device, ps10);
  IDirect3DDevice8_DeletePixelShader(g_device, ps_tex);
  IDirect3DDevice8_SetVertexShader(g_device, FVF_SCREEN);

  /* ValidateVertexShader, ValidatePixelShader (exports d3d8.h does not declare) */
  typedef HRESULT (WINAPI *ValidateVs)(const DWORD *, const DWORD *, const D3DCAPS8 *, BOOL, char **);
  typedef HRESULT (WINAPI *ValidatePs)(const DWORD *, const D3DCAPS8 *, BOOL, char **);
  HMODULE library = GetModuleHandleW(L"d3d8.dll");
  ValidateVs ValidateVertexShader = library ? (ValidateVs)(void (*)(void))GetProcAddress(library, "ValidateVertexShader") : NULL;
  ValidatePs ValidatePixelShader = library ? (ValidatePs)(void (*)(void))GetProcAddress(library, "ValidatePixelShader") : NULL;
  check(ValidateVertexShader && ValidatePixelShader && GetProcAddress(library, "DebugSetMute") && GetProcAddress(library, "D3D8GetSWInfo"), "the exports of d3d8.dll");
  if (!ValidateVertexShader || !ValidatePixelShader) return;
  char *errors = NULL;
  const DWORD only_normal[] = {D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_NORMAL, D3DVSDT_FLOAT3), D3DVSD_END()};
  check(ValidateVertexShader(g_vs_colour, declaration, NULL, FALSE, NULL) == S_OK && ValidateVertexShader(g_vs_colour, only_normal, NULL, TRUE, &errors) == E_FAIL &&
        errors && errors[0] && ValidatePixelShader(ps11, NULL, FALSE, NULL) == S_OK && ValidatePixelShader(ps20, NULL, FALSE, NULL) == E_FAIL,
        "ValidateVertexShader (an input not in the declaration) and ValidatePixelShader");
  if (errors) HeapFree(GetProcessHeap(), 0, errors);
}

static void test_states(void) {
  DWORD value = 0;
  check(IDirect3DDevice8_SetRenderState(g_device, D3DRS_ZBIAS, 8) == D3D_OK && IDirect3DDevice8_GetRenderState(g_device, D3DRS_ZBIAS, &value) == D3D_OK && value == 8 &&
        IDirect3DDevice8_SetRenderState(g_device, D3DRS_LINEPATTERN, 0x00010003) == D3D_OK && IDirect3DDevice8_GetRenderState(g_device, D3DRS_LINEPATTERN, &value) == D3D_OK &&
        value == 0x00010003 && IDirect3DDevice8_SetRenderState(g_device, D3DRS_ZVISIBLE, TRUE) == D3D_OK &&
        IDirect3DDevice8_SetRenderState(g_device, D3DRS_EDGEANTIALIAS, TRUE) == D3D_OK && IDirect3DDevice8_GetRenderState(g_device, D3DRS_EDGEANTIALIAS, &value) == D3D_OK && value == TRUE,
        "the render states Direct3D 9 has not: ZBIAS, LINEPATTERN, ZVISIBLE, EDGEANTIALIAS");
  check(IDirect3DDevice8_SetRenderState(g_device, D3DRS_SOFTWAREVERTEXPROCESSING, TRUE) == D3DERR_INVALIDCALL &&
        IDirect3DDevice8_GetRenderState(g_device, D3DRS_PATCHSEGMENTS, &value) == D3D_OK && value == 0x3F800000 &&
        IDirect3DDevice8_SetRenderState(g_device, (D3DRENDERSTATETYPE)174, TRUE) == D3DERR_INVALIDCALL,
        "no software vertex processing on a device of hardware vertex processing; one patch segment; Direct3D 9's own states are not Direct3D 8's");
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_ZBIAS, 0);
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_EDGEANTIALIAS, FALSE);

  /* State blocks by handle: a block of everything, of the vertex state (with the vertex shader's handle). */
  DWORD all = 0, vertex = 0, recorded = 0, handle = 0;
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_FOGCOLOR, 0x00112233);
  check(IDirect3DDevice8_CreateStateBlock(g_device, D3DSBT_ALL, &all) == D3D_OK && all != 0 && IDirect3DDevice8_CreateStateBlock(g_device, 7, &vertex) == D3DERR_INVALIDCALL,
        "CreateStateBlock gives a handle; an unknown kind is refused");
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_FOGCOLOR, 0x00445566);
  check(IDirect3DDevice8_ApplyStateBlock(g_device, all) == D3D_OK && IDirect3DDevice8_GetRenderState(g_device, D3DRS_FOGCOLOR, &value) == D3D_OK && value == 0x00112233,
        "ApplyStateBlock gives the state back");
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_FOGCOLOR, 0x00778899);
  check(IDirect3DDevice8_CaptureStateBlock(g_device, all) == D3D_OK && IDirect3DDevice8_SetRenderState(g_device, D3DRS_FOGCOLOR, 0) == D3D_OK &&
        IDirect3DDevice8_ApplyStateBlock(g_device, all) == D3D_OK && IDirect3DDevice8_GetRenderState(g_device, D3DRS_FOGCOLOR, &value) == D3D_OK && value == 0x00778899,
        "CaptureStateBlock takes the state that is set now");
  DWORD vs = 0;
  const DWORD declaration[] = {D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END()};
  IDirect3DDevice8_CreateVertexShader(g_device, declaration, g_vs_colour, &vs, 0);
  IDirect3DDevice8_SetVertexShader(g_device, vs);
  check(IDirect3DDevice8_CreateStateBlock(g_device, D3DSBT_VERTEXSTATE, &vertex) == D3D_OK && IDirect3DDevice8_SetVertexShader(g_device, FVF_SCREEN) == D3D_OK &&
        IDirect3DDevice8_ApplyStateBlock(g_device, vertex) == D3D_OK && IDirect3DDevice8_GetVertexShader(g_device, &handle) == D3D_OK && handle == vs,
        "a block of the vertex state has the vertex shader's handle");
  check(IDirect3DDevice8_BeginStateBlock(g_device) == D3D_OK && IDirect3DDevice8_BeginStateBlock(g_device) == D3DERR_INVALIDCALL &&
        IDirect3DDevice8_ApplyStateBlock(g_device, all) == D3DERR_INVALIDCALL && IDirect3DDevice8_CreateStateBlock(g_device, D3DSBT_ALL, &handle) == D3DERR_INVALIDCALL &&
        IDirect3DDevice8_EndStateBlock(g_device, &recorded) == D3D_OK && recorded != 0 && recorded != all && IDirect3DDevice8_EndStateBlock(g_device, &handle) == D3DERR_INVALIDCALL &&
        IDirect3DDevice8_ApplyStateBlock(g_device, recorded) == D3D_OK, "BeginStateBlock .. EndStateBlock, and what is refused while a block is recorded");
  check(IDirect3DDevice8_DeleteStateBlock(g_device, all) == D3D_OK && IDirect3DDevice8_DeleteStateBlock(g_device, all) == D3DERR_INVALIDCALL &&
        IDirect3DDevice8_ApplyStateBlock(g_device, all) == D3DERR_INVALIDCALL && IDirect3DDevice8_DeleteStateBlock(g_device, vertex) == D3D_OK &&
        IDirect3DDevice8_DeleteStateBlock(g_device, recorded) == D3D_OK, "DeleteStateBlock: a handle is gone with it");
  IDirect3DDevice8_SetVertexShader(g_device, FVF_SCREEN);
  IDirect3DDevice8_DeleteVertexShader(g_device, vs);
  check(IDirect3DDevice8_GetInfo(g_device, 4, &value, 4) == S_FALSE && IDirect3DDevice8_GetInfo(g_device, 1, &value, 4) == E_FAIL, "GetInfo: no numbers in the retail runtime");
}

/* CopyRects between surfaces in memory, into the back buffer and out of it; the sizes in the descriptions. */
static void test_copies(void) {
  IDirect3DSurface8 *a = NULL, *b = NULL, *back = NULL, *whole = NULL, *depth = NULL;
  D3DSURFACE_DESC desc;
  D3DLOCKED_RECT locked;
  IDirect3DDevice8_CreateImageSurface(g_device, 16, 16, D3DFMT_A8R8G8B8, &a);
  IDirect3DDevice8_CreateImageSurface(g_device, 16, 16, D3DFMT_A8R8G8B8, &b);
  IDirect3DDevice8_CreateImageSurface(g_device, SIZE, SIZE, D3DFMT_X8R8G8B8, &whole);
  if (!a || !b || !whole) { check(0, "CreateImageSurface"); goto done; }
  check(SUCCEEDED(IDirect3DSurface8_GetDesc(a, &desc)) && desc.Pool == D3DPOOL_SYSTEMMEM && desc.Size == 16 * 16 * 4 && desc.Width == 16 && desc.Type == D3DRTYPE_SURFACE,
        "an image surface is in system memory; the description has its size in bytes");
  if (SUCCEEDED(IDirect3DSurface8_LockRect(a, &locked, NULL, 0))) {
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) ((DWORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0xFF000000u | (DWORD)(y << 8) | (DWORD)x;
    IDirect3DSurface8_UnlockRect(a);
  }
  if (SUCCEEDED(IDirect3DSurface8_LockRect(b, &locked, NULL, 0))) {
    for (int y = 0; y < 16; ++y) memset((BYTE *)locked.pBits + y * locked.Pitch, 0, 16 * 4);
    IDirect3DSurface8_UnlockRect(b);
  }
  const RECT rects[2] = {{0, 0, 4, 4}, {8, 8, 12, 10}};
  const POINT points[2] = {{10, 10}, {0, 12}};
  check(IDirect3DDevice8_CopyRects(g_device, a, rects, 2, b, points) == D3D_OK, "CopyRects of two rectangles to two points");
  if (SUCCEEDED(IDirect3DSurface8_LockRect(b, &locked, NULL, D3DLOCK_READONLY))) {
    const DWORD *row = (const DWORD *)((const BYTE *)locked.pBits + 11 * locked.Pitch), *row12 = (const DWORD *)((const BYTE *)locked.pBits + 13 * locked.Pitch);
    check((row[11] & 0xFFFF) == 0x0101 && (row12[3] & 0xFFFF) == 0x090B && row12[5] == 0 && ((const DWORD *)locked.pBits)[0] == 0, "... each where it is put");
    IDirect3DSurface8_UnlockRect(b);
  }
  const RECT outside = {10, 10, 20, 12};
  IDirect3DSurface8 *other = NULL;
  IDirect3DDevice8_CreateImageSurface(g_device, 16, 16, D3DFMT_R5G6B5, &other);
  check(IDirect3DDevice8_CopyRects(g_device, a, &outside, 1, b, NULL) == D3DERR_INVALIDCALL && IDirect3DDevice8_CopyRects(g_device, a, NULL, 0, a, NULL) == D3DERR_INVALIDCALL &&
        (!other || IDirect3DDevice8_CopyRects(g_device, a, NULL, 0, other, NULL) == D3DERR_INVALIDCALL), "refused: a rectangle out of the surface, a copy onto itself, other formats");
  if (other) IDirect3DSurface8_Release(other);
  /* into the back buffer (memory into the device) and the whole back buffer out again */
  IDirect3DDevice8_GetBackBuffer(g_device, 0, D3DBACKBUFFER_TYPE_MONO, &back);
  IDirect3DSurface8 *x8 = NULL;
  IDirect3DDevice8_CreateImageSurface(g_device, 16, 16, D3DFMT_X8R8G8B8, &x8);
  if (back && x8 && SUCCEEDED(IDirect3DSurface8_LockRect(x8, &locked, NULL, 0))) {
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) ((DWORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] = 0xFF00FF00u;
    IDirect3DSurface8_UnlockRect(x8);
    clear(0xFF000000);
    const POINT at = {20, 20};
    check(IDirect3DDevice8_CopyRects(g_device, x8, NULL, 0, back, &at) == D3D_OK, "CopyRects of a surface in memory into the back buffer");
    read_back();
    check(pixel(24, 24) == 0x00FF00 && pixel(35, 35) == 0x00FF00 && pixel(36, 36) == 0 && pixel(19, 19) == 0, "... at its point");
    check(IDirect3DDevice8_CopyRects(g_device, back, NULL, 0, whole, NULL) == D3D_OK && SUCCEEDED(IDirect3DSurface8_LockRect(whole, &locked, NULL, D3DLOCK_READONLY)),
          "CopyRects of the back buffer into memory");
    check((((const DWORD *)((const BYTE *)locked.pBits + 30 * locked.Pitch))[30] & 0xFFFFFF) == 0x00FF00 && (((const DWORD *)locked.pBits)[0] & 0xFFFFFF) == 0, "... its pixels");
    IDirect3DSurface8_UnlockRect(whole);
  } else {
    check(0, "the back buffer and a surface to copy into it");
  }
  if (x8) IDirect3DSurface8_Release(x8);
  IDirect3DTexture8 *dxt = NULL;
  IDirect3DVolumeTexture8 *volume = NULL;
  IDirect3DVolume8 *level = NULL;
  D3DVOLUME_DESC vdesc;
  check(SUCCEEDED(IDirect3DDevice8_CreateTexture(g_device, 8, 8, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &dxt)) && SUCCEEDED(IDirect3DTexture8_GetLevelDesc(dxt, 0, &desc)) &&
        desc.Size == 32 && desc.Format == D3DFMT_DXT1, "a DXT1 level's size: blocks of 8 bytes");
  if (dxt) IDirect3DTexture8_Release(dxt);
  check(SUCCEEDED(IDirect3DDevice8_CreateVolumeTexture(g_device, 4, 4, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume)) &&
        SUCCEEDED(IDirect3DVolumeTexture8_GetVolumeLevel(volume, 0, &level)) && SUCCEEDED(IDirect3DVolume8_GetDesc(level, &vdesc)) && vdesc.Size == 4 * 4 * 2 * 4 &&
        vdesc.Depth == 2, "a volume texture's level as an IDirect3DVolume8, with its size");
  if (level) {
    D3DLOCKED_BOX box;
    check(SUCCEEDED(IDirect3DVolume8_LockBox(level, &box, NULL, 0)) && box.pBits && box.SlicePitch >= 64 && SUCCEEDED(IDirect3DVolume8_UnlockBox(level)), "... it locks");
    IDirect3DVolume8_Release(level);
  }
  if (volume) IDirect3DVolumeTexture8_Release(volume);
  check(SUCCEEDED(IDirect3DDevice8_CreateDepthStencilSurface(g_device, 32, 32, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &depth)) && depth &&
        IDirect3DDevice8_SetRenderTarget(g_device, NULL, depth) == D3DERR_INVALIDCALL, "SetRenderTarget refuses a depth buffer smaller than the target");
done:
  if (depth) IDirect3DSurface8_Release(depth);
  if (back) IDirect3DSurface8_Release(back);
  if (a) IDirect3DSurface8_Release(a);
  if (b) IDirect3DSurface8_Release(b);
  if (whole) IDirect3DSurface8_Release(whole);
}

/* A texture drawn into with SetRenderTarget(target, depth), the depth buffer kept. */
static void test_render_target(void) {
  IDirect3DTexture8 *texture = NULL;
  IDirect3DSurface8 *level = NULL, *back = NULL, *depth = NULL, *got = NULL;
  IDirect3DDevice8_CreateTexture(g_device, 32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &texture);
  if (texture) IDirect3DTexture8_GetSurfaceLevel(texture, 0, &level);
  IDirect3DDevice8_GetBackBuffer(g_device, 0, D3DBACKBUFFER_TYPE_MONO, &back);
  IDirect3DDevice8_GetDepthStencilSurface(g_device, &depth);
  if (!level || !back || !depth) { check(0, "a texture to draw into"); goto done; }
  check(IDirect3DDevice8_SetRenderTarget(g_device, level, depth) == D3D_OK && SUCCEEDED(IDirect3DDevice8_GetRenderTarget(g_device, &got)) && got == level,
        "SetRenderTarget of a texture's level; GetRenderTarget gives the level");
  if (got) { IDirect3DSurface8_Release(got); got = NULL; }
  IDirect3DDevice8_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0);
  IDirect3DDevice8_SetVertexShader(g_device, FVF_SCREEN);
  screen_quad(0, 0, 16, 32, 0xFFFFFF00);
  check(IDirect3DDevice8_SetRenderTarget(g_device, back, NULL) == D3D_OK && IDirect3DDevice8_GetDepthStencilSurface(g_device, &got) == D3DERR_NOTFOUND && got == NULL,
        "SetRenderTarget with no depth buffer leaves none");
  IDirect3DDevice8_SetRenderTarget(g_device, back, depth);
  IDirect3DDevice8_SetTexture(g_device, 0, (IDirect3DBaseTexture8 *)texture);
  IDirect3DDevice8_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  IDirect3DDevice8_SetTextureStageState(g_device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  IDirect3DDevice8_SetVertexShader(g_device, FVF_SCREEN_TEX);
  clear(0xFF000000);
  {
    const ScreenTexVertex q[4] = {{0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {64, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0}, {0, 64, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {64, 64, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
    IDirect3DDevice8_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
  }
  read_back();
  check(pixel(10, 32) == 0xFFFF00 && pixel(54, 32) == 0x0000FF, "what was drawn into the texture, sampled");
  IDirect3DDevice8_SetTexture(g_device, 0, NULL);
  IDirect3DDevice8_SetTextureStageState(g_device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  IDirect3DDevice8_SetTextureStageState(g_device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
done:
  if (level) IDirect3DSurface8_Release(level);
  if (texture) IDirect3DTexture8_Release(texture);
  if (back) IDirect3DSurface8_Release(back);
  if (depth) IDirect3DSurface8_Release(depth);
}

static void test_present_and_reset(void) {
  check(IDirect3DDevice8_Present(g_device, NULL, NULL, NULL, NULL) == D3D_OK && IDirect3DDevice8_TestCooperativeLevel(g_device) == D3D_OK, "Present, TestCooperativeLevel");
  IDirect3DSurface8 *before = NULL, *after = NULL;
  D3DSURFACE_DESC desc;
  DWORD vs = 0, handle = 1, size = 0;
  const DWORD declaration[] = {D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3), D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END()};
  IDirect3DDevice8_CreateVertexShader(g_device, declaration, g_vs_colour, &vs, 0);
  IDirect3DDevice8_SetVertexShader(g_device, vs);
  IDirect3DDevice8_GetBackBuffer(g_device, 0, D3DBACKBUFFER_TYPE_MONO, &before);
  D3DPRESENT_PARAMETERS pp;
  present_parameters(&pp, 32);
  /* A back buffer the program holds keeps Reset from happening: the device is lost until a Reset succeeds. */
  check(IDirect3DDevice8_Reset(g_device, &pp) == D3DERR_DEVICELOST && IDirect3DDevice8_TestCooperativeLevel(g_device) == D3DERR_DEVICENOTRESET &&
        IDirect3DDevice8_Present(g_device, NULL, NULL, NULL, NULL) == D3DERR_DEVICELOST, "Reset while the program holds the back buffer: D3DERR_DEVICELOST");
  if (before) IDirect3DSurface8_Release(before);
  check(IDirect3DDevice8_Reset(g_device, &pp) == D3D_OK && IDirect3DDevice8_TestCooperativeLevel(g_device) == D3D_OK && pp.BackBufferWidth == 32,
        "Reset to 32 x 32, and the device is back");
  IDirect3DDevice8_GetBackBuffer(g_device, 0, D3DBACKBUFFER_TYPE_MONO, &after);
  check(after && after == before && SUCCEEDED(IDirect3DSurface8_GetDesc(after, &desc)) && desc.Width == 32 && desc.Height == 32, "the back buffer is the same object, of the new size");
  if (after) IDirect3DSurface8_Release(after);
  check(IDirect3DDevice8_GetVertexShader(g_device, &handle) == D3D_OK && handle == 0 && IDirect3DDevice8_GetVertexShaderFunction(g_device, vs, NULL, &size) == D3D_OK &&
        size == sizeof g_vs_colour, "after Reset no shader is set, but the shaders are kept");
  IDirect3DDevice8_DeleteVertexShader(g_device, vs);
  present_parameters(&pp, SIZE);
  IDirect3DDevice8_Reset(g_device, &pp);
}

/* Wine's visual tests read a pixel back by CopyRects of the render target into a level of a texture in system memory: the same after a draw of
 * untransformed vertices through identity matrices. */
static DWORD readback_by_copy(int x, int y) {
  IDirect3DSurface8 *rt = NULL, *level = NULL;
  IDirect3DTexture8 *texture = NULL;
  D3DSURFACE_DESC desc;
  D3DLOCKED_RECT locked;
  DWORD colour = 0xDEADBEEF;
  IDirect3DDevice8_GetRenderTarget(g_device, &rt);
  if (rt && SUCCEEDED(IDirect3DSurface8_GetDesc(rt, &desc)) &&
      SUCCEEDED(IDirect3DDevice8_CreateTexture(g_device, desc.Width, desc.Height, 1, 0, desc.Format, D3DPOOL_SYSTEMMEM, &texture)) &&
      SUCCEEDED(IDirect3DTexture8_GetSurfaceLevel(texture, 0, &level)) && SUCCEEDED(IDirect3DDevice8_CopyRects(g_device, rt, NULL, 0, level, NULL)) &&
      SUCCEEDED(IDirect3DSurface8_LockRect(level, &locked, NULL, D3DLOCK_READONLY))) {
    colour = ((DWORD *)((BYTE *)locked.pBits + y * locked.Pitch))[x] & 0xFFFFFF;
    IDirect3DSurface8_UnlockRect(level);
  }
  if (level) IDirect3DSurface8_Release(level);
  if (texture) IDirect3DTexture8_Release(texture);
  if (rt) IDirect3DSurface8_Release(rt);
  return colour;
}
static void test_readback_like_wine(void) {
  static const D3DMATRIX identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  const ClipVertex quad[4] = {{-1, -1, 0.1f, 0xFFFF0000}, {-1, 1, 0.1f, 0xFFFF0000}, {1, -1, 0.1f, 0xFFFF0000}, {1, 1, 0.1f, 0xFFFF0000}};
  IDirect3DDevice8_Clear(g_device, 0, NULL, D3DCLEAR_TARGET, 0xFFFFFFFF, 0.0f, 0);
  const DWORD cleared = readback_by_copy(10, 10);
  IDirect3DDevice8_SetTransform(g_device, D3DTS_WORLDMATRIX(0), &identity);
  IDirect3DDevice8_SetTransform(g_device, D3DTS_VIEW, &identity);
  IDirect3DDevice8_SetTransform(g_device, D3DTS_PROJECTION, &identity);
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_ZENABLE, FALSE);
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice8_SetVertexShader(g_device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
  IDirect3DDevice8_BeginScene(g_device);
  const HRESULT hr = IDirect3DDevice8_DrawPrimitiveUP(g_device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0]);
  IDirect3DDevice8_EndScene(g_device);
  const DWORD drawn = readback_by_copy(32, 32);
  check(hr == D3D_OK && cleared == 0xFFFFFF && drawn == 0xFF0000, "a pixel read back by CopyRects into a texture in system memory (as Wine's tests read)");
  IDirect3DDevice8_SetRenderState(g_device, D3DRS_ZENABLE, D3DZB_TRUE);
}

int main(void) {
  test_object();
  if (g_d3d && make_device()) {
    test_references();
    test_draws();
    test_readback_like_wine();
    test_shaders();
    test_states();
    test_copies();
    test_render_target();
    test_present_and_reset();
    check(IDirect3DDevice8_Release(g_device) == 0, "the device is gone with its last reference");
  }
  if (g_d3d) check(IDirect3D8_Release(g_d3d) == 0, "the Direct3D object is gone with its last reference");
  if (g_window) DestroyWindow(g_window);
  if (g_failed == 0) printf("d3d8test: all %d checks passed\n", g_passed);
  else printf("d3d8test: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
