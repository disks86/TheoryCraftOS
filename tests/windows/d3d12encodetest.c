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
/* Direct3D 12 video encoding and motion estimation (d3d12.dll encode.inc over the kernel's CPU encoder): every encoder feature query (what is supported and what is refused,
 * SUPPORT .. SUPPORT2, the input map queries of the later SDKs), the motion estimator (a textured picture shifted by a known amount gives that vector for every inner block,
 * resolved into an R16G16_SINT texture), and H.264 encodes as a program does them - its own SPS and PPS, the decoded picture buffer an NV12 array of the picture's size,
 * IDR P B B P - in four configurations: Main CAVLC with two slices; High CABAC with the 8x8 transform and temporal direct prediction at 100x60 (cropped); High CAVLC with the
 * 8x8 transform, constrained intra prediction, spatial direct prediction, row-based intra refresh and slices of 400 bytes; Main CABAC with spatial direct prediction at
 * 118x46, a slice a row, through ID3D12VideoEncodeCommandList4::EncodeFrame1 with dirty rectangles and move regions from the CPU and the PSNR metadata. Each picture's
 * metadata is resolved and checked, then the stream decoded again through ID3D12VideoDecodeCommandList (tests/windows/dxva_streams.h) and compared with the encoder's
 * reconstructed pictures, sample for sample. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
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
static ID3D12VideoDevice3 *g_vdev;

static void wait_queue(ID3D12CommandQueue *q) {
  HANDLE e = CreateEventW(NULL, FALSE, FALSE, NULL);
  ID3D12CommandQueue_Signal(q, g_fence, ++g_value);
  ID3D12Fence_SetEventOnCompletion(g_fence, g_value, e);
  WaitForSingleObject(e, 60000);
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
static ID3D12Resource *texture(DXGI_FORMAT f, UINT w, UINT h, UINT layers) {
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, (UINT16)layers, 1, f, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  ID3D12Resource *r = NULL;
  ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&r);
  return r;
}
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
/* One subresource of `tex` from `data` (rows packed), or into it. */
static void copy_subresource(ID3D12Resource *tex, UINT sub, BYTE *data, UINT64 bytes, int upload) {
  D3D12_RESOURCE_DESC rd;
  tex->lpVtbl->GetDesc(tex, &rd);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT rows;
  UINT64 row_bytes, total = 0;
  ID3D12Device_GetCopyableFootprints(g_dev, &rd, sub, 1, 0, &fp, &rows, &row_bytes, &total);
  ID3D12Resource *buf = buffer(upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK, total);
  BYTE *map = NULL;
  ID3D12Resource_Map(buf, 0, NULL, (void **)&map);
  UINT64 at = 0;
  if (upload)
    for (UINT r = 0; r < rows && at + row_bytes <= bytes; ++r, at += row_bytes) memcpy(map + fp.Offset + (UINT64)r * fp.Footprint.RowPitch, data + at, (size_t)row_bytes);
  Copier c;
  copier_begin(&c);
  D3D12_TEXTURE_COPY_LOCATION t = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
  D3D12_TEXTURE_COPY_LOCATION b = {buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = fp}};
  if (upload) ID3D12GraphicsCommandList_CopyTextureRegion(c.l, &t, 0, 0, 0, &b, NULL);
  else ID3D12GraphicsCommandList_CopyTextureRegion(c.l, &b, 0, 0, 0, &t, NULL);
  copier_run(&c);
  if (!upload) {
    ID3D12Resource_Unmap(buf, 0, NULL);
    ID3D12Resource_Map(buf, 0, NULL, (void **)&map);
    for (UINT r = 0; r < rows && at + row_bytes <= bytes; ++r, at += row_bytes) memcpy(data + at, map + fp.Offset + (UINT64)r * fp.Footprint.RowPitch, (size_t)row_bytes);
  }
  ID3D12Resource_Unmap(buf, 0, NULL);
  ID3D12Resource_Release(buf);
}
/* Runs one video list on a queue of its type and resets it. */
static void run_video(ID3D12CommandQueue *q, ID3D12CommandList *l, ID3D12CommandAllocator *a, int encode) {
  if (encode) ID3D12VideoEncodeCommandList_Close((ID3D12VideoEncodeCommandList *)l);
  else ID3D12VideoDecodeCommandList_Close((ID3D12VideoDecodeCommandList *)l);
  ID3D12CommandQueue_ExecuteCommandLists(q, 1, &l);
  wait_queue(q);
  if (encode) ID3D12VideoEncodeCommandList_Reset((ID3D12VideoEncodeCommandList *)l, a);
  else ID3D12VideoDecodeCommandList_Reset((ID3D12VideoDecodeCommandList *)l, a);
}

/* A smooth textured picture: what moves in the tests. */
static BYTE pattern(int x, int y) {
  unsigned h = (unsigned)(x / 3) * 2654435761u ^ (unsigned)(y / 3) * 40503u;
  h ^= h >> 13;
  return (BYTE)(64 + (h & 127) / 2 + ((x + 2 * y) & 31));
}

/* ---- the feature queries ----------------------------------------------------------------------------------------------------------------------------------------- */

static D3D12_VIDEO_ENCODER_PROFILE_H264 g_main = D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
static D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 g_config = {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_NONE, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED,
                                                              D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_0_ALL_LUMA_CHROMA_SLICE_BLOCK_EDGES_ALWAYS_FILTERED};
static D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 g_gop = {30, 3, 0, 0, 2};
static D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP g_cqp = {26, 28, 30};

static D3D12_VIDEO_ENCODER_PROFILE_DESC main_profile(void) { D3D12_VIDEO_ENCODER_PROFILE_DESC p; p.DataSize = sizeof g_main; p.pH264Profile = &g_main; return p; }
static D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION config_desc(void) { D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION c; c.DataSize = sizeof g_config; c.pH264Config = &g_config; return c; }
static D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop_desc(void) { D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE g; g.DataSize = sizeof g_gop; g.pH264GroupOfPictures = &g_gop; return g; }
static D3D12_VIDEO_ENCODER_RATE_CONTROL cqp_desc(void) {
  D3D12_VIDEO_ENCODER_RATE_CONTROL rc;
  memset(&rc, 0, sizeof rc);
  rc.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
  rc.ConfigParams.DataSize = sizeof g_cqp;
  rc.ConfigParams.pConfiguration_CQP = &g_cqp;
  rc.TargetFrameRate.Numerator = 30;
  rc.TargetFrameRate.Denominator = 1;
  return rc;
}

static void test_features(void) {
  ID3D12VideoDevice3 *v = g_vdev;
  D3D12_FEATURE_DATA_VIDEO_FEATURE_AREA_SUPPORT area = {0};
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_FEATURE_AREA_SUPPORT, &area, sizeof area));
  CHECK(area.VideoEncodeSupport && area.VideoDecodeSupport);
  /* codecs: H.264 only */
  D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codec = {0, D3D12_VIDEO_ENCODER_CODEC_H264, FALSE};
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC, &codec, sizeof codec));
  CHECK(codec.IsSupported);
  codec.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC, &codec, sizeof codec));
  CHECK(!codec.IsSupported);
  codec.Codec = D3D12_VIDEO_ENCODER_CODEC_AV1;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC, &codec, sizeof codec));
  CHECK(!codec.IsSupported);
  CHECK(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC, &codec, sizeof codec - 1) == E_INVALIDARG);
  /* profiles and levels */
  D3D12_VIDEO_ENCODER_PROFILE_H264 prof = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
  D3D12_VIDEO_ENCODER_LEVELS_H264 lo = 99, hi = 99;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL pl;
  memset(&pl, 0, sizeof pl);
  pl.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
  pl.Profile.DataSize = sizeof prof; pl.Profile.pH264Profile = &prof;
  pl.MinSupportedLevel.DataSize = sizeof lo; pl.MinSupportedLevel.pH264LevelSetting = &lo;
  pl.MaxSupportedLevel.DataSize = sizeof hi; pl.MaxSupportedLevel.pH264LevelSetting = &hi;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL, &pl, sizeof pl));
  CHECK(pl.IsSupported && lo == D3D12_VIDEO_ENCODER_LEVELS_H264_1 && hi == D3D12_VIDEO_ENCODER_LEVELS_H264_62);
  prof = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH_10;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL, &pl, sizeof pl));
  CHECK(!pl.IsSupported);
  /* resolutions: whole macroblocks, 16 .. 4096 */
  D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT rc_count = {0, D3D12_VIDEO_ENCODER_CODEC_H264, 99};
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT, &rc_count, sizeof rc_count));
  CHECK(rc_count.ResolutionRatiosCount == 0);
  D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION res;
  memset(&res, 0, sizeof res);
  res.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION, &res, sizeof res));
  CHECK(res.IsSupported && res.MinResolutionSupported.Width == 16 && res.MaxResolutionSupported.Width == 4096 && res.ResolutionWidthMultipleRequirement == 2 &&
        res.ResolutionHeightMultipleRequirement == 2);
  /* input formats */
  D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT inf = {0, D3D12_VIDEO_ENCODER_CODEC_H264, main_profile(), DXGI_FORMAT_NV12, FALSE};
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &inf, sizeof inf));
  CHECK(inf.IsSupported);
  inf.Format = DXGI_FORMAT_P010;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &inf, sizeof inf));
  CHECK(!inf.IsSupported);
  /* rate control modes: all five */
  int modes = 0;
  for (int m = 0; m <= 4; ++m) {
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_MODE rm = {0, D3D12_VIDEO_ENCODER_CODEC_H264, (D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE)m, FALSE};
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_RATE_CONTROL_MODE, &rm, sizeof rm));
    modes += rm.IsSupported;
  }
  CHECK(modes == 5);
  /* intra refresh: none and row based */
  D3D12_VIDEO_ENCODER_LEVELS_H264 level = D3D12_VIDEO_ENCODER_LEVELS_H264_31;
  D3D12_VIDEO_ENCODER_LEVEL_SETTING ls;
  ls.DataSize = sizeof level; ls.pH264LevelSetting = &level;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_INTRA_REFRESH_MODE ir = {0, D3D12_VIDEO_ENCODER_CODEC_H264, main_profile(), ls, D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE, FALSE};
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_INTRA_REFRESH_MODE, &ir, sizeof ir));
  CHECK(ir.IsSupported);
  ir.IntraRefreshMode = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_INTRA_REFRESH_MODE, &ir, sizeof ir));
  CHECK(ir.IsSupported);
  ir.IntraRefreshMode = (D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE)2;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_INTRA_REFRESH_MODE, &ir, sizeof ir));
  CHECK(!ir.IsSupported);
  /* slice layouts: full, bytes, macroblocks, rows, count; not grids (AV1's) or automatic */
  int layouts = 0;
  for (int m = 0; m <= 7; ++m) {
    D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE sm = {0, D3D12_VIDEO_ENCODER_CODEC_H264, main_profile(), ls, (D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE)m, FALSE};
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE, &sm, sizeof sm));
    layouts |= sm.IsSupported << m;
  }
  CHECK(layouts == 0x1F);
  /* the heap: the rate controller's state and the reference store (motion, the coded size's last columns and rows) */
  D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC r96 = {96, 64}, odd = {101, 64}, cropped = {100, 60};
  D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE hs;
  memset(&hs, 0, sizeof hs);
  hs.HeapDesc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
  hs.HeapDesc.EncodeProfile = main_profile();
  hs.HeapDesc.EncodeLevel = ls;
  hs.HeapDesc.ResolutionsListCount = 1;
  hs.HeapDesc.pResolutionList = &r96;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE, &hs, sizeof hs));
  CHECK(hs.IsSupported && hs.MemoryPoolL1Size == 256 + 32 + 17 * (64 + 24 * 40 + (15 * (96 + 64) * 3 / 2 + 7) / 8 * 8));
  hs.HeapDesc.pResolutionList = &cropped;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE, &hs, sizeof hs));
  CHECK(hs.IsSupported);
  hs.HeapDesc.pResolutionList = &odd;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE, &hs, sizeof hs));
  CHECK(!hs.IsSupported);
  D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE1 hs1;
  memset(&hs1, 0, sizeof hs1);
  hs1.HeapDesc.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_ALLOW_DIRTY_REGIONS_;
  hs1.HeapDesc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
  hs1.HeapDesc.EncodeProfile = main_profile();
  hs1.HeapDesc.EncodeLevel = ls;
  hs1.HeapDesc.ResolutionsListCount = 1;
  hs1.HeapDesc.pResolutionList = &r96;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE1, &hs1, sizeof hs1));
  CHECK(hs1.IsSupported && hs1.MemoryPoolL1Size == 256 + 32 + 17 * (64 + 24 * 40 + (15 * (96 + 64) * 3 / 2 + 7) / 8 * 8));
  hs1.HeapDesc.Pow2DownscaleFactor = 1;      /* (frame analysis: not done) */
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE1, &hs1, sizeof hs1));
  CHECK(!hs1.IsSupported);
  hs1.HeapDesc.Pow2DownscaleFactor = 0;
  hs1.HeapDesc.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_ALLOW_SUBREGION_NOTIFICATION_ARRAY_OF_BUFFERS_;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE1, &hs1, sizeof hs1));
  CHECK(!hs1.IsSupported);
  /* the configuration's limits */
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264 cs;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT ccs;
  memset(&ccs, 0, sizeof ccs);
  ccs.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
  ccs.Profile = main_profile();
  ccs.CodecSupportLimits.DataSize = sizeof cs;
  ccs.CodecSupportLimits.pH264Support = &cs;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &ccs, sizeof ccs));
  CHECK(ccs.IsSupported && (cs.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CONSTRAINED_INTRAPREDICTION_SUPPORT) &&
        (cs.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CABAC_ENCODING_SUPPORT) &&
        (cs.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_DIRECT_SPATIAL_ENCODING_SUPPORT) &&
        (cs.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_DIRECT_TEMPORAL_ENCODING_SUPPORT) &&
        !(cs.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_ADAPTIVE_8x8_TRANSFORM_ENCODING_SUPPORT) &&
        !(cs.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_INTRA_SLICE_CONSTRAINED_ENCODING_SUPPORT) && cs.DisableDeblockingFilterSupportedModes == 7);
  D3D12_VIDEO_ENCODER_PROFILE_H264 high_profile = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
  ccs.Profile.pH264Profile = &high_profile;      /* (the 8x8 transform is High's) */
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &ccs, sizeof ccs));
  CHECK(ccs.IsSupported && (cs.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_ADAPTIVE_8x8_TRANSFORM_ENCODING_SUPPORT));
  /* the picture control's limits */
  D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_H264 ps;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT pcs;
  memset(&pcs, 0, sizeof pcs);
  pcs.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
  pcs.Profile = main_profile();
  pcs.PictureSupport.DataSize = sizeof ps;
  pcs.PictureSupport.pH264Support = &ps;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT, &pcs, sizeof pcs));
  CHECK(pcs.IsSupported && ps.MaxL0ReferencesForP >= 2 && ps.MaxL1ReferencesForB >= 1 && ps.MaxDPBCapacity == 16);
  /* a whole configuration (SUPPORT1): good, then with CABAC, then with slices by bytes, then an odd size */
  D3D12_VIDEO_ENCODER_PROFILE_H264 suggested = 99;
  D3D12_VIDEO_ENCODER_LEVELS_H264 suggested_level = 99;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS limits;
  D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES two = {{2}};
  D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1 s;
  memset(&s, 0, sizeof s);
  s.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
  s.InputFormat = DXGI_FORMAT_NV12;
  s.CodecConfiguration = config_desc();
  s.CodecGopSequence = gop_desc();
  s.RateControl = cqp_desc();
  s.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
  s.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME;
  s.ResolutionsListCount = 1;
  s.pResolutionList = &r96;
  s.SuggestedProfile.DataSize = sizeof suggested; s.SuggestedProfile.pH264Profile = &suggested;
  s.SuggestedLevel.DataSize = sizeof suggested_level; s.SuggestedLevel.pH264LevelSetting = &suggested_level;
  s.pResolutionDependentSupport = &limits;
  s.SubregionFrameEncodingData.DataSize = sizeof two;
  s.SubregionFrameEncodingData.pSlicesPartition_H264 = &two;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s));
  CHECK(s.ValidationFlags == 0 && (s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) && (s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_DELTA_QP_AVAILABLE));
  CHECK(suggested == D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN && suggested_level == D3D12_VIDEO_ENCODER_LEVELS_H264_1 && limits.MaxSubregionsNumber == 24 && limits.SubregionBlockPixelsSize == 16 &&
        limits.MaxIntraRefreshFrameDuration == 4);
  CHECK((s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_FRAME_PSNR_METADATA_AVAILABLE_) && (s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_SUBREGIONS_PSNR_METADATA_AVAILABLE_) &&
        (s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_INTRA_REFRESH_NO_SUBREGION_LAYOUT_CONSTRAINTS_AVAILABLE_) &&
        !(s.SupportFlags & (D3D12_VIDEO_ENCODER_SUPPORT_FLAGS)(0x10000 | 0x20000 | 0x40000 | 0x80000 | 0x100000)) &&      /* (no per-block maps, no subregion notification) */
        !(s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_FRAME_ANALYSIS_AVAILABLE));
  /* CABAC, the 8x8 transform (High suggested), direct prediction, intra refresh, slices by bytes, a cropped size: all taken */
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 cabac = g_config;
  cabac.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING | D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM;
  cabac.DirectModeConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_TEMPORAL;
  s.CodecConfiguration.pH264Config = &cabac;
  s.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s));
  CHECK(s.ValidationFlags == 0 && (s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) && suggested == D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH);
  s.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
  /* (the deblocking modes 3 - 6 are SVC's, intra constrained slices are not done) */
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 svc = g_config;
  svc.DisableDeblockingFilterConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_3_USE_TWO_STAGE_DEBLOCKING;
  s.CodecConfiguration.pH264Config = &svc;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s));
  CHECK((s.ValidationFlags & D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED) && !(s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK));
  svc = g_config;
  svc.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ALLOW_REQUEST_INTRA_CONSTRAINED_SLICES;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s));
  CHECK(s.ValidationFlags & D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED);
  s.CodecConfiguration = config_desc();
  D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES bytes = {{1000}};
  s.SubregionFrameEncodingData.pSlicesPartition_H264 = &bytes;
  s.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_BYTES_PER_SUBREGION;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s));
  CHECK(s.ValidationFlags == 0);
  s.SubregionFrameEncodingData.pSlicesPartition_H264 = &two;
  s.SubregionFrameEncoding = (D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE)5;      /* (UNIFORM_GRID_PARTITION, AV1's) */
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s));
  CHECK(s.ValidationFlags == D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_MODE_NOT_SUPPORTED);
  s.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
  s.pResolutionList = &cropped;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s));
  CHECK(s.ValidationFlags == 0 && limits.MaxSubregionsNumber == 28);
  s.pResolutionList = &odd;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s));
  CHECK(s.ValidationFlags == D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST);
  /* SUPPORT2: the input maps from the CPU are taken, from GPU textures not; no frame analysis */
  {
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS1 lim1;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT2 s2;
    memset(&s2, 0, sizeof s2);
    memcpy(&s2, &s, offsetof(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT2, pResolutionDependentSupport));
    s2.pResolutionList = &r96;
    s2.pResolutionDependentSupport = &lim1;
    s2.SubregionFrameEncodingData = s.SubregionFrameEncodingData;
    s2.QPMap.Enabled = TRUE; s2.QPMap.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER;
    s2.DirtyRegions.Enabled = TRUE; s2.DirtyRegions.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER; s2.DirtyRegions.MapValuesType = D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE_SKIP;
    s2.MotionSearch.Enabled = TRUE; s2.MotionSearch.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER; s2.MotionSearch.MotionSearchMode = D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_START_HINT_LIMITED_DISTANCE;
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT2, &s2, sizeof s2));
    CHECK(s2.ValidationFlags == 0 && (s2.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) && lim1.MaxSubregionsNumber == 24 && lim1.MaxIntraRefreshFrameDuration == 4 &&
          lim1.DirtyRegions.DirtyRegionsSupportFlags == (D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_REPEAT_FRAME | D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_DIRTY_REGIONS) &&
          lim1.MotionSearch.SupportFlags == D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAG_SUPPORTED && lim1.MotionSearch.MaxDeviation > 0 &&
          lim1.FrameAnalysis.SupportFlags == D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_NONE);
    s2.QPMap.MapSource = s2.DirtyRegions.MapSource = s2.MotionSearch.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_GPU_TEXTURE;
    s2.FrameAnalysis.Enabled = TRUE;
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT2, &s2, sizeof s2));
    CHECK(s2.ValidationFlags == (D3D12_VIDEO_ENCODER_VALIDATION_FLAG_QPMAP_NOT_SUPPORTED_ | D3D12_VIDEO_ENCODER_VALIDATION_FLAG_DIRTY_REGIONS_NOT_SUPPORTED_ |
                                 D3D12_VIDEO_ENCODER_VALIDATION_FLAG_MOTION_SEARCH_NOT_SUPPORTED_ | D3D12_VIDEO_ENCODER_VALIDATION_FLAG_FRAME_ANALYSIS_NOT_SUPPORTED_) &&
          !(s2.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK));
    /* the input map queries */
    D3D12_VIDEO_ENCODER_INPUT_MAP_SESSION_INFO si;
    memset(&si, 0, sizeof si);
    si.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    si.Profile = main_profile();
    si.Level = ls;
    si.InputFormat = DXGI_FORMAT_NV12;
    si.InputResolution = r96;
    si.CodecConfiguration = config_desc();
    si.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_QPMAP_INPUT qi = {0, si, D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER, FALSE, 0, 0};
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_QPMAP_INPUT, &qi, sizeof qi));
    CHECK(qi.IsSupported && qi.BlockSize == 16);
    qi.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_GPU_TEXTURE;
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_QPMAP_INPUT, &qi, sizeof qi));
    CHECK(!qi.IsSupported);
    D3D12_FEATURE_DATA_VIDEO_ENCODER_DIRTY_REGIONS dr = {0, si, D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER, D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE_DIRTY, 0, 0};
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_DIRTY_REGIONS, &dr, sizeof dr));
    CHECK(dr.SupportFlags == (D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_REPEAT_FRAME | D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_DIRTY_REGIONS));
    dr.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_GPU_TEXTURE;
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_DIRTY_REGIONS, &dr, sizeof dr));
    CHECK(dr.SupportFlags == D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_NONE);
    D3D12_FEATURE_DATA_VIDEO_ENCODER_MOTION_SEARCH ms;
    memset(&ms, 0, sizeof ms);
    ms.SessionInfo = si;
    ms.MotionSearchMode = D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_START_HINT;
    ms.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER;
    ms.BidirectionalRefFrameEnabled = TRUE;
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_MOTION_SEARCH, &ms, sizeof ms));
    CHECK(ms.SupportFlags == D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAG_SUPPORTED && ms.MaxMotionHints > 0 && ms.MotionUnitPrecisionSupport == 7);
    ms.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_GPU_TEXTURE;
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_MOTION_SEARCH, &ms, sizeof ms));
    CHECK(ms.SupportFlags == D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAG_NONE);
    for (int t = 0; t < 3; ++t) {
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT rl = {0, si, (D3D12_VIDEO_ENCODER_INPUT_MAP_TYPE)t, TRUE, 99};
      CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT, &rl, sizeof rl));
      CHECK(!rl.IsSupported && rl.MaxResolvedBufferAllocationSize == 0);
    }
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS fa;
    memset(&fa, 0, sizeof fa);
    fa.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    fa.Profile = main_profile();
    fa.Level = ls;
    fa.InputFormat = DXGI_FORMAT_NV12;
    fa.InputResolution = r96;
    fa.CodecConfiguration = config_desc();
    fa.Pow2DownscaleFactor = 1;
    fa.SupportFlags = (D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAGS)0x3F;
    CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS, &fa, sizeof fa));
    CHECK(fa.SupportFlags == D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_NONE);
  }
  /* (SUPPORT, the older one: a CBR rate with no bit rate is refused) */
  D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR cbr;
  memset(&cbr, 0, sizeof cbr);
  D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT s0;
  memcpy(&s0, &s, sizeof s0);
  s0.pResolutionList = &r96;
  s0.RateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
  s0.RateControl.ConfigParams.DataSize = sizeof cbr;
  s0.RateControl.ConfigParams.pConfiguration_CBR = &cbr;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &s0, sizeof s0));
  CHECK(s0.ValidationFlags == D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_CONFIGURATION_NOT_SUPPORTED);
  cbr.TargetBitRate = 1000000;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &s0, sizeof s0));
  CHECK(s0.ValidationFlags == 0 && (s0.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK));
  /* resource requirements */
  D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS rr = {0, D3D12_VIDEO_ENCODER_CODEC_H264, main_profile(), DXGI_FORMAT_NV12, {96, 64}, FALSE, 0, 0, 0};
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &rr, sizeof rr));
  CHECK(rr.IsSupported && rr.MaxEncoderOutputMetadataBufferSize >= sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA) + 24 * sizeof(D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA));
  D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS1 rr1;
  memset(&rr1, 0, sizeof rr1);
  rr1.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
  rr1.Profile = main_profile();
  rr1.InputFormat = DXGI_FORMAT_NV12;
  rr1.PictureTargetResolution = cropped;
  rr1.OptionalMetadata = D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_FRAME_PSNR | D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_SUBREGIONS_PSNR;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS1, &rr1, sizeof rr1));
  CHECK(rr1.IsSupported && rr1.EncoderOutputMetadataFramePSNRComponentsNumber == 3 && rr1.EncoderOutputMetadataSubregionsPSNRComponentsNumber == 3 &&
        rr1.EncoderOutputMetadataSubregionsPSNRResolvedMetadataBufferSize == 28 * 12 && rr1.MaxEncoderOutputMetadataBufferSize == 80 + 28 * 24 + 8 + 40 * 29);
  rr1.OptionalMetadata = D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_QP_MAP;      /* (the per-block maps: not offered) */
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS1, &rr1, sizeof rr1));
  CHECK(!rr1.IsSupported);
  /* the video device and the encode list of the later SDKs */
  ID3D12VideoDevice4 *v4 = NULL;
  CHECK_HR(ID3D12VideoDevice3_QueryInterface(v, &IID_ID3D12VideoDevice4, (void **)&v4));
  if (v4) v4->lpVtbl->Release(v4);
  /* (an encoder of the 8x8 transform is High's) */
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 t8 = g_config;
  t8.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM;
  D3D12_VIDEO_ENCODER_DESC bad;
  memset(&bad, 0, sizeof bad);
  bad.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
  bad.EncodeProfile = main_profile();
  bad.InputFormat = DXGI_FORMAT_NV12;
  bad.CodecConfiguration.DataSize = sizeof t8;
  bad.CodecConfiguration.pH264Config = &t8;
  void *none = (void *)1;
  CHECK(ID3D12VideoDevice3_CreateVideoEncoder(v, &bad, &IID_ID3D12VideoEncoder, &none) == E_INVALIDARG && !none);
  /* the motion estimator */
  D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR me = {0, DXGI_FORMAT_NV12, 0, 0, {0, 0, 0, 0}};
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR, &me, sizeof me));
  CHECK(me.BlockSizeFlags == (D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_8X8 | D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_16X16) &&
        me.PrecisionFlags == D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_FLAG_QUARTER_PEL && me.SizeRange.MinWidth == 16 && me.SizeRange.MaxWidth == 4096);
  me.InputFormat = DXGI_FORMAT_P010;
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR, &me, sizeof me));
  CHECK(me.BlockSizeFlags == 0);
  D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_SIZE mes;
  memset(&mes, 0, sizeof mes);
  mes.InputFormat = DXGI_FORMAT_NV12;
  mes.BlockSize = D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_16X16;
  mes.SizeRange = (D3D12_VIDEO_SIZE_RANGE){128, 128, 16, 16};
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_SIZE, &mes, sizeof mes));
  CHECK(mes.MotionVectorHeapMemoryPoolL1Size == 16 + 64 * 8);
  mes.Protected = TRUE;
  CHECK(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_SIZE, &mes, sizeof mes) == E_INVALIDARG);
  D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_PROTECTED_RESOURCES mep = {0, 1};
  CHECK_HR(ID3D12VideoDevice3_CheckFeatureSupport(v, D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_PROTECTED_RESOURCES, &mep, sizeof mep));
  CHECK(mep.SupportFlags == D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAG_NONE);
}

/* ---- motion estimation ------------------------------------------------------------------------------------------------------------------------------------------- */

static void test_motion(void) {
  const UINT W = 64, H = 48;
  ID3D12Resource *cur = texture(DXGI_FORMAT_NV12, W, H, 1), *ref = texture(DXGI_FORMAT_NV12, W, H, 1), *mvs = texture(DXGI_FORMAT_R16G16_SINT, W / 8, H / 8, 1);
  CHECK(cur && ref && mvs);
  if (!cur || !ref || !mvs) return;
  /* current(x, y) = reference(x + 3, y - 2): every block's vector is (12, -8) quarter samples */
  static BYTE y[64 * 48], uv[64 * 24];
  for (UINT j = 0; j < H; ++j)
    for (UINT i = 0; i < W; ++i) y[j * W + i] = pattern((int)i + 40, (int)j + 40);
  copy_subresource(ref, 0, y, sizeof y, 1);
  for (UINT j = 0; j < H; ++j)
    for (UINT i = 0; i < W; ++i) y[j * W + i] = pattern((int)i + 43, (int)j + 38);
  copy_subresource(cur, 0, y, sizeof y, 1);
  memset(uv, 128, sizeof uv);
  copy_subresource(cur, 1, uv, sizeof uv, 1);
  copy_subresource(ref, 1, uv, sizeof uv, 1);
  D3D12_VIDEO_MOTION_ESTIMATOR_DESC md = {0, DXGI_FORMAT_NV12, D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_8X8, D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL, {W, H, 16, 16}};
  D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC hd = {0, DXGI_FORMAT_NV12, D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_8X8, D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL, {W, H, 16, 16}};
  ID3D12VideoMotionEstimator *est = NULL;
  ID3D12VideoMotionVectorHeap *heap = NULL;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoMotionEstimator(g_vdev, &md, NULL, &IID_ID3D12VideoMotionEstimator, (void **)&est));
  CHECK_HR(ID3D12VideoDevice3_CreateVideoMotionVectorHeap(g_vdev, &hd, NULL, &IID_ID3D12VideoMotionVectorHeap, (void **)&heap));
  void *bad = (void *)1;
  CHECK(ID3D12VideoDevice3_CreateVideoMotionEstimator(g_vdev, &md, (ID3D12ProtectedResourceSession *)bad, &IID_ID3D12VideoMotionEstimator, &bad) == E_INVALIDARG && !bad);
  D3D12_VIDEO_MOTION_ESTIMATOR_DESC md2 = md;
  md2.InputFormat = DXGI_FORMAT_P010;
  bad = (void *)1;
  CHECK(ID3D12VideoDevice3_CreateVideoMotionEstimator(g_vdev, &md2, NULL, &IID_ID3D12VideoMotionEstimator, &bad) == E_INVALIDARG && !bad);
  if (est) {
    D3D12_VIDEO_MOTION_ESTIMATOR_DESC back;
    est->lpVtbl->GetDesc(est, &back);
    CHECK(back.BlockSize == md.BlockSize && back.SizeRange.MaxWidth == W);
  }
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *q = NULL;
  ID3D12CommandAllocator *a = NULL;
  ID3D12VideoEncodeCommandList *l = NULL;
  CHECK_HR(ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
  CHECK_HR(ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, &IID_ID3D12CommandAllocator, (void **)&a));
  if (a) CHECK_HR(ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, a, NULL, &IID_ID3D12VideoEncodeCommandList, (void **)&l));
  ID3D12VideoEncodeCommandList3 *l3 = NULL;
  if (l) CHECK_HR(ID3D12VideoEncodeCommandList_QueryInterface(l, &IID_ID3D12VideoEncodeCommandList3, (void **)&l3));
  if (l3) l3->lpVtbl->Release(l3);
  if (est && heap && q && l) {
    D3D12_RESOURCE_BARRIER b[2];
    memset(b, 0, sizeof b);
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = cur; b[1].Transition.pResource = ref;
    b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[0].Transition.StateAfter = b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
    ID3D12VideoEncodeCommandList_ResourceBarrier(l, 2, b);
    D3D12_VIDEO_MOTION_ESTIMATOR_OUTPUT o = {heap};
    D3D12_VIDEO_MOTION_ESTIMATOR_INPUT in = {cur, 0, ref, 0, NULL};
    ID3D12VideoEncodeCommandList_EstimateMotion(l, est, &o, &in);
    D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_OUTPUT ro = {mvs, {0, 0, 0, 0}};
    D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_INPUT ri = {heap, W, H};
    ID3D12VideoEncodeCommandList_ResolveMotionVectorHeap(l, &ro, &ri);
    run_video(q, (ID3D12CommandList *)l, a, 1);
    static short v[8 * 6 * 2];
    copy_subresource(mvs, 0, (BYTE *)v, sizeof v, 0);
    int exact = 0, inner = 0;
    for (UINT by = 1; by < H / 8; ++by)
      for (UINT bx = 0; bx + 1 < W / 8; ++bx) {
        ++inner;
        exact += v[(by * 8 + bx) * 2] == 12 && v[(by * 8 + bx) * 2 + 1] == -8;
      }
    printf("d3d12encodetest: motion estimation 8x8: %d of %d inner blocks have the vector (12, -8)\n", exact, inner);
    CHECK(inner > 0 && exact == inner);
  }
  if (l) ID3D12VideoEncodeCommandList_Release(l);
  if (a) ID3D12CommandAllocator_Release(a);
  if (q) ID3D12CommandQueue_Release(q);
  if (heap) ID3D12VideoMotionVectorHeap_Release(heap);
  if (est) ID3D12VideoMotionEstimator_Release(est);
  ID3D12Resource_Release(mvs);
  ID3D12Resource_Release(ref);
  ID3D12Resource_Release(cur);
}

/* ---- encoding ------------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct Bits { BYTE b[256]; int n; } Bits;
static void bit(Bits *s, unsigned v) { if (s->n % 8 == 0) s->b[s->n / 8] = 0; if (v & 1) s->b[s->n / 8] |= (BYTE)(0x80 >> (s->n % 8)); ++s->n; }
static void put(Bits *s, unsigned v, int len) { for (int i = len - 1; i >= 0; --i) bit(s, v >> i); }
static void ue(Bits *s, unsigned v) { unsigned x = v + 1; int len = 0; while ((x >> (len + 1)) != 0) ++len; put(s, 0, len); put(s, x, len + 1); }
static void se(Bits *s, int v) { ue(s, v > 0 ? (unsigned)(2 * v - 1) : (unsigned)(-2 * v)); }
static void trailing(Bits *s) { bit(s, 1); while (s->n % 8) bit(s, 0); }
static size_t nal(BYTE *out, int ref_idc, int type, const Bits *s) {
  size_t n = 0;
  out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
  out[n++] = (BYTE)((ref_idc << 5) | type);
  int zeros = 0;
  for (int i = 0; i < s->n / 8; ++i) {
    if (zeros >= 2 && s->b[i] <= 3) { out[n++] = 3; zeros = 0; }
    out[n++] = s->b[i];
    zeros = s->b[i] == 0 ? zeros + 1 : 0;
  }
  return n;
}
/* The program's parameter sets, as the encoder's slices need them (H264PictureEncoder.hpp): Main or High, CAVLC or CABAC, the 8x8 transform or not, POC type 0,
 * pic_init_qp 26, deblocking control present, the visible size cropped from whole macroblocks. */
static size_t parameter_sets(BYTE *out, UINT width, UINT height, int high, int cabac, int t8x8, int constrained) {
  Bits s = {{0}, 0}, p = {{0}, 0};
  const UINT mbw = (width + 15) / 16, mbh = (height + 15) / 16;
  put(&s, high ? 100 : 77, 8); put(&s, 0, 8); put(&s, 31, 8); ue(&s, 0);
  if (high) { ue(&s, 1); ue(&s, 0); ue(&s, 0); bit(&s, 0); bit(&s, 0); }
  ue(&s, g_gop.log2_max_frame_num_minus4); ue(&s, 0); ue(&s, g_gop.log2_max_pic_order_cnt_lsb_minus4);
  ue(&s, 4); bit(&s, 0); ue(&s, mbw - 1); ue(&s, mbh - 1); bit(&s, 1); bit(&s, 1);
  const UINT cr = (mbw * 16 - width) / 2, cb = (mbh * 16 - height) / 2;
  if (cr || cb) { bit(&s, 1); ue(&s, 0); ue(&s, cr); ue(&s, 0); ue(&s, cb); } else bit(&s, 0);
  bit(&s, 0);
  trailing(&s);
  ue(&p, 0); ue(&p, 0); bit(&p, cabac ? 1 : 0); bit(&p, 0); ue(&p, 0); ue(&p, 0); ue(&p, 0); bit(&p, 0); put(&p, 0, 2); se(&p, 0); se(&p, 0); se(&p, 0); bit(&p, 1);
  bit(&p, constrained ? 1 : 0); bit(&p, 0);
  if (high) { bit(&p, t8x8 ? 1 : 0); bit(&p, 0); se(&p, 0); }
  trailing(&p);
  size_t n = nal(out, 3, 7, &s);
  return n + nal(out + n, 3, 8, &p);
}

#include "dxva_streams.h"

/* One sequence: IDR P B B P coded as a program codes it (its DPB an NV12 array of the picture's size, its own parameter sets), every picture's metadata checked, the stream
 * decoded again through ID3D12VideoDecodeCommandList and compared with the reconstructions sample for sample. */
typedef struct EncodeCase {
  const char *name;
  UINT width, height;
  UINT flags;                                   /* D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAGS */
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES direct;
  D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE layout;
  UINT layout_value;
  UINT refresh;                                 /* the intra refresh wave's length (0: none) */
  int encode1;                                  /* EncodeFrame1: dirty rectangles and a move region on the P pictures, the PSNRs */
} EncodeCase;
#define FRAMES 5
static void encode_case(const EncodeCase *c) {
  const UINT W = c->width, H = c->height, DPB = 4, CW = (W + 1) / 2, CH = (H + 1) / 2;
  const int high = (c->flags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM) != 0;
  /* coding order: IDR (display 0), P (3), B (1), B (2), P (4); POC = 2 x display */
  static const struct { D3D12_VIDEO_ENCODER_FRAME_TYPE_H264 type; UINT display, slot, reference; int l0[2], l1; } frames[FRAMES] = {
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME, 0, 0, 1, {-1, -1}, -1},
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME, 3, 1, 1, {0, -1}, -1},
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME, 1, 2, 0, {0, -1}, 1},
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME, 2, 2, 0, {0, -1}, 1},
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME, 4, 3, 1, {1, 0}, -1}};
  D3D12_VIDEO_ENCODER_PROFILE_H264 profile = high ? D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH : D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 config = g_config;
  config.ConfigurationFlags = (D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAGS)c->flags;
  config.DirectModeConfig = c->direct;
  D3D12_VIDEO_ENCODER_DESC ed;
  memset(&ed, 0, sizeof ed);
  ed.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
  ed.EncodeProfile.DataSize = sizeof profile;
  ed.EncodeProfile.pH264Profile = &profile;
  ed.InputFormat = DXGI_FORMAT_NV12;
  ed.CodecConfiguration.DataSize = sizeof config;
  ed.CodecConfiguration.pH264Config = &config;
  ed.MaxMotionEstimationPrecision = D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
  ID3D12VideoEncoder *enc = NULL;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoEncoder(g_vdev, &ed, &IID_ID3D12VideoEncoder, (void **)&enc));
  D3D12_VIDEO_ENCODER_LEVELS_H264 level = D3D12_VIDEO_ENCODER_LEVELS_H264_3;
  D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC res = {W, H};
  ID3D12VideoEncoderHeap *heap = NULL;
  ID3D12VideoEncoderHeap1 *heap1 = NULL;
  ID3D12VideoDevice4 *vdev4 = NULL;
  if (c->encode1) {
    /* (EncodeFrame1 takes ID3D12VideoEncoderHeap1, from ID3D12VideoDevice4; dirty regions are allowed by the heap) */
    CHECK_HR(ID3D12VideoDevice3_QueryInterface(g_vdev, &IID_ID3D12VideoDevice4, (void **)&vdev4));
    D3D12_VIDEO_ENCODER_HEAP_DESC1 hd;
    memset(&hd, 0, sizeof hd);
    hd.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_ALLOW_DIRTY_REGIONS_;
    hd.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
    hd.EncodeProfile = ed.EncodeProfile;
    hd.EncodeLevel.DataSize = sizeof level;
    hd.EncodeLevel.pH264LevelSetting = &level;
    hd.ResolutionsListCount = 1;
    hd.pResolutionList = &res;
    if (vdev4) CHECK_HR(vdev4->lpVtbl->CreateVideoEncoderHeap1(vdev4, &hd, &IID_ID3D12VideoEncoderHeap1, (void **)&heap1));
    CHECK(heap1 && heap1->lpVtbl->GetPow2DownscaleFactor(heap1) == 0 && heap1->lpVtbl->GetEncoderHeapFlags(heap1) == D3D12_VIDEO_ENCODER_HEAP_FLAG_ALLOW_DIRTY_REGIONS_);
    if (heap1) heap1->lpVtbl->QueryInterface(heap1, &IID_ID3D12VideoEncoderHeap, (void **)&heap);
  } else {
    D3D12_VIDEO_ENCODER_HEAP_DESC hd;
    memset(&hd, 0, sizeof hd);
    hd.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
    hd.EncodeProfile = ed.EncodeProfile;
    hd.EncodeLevel.DataSize = sizeof level;
    hd.EncodeLevel.pH264LevelSetting = &level;
    hd.ResolutionsListCount = 1;
    hd.pResolutionList = &res;
    CHECK_HR(ID3D12VideoDevice3_CreateVideoEncoderHeap(g_vdev, &hd, &IID_ID3D12VideoEncoderHeap, (void **)&heap));
  }
  /* (the textures are of the picture's size: the coded size's last columns and rows are the heap's) */
  ID3D12Resource *input = texture(DXGI_FORMAT_NV12, W, H, 1), *dpb = texture(DXGI_FORMAT_NV12, W, H, DPB);
  ID3D12Resource *bits = buffer(D3D12_HEAP_TYPE_READBACK, 1 << 18), *meta = buffer(D3D12_HEAP_TYPE_DEFAULT, 1 << 16), *resolved = buffer(D3D12_HEAP_TYPE_READBACK, 1 << 16);
  ID3D12Resource *psnr = buffer(D3D12_HEAP_TYPE_READBACK, 4096);
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *q = NULL;
  ID3D12CommandAllocator *a = NULL;
  ID3D12VideoEncodeCommandList2 *l = NULL;
  ID3D12VideoEncodeCommandList4 *l4 = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, &IID_ID3D12CommandAllocator, (void **)&a);
  if (a) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, a, NULL, &IID_ID3D12VideoEncodeCommandList2, (void **)&l);
  if (l && c->encode1) CHECK_HR(ID3D12VideoEncodeCommandList2_QueryInterface(l, &IID_ID3D12VideoEncodeCommandList4, (void **)&l4));
  CHECK(enc && heap && input && dpb && bits && meta && resolved && psnr && q && l && (!c->encode1 || l4));
  if (!enc || !heap || !input || !dpb || !bits || !meta || !resolved || !psnr || !q || !l || (c->encode1 && !l4)) return;
  static BYTE stream[1 << 18];
  size_t stream_size = parameter_sets(stream, W, H, high, (c->flags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING) != 0, high,
                                      (c->flags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_CONSTRAINED_INTRAPREDICTION) != 0);
  static BYTE recon_y[FRAMES][128 * 128], recon_uv[FRAMES][128 * 64], src_y[128 * 128], src_uv[128 * 64];
  const size_t luma_bytes = (size_t)W * H, chroma_bytes = (size_t)CW * 2 * CH;
  ID3D12Resource *dpb_list[4];
  UINT dpb_subs[4];
  for (UINT i = 0; i < DPB; ++i) { dpb_list[i] = dpb; dpb_subs[i] = i; }
  int ok_meta = 0, total_slices = 0, psnr_ok = 0;
  UINT64 skipped_b = 0;
  for (UINT f = 0; f < FRAMES; ++f) {
    /* the picture: the pattern moving 1.25 samples right and 0.5 down a frame, its lowest quarter still */
    for (UINT j = 0; j < H; ++j)
      for (UINT i = 0; i < W; ++i) {
        const UINT t = j >= H * 3 / 4 ? 0 : frames[f].display;
        src_y[j * W + i] = pattern((int)(i * 4 + t * 5) / 4, (int)(j * 4 + t * 2) / 4);
      }
    for (UINT j = 0; j < CH; ++j)
      for (UINT i = 0; i < CW * 2; ++i) src_uv[j * CW * 2 + i] = (BYTE)(i & 1 ? 150 - j : 100 + (i / 2) % 32);
    copy_subresource(input, 0, src_y, luma_bytes, 1);
    copy_subresource(input, 1, src_uv, chroma_bytes, 1);
    /* the picture control: the references by their slots in the buffer, POC 2 x display, frame_num the reference pictures before */
    D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264 desc[2];
    UINT list0[2], list1[1], n0 = 0, n1 = 0, nd = 0;
    static const UINT frame_num[FRAMES] = {0, 1, 2, 2, 2};
    for (int k = 0; k < 2 && frames[f].l0[k] >= 0; ++k) {
      const UINT r = (UINT)frames[f].l0[k];      /* (the coded frame it is) */
      desc[nd] = (D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264){frames[r].slot, FALSE, 0, 2 * frames[r].display, frame_num[r], 0};
      list0[n0++] = nd++;
    }
    if (frames[f].l1 >= 0) {
      const UINT r = (UINT)frames[f].l1;
      desc[nd] = (D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264){frames[r].slot, FALSE, 0, 2 * frames[r].display, frame_num[r], 0};
      list1[n1++] = nd++;
    }
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 pic;
    memset(&pic, 0, sizeof pic);
    pic.FrameType = frames[f].type;
    pic.idr_pic_id = 0;
    pic.PictureOrderCountNumber = 2 * frames[f].display;
    pic.FrameDecodingOrderNumber = frame_num[f];
    pic.List0ReferenceFramesCount = n0; pic.pList0ReferenceFrames = list0;
    pic.List1ReferenceFramesCount = n1; pic.pList1ReferenceFrames = list1;
    pic.ReferenceFramesReconPictureDescriptorsCount = nd; pic.pReferenceFramesReconPictureDescriptors = desc;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES slices = {{c->layout_value}};
    D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC seq;
    memset(&seq, 0, sizeof seq);
    seq.RateControl = cqp_desc();
    seq.PictureTargetResolution = res;
    seq.SelectedLayoutMode = c->layout;
    seq.FrameSubregionsLayoutData.DataSize = sizeof slices;
    seq.FrameSubregionsLayoutData.pSlicesPartition_H264 = &slices;
    seq.CodecGopSequence = gop_desc();
    seq.IntraRefreshConfig.Mode = c->refresh ? D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED : D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
    seq.IntraRefreshConfig.IntraRefreshDuration = c->refresh;
    const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAGS reference = frames[f].reference ? D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE : D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_NONE;
    const UINT refresh_index = c->refresh ? f % c->refresh : 0;
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS ri;
    memset(&ri, 0, sizeof ri);
    ri.EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
    ri.EncoderProfile = ed.EncodeProfile;
    ri.EncoderInputFormat = DXGI_FORMAT_NV12;
    ri.EncodedPictureEffectiveResolution = res;
    ri.HWLayoutMetadata.pBuffer = meta;
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS ro = {{resolved, 0}};
    if (!c->encode1) {
      D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS in;
      memset(&in, 0, sizeof in);
      in.SequenceControlDesc = seq;
      in.PictureControlDesc.IntraRefreshFrameIndex = refresh_index;
      in.PictureControlDesc.Flags = reference;
      in.PictureControlDesc.PictureControlCodecData.DataSize = sizeof pic;
      in.PictureControlDesc.PictureControlCodecData.pH264PicData = &pic;
      in.PictureControlDesc.ReferenceFrames.NumTexture2Ds = DPB;
      in.PictureControlDesc.ReferenceFrames.ppTexture2Ds = dpb_list;
      in.PictureControlDesc.ReferenceFrames.pSubresources = dpb_subs;
      in.pInputFrame = input;
      D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS out;
      memset(&out, 0, sizeof out);
      out.Bitstream.pBuffer = bits;
      out.ReconstructedPicture.pReconstructedPicture = dpb;
      out.ReconstructedPicture.ReconstructedPictureSubresource = frames[f].slot;
      out.EncoderOutputMetadata.pBuffer = meta;
      ID3D12VideoEncodeCommandList2_EncodeFrame(l, enc, heap, &in, &out);
      ID3D12VideoEncodeCommandList2_ResolveEncoderOutputMetadata(l, &ri, &ro);
    } else {
      /* P pictures: the still quarter is not dirty, and a move region says where the rest came from (the first reference: 1.25 samples a frame -> whole samples) */
      RECT dirty = {0, 0, (LONG)W, (LONG)(H * 3 / 4) & ~15};
      D3D12_VIDEO_ENCODER_DIRTY_RECT_INFO dri = {FALSE, D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE_DIRTY, 1, &dirty, 0};
      D3D12_VIDEO_ENCODER_MOVE_RECT move;
      D3D12_VIDEO_ENCODER_MOVEREGION_INFO mri;
      memset(&mri, 0, sizeof mri);
      const int p_frame = frames[f].type == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
      if (p_frame) {
        const UINT r = (UINT)frames[f].l0[0];
        const LONG dt = (LONG)(frames[f].display - frames[r].display);
        dri.SourceDPBFrameReference = frames[r].slot;
        move.DestRect = dirty;
        move.SourcePoint.x = (5 * dt + 2) / 4;
        move.SourcePoint.y = (2 * dt + 2) / 4;
        mri.NumMoveRegions = 1;
        mri.pMoveRegions = &move;
        mri.MotionSearchModeConfiguration.MotionSearchMode = D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_START_HINT;
        mri.SourceDPBFrameReference = frames[r].slot;
        mri.MotionUnitPrecision = D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_FULL_PIXEL;
      }
      D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS1 in;
      memset(&in, 0, sizeof in);
      in.SequenceControlDesc = seq;
      in.PictureControlDesc.IntraRefreshFrameIndex = refresh_index;
      in.PictureControlDesc.Flags = reference | (p_frame ? D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_ENABLE_DIRTY_REGIONS_INPUT_ | D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_ENABLE_MOTION_VECTORS_INPUT_ : 0);
      in.PictureControlDesc.PictureControlCodecData.DataSize = sizeof pic;
      in.PictureControlDesc.PictureControlCodecData.pH264PicData = &pic;
      in.PictureControlDesc.ReferenceFrames.NumTexture2Ds = DPB;
      in.PictureControlDesc.ReferenceFrames.ppTexture2Ds = dpb_list;
      in.PictureControlDesc.ReferenceFrames.pSubresources = dpb_subs;
      in.PictureControlDesc.DirtyRects.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER;
      in.PictureControlDesc.DirtyRects.pCPUBuffer = &dri;
      in.PictureControlDesc.MotionVectors.MapSource = D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER;
      in.PictureControlDesc.MotionVectors.pCPUBuffer = &mri;
      in.pInputFrame = input;
      in.OptionalMetadata = D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_FRAME_PSNR | D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_SUBREGIONS_PSNR;
      D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS1 out;
      memset(&out, 0, sizeof out);
      out.Bitstream.NotificationMode = D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM_NOTIFICATION_MODE_FULL_FRAME;
      out.Bitstream.FrameOutputBuffer.pBuffer = bits;
      out.ReconstructedPicture.pReconstructedPicture = dpb;
      out.ReconstructedPicture.ReconstructedPictureSubresource = frames[f].slot;
      out.EncoderOutputMetadata.pBuffer = meta;
      l4->lpVtbl->EncodeFrame1(l4, enc, heap1, &in, &out);
      D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS1 ri1;
      memset(&ri1, 0, sizeof ri1);
      ri1.EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
      ri1.EncoderProfile = ed.EncodeProfile;
      ri1.EncoderInputFormat = DXGI_FORMAT_NV12;
      ri1.EncodedPictureEffectiveResolution = res;
      ri1.HWLayoutMetadata.pBuffer = meta;
      ri1.OptionalMetadata = in.OptionalMetadata;
      ri1.CodecConfiguration = ed.CodecConfiguration;
      D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS1 ro1;
      memset(&ro1, 0, sizeof ro1);
      ro1.ResolvedLayoutMetadata.pBuffer = resolved;
      ro1.ResolvedFramePSNRData.pBuffer = psnr;
      ro1.ResolvedSubregionsPSNRData.pBuffer = psnr;
      ro1.ResolvedSubregionsPSNRData.Offset = 256;
      l4->lpVtbl->ResolveEncoderOutputMetadata1(l4, &ri1, &ro1);
    }
    run_video(q, (ID3D12CommandList *)l, a, 1);
    /* the metadata, and the slices it describes */
    BYTE *m = NULL, *b = NULL;
    ID3D12Resource_Map(resolved, 0, NULL, (void **)&m);
    ID3D12Resource_Map(bits, 0, NULL, (void **)&b);
    D3D12_VIDEO_ENCODER_OUTPUT_METADATA md;
    memcpy(&md, m, sizeof md);
    const D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA *sub = (const D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA *)(m + sizeof md);
    UINT64 sum = 0;
    int starts = 1, small = 1;
    for (UINT64 i = 0; i < md.WrittenSubregionsCount && i < 256; ++i) {
      starts &= b[sum] == 0 && b[sum + 1] == 0 && b[sum + 2] == 0 && b[sum + 3] == 1 && sub[i].bStartOffset == 0 && sub[i].bHeaderSize > 40;
      small &= c->layout != D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_BYTES_PER_SUBREGION || sub[i].bSize <= c->layout_value || md.WrittenSubregionsCount == 1;
      sum += sub[i].bSize;
    }
    const UINT mbs = ((W + 15) / 16) * ((H + 15) / 16);
    const int expect_slices = c->layout == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME ? (int)c->layout_value
                              : c->layout == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME ? 1 : -1;
    const int good = md.EncodeErrorFlags == 0 && (expect_slices < 0 || md.WrittenSubregionsCount == (UINT64)expect_slices) && sum == md.EncodedBitstreamWrittenBytesCount && starts &&
                     small && md.EncodedBitstreamWrittenBytesCount > 0 && md.EncodeStats.IntraCodingUnitsCount + md.EncodeStats.InterCodingUnitsCount + md.EncodeStats.SkipCodingUnitsCount == mbs;
    if (!good) printf("%s frame %u: errors %llx, %llu subregions, %llu bytes\n", c->name, f, (unsigned long long)md.EncodeErrorFlags, (unsigned long long)md.WrittenSubregionsCount, (unsigned long long)md.EncodedBitstreamWrittenBytesCount);
    ok_meta += good;
    total_slices += (int)md.WrittenSubregionsCount;
    if (frames[f].type == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME) skipped_b += md.EncodeStats.SkipCodingUnitsCount;
    /* (intra refresh: a P picture's rows of the wave are intra) */
    if (c->refresh && frames[f].type == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME) CHECK(md.EncodeStats.IntraCodingUnitsCount >= (W + 15) / 16);
    if (md.EncodeErrorFlags == 0 && stream_size + md.EncodedBitstreamWrittenBytesCount <= sizeof stream) {
      memcpy(stream + stream_size, b, (size_t)md.EncodedBitstreamWrittenBytesCount);
      stream_size += (size_t)md.EncodedBitstreamWrittenBytesCount;
    }
    if (c->encode1) {
      float *ps = NULL;
      ID3D12Resource_Map(psnr, 0, NULL, (void **)&ps);
      psnr_ok += ps && ps[0] > 25 && ps[0] < 100 && ps[1] > 25 && ps[2] > 25 && ps[64] > 20;
      ID3D12Resource_Unmap(psnr, 0, NULL);
    }
    ID3D12Resource_Unmap(bits, 0, NULL);
    ID3D12Resource_Unmap(resolved, 0, NULL);
    /* the reconstruction */
    copy_subresource(dpb, frames[f].slot, recon_y[f], luma_bytes, 0);
    copy_subresource(dpb, DPB + frames[f].slot, recon_uv[f], chroma_bytes, 0);
  }
  printf("d3d12encodetest %s (%ux%u): %d of %d pictures with good metadata (%d slices, %llu B_Skip), %u bytes with the parameter sets\n", c->name, W, H, ok_meta, FRAMES, total_slices,
         (unsigned long long)skipped_b, (unsigned)stream_size);
  CHECK(ok_meta == FRAMES);
  if (c->encode1) CHECK(psnr_ok == FRAMES);
  if (c->direct != D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED) CHECK(skipped_b > 0);
  if (l4) l4->lpVtbl->Release(l4);
  ID3D12VideoEncodeCommandList2_Release(l);
  ID3D12CommandAllocator_Release(a);
  ID3D12CommandQueue_Release(q);

  /* decoded again by the decoder: every picture is the reconstruction (its visible part: the decoder's surfaces are whole macroblocks) */
  const UINT DW = (W + 15) / 16 * 16, DH = (H + 15) / 16 * 16;
  DxvaStream s;
  DxvaPicture p;
  if (dxva_stream_open(&s, DXVA_CODEC_H264, stream, stream_size, 8) != 0 || dxva_stream_next(&s, &p) != 1) { CHECK(0); return; }
  D3D12_VIDEO_DECODER_DESC dd = {0, {D3D12_VIDEO_DECODE_PROFILE_H264, D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE, D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE}};
  D3D12_VIDEO_DECODER_HEAP_DESC dh;
  memset(&dh, 0, sizeof dh);
  dh.Configuration = dd.Configuration;
  dh.DecodeWidth = DW;
  dh.DecodeHeight = DH;
  dh.Format = DXGI_FORMAT_NV12;
  dh.MaxDecodePictureBufferCount = 8;
  ID3D12VideoDecoder *dec = NULL;
  ID3D12VideoDecoderHeap *dheap = NULL;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&dec));
  CHECK_HR(ID3D12VideoDevice3_CreateVideoDecoderHeap(g_vdev, &dh, &IID_ID3D12VideoDecoderHeap, (void **)&dheap));
  ID3D12Resource *surfaces = texture(DXGI_FORMAT_NV12, DW, DH, 8), *dbits = buffer(D3D12_HEAP_TYPE_UPLOAD, 1 << 18);
  ID3D12Resource *tex[8];
  UINT subs[8];
  for (UINT i = 0; i < 8; ++i) { tex[i] = surfaces; subs[i] = i; }
  D3D12_COMMAND_QUEUE_DESC dq = {D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *vq = NULL;
  ID3D12CommandAllocator *va = NULL;
  ID3D12VideoDecodeCommandList *vl = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &dq, &IID_ID3D12CommandQueue, (void **)&vq);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, &IID_ID3D12CommandAllocator, (void **)&va);
  if (va) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, va, NULL, &IID_ID3D12VideoDecodeCommandList, (void **)&vl);
  CHECK(dec && dheap && surfaces && dbits && vq && vl);
  static BYTE y[128 * 128], uv[128 * 64];
  int n = 0, same = 0, r = 1;
  while (dec && dheap && vl && surfaces && dbits && r == 1 && n < FRAMES) {
    BYTE *map = NULL;
    ID3D12Resource_Map(dbits, 0, NULL, (void **)&map);
    memcpy(map, p.bitstream, p.bitstream_size);
    ID3D12Resource_Unmap(dbits, 0, NULL);
    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in;
    memset(&in, 0, sizeof in);
    in.NumFrameArguments = 3;
    in.FrameArguments[0] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, p.picture_parameters_size, (void *)p.picture_parameters};
    in.FrameArguments[1] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, p.qmatrix_size, (void *)p.qmatrix};
    in.FrameArguments[2] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, p.slice_control_size, (void *)p.slice_control};
    in.ReferenceFrames.NumTexture2Ds = 8;
    in.ReferenceFrames.ppTexture2Ds = tex;
    in.ReferenceFrames.pSubresources = subs;
    in.CompressedBitstream.pBuffer = dbits;
    in.CompressedBitstream.Size = p.bitstream_size;
    in.pHeap = dheap;
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS out;
    memset(&out, 0, sizeof out);
    out.pOutputTexture2D = surfaces;
    out.OutputSubresource = p.surface;
    ID3D12VideoDecodeCommandList_DecodeFrame(vl, dec, &out, &in);
    run_video(vq, (ID3D12CommandList *)vl, va, 0);
    copy_subresource(surfaces, p.surface, y, (size_t)DW * DH, 0);
    copy_subresource(surfaces, 8 + p.surface, uv, (size_t)DW * DH / 2, 0);
    /* (the decoder's pictures come in decoding order: picture n is the n-th one coded) */
    int eq = 1;
    for (UINT j = 0; j < H; ++j) eq &= memcmp(y + (size_t)j * DW, recon_y[n] + (size_t)j * W, W) == 0;
    for (UINT j = 0; j < CH; ++j) eq &= memcmp(uv + (size_t)j * DW, recon_uv[n] + (size_t)j * CW * 2, CW * 2) == 0;
    same += eq;
    ++n;
    r = dxva_stream_next(&s, &p);
  }
  printf("d3d12encodetest %s: the D3D12 decoder decodes %d of %d encoded pictures to their reconstructions, sample for sample\n", c->name, same, FRAMES);
  CHECK(n == FRAMES && same == FRAMES && r == 0);
  dxva_stream_close(&s);
  if (vl) ID3D12VideoDecodeCommandList_Release(vl);
  if (va) ID3D12CommandAllocator_Release(va);
  if (vq) ID3D12CommandQueue_Release(vq);
  if (dbits) ID3D12Resource_Release(dbits);
  if (surfaces) ID3D12Resource_Release(surfaces);
  if (dheap) ID3D12VideoDecoderHeap_Release(dheap);
  if (dec) ID3D12VideoDecoder_Release(dec);
  ID3D12Resource_Release(psnr);
  ID3D12Resource_Release(resolved);
  ID3D12Resource_Release(meta);
  ID3D12Resource_Release(bits);
  ID3D12Resource_Release(dpb);
  ID3D12Resource_Release(input);
  ID3D12VideoEncoderHeap_Release(heap);
  if (heap1) heap1->lpVtbl->Release(heap1);
  if (vdev4) vdev4->lpVtbl->Release(vdev4);
  ID3D12VideoEncoder_Release(enc);
}

static void test_encode(void) {
  const UINT cabac = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING, t8x8 = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM;
  const UINT constrained = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_CONSTRAINED_INTRAPREDICTION;
  const EncodeCase cases[] = {
      {"main", 96, 64, 0, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED, D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME, 2, 0, 0},
      {"cabac-8x8-temporal-cropped", 100, 60, cabac | t8x8, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_TEMPORAL, D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME, 0, 0, 0},
      {"cavlc-8x8-spatial-refresh-bytes", 96, 64, t8x8 | constrained, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_SPATIAL,
       D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_BYTES_PER_SUBREGION, 400, 2, 0},
      {"cabac-spatial-encodeframe1", 118, 46, cabac, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_SPATIAL, D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION, 1, 0, 1},
  };
  for (UINT i = 0; i < sizeof cases / sizeof cases[0]; ++i) encode_case(&cases[i]);
}

int main(void) {
  if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&g_dev)) || !g_dev) {
    printf("d3d12encodetest: no device\nd3d12encodetest: 1 checks, 1 failed\n");
    return 1;
  }
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
  CHECK_HR(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12VideoDevice3, (void **)&g_vdev));
  if (g_vdev) {
    test_features();
    test_motion();
    test_encode();
    ID3D12VideoDevice3_Release(g_vdev);
  }
  ID3D12Fence_Release(g_fence);
  ID3D12CommandQueue_Release(g_queue);
  ID3D12Device_Release(g_dev);
  printf("d3d12encodetest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
