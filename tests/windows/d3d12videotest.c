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
 * (NV12 -> BGRA, scaled, against BT.709 computed here), and, with tests/windows/dxva_streams.h, H.264, VC-1 and MPEG-4 Part 2 streams decoded through ID3D12VideoDecodeCommandList with their
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

/* (MPEG-4 Part 2 Advanced Simple with GMC: DXVA_ModeMPEG4pt2_VLD_AdvSimple_GMC, which d3d12video.h does not name) */
static const GUID g_mpeg4pt2_advsimple_gmc = {0xab998b5b, 0x4258, 0x44a9, {0x9f, 0xeb, 0x94, 0xe5, 0x97, 0xa6, 0xba, 0xae}};
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
  CHECK(count.ProfileCount == 17);      /* (H.264, MPEG-2, MPEG-1/2, VP9 0 and 2, HEVC Main and Main 10, AV1 0, VC-1 (two), MPEG-4 Part 2 (three), AV1 1, 2, 12-bit 2 and 12-bit 2 4:2:0) */
  GUID profiles[24];
  D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES list = {0, count.ProfileCount, profiles};
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_PROFILES, &list, sizeof list));
  int h264 = 0, av1 = 0, main10 = 0, vc1 = 0, mpeg4 = 0, av1_high = 0;
  for (UINT i = 0; i < count.ProfileCount && i < 24; ++i) {
    h264 |= IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_H264);
    av1 |= IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0);
    av1_high += IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE1) + IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE2) +
                IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_AV1_12BIT_PROFILE2) + IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_AV1_12BIT_PROFILE2_420);
    main10 |= IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10);
    vc1 += IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_VC1) + IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_VC1_D2010);
    mpeg4 += IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_MPEG4PT2_SIMPLE) + IsEqualGUID(&profiles[i], &D3D12_VIDEO_DECODE_PROFILE_MPEG4PT2_ADVSIMPLE_NOGMC) +
             IsEqualGUID(&profiles[i], &g_mpeg4pt2_advsimple_gmc);
  }
  CHECK(h264 && av1 && main10 && vc1 == 2 && mpeg4 == 3 && av1_high == 4);
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
  ds.Configuration.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_FIELD_BASED;      /* (H.264 field pictures and MBAFF frames: 32 lines aligned) */
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED && ds.ConfigurationFlags == D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_HEIGHT_ALIGNMENT_MULTIPLE_32_REQUIRED);
  ds.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_VP9;      /* (the other profiles are progressive here) */
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_NONE);
  ds.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_H264;
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
  /* HEVC Main 10: into P010 only */
  fc.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_FORMAT_COUNT, &fc, sizeof fc));
  CHECK(fc.FormatCount == 1);
  ff.Configuration = fc.Configuration;
  ff.FormatCount = 1;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_FORMATS, &ff, sizeof ff));
  CHECK(formats[0] == DXGI_FORMAT_P010);
  ds.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10;
  ds.Width = 1920;
  ds.Height = 1080;
  ds.DecodeFormat = DXGI_FORMAT_P010;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED);
  ds.DecodeFormat = DXGI_FORMAT_NV12;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_NONE);
  /* AV1 profiles 1 and 2: the formats Windows' decoders write them in */
  static const struct { const GUID *profile; UINT n; DXGI_FORMAT f[4]; } av1_formats[] = {
      {&D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE1, 2, {DXGI_FORMAT_AYUV, DXGI_FORMAT_Y410}},
      {&D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE2, 4, {DXGI_FORMAT_YUY2, DXGI_FORMAT_Y210, DXGI_FORMAT_NV12, DXGI_FORMAT_P010}},
      {&D3D12_VIDEO_DECODE_PROFILE_AV1_12BIT_PROFILE2, 2, {DXGI_FORMAT_Y216, DXGI_FORMAT_Y416}},
      {&D3D12_VIDEO_DECODE_PROFILE_AV1_12BIT_PROFILE2_420, 1, {DXGI_FORMAT_P016}}};
  for (UINT k = 0; k < 4; ++k) {
    fc.Configuration.DecodeProfile = *av1_formats[k].profile;
    CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_FORMAT_COUNT, &fc, sizeof fc));
    CHECK(fc.FormatCount == av1_formats[k].n);
    D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS f2 = {0, fc.Configuration, fc.FormatCount, formats};
    CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_FORMATS, &f2, sizeof f2));
    for (UINT i = 0; i < av1_formats[k].n && i < fc.FormatCount; ++i) CHECK(formats[i] == av1_formats[k].f[i]);
    memset(&ds, 0, sizeof ds);
    ds.Configuration = fc.Configuration;
    ds.Width = 1920;
    ds.Height = 1080;
    ds.DecodeFormat = av1_formats[k].f[0];
    CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
    CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED);
    ds.DecodeFormat = k == 3 ? DXGI_FORMAT_P010 : DXGI_FORMAT_P016;      /* (not one of the profile's) */
    CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
    CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_NONE);
  }
  fc.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0;
  /* the processor's packed formats */
  {
    D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT yps;
    memset(&yps, 0, sizeof yps);
    yps.InputSample.Width = 64;
    yps.InputSample.Height = 64;
    yps.InputSample.Format.Format = DXGI_FORMAT_Y416;
    yps.InputSample.Format.ColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    yps.OutputFormat.Format = DXGI_FORMAT_YUY2;
    yps.OutputFormat.ColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &yps, sizeof yps));
    CHECK(yps.SupportFlags == D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED);
    yps.InputSample.Format.Format = DXGI_FORMAT_V408;      /* (not the processor's) */
    CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &yps, sizeof yps));
    CHECK(yps.SupportFlags == D3D12_VIDEO_PROCESS_SUPPORT_FLAG_NONE);
  }
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
  ps.OutputFormat.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;      /* (SDR into HDR10) */
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &ps, sizeof ps));
  CHECK(ps.SupportFlags == D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED);
  ps.OutputFormat.ColorSpace = (DXGI_COLOR_SPACE_TYPE)4;      /* (reserved) */
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
  dd.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_H264_STEREO;      /* (no MVC) */
  CHECK(ID3D12VideoDevice_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&bad) == E_INVALIDARG && !bad);
  dd.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_VC1;
  CHECK_HR(ID3D12VideoDevice_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&bad));
  if (bad) { ID3D12VideoDecoder_Release(bad); bad = NULL; }
  ds.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_MPEG4PT2_ADVSIMPLE_NOGMC;      /* (VC-1 and MPEG-4 Part 2: NV12, progressive) */
  ds.DecodeFormat = DXGI_FORMAT_NV12;
  ds.Width = 720;
  ds.Height = 576;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED);
  ds.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_VC1_D2010;
  ds.DecodeFormat = DXGI_FORMAT_P010;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_NONE);
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
  deint.EnableAutoProcessing = TRUE;      /* (there is no automatic processing) */
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

/* ---- packed YUV (AYUV, Y410, Y416, YUY2, Y210, Y216) and planar 4:2:2 / 4:4:0 / 4:4:4 (P208, V208, V408) textures ------------------------------------------------ */

static D3D12_FORMAT_SUPPORT1 support_of(DXGI_FORMAT f) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {f, 0, 0};
  return SUCCEEDED(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ? fs.Support1 : 0;
}
static UINT plane_count(DXGI_FORMAT f) {
  D3D12_FEATURE_DATA_FORMAT_INFO fi = {f, 0};
  return SUCCEEDED(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_FORMAT_INFO, &fi, sizeof fi)) ? fi.PlaneCount : 0;
}

static void test_yuv_formats(void) {
  /* what each packed format is: the bytes of a row of 16 pixels (DXGI: AYUV and Y410 4 a pixel, Y416 8; YUY2 4 a pair, Y210 / Y216 8 a pair) */
  static const struct { DXGI_FORMAT f; const char *name; UINT row; int target; } packed[] = {
      {DXGI_FORMAT_AYUV, "AYUV", 64, 1}, {DXGI_FORMAT_Y410, "Y410", 64, 0}, {DXGI_FORMAT_Y416, "Y416", 128, 0},
      {DXGI_FORMAT_YUY2, "YUY2", 32, 0}, {DXGI_FORMAT_Y210, "Y210", 64, 0}, {DXGI_FORMAT_Y216, "Y216", 64, 0}};
  int all = 1;
  for (UINT i = 0; i < sizeof packed / sizeof packed[0]; ++i) {
    const DXGI_FORMAT f = packed[i].f;
    const D3D12_FORMAT_SUPPORT1 s = support_of(f);
    CHECK((s & D3D12_FORMAT_SUPPORT1_TEXTURE2D) && (s & D3D12_FORMAT_SUPPORT1_DECODER_OUTPUT) && (s & D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_INPUT) &&
          (s & D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_OUTPUT) && !!(s & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == packed[i].target && plane_count(f) == 1);
    ID3D12Resource *t = texture(f, 16, 4, 1, D3D12_RESOURCE_FLAG_NONE);
    CHECK(t != NULL);
    if (!t) { all = 0; continue; }
    D3D12_RESOURCE_DESC rd;
    t->lpVtbl->GetDesc(t, &rd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    ID3D12Device_GetCopyableFootprints(g_dev, &rd, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    CHECK(fp.Footprint.Format == f && fp.Footprint.Width == 16 && rows == 4 && row_bytes == packed[i].row);
    /* a round trip through a buffer, byte for byte, and a copy of a box of it into a second texture */
    BYTE a[4 * 128], b[4 * 128];
    for (UINT k = 0; k < sizeof a; ++k) a[k] = (BYTE)(k * 29 + i);
    copy_subresources(t, 0, 1, a, 4 * packed[i].row, 1);
    memset(b, 0, sizeof b);
    copy_subresources(t, 0, 1, b, 4 * packed[i].row, 0);
    const int same = memcmp(a, b, 4 * packed[i].row) == 0;
    ID3D12Resource *t2 = texture(f, 16, 4, 1, D3D12_RESOURCE_FLAG_NONE);
    int boxed = 0;
    if (t2) {
      Copier c;
      copier_begin(&c);
      D3D12_TEXTURE_COPY_LOCATION dst = {t2, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}}, src = {t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
      const D3D12_BOX box = {8, 1, 0, 16, 3, 1};      /* (columns 8 .. 15 - whole pairs - of rows 1 and 2, to column 4 row 0) */
      ID3D12GraphicsCommandList_CopyTextureRegion(c.l, &dst, 4, 0, 0, &src, &box);
      copier_run(&c);
      memset(b, 0, sizeof b);
      copy_subresources(t2, 0, 1, b, 4 * packed[i].row, 0);
      const UINT px = packed[i].row / 16;      /* (bytes of a pixel's share) */
      boxed = memcmp(b + 4 * px, a + packed[i].row + 8 * px, 8 * px) == 0 && memcmp(b + packed[i].row + 4 * px, a + 2 * packed[i].row + 8 * px, 8 * px) == 0 && b[0] == 0 &&
              b[2 * packed[i].row] == 0;
      ID3D12Resource_Release(t2);
    }
    if (!same || !boxed) printf("d3d12videotest: %s: round trip %d, box copy %d\n", packed[i].name, same, boxed);
    CHECK(same && boxed);
    all &= same && boxed;
    /* the views: of the pixels' format only (a view of the YUV format itself, or of another, is no view) */
    ID3D12Resource_Release(t);
    /* a render target: AYUV's only */
    ID3D12Resource *rt = texture(f, 16, 4, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(!!rt == packed[i].target);
    if (rt) ID3D12Resource_Release(rt);
    /* an array (each slice an image of its own below) */
    ID3D12Resource *arr = texture(f, 16, 4, 3, D3D12_RESOURCE_FLAG_NONE);
    CHECK(arr != NULL);
    if (arr) {
      copy_subresources(arr, 2, 1, a, 4 * packed[i].row, 1);
      memset(b, 0, sizeof b);
      copy_subresources(arr, 2, 1, b, 4 * packed[i].row, 0);
      CHECK(memcmp(a, b, 4 * packed[i].row) == 0);
      copy_subresources(arr, 1, 1, b, 4 * packed[i].row, 0);
      CHECK(b[0] == 0 && b[4 * packed[i].row - 1] == 0);
      ID3D12Resource_Release(arr);
    }
  }
  /* the 4:2:2 ones are of even width, as DXGI says */
  ID3D12Resource *odd = texture(DXGI_FORMAT_YUY2, 15, 4, 1, D3D12_RESOURCE_FLAG_NONE);
  CHECK(odd == NULL);
  odd = texture(DXGI_FORMAT_AYUV, 15, 3, 1, D3D12_RESOURCE_FLAG_NONE);
  CHECK(odd != NULL);
  if (odd) ID3D12Resource_Release(odd);
  /* the planar 4:2:2 (P208: U, V half as wide), 4:4:0 (V208: U and V planes half as high) and 4:4:4 (V408) formats: their planes' footprints and round trips */
  static const struct { DXGI_FORMAT f; UINT planes, cw, ch; DXGI_FORMAT chroma; } planar[] = {
      {DXGI_FORMAT_P208, 2, 8, 8, DXGI_FORMAT_R8G8_TYPELESS}, {DXGI_FORMAT_V208, 3, 16, 4, DXGI_FORMAT_R8_TYPELESS}, {DXGI_FORMAT_V408, 3, 16, 8, DXGI_FORMAT_R8_TYPELESS}};
  for (UINT i = 0; i < 3; ++i) {
    CHECK(plane_count(planar[i].f) == planar[i].planes);
    const D3D12_FORMAT_SUPPORT1 s = support_of(planar[i].f);
    CHECK((s & D3D12_FORMAT_SUPPORT1_TEXTURE2D) && (s & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) && !(s & D3D12_FORMAT_SUPPORT1_DECODER_OUTPUT) &&
          !!(s & D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_INPUT) == (planar[i].f == DXGI_FORMAT_P208));
    ID3D12Resource *t = texture(planar[i].f, 16, 8, 2, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    CHECK(t != NULL);
    if (!t) { all = 0; continue; }
    D3D12_RESOURCE_DESC rd;
    t->lpVtbl->GetDesc(t, &rd);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[6];
    UINT rows[6];
    UINT64 row_bytes[6], total = 0;
    ID3D12Device_GetCopyableFootprints(g_dev, &rd, 0, 2 * planar[i].planes, 0, fp, rows, row_bytes, &total);
    CHECK(fp[0].Footprint.Width == 16 && fp[0].Footprint.Height == 8 && fp[0].Footprint.Format == DXGI_FORMAT_R8_TYPELESS);
    CHECK(fp[2].Footprint.Width == planar[i].cw && fp[2].Footprint.Height == planar[i].ch && fp[2].Footprint.Format == planar[i].chroma);
    for (UINT p = 0; p < planar[i].planes; ++p) {
      const UINT sub = p * 2 + 1;      /* (slice 1 of plane p) */
      BYTE a[16 * 8 * 2], b[16 * 8 * 2];
      const UINT bytes = (UINT)(row_bytes[sub] * rows[sub]);
      for (UINT k = 0; k < bytes; ++k) a[k] = (BYTE)(k * 11 + p * 3 + i);
      copy_subresources(t, sub, 1, a, bytes, 1);
      memset(b, 0, sizeof b);
      copy_subresources(t, sub, 1, b, bytes, 0);
      CHECK(memcmp(a, b, bytes) == 0);
    }
    ID3D12Resource_Release(t);
  }
  CHECK(texture(DXGI_FORMAT_P208, 15, 8, 1, D3D12_RESOURCE_FLAG_NONE) == NULL);      /* (P208: even width; V208: even height) */
  CHECK(texture(DXGI_FORMAT_V208, 16, 7, 1, D3D12_RESOURCE_FLAG_NONE) == NULL);
  ID3D12Resource *v408 = texture(DXGI_FORMAT_V408, 15, 7, 1, D3D12_RESOURCE_FLAG_NONE);
  CHECK(v408 != NULL);
  if (v408) ID3D12Resource_Release(v408);
  printf("d3d12videotest: packed and planar YUV textures%s\n", all ? ": footprints, copies and arrays as DXGI has them" : " FAILED");
}

/* The processor reading and writing the packed formats: a Y210 picture to BGRA (BT.709), and BGRA to AYUV and back. */
static void process_once(ID3D12VideoProcessor *vp, ID3D12CommandQueue *pq, ID3D12CommandAllocator *pa, ID3D12VideoProcessCommandList *pl, ID3D12Resource *in, UINT iw, UINT ih,
                         ID3D12Resource *out, UINT ow, UINT oh) {
  D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS ia;
  memset(&ia, 0, sizeof ia);
  ia.InputStream[0].pTexture2D = in;
  ia.Transform.SourceRectangle = (D3D12_RECT){0, 0, (LONG)iw, (LONG)ih};
  ia.Transform.DestinationRectangle = (D3D12_RECT){0, 0, (LONG)ow, (LONG)oh};
  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS oa;
  memset(&oa, 0, sizeof oa);
  oa.OutputStream[0].pTexture2D = out;
  oa.TargetRectangle = (D3D12_RECT){0, 0, (LONG)ow, (LONG)oh};
  ID3D12VideoProcessCommandList_ProcessFrames(pl, vp, &oa, 1, &ia);
  ID3D12VideoProcessCommandList_Close(pl);
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)pl};
  ID3D12CommandQueue_ExecuteCommandLists(pq, 1, lists);
  wait_queue(pq);
  ID3D12VideoProcessCommandList_Reset(pl, pa);
}
static ID3D12VideoProcessor *processor_of(DXGI_FORMAT in, DXGI_COLOR_SPACE_TYPE in_space, DXGI_FORMAT out, DXGI_COLOR_SPACE_TYPE out_space) {
  D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC isd;
  memset(&isd, 0, sizeof isd);
  isd.Format = in;
  isd.ColorSpace = in_space;
  isd.SourceAspectRatio.Numerator = isd.SourceAspectRatio.Denominator = 1;
  isd.DestinationAspectRatio = isd.SourceAspectRatio;
  isd.FrameRate.Numerator = 30; isd.FrameRate.Denominator = 1;
  isd.SourceSizeRange.MaxWidth = isd.SourceSizeRange.MaxHeight = 4096; isd.SourceSizeRange.MinWidth = isd.SourceSizeRange.MinHeight = 1;
  isd.DestinationSizeRange = isd.SourceSizeRange;
  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC osd;
  memset(&osd, 0, sizeof osd);
  osd.Format = out;
  osd.ColorSpace = out_space;
  osd.AlphaFillMode = D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_OPAQUE;
  osd.FrameRate = isd.FrameRate;
  ID3D12VideoProcessor *vp = NULL;
  CHECK_HR(ID3D12VideoDevice_CreateVideoProcessor(g_vdev, 0, &osd, 1, &isd, &IID_ID3D12VideoProcessor, (void **)&vp));
  return vp;
}
static void test_processor_yuv(void) {
  if (!g_vdev) return;
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *pq = NULL;
  ID3D12CommandAllocator *pa = NULL;
  ID3D12VideoProcessCommandList *pl = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&pq);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, &IID_ID3D12CommandAllocator, (void **)&pa);
  if (pa) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, pa, NULL, &IID_ID3D12VideoProcessCommandList, (void **)&pl);
  ID3D12VideoProcessor *to_rgb = processor_of(DXGI_FORMAT_Y210, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
  ID3D12VideoProcessor *to_ayuv = processor_of(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, DXGI_FORMAT_AYUV, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
  ID3D12Resource *y210 = texture(DXGI_FORMAT_Y210, 32, 8, 1, D3D12_RESOURCE_FLAG_NONE), *bgra = texture(DXGI_FORMAT_B8G8R8A8_UNORM, 32, 8, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  ID3D12Resource *ayuv = texture(DXGI_FORMAT_AYUV, 32, 8, 1, D3D12_RESOURCE_FLAG_NONE);
  CHECK(pq && pl && to_rgb && to_ayuv && y210 && bgra && ayuv);
  if (pq && pl && to_rgb && to_ayuv && y210 && bgra && ayuv) {
    /* Y210 in 8 x 8 blocks of one colour (Y0, U, Y1, V words, the value in the top ten bits) */
    static UINT16 yuv[8 * 16 * 4];
    int code[4][3];
    for (UINT b = 0; b < 4; ++b) { code[b][0] = 60 + 40 * b; code[b][1] = 90 + 30 * b; code[b][2] = 200 - 35 * b; }
    for (UINT r = 0; r < 8; ++r)
      for (UINT p = 0; p < 16; ++p) {
        const int *c = code[p / 4];
        UINT16 *w = &yuv[(r * 16 + p) * 4];
        w[0] = w[2] = (UINT16)((c[0] * 4) << 6);
        w[1] = (UINT16)((c[1] * 4) << 6);
        w[3] = (UINT16)((c[2] * 4) << 6);
      }
    copy_subresources(y210, 0, 1, (BYTE *)yuv, sizeof yuv, 1);
    process_once(to_rgb, pq, pa, pl, y210, 32, 8, bgra, 32, 8);
    static BYTE px[32 * 8 * 4];
    copy_subresources(bgra, 0, 1, px, sizeof px, 0);
    int worst = 0;
    for (UINT b = 0; b < 4; ++b) {
      double rgb[3];
      bt709(code[b][0], code[b][1], code[b][2], rgb);
      const BYTE *p = &px[(4 * 32 + b * 8 + 4) * 4];      /* (a pixel inside the block) */
      for (int c = 0; c < 3; ++c) { const int d = abs((int)p[2 - c] - (int)lround(rgb[c])); if (d > worst) worst = d; }
    }
    printf("d3d12videotest: processor Y210 -> BGRA largest difference from BT.709: %d\n", worst);
    CHECK(worst <= 1);
    /* that BGRA to AYUV: the same codes again (8 bits), alpha opaque */
    process_once(to_ayuv, pq, pa, pl, bgra, 32, 8, ayuv, 32, 8);
    static BYTE vuya[32 * 8 * 4];
    copy_subresources(ayuv, 0, 1, vuya, sizeof vuya, 0);
    worst = 0;
    int alpha = 1;
    for (UINT k = 0; k < 32 * 8; ++k) {
      /* (BT.709 studio range of each pixel's R'G'B', 4:4:4: no chroma filter) */
      const double R = px[4 * k + 2] / 255.0, G = px[4 * k + 1] / 255.0, B = px[4 * k] / 255.0, Y = 0.2126 * R + 0.7152 * G + 0.0722 * B;
      const int want[3] = {(int)lround(16 + 219 * Y), (int)lround(128 + 224 * (B - Y) / 1.8556), (int)lround(128 + 224 * (R - Y) / 1.5748)};
      const BYTE *p = &vuya[4 * k];
      const int d[3] = {abs(p[2] - want[0]), abs(p[1] - want[1]), abs(p[0] - want[2])};
      for (int c = 0; c < 3; ++c) if (d[c] > worst) worst = d[c];
      alpha &= p[3] == 255;
    }
    printf("d3d12videotest: processor BGRA -> AYUV largest difference from BT.709: %d\n", worst);
    CHECK(worst <= 1 && alpha);
  }
  if (y210) ID3D12Resource_Release(y210);
  if (bgra) ID3D12Resource_Release(bgra);
  if (ayuv) ID3D12Resource_Release(ayuv);
  if (to_rgb) ID3D12VideoProcessor_Release(to_rgb);
  if (to_ayuv) ID3D12VideoProcessor_Release(to_ayuv);
  if (pl) ID3D12VideoProcessCommandList_Release(pl);
  if (pa) ID3D12CommandAllocator_Release(pa);
  if (pq) ID3D12CommandQueue_Release(pq);
}

#include "dxva_streams.h"
#include "d3d11video_data.h"
#include "hevc10_dxva.h"
#include "av1_dxva_data.h"
static void test_decode(void);
static void test_processor_effects(void);

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
  test_yuv_formats();
  test_features();
  test_processor();
  test_processor_yuv();
  test_processor_effects();
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

/* A stream (H.264, VC-1, MPEG-4 Part 2: `codec`, `profile`) decoded through ID3D12VideoDecodeCommandList, picture by picture, compared in display order with
 * ffmpeg's (FNV hashes of the visible I420 picture).
 * `separate`: every surface a texture of its own (the reference frames are different resources at subresource 0), else one NV12 array (the indices are its slices).
 * `interlaced`: field-based content (a decoder of D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_FIELD_BASED): a field picture writes its field's lines of the surface, the
 * frame is compared once its second field is in. The decode statistics of every picture are queried and resolved. */
static void decode_stream(const char *name, int codec, const GUID *profile, const char *b64, unsigned surfaces, const unsigned *hashes, unsigned frames, int separate,
                          int interlaced) {
  size_t size = 0;
  unsigned char *data = base64_decode(b64, &size);
  DxvaStream s;
  DxvaPicture p;
  if (!data || dxva_stream_open(&s, codec, data, size, surfaces) != 0 || dxva_stream_next(&s, &p) != 1) { CHECK(0); free(data); return; }
  D3D12_VIDEO_DECODER_DESC dd = {0, {*profile, D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE,
                                     interlaced ? D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_FIELD_BASED : D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE}};
  if (interlaced) {
    /* field-based H.264 is supported, with surfaces 32 lines aligned; not for the other profiles */
    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT fs;
    memset(&fs, 0, sizeof fs);
    fs.Configuration = dd.Configuration;
    fs.Width = s.width;
    fs.Height = s.height;
    fs.DecodeFormat = DXGI_FORMAT_NV12;
    CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &fs, sizeof fs));
    CHECK((fs.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED) && (fs.ConfigurationFlags & D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_HEIGHT_ALIGNMENT_MULTIPLE_32_REQUIRED));
    fs.Configuration.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
    CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &fs, sizeof fs));
    CHECK(!(fs.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED));
    D3D12_FEATURE_DATA_VIDEO_DECODE_FORMAT_COUNT fc = {0, dd.Configuration, 0};
    CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_FORMAT_COUNT, &fc, sizeof fc));
    CHECK(fc.FormatCount == 1);
  }
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
  unsigned n = 0, pictures = 0, statuses_ok = 0;
  int r = 1;
  while (dec && heap && vl && bits && tex[0] && r == 1 && pictures < 256) {
    /* (the compressed data in an upload buffer, the arguments as the program's own memory) */
    BYTE *map = NULL;
    ID3D12Resource_Map(bits, 0, NULL, (void **)&map);
    memcpy(map, p.bitstream, p.bitstream_size);
    ID3D12Resource_Unmap(bits, 0, NULL);
    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in;
    memset(&in, 0, sizeof in);
    /* (VC-1 has no matrix buffer) */
    in.NumFrameArguments = 0;
    in.FrameArguments[in.NumFrameArguments++] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, p.picture_parameters_size, (void *)p.picture_parameters};
    if (p.qmatrix_size)
      in.FrameArguments[in.NumFrameArguments++] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, p.qmatrix_size, (void *)p.qmatrix};
    in.FrameArguments[in.NumFrameArguments++] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, p.slice_control_size, (void *)p.slice_control};
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
    ID3D12VideoDecodeCommandList_EndQuery(vl, stats, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, pictures);
    ID3D12VideoDecodeCommandList_Close(vl);
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)vl};
    ID3D12CommandQueue_ExecuteCommandLists(vq, 1, lists);
    wait_queue(vq);
    ID3D12VideoDecodeCommandList_Reset(vl, va);
    ++pictures;
    /* (a first field: the frame is whole after the second, which the stream gives next in the same surface) */
    if (codec == DXVA_CODEC_H264 && p.field == 1) {
      r = dxva_stream_next(&s, &p);
      CHECK(r != 1 || p.field == 2);
      continue;
    }
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
  printf("d3d12videotest: %s (%s%s): %u of %u frames are ffmpeg's, bit for bit\n", name, separate ? "a texture a surface" : "an array", interlaced ? ", field-based" : "", same, frames);
  CHECK(same == frames);
  /* every picture's statistics: decoded, nothing affected */
  if (stats && pictures) {
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, 256 * sizeof(D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS));
    Copier c;
    copier_begin(&c);
    ID3D12GraphicsCommandList_ResolveQueryData(c.l, stats, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, 0, pictures, rb, 0);
    copier_run(&c);
    D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS *st = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&st);
    for (unsigned i = 0; st && i < pictures; ++i) statuses_ok += st[i].Status == D3D12_VIDEO_DECODE_STATUS_OK && st[i].NumMacroblocksAffected == 0;
    ID3D12Resource_Unmap(rb, 0, NULL);
    ID3D12Resource_Release(rb);
    CHECK(statuses_ok == pictures);
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

/* An AV1 stream of profile 1 or 2 as the DXVA buffers a program fills (av1_dxva_data.h: tests/data/make_av1_dxva_data.py), decoded through ID3D12VideoDecodeCommandList
 * into `format` - an array of the surfaces, or a texture each (`separate`) - and every picture the stream shows read back through GetCopyableFootprints and
 * CopyTextureRegion and compared with ffmpeg's (FNV hashes of the packed rows). The indices of the parameters are positions in the reference list (all the surfaces). */
static UINT le32_at(const BYTE *p) { return (UINT)p[0] | (UINT)p[1] << 8 | (UINT)p[2] << 16 | (UINT)p[3] << 24; }
static void decode_av1(const char *name, const char *b64, const GUID *profile, DXGI_FORMAT format, const unsigned *hashes, unsigned frames, int separate) {
  size_t size = 0;
  BYTE *data = base64_decode(b64, &size);
  if (!data || size < 32 || memcmp(data, "AV1D", 4) != 0) { CHECK(0); free(data); return; }
  const UINT surfaces = le32_at(data + 4), w = le32_at(data + 8), h = le32_at(data + 12), events = le32_at(data + 28);
  D3D12_VIDEO_DECODER_DESC dd = {0, {*profile, D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE, D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE}};
  D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT ds;
  memset(&ds, 0, sizeof ds);
  ds.Configuration = dd.Configuration;
  ds.Width = w;
  ds.Height = h;
  ds.DecodeFormat = format;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &ds, sizeof ds));
  CHECK(ds.SupportFlags == D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED);
  D3D12_VIDEO_DECODER_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.Configuration = dd.Configuration;
  hd.DecodeWidth = w;
  hd.DecodeHeight = h;
  hd.Format = format;
  hd.MaxDecodePictureBufferCount = surfaces;
  ID3D12VideoDecoder *dec = NULL;
  ID3D12VideoDecoderHeap *heap = NULL;
  CHECK_HR(ID3D12VideoDevice_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&dec));
  CHECK_HR(ID3D12VideoDevice_CreateVideoDecoderHeap(g_vdev, &hd, &IID_ID3D12VideoDecoderHeap, (void **)&heap));
  ID3D12Resource *tex[16] = {0};
  UINT subs[16];
  ID3D12Resource *array = separate ? NULL : texture(format, w, h, surfaces, D3D12_RESOURCE_FLAG_NONE);
  for (UINT i = 0; i < surfaces && i < 16; ++i) {
    tex[i] = separate ? texture(format, w, h, 1, D3D12_RESOURCE_FLAG_NONE) : array;
    subs[i] = separate ? 0 : i;
  }
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *vq = NULL;
  ID3D12CommandAllocator *va = NULL;
  ID3D12VideoDecodeCommandList *vl = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&vq);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, &IID_ID3D12CommandAllocator, (void **)&va);
  if (va) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, va, NULL, &IID_ID3D12VideoDecodeCommandList, (void **)&vl);
  ID3D12Resource *bits = buffer(D3D12_HEAP_TYPE_UPLOAD, 1 << 20);
  CHECK(surfaces <= 16 && dec && heap && vq && vl && bits && tex[0]);
  D3D12_RESOURCE_DESC rd;
  if (tex[0]) tex[0]->lpVtbl->GetDesc(tex[0], &rd);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  if (tex[0]) ID3D12Device_GetCopyableFootprints(g_dev, &rd, 0, 1, 0, &fp, &rows, &row_bytes, &total);
  BYTE *picture = malloc((size_t)(row_bytes * rows) + 1);
  unsigned shown = 0, same = 0;
  size_t at = 32;
  for (UINT e = 0; e < events && dec && heap && vl && bits && tex[0] && surfaces <= 16 && picture; ++e) {
    if (at + 12 > size) { CHECK(0); break; }
    const UINT kind = le32_at(data + at), show = le32_at(data + at + 4), surface = le32_at(data + at + 8);
    at += 12;
    if (surface >= surfaces) { CHECK(0); break; }
    if (kind == 0) {
      if (at + 12 > size) { CHECK(0); break; }
      const UINT pp = le32_at(data + at), sc = le32_at(data + at + 4), bs = le32_at(data + at + 8);
      at += 12;
      if (at + pp + sc + bs > size || bs > (1u << 20)) { CHECK(0); break; }
      BYTE *map = NULL;
      ID3D12Resource_Map(bits, 0, NULL, (void **)&map);
      memcpy(map, data + at + pp + sc, bs);
      ID3D12Resource_Unmap(bits, 0, NULL);
      D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in;
      memset(&in, 0, sizeof in);
      in.NumFrameArguments = 2;
      in.FrameArguments[0] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, pp, data + at};
      in.FrameArguments[1] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, sc, data + at + pp};
      in.ReferenceFrames.NumTexture2Ds = surfaces;
      in.ReferenceFrames.ppTexture2Ds = tex;
      in.ReferenceFrames.pSubresources = subs;
      in.CompressedBitstream.pBuffer = bits;
      in.CompressedBitstream.Size = bs;
      in.pHeap = heap;
      D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS out;
      memset(&out, 0, sizeof out);
      out.pOutputTexture2D = tex[surface];
      out.OutputSubresource = subs[surface];
      ID3D12VideoDecodeCommandList_DecodeFrame(vl, dec, &out, &in);
      ID3D12VideoDecodeCommandList_Close(vl);
      ID3D12CommandList *lists[1] = {(ID3D12CommandList *)vl};
      ID3D12CommandQueue_ExecuteCommandLists(vq, 1, lists);
      wait_queue(vq);
      ID3D12VideoDecodeCommandList_Reset(vl, va);
      at += pp + sc + bs;
    }
    if (!show) continue;
    copy_subresources(tex[surface], subs[surface], 1, picture, row_bytes * rows, 0);
    const unsigned hash = fnv(picture, (size_t)(row_bytes * rows), 0x811c9dc5u);
    if (shown < frames) same += hash == hashes[shown];
    ++shown;
  }
  printf("d3d12videotest: %s into %s (%s): %u of %u pictures are ffmpeg's, bit for bit\n", name,
         format == DXGI_FORMAT_AYUV ? "AYUV" : format == DXGI_FORMAT_Y210 ? "Y210" : format == DXGI_FORMAT_Y416 ? "Y416" : "?", separate ? "a texture a surface" : "an array",
         same, frames);
  CHECK(shown == frames && same == frames);
  free(picture);
  if (bits) ID3D12Resource_Release(bits);
  if (vl) ID3D12VideoDecodeCommandList_Release(vl);
  if (va) ID3D12CommandAllocator_Release(va);
  if (vq) ID3D12CommandQueue_Release(vq);
  if (separate) { for (unsigned i = 0; i < surfaces && i < 16; ++i) if (tex[i]) ID3D12Resource_Release(tex[i]); }
  else if (array) ID3D12Resource_Release(array);
  if (heap) ID3D12VideoDecoderHeap_Release(heap);
  if (dec) ID3D12VideoDecoder_Release(dec);
  free(data);
}

/* A Main 10 stream (tests/windows/hevc10_dxva.h: the DXVA buffers of every picture as ffmpeg makes them) decoded through ID3D12VideoDecodeCommandList into P010,
 * compared in display order with the reference (FNV hashes of each picture as yuv420p10le). `separate`: every surface a P010 texture of its own, else one P010 array. */
static void decode_hevc10(const Hevc10Stream *st, int separate) {
  const unsigned surfaces = st->surfaces, w = st->width, h = st->height;
  D3D12_VIDEO_DECODER_DESC dd = {0, {D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10, D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE, D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE}};
  D3D12_VIDEO_DECODER_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.Configuration = dd.Configuration;
  hd.DecodeWidth = w;
  hd.DecodeHeight = h;
  hd.Format = DXGI_FORMAT_P010;
  hd.MaxDecodePictureBufferCount = surfaces;
  ID3D12VideoDecoder *dec = NULL;
  ID3D12VideoDecoderHeap *heap = NULL;
  CHECK_HR(ID3D12VideoDevice_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&dec));
  CHECK_HR(ID3D12VideoDevice_CreateVideoDecoderHeap(g_vdev, &hd, &IID_ID3D12VideoDecoderHeap, (void **)&heap));
  ID3D12Resource *tex[DXVA_STREAM_MAX_SURFACES] = {0};
  UINT subs[DXVA_STREAM_MAX_SURFACES];
  ID3D12Resource *array = separate ? NULL : texture(DXGI_FORMAT_P010, w, h, surfaces, D3D12_RESOURCE_FLAG_NONE);
  for (unsigned i = 0; i < surfaces; ++i) {
    tex[i] = separate ? texture(DXGI_FORMAT_P010, w, h, 1, D3D12_RESOURCE_FLAG_NONE) : array;
    subs[i] = separate ? 0 : i;
  }
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *vq = NULL;
  ID3D12CommandAllocator *va = NULL;
  ID3D12VideoDecodeCommandList *vl = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&vq);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, &IID_ID3D12CommandAllocator, (void **)&va);
  if (va) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, va, NULL, &IID_ID3D12VideoDecodeCommandList, (void **)&vl);
  D3D12_QUERY_HEAP_DESC qhd = {D3D12_QUERY_HEAP_TYPE_VIDEO_DECODE_STATISTICS, 64, 0};
  ID3D12QueryHeap *stats = NULL;
  CHECK_HR(ID3D12Device_CreateQueryHeap(g_dev, &qhd, &IID_ID3D12QueryHeap, (void **)&stats));
  ID3D12Resource *bits = buffer(D3D12_HEAP_TYPE_UPLOAD, 1 << 20);
  CHECK(dec && heap && vq && vl && stats && bits && tex[0]);
  /* the planes as read back: luma rows of w 16-bit words, chroma rows of w / 2 Cb, Cr pairs; the picture as yuv420p10le */
  BYTE *y = malloc((size_t)w * h * 2), *uv = malloc((size_t)w * h), *f = malloc((size_t)w * h * 3);
  unsigned hashes[64] = {0}, low_bits = 0, decoded = 0, statuses_ok = 0;
  for (unsigned n = 0; dec && heap && vl && bits && tex[0] && n < st->count && n < 64; ++n) {
    const Hevc10Picture *p = &st->pictures[n];
    BYTE *map = NULL;
    ID3D12Resource_Map(bits, 0, NULL, (void **)&map);
    memcpy(map, st->bytes + p->bits, p->bits_size);
    ID3D12Resource_Unmap(bits, 0, NULL);
    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in;
    memset(&in, 0, sizeof in);
    in.NumFrameArguments = 2;
    in.FrameArguments[0] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, p->pp_size, (void *)(st->bytes + p->pp)};
    in.FrameArguments[1] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, p->slices_size, (void *)(st->bytes + p->slices)};
    if (p->qm_size) {
      in.FrameArguments[2] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, p->qm_size, (void *)(st->bytes + p->qm)};
      in.NumFrameArguments = 3;
    }
    in.ReferenceFrames.NumTexture2Ds = surfaces;
    in.ReferenceFrames.ppTexture2Ds = tex;
    in.ReferenceFrames.pSubresources = subs;
    in.CompressedBitstream.pBuffer = bits;
    in.CompressedBitstream.Size = p->bits_size;
    in.pHeap = heap;
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS out;
    memset(&out, 0, sizeof out);
    out.pOutputTexture2D = tex[p->surface];
    out.OutputSubresource = subs[p->surface];
    ID3D12VideoDecodeCommandList_DecodeFrame(vl, dec, &out, &in);
    ID3D12VideoDecodeCommandList_EndQuery(vl, stats, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, n);
    ID3D12VideoDecodeCommandList_Close(vl);
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)vl};
    ID3D12CommandQueue_ExecuteCommandLists(vq, 1, lists);
    wait_queue(vq);
    ID3D12VideoDecodeCommandList_Reset(vl, va);
    const UINT layers = separate ? 1 : surfaces;
    copy_subresources(tex[p->surface], subs[p->surface], 1, y, (size_t)w * h * 2, 0);
    copy_subresources(tex[p->surface], layers + subs[p->surface], 1, uv, (size_t)w * h, 0);
    BYTE *at = f;
    for (UINT i = 0; i < w * h; ++i) {
      const unsigned v = (unsigned)y[2 * i] | ((unsigned)y[2 * i + 1] << 8);
      low_bits |= v & 0x3F;
      *at++ = (BYTE)(v >> 6);
      *at++ = (BYTE)(v >> 14);
    }
    for (int c = 0; c < 2; ++c)
      for (UINT i = 0; i < w * h / 4; ++i) {
        const unsigned v = (unsigned)uv[4 * i + 2 * c] | ((unsigned)uv[4 * i + 2 * c + 1] << 8);
        low_bits |= v & 0x3F;
        *at++ = (BYTE)(v >> 6);
        *at++ = (BYTE)(v >> 14);
      }
    if (p->display < 64) hashes[p->display] = fnv(f, (size_t)w * h * 3, 0x811c9dc5u);
    ++decoded;
  }
  CHECK(decoded == st->count);
  CHECK(low_bits == 0);   /* (P010: the samples in the top ten bits) */
  unsigned same = 0;
  for (unsigned i = 0; i < st->count && i < 64; ++i) same += hashes[i] == st->fnv[i];
  printf("d3d12videotest: %s (HEVC Main 10, %s): %u of %u frames are the reference's, bit for bit\n", st->name, separate ? "a P010 texture a surface" : "a P010 array", same,
         st->count);
  CHECK(same == st->count);
  if (stats && decoded) {
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, 64 * sizeof(D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS));
    Copier c;
    copier_begin(&c);
    ID3D12GraphicsCommandList_ResolveQueryData(c.l, stats, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, 0, decoded, rb, 0);
    copier_run(&c);
    D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS *sd = NULL;
    ID3D12Resource_Map(rb, 0, NULL, (void **)&sd);
    for (unsigned i = 0; sd && i < decoded; ++i) statuses_ok += sd[i].Status == D3D12_VIDEO_DECODE_STATUS_OK && sd[i].NumMacroblocksAffected == 0;
    ID3D12Resource_Unmap(rb, 0, NULL);
    ID3D12Resource_Release(rb);
    CHECK(statuses_ok == decoded);
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
}

static void test_decode(void) {
  if (!g_vdev) return;
  decode_hevc10(&g_hevc10_streams[0], 0);
  decode_hevc10(&g_hevc10_streams[1], 1);
  decode_stream("h264_bframes", DXVA_CODEC_H264, &D3D12_VIDEO_DECODE_PROFILE_H264, g_h264_bframes_b64, 17, g_h264_bframes_fnv, 14, 0, 0);
  decode_stream("h264_ltr", DXVA_CODEC_H264, &D3D12_VIDEO_DECODE_PROFILE_H264, g_h264_ltr_b64, 20, g_h264_ltr_fnv, 24, 1, 0);
  decode_stream("h264_cqm", DXVA_CODEC_H264, &D3D12_VIDEO_DECODE_PROFILE_H264, g_h264_cqm_b64, 17, g_h264_cqm_fnv, 9, 0, 0);
  /* interlaced: MBAFF frames and field pictures (P and B fields, B MBAFF frames), CABAC */
  decode_stream("h264_paff_mbaff_cabac", DXVA_CODEC_H264, &D3D12_VIDEO_DECODE_PROFILE_H264, g_h264_paff_mbaff_cabac_b64, 17, g_h264_paff_mbaff_cabac_fnv, 8, 0, 1);
  decode_stream("vc1_adv_ipb", DXVA_CODEC_VC1, &D3D12_VIDEO_DECODE_PROFILE_VC1_D2010, g_vc1_adv_ipb_b64, 8, g_vc1_adv_ipb_fnv, 10, 0, 0);
  decode_stream("m4vdec_bframes", DXVA_CODEC_MPEG4, &D3D12_VIDEO_DECODE_PROFILE_MPEG4PT2_ADVSIMPLE_NOGMC, g_m4vdec_bframes_b64, 8, g_m4vdec_bframes_fnv, 8, 1, 0);
  decode_av1("av1_422_10bit (profile 2, film grain)", g_av1_422_10bit_dxva_b64, &D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE2, DXGI_FORMAT_Y210, g_av1_422_10bit_dxva_fnv, 6, 0);
  decode_av1("av1_444 (profile 1)", g_av1_444_dxva_b64, &D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE1, DXGI_FORMAT_AYUV, g_av1_444_dxva_fnv, 6, 1);
  decode_av1("av1_444_12bit (12-bit profile 2)", g_av1_444_12bit_dxva_b64, &D3D12_VIDEO_DECODE_PROFILE_AV1_12BIT_PROFILE2, DXGI_FORMAT_Y416, g_av1_444_12bit_dxva_fnv, 6, 0);
}

/* ---- the video processor's deinterlacing, filters, luma key, stereo and HDR ---------------------------------------------------------------------------------------- */

#include "deinterlace_data.h"

typedef struct VpRig { ID3D12CommandQueue *q; ID3D12CommandAllocator *a; ID3D12VideoProcessCommandList *l; } VpRig;
static int rig_open(VpRig *r) {
  memset(r, 0, sizeof *r);
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&r->q);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, &IID_ID3D12CommandAllocator, (void **)&r->a);
  if (r->a) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, r->a, NULL, &IID_ID3D12VideoProcessCommandList, (void **)&r->l);
  return r->q && r->a && r->l;
}
static void rig_close(VpRig *r) {
  if (r->l) ID3D12VideoProcessCommandList_Release(r->l);
  if (r->a) ID3D12CommandAllocator_Release(r->a);
  if (r->q) ID3D12CommandQueue_Release(r->q);
}
/* Records what `record` asks (ProcessFrames or ProcessFrames1), runs it and makes the list ready for the next. */
static void rig_run(VpRig *r, ID3D12VideoProcessor *vp, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *oa, UINT n, const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *ia,
                    const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS1 *ia1) {
  if (ia1) {
    ID3D12VideoProcessCommandList1 *l1 = NULL;
    if (SUCCEEDED(ID3D12VideoProcessCommandList_QueryInterface(r->l, &IID_ID3D12VideoProcessCommandList1, (void **)&l1))) {
      l1->lpVtbl->ProcessFrames1(l1, vp, oa, n, ia1);
      l1->lpVtbl->Release(l1);
    }
  } else ID3D12VideoProcessCommandList_ProcessFrames(r->l, vp, oa, n, ia);
  CHECK_HR(ID3D12VideoProcessCommandList_Close(r->l));
  ID3D12CommandList *lists[1] = {(ID3D12CommandList *)r->l};
  ID3D12CommandQueue_ExecuteCommandLists(r->q, 1, lists);
  wait_queue(r->q);
  ID3D12CommandAllocator_Reset(r->a);
  ID3D12VideoProcessCommandList_Reset(r->l, r->a);
}
static void vp_descs(DXGI_FORMAT fi, DXGI_COLOR_SPACE_TYPE si, DXGI_FORMAT fo, DXGI_COLOR_SPACE_TYPE so, D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *isd, D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *osd) {
  memset(isd, 0, sizeof *isd);
  isd->Format = fi;
  isd->ColorSpace = si;
  isd->SourceAspectRatio.Numerator = isd->SourceAspectRatio.Denominator = 1;
  isd->DestinationAspectRatio = isd->SourceAspectRatio;
  isd->FrameRate.Numerator = 30; isd->FrameRate.Denominator = 1;
  isd->SourceSizeRange.MaxWidth = isd->SourceSizeRange.MaxHeight = 4096; isd->SourceSizeRange.MinWidth = isd->SourceSizeRange.MinHeight = 1;
  isd->DestinationSizeRange = isd->SourceSizeRange;
  memset(osd, 0, sizeof *osd);
  osd->Format = fo;
  osd->ColorSpace = so;
  osd->AlphaFillMode = D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_OPAQUE;
  osd->BackgroundColor[3] = 1.0f;
  osd->FrameRate = isd->FrameRate;
}
static ID3D12VideoProcessor *make_vp(const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *isd, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *osd) {
  ID3D12VideoProcessor *vp = NULL;
  CHECK_HR(ID3D12VideoDevice_CreateVideoProcessor(g_vdev, 0, osd, 1, isd, &IID_ID3D12VideoProcessor, (void **)&vp));
  return vp;
}
static void stream_args(D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *ia, ID3D12Resource *in, UINT w, UINT h, UINT ow, UINT oh) {
  memset(ia, 0, sizeof *ia);
  ia->InputStream[0].pTexture2D = in;
  ia->Transform.SourceRectangle = (D3D12_RECT){0, 0, (LONG)w, (LONG)h};
  ia->Transform.DestinationRectangle = (D3D12_RECT){0, 0, (LONG)ow, (LONG)oh};
}
static void out_args(D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *oa, ID3D12Resource *out, UINT w, UINT h) {
  memset(oa, 0, sizeof *oa);
  oa->OutputStream[0].pTexture2D = out;
  oa->TargetRectangle = (D3D12_RECT){0, 0, (LONG)w, (LONG)h};
}
/* An NV12 texture holding `frame` (luma rows, then chroma rows, w bytes each). */
static ID3D12Resource *nv12_of(const BYTE *frame, UINT w, UINT h, int target) {
  ID3D12Resource *t = texture(DXGI_FORMAT_NV12, w, h, 1, target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
  if (t && frame) copy_subresources(t, 0, 2, (BYTE *)frame, (UINT64)w * h * 3 / 2, 1);
  return t;
}
static int bgra_at(const BYTE *px, UINT w, UINT x, UINT y, int r, int g, int b, int tolerance) {
  const BYTE *p = &px[(y * w + x) * 4];
  return abs(p[2] - r) <= tolerance && abs(p[1] - g) <= tolerance && abs(p[0] - b) <= tolerance;
}
static double pq_signal(double nits) {
  const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
  const double y = pow(nits / 10000, m1);
  return pow((c1 + c2 * y) / (1 + c3 * y), m2);
}

static void test_processor_effects(void) {
  if (!g_vdev) return;
  /* what the processor says it does */
  D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT ps;
  memset(&ps, 0, sizeof ps);
  ps.InputSample.Width = 720; ps.InputSample.Height = 480;
  ps.InputSample.Format.Format = DXGI_FORMAT_NV12;
  ps.InputSample.Format.ColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
  ps.InputFieldType = D3D12_VIDEO_FIELD_TYPE_INTERLACED_TOP_FIELD_FIRST;
  ps.OutputFormat.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  ps.OutputFormat.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &ps, sizeof ps));
  CHECK(ps.SupportFlags == D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED && ps.DeinterlaceSupport == (D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_BOB | D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_CUSTOM));
  CHECK((ps.FeatureSupport & D3D12_VIDEO_PROCESS_FEATURE_FLAG_LUMA_KEY) && (ps.FeatureSupport & D3D12_VIDEO_PROCESS_FEATURE_FLAG_STEREO) && ps.FilterSupport == 0x7F && !ps.AutoProcessingSupport);
  CHECK(ps.FilterRangeSupport[0].Minimum == -400 && ps.FilterRangeSupport[0].Maximum == 400 && ps.FilterRangeSupport[0].Default == 0 && ps.FilterRangeSupport[0].Multiplier == 0.25f);
  CHECK(ps.FilterRangeSupport[1].Maximum == 640 && ps.FilterRangeSupport[1].Default == 64 && ps.FilterRangeSupport[1].Multiplier == 1.0f / 64 && ps.FilterRangeSupport[7].Maximum == 0);
  ps.InputSample.Format.Format = DXGI_FORMAT_P010;
  ps.InputSample.Format.ColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &ps, sizeof ps));
  CHECK(ps.SupportFlags == D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED);      /* (HDR10 into SDR: tone mapped) */
  D3D12_FEATURE_DATA_VIDEO_PROCESS_REFERENCE_INFO ri;
  memset(&ri, 0, sizeof ri);
  ri.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_CUSTOM;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_REFERENCE_INFO, &ri, sizeof ri));
  CHECK(ri.PastFrames == 1 && ri.FutureFrames == 1);
  ri.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_BOB;
  ri.Filters = D3D12_VIDEO_PROCESS_FILTER_FLAG_NOISE_REDUCTION;
  CHECK_HR(ID3D12VideoDevice_CheckFeatureSupport(g_vdev, D3D12_FEATURE_VIDEO_PROCESS_REFERENCE_INFO, &ri, sizeof ri));
  CHECK(ri.PastFrames == 0 && ri.FutureFrames == 0);
  VpRig rig;
  if (!rig_open(&rig)) { CHECK(0); rig_close(&rig); return; }
  D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC isd;
  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC osd;
  D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS ia;
  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS oa;

  /* motion adaptive deinterlacing (CUSTOM) of the middle of three frames, both fields: what ffmpeg's yadif makes of them, processed as progressive pictures */
  {
    size_t n0, n1, n2, nf, ns;
    BYTE *f0 = base64_decode(g_deint_frame0_b64, &n0), *f1 = base64_decode(g_deint_frame1_b64, &n1), *f2 = base64_decode(g_deint_frame2_b64, &n2);
    BYTE *first = base64_decode(g_deint_yadif_first_b64, &nf), *second = base64_decode(g_deint_yadif_second_b64, &ns);
    const UINT w = DEINT_W, h = DEINT_H, bytes = w * h * 3 / 2;
    CHECK(f0 && f1 && f2 && first && second && n0 == bytes && nf == bytes);
    ID3D12Resource *t0 = nv12_of(f0, w, h, 0), *t1 = nv12_of(f1, w, h, 0), *t2 = nv12_of(f2, w, h, 0), *rf = nv12_of(first, w, h, 0), *rs = nv12_of(second, w, h, 0);
    ID3D12Resource *out = nv12_of(NULL, w, h, 1), *expect = nv12_of(NULL, w, h, 1);
    vp_descs(DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, &isd, &osd);
    ID3D12VideoProcessor *plain = make_vp(&isd, &osd);
    isd.FieldType = D3D12_VIDEO_FIELD_TYPE_INTERLACED_TOP_FIELD_FIRST;
    isd.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_CUSTOM;
    isd.NumPastFrames = isd.NumFutureFrames = 1;
    ID3D12VideoProcessor *adaptive = make_vp(&isd, &osd);
    int same = 0;
    for (UINT field = 0; plain && adaptive && t0 && out && field < 2; ++field) {
      stream_args(&ia, t1, w, h, w, h);
      ID3D12Resource *past[1] = {t0}, *future[1] = {t2};
      UINT subs[1] = {0};
      ia.InputStream[0].ReferenceSet = (D3D12_VIDEO_PROCESS_REFERENCE_SET){1, past, subs, 1, future, subs};
      ia.RateInfo.OutputIndex = field;
      ia.RateInfo.InputFrameOrField = 1;
      out_args(&oa, out, w, h);
      if (field == 0) rig_run(&rig, adaptive, &oa, 1, &ia, NULL);
      else {      /* (the second field through ProcessFrames1, its field type given with the frame) */
        D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS1 ia1;
        memset(&ia1, 0, sizeof ia1);
        memcpy(&ia1, &ia, sizeof ia);
        ia1.FieldType = D3D12_VIDEO_FIELD_TYPE_INTERLACED_TOP_FIELD_FIRST;
        rig_run(&rig, adaptive, &oa, 1, NULL, &ia1);
      }
      stream_args(&ia, field ? rs : rf, w, h, w, h);
      out_args(&oa, expect, w, h);
      rig_run(&rig, plain, &oa, 1, &ia, NULL);
      static BYTE a[64 * 32 * 3 / 2], b[64 * 32 * 3 / 2];
      copy_subresources(out, 0, 2, a, bytes, 0);
      copy_subresources(expect, 0, 2, b, bytes, 0);
      int diff = 0;
      for (UINT i = 0; i < bytes; ++i) if (a[i] != b[i]) ++diff;
      printf("d3d12videotest: motion adaptive deinterlacing, field %u: %d bytes differ from ffmpeg's yadif\n", field, diff);
      same += diff == 0;
    }
    CHECK(same == 2);
    /* bob of the same frame: the shown field's rows as they are */
    isd.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_BOB;
    isd.NumPastFrames = isd.NumFutureFrames = 0;
    ID3D12VideoProcessor *bob = make_vp(&isd, &osd);
    if (bob && out) {
      static BYTE a[64 * 32 * 3 / 2];
      int kept = 0, filled = 0;
      for (UINT field = 0; field < 2; ++field) {
        stream_args(&ia, t1, w, h, w, h);
        ia.RateInfo.OutputIndex = field;
        out_args(&oa, out, w, h);
        rig_run(&rig, bob, &oa, 1, &ia, NULL);
        copy_subresources(out, 0, 2, a, bytes, 0);
        for (UINT y = 0; y < h; ++y)
          for (UINT x = 0; x < w; ++x) {
            if ((y & 1) == field) kept += a[y * w + x] == f1[y * w + x];
            else if (y > 0 && y + 1 < h) filled += abs(a[y * w + x] - (f1[(y - 1) * w + x] + f1[(y + 1) * w + x] + 1) / 2) <= 1;      /* (a half rounds either way through the studio-range matrices) */
          }
      }
      printf("d3d12videotest: bob: %d of %u shown samples kept, %d of %u missing ones interpolated\n", kept, w * h, filled, w * (h - 2));
      CHECK(kept == (int)(w * h) && filled == (int)(w * (h - 2)));
      ID3D12VideoProcessor_Release(bob);
    }
    if (plain) ID3D12VideoProcessor_Release(plain);
    if (adaptive) ID3D12VideoProcessor_Release(adaptive);
    ID3D12Resource *all[] = {t0, t1, t2, rf, rs, out, expect};
    for (UINT i = 0; i < 7; ++i) if (all[i]) ID3D12Resource_Release(all[i]);
    free(f0); free(f1); free(f2); free(first); free(second);
  }

  /* ProcAmp on a flat colour: DXVA's formulas, then BT.709 */
  {
    static BYTE frame[32 * 16 * 3 / 2];
    memset(frame, 120, 32 * 16);
    for (UINT i = 0; i < 32 * 8; i += 2) { frame[32 * 16 + i] = 100; frame[32 * 16 + i + 1] = 150; }
    ID3D12Resource *in = nv12_of(frame, 32, 16, 0), *out = texture(DXGI_FORMAT_B8G8R8A8_UNORM, 32, 16, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    vp_descs(DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &isd, &osd);
    isd.FilterFlags = D3D12_VIDEO_PROCESS_FILTER_FLAG_BRIGHTNESS | D3D12_VIDEO_PROCESS_FILTER_FLAG_CONTRAST | D3D12_VIDEO_PROCESS_FILTER_FLAG_HUE | D3D12_VIDEO_PROCESS_FILTER_FLAG_SATURATION;
    ID3D12VideoProcessor *vp = make_vp(&isd, &osd);
    if (vp && in && out) {
      stream_args(&ia, in, 32, 16, 32, 16);
      ia.FilterLevels[0] = 40;       /* brightness 10 */
      ia.FilterLevels[1] = 77;       /* contrast 77 / 64 */
      ia.FilterLevels[2] = 80;       /* hue 20 degrees */
      ia.FilterLevels[3] = 51;       /* saturation 51 / 64 */
      out_args(&oa, out, 32, 16);
      rig_run(&rig, vp, &oa, 1, &ia, NULL);
      static BYTE px[32 * 16 * 4];
      copy_subresources(out, 0, 1, px, sizeof px, 0);
      const double c = 77 / 64.0, s = 51 / 64.0, hue = 20 * 3.14159265358979 / 180;
      const double y = (120 - 16) * c + 10 + 16, cb = ((100 - 128) * cos(hue) + (150 - 128) * sin(hue)) * c * s + 128, cr = ((150 - 128) * cos(hue) - (100 - 128) * sin(hue)) * c * s + 128;
      double rgb[3];
      const double Y = (y - 16) / 219.0, Pb = (cb - 128) / 224.0, Pr = (cr - 128) / 224.0;
      rgb[0] = Y + 1.5748 * Pr; rgb[1] = Y - 0.1873 * Pb - 0.4681 * Pr; rgb[2] = Y + 1.8556 * Pb;
      for (int k = 0; k < 3; ++k) rgb[k] = (rgb[k] < 0 ? 0 : rgb[k] > 1 ? 1 : rgb[k]) * 255;
      printf("d3d12videotest: ProcAmp: (%d, %d, %d), DXVA's formulas (%.1f, %.1f, %.1f)\n", px[(8 * 32 + 16) * 4 + 2], px[(8 * 32 + 16) * 4 + 1], px[(8 * 32 + 16) * 4], rgb[0], rgb[1], rgb[2]);
      CHECK(bgra_at(px, 32, 16, 8, (int)lround(rgb[0]), (int)lround(rgb[1]), (int)lround(rgb[2]), 2));
      /* a level outside the range: nothing is done */
      memset(px, 0, sizeof px);
      copy_subresources(out, 0, 1, px, sizeof px, 1);
      ia.FilterLevels[1] = 641;
      rig_run(&rig, vp, &oa, 1, &ia, NULL);
      copy_subresources(out, 0, 1, px, sizeof px, 0);
      CHECK(bgra_at(px, 32, 16, 8, 0, 0, 0, 0));
      ID3D12VideoProcessor_Release(vp);
    }
    if (in) ID3D12Resource_Release(in);
    if (out) ID3D12Resource_Release(out);
  }

  /* luma key: a ramp of luma codes 0, 4, .. 252; from 0.25 (code 63) to 0.5 (127) is transparent: the green background */
  {
    static BYTE frame[64 * 2 * 3 / 2];
    for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 64; ++x) frame[y * 64 + x] = (BYTE)(x * 4);
    memset(frame + 128, 128, 64);
    ID3D12Resource *in = nv12_of(frame, 64, 2, 0), *out = texture(DXGI_FORMAT_B8G8R8A8_UNORM, 64, 2, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    vp_descs(DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &isd, &osd);
    isd.LumaKey.Enable = TRUE;
    isd.LumaKey.Lower = 0.25f;
    isd.LumaKey.Upper = 0.5f;
    osd.BackgroundColor[1] = 1.0f;
    ID3D12VideoProcessor *vp = make_vp(&isd, &osd);
    if (vp && in && out) {
      stream_args(&ia, in, 64, 2, 64, 2);
      out_args(&oa, out, 64, 2);
      rig_run(&rig, vp, &oa, 1, &ia, NULL);
      static BYTE px[64 * 2 * 4];
      copy_subresources(out, 0, 1, px, sizeof px, 0);
      int right = 0;
      for (UINT x = 0; x < 64; ++x) {
        const int keyed = x * 4 >= 63 && x * 4 <= 127;
        right += keyed ? bgra_at(px, 64, x, 0, 0, 255, 0, 0) : bgra_at(px, 64, x, 0, x * 4, x * 4, x * 4, 1);
      }
      printf("d3d12videotest: luma key: %d of 64 columns right\n", right);
      CHECK(right == 64);
      ID3D12VideoProcessor_Release(vp);
    }
    if (in) ID3D12Resource_Release(in);
    if (out) ID3D12Resource_Release(out);
  }

  /* stereo: a side by side picture (left view red, right view blue) into two outputs, and into one (the left view) */
  {
    static BYTE pic[32 * 16 * 4];
    for (UINT y = 0; y < 16; ++y)
      for (UINT x = 0; x < 32; ++x) { BYTE *p = &pic[(y * 32 + x) * 4]; p[0] = x < 16 ? 0 : 255; p[1] = 0; p[2] = x < 16 ? 255 : 0; p[3] = 255; }
    ID3D12Resource *in = texture(DXGI_FORMAT_B8G8R8A8_UNORM, 32, 16, 1, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource *left = texture(DXGI_FORMAT_B8G8R8A8_UNORM, 16, 16, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), *right = texture(DXGI_FORMAT_B8G8R8A8_UNORM, 16, 16, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (in) copy_subresources(in, 0, 1, pic, sizeof pic, 1);
    vp_descs(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &isd, &osd);
    isd.StereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_HORIZONTAL;
    osd.EnableStereo = TRUE;
    ID3D12VideoProcessor *vp = make_vp(&isd, &osd);
    osd.EnableStereo = FALSE;
    ID3D12VideoProcessor *mono = make_vp(&isd, &osd);
    if (vp && mono && in && left && right) {
      stream_args(&ia, in, 16, 16, 16, 16);      /* (the source rectangle is in the view's pixels) */
      out_args(&oa, left, 16, 16);
      oa.OutputStream[1].pTexture2D = right;
      rig_run(&rig, vp, &oa, 1, &ia, NULL);
      static BYTE a[16 * 16 * 4], b[16 * 16 * 4];
      copy_subresources(left, 0, 1, a, sizeof a, 0);
      copy_subresources(right, 0, 1, b, sizeof b, 0);
      CHECK(bgra_at(a, 16, 0, 0, 255, 0, 0, 0) && bgra_at(a, 15, 15, 15, 255, 0, 0, 0) && bgra_at(b, 16, 0, 0, 0, 0, 255, 0) && bgra_at(b, 16, 15, 15, 0, 0, 255, 0));
      memset(b, 0, sizeof b);
      copy_subresources(right, 0, 1, b, sizeof b, 1);
      out_args(&oa, right, 16, 16);
      rig_run(&rig, mono, &oa, 1, &ia, NULL);
      copy_subresources(right, 0, 1, b, sizeof b, 0);
      CHECK(bgra_at(b, 16, 8, 8, 255, 0, 0, 0));
      printf("d3d12videotest: stereo side by side: left view red, right view blue\n");
    }
    if (vp) ID3D12VideoProcessor_Release(vp);
    if (mono) ID3D12VideoProcessor_Release(mono);
    if (in) ID3D12Resource_Release(in);
    if (left) ID3D12Resource_Release(left);
    if (right) ID3D12Resource_Release(right);
  }

  /* HDR10 (PQ, BT.2020) into SDR: 1000 cd/m2 (the content's peak when it is not known) is white, 50 cd/m2 below the knee is as it is (over 203 cd/m2) */
  {
    const UINT w = 16, h = 16;
    static USHORT y16[16 * 16], uv16[8 * 8 * 2];
    const UINT peak = (UINT)lround(64 + 876 * pq_signal(1000)), dim = (UINT)lround(64 + 876 * pq_signal(50));
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) y16[y * w + x] = (USHORT)((x < 8 ? peak : dim) << 6);
    for (UINT i = 0; i < 8 * 8 * 2; ++i) uv16[i] = 512 << 6;
    ID3D12Resource *in = texture(DXGI_FORMAT_P010, w, h, 1, D3D12_RESOURCE_FLAG_NONE), *out = texture(DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (in) { copy_subresources(in, 0, 1, (BYTE *)y16, sizeof y16, 1); copy_subresources(in, 1, 1, (BYTE *)uv16, sizeof uv16, 1); }
    vp_descs(DXGI_FORMAT_P010, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &isd, &osd);
    ID3D12VideoProcessor *vp = make_vp(&isd, &osd);
    if (vp && in && out) {
      stream_args(&ia, in, w, h, w, h);
      out_args(&oa, out, w, h);
      rig_run(&rig, vp, &oa, 1, &ia, NULL);
      static BYTE px[16 * 16 * 4];
      copy_subresources(out, 0, 1, px, sizeof px, 0);
      const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
      const double e = ((double)dim - 64) / 876, p = pow(e, 1 / m2), nits = 10000 * pow(fmax(p - c1, 0) / (c2 - c3 * p), 1 / m1), lin = nits / 203;
      const int expect = (int)lround(255 * (1.055 * pow(lin, 1 / 2.4) - 0.055));
      printf("d3d12videotest: PQ to SDR: 1000 cd/m2 -> %d, 50 cd/m2 -> %d (expected %d)\n", px[(4 * w + 2) * 4 + 1], px[(4 * w + 12) * 4 + 1], expect);
      CHECK(bgra_at(px, w, 2, 4, 255, 255, 255, 1) && bgra_at(px, w, 12, 4, expect, expect, expect, 2));
      ID3D12VideoProcessor_Release(vp);
    }
    if (in) ID3D12Resource_Release(in);
    if (out) ID3D12Resource_Release(out);
  }
  rig_close(&rig);
}
