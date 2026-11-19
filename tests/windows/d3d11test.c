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

/* Direct3D 11 (d3d11.dll, a layer on d3d12.dll) on the real kernel: a device and its other faces, buffers, a triangle drawn into a texture and read
 * back, constant buffers changed between draws and one a stage, colours as bytes in the vertices, a sampled texture and its update, depth, blending,
 * indexed draws, the state a program reads back, and a swap chain on a window. Built for both widths; ends with exit code 42 if every check passed.
 * dxfixtures.h (the shaders, tests/data/dxbc) is made by the script that builds it (tests/wow64_qemu.sh, as tests/emulated_dx.sh makes it).
 *   x86_64-w64-mingw32-gcc -O1 -I <folder of dxfixtures.h> -o d3d11test.exe tests/windows/d3d11test.c -ld3d11 -ldxgi -luuid -ldxguid -luser32 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include "dxfixtures.h"

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "d3d11test: ok: %s\n" : "d3d11test: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

#define SIZE 64
static ID3D11Device *g_device;
static ID3D11DeviceContext *g_context;
static ID3D11Texture2D *g_target, *g_staging;
static ID3D11RenderTargetView *g_target_view;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps_color, *g_ps_tex, *g_ps_cbuf;
static ID3D11InputLayout *g_layout_floats, *g_layout_bytes;
static ID3D11Buffer *g_matrix;
static ID3D11RasterizerState *g_no_cull;
static UINT32 g_pixels[SIZE * SIZE];

typedef struct { float x, y, z, r, g, b, a; } VertexF;
typedef struct { float x, y, z; BYTE r, g, b, a; } VertexB;

static ID3D11Buffer *make_buffer(UINT bind, D3D11_USAGE usage, UINT cpu, const void *data, UINT bytes) {
  D3D11_BUFFER_DESC desc = {bytes, usage, bind, cpu, 0, 0};
  D3D11_SUBRESOURCE_DATA initial = {data, 0, 0};
  ID3D11Buffer *buffer = NULL;
  ID3D11Device_CreateBuffer(g_device, &desc, data ? &initial : NULL, &buffer);
  return buffer;
}
static void set_matrix(float sx, float sy, float tx, float ty) {
  /* (the shader multiplies a row vector by the matrix, which is kept column by column: the translation is the fourth value of the first two columns) */
  const float m[16] = {sx, 0, 0, tx, 0, sy, 0, ty, 0, 0, 1, 0, 0, 0, 0, 1};
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)g_matrix, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, m, sizeof m);
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)g_matrix, 0);
  }
}
/* The render target's pixels (B8G8R8A8), by way of a staging texture. */
static int read_target(void) {
  D3D11_MAPPED_SUBRESOURCE mapped;
  ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)g_staging, (ID3D11Resource *)g_target);
  if (FAILED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)g_staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) return 0;
  for (int y = 0; y < SIZE; ++y) memcpy(g_pixels + y * SIZE, (const BYTE *)mapped.pData + (size_t)y * mapped.RowPitch, SIZE * 4);
  ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)g_staging, 0);
  return 1;
}
static UINT32 pixel(int x, int y) { return g_pixels[y * SIZE + x]; }
static int red(UINT32 p) { return (int)((p >> 16) & 255); }
static int green(UINT32 p) { return (int)((p >> 8) & 255); }
static int blue(UINT32 p) { return (int)(p & 255); }
static void clear(float r, float g, float b) {
  const float colour[4] = {r, g, b, 1};
  ID3D11DeviceContext_ClearRenderTargetView(g_context, g_target_view, colour);
}
/* The state every scene starts from: the target, a viewport of all of it, the vertex shader with its matrix, no culling. */
static void begin(ID3D11DepthStencilView *depth) {
  D3D11_VIEWPORT viewport = {0, 0, SIZE, SIZE, 0, 1};
  ID3D11DeviceContext_OMSetRenderTargets(g_context, 1, &g_target_view, depth);
  ID3D11DeviceContext_RSSetViewports(g_context, 1, &viewport);
  ID3D11DeviceContext_RSSetState(g_context, g_no_cull);
  ID3D11DeviceContext_VSSetShader(g_context, g_vs, NULL, 0);
  ID3D11DeviceContext_VSSetConstantBuffers(g_context, 0, 1, &g_matrix);
  ID3D11DeviceContext_IASetPrimitiveTopology(g_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11DeviceContext_OMSetBlendState(g_context, NULL, NULL, 0xFFFFFFFFu);
  ID3D11DeviceContext_OMSetDepthStencilState(g_context, NULL, 0);
  set_matrix(1, 1, 0, 0);
}
static void bind_floats(ID3D11Buffer *vertices) {
  const UINT stride = sizeof(VertexF), offset = 0;
  ID3D11DeviceContext_IASetInputLayout(g_context, g_layout_floats);
  ID3D11DeviceContext_IASetVertexBuffers(g_context, 0, 1, &vertices, &stride, &offset);
}
/* A triangle that covers the middle of the target, in one colour, at depth z. */
static ID3D11Buffer *triangle(float z, float r, float g, float b) {
  const VertexF v[3] = {{-0.8f, -0.8f, z, r, g, b, 1}, {0.8f, -0.8f, z, r, g, b, 1}, {0.0f, 0.8f, z, r, g, b, 1}};
  return make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, 0, v, sizeof v);
}

static void test_device(void) {
  D3D_FEATURE_LEVEL level = 0;
  const D3D_FEATURE_LEVEL wanted[2] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  check(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, wanted, 2, D3D11_SDK_VERSION, NULL, &level, NULL) == S_FALSE && level == D3D_FEATURE_LEVEL_11_0,
        "D3D11CreateDevice with nothing asked for says which level there would be");
  const HRESULT made = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, wanted, 2, D3D11_SDK_VERSION, &g_device, &level, &g_context);
  check(made == S_OK && g_device && g_context && level == D3D_FEATURE_LEVEL_11_0, "D3D11CreateDevice: a device and its context, feature level 11.0");
  if (!g_device || !g_context) return;
  check(ID3D11Device_GetFeatureLevel(g_device) == D3D_FEATURE_LEVEL_11_0 && ID3D11DeviceContext_GetType(g_context) == D3D11_DEVICE_CONTEXT_IMMEDIATE, "the level and the context's type");
  ID3D11DeviceContext *again = NULL;
  ID3D11Device_GetImmediateContext(g_device, &again);
  check(again == g_context, "GetImmediateContext is the same context");
  if (again) ID3D11DeviceContext_Release(again);
  ID3D11Device *owner = NULL;
  ID3D11DeviceContext_GetDevice(g_context, &owner);
  check(owner == g_device, "the context's device");
  if (owner) ID3D11Device_Release(owner);
  IDXGIDevice *dxgi = NULL;
  IDXGIAdapter *adapter = NULL;
  DXGI_ADAPTER_DESC description;
  memset(&description, 0, sizeof description);
  check(SUCCEEDED(ID3D11Device_QueryInterface(g_device, &IID_IDXGIDevice, (void **)&dxgi)) && dxgi && SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)) && adapter &&
        SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &description)) && description.Description[0] != 0, "the device as an IDXGIDevice, and its adapter");
  IDXGIFactory *factory = NULL;
  check(adapter && SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory)) && factory, "... and the adapter's factory");
  if (factory) IDXGIFactory_Release(factory);
  if (adapter) IDXGIAdapter_Release(adapter);
  if (dxgi) IDXGIDevice_Release(dxgi);
  UINT support = 0, levels = 0;
  check(SUCCEEDED(ID3D11Device_CheckFormatSupport(g_device, DXGI_FORMAT_B8G8R8A8_UNORM, &support)) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) &&
        SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(g_device, DXGI_FORMAT_B8G8R8A8_UNORM, 1, &levels)) && levels == 1, "CheckFormatSupport and CheckMultisampleQualityLevels");
  ID3D11Counter *counter = NULL;
  const D3D11_COUNTER_DESC counter_desc = {D3D11_COUNTER_DEVICE_DEPENDENT_0, 0};
  check(ID3D11Device_CreateCounter(g_device, &counter_desc, &counter) == E_NOTIMPL && counter == NULL, "what is not there answers E_NOTIMPL (and leaves the stack whole)");

  /* what ANGLE (Chrome's GL) asks of a device: DXGI 1.2 and 1.3, event queries for its fences, timestamps */
  IDXGIDevice2 *dxgi2 = NULL;
  check(SUCCEEDED(ID3D11Device_QueryInterface(g_device, &IID_IDXGIDevice2, (void **)&dxgi2)) && dxgi2, "the device as an IDXGIDevice2");
  if (dxgi2) {
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(SUCCEEDED(IDXGIDevice2_EnqueueSetEvent(dxgi2, done)) && WaitForSingleObject(done, 0) == WAIT_OBJECT_0, "EnqueueSetEvent sets the event once the work is done");
    CloseHandle(done);
    IDXGIDevice2_Release(dxgi2);
  }
  ID3D11Query *query = NULL;
  const D3D11_QUERY_DESC event_desc = {D3D11_QUERY_EVENT, 0};
  BOOL finished = FALSE;
  check(SUCCEEDED(ID3D11Device_CreateQuery(g_device, &event_desc, &query)) && query && ID3D11Query_GetDataSize(query) == sizeof(BOOL), "an event query");
  if (query) {
    check(ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)query, &finished, sizeof finished, 0) == DXGI_ERROR_INVALID_CALL, "... has no answer before End");
    ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)query);
    check(ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)query, &finished, sizeof finished, 0) == S_OK && finished, "... and is done after it");
    ID3D11Query_Release(query);
  }
  ID3D11Query *disjoint = NULL, *stamp = NULL;
  const D3D11_QUERY_DESC disjoint_desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp_desc = {D3D11_QUERY_TIMESTAMP, 0};
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency = {0, TRUE};
  UINT64 when = 0;
  check(SUCCEEDED(ID3D11Device_CreateQuery(g_device, &disjoint_desc, &disjoint)) && SUCCEEDED(ID3D11Device_CreateQuery(g_device, &stamp_desc, &stamp)), "timestamp queries");
  if (disjoint && stamp) {
    ID3D11DeviceContext_Begin(g_context, (ID3D11Asynchronous *)disjoint);
    ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)stamp);
    ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)disjoint);
    check(ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)stamp, &when, sizeof when, 0) == S_OK && when != 0 &&
          ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)disjoint, &frequency, sizeof frequency, 0) == S_OK && frequency.Frequency != 0 && !frequency.Disjoint,
          "... a time and the counter's frequency");
  }
  if (disjoint) ID3D11Query_Release(disjoint);
  if (stamp) ID3D11Query_Release(stamp);
}

static void test_buffers(void) {
  const float numbers[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  ID3D11Buffer *buffer = make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, numbers, sizeof numbers);
  ID3D11Buffer *staging = make_buffer(0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, NULL, sizeof numbers);
  D3D11_BUFFER_DESC desc;
  memset(&desc, 0, sizeof desc);
  if (buffer) ID3D11Buffer_GetDesc(buffer, &desc);
  check(buffer && staging && desc.ByteWidth == sizeof numbers && desc.Usage == D3D11_USAGE_DYNAMIC, "CreateBuffer and GetDesc");
  if (!buffer || !staging) return;
  D3D11_MAPPED_SUBRESOURCE mapped;
  ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)staging, (ID3D11Resource *)buffer);
  check(SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData && memcmp(mapped.pData, numbers, sizeof numbers) == 0,
        "a buffer's first contents, copied to a staging buffer and read");
  ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
  const float two[2] = {70, 80};
  const D3D11_BOX box = {8, 0, 0, 16, 1, 1};
  ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)staging, 0, &box, two, 0, 0);
  int updated = 0;
  if (SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) {
    const float *f = mapped.pData;
    updated = f[1] == 2 && f[2] == 70 && f[3] == 80 && f[4] == 5;
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
  }
  check(updated, "UpdateSubresource of a part of a buffer");
  ID3D11Buffer_Release(staging);
  ID3D11Buffer_Release(buffer);
}

static int make_scene(void) {
  D3D11_TEXTURE2D_DESC target = {SIZE, SIZE, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_TEXTURE2D_DESC staging = {SIZE, SIZE, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
  const D3D11_INPUT_ELEMENT_DESC floats[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                              {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  const D3D11_INPUT_ELEMENT_DESC bytes[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                             {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  D3D11_RASTERIZER_DESC raster = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
  HRESULT hr = ID3D11Device_CreateTexture2D(g_device, &target, NULL, &g_target);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateTexture2D(g_device, &staging, NULL, &g_staging);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)g_target, NULL, &g_target_view);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(g_device, dx_vs_dxbc, sizeof dx_vs_dxbc, NULL, &g_vs);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_color_dxbc, sizeof dx_ps_color_dxbc, NULL, &g_ps_color);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_tex_dxbc, sizeof dx_ps_tex_dxbc, NULL, &g_ps_tex);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_cbuf_dxbc, sizeof dx_ps_cbuf_dxbc, NULL, &g_ps_cbuf);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(g_device, floats, 2, dx_vs_dxbc, sizeof dx_vs_dxbc, &g_layout_floats);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(g_device, bytes, 2, dx_vs_dxbc, sizeof dx_vs_dxbc, &g_layout_bytes);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRasterizerState(g_device, &raster, &g_no_cull);
  g_matrix = make_buffer(D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, NULL, 64);
  check(SUCCEEDED(hr) && g_matrix, "a render target and its view, a staging texture, the shaders, two input layouts, a rasterizer state, a constant buffer");
  return SUCCEEDED(hr) && g_matrix;
}

static void test_triangle(void) {
  const VertexF v[3] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 0, 1, 1}};
  ID3D11Buffer *vertices = make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, 0, v, sizeof v);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  check(read_target(), "CopyResource to a staging texture and Map");
  int coloured = 0, top = 0, base = 0;
  for (int i = 0; i < SIZE * SIZE; ++i) coloured += (g_pixels[i] & 0xFFFFFF) != 0;
  for (int x = 0; x < SIZE; ++x) { top += (pixel(x, 10) & 0xFFFFFF) != 0; base += (pixel(x, 54) & 0xFFFFFF) != 0; }
  if (!(pixel(0, 0) == 0xFF000000u && coloured > 600 && coloured < 2000))
    printf("d3d11test: (%d coloured pixels, the corner is %08x, the middle %08x)\n", coloured, (unsigned)pixel(0, 0), (unsigned)pixel(32, 32));
  check(pixel(0, 0) == 0xFF000000u && coloured > 600 && coloured < 2000 && (pixel(32, 32) & 0xFFFFFF) != 0, "a triangle in the middle of a black target");
  check(top > 0 && top < base, "its apex is at the top");

  /* the matrix changes between two draws of one list: each draw has the constants it was drawn with */
  clear(0, 0, 0);
  set_matrix(0.4f, 0.4f, -0.5f, 0);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  set_matrix(0.4f, 0.4f, 0.5f, 0);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check((pixel(16, 36) & 0xFFFFFF) != 0 && (pixel(48, 36) & 0xFFFFFF) != 0 && pixel(32, 36) == 0xFF000000u, "a constant buffer changed between two draws: a small triangle on each side");
  if (vertices) ID3D11Buffer_Release(vertices);
}

static void test_byte_colours(void) {
  const VertexB v[3] = {{-0.8f, -0.8f, 0.5f, 255, 0, 0, 255}, {0.8f, -0.8f, 0.5f, 255, 0, 0, 255}, {0.0f, 0.8f, 0.5f, 255, 0, 0, 255}};
  ID3D11Buffer *vertices = make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, v, sizeof v);
  const UINT stride = sizeof(VertexB), offset = 0;
  begin(NULL);
  clear(0, 0, 0);
  ID3D11DeviceContext_IASetInputLayout(g_context, g_layout_bytes);
  ID3D11DeviceContext_IASetVertexBuffers(g_context, 0, 1, &vertices, &stride, &offset);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check(pixel(32, 40) == 0xFFFF0000u, "colours as four bytes in the vertices: red");
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (vertices && SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    VertexB *w = mapped.pData;
    memcpy(w, v, sizeof v);
    for (int i = 0; i < 3; ++i) { w[i].r = 0; w[i].b = 255; }
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)vertices, 0);
  }
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check(pixel(32, 40) == 0xFF0000FFu, "... and blue after the buffer was written again");
  if (vertices) ID3D11Buffer_Release(vertices);
}

static void test_pixel_constants(void) {
  /* ps_cbuf: the average of two of the 64 colours of its own constant buffer, plus a tenth of the vertex colour. All 64 are green here; the
   * vertex shader's matrix is in the vertex shader's b0, and is not what the pixel shader's b0 reads. */
  float colours[64][4];
  for (int i = 0; i < 64; ++i) { colours[i][0] = 0; colours[i][1] = 1; colours[i][2] = 0; colours[i][3] = 1; }
  ID3D11Buffer *constants = make_buffer(D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, colours, sizeof colours);
  ID3D11Buffer *vertices = triangle(0.5f, 0.1f, 0.1f, 0.1f);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_cbuf, NULL, 0);
  ID3D11DeviceContext_PSSetConstantBuffers(g_context, 0, 1, &constants);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check(green(pixel(32, 40)) == 255 && red(pixel(32, 40)) < 10 && blue(pixel(32, 40)) < 10, "the pixel shader's own constant buffer, beside the vertex shader's");
  for (int i = 0; i < 64; ++i) { colours[i][0] = 1; colours[i][1] = 0; }
  ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)constants, 0, NULL, colours, 0, 0);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check(red(pixel(32, 40)) == 255 && green(pixel(32, 40)) < 10, "... and after UpdateSubresource of it");
  ID3D11Buffer *none = NULL;
  ID3D11DeviceContext_PSSetConstantBuffers(g_context, 0, 1, &none);
  if (constants) ID3D11Buffer_Release(constants);
  if (vertices) ID3D11Buffer_Release(vertices);
}

static void test_texture(void) {
  /* ps_tex: the texture at twice the vertex colour's red and green, times the vertex colour. The colour (0.1, 0.1, 1) samples the first of 2 x 2
   * texels and keeps its blue. */
  const UINT32 texels[4] = {0xFFFFFFFFu, 0xFF000000u, 0xFF000000u, 0xFF000000u};
  D3D11_TEXTURE2D_DESC desc = {2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA initial = {texels, 8, 0};
  D3D11_SAMPLER_DESC sampler_desc;
  memset(&sampler_desc, 0, sizeof sampler_desc);
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11Texture2D *texture = NULL;
  ID3D11ShaderResourceView *view = NULL;
  ID3D11SamplerState *sampler = NULL;
  HRESULT hr = ID3D11Device_CreateTexture2D(g_device, &desc, &initial, &texture);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)texture, NULL, &view);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateSamplerState(g_device, &sampler_desc, &sampler);
  check(SUCCEEDED(hr) && texture && view && sampler, "a texture with its first pixels, a view of it, a sampler");
  if (FAILED(hr)) return;
  ID3D11Buffer *vertices = triangle(0.5f, 0.1f, 0.1f, 1.0f);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_tex, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &view);
  ID3D11DeviceContext_PSSetSamplers(g_context, 0, 1, &sampler);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check(blue(pixel(32, 40)) == 255 && red(pixel(32, 40)) > 15 && red(pixel(32, 40)) < 40, "a sampled texture: its white texel times the vertex colour");
  const UINT32 black = 0xFF000000u;
  const D3D11_BOX box = {0, 0, 0, 1, 1, 1};
  ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)texture, 0, &box, &black, 4, 0);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check(blue(pixel(32, 40)) == 0, "... and black after UpdateSubresource of that texel");

  /* a dynamic texture written through Map */
  D3D11_TEXTURE2D_DESC dynamic_desc = {2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0};
  ID3D11Texture2D *dynamic = NULL;
  ID3D11ShaderResourceView *dynamic_view = NULL;
  D3D11_MAPPED_SUBRESOURCE mapped;
  int written = SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &dynamic_desc, NULL, &dynamic)) &&
                SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)dynamic, NULL, &dynamic_view)) &&
                SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)dynamic, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData && mapped.RowPitch >= 8;
  if (written) {
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) ((UINT32 *)((BYTE *)mapped.pData + (size_t)y * mapped.RowPitch))[x] = 0xFFFFFFFFu;
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)dynamic, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &dynamic_view);
    ID3D11DeviceContext_Draw(g_context, 3, 0);
    read_target();
  }
  check(written && blue(pixel(32, 40)) == 255, "a dynamic texture filled through Map");
  ID3D11ShaderResourceView *bound = NULL;
  ID3D11DeviceContext_PSGetShaderResources(g_context, 0, 1, &bound);
  check(bound == (written ? dynamic_view : view), "PSGetShaderResources gives back what is bound");
  if (bound) ID3D11ShaderResourceView_Release(bound);
  ID3D11ShaderResourceView *none = NULL;
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &none);
  if (dynamic_view) ID3D11ShaderResourceView_Release(dynamic_view);
  if (dynamic) ID3D11Texture2D_Release(dynamic);
  if (vertices) ID3D11Buffer_Release(vertices);
  ID3D11SamplerState_Release(sampler);
  ID3D11ShaderResourceView_Release(view);
  ID3D11Texture2D_Release(texture);
}

static void test_depth_and_blend(void) {
  D3D11_TEXTURE2D_DESC desc = {SIZE, SIZE, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0};
  ID3D11Texture2D *depth = NULL;
  ID3D11DepthStencilView *depth_view = NULL;
  HRESULT hr = ID3D11Device_CreateTexture2D(g_device, &desc, NULL, &depth);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateDepthStencilView(g_device, (ID3D11Resource *)depth, NULL, &depth_view);
  check(SUCCEEDED(hr) && depth_view, "a depth buffer and its view");
  ID3D11Buffer *near_red = triangle(0.2f, 1, 0, 0), *far_green = triangle(0.8f, 0, 1, 0);
  if (SUCCEEDED(hr)) {
    begin(depth_view);
    clear(0, 0, 0);
    ID3D11DeviceContext_ClearDepthStencilView(g_context, depth_view, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11DeviceContext_Flush(g_context);      /* (the clear must not be lost with the list it was asked in) */
    ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
    bind_floats(near_red);
    ID3D11DeviceContext_Draw(g_context, 3, 0);
    bind_floats(far_green);
    ID3D11DeviceContext_Draw(g_context, 3, 0);
    read_target();
    check(pixel(32, 40) == 0xFFFF0000u, "the depth test (the default state): the nearer red triangle stays in front of the green one drawn after it");
    /* the same two with the depth test switched off: the later one wins */
    D3D11_DEPTH_STENCIL_DESC off_desc;
    memset(&off_desc, 0, sizeof off_desc);
    off_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState *off = NULL;
    ID3D11Device_CreateDepthStencilState(g_device, &off_desc, &off);
    ID3D11DeviceContext_OMSetDepthStencilState(g_context, off, 0);
    ID3D11DeviceContext_Draw(g_context, 3, 0);
    read_target();
    check(off && pixel(32, 40) == 0xFF00FF00u, "a depth-stencil state without the test: the green one is drawn over it");
    if (off) ID3D11DepthStencilState_Release(off);
  }
  /* adding blend: red, then green on top, is yellow */
  D3D11_BLEND_DESC blend_desc;
  memset(&blend_desc, 0, sizeof blend_desc);
  blend_desc.RenderTarget[0].BlendEnable = TRUE;
  blend_desc.RenderTarget[0].SrcBlend = blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].SrcBlendAlpha = blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].BlendOp = blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState *blend = NULL;
  ID3D11Device_CreateBlendState(g_device, &blend_desc, &blend);
  begin(NULL);
  clear(0, 0, 0);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  ID3D11DeviceContext_OMSetBlendState(g_context, blend, NULL, 0xFFFFFFFFu);
  bind_floats(near_red);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  bind_floats(far_green);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check(blend && red(pixel(32, 40)) == 255 && green(pixel(32, 40)) == 255 && blue(pixel(32, 40)) == 0, "a blend state that adds: red and green make yellow");
  ID3D11BlendState *bound = NULL;
  FLOAT factor[4];
  UINT mask = 0;
  ID3D11DeviceContext_OMGetBlendState(g_context, &bound, factor, &mask);
  check(bound == blend && mask == 0xFFFFFFFFu, "OMGetBlendState gives back the state and the mask");
  if (bound) ID3D11BlendState_Release(bound);
  ID3D11DeviceContext_OMSetBlendState(g_context, NULL, NULL, 0xFFFFFFFFu);
  if (blend) ID3D11BlendState_Release(blend);
  if (near_red) ID3D11Buffer_Release(near_red);
  if (far_green) ID3D11Buffer_Release(far_green);
  ID3D11RenderTargetView *no_target = NULL;
  ID3D11DeviceContext_OMSetRenderTargets(g_context, 1, &no_target, NULL);
  if (depth_view) ID3D11DepthStencilView_Release(depth_view);
  if (depth) ID3D11Texture2D_Release(depth);
}

static void test_indexed(void) {
  const VertexF v[4] = {{-1, -1, 0.5f, 0, 0, 1, 1}, {-1, 1, 0.5f, 0, 0, 1, 1}, {1, 1, 0.5f, 0, 0, 1, 1}, {1, -1, 0.5f, 0, 0, 1, 1}};
  const WORD indices[6] = {0, 1, 2, 0, 2, 3};
  ID3D11Buffer *vertices = make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, 0, v, sizeof v);
  ID3D11Buffer *index_buffer = make_buffer(D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DEFAULT, 0, indices, sizeof indices);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_IASetIndexBuffer(g_context, index_buffer, DXGI_FORMAT_R16_UINT, 0);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  ID3D11DeviceContext_DrawIndexed(g_context, 6, 0, 0);
  read_target();
  check(pixel(1, 1) == 0xFF0000FFu && pixel(62, 62) == 0xFF0000FFu && pixel(62, 1) == 0xFF0000FFu && pixel(1, 62) == 0xFF0000FFu, "DrawIndexed: two triangles cover the target");
  /* a scissor rectangle, with a rasterizer state that uses it */
  D3D11_RASTERIZER_DESC raster = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, TRUE, FALSE, FALSE};
  ID3D11RasterizerState *scissored = NULL;
  const D3D11_RECT rect = {0, 0, 32, SIZE};
  ID3D11Device_CreateRasterizerState(g_device, &raster, &scissored);
  clear(0, 0, 0);
  ID3D11DeviceContext_RSSetState(g_context, scissored);
  ID3D11DeviceContext_RSSetScissorRects(g_context, 1, &rect);
  ID3D11DeviceContext_DrawIndexed(g_context, 6, 0, 0);
  read_target();
  check(scissored && pixel(8, 32) == 0xFF0000FFu && pixel(56, 32) == 0xFF000000u, "a scissor rectangle keeps the draw to the left half");
  ID3D11Buffer *bound = NULL;
  DXGI_FORMAT format = 0;
  UINT offset = 99;
  D3D11_PRIMITIVE_TOPOLOGY topology = 0;
  ID3D11DeviceContext_IAGetIndexBuffer(g_context, &bound, &format, &offset);
  ID3D11DeviceContext_IAGetPrimitiveTopology(g_context, &topology);
  check(bound == index_buffer && format == DXGI_FORMAT_R16_UINT && offset == 0 && topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, "IAGetIndexBuffer and IAGetPrimitiveTopology");
  if (bound) ID3D11Buffer_Release(bound);
  ID3D11DeviceContext_RSSetState(g_context, g_no_cull);
  if (scissored) ID3D11RasterizerState_Release(scissored);
  if (vertices) ID3D11Buffer_Release(vertices);
  if (index_buffer) ID3D11Buffer_Release(index_buffer);
}

static void test_swap_chain(void) {
  WNDCLASSW cls;
  memset(&cls, 0, sizeof cls);
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(NULL);
  cls.lpszClassName = L"d3d11test";
  RegisterClassW(&cls);
  const HWND window = CreateWindowExW(0, L"d3d11test", L"d3d11test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, 160, 120, NULL, NULL, cls.hInstance, NULL);
  check(window != NULL, "a window for the swap chain");
  if (!window) return;
  DXGI_SWAP_CHAIN_DESC desc;
  memset(&desc, 0, sizeof desc);
  desc.BufferDesc.Width = 96;
  desc.BufferDesc.Height = 64;
  desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 1;
  desc.OutputWindow = window;
  desc.Windowed = TRUE;
  IDXGISwapChain *chain = NULL;
  ID3D11Device *device = NULL;
  ID3D11DeviceContext *context = NULL;
  const HRESULT made = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &desc, &chain, &device, NULL, &context);
  check(made == S_OK && chain && device && context, "D3D11CreateDeviceAndSwapChain");
  if (made == S_OK && chain && device && context) {
    ID3D11Texture2D *back = NULL, *copy = NULL;
    ID3D11RenderTargetView *view = NULL;
    D3D11_TEXTURE2D_DESC back_desc;
    memset(&back_desc, 0, sizeof back_desc);
    HRESULT hr = IDXGISwapChain_GetBuffer(chain, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (SUCCEEDED(hr) && back) ID3D11Texture2D_GetDesc(back, &back_desc);
    check(SUCCEEDED(hr) && back && back_desc.Width == 96 && back_desc.Height == 64, "the swap chain's back buffer as a texture");
    if (SUCCEEDED(hr) && back) hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &view);
    if (SUCCEEDED(hr) && view) {
      const float blue_colour[4] = {0, 0, 1, 1};
      ID3D11DeviceContext_ClearRenderTargetView(context, view, blue_colour);
      check(IDXGISwapChain_Present(chain, 0, 0) == S_OK, "Present");
      D3D11_TEXTURE2D_DESC copy_desc = back_desc;
      copy_desc.Usage = D3D11_USAGE_STAGING;
      copy_desc.BindFlags = 0;
      copy_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      D3D11_MAPPED_SUBRESOURCE mapped;
      UINT32 first = 0;
      if (SUCCEEDED(ID3D11Device_CreateTexture2D(device, &copy_desc, NULL, &copy)) && copy) {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)copy, (ID3D11Resource *)back);
        if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)copy, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
          first = *(const UINT32 *)mapped.pData;
          ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)copy, 0);
        }
      }
      check(first == 0xFF0000FFu, "the back buffer holds what was cleared into it");
    } else check(0, "a render target view of the back buffer");
    if (copy) ID3D11Texture2D_Release(copy);
    if (view) ID3D11RenderTargetView_Release(view);
    if (back) ID3D11Texture2D_Release(back);
  }
  if (chain) IDXGISwapChain_Release(chain);
  if (context) ID3D11DeviceContext_Release(context);
  if (device) check(ID3D11Device_Release(device) == 0, "the second device is gone with its last reference");
  DestroyWindow(window);
}

/* Multisampling, as Skia (Chrome's compositor) wants it of an OpenGL ES 3 context: a 4x render target drawn into and resolved into an ordinary texture.
 * An adapter without 4x says so and refuses the texture. */
static void test_multisample(void) {
  UINT levels = 0;
  check(SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(g_device, DXGI_FORMAT_B8G8R8A8_UNORM, 4, &levels)), "CheckMultisampleQualityLevels for 4 samples");
  D3D11_TEXTURE2D_DESC desc = {SIZE, SIZE, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {4, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
  ID3D11Texture2D *ms = NULL;
  const HRESULT made = ID3D11Device_CreateTexture2D(g_device, &desc, NULL, &ms);
  UINT support = 0;
  ID3D11Device_CheckFormatSupport(g_device, DXGI_FORMAT_B8G8R8A8_UNORM, &support);
  if (levels == 0) {
    printf("d3d11test: (this adapter has no 4x multisampling)\n");
    check(FAILED(made) && !ms && !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET), "... so a 4x texture is refused and the format does not claim it");
    if (ms) ID3D11Texture2D_Release(ms);
    return;
  }
  check(SUCCEEDED(made) && ms && (support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) && (support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE),
        "a 4x render target, and the format says it can be one");
  ID3D11RenderTargetView *view = NULL;
  check(ms && SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)ms, NULL, &view)) && view, "... its view");
  if (!view) { if (ms) ID3D11Texture2D_Release(ms); return; }
  D3D11_RENDER_TARGET_VIEW_DESC vd;
  ID3D11RenderTargetView_GetDesc(view, &vd);
  check(vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS, "... which is a multisampled view");
  begin(NULL);
  ID3D11DeviceContext_OMSetRenderTargets(g_context, 1, &view, NULL);
  const float sky[4] = {0, 0, 1, 1};
  ID3D11DeviceContext_ClearRenderTargetView(g_context, view, sky);
  ID3D11Buffer *vertices = triangle(0.5f, 0, 1, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11DeviceContext_ResolveSubresource(g_context, (ID3D11Resource *)g_target, 0, (ID3D11Resource *)ms, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
  check(read_target(), "ResolveSubresource into the ordinary target, read back");
  int green_inside = 0, mixed = 0;
  for (int i = 0; i < SIZE * SIZE; ++i) {
    const UINT32 p = g_pixels[i];
    if (green(p) > 250 && blue(p) < 5) ++green_inside;
    else if (green(p) > 20 && blue(p) > 20) ++mixed;      /* (an edge pixel: some samples green, some blue) */
  }
  printf("d3d11test: (resolved: corner %08x, middle %08x, %d green, %d on the edges)\n", (unsigned)pixel(0, 0), (unsigned)pixel(32, 32), green_inside, mixed);
  check(blue(pixel(0, 0)) > 250 && green(pixel(0, 0)) < 5 && green(pixel(32, 32)) > 250 && green_inside > 600, "the triangle drawn over the clear colour");
  check(mixed > 0, "edge pixels mix both colours (the samples were kept apart and averaged)");
  ID3D11Buffer_Release(vertices);
  ID3D11RenderTargetView_Release(view);
  ID3D11Texture2D_Release(ms);
  begin(NULL);
}

/* One- and two-channel textures, as Chrome keeps the planes of video frames: an R8 render target cleared and drawn into (only red is written), and RG8 data
 * that comes back as it went in. */
static void test_small_formats(void) {
  D3D11_TEXTURE2D_DESC desc = {SIZE, SIZE, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_TEXTURE2D_DESC staging = {SIZE, SIZE, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
  ID3D11Texture2D *r8 = NULL, *r8_read = NULL;
  ID3D11RenderTargetView *view = NULL;
  UINT support = 0;
  check(SUCCEEDED(ID3D11Device_CheckFormatSupport(g_device, DXGI_FORMAT_R8_UNORM, &support)) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET), "R8 can be a render target");
  check(SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &desc, NULL, &r8)) && SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &staging, NULL, &r8_read)) &&
        SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)r8, NULL, &view)), "an R8 render target, its view and an R8 staging texture");
  if (view && r8_read) {
    begin(NULL);
    ID3D11DeviceContext_OMSetRenderTargets(g_context, 1, &view, NULL);
    const float half[4] = {0.5f, 0.75f, 0.25f, 0.5f};
    ID3D11DeviceContext_ClearRenderTargetView(g_context, view, half);
    ID3D11Buffer *vertices = triangle(0.5f, 1, 1, 1);      /* (white: 1 into red) */
    bind_floats(vertices);
    ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
    ID3D11DeviceContext_Draw(g_context, 3, 0);
    ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)r8_read, (ID3D11Resource *)r8);
    D3D11_MAPPED_SUBRESOURCE m;
    int corner = -1, middle = -1;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)r8_read, 0, D3D11_MAP_READ, 0, &m))) {
      const BYTE *b = m.pData;
      corner = b[0];
      middle = b[32 * m.RowPitch + 32];
      ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)r8_read, 0);
    }
    printf("d3d11test: (R8 target: corner %d, middle %d)\n", corner, middle);
    check(corner >= 127 && corner <= 128 && middle == 255, "an R8 target holds the clear's red and the draw's red, one byte a pixel");
    ID3D11Buffer_Release(vertices);
    begin(NULL);
  }
  if (view) ID3D11RenderTargetView_Release(view);
  if (r8) ID3D11Texture2D_Release(r8);
  if (r8_read) ID3D11Texture2D_Release(r8_read);

  BYTE rg[4 * 2 * 2];
  for (int i = 0; i < 8; ++i) { rg[2 * i] = (BYTE)(10 + i); rg[2 * i + 1] = (BYTE)(200 - i); }
  D3D11_TEXTURE2D_DESC rg_desc = {4, 2, 1, 1, DXGI_FORMAT_R8G8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, 0};
  D3D11_SUBRESOURCE_DATA initial = {rg, 8, 0};
  ID3D11Texture2D *rg8 = NULL;
  int same = 0;
  if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &rg_desc, &initial, &rg8)) && rg8) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)rg8, 0, D3D11_MAP_READ, 0, &m))) {
      same = 1;
      for (int y = 0; y < 2; ++y) for (int x = 0; x < 8; ++x) same &= ((const BYTE *)m.pData)[y * m.RowPitch + x] == rg[y * 8 + x];
      ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)rg8, 0);
    }
    ID3D11Texture2D_Release(rg8);
  }
  check(same, "an RG8 texture gives back the two bytes a pixel it was made with");
}

int main(void) {
  test_device();
  if (g_device && g_context) {
    test_buffers();
    if (make_scene()) {
      test_triangle();
      test_byte_colours();
      test_pixel_constants();
      test_texture();
      test_depth_and_blend();
      test_indexed();
      test_multisample();
      test_small_formats();
    }
    ID3D11DeviceContext_ClearState(g_context);
    ID3D11DeviceContext_Flush(g_context);
    test_swap_chain();
  }
  if (g_failed == 0) printf("d3d11test: all %d checks passed\n", g_passed);
  else printf("d3d11test: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
