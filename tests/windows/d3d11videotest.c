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

/* Direct3D 11's video (d3d11.dll's video.inc over d3d12.dll's video): NV12 / P010 textures (the rules they are made by, Map, UpdateSubresource and the copies byte for
 * byte, views of their planes sampled and drawn into), the decoder's profiles, formats and configurations, H.264 and MPEG-2 streams decoded through ID3D11VideoContext
 * (the DXVA buffers filled as ffmpeg does: dxva_streams.h) and compared with ffmpeg's decode (d3d11video_data.h), the video processor (formats, scaling, rotation,
 * mirroring, the background, alpha, the target rectangle, what it refuses), and the content protection there is not. Ends with exit code 42 when every check passed.
 *   tests/emulated_dx.sh tests/windows/d3d11videotest.c */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include "dxfixtures.h"
#include "dxva_streams.h"
#include "d3d11video_data.h"

/* (what mingw's d3d11_4.h does not have: the Windows SDK's) */
DEFINE_GUID(IID_ID3D11VideoDevice2_, 0x59c0cb01, 0x35f0, 0x4a70, 0x8f, 0x67, 0x87, 0x90, 0x5c, 0x90, 0x6a, 0x53);
DEFINE_GUID(IID_ID3D11VideoContext3_, 0xa9e2faa0, 0xcb39, 0x418f, 0xa0, 0xb7, 0xd8, 0xaa, 0xd4, 0xde, 0x67, 0x2e);
DEFINE_GUID(D3D11_KEY_EXCHANGE_HW_PROTECTION_, 0xb1170d8a, 0x628d, 0x4da3, 0xad, 0x3b, 0x82, 0xdd, 0xb0, 0x8b, 0x49, 0x70);
DEFINE_GUID(D3D11_CRYPTO_TYPE_AES128_CTR_, 0x9b6bd711, 0x4f74, 0x41c9, 0x9e, 0x7b, 0x0b, 0xe2, 0xd7, 0xd9, 0x3b, 0x4f);

/* The methods of ID3D11VideoDevice2 and ID3D11VideoContext3 after their bases' (the SDK's d3d11_4.h order): reached through the vtable, which mingw's headers
 * do not declare for them. */
typedef HRESULT(STDMETHODCALLTYPE *VideoCheckFeatureSupportFn)(void *, UINT, void *, UINT);
typedef HRESULT(STDMETHODCALLTYPE *DecoderBeginFrame1Fn)(void *, ID3D11VideoDecoder *, ID3D11VideoDecoderOutputView *, UINT, const void *, UINT, const UINT *, ID3D11Buffer *const *);
typedef struct { D3D11_VIDEO_DECODER_BUFFER_TYPE BufferType; UINT DataOffset, DataSize; void *pIV; UINT IVSize; void *pSubSampleMappingBlock; UINT SubSampleMappingCount, cBlocksStripeEncrypted,
                 cBlocksStripeClear; } BufferDesc2;
typedef HRESULT(STDMETHODCALLTYPE *SubmitDecoderBuffers2Fn)(void *, ID3D11VideoDecoder *, UINT, const BufferDesc2 *);
static void *vtable_entry(void *object, size_t index) { return (*(void ***)object)[index]; }
#define VIDEO_DEVICE1_METHODS (sizeof(ID3D11VideoDevice1Vtbl) / sizeof(void *))
#define VIDEO_CONTEXT2_METHODS (sizeof(ID3D11VideoContext2Vtbl) / sizeof(void *))

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "d3d11videotest: ok: %s\n" : "d3d11videotest: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}
static void checkf(int ok, const char *format, ...) {
  char text[400];
  va_list a;
  va_start(a, format);
  vsnprintf(text, sizeof text, format, a);
  va_end(a);
  check(ok, text);
}

static ID3D11Device *g_device;
static ID3D11DeviceContext *g_context;
static ID3D11VideoDevice *g_vdevice;
static ID3D11VideoContext *g_vcontext;

/* ---- helpers -------------------------------------------------------------------------------------------------------------------------------------------------- */

static unsigned char *base64_decode(const char *s, size_t *size) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t n = strlen(s);
  unsigned char *out = malloc(n / 4 * 3 + 3);
  size_t o = 0;
  unsigned v = 0, bits = 0;
  for (size_t i = 0; i < n && out; ++i) {
    if (s[i] == '=') break;
    const char *at = strchr(table, s[i]);
    if (!at) continue;
    v = (v << 6) | (unsigned)(at - table);
    bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)(v >> bits); }
  }
  *size = o;
  return out;
}
static unsigned fnv(const unsigned char *p, size_t n, unsigned h) {
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}
static ID3D11Texture2D *texture(UINT w, UINT h, UINT layers, DXGI_FORMAT format, D3D11_USAGE usage, UINT bind, UINT cpu, const D3D11_SUBRESOURCE_DATA *initial, HRESULT *hr) {
  const D3D11_TEXTURE2D_DESC desc = {w, h, 1, layers, format, {1, 0}, usage, bind, cpu, 0};
  ID3D11Texture2D *t = NULL;
  const HRESULT r = ID3D11Device_CreateTexture2D(g_device, &desc, initial, &t);
  if (hr) *hr = r;
  return t;
}
/* A slice of an NV12 / P010 texture as the CPU sees it (luma rows, then chroma rows, `w * bpp` bytes each), by way of a staging texture of one slice. */
static int read_planar(ID3D11Texture2D *t, UINT slice, BYTE *out) {
  D3D11_TEXTURE2D_DESC desc;
  ID3D11Texture2D_GetDesc(t, &desc);
  const UINT bpp = desc.Format == DXGI_FORMAT_NV12 ? 1 : 2;
  ID3D11Texture2D *staging = texture(desc.Width, desc.Height, 1, desc.Format, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL, NULL);
  if (!staging) return 0;
  ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)staging, 0, 0, 0, 0, (ID3D11Resource *)t, slice, NULL);
  D3D11_MAPPED_SUBRESOURCE m;
  int ok = SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m)) && m.pData;
  if (ok) {
    for (UINT y = 0; y < desc.Height * 3 / 2; ++y) memcpy(out + (size_t)y * desc.Width * bpp, (BYTE *)m.pData + (size_t)y * m.RowPitch, desc.Width * bpp);
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
  }
  ID3D11Texture2D_Release(staging);
  return ok;
}
static int read_bgra(ID3D11Texture2D *t, UINT32 *out) {
  D3D11_TEXTURE2D_DESC desc;
  ID3D11Texture2D_GetDesc(t, &desc);
  ID3D11Texture2D *staging = texture(desc.Width, desc.Height, 1, desc.Format, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL, NULL);
  if (!staging) return 0;
  ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)staging, (ID3D11Resource *)t);
  D3D11_MAPPED_SUBRESOURCE m;
  int ok = SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m)) && m.pData;
  if (ok) {
    for (UINT y = 0; y < desc.Height; ++y) memcpy(out + (size_t)y * desc.Width, (BYTE *)m.pData + (size_t)y * m.RowPitch, desc.Width * 4);
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
  }
  ID3D11Texture2D_Release(staging);
  return ok;
}
static int close_to(int a, int b, int tolerance) { return abs(a - b) <= tolerance; }
static void video_config_of(D3D11_VIDEO_DECODER_CONFIG *c, UINT raw);

/* ---- the device and its faces --------------------------------------------------------------------------------------------------------------------------------- */

static int test_device(void) {
  D3D_FEATURE_LEVEL level;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &g_device, &level, &g_context);
  check(SUCCEEDED(hr) && g_device && g_context, "D3D11CreateDevice with D3D11_CREATE_DEVICE_VIDEO_SUPPORT");
  if (FAILED(hr)) return 0;
  void *p = NULL;
  hr = ID3D11Device_QueryInterface(g_device, &IID_ID3D11VideoDevice, (void **)&g_vdevice);
  check(SUCCEEDED(hr) && g_vdevice, "the device is an ID3D11VideoDevice");
  check(SUCCEEDED(ID3D11Device_QueryInterface(g_device, &IID_ID3D11VideoDevice1, &p)) && p, "... and an ID3D11VideoDevice1");
  if (p) IUnknown_Release((IUnknown *)p);
  p = NULL;
  check(SUCCEEDED(ID3D11Device_QueryInterface(g_device, &IID_ID3D11VideoDevice2_, &p)) && p, "... and an ID3D11VideoDevice2");
  if (p) IUnknown_Release((IUnknown *)p);
  p = NULL;
  check(g_vdevice && SUCCEEDED(ID3D11VideoDevice_QueryInterface(g_vdevice, &IID_ID3D11Device, &p)) && p == (void *)g_device, "the video device's ID3D11Device is the device");
  if (p) IUnknown_Release((IUnknown *)p);
  hr = ID3D11DeviceContext_QueryInterface(g_context, &IID_ID3D11VideoContext, (void **)&g_vcontext);
  check(SUCCEEDED(hr) && g_vcontext, "the immediate context is an ID3D11VideoContext");
  const GUID *more[3] = {&IID_ID3D11VideoContext1, &IID_ID3D11VideoContext2, &IID_ID3D11VideoContext3_};
  for (int i = 0; i < 3; ++i) {
    p = NULL;
    checkf(SUCCEEDED(ID3D11DeviceContext_QueryInterface(g_context, more[i], &p)) && p, "... and an ID3D11VideoContext%d", i + 1);
    if (p) IUnknown_Release((IUnknown *)p);
  }
  ID3D11Device *from = NULL;
  if (g_vcontext) ID3D11VideoContext_GetDevice(g_vcontext, &from);
  check(from == g_device, "ID3D11VideoContext::GetDevice is the device");
  if (from) ID3D11Device_Release(from);
  ID3D11DeviceContext *deferred = NULL;
  if (SUCCEEDED(ID3D11Device_CreateDeferredContext(g_device, 0, &deferred))) {
    p = (void *)1;
    check(ID3D11DeviceContext_QueryInterface(deferred, &IID_ID3D11VideoContext, &p) == E_NOINTERFACE && !p, "a deferred context is not a video context (as on Windows)");
    ID3D11DeviceContext_Release(deferred);
  }
  return g_vdevice && g_vcontext;
}

/* ---- NV12 / P010 textures ------------------------------------------------------------------------------------------------------------------------------------- */

static void test_planar_rules(void) {
  HRESULT hr;
  ID3D11Texture2D *t;
  static const struct { UINT w, h, mips, layers, samples; DXGI_FORMAT format; D3D11_USAGE usage; UINT bind, cpu, misc; HRESULT want; const char *what; } cases[] = {
      {64, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0, S_OK, "an NV12 texture to sample and draw into"},
      {64, 32, 1, 8, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, 0, S_OK, "an NV12 array for a decoder"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_P010, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DECODER, 0, 0, S_OK, "a P010 texture a decoder writes"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_P016, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0, S_OK, "a P016 texture"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, 0, S_OK, "a staging NV12 texture"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0, S_OK, "a dynamic NV12 texture"},
      {63, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, E_INVALIDARG, "an odd width"},
      {64, 31, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, E_INVALIDARG, "an odd height"},
      {64, 32, 2, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, E_INVALIDARG, "mipmaps"},
      {64, 32, 0, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, E_INVALIDARG, "all the mipmaps (MipLevels 0)"},
      {64, 32, 1, 1, 4, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0, E_INVALIDARG, "multisampling"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0, E_INVALIDARG, "a depth buffer"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_VIDEO_ENCODER, 0, 0, E_INVALIDARG, "a video encoder's input (there is no encoder)"},
      {64, 32, 1, 2, 1, DXGI_FORMAT_NV12, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, E_INVALIDARG, "a staging NV12 array"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED, E_INVALIDARG, "a shared NV12 texture"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_P016, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, 0, E_INVALIDARG, "a P016 texture for a decoder (none puts it out)"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, 0, E_INVALIDARG, "a BGRA texture for a decoder"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
    const D3D11_TEXTURE2D_DESC desc = {cases[i].w, cases[i].h, cases[i].mips, cases[i].layers, cases[i].format, {cases[i].samples, 0}, cases[i].usage, cases[i].bind, cases[i].cpu, cases[i].misc};
    t = NULL;
    hr = ID3D11Device_CreateTexture2D(g_device, &desc, NULL, &t);
    int same = 1;
    if (t) {
      D3D11_TEXTURE2D_DESC got;
      ID3D11Texture2D_GetDesc(t, &got);
      same = memcmp(&got, &desc, sizeof got) == 0;
      ID3D11Texture2D_Release(t);
    }
    checkf(hr == cases[i].want && same, "CreateTexture2D of %s: %#lx (want %#lx)%s", cases[i].what, hr, cases[i].want, same ? "" : ", and GetDesc differs");
  }
  UINT support = 0;
  hr = ID3D11Device_CheckFormatSupport(g_device, DXGI_FORMAT_NV12, &support);
  const UINT want = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_DECODER_OUTPUT |
                   D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
  checkf(hr == S_OK && (support & want) == want && !(support & (D3D11_FORMAT_SUPPORT_MIP | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET | D3D11_FORMAT_SUPPORT_VIDEO_ENCODER)),
         "CheckFormatSupport(NV12): %#x", support);
  hr = ID3D11Device_CheckFormatSupport(g_device, DXGI_FORMAT_P016, &support);
  check(hr == S_OK && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) && !(support & D3D11_FORMAT_SUPPORT_DECODER_OUTPUT), "CheckFormatSupport(P016): a texture, not a decoder's output");
  hr = ID3D11Device_CheckFormatSupport(g_device, DXGI_FORMAT_B8G8R8A8_UNORM, &support);
  check(hr == S_OK && (support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT) && (support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT), "CheckFormatSupport(BGRA8): the video processor's");
  D3D11_FEATURE_DATA_FORMAT_SUPPORT fs = {DXGI_FORMAT_P010, 0};
  hr = ID3D11Device_CheckFeatureSupport(g_device, D3D11_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs);
  check(hr == S_OK && (fs.OutFormatSupport & D3D11_FORMAT_SUPPORT_DECODER_OUTPUT), "CheckFeatureSupport(FORMAT_SUPPORT, P010)");
  D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2 = {DXGI_FORMAT_NV12, 0xFFFFFFFFu};
  hr = ID3D11Device_CheckFeatureSupport(g_device, D3D11_FEATURE_FORMAT_SUPPORT2, &fs2, sizeof fs2);
  check(hr == S_OK && fs2.OutFormatSupport2 == 0, "CheckFeatureSupport(FORMAT_SUPPORT2, NV12): none of the second set");
  D3D11_FEATURE_DATA_D3D11_OPTIONS4 o4 = {TRUE};
  hr = ID3D11Device_CheckFeatureSupport(g_device, D3D11_FEATURE_D3D11_OPTIONS4, &o4, sizeof o4);
  check(hr == S_OK && !o4.ExtendedNV12SharedTextureSupported, "D3D11_OPTIONS4: no shared NV12 textures");
}

/* An NV12 (or P010) frame of a pattern: Y = f(x, y), U, V = g(x, y) (P010: the values in the top 10 bits). */
static void planar_pattern(BYTE *rows, UINT w, UINT h, UINT bpp, UINT pitch, int seed) {
  for (UINT y = 0; y < h; ++y)
    for (UINT x = 0; x < w; ++x) {
      const UINT v = (x * 7 + y * 13 + (UINT)seed) & 0xFF;
      if (bpp == 1) rows[(size_t)y * pitch + x] = (BYTE)v;
      else { const UINT16 s = (UINT16)((v << 2 | 3) << 6); memcpy(rows + (size_t)y * pitch + 2 * x, &s, 2); }
    }
  for (UINT y = 0; y < h / 2; ++y)
    for (UINT x = 0; x < w; ++x) {
      const UINT v = (x * 3 + y * 29 + 100 + (UINT)seed) & 0xFF;
      if (bpp == 1) rows[(size_t)(h + y) * pitch + x] = (BYTE)v;
      else { const UINT16 s = (UINT16)((v << 2) << 6); memcpy(rows + (size_t)(h + y) * pitch + 2 * x, &s, 2); }
    }
}
static void test_planar_cpu(DXGI_FORMAT format, const char *name) {
  const UINT w = 32, h = 16, bpp = format == DXGI_FORMAT_NV12 ? 1 : 2, pitch = w * bpp, bytes = pitch * h * 3 / 2;
  BYTE *a = malloc(bytes), *b = malloc(bytes), *got = malloc(bytes);
  planar_pattern(a, w, h, bpp, pitch, 0);
  planar_pattern(b, w, h, bpp, pitch, 77);
  const D3D11_SUBRESOURCE_DATA initial = {a, pitch, 0};
  HRESULT hr;
  ID3D11Texture2D *t = texture(w, h, 1, format, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &initial, &hr);
  checkf(t && read_planar(t, 0, got) && memcmp(got, a, bytes) == 0, "%s made with its data reads back byte for byte", name);
  /* UpdateSubresource of a box: both planes; a box not on whole 2 x 2 blocks writes nothing */
  BYTE *box_rows = malloc(pitch * 12);
  planar_pattern(box_rows, 8, 8, bpp, 8 * bpp, 31);
  const D3D11_BOX box = {4, 6, 0, 12, 14, 1};
  if (t) ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)t, 0, &box, box_rows, 8 * bpp, 0);
  BYTE *want = malloc(bytes);
  memcpy(want, a, bytes);
  for (UINT y = 0; y < 8; ++y) memcpy(want + (size_t)(6 + y) * pitch + 4 * bpp, box_rows + (size_t)y * 8 * bpp, 8 * bpp);
  for (UINT y = 0; y < 4; ++y) memcpy(want + (size_t)(h + 3 + y) * pitch + 4 * bpp, box_rows + (size_t)(8 + y) * 8 * bpp, 8 * bpp);
  checkf(t && read_planar(t, 0, got) && memcmp(got, want, bytes) == 0, "%s: UpdateSubresource of a box writes it and its chroma", name);
  const D3D11_BOX odd = {5, 6, 0, 12, 14, 1};
  if (t) ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)t, 0, &odd, b, pitch, 0);
  checkf(t && read_planar(t, 0, got) && memcmp(got, want, bytes) == 0, "%s: ... a box not on whole 2 x 2 blocks writes nothing (as Windows)", name);
  /* a staging texture: Map gives the luma rows then the chroma rows at one pitch, DepthPitch both planes */
  ID3D11Texture2D *staging = texture(w, h, 1, format, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, NULL, NULL);
  D3D11_MAPPED_SUBRESOURCE m;
  int mapped = staging && SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_WRITE, 0, &m)) && m.pData;
  checkf(mapped && m.RowPitch >= pitch && m.DepthPitch == m.RowPitch * h * 3 / 2, "%s staging Map: RowPitch %u, DepthPitch %u (the rows of both planes)", name, mapped ? m.RowPitch : 0,
         mapped ? m.DepthPitch : 0);
  if (mapped) {
    for (UINT y = 0; y < h * 3 / 2; ++y) memcpy((BYTE *)m.pData + (size_t)y * m.RowPitch, b + (size_t)y * pitch, pitch);
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
  }
  ID3D11Texture2D *t2 = texture(w, h, 1, format, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  if (t2 && staging) ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)t2, (ID3D11Resource *)staging);
  checkf(t2 && read_planar(t2, 0, got) && memcmp(got, b, bytes) == 0, "%s: written through Map, CopyResource into a default texture, read back byte for byte", name);
  /* CopySubresourceRegion of a box: both planes, at an even place */
  const D3D11_BOX cbox = {2, 4, 0, 10, 8, 1};
  if (t && t2) ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)t, 0, 20, 2, 0, (ID3D11Resource *)t2, 0, &cbox);
  for (UINT y = 0; y < 4; ++y) memcpy(want + (size_t)(2 + y) * pitch + 20 * bpp, b + (size_t)(4 + y) * pitch + 2 * bpp, 8 * bpp);
  for (UINT y = 0; y < 2; ++y) memcpy(want + (size_t)(h + 1 + y) * pitch + 20 * bpp, b + (size_t)(h + 2 + y) * pitch + 2 * bpp, 8 * bpp);
  checkf(t && read_planar(t, 0, got) && memcmp(got, want, bytes) == 0, "%s: CopySubresourceRegion of a box copies it and its chroma", name);
  const D3D11_BOX obox = {3, 4, 0, 10, 8, 1};
  if (t && t2) ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)t, 0, 0, 0, 0, (ID3D11Resource *)t2, 0, &obox);
  checkf(t && read_planar(t, 0, got) && memcmp(got, want, bytes) == 0, "%s: ... and nothing of a box not on whole 2 x 2 blocks", name);
  /* a dynamic texture: Map with WRITE_DISCARD */
  ID3D11Texture2D *dyn = texture(w, h, 1, format, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, NULL, NULL);
  mapped = dyn && SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)dyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) && m.pData;
  if (mapped) {
    for (UINT y = 0; y < h * 3 / 2; ++y) memcpy((BYTE *)m.pData + (size_t)y * m.RowPitch, a + (size_t)y * pitch, pitch);
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)dyn, 0);
  }
  checkf(mapped && read_planar(dyn, 0, got) && memcmp(got, a, bytes) == 0, "%s: a dynamic texture written through Map(WRITE_DISCARD)", name);
  /* an array: a slice copied into another texture's slice, the other slices as they were */
  ID3D11Texture2D *array = texture(w, h, 3, format, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER * (format != DXGI_FORMAT_P016) | D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  if (array && t2) {
    ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)array, 2, 0, 0, 0, (ID3D11Resource *)t2, 0, NULL);
    ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)array, 1, NULL, a, pitch, 0);
  }
  int ok = array && read_planar(array, 2, got) && memcmp(got, b, bytes) == 0;
  ok = ok && read_planar(array, 1, got) && memcmp(got, a, bytes) == 0;
  checkf(ok, "%s array: slice 2 copied from a texture, slice 1 by UpdateSubresource", name);
  if (array) ID3D11Texture2D_Release(array);
  if (dyn) ID3D11Texture2D_Release(dyn);
  if (t2) ID3D11Texture2D_Release(t2);
  if (staging) ID3D11Texture2D_Release(staging);
  if (t) ID3D11Texture2D_Release(t);
  free(want);
  free(box_rows);
  free(a);
  free(b);
  free(got);
}

/* The views of planes: which formats and plane slices; sampling each plane in a draw; clearing each plane's render target view. */
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps_tex;
static ID3D11InputLayout *g_layout;
static ID3D11Texture2D *g_target;
static ID3D11RenderTargetView *g_target_view;
static ID3D11Buffer *g_matrix, *g_triangle;
static ID3D11SamplerState *g_point;
static ID3D11RasterizerState *g_no_cull;
static int make_scene(void) {
  const D3D11_INPUT_ELEMENT_DESC floats[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                              {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  HRESULT hr = ID3D11Device_CreateVertexShader(g_device, dx_vs_dxbc, sizeof dx_vs_dxbc, NULL, &g_vs);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_tex_dxbc, sizeof dx_ps_tex_dxbc, NULL, &g_ps_tex);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(g_device, floats, 2, dx_vs_dxbc, sizeof dx_vs_dxbc, &g_layout);
  g_target = texture(64, 64, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  if (SUCCEEDED(hr) && g_target) hr = ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)g_target, NULL, &g_target_view);
  const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const D3D11_BUFFER_DESC cb = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  const D3D11_SUBRESOURCE_DATA cbd = {m, 0, 0};
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(g_device, &cb, &cbd, &g_matrix);
  /* (a white triangle over the middle; the shader samples at twice the colour's red and green: (2, 2), clamped to the last texel) */
  const float v[21] = {-0.8f, -0.8f, 0.5f, 1, 1, 1, 1, 0.8f, -0.8f, 0.5f, 1, 1, 1, 1, 0.0f, 0.8f, 0.5f, 1, 1, 1, 1};
  const D3D11_BUFFER_DESC vb = {sizeof v, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  const D3D11_SUBRESOURCE_DATA vbd = {v, 0, 0};
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(g_device, &vb, &vbd, &g_triangle);
  D3D11_SAMPLER_DESC sd;
  memset(&sd, 0, sizeof sd);
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateSamplerState(g_device, &sd, &g_point);
  const D3D11_RASTERIZER_DESC raster = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRasterizerState(g_device, &raster, &g_no_cull);
  check(SUCCEEDED(hr), "the shaders, a target and a triangle for the planes' draws");
  return SUCCEEDED(hr);
}
/* What the middle of the target is after a draw that samples `view` (the last texel of the plane). */
static UINT32 sample_view(ID3D11ShaderResourceView *view) {
  const float blue[4] = {0, 0, 1, 1};
  const D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  const UINT stride = 28, offset = 0;
  ID3D11DeviceContext_ClearRenderTargetView(g_context, g_target_view, blue);
  ID3D11DeviceContext_OMSetRenderTargets(g_context, 1, &g_target_view, NULL);
  ID3D11DeviceContext_RSSetViewports(g_context, 1, &vp);
  ID3D11DeviceContext_RSSetState(g_context, g_no_cull);
  ID3D11DeviceContext_IASetInputLayout(g_context, g_layout);
  ID3D11DeviceContext_IASetVertexBuffers(g_context, 0, 1, &g_triangle, &stride, &offset);
  ID3D11DeviceContext_IASetPrimitiveTopology(g_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11DeviceContext_VSSetShader(g_context, g_vs, NULL, 0);
  ID3D11DeviceContext_VSSetConstantBuffers(g_context, 0, 1, &g_matrix);
  ID3D11DeviceContext_PSSetShader(g_context, g_ps_tex, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &view);
  ID3D11DeviceContext_PSSetSamplers(g_context, 0, 1, &g_point);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11ShaderResourceView *none = NULL;
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &none);
  static UINT32 pixels[64 * 64];
  return read_bgra(g_target, pixels) ? pixels[40 * 64 + 32] : 0;
}
static int red(UINT32 p) { return (int)((p >> 16) & 255); }
static int green(UINT32 p) { return (int)((p >> 8) & 255); }
static int blue(UINT32 p) { return (int)(p & 255); }

static void test_planar_views(void) {
  const UINT w = 16, h = 8;
  BYTE frame[16 * 12];
  memset(frame, 30, sizeof frame);
  frame[(h - 1) * w + w - 1] = 200;                    /* (the luma's last texel) */
  frame[(h + h / 2 - 1) * w + w - 2] = 50;             /* (the chroma's last: U, V) */
  frame[(h + h / 2 - 1) * w + w - 1] = 100;
  const D3D11_SUBRESOURCE_DATA initial = {frame, w, 0};
  ID3D11Texture2D *t = texture(w, h, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, &initial, NULL);
  if (!t) { check(0, "an NV12 texture for its views"); return; }
  ID3D11ShaderResourceView *luma = NULL, *chroma = NULL, *bad = NULL;
  D3D11_SHADER_RESOURCE_VIEW_DESC sd;
  memset(&sd, 0, sizeof sd);
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  check(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, NULL, &bad) == E_INVALIDARG, "a shader resource view of NV12 without a description: E_INVALIDARG");
  sd.Format = DXGI_FORMAT_NV12;
  check(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &bad) == E_INVALIDARG, "... of the NV12 format (a view is of one plane): E_INVALIDARG");
  sd.Format = DXGI_FORMAT_R8_UNORM;
  check(SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &luma)), "an R8_UNORM view: the luma plane");
  sd.Format = DXGI_FORMAT_R8G8_UNORM;
  check(SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &chroma)), "an R8G8_UNORM view: the chroma plane");
  sd.Format = DXGI_FORMAT_R16_UNORM;
  check(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &bad) == E_INVALIDARG, "an R16_UNORM view of NV12: E_INVALIDARG");
  /* the *1 descriptions: the plane slice must be the format's plane */
  ID3D11Device3 *d3 = NULL;
  if (SUCCEEDED(ID3D11Device_QueryInterface(g_device, &IID_ID3D11Device3, (void **)&d3))) {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 s1;
    memset(&s1, 0, sizeof s1);
    s1.Format = DXGI_FORMAT_R8G8_UNORM;
    s1.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    s1.Texture2D.MipLevels = 1;
    s1.Texture2D.PlaneSlice = 1;
    ID3D11ShaderResourceView1 *v1 = NULL;
    HRESULT hr = ID3D11Device3_CreateShaderResourceView1(d3, (ID3D11Resource *)t, &s1, &v1);
    D3D11_SHADER_RESOURCE_VIEW_DESC1 got;
    memset(&got, 0, sizeof got);
    if (v1) ID3D11ShaderResourceView1_GetDesc1(v1, &got);
    check(SUCCEEDED(hr) && got.Texture2D.PlaneSlice == 1 && got.Format == DXGI_FORMAT_R8G8_UNORM, "CreateShaderResourceView1 of plane slice 1 (R8G8), and its GetDesc1");
    if (v1) ID3D11ShaderResourceView1_Release(v1);
    s1.Texture2D.PlaneSlice = 0;
    v1 = NULL;
    check(ID3D11Device3_CreateShaderResourceView1(d3, (ID3D11Resource *)t, &s1, &v1) == E_INVALIDARG, "... R8G8 with plane slice 0: E_INVALIDARG");
    D3D11_RENDER_TARGET_VIEW_DESC1 r1;
    memset(&r1, 0, sizeof r1);
    r1.Format = DXGI_FORMAT_R8_UNORM;
    r1.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    r1.Texture2D.PlaneSlice = 1;
    ID3D11RenderTargetView1 *rv1 = NULL;
    check(ID3D11Device3_CreateRenderTargetView1(d3, (ID3D11Resource *)t, &r1, &rv1) == E_INVALIDARG, "CreateRenderTargetView1 of R8 with plane slice 1: E_INVALIDARG");
    ID3D11Texture2D *rgba = texture(4, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
    s1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    s1.Texture2D.PlaneSlice = 1;
    check(rgba && ID3D11Device3_CreateShaderResourceView1(d3, (ID3D11Resource *)rgba, &s1, &v1) == E_INVALIDARG, "plane slice 1 of a format of one plane: E_INVALIDARG");
    if (rgba) ID3D11Texture2D_Release(rgba);
    ID3D11Device3_Release(d3);
  }
  /* sampled: the luma as (Y, 0, 0), the chroma as (U, V, 0) */
  UINT32 got = luma ? sample_view(luma) : 0;
  checkf(luma && red(got) == 200 && green(got) == 0 && blue(got) == 0, "the luma plane sampled: (%d, %d, %d), want (200, 0, 0)", red(got), green(got), blue(got));
  got = chroma ? sample_view(chroma) : 0;
  checkf(chroma && red(got) == 50 && green(got) == 100 && blue(got) == 0, "the chroma plane sampled: (%d, %d, %d), want (50, 100, 0)", red(got), green(got), blue(got));
  /* render target views of each plane, cleared */
  ID3D11RenderTargetView *rl = NULL, *rc = NULL, *rbad = NULL;
  D3D11_RENDER_TARGET_VIEW_DESC rd;
  memset(&rd, 0, sizeof rd);
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  check(ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)t, NULL, &rbad) == E_INVALIDARG, "a render target view of NV12 without a description: E_INVALIDARG");
  rd.Format = DXGI_FORMAT_R8_UNORM;
  HRESULT hr = ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)t, &rd, &rl);
  rd.Format = DXGI_FORMAT_R8G8_UNORM;
  hr |= ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)t, &rd, &rc);
  check(SUCCEEDED(hr) && rl && rc, "render target views of the luma (R8_UNORM) and the chroma (R8G8_UNORM)");
  if (rl && rc) {
    const float yc[4] = {0.5f, 0.9f, 0.9f, 0.9f}, uv[4] = {0.25f, 0.75f, 0.9f, 0.9f};
    ID3D11DeviceContext_ClearRenderTargetView(g_context, rl, yc);
    ID3D11DeviceContext_ClearRenderTargetView(g_context, rc, uv);
    BYTE back[16 * 12];
    int ok = read_planar(t, 0, back), y_ok = 1, uv_ok = 1;
    for (UINT i = 0; ok && i < w * h; ++i) y_ok &= close_to(back[i], 128, 1);
    for (UINT i = 0; ok && i < w * h / 2; i += 2) uv_ok &= close_to(back[w * h + i], 64, 1) && close_to(back[w * h + i + 1], 191, 1);
    checkf(ok && y_ok && uv_ok, "ClearRenderTargetView of the planes: Y %d (want 128), U %d V %d (want 64, 191)", back[0], back[w * h], back[w * h + 1]);
    /* (ClearView of a rectangle of the luma) */
    ID3D11DeviceContext1 *c1 = NULL;
    if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(g_context, &IID_ID3D11DeviceContext1, (void **)&c1))) {
      const float one[4] = {1, 1, 1, 1};
      const D3D11_RECT rect = {2, 2, 6, 4};
      ID3D11DeviceContext1_ClearView(c1, (ID3D11View *)rl, one, &rect, 1);
      ID3D11DeviceContext1_Release(c1);
      ok = read_planar(t, 0, back);
      checkf(ok && back[2 * w + 2] == 255 && back[3 * w + 5] == 255 && close_to(back[2 * w + 6], 128, 1) && close_to(back[w * h], 64, 1), "ClearView of a rectangle of the luma: %d in it, %d beside it",
             back[2 * w + 2], back[2 * w + 6]);
    }
  }
  if (rl) ID3D11RenderTargetView_Release(rl);
  if (rc) ID3D11RenderTargetView_Release(rc);
  if (luma) ID3D11ShaderResourceView_Release(luma);
  if (chroma) ID3D11ShaderResourceView_Release(chroma);
  ID3D11Texture2D_Release(t);
}

/* ---- the decoder: profiles, formats, configurations ------------------------------------------------------------------------------------------------------------- */

static void test_decoder_caps(void) {
  const UINT count = ID3D11VideoDevice_GetVideoDecoderProfileCount(g_vdevice);
  int h264 = 0, mpeg2 = 0, hevc = 0, vp9 = 0, av1 = 0;
  for (UINT i = 0; i < count; ++i) {
    GUID g;
    if (FAILED(ID3D11VideoDevice_GetVideoDecoderProfile(g_vdevice, i, &g))) continue;
    h264 |= IsEqualGUID(&g, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT);
    mpeg2 |= IsEqualGUID(&g, &D3D11_DECODER_PROFILE_MPEG2_VLD);
    hevc |= IsEqualGUID(&g, &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN);
    vp9 |= IsEqualGUID(&g, &D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0);
    av1 |= IsEqualGUID(&g, &DXVA_ModeAV1_VLD_Profile0);
  }
  checkf(count == 7 && h264 && mpeg2 && hevc && vp9 && av1, "GetVideoDecoderProfileCount %u: H.264, MPEG-2, MPEG-1/2, VP9 (two), HEVC, AV1", count);
  GUID g;
  check(ID3D11VideoDevice_GetVideoDecoderProfile(g_vdevice, count, &g) == E_INVALIDARG, "GetVideoDecoderProfile past the last: E_INVALIDARG");
  BOOL ok = FALSE;
  check(SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_NV12, &ok)) && ok, "H.264 decodes into NV12");
  check(SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_P010, &ok)) && !ok, "... not into P010");
  check(SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, &D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2, DXGI_FORMAT_P010, &ok)) && ok, "VP9 profile 2 decodes into P010");
  check(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, &D3D11_DECODER_PROFILE_WMV9_IDCT, DXGI_FORMAT_NV12, &ok) == E_INVALIDARG, "a profile there is not: E_INVALIDARG");
  D3D11_VIDEO_DECODER_DESC desc = {D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 480, 320, DXGI_FORMAT_NV12};
  UINT configs = 0;
  check(SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderConfigCount(g_vdevice, &desc, &configs)) && configs == 1, "GetVideoDecoderConfigCount: one for H.264 480 x 320");
  D3D11_VIDEO_DECODER_CONFIG c;
  memset(&c, 0xCC, sizeof c);
  HRESULT hr = ID3D11VideoDevice_GetVideoDecoderConfig(g_vdevice, &desc, 0, &c);
  check(SUCCEEDED(hr) && IsEqualGUID(&c.guidConfigBitstreamEncryption, &DXVA_NoEncrypt) && IsEqualGUID(&c.guidConfigMBcontrolEncryption, &DXVA_NoEncrypt) &&
            IsEqualGUID(&c.guidConfigResidDiffEncryption, &DXVA_NoEncrypt) && c.ConfigBitstreamRaw == 2 && c.ConfigResidDiffAccelerator == 1 && c.ConfigHostInverseScan == 1 &&
            c.ConfigSpecificIDCT == 2 && !c.ConfigMBcontrolRasterOrder && !c.ConfigResidDiffHost && !c.Config4GroupedCoefs,
        "the H.264 configuration: no encryption, the short slice format (ConfigBitstreamRaw 2), as Windows' drivers give it");
  check(ID3D11VideoDevice_GetVideoDecoderConfig(g_vdevice, &desc, 1, &c) == E_INVALIDARG, "GetVideoDecoderConfig past the last: E_INVALIDARG");
  D3D11_VIDEO_DECODER_DESC hevc_desc = {D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, 64, 64, DXGI_FORMAT_NV12};
  check(SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderConfig(g_vdevice, &hevc_desc, 0, &c)) && c.ConfigBitstreamRaw == 1, "the HEVC configuration: ConfigBitstreamRaw 1 (what ffmpeg takes)");
  D3D11_VIDEO_DECODER_DESC p010 = {D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 64, 64, DXGI_FORMAT_P010};
  check(SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderConfigCount(g_vdevice, &p010, &configs)) && configs == 0, "... no configuration of H.264 into P010");
  /* decoders */
  D3D11_VIDEO_DECODER_CONFIG config;
  video_config_of(&config, 2);
  ID3D11VideoDecoder *dec = NULL;
  D3D11_VIDEO_DECODER_DESC bad = desc;
  memset(&bad.Guid, 0xCC, sizeof bad.Guid);
  check(ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &bad, &config, &dec) == E_INVALIDARG, "CreateVideoDecoder of a profile there is not: E_INVALIDARG");
  config.ConfigBitstreamRaw = 1;
  check(ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &desc, &config, &dec) == E_INVALIDARG, "... of H.264 with the long slice format: E_INVALIDARG");
  config.ConfigBitstreamRaw = 2;
  config.guidConfigBitstreamEncryption = D3D11_CRYPTO_TYPE_AES128_CTR_;
  check(ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &desc, &config, &dec) == E_INVALIDARG, "... with encryption: E_INVALIDARG");
  video_config_of(&config, 2);
  hr = ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &desc, &config, &dec);
  check(SUCCEEDED(hr) && dec, "CreateVideoDecoder of H.264 480 x 320");
  if (dec) {
    D3D11_VIDEO_DECODER_DESC d2;
    D3D11_VIDEO_DECODER_CONFIG c2;
    check(SUCCEEDED(ID3D11VideoDecoder_GetCreationParameters(dec, &d2, &c2)) && !memcmp(&d2, &desc, sizeof d2) && !memcmp(&c2, &config, sizeof c2), "GetCreationParameters");
    HANDLE handle = NULL;
    check(SUCCEEDED(ID3D11VideoDecoder_GetDriverHandle(dec, &handle)) && handle, "GetDriverHandle");
    /* the buffers: one of each type at a time, the same memory each time (Wine's test_h264_decoder) */
    UINT size = 0;
    void *a = NULL, *b2 = NULL;
    hr = ID3D11VideoContext_GetDecoderBuffer(g_vcontext, dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &size, &a);
    check(SUCCEEDED(hr) && a && size >= sizeof(DXVA_PicParams_H264), "GetDecoderBuffer of the picture parameters");
    check(ID3D11VideoContext_GetDecoderBuffer(g_vcontext, dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &size, &b2) == DXGI_ERROR_INVALID_CALL, "... again before it is released: DXGI_ERROR_INVALID_CALL");
    check(ID3D11VideoContext_ReleaseDecoderBuffer(g_vcontext, dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS) == S_OK, "ReleaseDecoderBuffer");
    check(ID3D11VideoContext_ReleaseDecoderBuffer(g_vcontext, dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS) == DXGI_ERROR_INVALID_CALL, "... again: DXGI_ERROR_INVALID_CALL");
    check(SUCCEEDED(ID3D11VideoContext_GetDecoderBuffer(g_vcontext, dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &size, &b2)) && b2 == a, "... and the same memory the next time");
    ID3D11VideoContext_ReleaseDecoderBuffer(g_vcontext, dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);
    static const UINT types[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    int all = 1;
    for (int i = 0; i < 9; ++i) {
      size = 0;
      a = NULL;
      all &= SUCCEEDED(ID3D11VideoContext_GetDecoderBuffer(g_vcontext, dec, (D3D11_VIDEO_DECODER_BUFFER_TYPE)types[i], &size, &a)) && a && size >= 4096;
      ID3D11VideoContext_ReleaseDecoderBuffer(g_vcontext, dec, (D3D11_VIDEO_DECODER_BUFFER_TYPE)types[i]);
    }
    check(all, "memory of every buffer type (picture parameters .. film grain)");
    check(ID3D11VideoContext_GetDecoderBuffer(g_vcontext, dec, (D3D11_VIDEO_DECODER_BUFFER_TYPE)9, &size, &a) == E_INVALIDARG, "a buffer type there is not: E_INVALIDARG");
    /* the status before anything was decoded: zeros (NVIDIA's answer); a buffer too small for one: E_FAIL */
    DXVA_Status_H264 status;
    memset(&status, 0xCC, sizeof status);
    D3D11_VIDEO_DECODER_EXTENSION ext;
    memset(&ext, 0, sizeof ext);
    ext.Function = DXVA_STATUS_REPORTING_FUNCTION;
    ext.pPrivateOutputData = &status;
    ext.PrivateOutputDataSize = sizeof status;
    const DXVA_Status_H264 zero = {0};
    check(ID3D11VideoContext_DecoderExtension(g_vcontext, dec, &ext) == S_OK && !memcmp(&status, &zero, sizeof zero), "DecoderExtension status before any picture: zeros");
    ext.PrivateOutputDataSize = sizeof status - 1;
    check(ID3D11VideoContext_DecoderExtension(g_vcontext, dec, &ext) == E_FAIL, "... into less than one report: E_FAIL");
    ext.Function = 1;
    check(ID3D11VideoContext_DecoderExtension(g_vcontext, dec, &ext) == E_INVALIDARG, "an extension function there is not: E_INVALIDARG");
    /* output views */
    ID3D11Texture2D *out = texture(480, 320, 4, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, NULL, NULL);
    ID3D11Texture2D *no_decoder = texture(480, 320, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {D3D11_DECODER_PROFILE_H264_VLD_NOFGT, D3D11_VDOV_DIMENSION_UNKNOWN, {{2}}};
    ID3D11VideoDecoderOutputView *view = NULL;
    check(out && ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)out, &vd, &view) == E_INVALIDARG, "an output view of no dimension: E_INVALIDARG");
    vd.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
    check(no_decoder && ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)no_decoder, &vd, &view) == E_INVALIDARG, "... of a texture not bound for a decoder: E_INVALIDARG");
    vd.Texture2D.ArraySlice = 4;
    check(ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)out, &vd, &view) == E_INVALIDARG, "... of a slice there is not: E_INVALIDARG");
    vd.Texture2D.ArraySlice = 2;
    hr = ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)out, &vd, &view);
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC got;
    memset(&got, 0, sizeof got);
    ID3D11Resource *res = NULL;
    if (view) { ID3D11VideoDecoderOutputView_GetDesc(view, &got); ID3D11VideoDecoderOutputView_GetResource(view, &res); }
    check(SUCCEEDED(hr) && !memcmp(&got, &vd, sizeof got) && res == (ID3D11Resource *)out, "an output view of slice 2, its GetDesc and GetResource");
    if (res) ID3D11Resource_Release(res);
    /* the frame calls in their order */
    check(ID3D11VideoContext_DecoderEndFrame(g_vcontext, dec) == DXGI_ERROR_INVALID_CALL, "DecoderEndFrame with no picture begun: DXGI_ERROR_INVALID_CALL");
    D3D11_VIDEO_DECODER_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    check(ID3D11VideoContext_SubmitDecoderBuffers(g_vcontext, dec, 1, &bd) == DXGI_ERROR_INVALID_CALL, "SubmitDecoderBuffers with no picture begun: DXGI_ERROR_INVALID_CALL");
    const BYTE key[16] = {1};
    check(ID3D11VideoContext_DecoderBeginFrame(g_vcontext, dec, view, 16, key) == E_INVALIDARG, "DecoderBeginFrame with a content key (no encryption): E_INVALIDARG");
    void *c3 = NULL;
    if (SUCCEEDED(ID3D11VideoContext_QueryInterface(g_vcontext, &IID_ID3D11VideoContext3_, &c3))) {
      const UINT offsets[1] = {0};
      ID3D11Buffer *histogram = NULL;
      check(((DecoderBeginFrame1Fn)vtable_entry(c3, VIDEO_CONTEXT2_METHODS))(c3, dec, view, 0, NULL, 1, offsets, &histogram) == E_INVALIDARG,
            "DecoderBeginFrame1 with a histogram (there are none): E_INVALIDARG");
      IUnknown_Release((IUnknown *)c3);
    }
    check(ID3D11VideoContext_DecoderBeginFrame(g_vcontext, dec, view, 0, NULL) == S_OK, "DecoderBeginFrame");
    check(ID3D11VideoContext_DecoderBeginFrame(g_vcontext, dec, view, 0, NULL) == DXGI_ERROR_INVALID_CALL, "... again before the end: DXGI_ERROR_INVALID_CALL");
    bd.BufferType = D3D11_VIDEO_DECODER_BUFFER_RESIDUAL_DIFFERENCE;
    bd.DataSize = 16;
    check(ID3D11VideoContext_SubmitDecoderBuffers(g_vcontext, dec, 1, &bd) == E_INVALIDARG, "a residual difference buffer to a VLD decoder: E_INVALIDARG");
    bd.BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
    bd.DataSize = 1u << 30;
    check(ID3D11VideoContext_SubmitDecoderBuffers(g_vcontext, dec, 1, &bd) == E_INVALIDARG, "a bitstream larger than its buffer: E_INVALIDARG");
    check(ID3D11VideoContext_DecoderEndFrame(g_vcontext, dec) == E_INVALIDARG, "DecoderEndFrame of a picture that was not described: E_INVALIDARG");
    check(ID3D11VideoContext_DecoderEndFrame(g_vcontext, dec) == DXGI_ERROR_INVALID_CALL, "... and the picture is over");
    if (view) ID3D11VideoDecoderOutputView_Release(view);
    if (out) ID3D11Texture2D_Release(out);
    if (no_decoder) ID3D11Texture2D_Release(no_decoder);
    ID3D11VideoDecoder_Release(dec);
  }
  /* ID3D11VideoDevice1 .. 2 */
  ID3D11VideoDevice1 *v1 = NULL;
  if (SUCCEEDED(ID3D11VideoDevice_QueryInterface(g_vdevice, &IID_ID3D11VideoDevice1, (void **)&v1))) {
    UINT caps = 0xFF;
    const DXGI_RATIONAL rate = {30, 1};
    check(SUCCEEDED(ID3D11VideoDevice1_GetVideoDecoderCaps(v1, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 480, 320, &rate, 0, NULL, &caps)) && caps == 0, "GetVideoDecoderCaps: no downsampling");
    check(SUCCEEDED(ID3D11VideoDevice1_GetVideoDecoderCaps(v1, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 480, 320, &rate, 0, &D3D11_CRYPTO_TYPE_AES128_CTR_, &caps)) &&
              caps == D3D11_VIDEO_DECODER_CAPS_UNSUPPORTED,
          "... encrypted: UNSUPPORTED");
    BOOL supported = TRUE, real_time = TRUE;
    D3D11_VIDEO_SAMPLE_DESC sample = {240, 160, DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709};
    video_config_of(&config, 2);
    check(SUCCEEDED(ID3D11VideoDevice1_CheckVideoDecoderDownsampling(v1, &desc, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, &config, &rate, &sample, &supported, &real_time)) && !supported,
          "CheckVideoDecoderDownsampling: not supported");
    check(ID3D11VideoDevice1_RecommendVideoDecoderDownsampleParameters(v1, &desc, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, &config, &rate, &sample) == E_INVALIDARG,
          "RecommendVideoDecoderDownsampleParameters: E_INVALIDARG");
    UINT in = 1, outsz = 1;
    check(ID3D11VideoDevice1_GetCryptoSessionPrivateDataSize(v1, &D3D11_CRYPTO_TYPE_AES128_CTR_, NULL, &D3D11_KEY_EXCHANGE_HW_PROTECTION_, &in, &outsz) == E_INVALIDARG && !in && !outsz,
          "GetCryptoSessionPrivateDataSize: E_INVALIDARG (no crypto type)");
    ID3D11VideoDevice1_Release(v1);
  }
  void *v2 = NULL;
  if (SUCCEEDED(ID3D11VideoDevice_QueryInterface(g_vdevice, &IID_ID3D11VideoDevice2_, &v2))) {
    /* D3D11_FEATURE_DATA_VIDEO_DECODER_HISTOGRAM: the decoder's description, then the components, bins and counter bits */
    struct { D3D11_VIDEO_DECODER_DESC desc; UINT components, bins, bits; } h = {desc, 0xFF, 0xFF, 0xFF};
    VideoCheckFeatureSupportFn f = (VideoCheckFeatureSupportFn)vtable_entry(v2, VIDEO_DEVICE1_METHODS);
    check(SUCCEEDED(f(v2, 0, &h, sizeof h)) && h.components == 0 && h.bins == 0 && h.bits == 0, "ID3D11VideoDevice2::CheckFeatureSupport(DECODER_HISTOGRAM): no histograms");
    check(f(v2, 1, &h, sizeof h) == E_INVALIDARG, "... a feature there is not: E_INVALIDARG");
    IUnknown_Release((IUnknown *)v2);
  }
}
/* (the configuration this runtime gives: what a program passes back) */
static void video_config_of(D3D11_VIDEO_DECODER_CONFIG *c, UINT raw) {
  memset(c, 0, sizeof *c);
  c->guidConfigBitstreamEncryption = c->guidConfigMBcontrolEncryption = c->guidConfigResidDiffEncryption = DXVA_NoEncrypt;
  c->ConfigBitstreamRaw = raw;
  c->ConfigResidDiffAccelerator = 1;
  c->ConfigHostInverseScan = 1;
  c->ConfigSpecificIDCT = 2;
  c->ConfigMinRenderTargetBuffCount = 1;
}

/* ---- decoding streams ----------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct Decoded { long long key; unsigned hash; BYTE *frame; } Decoded;
static int by_key(const void *a, const void *b) {
  const long long x = ((const Decoded *)a)->key, y = ((const Decoded *)b)->key;
  return x < y ? -1 : x > y;
}
/* Copies a buffer of the decoder's in and gives it back. */
static int fill_buffer(ID3D11VideoDecoder *dec, D3D11_VIDEO_DECODER_BUFFER_TYPE type, UINT offset, const void *data, UINT size) {
  UINT room = 0;
  void *memory = NULL;
  if (FAILED(ID3D11VideoContext_GetDecoderBuffer(g_vcontext, dec, type, &room, &memory)) || !memory || offset > room || size > room - offset) return 0;
  memcpy((BYTE *)memory + offset, data, size);
  return SUCCEEDED(ID3D11VideoContext_ReleaseDecoderBuffer(g_vcontext, dec, type));
}
static D3D11_VIDEO_DECODER_BUFFER_DESC buffer_desc(D3D11_VIDEO_DECODER_BUFFER_TYPE type, UINT offset, UINT size) {
  D3D11_VIDEO_DECODER_BUFFER_DESC d;
  memset(&d, 0, sizeof d);
  d.BufferType = type;
  d.DataOffset = offset;
  d.DataSize = size;
  return d;
}
/* One picture through the video context. A picture of several slices goes in two SubmitDecoderBuffers calls (the parameters and the first slice; the other slices, put at
 * an offset of 64 into the bitstream buffer), as a program splits a large picture. */
static int decode_picture(ID3D11VideoDecoder *dec, ID3D11VideoDecoderOutputView *view, const DxvaPicture *p, UINT slice_bytes, UINT location_offset, int split) {
  if (split == 2) {
    /* (by ID3D11VideoContext3: DecoderBeginFrame1 without histograms, SubmitDecoderBuffers2) */
    void *c3 = NULL;
    if (FAILED(ID3D11VideoContext_QueryInterface(g_vcontext, &IID_ID3D11VideoContext3_, &c3))) return 0;
    int ok = SUCCEEDED(((DecoderBeginFrame1Fn)vtable_entry(c3, VIDEO_CONTEXT2_METHODS))(c3, dec, view, 0, NULL, 0, NULL, NULL)) &&
             fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, p->picture_parameters, p->picture_parameters_size) &&
             fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 0, p->qmatrix, p->qmatrix_size) &&
             fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, p->slice_control, p->slice_control_size) &&
             fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, p->bitstream, p->bitstream_size);
    BufferDesc2 b2[4];
    memset(b2, 0, sizeof b2);
    const D3D11_VIDEO_DECODER_BUFFER_TYPE types[4] = {D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
                                                      D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM};
    const UINT sizes[4] = {p->picture_parameters_size, p->qmatrix_size, p->slice_control_size, p->bitstream_size};
    for (int i = 0; i < 4; ++i) { b2[i].BufferType = types[i]; b2[i].DataSize = sizes[i]; }
    ok = ok && SUCCEEDED(((SubmitDecoderBuffers2Fn)vtable_entry(c3, VIDEO_CONTEXT2_METHODS + 1))(c3, dec, 4, b2));
    IUnknown_Release((IUnknown *)c3);
    return SUCCEEDED(ID3D11VideoContext_DecoderEndFrame(g_vcontext, dec)) && ok;
  }
  if (FAILED(ID3D11VideoContext_DecoderBeginFrame(g_vcontext, dec, view, 0, NULL))) return 0;
  int ok = fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, p->picture_parameters, p->picture_parameters_size) &&
           fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 0, p->qmatrix, p->qmatrix_size);
  D3D11_VIDEO_DECODER_BUFFER_DESC b[4];
  b[0] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, p->picture_parameters_size);
  b[1] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 0, p->qmatrix_size);
  if (!split || p->slice_count < 2) {
    ok = ok && fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, p->slice_control, p->slice_control_size) &&
         fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, p->bitstream, p->bitstream_size);
    b[2] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, p->slice_control_size);
    b[3] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, p->bitstream_size);
    ok = ok && SUCCEEDED(ID3D11VideoContext_SubmitDecoderBuffers(g_vcontext, dec, 4, b));
  } else {
    /* (the first slice: its bytes run up to where the second's begin) */
    UINT second;
    memcpy(&second, (const BYTE *)p->slice_control + slice_bytes + location_offset, 4);
    ok = ok && fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, p->slice_control, slice_bytes) &&
         fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, p->bitstream, second);
    b[2] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, slice_bytes);
    b[3] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, second);
    ok = ok && SUCCEEDED(ID3D11VideoContext_SubmitDecoderBuffers(g_vcontext, dec, 4, b));
    /* (the rest: their offsets into the buffer, which holds their bytes from 64 on) */
    const UINT rest = p->slice_control_size - slice_bytes;
    BYTE *controls = malloc(rest);
    memcpy(controls, (const BYTE *)p->slice_control + slice_bytes, rest);
    for (UINT e = 0; e + slice_bytes <= rest; e += slice_bytes) {
      UINT at;
      memcpy(&at, controls + e + location_offset, 4);
      at = at - second + 64;
      memcpy(controls + e + location_offset, &at, 4);
    }
    ok = ok && fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, controls, rest) &&
         fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 64, p->bitstream + second, p->bitstream_size - second);
    free(controls);
    b[0] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, rest);
    b[1] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 64, p->bitstream_size - second);
    ID3D11VideoContext1 *c1 = NULL;
    if (ok && SUCCEEDED(ID3D11VideoContext_QueryInterface(g_vcontext, &IID_ID3D11VideoContext1, (void **)&c1))) {
      /* (by SubmitDecoderBuffers1) */
      D3D11_VIDEO_DECODER_BUFFER_DESC1 b1[2];
      memset(b1, 0, sizeof b1);
      for (int i = 0; i < 2; ++i) { b1[i].BufferType = b[i].BufferType; b1[i].DataOffset = b[i].DataOffset; b1[i].DataSize = b[i].DataSize; }
      ok = SUCCEEDED(ID3D11VideoContext1_SubmitDecoderBuffers1(c1, dec, 2, b1));
      ID3D11VideoContext1_Release(c1);
    } else ok = 0;
  }
  return SUCCEEDED(ID3D11VideoContext_DecoderEndFrame(g_vcontext, dec)) && ok;
}
/* A stream decoded into an NV12 array of `surfaces` slices (the DXVA indices); each picture read back as it is decoded; then in display order against ffmpeg's frames:
 * their hashes (H.264), or the means of their 4 x 4 blocks within 2 (MPEG-2). */
static void decode_stream(const char *name, int codec, const GUID *profile, const char *b64, unsigned surfaces, const unsigned *hashes, UINT frames,
                          const unsigned char (*means)[576], int split) {
  size_t size = 0;
  unsigned char *data = base64_decode(b64, &size);
  DxvaStream s;
  DxvaPicture p;
  if (!data || dxva_stream_open(&s, codec, data, size, surfaces) != 0 || dxva_stream_next(&s, &p) != 1) { checkf(0, "%s: the stream parses (%s)", name, s.error ? s.error : ""); free(data); return; }
  const D3D11_VIDEO_DECODER_DESC desc = {*profile, s.width, s.height, DXGI_FORMAT_NV12};
  D3D11_VIDEO_DECODER_CONFIG config;
  UINT count = 0;
  HRESULT hr = ID3D11VideoDevice_GetVideoDecoderConfigCount(g_vdevice, &desc, &count);
  if (SUCCEEDED(hr) && count) hr = ID3D11VideoDevice_GetVideoDecoderConfig(g_vdevice, &desc, 0, &config);
  ID3D11VideoDecoder *dec = NULL;
  if (SUCCEEDED(hr)) hr = ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &desc, &config, &dec);
  ID3D11Texture2D *out = texture(s.width, s.height, surfaces, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, NULL, NULL);
  ID3D11VideoDecoderOutputView *views[DXVA_STREAM_MAX_SURFACES] = {0};
  for (unsigned i = 0; SUCCEEDED(hr) && out && i < surfaces; ++i) {
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {*profile, D3D11_VDOV_DIMENSION_TEXTURE2D, {{i}}};
    hr = ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)out, &vd, &views[i]);
  }
  checkf(SUCCEEDED(hr) && dec && out, "%s: a decoder of %u x %u, an NV12 array of %u slices and their output views", name, s.width, s.height, surfaces);
  const UINT vw = s.width - s.crop_left - s.crop_right, vh = s.height - s.crop_top - s.crop_bottom, cw = (vw + 1) / 2, ch = (vh + 1) / 2;
  const UINT frame_bytes = vw * vh + 2 * cw * ch;
  Decoded *decoded = calloc(256, sizeof *decoded);
  BYTE *nv12 = malloc((size_t)s.width * s.height * 3 / 2);
  UINT n = 0, failed = 0, last_feedback = 0;
  int r = 1;
  while (SUCCEEDED(hr) && dec && out && r == 1 && n < 256) {
    if (codec == DXVA_CODEC_H264) last_feedback = ((const DXVA_PicParams_H264 *)p.picture_parameters)->StatusReportFeedbackNumber;
    const UINT slice_bytes = codec == DXVA_CODEC_H264 ? sizeof(DXVA_Slice_H264_Short) : sizeof(DXVA_SliceInfo);
    const UINT location = codec == DXVA_CODEC_H264 ? offsetof(DXVA_Slice_H264_Short, BSNALunitDataLocation) : offsetof(DXVA_SliceInfo, dwSliceDataLocation);
    if (!decode_picture(dec, views[p.surface], &p, slice_bytes, location, split) || !read_planar(out, p.surface, nv12)) { ++failed; break; }
    /* (the visible picture as I420) */
    BYTE *f = malloc(frame_bytes);
    for (UINT y = 0; y < vh; ++y) memcpy(f + (size_t)y * vw, nv12 + (size_t)(s.crop_top + y) * s.width + s.crop_left, vw);
    for (UINT y = 0; y < ch; ++y)
      for (UINT x = 0; x < cw; ++x) {
        const BYTE *uv = nv12 + (size_t)s.width * s.height + (size_t)(s.crop_top / 2 + y) * s.width + 2 * (s.crop_left / 2 + x);
        f[vw * vh + y * cw + x] = uv[0];
        f[vw * vh + cw * ch + y * cw + x] = uv[1];
      }
    decoded[n].key = p.display_key;
    decoded[n].hash = fnv(f, frame_bytes, 0x811c9dc5u);
    decoded[n].frame = f;
    ++n;
    r = dxva_stream_next(&s, &p);
  }
  checkf(!failed && r == 0, "%s: %u pictures decoded through ID3D11VideoContext%s (%s)", name, n,
         split == 2 ? "3 (DecoderBeginFrame1, SubmitDecoderBuffers2)" : split ? ", the slices of a picture in two SubmitDecoderBuffers calls" : "",
         s.error ? s.error : "to the end of the stream");
  qsort(decoded, n, sizeof *decoded, by_key);
  if (hashes) {
    UINT same = 0, first_bad = ~0u;
    for (UINT i = 0; i < n && i < frames; ++i) {
      if (decoded[i].hash == hashes[i]) ++same;
      else if (first_bad == ~0u) first_bad = i;
    }
    checkf(n == frames && same == frames, "%s: in display order every frame is ffmpeg's, bit for bit (%u of %u; the first that differs: %d)", name, same, frames, (int)first_bad);
  } else {
    int worst = 0;
    long long total = 0, blocks = 0;
    for (UINT i = 0; i < n && i < frames; ++i) {
      const BYTE *f = decoded[i].frame;
      UINT k = 0;
      const UINT dims[3][3] = {{vw, vh, 0}, {cw, ch, vw * vh}, {cw, ch, vw * vh + cw * ch}};
      for (int pl = 0; pl < 3; ++pl)
        for (UINT by = 0; by + 4 <= dims[pl][1]; by += 4)
          for (UINT bx = 0; bx + 4 <= dims[pl][0]; bx += 4, ++k) {
            UINT sum = 0;
            for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x) sum += f[dims[pl][2] + (by + y) * dims[pl][0] + bx + x];
            const int d = abs((int)((sum + 8) / 16) - (int)means[i][k]);
            if (d > worst) worst = d;
            total += d;
            ++blocks;
          }
    }
    checkf(n == frames && worst <= 2 && blocks && total * 4 < blocks, "%s: %u frames in display order match ffmpeg's (4 x 4 block means: largest difference %d, mean %.3f)", name, n, worst,
           blocks ? (double)total / (double)blocks : 0.0);
  }
  if (dec && codec == DXVA_CODEC_H264) {
    DXVA_Status_H264 st[2];
    memset(st, 0xCC, sizeof st);
    D3D11_VIDEO_DECODER_EXTENSION ext;
    memset(&ext, 0, sizeof ext);
    ext.Function = DXVA_STATUS_REPORTING_FUNCTION;
    ext.pPrivateOutputData = st;
    ext.PrivateOutputDataSize = sizeof st;
    hr = ID3D11VideoContext_DecoderExtension(g_vcontext, dec, &ext);
    checkf(hr == S_OK && st[0].StatusReportFeedbackNumber == last_feedback && st[1].StatusReportFeedbackNumber == last_feedback - 1 && st[0].bStatus == 0 &&
               st[0].bDXVA_Func == DXVA_PICTURE_DECODING_FUNCTION && st[0].bBufType == 0xFF,
           "%s: the status reports, the newest first: feedback %u and %u, status %u", name, st[0].StatusReportFeedbackNumber, st[1].StatusReportFeedbackNumber, st[0].bStatus);
  }
  for (UINT i = 0; i < n; ++i) free(decoded[i].frame);
  free(decoded);
  free(nv12);
  for (unsigned i = 0; i < surfaces; ++i) if (views[i]) ID3D11VideoDecoderOutputView_Release(views[i]);
  if (out) ID3D11Texture2D_Release(out);
  if (dec) ID3D11VideoDecoder_Release(dec);
  dxva_stream_close(&s);
  free(data);
}

/* A decoded picture sampled in a draw: the draw after the decode sees it (the device orders its video work with its draws). */
static void test_decode_then_draw(void) {
  size_t size = 0;
  unsigned char *data = base64_decode(g_h264_cavlc_b64, &size);
  DxvaStream s;
  DxvaPicture p;
  if (!data || dxva_stream_open(&s, DXVA_CODEC_H264, data, size, 1) != 0 || dxva_stream_next(&s, &p) != 1) { check(0, "the stream for a decode and a draw"); free(data); return; }
  const D3D11_VIDEO_DECODER_DESC desc = {D3D11_DECODER_PROFILE_H264_VLD_NOFGT, s.width, s.height, DXGI_FORMAT_NV12};
  D3D11_VIDEO_DECODER_CONFIG config;
  video_config_of(&config, 2);
  ID3D11VideoDecoder *dec = NULL;
  ID3D11Texture2D *t = texture(s.width, s.height, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
  ID3D11VideoDecoderOutputView *view = NULL;
  ID3D11ShaderResourceView *luma = NULL;
  ID3D11RenderTargetView *luma_target = NULL;
  D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {D3D11_DECODER_PROFILE_H264_VLD_NOFGT, D3D11_VDOV_DIMENSION_TEXTURE2D, {{0}}};
  D3D11_SHADER_RESOURCE_VIEW_DESC sd;
  memset(&sd, 0, sizeof sd);
  sd.Format = DXGI_FORMAT_R8_UNORM;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  D3D11_RENDER_TARGET_VIEW_DESC rd;
  memset(&rd, 0, sizeof rd);
  rd.Format = DXGI_FORMAT_R8_UNORM;
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  HRESULT hr = ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &desc, &config, &dec);
  if (SUCCEEDED(hr)) hr = t ? ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)t, &vd, &view) : E_FAIL;
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &luma);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)t, &rd, &luma_target);
  if (SUCCEEDED(hr)) {
    /* (a clear recorded before the decode: the decode comes after it and overwrites it) */
    const float white[4] = {1, 1, 1, 1};
    ID3D11DeviceContext_ClearRenderTargetView(g_context, luma_target, white);
    const int decoded = decode_picture(dec, view, &p, sizeof(DXVA_Slice_H264_Short), 0, 0);
    BYTE *nv12 = malloc((size_t)s.width * s.height * 3 / 2);
    const int read = decoded && read_planar(t, 0, nv12);
    const int last = read ? nv12[(size_t)(s.height - 1) * s.width + s.width - 1] : -1;
    const UINT32 got = read ? sample_view(luma) : 0;
    checkf(read && last != 255 && red(got) == last && green(got) == 0, "a picture decoded after a clear, then sampled in a draw: %d (the decoded %d)", red(got), last);
    free(nv12);
  } else check(0, "a decoder, an NV12 texture to decode into, sample and draw into, and its views");
  if (luma_target) ID3D11RenderTargetView_Release(luma_target);
  if (luma) ID3D11ShaderResourceView_Release(luma);
  if (view) ID3D11VideoDecoderOutputView_Release(view);
  if (t) ID3D11Texture2D_Release(t);
  if (dec) ID3D11VideoDecoder_Release(dec);
  dxva_stream_close(&s);
  free(data);
}

/* ---- the video processor ---------------------------------------------------------------------------------------------------------------------------------------- */

/* BT.601, studio range YCbCr as full-range RGB. */
static void rgb601(int y, int u, int v, int rgb[3]) {
  const double Y = 1.164383 * (y - 16), U = u - 128, V = v - 128;
  const double c[3] = {Y + 1.596027 * V, Y - 0.391762 * U - 0.812968 * V, Y + 2.017232 * U};
  for (int k = 0; k < 3; ++k) { const int x = (int)(c[k] + 0.5); rgb[k] = x < 0 ? 0 : x > 255 ? 255 : x; }
}
static const int g_quads[4][3] = {{81, 90, 240}, {145, 54, 34}, {41, 240, 110}, {235, 128, 128}};      /* (top left, top right, bottom left, bottom right: Y, U, V) */
static ID3D11Texture2D *quad_texture(void) {
  static BYTE f[64 * 96];
  for (UINT y = 0; y < 64; ++y)
    for (UINT x = 0; x < 64; ++x) f[y * 64 + x] = (BYTE)g_quads[(y >= 32) * 2 + (x >= 32)][0];
  for (UINT y = 0; y < 32; ++y)
    for (UINT x = 0; x < 32; ++x) {
      const int q = (y >= 16) * 2 + (x >= 16);
      f[64 * 64 + y * 64 + 2 * x] = (BYTE)g_quads[q][1];
      f[64 * 64 + y * 64 + 2 * x + 1] = (BYTE)g_quads[q][2];
    }
  const D3D11_SUBRESOURCE_DATA initial = {f, 64, 0};
  return texture(64, 64, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &initial, NULL);
}
static int pixel_is(const UINT32 *pixels, int x, int y, const int rgb[3], int tolerance) {
  const UINT32 p = pixels[y * 64 + x];
  return close_to(red(p), rgb[0], tolerance) && close_to(green(p), rgb[1], tolerance) && close_to(blue(p), rgb[2], tolerance);
}
static int quad_at(const UINT32 *pixels, int x, int y, int q) {
  int rgb[3];
  rgb601(g_quads[q][0], g_quads[q][1], g_quads[q][2], rgb);
  return pixel_is(pixels, x, y, rgb, 3);
}

static void test_processor(void) {
  const D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {30, 1}, 64, 64, {30, 1}, 64, 64, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL};
  ID3D11VideoProcessorEnumerator *e = NULL;
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC bad = content;
  bad.InputWidth = 0;
  check(ID3D11VideoDevice_CreateVideoProcessorEnumerator(g_vdevice, &bad, &e) == E_INVALIDARG, "CreateVideoProcessorEnumerator of no width: E_INVALIDARG");
  HRESULT hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(g_vdevice, &content, &e);
  check(SUCCEEDED(hr) && e, "CreateVideoProcessorEnumerator");
  if (!e) return;
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC got;
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorContentDesc(e, &got)) && !memcmp(&got, &content, sizeof got), "GetVideoProcessorContentDesc");
  D3D11_VIDEO_PROCESSOR_CAPS caps;
  hr = ID3D11VideoProcessorEnumerator_GetVideoProcessorCaps(e, &caps);
  const UINT want_features = D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ALPHA_FILL | D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION | D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ALPHA_STREAM |
                             D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_MIRROR;
  checkf(SUCCEEDED(hr) && (caps.FeatureCaps & want_features) == want_features && !(caps.FeatureCaps & (D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_LUMA_KEY | D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_STEREO |
                                                                                                         D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_CONSTRICTION)) &&
             caps.FilterCaps == 0 && caps.RateConversionCapsCount == 1 && caps.MaxInputStreams == 16 && caps.MaxStreamStates == 16,
         "GetVideoProcessorCaps: device %#x, features %#x, filters %#x, %u rate conversions, %u streams", caps.DeviceCaps, caps.FeatureCaps, caps.FilterCaps, caps.RateConversionCapsCount,
         caps.MaxInputStreams);
  D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rate;
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorRateConversionCaps(e, 0, &rate)) && !rate.PastFrames && !rate.FutureFrames && !rate.ProcessorCaps && !rate.CustomRateCount,
        "the rate conversion: progressive frames, no past or future frames, no deinterlacing");
  check(ID3D11VideoProcessorEnumerator_GetVideoProcessorRateConversionCaps(e, 1, &rate) == E_INVALIDARG, "... there is one: E_INVALIDARG for the second");
  D3D11_VIDEO_PROCESSOR_CUSTOM_RATE custom;
  check(ID3D11VideoProcessorEnumerator_GetVideoProcessorCustomRate(e, 0, 0, &custom) == E_INVALIDARG, "GetVideoProcessorCustomRate: none (E_INVALIDARG)");
  D3D11_VIDEO_PROCESSOR_FILTER_RANGE range;
  check(ID3D11VideoProcessorEnumerator_GetVideoProcessorFilterRange(e, D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS, &range) == E_INVALIDARG, "GetVideoProcessorFilterRange: no filters (E_INVALIDARG)");
  UINT flags = 0;
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(e, DXGI_FORMAT_NV12, &flags)) && flags == 3, "CheckVideoProcessorFormat(NV12): input and output");
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(e, DXGI_FORMAT_R8_UNORM, &flags)) && flags == 0, "CheckVideoProcessorFormat(R8): neither");
  ID3D11VideoProcessorEnumerator1 *e1 = NULL;
  if (SUCCEEDED(ID3D11VideoProcessorEnumerator_QueryInterface(e, &IID_ID3D11VideoProcessorEnumerator1, (void **)&e1))) {
    BOOL ok = FALSE;
    check(SUCCEEDED(ID3D11VideoProcessorEnumerator1_CheckVideoProcessorFormatConversion(e1, DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                                        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &ok)) && ok,
          "CheckVideoProcessorFormatConversion: NV12 BT.709 to BGRA sRGB");
    check(SUCCEEDED(ID3D11VideoProcessorEnumerator1_CheckVideoProcessorFormatConversion(e1, DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                                        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &ok)) && !ok,
          "... not from PQ (no tone mapping)");
    ID3D11VideoProcessorEnumerator1_Release(e1);
  } else check(0, "the enumerator is an ID3D11VideoProcessorEnumerator1");
  ID3D11VideoProcessor *vp = NULL;
  check(ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, 1, &vp) == E_INVALIDARG, "CreateVideoProcessor of a rate conversion there is not: E_INVALIDARG");
  hr = ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, 0, &vp);
  check(SUCCEEDED(hr) && vp, "CreateVideoProcessor");
  if (!vp) { ID3D11VideoProcessorEnumerator_Release(e); return; }
  D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS r2;
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC c2;
  ID3D11VideoProcessor_GetRateConversionCaps(vp, &r2);
  ID3D11VideoProcessor_GetContentDesc(vp, &c2);
  check(!memcmp(&c2, &content, sizeof c2) && !r2.PastFrames, "the processor's GetContentDesc and GetRateConversionCaps");

  /* the state, set and read back */
  ID3D11VideoContext *v = g_vcontext;
  BOOL enabled;
  RECT rect;
  const RECT target = {1, 2, 30, 40};
  ID3D11VideoContext_VideoProcessorSetOutputTargetRect(v, vp, TRUE, &target);
  ID3D11VideoContext_VideoProcessorGetOutputTargetRect(v, vp, &enabled, &rect);
  check(enabled && !memcmp(&rect, &target, sizeof rect), "the output target rectangle");
  ID3D11VideoContext_VideoProcessorSetOutputTargetRect(v, vp, FALSE, NULL);
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE space = {0, 1, 1, 0, 2, 0}, space_got;
  ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, vp, 3, &space);
  ID3D11VideoContext_VideoProcessorGetStreamColorSpace(v, vp, 3, &space_got);
  check(!memcmp(&space, &space_got, sizeof space), "a stream's colour space");
  ID3D11VideoContext_VideoProcessorSetStreamAlpha(v, vp, 2, TRUE, 0.25f);
  FLOAT alpha = 0;
  ID3D11VideoContext_VideoProcessorGetStreamAlpha(v, vp, 2, &enabled, &alpha);
  check(enabled && alpha == 0.25f, "a stream's planar alpha");
  ID3D11VideoContext_VideoProcessorSetStreamRotation(v, vp, 1, TRUE, D3D11_VIDEO_PROCESSOR_ROTATION_270);
  D3D11_VIDEO_PROCESSOR_ROTATION rot;
  ID3D11VideoContext_VideoProcessorGetStreamRotation(v, vp, 1, &enabled, &rot);
  check(enabled && rot == D3D11_VIDEO_PROCESSOR_ROTATION_270, "a stream's rotation");
  ID3D11VideoContext_VideoProcessorSetStreamRotation(v, vp, 1, FALSE, D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY);
  ID3D11VideoContext_VideoProcessorSetStreamAlpha(v, vp, 2, FALSE, 1.0f);
  const UINT palette[3] = {0x11223344u, 0x55667788u, 0x99AABBCCu};
  UINT palette_got[4] = {1, 1, 1, 1};
  ID3D11VideoContext_VideoProcessorSetStreamPalette(v, vp, 0, 3, palette);
  ID3D11VideoContext_VideoProcessorGetStreamPalette(v, vp, 0, 4, palette_got);
  check(!memcmp(palette, palette_got, sizeof palette) && palette_got[3] == 0, "a stream's palette");
  ID3D11VideoContext_VideoProcessorSetStreamPalette(v, vp, 0, 0, NULL);
  DXGI_HDR_METADATA_HDR10 hdr;
  memset(&hdr, 0, sizeof hdr);
  hdr.MaxMasteringLuminance = 1000;
  hdr.MaxContentLightLevel = 400;
  ID3D11VideoContext2 *v2 = NULL;
  if (SUCCEEDED(ID3D11VideoContext_QueryInterface(v, &IID_ID3D11VideoContext2, (void **)&v2))) {
    DXGI_HDR_METADATA_HDR10 hdr_got;
    DXGI_HDR_METADATA_TYPE type;
    ID3D11VideoContext2_VideoProcessorSetStreamHDRMetaData(v2, vp, 0, DXGI_HDR_METADATA_TYPE_HDR10, sizeof hdr, &hdr);
    ID3D11VideoContext2_VideoProcessorGetStreamHDRMetaData(v2, vp, 0, &type, sizeof hdr_got, &hdr_got);
    check(type == DXGI_HDR_METADATA_TYPE_HDR10 && !memcmp(&hdr, &hdr_got, sizeof hdr), "a stream's HDR10 metadata, kept and given back");
    ID3D11VideoContext2_VideoProcessorSetOutputColorSpace1(v2, vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    DXGI_COLOR_SPACE_TYPE cs;
    ID3D11VideoContext2_VideoProcessorGetOutputColorSpace1(v2, vp, &cs);
    check(cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, "the output's colour space (1)");
    BOOL shader = FALSE;
    ID3D11VideoContext2_VideoProcessorSetOutputShaderUsage(v2, vp, TRUE);
    ID3D11VideoContext2_VideoProcessorGetOutputShaderUsage(v2, vp, &shader);
    check(shader, "the output's shader usage");
    UINT hints = 0xFF;
    const D3D11_VIDEO_PROCESSOR_STREAM_BEHAVIOR_HINT sh = {TRUE, 64, 64, DXGI_FORMAT_NV12};
    check(SUCCEEDED(ID3D11VideoContext2_VideoProcessorGetBehaviorHints(v2, vp, 64, 64, DXGI_FORMAT_B8G8R8A8_UNORM, 1, &sh, &hints)) && hints == 0, "VideoProcessorGetBehaviorHints: none");
    ID3D11VideoContext2_Release(v2);
  } else check(0, "an ID3D11VideoContext2");
  GUID ext_guid = {1, 2, 3, {4}};
  BYTE ext_data[4];
  check(ID3D11VideoContext_VideoProcessorSetOutputExtension(v, vp, &ext_guid, sizeof ext_data, ext_data) == E_INVALIDARG, "an output extension: none (E_INVALIDARG)");

  /* views */
  ID3D11Texture2D *input = quad_texture();
  ID3D11Texture2D *output = texture(64, 64, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  ID3D11Texture2D *not_target = texture(64, 64, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  ID3D11Texture2D *r8 = texture(64, 64, 1, DXGI_FORMAT_R8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  ID3D11VideoProcessorInputView *in_view = NULL, *bad_in = NULL;
  ID3D11VideoProcessorOutputView *out_view = NULL, *bad_out = NULL;
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {0, D3D11_VPIV_DIMENSION_TEXTURE2D, {{0, 0}}};
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {D3D11_VPOV_DIMENSION_TEXTURE2D, {{0}}};
  check(r8 && ID3D11VideoDevice_CreateVideoProcessorInputView(g_vdevice, (ID3D11Resource *)r8, e, &ivd, &bad_in) == E_INVALIDARG, "an input view of R8: E_INVALIDARG");
  ivd.FourCC = 0x30313050;      /* ('P010' for an NV12 texture) */
  check(input && ID3D11VideoDevice_CreateVideoProcessorInputView(g_vdevice, (ID3D11Resource *)input, e, &ivd, &bad_in) == E_INVALIDARG, "an input view of a FourCC not the texture's: E_INVALIDARG");
  ivd.FourCC = 0x3231564E;      /* ('NV12') */
  hr = input ? ID3D11VideoDevice_CreateVideoProcessorInputView(g_vdevice, (ID3D11Resource *)input, e, &ivd, &in_view) : E_FAIL;
  check(SUCCEEDED(hr) && in_view, "an input view of the NV12 texture");
  check(not_target && ID3D11VideoDevice_CreateVideoProcessorOutputView(g_vdevice, (ID3D11Resource *)not_target, e, &ovd, &bad_out) == E_INVALIDARG, "an output view of no render target: E_INVALIDARG");
  hr = output ? ID3D11VideoDevice_CreateVideoProcessorOutputView(g_vdevice, (ID3D11Resource *)output, e, &ovd, &out_view) : E_FAIL;
  check(SUCCEEDED(hr) && out_view, "an output view of the BGRA texture");
  if (!in_view || !out_view) goto done;
  static UINT32 pixels[64 * 64];
  D3D11_VIDEO_PROCESSOR_STREAM stream;
  memset(&stream, 0, sizeof stream);
  stream.Enable = TRUE;
  stream.pInputSurface = in_view;
  /* NV12 into BGRA, all of it: the four colours, BT.601 */
  hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream);
  int ok = SUCCEEDED(hr) && read_bgra(output, pixels);
  checkf(ok && quad_at(pixels, 16, 16, 0) && quad_at(pixels, 48, 16, 1) && quad_at(pixels, 16, 48, 2) && quad_at(pixels, 48, 48, 3), "VideoProcessorBlt NV12 to BGRA: the four quarters' colours (%#lx)", hr);
  /* scaled into a destination rectangle, over a red background */
  const D3D11_VIDEO_COLOR red_bg = {{{1, 0, 0, 1}}};
  const RECT dest = {16, 16, 48, 48};
  ID3D11VideoContext_VideoProcessorSetOutputBackgroundColor(v, vp, FALSE, &red_bg);
  ID3D11VideoContext_VideoProcessorSetStreamDestRect(v, vp, 0, TRUE, &dest);
  hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream);
  ok = SUCCEEDED(hr) && read_bgra(output, pixels);
  const int red_rgb[3] = {255, 0, 0};
  check(ok && pixel_is(pixels, 4, 4, red_rgb, 1) && pixel_is(pixels, 60, 30, red_rgb, 1) && quad_at(pixels, 24, 24, 0) && quad_at(pixels, 40, 24, 1) && quad_at(pixels, 24, 40, 2) &&
            quad_at(pixels, 40, 40, 3),
        "... scaled to half into a destination rectangle, over the background colour");
  ID3D11VideoContext_VideoProcessorSetStreamDestRect(v, vp, 0, FALSE, NULL);
  /* a source rectangle: the top right quarter, all over the output */
  const RECT source = {32, 0, 64, 32};
  ID3D11VideoContext_VideoProcessorSetStreamSourceRect(v, vp, 0, TRUE, &source);
  hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream);
  ok = SUCCEEDED(hr) && read_bgra(output, pixels);
  check(ok && quad_at(pixels, 8, 8, 1) && quad_at(pixels, 56, 56, 1), "... a source rectangle (the top right quarter) scaled up to all of it");
  ID3D11VideoContext_VideoProcessorSetStreamSourceRect(v, vp, 0, FALSE, NULL);
  /* rotated a quarter clockwise: the top left goes to the top right, the bottom left to the top left */
  ID3D11VideoContext_VideoProcessorSetStreamRotation(v, vp, 0, TRUE, D3D11_VIDEO_PROCESSOR_ROTATION_90);
  hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream);
  ok = SUCCEEDED(hr) && read_bgra(output, pixels);
  check(ok && quad_at(pixels, 48, 16, 0) && quad_at(pixels, 16, 16, 2) && quad_at(pixels, 48, 48, 1) && quad_at(pixels, 16, 48, 3), "... rotated 90 degrees clockwise");
  ID3D11VideoContext_VideoProcessorSetStreamRotation(v, vp, 0, FALSE, D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY);
  /* mirrored left to right */
  ID3D11VideoContext2 *mirror = NULL;
  if (SUCCEEDED(ID3D11VideoContext_QueryInterface(v, &IID_ID3D11VideoContext2, (void **)&mirror))) {
    ID3D11VideoContext2_VideoProcessorSetStreamMirror(mirror, vp, 0, TRUE, TRUE, FALSE);
    hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream);
    ok = SUCCEEDED(hr) && read_bgra(output, pixels);
    check(ok && quad_at(pixels, 16, 16, 1) && quad_at(pixels, 48, 16, 0) && quad_at(pixels, 16, 48, 3), "... mirrored left to right");
    ID3D11VideoContext2_VideoProcessorSetStreamMirror(mirror, vp, 0, FALSE, FALSE, FALSE);
    ID3D11VideoContext2_Release(mirror);
  }
  /* a target rectangle: the right half is left as it was (green, cleared before the Blt: the clear comes first) */
  ID3D11RenderTargetView *out_rtv = NULL;
  ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)output, NULL, &out_rtv);
  const float green_c[4] = {0, 1, 0, 1};
  if (out_rtv) ID3D11DeviceContext_ClearRenderTargetView(g_context, out_rtv, green_c);
  const RECT left = {0, 0, 32, 64};
  ID3D11VideoContext_VideoProcessorSetOutputTargetRect(v, vp, TRUE, &left);
  hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream);
  ok = SUCCEEDED(hr) && read_bgra(output, pixels);
  const int green_rgb[3] = {0, 255, 0};
  check(ok && quad_at(pixels, 16, 16, 0) && quad_at(pixels, 16, 48, 2) && pixel_is(pixels, 48, 16, green_rgb, 0) && pixel_is(pixels, 48, 48, green_rgb, 0),
        "... a target rectangle: the left half drawn, the right half as a clear before it left it");
  ID3D11VideoContext_VideoProcessorSetOutputTargetRect(v, vp, FALSE, NULL);
  /* planar alpha: a white BGRA stream at half over the picture */
  BYTE white[64 * 64 * 4];
  memset(white, 255, sizeof white);
  const D3D11_SUBRESOURCE_DATA wd = {white, 256, 0};
  ID3D11Texture2D *overlay = texture(64, 64, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &wd, NULL);
  ID3D11VideoProcessorInputView *overlay_view = NULL;
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ovd2 = {0, D3D11_VPIV_DIMENSION_TEXTURE2D, {{0, 0}}};
  if (overlay) ID3D11VideoDevice_CreateVideoProcessorInputView(g_vdevice, (ID3D11Resource *)overlay, e, &ovd2, &overlay_view);
  if (overlay_view) {
    D3D11_VIDEO_PROCESSOR_STREAM two[2];
    memset(two, 0, sizeof two);
    two[0] = stream;
    two[1].Enable = TRUE;
    two[1].pInputSurface = overlay_view;
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb_full = {0, 0, 0, 0, 0, 0};
    ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, vp, 1, &rgb_full);
    ID3D11VideoContext_VideoProcessorSetStreamAlpha(v, vp, 1, TRUE, 0.5f);
    hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 2, two);
    ok = SUCCEEDED(hr) && read_bgra(output, pixels);
    int q0[3], blend[3];
    rgb601(g_quads[0][0], g_quads[0][1], g_quads[0][2], q0);
    for (int k = 0; k < 3; ++k) blend[k] = (q0[k] + 255 + 1) / 2;
    checkf(ok && pixel_is(pixels, 16, 16, blend, 4), "... a second stream blended at half alpha over the first: (%d, %d, %d), want about (%d, %d, %d)", red(pixels[16 * 64 + 16]),
           green(pixels[16 * 64 + 16]), blue(pixels[16 * 64 + 16]), blend[0], blend[1], blend[2]);
    ID3D11VideoContext_VideoProcessorSetStreamAlpha(v, vp, 1, FALSE, 1.0f);
    /* RGB into NV12: white is Y 235, U V 128 */
    ID3D11Texture2D *nv12_out = texture(64, 64, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11VideoProcessorOutputView *nv12_view = NULL;
    if (nv12_out) ID3D11VideoDevice_CreateVideoProcessorOutputView(g_vdevice, (ID3D11Resource *)nv12_out, e, &ovd, &nv12_view);
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE ycbcr601 = {0, 0, 0, 0, 1, 0};
    ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, vp, &ycbcr601);
    two[0] = two[1];
    BYTE *frame = malloc(64 * 96);
    hr = nv12_view ? ID3D11VideoContext_VideoProcessorBlt(v, vp, nv12_view, 0, 1, two) : E_FAIL;
    ok = SUCCEEDED(hr) && read_planar(nv12_out, 0, frame);
    checkf(ok && close_to(frame[100], 235, 1) && close_to(frame[64 * 64 + 10], 128, 1) && close_to(frame[64 * 64 + 11], 128, 1), "white BGRA into NV12: Y %d, U %d, V %d", ok ? frame[100] : -1,
           ok ? frame[64 * 64 + 10] : -1, ok ? frame[64 * 64 + 11] : -1);
    free(frame);
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb_out = {0, 0, 0, 0, 0, 0};
    ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, vp, &rgb_out);
    if (nv12_view) ID3D11VideoProcessorOutputView_Release(nv12_view);
    if (nv12_out) ID3D11Texture2D_Release(nv12_out);
    ID3D11VideoProcessorInputView_Release(overlay_view);
  } else check(0, "an input view of a BGRA texture");
  if (overlay) ID3D11Texture2D_Release(overlay);
  /* no stream drawn: the background, here given as YCbCr (the red of the first quarter) */
  const D3D11_VIDEO_COLOR ycbcr_bg = {{{81.0f / 255.0f, 90.0f / 255.0f, 240.0f / 255.0f, 1}}};
  ID3D11VideoContext_VideoProcessorSetOutputBackgroundColor(v, vp, TRUE, &ycbcr_bg);
  D3D11_VIDEO_PROCESSOR_STREAM off = stream;
  off.Enable = FALSE;
  hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &off);
  ok = SUCCEEDED(hr) && read_bgra(output, pixels);
  checkf(ok && quad_at(pixels, 5, 5, 0) && quad_at(pixels, 60, 60, 0), "no stream enabled: the target is the background, a YCbCr colour into RGB (%#lx: %d, %d, %d)", hr,
         red(pixels[5 * 64 + 5]), green(pixels[5 * 64 + 5]), blue(pixels[5 * 64 + 5]));
  /* into the second slice of a BGRA array (by way of a copy below) and out of a level of a texture with levels */
  ID3D11Texture2D *array = texture(64, 64, 2, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  ID3D11VideoProcessorOutputView *slice_view = NULL;
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC avd = {D3D11_VPOV_DIMENSION_TEXTURE2DARRAY, {{0}}};
  avd.Texture2DArray.FirstArraySlice = 1;
  avd.Texture2DArray.ArraySize = 1;
  if (array) ID3D11VideoDevice_CreateVideoProcessorOutputView(g_vdevice, (ID3D11Resource *)array, e, &avd, &slice_view);
  ID3D11Texture2D *flat = texture(64, 64, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  if (slice_view && flat) {
    ID3D11VideoContext_VideoProcessorSetOutputBackgroundColor(v, vp, FALSE, &red_bg);
    hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, slice_view, 0, 1, &stream);
    ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)flat, 0, 0, 0, 0, (ID3D11Resource *)array, 1, NULL);
    ok = SUCCEEDED(hr) && read_bgra(flat, pixels);
    check(ok && quad_at(pixels, 16, 16, 0) && quad_at(pixels, 48, 48, 3), "VideoProcessorBlt into the second slice of an array");
    /* that slice read back through an input view of a mip-mapped copy's first level */
    D3D11_TEXTURE2D_DESC md = {64, 64, 2, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *levels = NULL;
    ID3D11VideoProcessorInputView *level_view = NULL;
    ID3D11Device_CreateTexture2D(g_device, &md, NULL, &levels);
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC lvd = {0, D3D11_VPIV_DIMENSION_TEXTURE2D, {{0, 0}}};
    if (levels) {
      ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)levels, 0, 0, 0, 0, (ID3D11Resource *)flat, 0, NULL);
      ID3D11VideoDevice_CreateVideoProcessorInputView(g_vdevice, (ID3D11Resource *)levels, e, &lvd, &level_view);
    }
    if (level_view) {
      D3D11_VIDEO_PROCESSOR_STREAM ls = stream;
      ls.pInputSurface = level_view;
      const D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb_in = {0, 0, 0, 0, 0, 0};
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, vp, 0, &rgb_in);
      ID3D11VideoContext_VideoProcessorSetStreamRotation(v, vp, 0, TRUE, D3D11_VIDEO_PROCESSOR_ROTATION_180);
      hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &ls);
      ok = SUCCEEDED(hr) && read_bgra(output, pixels);
      check(ok && quad_at(pixels, 48, 48, 0) && quad_at(pixels, 16, 16, 3), "... and out of a level of a texture with levels, turned half round");
      ID3D11VideoContext_VideoProcessorSetStreamRotation(v, vp, 0, FALSE, D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY);
      const D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv_in = {0, 0, 0, 0, 0, 0};
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, vp, 0, &yuv_in);
      ID3D11VideoProcessorInputView_Release(level_view);
    } else check(0, "an input view of a level of a texture with levels");
    if (levels) ID3D11Texture2D_Release(levels);
  } else check(0, "an output view of the second slice of a BGRA array");
  if (slice_view) ID3D11VideoProcessorOutputView_Release(slice_view);
  if (array) ID3D11Texture2D_Release(array);
  if (flat) ID3D11Texture2D_Release(flat);
  /* what the processor does not do */
  ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, vp, 0, D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST);
  check(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream) == E_INVALIDARG, "Blt of an interlaced stream (no deinterlacing): E_INVALIDARG");
  ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  ID3D11VideoContext_VideoProcessorSetStreamLumaKey(v, vp, 0, TRUE, 0.1f, 0.2f);
  check(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream) == E_INVALIDARG, "... with a luma key: E_INVALIDARG");
  ID3D11VideoContext_VideoProcessorSetStreamLumaKey(v, vp, 0, FALSE, 0, 0);
  ID3D11VideoContext_VideoProcessorSetStreamFilter(v, vp, 0, D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS, TRUE, 10);
  check(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream) == E_INVALIDARG, "... with a filter: E_INVALIDARG");
  ID3D11VideoContext_VideoProcessorSetStreamFilter(v, vp, 0, D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS, FALSE, 0);
  ID3D11VideoContext_VideoProcessorSetOutputStereoMode(v, vp, TRUE);
  check(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream) == E_INVALIDARG, "... into a stereo output: E_INVALIDARG");
  ID3D11VideoContext_VideoProcessorSetOutputStereoMode(v, vp, FALSE);
  const SIZE small = {32, 32}, none_size = {0, 0};
  ID3D11VideoContext_VideoProcessorSetOutputConstriction(v, vp, TRUE, small);
  check(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream) == E_INVALIDARG, "... with constriction: E_INVALIDARG");
  ID3D11VideoContext_VideoProcessorSetOutputConstriction(v, vp, FALSE, none_size);
  D3D11_VIDEO_PROCESSOR_STREAM past = stream;
  past.PastFrames = 1;
  past.ppPastSurfaces = &in_view;
  check(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &past) == E_INVALIDARG, "... with a past frame: E_INVALIDARG");
  check(SUCCEEDED(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream)), "... and with the state back as it was, S_OK");
  if (out_rtv) ID3D11RenderTargetView_Release(out_rtv);
done:
  if (in_view) ID3D11VideoProcessorInputView_Release(in_view);
  if (out_view) ID3D11VideoProcessorOutputView_Release(out_view);
  if (input) ID3D11Texture2D_Release(input);
  if (output) ID3D11Texture2D_Release(output);
  if (not_target) ID3D11Texture2D_Release(not_target);
  if (r8) ID3D11Texture2D_Release(r8);
  ID3D11VideoProcessor_Release(vp);
  ID3D11VideoProcessorEnumerator_Release(e);
}

/* ---- content protection: there is none -------------------------------------------------------------------------------------------------------------------------- */

static void test_content_protection(void) {
  D3D11_VIDEO_CONTENT_PROTECTION_CAPS caps;
  memset(&caps, 0xCC, sizeof caps);
  HRESULT hr = ID3D11VideoDevice_GetContentProtectionCaps(g_vdevice, NULL, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, &caps);
  check(hr == S_OK && caps.Caps == 0 && caps.KeyExchangeTypeCount == 0 && caps.ProtectedMemorySize == 0, "GetContentProtectionCaps without encryption: no capabilities, no key exchange");
  check(ID3D11VideoDevice_GetContentProtectionCaps(g_vdevice, &D3D11_CRYPTO_TYPE_AES128_CTR_, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, &caps) == E_INVALIDARG,
        "... of AES-128 CTR (no crypto type here): E_INVALIDARG");
  GUID exchange;
  check(ID3D11VideoDevice_CheckCryptoKeyExchange(g_vdevice, &D3D11_CRYPTO_TYPE_AES128_CTR_, NULL, 0, &exchange) == E_INVALIDARG, "CheckCryptoKeyExchange: E_INVALIDARG");
  ID3D11CryptoSession *session = (ID3D11CryptoSession *)1;
  check(ID3D11VideoDevice_CreateCryptoSession(g_vdevice, &D3D11_CRYPTO_TYPE_AES128_CTR_, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, &D3D11_KEY_EXCHANGE_HW_PROTECTION_, &session) == E_INVALIDARG &&
            !session,
        "CreateCryptoSession: E_INVALIDARG");
  ID3D11AuthenticatedChannel *channel = (ID3D11AuthenticatedChannel *)1;
  check(ID3D11VideoDevice_CreateAuthenticatedChannel(g_vdevice, D3D11_AUTHENTICATED_CHANNEL_D3D11, &channel) == DXGI_ERROR_UNSUPPORTED && !channel,
        "CreateAuthenticatedChannel(D3D11): DXGI_ERROR_UNSUPPORTED");
  check(ID3D11VideoDevice_CreateAuthenticatedChannel(g_vdevice, D3D11_AUTHENTICATED_CHANNEL_DRIVER_HARDWARE, &channel) == DXGI_ERROR_UNSUPPORTED, "... (DRIVER_HARDWARE): DXGI_ERROR_UNSUPPORTED");
  check(ID3D11VideoDevice_CreateAuthenticatedChannel(g_vdevice, (D3D11_AUTHENTICATED_CHANNEL_TYPE)7, &channel) == E_INVALIDARG, "... of a type there is not: E_INVALIDARG");
  BYTE data[64];
  check(ID3D11VideoContext_NegotiateCryptoSessionKeyExchange(g_vcontext, NULL, sizeof data, data) == E_INVALIDARG, "NegotiateCryptoSessionKeyExchange: E_INVALIDARG");
  check(ID3D11VideoContext_GetEncryptionBltKey(g_vcontext, NULL, sizeof data, data) == E_INVALIDARG, "GetEncryptionBltKey: E_INVALIDARG");
  check(ID3D11VideoContext_NegotiateAuthenticatedChannelKeyExchange(g_vcontext, NULL, sizeof data, data) == E_INVALIDARG, "NegotiateAuthenticatedChannelKeyExchange: E_INVALIDARG");
  check(ID3D11VideoContext_QueryAuthenticatedChannel(g_vcontext, NULL, sizeof data, data, sizeof data, data) == E_INVALIDARG, "QueryAuthenticatedChannel: E_INVALIDARG");
  D3D11_AUTHENTICATED_CONFIGURE_OUTPUT co;
  check(ID3D11VideoContext_ConfigureAuthenticatedChannel(g_vcontext, NULL, sizeof data, data, &co) == E_INVALIDARG, "ConfigureAuthenticatedChannel: E_INVALIDARG");
  ID3D11VideoContext1 *c1 = NULL;
  if (SUCCEEDED(ID3D11VideoContext_QueryInterface(g_vcontext, &IID_ID3D11VideoContext1, (void **)&c1))) {
    D3D11_CRYPTO_SESSION_STATUS status;
    UINT64 key = 1;
    check(ID3D11VideoContext1_CheckCryptoSessionStatus(c1, NULL, &status) == E_INVALIDARG, "CheckCryptoSessionStatus: E_INVALIDARG");
    check(ID3D11VideoContext1_GetDataForNewHardwareKey(c1, NULL, sizeof data, data, &key) == E_INVALIDARG && key == 0, "GetDataForNewHardwareKey: E_INVALIDARG");
    ID3D11VideoContext1_Release(c1);
  }
}

int main(void) {
  if (test_device()) {
    test_planar_rules();
    test_planar_cpu(DXGI_FORMAT_NV12, "NV12");
    test_planar_cpu(DXGI_FORMAT_P010, "P010");
    if (make_scene()) {
      test_planar_views();
      test_decoder_caps();
      decode_stream("h264_bframes", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_bframes_b64, 17, g_h264_bframes_fnv, 14, NULL, 0);
      decode_stream("h264_cavlc", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_cavlc_b64, 17, g_h264_cavlc_fnv, 10, NULL, 1);
      decode_stream("h264_ltr", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_ltr_b64, 20, g_h264_ltr_fnv, 24, NULL, 0);
      decode_stream("h264_cqm", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_cqm_b64, 17, g_h264_cqm_fnv, 9, NULL, 2);
      decode_stream("mpeg2_ibp", DXVA_CODEC_MPEG2, &D3D11_DECODER_PROFILE_MPEG2_VLD, g_mpeg2_ibp_b64, 8, NULL, 8, g_mpeg2_ibp_means, 1);
      test_decode_then_draw();
      test_processor();
    }
    test_content_protection();
  }
  /* (as d3d11test reports: what tests/d3d12_qemu.sh and tests/wow64_qemu.sh look for) */
  if (g_failed == 0) printf("d3d11videotest: all %d checks passed\n", g_passed);
  else printf("d3d11videotest: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  return g_failed ? 1 : 42;
}
