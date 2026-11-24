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
 * (the DXVA buffers filled as ffmpeg does: dxva_streams.h; also VC-1 and MPEG-4 Part 2) and compared with ffmpeg's decode (d3d11video_data.h), the video processor (formats, scaling, rotation,
 * mirroring, the background, alpha, the target rectangle, what it refuses), and the content protection there is not. Ends with exit code 42 when every check passed.
 *   tests/emulated_dx.sh tests/windows/d3d11videotest.c */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_3.h>
#include "dxfixtures.h"
#include "dxva_streams.h"
#include "hevc10_dxva.h"
#include "hevcrext_dxva.h"
#include "hevcrext_unpack.h"

/* The HEVC range extension profiles (dxva.h's DXVA_ModeHEVC_VLD_*): the kernel's number of each (hardware::DxvaProfile, as hevcrext_dxva.h names them) and its
 * formats. */
static const struct { const GUID *profile; unsigned kernel; UINT n; DXGI_FORMAT f[2]; } g_hevc_rext_profiles[] = {
    {&DXVA_ModeHEVC_VLD_Main12, 17, 1, {DXGI_FORMAT_P016}},     {&DXVA_ModeHEVC_VLD_Main10_422, 18, 2, {DXGI_FORMAT_Y210, DXGI_FORMAT_YUY2}},
    {&DXVA_ModeHEVC_VLD_Main12_422, 19, 1, {DXGI_FORMAT_Y216}}, {&DXVA_ModeHEVC_VLD_Main_444, 20, 1, {DXGI_FORMAT_AYUV}},
    {&DXVA_ModeHEVC_VLD_Main10_Ext, 21, 1, {DXGI_FORMAT_P010}}, {&DXVA_ModeHEVC_VLD_Main10_444, 22, 1, {DXGI_FORMAT_Y410}},
    {&DXVA_ModeHEVC_VLD_Main12_444, 23, 1, {DXGI_FORMAT_Y416}}, {&DXVA_ModeHEVC_VLD_Main16, 24, 1, {DXGI_FORMAT_Y416}},
    {&DXVA_ModeHEVC_VLD_Monochrome, 25, 1, {DXGI_FORMAT_NV12}}, {&DXVA_ModeHEVC_VLD_Monochrome10, 26, 1, {DXGI_FORMAT_P010}}};
#include "d3d11video_data.h"
#include "av1_dxva_data.h"

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
      {64, 32, 1, 2, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0, E_INVALIDARG, "a dynamic NV12 array"},
      {64, 32, 1, 2, 1, DXGI_FORMAT_P010, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE, 0, E_INVALIDARG, "a staging P010 array"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED, S_OK, "a shared NV12 texture"},
      {64, 32, 1, 4, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, S_OK,
       "a shared (NT handle) NV12 array for a decoder"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_P010, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, S_OK, "a P010 texture with a keyed mutex"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE, E_INVALIDARG, "an NV12 cube"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS, E_INVALIDARG, "NV12 mipmaps generated"},
      {64, 32, 1, 1, 1, DXGI_FORMAT_P016, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, 0, S_OK, "a P016 texture for a decoder (AV1's 12-bit 4:2:0)"},
      {64, 32, 1, 4, 1, DXGI_FORMAT_Y216, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, 0, S_OK, "a Y216 array for a decoder (AV1's 12-bit 4:2:2)"},
      {63, 32, 1, 1, 1, DXGI_FORMAT_Y210, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, 0, E_INVALIDARG, "a Y210 texture of odd width"},
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
  check(hr == S_OK && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) && (support & D3D11_FORMAT_SUPPORT_DECODER_OUTPUT), "CheckFormatSupport(P016): a texture, a decoder's output (AV1's 12-bit 4:2:0)");
  hr = ID3D11Device_CheckFormatSupport(g_device, DXGI_FORMAT_B8G8R8A8_UNORM, &support);
  check(hr == S_OK && (support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT) && (support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT), "CheckFormatSupport(BGRA8): the video processor's");
  D3D11_FEATURE_DATA_FORMAT_SUPPORT fs = {DXGI_FORMAT_P010, 0};
  hr = ID3D11Device_CheckFeatureSupport(g_device, D3D11_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs);
  check(hr == S_OK && (fs.OutFormatSupport & D3D11_FORMAT_SUPPORT_DECODER_OUTPUT), "CheckFeatureSupport(FORMAT_SUPPORT, P010)");
  D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2 = {DXGI_FORMAT_NV12, 0xFFFFFFFFu};
  hr = ID3D11Device_CheckFeatureSupport(g_device, D3D11_FEATURE_FORMAT_SUPPORT2, &fs2, sizeof fs2);
  /* (the planes take typed unordered access views - written back through mirrors - and nothing else of the second set: no atomics, not tiled - video formats never are) */
  check(hr == S_OK && fs2.OutFormatSupport2 == (D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE),
        "CheckFeatureSupport(FORMAT_SUPPORT2, NV12): typed loads and stores of its planes, nothing else of the second set");
  D3D11_FEATURE_DATA_D3D11_OPTIONS4 o4 = {TRUE};
  hr = ID3D11Device_CheckFeatureSupport(g_device, D3D11_FEATURE_D3D11_OPTIONS4, &o4, sizeof o4);
  check(hr == S_OK && o4.ExtendedNV12SharedTextureSupported, "D3D11_OPTIONS4: NV12 textures are shared");
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

/* Views of several slices of a plane (a Texture2DArray shader reads each slice) and integer views of a plane (R8_UINT / R8G8_UINT read the bytes as integers; an R8_UINT
 * render target writes them), on an NV12 array of three slices. */
static UINT32 draw_with(ID3D11VertexShader *vs, ID3D11PixelShader *ps, ID3D11ShaderResourceView *view, const float colour[4]) {
  const float blue[4] = {0, 0, 1, 1};
  const D3D11_VIEWPORT vp = {0, 0, 64, 64, 0, 1};
  const UINT stride = 28, offset = 0;
  const float v[21] = {-1, -1, 0.5f, colour[0], colour[1], colour[2], colour[3], 3, -1, 0.5f, colour[0], colour[1], colour[2], colour[3],
                       -1, 3, 0.5f, colour[0], colour[1], colour[2], colour[3]};
  const D3D11_BUFFER_DESC bd = {sizeof v, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  const D3D11_SUBRESOURCE_DATA init = {v, 0, 0};
  ID3D11Buffer *vb = NULL;
  if (FAILED(ID3D11Device_CreateBuffer(g_device, &bd, &init, &vb))) return 0;
  ID3D11DeviceContext_ClearRenderTargetView(g_context, g_target_view, blue);
  ID3D11DeviceContext_OMSetRenderTargets(g_context, 1, &g_target_view, NULL);
  ID3D11DeviceContext_RSSetViewports(g_context, 1, &vp);
  ID3D11DeviceContext_RSSetState(g_context, g_no_cull);
  ID3D11DeviceContext_IASetInputLayout(g_context, g_layout);
  ID3D11DeviceContext_IASetVertexBuffers(g_context, 0, 1, &vb, &stride, &offset);
  ID3D11DeviceContext_IASetPrimitiveTopology(g_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11DeviceContext_VSSetShader(g_context, vs, NULL, 0);
  ID3D11DeviceContext_VSSetConstantBuffers(g_context, 0, 1, &g_matrix);
  ID3D11DeviceContext_PSSetShader(g_context, ps, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &view);
  ID3D11DeviceContext_PSSetSamplers(g_context, 0, 1, &g_point);
  ID3D11DeviceContext_Draw(g_context, 3, 0);
  ID3D11ShaderResourceView *none = NULL;
  ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &none);
  ID3D11Buffer_Release(vb);
  static UINT32 pixels[64 * 64];
  return read_bgra(g_target, pixels) ? pixels[3 * 64 + 3] : 0;
}
static void test_plane_array_and_integer_views(void) {
  const UINT w = 16, h = 8;
  ID3D11Texture2D *t = texture(w, h, 3, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
  ID3D11VertexShader *vs_vol = NULL;
  ID3D11PixelShader *ps_array = NULL, *ps_load = NULL, *ps_write = NULL;
  HRESULT hr = ID3D11Device_CreateVertexShader(g_device, dx_vs_vol_dxbc, sizeof dx_vs_vol_dxbc, NULL, &vs_vol);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_texarray_dxbc, sizeof dx_ps_texarray_dxbc, NULL, &ps_array);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_int_load_dxbc, sizeof dx_ps_int_load_dxbc, NULL, &ps_load);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_int_write_dxbc, sizeof dx_ps_int_write_dxbc, NULL, &ps_write);
  check(SUCCEEDED(hr) && t, "an NV12 array of three slices to sample and draw into, and the shaders that read an array and integers");
  if (FAILED(hr) || !t) goto done;
  /* (slice s: Y = 30 + 70 s, U = 20 + s, V = 220 - s) */
  BYTE frame[16 * 12];
  for (UINT s = 0; s < 3; ++s) {
    for (UINT i = 0; i < w * h; ++i) frame[i] = (BYTE)(30 + 70 * s);
    for (UINT i = 0; i < w * h / 2; i += 2) { frame[w * h + i] = (BYTE)(20 + s); frame[w * h + i + 1] = (BYTE)(220 - s); }
    ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)t, s, NULL, frame, w, 0);
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC sd;
  memset(&sd, 0, sizeof sd);
  sd.Format = DXGI_FORMAT_R8_UNORM;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  sd.Texture2DArray.MipLevels = 1;
  sd.Texture2DArray.ArraySize = 3;
  ID3D11ShaderResourceView *luma = NULL, *chroma = NULL, *luma_int = NULL, *chroma_int = NULL;
  hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &luma);
  sd.Format = DXGI_FORMAT_R8G8_UNORM;
  sd.Texture2DArray.FirstArraySlice = 1;
  sd.Texture2DArray.ArraySize = 2;
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &chroma);
  sd.Format = DXGI_FORMAT_R8_UINT;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &luma_int);
  sd.Format = DXGI_FORMAT_R8G8_UINT;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  sd.Texture2DArray.MipLevels = 1;
  sd.Texture2DArray.FirstArraySlice = 2;
  sd.Texture2DArray.ArraySize = 1;
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &chroma_int);
  check(SUCCEEDED(hr), "shader resource views of NV12: arrays of R8_UNORM and R8G8_UNORM, R8_UINT and R8G8_UINT");
  int luma_ok = 1, chroma_ok = 1;
  for (UINT s = 0; luma && chroma && s < 3; ++s) {
    const float at[4] = {0.5f, 0.5f, (float)s, 1};
    UINT32 px = draw_with(vs_vol, ps_array, luma, at);
    luma_ok &= close_to(red(px), 30 + 70 * (int)s, 1) && green(px) == 0;
    if (s < 2) {
      px = draw_with(vs_vol, ps_array, chroma, at);
      chroma_ok &= close_to(red(px), 21 + (int)s, 1) && close_to(green(px), 219 - (int)s, 1) && blue(px) == 0;
    }
  }
  checkf(luma && luma_ok, "a Texture2DArray shader reads each of the three slices of the luma (an array view of R8_UNORM)");
  checkf(chroma && chroma_ok, "... and slices 1 and 2 of the chroma (an array view of R8G8_UNORM from slice 1)");
  const float any[4] = {0, 0, 0, 1};
  UINT32 px = luma_int ? draw_with(g_vs, ps_load, luma_int, any) : 0;
  checkf(luma_int && red(px) == 30 && green(px) == 0 && blue(px) == 0, "an R8_UINT view of the luma reads its bytes as integers: (%d, %d, %d), want (30, 0, 0)", red(px), green(px), blue(px));
  px = chroma_int ? draw_with(g_vs, ps_load, chroma_int, any) : 0;
  checkf(chroma_int && red(px) == 22 && green(px) == 218 && blue(px) == 0, "an R8G8_UINT view of slice 2's chroma: (%d, %d, %d), want (22, 218, 0)", red(px), green(px), blue(px));
  /* an R8_UINT render target of slice 1's luma, drawn into by a shader that writes integers */
  D3D11_RENDER_TARGET_VIEW_DESC rd;
  memset(&rd, 0, sizeof rd);
  rd.Format = DXGI_FORMAT_R8_UINT;
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
  rd.Texture2DArray.FirstArraySlice = 1;
  rd.Texture2DArray.ArraySize = 1;
  ID3D11RenderTargetView *target = NULL;
  hr = ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)t, &rd, &target);
  check(SUCCEEDED(hr) && target, "an R8_UINT render target view of slice 1's luma");
  if (target) {
    const float c[4] = {0.4f, 0.1f, 0.1f, 1};      /* (uint4(c * 255 + 0.5): 102) */
    const float v[21] = {-1, -1, 0.5f, c[0], c[1], c[2], c[3], 3, -1, 0.5f, c[0], c[1], c[2], c[3], -1, 3, 0.5f, c[0], c[1], c[2], c[3]};
    const D3D11_BUFFER_DESC bd = {sizeof v, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA init = {v, 0, 0};
    ID3D11Buffer *vb = NULL;
    if (SUCCEEDED(ID3D11Device_CreateBuffer(g_device, &bd, &init, &vb))) {
      const D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
      const UINT stride = 28, offset = 0;
      ID3D11DeviceContext_OMSetRenderTargets(g_context, 1, &target, NULL);
      ID3D11DeviceContext_RSSetViewports(g_context, 1, &vp);
      ID3D11DeviceContext_IASetVertexBuffers(g_context, 0, 1, &vb, &stride, &offset);
      ID3D11DeviceContext_VSSetShader(g_context, g_vs, NULL, 0);
      ID3D11DeviceContext_PSSetShader(g_context, ps_write, NULL, 0);
      ID3D11DeviceContext_Draw(g_context, 3, 0);
      ID3D11DeviceContext_OMSetRenderTargets(g_context, 1, &g_target_view, NULL);
      ID3D11Buffer_Release(vb);
    }
    BYTE back[16 * 12];
    int ok = read_planar(t, 1, back), y_ok = 1;
    for (UINT i = 0; ok && i < w * h; ++i) y_ok &= back[i] == 102;
    const int uv_kept = ok && back[w * h] == 21 && back[w * h + 1] == 219;
    checkf(ok && y_ok && uv_kept, "the integer target wrote slice 1's luma: %d (want 102), its chroma kept: %d %d", back[0], back[w * h], back[w * h + 1]);
    ok = read_planar(t, 0, back);
    checkf(ok && back[0] == 30, "... and slice 0 is as it was: %d", back[0]);
    ID3D11RenderTargetView_Release(target);
  }
  /* (an unordered access view of an integer form is refused: there are no typed stores to textures) */
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
  memset(&ud, 0, sizeof ud);
  ud.Format = DXGI_FORMAT_R8_UINT;
  ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
  ID3D11UnorderedAccessView *uav = NULL;
  check(ID3D11Device_CreateUnorderedAccessView(g_device, (ID3D11Resource *)t, &ud, &uav) == E_INVALIDARG && !uav, "an R8_UINT unordered access view of NV12: E_INVALIDARG");
  if (luma) ID3D11ShaderResourceView_Release(luma);
  if (chroma) ID3D11ShaderResourceView_Release(chroma);
  if (luma_int) ID3D11ShaderResourceView_Release(luma_int);
  if (chroma_int) ID3D11ShaderResourceView_Release(chroma_int);
done:
  if (vs_vol) ID3D11VertexShader_Release(vs_vol);
  if (ps_array) ID3D11PixelShader_Release(ps_array);
  if (ps_load) ID3D11PixelShader_Release(ps_load);
  if (ps_write) ID3D11PixelShader_Release(ps_write);
  if (t) ID3D11Texture2D_Release(t);
}

/* ---- the decoder: profiles, formats, configurations ------------------------------------------------------------------------------------------------------------- */

static void test_decoder_caps(void) {
  const UINT count = ID3D11VideoDevice_GetVideoDecoderProfileCount(g_vdevice);
  int h264 = 0, mpeg2 = 0, hevc = 0, vp9 = 0, av1 = 0, vc1 = 0, mpeg4 = 0, av1_more = 0, hevc10 = 0, hevc_rext = 0;
  for (UINT i = 0; i < count; ++i) {
    GUID g;
    if (FAILED(ID3D11VideoDevice_GetVideoDecoderProfile(g_vdevice, i, &g))) continue;
    for (UINT k = 0; k < sizeof g_hevc_rext_profiles / sizeof g_hevc_rext_profiles[0]; ++k) hevc_rext += IsEqualGUID(&g, g_hevc_rext_profiles[k].profile);
    h264 |= IsEqualGUID(&g, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT);
    mpeg2 |= IsEqualGUID(&g, &D3D11_DECODER_PROFILE_MPEG2_VLD);
    hevc |= IsEqualGUID(&g, &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN);
    vp9 |= IsEqualGUID(&g, &D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0);
    av1 |= IsEqualGUID(&g, &DXVA_ModeAV1_VLD_Profile0);
    hevc10 |= IsEqualGUID(&g, &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10);
    vc1 += IsEqualGUID(&g, &D3D11_DECODER_PROFILE_VC1_VLD) + IsEqualGUID(&g, &D3D11_DECODER_PROFILE_VC1_D2010);
    mpeg4 += IsEqualGUID(&g, &D3D11_DECODER_PROFILE_MPEG4PT2_VLD_SIMPLE) + IsEqualGUID(&g, &D3D11_DECODER_PROFILE_MPEG4PT2_VLD_ADVSIMPLE_NOGMC) +
             IsEqualGUID(&g, &D3D11_DECODER_PROFILE_MPEG4PT2_VLD_ADVSIMPLE_GMC);
    av1_more += IsEqualGUID(&g, &DXVA_ModeAV1_VLD_Profile1) + IsEqualGUID(&g, &DXVA_ModeAV1_VLD_Profile2) + IsEqualGUID(&g, &DXVA_ModeAV1_VLD_12bit_Profile2) +
                IsEqualGUID(&g, &DXVA_ModeAV1_VLD_12bit_Profile2_420);
  }
  checkf(count == 27 && h264 && mpeg2 && hevc && hevc10 && vp9 && av1 && vc1 == 2 && mpeg4 == 3 && av1_more == 4 && hevc_rext == 10,
         "GetVideoDecoderProfileCount %u: H.264, MPEG-2, MPEG-1/2, VP9 (two), HEVC Main and Main 10 and the ten range extension profiles, AV1 (five), VC-1 (two), "
         "MPEG-4 Part 2 (three)", count);
  {
    /* each HEVC range extension profile decodes into its formats and not into one of another chroma layout */
    int formats_ok = 1;
    for (UINT k = 0; k < sizeof g_hevc_rext_profiles / sizeof g_hevc_rext_profiles[0]; ++k) {
      for (UINT i = 0; i < g_hevc_rext_profiles[k].n; ++i) {
        BOOL f = FALSE;
        formats_ok &= SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, g_hevc_rext_profiles[k].profile, g_hevc_rext_profiles[k].f[i], &f)) && f;
      }
      BOOL other = TRUE;
      const DXGI_FORMAT not_it = g_hevc_rext_profiles[k].f[0] == DXGI_FORMAT_Y216 ? DXGI_FORMAT_Y416 : DXGI_FORMAT_Y216;
      formats_ok &= SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, g_hevc_rext_profiles[k].profile, not_it, &other)) && !other;
    }
    check(formats_ok, "CheckVideoDecoderFormat of the HEVC range extension profiles: their formats and no other");
  }
  {
    BOOL f1 = FALSE, f2 = TRUE, f3 = FALSE, f4 = FALSE;
    check(SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, &DXVA_ModeAV1_VLD_Profile1, DXGI_FORMAT_Y410, &f1)) && f1 &&
              SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, &DXVA_ModeAV1_VLD_Profile1, DXGI_FORMAT_NV12, &f2)) && !f2 &&
              SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, &DXVA_ModeAV1_VLD_12bit_Profile2, DXGI_FORMAT_Y216, &f3)) && f3 &&
              SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, &DXVA_ModeAV1_VLD_12bit_Profile2_420, DXGI_FORMAT_P016, &f4)) && f4,
          "AV1 profile 1 decodes into Y410 (not NV12), 12-bit profile 2 into Y216, 12-bit profile 2 4:2:0 into P016");
    ID3D11VideoDevice1 *vd1 = NULL;
    if (SUCCEEDED(ID3D11VideoDevice_QueryInterface(g_vdevice, &IID_ID3D11VideoDevice1, (void **)&vd1))) {
      UINT caps = 1;
      const DXGI_RATIONAL rate = {30, 1};
      check(SUCCEEDED(ID3D11VideoDevice1_GetVideoDecoderCaps(vd1, &DXVA_ModeAV1_VLD_Profile2, 1920, 1080, &rate, 0, NULL, &caps)) &&
                caps == (D3D11_VIDEO_DECODER_CAPS_DOWNSAMPLE | D3D11_VIDEO_DECODER_CAPS_DOWNSAMPLE_DYNAMIC),
            "GetVideoDecoderCaps of AV1 profile 2: decoded (into YUY2 / Y210 / NV12 / P010), downsampled too");
      ID3D11VideoDevice1_Release(vd1);
    }
  }
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
  D3D11_VIDEO_DECODER_DESC vc1_desc = {D3D11_DECODER_PROFILE_VC1_D2010, 720, 576, DXGI_FORMAT_NV12};
  check(SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderConfigCount(g_vdevice, &vc1_desc, &configs)) && configs == 1 &&
            SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderConfig(g_vdevice, &vc1_desc, 0, &c)) && c.ConfigBitstreamRaw == 1,
        "the VC-1 configuration: one, ConfigBitstreamRaw 1 (what ffmpeg's dxva2_vc1.c takes)");
  D3D11_VIDEO_DECODER_DESC mpeg4_desc = {D3D11_DECODER_PROFILE_MPEG4PT2_VLD_ADVSIMPLE_GMC, 352, 288, DXGI_FORMAT_NV12};
  check(SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderConfigCount(g_vdevice, &mpeg4_desc, &configs)) && configs == 1 &&
            SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderConfig(g_vdevice, &mpeg4_desc, 0, &c)) && c.ConfigBitstreamRaw == 1,
        "the MPEG-4 Part 2 (GMC) configuration: one, ConfigBitstreamRaw 1");
  check(SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, &D3D11_DECODER_PROFILE_VC1_VLD, DXGI_FORMAT_P010, &ok)) && !ok, "VC-1 does not decode into P010");
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
      const UINT offsets[4] = {100, 0, 0, 0};
      const D3D11_BUFFER_DESC hd = {4096, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
      ID3D11Buffer *histogram = NULL, *histograms[4] = {NULL, NULL, NULL, NULL};
      ID3D11Device_CreateBuffer(g_device, &hd, NULL, &histogram);
      histograms[0] = histogram;
      check(((DecoderBeginFrame1Fn)vtable_entry(c3, VIDEO_CONTEXT2_METHODS))(c3, dec, view, 0, NULL, 1, offsets, histograms) == E_INVALIDARG,
            "DecoderBeginFrame1 with a histogram at an offset not 256-aligned: E_INVALIDARG");
      check(((DecoderBeginFrame1Fn)vtable_entry(c3, VIDEO_CONTEXT2_METHODS))(c3, dec, view, 0, NULL, 5, offsets, histograms) == E_INVALIDARG,
            "... with five components: E_INVALIDARG");
      const UINT alpha_offsets[4] = {0, 0, 0, 256};
      histograms[0] = NULL;
      histograms[3] = histogram;
      check(((DecoderBeginFrame1Fn)vtable_entry(c3, VIDEO_CONTEXT2_METHODS))(c3, dec, view, 0, NULL, 4, alpha_offsets, histograms) == E_INVALIDARG,
            "... of alpha, which NV12 has not got: E_INVALIDARG");
      if (histogram) ID3D11Buffer_Release(histogram);
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
    check(SUCCEEDED(ID3D11VideoDevice1_GetVideoDecoderCaps(v1, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 480, 320, &rate, 0, NULL, &caps)) &&
              caps == (D3D11_VIDEO_DECODER_CAPS_DOWNSAMPLE | D3D11_VIDEO_DECODER_CAPS_DOWNSAMPLE_DYNAMIC),
          "GetVideoDecoderCaps: downsampling, also changed while decoding");
    check(SUCCEEDED(ID3D11VideoDevice1_GetVideoDecoderCaps(v1, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 480, 320, &rate, 0, &D3D11_CRYPTO_TYPE_AES128_CTR_, &caps)) &&
              caps == D3D11_VIDEO_DECODER_CAPS_UNSUPPORTED,
          "... encrypted: UNSUPPORTED");
    BOOL supported = TRUE, real_time = TRUE;
    D3D11_VIDEO_SAMPLE_DESC sample = {240, 160, DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709};
    video_config_of(&config, 2);
    check(SUCCEEDED(ID3D11VideoDevice1_CheckVideoDecoderDownsampling(v1, &desc, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, &config, &rate, &sample, &supported, &real_time)) &&
              supported && real_time,
          "CheckVideoDecoderDownsampling: NV12 BT.709 to half size, in real time");
    D3D11_VIDEO_SAMPLE_DESC rgb = {240, 160, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    check(SUCCEEDED(ID3D11VideoDevice1_CheckVideoDecoderDownsampling(v1, &desc, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, &config, &rate, &rgb, &supported, &real_time)) && !supported,
          "... not to RGB (decoder output views are of YUV formats)");
    D3D11_VIDEO_SAMPLE_DESC recommended;
    check(SUCCEEDED(ID3D11VideoDevice1_RecommendVideoDecoderDownsampleParameters(v1, &desc, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, &config, &rate, &recommended)) &&
              recommended.Width == desc.SampleWidth && recommended.Height == desc.SampleHeight && recommended.Format == DXGI_FORMAT_NV12 &&
              recommended.ColorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709,
          "RecommendVideoDecoderDownsampleParameters: the decoder's own size and format");
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
    check(SUCCEEDED(f(v2, 0, &h, sizeof h)) && h.components == 7 && h.bins == 64 && h.bits == 32,
          "ID3D11VideoDevice2::CheckFeatureSupport(DECODER_HISTOGRAM): Y, U and V, 64 bins of 32-bit counters");
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
/* A stream decoded into an NV12 array of `surfaces` slices (the DXVA indices) - or, `separate`, into that many NV12 textures of one slice each (index n the n-th
 * texture, every output view of slice 0) -; each picture read back as it is decoded; then in display order against ffmpeg's frames: their hashes (H.264), or the means
 * of their 4 x 4 blocks within 2 (MPEG-2). */
static void decode_stream(const char *name, int codec, const GUID *profile, const char *b64, unsigned surfaces, const unsigned *hashes, UINT frames,
                          const unsigned char (*means)[576], int split, int separate) {
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
  ID3D11Texture2D *outs[DXVA_STREAM_MAX_SURFACES] = {0};
  ID3D11Texture2D *out = texture(s.width, s.height, separate ? 1 : surfaces, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, NULL, NULL);
  for (unsigned i = 0; i < surfaces; ++i) outs[i] = !separate ? out : i == 0 ? out : texture(s.width, s.height, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, NULL, NULL);
  ID3D11VideoDecoderOutputView *views[DXVA_STREAM_MAX_SURFACES] = {0};
  for (unsigned i = 0; SUCCEEDED(hr) && out && i < surfaces; ++i) {
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {*profile, D3D11_VDOV_DIMENSION_TEXTURE2D, {{separate ? 0 : i}}};
    hr = outs[i] ? ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)outs[i], &vd, &views[i]) : E_OUTOFMEMORY;
  }
  checkf(SUCCEEDED(hr) && dec && out, separate ? "%s: a decoder of %u x %u, %u NV12 textures of one slice and their output views" : "%s: a decoder of %u x %u, an NV12 array of %u slices and their output views",
         name, s.width, s.height, surfaces);
  const UINT vw = s.width - s.crop_left - s.crop_right, vh = s.height - s.crop_top - s.crop_bottom, cw = (vw + 1) / 2, ch = (vh + 1) / 2;
  const UINT frame_bytes = vw * vh + 2 * cw * ch;
  Decoded *decoded = calloc(256, sizeof *decoded);
  BYTE *nv12 = malloc((size_t)s.width * s.height * 3 / 2);
  UINT n = 0, pictures = 0, failed = 0, last_feedback = 0, last_surface = 0;
  int r = 1;
  while (SUCCEEDED(hr) && dec && out && r == 1 && n < 256) {
    if (codec == DXVA_CODEC_H264) last_feedback = ((const DXVA_PicParams_H264 *)p.picture_parameters)->StatusReportFeedbackNumber;
    if (codec == DXVA_CODEC_VC1) {
      const DXVA_PictureParameters *pp = p.picture_parameters;
      last_feedback = (UINT)pp->bPicScanFixed << 8 | pp->bPicScanMethod;      /* (VC-1's feedback number) */
    }
    if (codec == DXVA_CODEC_MPEG4) last_feedback = ((const DxvaPicParamsMpeg4 *)p.picture_parameters)->StatusReportFeedbackNumber;
    last_surface = p.surface;
    const UINT slice_bytes = codec == DXVA_CODEC_H264 ? sizeof(DXVA_Slice_H264_Short) : sizeof(DXVA_SliceInfo);
    const UINT location = codec == DXVA_CODEC_H264 ? offsetof(DXVA_Slice_H264_Short, BSNALunitDataLocation) : offsetof(DXVA_SliceInfo, dwSliceDataLocation);
    if (!decode_picture(dec, views[p.surface], &p, slice_bytes, location, split)) { ++failed; break; }
    /* (an H.264 first field: the frame is whole after the second field, which goes into the same surface next) */
    if (codec == DXVA_CODEC_H264 && p.field == 1) {
      ++pictures;
      r = dxva_stream_next(&s, &p);
      checkf(r != 1 || p.field == 2, "%s: a first field is followed by its second", name);
      continue;
    }
    if (!read_planar(outs[p.surface], separate ? 0 : p.surface, nv12)) { ++failed; break; }
    ++pictures;
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
  checkf(!failed && r == 0, "%s: %u pictures decoded through ID3D11VideoContext%s%s (%s)", name, pictures,
         split == 2 ? "3 (DecoderBeginFrame1, SubmitDecoderBuffers2)" : split ? ", the slices of a picture in two SubmitDecoderBuffers calls" : "",
         separate ? ", each surface index a texture of its own" : "",
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
  if (dec && (codec == DXVA_CODEC_VC1 || codec == DXVA_CODEC_MPEG4)) {
    /* (DXVA_Status_VC1 of the Windows SDK; MPEG-4 Part 2 reports in the same form) */
#pragma pack(push, 1)
    typedef struct { USHORT StatusReportFeedbackNumber; WORD wDecodedPictureIndex, wDeblockedPictureIndex; UCHAR bPicStructure, bBufType, bStatus, bReserved8Bits; USHORT wNumMbsAffected; } StatusVc1;
#pragma pack(pop)
    StatusVc1 st[2];
    memset(st, 0xCC, sizeof st);
    D3D11_VIDEO_DECODER_EXTENSION ext;
    memset(&ext, 0, sizeof ext);
    ext.Function = DXVA_STATUS_REPORTING_FUNCTION;
    ext.pPrivateOutputData = st;
    ext.PrivateOutputDataSize = sizeof st;
    hr = ID3D11VideoContext_DecoderExtension(g_vcontext, dec, &ext);
    checkf(hr == S_OK && st[0].StatusReportFeedbackNumber == last_feedback && st[1].StatusReportFeedbackNumber == last_feedback - 1 && st[0].bStatus == 0 &&
               st[0].wDecodedPictureIndex == last_surface && st[0].bPicStructure == 3 && st[0].bBufType == 0xFF && st[0].wNumMbsAffected == 0,
           "%s: the status reports (DXVA_Status_VC1), the newest first: feedback %u and %u, picture %u, status %u", name, st[0].StatusReportFeedbackNumber,
           st[1].StatusReportFeedbackNumber, st[0].wDecodedPictureIndex, st[0].bStatus);
  }
  for (UINT i = 0; i < n; ++i) free(decoded[i].frame);
  free(decoded);
  free(nv12);
  for (unsigned i = 0; i < surfaces; ++i) if (views[i]) ID3D11VideoDecoderOutputView_Release(views[i]);
  for (unsigned i = 1; separate && i < surfaces; ++i) if (outs[i]) ID3D11Texture2D_Release(outs[i]);
  if (out) ID3D11Texture2D_Release(out);
  if (dec) ID3D11VideoDecoder_Release(dec);
  dxva_stream_close(&s);
  free(data);
}

/* ---- packed YUV textures (AYUV, Y410, Y416, YUY2, Y210, Y216) -------------------------------------------------------------------------------------------------------- */

/* A slice of a packed YUV texture as the CPU sees it (`row` bytes a row), by way of a staging texture of one slice. */
static int read_packed(ID3D11Texture2D *t, UINT slice, UINT row, BYTE *out) {
  D3D11_TEXTURE2D_DESC desc;
  ID3D11Texture2D_GetDesc(t, &desc);
  ID3D11Texture2D *staging = texture(desc.Width, desc.Height, 1, desc.Format, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL, NULL);
  if (!staging) return 0;
  ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)staging, 0, 0, 0, 0, (ID3D11Resource *)t, slice, NULL);
  D3D11_MAPPED_SUBRESOURCE m;
  int ok = SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m)) && m.pData && m.RowPitch >= row;
  if (ok) {
    for (UINT y = 0; y < desc.Height; ++y) memcpy(out + (size_t)y * row, (BYTE *)m.pData + (size_t)y * m.RowPitch, row);
    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
  }
  ID3D11Texture2D_Release(staging);
  return ok;
}

static void test_packed_yuv(void) {
  static const struct { DXGI_FORMAT f; const char *name; UINT bpp; DXGI_FORMAT view; int target; } formats[] = {
      {DXGI_FORMAT_AYUV, "AYUV", 4, DXGI_FORMAT_R8G8B8A8_UNORM, 1}, {DXGI_FORMAT_Y410, "Y410", 4, DXGI_FORMAT_R10G10B10A2_UNORM, 0},
      {DXGI_FORMAT_Y416, "Y416", 8, DXGI_FORMAT_R16G16B16A16_UNORM, 0}, {DXGI_FORMAT_YUY2, "YUY2", 2, DXGI_FORMAT_R8G8B8A8_UNORM, 0},
      {DXGI_FORMAT_Y210, "Y210", 4, DXGI_FORMAT_R16G16B16A16_UNORM, 0}, {DXGI_FORMAT_Y216, "Y216", 4, DXGI_FORMAT_R16G16B16A16_UNORM, 0}};
  const UINT w = 16, h = 8;
  for (UINT i = 0; i < sizeof formats / sizeof formats[0]; ++i) {
    const DXGI_FORMAT f = formats[i].f;
    const char *name = formats[i].name;
    UINT support = 0;
    HRESULT hr = ID3D11Device_CheckFormatSupport(g_device, f, &support);
    checkf(hr == S_OK && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) && (support & D3D11_FORMAT_SUPPORT_DECODER_OUTPUT) &&
               (support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) && !!(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) == formats[i].target &&
               !!(support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT) == formats[i].target && !!(support & D3D11_FORMAT_SUPPORT_MIP) == (f == DXGI_FORMAT_AYUV),
           "CheckFormatSupport(%s): a 2D texture, sampled, a decoder's output, the processor's input%s", name, formats[i].target ? " and output, a render target" : "");
    /* made with its contents, read back byte for byte */
    BYTE rows[16 * 8 * 8], back[16 * 8 * 8];
    const UINT row = w * formats[i].bpp;
    for (UINT k = 0; k < row * h; ++k) rows[k] = (BYTE)(k * 37 + i * 5 + 1);
    const D3D11_SUBRESOURCE_DATA initial = {rows, row, 0};
    ID3D11Texture2D *t = texture(w, h, 1, f, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | (formats[i].target ? D3D11_BIND_RENDER_TARGET : 0), 0, &initial, &hr);
    checkf(t && SUCCEEDED(hr) && read_packed(t, 0, row, back) && !memcmp(rows, back, row * h), "%s: a texture made with its pixels, read back byte for byte", name);
    if (!t) continue;
    /* UpdateSubresource of a box (on whole pairs for the 4:2:2 ones) */
    const D3D11_BOX box = {4, 2, 0, 8, 4, 1};
    BYTE patch[4 * 8 * 2];
    memset(patch, 0x5A, sizeof patch);
    ID3D11DeviceContext_UpdateSubresource(g_context, (ID3D11Resource *)t, 0, &box, patch, 4 * formats[i].bpp, 0);
    int ok = read_packed(t, 0, row, back);
    ok = ok && back[2 * row + 4 * formats[i].bpp] == 0x5A && back[3 * row + 8 * formats[i].bpp - 1] == 0x5A && back[2 * row + 4 * formats[i].bpp - 1] == rows[2 * row + 4 * formats[i].bpp - 1] &&
         back[4 * row + 4 * formats[i].bpp] == rows[4 * row + 4 * formats[i].bpp];
    checkf(ok, "%s: UpdateSubresource of a box writes that box alone", name);
    /* the views: of the pixels' format alone */
    ID3D11ShaderResourceView *srv = NULL, *bad = NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    check(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, NULL, &bad) == E_INVALIDARG, "a shader resource view of a packed YUV texture without a description: E_INVALIDARG");
    sd.Format = f;
    check(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &bad) == E_INVALIDARG, "... of the YUV format itself: E_INVALIDARG");
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    check(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &bad) == E_INVALIDARG, "... of another format: E_INVALIDARG");
    sd.Format = formats[i].view;
    hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)t, &sd, &srv);
    /* sampled: the last texel of the view (a pair of pixels for YUY2 / Y210 / Y216) as its channels */
    const BYTE *last = back + (size_t)(h - 1) * row + row - (f == DXGI_FORMAT_YUY2 || f == DXGI_FORMAT_Y210 || f == DXGI_FORMAT_Y216 ? 2 : 1) * formats[i].bpp;
    int want[3];
    for (int c = 0; c < 3; ++c) {
      if (formats[i].view == DXGI_FORMAT_R8G8B8A8_UNORM) want[c] = last[c];
      else if (formats[i].view == DXGI_FORMAT_R16G16B16A16_UNORM) want[c] = (int)lround((last[2 * c] | last[2 * c + 1] << 8) * 255.0 / 65535.0);
      else { const UINT v = (UINT)last[0] | (UINT)last[1] << 8 | (UINT)last[2] << 16 | (UINT)last[3] << 24; want[c] = (int)lround(((v >> (10 * c)) & 1023) * 255.0 / 1023.0); }
    }
    const UINT32 got = srv ? sample_view(srv) : 0;
    checkf(SUCCEEDED(hr) && srv && close_to(red(got), want[0], 1) && close_to(green(got), want[1], 1) && close_to(blue(got), want[2], 1),
           "%s sampled through a view of its pixels' format: (%d, %d, %d), want (%d, %d, %d)", name, red(got), green(got), blue(got), want[0], want[1], want[2]);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (formats[i].target) {
      /* AYUV: a render target, cleared: V, U, Y, A */
      ID3D11RenderTargetView *rtv = NULL;
      D3D11_RENDER_TARGET_VIEW_DESC rd;
      memset(&rd, 0, sizeof rd);
      rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      check(ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)t, NULL, &rtv) == E_INVALIDARG && !rtv, "a render target view of AYUV without a description: E_INVALIDARG");
      rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      hr = ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)t, &rd, &rtv);
      if (rtv) {
        const float vuya[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        ID3D11DeviceContext_ClearRenderTargetView(g_context, rtv, vuya);
        ok = read_packed(t, 0, row, back);
        checkf(ok && close_to(back[0], 64, 1) && close_to(back[1], 128, 1) && close_to(back[2], 191, 1) && back[3] == 255 && close_to(back[row * h - 2], 191, 1),
               "AYUV cleared through an R8G8B8A8_UNORM render target: (%d, %d, %d, %d)", back[0], back[1], back[2], back[3]);
        ID3D11RenderTargetView_Release(rtv);
      } else check(0, "a render target view of AYUV (R8G8B8A8_UNORM)");
    } else {
      HRESULT rh = S_OK;
      ID3D11Texture2D *rt = texture(w, h, 1, f, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, &rh);
      checkf(!rt && rh == E_INVALIDARG, "%s is no render target: E_INVALIDARG", name);
      if (rt) ID3D11Texture2D_Release(rt);
    }
    UINT vp = 0;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {30, 1}, 16, 8, {30, 1}, 16, 8, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL};
    ID3D11VideoProcessorEnumerator *e = NULL;
    if (SUCCEEDED(ID3D11VideoDevice_CreateVideoProcessorEnumerator(g_vdevice, &cd, &e))) {
      checkf(SUCCEEDED(ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(e, f, &vp)) && (vp & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) &&
                 !!(vp & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == formats[i].target,
             "CheckVideoProcessorFormat(%s): input%s", name, formats[i].target ? " and output" : " (no output: it is no render target)");
      ID3D11VideoProcessorEnumerator_Release(e);
    }
    ID3D11Texture2D_Release(t);
  }
  HRESULT hr = S_OK;
  ID3D11Texture2D *odd = texture(15, 8, 1, DXGI_FORMAT_YUY2, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, NULL, &hr);
  check(!odd && hr == E_INVALIDARG, "a YUY2 texture of odd width: E_INVALIDARG");
  /* levels: AYUV's (and Y416's, which Direct3D 12 below has not: the kernel's 16-bit images have one level), none of the others' (DXGI's tables) */
  D3D11_TEXTURE2D_DESC mips = {16, 16, 2, 1, DXGI_FORMAT_AYUV, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  ID3D11Texture2D *m = NULL;
  {
    BYTE level0[16 * 16 * 4], level1[8 * 8 * 4], back[8 * 8 * 4];
    for (UINT k = 0; k < sizeof level0; ++k) level0[k] = (BYTE)(k * 3 + 1);
    for (UINT k = 0; k < sizeof level1; ++k) level1[k] = (BYTE)(k * 11 + 5);
    const D3D11_SUBRESOURCE_DATA levels[2] = {{level0, 64, 0}, {level1, 32, 0}};
    check(SUCCEEDED(ID3D11Device_CreateTexture2D(g_device, &mips, levels, &m)) && m, "an AYUV texture of two levels");
    if (m) {
      /* (level 1 through a staging texture of 8 x 8) */
      ID3D11Texture2D *staging = texture(8, 8, 1, DXGI_FORMAT_AYUV, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL, NULL);
      int ok = staging != NULL;
      D3D11_MAPPED_SUBRESOURCE mapped;
      if (ok) {
        ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)staging, 0, 0, 0, 0, (ID3D11Resource *)m, 1, NULL);
        ok = SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData;
        for (UINT y = 0; ok && y < 8; ++y) memcpy(back + y * 32, (BYTE *)mapped.pData + (size_t)y * mapped.RowPitch, 32);
        if (ok) ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)staging, 0);
        ID3D11Texture2D_Release(staging);
      }
      check(ok && !memcmp(back, level1, sizeof level1), "... its second level is what it was made with");
      ID3D11Texture2D_Release(m);
      m = NULL;
    }
  }
  mips.Format = DXGI_FORMAT_Y410;
  check(ID3D11Device_CreateTexture2D(g_device, &mips, NULL, &m) == E_INVALIDARG && !m, "a Y410 texture of two levels: E_INVALIDARG (none in DXGI's tables)");
  mips.Format = DXGI_FORMAT_YUY2;
  check(ID3D11Device_CreateTexture2D(g_device, &mips, NULL, &m) == E_INVALIDARG && !m, "a YUY2 texture of two levels: E_INVALIDARG");
  mips.Format = DXGI_FORMAT_Y416;
  check(ID3D11Device_CreateTexture2D(g_device, &mips, NULL, &m) == DXGI_ERROR_UNSUPPORTED && !m, "a Y416 texture of two levels: DXGI_ERROR_UNSUPPORTED (no 16-bit levels below)");
  mips.Format = DXGI_FORMAT_AYUV;
  mips.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  check(ID3D11Device_CreateTexture2D(g_device, &mips, NULL, &m) == DXGI_ERROR_UNSUPPORTED && !m, "an AYUV render target of two levels: DXGI_ERROR_UNSUPPORTED");
  if (m) ID3D11Texture2D_Release(m);
  UINT support = 0;
  check(ID3D11Device_CheckFormatSupport(g_device, DXGI_FORMAT_P208, &support) == E_FAIL && !support, "CheckFormatSupport(P208): not a Direct3D 11 format, E_FAIL");
  /* an array: each slice its own */
  BYTE rows[16 * 8 * 4 * 3];
  for (UINT k = 0; k < sizeof rows; ++k) rows[k] = (BYTE)(k * 13 + 7);
  D3D11_SUBRESOURCE_DATA init[3] = {{rows, 64, 0}, {rows + 512, 64, 0}, {rows + 1024, 64, 0}};
  ID3D11Texture2D *arr = texture(16, 8, 3, DXGI_FORMAT_Y410, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DECODER, 0, init, &hr);
  BYTE back[512];
  check(arr && read_packed(arr, 2, 64, back) && !memcmp(back, rows + 1024, 512) && read_packed(arr, 0, 64, back) && !memcmp(back, rows, 512),
        "a Y410 array of three slices for a decoder, made with its pixels: each slice its own");
  if (arr) ID3D11Texture2D_Release(arr);
}

/* An AYUV array of three slices: a render target view of slice 1 (cleared, the others kept) and a Texture2DArray view of slices 1 and 2 (a Texture2DArray shader reads each). */
static void test_packed_yuv_arrays(void) {
  BYTE rows[3][16 * 8 * 4];
  for (UINT s = 0; s < 3; ++s)
    for (UINT k = 0; k < sizeof rows[s]; k += 4) { rows[s][k] = (BYTE)(40 + 60 * s); rows[s][k + 1] = (BYTE)(200 - 50 * s); rows[s][k + 2] = (BYTE)(10 + 20 * s); rows[s][k + 3] = 255; }
  D3D11_SUBRESOURCE_DATA init[3] = {{rows[0], 64, 0}, {rows[1], 64, 0}, {rows[2], 64, 0}};
  HRESULT hr = S_OK;
  ID3D11Texture2D *arr = texture(16, 8, 3, DXGI_FORMAT_AYUV, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, init, &hr);
  check(arr && SUCCEEDED(hr), "an AYUV array of three slices to sample and draw into");
  if (!arr) return;
  D3D11_RENDER_TARGET_VIEW_DESC rd;
  memset(&rd, 0, sizeof rd);
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
  rd.Texture2DArray.FirstArraySlice = 1;
  rd.Texture2DArray.ArraySize = 1;
  ID3D11RenderTargetView *rtv = NULL;
  hr = ID3D11Device_CreateRenderTargetView(g_device, (ID3D11Resource *)arr, &rd, &rtv);
  check(SUCCEEDED(hr) && rtv, "a render target view of slice 1 of the AYUV array");
  BYTE back[16 * 8 * 4];
  if (rtv) {
    const float vuya[4] = {1.0f, 0.0f, 0.5f, 1.0f};
    ID3D11DeviceContext_ClearRenderTargetView(g_context, rtv, vuya);
    int ok = read_packed(arr, 1, 64, back) && back[0] == 255 && back[1] == 0 && close_to(back[2], 128, 1) && back[3] == 255 && back[sizeof back - 4] == 255;
    checkf(ok, "slice 1 cleared through it: (%d, %d, %d, %d)", back[0], back[1], back[2], back[3]);
    check(read_packed(arr, 0, 64, back) && !memcmp(back, rows[0], sizeof back) && read_packed(arr, 2, 64, back) && !memcmp(back, rows[2], sizeof back),
          "... slices 0 and 2 kept");
    ID3D11RenderTargetView_Release(rtv);
  }
  ID3D11VertexShader *vs_vol = NULL;
  ID3D11PixelShader *ps_array = NULL;
  hr = ID3D11Device_CreateVertexShader(g_device, dx_vs_vol_dxbc, sizeof dx_vs_vol_dxbc, NULL, &vs_vol);
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_device, dx_ps_texarray_dxbc, sizeof dx_ps_texarray_dxbc, NULL, &ps_array);
  D3D11_SHADER_RESOURCE_VIEW_DESC sd;
  memset(&sd, 0, sizeof sd);
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  sd.Texture2DArray.MipLevels = 1;
  sd.Texture2DArray.FirstArraySlice = 1;
  sd.Texture2DArray.ArraySize = 2;
  ID3D11ShaderResourceView *view = NULL;
  if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)arr, &sd, &view);
  check(SUCCEEDED(hr) && view, "a Texture2DArray view of slices 1 and 2 of the AYUV array");
  if (view) {
    const float at0[4] = {0.5f, 0.5f, 0.0f, 1}, at1[4] = {0.5f, 0.5f, 1.0f, 1};
    const UINT32 p0 = draw_with(vs_vol, ps_array, view, at0), p1 = draw_with(vs_vol, ps_array, view, at1);
    checkf(red(p0) == 255 && green(p0) == 0 && close_to(blue(p0), 128, 1), "a Texture2DArray shader reads slice 1 (the clear): (%d, %d, %d)", red(p0), green(p0), blue(p0));
    checkf(red(p1) == rows[2][0] && green(p1) == rows[2][1] && blue(p1) == rows[2][2], "... and slice 2: (%d, %d, %d), want (%d, %d, %d)", red(p1), green(p1), blue(p1),
           rows[2][0], rows[2][1], rows[2][2]);
    ID3D11ShaderResourceView_Release(view);
  }
  if (vs_vol) ID3D11VertexShader_Release(vs_vol);
  if (ps_array) ID3D11PixelShader_Release(ps_array);
  ID3D11Texture2D_Release(arr);
}

/* An AV1 stream of profile 1 or 2 (av1_dxva_data.h: the DXVA buffers a program fills, picture by picture) decoded through ID3D11VideoContext into an array of
 * `format` (the indices are its slices); every picture the stream shows read back and compared with ffmpeg's (FNV hashes of the packed rows). */
static UINT le32_at(const BYTE *p) { return (UINT)p[0] | (UINT)p[1] << 8 | (UINT)p[2] << 16 | (UINT)p[3] << 24; }
static void decode_av1(const char *name, const char *b64, const GUID *profile, DXGI_FORMAT format, UINT bpp, const unsigned *hashes, unsigned frames) {
  size_t size = 0;
  BYTE *data = base64_decode(b64, &size);
  if (!data || size < 32 || memcmp(data, "AV1D", 4) != 0) { checkf(0, "%s: the DXVA buffers", name); free(data); return; }
  const UINT surfaces = le32_at(data + 4), w = le32_at(data + 8), h = le32_at(data + 12), events = le32_at(data + 28);
  BOOL fits = FALSE;
  check(SUCCEEDED(ID3D11VideoDevice_CheckVideoDecoderFormat(g_vdevice, profile, format, &fits)) && fits, "CheckVideoDecoderFormat of the AV1 profile and its format");
  const D3D11_VIDEO_DECODER_DESC desc = {*profile, w, h, format};
  D3D11_VIDEO_DECODER_CONFIG config;
  UINT count = 0;
  HRESULT hr = ID3D11VideoDevice_GetVideoDecoderConfigCount(g_vdevice, &desc, &count);
  if (SUCCEEDED(hr) && count) hr = ID3D11VideoDevice_GetVideoDecoderConfig(g_vdevice, &desc, 0, &config);
  else hr = E_FAIL;
  ID3D11VideoDecoder *dec = NULL;
  if (SUCCEEDED(hr)) hr = ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &desc, &config, &dec);
  ID3D11Texture2D *out = texture(w, h, surfaces, format, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  ID3D11VideoDecoderOutputView *views[16] = {0};
  for (UINT i = 0; SUCCEEDED(hr) && out && i < surfaces && i < 16; ++i) {
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {*profile, D3D11_VDOV_DIMENSION_TEXTURE2D, {{i}}};
    hr = ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)out, &vd, &views[i]);
  }
  checkf(SUCCEEDED(hr) && dec && out && surfaces <= 16, "%s: a decoder of %u x %u and an array of %u slices with their output views", name, w, h, surfaces);
  const UINT row = (format == DXGI_FORMAT_YUY2 || format == DXGI_FORMAT_Y210 || format == DXGI_FORMAT_Y216 ? (w + 1) / 2 * 2 : w) * bpp;
  /* (NV12, a large-scale tile list's output frame: hashed as I420, as the reference) */
  const int nv12 = format == DXGI_FORMAT_NV12;
  BYTE *picture = malloc(nv12 ? (size_t)w * h * 3 : (size_t)row * h);
  unsigned shown = 0, same = 0, failures = 0;
  size_t at = 32;
  for (UINT e = 0; SUCCEEDED(hr) && dec && out && picture && e < events; ++e) {
    if (at + 12 > size) { ++failures; break; }
    const UINT kind = le32_at(data + at), show = le32_at(data + at + 4), surface = le32_at(data + at + 8);
    at += 12;
    if (surface >= surfaces) { ++failures; break; }
    if (kind == 0) {
      if (at + 12 > size) { ++failures; break; }
      const UINT pp = le32_at(data + at), sc = le32_at(data + at + 4), bs = le32_at(data + at + 8);
      at += 12;
      if (at + pp + sc + bs > size) { ++failures; break; }
      int ok = SUCCEEDED(ID3D11VideoContext_DecoderBeginFrame(g_vcontext, dec, views[surface], 0, NULL)) &&
               fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, data + at, pp) && fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, data + at + pp, sc) &&
               fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, data + at + pp + sc, bs);
      D3D11_VIDEO_DECODER_BUFFER_DESC b[3] = {buffer_desc(D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, pp), buffer_desc(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, sc),
                                              buffer_desc(D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, bs)};
      ok = ok && SUCCEEDED(ID3D11VideoContext_SubmitDecoderBuffers(g_vcontext, dec, 3, b));
      ok = SUCCEEDED(ID3D11VideoContext_DecoderEndFrame(g_vcontext, dec)) && ok;
      if (!ok) ++failures;
      at += pp + sc + bs;
    }
    if (!show) continue;
    if (nv12) {
      if (!read_planar(out, surface, picture)) { ++failures; continue; }
      BYTE *i420 = picture + (size_t)w * h * 3 / 2;
      memcpy(i420, picture, (size_t)w * h);
      for (UINT y = 0; y < h / 2; ++y)
        for (UINT x = 0; x < w / 2; ++x) {
          i420[(size_t)w * h + (size_t)y * (w / 2) + x] = picture[(size_t)w * h + (size_t)y * w + 2 * x];
          i420[(size_t)w * h * 5 / 4 + (size_t)y * (w / 2) + x] = picture[(size_t)w * h + (size_t)y * w + 2 * x + 1];
        }
      if (shown < frames) same += fnv(i420, (size_t)w * h * 3 / 2, 0x811c9dc5u) == hashes[shown];
      ++shown;
      continue;
    }
    if (!read_packed(out, surface, row, picture)) { ++failures; continue; }
    if (shown < frames) same += fnv(picture, (size_t)row * h, 0x811c9dc5u) == hashes[shown];
    ++shown;
  }
  checkf(!failures && shown == frames && same == frames, "%s: decoded through ID3D11VideoContext, every picture shown is ffmpeg's, bit for bit (%u of %u)", name, same, frames);
  free(picture);
  for (UINT i = 0; i < 16; ++i) if (views[i]) ID3D11VideoDecoderOutputView_Release(views[i]);
  if (out) ID3D11Texture2D_Release(out);
  if (dec) ID3D11VideoDecoder_Release(dec);
  free(data);
}

/* A Main 10 stream (hevc10_dxva.h: the DXVA buffers of every picture as ffmpeg fills them) decoded into a P010 array through ID3D11VideoContext; each picture read
 * back as it is decoded and compared in display order with the reference (FNV hashes of the pictures as yuv420p10le). */
static void decode_hevc10(const Hevc10Stream *st) {
  const UINT w = st->width, h = st->height, surfaces = st->surfaces;
  const D3D11_VIDEO_DECODER_DESC desc = {D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, w, h, DXGI_FORMAT_P010};
  D3D11_VIDEO_DECODER_CONFIG config;
  UINT count = 0;
  HRESULT hr = ID3D11VideoDevice_GetVideoDecoderConfigCount(g_vdevice, &desc, &count);
  if (SUCCEEDED(hr) && count) hr = ID3D11VideoDevice_GetVideoDecoderConfig(g_vdevice, &desc, 0, &config);
  checkf(SUCCEEDED(hr) && count == 1 && config.ConfigBitstreamRaw == 1, "%s: one HEVC Main 10 configuration into P010 (ConfigBitstreamRaw 1)", st->name);
  ID3D11VideoDecoder *dec = NULL;
  if (SUCCEEDED(hr)) hr = ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &desc, &config, &dec);
  ID3D11Texture2D *out = texture(w, h, surfaces, DXGI_FORMAT_P010, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, NULL, NULL);
  ID3D11VideoDecoderOutputView *views[DXVA_STREAM_MAX_SURFACES] = {0};
  for (unsigned i = 0; SUCCEEDED(hr) && out && i < surfaces; ++i) {
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, D3D11_VDOV_DIMENSION_TEXTURE2D, {{i}}};
    hr = ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)out, &vd, &views[i]);
  }
  checkf(SUCCEEDED(hr) && dec && out, "%s: a Main 10 decoder of %u x %u, a P010 array of %u slices and their output views", st->name, w, h, surfaces);
  BYTE *p010 = malloc((size_t)w * h * 3), *f = malloc((size_t)w * h * 3);
  unsigned hashes[64] = {0}, n = 0, low_bits = 0;
  for (; SUCCEEDED(hr) && dec && out && n < st->count && n < 64; ++n) {
    const Hevc10Picture *p = &st->pictures[n];
    if (FAILED(ID3D11VideoContext_DecoderBeginFrame(g_vcontext, dec, views[p->surface], 0, NULL))) break;
    int ok = fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, st->bytes + p->pp, p->pp_size) &&
             (!p->qm_size || fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 0, st->bytes + p->qm, p->qm_size)) &&
             fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, st->bytes + p->slices, p->slices_size) &&
             fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, st->bytes + p->bits, p->bits_size);
    D3D11_VIDEO_DECODER_BUFFER_DESC b[4];
    UINT nb = 0;
    b[nb++] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, p->pp_size);
    if (p->qm_size) b[nb++] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 0, p->qm_size);
    b[nb++] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, p->slices_size);
    b[nb++] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, p->bits_size);
    ok = ok && SUCCEEDED(ID3D11VideoContext_SubmitDecoderBuffers(g_vcontext, dec, nb, b));
    ok = SUCCEEDED(ID3D11VideoContext_DecoderEndFrame(g_vcontext, dec)) && ok;
    if (!ok || !read_planar(out, p->surface, p010)) break;
    /* (luma rows of w words, then h / 2 rows of w / 2 Cb, Cr pairs: as yuv420p10le, planar) */
    BYTE *at = f;
    for (UINT i = 0; i < w * h; ++i) {
      const unsigned v = (unsigned)p010[2 * i] | ((unsigned)p010[2 * i + 1] << 8);
      low_bits |= v & 0x3F;
      *at++ = (BYTE)(v >> 6);
      *at++ = (BYTE)(v >> 14);
    }
    const BYTE *chroma = p010 + (size_t)w * h * 2;
    for (int c = 0; c < 2; ++c)
      for (UINT i = 0; i < w * h / 4; ++i) {
        const unsigned v = (unsigned)chroma[4 * i + 2 * c] | ((unsigned)chroma[4 * i + 2 * c + 1] << 8);
        low_bits |= v & 0x3F;
        *at++ = (BYTE)(v >> 6);
        *at++ = (BYTE)(v >> 14);
      }
    if (p->display < 64) hashes[p->display] = fnv(f, (size_t)w * h * 3, 0x811c9dc5u);
  }
  checkf(n == st->count && low_bits == 0, "%s: %u pictures decoded through ID3D11VideoContext into P010 (the samples in the top ten bits)", st->name, n);
  UINT same = 0, first_bad = ~0u;
  for (UINT i = 0; i < st->count && i < 64; ++i) {
    if (hashes[i] == st->fnv[i]) ++same;
    else if (first_bad == ~0u) first_bad = i;
  }
  checkf(same == st->count, "%s: in display order every frame is the reference's, bit for bit (%u of %u; the first that differs: %d)", st->name, same, st->count, (int)first_bad);
  free(p010);
  free(f);
  for (unsigned i = 0; i < surfaces; ++i) if (views[i]) ID3D11VideoDecoderOutputView_Release(views[i]);
  if (out) ID3D11Texture2D_Release(out);
  if (dec) ID3D11VideoDecoder_Release(dec);
}

/* A HEVC range extension stream (hevcrext_dxva.h: DXVA_PicParams_HEVC_RangeExt and the rest of every picture's buffers as ffmpeg fills them) decoded into an array of its
 * profile's format (Y210, AYUV, Y416) through ID3D11VideoContext; each picture read back, taken apart into its planes and compared in display order with the hash
 * every picture of the stream carries. */
static void decode_hevcrext(const HevcRextStream *st) {
  const UINT w = st->width, h = st->height, surfaces = st->surfaces;
  const GUID *profile = NULL;
  for (UINT k = 0; k < sizeof g_hevc_rext_profiles / sizeof g_hevc_rext_profiles[0]; ++k)
    if (g_hevc_rext_profiles[k].kernel == st->profile) profile = g_hevc_rext_profiles[k].profile;
  if (!profile) { checkf(0, "%s: the profile", st->name); return; }
  const DXGI_FORMAT format = (DXGI_FORMAT)st->format;
  const D3D11_VIDEO_DECODER_DESC desc = {*profile, w, h, format};
  D3D11_VIDEO_DECODER_CONFIG config;
  UINT count = 0;
  HRESULT hr = ID3D11VideoDevice_GetVideoDecoderConfigCount(g_vdevice, &desc, &count);
  if (SUCCEEDED(hr) && count) hr = ID3D11VideoDevice_GetVideoDecoderConfig(g_vdevice, &desc, 0, &config);
  checkf(SUCCEEDED(hr) && count == 1 && config.ConfigBitstreamRaw == 1, "%s: one HEVC range extension configuration (ConfigBitstreamRaw 1)", st->name);
  ID3D11VideoDecoder *dec = NULL;
  if (SUCCEEDED(hr)) hr = ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &desc, &config, &dec);
  ID3D11Texture2D *out = texture(w, h, surfaces, format, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER, 0, NULL, NULL);
  ID3D11VideoDecoderOutputView *views[DXVA_STREAM_MAX_SURFACES] = {0};
  for (unsigned i = 0; SUCCEEDED(hr) && out && i < surfaces; ++i) {
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {*profile, D3D11_VDOV_DIMENSION_TEXTURE2D, {{i}}};
    hr = ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)out, &vd, &views[i]);
  }
  checkf(SUCCEEDED(hr) && dec && out, "%s: a decoder of %u x %u, an array of %u slices and their output views", st->name, w, h, surfaces);
  const int planar = format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010 || format == DXGI_FORMAT_P016;
  const UINT bpp = format == DXGI_FORMAT_Y416 ? 8 : format == DXGI_FORMAT_YUY2 ? 2 : 4;
  const UINT row = (format == DXGI_FORMAT_YUY2 || format == DXGI_FORMAT_Y210 || format == DXGI_FORMAT_Y216 ? (w + 1) / 2 * 2 : w) * bpp;
  BYTE *surface = malloc((size_t)w * h * 8), *f = malloc((size_t)w * h * 6);
  unsigned hashes[64] = {0}, n = 0, bad = 0;
  for (; SUCCEEDED(hr) && dec && out && n < st->count && n < 64; ++n) {
    const HevcRextPicture *p = &st->pictures[n];
    if (FAILED(ID3D11VideoContext_DecoderBeginFrame(g_vcontext, dec, views[p->surface], 0, NULL))) break;
    int ok = fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, st->bytes + p->pp, p->pp_size) &&
             (!p->qm_size || fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 0, st->bytes + p->qm, p->qm_size)) &&
             fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, st->bytes + p->slices, p->slices_size) &&
             fill_buffer(dec, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, st->bytes + p->bits, p->bits_size);
    D3D11_VIDEO_DECODER_BUFFER_DESC b[4];
    UINT nb = 0;
    b[nb++] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, p->pp_size);
    if (p->qm_size) b[nb++] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 0, p->qm_size);
    b[nb++] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, p->slices_size);
    b[nb++] = buffer_desc(D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, p->bits_size);
    ok = ok && SUCCEEDED(ID3D11VideoContext_SubmitDecoderBuffers(g_vcontext, dec, nb, b));
    ok = SUCCEEDED(ID3D11VideoContext_DecoderEndFrame(g_vcontext, dec)) && ok;
    if (!ok || !(planar ? read_planar(out, p->surface, surface) : read_packed(out, p->surface, row, surface))) break;
    const BYTE *chroma = surface + (size_t)w * h * (format == DXGI_FORMAT_NV12 ? 1 : 2);
    const size_t size = hevcrext_unpack(format, st->chroma, st->depth, w, h, surface, chroma, f, &bad);
    if (p->display < 64) hashes[p->display] = fnv(f, size, 0x811c9dc5u);
  }
  checkf(n == st->count && bad == 0, "%s: %u pictures decoded through ID3D11VideoContext (the samples in the top bits of their fields, alpha opaque)", st->name, n);
  UINT same = 0, first_bad = ~0u;
  for (UINT i = 0; i < st->count && i < 64; ++i) {
    if (hashes[i] == st->fnv[i]) ++same;
    else if (first_bad == ~0u) first_bad = i;
  }
  checkf(same == st->count, "%s: in display order every frame is the reference's, bit for bit (%u of %u; the first that differs: %d)", st->name, same, st->count, (int)first_bad);
  free(surface);
  free(f);
  for (unsigned i = 0; i < surfaces; ++i) if (views[i]) ID3D11VideoDecoderOutputView_Release(views[i]);
  if (out) ID3D11Texture2D_Release(out);
  if (dec) ID3D11VideoDecoder_Release(dec);
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
  checkf(SUCCEEDED(hr) && (caps.FeatureCaps & want_features) == want_features && !(caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_CONSTRICTION) &&
             caps.FilterCaps == 0xFF && caps.RateConversionCapsCount == 3 && caps.MaxInputStreams == 16 && caps.MaxStreamStates == 16,
         "GetVideoProcessorCaps: device %#x, features %#x, filters %#x, %u rate conversions, %u streams", caps.DeviceCaps, caps.FeatureCaps, caps.FilterCaps, caps.RateConversionCapsCount,
         caps.MaxInputStreams);
  D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rate;
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorRateConversionCaps(e, 0, &rate)) && !rate.PastFrames && rate.FutureFrames == 1 &&
            rate.ProcessorCaps == (D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB | D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION) && rate.CustomRateCount == 5,
        "the first rate conversion: bob deinterlacing, frame rate conversion (five custom rates, a future frame)");
  check(ID3D11VideoProcessorEnumerator_GetVideoProcessorRateConversionCaps(e, 3, &rate) == E_INVALIDARG, "... there are three: E_INVALIDARG for the fourth");
  D3D11_VIDEO_PROCESSOR_CUSTOM_RATE custom;
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorCustomRate(e, 0, 0, &custom)) && custom.CustomRate.Numerator == 5 && custom.CustomRate.Denominator == 2 &&
            custom.OutputFrames == 5 && !custom.InputInterlaced && custom.InputFramesOrFields == 2,
        "GetVideoProcessorCustomRate(0, 0): 24p to 60p, five frames of two");
  check(ID3D11VideoProcessorEnumerator_GetVideoProcessorCustomRate(e, 0, 5, &custom) == E_INVALIDARG, "... the first conversion has no inverse telecine rates (E_INVALIDARG)");
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorCustomRate(e, 1, 5, &custom)) && custom.CustomRate.Numerator == 4 && custom.CustomRate.Denominator == 5 &&
            custom.OutputFrames == 4 && custom.InputInterlaced && custom.InputFramesOrFields == 10,
        "GetVideoProcessorCustomRate(1, 5): 3:2 pulldown, four frames of ten fields");
  D3D11_VIDEO_PROCESSOR_FILTER_RANGE range;
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorFilterRange(e, D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS, &range)) && range.Minimum == -400 && range.Maximum == 400 &&
            range.Multiplier == 0.25f,
        "GetVideoProcessorFilterRange(brightness): -400..400 times 0.25");
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
                                                                                        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &ok)) && ok,
          "... and from PQ (tone mapped)");
    ID3D11VideoProcessorEnumerator1_Release(e1);
  } else check(0, "the enumerator is an ID3D11VideoProcessorEnumerator1");
  ID3D11VideoProcessor *vp = NULL;
  check(ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, 3, &vp) == E_INVALIDARG, "CreateVideoProcessor of a rate conversion there is not: E_INVALIDARG");
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
  /* what the processor takes now, and what it does not do */
  ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, vp, 0, D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST);
  check(SUCCEEDED(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream)), "Blt of an interlaced stream (bob): S_OK");
  ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  ID3D11VideoContext_VideoProcessorSetStreamLumaKey(v, vp, 0, TRUE, 0.1f, 0.2f);
  check(SUCCEEDED(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream)), "... with a luma key: S_OK");
  ID3D11VideoContext_VideoProcessorSetStreamLumaKey(v, vp, 0, FALSE, 0, 0);
  ID3D11VideoContext_VideoProcessorSetStreamFilter(v, vp, 0, D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS, TRUE, 10);
  check(SUCCEEDED(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream)), "... with a filter: S_OK");
  ID3D11VideoContext_VideoProcessorSetStreamFilter(v, vp, 0, D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS, FALSE, 0);
  ID3D11VideoContext_VideoProcessorSetOutputStereoMode(v, vp, TRUE);
  check(ID3D11VideoContext_VideoProcessorBlt(v, vp, out_view, 0, 1, &stream) == E_INVALIDARG, "... into a stereo output of one slice: E_INVALIDARG");
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

/* ---- the video processor's deinterlacing, filters, luma key, stereo and HDR ---------------------------------------------------------------------------------------- */

#include "deinterlace_data.h"

static ID3D11VideoProcessorInputView *input_view(ID3D11Texture2D *t, ID3D11VideoProcessorEnumerator *e, UINT slice) {
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d = {0, D3D11_VPIV_DIMENSION_TEXTURE2D, {{0, 0}}};
  d.Texture2D.ArraySlice = slice;
  ID3D11VideoProcessorInputView *v = NULL;
  if (t) ID3D11VideoDevice_CreateVideoProcessorInputView(g_vdevice, (ID3D11Resource *)t, e, &d, &v);
  return v;
}
static ID3D11VideoProcessorOutputView *output_view(ID3D11Texture2D *t, ID3D11VideoProcessorEnumerator *e, UINT slices) {
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d = {slices ? D3D11_VPOV_DIMENSION_TEXTURE2DARRAY : D3D11_VPOV_DIMENSION_TEXTURE2D, {{0}}};
  if (slices) d.Texture2DArray.ArraySize = slices;
  ID3D11VideoProcessorOutputView *v = NULL;
  if (t) ID3D11VideoDevice_CreateVideoProcessorOutputView(g_vdevice, (ID3D11Resource *)t, e, &d, &v);
  return v;
}
static ID3D11VideoProcessorEnumerator *enumerator_of(D3D11_VIDEO_FRAME_FORMAT format, UINT w, UINT h, UINT ow, UINT oh) {
  const D3D11_VIDEO_PROCESSOR_CONTENT_DESC c = {format, {30, 1}, w, h, {60, 1}, ow, oh, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL};
  ID3D11VideoProcessorEnumerator *e = NULL;
  ID3D11VideoDevice_CreateVideoProcessorEnumerator(g_vdevice, &c, &e);
  return e;
}
/* Slice `slice` of a BGRA texture (an array's) as pixels. */
static int read_bgra_slice(ID3D11Texture2D *t, UINT slice, UINT32 *out) {
  D3D11_TEXTURE2D_DESC desc;
  ID3D11Texture2D_GetDesc(t, &desc);
  ID3D11Texture2D *flat = texture(desc.Width, desc.Height, 1, desc.Format, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
  if (!flat) return 0;
  ID3D11DeviceContext_CopySubresourceRegion(g_context, (ID3D11Resource *)flat, 0, 0, 0, 0, (ID3D11Resource *)t, slice, NULL);
  const int ok = read_bgra(flat, out);
  ID3D11Texture2D_Release(flat);
  return ok;
}
static double pq_signal(double nits) {
  const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
  const double y = pow(nits / 10000, m1);
  return pow((c1 + c2 * y) / (1 + c3 * y), m2);
}

static void test_processor_effects(void) {
  ID3D11VideoContext *v = g_vcontext;
  ID3D11VideoProcessorEnumerator *e = enumerator_of(D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST, DEINT_W, DEINT_H, DEINT_W, DEINT_H);
  check(e != NULL, "an enumerator of interlaced content");
  if (!e) return;
  D3D11_VIDEO_PROCESSOR_CAPS caps;
  ID3D11VideoProcessorEnumerator_GetVideoProcessorCaps(e, &caps);
  checkf((caps.FeatureCaps & (D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_LUMA_KEY | D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_STEREO)) == (D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_LUMA_KEY | D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_STEREO) &&
             caps.FilterCaps == 0xFF && caps.StereoCaps == 0x1F && caps.RateConversionCapsCount == 3 && caps.AutoStreamCaps == 0xF && caps.InputFormatCaps == 7,
         "the caps: luma key, stereo (and every optional packing, flipping), eight filters, three rate conversions, automatic denoise, deringing, edge enhancement and "
         "colour correction (features %#x, filters %#x, stereo %#x)", caps.FeatureCaps,
         caps.FilterCaps, caps.StereoCaps);
  D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rc;
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorRateConversionCaps(e, 0, &rc)) && (rc.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB) &&
            !rc.PastFrames && rc.FutureFrames == 1 && !rc.ITelecineCaps,
        "rate conversion 0: bob, a future frame (frame rate conversion), no inverse telecine");
  const UINT itc = D3D11_VIDEO_PROCESSOR_ITELECINE_CAPS_32 | D3D11_VIDEO_PROCESSOR_ITELECINE_CAPS_22 | D3D11_VIDEO_PROCESSOR_ITELECINE_CAPS_2224 | D3D11_VIDEO_PROCESSOR_ITELECINE_CAPS_2332;
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorRateConversionCaps(e, 1, &rc)) &&
            rc.ProcessorCaps == (D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE | D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_INVERSE_TELECINE |
                                 D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION) &&
            rc.PastFrames == 5 && rc.FutureFrames == 5 && rc.CustomRateCount == 7 && rc.ITelecineCaps == itc,
        "rate conversion 1: motion adaptive, inverse telecine (3:2, 2:2, 2:2:2:4, 2:3:3:2), frame rate conversion, five frames either way");
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorRateConversionCaps(e, 2, &rc)) &&
            rc.ProcessorCaps == (D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_MOTION_COMPENSATION | D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_INVERSE_TELECINE |
                                 D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION) &&
            rc.PastFrames == 5 && rc.FutureFrames == 5 && rc.ITelecineCaps == itc,
        "rate conversion 2: motion compensated, the same");
  D3D11_VIDEO_PROCESSOR_FILTER_RANGE fr;
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorFilterRange(e, D3D11_VIDEO_PROCESSOR_FILTER_HUE, &fr)) && fr.Minimum == -720 && fr.Maximum == 720 && fr.Default == 0 &&
            fr.Multiplier == 0.25f,
        "the hue filter's range: -720..720 times 0.25 (degrees)");
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorFilterRange(e, D3D11_VIDEO_PROCESSOR_FILTER_CONTRAST, &fr)) && fr.Default == 64 && fr.Multiplier == 1.0f / 64,
        "the contrast filter's range: default 64 times 1/64");
  check(SUCCEEDED(ID3D11VideoProcessorEnumerator_GetVideoProcessorFilterRange(e, D3D11_VIDEO_PROCESSOR_FILTER_STEREO_ADJUSTMENT, &fr)) && fr.Minimum == -128 && fr.Maximum == 127 &&
            fr.Default == 0 && fr.Multiplier == 1.0f,
        "the stereo adjustment filter's range: -128..127 pixels");
  ID3D11VideoProcessor *bob = NULL, *adaptive = NULL;
  ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, 0, &bob);
  ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, 1, &adaptive);
  check(bob && adaptive, "processors of both rate conversions");
  if (!bob || !adaptive) goto done;

  /* motion adaptive deinterlacing of the middle of three frames, both fields: ffmpeg's yadif of it, processed as progressive pictures */
  {
    size_t n[5];
    const char *b64[5] = {g_deint_frame0_b64, g_deint_frame1_b64, g_deint_frame2_b64, g_deint_yadif_first_b64, g_deint_yadif_second_b64};
    BYTE *f[5];
    ID3D11Texture2D *t[5];
    ID3D11VideoProcessorInputView *iv[5];
    for (int k = 0; k < 5; ++k) {
      f[k] = base64_decode(b64[k], &n[k]);
      const D3D11_SUBRESOURCE_DATA init = {f[k], DEINT_W, 0};
      t[k] = f[k] ? texture(DEINT_W, DEINT_H, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &init, NULL) : NULL;
      iv[k] = input_view(t[k], e, 0);
    }
    ID3D11Texture2D *out = texture(DEINT_W, DEINT_H, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11Texture2D *ref = texture(DEINT_W, DEINT_H, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11VideoProcessorOutputView *ov = output_view(out, e, 0), *rv = output_view(ref, e, 0);
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE bt709 = {0, 0, 1, 0, 2, 0}, bt709_out = {0, 0, 1, 0, 2, 0};      /* (YCbCr BT.709 studio range) */
    static BYTE a[64 * 32 * 3 / 2], b[64 * 32 * 3 / 2];
    int same = 0;
    for (UINT field = 0; iv[0] && iv[1] && iv[2] && iv[3] && iv[4] && ov && rv && field < 2; ++field) {
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, adaptive, 0, &bt709);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, adaptive, &bt709_out);
      ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, adaptive, 0, D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST);
      D3D11_VIDEO_PROCESSOR_STREAM s;
      memset(&s, 0, sizeof s);
      s.Enable = TRUE;
      s.OutputIndex = field;
      s.InputFrameOrField = 1;
      s.PastFrames = s.FutureFrames = 1;
      s.ppPastSurfaces = &iv[0];
      s.pInputSurface = iv[1];
      s.ppFutureSurfaces = &iv[2];
      HRESULT hr = ID3D11VideoContext_VideoProcessorBlt(v, adaptive, ov, field, 1, &s);
      ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, adaptive, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
      D3D11_VIDEO_PROCESSOR_STREAM p = {TRUE, 0, 0, 0, 0, NULL, iv[3 + field], NULL, NULL, NULL, NULL};
      const HRESULT hr2 = ID3D11VideoContext_VideoProcessorBlt(v, adaptive, rv, 0, 1, &p);
      int diff = -1;
      if (SUCCEEDED(hr) && SUCCEEDED(hr2) && read_planar(out, 0, a) && read_planar(ref, 0, b)) { diff = 0; for (UINT i = 0; i < sizeof a; ++i) diff += a[i] != b[i]; }
      checkf(diff == 0, "motion adaptive deinterlacing (past and future frames), field %u: ffmpeg's yadif (%d bytes differ, %#lx)", field, diff, hr);
      same += diff == 0;
    }
    check(same == 2, "... both fields of the frame deinterlaced");
    /* bob, at half rate: the first field whatever OutputIndex says */
    if (iv[1] && ov) {
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, bob, 0, &bt709);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, bob, &bt709_out);
      ID3D11VideoContext_VideoProcessorSetStreamOutputRate(v, bob, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_HALF, FALSE, NULL);
      D3D11_VIDEO_PROCESSOR_STREAM s = {TRUE, 1, 2, 0, 0, NULL, iv[1], NULL, NULL, NULL, NULL};
      HRESULT hr = ID3D11VideoContext_VideoProcessorBlt(v, bob, ov, 0, 1, &s);
      int kept = 0;
      if (SUCCEEDED(hr) && read_planar(out, 0, a))
        for (UINT y = 0; y < DEINT_H; y += 2) for (UINT x = 0; x < DEINT_W; ++x) kept += a[y * DEINT_W + x] == f[1][y * DEINT_W + x];
      checkf(kept == DEINT_W * DEINT_H / 2, "bob at half rate: the top field's rows kept (%d of %d)", kept, DEINT_W * DEINT_H / 2);
      /* a reference frame more than the rate conversion takes: E_INVALIDARG */
      s.PastFrames = 1;
      s.ppPastSurfaces = &iv[0];
      check(ID3D11VideoContext_VideoProcessorBlt(v, bob, ov, 0, 1, &s) == E_INVALIDARG, "bob with a past frame: E_INVALIDARG (it takes none)");
      ID3D11VideoContext_VideoProcessorSetStreamOutputRate(v, bob, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, NULL);
    }
    for (int k = 0; k < 5; ++k) { if (iv[k]) ID3D11VideoProcessorInputView_Release(iv[k]); if (t[k]) ID3D11Texture2D_Release(t[k]); free(f[k]); }
    if (ov) ID3D11VideoProcessorOutputView_Release(ov);
    if (rv) ID3D11VideoProcessorOutputView_Release(rv);
    if (out) ID3D11Texture2D_Release(out);
    if (ref) ID3D11Texture2D_Release(ref);
  }
  ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, bob, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

  /* ProcAmp (VideoProcessorSetStreamFilter): a flat BT.601 colour into BGRA, against DXVA's formulas */
  {
    static BYTE f[64 * 32 * 3 / 2];
    memset(f, 120, 64 * 32);
    for (UINT i = 0; i < 64 * 16; i += 2) { f[64 * 32 + i] = 100; f[64 * 32 + i + 1] = 150; }
    const D3D11_SUBRESOURCE_DATA init = {f, 64, 0};
    ID3D11Texture2D *in = texture(64, 32, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &init, NULL);
    ID3D11Texture2D *out = texture(64, 32, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11VideoProcessorInputView *iv = input_view(in, e, 0);
    ID3D11VideoProcessorOutputView *ov = output_view(out, e, 0);
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv601 = {0, 0, 0, 0, 1, 0}, rgb = {0, 0, 0, 0, 0, 0};
    ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, bob, 0, &yuv601);
    ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, bob, &rgb);
    ID3D11VideoContext_VideoProcessorSetStreamFilter(v, bob, 0, D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS, TRUE, -40);      /* -10 */
    ID3D11VideoContext_VideoProcessorSetStreamFilter(v, bob, 0, D3D11_VIDEO_PROCESSOR_FILTER_CONTRAST, TRUE, 80);         /* 1.25 */
    ID3D11VideoContext_VideoProcessorSetStreamFilter(v, bob, 0, D3D11_VIDEO_PROCESSOR_FILTER_HUE, TRUE, -120);            /* -30 degrees */
    ID3D11VideoContext_VideoProcessorSetStreamFilter(v, bob, 0, D3D11_VIDEO_PROCESSOR_FILTER_SATURATION, TRUE, 96);       /* 1.5 */
    static UINT32 px[64 * 32];
    D3D11_VIDEO_PROCESSOR_STREAM s = {TRUE, 0, 0, 0, 0, NULL, iv, NULL, NULL, NULL, NULL};
    HRESULT hr = iv && ov ? ID3D11VideoContext_VideoProcessorBlt(v, bob, ov, 0, 1, &s) : E_FAIL;
    const double c = 1.25, sat = 1.5, h = -30 * 3.14159265358979 / 180;
    const double y = (120 - 16) * c - 10 + 16, cb = ((100 - 128) * cos(h) + (150 - 128) * sin(h)) * c * sat + 128, cr = ((150 - 128) * cos(h) - (100 - 128) * sin(h)) * c * sat + 128;
    int want[3];
    rgb601((int)lround(y), (int)lround(cb), (int)lround(cr), want);
    const int ok = SUCCEEDED(hr) && read_bgra(out, px);
    const UINT32 got = px[16 * 64 + 32];
    checkf(ok && close_to(red(got), want[0], 3) && close_to(green(got), want[1], 3) && close_to(blue(got), want[2], 3),
           "ProcAmp (brightness -10, contrast 1.25, hue -30, saturation 1.5): (%d, %d, %d), DXVA's formulas (%d, %d, %d)", red(got), green(got), blue(got), want[0], want[1], want[2]);
    ID3D11VideoContext_VideoProcessorSetStreamFilter(v, bob, 0, D3D11_VIDEO_PROCESSOR_FILTER_CONTRAST, TRUE, 1000);
    check(ID3D11VideoContext_VideoProcessorBlt(v, bob, ov, 0, 1, &s) == E_INVALIDARG, "a filter level outside its range: E_INVALIDARG");
    ID3D11VideoContext_VideoProcessorSetStreamFilter(v, bob, 0, D3D11_VIDEO_PROCESSOR_FILTER_STEREO_ADJUSTMENT, TRUE, 200);
    ID3D11VideoContext_VideoProcessorSetStreamFilter(v, bob, 0, D3D11_VIDEO_PROCESSOR_FILTER_CONTRAST, FALSE, 0);
    check(ID3D11VideoContext_VideoProcessorBlt(v, bob, ov, 0, 1, &s) == E_INVALIDARG, "the stereo adjustment filter outside its range: E_INVALIDARG");
    for (UINT k = 0; k < 8; ++k) ID3D11VideoContext_VideoProcessorSetStreamFilter(v, bob, 0, (D3D11_VIDEO_PROCESSOR_FILTER)k, FALSE, 0);
    /* luma key: Y 120 of 0..255 (0.47) is in 0.4 .. 0.6: the background (green) instead */
    const D3D11_VIDEO_COLOR green_bg = {{{0, 1, 0, 1}}};
    ID3D11VideoContext_VideoProcessorSetOutputBackgroundColor(v, bob, FALSE, &green_bg);
    ID3D11VideoContext_VideoProcessorSetStreamLumaKey(v, bob, 0, TRUE, 0.4f, 0.6f);
    hr = ID3D11VideoContext_VideoProcessorBlt(v, bob, ov, 0, 1, &s);
    checkf(SUCCEEDED(hr) && read_bgra(out, px) && red(px[100]) == 0 && green(px[100]) == 255 && blue(px[100]) == 0, "luma key 0.4 .. 0.6 over Y 120: the background (%#lx)", hr);
    ID3D11VideoContext_VideoProcessorSetStreamLumaKey(v, bob, 0, TRUE, 0.5f, 0.6f);
    hr = ID3D11VideoContext_VideoProcessorBlt(v, bob, ov, 0, 1, &s);
    checkf(SUCCEEDED(hr) && read_bgra(out, px) && green(px[100]) != 255, "luma key 0.5 .. 0.6 over Y 120: the picture");
    ID3D11VideoContext_VideoProcessorSetStreamLumaKey(v, bob, 0, FALSE, 0, 0);
    if (iv) ID3D11VideoProcessorInputView_Release(iv);
    if (ov) ID3D11VideoProcessorOutputView_Release(ov);
    if (in) ID3D11Texture2D_Release(in);
    if (out) ID3D11Texture2D_Release(out);
  }

  /* stereo: side by side with frame 0 the right view, into an output of two slices (left, right); rows interleaved into a mono output (the base view, frame 1) */
  {
    ID3D11VideoProcessorEnumerator *se = enumerator_of(D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, 32, 16, 16, 16);
    ID3D11VideoProcessor *sp = NULL;
    if (se) ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, se, 0, &sp);
    static UINT32 pic[32 * 16], rows[16 * 16];
    for (UINT y = 0; y < 16; ++y) for (UINT x = 0; x < 32; ++x) pic[y * 32 + x] = x < 16 ? 0xFFFF0000u : 0xFF0000FFu;      /* (frame 0 red, frame 1 blue) */
    for (UINT y = 0; y < 16; ++y) for (UINT x = 0; x < 16; ++x) rows[y * 16 + x] = (y & 1) ? 0xFF0000FFu : 0xFFFF0000u;
    const D3D11_SUBRESOURCE_DATA i1 = {pic, 32 * 4, 0}, i2 = {rows, 16 * 4, 0};
    ID3D11Texture2D *in = texture(32, 16, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &i1, NULL);
    ID3D11Texture2D *in2 = texture(16, 16, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &i2, NULL);
    ID3D11Texture2D *pair = texture(16, 16, 2, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11Texture2D *one = texture(16, 8, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11VideoProcessorInputView *iv = se ? input_view(in, se, 0) : NULL, *iv2 = se ? input_view(in2, se, 0) : NULL;
    ID3D11VideoProcessorOutputView *pv = se ? output_view(pair, se, 2) : NULL, *ov = se ? output_view(one, se, 0) : NULL;
    if (sp && iv && iv2 && pv && ov) {
      const D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb = {0, 0, 0, 0, 0, 0};
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, sp, 0, &rgb);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, sp, &rgb);
      ID3D11VideoContext_VideoProcessorSetStreamStereoFormat(v, sp, 0, TRUE, D3D11_VIDEO_PROCESSOR_STEREO_FORMAT_HORIZONTAL, FALSE, TRUE, D3D11_VIDEO_PROCESSOR_STEREO_FLIP_NONE, 0);
      ID3D11VideoContext_VideoProcessorSetOutputStereoMode(v, sp, TRUE);
      D3D11_VIDEO_PROCESSOR_STREAM s = {TRUE, 0, 0, 0, 0, NULL, iv, NULL, NULL, NULL, NULL};
      check(ID3D11VideoContext_VideoProcessorBlt(v, sp, ov, 0, 1, &s) == E_INVALIDARG, "stereo output into a view of one slice: E_INVALIDARG");
      HRESULT hr = ID3D11VideoContext_VideoProcessorBlt(v, sp, pv, 0, 1, &s);
      static UINT32 l[16 * 16], r[16 * 16];
      const int ok = SUCCEEDED(hr) && read_bgra_slice(pair, 0, l) && read_bgra_slice(pair, 1, r);
      checkf(ok && l[0] == 0xFF0000FFu && l[255] == 0xFF0000FFu && r[0] == 0xFFFF0000u && r[255] == 0xFFFF0000u,
             "side by side, frame 1 the left view, into two slices: left blue %#x, right red %#x (%#lx)", ok ? l[0] : 0, ok ? r[0] : 0, hr);
      ID3D11VideoContext_VideoProcessorSetOutputStereoMode(v, sp, FALSE);
      ID3D11VideoContext_VideoProcessorSetStreamStereoFormat(v, sp, 0, TRUE, D3D11_VIDEO_PROCESSOR_STEREO_FORMAT_ROW_INTERLEAVED, TRUE, FALSE, D3D11_VIDEO_PROCESSOR_STEREO_FLIP_NONE, 0);
      s.pInputSurface = iv2;
      hr = ID3D11VideoContext_VideoProcessorBlt(v, sp, ov, 0, 1, &s);
      static UINT32 m[16 * 8];
      checkf(SUCCEEDED(hr) && read_bgra(one, m) && m[0] == 0xFF0000FFu && m[127] == 0xFF0000FFu, "rows interleaved, mono output: the base view (frame 1, the odd rows: blue)");
      ID3D11VideoContext_VideoProcessorSetStreamStereoFormat(v, sp, 0, FALSE, D3D11_VIDEO_PROCESSOR_STEREO_FORMAT_MONO, TRUE, TRUE, D3D11_VIDEO_PROCESSOR_STEREO_FLIP_NONE, 0);
    } else check(0, "the stereo pictures and views");
    if (iv) ID3D11VideoProcessorInputView_Release(iv);
    if (iv2) ID3D11VideoProcessorInputView_Release(iv2);
    if (pv) ID3D11VideoProcessorOutputView_Release(pv);
    if (ov) ID3D11VideoProcessorOutputView_Release(ov);
    if (in) ID3D11Texture2D_Release(in);
    if (in2) ID3D11Texture2D_Release(in2);
    if (pair) ID3D11Texture2D_Release(pair);
    if (one) ID3D11Texture2D_Release(one);
    if (sp) ID3D11VideoProcessor_Release(sp);
    if (se) ID3D11VideoProcessorEnumerator_Release(se);
  }

  /* HDR10 into SDR with the stream's metadata: 300 cd/m2 is white when the content's light level is 300, not when it is not known (1000 assumed) */
  {
    ID3D11VideoContext1 *v1 = NULL;
    ID3D11VideoContext2 *v2 = NULL;
    ID3D11VideoContext_QueryInterface(v, &IID_ID3D11VideoContext1, (void **)&v1);
    ID3D11VideoContext_QueryInterface(v, &IID_ID3D11VideoContext2, (void **)&v2);
    static USHORT f[16 * 24];
    const UINT code = (UINT)lround(64 + 876 * pq_signal(300));
    for (UINT i = 0; i < 16 * 16; ++i) f[i] = (USHORT)(code << 6);
    for (UINT i = 16 * 16; i < 16 * 24; ++i) f[i] = 512 << 6;
    const D3D11_SUBRESOURCE_DATA init = {f, 32, 0};
    ID3D11VideoProcessorEnumerator *he = enumerator_of(D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, 16, 16, 16, 16);
    BOOL conv = FALSE;
    ID3D11VideoProcessorEnumerator1 *he1 = NULL;
    if (he && SUCCEEDED(ID3D11VideoProcessorEnumerator_QueryInterface(he, &IID_ID3D11VideoProcessorEnumerator1, (void **)&he1))) {
      ID3D11VideoProcessorEnumerator1_CheckVideoProcessorFormatConversion(he1, DXGI_FORMAT_P010, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                          DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &conv);
      ID3D11VideoProcessorEnumerator1_Release(he1);
    }
    check(conv, "CheckVideoProcessorFormatConversion: HDR10 (P010, PQ) into BGRA sRGB (tone mapped)");
    ID3D11VideoProcessor *hp = NULL;
    if (he) ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, he, 0, &hp);
    ID3D11Texture2D *in = texture(16, 16, 1, DXGI_FORMAT_P010, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &init, NULL);
    ID3D11Texture2D *out = texture(16, 16, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11VideoProcessorInputView *iv = he ? input_view(in, he, 0) : NULL;
    ID3D11VideoProcessorOutputView *ov = he ? output_view(out, he, 0) : NULL;
    if (v1 && v2 && hp && iv && ov) {
      ID3D11VideoContext1_VideoProcessorSetStreamColorSpace1(v1, hp, 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
      ID3D11VideoContext1_VideoProcessorSetOutputColorSpace1(v1, hp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      D3D11_VIDEO_PROCESSOR_STREAM s = {TRUE, 0, 0, 0, 0, NULL, iv, NULL, NULL, NULL, NULL};
      static UINT32 px[16 * 16];
      HRESULT hr = ID3D11VideoContext_VideoProcessorBlt(v, hp, ov, 0, 1, &s);
      const int unknown = SUCCEEDED(hr) && read_bgra(out, px) ? green(px[40]) : -1;
      DXGI_HDR_METADATA_HDR10 md;
      memset(&md, 0, sizeof md);
      md.MaxMasteringLuminance = 1000;
      md.MinMasteringLuminance = 50;      /* (0.005 cd/m2) */
      md.MaxContentLightLevel = 300;
      md.MaxFrameAverageLightLevel = 200;
      ID3D11VideoContext2_VideoProcessorSetStreamHDRMetaData(v2, hp, 0, DXGI_HDR_METADATA_TYPE_HDR10, sizeof md, &md);
      hr = ID3D11VideoContext_VideoProcessorBlt(v, hp, ov, 0, 1, &s);
      const int known = SUCCEEDED(hr) && read_bgra(out, px) ? green(px[40]) : -1;
      checkf(unknown > 200 && unknown < 255 && known == 255 && red(px[40]) == 255 && blue(px[40]) == 255,
             "PQ 300 cd/m2 into SDR: %d with no metadata (content peak 1000), %d with MaxCLL 300 (the peak to white)", unknown, known);
      ID3D11VideoContext2_VideoProcessorSetStreamHDRMetaData(v2, hp, 0, DXGI_HDR_METADATA_TYPE_NONE, 0, NULL);
    } else check(0, "the HDR pictures, views and contexts");
    if (iv) ID3D11VideoProcessorInputView_Release(iv);
    if (ov) ID3D11VideoProcessorOutputView_Release(ov);
    if (in) ID3D11Texture2D_Release(in);
    if (out) ID3D11Texture2D_Release(out);
    if (hp) ID3D11VideoProcessor_Release(hp);
    if (he) ID3D11VideoProcessorEnumerator_Release(he);
    if (v1) ID3D11VideoContext1_Release(v1);
    if (v2) ID3D11VideoContext2_Release(v2);
  }
done:
  if (bob) ID3D11VideoProcessor_Release(bob);
  if (adaptive) ID3D11VideoProcessor_Release(adaptive);
  ID3D11VideoProcessorEnumerator_Release(e);
}

/* ---- frame rate conversion, inverse telecine, motion compensated deinterlacing, stereo adjustment, HDR10+ -------------------------------------------------------- */

/* The test's own sine and cosine (a reduced argument's Taylor series): the CPU emulator's x87 fsin gives NaN, and the scene must not depend on it. */
static double tsin(double x) {
  const double pi = 3.14159265358979323846;
  x = fmod(x, 2 * pi);
  if (x > pi) x -= 2 * pi;
  if (x < -pi) x += 2 * pi;
  double term = x, sum = 0;
  for (int k = 1; k < 40; k += 2) { sum += term; term *= -x * x / ((k + 1) * (k + 2)); }
  return sum;
}
static double tcos(double x) { return tsin(x + 1.57079632679489661923); }
/* The moving test picture (full-range NV12; d3d12videotest has the same): luma and a little colour, at film frame (time) k, moving 3 and 1 pixels a frame. */
static BYTE scene_luma(double x, double y, double k) {
  const double fx = x - 3.0 * k, fy = y - 1.0 * k;
  double v = 120 + 50 * tsin(0.27 * fx + 0.11 * fy) + 30 * tsin(0.61 * fy - 0.2 * fx) + 20 * tsin(0.9 * fx + 0.75 * fy);
  if ((fx - 20) * (fx - 20) + (fy - 12) * (fy - 12) < 49) v += 60;
  return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v);
}
static void scene_frame(BYTE *f, UINT w, UINT h, double k) {
  for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) f[y * w + x] = scene_luma(x, y, k);
  for (UINT y = 0; y < h / 2; ++y)
    for (UINT x = 0; x < w / 2; ++x) {
      f[w * h + y * w + 2 * x] = (BYTE)(128 + 30 * tsin(0.2 * (2.0 * x - 3 * k)));
      f[w * h + y * w + 2 * x + 1] = (BYTE)(128 + 20 * tcos(0.15 * (2.0 * y - k)));
    }
}
static void field_rows(BYTE *to, const BYTE *from, UINT w, UINT h, int parity) {
  for (UINT y = (UINT)parity; y < h; y += 2) memcpy(to + y * w, from + y * w, w);
  for (UINT y = (UINT)parity; y < h / 2; y += 2) memcpy(to + w * h + y * w, from + w * h + y * w, w);
}
static ID3D11Texture2D *nv12_texture(const BYTE *f, UINT w, UINT h, UINT bind) {
  const D3D11_SUBRESOURCE_DATA init = {f, w, 0};
  return texture(w, h, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, bind, 0, f ? &init : NULL, NULL);
}
static double luma_psnr(const BYTE *a, const BYTE *b, UINT w, UINT h, UINT border) {
  double se = 0;
  UINT n = 0;
  for (UINT y = border; y + border < h; ++y) for (UINT x = border; x + border < w; ++x) { const double d = (double)a[y * w + x] - b[y * w + x]; se += d * d; ++n; }
  return se == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / (se / n));
}
/* A bit writer for HDR10+ metadata (ST 2094-40) */
typedef struct Bits { BYTE *b; UINT n; } Bits;
static void put_bits(Bits *w, UINT v, UINT n) {
  for (UINT i = n; i-- > 0; ++w->n)
    if ((v >> i) & 1) w->b[w->n / 8] |= (BYTE)(0x80 >> (w->n % 8));
}

static void test_processor_rates(void) {
  ID3D11VideoContext *v = g_vcontext;
  enum { W = 64, H = 48 };
  const D3D11_VIDEO_PROCESSOR_COLOR_SPACE full = {0, 0, 1, 0, 2, 0};      /* (YCbCr BT.709, 0..255) */
  static BYTE frame[W * H * 3 / 2], got[W * H * 3 / 2], want[W * H * 3 / 2];

  /* frame rate conversion by a custom rate, 24p to 60p: blended (the frames' luma 40 and 200: 40, 104, 168), or repeated */
  {
    ID3D11VideoProcessorEnumerator *e = enumerator_of(D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, W, H, W, H);
    ID3D11VideoProcessor *vp = NULL;
    if (e) ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, 0, &vp);
    memset(frame, 40, W * H);
    memset(frame + W * H, 128, W * H / 2);
    ID3D11Texture2D *a = nv12_texture(frame, W, H, D3D11_BIND_SHADER_RESOURCE);
    memset(frame, 200, W * H);
    ID3D11Texture2D *b = nv12_texture(frame, W, H, D3D11_BIND_SHADER_RESOURCE), *out = nv12_texture(NULL, W, H, D3D11_BIND_RENDER_TARGET);
    ID3D11VideoProcessorInputView *ia = input_view(a, e, 0), *ib = input_view(b, e, 0);
    ID3D11VideoProcessorOutputView *ov = output_view(out, e, 0);
    int blended = 0, repeated = 0;
    if (vp && ia && ib && ov) {
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, vp, 0, &full);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, vp, &full);
      ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(v, vp, 0, FALSE);
      const DXGI_RATIONAL five_halves = {5, 2}, seven_thirds = {7, 3};
      for (int repeat = 0; repeat < 2; ++repeat) {
        ID3D11VideoContext_VideoProcessorSetStreamOutputRate(v, vp, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_CUSTOM, repeat, &five_halves);
        for (UINT k = 0; k < 3; ++k) {
          D3D11_VIDEO_PROCESSOR_STREAM s;
          memset(&s, 0, sizeof s);
          s.Enable = TRUE;
          s.OutputIndex = k;
          s.InputFrameOrField = 0;
          s.FutureFrames = 1;
          s.ppFutureSurfaces = &ib;
          s.pInputSurface = ia;
          const HRESULT hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, ov, k, 1, &s);
          const int luma = SUCCEEDED(hr) && read_planar(out, 0, got) ? got[W * 10 + 10] : -1;
          const int expect = repeat ? 40 : (int)(40 + 160 * (2.0 * k / 5));
          if (close_to(luma, expect, 1) && repeat) ++repeated;
          else if (close_to(luma, expect, 1)) ++blended;
          else printf("d3d11videotest: 24p -> 60p output %u (%s): luma %d, want %d (%#lx)\n", k, repeat ? "repeated" : "blended", luma, expect, hr);
        }
      }
      ID3D11VideoContext_VideoProcessorSetStreamOutputRate(v, vp, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_CUSTOM, FALSE, &seven_thirds);
      D3D11_VIDEO_PROCESSOR_STREAM s = {TRUE, 0, 0, 0, 0, NULL, ia, NULL, NULL, NULL, NULL};
      check(ID3D11VideoContext_VideoProcessorBlt(v, vp, ov, 0, 1, &s) == E_INVALIDARG, "a custom rate the processor has not got (7/3): E_INVALIDARG");
      ID3D11VideoContext_VideoProcessorSetStreamOutputRate(v, vp, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, NULL);
    }
    check(blended == 3 && repeated == 3, "custom rate 5/2 (24p to 60p): outputs blended between the frames around them, or repeated with RepeatFrame");
    if (ia) ID3D11VideoProcessorInputView_Release(ia);
    if (ib) ID3D11VideoProcessorInputView_Release(ib);
    if (ov) ID3D11VideoProcessorOutputView_Release(ov);
    if (a) ID3D11Texture2D_Release(a);
    if (b) ID3D11Texture2D_Release(b);
    if (out) ID3D11Texture2D_Release(out);
    if (vp) ID3D11VideoProcessor_Release(vp);
    if (e) ID3D11VideoProcessorEnumerator_Release(e);
  }

  /* inverse telecine: 3:2 pulldown (custom rate 4/5 of interlaced input) and 2:2 (1/1) of the moving picture, each output the film frame bit for bit (both rate
   * conversions that do it) */
  {
    enum { FILM = 12, FRAMES = 15 };
    static BYTE film[FILM][W * H * 3 / 2], tele[FRAMES][W * H * 3 / 2], two[FILM][W * H * 3 / 2];
    for (int k = 0; k < FILM; ++k) scene_frame(film[k], W, H, k);
    int field = 0, parity = 0;
    for (int k = 0; k < FILM && field / 2 < FRAMES; ++k)
      for (int n = 0; n < (k % 2 ? 3 : 2) && field / 2 < FRAMES; ++n, ++field, parity ^= 1) field_rows(tele[field / 2], film[k], W, H, parity);
    for (int k = 0; k + 1 < FILM; ++k) { field_rows(two[k], film[k], W, H, 0); field_rows(two[k], film[k + 1], W, H, 1); }      /* (2:2, a field late: phase-shifted) */
    ID3D11VideoProcessorEnumerator *e = enumerator_of(D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST, W, H, W, H);
    ID3D11Texture2D *t[FRAMES], *f[FILM], *tw[FILM];
    ID3D11VideoProcessorInputView *it[FRAMES], *ifm[FILM], *itw[FILM];
    for (int k = 0; k < FRAMES; ++k) { t[k] = nv12_texture(tele[k], W, H, D3D11_BIND_SHADER_RESOURCE); it[k] = input_view(t[k], e, 0); }
    for (int k = 0; k < FILM; ++k) {
      f[k] = nv12_texture(film[k], W, H, D3D11_BIND_SHADER_RESOURCE);
      ifm[k] = input_view(f[k], e, 0);
      tw[k] = nv12_texture(two[k], W, H, D3D11_BIND_SHADER_RESOURCE);
      itw[k] = input_view(tw[k], e, 0);
    }
    ID3D11Texture2D *out = nv12_texture(NULL, W, H, D3D11_BIND_RENDER_TARGET), *ref = nv12_texture(NULL, W, H, D3D11_BIND_RENDER_TARGET);
    ID3D11VideoProcessorOutputView *ov = output_view(out, e, 0), *rv = output_view(ref, e, 0);
    int same = 0, outputs = 0;
    for (UINT index = 1; e && ov && rv && index < 3; ++index) {
      ID3D11VideoProcessor *vp = NULL;
      ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, index, &vp);
      if (!vp) continue;
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, vp, 0, &full);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, vp, &full);
      ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(v, vp, 0, FALSE);
      for (int pattern = 0; pattern < 2; ++pattern) {
        const DXGI_RATIONAL rate = pattern == 0 ? (DXGI_RATIONAL){4, 5} : (DXGI_RATIONAL){1, 1};
        for (int n = pattern == 0 ? 5 : 2; n < (pattern == 0 ? 10 : 7); ++n) {      /* (2:2: frames 2 .. 6, their five future frames within the stream) */
          if (pattern == 0 && n % 5 == 4) continue;      /* (the cycle's last frame makes no output) */
          ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, vp, 0, D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST);
          ID3D11VideoContext_VideoProcessorSetStreamOutputRate(v, vp, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_CUSTOM, FALSE, &rate);
          ID3D11VideoProcessorInputView **frames = pattern == 0 ? it : itw;
          ID3D11VideoProcessorInputView *past[5], *future[5];
          const int npast = n < 5 ? n : 5;
          for (int k = 0; k < 5; ++k) { past[k] = n - npast + k >= 0 ? frames[n - npast + k] : NULL; future[k] = frames[n + 1 + k]; }
          D3D11_VIDEO_PROCESSOR_STREAM s = {TRUE, 0, (UINT)n, (UINT)npast, 5, past, frames[n], future, NULL, NULL, NULL};
          const HRESULT hr = ID3D11VideoContext_VideoProcessorBlt(v, vp, ov, 0, 1, &s);
          const int film_frame = pattern == 0 ? 4 + (n % 5 * 4 + 4) / 5 : n;      /* (3:2: the second cycle's film frames 4 .. 7; 2:2: frame n) */
          ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
          ID3D11VideoContext_VideoProcessorSetStreamOutputRate(v, vp, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, NULL);
          D3D11_VIDEO_PROCESSOR_STREAM p = {TRUE, 0, 0, 0, 0, NULL, ifm[film_frame], NULL, NULL, NULL, NULL};
          const HRESULT hr2 = ID3D11VideoContext_VideoProcessorBlt(v, vp, rv, 0, 1, &p);
          const int ok = SUCCEEDED(hr) && SUCCEEDED(hr2) && read_planar(out, 0, got) && read_planar(ref, 0, want) && !memcmp(got, want, sizeof got);
          if (!ok) printf("d3d11videotest: inverse telecine (%s, rate conversion %u) of frame %d: not film frame %d (%#lx)\n", pattern ? "2:2" : "3:2", index, n, film_frame, hr);
          same += ok;
          ++outputs;
        }
      }
      ID3D11VideoProcessor_Release(vp);
    }
    checkf(outputs == 18 && same == outputs, "inverse telecine (3:2 at 4/5, 2:2 at 1/1, motion adaptive and compensated): %d of %d outputs the film frames, bit for bit", same, outputs);
    for (int k = 0; k < FRAMES; ++k) { if (it[k]) ID3D11VideoProcessorInputView_Release(it[k]); if (t[k]) ID3D11Texture2D_Release(t[k]); }
    for (int k = 0; k < FILM; ++k) {
      if (ifm[k]) ID3D11VideoProcessorInputView_Release(ifm[k]);
      if (f[k]) ID3D11Texture2D_Release(f[k]);
      if (itw[k]) ID3D11VideoProcessorInputView_Release(itw[k]);
      if (tw[k]) ID3D11Texture2D_Release(tw[k]);
    }
    if (ov) ID3D11VideoProcessorOutputView_Release(ov);
    if (rv) ID3D11VideoProcessorOutputView_Release(rv);
    if (out) ID3D11Texture2D_Release(out);
    if (ref) ID3D11Texture2D_Release(ref);
    if (e) ID3D11VideoProcessorEnumerator_Release(e);
  }

  /* motion compensated deinterlacing (rate conversion 2) of an interlaced capture of the moving picture - each field at its own time - against the progressive
   * picture at each field's time: better than motion adaptive (1) */
  {
    enum { N = 5 };
    static BYTE inter[N][W * H * 3 / 2], truth[W * H * 3 / 2], half_frame[W * H * 3 / 2];
    for (int n = 0; n < N; ++n) {
      scene_frame(half_frame, W, H, n);
      field_rows(inter[n], half_frame, W, H, 0);
      scene_frame(half_frame, W, H, n + 0.5);
      field_rows(inter[n], half_frame, W, H, 1);
    }
    ID3D11VideoProcessorEnumerator *e = enumerator_of(D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST, W, H, W, H);
    ID3D11Texture2D *t[N];
    ID3D11VideoProcessorInputView *iv[N];
    for (int n = 0; n < N; ++n) { t[n] = nv12_texture(inter[n], W, H, D3D11_BIND_SHADER_RESOURCE); iv[n] = input_view(t[n], e, 0); }
    ID3D11Texture2D *out = nv12_texture(NULL, W, H, D3D11_BIND_RENDER_TARGET);
    ID3D11VideoProcessorOutputView *ov = output_view(out, e, 0);
    double psnr[3] = {0, 0, 0};
    int runs = 0;
    for (UINT index = 1; e && ov && index < 3; ++index) {
      ID3D11VideoProcessor *vp = NULL;
      ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, index, &vp);
      if (!vp) continue;
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, vp, 0, &full);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, vp, &full);
      ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(v, vp, 0, FALSE);
      ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(v, vp, 0, D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST);
      for (int n = 1; n + 1 < N; ++n)
        for (UINT fieldn = 0; fieldn < 2; ++fieldn) {
          D3D11_VIDEO_PROCESSOR_STREAM s = {TRUE, fieldn, (UINT)n, 1, 1, &iv[n - 1], iv[n], &iv[n + 1], NULL, NULL, NULL};
          if (FAILED(ID3D11VideoContext_VideoProcessorBlt(v, vp, ov, fieldn, 1, &s)) || !read_planar(out, 0, got)) continue;
          scene_frame(truth, W, H, n + 0.5 * fieldn);
          psnr[index] += luma_psnr(got, truth, W, H, 4);
          if (index == 2) ++runs;
        }
      ID3D11VideoProcessor_Release(vp);
    }
    psnr[1] /= runs ? runs : 1;
    psnr[2] /= runs ? runs : 1;
    checkf(runs == 6 && psnr[2] > psnr[1] + 1.0, "motion compensated deinterlacing: luma PSNR %.2f dB, motion adaptive %.2f dB", psnr[2], psnr[1]);
    for (int n = 0; n < N; ++n) { if (iv[n]) ID3D11VideoProcessorInputView_Release(iv[n]); if (t[n]) ID3D11Texture2D_Release(t[n]); }
    if (ov) ID3D11VideoProcessorOutputView_Release(ov);
    if (out) ID3D11Texture2D_Release(out);
    if (e) ID3D11VideoProcessorEnumerator_Release(e);
  }

  /* the stereo adjustment filter: a side by side picture into the two views of a stereo output, moved apart by the level */
  {
    for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) frame[y * W + x] = (BYTE)(x < W / 2 ? 4 * x : 4 * (x - W / 2) + 100);
    memset(frame + W * H, 128, W * H / 2);
    ID3D11VideoProcessorEnumerator *e = enumerator_of(D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, W, H, W / 2, H);
    ID3D11VideoProcessor *vp = NULL;
    if (e) ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, 0, &vp);
    ID3D11Texture2D *in = nv12_texture(frame, W, H, D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D *out = texture(W / 2, H, 2, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11VideoProcessorInputView *iv = input_view(in, e, 0);
    ID3D11VideoProcessorOutputView *ov = output_view(out, e, 2);
    int right = 0;
    if (vp && iv && ov) {
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(v, vp, 0, &full);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(v, vp, &full);
      ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(v, vp, 0, FALSE);
      ID3D11VideoContext_VideoProcessorSetOutputStereoMode(v, vp, TRUE);
      ID3D11VideoContext_VideoProcessorSetStreamStereoFormat(v, vp, 0, TRUE, D3D11_VIDEO_PROCESSOR_STEREO_FORMAT_HORIZONTAL, TRUE, TRUE, D3D11_VIDEO_PROCESSOR_STEREO_FLIP_NONE, 0);
      ID3D11VideoContext_VideoProcessorSetStreamFilter(v, vp, 0, D3D11_VIDEO_PROCESSOR_FILTER_STEREO_ADJUSTMENT, TRUE, 3);
      D3D11_VIDEO_PROCESSOR_STREAM s = {TRUE, 0, 0, 0, 0, NULL, iv, NULL, NULL, NULL, NULL};
      if (SUCCEEDED(ID3D11VideoContext_VideoProcessorBlt(v, vp, ov, 0, 1, &s)))
        for (UINT k = 0; k < 2; ++k) {
          static BYTE view[W / 2 * H * 3 / 2];
          if (!read_planar(out, k, view)) continue;
          for (int x = 0; x < W / 2; ++x) {
            int from = k == 0 ? x + 3 : x - 3;
            from = from < 0 ? 0 : from > W / 2 - 1 ? W / 2 - 1 : from;
            right += view[5 * (W / 2) + x] == (k == 0 ? 4 * from : 4 * from + 100);
          }
        }
    }
    checkf(right == W, "the stereo adjustment filter (3): %d of %d columns of the two views moved apart", right, W);
    if (iv) ID3D11VideoProcessorInputView_Release(iv);
    if (ov) ID3D11VideoProcessorOutputView_Release(ov);
    if (in) ID3D11Texture2D_Release(in);
    if (out) ID3D11Texture2D_Release(out);
    if (vp) ID3D11VideoProcessor_Release(vp);
    if (e) ID3D11VideoProcessorEnumerator_Release(e);
  }

  /* HDR10+ metadata of a stream (DXGI_HDR_METADATA_TYPE_HDR10PLUS, ST 2094-40): grey PQ content of 2000 cd/m2 onto a 1000 cd/m2 PQ display goes along the metadata's
   * curve (the scene's peak 4000, the targeted display 1000, the knee at (0.25, 0.5), anchors 0.6 .. 0.9) - not the static levels' curve */
  {
    BYTE meta[72];
    memset(meta, 0, sizeof meta);
    Bits b = {meta, 0};
    put_bits(&b, 0xB5, 8); put_bits(&b, 0x003C, 16); put_bits(&b, 0x0001, 16); put_bits(&b, 4, 8);      /* (the T.35 header) */
    put_bits(&b, 1, 8); put_bits(&b, 1, 2);      /* application version, one window */
    put_bits(&b, 1000, 27); put_bits(&b, 0, 1);
    for (int c = 0; c < 3; ++c) put_bits(&b, 40000, 17);
    put_bits(&b, 5000, 17); put_bits(&b, 0, 4); put_bits(&b, 0, 10);
    put_bits(&b, 0, 1);
    put_bits(&b, 1, 1); put_bits(&b, 1024, 12); put_bits(&b, 2048, 12); put_bits(&b, 4, 4);
    const UINT anchors[4] = {614, 716, 819, 921};
    for (int i = 0; i < 4; ++i) put_bits(&b, anchors[i], 10);
    put_bits(&b, 0, 1);
    ID3D11VideoProcessorEnumerator *e = enumerator_of(D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, 16, 8, 16, 8);
    ID3D11VideoProcessor *vp = NULL;
    if (e) ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, 0, &vp);
    static UINT16 p010[16 * 8 * 3 / 2];
    const double grey = pq_signal(2000.0);
    for (int i = 0; i < 16 * 8; ++i) p010[i] = (UINT16)(lround(64 + 876 * grey) << 6);
    for (int i = 16 * 8; i < 16 * 8 * 3 / 2; ++i) p010[i] = 512 << 6;
    const D3D11_SUBRESOURCE_DATA init = {p010, 32, 0};
    ID3D11Texture2D *in = texture(16, 8, 1, DXGI_FORMAT_P010, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &init, NULL);
    ID3D11Texture2D *out = texture(16, 8, 1, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11VideoProcessorInputView *iv = input_view(in, e, 0);
    ID3D11VideoProcessorOutputView *ov = output_view(out, e, 0);
    ID3D11VideoContext2 *v2 = NULL;
    double nits[2] = {-1, -1};
    if (vp && iv && ov && SUCCEEDED(ID3D11VideoContext_QueryInterface(v, &IID_ID3D11VideoContext2, (void **)&v2))) {
      ID3D11VideoContext2_VideoProcessorSetStreamColorSpace1(v2, vp, 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
      ID3D11VideoContext2_VideoProcessorSetOutputColorSpace1(v2, vp, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
      ID3D11VideoContext2_VideoProcessorSetStreamAutoProcessingMode(v2, vp, 0, FALSE);
      DXGI_HDR_METADATA_HDR10 display, content;
      memset(&display, 0, sizeof display);
      memset(&content, 0, sizeof content);
      display.MaxMasteringLuminance = 1000;
      content.MaxMasteringLuminance = 4000;
      content.MaxContentLightLevel = 4000;
      ID3D11VideoContext2_VideoProcessorSetOutputHDRMetaData(v2, vp, DXGI_HDR_METADATA_TYPE_HDR10, sizeof display, &display);
      for (int k = 0; k < 2; ++k) {
        if (k == 0) ID3D11VideoContext2_VideoProcessorSetStreamHDRMetaData(v2, vp, 0, DXGI_HDR_METADATA_TYPE_HDR10, sizeof content, &content);
        else ID3D11VideoContext2_VideoProcessorSetStreamHDRMetaData(v2, vp, 0, DXGI_HDR_METADATA_TYPE_HDR10PLUS, sizeof meta, meta);
        D3D11_VIDEO_PROCESSOR_STREAM s = {TRUE, 0, 0, 0, 0, NULL, iv, NULL, NULL, NULL, NULL};
        static UINT32 px[16 * 8];
        if (SUCCEEDED(ID3D11VideoContext2_VideoProcessorBlt(v2, vp, ov, 0, 1, &s)) && read_bgra(out, px)) {
          const double e1 = ((px[4 * 16 + 8] >> 10) & 0x3FF) / 1023.0;      /* (green, PQ) */
          const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32, pp = pow(e1, 1 / m2);
          nits[k] = 10000 * pow(fmax(pp - c1, 0) / (c2 - c3 * pp), 1 / m1);
        }
      }
      ID3D11VideoContext2_Release(v2);
    }
    /* the curve: x = 2000 / 4000 = 0.5 past the knee 0.25: t = 1/3 on the Bezier curve of 0, 0.6, 0.7, 0.8, 0.9, 1 from y 0.5, times the target's 1000 */
    const double kx = 1024 / 4095.0, ky = 2048 / 4095.0, t = (0.5 - kx) / (1 - kx), pts[6] = {0, 614 / 1023.0, 716 / 1023.0, 819 / 1023.0, 921 / 1023.0, 1};
    double bez = 0, binom = 1;
    for (int k = 0; k <= 5; ++k) { bez += binom * pow(t, k) * pow(1 - t, 5 - k) * pts[k]; binom = binom * (5 - k) / (k + 1); }
    const double want_nits = (ky + (1 - ky) * bez) * 1000;
    checkf(fabs(nits[1] - want_nits) < 0.03 * want_nits && fabs(nits[0] - nits[1]) > 20,
           "HDR10+ metadata: 2000 cd/m2 -> %.0f cd/m2 on a 1000 cd/m2 display (the ST 2094-40 curve: %.0f; by the static levels: %.0f)", nits[1], want_nits, nits[0]);
    if (iv) ID3D11VideoProcessorInputView_Release(iv);
    if (ov) ID3D11VideoProcessorOutputView_Release(ov);
    if (in) ID3D11Texture2D_Release(in);
    if (out) ID3D11Texture2D_Release(out);
    if (vp) ID3D11VideoProcessor_Release(vp);
    if (e) ID3D11VideoProcessorEnumerator_Release(e);
  }
}

/* ---- downsampling while decoding, histograms ----------------------------------------------------------------------------------------------------------------------- */

/* h264_bframes decoded twice: as it is, and by a decoder that downsamples into half-size NV12 output views (DecoderEnableDownsampling), the Y, U and V histograms of
 * every other picture asked for (DecoderBeginFrame1). Each downsampled picture is what VideoProcessorBlt makes of the full-size one at that size, bit for bit; the
 * histograms are the full-size picture's counted here; after DecoderUpdateDownsampling the pictures are of the new size. */
static void test_decode_downsampling(void) {
  size_t size = 0;
  unsigned char *data = base64_decode(g_h264_bframes_b64, &size);
  DxvaStream s[2];
  DxvaPicture p[2];
  const unsigned surfaces = 17;
  if (!data || dxva_stream_open(&s[0], DXVA_CODEC_H264, data, size, surfaces) != 0 || dxva_stream_open(&s[1], DXVA_CODEC_H264, data, size, surfaces) != 0 ||
      dxva_stream_next(&s[0], &p[0]) != 1 || dxva_stream_next(&s[1], &p[1]) != 1) {
    check(0, "downsampling: the stream parses");
    free(data);
    return;
  }
  const UINT w = s[0].width, h = s[0].height, hw = w / 2, hh = h / 2;
  const D3D11_VIDEO_DECODER_DESC desc = {D3D11_DECODER_PROFILE_H264_VLD_NOFGT, w, h, DXGI_FORMAT_NV12};
  D3D11_VIDEO_DECODER_CONFIG config;
  video_config_of(&config, 2);
  ID3D11VideoDecoder *dec[2] = {NULL, NULL};
  for (int k = 0; k < 2; ++k) ID3D11VideoDevice_CreateVideoDecoder(g_vdevice, &desc, &config, &dec[k]);
  ID3D11VideoContext1 *c1 = NULL;
  ID3D11VideoContext_QueryInterface(g_vcontext, &IID_ID3D11VideoContext1, (void **)&c1);
  void *c3 = NULL;
  ID3D11VideoContext_QueryInterface(g_vcontext, &IID_ID3D11VideoContext3_, &c3);
  const DXGI_COLOR_SPACE_TYPE studio = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
  const D3D11_VIDEO_SAMPLE_DESC half = {hw, hh, DXGI_FORMAT_NV12, studio}, smaller = {w * 3 / 8, h * 3 / 8, DXGI_FORMAT_NV12, studio};
  if (!dec[0] || !dec[1] || !c1 || !c3) { check(0, "downsampling: two decoders, ID3D11VideoContext1 and 3"); goto done; }
  check(ID3D11VideoContext1_DecoderUpdateDownsampling(c1, dec[1], &half) == E_INVALIDARG, "DecoderUpdateDownsampling of a decoder that does not downsample: E_INVALIDARG");
  check(SUCCEEDED(ID3D11VideoContext1_DecoderEnableDownsampling(c1, dec[1], studio, &half, surfaces)), "DecoderEnableDownsampling: half size NV12");
  check(ID3D11VideoContext1_DecoderEnableDownsampling(c1, dec[1], studio, &half, surfaces) == E_INVALIDARG, "... again: E_INVALIDARG (DecoderUpdateDownsampling changes it)");
  {
    ID3D11Texture2D *full = texture(w, h, surfaces, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
    ID3D11Texture2D *small = texture(hw, hh, surfaces, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE, 0, NULL, NULL);
    ID3D11VideoDecoderOutputView *fv[DXVA_STREAM_MAX_SURFACES] = {0}, *sv[DXVA_STREAM_MAX_SURFACES] = {0};
    for (unsigned i = 0; full && small && i < surfaces; ++i) {
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd = {D3D11_DECODER_PROFILE_H264_VLD_NOFGT, D3D11_VDOV_DIMENSION_TEXTURE2D, {{i}}};
      ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)full, &vd, &fv[i]);
      ID3D11VideoDevice_CreateVideoDecoderOutputView(g_vdevice, (ID3D11Resource *)small, &vd, &sv[i]);
    }
    /* the processor that makes the reference pictures, and the histograms' buffer and its staging copy */
    const D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {30, 1}, w, h, {30, 1}, hw, hh, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL};
    ID3D11VideoProcessorEnumerator *e = NULL;
    ID3D11VideoDevice_CreateVideoProcessorEnumerator(g_vdevice, &cd, &e);
    ID3D11VideoProcessor *vp = NULL;
    if (e) ID3D11VideoDevice_CreateVideoProcessor(g_vdevice, e, 0, &vp);
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE bt709 = {0, 0, 1, 0, 1, 0};      /* (YCbCr BT.709, 16..235) */
    if (vp) {
      ID3D11VideoContext_VideoProcessorSetStreamColorSpace(g_vcontext, vp, 0, &bt709);
      ID3D11VideoContext_VideoProcessorSetOutputColorSpace(g_vcontext, vp, &bt709);
      ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(g_vcontext, vp, 0, FALSE);
    }
    ID3D11Texture2D *expect = texture(hw, hh, 1, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, NULL);
    ID3D11VideoProcessorOutputView *ev = output_view(expect, e, 0);
    const D3D11_BUFFER_DESC bd = {1024, D3D11_USAGE_DEFAULT, 0, 0, 0, 0}, sd = {1024, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer *hist = NULL, *hist_read = NULL;
    ID3D11Device_CreateBuffer(g_device, &bd, NULL, &hist);
    ID3D11Device_CreateBuffer(g_device, &sd, NULL, &hist_read);
    BYTE *a = malloc((size_t)w * h * 3 / 2), *b = malloc((size_t)w * h * 3 / 2), *ref = malloc((size_t)w * h * 3 / 2);
    int pictures = 0, same = 0, hist_ok = 0, updated_same = 0, r = 1;
    while (full && small && vp && ev && hist && hist_read && a && b && ref && r == 1 && pictures < 64) {
      if (pictures == 10) check(SUCCEEDED(ID3D11VideoContext1_DecoderUpdateDownsampling(c1, dec[1], &smaller)), "DecoderUpdateDownsampling: 3/8 of the size");
      const D3D11_VIDEO_SAMPLE_DESC *now = pictures >= 10 ? &smaller : &half;
      const int with_histograms = pictures % 2 == 1;
      int ok = decode_picture(dec[0], fv[p[0].surface], &p[0], sizeof(DXVA_Slice_H264_Short), offsetof(DXVA_Slice_H264_Short, BSNALunitDataLocation), 0);
      if (with_histograms) {
        const UINT offsets[4] = {0, 256, 512, 0};
        ID3D11Buffer *buffers[4] = {hist, hist, hist, NULL};
        ok = ok && SUCCEEDED(((DecoderBeginFrame1Fn)vtable_entry(c3, VIDEO_CONTEXT2_METHODS))(c3, dec[1], sv[p[1].surface], 0, NULL, 3, offsets, buffers));
        ok = ok && fill_buffer(dec[1], D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, p[1].picture_parameters, p[1].picture_parameters_size) &&
             fill_buffer(dec[1], D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 0, p[1].qmatrix, p[1].qmatrix_size) &&
             fill_buffer(dec[1], D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, p[1].slice_control, p[1].slice_control_size) &&
             fill_buffer(dec[1], D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, p[1].bitstream, p[1].bitstream_size);
        D3D11_VIDEO_DECODER_BUFFER_DESC bdesc[4] = {buffer_desc(D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, 0, p[1].picture_parameters_size),
                                                    buffer_desc(D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, 0, p[1].qmatrix_size),
                                                    buffer_desc(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, 0, p[1].slice_control_size),
                                                    buffer_desc(D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, 0, p[1].bitstream_size)};
        ok = ok && SUCCEEDED(ID3D11VideoContext_SubmitDecoderBuffers(g_vcontext, dec[1], 4, bdesc)) && SUCCEEDED(ID3D11VideoContext_DecoderEndFrame(g_vcontext, dec[1]));
      } else {
        ok = ok && decode_picture(dec[1], sv[p[1].surface], &p[1], sizeof(DXVA_Slice_H264_Short), offsetof(DXVA_Slice_H264_Short, BSNALunitDataLocation), 0);
      }
      /* the reference: the full-size picture through the processor at the output's size */
      ID3D11VideoProcessorInputView *iv = NULL;
      const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {0, D3D11_VPIV_DIMENSION_TEXTURE2D, {{0, p[0].surface}}};
      ID3D11VideoDevice_CreateVideoProcessorInputView(g_vdevice, (ID3D11Resource *)full, e, &ivd, &iv);
      const RECT target = {0, 0, (LONG)now->Width, (LONG)now->Height};
      ID3D11VideoContext_VideoProcessorSetOutputTargetRect(g_vcontext, vp, TRUE, &target);
      ID3D11VideoContext_VideoProcessorSetStreamDestRect(g_vcontext, vp, 0, TRUE, &target);
      D3D11_VIDEO_PROCESSOR_STREAM st = {TRUE, 0, 0, 0, 0, NULL, iv, NULL, NULL, NULL, NULL};
      ok = ok && iv && SUCCEEDED(ID3D11VideoContext_VideoProcessorBlt(g_vcontext, vp, ev, 0, 1, &st));
      if (iv) ID3D11VideoProcessorInputView_Release(iv);
      /* (the picture's region of the output: the rest of a slice is what an earlier, larger picture left) */
      ok = ok && read_planar(small, p[1].surface, a) && read_planar(expect, 0, b);
      int match = ok;
      for (UINT y = 0; match && y < now->Height; ++y) match = !memcmp(a + (size_t)y * hw, b + (size_t)y * hw, now->Width);
      for (UINT y = 0; match && y < now->Height / 2; ++y) match = !memcmp(a + (size_t)(hh + y) * hw, b + (size_t)(hh + y) * hw, now->Width);
      if (pictures >= 10) updated_same += match; else same += match;
      if (with_histograms && ok) {
        UINT want[3][64];
        memset(want, 0, sizeof want);
        if (read_planar(full, p[0].surface, ref)) {
          for (UINT i = 0; i < w * h; ++i) ++want[0][ref[i] >> 2];
          for (UINT i = 0; i < w * h / 4; ++i) { ++want[1][ref[w * h + 2 * i] >> 2]; ++want[2][ref[w * h + 2 * i + 1] >> 2]; }
        }
        ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)hist_read, (ID3D11Resource *)hist);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)hist_read, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
          int all = 1;
          for (int c = 0; c < 3; ++c) all &= !memcmp((const BYTE *)m.pData + 256 * c, want[c], sizeof want[c]);
          hist_ok += all;
          ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)hist_read, 0);
        }
      }
      ++pictures;
      r = dxva_stream_next(&s[0], &p[0]);
      if (dxva_stream_next(&s[1], &p[1]) != r) r = -1;
    }
    checkf(r == 0 && pictures == 14 && same == 10 && updated_same == 4,
           "downsampled decoding: %d of %d half-size pictures and %d of %d at 3/8 (after DecoderUpdateDownsampling) the processor's of the full-size ones, bit for bit", same, 10,
           updated_same, 4);
    checkf(hist_ok == 7, "DecoderBeginFrame1 histograms: %d of 7 pictures' Y, U and V the full-size picture's counted here", hist_ok);
    free(a); free(b); free(ref);
    if (hist) ID3D11Buffer_Release(hist);
    if (hist_read) ID3D11Buffer_Release(hist_read);
    if (ev) ID3D11VideoProcessorOutputView_Release(ev);
    if (expect) ID3D11Texture2D_Release(expect);
    if (vp) ID3D11VideoProcessor_Release(vp);
    if (e) ID3D11VideoProcessorEnumerator_Release(e);
    for (unsigned i = 0; i < surfaces; ++i) { if (fv[i]) ID3D11VideoDecoderOutputView_Release(fv[i]); if (sv[i]) ID3D11VideoDecoderOutputView_Release(sv[i]); }
    if (full) ID3D11Texture2D_Release(full);
    if (small) ID3D11Texture2D_Release(small);
  }
done:
  if (c3) IUnknown_Release((IUnknown *)c3);
  if (c1) ID3D11VideoContext1_Release(c1);
  for (int k = 0; k < 2; ++k) if (dec[k]) ID3D11VideoDecoder_Release(dec[k]);
  dxva_stream_close(&s[0]);
  dxva_stream_close(&s[1]);
  free(data);
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
      test_packed_yuv();
      test_packed_yuv_arrays();
      test_plane_array_and_integer_views();
      test_decoder_caps();
      decode_stream("h264_bframes", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_bframes_b64, 17, g_h264_bframes_fnv, 14, NULL, 0, 0);
      decode_stream("h264_cavlc", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_cavlc_b64, 17, g_h264_cavlc_fnv, 10, NULL, 1, 0);
      decode_stream("h264_ltr", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_ltr_b64, 20, g_h264_ltr_fnv, 24, NULL, 0, 0);
      decode_stream("h264_ltr (separate textures)", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_ltr_b64, 20, g_h264_ltr_fnv, 24, NULL, 0, 1);
      decode_stream("h264_bframes (separate textures)", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_bframes_b64, 17, g_h264_bframes_fnv, 14, NULL, 0, 1);
      decode_stream("h264_cqm", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_cqm_b64, 17, g_h264_cqm_fnv, 9, NULL, 2, 0);
      /* interlaced: MBAFF frames and field pictures (a field writes its lines of the slice; the frame is compared after its second field) */
      decode_stream("h264_paff_mbaff_cabac", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_paff_mbaff_cabac_b64, 17, g_h264_paff_mbaff_cabac_fnv, 8, NULL,
                    0, 0);
      /* gaps in frame_num: "non-existing" frames named with the surface of the reference before them, as ffmpeg does */
      decode_stream("h264_dxva_gaps", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_dxva_gaps_b64, 17, g_h264_dxva_gaps_fnv, 18, NULL, 0, 0);
      decode_stream("h264_dxva_gaps_p", DXVA_CODEC_H264, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, g_h264_dxva_gaps_p_b64, 17, g_h264_dxva_gaps_p_fnv, 19, NULL, 0, 1);
      for (unsigned i = 0; i < sizeof g_hevc10_streams / sizeof g_hevc10_streams[0]; ++i) decode_hevc10(&g_hevc10_streams[i]);
      for (unsigned i = 0; i < sizeof g_hevcrext_streams / sizeof g_hevcrext_streams[0]; ++i) decode_hevcrext(&g_hevcrext_streams[i]);
      decode_stream("mpeg2_ibp", DXVA_CODEC_MPEG2, &D3D11_DECODER_PROFILE_MPEG2_VLD, g_mpeg2_ibp_b64, 8, NULL, 8, g_mpeg2_ibp_means, 1, 0);
      decode_stream("mpeg2_ibp (separate textures)", DXVA_CODEC_MPEG2, &D3D11_DECODER_PROFILE_MPEG2_VLD, g_mpeg2_ibp_b64, 8, NULL, 8, g_mpeg2_ibp_means, 0, 1);
      decode_stream("vc1_adv_ipb", DXVA_CODEC_VC1, &D3D11_DECODER_PROFILE_VC1_VLD, g_vc1_adv_ipb_b64, 8, g_vc1_adv_ipb_fnv, 10, NULL, 0, 0);
      decode_stream("m4vdec_bframes", DXVA_CODEC_MPEG4, &D3D11_DECODER_PROFILE_MPEG4PT2_VLD_ADVSIMPLE_NOGMC, g_m4vdec_bframes_b64, 8, g_m4vdec_bframes_fnv, 8, NULL, 2, 0);
      decode_av1("av1_444 (profile 1, AYUV)", g_av1_444_dxva_b64, &DXVA_ModeAV1_VLD_Profile1, DXGI_FORMAT_AYUV, 4, g_av1_444_dxva_fnv, 6);
      decode_av1("av1_422_10bit (profile 2, Y210, film grain)", g_av1_422_10bit_dxva_b64, &DXVA_ModeAV1_VLD_Profile2, DXGI_FORMAT_Y210, 4, g_av1_422_10bit_dxva_fnv, 6);
      decode_av1("av1_444_12bit (12-bit profile 2, Y416)", g_av1_444_12bit_dxva_b64, &DXVA_ModeAV1_VLD_12bit_Profile2, DXGI_FORMAT_Y416, 8, g_av1_444_12bit_dxva_fnv, 6);
      /* large-scale tile decoding: the anchors, then tile lists (DXVA_Tile_AV1.anchor_frame) into 512 x 512 NV12 output frames, libaom's */
      decode_av1("av1_lst (large-scale tile, NV12)", g_av1_lst_dxva_b64, &DXVA_ModeAV1_VLD_Profile0, DXGI_FORMAT_NV12, 1, g_av1_lst_dxva_fnv, 3);
      test_decode_then_draw();
      test_processor();
      test_processor_effects();
      test_processor_rates();
      test_decode_downsampling();
    }
    test_content_protection();
  }
  /* (as d3d11test reports: what tests/d3d12_qemu.sh and tests/wow64_qemu.sh look for) */
  if (g_failed == 0) printf("d3d11videotest: all %d checks passed\n", g_passed);
  else printf("d3d11videotest: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  return g_failed ? 1 : 42;
}
