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
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <d3d12.h>
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
  D3D11_COUNTER_INFO counter_info = {1, 1, 0};
  ID3D11Device_CheckCounterInfo(g_device, &counter_info);
  check(counter_info.LastDeviceDependentCounter == 0 && counter_info.NumSimultaneousCounters == 0 && counter_info.NumDetectableParallelUnits == 1 &&
        ID3D11Device_CreateCounter(g_device, &counter_desc, &counter) == DXGI_ERROR_UNSUPPORTED && counter == NULL,
        "the device has no counters of the driver's: CheckCounterInfo says none, CreateCounter answers DXGI_ERROR_UNSUPPORTED (and leaves the stack whole)");

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

/* A buffer written again between two draws of one list (no readback between them, so both are in the list together): each draw has the vertices it was
 * drawn with. Map with WRITE_DISCARD on a buffer whose colours are four bytes (the runtime draws from a converted copy), then UpdateSubresource and a
 * buffer-to-buffer copy on one of floats. */
static void test_rewrite_in_one_list(void) {
  const VertexB left[3] = {{-0.9f, -0.5f, 0.5f, 255, 0, 0, 255}, {-0.1f, -0.5f, 0.5f, 255, 0, 0, 255}, {-0.5f, 0.5f, 0.5f, 255, 0, 0, 255}};
  ID3D11Buffer *bytes = make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, left, sizeof left);
  const UINT stride_b = sizeof(VertexB), offset = 0;
  begin(NULL);
  clear(0, 0, 0);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  ID3D11DeviceContext_IASetInputLayout(g_context, g_layout_bytes);
  ID3D11DeviceContext_IASetVertexBuffers(g_context, 0, 1, &bytes, &stride_b, &offset);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  D3D11_MAPPED_SUBRESOURCE mapped;
  int wrote = bytes && SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)bytes, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
  if (wrote) {
    VertexB *w = mapped.pData;
    for (int i = 0; i < 3; ++i) { w[i] = left[i]; w[i].x += 1.0f; w[i].r = 0; w[i].b = 255; }      /* the right half, blue */
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)bytes, 0);
  }
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check(wrote && pixel(16, 32) == 0xFFFF0000u && pixel(48, 32) == 0xFF0000FFu, "Map(WRITE_DISCARD) between two draws of one list: the first keeps its vertices (red left, blue right)");
  if (bytes) ID3D11Buffer_Release(bytes);

  const VertexF top[3] = {{-0.5f, 0.1f, 0.5f, 0, 1, 0, 1}, {0.5f, 0.1f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.9f, 0.5f, 0, 1, 0, 1}};
  VertexF bottom[3];
  for (int i = 0; i < 3; ++i) { bottom[i] = top[i]; bottom[i].y -= 1.0f; bottom[i].g = 0; bottom[i].r = 1; }
  ID3D11Buffer *floats = make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, 0, top, sizeof top);
  ID3D11Buffer *source = make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, 0, top, sizeof top);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(floats);
  ID3D11DeviceContext_Draw(g_context, 3, 0);                                                     /* green, top */
  ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)floats, 0, NULL, bottom, 0, 0);
  ID3D11DeviceContext_Draw(g_context, 3, 0);                                                     /* red, bottom */
  ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)source, 0, NULL, top, 0, 0);
  VertexF moved[3];
  for (int i = 0; i < 3; ++i) { moved[i] = top[i]; moved[i].y -= 1.0f; moved[i].x -= 0.5f; moved[i].g = 0; moved[i].b = 1; }      /* lower left, blue */
  ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)source, 0, NULL, moved, 0, 0);
  ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)floats, (ID3D11Resource *)source);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  check(green(pixel(32, 16)) == 255 && red(pixel(32, 16)) == 0 && red(pixel(34, 52)) == 255 && blue(pixel(14, 50)) == 255,
        "UpdateSubresource and CopyResource between draws of one list: each draw has the vertices it was drawn with");
  if (floats) ID3D11Buffer_Release(floats);
  if (source) ID3D11Buffer_Release(source);
}

/* Many draws of one list from one buffer at different offsets, as ANGLE streams vertices: with colours as bytes each offset is a converted copy of its own. */
static void test_streamed_offsets(void) {
  enum { N = 16 };
  VertexB v[N * 3];
  for (int i = 0; i < N; ++i) {
    const float cx = -0.75f + (float)(i % 4) * 0.5f, cy = 0.75f - (float)(i / 4) * 0.5f;
    for (int k = 0; k < 3; ++k) {
      VertexB *p = &v[i * 3 + k];
      p->x = cx + (k == 0 ? -0.2f : k == 1 ? 0.2f : 0.0f);
      p->y = cy + (k == 2 ? 0.2f : -0.2f);
      p->z = 0.5f;
      p->r = (BYTE)(i * 16 + 15); p->g = (BYTE)(255 - i * 16); p->b = (BYTE)(i & 1 ? 255 : 0); p->a = 255;
    }
  }
  ID3D11Buffer *buffer = make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, v, sizeof v);
  begin(NULL);
  clear(0, 0, 0);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  ID3D11DeviceContext_IASetInputLayout(g_context, g_layout_bytes);
  const UINT stride = sizeof(VertexB);
  for (int i = 0; i < N; ++i) {
    const UINT offset = (UINT)(i * 3) * stride;
    ID3D11DeviceContext_IASetVertexBuffers(g_context, 0, 1, &buffer, &stride, &offset);
    ID3D11DeviceContext_Draw(g_context, 3, 0);
  }
  read_target();
  int good = 0;
  for (int i = 0; i < N; ++i) {
    const int x = 8 + (i % 4) * 16, y = 8 + (i / 4) * 16 + 2;
    const UINT32 p = pixel(x, y);
    if (red(p) == i * 16 + 15 && green(p) == 255 - i * 16 && blue(p) == (i & 1 ? 255 : 0)) ++good;
  }
  check(good == N, "sixteen draws of one list from one buffer at sixteen offsets (colours as bytes): each triangle in its place and colour");
  if (good != N) printf("d3d11test: (%d of %d right)\n", good, (int)N);
  if (buffer) ID3D11Buffer_Release(buffer);
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

/* R8 and RG8 planes sampled the way Chrome's video shaders read them: ps_tex with a white vertex colour samples (2, 2), which the clamp turns into the
 * last texel, and gives it back unchanged. Every other texel holds something else, and the widths are odd, so a row laid out wrongly (pitch, bytes a
 * pixel) shows. Each plane is filled four ways: at creation, by UpdateSubresource, through Map of a dynamic texture, and (as ANGLE uploads) through Map of
 * a staging texture copied by CopySubresourceRegion into one that is also a render target. */
static UINT32 sample_plane(ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler, ID3D11Buffer *vertices) {
  begin(NULL);
  clear(0, 0, 1);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_tex, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &view);
  ID3D11DeviceContext_PSSetSamplers(g_context, 0, 1, &sampler);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11ShaderResourceView *none = NULL;
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &none);
  return read_target() ? pixel(32, 40) : 0;
}
static void test_sampled_planes(void) {
  D3D11_SAMPLER_DESC sampler_desc;
  memset(&sampler_desc, 0, sizeof sampler_desc);
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *sampler = NULL;
  ID3D11Buffer *vertices = triangle(0.5f, 1, 1, 1);
  if (FAILED(ID3D11Device_CreateSamplerState(g_device, &sampler_desc, &sampler)) || !vertices) { check(0, "a sampler and a triangle for the planes"); return; }
  static const struct { DXGI_FORMAT format; UINT w, h, bpp; BYTE last[2]; const char *name; } planes[2] = {
      {DXGI_FORMAT_R8_UNORM, 5, 3, 1, {100, 0}, "R8"}, {DXGI_FORMAT_R8G8_UNORM, 3, 3, 2, {50, 200}, "RG8"}};
  for (int p = 0; p < 2; ++p) {
    const UINT w = planes[p].w, h = planes[p].h, bpp = planes[p].bpp, pitch = 16;      /* (rows further apart than their bytes, as a decoder's are) */
    BYTE bytes[16 * 3];
    memset(bytes, 7, sizeof bytes);
    memcpy(bytes + (h - 1) * pitch + (w - 1) * bpp, planes[p].last, bpp);
    const int want_r = planes[p].last[0], want_g = bpp == 2 ? planes[p].last[1] : 0;
    char what[160];
    for (int way = 0; way < 4; ++way) {
      D3D11_TEXTURE2D_DESC desc = {w, h, 1, 1, planes[p].format, {1, 0}, way == 2 ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE,
                                   way == 2 ? D3D11_CPU_ACCESS_WRITE : 0, 0};
      if (way == 3) desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
      D3D11_SUBRESOURCE_DATA initial = {bytes, pitch, 0};
      ID3D11Texture2D *texture = NULL, *staging = NULL;
      ID3D11ShaderResourceView *view = NULL;
      int made = SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &desc, way == 0 ? &initial : NULL, &texture)) &&
                 SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)texture, NULL, &view));
      if (made && way == 3) {
        D3D11_TEXTURE2D_DESC staging_desc = {w, h, 1, 1, planes[p].format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE, 0};
        D3D11_MAPPED_SUBRESOURCE m;
        made = SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &staging_desc, NULL, &staging)) &&
               SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_WRITE, 0, &m)) && m.pData && m.RowPitch >= w * bpp;
        if (made) {
          for (UINT y = 0; y < h; ++y) memcpy((BYTE *)m.pData + (size_t)y * m.RowPitch, bytes + y * pitch, w * bpp);
          ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
          const D3D11_BOX all = {0, 0, 0, w, h, 1};
          ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)texture, 0, 0, 0, 0, (ID3D11Resource *)staging, 0, &all);
        }
      }
      if (made && way == 1) ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)texture, 0, NULL, bytes, pitch, 0);
      if (made && way == 2) {
        D3D11_MAPPED_SUBRESOURCE m;
        made = SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) && m.pData && m.RowPitch >= w * bpp;
        if (made) {
          for (UINT y = 0; y < h; ++y) memcpy((BYTE *)m.pData + (size_t)y * m.RowPitch, bytes + y * pitch, w * bpp);
          ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)texture, 0);
        }
      }
      const UINT32 got = made ? sample_plane(view, sampler, vertices) : 0;
      snprintf(what, sizeof what, "%s plane %s samples as (%d, %d, 0): got (%d, %d, %d)", planes[p].name,
               way == 0 ? "made with its data" : way == 1 ? "filled by UpdateSubresource" : way == 2 ? "filled through Map" : "copied from a staging texture", want_r, want_g, red(got), green(got), blue(got));
      check(made && red(got) == want_r && green(got) == want_g && blue(got) == 0, what);
      if (view) ID3D11ShaderResourceView_Release(view);
      if (texture) ID3D11Texture2D_Release(texture);
      if (staging) ID3D11Texture2D_Release(staging);
    }
  }
  ID3D11Buffer_Release(vertices);
  ID3D11SamplerState_Release(sampler);
  begin(NULL);
}

/* An I420 frame as Chrome draws one on the GPU: three R8 planes (also render targets, as ANGLE makes them), each filled by UpdateSubresource and each
 * sampled with its own sampler in one draw (ps_planes: Y, U and V into red, green and blue). The clamp at (2, 2) reads each plane's last texel.
 * `packed`: through vs_packed / ps_packed instead, whose plane coordinates the compiler packs two to a register as fxc does ANGLE's (Chrome's video showed
 * only its V plane on virgl while the two stages linked such a register through different varyings). */
static void test_three_planes(int packed) {
  ID3D11PixelShader *ps = NULL;
  ID3D11VertexShader *vs = NULL;
  ID3D11InputLayout *layout = NULL;
  const D3D11_INPUT_ELEMENT_DESC floats[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                              {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  if (packed ? FAILED(ID3D11Device_CreatePixelShader(g_device, dx_ps_packed_dxbc, sizeof dx_ps_packed_dxbc, NULL, &ps)) ||
                   FAILED(ID3D11Device_CreateVertexShader(g_device, dx_vs_packed_dxbc, sizeof dx_vs_packed_dxbc, NULL, &vs)) ||
                   FAILED(ID3D11Device_CreateInputLayout(g_device, floats, 2, dx_vs_packed_dxbc, sizeof dx_vs_packed_dxbc, &layout))
             : FAILED(ID3D11Device_CreatePixelShader(g_device, dx_ps_planes_dxbc, sizeof dx_ps_planes_dxbc, NULL, &ps))) {
    check(0, packed ? "the shaders with packed plane coordinates" : "the three-plane pixel shader");
    if (ps) ID3D11PixelShader_Release(ps);
    if (vs) ID3D11VertexShader_Release(vs);
    return;
  }
  static const struct { UINT w, h; BYTE last; } planes[3] = {{9, 5, 200}, {5, 3, 60}, {5, 3, 150}};
  ID3D11Texture2D *textures[3] = {0};
  ID3D11ShaderResourceView *views[3] = {0};
  ID3D11SamplerState *samplers[3] = {0};
  int made = 1;
  for (int i = 0; i < 3; ++i) {
    D3D11_TEXTURE2D_DESC desc = {planes[i].w, planes[i].h, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
    D3D11_SAMPLER_DESC sampler_desc;
    memset(&sampler_desc, 0, sizeof sampler_desc);
    sampler_desc.Filter = i == 0 ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    made &= SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &desc, NULL, &textures[i])) &&
            SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)textures[i], NULL, &views[i])) &&
            SUCCEEDED(ID3D11Device_CreateSamplerState(g_device, &sampler_desc, &samplers[i]));
  }
  UINT32 got = 0;
  ID3D11Buffer *vertices = triangle(0.5f, 1, 1, 1);
  if (made && vertices) {
    for (int frame = 0; frame < 2; ++frame) {      /* (the second frame writes the planes again, as each new video frame does) */
      for (int i = 0; i < 3; ++i) {
        BYTE bytes[16 * 5];
        const UINT pitch = 16;
        memset(bytes, frame ? 30 : 7, sizeof bytes);
        bytes[(planes[i].h - 1) * pitch + planes[i].w - 1] = (BYTE)(planes[i].last - frame);
        ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)textures[i], 0, NULL, bytes, pitch, 0);
      }
      begin(NULL);
      clear(0, 0, 0);
      bind_floats(vertices);
      if (packed) {
        ID3D11DeviceContext_IASetInputLayout(g_context, layout);
        ID3D11DeviceContext_VSSetShader(g_context, vs, NULL, 0);
      }
      ID3D11DeviceContext_PSSetShader(g_context, ps, NULL, 0);
      ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 3, views);
      ID3D11DeviceContext_PSSetSamplers(g_context, 0, 3, samplers);
      ID3D11DeviceContext_Draw(g_context, 3, 0);
      ID3D11ShaderResourceView *none[3] = {0};
      ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 3, none);
      got = read_target() ? pixel(32, 40) : 0;
      char what[160];
      snprintf(what, sizeof what, "frame %d of three R8 planes in one draw%s reads Y, U, V = (%d, %d, %d): got (%d, %d, %d)", frame + 1,
               packed ? " (packed coordinates)" : "", planes[0].last - frame,
               planes[1].last - frame, planes[2].last - frame, red(got), green(got), blue(got));
      check(red(got) == planes[0].last - frame && green(got) == planes[1].last - frame && blue(got) == planes[2].last - frame, what);
    }
  } else check(0, "three R8 planes, their views and samplers");
  if (vertices) ID3D11Buffer_Release(vertices);
  for (int i = 0; i < 3; ++i) {
    if (samplers[i]) ID3D11SamplerState_Release(samplers[i]);
    if (views[i]) ID3D11ShaderResourceView_Release(views[i]);
    if (textures[i]) ID3D11Texture2D_Release(textures[i]);
  }
  ID3D11PixelShader_Release(ps);
  if (vs) ID3D11VertexShader_Release(vs);
  if (layout) ID3D11InputLayout_Release(layout);
  begin(NULL);
}

/* ---- volumes, arrays and cubes ------------------------------------------------------------------------------------------------------------------------------------------- */

static ID3D11SamplerState *point_sampler(void) {
  D3D11_SAMPLER_DESC desc;
  memset(&desc, 0, sizeof desc);
  desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  desc.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *sampler = NULL;
  ID3D11Device_CreateSamplerState(g_device, &desc, &sampler);
  return sampler;
}
/* Draws the middle triangle with vs_vol (the texture coordinate is the vertex colour: a direction, a layer or a depth) and `ps` sampling `view`; the pixel in the middle. */
static UINT32 draw_lookup(ID3D11VertexShader *vs, ID3D11PixelShader *ps, ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler, float x, float y, float z) {
  ID3D11Buffer *vertices = triangle(0.5f, x, y, z);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_VSSetShader(g_context, vs, NULL, 0);
  ID3D11DeviceContext_PSSetShader(g_context, ps, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &view);
  ID3D11DeviceContext_PSSetSamplers(g_context, 0, 1, &sampler);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11ShaderResourceView *none = NULL;
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &none);
  const UINT32 got = read_target() ? pixel(32, 40) : 0;
  if (vertices) ID3D11Buffer_Release(vertices);
  return got;
}

static void test_texture_kinds(void) {
  ID3D11VertexShader *vs = NULL;
  ID3D11PixelShader *ps_cube = NULL, *ps_array = NULL, *ps_volume = NULL;
  ID3D11SamplerState *sampler = point_sampler();
  HRESULT hr = ID3D11Device_CreateVertexShader(g_device, dx_vs_vol_dxbc, sizeof dx_vs_vol_dxbc, NULL, &vs);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_texcube_dxbc, sizeof dx_ps_texcube_dxbc, NULL, &ps_cube);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_texarray_dxbc, sizeof dx_ps_texarray_dxbc, NULL, &ps_array);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_tex3d_dxbc, sizeof dx_ps_tex3d_dxbc, NULL, &ps_volume);
  check(SUCCEEDED(hr) && sampler, "shaders that sample a cube, an array and a volume");
  if (FAILED(hr) || !sampler) goto done;

  /* A cube of 4 x 4 faces with all its levels (the program's subresources face by face, level by level), each face its own colour. */
  {
    static const UINT32 faces[6] = {0xFFFF0000u, 0xFF00FFFFu, 0xFF00FF00u, 0xFFFF00FFu, 0xFF0000FFu, 0xFFFFFF00u};      /* +X red, -X cyan, +Y green, -Y magenta, +Z blue, -Z yellow (as BGRA words) */
    static UINT32 pixels[6][3][16];
    D3D11_SUBRESOURCE_DATA initial[6 * 3];
    for (int f = 0; f < 6; ++f)
      for (int level = 0; level < 3; ++level) {
        for (int i = 0; i < 16; ++i) pixels[f][level][i] = faces[f];
        initial[f * 3 + level].pSysMem = pixels[f][level];
        initial[f * 3 + level].SysMemPitch = (4 >> level) * 4;
        initial[f * 3 + level].SysMemSlicePitch = 0;
      }
    D3D11_TEXTURE2D_DESC desc = {4, 4, 0, 6, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE};
    ID3D11Texture2D *cube = NULL;
    ID3D11ShaderResourceView *view = NULL;
    hr = ID3D11Device_CreateTexture2D(g_device, &desc, initial, &cube);
    D3D11_TEXTURE2D_DESC got_desc;
    memset(&got_desc, 0, sizeof got_desc);
    if (cube) ID3D11Texture2D_GetDesc(cube, &got_desc);
    check(SUCCEEDED(hr) && cube && got_desc.MipLevels == 3 && got_desc.ArraySize == 6 && (got_desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE), "a cube texture with all its levels, and its description");
    if (cube) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)cube, NULL, &view);
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc;
    memset(&view_desc, 0, sizeof view_desc);
    if (view) ID3D11ShaderResourceView_GetDesc(view, &view_desc);
    check(view && view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE && view_desc.TextureCube.MipLevels == 3, "a view of the cube is a TEXTURECUBE view");
    if (view) {
      static const float directions[6][3] = {{1, 0.2f, 0.1f}, {-1, 0.1f, 0.2f}, {0.2f, 1, 0.1f}, {0.1f, -1, 0.2f}, {0.2f, 0.1f, 1}, {0.1f, 0.2f, -1}};
      int good = 0;
      char what[160];
      for (int f = 0; f < 6; ++f) {
        const UINT32 got = draw_lookup(vs, ps_cube, view, sampler, directions[f][0], directions[f][1], directions[f][2]);
        if ((got & 0xFFFFFFu) == (faces[f] & 0xFFFFFFu)) ++good;
        else { snprintf(what, sizeof what, "cube face %d: got %08X, wanted %08X", f, (unsigned)got, (unsigned)faces[f]); check(0, what); }
      }
      check(good == 6, "a cube sampled by direction: each of the six faces");
      /* UpdateSubresource of face -Z (subresource 5 * 3 levels + level 0) */
      UINT32 white[16];
      for (int i = 0; i < 16; ++i) white[i] = 0xFFFFFFFFu;
      ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)cube, (5 * 3 + 0), NULL, white, 16, 0);
      const UINT32 got = draw_lookup(vs, ps_cube, view, sampler, directions[5][0], directions[5][1], directions[5][2]);
      check((got & 0xFFFFFFu) == 0xFFFFFFu, "UpdateSubresource of a face of the cube");
      ID3D11ShaderResourceView_Release(view);
    }
    if (cube) ID3D11Texture2D_Release(cube);
    /* nothing bound to a slot the shader declares as a cube: a white cube is sampled (the draw is not left out) */
    const UINT32 blank = draw_lookup(vs, ps_cube, NULL, sampler, 1, 0, 0);
    check((blank & 0xFFFFFFu) == 0xFFFFFFu, "a cube slot with nothing bound samples white");
    desc.ArraySize = 12;
    ID3D11Texture2D *array_of_cubes = NULL;
    check(ID3D11Device_CreateTexture2D(g_device, &desc, NULL, &array_of_cubes) == DXGI_ERROR_UNSUPPORTED && !array_of_cubes, "an array of cubes is refused (not there)");
    if (array_of_cubes) ID3D11Texture2D_Release(array_of_cubes);
  }

  /* An array of four layers of 2 x 2, sampled at layer 2; and layer 1 changed with UpdateSubresource. */
  {
    static UINT32 layers[4][4];
    static const UINT32 colours[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    D3D11_SUBRESOURCE_DATA initial[4];
    for (int l = 0; l < 4; ++l) {
      for (int i = 0; i < 4; ++i) layers[l][i] = colours[l];
      initial[l].pSysMem = layers[l];
      initial[l].SysMemPitch = 8;
      initial[l].SysMemSlicePitch = 0;
    }
    D3D11_TEXTURE2D_DESC desc = {2, 2, 1, 4, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *array = NULL;
    ID3D11ShaderResourceView *view = NULL;
    hr = ID3D11Device_CreateTexture2D(g_device, &desc, initial, &array);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)array, NULL, &view);
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc;
    memset(&view_desc, 0, sizeof view_desc);
    if (view) ID3D11ShaderResourceView_GetDesc(view, &view_desc);
    check(SUCCEEDED(hr) && view && view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY && view_desc.Texture2DArray.ArraySize == 4, "a 2D array of four layers and its view");
    if (view) {
      const UINT32 got = draw_lookup(vs, ps_array, view, sampler, 0.5f, 0.5f, 2.0f);
      check((got & 0xFFFFFFu) == 0x0000FFu, "an array sampled at layer 2");
      UINT32 grey[4] = {0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u};
      ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)array, 1, NULL, grey, 8, 0);
      const UINT32 again = draw_lookup(vs, ps_array, view, sampler, 0.5f, 0.5f, 1.0f);
      check(red(again) == 0x80 && green(again) == 0x80, "UpdateSubresource of layer 1, sampled");
      ID3D11ShaderResourceView_Release(view);
    }
    if (array) ID3D11Texture2D_Release(array);
  }

  /* A volume of 2 x 2 x 4: its slices from the initial data, then slice 2 through UpdateSubresource with a box. */
  {
    static UINT32 slices[4][4];
    static const UINT32 colours[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u};
    for (int z = 0; z < 4; ++z) for (int i = 0; i < 4; ++i) slices[z][i] = colours[z];
    D3D11_SUBRESOURCE_DATA initial = {slices, 8, 16};
    D3D11_TEXTURE3D_DESC desc = {2, 2, 4, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture3D *volume = NULL;
    ID3D11ShaderResourceView *view = NULL;
    hr = ID3D11Device_CreateTexture3D(g_device, &desc, &initial, &volume);
    D3D11_TEXTURE3D_DESC got_desc;
    D3D11_RESOURCE_DIMENSION type = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    memset(&got_desc, 0, sizeof got_desc);
    if (volume) { ID3D11Texture3D_GetDesc(volume, &got_desc); ID3D11Texture3D_GetType(volume, &type); }
    check(SUCCEEDED(hr) && volume && got_desc.Depth == 4 && got_desc.Width == 2 && type == D3D11_RESOURCE_DIMENSION_TEXTURE3D, "CreateTexture3D: a volume, its description and type");
    if (volume) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)volume, NULL, &view);
    if (view) {
      int good = 1;
      for (int z = 0; z < 4; ++z) {
        const UINT32 got = draw_lookup(vs, ps_volume, view, sampler, 0.5f, 0.5f, (z + 0.5f) / 4);
        if ((got & 0xFFFFFFu) != (colours[z] & 0xFFFFFFu)) {
          char what[120];
          snprintf(what, sizeof what, "volume slice %d: got %08X", z, (unsigned)got);
          check(0, what);
          good = 0;
        }
      }
      check(good, "a volume sampled at the middle of each of its four slices");
      UINT32 white[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
      const D3D11_BOX box = {0, 0, 2, 2, 2, 3};
      ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)volume, 0, &box, white, 8, 16);
      const UINT32 got = draw_lookup(vs, ps_volume, view, sampler, 0.5f, 0.5f, 2.5f / 4);
      const UINT32 other = draw_lookup(vs, ps_volume, view, sampler, 0.5f, 0.5f, 1.5f / 4);
      check((got & 0xFFFFFFu) == 0xFFFFFFu && (other & 0xFFFFFFu) == 0x00FF00u, "UpdateSubresource of one slice of the volume");
      ID3D11ShaderResourceView_Release(view);
    }
    if (volume) ID3D11Texture3D_Release(volume);
  }
done:
  if (sampler) ID3D11SamplerState_Release(sampler);
  if (ps_cube) ID3D11PixelShader_Release(ps_cube);
  if (ps_array) ID3D11PixelShader_Release(ps_array);
  if (ps_volume) ID3D11PixelShader_Release(ps_volume);
  if (vs) ID3D11VertexShader_Release(vs);
  begin(NULL);
}

/* ---- geometry, hull and domain shaders ----------------------------------------------------------------------------------------------------------------------------------- */

static void test_shader_stages(void) {
  ID3D11GeometryShader *gs = NULL;
  ID3D11HullShader *hs = NULL;
  ID3D11DomainShader *ds = NULL;
  HRESULT hr = ID3D11Device_CreateGeometryShader(g_device, dx_gs_copy_dxbc, sizeof dx_gs_copy_dxbc, NULL, &gs);
  check(SUCCEEDED(hr) && gs, "CreateGeometryShader");
  if (gs) {
    /* gs_copy: the triangle as it is, and a copy half the size moved to the lower right with its colour's red and blue exchanged */
    ID3D11Buffer *vertices = triangle(0.5f, 1, 0, 0);
    begin(NULL);
    clear(0, 0, 0);
    bind_floats(vertices);
    ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
    ID3D11DeviceContext_GSSetShader(g_context, gs, NULL, 0);
    ID3D11DeviceContext_Draw(g_context, 3, 0);
    ID3D11GeometryShader *bound = NULL;
    ID3D11DeviceContext_GSGetShader(g_context, &bound, NULL, NULL);
    check(bound == gs, "GSGetShader gives back what is bound");
    if (bound) ID3D11GeometryShader_Release(bound);
    ID3D11DeviceContext_GSSetShader(g_context, NULL, NULL, 0);
    read_target();
    /* (the copy: x / 2 + 0.5, y / 2 - 0.5; its middle is at about (48, 44) of 64, below the original's apex region) */
    check(red(pixel(32, 30)) == 255 && blue(pixel(32, 30)) == 0, "a geometry shader: the triangle it passes on");
    check(blue(pixel(48, 54)) == 255 && red(pixel(48, 54)) == 0, "... and the copy it adds, in the lower right with red and blue exchanged");
    if (vertices) ID3D11Buffer_Release(vertices);
    ID3D11GeometryShader_Release(gs);
  }

  hr = ID3D11Device_CreateHullShader(g_device, dx_hs_tri_cb_dxbc, sizeof dx_hs_tri_cb_dxbc, NULL, &hs);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateDomainShader(g_device, dx_ds_shift_dxbc, sizeof dx_ds_shift_dxbc, NULL, &ds);
  check(SUCCEEDED(hr) && hs && ds, "CreateHullShader and CreateDomainShader");
  if (hs && ds) {
    /* hs_tri_cb: the factors from its constant buffer (after a matrix); ds_shift: the points moved by its own constant buffer */
    const float hull_constants[24] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 4, 4, 4, 4, 0, 0, 0, 0};
    const float domain_constants[4] = {0.5f, 0, 0, 0};
    ID3D11Buffer *hull_buffer = make_buffer(D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, hull_constants, sizeof hull_constants);
    ID3D11Buffer *domain_buffer = make_buffer(D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, domain_constants, sizeof domain_constants);
    ID3D11Buffer *vertices = triangle(0.5f, 0, 1, 0);
    begin(NULL);
    clear(0, 0, 0);
    bind_floats(vertices);
    ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
    ID3D11DeviceContext_HSSetShader(g_context, hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(g_context, ds, NULL, 0);
    ID3D11DeviceContext_HSSetConstantBuffers(g_context, 0, 1, &hull_buffer);
    ID3D11DeviceContext_DSSetConstantBuffers(g_context, 0, 1, &domain_buffer);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_context, D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ID3D11DeviceContext_Draw(g_context, 3, 0);
    read_target();
    /* (moved right by a quarter of the target: x 0.5 in clip space; the triangle's left foot at 6 of 64 is now at about 22) */
    int lit = 0;
    for (int i = 0; i < SIZE * SIZE; ++i) if (g_pixels[i] & 0xFFFFFF) ++lit;
    check(green(pixel(48, 40)) == 255 && (pixel(10, 56) & 0xFFFFFF) == 0 && lit > 600, "tessellation: hull and domain shaders with constant buffers of their own (the triangle moved right)");
    ID3D11HullShader *bound_hs = NULL;
    ID3D11DomainShader *bound_ds = NULL;
    ID3D11DeviceContext_HSGetShader(g_context, &bound_hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(g_context, &bound_ds, NULL, NULL);
    check(bound_hs == hs && bound_ds == ds, "HSGetShader and DSGetShader");
    if (bound_hs) ID3D11HullShader_Release(bound_hs);
    if (bound_ds) ID3D11DomainShader_Release(bound_ds);
    ID3D11DeviceContext_HSSetShader(g_context, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(g_context, NULL, NULL, 0);
    ID3D11Buffer *none = NULL;
    ID3D11DeviceContext_HSSetConstantBuffers(g_context, 0, 1, &none);
    ID3D11DeviceContext_DSSetConstantBuffers(g_context, 0, 1, &none);
    if (vertices) ID3D11Buffer_Release(vertices);
    if (hull_buffer) ID3D11Buffer_Release(hull_buffer);
    if (domain_buffer) ID3D11Buffer_Release(domain_buffer);
  }
  if (hs) ID3D11HullShader_Release(hs);
  if (ds) ID3D11DomainShader_Release(ds);
  begin(NULL);
}

/* ---- block-compressed textures ----------------------------------------------------------------------------------------------------------------------------------------- */

static void test_compressed(void) {
  /* An 8 x 8 texture of four blocks, BC1 (DXT1) and BC3 (DXT5): blue, blue / blue, white. */
  const DXGI_FORMAT formats[2] = {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC3_UNORM};
  ID3D11SamplerState *sampler = point_sampler();
  for (int i = 0; i < 2; ++i) {
    const UINT size = i == 0 ? 8 : 16;
    BYTE blocks[4 * 16];
    memset(blocks, 0, sizeof blocks);
    static const WORD colours[4] = {0x001F, 0x001F, 0x001F, 0xFFFF};
    for (int b = 0; b < 4; ++b) {
      BYTE *block = blocks + b * size;
      if (i == 1) { block[0] = 255; block[1] = 255; }
      BYTE *colour = block + size - 8;
      colour[0] = (BYTE)colours[b]; colour[1] = (BYTE)(colours[b] >> 8); colour[2] = (BYTE)colours[b]; colour[3] = (BYTE)(colours[b] >> 8);
    }
    D3D11_TEXTURE2D_DESC desc = {8, 8, 1, 1, formats[i], {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA initial = {blocks, 2 * size, 0};
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *view = NULL;
    UINT support = 0;
    ID3D11Device_CheckFormatSupport(g_device, formats[i], &support);
    HRESULT hr = ID3D11Device_CreateTexture2D(g_device, &desc, &initial, &texture);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)texture, NULL, &view);
    char what[160];
    snprintf(what, sizeof what, "a %s texture of 8 x 8 with its blocks, and a view of it", i == 0 ? "BC1" : "BC3");
    check(SUCCEEDED(hr) && view && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D), what);
    if (view) {
      /* ps_tex samples at twice the colour's red and green: (0.1, 0.1) -> texel (1, 1) of block (0, 0), blue; (0.4, 0.4) -> texel (6, 6), white */
      UINT32 got = 0, white = 0;
      ID3D11Buffer *vertices = triangle(0.5f, 0.1f, 0.1f, 1.0f);
      begin(NULL);
      clear(0, 0, 0);
      bind_floats(vertices);
      ID3D11DeviceContext_PSSetShader(g_context, g_ps_tex, NULL, 0);
      ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &view);
      ID3D11DeviceContext_PSSetSamplers(g_context, 0, 1, &sampler);
      ID3D11DeviceContext_Draw(g_context, 3, 0);
      if (read_target()) got = pixel(32, 40);
      if (vertices) ID3D11Buffer_Release(vertices);
      vertices = triangle(0.5f, 0.4f, 0.4f, 1.0f);
      bind_floats(vertices);
      clear(0, 0, 0);
      ID3D11DeviceContext_Draw(g_context, 3, 0);
      if (read_target()) white = pixel(32, 40);
      if (vertices) ID3D11Buffer_Release(vertices);
      snprintf(what, sizeof what, "the %s texture sampled: a blue block and the white one (got %08X and %08X)", i == 0 ? "BC1" : "BC3", (unsigned)got, (unsigned)white);
      check(blue(got) == 255 && red(got) == 0 && green(got) == 0 && blue(white) == 255 && red(white) > 90 && red(white) < 112, what);
      /* UpdateSubresource of the last block: green now */
      BYTE green_block[16];
      memset(green_block, 0, sizeof green_block);
      if (i == 1) { green_block[0] = 255; green_block[1] = 255; }
      green_block[size - 8] = 0xE0; green_block[size - 7] = 0x07; green_block[size - 6] = 0xE0; green_block[size - 5] = 0x07;
      const D3D11_BOX box = {4, 4, 0, 8, 8, 1};
      ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)texture, 0, &box, green_block, size, 0);
      clear(0, 0, 0);
      ID3D11DeviceContext_Draw(g_context, 3, 0);
      const UINT32 updated = read_target() ? pixel(32, 40) : 0;
      check(green(updated) > 90 && red(updated) == 0 && blue(updated) == 0, "UpdateSubresource of one block of it");
      ID3D11ShaderResourceView *none = NULL;
      ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &none);
      ID3D11ShaderResourceView_Release(view);
    }
    if (texture) ID3D11Texture2D_Release(texture);
  }
  D3D11_TEXTURE2D_DESC target_desc = {8, 8, 1, 1, DXGI_FORMAT_BC1_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
  ID3D11Texture2D *target = NULL;
  check(FAILED(ID3D11Device_CreateTexture2D(g_device, &target_desc, NULL, &target)) && !target, "a BC1 render target is refused");
  if (sampler) ID3D11SamplerState_Release(sampler);
  begin(NULL);
}

/* ---- compute shaders, buffer views and unordered access ------------------------------------------------------------------------------------------------- */

/* A buffer's bytes, by way of a staging buffer. */
static int read_buffer(ID3D11Buffer *buffer, void *out, UINT bytes) {
  ID3D11Buffer *staging = make_buffer(0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, NULL, bytes);
  D3D11_MAPPED_SUBRESOURCE mapped;
  int ok = 0;
  if (!staging) return 0;
  D3D11_BOX box = {0, 0, 0, bytes, 1, 1};
  ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)staging, 0, 0, 0, 0, (ID3D11Resource *)buffer, 0, &box);
  if (SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
    memcpy(out, mapped.pData, bytes);
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
    ok = 1;
  }
  ID3D11Buffer_Release(staging);
  return ok;
}
static ID3D11Buffer *structured(UINT bind, UINT stride, UINT count, const void *data, UINT misc) {
  D3D11_BUFFER_DESC desc = {stride * count, D3D11_USAGE_DEFAULT, bind, 0, misc | D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, stride};
  D3D11_SUBRESOURCE_DATA initial = {data, 0, 0};
  ID3D11Buffer *buffer = NULL;
  ID3D11Device_CreateBuffer(g_device, &desc, data ? &initial : NULL, &buffer);
  return buffer;
}
static UINT64 query_result(ID3D11Query *query) {
  UINT64 value = 0;
  while (ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)query, &value, sizeof value, 0) == S_FALSE) {}
  return value;
}

static void test_compute(void) {
  UINT source[32], result[32], sum[4];
  for (UINT i = 0; i < 32; ++i) source[i] = i + 1;
  ID3D11Buffer *src = structured(D3D11_BIND_SHADER_RESOURCE, 4, 32, source, 0);
  ID3D11Buffer *dst = structured(D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 4, 32, NULL, 0);
  ID3D11Buffer *total = structured(D3D11_BIND_UNORDERED_ACCESS, 4, 4, NULL, 0);
  const UINT k[4] = {3, 5, 0, 0};
  ID3D11Buffer *constants = make_buffer(D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, k, sizeof k);
  ID3D11ComputeShader *cs = NULL;
  ID3D11ShaderResourceView *src_view = NULL, *dst_view = NULL;
  ID3D11UnorderedAccessView *uavs[2] = {NULL, NULL};
  HRESULT hr = ID3D11Device_CreateComputeShader(g_device, dx_cs_d3d11_dxbc, sizeof dx_cs_d3d11_dxbc, NULL, &cs);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)src, NULL, &src_view);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)dst, NULL, &dst_view);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateUnorderedAccessView(g_device, (ID3D11Resource *)dst, NULL, &uavs[0]);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateUnorderedAccessView(g_device, (ID3D11Resource *)total, NULL, &uavs[1]);
  check(SUCCEEDED(hr) && src && dst && total && constants && cs && src_view && uavs[0] && uavs[1],
        "CreateComputeShader, structured buffers, their shader resource and unordered access views");
  if (FAILED(hr)) return;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc;
  ID3D11UnorderedAccessView_GetDesc(uavs[0], &uav_desc);
  check(uav_desc.ViewDimension == D3D11_UAV_DIMENSION_BUFFER && uav_desc.Format == DXGI_FORMAT_UNKNOWN && uav_desc.Buffer.NumElements == 32, "the view's description");
  ID3D11ShaderResourceView *refused = NULL;
  ID3D11Buffer *plain = make_buffer(D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, NULL, 64);
  check(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)plain, NULL, &refused) == E_INVALIDARG && !refused,
        "a view of a buffer that is not structured needs a description");
  if (plain) ID3D11Buffer_Release(plain);

  const UINT zero[4] = {0, 0, 0, 0};
  ID3D11DeviceContext_CSSetShader(g_context, cs, NULL, 0);
  ID3D11DeviceContext_CSSetConstantBuffers(g_context, 0, 1, &constants);
  ID3D11DeviceContext_CSSetShaderResources(g_context, 0, 1, &src_view);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(g_context, 0, 2, uavs, NULL);
  ID3D11DeviceContext_ClearUnorderedAccessViewUint(g_context, uavs[1], zero);
  ID3D11Query *stats = NULL;
  const D3D11_QUERY_DESC stats_desc = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
  ID3D11Device_CreateQuery(g_device, &stats_desc, &stats);
  if (stats) ID3D11DeviceContext_Begin(g_context, (ID3D11Asynchronous *)stats);
  ID3D11DeviceContext_Dispatch(g_context, 2, 1, 1);
  if (stats) ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)stats);
  int good = read_buffer(dst, result, sizeof result);
  for (UINT i = 0; good && i < 32; ++i) good = result[i] == source[i] * 3 + 5;
  if (!good) printf("d3d11test: (dispatched: %u %u %u ... %u)\n", result[0], result[1], result[2], result[31]);
  check(good, "Dispatch: a compute shader reads a structured buffer and its constants, and writes another");
  check(read_buffer(total, sum, sizeof sum) && sum[0] == 528, "... and adds with atomics into a buffer cleared with ClearUnorderedAccessViewUint");
  D3D11_QUERY_DATA_PIPELINE_STATISTICS counted;
  memset(&counted, 0xFF, sizeof counted);
  check(stats && ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)stats, &counted, sizeof counted, 0) == S_OK && counted.CSInvocations == 32 && counted.IAVertices == 0,
        "a pipeline statistics query counts the compute shader's invocations");

  ID3D11ComputeShader *bound = NULL;
  ID3D11UnorderedAccessView *bound_uav = NULL;
  ID3D11DeviceContext_CSGetShader(g_context, &bound, NULL, NULL);
  ID3D11DeviceContext_CSGetUnorderedAccessViews(g_context, 1, 1, &bound_uav);
  check(bound == cs && bound_uav == uavs[1], "CSGetShader and CSGetUnorderedAccessViews give back what is bound");
  if (bound) ID3D11ComputeShader_Release(bound);
  if (bound_uav) ID3D11UnorderedAccessView_Release(bound_uav);
  ID3D11ShaderResourceView *read_back = (ID3D11ShaderResourceView *)1;
  ID3D11DeviceContext_CSSetShaderResources(g_context, 1, 1, &dst_view);
  ID3D11DeviceContext_CSGetShaderResources(g_context, 1, 1, &read_back);
  check(read_back == NULL, "a buffer bound for unordered access is not bound as a shader resource too");

  /* a clear with a value, and a dispatch whose group count is in a buffer */
  const UINT sevens[4] = {7, 0, 0, 0};
  ID3D11DeviceContext_ClearUnorderedAccessViewUint(g_context, uavs[0], sevens);
  good = read_buffer(dst, result, sizeof result);
  for (UINT i = 0; good && i < 32; ++i) good = result[i] == 7;
  check(good, "ClearUnorderedAccessViewUint fills a structured buffer with the first value");
  const UINT groups[3] = {1, 1, 1};
  D3D11_BUFFER_DESC args_desc = {12, D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0};
  D3D11_SUBRESOURCE_DATA args_data = {groups, 0, 0};
  ID3D11Buffer *args = NULL;
  ID3D11Device_CreateBuffer(g_device, &args_desc, &args_data, &args);
  ID3D11DeviceContext_ClearUnorderedAccessViewUint(g_context, uavs[1], zero);
  if (args) ID3D11DeviceContext_DispatchIndirect(g_context, args, 0);
  good = read_buffer(dst, result, sizeof result);
  for (UINT i = 0; good && i < 32; ++i) good = result[i] == (i < 16 ? source[i] * 3 + 5 : 7);
  check(args && good && read_buffer(total, sum, sizeof sum) && sum[0] == 136, "DispatchIndirect: one group, from the arguments in a buffer");
  if (args) ID3D11Buffer_Release(args);

  /* the hidden count of an append view: set when it is bound, copied out with CopyStructureCount */
  ID3D11Buffer *appended = structured(D3D11_BIND_UNORDERED_ACCESS, 8, 16, NULL, 0);
  ID3D11Buffer *count = make_buffer(D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, NULL, 16);
  ID3D11UnorderedAccessView *append_view = NULL;
  D3D11_UNORDERED_ACCESS_VIEW_DESC append_desc;
  memset(&append_desc, 0, sizeof append_desc);
  append_desc.Format = DXGI_FORMAT_UNKNOWN;
  append_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  append_desc.Buffer.NumElements = 16;
  append_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
  if (appended) ID3D11Device_CreateUnorderedAccessView(g_device, (ID3D11Resource *)appended, &append_desc, &append_view);
  const UINT initial_count = 7;
  UINT counted_words[4] = {0, 0, 0, 0};
  if (append_view && count) {
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g_context, 2, 1, &append_view, &initial_count);
    ID3D11DeviceContext_CopyStructureCount(g_context, count, 4, append_view);
  }
  check(append_view && count && read_buffer(count, counted_words, sizeof counted_words) && counted_words[1] == 7 && counted_words[0] == 0,
        "an append view's count, set by CSSetUnorderedAccessViews, copied with CopyStructureCount");
  if (append_view) ID3D11UnorderedAccessView_Release(append_view);
  if (appended) ID3D11Buffer_Release(appended);
  if (count) ID3D11Buffer_Release(count);

  ID3D11DeviceContext_CSSetShader(g_context, NULL, NULL, 0);
  ID3D11UnorderedAccessView *none[2] = {NULL, NULL};
  ID3D11DeviceContext_CSSetUnorderedAccessViews(g_context, 0, 2, none, NULL);
  if (stats) ID3D11Query_Release(stats);
  ID3D11ComputeShader_Release(cs);
  ID3D11ShaderResourceView_Release(src_view);
  if (dst_view) ID3D11ShaderResourceView_Release(dst_view);
  ID3D11UnorderedAccessView_Release(uavs[0]);
  ID3D11UnorderedAccessView_Release(uavs[1]);
  ID3D11Buffer_Release(src);
  ID3D11Buffer_Release(dst);
  ID3D11Buffer_Release(total);
  ID3D11Buffer_Release(constants);
}

/* ---- the pixel shader's unordered access views, queries, predication ----------------------------------------------------------------------------------- */

static void test_pixel_uav_and_queries(void) {
  const float colours[8] = {1, 0, 0, 1, 0, 1, 0, 1};
  ID3D11Buffer *palette = structured(D3D11_BIND_SHADER_RESOURCE, 16, 2, colours, 0);
  ID3D11Buffer *hits = structured(D3D11_BIND_UNORDERED_ACCESS, 4, 4, NULL, 0);
  ID3D11PixelShader *ps = NULL;
  ID3D11ShaderResourceView *palette_view = NULL;
  ID3D11UnorderedAccessView *hits_view = NULL;
  HRESULT hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_sbuf_dxbc, sizeof dx_ps_sbuf_dxbc, NULL, &ps);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)palette, NULL, &palette_view);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateUnorderedAccessView(g_device, (ID3D11Resource *)hits, NULL, &hits_view);
  ID3D11Query *occlusion = NULL, *stats = NULL;
  ID3D11Predicate *predicate = NULL;
  const D3D11_QUERY_DESC occlusion_desc = {D3D11_QUERY_OCCLUSION, 0}, stats_desc = {D3D11_QUERY_PIPELINE_STATISTICS, 0}, predicate_desc = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateQuery(g_device, &occlusion_desc, &occlusion);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateQuery(g_device, &stats_desc, &stats);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePredicate(g_device, &predicate_desc, &predicate);
  ID3D11Predicate *wrong = NULL;
  check(SUCCEEDED(hr) && ID3D11Device_CreatePredicate(g_device, &occlusion_desc, &wrong) == E_INVALIDARG && !wrong,
        "a pixel shader with a buffer view and an unordered access view; occlusion and statistics queries; a predicate (and none of a query that is not one)");
  if (FAILED(hr)) return;
  ID3D11Buffer *vertices = triangle(0.5f, 1, 1, 1);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  const UINT zero[4] = {0, 0, 0, 0};
  ID3D11DeviceContext_ClearUnorderedAccessViewUint(g_context, hits_view, zero);
  ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g_context, 1, &g_target_view, NULL, 1, 1, &hits_view, NULL);
  ID3D11DeviceContext_PSSetShader(g_context, ps, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &palette_view);
  ID3D11DeviceContext_Begin(g_context, (ID3D11Asynchronous *)occlusion);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)occlusion);
  read_target();
  int greens = 0;
  for (int i = 0; i < SIZE * SIZE; ++i) greens += g_pixels[i] == 0xFF00FF00u;
  UINT counted[4] = {0};
  const UINT64 samples = query_result(occlusion);
  read_buffer(hits, counted, sizeof counted);
  if (!(greens > 600 && (int)counted[0] == greens && samples == (UINT64)greens))
    printf("d3d11test: (%d green pixels, the shader counted %u, the occlusion query %u)\n", greens, counted[0], (unsigned)samples);
  check(greens > 600 && pixel(32, 32) == 0xFF00FF00u, "the pixel shader reads its colour out of a structured buffer");
  check((int)counted[0] == greens, "... and counts its pixels with an atomic in an unordered access view");
  check(samples == (UINT64)greens, "an occlusion query counts the samples drawn");
  ID3D11UnorderedAccessView *got = NULL;
  ID3D11RenderTargetView *got_target = NULL;
  ID3D11DeviceContext_OMGetRenderTargetsAndUnorderedAccessViews(g_context, 1, &got_target, NULL, 1, 1, &got);
  check(got == hits_view && got_target == g_target_view, "OMGetRenderTargetsAndUnorderedAccessViews");
  if (got) ID3D11UnorderedAccessView_Release(got);
  if (got_target) ID3D11RenderTargetView_Release(got_target);

  /* the statistics of a draw */
  ID3D11DeviceContext_Begin(g_context, (ID3D11Asynchronous *)stats);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)stats);
  D3D11_QUERY_DATA_PIPELINE_STATISTICS s;
  memset(&s, 0, sizeof s);
  while (ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)stats, &s, sizeof s, 0) == S_FALSE) {}
  if (!(s.IAVertices == 3 && s.IAPrimitives == 1 && s.VSInvocations == 3 && s.CInvocations == 1 && s.PSInvocations == (UINT64)greens))
    printf("d3d11test: (statistics: %u vertices, %u primitives, %u vs, %u c, %u ps)\n", (unsigned)s.IAVertices, (unsigned)s.IAPrimitives, (unsigned)s.VSInvocations,
           (unsigned)s.CInvocations, (unsigned)s.PSInvocations);
  check(s.IAVertices == 3 && s.IAPrimitives == 1 && s.VSInvocations == 3 && s.CInvocations == 1 && s.PSInvocations == (UINT64)greens && s.CSInvocations == 0,
        "a pipeline statistics query: the vertices, the primitive, the vertex and pixel shaders' invocations");

  /* an occlusion predicate of a draw outside the target, and the commands it holds back */
  set_matrix(0.2f, 0.2f, 5.0f, 0);
  ID3D11DeviceContext_Begin(g_context, (ID3D11Asynchronous *)predicate);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)predicate);
  BOOL visible = TRUE;
  while (ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)predicate, &visible, sizeof visible, 0) == S_FALSE) {}
  check(visible == FALSE, "an occlusion predicate of a draw that covers no pixel is false");
  ID3D11DeviceContext_SetPredication(g_context, predicate, FALSE);
  clear(1, 0, 0);
  set_matrix(1, 1, 0, 0);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11Predicate *got_predicate = NULL;
  BOOL got_value = TRUE;
  ID3D11DeviceContext_GetPredication(g_context, &got_predicate, &got_value);
  check(got_predicate == predicate && got_value == FALSE, "GetPredication");
  if (got_predicate) ID3D11Predicate_Release(got_predicate);
  ID3D11DeviceContext_SetPredication(g_context, NULL, FALSE);
  read_target();
  check(pixel(32, 32) == 0xFF00FF00u && pixel(1, 1) == 0xFF000000u, "SetPredication: a clear and a draw are left out while the predicate is false");
  ID3D11DeviceContext_SetPredication(g_context, predicate, TRUE);
  clear(0, 0, 1);
  ID3D11DeviceContext_SetPredication(g_context, NULL, FALSE);
  read_target();
  check(pixel(1, 1) == 0xFF0000FFu, "... and done while it is not the value given");

  ID3D11UnorderedAccessView *no_uav = NULL;
  ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(g_context, 1, &g_target_view, NULL, 1, 1, &no_uav, NULL);
  ID3D11ShaderResourceView *no_view = NULL;
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &no_view);
  ID3D11Predicate_Release(predicate);
  ID3D11Query_Release(stats);
  ID3D11Query_Release(occlusion);
  ID3D11Buffer_Release(vertices);
  ID3D11UnorderedAccessView_Release(hits_view);
  ID3D11ShaderResourceView_Release(palette_view);
  ID3D11PixelShader_Release(ps);
  ID3D11Buffer_Release(hits);
  ID3D11Buffer_Release(palette);
}

/* ---- stream output and the draws whose counts are in buffers ---------------------------------------------------------------------------------------------- */

static void test_stream_output(void) {
  /* the vertex shader's position and colour captured (a "geometry shader" made of a vertex shader's code: there is no geometry stage), 32 bytes a vertex */
  const D3D11_SO_DECLARATION_ENTRY entries[2] = {{0, "SV_POSITION", 0, 0, 4, 0}, {0, "COLOR", 0, 0, 4, 0}};
  const UINT stride = 32;
  ID3D11GeometryShader *so = NULL;
  HRESULT hr = ID3D11Device_CreateGeometryShaderWithStreamOutput(g_device, dx_vs_dxbc, sizeof dx_vs_dxbc, entries, 2, &stride, 1, 0, NULL, &so);
  ID3D11GeometryShader *refused = NULL;
  const D3D11_SO_DECLARATION_ENTRY bad = {0, "COLOR", 0, 0, 5, 0};
  check(SUCCEEDED(hr) && so && ID3D11Device_CreateGeometryShaderWithStreamOutput(g_device, dx_vs_dxbc, sizeof dx_vs_dxbc, &bad, 1, &stride, 1, 0, NULL, &refused) == E_INVALIDARG,
        "CreateGeometryShaderWithStreamOutput of a vertex shader's code (and a declaration of five components is refused)");
  if (!so) return;
  D3D11_BUFFER_DESC desc = {6 * 32, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  ID3D11Buffer *captured = NULL, *small = NULL;
  ID3D11Device_CreateBuffer(g_device, &desc, NULL, &captured);
  desc.ByteWidth = 32;
  ID3D11Device_CreateBuffer(g_device, &desc, NULL, &small);
  ID3D11Query *so_stats = NULL;
  ID3D11Predicate *overflow = NULL;
  const D3D11_QUERY_DESC stats_desc = {D3D11_QUERY_SO_STATISTICS_STREAM0, 0}, overflow_desc = {D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0, 0};
  ID3D11Device_CreateQuery(g_device, &stats_desc, &so_stats);
  ID3D11Device_CreatePredicate(g_device, &overflow_desc, &overflow);
  check(captured && small && so_stats && overflow, "stream output buffers, a statistics query of stream 0 and its overflow predicate");
  if (!captured || !small || !so_stats || !overflow) return;

  const VertexF v[3] = {{-0.8f, -0.8f, 0.5f, 1, 0, 0, 1}, {0.8f, -0.8f, 0.5f, 0, 1, 0, 1}, {0.0f, 0.8f, 0.5f, 0, 0, 1, 1}};
  ID3D11Buffer *vertices = make_buffer(D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DEFAULT, 0, v, sizeof v);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  ID3D11DeviceContext_GSSetShader(g_context, so, NULL, 0);
  UINT offset = 0;
  ID3D11DeviceContext_SOSetTargets(g_context, 1, &captured, &offset);
  ID3D11DeviceContext_Begin(g_context, (ID3D11Asynchronous *)so_stats);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)so_stats);
  D3D11_QUERY_DATA_SO_STATISTICS st = {99, 99};
  while (ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)so_stats, &st, sizeof st, 0) == S_FALSE) {}
  if (st.NumPrimitivesWritten != 1 || st.PrimitivesStorageNeeded != 1) printf("d3d11test: (stream output statistics: %u written, %u needed)\n", (unsigned)st.NumPrimitivesWritten, (unsigned)st.PrimitivesStorageNeeded);
  check(st.NumPrimitivesWritten == 1 && st.PrimitivesStorageNeeded == 1, "a stream output statistics query: one triangle written");
  read_target();
  check((pixel(32, 32) & 0xFFFFFF) != 0, "... and drawn (stream 0 is rasterized)");
  float records[6 * 8];
  memset(records, 0, sizeof records);
  check(read_buffer(captured, records, sizeof records) && records[0] == -0.8f && records[1] == -0.8f && records[2] == 0.5f && records[3] == 1.0f && records[4] == 1.0f &&
        records[8 + 5] == 1.0f && records[16 + 6] == 1.0f, "the captured records: each vertex's position and colour");
  ID3D11Buffer *got[2] = {NULL, NULL};
  ID3D11DeviceContext_SOGetTargets(g_context, 2, got);
  check(got[0] == captured && got[1] == NULL, "SOGetTargets");
  for (int i = 0; i < 2; ++i) if (got[i]) ID3D11Buffer_Release(got[i]);
  /* appended: a second draw with the offset -1 goes on after the first triangle */
  offset = (UINT)-1;
  set_matrix(1, 1, 0.1f, 0);
  ID3D11DeviceContext_SOSetTargets(g_context, 1, &captured, &offset);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_buffer(captured, records, sizeof records);
  check(records[24] > -0.75f && records[24] < -0.65f && records[0] == -0.8f, "SOSetTargets with the offset -1 appends to what the buffer holds");

  /* DrawAuto: what was captured, drawn as vertices (the position's x, y and z, and the colour) */
  const D3D11_INPUT_ELEMENT_DESC elements[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ID3D11InputLayout *layout = NULL;
  ID3D11Device_CreateInputLayout(g_device, elements, 2, dx_vs_dxbc, sizeof dx_vs_dxbc, &layout);
  ID3D11DeviceContext_SOSetTargets(g_context, 0, NULL, NULL);
  ID3D11DeviceContext_GSSetShader(g_context, NULL, NULL, 0);
  set_matrix(0.5f, 0.5f, 0, 0);
  clear(0, 0, 0);
  const UINT so_stride = 32, so_offset = 0;
  ID3D11DeviceContext_IASetInputLayout(g_context, layout);
  ID3D11DeviceContext_IASetVertexBuffers(g_context, 0, 1, &captured, &so_stride, &so_offset);
  ID3D11Query *stats = NULL;
  const D3D11_QUERY_DESC pipeline_desc = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
  ID3D11Device_CreateQuery(g_device, &pipeline_desc, &stats);
  if (stats) ID3D11DeviceContext_Begin(g_context, (ID3D11Asynchronous *)stats);
  ID3D11DeviceContext_DrawAuto(g_context);
  if (stats) ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)stats);
  D3D11_QUERY_DATA_PIPELINE_STATISTICS ps;
  memset(&ps, 0, sizeof ps);
  if (stats) while (ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)stats, &ps, sizeof ps, 0) == S_FALSE) {}
  read_target();
  check(layout && (pixel(32, 32) & 0xFFFFFF) != 0 && pixel(32, 8) == 0xFF000000u && ps.IAVertices == 6, "DrawAuto draws the six vertices captured, at half the size");
  if (stats) ID3D11Query_Release(stats);

  /* a buffer too small for the triangle: the overflow predicate is true, nothing is written */
  set_matrix(1, 1, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_GSSetShader(g_context, so, NULL, 0);
  offset = 0;
  ID3D11DeviceContext_SOSetTargets(g_context, 1, &small, &offset);
  ID3D11DeviceContext_Begin(g_context, (ID3D11Asynchronous *)overflow);
  ID3D11DeviceContext_Begin(g_context, (ID3D11Asynchronous *)so_stats);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)so_stats);
  ID3D11DeviceContext_End(g_context, (ID3D11Asynchronous *)overflow);
  BOOL overflowed = FALSE;
  while (ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)overflow, &overflowed, sizeof overflowed, 0) == S_FALSE) {}
  while (ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)so_stats, &st, sizeof st, 0) == S_FALSE) {}
  if (!(overflowed == TRUE && st.NumPrimitivesWritten == 0 && st.PrimitivesStorageNeeded == 1))
    printf("d3d11test: (overflow %d, %u written, %u needed)\n", overflowed, (unsigned)st.NumPrimitivesWritten, (unsigned)st.PrimitivesStorageNeeded);
  check(overflowed == TRUE && st.NumPrimitivesWritten == 0 && st.PrimitivesStorageNeeded == 1, "a stream output buffer too small: the overflow predicate, written 0 of 1");

  ID3D11DeviceContext_SOSetTargets(g_context, 0, NULL, NULL);
  ID3D11DeviceContext_GSSetShader(g_context, NULL, NULL, 0);
  if (layout) ID3D11InputLayout_Release(layout);
  ID3D11Buffer_Release(vertices);
  ID3D11Predicate_Release(overflow);
  ID3D11Query_Release(so_stats);
  ID3D11Buffer_Release(small);
  ID3D11Buffer_Release(captured);
  ID3D11GeometryShader_Release(so);
}

static void test_indirect_draws(void) {
  const UINT draw[4] = {3, 1, 0, 0}, indexed[5] = {3, 1, 0, 0, 0};
  const UINT16 indices[3] = {0, 1, 2};
  D3D11_BUFFER_DESC desc = {20, D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0};
  D3D11_SUBRESOURCE_DATA d1 = {draw, 0, 0}, d2 = {indexed, 0, 0};
  ID3D11Buffer *args = NULL, *args_indexed = NULL;
  desc.ByteWidth = 16;
  ID3D11Device_CreateBuffer(g_device, &desc, &d1, &args);
  desc.ByteWidth = 20;
  ID3D11Device_CreateBuffer(g_device, &desc, &d2, &args_indexed);
  ID3D11Buffer *index = make_buffer(D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_DEFAULT, 0, indices, sizeof indices);
  ID3D11Buffer *vertices = triangle(0.5f, 0, 1, 0);
  check(args && args_indexed && index && vertices, "buffers of draw arguments (D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS)");
  if (!args || !args_indexed || !index || !vertices) return;
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  ID3D11DeviceContext_DrawInstancedIndirect(g_context, args, 0);
  read_target();
  check(pixel(32, 32) == 0xFF00FF00u && pixel(1, 1) == 0xFF000000u, "DrawInstancedIndirect");
  clear(0, 0, 0);
  ID3D11DeviceContext_IASetIndexBuffer(g_context, index, DXGI_FORMAT_R16_UINT, 0);
  ID3D11DeviceContext_DrawIndexedInstancedIndirect(g_context, args_indexed, 0);
  read_target();
  check(pixel(32, 32) == 0xFF00FF00u && pixel(1, 1) == 0xFF000000u, "DrawIndexedInstancedIndirect");
  ID3D11Buffer_Release(vertices);
  ID3D11Buffer_Release(index);
  ID3D11Buffer_Release(args);
  ID3D11Buffer_Release(args_indexed);
}

/* SetResourceMinLOD: a texture of three levels (red, green, blue) sampled at its most detailed level, then no more detailed than level 1. */
static void test_min_lod(void) {
  static UINT32 level0[16], level1[4], level2[1];
  for (int i = 0; i < 16; ++i) level0[i] = 0xFF0000FFu;
  for (int i = 0; i < 4; ++i) level1[i] = 0xFF00FF00u;
  level2[0] = 0xFFFF0000u;
  D3D11_TEXTURE2D_DESC desc = {4, 4, 3, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA initial[3] = {{level0, 16, 0}, {level1, 8, 0}, {level2, 4, 0}};
  ID3D11Texture2D *texture = NULL;
  ID3D11ShaderResourceView *view = NULL;
  ID3D11SamplerState *sampler = NULL;
  D3D11_SAMPLER_DESC sd;
  memset(&sd, 0, sizeof sd);
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  HRESULT hr = ID3D11Device_CreateTexture2D(g_device, &desc, initial, &texture);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)texture, NULL, &view);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateSamplerState(g_device, &sd, &sampler);
  check(SUCCEEDED(hr), "a texture of three levels");
  if (FAILED(hr)) return;
  /* (ps_tex samples at twice the colour's red and green, times the colour: (0.25, 0.25, 1) is the middle, at the same place for every pixel) */
  ID3D11Buffer *vertices = triangle(0.5f, 0.25f, 0.25f, 1.0f);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_tex, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &view);
  ID3D11DeviceContext_PSSetSamplers(g_context, 0, 1, &sampler);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  const UINT32 first = pixel(32, 32);
  ID3D11DeviceContext_SetResourceMinLOD(g_context, (ID3D11Resource *)texture, 1.0f);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  const UINT32 clamped = pixel(32, 32);
  ID3D11DeviceContext_SetResourceMinLOD(g_context, (ID3D11Resource *)texture, 0.0f);
  if (!(red(first) > 40 && green(first) == 0 && green(clamped) > 40 && red(clamped) == 0)) printf("d3d11test: (sampled %08x, with the clamp %08x)\n", (unsigned)first, (unsigned)clamped);
  check(red(first) > 40 && green(first) == 0, "the texture sampled at level 0");
  check(green(clamped) > 40 && red(clamped) == 0 && ID3D11DeviceContext_GetResourceMinLOD(g_context, (ID3D11Resource *)texture) == 0.0f,
        "SetResourceMinLOD 1: sampled no more detailed than level 1 (and GetResourceMinLOD)");
  ID3D11ShaderResourceView *no_view = NULL;
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &no_view);
  ID3D11Buffer_Release(vertices);
  ID3D11SamplerState_Release(sampler);
  ID3D11ShaderResourceView_Release(view);
  ID3D11Texture2D_Release(texture);
}

/* ---- deferred contexts and command lists ----------------------------------------------------------------------------------------------------------------- */

static void test_deferred(void) {
  ID3D11DeviceContext *deferred = NULL;
  HRESULT hr = ID3D11Device_CreateDeferredContext(g_device, 0, &deferred);
  check(SUCCEEDED(hr) && deferred && ID3D11DeviceContext_GetType(deferred) == D3D11_DEVICE_CONTEXT_DEFERRED && ID3D11DeviceContext_GetType(g_context) == D3D11_DEVICE_CONTEXT_IMMEDIATE,
        "CreateDeferredContext: a deferred context");
  if (!deferred) return;
  ID3D11CommandList *list = NULL;
  check(ID3D11DeviceContext_FinishCommandList(g_context, FALSE, &list) == DXGI_ERROR_INVALID_CALL && !list, "the immediate context has no command list to finish");
  ID3D11Buffer *matrix = make_buffer(D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, NULL, 64);
  ID3D11Buffer *vertices = triangle(0.5f, 0, 0, 1);
  ID3D11Query *occlusion = NULL;
  const D3D11_QUERY_DESC occlusion_desc = {D3D11_QUERY_OCCLUSION, 0};
  ID3D11Device_CreateQuery(g_device, &occlusion_desc, &occlusion);
  /* recorded: the bindings, a Map of the matrix (half the size), a clear and a draw counted by a query */
  D3D11_VIEWPORT viewport = {0, 0, SIZE, SIZE, 0, 1};
  const UINT stride = sizeof(VertexF), offset = 0;
  ID3D11DeviceContext_OMSetRenderTargets(deferred, 1, &g_target_view, NULL);
  ID3D11DeviceContext_RSSetViewports(deferred, 1, &viewport);
  ID3D11DeviceContext_RSSetState(deferred, g_no_cull);
  ID3D11DeviceContext_VSSetShader(deferred, g_vs, NULL, 0);
  ID3D11DeviceContext_VSSetConstantBuffers(deferred, 0, 1, &matrix);
  ID3D11DeviceContext_PSSetShader(deferred, g_ps_color, NULL, 0);
  ID3D11DeviceContext_IASetPrimitiveTopology(deferred, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11DeviceContext_IASetInputLayout(deferred, g_layout_floats);
  ID3D11DeviceContext_IASetVertexBuffers(deferred, 0, 1, &vertices, &stride, &offset);
  D3D11_MAPPED_SUBRESOURCE mapped;
  const float half[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  check(ID3D11DeviceContext_Map(deferred, (ID3D11Resource *)matrix, 0, D3D11_MAP_WRITE, 0, &mapped) == E_INVALIDARG, "a deferred context maps for WRITE_DISCARD and WRITE_NO_OVERWRITE only");
  hr = ID3D11DeviceContext_Map(deferred, (ID3D11Resource *)matrix, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (SUCCEEDED(hr)) { memcpy(mapped.pData, half, sizeof half); ID3D11DeviceContext_Unmap(deferred, (ID3D11Resource *)matrix, 0); }
  const float red[4] = {1, 0, 0, 1};
  ID3D11DeviceContext_ClearRenderTargetView(deferred, g_target_view, red);
  if (occlusion) ID3D11DeviceContext_Begin(deferred, (ID3D11Asynchronous *)occlusion);
  ID3D11DeviceContext_Draw(deferred, 3, 0);
  if (occlusion) ID3D11DeviceContext_End(deferred, (ID3D11Asynchronous *)occlusion);
  UINT64 samples = 0;
  check(SUCCEEDED(hr) && ID3D11DeviceContext_GetData(deferred, (ID3D11Asynchronous *)occlusion, &samples, sizeof samples, 0) == DXGI_ERROR_INVALID_CALL,
        "Map with WRITE_DISCARD on the deferred context; it answers no queries");
  hr = ID3D11DeviceContext_FinishCommandList(deferred, FALSE, &list);
  check(SUCCEEDED(hr) && list && ID3D11CommandList_GetContextFlags(list) == 0, "FinishCommandList");
  ID3D11RenderTargetView *bound = (ID3D11RenderTargetView *)1;
  ID3D11DeviceContext_OMGetRenderTargets(deferred, 1, &bound, NULL);
  check(bound == NULL, "... after which the deferred context's bindings are the default ones (RestoreDeferredContextState FALSE)");
  if (!list) { ID3D11DeviceContext_Release(deferred); return; }

  /* nothing happened yet: the immediate context runs the list */
  begin(NULL);
  clear(0, 0, 0);
  read_target();
  check(pixel(1, 1) == 0xFF000000u, "nothing recorded has run before ExecuteCommandList");
  ID3D11DeviceContext_ExecuteCommandList(g_context, list, FALSE);
  read_target();
  if (!(pixel(1, 1) == 0xFFFF0000u && pixel(32, 32) == 0xFF0000FFu && pixel(32, 12) == 0xFFFF0000u))
    printf("d3d11test: (after the list: corner %08x, middle %08x, above %08x)\n", (unsigned)pixel(1, 1), (unsigned)pixel(32, 32), (unsigned)pixel(32, 12));
  check(pixel(1, 1) == 0xFFFF0000u && pixel(32, 32) == 0xFF0000FFu && pixel(32, 12) == 0xFFFF0000u, "ExecuteCommandList: the clear, and the draw with the matrix the deferred Map wrote");
  samples = 0;
  check(occlusion && ID3D11DeviceContext_GetData(g_context, (ID3D11Asynchronous *)occlusion, &samples, sizeof samples, 0) == S_OK && samples > 100 && samples < 1000,
        "a query begun and ended in the list is answered on the immediate context");
  bound = (ID3D11RenderTargetView *)1;
  ID3D11DeviceContext_OMGetRenderTargets(g_context, 1, &bound, NULL);
  check(bound == NULL, "after ExecuteCommandList(FALSE) the immediate context's bindings are cleared");

  /* again, keeping the immediate context's bindings */
  begin(NULL);
  clear(0, 0, 0);
  ID3D11DeviceContext_ExecuteCommandList(g_context, list, TRUE);
  read_target();
  bound = NULL;
  ID3D11DeviceContext_OMGetRenderTargets(g_context, 1, &bound, NULL);
  check(pixel(32, 32) == 0xFF0000FFu && bound == g_target_view, "a list runs again; ExecuteCommandList(TRUE) keeps the immediate context's bindings");
  if (bound) ID3D11RenderTargetView_Release(bound);

  /* a list recorded into another deferred context's list */
  ID3D11DeviceContext_ExecuteCommandList(deferred, list, FALSE);
  ID3D11CommandList *outer = NULL;
  ID3D11DeviceContext_FinishCommandList(deferred, TRUE, &outer);
  clear(0, 1, 0);
  if (outer) ID3D11DeviceContext_ExecuteCommandList(g_context, outer, FALSE);
  read_target();
  check(outer && pixel(1, 1) == 0xFFFF0000u && pixel(32, 32) == 0xFF0000FFu, "ExecuteCommandList on a deferred context: a list inside a list");
  if (outer) ID3D11CommandList_Release(outer);

  ID3D11CommandList_Release(list);
  if (occlusion) ID3D11Query_Release(occlusion);
  ID3D11Buffer_Release(vertices);
  ID3D11Buffer_Release(matrix);
  ULONG left = ID3D11DeviceContext_Release(deferred);
  check(left == 0, "the deferred context goes with its last reference");
}

/* ---- Direct3D 11.1 ------------------------------------------------------------------------------------------------------------------------------------------- */

static void test_d3d11_1(void) {
  ID3D11Device1 *device1 = NULL;
  ID3D11DeviceContext1 *context1 = NULL;
  ID3D11Device_QueryInterface(g_device, &IID_ID3D11Device1, (void **)&device1);
  ID3D11DeviceContext_QueryInterface(g_context, &IID_ID3D11DeviceContext1, (void **)&context1);
  check(device1 && context1, "the device is an ID3D11Device1, its context an ID3D11DeviceContext1");
  if (!device1 || !context1) return;
  /* a constant buffer of two matrices, the second 256 bytes in: VSSetConstantBuffers1 binds the window of the second */
  float two[128];
  memset(two, 0, sizeof two);
  const float full[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, small[16] = {0.25f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  memcpy(two, full, sizeof full);
  memcpy(two + 64, small, sizeof small);
  ID3D11Buffer *matrices = make_buffer(D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, two, sizeof two);
  ID3D11Buffer *vertices = triangle(0.5f, 0, 1, 0);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_color, NULL, 0);
  const UINT first = 16, number = 16;
  ID3D11DeviceContext1_VSSetConstantBuffers1(context1, 0, 1, &matrices, &first, &number);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  int greens = 0;
  for (int i = 0; i < SIZE * SIZE; ++i) greens += g_pixels[i] == 0xFF00FF00u;
  UINT got_first = 0, got_number = 0;
  ID3D11Buffer *got = NULL;
  ID3D11DeviceContext1_VSGetConstantBuffers1(context1, 0, 1, &got, &got_first, &got_number);
  check(matrices && greens > 20 && greens < 200 && got == matrices && got_first == 16 && got_number == 16, "VSSetConstantBuffers1: the window of the buffer the shader reads (and the getter)");
  if (got) ID3D11Buffer_Release(got);

  /* ClearView of a rectangle */
  const float blue[4] = {0, 0, 1, 1};
  const D3D11_RECT rect = {4, 4, 12, 12};
  ID3D11DeviceContext1_ClearView(context1, (ID3D11View *)g_target_view, blue, &rect, 1);
  read_target();
  check(pixel(6, 6) == 0xFF0000FFu && pixel(13, 13) == 0xFF000000u && pixel(3, 3) == 0xFF000000u, "ClearView of a rectangle of the target");

  /* the same description, the same object */
  D3D11_BLEND_DESC bd;
  memset(&bd, 0, sizeof bd);
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState *b1 = NULL, *b2 = NULL;
  ID3D11Device_CreateBlendState(g_device, &bd, &b1);
  ID3D11Device_CreateBlendState(g_device, &bd, &b2);
  D3D11_BLEND_DESC back;
  if (b1) ID3D11BlendState_GetDesc(b1, &back);
  check(b1 && b1 == b2 && back.RenderTarget[3].BlendEnable == TRUE && back.RenderTarget[3].SrcBlend == D3D11_BLEND_SRC_ALPHA,
        "CreateBlendState: the same object for the same description (each target as the first without IndependentBlendEnable)");
  if (b1) ID3D11BlendState_Release(b1);
  if (b2) ID3D11BlendState_Release(b2);

  /* a context state object: swapped in, the bindings are the default ones; swapped back, they return */
  ID3DDeviceContextState *state = NULL, *previous = NULL;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  D3D_FEATURE_LEVEL chosen = 0;
  HRESULT hr = ID3D11Device1_CreateDeviceContextState(device1, 0, &level, 1, D3D11_SDK_VERSION, &IID_ID3D11Device1, &chosen, &state);
  check(SUCCEEDED(hr) && state && chosen == D3D_FEATURE_LEVEL_11_0, "CreateDeviceContextState");
  if (state) {
    ID3D11DeviceContext1_SwapDeviceContextState(context1, state, &previous);
    ID3D11RenderTargetView *bound = (ID3D11RenderTargetView *)1;
    ID3D11DeviceContext_OMGetRenderTargets(g_context, 1, &bound, NULL);
    check(previous && bound == NULL, "SwapDeviceContextState: the new state's bindings (none)");
    ID3D11DeviceContext1_SwapDeviceContextState(context1, previous, NULL);
    bound = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(g_context, 1, &bound, NULL);
    check(bound == g_target_view, "... and the old ones back");
    if (bound) ID3D11RenderTargetView_Release(bound);
    if (previous) ID3DDeviceContextState_Release(previous);
    ID3DDeviceContextState_Release(state);
  }
  if (matrices) ID3D11Buffer_Release(matrices);
  ID3D11Buffer_Release(vertices);
  ID3D11DeviceContext1_Release(context1);
  ID3D11Device1_Release(device1);
}

/* ---- one-dimensional textures ---------------------------------------------------------------------------------------------------------------------------- */

static void test_texture1d(void) {
  const UINT32 texels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};      /* (R8G8B8A8: red, green, blue, white) */
  D3D11_TEXTURE1D_DESC desc = {4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA initial = {texels, 16, 0};
  ID3D11Texture1D *texture = NULL;
  ID3D11ShaderResourceView *view = NULL;
  ID3D11PixelShader *ps = NULL, *ps_array = NULL;
  HRESULT hr = ID3D11Device_CreateTexture1D(g_device, &desc, &initial, &texture);
  D3D11_RESOURCE_DIMENSION type = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  D3D11_TEXTURE1D_DESC back;
  memset(&back, 0, sizeof back);
  if (texture) { ID3D11Texture1D_GetType(texture, &type); ID3D11Texture1D_GetDesc(texture, &back); }
  check(SUCCEEDED(hr) && type == D3D11_RESOURCE_DIMENSION_TEXTURE1D && back.Width == 4 && back.ArraySize == 1, "CreateTexture1D: its type and description");
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)texture, NULL, &view);
  D3D11_SHADER_RESOURCE_VIEW_DESC vd;
  memset(&vd, 0, sizeof vd);
  if (view) ID3D11ShaderResourceView_GetDesc(view, &vd);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_tex1d_dxbc, sizeof dx_ps_tex1d_dxbc, NULL, &ps);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_tex1darray_dxbc, sizeof dx_ps_tex1darray_dxbc, NULL, &ps_array);
  check(SUCCEEDED(hr) && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE1D, "a view of it (TEXTURE1D), and pixel shaders of a Texture1D and a Texture1DArray");
  if (FAILED(hr)) return;
  ID3D11SamplerState *sampler = point_sampler();
  /* (the colour (0.3125, 0.25, 1) makes the coordinate 0.625: texel 2, blue) */
  ID3D11Buffer *vertices = triangle(0.5f, 0.3125f, 0.25f, 1.0f);
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, ps, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &view);
  ID3D11DeviceContext_PSSetSamplers(g_context, 0, 1, &sampler);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  const UINT32 p = pixel(32, 32);
  if (!(blue(p) > 250 && red(p) > 120 && red(p) < 136 && green(p) > 120 && green(p) < 136 && (p >> 24) > 120 && (p >> 24) < 136)) printf("d3d11test: (1D: %08x)\n", (unsigned)p);
  check(blue(p) > 250 && red(p) > 120 && red(p) < 136 && green(p) > 120 && green(p) < 136, "a Texture1D sampled (blue) and loaded at texel 3 (white), half each");
  check((p >> 24) > 120 && (p >> 24) < 136, "... and its GetDimensions: 4 texels wide");

  /* an array of two: the second layer green */
  UINT32 layers[8];
  for (int i = 0; i < 8; ++i) layers[i] = i < 4 ? 0xFF0000FFu : 0xFF00FF00u;
  D3D11_TEXTURE1D_DESC array_desc = {4, 1, 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA array_initial[2] = {{layers, 16, 0}, {layers + 4, 16, 0}};
  ID3D11Texture1D *array = NULL;
  ID3D11ShaderResourceView *array_view = NULL;
  ID3D11Device_CreateTexture1D(g_device, &array_desc, array_initial, &array);
  if (array) ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)array, NULL, &array_view);
  memset(&vd, 0, sizeof vd);
  if (array_view) ID3D11ShaderResourceView_GetDesc(array_view, &vd);
  clear(0, 0, 0);
  ID3D11DeviceContext_PSSetShader(g_context, ps_array, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &array_view);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  const UINT32 q = pixel(32, 32);
  if (!(green(q) > 250 && red(q) < 5 && (q >> 24) > 120 && (q >> 24) < 136)) printf("d3d11test: (1D array: %08x)\n", (unsigned)q);
  check(array_view && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE1DARRAY && vd.Texture1DArray.ArraySize == 2 && green(q) > 250 && red(q) < 5 && (q >> 24) > 120 && (q >> 24) < 136,
        "a Texture1DArray of two sampled at layer 1, and its GetDimensions (two layers)");
  ID3D11ShaderResourceView *no_view = NULL;
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &no_view);
  if (array_view) ID3D11ShaderResourceView_Release(array_view);
  if (array) ID3D11Texture1D_Release(array);
  ID3D11Buffer_Release(vertices);
  if (sampler) ID3D11SamplerState_Release(sampler);
  ID3D11PixelShader_Release(ps_array);
  ID3D11PixelShader_Release(ps);
  ID3D11ShaderResourceView_Release(view);
  ID3D11Texture1D_Release(texture);
}

/* ---- formats the kernel keeps in a wider one ---------------------------------------------------------------------------------------------------------------- */

/* A 4 x 4 render target of `format` cleared to (0.5, 0.25, 1, 1): the bytes of its first texel, read back through a staging texture of the same format. */
static int cleared_texel(DXGI_FORMAT format, BYTE out[16], UINT *bytes) {
  D3D11_TEXTURE2D_DESC desc = {4, 4, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_TEXTURE2D_DESC staging_desc = {4, 4, 1, 1, format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
  ID3D11Texture2D *texture = NULL, *staging = NULL;
  ID3D11RenderTargetView *view = NULL;
  int ok = 0;
  if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &desc, NULL, &texture)) && SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &staging_desc, NULL, &staging)) &&
      SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)texture, NULL, &view))) {
    const float colour[4] = {0.5f, 0.25f, 1.0f, 1.0f};
    ID3D11DeviceContext_ClearRenderTargetView(g_context, view, colour);
    ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)staging, (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) {
      *bytes = mapped.RowPitch / 4;
      memcpy(out, mapped.pData, *bytes <= 16 ? *bytes : 16);
      ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
      ok = 1;
    }
  }
  if (view) ID3D11RenderTargetView_Release(view);
  if (staging) ID3D11Texture2D_Release(staging);
  if (texture) ID3D11Texture2D_Release(texture);
  return ok;
}
static void test_formats(void) {
  BYTE t[16];
  UINT bytes = 0;
  UINT32 w = 0;
  UINT16 h[2] = {0, 0};
  memset(t, 0, sizeof t);
  check(cleared_texel(DXGI_FORMAT_R32_FLOAT, t, &bytes) && bytes == 4 && (memcpy(&w, t, 4), w == 0x3F000000u), "an R32_FLOAT target: cleared, read back (0.5)");
  check(cleared_texel(DXGI_FORMAT_R16G16_FLOAT, t, &bytes) && bytes == 4 && (memcpy(h, t, 4), h[0] == 0x3800 && h[1] == 0x3400), "an R16G16_FLOAT target (0.5, 0.25 as halves)");
  check(cleared_texel(DXGI_FORMAT_R11G11B10_FLOAT, t, &bytes) && bytes == 4 && (memcpy(&w, t, 4), w == 0x781A0380u), "an R11G11B10_FLOAT target (packed 0.5, 0.25, 1)");
  check(cleared_texel(DXGI_FORMAT_R16_UNORM, t, &bytes) && bytes == 2 && (memcpy(h, t, 2), h[0] >= 32767 && h[0] <= 32768), "an R16_UNORM target");
  check(cleared_texel(DXGI_FORMAT_R8G8B8A8_SNORM, t, &bytes) && bytes == 4 && t[0] == 64 && t[1] == 32 && t[2] == 127 && t[3] == 127, "an R8G8B8A8_SNORM target (kept in 16 bits)");
  check(cleared_texel(DXGI_FORMAT_B5G6R5_UNORM, t, &bytes) && bytes == 2 && (memcpy(h, t, 2), h[0] == 0x821F), "a B5G6R5_UNORM target (kept as BGRA8)");
  if (h[0] != 0x821F) printf("d3d11test: (B5G6R5: %04x)\n", h[0]);

  /* an R32_FLOAT texture written, sampled and read back */
  const float values[4] = {0.75f, 0.25f, 0.5f, 1.0f};
  D3D11_TEXTURE2D_DESC desc = {2, 2, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA initial = {values, 8, 0};
  ID3D11Texture2D *texture = NULL;
  ID3D11ShaderResourceView *view = NULL;
  ID3D11Device_CreateTexture2D(g_device, &desc, &initial, &texture);
  if (texture) ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)texture, NULL, &view);
  ID3D11SamplerState *sampler = point_sampler();
  ID3D11Buffer *vertices = triangle(0.5f, 0.1f, 0.1f, 1.0f);      /* (ps_tex: the texel at twice the colour's red and green, (0.2, 0.2): the first; times the colour) */
  begin(NULL);
  clear(0, 0, 0);
  bind_floats(vertices);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_tex, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &view);
  ID3D11DeviceContext_PSSetSamplers(g_context, 0, 1, &sampler);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  read_target();
  const UINT32 p = pixel(32, 32);
  /* (0.75 times 0.1 in red; green and blue 0 (the format has no green or blue); alpha 1) */
  check(view && red(p) >= 17 && red(p) <= 21 && green(p) == 0 && blue(p) == 0, "an R32_FLOAT texture sampled: red as written, green and blue 0");
  if (!(red(p) >= 17 && red(p) <= 21 && green(p) == 0 && blue(p) == 0)) printf("d3d11test: (R32_FLOAT sampled: %08x)\n", (unsigned)p);
  D3D11_TEXTURE2D_DESC staging_desc = {2, 2, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
  ID3D11Texture2D *staging = NULL;
  ID3D11Device_CreateTexture2D(g_device, &staging_desc, NULL, &staging);
  float back[4] = {0, 0, 0, 0};
  if (staging && texture) {
    ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)staging, (ID3D11Resource *)texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) {
      memcpy(back, mapped.pData, 8);
      memcpy(back + 2, (const BYTE *)mapped.pData + mapped.RowPitch, 8);
      ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
    }
  }
  check(memcmp(back, values, sizeof values) == 0, "... and read back as it was written");
  ID3D11ShaderResourceView *no_view = NULL;
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &no_view);
  if (staging) ID3D11Texture2D_Release(staging);
  ID3D11Buffer_Release(vertices);
  if (sampler) ID3D11SamplerState_Release(sampler);
  if (view) ID3D11ShaderResourceView_Release(view);
  if (texture) ID3D11Texture2D_Release(texture);
  UINT support = 0;
  ID3D11Device_CheckFormatSupport(g_device, DXGI_FORMAT_R16G16_FLOAT, &support);
  check((support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) && (support & D3D11_FORMAT_SUPPORT_BLENDABLE),
        "CheckFormatSupport of R16G16_FLOAT: a render target, sampled, blended");
}

/* ---- making devices, 11 on 12, the Windows Runtime's face ------------------------------------------------------------------------------------------------ */

static void test_creation_and_interop(void) {
  ID3D11Device *device = (ID3D11Device *)1;
  D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)1;
  check(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, &level, NULL) == E_INVALIDARG && !device && !level,
        "D3D11CreateDevice: no adapter needs a driver type");
  device = (ID3D11Device *)1;
  check(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_DEBUG, NULL, 0, D3D11_SDK_VERSION, &device, NULL, NULL) == DXGI_ERROR_SDK_COMPONENT_MISSING && !device,
        "a debug device needs the SDK's layers, which are not there");
  ID3D11Multithread *mt = NULL;
  ID3DUserDefinedAnnotation *annotation = NULL;
  ID3D11DeviceContext_QueryInterface(g_context, &IID_ID3D11Multithread, (void **)&mt);
  ID3D11DeviceContext_QueryInterface(g_context, &IID_ID3DUserDefinedAnnotation, (void **)&annotation);
  check(mt && !ID3D11Multithread_GetMultithreadProtected(mt) && ID3D11Multithread_SetMultithreadProtected(mt, TRUE) == FALSE && ID3D11Multithread_GetMultithreadProtected(mt),
        "ID3D11Multithread of the immediate context");
  if (mt) { ID3D11Multithread_Enter(mt); ID3D11Multithread_Leave(mt); ID3D11Multithread_SetMultithreadProtected(mt, FALSE); ID3D11Multithread_Release(mt); }
  check(annotation && ID3DUserDefinedAnnotation_BeginEvent(annotation, L"event") == -1 && ID3DUserDefinedAnnotation_EndEvent(annotation) == -1 &&
        ID3DUserDefinedAnnotation_GetStatus(annotation) == FALSE, "ID3DUserDefinedAnnotation: no profiler (BeginEvent and EndEvent answer -1)");
  if (annotation) ID3DUserDefinedAnnotation_Release(annotation);

  /* D3D11CoreCreateDevice */
  typedef HRESULT (WINAPI *CoreCreate)(IDXGIFactory *, IDXGIAdapter *, UINT, const D3D_FEATURE_LEVEL *, UINT, ID3D11Device **);
  CoreCreate core = (CoreCreate)(void (*)(void))GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11CoreCreateDevice");
  IDXGIFactory *factory = NULL;
  CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
  device = NULL;
  check(core && factory && SUCCEEDED(core(factory, NULL, 0, NULL, 0, &device)) && device && ID3D11Device_GetFeatureLevel(device) == D3D_FEATURE_LEVEL_11_0, "D3D11CoreCreateDevice");
  if (device) ID3D11Device_Release(device);
  if (factory) IDXGIFactory_Release(factory);

  /* the Windows Runtime's device and surface */
  typedef HRESULT (WINAPI *FromDevice)(IDXGIDevice *, IUnknown **);
  typedef HRESULT (WINAPI *FromSurface)(IDXGISurface *, IUnknown **);
  FromDevice from_device = (FromDevice)(void (*)(void))GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "CreateDirect3D11DeviceFromDXGIDevice");
  FromSurface from_surface = (FromSurface)(void (*)(void))GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "CreateDirect3D11SurfaceFromDXGISurface");
  static const GUID iid_access = {0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
  static const GUID iid_closable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
  typedef struct { const struct { HRESULT (WINAPI *qi)(void *, REFIID, void **); ULONG (WINAPI *add)(void *); ULONG (WINAPI *rel)(void *); HRESULT (WINAPI *get)(void *, REFIID, void **); } *v; } Access;
  typedef struct { const struct { void *qi, *add; ULONG (WINAPI *rel)(void *); void *iids, *name, *trust; HRESULT (WINAPI *close)(void *); } *v; } Closable;
  IDXGIDevice *dxgi = NULL;
  ID3D11Device_QueryInterface(g_device, &IID_IDXGIDevice, (void **)&dxgi);
  IUnknown *winrt = NULL;
  Access *access = NULL;
  ID3D11Device *back = NULL;
  check(from_device && dxgi && SUCCEEDED(from_device(dxgi, &winrt)) && winrt && SUCCEEDED(IUnknown_QueryInterface(winrt, &iid_access, (void **)&access)) && access &&
        SUCCEEDED(access->v->get(access, &IID_ID3D11Device, (void **)&back)) && back == g_device,
        "CreateDirect3D11DeviceFromDXGIDevice: an IDirect3DDevice whose IDirect3DDxgiInterfaceAccess gives the device back");
  if (back) ID3D11Device_Release(back);
  Closable *closable = NULL;
  if (winrt) IUnknown_QueryInterface(winrt, &iid_closable, (void **)&closable);
  back = NULL;
  check(closable && SUCCEEDED(closable->v->close(closable)) && access && access->v->get(access, &IID_ID3D11Device, (void **)&back) == (HRESULT)0x80000013 && !back,
        "... and once closed (IClosable) it answers RO_E_CLOSED");
  if (closable) closable->v->rel(closable);
  if (access) access->v->rel(access);
  if (winrt) IUnknown_Release(winrt);
  if (dxgi) IDXGIDevice_Release(dxgi);
  IDXGISurface *surface = NULL;
  ID3D11Texture2D_QueryInterface(g_target, &IID_IDXGISurface, (void **)&surface);
  winrt = NULL;
  typedef struct { const struct { void *qi, *add; ULONG (WINAPI *rel)(void *); void *iids, *name, *trust; HRESULT (WINAPI *desc)(void *, INT32 *); } *v; } WinRtSurface;
  INT32 description[5] = {0, 0, 0, 0, 0};
  check(from_surface && surface && SUCCEEDED(from_surface(surface, &winrt)) && winrt && SUCCEEDED(((WinRtSurface *)winrt)->v->desc(winrt, description)) &&
        description[0] == SIZE && description[1] == SIZE && description[2] == DXGI_FORMAT_B8G8R8A8_UNORM && description[3] == 1,
        "CreateDirect3D11SurfaceFromDXGISurface: an IDirect3DSurface and its description");
  if (winrt) IUnknown_Release(winrt);
  if (surface) IDXGISurface_Release(surface);

  /* 11 on 12: a Direct3D 11 device on the program's Direct3D 12 device, a texture of Direct3D 12 wrapped and cleared through Direct3D 11 */
  ID3D12Device *d12 = NULL;
  ID3D12CommandQueue *queue = NULL;
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  if (SUCCEEDED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&d12)) && d12)
    ID3D12Device_CreateCommandQueue(d12, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
  ID3D11Device *on12 = NULL;
  ID3D11DeviceContext *on12_context = NULL;
  ID3D11On12Device *interop = NULL;
  HRESULT hr = d12 && queue ? D3D11On12CreateDevice((IUnknown *)d12, 0, NULL, 0, (IUnknown **)&queue, 1, 0, &on12, &on12_context, NULL) : E_FAIL;
  if (on12) ID3D11Device_QueryInterface(on12, &IID_ID3D11On12Device, (void **)&interop);
  check(SUCCEEDED(hr) && on12 && on12_context && interop, "D3D11On12CreateDevice: a device on a Direct3D 12 device and queue, an ID3D11On12Device");
  if (interop) {
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ID3D12Resource *res = NULL;
    ID3D12Device_CreateCommittedResource(d12, &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&res);
    const D3D11_RESOURCE_FLAGS flags = {D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
    ID3D11Texture2D *wrapped = NULL;
    if (res) ID3D11On12Device_CreateWrappedResource(interop, (IUnknown *)res, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE, &IID_ID3D11Texture2D, (void **)&wrapped);
    D3D11_TEXTURE2D_DESC wd;
    memset(&wd, 0, sizeof wd);
    if (wrapped) ID3D11Texture2D_GetDesc(wrapped, &wd);
    ID3D11RenderTargetView *view = NULL;
    if (wrapped) ID3D11Device_CreateRenderTargetView(on12, (ID3D11Resource *)wrapped, NULL, &view);
    UINT32 texel = 0;
    if (view) {
      const float green[4] = {0, 1, 0, 1};
      ID3D11DeviceContext_ClearRenderTargetView(on12_context, view, green);
      ID3D11On12Device_ReleaseWrappedResources(interop, (ID3D11Resource **)&wrapped, 1);
      D3D11_TEXTURE2D_DESC sd = {8, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
      ID3D11Texture2D *staging = NULL;
      ID3D11Device_CreateTexture2D(on12, &sd, NULL, &staging);
      if (staging) {
        ID3D11DeviceContext_CopyResource(on12_context, (ID3D11Resource *)staging, (ID3D11Resource *)wrapped);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ID3D11DeviceContext_Map(on12_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) { memcpy(&texel, mapped.pData, 4); ID3D11DeviceContext_Unmap(on12_context, (ID3D11Resource *)staging, 0); }
        ID3D11Texture2D_Release(staging);
      }
    }
    check(wrapped && wd.Width == 8 && wd.Height == 8 && wd.Format == DXGI_FORMAT_R8G8B8A8_UNORM && texel == 0xFF00FF00u, "CreateWrappedResource: the Direct3D 12 texture cleared through Direct3D 11");
    if (view) ID3D11RenderTargetView_Release(view);
    if (wrapped) ID3D11Texture2D_Release(wrapped);
    if (res) ID3D12Resource_Release(res);
    ID3D11On12Device_Release(interop);
  }
  if (on12_context) ID3D11DeviceContext_Release(on12_context);
  if (on12) ID3D11Device_Release(on12);
  if (queue) ID3D12CommandQueue_Release(queue);
  if (d12) ID3D12Device_Release(d12);
}

int main(void) {
  test_device();
  if (g_device && g_context) {
    test_buffers();
    if (make_scene()) {
      test_triangle();
      test_byte_colours();
      test_rewrite_in_one_list();
      test_streamed_offsets();
      test_pixel_constants();
      test_texture();
      test_depth_and_blend();
      test_indexed();
      test_multisample();
      test_small_formats();
      test_sampled_planes();
      test_three_planes(0);
      test_three_planes(1);
      test_texture_kinds();
      test_shader_stages();
      test_compressed();
      test_compute();
      test_pixel_uav_and_queries();
      test_stream_output();
      test_indirect_draws();
      test_min_lod();
      test_deferred();
      test_d3d11_1();
      test_texture1d();
      test_formats();
      test_creation_and_interop();
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
