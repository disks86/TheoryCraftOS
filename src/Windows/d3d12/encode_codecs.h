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

/* d3d12.dll: the HEVC and AV1 encoders' part of encode.inc (encode_codecs.inc, included at its end): what an encoder and an encoder heap keep of them, and the hooks
 * encode.inc calls for any codec but H.264. */
#pragma once
#include "video_encode_types.h"

typedef struct VEncCodecData {
  D3D12_VIDEO_ENCODER_PROFILE_HEVC hevc_profile;
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC hevc_config;
  D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC hevc_level;
  D3D12_VIDEO_ENCODER_AV1_PROFILE av1_profile;
  D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION av1_config;
  D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS av1_level;
} VEncCodecData;

/* Whether `c` is one of the codecs encode_codecs.inc does. */
static int vencx_codec(D3D12_VIDEO_ENCODER_CODEC c) { return c == D3D12_VIDEO_ENCODER_CODEC_HEVC || c == D3D12_VIDEO_ENCODER_CODEC_AV1; }

struct VEnc;
struct VEncHeap;
struct VencExtras;
/* The encoder queries of HEVC and AV1 (and FRAME_SUBREGION_LAYOUT_CONFIG of every codec); S_FALSE: one encode.inc answers. */
static HRESULT vencx_feature(Dev *d, D3D12_FEATURE_VIDEO feature, void *data, UINT size);
/* CreateVideoEncoder / CreateVideoEncoderHeap(1) of HEVC and AV1. */
static HRESULT vencx_make(Dev *d, const D3D12_VIDEO_ENCODER_DESC *desc, REFIID riid, void **out);
static HRESULT vencx_heap_make(Dev *d, const D3D12_VIDEO_ENCODER_HEAP_DESC1 *desc, REFIID riid, void **out);
/* GetCodecProfile / GetCodecConfiguration / GetCodecLevel of an HEVC or AV1 encoder or heap. */
static HRESULT vencx_get_profile(D3D12_VIDEO_ENCODER_CODEC codec, const VEncCodecData *x, D3D12_VIDEO_ENCODER_PROFILE_DESC dst);
static HRESULT vencx_get_config(D3D12_VIDEO_ENCODER_CODEC codec, const VEncCodecData *x, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION dst);
static HRESULT vencx_get_level(D3D12_VIDEO_ENCODER_CODEC codec, const VEncCodecData *x, D3D12_VIDEO_ENCODER_LEVEL_SETTING dst);
/* EncodeFrame / EncodeFrame1 of an HEVC or AV1 encoder (venc_record's arguments: `p` is the picture control's codec data). */
static void vencx_record(CmdList *l, struct VEnc *e, struct VEncHeap *h, const D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC *seq, D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAGS pflags,
                         UINT codec_data_size, const void *p, const D3D12_VIDEO_ENCODE_REFERENCE_FRAMES *rf, Resource *input, UINT input_sub, Resource *bits, UINT64 bits_offset,
                         Resource *recon, UINT recon_sub, Resource *meta, UINT64 meta_offset, const struct VencExtras *x, UINT errors);
