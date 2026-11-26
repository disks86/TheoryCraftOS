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

/* Direct3D 10 (d3d10.dll, d3d10_1.dll, d3d10core.dll over d3d11.dll's device) on the real kernel: the device and its other faces, buffers, textures and views, a triangle
 * drawn into a texture and read back by pixel, clears, a sampled texture, the state objects, depth, a geometry shader and stream output, queries, Map of buffers and
 * textures, Direct3D 10 <-> 11 QueryInterface of the same objects, state blocks, a swap chain's Present, and a triangle drawn through an effect (ID3D10Effect). Built for
 * both widths; ends with exit code 42 if every check passed. dxfixtures.h (the shaders of tests/data/dxbc and the effects of tests/data/fx) is made by the script that
 * builds it (tests/emulated_dx.sh, tests/d3d12_qemu.sh, tests/wow64_qemu.sh).
 *   x86_64-w64-mingw32-gcc -O1 -I <folder of dxfixtures.h> -o d3d10test.exe tests/windows/d3d10test.c -ld3d10 -ld3d11 -ldxgi -luuid -ldxguid -luser32 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d10_1.h>
#include <d3d10effect.h>
#include <d3d11.h>
#include <dxgi.h>
#include "dxfixtures.h"

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "d3d10test: ok: %s\n" : "d3d10test: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

#define SIZE 64
static ID3D10Device *g_device;
static ID3D10Texture2D *g_target, *g_staging;
static ID3D10RenderTargetView *g_target_view;
static ID3D10VertexShader *g_vs;
static ID3D10PixelShader *g_ps_color, *g_ps_tex;
static ID3D10InputLayout *g_layout;
static ID3D10Buffer *g_matrix;
static ID3D10RasterizerState *g_no_cull;
static UINT32 g_pixels[SIZE * SIZE];

typedef struct { float x, y, z, r, g, b, a; } Vertex;
static const D3D10_INPUT_ELEMENT_DESC g_elements[2] = {
  {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
  {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0},
};

static ULONG refs(void *object) { IUnknown *u = object; IUnknown_AddRef(u); return IUnknown_Release(u); }
static ID3D10Buffer *make_buffer(UINT bind, D3D10_USAGE usage, UINT cpu, const void *data, UINT bytes) {
  D3D10_BUFFER_DESC desc = {bytes, usage, bind, cpu, 0};
  D3D10_SUBRESOURCE_DATA initial = {data, 0, 0};
  ID3D10Buffer *buffer = NULL;
  ID3D10Device_CreateBuffer(g_device, &desc, data ? &initial : NULL, &buffer);
  return buffer;
}
static void set_matrix(float sx, float sy, float tx, float ty) {
  const float m[16] = {sx, 0, 0, tx, 0, sy, 0, ty, 0, 0, 1, 0, 0, 0, 0, 1};
  void *data = NULL;
  if (SUCCEEDED(ID3D10Buffer_Map(g_matrix, D3D10_MAP_WRITE_DISCARD, 0, &data)) && data) {
    memcpy(data, m, sizeof m);
    ID3D10Buffer_Unmap(g_matrix);
  }
}
/* The render target's pixels (R8G8B8A8, as 0xAABBGGRR), by way of a staging texture and ID3D10Texture2D::Map. */
static int read_target(void) {
  D3D10_MAPPED_TEXTURE2D mapped;
  ID3D10Device_CopyResource(g_device, (ID3D10Resource *)g_staging, (ID3D10Resource *)g_target);
  if (FAILED(ID3D10Texture2D_Map(g_staging, 0, D3D10_MAP_READ, 0, &mapped)) || !mapped.pData) return 0;
  for (int y = 0; y < SIZE; ++y) memcpy(g_pixels + y * SIZE, (const BYTE *)mapped.pData + (size_t)y * mapped.RowPitch, SIZE * 4);
  ID3D10Texture2D_Unmap(g_staging, 0);
  return 1;
}
static UINT32 pixel(int x, int y) { return g_pixels[y * SIZE + x]; }
static int red(UINT32 p) { return (int)(p & 255); }
static int green(UINT32 p) { return (int)((p >> 8) & 255); }
static int blue(UINT32 p) { return (int)((p >> 16) & 255); }
static int close_to(int a, int b) { return a - b <= 3 && b - a <= 3; }
static void clear(float r, float g, float b) {
  const float colour[4] = {r, g, b, 1};
  ID3D10Device_ClearRenderTargetView(g_device, g_target_view, colour);
}
static void begin(ID3D10DepthStencilView *depth) {
  D3D10_VIEWPORT viewport = {0, 0, SIZE, SIZE, 0, 1};
  ID3D10Device_OMSetRenderTargets(g_device, 1, &g_target_view, depth);
  ID3D10Device_RSSetViewports(g_device, 1, &viewport);
  ID3D10Device_RSSetState(g_device, g_no_cull);
  ID3D10Device_VSSetShader(g_device, g_vs);
  ID3D10Device_GSSetShader(g_device, NULL);
  ID3D10Device_VSSetConstantBuffers(g_device, 0, 1, &g_matrix);
  ID3D10Device_IASetPrimitiveTopology(g_device, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D10Device_IASetInputLayout(g_device, g_layout);
  ID3D10Device_OMSetBlendState(g_device, NULL, NULL, 0xFFFFFFFFu);
  ID3D10Device_OMSetDepthStencilState(g_device, NULL, 0);
  set_matrix(1, 1, 0, 0);
}
static void bind_vertices(ID3D10Buffer *vertices) {
  const UINT stride = sizeof(Vertex), offset = 0;
  ID3D10Device_IASetVertexBuffers(g_device, 0, 1, &vertices, &stride, &offset);
}
/* A triangle over the middle of the target, in one colour, at depth z. */
static ID3D10Buffer *triangle(float z, float r, float g, float b) {
  const Vertex v[3] = {{-0.8f, -0.8f, z, r, g, b, 1}, {0.8f, -0.8f, z, r, g, b, 1}, {0.0f, 0.8f, z, r, g, b, 1}};
  return make_buffer(D3D10_BIND_VERTEX_BUFFER, D3D10_USAGE_DEFAULT, 0, v, sizeof v);
}

static void test_device(void) {
  ID3D10Device *other = NULL;
  check(D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION + 1, &other) == E_INVALIDARG && !other,
        "D3D10CreateDevice refuses another SDK version");
  const HRESULT made = D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &g_device);
  check(made == S_OK && g_device, "D3D10CreateDevice: a hardware device");
  if (!g_device) return;
  check(ID3D10Device_GetCreationFlags(g_device) == 0, "GetCreationFlags answers what it was made with");
  ID3D10Device1 *d1 = NULL;
  check(SUCCEEDED(ID3D10Device_QueryInterface(g_device, &IID_ID3D10Device1, (void **)&d1)) && d1 && ID3D10Device1_GetFeatureLevel(d1) == D3D10_FEATURE_LEVEL_10_0,
        "the device is ID3D10Device1 too, at feature level 10_0");
  if (d1) ID3D10Device1_Release(d1);
  ID3D11Device *d11 = NULL;
  ID3D10Device *back = NULL;
  check(SUCCEEDED(ID3D10Device_QueryInterface(g_device, &IID_ID3D11Device, (void **)&d11)) && d11 && ID3D11Device_GetFeatureLevel(d11) == D3D_FEATURE_LEVEL_10_0 &&
            SUCCEEDED(ID3D11Device_QueryInterface(d11, &IID_ID3D10Device, (void **)&back)) && back == g_device,
        "ID3D11Device and back to the same ID3D10Device");
  if (back) ID3D10Device_Release(back);
  if (d11) ID3D11Device_Release(d11);
  IUnknown *u1 = NULL, *u2 = NULL;
  ID3D10Device_QueryInterface(g_device, &IID_IUnknown, (void **)&u1);
  if (u1) IUnknown_QueryInterface(u1, &IID_IUnknown, (void **)&u2);
  check(u1 && u1 == u2, "one IUnknown");
  if (u1) IUnknown_Release(u1);
  if (u2) IUnknown_Release(u2);
  ID3D10Multithread *mt = NULL;
  check(SUCCEEDED(ID3D10Device_QueryInterface(g_device, &IID_ID3D10Multithread, (void **)&mt)) && mt && ID3D10Multithread_GetMultithreadProtected(mt),
        "ID3D10Multithread: a Direct3D 10 device is thread-safe unless made single-threaded");
  if (mt) { ID3D10Multithread_Enter(mt); ID3D10Multithread_Leave(mt); ID3D10Multithread_Release(mt); }
  IDXGIDevice *dxgi = NULL;
  IDXGIAdapter *adapter = NULL;
  check(SUCCEEDED(ID3D10Device_QueryInterface(g_device, &IID_IDXGIDevice, (void **)&dxgi)) && dxgi && SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)) && adapter,
        "IDXGIDevice and its adapter");
  if (adapter) {
    ID3D10Device *on_adapter = NULL;
    check(D3D10CreateDevice(adapter, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &on_adapter) == S_OK && on_adapter, "a device on the adapter");
    if (on_adapter) check(ID3D10Device_Release(on_adapter) == 0, "... gone with its last reference");
    check(D3D10CreateDevice(adapter, D3D10_DRIVER_TYPE_REFERENCE, NULL, 0, D3D10_SDK_VERSION, &other) == E_INVALIDARG, "an adapter goes with the hardware driver type only");
    IDXGIAdapter_Release(adapter);
  }
  if (dxgi) IDXGIDevice_Release(dxgi);
  check(D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, D3D10_CREATE_DEVICE_DEBUG, D3D10_SDK_VERSION, &other) == DXGI_ERROR_SDK_COMPONENT_MISSING,
        "the debug layer is the SDK's (not on this system)");
  ID3D10Device *single = NULL;
  if (SUCCEEDED(D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, D3D10_CREATE_DEVICE_SINGLETHREADED, D3D10_SDK_VERSION, &single)) && single) {
    ID3D10Multithread *m = NULL;
    ID3D10Device_QueryInterface(single, &IID_ID3D10Multithread, (void **)&m);
    check(m && !ID3D10Multithread_GetMultithreadProtected(m) && ID3D10Device_GetCreationFlags(single) == D3D10_CREATE_DEVICE_SINGLETHREADED, "a single-threaded device");
    if (m) ID3D10Multithread_Release(m);
    ID3D10Device_Release(single);
  }
  /* Direct3D 10.1: d3d10_1.dll's D3D10CreateDevice1 at 10_1 (the CPU renderer and virgl have 11_0 and up) */
  HMODULE m101 = LoadLibraryA("d3d10_1.dll");
  typedef HRESULT (WINAPI *Create1)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT, ID3D10Device1 **);
  Create1 create1 = m101 ? (Create1)(void (*)(void))GetProcAddress(m101, "D3D10CreateDevice1") : NULL;
  check(create1 != NULL, "d3d10_1.dll has D3D10CreateDevice1");
  if (create1) {
    ID3D10Device1 *dev1 = NULL;
    check(create1(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &dev1) == S_OK && dev1 &&
              ID3D10Device1_GetFeatureLevel(dev1) == D3D10_FEATURE_LEVEL_10_1,
          "D3D10CreateDevice1 at feature level 10_1");
    if (dev1) ID3D10Device1_Release(dev1);
    dev1 = (ID3D10Device1 *)1;
    check(create1(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, (D3D10_FEATURE_LEVEL1)0, D3D10_1_SDK_VERSION, &dev1) == E_INVALIDARG && !dev1, "... and refuses a level that is none");
  }
}

static void test_setup(void) {
  D3D10_TEXTURE2D_DESC desc = {SIZE, SIZE, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE, 0, 0};
  HRESULT hr = ID3D10Device_CreateTexture2D(g_device, &desc, NULL, &g_target);
  desc.Usage = D3D10_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
  if (SUCCEEDED(hr)) hr = ID3D10Device_CreateTexture2D(g_device, &desc, NULL, &g_staging);
  if (SUCCEEDED(hr)) hr = ID3D10Device_CreateRenderTargetView(g_device, (ID3D10Resource *)g_target, NULL, &g_target_view);
  check(SUCCEEDED(hr) && g_target && g_staging && g_target_view, "a render target, its view and a staging texture");
  check(ID3D10Device_CreateVertexShader(g_device, dx_vs_dxbc, sizeof dx_vs_dxbc, &g_vs) == S_OK && g_vs &&
            ID3D10Device_CreatePixelShader(g_device, dx_ps_color_dxbc, sizeof dx_ps_color_dxbc, &g_ps_color) == S_OK &&
            ID3D10Device_CreatePixelShader(g_device, dx_ps_tex_dxbc, sizeof dx_ps_tex_dxbc, &g_ps_tex) == S_OK,
        "shader model 4 vertex and pixel shaders");
  ID3D10PixelShader *wrong = (ID3D10PixelShader *)1;
  check(ID3D10Device_CreatePixelShader(g_device, dx_vs_dxbc, sizeof dx_vs_dxbc, &wrong) == E_INVALIDARG && !wrong, "a vertex shader's code is not a pixel shader");
  ID3D10VertexShader *sm5 = NULL;
  check(ID3D10Device_CreateVertexShader(g_device, dx_vs_uav_dxbc, sizeof dx_vs_uav_dxbc, &sm5) == E_INVALIDARG && !sm5, "shader model 5 is Direct3D 11's");
  ID3D10PixelShader *sm41 = NULL;
  check(ID3D10Device_CreatePixelShader(g_device, dx_ps_texarray_lod_dxbc, sizeof dx_ps_texarray_lod_dxbc, &sm41) == E_INVALIDARG && !sm41,
        "shader model 4.1 needs a 10.1 device");
  check(ID3D10Device_CreateInputLayout(g_device, g_elements, 2, dx_vs_dxbc, sizeof dx_vs_dxbc, &g_layout) == S_OK && g_layout, "an input layout");
  g_matrix = make_buffer(D3D10_BIND_CONSTANT_BUFFER, D3D10_USAGE_DYNAMIC, D3D10_CPU_ACCESS_WRITE, NULL, 64);
  const D3D10_RASTERIZER_DESC raster = {D3D10_FILL_SOLID, D3D10_CULL_NONE, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE};
  check(g_matrix && ID3D10Device_CreateRasterizerState(g_device, &raster, &g_no_cull) == S_OK && g_no_cull, "a dynamic constant buffer and a rasterizer state");
  D3D10_RASTERIZER_DESC got;
  memset(&got, 0xAB, sizeof got);
  if (g_no_cull) ID3D10RasterizerState_GetDesc(g_no_cull, &got);
  check(got.CullMode == D3D10_CULL_NONE && got.DepthClipEnable == TRUE && got.FillMode == D3D10_FILL_SOLID, "the rasterizer state's description");
}

static void test_clear_and_draw(void) {
  clear(0, 0, 1);
  check(read_target() && red(pixel(5, 5)) == 0 && blue(pixel(5, 5)) == 255 && blue(pixel(60, 60)) == 255, "ClearRenderTargetView, read back through Map of a staging texture");
  begin(NULL);
  ID3D10Device_PSSetShader(g_device, g_ps_color);
  ID3D10Buffer *t = triangle(0.5f, 1, 0, 0);
  bind_vertices(t);
  ID3D10Device_Draw(g_device, 3, 0);
  check(read_target() && red(pixel(32, 32)) == 255 && blue(pixel(32, 32)) == 0 && blue(pixel(2, 2)) == 255 && red(pixel(2, 2)) == 0,
        "a red triangle drawn: inside red, the corner still the clear colour");
  /* (the constant buffer moves it: scaled down to the top-left quarter) */
  clear(0, 0, 0);
  set_matrix(0.5f, 0.5f, -0.5f, 0.5f);
  ID3D10Device_Draw(g_device, 3, 0);
  check(read_target() && red(pixel(16, 16)) == 255 && red(pixel(48, 48)) == 0, "the matrix of a dynamic constant buffer, mapped WRITE_DISCARD");
  /* (indexed: a quad of two triangles over all of it) */
  const Vertex quad[4] = {{-1, -1, 0.5f, 0, 1, 0, 1}, {1, -1, 0.5f, 0, 1, 0, 1}, {-1, 1, 0.5f, 0, 1, 0, 1}, {1, 1, 0.5f, 0, 1, 0, 1}};
  const UINT16 indices[6] = {0, 1, 2, 2, 1, 3};
  ID3D10Buffer *vb = make_buffer(D3D10_BIND_VERTEX_BUFFER, D3D10_USAGE_IMMUTABLE, 0, quad, sizeof quad);
  ID3D10Buffer *ib = make_buffer(D3D10_BIND_INDEX_BUFFER, D3D10_USAGE_DEFAULT, 0, indices, sizeof indices);
  set_matrix(1, 1, 0, 0);
  bind_vertices(vb);
  ID3D10Device_IASetIndexBuffer(g_device, ib, DXGI_FORMAT_R16_UINT, 0);
  ID3D10Device_DrawIndexed(g_device, 6, 0, 0);
  check(read_target() && green(pixel(1, 1)) == 255 && green(pixel(62, 62)) == 255 && green(pixel(32, 5)) == 255, "DrawIndexed: a quad over the target");
  ID3D10Buffer *got_ib = NULL;
  DXGI_FORMAT format = 0;
  UINT offset = 7;
  ID3D10Device_IAGetIndexBuffer(g_device, &got_ib, &format, &offset);
  check(got_ib == ib && format == DXGI_FORMAT_R16_UINT && offset == 0, "IAGetIndexBuffer");
  if (got_ib) ID3D10Buffer_Release(got_ib);
  D3D10_VIEWPORT vp[2];
  UINT count = 2;
  ID3D10Device_RSGetViewports(g_device, &count, vp);
  check(count == 1 && vp[0].Width == SIZE && vp[0].TopLeftX == 0 && vp[0].MaxDepth == 1.0f, "RSGetViewports in whole pixels");
  /* (a viewport of the right half: Direct3D 10's integer viewport) */
  clear(0, 0, 0);
  const D3D10_VIEWPORT right = {32, 0, 32, 64, 0, 1};
  ID3D10Device_RSSetViewports(g_device, 1, &right);
  ID3D10Device_DrawIndexed(g_device, 6, 0, 0);
  check(read_target() && green(pixel(10, 32)) == 0 && green(pixel(50, 32)) == 255, "a viewport of the right half");
  ID3D10Buffer_Release(vb);
  ID3D10Buffer_Release(ib);
  ID3D10Buffer_Release(t);
}

static void test_texture(void) {
  /* (a 2 x 2 texture: red, green / blue, white; sampled with point filtering, the four quarters) */
  const UINT32 texels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};
  D3D10_TEXTURE2D_DESC desc = {2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_SHADER_RESOURCE, 0, 0};
  D3D10_SUBRESOURCE_DATA initial = {texels, 8, 0};
  ID3D10Texture2D *tex = NULL;
  ID3D10ShaderResourceView *view = NULL;
  ID3D10SamplerState *sampler = NULL;
  HRESULT hr = ID3D10Device_CreateTexture2D(g_device, &desc, &initial, &tex);
  D3D10_SHADER_RESOURCE_VIEW_DESC vd;
  memset(&vd, 0, sizeof vd);
  vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  vd.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
  vd.Texture2D.MipLevels = 1;
  if (SUCCEEDED(hr)) hr = ID3D10Device_CreateShaderResourceView(g_device, (ID3D10Resource *)tex, &vd, &view);
  const D3D10_SAMPLER_DESC sd = {D3D10_FILTER_MIN_MAG_MIP_POINT, D3D10_TEXTURE_ADDRESS_CLAMP, D3D10_TEXTURE_ADDRESS_CLAMP, D3D10_TEXTURE_ADDRESS_CLAMP, 0, 1,
                                 D3D10_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D10_FLOAT32_MAX};
  if (SUCCEEDED(hr)) hr = ID3D10Device_CreateSamplerState(g_device, &sd, &sampler);
  check(SUCCEEDED(hr) && tex && view && sampler, "a texture with its first texels, a view and a sampler");
  if (FAILED(hr)) return;
  D3D10_SHADER_RESOURCE_VIEW_DESC back;
  ID3D10ShaderResourceView_GetDesc(view, &back);
  check(back.ViewDimension == D3D10_SRV_DIMENSION_TEXTURE2D && back.Format == DXGI_FORMAT_R8G8B8A8_UNORM && back.Texture2D.MipLevels == 1, "the view's description");
  ID3D10Resource *res = NULL;
  ID3D10ShaderResourceView_GetResource(view, &res);
  check(res == (ID3D10Resource *)tex, "the view's resource is the texture's Direct3D 10 interface");
  if (res) ID3D10Resource_Release(res);
  /* (the vertex shader's texture coordinate is the colour's xy times 2: corners of colours (0,0) .. (0.5,0.5) map to the texture's corners) */
  const Vertex quad[6] = {{-1, 1, 0.5f, 0, 0, 1, 1}, {1, 1, 0.5f, 0.5f, 0, 1, 1}, {-1, -1, 0.5f, 0, 0.5f, 1, 1},
                          {-1, -1, 0.5f, 0, 0.5f, 1, 1}, {1, 1, 0.5f, 0.5f, 0, 1, 1}, {1, -1, 0.5f, 0.5f, 0.5f, 1, 1}};
  ID3D10Buffer *vb = make_buffer(D3D10_BIND_VERTEX_BUFFER, D3D10_USAGE_DEFAULT, 0, quad, sizeof quad);
  begin(NULL);
  bind_vertices(vb);
  ID3D10Device_PSSetShader(g_device, g_ps_tex);
  ID3D10Device_PSSetShaderResources(g_device, 0, 1, &view);
  ID3D10Device_PSSetSamplers(g_device, 0, 1, &sampler);
  clear(0, 0, 0);
  ID3D10Device_Draw(g_device, 6, 0);
  /* (the colour multiplies the texel: its red and green are the coordinates, blue 1 - so the blue quarter shows plainly) */
  read_target();
  check(blue(pixel(10, 54)) > 200 && red(pixel(10, 54)) == 0, "the texture sampled with point filtering (the blue texel)");
  /* (UpdateSubresource: the texture all white, through a Direct3D 10 box) */
  const UINT32 white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  const D3D10_BOX box = {0, 0, 0, 2, 2, 1};
  ID3D10Device_UpdateSubresource(g_device, (ID3D10Resource *)tex, 0, &box, white, 8, 0);
  clear(0, 0, 0);
  ID3D10Device_Draw(g_device, 6, 0);
  read_target();
  check(blue(pixel(10, 54)) > 200 && red(pixel(54, 10)) > 80 && blue(pixel(54, 10)) > 200, "UpdateSubresource of the texture");
  ID3D10ShaderResourceView *got[2] = {NULL, (ID3D10ShaderResourceView *)1};
  ID3D10Device_PSGetShaderResources(g_device, 0, 2, got);
  check(got[0] == view && got[1] == NULL, "PSGetShaderResources gives the Direct3D 10 view");
  if (got[0]) ID3D10ShaderResourceView_Release(got[0]);
  ID3D10SamplerState *gs = NULL;
  ID3D10Device_PSGetSamplers(g_device, 0, 1, &gs);
  check(gs == sampler, "PSGetSamplers");
  if (gs) ID3D10SamplerState_Release(gs);
  ID3D10ShaderResourceView *none = NULL;
  ID3D10Device_PSSetShaderResources(g_device, 0, 1, &none);
  ID3D10Buffer_Release(vb);
  ID3D10SamplerState_Release(sampler);
  ID3D10ShaderResourceView_Release(view);
  ID3D10Texture2D_Release(tex);
}

static void test_states(void) {
  /* (Direct3D 10's blend: one blend for every target, enabled per target) */
  D3D10_BLEND_DESC bd;
  memset(&bd, 0, sizeof bd);
  bd.BlendEnable[0] = TRUE;
  bd.SrcBlend = D3D10_BLEND_ONE;
  bd.DestBlend = D3D10_BLEND_ONE;
  bd.BlendOp = D3D10_BLEND_OP_ADD;
  bd.SrcBlendAlpha = D3D10_BLEND_ONE;
  bd.DestBlendAlpha = D3D10_BLEND_ZERO;
  bd.BlendOpAlpha = D3D10_BLEND_OP_ADD;
  for (int i = 0; i < 8; ++i) bd.RenderTargetWriteMask[i] = D3D10_COLOR_WRITE_ENABLE_ALL;
  ID3D10BlendState *add = NULL, *again = NULL;
  check(ID3D10Device_CreateBlendState(g_device, &bd, &add) == S_OK && add && ID3D10Device_CreateBlendState(g_device, &bd, &again) == S_OK && again == add,
        "a blend state, and the same object for the same description");
  if (again) ID3D10BlendState_Release(again);
  if (!add) return;
  D3D10_BLEND_DESC back;
  ID3D10BlendState_GetDesc(add, &back);
  check(back.BlendEnable[0] && !back.BlendEnable[1] && back.SrcBlend == D3D10_BLEND_ONE && back.DestBlend == D3D10_BLEND_ONE && back.RenderTargetWriteMask[3] == 15,
        "its Direct3D 10 description");
  ID3D10BlendState1 *b1 = NULL;
  D3D10_BLEND_DESC1 back1;
  memset(&back1, 0, sizeof back1);
  if (SUCCEEDED(ID3D10BlendState_QueryInterface(add, &IID_ID3D10BlendState1, (void **)&b1)) && b1) { ID3D10BlendState1_GetDesc1(b1, &back1); ID3D10BlendState1_Release(b1); }
  check(b1 && back1.IndependentBlendEnable && back1.RenderTarget[0].BlendEnable && !back1.RenderTarget[1].BlendEnable, "... and its 10.1 description");
  begin(NULL);
  ID3D10Device_PSSetShader(g_device, g_ps_color);
  ID3D10Buffer *t = triangle(0.5f, 1, 0, 0);
  bind_vertices(t);
  clear(0, 0, 1);
  ID3D10Device_OMSetBlendState(g_device, add, NULL, 0xFFFFFFFFu);
  ID3D10Device_Draw(g_device, 3, 0);
  check(read_target() && red(pixel(32, 32)) == 255 && blue(pixel(32, 32)) == 255, "additive blending: red on blue is magenta");
  ID3D10BlendState *got = NULL;
  FLOAT factor[4] = {0, 0, 0, 0};
  UINT mask = 0;
  ID3D10Device_OMGetBlendState(g_device, &got, factor, &mask);
  check(got == add && factor[0] == 1.0f && mask == 0xFFFFFFFFu, "OMGetBlendState (a NULL factor is 1)");
  if (got) ID3D10BlendState_Release(got);
  ID3D10Device_OMSetBlendState(g_device, NULL, NULL, 0xFFFFFFFFu);
  /* (depth: a near green triangle, then a far red one that loses) */
  D3D10_TEXTURE2D_DESC dd = {SIZE, SIZE, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_DEPTH_STENCIL, 0, 0};
  ID3D10Texture2D *depth = NULL;
  ID3D10DepthStencilView *dsv = NULL;
  D3D10_DEPTH_STENCIL_VIEW_DESC dvd;
  memset(&dvd, 0, sizeof dvd);
  dvd.Format = DXGI_FORMAT_D32_FLOAT;
  dvd.ViewDimension = D3D10_DSV_DIMENSION_TEXTURE2D;
  HRESULT hr = ID3D10Device_CreateTexture2D(g_device, &dd, NULL, &depth);
  if (SUCCEEDED(hr)) hr = ID3D10Device_CreateDepthStencilView(g_device, (ID3D10Resource *)depth, &dvd, &dsv);
  check(SUCCEEDED(hr) && dsv, "a depth buffer and its view (Direct3D 10's description, without flags)");
  if (dsv) {
    D3D10_DEPTH_STENCIL_VIEW_DESC dback;
    ID3D10DepthStencilView_GetDesc(dsv, &dback);
    check(dback.Format == DXGI_FORMAT_D32_FLOAT && dback.ViewDimension == D3D10_DSV_DIMENSION_TEXTURE2D && dback.Texture2D.MipSlice == 0, "the depth view's description");
    begin(dsv);
    ID3D10Device_PSSetShader(g_device, g_ps_color);
    clear(0, 0, 0);
    ID3D10Device_ClearDepthStencilView(g_device, dsv, D3D10_CLEAR_DEPTH, 1.0f, 0);
    ID3D10Buffer *nearer = triangle(0.2f, 0, 1, 0), *farther = triangle(0.7f, 1, 0, 0);
    bind_vertices(nearer);
    ID3D10Device_Draw(g_device, 3, 0);
    bind_vertices(farther);
    ID3D10Device_Draw(g_device, 3, 0);
    check(read_target() && green(pixel(32, 32)) == 255 && red(pixel(32, 32)) == 0, "the depth test: the nearer triangle stays");
    const D3D10_DEPTH_STENCIL_DESC always = {TRUE, D3D10_DEPTH_WRITE_MASK_ALL, D3D10_COMPARISON_ALWAYS, FALSE, 0xFF, 0xFF,
                                             {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_ALWAYS},
                                             {D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_STENCIL_OP_KEEP, D3D10_COMPARISON_ALWAYS}};
    ID3D10DepthStencilState *ds = NULL;
    check(ID3D10Device_CreateDepthStencilState(g_device, &always, &ds) == S_OK && ds, "a depth-stencil state");
    ID3D10Device_OMSetDepthStencilState(g_device, ds, 3);
    ID3D10Device_Draw(g_device, 3, 0);
    check(read_target() && red(pixel(32, 32)) == 255, "... ALWAYS: the farther triangle drawn over");
    ID3D10DepthStencilState *gds = NULL;
    UINT ref = 0;
    ID3D10Device_OMGetDepthStencilState(g_device, &gds, &ref);
    check(gds == ds && ref == 3, "OMGetDepthStencilState");
    if (gds) ID3D10DepthStencilState_Release(gds);
    ID3D10RenderTargetView *rt[2] = {NULL, NULL};
    ID3D10DepthStencilView *gdsv = NULL;
    ID3D10Device_OMGetRenderTargets(g_device, 2, rt, &gdsv);
    check(rt[0] == g_target_view && rt[1] == NULL && gdsv == dsv, "OMGetRenderTargets");
    if (rt[0]) ID3D10RenderTargetView_Release(rt[0]);
    if (gdsv) ID3D10DepthStencilView_Release(gdsv);
    ID3D10Device_OMSetDepthStencilState(g_device, NULL, 0);
    ID3D10Device_OMSetRenderTargets(g_device, 1, &g_target_view, NULL);
    if (ds) ID3D10DepthStencilState_Release(ds);
    ID3D10Buffer_Release(nearer);
    ID3D10Buffer_Release(farther);
    ID3D10DepthStencilView_Release(dsv);
  }
  if (depth) ID3D10Texture2D_Release(depth);
  ID3D10Buffer_Release(t);
  ID3D10BlendState_Release(add);
}

static void test_geometry_and_stream_output(void) {
  /* (gs_copy: the triangle, then a copy scaled into the bottom-right quarter with its colour's channels turned around: red becomes blue) */
  ID3D10GeometryShader *gs = NULL;
  check(ID3D10Device_CreateGeometryShader(g_device, dx_gs_copy_dxbc, sizeof dx_gs_copy_dxbc, &gs) == S_OK && gs, "a geometry shader");
  if (gs) {
    begin(NULL);
    ID3D10Device_PSSetShader(g_device, g_ps_color);
    ID3D10Device_GSSetShader(g_device, gs);
    ID3D10Buffer *t = triangle(0.5f, 1, 0, 0);
    bind_vertices(t);
    clear(0, 0, 0);
    ID3D10Device_Draw(g_device, 3, 0);
    read_target();
    check(red(pixel(32, 20)) == 255 && blue(pixel(48, 58)) == 255 && red(pixel(48, 58)) == 0, "the geometry shader's two triangles");
    ID3D10GeometryShader *got = NULL;
    ID3D10Device_GSGetShader(g_device, &got);
    check(got == gs, "GSGetShader");
    if (got) ID3D10GeometryShader_Release(got);
    ID3D10Device_GSSetShader(g_device, NULL);
    ID3D10Buffer_Release(t);
    ID3D10GeometryShader_Release(gs);
  }
  /* Stream output of the vertex shader's positions (no geometry stage, no rasterization needed): one buffer, a record of 16 bytes */
  const D3D10_SO_DECLARATION_ENTRY entries[1] = {{"SV_Position", 0, 0, 4, 0}};
  ID3D10GeometryShader *so = NULL, *refused = (ID3D10GeometryShader *)1;
  check(ID3D10Device_CreateGeometryShaderWithStreamOutput(g_device, dx_vs_dxbc, sizeof dx_vs_dxbc, entries, 1, 16, &so) == S_OK && so,
        "CreateGeometryShaderWithStreamOutput of a vertex shader's code");
  check(ID3D10Device_CreateGeometryShaderWithStreamOutput(g_device, dx_vs_dxbc, sizeof dx_vs_dxbc, entries, 1, 0, &refused) == E_INVALIDARG && !refused,
        "... a single buffer needs its stride");
  const D3D10_SO_DECLARATION_ENTRY unknown[1] = {{"NOTHERE", 0, 0, 4, 0}};
  refused = (ID3D10GeometryShader *)1;
  check(ID3D10Device_CreateGeometryShaderWithStreamOutput(g_device, dx_vs_dxbc, sizeof dx_vs_dxbc, unknown, 1, 16, &refused) == E_INVALIDARG && !refused,
        "... and an output the shader has not is refused");
  if (!so) return;
  ID3D10Buffer *target = make_buffer(D3D10_BIND_STREAM_OUTPUT, D3D10_USAGE_DEFAULT, 0, NULL, 256);
  D3D10_QUERY_DESC qd = {D3D10_QUERY_SO_STATISTICS, 0};
  ID3D10Query *stats = NULL;
  check(target && ID3D10Device_CreateQuery(g_device, &qd, &stats) == S_OK && stats, "a stream output buffer and a stream output statistics query");
  begin(NULL);
  ID3D10Device_GSSetShader(g_device, so);
  ID3D10Device_PSSetShader(g_device, g_ps_color);
  set_matrix(1, 1, 0, 0);
  ID3D10Buffer *t = triangle(0.25f, 0, 1, 0);
  bind_vertices(t);
  const UINT zero = 0;
  ID3D10Device_SOSetTargets(g_device, 1, &target, &zero);
  if (stats) ID3D10Query_Begin(stats);
  ID3D10Device_Draw(g_device, 3, 0);
  if (stats) ID3D10Query_End(stats);
  ID3D10Buffer *off[1] = {NULL};
  ID3D10Device_SOSetTargets(g_device, 1, off, &zero);
  D3D10_QUERY_DATA_SO_STATISTICS sd = {0, 0};
  HRESULT hr = stats ? ID3D10Query_GetData(stats, &sd, sizeof sd, 0) : E_FAIL;
  check(hr == S_OK && sd.NumPrimitivesWritten == 1 && sd.PrimitivesStorageNeeded == 1, "SO_STATISTICS: one triangle written");
  D3D10_BUFFER_DESC sdesc = {256, D3D10_USAGE_STAGING, 0, D3D10_CPU_ACCESS_READ, 0};
  ID3D10Buffer *copy = NULL;
  ID3D10Device_CreateBuffer(g_device, &sdesc, NULL, &copy);
  void *data = NULL;
  if (copy) {
    ID3D10Device_CopyResource(g_device, (ID3D10Resource *)copy, (ID3D10Resource *)target);
    ID3D10Buffer_Map(copy, D3D10_MAP_READ, 0, &data);
  }
  const float *f = data;
  check(f && f[0] == -0.8f && f[1] == -0.8f && f[2] == 0.25f && f[3] == 1.0f && f[4] == 0.8f && f[8] == 0.0f && f[9] == 0.8f,
        "the captured positions, read back through ID3D10Buffer::Map");
  if (data) ID3D10Buffer_Unmap(copy);
  ID3D10Buffer *gt[4] = {NULL, NULL, NULL, NULL};
  UINT go[4] = {1, 1, 1, 1};
  ID3D10Device_SOSetTargets(g_device, 1, &target, &zero);
  ID3D10Device_SOGetTargets(g_device, 4, gt, go);
  check(gt[0] == target && go[0] == ~0u && !gt[1] && go[1] == 0, "SOGetTargets: the buffer (its offset ~0, the records go on) and empty slots");
  for (int i = 0; i < 4; ++i) if (gt[i]) ID3D10Buffer_Release(gt[i]);
  ID3D10Device_SOSetTargets(g_device, 1, off, &zero);
  ID3D10Device_GSSetShader(g_device, NULL);
  if (copy) ID3D10Buffer_Release(copy);
  if (stats) ID3D10Query_Release(stats);
  ID3D10Buffer_Release(t);
  if (target) ID3D10Buffer_Release(target);
  ID3D10GeometryShader_Release(so);
}

static void test_queries(void) {
  begin(NULL);
  ID3D10Device_PSSetShader(g_device, g_ps_color);
  ID3D10Buffer *t = triangle(0.5f, 1, 1, 0);
  bind_vertices(t);
  ID3D10Query *occlusion = NULL, *event = NULL, *pipeline = NULL, *disjoint = NULL;
  D3D10_QUERY_DESC qd = {D3D10_QUERY_OCCLUSION, 0};
  ID3D10Device_CreateQuery(g_device, &qd, &occlusion);
  qd.Query = D3D10_QUERY_EVENT;
  ID3D10Device_CreateQuery(g_device, &qd, &event);
  qd.Query = D3D10_QUERY_PIPELINE_STATISTICS;
  ID3D10Device_CreateQuery(g_device, &qd, &pipeline);
  qd.Query = D3D10_QUERY_TIMESTAMP_DISJOINT;
  ID3D10Device_CreateQuery(g_device, &qd, &disjoint);
  check(occlusion && event && pipeline && disjoint, "occlusion, event, pipeline statistics and timestamp-disjoint queries");
  ID3D10Query *refused = (ID3D10Query *)1;
  qd.Query = (D3D10_QUERY)8;      /* (D3D11_QUERY_SO_STATISTICS_STREAM0: Direct3D 11's) */
  check(ID3D10Device_CreateQuery(g_device, &qd, &refused) == E_INVALIDARG && !refused, "a Direct3D 11 query is not Direct3D 10's");
  if (!occlusion || !event || !pipeline || !disjoint) return;
  check(ID3D10Query_GetDataSize(pipeline) == sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS) && sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS) == 64,
        "the pipeline statistics are Direct3D 10's eight counts");
  ID3D10Asynchronous_Begin(disjoint);
  ID3D10Asynchronous_Begin(occlusion);
  ID3D10Asynchronous_Begin(pipeline);
  clear(0, 0, 0);
  ID3D10Device_Draw(g_device, 3, 0);
  ID3D10Asynchronous_End(pipeline);
  ID3D10Asynchronous_End(occlusion);
  ID3D10Asynchronous_End(event);
  ID3D10Asynchronous_End(disjoint);
  UINT64 samples = 0;
  BOOL done = FALSE;
  D3D10_QUERY_DATA_PIPELINE_STATISTICS stats;
  memset(&stats, 0, sizeof stats);
  D3D10_QUERY_DATA_TIMESTAMP_DISJOINT dj = {0, TRUE};
  check(ID3D10Asynchronous_GetData(occlusion, &samples, sizeof samples, 0) == S_OK && samples > 500 && samples < SIZE * SIZE, "the occlusion query counts the triangle's samples");
  check(ID3D10Asynchronous_GetData(event, &done, sizeof done, 0) == S_OK && done, "the event query is done");
  check(ID3D10Asynchronous_GetData(pipeline, &stats, sizeof stats, 0) == S_OK && stats.IAVertices == 3 && stats.IAPrimitives == 1 && stats.VSInvocations >= 3 &&
            stats.PSInvocations > 500,
        "pipeline statistics: three vertices, a primitive, the pixel shader's invocations");
  D3D11_QUERY_DATA_PIPELINE_STATISTICS wide;
  check(ID3D10Asynchronous_GetData(pipeline, &wide, sizeof wide, 0) == E_INVALIDARG, "... Direct3D 11's structure is the wrong size here");
  check(ID3D10Asynchronous_GetData(disjoint, &dj, sizeof dj, 0) == S_OK && dj.Frequency > 0 && !dj.Disjoint, "the timestamp frequency");
  D3D10_QUERY_DESC back;
  ID3D10Query_GetDesc(occlusion, &back);
  check(back.Query == D3D10_QUERY_OCCLUSION && back.MiscFlags == 0, "a query's description");
  /* (predication: an occlusion predicate of a draw outside the viewport saw nothing, so a draw predicated on FALSE is left out) */
  ID3D10Predicate *pred = NULL;
  qd.Query = D3D10_QUERY_OCCLUSION_PREDICATE;
  check(ID3D10Device_CreatePredicate(g_device, &qd, &pred) == S_OK && pred, "an occlusion predicate");
  if (pred) {
    set_matrix(0.1f, 0.1f, 5, 5);      /* (moved away: no samples) */
    ID3D10Asynchronous_Begin(pred);
    ID3D10Device_Draw(g_device, 3, 0);
    ID3D10Asynchronous_End(pred);
    BOOL visible = TRUE;
    check(ID3D10Asynchronous_GetData(pred, &visible, sizeof visible, 0) == S_OK && !visible, "... of a draw that passed no samples");
    set_matrix(1, 1, 0, 0);
    clear(0, 0, 0);
    ID3D10Device_SetPredication(g_device, pred, FALSE);
    ID3D10Device_Draw(g_device, 3, 0);
    ID3D10Predicate *gp = NULL;
    BOOL value = TRUE;
    ID3D10Device_GetPredication(g_device, &gp, &value);
    check(gp == pred && value == FALSE, "GetPredication");
    if (gp) ID3D10Predicate_Release(gp);
    ID3D10Device_SetPredication(g_device, NULL, FALSE);
    check(read_target() && red(pixel(32, 32)) == 0, "a draw predicated on a predicate that saw nothing is left out");
    ID3D10Predicate_Release(pred);
  }
  ID3D10Counter *counter = (ID3D10Counter *)1;
  const D3D10_COUNTER_DESC cd = {D3D10_COUNTER_GPU_IDLE, 0};
  check(ID3D10Device_CreateCounter(g_device, &cd, &counter) == DXGI_ERROR_UNSUPPORTED && !counter, "Direct3D 10's optional counters: none on this device");
  ID3D10Query_Release(occlusion);
  ID3D10Query_Release(event);
  ID3D10Query_Release(pipeline);
  ID3D10Query_Release(disjoint);
  ID3D10Buffer_Release(t);
}

static void test_map_and_copies(void) {
  /* (a dynamic texture written through Map WRITE_DISCARD, copied into a default one and read back) */
  D3D10_TEXTURE2D_DESC desc = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_DYNAMIC, D3D10_BIND_SHADER_RESOURCE, D3D10_CPU_ACCESS_WRITE, 0};
  ID3D10Texture2D *dyn = NULL, *def = NULL, *stage = NULL;
  ID3D10Device_CreateTexture2D(g_device, &desc, NULL, &dyn);
  D3D10_MAPPED_TEXTURE2D m;
  memset(&m, 0, sizeof m);
  const HRESULT mapped = dyn ? ID3D10Texture2D_Map(dyn, 0, D3D10_MAP_WRITE_DISCARD, 0, &m) : E_FAIL;
  check(mapped == S_OK && m.pData && m.RowPitch >= 16, "ID3D10Texture2D::Map of a dynamic texture");
  if (mapped == S_OK) {
    for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x) ((UINT32 *)((BYTE *)m.pData + y * m.RowPitch))[x] = 0xFF000000u | (y * 4 + x);
    ID3D10Texture2D_Unmap(dyn, 0);
  }
  desc.Usage = D3D10_USAGE_DEFAULT;
  desc.CPUAccessFlags = 0;
  ID3D10Device_CreateTexture2D(g_device, &desc, NULL, &def);
  desc.Usage = D3D10_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ | D3D10_CPU_ACCESS_WRITE;
  ID3D10Device_CreateTexture2D(g_device, &desc, NULL, &stage);
  if (dyn && def && stage) {
    ID3D10Device_CopyResource(g_device, (ID3D10Resource *)def, (ID3D10Resource *)dyn);
    const D3D10_BOX box = {1, 1, 0, 3, 3, 1};
    ID3D10Device_CopySubresourceRegion(g_device, (ID3D10Resource *)stage, 0, 0, 0, 0, (ID3D10Resource *)def, 0, &box);
    D3D10_MAPPED_TEXTURE2D r;
    if (SUCCEEDED(ID3D10Texture2D_Map(stage, 0, D3D10_MAP_READ, 0, &r)) && r.pData) {
      const UINT32 *row0 = r.pData, *row1 = (const UINT32 *)((const BYTE *)r.pData + r.RowPitch);
      check((row0[0] & 255) == 5 && (row0[1] & 255) == 6 && (row1[0] & 255) == 9 && (row1[1] & 255) == 10, "CopyResource and CopySubresourceRegion with a Direct3D 10 box");
      ID3D10Texture2D_Unmap(stage, 0);
    } else check(0, "Map of the staging texture");
  }
  D3D10_MAPPED_TEXTURE2D bad;
  check(def && ID3D10Texture2D_Map(def, 0, D3D10_MAP_READ, 0, &bad) == E_INVALIDARG && !bad.pData, "a default texture cannot be mapped");
  /* a staging buffer written and read through ID3D10Buffer::Map */
  D3D10_BUFFER_DESC bd = {64, D3D10_USAGE_STAGING, 0, D3D10_CPU_ACCESS_READ | D3D10_CPU_ACCESS_WRITE, 0};
  ID3D10Buffer *b = NULL;
  void *p = NULL;
  ID3D10Device_CreateBuffer(g_device, &bd, NULL, &b);
  if (b && SUCCEEDED(ID3D10Buffer_Map(b, D3D10_MAP_WRITE, 0, &p)) && p) { memset(p, 0x5A, 64); ID3D10Buffer_Unmap(b); }
  p = NULL;
  check(b && SUCCEEDED(ID3D10Buffer_Map(b, D3D10_MAP_READ, 0, &p)) && p && ((BYTE *)p)[63] == 0x5A, "ID3D10Buffer::Map of a staging buffer, written and read");
  if (p) ID3D10Buffer_Unmap(b);
  D3D10_BUFFER_DESC back;
  if (b) ID3D10Buffer_GetDesc(b, &back);
  check(b && back.ByteWidth == 64 && back.Usage == D3D10_USAGE_STAGING && back.CPUAccessFlags == (D3D10_CPU_ACCESS_READ | D3D10_CPU_ACCESS_WRITE), "the buffer's description");
  D3D10_BUFFER_DESC uav = {64, D3D10_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0};
  ID3D10Buffer *refused = (ID3D10Buffer *)1;
  check(ID3D10Device_CreateBuffer(g_device, &uav, NULL, &refused) == E_INVALIDARG && !refused, "unordered access is not a Direct3D 10 binding");
  if (b) ID3D10Buffer_Release(b);
  if (dyn) ID3D10Texture2D_Release(dyn);
  if (def) ID3D10Texture2D_Release(def);
  if (stage) ID3D10Texture2D_Release(stage);
}

static void test_interfaces(void) {
  D3D10_TEXTURE2D_DESC desc = {16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_SHADER_RESOURCE, 0, D3D10_RESOURCE_MISC_GDI_COMPATIBLE};
  desc.MiscFlags = 0;
  ID3D10Texture2D *t10 = NULL;
  ID3D10Device_CreateTexture2D(g_device, &desc, NULL, &t10);
  ID3D11Texture2D *t11 = NULL;
  ID3D10Texture2D *again = NULL;
  check(t10 && SUCCEEDED(ID3D10Texture2D_QueryInterface(t10, &IID_ID3D11Texture2D, (void **)&t11)) && t11 &&
            SUCCEEDED(ID3D11Texture2D_QueryInterface(t11, &IID_ID3D10Texture2D, (void **)&again)) && again == t10,
        "a texture: ID3D11Texture2D and back to the same ID3D10Texture2D");
  if (t10) check(refs(t10) == 3 && refs(t11) == 3, "... one count for both");
  IUnknown *u10 = NULL, *u11 = NULL;
  if (t10) ID3D10Texture2D_QueryInterface(t10, &IID_IUnknown, (void **)&u10);
  if (t11) ID3D11Texture2D_QueryInterface(t11, &IID_IUnknown, (void **)&u11);
  check(u10 && u10 == u11 && u10 == (IUnknown *)t11, "... and one IUnknown (Direct3D 11's)");
  if (u10) IUnknown_Release(u10);
  if (u11) IUnknown_Release(u11);
  IDXGISurface *surface = NULL;
  ID3D10Resource *res = NULL;
  check(t10 && SUCCEEDED(ID3D10Texture2D_QueryInterface(t10, &IID_IDXGISurface, (void **)&surface)) && surface &&
            SUCCEEDED(IDXGISurface_QueryInterface(surface, &IID_ID3D10Resource, (void **)&res)) && res == (ID3D10Resource *)t10,
        "IDXGISurface of a Direct3D 10 texture, and back");
  if (res) ID3D10Resource_Release(res);
  if (surface) IDXGISurface_Release(surface);
  ID3D10Device *owner = NULL;
  if (t10) ID3D10Texture2D_GetDevice(t10, &owner);
  check(owner == g_device, "GetDevice of a Direct3D 10 object is the Direct3D 10 device");
  if (owner) ID3D10Device_Release(owner);
  D3D10_RESOURCE_DIMENSION dim = 0;
  if (t10) ID3D10Texture2D_GetType(t10, &dim);
  check(dim == D3D10_RESOURCE_DIMENSION_TEXTURE2D, "GetType");
  if (again) ID3D10Texture2D_Release(again);
  if (t11) ID3D11Texture2D_Release(t11);
  if (t10) check(ID3D10Texture2D_Release(t10) == 0, "... gone with the last reference of either");
  /* (private data is the object's: set through one face, read through the other) */
  ID3D10Buffer *b = make_buffer(D3D10_BIND_VERTEX_BUFFER, D3D10_USAGE_DEFAULT, 0, NULL, 16);
  const GUID key = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
  const DWORD value = 0xC0FFEE;
  DWORD read = 0;
  UINT size = sizeof read;
  ID3D11Buffer *b11 = NULL;
  if (b) { ID3D10Buffer_SetPrivateData(b, &key, sizeof value, &value); ID3D10Buffer_QueryInterface(b, &IID_ID3D11Buffer, (void **)&b11); }
  check(b11 && ID3D11Buffer_GetPrivateData(b11, &key, &size, &read) == S_OK && read == value, "private data of one object, through both faces");
  if (b11) ID3D11Buffer_Release(b11);
  if (b) ID3D10Buffer_Release(b);
  /* (what Direct3D 10 has no word for has no Direct3D 10 interface: a structured buffer, a compute shader) */
  ID3D11Device *d11 = NULL;
  ID3D10Device_QueryInterface(g_device, &IID_ID3D11Device, (void **)&d11);
  if (d11) {
    D3D11_BUFFER_DESC sd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
    ID3D11Buffer *structured = NULL;
    ID3D10Buffer *as10 = (ID3D10Buffer *)1;
    if (SUCCEEDED(ID3D11Device_CreateBuffer(d11, &sd, NULL, &structured)) && structured) {
      check(ID3D11Buffer_QueryInterface(structured, &IID_ID3D10Buffer, (void **)&as10) == E_NOINTERFACE && !as10, "a structured buffer is not an ID3D10Buffer");
      ID3D11Buffer_Release(structured);
    }
    ID3D11Device_Release(d11);
  }
  /* (a device Direct3D 11 made has no Direct3D 10 face, but its objects have theirs, whose device is NULL) */
  ID3D11Device *plain = NULL;
  ID3D11DeviceContext *ctx = NULL;
  if (SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &plain, NULL, &ctx)) && plain) {
    ID3D10Device *as10 = (ID3D10Device *)1;
    check(ID3D11Device_QueryInterface(plain, &IID_ID3D10Device, (void **)&as10) == E_NOINTERFACE && !as10, "a Direct3D 11 device is not an ID3D10Device");
    ID3D10Multithread *mt = NULL;
    check(SUCCEEDED(ID3D11Device_QueryInterface(plain, &IID_ID3D10Multithread, (void **)&mt)) && mt, "... but has ID3D10Multithread");
    if (mt) ID3D10Multithread_Release(mt);
    D3D11_TEXTURE2D_DESC td = {8, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *tex = NULL;
    ID3D10Texture2D *tex10 = NULL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(plain, &td, NULL, &tex)) && tex && SUCCEEDED(ID3D11Texture2D_QueryInterface(tex, &IID_ID3D10Texture2D, (void **)&tex10)) && tex10) {
      ID3D10Device *dev = (ID3D10Device *)1;
      ID3D10Texture2D_GetDevice(tex10, &dev);
      check(dev == NULL, "... a Direct3D 11 texture's ID3D10Texture2D has no Direct3D 10 device");
      ID3D10Texture2D_Release(tex10);
    }
    if (tex) ID3D11Texture2D_Release(tex);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    check(ID3D11Device_Release(plain) == 0, "... and goes with its last reference");
  }
}

static void test_state_block(void) {
  D3D10_STATE_BLOCK_MASK mask;
  check(D3D10StateBlockMaskDisableAll(&mask) == S_OK && D3D10StateBlockMaskEnableCapture(&mask, D3D10_DST_VS, 0, 1) == S_OK &&
            D3D10StateBlockMaskEnableCapture(&mask, D3D10_DST_PS_SHADER_RESOURCES, 3, 2) == S_OK && mask.PSShaderResources[0] == 0x18 &&
            D3D10StateBlockMaskEnableCapture(&mask, D3D10_DST_VS, 1, 1) == E_INVALIDARG && D3D10StateBlockMaskGetSetting(&mask, D3D10_DST_PS_SHADER_RESOURCES, 4) == 0x10,
        "state block masks: the bits of a state type, a range of slots, refused beyond them");
  D3D10StateBlockMaskEnableCapture(&mask, D3D10_DST_RS_RASTERIZER_STATE, 0, 1);
  ID3D10StateBlock *sb = NULL;
  check(D3D10CreateStateBlock(g_device, &mask, &sb) == S_OK && sb, "D3D10CreateStateBlock");
  if (!sb) return;
  ID3D10Device_VSSetShader(g_device, g_vs);
  ID3D10Device_RSSetState(g_device, g_no_cull);
  check(ID3D10StateBlock_Capture(sb) == S_OK, "Capture");
  ID3D10Device_VSSetShader(g_device, NULL);
  ID3D10Device_RSSetState(g_device, NULL);
  ID3D10Device_PSSetShader(g_device, g_ps_tex);
  check(ID3D10StateBlock_Apply(sb) == S_OK, "Apply");
  ID3D10VertexShader *vs = NULL;
  ID3D10RasterizerState *rs = NULL;
  ID3D10PixelShader *ps = NULL;
  ID3D10Device_VSGetShader(g_device, &vs);
  ID3D10Device_RSGetState(g_device, &rs);
  ID3D10Device_PSGetShader(g_device, &ps);
  check(vs == g_vs && rs == g_no_cull && ps == g_ps_tex, "Apply puts back what the mask has, and only that");
  if (vs) ID3D10VertexShader_Release(vs);
  if (rs) ID3D10RasterizerState_Release(rs);
  if (ps) ID3D10PixelShader_Release(ps);
  ID3D10Device *dev = NULL;
  check(ID3D10StateBlock_GetDevice(sb, &dev) == S_OK && dev == g_device, "the state block's device");
  if (dev) ID3D10Device_Release(dev);
  check(ID3D10StateBlock_ReleaseAllDeviceObjects(sb) == S_OK, "ReleaseAllDeviceObjects");
  ID3D10StateBlock_Release(sb);
  ID3D10Device_ClearState(g_device);
  ID3D10Device_VSGetShader(g_device, &vs);
  ID3D10RenderTargetView *rt = (ID3D10RenderTargetView *)1;
  ID3D10Device_OMGetRenderTargets(g_device, 1, &rt, NULL);
  D3D10_PRIMITIVE_TOPOLOGY topology = D3D10_PRIMITIVE_TOPOLOGY_POINTLIST;
  ID3D10Device_IAGetPrimitiveTopology(g_device, &topology);
  check(!vs && !rt && topology == D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED, "ClearState");
}

static void test_swap_chain(void) {
  WNDCLASSW cls;
  memset(&cls, 0, sizeof cls);
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(NULL);
  cls.lpszClassName = L"d3d10test";
  RegisterClassW(&cls);
  const HWND window = CreateWindowExW(0, L"d3d10test", L"d3d10test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, 160, 120, NULL, NULL, cls.hInstance, NULL);
  check(window != NULL, "a window for the swap chain");
  if (!window) return;
  DXGI_SWAP_CHAIN_DESC desc;
  memset(&desc, 0, sizeof desc);
  desc.BufferDesc.Width = 96;
  desc.BufferDesc.Height = 64;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 1;
  desc.OutputWindow = window;
  desc.Windowed = TRUE;
  IDXGISwapChain *chain = NULL;
  ID3D10Device *device = NULL;
  const HRESULT made = D3D10CreateDeviceAndSwapChain(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &desc, &chain, &device);
  check(made == S_OK && chain && device, "D3D10CreateDeviceAndSwapChain");
  if (made == S_OK && chain && device) {
    ID3D10Texture2D *back = NULL, *copy = NULL;
    ID3D10RenderTargetView *view = NULL;
    D3D10_TEXTURE2D_DESC bd;
    memset(&bd, 0, sizeof bd);
    HRESULT hr = IDXGISwapChain_GetBuffer(chain, 0, &IID_ID3D10Texture2D, (void **)&back);
    if (SUCCEEDED(hr) && back) ID3D10Texture2D_GetDesc(back, &bd);
    check(SUCCEEDED(hr) && back && bd.Width == 96 && bd.Height == 64, "GetBuffer(IID_ID3D10Texture2D): the back buffer");
    if (SUCCEEDED(hr) && back) hr = ID3D10Device_CreateRenderTargetView(device, (ID3D10Resource *)back, NULL, &view);
    if (SUCCEEDED(hr) && view) {
      const float green_colour[4] = {0, 1, 0, 1};
      ID3D10Device_ClearRenderTargetView(device, view, green_colour);
      check(IDXGISwapChain_Present(chain, 0, 0) == S_OK, "Present");
      D3D10_TEXTURE2D_DESC cd = bd;
      cd.Usage = D3D10_USAGE_STAGING;
      cd.BindFlags = 0;
      cd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
      cd.MiscFlags = 0;
      if (SUCCEEDED(ID3D10Device_CreateTexture2D(device, &cd, NULL, &copy)) && copy) {
        ID3D10Device_CopyResource(device, (ID3D10Resource *)copy, (ID3D10Resource *)back);
        D3D10_MAPPED_TEXTURE2D m;
        if (SUCCEEDED(ID3D10Texture2D_Map(copy, 0, D3D10_MAP_READ, 0, &m)) && m.pData) {
          check(((const UINT32 *)m.pData)[10] == 0xFF00FF00u, "the back buffer holds the clear colour after Present");
          ID3D10Texture2D_Unmap(copy, 0);
        }
        ID3D10Texture2D_Release(copy);
      }
      ID3D10RenderTargetView_Release(view);
    }
    if (back) ID3D10Texture2D_Release(back);
    IUnknown *chain_device = NULL;
    check(SUCCEEDED(IDXGISwapChain_GetDevice(chain, &IID_ID3D10Device, (void **)&chain_device)) && chain_device == (IUnknown *)device, "the swap chain's device");
    if (chain_device) IUnknown_Release(chain_device);
  }
  if (chain) IDXGISwapChain_Release(chain);
  if (device) check(ID3D10Device_Release(device) == 0, "the swap chain's device goes with its last reference");
  DestroyWindow(window);
}

static void test_effect(void) {
  ID3D10Effect *effect = NULL;
  const HRESULT hr = D3D10CreateEffectFromMemory((void *)dx_draw_fxo, sizeof dx_draw_fxo, 0, g_device, NULL, &effect);
  check(hr == S_OK && effect && ID3D10Effect_IsValid(effect), "D3D10CreateEffectFromMemory of an fx_4_0 effect");
  if (!effect) return;
  D3D10_EFFECT_DESC ed;
  memset(&ed, 0, sizeof ed);
  ID3D10Effect_GetDesc(effect, &ed);
  check(ed.Techniques == 2 && ed.ConstantBuffers >= 1, "its description: two techniques, the constant buffer");
  ID3D10EffectTechnique *solid = ID3D10Effect_GetTechniqueByName(effect, "solid");
  ID3D10EffectTechnique *blended = ID3D10Effect_GetTechniqueByIndex(effect, 1);
  ID3D10EffectTechnique *none = ID3D10Effect_GetTechniqueByName(effect, "nothing");
  check(ID3D10EffectTechnique_IsValid(solid) && ID3D10EffectTechnique_IsValid(blended) && !ID3D10EffectTechnique_IsValid(none), "techniques by name and index (and an invalid one)");
  ID3D10EffectVariable *tint = ID3D10Effect_GetVariableByName(effect, "tint");
  ID3D10EffectVariable *m = ID3D10Effect_GetVariableByName(effect, "m");
  const float colour[4] = {0, 1, 0, 1};
  const float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  ID3D10EffectVectorVariable *v = ID3D10EffectVariable_AsVector(tint);
  ID3D10EffectMatrixVariable *mv = ID3D10EffectVariable_AsMatrix(m);
  check(ID3D10EffectVectorVariable_SetFloatVector(v, (float *)colour) == S_OK && ID3D10EffectMatrixVariable_SetMatrix(mv, (float *)matrix) == S_OK,
        "setting a vector and a matrix variable");
  float back[4] = {0, 0, 0, 0};
  ID3D10EffectVectorVariable_GetFloatVector(v, back);
  check(back[1] == 1.0f && back[0] == 0.0f, "... read back");
  ID3D10EffectPass *pass = ID3D10EffectTechnique_GetPassByIndex(solid, 0);
  D3D10_PASS_DESC pd;
  memset(&pd, 0, sizeof pd);
  check(ID3D10EffectPass_IsValid(pass) && ID3D10EffectPass_GetDesc(pass, &pd) == S_OK && pd.pIAInputSignature && pd.IAInputSignatureSize > 0,
        "the pass and its input signature");
  ID3D10InputLayout *layout = NULL;
  if (pd.pIAInputSignature) ID3D10Device_CreateInputLayout(g_device, g_elements, 2, pd.pIAInputSignature, pd.IAInputSignatureSize, &layout);
  check(layout != NULL, "an input layout for the pass's vertex shader");
  ID3D10Buffer *t = triangle(0.5f, 1, 1, 1);
  ID3D10Device_OMSetRenderTargets(g_device, 1, &g_target_view, NULL);
  D3D10_VIEWPORT viewport = {0, 0, SIZE, SIZE, 0, 1};
  ID3D10Device_RSSetViewports(g_device, 1, &viewport);
  ID3D10Device_IASetInputLayout(g_device, layout);
  ID3D10Device_IASetPrimitiveTopology(g_device, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  bind_vertices(t);
  clear(0, 0, 0);
  check(ID3D10EffectPass_Apply(pass, 0) == S_OK, "Apply");
  ID3D10Device_Draw(g_device, 3, 0);
  check(read_target() && green(pixel(32, 32)) == 255 && red(pixel(32, 32)) == 0 && green(pixel(2, 2)) == 0, "a triangle drawn through the effect: the tint variable's colour");
  ID3D10RasterizerState *rs = NULL;
  ID3D10Device_RSGetState(g_device, &rs);
  D3D10_RASTERIZER_DESC rd;
  memset(&rd, 0, sizeof rd);
  if (rs) { ID3D10RasterizerState_GetDesc(rs, &rd); ID3D10RasterizerState_Release(rs); }
  check(rs && rd.CullMode == D3D10_CULL_NONE, "the pass's rasterizer state is bound");
  /* (the second technique: blend factor 0.5 - white tint over blue is half of each) */
  const float white[4] = {1, 1, 1, 1};
  ID3D10EffectVectorVariable_SetFloatVector(v, (float *)white);
  clear(0, 0, 1);
  ID3D10EffectPass_Apply(ID3D10EffectTechnique_GetPassByIndex(blended, 0), 0);
  ID3D10Device_Draw(g_device, 3, 0);
  read_target();
  check(close_to(red(pixel(32, 32)), 128) && close_to(blue(pixel(32, 32)), 255), "the blended technique: the pass's blend state and factor");
  D3D10_STATE_BLOCK_MASK mask;
  memset(&mask, 0, sizeof mask);
  check(ID3D10EffectPass_ComputeStateBlockMask(pass, &mask) == S_OK && (mask.VS & 1) && (mask.PS & 1) && (mask.RSRasterizerState & 1), "ComputeStateBlockMask of the pass");
  ID3D10Buffer_Release(t);
  if (layout) ID3D10InputLayout_Release(layout);
  ID3D10Effect_Release(effect);
  ID3D10Device_ClearState(g_device);
}

static void cleanup(void) {
  ID3D10Device_ClearState(g_device);
  if (g_layout) ID3D10InputLayout_Release(g_layout);
  if (g_vs) ID3D10VertexShader_Release(g_vs);
  if (g_ps_color) ID3D10PixelShader_Release(g_ps_color);
  if (g_ps_tex) ID3D10PixelShader_Release(g_ps_tex);
  if (g_matrix) ID3D10Buffer_Release(g_matrix);
  if (g_no_cull) ID3D10RasterizerState_Release(g_no_cull);
  if (g_target_view) ID3D10RenderTargetView_Release(g_target_view);
  if (g_target) ID3D10Texture2D_Release(g_target);
  if (g_staging) ID3D10Texture2D_Release(g_staging);
  check(ID3D10Device_Release(g_device) == 0, "the device is gone with its last reference");
}

int main(void) {
  test_device();
  if (g_device) {
    test_setup();
    test_clear_and_draw();
    test_texture();
    test_states();
    test_geometry_and_stream_output();
    test_queries();
    test_map_and_copies();
    test_interfaces();
    test_state_block();
    test_effect();
    test_swap_chain();
    cleanup();
  }
  if (g_failed) printf("d3d10test: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  else printf("d3d10test: all %d checks passed\n", g_passed);
  fflush(stdout);
  return g_failed ? 1 : 42;
}
