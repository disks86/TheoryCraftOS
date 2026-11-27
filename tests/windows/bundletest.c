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

/* A program that brings its own Direct3D libraries (docs/WindowsUserMode.md, "Libraries of the same name"). Run by tests/bundled_dlls_qemu.sh, which puts it
 * twice on the disk:
 *   C:\BundleA\  with "foreign" d3d9.dll, d3d12.dll, d3d11.dll and d3d10.dll (tests/windows/bundle_foreign.c: the public entry points, failing; none of this OS's
 *                private exports), as a program that ships DXVK or vkd3d-proton has them. LoadLibrary("d3d9.dll") and the others are those files, and this OS's
 *                ddraw.dll (Direct3D 7), d3d8.dll, d3d11.dll and d3d10.dll/d3d10_1.dll (loaded from System32 by path) still draw: their internal layers are the
 *                System32 libraries. Then it starts the copy in C:\BundleB\ and expects 42 from it.
 *   C:\BundleB\  with ReShade-style proxies d3d9.dll and dxgi.dll (tests/windows/bundle_proxy.c) that load the System32 libraries by path and pass the calls on;
 *                the program draws through them with Direct3D 9 and 11.
 * Both parts check GetModuleHandle and GetModuleFileName for the two libraries of one name. Ends with exit code 42 if every check passed. Built for both widths;
 * nothing is imported but kernel32, user32 and the C runtime (the Direct3D functions are looked up), and dxfixtures.h has dx_vs_dxbc and dx_ps_color_dxbc.
 * The DirectDraw and Direct3D 8 draws are in tests/windows/bundletest_legacy.c, compiled twice (-DBUNDLE_DDRAW, -DBUNDLE_D3D8):
 *   x86_64-w64-mingw32-gcc -O1 -DBUNDLE_DDRAW -c -o ddraw.o tests/windows/bundletest_legacy.c; ... -DBUNDLE_D3D8 -c -o d3d8.o ...
 *   x86_64-w64-mingw32-gcc -O1 -I <folder of dxfixtures.h> -o bundletest.exe tests/windows/bundletest.c ddraw.o d3d8.o -ldxguid -luuid -luser32 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <d3d9.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include "dxfixtures.h"

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "bundletest: ok: %s\n" : "bundletest: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

#define SIZE 64
static HWND g_window;
static WCHAR g_folder[MAX_PATH], g_system[MAX_PATH];      /* the program's folder and System32, without the backslash at the end */
/* the folder GetModuleFileName says the system libraries are in: System32, SysWOW64 for a 32-bit program (as on Windows) */
static WCHAR g_listed[MAX_PATH];

static void path_in(WCHAR *out, const WCHAR *folder, const WCHAR *file) {
  lstrcpyW(out, folder);
  lstrcatW(out, L"\\");
  lstrcatW(out, file);
}
/* Is the library at `module` the file `folder`\`file` (GetModuleFileName, any case)? */
static int file_is(HMODULE module, const WCHAR *folder, const WCHAR *file) {
  WCHAR expected[MAX_PATH * 2], got[MAX_PATH * 2];
  path_in(expected, folder, file);
  const DWORD n = GetModuleFileNameW(module, got, MAX_PATH * 2);
  if (n == 0 || lstrcmpiW(got, expected) != 0) {
    printf("bundletest: GetModuleFileName: %ls, expected %ls\n", n ? got : L"(none)", expected);
    return 0;
  }
  return 1;
}
static HMODULE system_library(const WCHAR *file) {
  WCHAR path[MAX_PATH * 2];
  path_in(path, g_system, file);
  return LoadLibraryW(path);
}
static HMODULE system_handle(const WCHAR *file) {
  WCHAR path[MAX_PATH * 2];
  path_in(path, g_system, file);
  return GetModuleHandleW(path);
}
static int near_colour(DWORD p, DWORD want) {
  for (int s = 0; s < 24; s += 8) {
    const int a = (int)((p >> s) & 255), b = (int)((want >> s) & 255);
    if (a - b > 3 || b - a > 3) return 0;
  }
  return 1;
}
#define PROC(module, name) ((void *)GetProcAddress(module, name))

/* (tests/windows/bundletest_legacy.c: their headers cannot be in one file with d3d9.h) */
int bundle_draw_ddraw(HWND window);
int bundle_draw_d3d8(HMODULE d3d8, HWND window);
static int draw_ddraw(void) { return bundle_draw_ddraw(g_window); }
static int draw_d3d8(HMODULE d3d8) { return bundle_draw_d3d8(d3d8, g_window); }

/* ---- Direct3D 9: a device on the window, a quad drawn into the left half, the back buffer read back ---- */
typedef struct { float x, y, z, rhw; DWORD diffuse; } TlVertex;
static int draw_d3d9(IDirect3D9 *d3d) {
  D3DPRESENT_PARAMETERS pp;
  memset(&pp, 0, sizeof pp);
  pp.BackBufferWidth = pp.BackBufferHeight = SIZE;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_window;
  pp.Windowed = TRUE;
  pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
  IDirect3DDevice9 *device = NULL;
  int ok = 0;
  if (SUCCEEDED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, g_window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device)) && device) {
    const TlVertex q[4] = {{0, 0, 0.5f, 1, 0xFF00FF00}, {32, 0, 0.5f, 1, 0xFF00FF00}, {0, 64, 0.5f, 1, 0xFF00FF00}, {32, 64, 0.5f, 1, 0xFF00FF00}};
    IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xFFFFFF00, 1.0f, 0);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_BeginScene(device);
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    IDirect3DDevice9_EndScene(device);
    IDirect3DSurface9 *back = NULL;
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
      if (SUCCEEDED(IDirect3DSurface9_LockRect(back, &locked, NULL, D3DLOCK_READONLY))) {
        const DWORD *row = (const DWORD *)((const BYTE *)locked.pBits + 10 * locked.Pitch);
        ok = near_colour(row[10], 0x00FF00) && near_colour(row[50], 0xFFFF00);
        if (!ok) printf("bundletest: d3d9 read back %#lx %#lx\n", (unsigned long)row[10], (unsigned long)row[50]);
        IDirect3DSurface9_UnlockRect(back);
      }
      IDirect3DSurface9_Release(back);
    }
    IDirect3DDevice9_Release(device);
  }
  return ok;
}

/* ---- Direct3D 11 and 10: a triangle drawn into a texture (tests/data/dxbc vs.hlsl and ps_color.hlsl) and read back through a staging texture ---- */
typedef struct { float x, y, z, r, g, b, a; } Vertex;
static const Vertex kTriangle[3] = {{-0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 1, 0, 1}};
static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
/* (the middle is the triangle's, a top corner the clear colour's; B8G8R8A8) */
static int read_ok(const BYTE *data, UINT pitch, const char *what) {
  DWORD middle, corner;
  memcpy(&middle, data + 32 * pitch + 32 * 4, 4);
  memcpy(&corner, data + 2 * pitch + 2 * 4, 4);
  const int ok = near_colour(middle, 0x00FF00) && near_colour(corner, 0x0000FF);
  if (!ok) printf("bundletest: %s read back %#lx %#lx\n", what, (unsigned long)middle, (unsigned long)corner);
  return ok;
}
static int draw_d3d11(HMODULE d3d11) {
  PFN_D3D11_CREATE_DEVICE create = d3d11 ? (PFN_D3D11_CREATE_DEVICE)PROC(d3d11, "D3D11CreateDevice") : NULL;
  ID3D11Device *device = NULL;
  ID3D11DeviceContext *context = NULL;
  if (!create || FAILED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context)) || !device || !context) return 0;
  D3D11_TEXTURE2D_DESC target = {SIZE, SIZE, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
  D3D11_TEXTURE2D_DESC staging = {SIZE, SIZE, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
  const D3D11_INPUT_ELEMENT_DESC elements[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  D3D11_BUFFER_DESC vb = {sizeof kTriangle, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0}, cb = {sizeof kIdentity, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA vertices = {kTriangle, 0, 0}, constants = {kIdentity, 0, 0};
  D3D11_RASTERIZER_DESC raster = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
  ID3D11Texture2D *rt = NULL, *copy = NULL;
  ID3D11RenderTargetView *view = NULL;
  ID3D11VertexShader *vs = NULL;
  ID3D11PixelShader *ps = NULL;
  ID3D11InputLayout *layout = NULL;
  ID3D11Buffer *vbuffer = NULL, *cbuffer = NULL;
  ID3D11RasterizerState *state = NULL;
  int ok = 0;
  if (SUCCEEDED(ID3D11Device_CreateTexture2D(device, &target, NULL, &rt)) && SUCCEEDED(ID3D11Device_CreateTexture2D(device, &staging, NULL, &copy)) &&
      SUCCEEDED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)rt, NULL, &view)) &&
      SUCCEEDED(ID3D11Device_CreateVertexShader(device, dx_vs_dxbc, sizeof dx_vs_dxbc, NULL, &vs)) &&
      SUCCEEDED(ID3D11Device_CreatePixelShader(device, dx_ps_color_dxbc, sizeof dx_ps_color_dxbc, NULL, &ps)) &&
      SUCCEEDED(ID3D11Device_CreateInputLayout(device, elements, 2, dx_vs_dxbc, sizeof dx_vs_dxbc, &layout)) &&
      SUCCEEDED(ID3D11Device_CreateBuffer(device, &vb, &vertices, &vbuffer)) && SUCCEEDED(ID3D11Device_CreateBuffer(device, &cb, &constants, &cbuffer)) &&
      SUCCEEDED(ID3D11Device_CreateRasterizerState(device, &raster, &state))) {
    const float blue[4] = {0, 0, 1, 1};
    const UINT stride = sizeof(Vertex), offset = 0;
    D3D11_VIEWPORT viewport = {0, 0, SIZE, SIZE, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(context, view, blue);
    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &view, NULL);
    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
    ID3D11DeviceContext_RSSetState(context, state);
    ID3D11DeviceContext_IASetInputLayout(context, layout);
    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &vbuffer, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &cbuffer);
    ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
    ID3D11DeviceContext_Draw(context, 3, 0);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)copy, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)copy, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
      ok = read_ok(mapped.pData, mapped.RowPitch, "d3d11");
      ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)copy, 0);
    }
  }
  if (state) ID3D11RasterizerState_Release(state);
  if (cbuffer) ID3D11Buffer_Release(cbuffer);
  if (vbuffer) ID3D11Buffer_Release(vbuffer);
  if (layout) ID3D11InputLayout_Release(layout);
  if (ps) ID3D11PixelShader_Release(ps);
  if (vs) ID3D11VertexShader_Release(vs);
  if (view) ID3D11RenderTargetView_Release(view);
  if (copy) ID3D11Texture2D_Release(copy);
  if (rt) ID3D11Texture2D_Release(rt);
  ID3D11DeviceContext_Release(context);
  ID3D11Device_Release(device);
  return ok;
}
static int draw_d3d10(ID3D10Device *device) {
  D3D10_TEXTURE2D_DESC target = {SIZE, SIZE, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET, 0, 0};
  D3D10_TEXTURE2D_DESC staging = {SIZE, SIZE, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D10_USAGE_STAGING, 0, D3D10_CPU_ACCESS_READ, 0};
  const D3D10_INPUT_ELEMENT_DESC elements[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
                                                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0}};
  D3D10_BUFFER_DESC vb = {sizeof kTriangle, D3D10_USAGE_DEFAULT, D3D10_BIND_VERTEX_BUFFER, 0, 0}, cb = {sizeof kIdentity, D3D10_USAGE_DEFAULT, D3D10_BIND_CONSTANT_BUFFER, 0, 0};
  D3D10_SUBRESOURCE_DATA vertices = {kTriangle, 0, 0}, constants = {kIdentity, 0, 0};
  D3D10_RASTERIZER_DESC raster = {D3D10_FILL_SOLID, D3D10_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
  ID3D10Texture2D *rt = NULL, *copy = NULL;
  ID3D10RenderTargetView *view = NULL;
  ID3D10VertexShader *vs = NULL;
  ID3D10PixelShader *ps = NULL;
  ID3D10InputLayout *layout = NULL;
  ID3D10Buffer *vbuffer = NULL, *cbuffer = NULL;
  ID3D10RasterizerState *state = NULL;
  int ok = 0;
  if (SUCCEEDED(ID3D10Device_CreateTexture2D(device, &target, NULL, &rt)) && SUCCEEDED(ID3D10Device_CreateTexture2D(device, &staging, NULL, &copy)) &&
      SUCCEEDED(ID3D10Device_CreateRenderTargetView(device, (ID3D10Resource *)rt, NULL, &view)) &&
      SUCCEEDED(ID3D10Device_CreateVertexShader(device, dx_vs_dxbc, sizeof dx_vs_dxbc, &vs)) &&
      SUCCEEDED(ID3D10Device_CreatePixelShader(device, dx_ps_color_dxbc, sizeof dx_ps_color_dxbc, &ps)) &&
      SUCCEEDED(ID3D10Device_CreateInputLayout(device, elements, 2, dx_vs_dxbc, sizeof dx_vs_dxbc, &layout)) &&
      SUCCEEDED(ID3D10Device_CreateBuffer(device, &vb, &vertices, &vbuffer)) && SUCCEEDED(ID3D10Device_CreateBuffer(device, &cb, &constants, &cbuffer)) &&
      SUCCEEDED(ID3D10Device_CreateRasterizerState(device, &raster, &state))) {
    const float blue[4] = {0, 0, 1, 1};
    const UINT stride = sizeof(Vertex), offset = 0;
    D3D10_VIEWPORT viewport = {0, 0, SIZE, SIZE, 0, 1};
    ID3D10Device_ClearRenderTargetView(device, view, blue);
    ID3D10Device_OMSetRenderTargets(device, 1, &view, NULL);
    ID3D10Device_RSSetViewports(device, 1, &viewport);
    ID3D10Device_RSSetState(device, state);
    ID3D10Device_IASetInputLayout(device, layout);
    ID3D10Device_IASetVertexBuffers(device, 0, 1, &vbuffer, &stride, &offset);
    ID3D10Device_IASetPrimitiveTopology(device, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D10Device_VSSetShader(device, vs);
    ID3D10Device_VSSetConstantBuffers(device, 0, 1, &cbuffer);
    ID3D10Device_PSSetShader(device, ps);
    ID3D10Device_Draw(device, 3, 0);
    ID3D10Device_CopyResource(device, (ID3D10Resource *)copy, (ID3D10Resource *)rt);
    D3D10_MAPPED_TEXTURE2D mapped;
    if (SUCCEEDED(ID3D10Texture2D_Map(copy, 0, D3D10_MAP_READ, 0, &mapped)) && mapped.pData) {
      ok = read_ok(mapped.pData, mapped.RowPitch, "d3d10");
      ID3D10Texture2D_Unmap(copy, 0);
    }
  }
  if (state) ID3D10RasterizerState_Release(state);
  if (cbuffer) ID3D10Buffer_Release(cbuffer);
  if (vbuffer) ID3D10Buffer_Release(vbuffer);
  if (layout) ID3D10InputLayout_Release(layout);
  if (ps) ID3D10PixelShader_Release(ps);
  if (vs) ID3D10VertexShader_Release(vs);
  if (view) ID3D10RenderTargetView_Release(view);
  if (copy) ID3D10Texture2D_Release(copy);
  if (rt) ID3D10Texture2D_Release(rt);
  return ok;
}

/* ---- C:\BundleA: foreign libraries beside the program ---- */
static void part_foreign(void) {
  HMODULE own9 = LoadLibraryA("d3d9.dll"), own12 = LoadLibraryA("d3d12.dll"), own11 = LoadLibraryA("d3d11.dll"), own10 = LoadLibraryA("d3d10.dll");
  check(own9 && PROC(own9, "TcosForeignLibrary") && file_is(own9, g_folder, L"d3d9.dll"), "LoadLibrary(\"d3d9.dll\") is the program's own d3d9.dll");
  check(own12 && PROC(own12, "TcosForeignLibrary") && file_is(own12, g_folder, L"d3d12.dll") && own11 && PROC(own11, "TcosForeignLibrary") &&
            file_is(own11, g_folder, L"d3d11.dll") && own10 && PROC(own10, "TcosForeignLibrary") && file_is(own10, g_folder, L"d3d10.dll"),
        "so are d3d12.dll, d3d11.dll and d3d10.dll");
  if (!own9 || !own12 || !own11 || !own10) return;
  typedef void *(WINAPI *Create9)(UINT);
  Create9 create9 = (Create9)PROC(own9, "Direct3DCreate9");
  check(create9 && create9(D3D_SDK_VERSION) == NULL, "and the program's Direct3DCreate9 is the foreign one (it fails)");
  PFN_D3D12_CREATE_DEVICE create12 = (PFN_D3D12_CREATE_DEVICE)PROC(own12, "D3D12CreateDevice");
  void *nothing = NULL;
  check(create12 && create12(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, &nothing) == E_FAIL && !PROC(own12, "TcosD3D12DeviceFeatures"), "the foreign d3d12.dll has no private exports");

  // ddraw.dll (Direct3D 7 draws through this OS's d3d9.dll, which it loads by its System32 path)
  check(draw_ddraw(), "ddraw.dll: a Direct3D 7 device draws and reads back, whatever d3d9.dll the program has");
  HMODULE system9 = system_handle(L"d3d9.dll");
  check(system9 && system9 != own9 && !PROC(system9, "TcosForeignLibrary") && file_is(system9, g_listed, L"d3d9.dll"),
        "the System32 d3d9.dll is a second library of that name (GetModuleHandle and GetModuleFileName of its path)");
  check(GetModuleHandleA("d3d9.dll") == own9 && GetModuleHandleW(L"D3D9") == own9, "GetModuleHandle(\"d3d9.dll\") is still the program's (the first of that name)");
  // d3d8.dll (its d3d9 import is the System32 one)
  HMODULE d3d8 = LoadLibraryA("d3d8.dll");
  check(d3d8 && file_is(d3d8, g_listed, L"d3d8.dll"), "d3d8.dll is the System32 one");
  check(draw_d3d8(d3d8), "d3d8.dll: a device draws and reads back");
  // d3d11.dll of System32, by its path (its d3d12 import is the System32 one, its dxgi Windows' own)
  HMODULE system11 = system_library(L"d3d11.dll");
  check(system11 && system11 != own11 && file_is(system11, g_listed, L"d3d11.dll") && GetModuleHandleA("d3d11.dll") == own11 && system_handle(L"d3d11.dll") == system11,
        "LoadLibrary of the System32 d3d11.dll by its path while the program's is loaded: another library");
  check(draw_d3d11(system11), "d3d11.dll: a triangle drawn and read back");
  HMODULE system12 = system_handle(L"d3d12.dll");
  check(system12 && system12 != own12 && file_is(system12, g_listed, L"d3d12.dll") && PROC(system12, "TcosD3D12DeviceFeatures"),
        "the d3d12.dll that d3d9 and d3d11 are built on is the System32 one");
  check(GetModuleHandleA("d3d12.dll") == own12 && LoadLibraryA("d3d12.dll") == own12, "while the program's d3d12.dll is the one of that name");
  FreeLibrary(own12);
  // d3d10.dll of System32 by path, and d3d10_1.dll (on d3d10core.dll, on d3d11.dll: the System32 one)
  HMODULE system10 = system_library(L"d3d10.dll");
  typedef HRESULT (WINAPI *Create10)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device **);
  typedef HRESULT (WINAPI *Create10_1)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT, ID3D10Device1 **);
  Create10 create10 = system10 ? (Create10)PROC(system10, "D3D10CreateDevice") : NULL;
  ID3D10Device *device10 = NULL;
  check(system10 && system10 != own10 && create10 && create10(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &device10) == S_OK && device10,
        "the System32 d3d10.dll makes a device");
  check(device10 && draw_d3d10(device10), "d3d10.dll: a triangle drawn and read back");
  if (device10) ID3D10Device_Release(device10);
  HMODULE d3d10_1 = LoadLibraryA("d3d10_1.dll");
  Create10_1 create10_1 = d3d10_1 ? (Create10_1)PROC(d3d10_1, "D3D10CreateDevice1") : NULL;
  ID3D10Device1 *device10_1 = NULL;
  check(create10_1 && create10_1(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &device10_1) == S_OK && device10_1,
        "d3d10_1.dll makes a device");
  check(device10_1 && draw_d3d10((ID3D10Device *)device10_1), "d3d10_1.dll: a triangle drawn and read back");
  if (device10_1) ID3D10Device1_Release(device10_1);
  // GetProcAddress of their forwarders: d3d10core's OpenAdapter10 is the System32 d3d11's, d3d10_1's D3D10CreateEffectFromMemory the System32 d3d10's
  HMODULE d3d10core = GetModuleHandleA("d3d10core.dll");
  void *forwarded = d3d10core ? PROC(d3d10core, "OpenAdapter10") : NULL;
  check(d3d10core && file_is(d3d10core, g_listed, L"d3d10core.dll") && forwarded && forwarded == PROC(system11, "OpenAdapter10") && !PROC(own11, "OpenAdapter10"),
        "GetProcAddress(d3d10core, \"OpenAdapter10\"): the forwarder to d3d11 is the System32 d3d11's");
  forwarded = d3d10_1 ? PROC(d3d10_1, "D3D10CreateEffectFromMemory") : NULL;
  check(forwarded && system10 && forwarded == PROC(system10, "TcosD3D10_1CreateEffectFromMemory"), "GetProcAddress(d3d10_1, \"D3D10CreateEffectFromMemory\"): the System32 d3d10's");
  check(LoadLibraryA("d3d9.dll") == own9 && LoadLibraryA("d3d11.dll") == own11 && LoadLibraryA("d3d10.dll") == own10, "LoadLibrary by name: still the program's libraries");
  FreeLibrary(own9);
  FreeLibrary(own11);
  FreeLibrary(own10);
  FreeLibrary(system11);
  FreeLibrary(system10);
  FreeLibrary(d3d10_1);
  FreeLibrary(d3d8);
}

/* ---- C:\BundleB: proxies of d3d9.dll and dxgi.dll beside the program ---- */
static void part_proxies(void) {
  HMODULE proxy9 = LoadLibraryA("d3d9.dll");
  typedef LONG (WINAPI *Calls)(void);
  typedef HMODULE (WINAPI *System)(void);
  Calls calls9 = proxy9 ? (Calls)PROC(proxy9, "TcosProxyCalls") : NULL;
  System real9_of = proxy9 ? (System)PROC(proxy9, "TcosProxySystemLibrary") : NULL;
  check(calls9 && real9_of && file_is(proxy9, g_folder, L"d3d9.dll"), "LoadLibrary(\"d3d9.dll\") is the proxy beside the program");
  if (!calls9 || !real9_of) return;
  typedef IDirect3D9 *(WINAPI *Create9)(UINT);
  Create9 create9 = (Create9)PROC(proxy9, "Direct3DCreate9");
  IDirect3D9 *d3d9 = create9 ? create9(D3D_SDK_VERSION) : NULL;
  HMODULE real9 = real9_of();
  check(d3d9 && real9 && real9 != proxy9 && file_is(real9, g_listed, L"d3d9.dll") && system_handle(L"d3d9.dll") == real9 && GetModuleHandleA("d3d9.dll") == proxy9,
        "the proxy loads the System32 d3d9.dll by its path: a second library of that name, not itself");
  if (!d3d9) return;
  const LONG before = calls9();
  check(draw_d3d9(d3d9), "Direct3D 9 through the proxy: a device draws and reads back");
  check(calls9() > before, "the device was made through the proxy's IDirect3D9");
  IUnknown *other = NULL;
  const LONG before_qi = calls9();
  const HRESULT qi = IDirect3D9_QueryInterface(d3d9, &IID_IDirect3DDevice9, (void **)&other);      /* (an interface the wrapper does not know: passed on) */
  check(calls9() == before_qi + 1 && qi == E_NOINTERFACE && !other, "the wrapper's QueryInterface passes on what it does not know");
  IDirect3D9_Release(d3d9);
  const LONG before_ddraw = calls9();
  check(draw_ddraw() && calls9() == before_ddraw, "ddraw.dll draws on the System32 d3d9.dll, not through the program's proxy");

  HMODULE proxy_dxgi = LoadLibraryA("dxgi.dll");
  Calls calls_dxgi = proxy_dxgi ? (Calls)PROC(proxy_dxgi, "TcosProxyCalls") : NULL;
  System real_dxgi_of = proxy_dxgi ? (System)PROC(proxy_dxgi, "TcosProxySystemLibrary") : NULL;
  check(calls_dxgi && real_dxgi_of && file_is(proxy_dxgi, g_folder, L"dxgi.dll"), "LoadLibrary(\"dxgi.dll\") is the proxy beside the program");
  if (!calls_dxgi || !real_dxgi_of) return;
  HMODULE d3d11 = LoadLibraryA("d3d11.dll");
  check(d3d11 && file_is(d3d11, g_listed, L"d3d11.dll"), "d3d11.dll is the System32 one (the program has none)");
  check(draw_d3d11(d3d11), "Direct3D 11 with the proxy dxgi.dll: a triangle drawn and read back");
  HMODULE real_dxgi = real_dxgi_of();
  check(calls_dxgi() > 0 && real_dxgi && real_dxgi != proxy_dxgi && file_is(real_dxgi, g_listed, L"dxgi.dll") && system_handle(L"dxgi.dll") == real_dxgi &&
            GetModuleHandleA("dxgi.dll") == proxy_dxgi,
        "d3d11.dll's dxgi.dll is the program's proxy (Windows' rule), which passes the calls on to the System32 one");
  FreeLibrary(d3d11);
  FreeLibrary(proxy_dxgi);
  FreeLibrary(proxy9);
}

int main(int argc, char **argv) {
  const int proxies = argc > 1 && strcmp(argv[1], "b") == 0;
  DWORD n = GetModuleFileNameW(NULL, g_folder, MAX_PATH);
  while (n > 0 && g_folder[n - 1] != '\\') --n;
  if (n > 0) g_folder[n - 1] = 0;
  n = GetSystemDirectoryW(g_system, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) g_system[0] = 0;
  lstrcpyW(g_listed, g_system);
  if (sizeof(void *) == 4 && n > 8) lstrcpyW(g_listed + n - 8, L"SysWOW64");      /* (C:\Windows\System32 -> C:\Windows\SysWOW64) */
  printf("bundletest: %s, program folder %ls, system folder %ls (%d-bit)\n", proxies ? "proxies" : "foreign libraries", g_folder, g_system, (int)sizeof(void *) * 8);
  WNDCLASSW cls;
  memset(&cls, 0, sizeof cls);
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(NULL);
  cls.lpszClassName = L"bundletest";
  RegisterClassW(&cls);
  g_window = CreateWindowExW(0, L"bundletest", L"bundletest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 200, 160, NULL, NULL, cls.hInstance, NULL);
  check(g_window != NULL, "a window");
  if (proxies) {
    part_proxies();
  } else {
    part_foreign();
    // The second part, from the other folder: a program of its own.
    WCHAR other[MAX_PATH * 2], line[MAX_PATH * 2 + 8];
    lstrcpyW(other, g_folder);
    n = (DWORD)lstrlenW(other);
    while (n > 0 && other[n - 1] != '\\') --n;
    other[n] = 0;
    lstrcatW(other, L"BundleB\\bundletest.exe");
    wsprintfW(line, L"\"%s\" b", other);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    DWORD code = 0;
    const BOOL started = CreateProcessW(other, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (started) {
      WaitForSingleObject(pi.hProcess, 120000);
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
    check(started && code == 42, "the proxies' part (C:\\BundleB\\bundletest.exe) passed");
  }
  if (g_window) DestroyWindow(g_window);
  if (g_failed) printf("bundletest: %d of %d checks failed\n", g_failed, g_passed + g_failed);
  else printf("bundletest: all %d checks passed\n", g_passed);
  fflush(stdout);
  return g_failed ? 1 : 42;
}
