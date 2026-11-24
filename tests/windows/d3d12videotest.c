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
/* Direct3D 12 video (d3d12.dll video.inc over the kernel's DXVA decoders and video processor): planar NV12 / P010 textures and the narrow formats (footprints, copies through
 * buffers both ways, byte for byte), the video device's feature queries, decoders, heaps and processors made and refused, the video queues and lists, a video processor pass
 * (NV12 -> BGRA, scaled, against BT.709 computed here), and, with tests/windows/dxva_streams.h, an H.264 stream decoded through ID3D12VideoDecodeCommandList with its
 * statistics queried. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_4.h>
#include "../../src/Windows/d3d12/video_types.h"

static int checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)
#define CHECK_HR(x) do { HRESULT hr_ = (x); ++checks; if (FAILED(hr_)) { ++failed; printf("FAILED line %d: %s = %08lx\n", __LINE__, #x, (unsigned long)hr_); } } while (0)

static ID3D12Device *g_dev;
static ID3D12CommandQueue *g_queue;
static ID3D12Fence *g_fence;
static UINT64 g_value;

static void wait_queue(ID3D12CommandQueue *q) {
  HANDLE e = CreateEventW(NULL, FALSE, FALSE, NULL);
  ID3D12CommandQueue_Signal(q, g_fence, ++g_value);
  ID3D12Fence_SetEventOnCompletion(g_fence, g_value, e);
  WaitForSingleObject(e, 30000);
  CloseHandle(e);
}
static ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size) {
  D3D12_HEAP_PROPERTIES hp = {type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *r = NULL;
  ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                       &IID_ID3D12Resource, (void **)&r);
  return r;
}
static ID3D12Resource *texture(DXGI_FORMAT f, UINT w, UINT h, UINT layers, D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, (UINT16)layers, 1, f, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
  ID3D12Resource *r = NULL;
  ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&r);
  return r;
}
/* A direct list for copies. */
typedef struct Copier { ID3D12CommandAllocator *a; ID3D12GraphicsCommandList *l; } Copier;
static void copier_begin(Copier *c) {
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&c->a);
  ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->a, NULL, &IID_ID3D12GraphicsCommandList, (void **)&c->l);
}
static void copier_run(Copier *c) {
  ID3D12GraphicsCommandList_Close(c->l);
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)c->l};
  ID3D12CommandQueue_ExecuteCommandLists(g_queue, 1, lists);
  wait_queue(g_queue);
  ID3D12GraphicsCommandList_Release(c->l);
  ID3D12CommandAllocator_Release(c->a);
}
/* Subresources first .. first + n - 1 of `tex` from `data` (packed in the footprints' layout, as GetCopyableFootprints gives), or to it. */
static void copy_subresources(ID3D12Resource *tex, UINT first, UINT n, BYTE *data, UINT64 bytes, int upload) {
  D3D12_RESOURCE_DESC rd;
  tex->lpVtbl->GetDesc(tex, &rd);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[8];
  UINT rows[8];
  UINT64 row_bytes[8], total = 0;
  ID3D12Device_GetCopyableFootprints(g_dev, &rd, first, n, 0, fp, rows, row_bytes, &total);
  ID3D12Resource *buf = buffer(upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK, total);
  BYTE *map = NULL;
  ID3D12Resource_Map(buf, 0, NULL, (void **)&map);
  UINT64 at = 0;
  if (upload)
    for (UINT s = 0; s < n; ++s)
      for (UINT r = 0; r < rows[s] && at + row_bytes[s] <= bytes; ++r, at += row_bytes[s]) memcpy(map + fp[s].Offset + (UINT64)r * fp[s].Footprint.RowPitch, data + at, (size_t)row_bytes[s]);
  Copier c;
  copier_begin(&c);
  for (UINT s = 0; s < n; ++s) {
    D3D12_TEXTURE_COPY_LOCATION t = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = first + s}};
    D3D12_TEXTURE_COPY_LOCATION b = {buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = fp[s]}};
    if (upload) ID3D12GraphicsCommandList_CopyTextureRegion(c.l, &t, 0, 0, 0, &b, NULL);
    else ID3D12GraphicsCommandList_CopyTextureRegion(c.l, &b, 0, 0, 0, &t, NULL);
  }
  copier_run(&c);
  if (!upload) {
    ID3D12Resource_Unmap(buf, 0, NULL);
    ID3D12Resource_Map(buf, 0, NULL, (void **)&map);
    for (UINT s = 0; s < n; ++s)
      for (UINT r = 0; r < rows[s] && at + row_bytes[s] <= bytes; ++r, at += row_bytes[s]) memcpy(data + at, map + fp[s].Offset + (UINT64)r * fp[s].Footprint.RowPitch, (size_t)row_bytes[s]);
  }
  ID3D12Resource_Unmap(buf, 0, NULL);
  ID3D12Resource_Release(buf);
}

/* ---- planar textures and narrow formats ---------------------------------------------------------------------------------------------------------------------------- */

static void test_planar(void) {
  ID3D12Resource *nv12 = texture(DXGI_FORMAT_NV12, 64, 32, 2, D3D12_RESOURCE_FLAG_NONE);
  CHECK(nv12 != NULL);
  ID3D12Resource *odd = texture(DXGI_FORMAT_NV12, 63, 32, 1, D3D12_RESOURCE_FLAG_NONE);
  CHECK(odd == NULL);      /* (NV12 is of even width and height) */
  D3D12_FEATURE_DATA_FORMAT_INFO fi = {DXGI_FORMAT_NV12, 0};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_FORMAT_INFO, &fi, sizeof fi));
  CHECK(fi.PlaneCount == 2);
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {DXGI_FORMAT_NV12, 0, 0};
  CHECK_HR(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs));
  CHECK((fs.Support1 & D3D12_FORMAT_SUPPORT1_DECODER_OUTPUT) && (fs.Support1 & D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_INPUT));
  if (!nv12) return;
  /* (four subresources: plane 0 of slices 0 and 1, then plane 1 of both) */
  D3D12_RESOURCE_DESC rd;
  nv12->lpVtbl->GetDesc(nv12, &rd);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[4];
  UINT rows[4];
  UINT64 row_bytes[4], total = 0;
  ID3D12Device_GetCopyableFootprints(g_dev, &rd, 0, 4, 0, fp, rows, row_bytes, &total);
  CHECK(fp[0].Footprint.Width == 64 && fp[0].Footprint.Height == 32 && row_bytes[0] == 64 && fp[0].Footprint.Format == DXGI_FORMAT_R8_TYPELESS);
  CHECK(fp[2].Footprint.Width == 32 && fp[2].Footprint.Height == 16 && row_bytes[2] == 64 && fp[2].Footprint.Format == DXGI_FORMAT_R8G8_TYPELESS);
  /* a round trip of every plane of slice 1, byte for byte */
  static BYTE y[64 * 32], uv[64 * 16], y2[64 * 32], uv2[64 * 16];
  for (UINT i = 0; i < sizeof y; ++i) y[i] = (BYTE)(i * 7 + 3);
  for (UINT i = 0; i < sizeof uv; ++i) uv[i] = (BYTE)(i * 13 + 5);
  copy_subresources(nv12, 1, 1, y, sizeof y, 1);
  copy_subresources(nv12, 3, 1, uv, sizeof uv, 1);
  copy_subresources(nv12, 1, 1, y2, sizeof y2, 0);
  copy_subresources(nv12, 3, 1, uv2, sizeof uv2, 0);
  CHECK(memcmp(y, y2, sizeof y) == 0);
  CHECK(memcmp(uv, uv2, sizeof uv) == 0);
  /* slice 0 is untouched: zeros */
  memset(y2, 0xEE, sizeof y2);
  copy_subresources(nv12, 0, 1, y2, sizeof y2, 0);
  CHECK(y2[0] == 0 && y2[sizeof y2 - 1] == 0);
  /* an R8 texture and an R16G16 one: the footprints are narrow and the copies too */
  ID3D12Resource *r8 = texture(DXGI_FORMAT_R8_UNORM, 16, 4, 1, D3D12_RESOURCE_FLAG_NONE), *rg16 = texture(DXGI_FORMAT_R16G16_UNORM, 4, 2, 1, D3D12_RESOURCE_FLAG_NONE);
  CHECK(r8 && rg16);
  if (r8) {
    BYTE a[64], b[64];
    for (UINT i = 0; i < 64; ++i) a[i] = (BYTE)(200 - i);
    copy_subresources(r8, 0, 1, a, sizeof a, 1);
    copy_subresources(r8, 0, 1, b, sizeof b, 0);
    CHECK(memcmp(a, b, sizeof a) == 0);
    ID3D12Resource_Release(r8);
  }
  if (rg16) {
    BYTE a[32], b[32];
    for (UINT i = 0; i < 32; ++i) a[i] = (BYTE)(i * 9);
    copy_subresources(rg16, 0, 1, a, sizeof a, 1);
    copy_subresources(rg16, 0, 1, b, sizeof b, 0);
    CHECK(memcmp(a, b, sizeof a) == 0);
    ID3D12Resource_Release(rg16);
  }
  ID3D12Resource *p010 = texture(DXGI_FORMAT_P010, 16, 8, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  CHECK(p010 != NULL);      /* (a 16-bit planar render target: each plane its own image) */
  if (p010) ID3D12Resource_Release(p010);
  ID3D12Resource_Release(nv12);
}

/* ---- the video device ------------------------------------------------------------------------------------------------------------------------------------------------ */

static ID3D12VideoDevice *g_vdev;

static void test_features(void) {
  CHECK_HR(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12VideoDevice, (void **)&g_vdev));
  if (!g_vdev) return;
  ID3D12VideoDevice2 *v2 = NULL;
  CHECK_HR(ID3D12VideoDevice_QueryInterface(g_vdev, &IID_ID3D12VideoDevice2, (void **)&v2));
  ID3D12Device *back = NULL;
  CHECK_HR(ID3D12VideoDevice_QueryInterface(g_vdev, &IID_ID3D12Device, (void **)&back));
  CHECK(back == g_dev);
  if (back) ID3D12Device_Release(back);
  if (v2) ID3D12VideoDevice2_Release(v2);
  D3D12_FEATURE_DATA_VIDEO_FEATURE_AREA_SUPPORT area = {0};
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_FEATURE_AREA_SUPPORT, &area, sizeof area));
  CHECK(area.VideoDecodeSupport && area.VideoProcessSupport && area.VideoEncodeSupport);      /* (encoding: d3d12encodetest) */
  D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT count = {0};
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_PROFILE_COUNT, &count, sizeof count));
  CHECK(count.ProfileCount == 7);
  GUID profiles[16];
  D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES list = {0, count.ProfileCount, profiles};
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_PROFILES, &list, sizeof list));
  int h264 = 0, av1 = 0, main10 = 0;
  for (UINT i = 0; i < count.ProfileCount && i < 16; ++i) {
    h264 |= IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_H264);
    av1 |= IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0);
    main10 |= IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10);
  }
  CHECK(h264 && av1 && !main10);
  D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT ds;
  memset(&ds, 0, sizeof ds);
  ds.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_H264;
  ds.Width = 1920;
  ds.Height = 1088;
  ds.DecodeFormat = DXGI_FORMAT_NV12;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED && ds.DecodeTier == D3D12_VIDEO_DECODE_TIER_2);
  ds.DecodeFormat = DXGI_FORMAT_P010;      /* (H.264 here is 8 bits) */
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_NONE);
  ds.DecodeFormat = DXGI_FORMAT_NV12;
  ds.Configuration.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_FIELD_BASED;      /* (progressive only) */
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_NONE);
  ds.Configuration.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
  ds.Width = 8192;      /* (the planes are the kernel's textures: 4096 at most) */
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_NONE);
  CHECK(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds - 4) == E_INVALIDARG);
  DXGI_FORMAT formats[4];
  D3D12_FEATURE_DATA_VIDEO_DECODE_FORMAT_COUNT fc;
  memset(&fc, 0, sizeof fc);
  fc.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_FORMAT_COUNT, &fc, sizeof fc));
  CHECK(fc.FormatCount == 2);
  D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS ff = {0, fc.Configuration, fc.FormatCount, formats};
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_FORMATS, &ff, sizeof ff));
  CHECK(formats[0] == DXGI_FORMAT_NV12 && formats[1] == DXGI_FORMAT_P010);
  D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT ps;
  memset(&ps, 0, sizeof ps);
  ps.InputSample.Width = 1920;
  ps.InputSample.Height = 1080;
  ps.InputSample.Format.Format = DXGI_FORMAT_NV12;
  ps.InputSample.Format.ColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
  ps.OutputFormat.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  ps.OutputFormat.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &ps, sizeof ps));
  CHECK(ps.SupportFlags == D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED && (ps.FeatureSupport & D3D12_VIDEO_PROCESS_FEATURE_FLAG_ROTATION) && ps.DeinterlaceSupport == 0);
  ps.OutputFormat.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;      /* (HDR: no tone mapping) */
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &ps, sizeof ps));
  CHECK(ps.SupportFlags == D3D12_VIDEO_PROCESS_SUPPORT_FLAG_NONE);
  D3D12_FEATURE_DATA_VIDEO_PROCESS_MAX_INPUT_STREAMS mis = {0, 0};
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_MAX_INPUT_STREAMS, &mis, sizeof mis));
  CHECK(mis.MaxInputStreams == 16);
  CHECK(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, (D3D12_FEATURE_VIDEO)99, &mis, sizeof mis) == E_INVALIDARG);
  /* made and refused */
  D3D12_VIDEO_DECODER_DESC dd = {0, {D3D12_VIDEO_DECODE_PROFILE_H264, D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE, D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE}};
  ID3D12VideoDecoder *dec = NULL, *bad = NULL;
  CHECK_HR(ID3D12VideoDevice_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&dec));
  dd.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_VC1;
  CHECK(ID3D12VideoDevice_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&bad) == E_INVALIDARG && !bad);
  if (dec) {
    ID3D12VideoDecoder1 *d1 = NULL;
    CHECK_HR(ID3D12VideoDecoder_QueryInterface(dec, &IID_ID3D12VideoDecoder1, (void **)&d1));
    if (d1) {
      void *session = (void *)1;
      CHECK(d1->lpVtbl->GetProtectedResourceSession(d1, &IID_ID3D12ProtectedResourceSession, &session) == DXGI_ERROR_NOT_FOUND && !session);
      d1->lpVtbl->Release(d1);
    }
    ID3D12VideoDecoder_Release(dec);
  }
  D3D12_VIDEO_DECODER_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
  hd.DecodeWidth = 640;
  hd.DecodeHeight = 368;
  hd.Format = DXGI_FORMAT_NV12;
  hd.MaxDecodePictureBufferCount = 16;
  ID3D12VideoDecoderHeap *heap = NULL;
  CHECK_HR(ID3D12VideoDevice_CreateVideoDecoderHeap(g_vdev, &hd, &IID_ID3D12VideoDecoderHeap, (void **)&heap));
  if (heap) {
    D3D12_VIDEO_DECODER_HEAP_DESC got;
    heap->lpVtbl->GetDesc(heap, &got);
    CHECK(got.DecodeWidth == 640 && got.MaxDecodePictureBufferCount == 16);
    ID3D12VideoDecoderHeap_Release(heap);
  }
  hd.Format = DXGI_FORMAT_P010;      /* (HEVC Main is 8 bits) */
  heap = NULL;
  CHECK(ID3D12VideoDevice_CreateVideoDecoderHeap(g_vdev, &hd, &IID_ID3D12VideoDecoderHeap, (void **)&heap) == E_INVALIDARG && !heap);
  /* an encoder of nothing is refused, an HEVC one is not there (tests/windows/d3d12encodetest.c tests encoding) */
  ID3D12VideoDevice3 *v3 = NULL;
  CHECK_HR(ID3D12VideoDevice_QueryInterface(g_vdev, &IID_ID3D12VideoDevice3, (void **)&v3));
  if (v3) {
    D3D12_VIDEO_ENCODER_DESC ed;
    memset(&ed, 0, sizeof ed);
    void *enc = (void *)1;
    CHECK(ID3D12VideoDevice3_CreateVideoEncoder(v3, &ed, &IID_ID3D12VideoEncoder, &enc) == E_INVALIDARG && !enc);
    ed.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    enc = (void *)1;
    CHECK(ID3D12VideoDevice3_CreateVideoEncoder(v3, &ed, &IID_ID3D12VideoEncoder, &enc) == DXGI_ERROR_UNSUPPORTED && !enc);
    ID3D12VideoDevice3_Release(v3);
  }
  /* the video queues, allocators and lists */
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *vq = NULL;
  CHECK_HR(ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&vq));
  ID3D12CommandAllocator *va = NULL;
  CHECK_HR(ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, &IID_ID3D12CommandAllocator, (void **)&va));
  ID3D12VideoDecodeCommandList *vl = NULL;
  if (va) CHECK_HR(ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, va, NULL, &IID_ID3D12VideoDecodeCommandList, (void **)&vl));
  if (vl) {
    CHECK(ID3D12VideoDecodeCommandList_GetType(vl) == D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE);
    ID3D12GraphicsCommandList *gl = NULL;
    CHECK(ID3D12VideoDecodeCommandList_QueryInterface(vl, &IID_ID3D12GraphicsCommandList, (void **)&gl) == E_NOINTERFACE && !gl);
    CHECK_HR(ID3D12VideoDecodeCommandList_Close(vl));
    CHECK_HR(ID3D12VideoDecodeCommandList_Reset(vl, va));
    CHECK_HR(ID3D12VideoDecodeCommandList_Close(vl));
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)vl};
    ID3D12CommandQueue_ExecuteCommandLists(vq, 1, lists);      /* (an empty list runs) */
    ID3D12VideoDecodeCommandList_Release(vl);
  }
  if (va) ID3D12CommandAllocator_Release(va);
  if (vq) ID3D12CommandQueue_Release(vq);
}

/* ---- the video processor ------------------------------------------------------------------------------------------------------------------------------------------- */

/* BT.709 studio range YCbCr (8 bits) to full range RGB with the gamma kept (what a G22 -> G22 conversion is): 0..255 */
static void bt709(int y, int cb, int cr, double rgb[3]) {
  const double Y = (y - 16) / 219.0, Pb = (cb - 128) / 224.0, Pr = (cr - 128) / 224.0;
  rgb[0] = Y + 1.5748 * Pr;
  rgb[1] = Y - 0.1873 * Pb - 0.4681 * Pr;
  rgb[2] = Y + 1.8556 * Pb;
  for (int c = 0; c < 3; ++c) rgb[c] = rgb[c] < 0 ? 0 : rgb[c] > 1 ? 255 : rgb[c] * 255;
}

static void test_processor(void) {
  if (!g_vdev) return;
  /* an NV12 picture of flat 4 x 4 blocks of colour (so bilinear scaling keeps the block centres exact), 32 x 16, made 64 x 32 in BGRA */
  ID3D12Resource *in = texture(DXGI_FORMAT_NV12, 32, 16, 1, D3D12_RESOURCE_FLAG_NONE);
  ID3D12Resource *out = texture(DXGI_FORMAT_B8G8R8A8_UNORM, 80, 40, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  CHECK(in && out);
  if (!in || !out) return;
  static BYTE y[32 * 16], uv[32 * 8];
  for (UINT r = 0; r < 16; ++r)
    for (UINT x = 0; x < 32; ++x) y[r * 32 + x] = (BYTE)(40 + 20 * (x / 8) + 30 * (r / 8));
  for (UINT r = 0; r < 8; ++r)
    for (UINT x = 0; x < 16; ++x) { uv[r * 32 + 2 * x] = (BYTE)(100 + 10 * (x / 4)); uv[r * 32 + 2 * x + 1] = (BYTE)(160 - 15 * (r / 4)); }
  copy_subresources(in, 0, 1, y, sizeof y, 1);
  copy_subresources(in, 1, 1, uv, sizeof uv, 1);
  D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC isd;
  memset(&isd, 0, sizeof isd);
  isd.Format = DXGI_FORMAT_NV12;
  isd.ColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
  isd.SourceAspectRatio.Numerator = isd.SourceAspectRatio.Denominator = 1;
  isd.DestinationAspectRatio = isd.SourceAspectRatio;
  isd.FrameRate.Numerator = 30; isd.FrameRate.Denominator = 1;
  isd.SourceSizeRange.MaxWidth = isd.SourceSizeRange.MaxHeight = 4096; isd.SourceSizeRange.MinWidth = isd.SourceSizeRange.MinHeight = 1;
  isd.DestinationSizeRange = isd.SourceSizeRange;
  isd.EnableOrientation = TRUE;
  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC osd;
  memset(&osd, 0, sizeof osd);
  osd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  osd.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  osd.AlphaFillMode = D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_OPAQUE;
  osd.BackgroundColor[0] = 1.0f; osd.BackgroundColor[3] = 1.0f;      /* (red where no picture goes) */
  osd.FrameRate = isd.FrameRate;
  ID3D12VideoProcessor *vp = NULL;
  CHECK_HR(ID3D12VideoDevice_CreateVideoProcessor(g_vdev, 0, &osd, 1, &isd, &IID_ID3D12VideoProcessor, (void **)&vp));
  D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC deint = isd;
  deint.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_BOB;
  ID3D12VideoProcessor *bad = NULL;
  CHECK(ID3D12VideoDevice_CreateVideoProcessor(g_vdev, 0, &osd, 1, &deint, &IID_ID3D12VideoProcessor, (void **)&bad) == E_INVALIDARG && !bad);
  if (!vp) return;
  CHECK(ID3D12VideoProcessor_GetNumInputStreamDescs(vp) == 1);
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *pq = NULL;
  ID3D12CommandAllocator *pa = NULL;
  ID3D12VideoProcessCommandList *pl = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&pq);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, &IID_ID3D12CommandAllocator, (void **)&pa);
  if (pa) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, pa, NULL, &IID_ID3D12VideoProcessCommandList, (void **)&pl);
  CHECK(pq && pa && pl);
  if (pq && pl) {
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS ia;
    memset(&ia, 0, sizeof ia);
    ia.InputStream[0].pTexture2D = in;
    ia.Transform.SourceRectangle = (D3D12_RECT){0, 0, 32, 16};
    ia.Transform.DestinationRectangle = (D3D12_RECT){8, 4, 72, 36};      /* (twice the size, inside a red frame) */
    ia.Transform.Orientation = D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT;
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS oa;
    memset(&oa, 0, sizeof oa);
    oa.OutputStream[0].pTexture2D = out;
    oa.TargetRectangle = (D3D12_RECT){0, 0, 80, 40};
    ID3D12VideoProcessCommandList_ProcessFrames(pl, vp, &oa, 1, &ia);
    CHECK_HR(ID3D12VideoProcessCommandList_Close(pl));
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)pl};
    ID3D12CommandQueue_ExecuteCommandLists(pq, 1, lists);
    wait_queue(pq);
    static BYTE px[80 * 40 * 4];
    copy_subresources(out, 0, 1, px, sizeof px, 0);
    /* the frame is the background */
    CHECK(px[2] == 255 && px[1] == 0 && px[0] == 0 && px[3] == 255);
    const UINT edge = (39 * 80 + 79) * 4;
    CHECK(px[edge + 2] == 255 && px[edge + 1] == 0);
    /* the centre of each 4 x 4 chroma block (8 x 8 output pixels): the colour of that block */
    int worst = 0;
    for (UINT by = 0; by < 2; ++by)
      for (UINT bx = 0; bx < 4; ++bx) {
        const UINT sx = bx * 8 + 4, sy = by * 8 + 4;      /* (a source pixel inside the block, away from the luma edges) */
        const UINT ox = 8 + sx * 2, oy = 4 + sy * 2;
        double rgb[3];
        bt709(y[sy * 32 + sx], uv[(sy / 2) * 32 + (sx / 2) * 2], uv[(sy / 2) * 32 + (sx / 2) * 2 + 1], rgb);
        const BYTE *p = &px[(oy * 80 + ox) * 4];
        for (int c = 0; c < 3; ++c) {
          const int d = abs((int)p[2 - c] - (int)lround(rgb[c]));
          if (d > worst) worst = d;
        }
      }
    printf("d3d12videotest: processor NV12 -> BGRA largest difference from BT.709: %d\n", worst);
    CHECK(worst <= 2);
    ID3D12VideoProcessCommandList_Release(pl);
  }
  if (pa) ID3D12CommandAllocator_Release(pa);
  if (pq) ID3D12CommandQueue_Release(pq);
  ID3D12VideoProcessor_Release(vp);
  ID3D12Resource_Release(in);
  ID3D12Resource_Release(out);
}

#include "dxva_streams.h"
#include "d3d11video_data.h"
static void test_decode(void);

int main(void) {
  if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&g_dev)) || !g_dev) {
    printf("d3d12videotest: no device\nd3d12videotest: 1 checks, 1 failed\n");
    return 1;
  }
  /* (what the device draws with: the video work runs on the CPU in the kernel whatever it is, the copies and views on it) */
  IDXGIFactory4 *factory = NULL;
  IDXGIAdapter1 *adapter = NULL;
  if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory)) && SUCCEEDED(IDXGIFactory4_EnumAdapters1(factory, 0, &adapter))) {
    DXGI_ADAPTER_DESC1 d;
    if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &d))) printf("device: %s\n", (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "software (the CPU renderer)" : "hardware");
    IDXGIAdapter1_Release(adapter);
  }
  if (factory) IDXGIFactory4_Release(factory);
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&g_queue);
  ID3D12Device_CreateFence(g_dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&g_fence);
  test_planar();
  test_features();
  test_processor();
  test_decode();
  if (g_vdev) ID3D12VideoDevice_Release(g_vdev);
  ID3D12Fence_Release(g_fence);
  ID3D12CommandQueue_Release(g_queue);
  ID3D12Device_Release(g_dev);
  printf("d3d12videotest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}

/* ---- decoding ------------------------------------------------------------------------------------------------------------------------------------------------------ */

static unsigned char *base64_decode(const char *t, size_t *size) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t n = strlen(t);
  unsigned char *out = malloc(n / 4 * 3 + 3);
  size_t o = 0;
  unsigned v = 0, bits = 0;
  for (size_t i = 0; i < n && out; ++i) {
    if (t[i] == '=') break;
    const char *at = strchr(table, t[i]);
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
typedef struct Decoded { long long key; unsigned hash; } Decoded;
static int by_key(const void *a, const void *b) {
  const long long x = ((const Decoded *)a)->key, y = ((const Decoded *)b)->key;
  return x < y ? -1 : x > y;
}

/* An H.264 stream decoded through ID3D12VideoDecodeCommandList, picture by picture, compared in display order with ffmpeg's (FNV hashes of the visible I420 picture).
 * `separate`: every surface a texture of its own (the reference frames are different resources at subresource 0), else one NV12 array (the indices are its slices).
 * The decode statistics of every picture are queried and resolved. */
static void decode_h264(const char *name, const char *b64, unsigned surfaces, const unsigned *hashes, unsigned frames, int separate) {
  size_t size = 0;
  unsigned char *data = base64_decode(b64, &size);
  DxvaStream s;
  DxvaPicture p;
  if (!data || dxva_stream_open(&s, DXVA_CODEC_H264, data, size, surfaces) != 0 || dxva_stream_next(&s, &p) != 1) { CHECK(0); free(data); return; }
  D3D12_VIDEO_DECODER_DESC dd = {0, {D3D12_VIDEO_DECODE_PROFILE_H264, D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE, D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE}};
  D3D12_VIDEO_DECODER_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.Configuration = dd.Configuration;
  hd.DecodeWidth = s.width;
  hd.DecodeHeight = s.height;
  hd.Format = DXGI_FORMAT_NV12;
  hd.MaxDecodePictureBufferCount = surfaces;
  ID3D12VideoDecoder *dec = NULL;
  ID3D12VideoDecoderHeap *heap = NULL;
  CHECK_HR(ID3D12VideoDevice_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&dec));
  CHECK_HR(ID3D12VideoDevice_CreateVideoDecoderHeap(g_vdev, &hd, &IID_ID3D12VideoDecoderHeap, (void **)&heap));
  ID3D12Resource *tex[DXVA_STREAM_MAX_SURFACES] = {0};
  UINT subs[DXVA_STREAM_MAX_SURFACES];
  ID3D12Resource *array = separate ? NULL : texture(DXGI_FORMAT_NV12, s.width, s.height, surfaces, D3D12_RESOURCE_FLAG_NONE);
  for (unsigned i = 0; i < surfaces; ++i) {
    tex[i] = separate ? texture(DXGI_FORMAT_NV12, s.width, s.height, 1, D3D12_RESOURCE_FLAG_NONE) : array;
    subs[i] = separate ? 0 : i;
  }
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *vq = NULL;
  ID3D12CommandAllocator *va = NULL;
  ID3D12VideoDecodeCommandList *vl = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&vq);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, &IID_ID3D12CommandAllocator, (void **)&va);
  if (va) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, va, NULL, &IID_ID3D12VideoDecodeCommandList, (void **)&vl);
  D3D12_QUERY_HEAP_DESC qhd = {D3D12_QUERY_HEAP_TYPE_VIDEO_DECODE_STATISTICS, 256, 0};
  ID3D12QueryHeap *stats = NULL;
  CHECK_HR(ID3D12Device_CreateQueryHeap(g_dev, &qhd, &IID_ID3D12QueryHeap, (void **)&stats));
  ID3D12Resource *bits = buffer(D3D12_HEAP_TYPE_UPLOAD, 1 << 20);
  CHECK(dec && heap && vq && vl && stats && bits && tex[0]);
  const UINT vw = s.width - s.crop_left - s.crop_right, vh = s.height - s.crop_top - s.crop_bottom, cw = (vw + 1) / 2, ch = (vh + 1) / 2;
  BYTE *y = malloc((size_t)s.width * s.height), *uv = malloc((size_t)s.width * s.height / 2), *f = malloc((size_t)vw * vh + 2 * cw * ch);
  Decoded decoded[256];
  unsigned n = 0, statuses_ok = 0;
  int r = 1;
  while (dec && heap && vl && bits && tex[0] && r == 1 && n < 256) {
    /* (the compressed data in an upload buffer, the arguments as the program's own memory) */
    BYTE *map = NULL;
    ID3D12Resource_Map(bits, 0, NULL, (void **)&map);
    memcpy(map, p.bitstream, p.bitstream_size);
    ID3D12Resource_Unmap(bits, 0, NULL);
    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in;
    memset(&in, 0, sizeof in);
    in.NumFrameArguments = 3;
    in.FrameArguments[0] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, p.picture_parameters_size, (void *)p.picture_parameters};
    in.FrameArguments[1] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, p.qmatrix_size, (void *)p.qmatrix};
    in.FrameArguments[2] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, p.slice_control_size, (void *)p.slice_control};
    in.ReferenceFrames.NumTexture2Ds = surfaces;
    in.ReferenceFrames.ppTexture2Ds = tex;
    in.ReferenceFrames.pSubresources = subs;
    in.CompressedBitstream.pBuffer = bits;
    in.CompressedBitstream.Size = p.bitstream_size;
    in.pHeap = heap;
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS out;
    memset(&out, 0, sizeof out);
    out.pOutputTexture2D = tex[p.surface];
    out.OutputSubresource = subs[p.surface];
    ID3D12VideoDecodeCommandList_DecodeFrame(vl, dec, &out, &in);
    ID3D12VideoDecodeCommandList_EndQuery(vl, stats, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, n);
    ID3D12VideoDecodeCommandList_Close(vl);
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)vl};
    ID3D12CommandQueue_ExecuteCommandLists(vq, 1, lists);
    wait_queue(vq);
    ID3D12VideoDecodeCommandList_Reset(vl, va);
    /* (the luma plane of the slice is subresource `slice`, the chroma `ArraySize + slice`) */
    const UINT layers = separate ? 1 : surfaces;
    copy_subresources(tex[p.surface], subs[p.surface], 1, y, (size_t)s.width * s.height, 0);
    copy_subresources(tex[p.surface], layers + subs[p.surface], 1, uv, (size_t)s.width * s.height / 2, 0);
    for (UINT row = 0; row < vh; ++row) memcpy(f + (size_t)row * vw, y + (size_t)(s.crop_top + row) * s.width + s.crop_left, vw);
    for (UINT row = 0; row < ch; ++row)
      for (UINT x = 0; x < cw; ++x) {
        const BYTE *c = uv + (size_t)(s.crop_top / 2 + row) * s.width + 2 * (s.crop_left / 2 + x);
        f[vw * vh + row * cw + x] = c[0];
        f[vw * vh + cw * ch + row * cw + x] = c[1];
      }
    decoded[n].key = p.display_key;
    decoded[n].hash = fnv(f, (size_t)vw * vh + 2 * cw * ch, 0x811c9dc5u);
    ++n;
    r = dxva_stream_next(&s, &p);
  }
  CHECK(r == 0 && n == frames);
  qsort(decoded, n, sizeof *decoded, by_key);
  unsigned same = 0;
  for (unsigned i = 0; i < n && i < frames; ++i) same += decoded[i].hash == hashes[i];
  printf("d3d12videotest: %s (%s): %u of %u frames are ffmpeg's, bit for bit\n", name, separate ? "a texture a surface" : "an array", same, frames);
  CHECK(same == frames);
  /* every picture's statistics: decoded, nothing affected */
  if (stats && n) {
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, 256 * sizeof(D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS));
    Copier c;
    copier_begin(&c);
    ID3D12GraphicsCommandList_ResolveQueryData(c.l, stats, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, 0, n, rb, 0);
    copier_run(&c);
    D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS *st = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&st);
    for (unsigned i = 0; st && i < n; ++i) statuses_ok += st[i].Status == D3D12_VIDEO_DECODE_STATUS_OK && st[i].NumMacroblocksAffected == 0;
    ID3D12Resource_Unmap(rb, 0, NULL);
    ID3D12Resource_Release(rb);
    CHECK(statuses_ok == n);
  }
  free(y); free(uv); free(f);
  if (bits) ID3D12Resource_Release(bits);
  if (stats) ID3D12QueryHeap_Release(stats);
  if (vl) ID3D12VideoDecodeCommandList_Release(vl);
  if (va) ID3D12CommandAllocator_Release(va);
  if (vq) ID3D12CommandQueue_Release(vq);
  if (separate) { for (unsigned i = 0; i < surfaces; ++i) if (tex[i]) ID3D12Resource_Release(tex[i]); }
  else if (array) ID3D12Resource_Release(array);
  if (heap) ID3D12VideoDecoderHeap_Release(heap);
  if (dec) ID3D12VideoDecoder_Release(dec);
  dxva_stream_close(&s);
  free(data);
}

static void test_decode(void) {
  if (!g_vdev) return;
  decode_h264("h264_bframes", g_h264_bframes_b64, 17, g_h264_bframes_fnv, 14, 0);
  decode_h264("h264_ltr", g_h264_ltr_b64, 20, g_h264_ltr_fnv, 24, 1);
  decode_h264("h264_cqm", g_h264_cqm_b64, 17, g_h264_cqm_fnv, 9, 0);
}
