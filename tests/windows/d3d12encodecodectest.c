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

/* Direct3D 12 HEVC and AV1 video encoding (d3d12.dll encode_codecs.inc over the kernel's CPU encoders): the encoder queries of both codecs (what is supported, what is
 * refused, the AV1 tile layout query), then encodes as a program does them - HEVC Main and Main 10 (IDR P B P B with a long-lived reference no longer used, SAO,
 * slices by rows), AV1 8- and 10-bit (key, inter frames over several slots, an intra-only frame, tiles of a uniform grid, CDEF, switchable filters) - whose metadata
 * is resolved and checked (the AV1 post-encode values and tile layout too), and every picture decoded again through ID3D12VideoDecoder, its DXVA parameters made from
 * what the program asked for and from the post-encode values, and compared with the encoder's reconstruction sample for sample. */
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
#include <dxva.h>
#include "../../src/Windows/d3d12/video_types.h"
#include "../../src/Windows/d3d12/video_encode_types.h"

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
  WaitForSingleObject(e, 120000);
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
static void run_video(ID3D12CommandQueue *q, ID3D12CommandList *l, ID3D12CommandAllocator *a, int encode) {
  if (encode) ID3D12VideoEncodeCommandList_Close((ID3D12VideoEncodeCommandList *)l);
  else ID3D12VideoDecodeCommandList_Close((ID3D12VideoDecodeCommandList *)l);
  ID3D12CommandQueue_ExecuteCommandLists(q, 1, &l);
  wait_queue(q);
  if (encode) ID3D12VideoEncodeCommandList_Reset((ID3D12VideoEncodeCommandList *)l, a);
  else ID3D12VideoDecodeCommandList_Reset((ID3D12VideoDecodeCommandList *)l, a);
}

/* The picture t of a moving texture, as NV12 or P010 planes (rows packed: luma, then interleaved U V). */
static int pattern(int x, int y) {
  unsigned h = (unsigned)(x / 3) * 2654435761u ^ (unsigned)(y / 3) * 40503u;
  h ^= h >> 13;
  return 64 + (int)(h & 127) / 2 + ((x + 2 * y) & 31);
}
static void make_picture(BYTE *y, BYTE *uv, UINT W, UINT H, int deep, int t) {
  for (UINT j = 0; j < H; ++j)
    for (UINT i = 0; i < W; ++i) {
      const int v = j >= H * 3 / 4 ? pattern((int)i, (int)j) : pattern((int)(i * 4 + (UINT)t * 5) / 4, (int)(j * 4 + (UINT)t * 2) / 4);
      if (deep) { const unsigned s = (unsigned)(v * 4 + (int)(i & 3)) << 6; y[(j * W + i) * 2] = (BYTE)s; y[(j * W + i) * 2 + 1] = (BYTE)(s >> 8); }
      else y[j * W + i] = (BYTE)v;
    }
  for (UINT j = 0; j < H / 2; ++j)
    for (UINT i = 0; i < W; ++i) {
      const int v = i & 1 ? 150 - (int)j : 100 + (int)((i / 2 + (UINT)t) % 32);
      if (deep) { const unsigned s = (unsigned)(v * 4) << 6; uv[(j * W + i) * 2] = (BYTE)s; uv[(j * W + i) * 2 + 1] = (BYTE)(s >> 8); }
      else uv[j * W + i] = (BYTE)v;
    }
}

/* ---- the queries ------------------------------------------------------------------------------------------------------------------------------------------------- */

static D3D12_VIDEO_ENCODER_PROFILE_HEVC g_hevc_main = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN, g_hevc_main10 = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10;
static D3D12_VIDEO_ENCODER_AV1_PROFILE g_av1_main = D3D12_VIDEO_ENCODER_AV1_PROFILE_MAIN;
static D3D12_VIDEO_ENCODER_PROFILE_DESC hevc_profile(int ten) { D3D12_VIDEO_ENCODER_PROFILE_DESC p; p.DataSize = sizeof g_hevc_main; p.pHEVCProfile = ten ? &g_hevc_main10 : &g_hevc_main; return p; }
static D3D12_VIDEO_ENCODER_PROFILE_DESC av1_profile(void) { D3D12_VIDEO_ENCODER_PROFILE_DESC p; p.DataSize = sizeof g_av1_main; p.pAV1Profile = &g_av1_main; return p; }
static int feature(D3D12_FEATURE_VIDEO f, void *data, UINT size) { return SUCCEEDED(ID3D12VideoDevice3_CheckFeatureSupport(g_vdev, f, data, size)); }

static void test_hevc_features(void) {
  D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codec = {0, D3D12_VIDEO_ENCODER_CODEC_HEVC, FALSE};
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_CODEC, &codec, sizeof codec) && codec.IsSupported);
  D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC lo, hi;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL pl;
  memset(&pl, 0, sizeof pl);
  pl.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  pl.Profile = hevc_profile(1);
  pl.MinSupportedLevel.DataSize = pl.MaxSupportedLevel.DataSize = sizeof lo;
  pl.MinSupportedLevel.pHEVCLevelSetting = &lo;
  pl.MaxSupportedLevel.pHEVCLevelSetting = &hi;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL, &pl, sizeof pl) && pl.IsSupported && lo.Level == D3D12_VIDEO_ENCODER_LEVELS_HEVC_1 && hi.Level == D3D12_VIDEO_ENCODER_LEVELS_HEVC_62);
  D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT inf = {0, D3D12_VIDEO_ENCODER_CODEC_HEVC, hevc_profile(0), DXGI_FORMAT_NV12, FALSE};
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &inf, sizeof inf) && inf.IsSupported);
  inf.Format = DXGI_FORMAT_P010;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &inf, sizeof inf) && !inf.IsSupported);   /* (Main takes NV12; Main 10 P010) */
  inf.Profile = hevc_profile(1);
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &inf, sizeof inf) && inf.IsSupported);
  D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION res;
  memset(&res, 0, sizeof res);
  res.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION, &res, sizeof res) && res.IsSupported && res.ResolutionWidthMultipleRequirement == 8 && res.MaxResolutionSupported.Width == 4096);
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC cs;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT ccs;
  memset(&ccs, 0, sizeof ccs);
  ccs.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  ccs.Profile = hevc_profile(0);
  ccs.CodecSupportLimits.DataSize = sizeof cs;
  ccs.CodecSupportLimits.pHEVCSupport = &cs;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &ccs, sizeof ccs) && ccs.IsSupported && cs.MinLumaCodingUnitSize == D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8 &&
        cs.MaxLumaCodingUnitSize == D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64 && (cs.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_SAO_FILTER_SUPPORT) &&
        !(cs.SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_INTRA_SLICE_CONSTRAINED_ENCODING_SUPPORT));
  D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_HEVC ps;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT pcs;
  memset(&pcs, 0, sizeof pcs);
  pcs.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  pcs.Profile = hevc_profile(0);
  pcs.PictureSupport.DataSize = sizeof ps;
  pcs.PictureSupport.pHEVCSupport = &ps;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT, &pcs, sizeof pcs) && pcs.IsSupported && ps.MaxDPBCapacity == 15 && ps.MaxL1ReferencesForB == 8);
  /* SUPPORT1: a good configuration, then one with a 4x4 maximum transform and slices by bytes */
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC cfg = {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_SAO_FILTER, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8,
                                                       D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_32x32, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4,
                                                       D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32, 1, 1};
  D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC gop = {30, 2, 4};
  D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {28, 30, 32};
  D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC list[2] = {{128, 96}, {100, 60}};
  D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS limits[2];
  D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC suggested;
  D3D12_VIDEO_ENCODER_PROFILE_HEVC sprofile;
  D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES rows = {{1}};
  D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1 s;
  memset(&s, 0, sizeof s);
  s.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  s.InputFormat = DXGI_FORMAT_NV12;
  s.CodecConfiguration.DataSize = sizeof cfg;
  s.CodecConfiguration.pHEVCConfig = &cfg;
  s.CodecGopSequence.DataSize = sizeof gop;
  s.CodecGopSequence.pHEVCGroupOfPictures = &gop;
  s.RateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
  s.RateControl.ConfigParams.DataSize = sizeof cqp;
  s.RateControl.ConfigParams.pConfiguration_CQP = &cqp;
  s.RateControl.TargetFrameRate.Numerator = 30;
  s.RateControl.TargetFrameRate.Denominator = 1;
  s.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION;
  s.SubregionFrameEncodingData.DataSize = sizeof rows;
  s.SubregionFrameEncodingData.pSlicesPartition_HEVC = &rows;
  s.ResolutionsListCount = 1;
  s.pResolutionList = list;
  s.pResolutionDependentSupport = limits;
  s.SuggestedProfile.DataSize = sizeof sprofile;
  s.SuggestedProfile.pHEVCProfile = &sprofile;
  s.SuggestedLevel.DataSize = sizeof suggested;
  s.SuggestedLevel.pHEVCLevelSetting = &suggested;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s) && s.ValidationFlags == 0 && (s.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) &&
        limits[0].SubregionBlockPixelsSize == 32 && limits[0].MaxSubregionsNumber == 12 && limits[0].QPMapRegionPixelsSize == 32 && sprofile == D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN &&
        suggested.Level == D3D12_VIDEO_ENCODER_LEVELS_HEVC_1);
  s.ResolutionsListCount = 2;   /* (100 x 60 is not whole 8x8 blocks) */
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s) && (s.ValidationFlags & D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST));
  s.ResolutionsListCount = 1;
  cfg.MaxLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4;
  s.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_BYTES_PER_SUBREGION;
  s.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1, &s, sizeof s) && (s.ValidationFlags & D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED) &&
        (s.ValidationFlags & D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_MODE_NOT_SUPPORTED) && (s.ValidationFlags & D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INTRA_REFRESH_MODE_NOT_SUPPORTED) &&
        s.SupportFlags == 0);
  /* creation: a configuration it cannot code is refused */
  D3D12_VIDEO_ENCODER_DESC ed;
  memset(&ed, 0, sizeof ed);
  ed.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  ed.EncodeProfile = hevc_profile(0);
  ed.InputFormat = DXGI_FORMAT_NV12;
  ed.CodecConfiguration.DataSize = sizeof cfg;
  ed.CodecConfiguration.pHEVCConfig = &cfg;
  ID3D12VideoEncoder *enc = NULL;
  CHECK(ID3D12VideoDevice3_CreateVideoEncoder(g_vdev, &ed, &IID_ID3D12VideoEncoder, (void **)&enc) == E_INVALIDARG && !enc);
  cfg.MaxLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoEncoder(g_vdev, &ed, &IID_ID3D12VideoEncoder, (void **)&enc));
  if (enc) {
    D3D12_VIDEO_ENCODER_PROFILE_HEVC got = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10;
    D3D12_VIDEO_ENCODER_PROFILE_DESC pd = {sizeof got, {.pHEVCProfile = &got}};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC gotc;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION cd = {sizeof gotc, {.pHEVCConfig = &gotc}};
    CHECK(ID3D12VideoEncoder_GetCodec(enc) == D3D12_VIDEO_ENCODER_CODEC_HEVC && SUCCEEDED(ID3D12VideoEncoder_GetCodecProfile(enc, pd)) && got == D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN &&
          SUCCEEDED(ID3D12VideoEncoder_GetCodecConfiguration(enc, cd)) && memcmp(&gotc, &cfg, sizeof cfg) == 0);
    ID3D12VideoEncoder_Release(enc);
  }
  D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS rr = {0, D3D12_VIDEO_ENCODER_CODEC_HEVC, hevc_profile(0), DXGI_FORMAT_NV12, {128, 96}, FALSE, 0, 0, 0};
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &rr, sizeof rr) && rr.IsSupported && rr.MaxEncoderOutputMetadataBufferSize == 8 + 72 + 48 * 24);
}

static void test_av1_features(void) {
  D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codec = {0, D3D12_VIDEO_ENCODER_CODEC_AV1, FALSE};
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_CODEC, &codec, sizeof codec) && codec.IsSupported);
  D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT inf = {0, D3D12_VIDEO_ENCODER_CODEC_AV1, av1_profile(), DXGI_FORMAT_P010, FALSE};
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT, &inf, sizeof inf) && inf.IsSupported);
  D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION_SUPPORT cs;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT ccs;
  memset(&ccs, 0, sizeof ccs);
  ccs.Codec = D3D12_VIDEO_ENCODER_CODEC_AV1;
  ccs.Profile = av1_profile();
  ccs.CodecSupportLimits.DataSize = sizeof cs;
  ccs.CodecSupportLimits.pAV1Support = &cs;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &ccs, sizeof ccs) && ccs.IsSupported &&
        (cs.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CDEF_FILTERING) && !(cs.SupportedFeatureFlags & D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_PALETTE_ENCODING) &&
        cs.RequiredFeatureFlags == 0 && (cs.PostEncodeValuesFlags & D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_PRIMARY_REF_FRAME) &&
        cs.SupportedRestorationParams[0][0] == D3D12_VIDEO_ENCODER_AV1_RESTORATION_SUPPORT_FLAG_NOT_SUPPORTED &&
        cs.SupportedTxModes[1] == (D3D12_VIDEO_ENCODER_AV1_TX_MODE_FLAG_LARGEST | D3D12_VIDEO_ENCODER_AV1_TX_MODE_FLAG_SELECT));
  D3D12_VIDEO_ENCODER_CODEC_AV1_PICTURE_CONTROL_SUPPORT ps;
  memset(&ps, 0, sizeof ps);
  D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT pcs;
  memset(&pcs, 0, sizeof pcs);
  pcs.Codec = D3D12_VIDEO_ENCODER_CODEC_AV1;
  pcs.Profile = av1_profile();
  pcs.PictureSupport.DataSize = sizeof ps;
  pcs.PictureSupport.pAV1Support = &ps;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT, &pcs, sizeof pcs) && pcs.IsSupported && ps.MaxUniqueReferencesPerFrame == 7 &&
        (ps.SupportedFrameTypes & D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_FLAG_SWITCH_FRAME));
  ps.PredictionMode = D3D12_VIDEO_ENCODER_AV1_COMP_PREDICTION_TYPE_COMPOUND_REFERENCE;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT, &pcs, sizeof pcs) && !pcs.IsSupported);
  /* the tile layout query: a uniform grid of 4 x 1 asked for 160 x 96 (3 x 2 superblocks) is what AV1's spacing makes of it - TileColsLog2 2, tiles one superblock
   * wide, so three of them - and 2 x 1 is 2 + 1 */
  D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS level = {D3D12_VIDEO_ENCODER_AV1_LEVELS_3_0, D3D12_VIDEO_ENCODER_AV1_TIER_MAIN};
  D3D12_VIDEO_ENCODER_AV1_FRAME_SUBREGION_LAYOUT_CONFIG_SUPPORT ts;
  memset(&ts, 0, sizeof ts);
  ts.TilesConfiguration.ColCount = 4;
  ts.TilesConfiguration.RowCount = 1;
  D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG lc;
  memset(&lc, 0, sizeof lc);
  lc.Codec = D3D12_VIDEO_ENCODER_CODEC_AV1;
  lc.Profile = av1_profile();
  lc.Level.DataSize = sizeof level;
  lc.Level.pAV1LevelSetting = &level;
  lc.SubregionMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_GRID_PARTITION_;
  lc.FrameResolution.Width = 160;
  lc.FrameResolution.Height = 96;
  lc.CodecSupport.DataSize = sizeof ts;
  lc.CodecSupport.pAV1Support = &ts;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG, &lc, sizeof lc) && lc.IsSupported && ts.TilesConfiguration.ColCount == 3 &&
        ts.TilesConfiguration.ColWidths[0] == 1 && ts.TilesConfiguration.ColWidths[2] == 1 && ts.TilesConfiguration.RowHeights[0] == 2 && ts.TileSizeBytesMinus1 == 3);
  memset(&ts.TilesConfiguration, 0, sizeof ts.TilesConfiguration);
  ts.TilesConfiguration.ColCount = 2;
  ts.TilesConfiguration.RowCount = 1;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG, &lc, sizeof lc) && lc.IsSupported && ts.TilesConfiguration.ColCount == 2 &&
        ts.TilesConfiguration.ColWidths[0] == 2 && ts.TilesConfiguration.ColWidths[1] == 1);
  /* an explicit layout that does not cover the frame */
  lc.SubregionMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_CONFIGURABLE_GRID_PARTITION_;
  ts.TilesConfiguration.ColCount = 2;
  ts.TilesConfiguration.ColWidths[0] = ts.TilesConfiguration.ColWidths[1] = 1;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG, &lc, sizeof lc) && !lc.IsSupported && ts.ValidationFlags != 0);
  /* (the query is AV1's) */
  lc.Codec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG, &lc, sizeof lc) && !lc.IsSupported);
  /* SUPPORT: compound-free configurations, CQP in quantizer indices (0 is lossless: refused) */
  D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION cfg = {D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ORDER_HINT_TOOLS | D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CDEF_FILTERING, 6};
  D3D12_VIDEO_ENCODER_AV1_SEQUENCE_STRUCTURE gop = {30, 1};
  D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {100, 130, 150};
  D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC list[1] = {{160, 96}};
  D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS limits[1];
  D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT s;
  memset(&s, 0, sizeof s);
  s.Codec = D3D12_VIDEO_ENCODER_CODEC_AV1;
  s.InputFormat = DXGI_FORMAT_NV12;
  s.CodecConfiguration.DataSize = sizeof cfg;
  s.CodecConfiguration.pAV1Config = &cfg;
  s.CodecGopSequence.DataSize = sizeof gop;
  s.CodecGopSequence.pAV1SequenceStructure = &gop;
  s.RateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
  s.RateControl.ConfigParams.DataSize = sizeof cqp;
  s.RateControl.ConfigParams.pConfiguration_CQP = &cqp;
  s.RateControl.TargetFrameRate.Numerator = 30;
  s.RateControl.TargetFrameRate.Denominator = 1;
  s.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_GRID_PARTITION_;
  s.ResolutionsListCount = 1;
  s.pResolutionList = list;
  s.pResolutionDependentSupport = limits;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &s, sizeof s) && s.ValidationFlags == 0 && limits[0].SubregionBlockPixelsSize == 64 && limits[0].MaxSubregionsNumber == 6);
  cqp.ConstantQP_FullIntracodedFrame = 0;
  cfg.FeatureFlags |= D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_WARPED_MOTION;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, &s, sizeof s) && (s.ValidationFlags & D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_CONFIGURATION_NOT_SUPPORTED) &&
        (s.ValidationFlags & D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED));
  D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS rr = {0, D3D12_VIDEO_ENCODER_CODEC_AV1, av1_profile(), DXGI_FORMAT_NV12, {160, 96}, FALSE, 0, 0, 0};
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS, &rr, sizeof rr) && rr.IsSupported && rr.MaxEncoderOutputMetadataBufferSize == 8 + 72 + 6 * 24 + 1048 + 1216);
  /* heaps */
  D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE hs;
  memset(&hs, 0, sizeof hs);
  hs.HeapDesc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_AV1;
  hs.HeapDesc.EncodeProfile = av1_profile();
  hs.HeapDesc.EncodeLevel.DataSize = sizeof level;
  hs.HeapDesc.EncodeLevel.pAV1LevelSetting = &level;
  hs.HeapDesc.ResolutionsListCount = 1;
  hs.HeapDesc.pResolutionList = list;
  CHECK(feature(D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE, &hs, sizeof hs) && hs.IsSupported && hs.MemoryPoolL1Size > 0);
}

/* ---- HEVC -------------------------------------------------------------------------------------------------------------------------------------------------------- */

#define HFRAMES 5
#define MAX_BYTES (128 * 96 * 2)

/* The size in bits of an st_ref_pic_set() of the slice header (7.3.7, idx 0: no prediction) of `neg` pictures before (order counts, closest first) and `pos` after. */
static UINT ue_bits(UINT v) { UINT x = v + 1, n = 0; while (x >> (n + 1)) ++n; return 2 * n + 1; }
static UINT st_rps_bits(int cur, const int *before, UINT nb, const int *after, UINT na) {
  UINT bits = ue_bits(nb) + ue_bits(na);
  int prev = cur;
  for (UINT i = 0; i < nb; ++i) { bits += ue_bits((UINT)(prev - before[i] - 1)) + 1; prev = before[i]; }
  prev = cur;
  for (UINT i = 0; i < na; ++i) { bits += ue_bits((UINT)(after[i] - prev - 1)) + 1; prev = after[i]; }
  return bits;
}

static void hevc_case(int ten) {
  const UINT W = 128, H = 96, BPS = ten ? 2 : 1;
  const DXGI_FORMAT fmt = ten ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
  const char *name = ten ? "HEVC Main 10" : "HEVC Main";
  /* coding order: IDR 0, P 4 (0), B 2 (0 | 4), P 8 (4, 0), B 6 (4 | 8, with 0 kept but not used) */
  static const struct { D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC type; int poc, reference; int l0[2], l1; int dpb[3]; } frames[HFRAMES] = {
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_IDR_FRAME, 0, 1, {-1, -1}, -1, {-1, -1, -1}},
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_P_FRAME, 4, 1, {0, -1}, -1, {0, -1, -1}},
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_B_FRAME, 2, 0, {0, -1}, 1, {0, 1, -1}},
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_P_FRAME, 8, 1, {1, 0}, -1, {0, 1, -1}},
      {D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_B_FRAME, 6, 0, {1, -1}, 3, {0, 1, 3}}};
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC cfg = {D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_SAO_FILTER, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8,
                                                       D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_32x32, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4,
                                                       D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32, 1, 1};
  D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC gop = {30, 2, 4};
  D3D12_VIDEO_ENCODER_DESC ed;
  memset(&ed, 0, sizeof ed);
  ed.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  ed.EncodeProfile = hevc_profile(ten);
  ed.InputFormat = fmt;
  ed.CodecConfiguration.DataSize = sizeof cfg;
  ed.CodecConfiguration.pHEVCConfig = &cfg;
  ed.MaxMotionEstimationPrecision = D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
  ID3D12VideoEncoder *enc = NULL;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoEncoder(g_vdev, &ed, &IID_ID3D12VideoEncoder, (void **)&enc));
  D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC level = {D3D12_VIDEO_ENCODER_LEVELS_HEVC_31, D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN};
  D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC res = {W, H};
  D3D12_VIDEO_ENCODER_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
  hd.EncodeProfile = ed.EncodeProfile;
  hd.EncodeLevel.DataSize = sizeof level;
  hd.EncodeLevel.pHEVCLevelSetting = &level;
  hd.ResolutionsListCount = 1;
  hd.pResolutionList = &res;
  ID3D12VideoEncoderHeap *heap = NULL;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoEncoderHeap(g_vdev, &hd, &IID_ID3D12VideoEncoderHeap, (void **)&heap));
  ID3D12Resource *input = texture(fmt, W, H, 1), *dpb = texture(fmt, W, H, HFRAMES);
  ID3D12Resource *bits = buffer(D3D12_HEAP_TYPE_READBACK, 1 << 18), *meta = buffer(D3D12_HEAP_TYPE_DEFAULT, 1 << 16), *resolved = buffer(D3D12_HEAP_TYPE_READBACK, 1 << 16);
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *q = NULL;
  ID3D12CommandAllocator *a = NULL;
  ID3D12VideoEncodeCommandList2 *l = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, &IID_ID3D12CommandAllocator, (void **)&a);
  if (a) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, a, NULL, &IID_ID3D12VideoEncodeCommandList2, (void **)&l);
  CHECK(enc && heap && input && dpb && bits && meta && resolved && q && l);
  if (!enc || !heap || !input || !dpb || !bits || !meta || !resolved || !q || !l) return;
  static BYTE src_y[MAX_BYTES], src_uv[MAX_BYTES / 2], recon_y[HFRAMES][MAX_BYTES], recon_uv[HFRAMES][MAX_BYTES / 2];
  static BYTE slices_data[HFRAMES][1 << 16];
  static UINT slice_size[HFRAMES][8], slice_count[HFRAMES], data_size[HFRAMES];
  const size_t luma_bytes = (size_t)W * H * BPS, chroma_bytes = (size_t)W * H / 2 * BPS;
  ID3D12Resource *dpb_list[HFRAMES];
  UINT dpb_subs[HFRAMES];
  for (UINT i = 0; i < HFRAMES; ++i) { dpb_list[i] = dpb; dpb_subs[i] = i; }
  int ok_meta = 0;
  for (UINT f = 0; f < HFRAMES; ++f) {
    make_picture(src_y, src_uv, W, H, ten, frames[f].poc / 2);
    copy_subresource(input, 0, src_y, luma_bytes, 1);
    copy_subresource(input, 1, src_uv, chroma_bytes, 1);
    /* the picture control: the descriptors are the pictures kept (coded frames, by their layer of the buffer), the lists name descriptors */
    D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC desc[3];
    UINT nd = 0, list0[2], list1[1], n0 = 0, n1 = 0;
    int where[HFRAMES];
    for (UINT k = 0; k < 3; ++k) {
      const int r = frames[f].dpb[k];
      if (r < 0) continue;
      const int used = r == frames[f].l0[0] || r == frames[f].l0[1] || r == frames[f].l1;
      desc[nd] = (D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC){(UINT)r, used, FALSE, (UINT)frames[r].poc, 0};
      where[r] = (int)nd++;
    }
    for (int k = 0; k < 2 && frames[f].l0[k] >= 0; ++k) list0[n0++] = (UINT)where[frames[f].l0[k]];
    if (frames[f].l1 >= 0) list1[n1++] = (UINT)where[frames[f].l1];
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC pic;
    memset(&pic, 0, sizeof pic);
    pic.FrameType = frames[f].type;
    pic.PictureOrderCountNumber = (UINT)frames[f].poc;
    pic.List0ReferenceFramesCount = n0; pic.pList0ReferenceFrames = list0;
    pic.List1ReferenceFramesCount = n1; pic.pList1ReferenceFrames = list1;
    pic.ReferenceFramesReconPictureDescriptorsCount = nd; pic.pReferenceFramesReconPictureDescriptors = desc;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {28, 30, 32};
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES rows = {{1}};
    D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC seq;
    memset(&seq, 0, sizeof seq);
    seq.RateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
    seq.RateControl.ConfigParams.DataSize = sizeof cqp;
    seq.RateControl.ConfigParams.pConfiguration_CQP = &cqp;
    seq.RateControl.TargetFrameRate.Numerator = 30;
    seq.RateControl.TargetFrameRate.Denominator = 1;
    seq.PictureTargetResolution = res;
    seq.SelectedLayoutMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION;
    seq.FrameSubregionsLayoutData.DataSize = sizeof rows;
    seq.FrameSubregionsLayoutData.pSlicesPartition_HEVC = &rows;
    seq.CodecGopSequence.DataSize = sizeof gop;
    seq.CodecGopSequence.pHEVCGroupOfPictures = &gop;
    D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS in;
    memset(&in, 0, sizeof in);
    in.SequenceControlDesc = seq;
    in.PictureControlDesc.Flags = frames[f].reference ? D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE : D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_NONE;
    in.PictureControlDesc.PictureControlCodecData.DataSize = sizeof pic;
    in.PictureControlDesc.PictureControlCodecData.pHEVCPicData = &pic;
    in.PictureControlDesc.ReferenceFrames.NumTexture2Ds = HFRAMES;
    in.PictureControlDesc.ReferenceFrames.ppTexture2Ds = dpb_list;
    in.PictureControlDesc.ReferenceFrames.pSubresources = dpb_subs;
    in.pInputFrame = input;
    D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS out;
    memset(&out, 0, sizeof out);
    out.Bitstream.pBuffer = bits;
    out.ReconstructedPicture.pReconstructedPicture = dpb;
    out.ReconstructedPicture.ReconstructedPictureSubresource = f;
    out.EncoderOutputMetadata.pBuffer = meta;
    ID3D12VideoEncodeCommandList2_EncodeFrame(l, enc, heap, &in, &out);
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS ri;
    memset(&ri, 0, sizeof ri);
    ri.EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_HEVC;
    ri.EncoderProfile = ed.EncodeProfile;
    ri.EncoderInputFormat = fmt;
    ri.EncodedPictureEffectiveResolution = res;
    ri.HWLayoutMetadata.pBuffer = meta;
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS ro = {{resolved, 0}};
    ID3D12VideoEncodeCommandList2_ResolveEncoderOutputMetadata(l, &ri, &ro);
    run_video(q, (ID3D12CommandList *)l, a, 1);
    BYTE *m = NULL, *b = NULL;
    ID3D12Resource_Map(resolved, 0, NULL, (void **)&m);
    ID3D12Resource_Map(bits, 0, NULL, (void **)&b);
    D3D12_VIDEO_ENCODER_OUTPUT_METADATA md;
    memcpy(&md, m, sizeof md);
    const D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA *sub = (const D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA *)(m + sizeof md);
    UINT64 sum = 0;
    int starts = 1;
    for (UINT64 i = 0; i < md.WrittenSubregionsCount && i < 8; ++i) {
      starts &= b[sum] == 0 && b[sum + 1] == 0 && b[sum + 2] == 0 && b[sum + 3] == 1 && sub[i].bHeaderSize > 48;
      slice_size[f][i] = (UINT)sub[i].bSize;
      sum += sub[i].bSize;
    }
    const UINT qp = frames[f].type == D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_B_FRAME ? 32 : frames[f].type == D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_P_FRAME ? 30 : 28;
    /* (three rows of 32 x 32 coding tree blocks: three slices; the statistics count 8x8 units) */
    const int good = md.EncodeErrorFlags == 0 && md.WrittenSubregionsCount == 3 && sum == md.EncodedBitstreamWrittenBytesCount && starts && sum <= sizeof slices_data[f] &&
                     md.EncodeStats.AverageQP == qp && md.EncodeStats.IntraCodingUnitsCount + md.EncodeStats.InterCodingUnitsCount + md.EncodeStats.SkipCodingUnitsCount == (W / 8) * (H / 8);
    if (!good) printf("%s frame %u: errors %llx, %llu slices, %llu bytes, QP %llu\n", name, f, (unsigned long long)md.EncodeErrorFlags, (unsigned long long)md.WrittenSubregionsCount,
                      (unsigned long long)md.EncodedBitstreamWrittenBytesCount, (unsigned long long)md.EncodeStats.AverageQP);
    ok_meta += good;
    slice_count[f] = good ? (UINT)md.WrittenSubregionsCount : 0;
    data_size[f] = good ? (UINT)sum : 0;
    if (good) memcpy(slices_data[f], b, (size_t)sum);
    ID3D12Resource_Unmap(bits, 0, NULL);
    ID3D12Resource_Unmap(resolved, 0, NULL);
    copy_subresource(dpb, f, recon_y[f], luma_bytes, 0);
    copy_subresource(dpb, HFRAMES + f, recon_uv[f], chroma_bytes, 0);
  }
  printf("d3d12encodecodectest %s (%ux%u): %d of %d pictures with good metadata\n", name, W, H, ok_meta, HFRAMES);
  CHECK(ok_meta == HFRAMES);
  ID3D12VideoEncodeCommandList2_Release(l);
  ID3D12CommandAllocator_Release(a);
  ID3D12CommandQueue_Release(q);

  /* decoded again: picture f into surface f; the program's parameter sets as DXVA_PicParams_HEVC */
  D3D12_VIDEO_DECODER_DESC dd = {0, {ten ? D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10 : D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN, D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE, D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE}};
  D3D12_VIDEO_DECODER_HEAP_DESC dh;
  memset(&dh, 0, sizeof dh);
  dh.Configuration = dd.Configuration;
  dh.DecodeWidth = W;
  dh.DecodeHeight = H;
  dh.Format = fmt;
  dh.MaxDecodePictureBufferCount = HFRAMES;
  ID3D12VideoDecoder *dec = NULL;
  ID3D12VideoDecoderHeap *dheap = NULL;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&dec));
  CHECK_HR(ID3D12VideoDevice3_CreateVideoDecoderHeap(g_vdev, &dh, &IID_ID3D12VideoDecoderHeap, (void **)&dheap));
  ID3D12Resource *surfaces = texture(fmt, W, H, HFRAMES), *dbits = buffer(D3D12_HEAP_TYPE_UPLOAD, 1 << 16);
  ID3D12Resource *tex[HFRAMES];
  UINT subs[HFRAMES];
  for (UINT i = 0; i < HFRAMES; ++i) { tex[i] = surfaces; subs[i] = i; }
  D3D12_COMMAND_QUEUE_DESC dq = {D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *vq = NULL;
  ID3D12CommandAllocator *va = NULL;
  ID3D12VideoDecodeCommandList *vl = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &dq, &IID_ID3D12CommandQueue, (void **)&vq);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, &IID_ID3D12CommandAllocator, (void **)&va);
  if (va) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, va, NULL, &IID_ID3D12VideoDecodeCommandList, (void **)&vl);
  CHECK(dec && dheap && surfaces && dbits && vq && vl);
  static BYTE y[MAX_BYTES], uv[MAX_BYTES / 2];
  int same = 0;
  for (UINT f = 0; f < HFRAMES && dec && dheap && vl && surfaces && dbits; ++f) {
    DXVA_PicParams_HEVC pp;
    memset(&pp, 0, sizeof pp);
    pp.PicWidthInMinCbsY = (USHORT)(W / 8);
    pp.PicHeightInMinCbsY = (USHORT)(H / 8);
    pp.chroma_format_idc = 1;
    pp.bit_depth_luma_minus8 = pp.bit_depth_chroma_minus8 = ten ? 2 : 0;
    pp.log2_max_pic_order_cnt_lsb_minus4 = gop.log2_max_pic_order_cnt_lsb_minus4;
    pp.CurrPic.Index7Bits = (UCHAR)f;
    pp.sps_max_dec_pic_buffering_minus1 = 4;
    pp.log2_min_luma_coding_block_size_minus3 = 0;
    pp.log2_diff_max_min_luma_coding_block_size = 2;   /* (CTB 32) */
    pp.log2_min_transform_block_size_minus2 = 0;
    pp.log2_diff_max_min_transform_block_size = 3;
    pp.max_transform_hierarchy_depth_inter = cfg.max_transform_hierarchy_depth_inter;
    pp.max_transform_hierarchy_depth_intra = cfg.max_transform_hierarchy_depth_intra;
    pp.sample_adaptive_offset_enabled_flag = 1;
    pp.cu_qp_delta_enabled_flag = 1;
    pp.pps_loop_filter_across_slices_enabled_flag = 1;
    pp.lists_modification_present_flag = 1;
    const int idr = frames[f].type == D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_IDR_FRAME;
    pp.IrapPicFlag = pp.IdrPicFlag = pp.IntraPicFlag = idr;
    pp.CurrPicOrderCntVal = frames[f].poc;
    memset(pp.RefPicList, 0xFF, sizeof pp.RefPicList);
    memset(pp.RefPicSetStCurrBefore, 0xFF, sizeof pp.RefPicSetStCurrBefore);
    memset(pp.RefPicSetStCurrAfter, 0xFF, sizeof pp.RefPicSetStCurrAfter);
    memset(pp.RefPicSetLtCurr, 0xFF, sizeof pp.RefPicSetLtCurr);
    /* the reference picture set: every picture kept, the used ones in the sets (before: closest first; after: closest first) */
    int before[3], after[3], before_all[3], after_all[3];
    UINT nb = 0, na = 0, nba = 0, naa = 0, ne = 0;
    UCHAR entry_of[HFRAMES];
    for (UINT k = 0; k < 3; ++k) {
      const int r = frames[f].dpb[k];
      if (r < 0) continue;
      pp.RefPicList[ne].Index7Bits = (UCHAR)r;
      pp.PicOrderCntValList[ne] = frames[r].poc;
      entry_of[r] = (UCHAR)ne++;
    }
    /* (sorted by order count: the dpb lists of the test are in coding order) */
    int order[3];
    UINT no = 0;
    for (UINT k = 0; k < 3; ++k) if (frames[f].dpb[k] >= 0) order[no++] = frames[f].dpb[k];
    for (UINT i = 0; i < no; ++i)
      for (UINT j = i + 1; j < no; ++j) {
        const int pi = frames[order[i]].poc, pj = frames[order[j]].poc;
        const int closer = (pi < frames[f].poc) == (pj < frames[f].poc) ? (pi < frames[f].poc ? pj > pi : pj < pi) : 0;
        if (closer) { const int t = order[i]; order[i] = order[j]; order[j] = t; }
      }
    for (UINT i = 0; i < no; ++i) {
      const int r = order[i];
      const int used = r == frames[f].l0[0] || r == frames[f].l0[1] || r == frames[f].l1;
      if (frames[r].poc < frames[f].poc) { before_all[nba++] = frames[r].poc; if (used) pp.RefPicSetStCurrBefore[nb] = entry_of[r], before[nb++] = frames[r].poc; }
      else { after_all[naa++] = frames[r].poc; if (used) pp.RefPicSetStCurrAfter[na] = entry_of[r], after[na++] = frames[r].poc; }
    }
    (void)before; (void)after;
    pp.wNumBitsForShortTermRPSInSlice = idr ? 0 : (USHORT)st_rps_bits(frames[f].poc, before_all, nba, after_all, naa);
    pp.StatusReportFeedbackNumber = f + 1;
    DXVA_Slice_HEVC_Short sc[8];
    UINT at = 0;
    for (UINT i = 0; i < slice_count[f]; ++i) {
      sc[i].BSNALunitDataLocation = at;
      sc[i].SliceBytesInBuffer = slice_size[f][i];
      sc[i].wBadSliceChopping = 0;
      at += slice_size[f][i];
    }
    BYTE *map = NULL;
    ID3D12Resource_Map(dbits, 0, NULL, (void **)&map);
    memcpy(map, slices_data[f], data_size[f]);
    ID3D12Resource_Unmap(dbits, 0, NULL);
    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS din;
    memset(&din, 0, sizeof din);
    din.NumFrameArguments = 2;
    din.FrameArguments[0] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, sizeof pp, &pp};
    din.FrameArguments[1] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, (UINT)(sizeof sc[0] * slice_count[f]), sc};
    din.ReferenceFrames.NumTexture2Ds = HFRAMES;
    din.ReferenceFrames.ppTexture2Ds = tex;
    din.ReferenceFrames.pSubresources = subs;
    din.CompressedBitstream.pBuffer = dbits;
    din.CompressedBitstream.Size = data_size[f];
    din.pHeap = dheap;
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS dout;
    memset(&dout, 0, sizeof dout);
    dout.pOutputTexture2D = surfaces;
    dout.OutputSubresource = f;
    ID3D12VideoDecodeCommandList_DecodeFrame(vl, dec, &dout, &din);
    run_video(vq, (ID3D12CommandList *)vl, va, 0);
    copy_subresource(surfaces, f, y, luma_bytes, 0);
    copy_subresource(surfaces, HFRAMES + f, uv, chroma_bytes, 0);
    const int eq = memcmp(y, recon_y[f], luma_bytes) == 0 && memcmp(uv, recon_uv[f], chroma_bytes) == 0;
    if (!eq) printf("%s picture %u (POC %d): the decoder's picture is not the reconstruction\n", name, f, frames[f].poc);
    same += eq;
  }
  printf("d3d12encodecodectest %s: the D3D12 decoder decodes %d of %d encoded pictures to their reconstructions, sample for sample\n", name, same, HFRAMES);
  CHECK(same == HFRAMES);
  if (vl) ID3D12VideoDecodeCommandList_Release(vl);
  if (va) ID3D12CommandAllocator_Release(va);
  if (vq) ID3D12CommandQueue_Release(vq);
  if (dbits) ID3D12Resource_Release(dbits);
  if (surfaces) ID3D12Resource_Release(surfaces);
  if (dheap) ID3D12VideoDecoderHeap_Release(dheap);
  if (dec) ID3D12VideoDecoder_Release(dec);
  ID3D12Resource_Release(resolved);
  ID3D12Resource_Release(meta);
  ID3D12Resource_Release(bits);
  ID3D12Resource_Release(dpb);
  ID3D12Resource_Release(input);
  ID3D12VideoEncoderHeap_Release(heap);
  ID3D12VideoEncoder_Release(enc);
}

/* ---- AV1 --------------------------------------------------------------------------------------------------------------------------------------------------------- */

#define AFRAMES 5

static void av1_case(int ten) {
  const UINT W = 160, H = 96, BPS = ten ? 2 : 1;
  const DXGI_FORMAT fmt = ten ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
  const char *name = ten ? "AV1 10-bit" : "AV1 8-bit";
  /* key (all slots), inter from slot 0 into slot 1, inter (LAST slot 1, GOLDEN slot 0; switchable filters, TX_MODE_SELECT) into slot 2, intra-only into slot 3,
   * inter (LAST slot 3, LAST2 slot 2) into slot 4 */
  static const struct { D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE type; UINT refresh; UINT refs[7]; D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS filter; D3D12_VIDEO_ENCODER_AV1_TX_MODE tx; } frames[AFRAMES] = {
      {D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_KEY_FRAME, 0xFF, {0, 0, 0, 0, 0, 0, 0}, D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_EIGHTTAP, D3D12_VIDEO_ENCODER_AV1_TX_MODE_LARGEST},
      {D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTER_FRAME, 0x02, {0, 0, 0, 0, 0, 0, 0}, D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_EIGHTTAP, D3D12_VIDEO_ENCODER_AV1_TX_MODE_LARGEST},
      {D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTER_FRAME, 0x04, {1, 1, 1, 0, 0, 0, 0}, D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_SWITCHABLE, D3D12_VIDEO_ENCODER_AV1_TX_MODE_SELECT},
      {D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTRA_ONLY_FRAME, 0x08, {0, 0, 0, 0, 0, 0, 0}, D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_EIGHTTAP, D3D12_VIDEO_ENCODER_AV1_TX_MODE_LARGEST},
      {D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTER_FRAME, 0x10, {3, 2, 2, 0, 1, 1, 1}, D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_EIGHTTAP_SMOOTH, D3D12_VIDEO_ENCODER_AV1_TX_MODE_LARGEST}};
  const UINT features = D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ORDER_HINT_TOOLS | D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CDEF_FILTERING | D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_EDGE_FILTER |
                        D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ALLOW_HIGH_PRECISION_MV;
  D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION cfg = {(D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAGS)features, 6};
  D3D12_VIDEO_ENCODER_AV1_SEQUENCE_STRUCTURE gop = {30, 1};
  D3D12_VIDEO_ENCODER_DESC ed;
  memset(&ed, 0, sizeof ed);
  ed.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_AV1;
  ed.EncodeProfile = av1_profile();
  ed.InputFormat = fmt;
  ed.CodecConfiguration.DataSize = sizeof cfg;
  ed.CodecConfiguration.pAV1Config = &cfg;
  ed.MaxMotionEstimationPrecision = D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
  ID3D12VideoEncoder *enc = NULL;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoEncoder(g_vdev, &ed, &IID_ID3D12VideoEncoder, (void **)&enc));
  D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS level = {D3D12_VIDEO_ENCODER_AV1_LEVELS_3_0, D3D12_VIDEO_ENCODER_AV1_TIER_MAIN};
  D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC res = {W, H};
  D3D12_VIDEO_ENCODER_HEAP_DESC hd;
  memset(&hd, 0, sizeof hd);
  hd.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_AV1;
  hd.EncodeProfile = ed.EncodeProfile;
  hd.EncodeLevel.DataSize = sizeof level;
  hd.EncodeLevel.pAV1LevelSetting = &level;
  hd.ResolutionsListCount = 1;
  hd.pResolutionList = &res;
  ID3D12VideoEncoderHeap *heap = NULL;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoEncoderHeap(g_vdev, &hd, &IID_ID3D12VideoEncoderHeap, (void **)&heap));
  ID3D12Resource *input = texture(fmt, W, H, 1), *dpb = texture(fmt, W, H, AFRAMES);
  ID3D12Resource *bits = buffer(D3D12_HEAP_TYPE_READBACK, 1 << 18), *meta = buffer(D3D12_HEAP_TYPE_DEFAULT, 1 << 16), *resolved = buffer(D3D12_HEAP_TYPE_READBACK, 1 << 16);
  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *q = NULL;
  ID3D12CommandAllocator *a = NULL;
  ID3D12VideoEncodeCommandList2 *l = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, &IID_ID3D12CommandAllocator, (void **)&a);
  if (a) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, a, NULL, &IID_ID3D12VideoEncodeCommandList2, (void **)&l);
  CHECK(enc && heap && input && dpb && bits && meta && resolved && q && l);
  if (!enc || !heap || !input || !dpb || !bits || !meta || !resolved || !q || !l) return;
  static BYTE src_y[MAX_BYTES * 2], src_uv[MAX_BYTES], recon_y[AFRAMES][MAX_BYTES * 2], recon_uv[AFRAMES][MAX_BYTES];
  static BYTE tile_data[AFRAMES][1 << 16];
  static UINT tile_size[AFRAMES][8], tile_count[AFRAMES], data_size[AFRAMES];
  static D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES post[AFRAMES];
  static D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES layout[AFRAMES];
  static D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA pics[AFRAMES];
  const size_t luma_bytes = (size_t)W * H * BPS, chroma_bytes = (size_t)W * H / 2 * BPS;
  ID3D12Resource *dpb_list[AFRAMES];
  UINT dpb_subs[AFRAMES];
  for (UINT i = 0; i < AFRAMES; ++i) { dpb_list[i] = dpb; dpb_subs[i] = i; }
  UINT slot_frame[8];   /* the coded frame each slot holds */
  int ok_meta = 0;
  for (UINT f = 0; f < AFRAMES; ++f) {
    make_picture(src_y, src_uv, W, H, ten, (int)f);
    copy_subresource(input, 0, src_y, luma_bytes, 1);
    copy_subresource(input, 1, src_uv, chroma_bytes, 1);
    D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA *p = &pics[f];
    memset(p, 0, sizeof *p);
    p->Flags = frames[f].type == D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTER_FRAME ? D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ALLOW_HIGH_PRECISION_MV : D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_NONE;
    p->FrameType = frames[f].type;
    p->CompoundPredictionType = D3D12_VIDEO_ENCODER_AV1_COMP_PREDICTION_TYPE_SINGLE_REFERENCE;
    p->InterpolationFilter = frames[f].filter;
    p->TxMode = frames[f].tx;
    p->SuperResDenominator = 8;
    p->OrderHint = f;
    p->PictureIndex = f;
    for (UINT i = 0; i < 8; ++i) {
      D3D12_VIDEO_ENCODER_AV1_REFERENCE_PICTURE_DESCRIPTOR *r = &p->ReferenceFramesReconPictureDescriptors[i];
      if (f == 0) { r->ReconstructedPictureResourceIndex = 0xFFFFFFFFu; continue; }
      r->ReconstructedPictureResourceIndex = slot_frame[i];
      r->FrameType = frames[slot_frame[i]].type;
      r->OrderHint = slot_frame[i];
      r->PictureIndex = slot_frame[i];
    }
    for (UINT i = 0; i < 7; ++i) p->ReferenceIndices[i] = frames[f].refs[i];
    p->PrimaryRefFrame = f ? 0 : 7;   /* (asked for; the post-encode values say the one used) */
    p->RefreshFrameFlags = frames[f].refresh;
    p->LoopFilter.LoopFilterLevel[0] = 12;
    p->LoopFilter.LoopFilterLevel[1] = 10;
    p->LoopFilter.LoopFilterLevelU = 6;
    p->LoopFilter.LoopFilterLevelV = 6;
    p->LoopFilter.LoopFilterSharpnessLevel = 2;
    p->LoopFilter.RefDeltas[0] = 1; p->LoopFilter.RefDeltas[4] = -1; p->LoopFilter.RefDeltas[6] = -1; p->LoopFilter.RefDeltas[7] = -1;
    p->CDEF.CdefBits = 1;
    p->CDEF.CdefDampingMinus3 = 1;
    p->CDEF.CdefYPriStrength[1] = 5;
    p->CDEF.CdefYSecStrength[1] = 2;
    p->CDEF.CdefUVPriStrength[1] = 3;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {100, 130, 150};
    D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES tiles;
    memset(&tiles, 0, sizeof tiles);
    tiles.RowCount = 1;
    tiles.ColCount = 2;
    D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC seq;
    memset(&seq, 0, sizeof seq);
    seq.RateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
    seq.RateControl.ConfigParams.DataSize = sizeof cqp;
    seq.RateControl.ConfigParams.pConfiguration_CQP = &cqp;
    seq.RateControl.TargetFrameRate.Numerator = 30;
    seq.RateControl.TargetFrameRate.Denominator = 1;
    seq.PictureTargetResolution = res;
    seq.SelectedLayoutMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_GRID_PARTITION_;
    seq.FrameSubregionsLayoutData.DataSize = sizeof tiles;
    seq.FrameSubregionsLayoutData.pTilesPartition_AV1 = &tiles;
    seq.CodecGopSequence.DataSize = sizeof gop;
    seq.CodecGopSequence.pAV1SequenceStructure = &gop;
    D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS in;
    memset(&in, 0, sizeof in);
    in.SequenceControlDesc = seq;
    in.PictureControlDesc.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE;
    in.PictureControlDesc.PictureControlCodecData.DataSize = sizeof *p;
    in.PictureControlDesc.PictureControlCodecData.pAV1PicData = p;
    in.PictureControlDesc.ReferenceFrames.NumTexture2Ds = f ? AFRAMES : 0;
    in.PictureControlDesc.ReferenceFrames.ppTexture2Ds = f ? dpb_list : NULL;
    in.PictureControlDesc.ReferenceFrames.pSubresources = f ? dpb_subs : NULL;
    in.pInputFrame = input;
    D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS out;
    memset(&out, 0, sizeof out);
    out.Bitstream.pBuffer = bits;
    out.ReconstructedPicture.pReconstructedPicture = dpb;
    out.ReconstructedPicture.ReconstructedPictureSubresource = f;
    out.EncoderOutputMetadata.pBuffer = meta;
    ID3D12VideoEncodeCommandList2_EncodeFrame(l, enc, heap, &in, &out);
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS ri;
    memset(&ri, 0, sizeof ri);
    ri.EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_AV1;
    ri.EncoderProfile = ed.EncodeProfile;
    ri.EncoderInputFormat = fmt;
    ri.EncodedPictureEffectiveResolution = res;
    ri.HWLayoutMetadata.pBuffer = meta;
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS ro = {{resolved, 0}};
    ID3D12VideoEncodeCommandList2_ResolveEncoderOutputMetadata(l, &ri, &ro);
    run_video(q, (ID3D12CommandList *)l, a, 1);
    /* the resolved layout: the output metadata, a record a tile, the tile layout, the post-encode values */
    BYTE *m = NULL, *b = NULL;
    ID3D12Resource_Map(resolved, 0, NULL, (void **)&m);
    ID3D12Resource_Map(bits, 0, NULL, (void **)&b);
    D3D12_VIDEO_ENCODER_OUTPUT_METADATA md;
    memcpy(&md, m, sizeof md);
    const D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA *sub = (const D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA *)(m + sizeof md);
    const UINT n = md.WrittenSubregionsCount <= 8 ? (UINT)md.WrittenSubregionsCount : 0;
    memcpy(&layout[f], m + sizeof md + n * sizeof *sub, sizeof layout[f]);
    memcpy(&post[f], m + sizeof md + n * sizeof *sub + sizeof layout[f], sizeof post[f]);
    UINT64 sum = 0;
    for (UINT i = 0; i < n; ++i) { tile_size[f][i] = (UINT)sub[i].bSize; sum += sub[i].bSize; }
    const UINT qindex = frames[f].type == D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTER_FRAME ? 130 : 100;
    const int good = md.EncodeErrorFlags == 0 && n == 2 && sum == md.EncodedBitstreamWrittenBytesCount && sum <= sizeof tile_data[f] && md.EncodeStats.AverageQP == qindex &&
                     layout[f].ColCount == 2 && layout[f].RowCount == 1 && layout[f].ColWidths[0] == 2 && layout[f].ColWidths[1] == 1 && layout[f].RowHeights[0] == 2 &&
                     post[f].Quantization.BaseQIndex == qindex && post[f].PrimaryRefFrame == 7 && post[f].CompoundPredictionType == 0 && post[f].ReferenceIndices[0] == frames[f].refs[0] &&
                     post[f].LoopFilter.LoopFilterLevel[0] == 12 && post[f].CDEF.CdefBits == 1 && post[f].CDEF.CdefYPriStrength[1] == 5 &&
                     md.EncodeStats.IntraCodingUnitsCount + md.EncodeStats.InterCodingUnitsCount + md.EncodeStats.SkipCodingUnitsCount == (W / 8) * (H / 8);
    if (!good)
      printf("%s frame %u: errors %llx, %llu tiles, %llu bytes, qindex %llu / %llu, primary %llu, layout %llux%llu (%llu %llu | %llu), refs[0] %llu, lf %llu, cdef %llu/%llu, units %llu+%llu+%llu\n",
             name, f, (unsigned long long)md.EncodeErrorFlags, (unsigned long long)md.WrittenSubregionsCount, (unsigned long long)md.EncodedBitstreamWrittenBytesCount,
             (unsigned long long)md.EncodeStats.AverageQP, (unsigned long long)post[f].Quantization.BaseQIndex, (unsigned long long)post[f].PrimaryRefFrame,
             (unsigned long long)layout[f].ColCount, (unsigned long long)layout[f].RowCount, (unsigned long long)layout[f].ColWidths[0], (unsigned long long)layout[f].ColWidths[1],
             (unsigned long long)layout[f].RowHeights[0], (unsigned long long)post[f].ReferenceIndices[0], (unsigned long long)post[f].LoopFilter.LoopFilterLevel[0],
             (unsigned long long)post[f].CDEF.CdefBits, (unsigned long long)post[f].CDEF.CdefYPriStrength[1], (unsigned long long)md.EncodeStats.IntraCodingUnitsCount,
             (unsigned long long)md.EncodeStats.InterCodingUnitsCount, (unsigned long long)md.EncodeStats.SkipCodingUnitsCount);
    ok_meta += good;
    tile_count[f] = good ? n : 0;
    data_size[f] = good ? (UINT)sum : 0;
    if (good) memcpy(tile_data[f], b, (size_t)sum);
    ID3D12Resource_Unmap(bits, 0, NULL);
    ID3D12Resource_Unmap(resolved, 0, NULL);
    copy_subresource(dpb, f, recon_y[f], luma_bytes, 0);
    copy_subresource(dpb, AFRAMES + f, recon_uv[f], chroma_bytes, 0);
    for (UINT i = 0; i < 8; ++i)
      if (frames[f].refresh & (1u << i)) slot_frame[i] = f;
  }
  printf("d3d12encodecodectest %s (%ux%u): %d of %d frames with good metadata and post-encode values\n", name, W, H, ok_meta, AFRAMES);
  CHECK(ok_meta == AFRAMES);
  ID3D12VideoEncodeCommandList2_Release(l);
  ID3D12CommandAllocator_Release(a);
  ID3D12CommandQueue_Release(q);

  /* decoded again: frame f into surface f; DXVA_PicParams_AV1 from the sequence (what the program's sequence header says), the picture control and the post-encode values */
  D3D12_VIDEO_DECODER_DESC dd = {0, {D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0, D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE, D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE}};
  D3D12_VIDEO_DECODER_HEAP_DESC dh;
  memset(&dh, 0, sizeof dh);
  dh.Configuration = dd.Configuration;
  dh.DecodeWidth = W;
  dh.DecodeHeight = H;
  dh.Format = fmt;
  dh.MaxDecodePictureBufferCount = AFRAMES;
  ID3D12VideoDecoder *dec = NULL;
  ID3D12VideoDecoderHeap *dheap = NULL;
  CHECK_HR(ID3D12VideoDevice3_CreateVideoDecoder(g_vdev, &dd, &IID_ID3D12VideoDecoder, (void **)&dec));
  CHECK_HR(ID3D12VideoDevice3_CreateVideoDecoderHeap(g_vdev, &dh, &IID_ID3D12VideoDecoderHeap, (void **)&dheap));
  ID3D12Resource *surfaces = texture(fmt, W, H, AFRAMES), *dbits = buffer(D3D12_HEAP_TYPE_UPLOAD, 1 << 16);
  ID3D12Resource *tex[AFRAMES];
  UINT subs[AFRAMES];
  for (UINT i = 0; i < AFRAMES; ++i) { tex[i] = surfaces; subs[i] = i; }
  D3D12_COMMAND_QUEUE_DESC dq = {D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
  ID3D12CommandQueue *vq = NULL;
  ID3D12CommandAllocator *va = NULL;
  ID3D12VideoDecodeCommandList *vl = NULL;
  ID3D12Device_CreateCommandQueue(g_dev, &dq, &IID_ID3D12CommandQueue, (void **)&vq);
  ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, &IID_ID3D12CommandAllocator, (void **)&va);
  if (va) ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, va, NULL, &IID_ID3D12VideoDecodeCommandList, (void **)&vl);
  CHECK(dec && dheap && surfaces && dbits && vq && vl);
  static BYTE y[MAX_BYTES * 2], uv[MAX_BYTES];
  int same = 0;
  UINT slot_surface[8];
  for (UINT i = 0; i < 8; ++i) slot_surface[i] = 0xFF;
  for (UINT f = 0; f < AFRAMES && dec && dheap && vl && surfaces && dbits; ++f) {
    const D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA *p = &pics[f];
    const D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES *v = &post[f];
    const int intra = p->FrameType == D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_KEY_FRAME || p->FrameType == D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_INTRA_ONLY_FRAME;
    DXVA_PicParams_AV1 pp;
    memset(&pp, 0, sizeof pp);
    pp.width = pp.max_width = W;
    pp.height = pp.max_height = H;
    pp.CurrPicTextureIndex = (UCHAR)f;
    pp.superres_denom = 8;
    pp.bitdepth = ten ? 10 : 8;
    pp.seq_profile = 0;
    pp.tiles.cols = (UCHAR)layout[f].ColCount;
    pp.tiles.rows = (UCHAR)layout[f].RowCount;
    pp.tiles.context_update_id = (USHORT)layout[f].ContextUpdateTileId;
    for (UINT i = 0; i < layout[f].ColCount && i < 64; ++i) pp.tiles.widths[i] = (USHORT)layout[f].ColWidths[i];
    for (UINT i = 0; i < layout[f].RowCount && i < 64; ++i) pp.tiles.heights[i] = (USHORT)layout[f].RowHeights[i];
    pp.coding.intra_edge_filter = 1;
    pp.coding.cdef = 1;
    pp.coding.high_precision_mv = (p->Flags & D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ALLOW_HIGH_PRECISION_MV) != 0;
    pp.coding.tx_mode = p->TxMode;
    pp.coding.reference_frame_update = 1;
    pp.format.frame_type = p->FrameType;
    pp.format.show_frame = 1;
    pp.format.showable_frame = p->FrameType != D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_KEY_FRAME;
    pp.format.subsampling_x = pp.format.subsampling_y = 1;
    pp.primary_ref_frame = (UCHAR)v->PrimaryRefFrame;
    pp.order_hint = (UCHAR)p->OrderHint;
    pp.order_hint_bits = (UCHAR)(cfg.OrderHintBitsMinus1 + 1);
    for (UINT i = 0; i < 7; ++i) {
      pp.frame_refs[i].width = W;
      pp.frame_refs[i].height = H;
      pp.frame_refs[i].Index = intra ? 0xFF : (UCHAR)v->ReferenceIndices[i];
    }
    for (UINT i = 0; i < 8; ++i) pp.RefFrameMapTextureIndex[i] = (UCHAR)slot_surface[i];
    pp.loop_filter.filter_level[0] = (UCHAR)v->LoopFilter.LoopFilterLevel[0];
    pp.loop_filter.filter_level[1] = (UCHAR)v->LoopFilter.LoopFilterLevel[1];
    pp.loop_filter.filter_level_u = (UCHAR)v->LoopFilter.LoopFilterLevelU;
    pp.loop_filter.filter_level_v = (UCHAR)v->LoopFilter.LoopFilterLevelV;
    pp.loop_filter.sharpness_level = (UCHAR)v->LoopFilter.LoopFilterSharpnessLevel;
    pp.loop_filter.mode_ref_delta_enabled = (UCHAR)v->LoopFilter.LoopFilterDeltaEnabled;
    for (UINT i = 0; i < 8; ++i) pp.loop_filter.ref_deltas[i] = (CHAR)v->LoopFilter.RefDeltas[i];
    for (UINT i = 0; i < 2; ++i) pp.loop_filter.mode_deltas[i] = (CHAR)v->LoopFilter.ModeDeltas[i];
    pp.quantization.delta_q_present = (UCHAR)v->QuantizationDelta.DeltaQPresent;
    pp.quantization.delta_q_res = (UCHAR)v->QuantizationDelta.DeltaQRes;
    pp.quantization.base_qindex = (UCHAR)v->Quantization.BaseQIndex;
    pp.quantization.y_dc_delta_q = (CHAR)v->Quantization.YDCDeltaQ;
    pp.quantization.u_dc_delta_q = (CHAR)v->Quantization.UDCDeltaQ;
    pp.quantization.u_ac_delta_q = (CHAR)v->Quantization.UACDeltaQ;
    pp.quantization.v_dc_delta_q = (CHAR)v->Quantization.VDCDeltaQ;
    pp.quantization.v_ac_delta_q = (CHAR)v->Quantization.VACDeltaQ;
    pp.quantization.qm_y = pp.quantization.qm_u = pp.quantization.qm_v = 0xFF;
    pp.cdef.damping = (UCHAR)v->CDEF.CdefDampingMinus3;
    pp.cdef.bits = (UCHAR)v->CDEF.CdefBits;
    for (UINT i = 0; i < 8; ++i) {
      pp.cdef.y_strengths[i].combined = (UCHAR)(v->CDEF.CdefYPriStrength[i] | v->CDEF.CdefYSecStrength[i] << 6);
      pp.cdef.uv_strengths[i].combined = (UCHAR)(v->CDEF.CdefUVPriStrength[i] | v->CDEF.CdefUVSecStrength[i] << 6);
    }
    pp.interp_filter = (UCHAR)p->InterpolationFilter;
    pp.StatusReportFeedbackNumber = f + 1;
    DXVA_Tile_AV1 tc[8];
    UINT at = 0;
    for (UINT i = 0; i < tile_count[f]; ++i) {
      memset(&tc[i], 0, sizeof tc[i]);
      tc[i].DataOffset = at;
      tc[i].DataSize = tile_size[f][i];
      tc[i].row = (USHORT)(i / layout[f].ColCount);
      tc[i].column = (USHORT)(i % layout[f].ColCount);
      tc[i].anchor_frame = 0xFF;
      at += tile_size[f][i];
    }
    BYTE *map = NULL;
    ID3D12Resource_Map(dbits, 0, NULL, (void **)&map);
    memcpy(map, tile_data[f], data_size[f]);
    ID3D12Resource_Unmap(dbits, 0, NULL);
    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS din;
    memset(&din, 0, sizeof din);
    din.NumFrameArguments = 2;
    din.FrameArguments[0] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, sizeof pp, &pp};
    din.FrameArguments[1] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, (UINT)(sizeof tc[0] * tile_count[f]), tc};
    din.ReferenceFrames.NumTexture2Ds = AFRAMES;
    din.ReferenceFrames.ppTexture2Ds = tex;
    din.ReferenceFrames.pSubresources = subs;
    din.CompressedBitstream.pBuffer = dbits;
    din.CompressedBitstream.Size = data_size[f];
    din.pHeap = dheap;
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS dout;
    memset(&dout, 0, sizeof dout);
    dout.pOutputTexture2D = surfaces;
    dout.OutputSubresource = f;
    ID3D12VideoDecodeCommandList_DecodeFrame(vl, dec, &dout, &din);
    run_video(vq, (ID3D12CommandList *)vl, va, 0);
    copy_subresource(surfaces, f, y, luma_bytes, 0);
    copy_subresource(surfaces, AFRAMES + f, uv, chroma_bytes, 0);
    const int eq = memcmp(y, recon_y[f], luma_bytes) == 0 && memcmp(uv, recon_uv[f], chroma_bytes) == 0;
    if (!eq) {
      size_t diff = 0, first = (size_t)-1;
      for (size_t i = 0; i < luma_bytes; ++i)
        if (y[i] != recon_y[f][i]) { ++diff; if (first == (size_t)-1) first = i; }
      printf("%s frame %u: the decoder's picture is not the reconstruction (%u luma bytes differ, the first at x %u y %u: %u, not %u)\n", name, f, (unsigned)diff,
             first == (size_t)-1 ? 0 : (unsigned)(first % (W * BPS)), first == (size_t)-1 ? 0 : (unsigned)(first / (W * BPS)), first == (size_t)-1 ? 0 : y[first],
             first == (size_t)-1 ? 0 : recon_y[f][first]);
    }
    same += eq;
    for (UINT i = 0; i < 8; ++i)
      if (p->RefreshFrameFlags & (1u << i)) slot_surface[i] = f;
  }
  printf("d3d12encodecodectest %s: the D3D12 decoder decodes %d of %d encoded frames to their reconstructions, sample for sample\n", name, same, AFRAMES);
  CHECK(same == AFRAMES);
  if (vl) ID3D12VideoDecodeCommandList_Release(vl);
  if (va) ID3D12CommandAllocator_Release(va);
  if (vq) ID3D12CommandQueue_Release(vq);
  if (dbits) ID3D12Resource_Release(dbits);
  if (surfaces) ID3D12Resource_Release(surfaces);
  if (dheap) ID3D12VideoDecoderHeap_Release(dheap);
  if (dec) ID3D12VideoDecoder_Release(dec);
  ID3D12Resource_Release(resolved);
  ID3D12Resource_Release(meta);
  ID3D12Resource_Release(bits);
  ID3D12Resource_Release(dpb);
  ID3D12Resource_Release(input);
  ID3D12VideoEncoderHeap_Release(heap);
  ID3D12VideoEncoder_Release(enc);
}

int main(void) {
  if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&g_dev)) || !g_dev) {
    printf("d3d12encodecodectest: no device\nd3d12encodecodectest: 1 checks, 1 failed\n");
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
    test_hevc_features();
    test_av1_features();
    hevc_case(0);
    hevc_case(1);
    av1_case(0);
    av1_case(1);
    ID3D12VideoDevice3_Release(g_vdev);
  }
  ID3D12Fence_Release(g_fence);
  ID3D12CommandQueue_Release(g_queue);
  ID3D12Device_Release(g_dev);
  printf("d3d12encodecodectest: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
