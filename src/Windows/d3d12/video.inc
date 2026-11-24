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

/* d3d12.dll: video (ID3D12VideoDevice .. 3, decoders and their heaps, video processors, the decode and process command lists). Included by d3d12.c.
 *
 * Decoding is the kernel's (src/Kernel/Dxgkrnl: a video decoder made by an escape, the stream's DecodeFrame; Hardware/VideoCodec/Dxva decodes the DXVA buffers on the CPU
 * into the planes of the output). A decoder heap is the kernel's decoder (what it keeps of earlier pictures is the heap's state, as Direct3D 12 says); the decoder object
 * names the profile. Every picture is known by its texture (the kernel's allocation of its luma plane), so the reference list may change from picture to picture.
 * Video processing is the stream's VideoProcess (Hardware/VideoCodec/VideoProcess.hpp, on the CPU): colour space and format conversion, scaling, the eight orientations,
 * alpha blending and fill, deinterlacing (bob, and motion adaptive with a past and a future frame: CUSTOM), the ProcAmp, noise reduction, edge enhancement and
 * anamorphic scaling filters, luma keying, stereo (packed and separate pictures, two outputs), PQ and HLG with tone mapping; no auto processing and no stereo adjustment
 * filter (the feature queries say so).
 * Video encoding and motion estimation are encode.inc's (included after this file); this file hands their feature queries and creators over.
 * Not here: extension commands (none are enumerated), protected sessions, decode histograms and decode-time conversion (reported as not supported), timestamps on video lists. */

#include "video_extra.h"

/* ---- the profiles ------------------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct VideoProfile { const GUID *guid; UINT kernel; DXGI_FORMAT formats[4]; UINT nformats; } VideoProfile;
/* (MPEG-4 Part 2 Advanced Simple with GMC: d3d12video.h names no such profile, but it is DXVA's - D3D11_DECODER_PROFILE_MPEG4PT2_VLD_ADVSIMPLE_GMC,
 * DXVA_ModeMPEG4pt2_VLD_AdvSimple_GMC - and Direct3D 11's decoders of it are made on this one) */
static const GUID g_video_profile_mpeg4pt2_advsimple_gmc = {0xab998b5b, 0x4258, 0x44a9, {0x9f, 0xeb, 0x94, 0xe5, 0x97, 0xa6, 0xba, 0xae}};
/* (`kernel` is hardware::DxvaProfile) */
static const VideoProfile g_video_profiles[] = {
    {&D3D12_VIDEO_DECODE_PROFILE_H264, 0, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_MPEG2, 1, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_MPEG1_AND_MPEG2, 2, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_VP9, 3, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_VP9_10BIT_PROFILE2, 4, {DXGI_FORMAT_P010}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN, 5, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0, 6, {DXGI_FORMAT_NV12, DXGI_FORMAT_P010}, 2},
    {&D3D12_VIDEO_DECODE_PROFILE_VC1, 7, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_VC1_D2010, 8, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_MPEG4PT2_SIMPLE, 9, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_MPEG4PT2_ADVSIMPLE_NOGMC, 10, {DXGI_FORMAT_NV12}, 1},
    {&g_video_profile_mpeg4pt2_advsimple_gmc, 11, {DXGI_FORMAT_NV12}, 1},
    /* (AV1 High: 4:4:4 of 8 and 10 bits; Professional: 4:2:2 of 8 and 10 bits and monochrome, and of 12 bits 4:2:2 / 4:4:4 or 4:2:0 / monochrome - the formats Windows'
     * decoders write them in) */
    {&D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE1, 12, {DXGI_FORMAT_AYUV, DXGI_FORMAT_Y410}, 2},
    {&D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE2, 13, {DXGI_FORMAT_YUY2, DXGI_FORMAT_Y210, DXGI_FORMAT_NV12, DXGI_FORMAT_P010}, 4},
    {&D3D12_VIDEO_DECODE_PROFILE_AV1_12BIT_PROFILE2, 14, {DXGI_FORMAT_Y216, DXGI_FORMAT_Y416}, 2},
    {&D3D12_VIDEO_DECODE_PROFILE_AV1_12BIT_PROFILE2_420, 15, {DXGI_FORMAT_P016}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10, 16, {DXGI_FORMAT_P010}, 1},
};
/* The kernel's surface format of a decoder's output (hardware::DxvaSurfaceFormat; DxgTypes.hpp VideoDecoderDesc::surface_format). */
static UINT video_surface_format(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_P010: return 1;
    case DXGI_FORMAT_P016: return 2;
    case DXGI_FORMAT_AYUV: return 3;
    case DXGI_FORMAT_Y410: return 4;
    case DXGI_FORMAT_Y416: return 5;
    case DXGI_FORMAT_YUY2: return 6;
    case DXGI_FORMAT_Y210: return 7;
    case DXGI_FORMAT_Y216: return 8;
    default: return 0;      /* NV12 */
  }
}
/* The bytes of a pixel of a decoder's output, both planes (for the heap size estimate). */
static UINT64 video_surface_bytes(DXGI_FORMAT f, UINT64 pixels) {
  switch (f) {
    case DXGI_FORMAT_NV12: return pixels * 3 / 2;
    case DXGI_FORMAT_P010: case DXGI_FORMAT_P016: return pixels * 3;
    case DXGI_FORMAT_YUY2: case DXGI_FORMAT_Y410: case DXGI_FORMAT_AYUV: return pixels * (f == DXGI_FORMAT_YUY2 ? 2 : 4);
    case DXGI_FORMAT_Y210: case DXGI_FORMAT_Y216: return pixels * 4;
    default: return pixels * 8;      /* Y416 */
  }
}
#define VIDEO_PROFILES (sizeof g_video_profiles / sizeof g_video_profiles[0])
enum { ESC_CREATE_VIDEO_DECODER = 15, ESC_DESTROY_VIDEO_DECODER = 16, ESC_QUERY_VIDEO_DECODE_SUPPORT = 17 };

static const VideoProfile *video_profile(const GUID *g) {
  for (UINT i = 0; g && i < VIDEO_PROFILES; ++i)
    if (guid_eq(g, g_video_profiles[i].guid)) return &g_video_profiles[i];
  return NULL;
}
/* The interlace types a profile decodes: progressive, and field-based content (field pictures and MBAFF frames) for H.264, as Windows' H.264 drivers report. */
static int video_interlace_ok(const VideoProfile *p, D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE t) {
  return t == D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE || (t == D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_FIELD_BASED && p && p->kernel == 0);
}
/* Whether the kernel decodes the configuration into `format` at that size (not encrypted). */
static int video_decode_supported(Dev *d, const D3D12_VIDEO_DECODE_CONFIGURATION *c, DXGI_FORMAT format, UINT width, UINT height) {
  const VideoProfile *p = video_profile(&c->DecodeProfile);
  if (!p || c->BitstreamEncryption != D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE || !video_interlace_ok(p, c->InterlaceType)) return 0;
  int known = 0;
  for (UINT i = 0; i < p->nformats; ++i) known |= p->formats[i] == format;
  if (!known || width == 0 || height == 0) return 0;
  UINT request[5] = {ESC_QUERY_VIDEO_DECODE_SUPPORT, p->kernel, video_surface_format(format), width, height}, reply[1] = {0};
  return KMT_OK(dev_escape(d, request, 5, reply, 1)) && reply[0] == 1;
}

/* ---- the video processor's formats and colour spaces (Hardware/VideoCodec/VideoProcess.hpp) ----------------------------------------------------------------------- */

enum { VP_NV12, VP_P010, VP_P016, VP_BGRA8, VP_BGRX8, VP_RGBA8, VP_RGB10A2, VP_RGBA16F, VP_AYUV, VP_Y410, VP_Y416, VP_YUY2, VP_Y210, VP_Y216, VP_P208 };
static int vp_yuv(int v) { return v == VP_NV12 || v == VP_P010 || v == VP_P016 || v >= VP_AYUV; }
static int vp_format(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_NV12: return VP_NV12;
    case DXGI_FORMAT_P010: return VP_P010;
    case DXGI_FORMAT_P016: return VP_P016;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return VP_BGRA8;
    case DXGI_FORMAT_B8G8R8X8_UNORM: return VP_BGRX8;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return VP_RGBA8;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return VP_RGB10A2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VP_RGBA16F;
    case DXGI_FORMAT_AYUV: return VP_AYUV;
    case DXGI_FORMAT_Y410: return VP_Y410;
    case DXGI_FORMAT_Y416: return VP_Y416;
    case DXGI_FORMAT_YUY2: return VP_YUY2;
    case DXGI_FORMAT_Y210: return VP_Y210;
    case DXGI_FORMAT_Y216: return VP_Y216;
    case DXGI_FORMAT_P208: return VP_P208;
    default: return -1;
  }
}
/* (gamma 2.2 and 2.4, PQ and HLG: YCbCr for the YUV formats, RGB for the others - what vp_color_space_supported takes) */
static int vp_space_ok(DXGI_FORMAT f, DXGI_COLOR_SPACE_TYPE s) {
  const int v = vp_format(f);
  if (v < 0) return 0;
  if (vp_yuv(v))
    return s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 || s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 ||
           s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709 || s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020 ||
           s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709 ||
           s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 ||
           s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020 || s == DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020;
  return s == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 || s == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 || s == DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 ||
         s == DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020 || s == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020 || s == DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709 ||
         s == DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020 || s == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || s == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}
#define VP_MAX_STREAMS 16      /* (the kernel's kMaxVideoProcessStreams) */
#define VP_FEATURES (D3D12_VIDEO_PROCESS_FEATURE_FLAG_ALPHA_FILL | D3D12_VIDEO_PROCESS_FEATURE_FLAG_ROTATION | D3D12_VIDEO_PROCESS_FEATURE_FLAG_FLIP | D3D12_VIDEO_PROCESS_FEATURE_FLAG_ALPHA_BLENDING | \
                     D3D12_VIDEO_PROCESS_FEATURE_FLAG_LUMA_KEY | D3D12_VIDEO_PROCESS_FEATURE_FLAG_STEREO)
#define VP_DEINTERLACE (D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_BOB | D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_CUSTOM)
/* The filters (all but stereo adjustment) and their ranges, in DXVA-HD's units once multiplied: brightness -100..100, contrast 0..10 (1), hue -180..180 degrees,
 * saturation 0..10 (1), noise reduction, edge enhancement and anamorphic scaling 0..1 (strength); the multipliers are binary fractions, as Direct3D asks. */
#define VP_FILTERS 0x7Fu
static const D3D12_VIDEO_PROCESS_FILTER_RANGE g_vp_filter_ranges[7] = {
    {-400, 400, 0, 0.25f}, {0, 640, 64, 1.0f / 64}, {-720, 720, 0, 0.25f}, {0, 640, 64, 1.0f / 64}, {0, 64, 0, 1.0f / 64}, {0, 64, 0, 1.0f / 64}, {0, 64, 0, 1.0f / 64}};
/* (the past and future frames the motion adaptive deinterlacer takes) */
#define VP_PAST_FRAMES 1
#define VP_FUTURE_FRAMES 1

static int vp_input_ok(const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *in) {
  return vp_space_ok(in->Format, in->ColorSpace) && (UINT)in->FieldType <= D3D12_VIDEO_FIELD_TYPE_INTERLACED_BOTTOM_FIELD_FIRST &&
         (UINT)in->StereoFormat <= D3D12_VIDEO_FRAME_STEREO_FORMAT_SEPARATE &&
         (in->DeinterlaceMode == D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE || in->DeinterlaceMode == D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_BOB ||
          in->DeinterlaceMode == D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_CUSTOM) &&
         !(in->FilterFlags & ~VP_FILTERS) && (!in->LumaKey.Enable || (in->LumaKey.Lower >= 0.0f && in->LumaKey.Lower <= in->LumaKey.Upper && in->LumaKey.Upper <= 1.0f)) &&
         in->NumPastFrames <= 16 && in->NumFutureFrames <= 16 && !in->EnableAutoProcessing;
}
static int vp_output_ok(const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *out) {
  return vp_space_ok(out->Format, out->ColorSpace) && (UINT)out->AlphaFillMode <= D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_SOURCE_STREAM;
}

/* ---- decoders, decoder heaps, processors ------------------------------------------------------------------------------------------------------------------------- */

typedef struct VDec { DObj d; D3D12_VIDEO_DECODER_DESC desc; } VDec;
typedef struct VDHeap { DObj d; D3D12_VIDEO_DECODER_HEAP_DESC desc; UINT kdec; } VDHeap;
typedef struct VProc {
  DObj d;
  UINT node_mask;
  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC out;
  D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC in[VP_MAX_STREAMS];
  UINT nin;
} VProc;
static const GUID *const g_vdec_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12VideoDecoder, &IID_ID3D12VideoDecoder1, NULL};
static const GUID *const g_vdheap_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12VideoDecoderHeap, &IID_ID3D12VideoDecoderHeap1, NULL};
static const GUID *const g_vproc_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12VideoProcessor, &IID_ID3D12VideoProcessor1, NULL};

static void vdheap_destroy(DxObj *o) {
  VDHeap *h = (VDHeap *)o;
  if (h->kdec) { UINT request[2] = {ESC_DESTROY_VIDEO_DECODER, h->kdec}; dev_escape(h->d.dev, request, 2, NULL, 0); }
  dobj_child_destroy(o);
}

static D3D12_VIDEO_DECODER_DESC *STDMETHODCALLTYPE vdec_GetDesc(VDec *This, D3D12_VIDEO_DECODER_DESC *ret) { *ret = This->desc; return ret; }
static D3D12_VIDEO_DECODER_HEAP_DESC *STDMETHODCALLTYPE vdheap_GetDesc(VDHeap *This, D3D12_VIDEO_DECODER_HEAP_DESC *ret) { *ret = This->desc; return ret; }
/* (made without a protected session: there is none to give - as ID3D12Resource1's) */
static HRESULT STDMETHODCALLTYPE vdec_GetProtectedResourceSession(VDec *This, REFIID riid, void **out) { if (!out) return E_INVALIDARG; *out = NULL; return DXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE vdheap_GetProtectedResourceSession(VDHeap *This, REFIID riid, void **out) { if (!out) return E_INVALIDARG; *out = NULL; return DXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE vproc_GetProtectedResourceSession(VProc *This, REFIID riid, void **out) { if (!out) return E_INVALIDARG; *out = NULL; return DXGI_ERROR_NOT_FOUND; }
static UINT STDMETHODCALLTYPE vproc_GetNodeMask(VProc *This) { return This->node_mask; }
static UINT STDMETHODCALLTYPE vproc_GetNumInputStreamDescs(VProc *This) { return This->nin; }
static HRESULT STDMETHODCALLTYPE vproc_GetInputStreamDescs(VProc *This, UINT count, D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *descs) {
  if (!descs || count != This->nin) return E_INVALIDARG;
  memcpy(descs, This->in, sizeof *descs * count);
  return S_OK;
}
static D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *STDMETHODCALLTYPE vproc_GetOutputStreamDesc(VProc *This, D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *ret) { *ret = This->out; return ret; }

/* ---- ID3D12VideoDevice .. 3 -------------------------------------------------------------------------------------------------------------------------------------- */

static Dev *dev_of_video(ID3D12VideoDevice3 *v) { return (Dev *)((BYTE *)v - offsetof(Dev, video)); }

/* The device answers the video device's interfaces with its second face. */
static HRESULT STDMETHODCALLTYPE dev_QueryInterface(Dev *This, REFIID riid, void **out) {
  if (out && riid && (guid_eq(riid, &IID_ID3D12VideoDevice) || guid_eq(riid, &IID_ID3D12VideoDevice1) || guid_eq(riid, &IID_ID3D12VideoDevice2) || guid_eq(riid, &IID_ID3D12VideoDevice3))) {
    dx_AddRef(OBJ(This));
    *out = &This->video;
    return S_OK;
  }
  return dx_QueryInterface(OBJ(This), riid, out);
}
static HRESULT STDMETHODCALLTYPE vdev_QueryInterface(ID3D12VideoDevice3 *v, REFIID riid, void **out) { return dev_QueryInterface(dev_of_video(v), riid, out); }
static ULONG STDMETHODCALLTYPE vdev_AddRef(ID3D12VideoDevice3 *v) { return dx_AddRef(OBJ(dev_of_video(v))); }
static ULONG STDMETHODCALLTYPE vdev_Release(ID3D12VideoDevice3 *v) { return dx_Release(OBJ(dev_of_video(v))); }

/* (encode.inc) */
static HRESULT venc_feature(Dev *d, D3D12_FEATURE_VIDEO feature, void *data, UINT size);
static HRESULT vme_make(Dev *d, const D3D12_VIDEO_MOTION_ESTIMATOR_DESC *desc, ID3D12ProtectedResourceSession *session, REFIID riid, void **out);
static HRESULT vmvh_make(Dev *d, const D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC *desc, ID3D12ProtectedResourceSession *session, REFIID riid, void **out);
static HRESULT venc_make(Dev *d, const D3D12_VIDEO_ENCODER_DESC *desc, REFIID riid, void **out);
static HRESULT veheap_make(Dev *d, const D3D12_VIDEO_ENCODER_HEAP_DESC *desc, REFIID riid, void **out);

#define VIDEO_FEATURE(type) do { if (size != sizeof(type)) return E_INVALIDARG; } while (0)
static HRESULT STDMETHODCALLTYPE vdev_CheckFeatureSupport(ID3D12VideoDevice3 *v, D3D12_FEATURE_VIDEO feature, void *data, UINT size) {
  Dev *d = dev_of_video(v);
  if (!data) return E_INVALIDARG;
  switch (feature) {
    case D3D12_FEATURE_VIDEO_DECODE_SUPPORT: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      const int ok = video_decode_supported(d, &f->Configuration, f->DecodeFormat, f->Width, f->Height);
      f->SupportFlags = ok ? D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED : D3D12_VIDEO_DECODE_SUPPORT_FLAG_NONE;
      /* (field-based content is coded in macroblock pairs: its surfaces are 32 lines aligned) */
      f->ConfigurationFlags = ok && f->Configuration.InterlaceType == D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_FIELD_BASED ? D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_HEIGHT_ALIGNMENT_MULTIPLE_32_REQUIRED
                                                                                                                       : D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_NONE;
      /* (tier 2: a reference may be a texture of its own or a slice of an array, each picture is known by its texture) */
      f->DecodeTier = ok ? D3D12_VIDEO_DECODE_TIER_2 : D3D12_VIDEO_DECODE_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODE_PROFILE_COUNT: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT);
      D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->ProfileCount = VIDEO_PROFILES;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODE_PROFILES: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES);
      D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES *f = data;
      if (f->NodeIndex || f->ProfileCount != VIDEO_PROFILES || !f->pProfiles) return E_INVALIDARG;
      for (UINT i = 0; i < VIDEO_PROFILES; ++i) f->pProfiles[i] = *g_video_profiles[i].guid;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODE_FORMAT_COUNT: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODE_FORMAT_COUNT);
      D3D12_FEATURE_DATA_VIDEO_DECODE_FORMAT_COUNT *f = data;
      const VideoProfile *p = video_profile(&f->Configuration.DecodeProfile);
      if (f->NodeIndex || !p) return E_INVALIDARG;
      f->FormatCount = f->Configuration.BitstreamEncryption == D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE && video_interlace_ok(p, f->Configuration.InterlaceType) ? p->nformats : 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODE_FORMATS: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS);
      D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS *f = data;
      const VideoProfile *p = video_profile(&f->Configuration.DecodeProfile);
      const UINT n = p && f->Configuration.BitstreamEncryption == D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE && video_interlace_ok(p, f->Configuration.InterlaceType) ? p->nformats : 0;
      if (f->NodeIndex || !p || f->FormatCount != n || (n && !f->pOutputFormats)) return E_INVALIDARG;
      for (UINT i = 0; i < n; ++i) f->pOutputFormats[i] = p->formats[i];
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODE_CONVERSION_SUPPORT: {
      /* (no conversion while decoding: the video processor converts afterwards) */
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODE_CONVERSION_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_DECODE_CONVERSION_SUPPORT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->SupportFlags = D3D12_VIDEO_DECODE_CONVERSION_SUPPORT_FLAG_NONE;
      memset(&f->ScaleSupport, 0, sizeof f->ScaleSupport);
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODE_HISTOGRAM: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODE_HISTOGRAM);
      D3D12_FEATURE_DATA_VIDEO_DECODE_HISTOGRAM *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->Components = D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_NONE;
      f->BinCount = f->CounterBitDepth = 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODER_HEAP_SIZE: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE);
      D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE *f = data;
      const D3D12_VIDEO_DECODER_HEAP_DESC *h = &f->VideoDecoderHeapDesc;
      if (!video_decode_supported(d, &h->Configuration, h->Format, h->DecodeWidth, h->DecodeHeight)) return E_INVALIDARG;
      /* (the kernel keeps the pictures the decoder needs: about a frame for each picture of the buffer, in system memory) */
      f->MemoryPoolL0Size = 0;
      f->MemoryPoolL1Size = video_surface_bytes(h->Format, (UINT64)h->DecodeWidth * h->DecodeHeight) * (h->MaxDecodePictureBufferCount + 1);
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODER_HEAP_SIZE1: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE1);
      D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE1 *f = data;
      const D3D12_VIDEO_DECODER_HEAP_DESC *h = &f->VideoDecoderHeapDesc;
      if (f->Protected || !video_decode_supported(d, &h->Configuration, h->Format, h->DecodeWidth, h->DecodeHeight)) return E_INVALIDARG;
      f->MemoryPoolL0Size = 0;
      f->MemoryPoolL1Size = video_surface_bytes(h->Format, (UINT64)h->DecodeWidth * h->DecodeHeight) * (h->MaxDecodePictureBufferCount + 1);
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_PROCESS_SUPPORT: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      const int ok = vp_space_ok(f->InputSample.Format.Format, f->InputSample.Format.ColorSpace) && vp_space_ok(f->OutputFormat.Format, f->OutputFormat.ColorSpace) &&
                     f->InputSample.Width && f->InputSample.Height && f->InputSample.Width <= KMAX_TEXTURE_DIMENSION && f->InputSample.Height <= KMAX_TEXTURE_DIMENSION &&
                     (UINT)f->InputFieldType <= D3D12_VIDEO_FIELD_TYPE_INTERLACED_BOTTOM_FIELD_FIRST && (UINT)f->InputStereoFormat <= D3D12_VIDEO_FRAME_STEREO_FORMAT_SEPARATE &&
                     (f->OutputStereoFormat == D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE || f->OutputStereoFormat == D3D12_VIDEO_FRAME_STEREO_FORMAT_MONO ||
                      f->OutputStereoFormat == D3D12_VIDEO_FRAME_STEREO_FORMAT_SEPARATE);
      f->SupportFlags = ok ? D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED : D3D12_VIDEO_PROCESS_SUPPORT_FLAG_NONE;
      memset(&f->ScaleSupport, 0, sizeof f->ScaleSupport);
      if (ok) {
        f->ScaleSupport.OutputSizeRange.MaxWidth = f->ScaleSupport.OutputSizeRange.MaxHeight = KMAX_TEXTURE_DIMENSION;
        f->ScaleSupport.OutputSizeRange.MinWidth = f->ScaleSupport.OutputSizeRange.MinHeight = 1;
      }
      f->FeatureSupport = ok ? VP_FEATURES : D3D12_VIDEO_PROCESS_FEATURE_FLAG_NONE;
      f->DeinterlaceSupport = ok && f->InputFieldType != D3D12_VIDEO_FIELD_TYPE_NONE ? (D3D12_VIDEO_PROCESS_DEINTERLACE_FLAGS)VP_DEINTERLACE : D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE;
      f->AutoProcessingSupport = D3D12_VIDEO_PROCESS_AUTO_PROCESSING_FLAG_NONE;
      f->FilterSupport = ok ? (D3D12_VIDEO_PROCESS_FILTER_FLAGS)VP_FILTERS : D3D12_VIDEO_PROCESS_FILTER_FLAG_NONE;
      memset(f->FilterRangeSupport, 0, sizeof f->FilterRangeSupport);
      for (UINT i = 0; ok && i < 7; ++i) f->FilterRangeSupport[i] = g_vp_filter_ranges[i];
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_PROCESS_MAX_INPUT_STREAMS: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_PROCESS_MAX_INPUT_STREAMS);
      D3D12_FEATURE_DATA_VIDEO_PROCESS_MAX_INPUT_STREAMS *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->MaxInputStreams = VP_MAX_STREAMS;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_PROCESS_REFERENCE_INFO: {
      /* (the motion adaptive deinterlacer takes a frame before and one after; bob and the filters none: they are spatial) */
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_PROCESS_REFERENCE_INFO);
      D3D12_FEATURE_DATA_VIDEO_PROCESS_REFERENCE_INFO *f = data;
      if (f->NodeIndex || (f->DeinterlaceMode & ~VP_DEINTERLACE) || (f->Filters & ~VP_FILTERS) || (f->FeatureSupport & ~VP_FEATURES) || f->EnableAutoProcessing) return E_INVALIDARG;
      const int adaptive = (f->DeinterlaceMode & D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_CUSTOM) != 0;
      f->PastFrames = adaptive ? VP_PAST_FRAMES : 0;
      f->FutureFrames = adaptive ? VP_FUTURE_FRAMES : 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_PROCESSOR_SIZE: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE);
      D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE *f = data;
      if (!f->pOutputStreamDesc || (f->NumInputStreamDescs && !f->pInputStreamDescs)) return E_INVALIDARG;
      f->MemoryPoolL0Size = f->MemoryPoolL1Size = 0;      /* (it works in the memory of the pictures it is given) */
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_PROCESSOR_SIZE1: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE1);
      D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE1 *f = data;
      if (f->Protected || !f->pOutputStreamDesc || (f->NumInputStreamDescs && !f->pInputStreamDescs)) return E_INVALIDARG;
      f->MemoryPoolL0Size = f->MemoryPoolL1Size = 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ARCHITECTURE: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_ARCHITECTURE);
      ((D3D12_FEATURE_DATA_VIDEO_ARCHITECTURE *)data)->IOCoherent = TRUE;      /* (the CPU decodes into the memory the GPU reads) */
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_FEATURE_AREA_SUPPORT: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_FEATURE_AREA_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_FEATURE_AREA_SUPPORT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->VideoDecodeSupport = TRUE;
      f->VideoProcessSupport = TRUE;
      f->VideoEncodeSupport = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_EXTENSION_COMMAND_COUNT: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_EXTENSION_COMMAND_COUNT);
      D3D12_FEATURE_DATA_VIDEO_EXTENSION_COMMAND_COUNT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->CommandCount = 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODE_PROTECTED_RESOURCES: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODE_PROTECTED_RESOURCES);
      ((D3D12_FEATURE_DATA_VIDEO_DECODE_PROTECTED_RESOURCES *)data)->SupportFlags = D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAG_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_PROCESS_PROTECTED_RESOURCES: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_PROCESS_PROTECTED_RESOURCES);
      ((D3D12_FEATURE_DATA_VIDEO_PROCESS_PROTECTED_RESOURCES *)data)->SupportFlags = D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAG_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_PROTECTED_RESOURCES: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_PROTECTED_RESOURCES);
      ((D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_PROTECTED_RESOURCES *)data)->SupportFlags = D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAG_NONE;
      return S_OK;
    }
    default: {
      /* (the motion estimator's and the encoder's: encode.inc) */
      const HRESULT hr = venc_feature(d, feature, data, size);
      return hr == S_FALSE ? E_INVALIDARG : hr;
    }
  }
}
#undef VIDEO_FEATURE

static HRESULT vdec_make(Dev *d, const D3D12_VIDEO_DECODER_DESC *desc, ID3D12ProtectedResourceSession *session, REFIID riid, void **out) {
  extern const ID3D12VideoDecoder1Vtbl vdec_vtbl;
  if (out) *out = NULL;
  if (!desc || !out || desc->NodeMask > 1 || session) return E_INVALIDARG;
  const VideoProfile *p = video_profile(&desc->Configuration.DecodeProfile);
  if (!p || desc->Configuration.BitstreamEncryption != D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE || !video_interlace_ok(p, desc->Configuration.InterlaceType))
    return E_INVALIDARG;
  VDec *v = dobj_new(sizeof *v, d, &vdec_vtbl, g_vdec_iids, dobj_child_destroy);
  if (!v) return E_OUTOFMEMORY;
  v->desc = *desc;
  return query_out((IUnknown *)v, riid, out);
}
static HRESULT vdheap_make(Dev *d, const D3D12_VIDEO_DECODER_HEAP_DESC *desc, ID3D12ProtectedResourceSession *session, REFIID riid, void **out) {
  extern const ID3D12VideoDecoderHeap1Vtbl vdheap_vtbl;
  if (out) *out = NULL;
  if (!desc || !out || desc->NodeMask > 1 || session) return E_INVALIDARG;
  if (!video_decode_supported(d, &desc->Configuration, desc->Format, desc->DecodeWidth, desc->DecodeHeight)) return E_INVALIDARG;
  VDHeap *h = dobj_new(sizeof *h, d, &vdheap_vtbl, g_vdheap_iids, vdheap_destroy);
  if (!h) return E_OUTOFMEMORY;
  h->desc = *desc;
  /* (the indices the pictures may name: the buffer of pictures and the current one, at least what the codecs' references reach) */
  UINT surfaces = desc->MaxDecodePictureBufferCount + 1;
  if (surfaces < 17) surfaces = 17;
  if (surfaces > 128) surfaces = 128;
  const VideoProfile *p = video_profile(&desc->Configuration.DecodeProfile);
  UINT request[6] = {ESC_CREATE_VIDEO_DECODER, p->kernel, desc->DecodeWidth, desc->DecodeHeight, surfaces, video_surface_format(desc->Format)}, reply[1] = {0};
  const NTSTATUS st = dev_escape(d, request, 6, reply, 1);
  if (!KMT_OK(st)) { dx_Release(OBJ(h)); return hr_from(st); }
  h->kdec = reply[0];
  return query_out((IUnknown *)h, riid, out);
}
static HRESULT vproc_make(Dev *d, UINT mask, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *outd, UINT count, const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *ins,
                          ID3D12ProtectedResourceSession *session, REFIID riid, void **out) {
  extern const ID3D12VideoProcessor1Vtbl vproc_vtbl;
  if (out) *out = NULL;
  if (!outd || !out || mask > 1 || session || count == 0 || count > VP_MAX_STREAMS || !ins || !vp_output_ok(outd)) return E_INVALIDARG;
  if (outd->AlphaFillMode == D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_SOURCE_STREAM && outd->AlphaFillModeSourceStreamIndex >= count) return E_INVALIDARG;
  for (UINT i = 0; i < count; ++i)
    if (!vp_input_ok(&ins[i])) return E_INVALIDARG;
  VProc *v = dobj_new(sizeof *v, d, &vproc_vtbl, g_vproc_iids, dobj_child_destroy);
  if (!v) return E_OUTOFMEMORY;
  v->node_mask = mask;
  v->out = *outd;
  memcpy(v->in, ins, sizeof *ins * count);
  v->nin = count;
  return query_out((IUnknown *)v, riid, out);
}
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoDecoder(ID3D12VideoDevice3 *v, const D3D12_VIDEO_DECODER_DESC *desc, REFIID riid, void **out) { return vdec_make(dev_of_video(v), desc, NULL, riid, out); }
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoDecoder1(ID3D12VideoDevice3 *v, const D3D12_VIDEO_DECODER_DESC *desc, ID3D12ProtectedResourceSession *s, REFIID riid, void **out) {
  return vdec_make(dev_of_video(v), desc, s, riid, out);
}
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoDecoderHeap(ID3D12VideoDevice3 *v, const D3D12_VIDEO_DECODER_HEAP_DESC *desc, REFIID riid, void **out) { return vdheap_make(dev_of_video(v), desc, NULL, riid, out); }
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoDecoderHeap1(ID3D12VideoDevice3 *v, const D3D12_VIDEO_DECODER_HEAP_DESC *desc, ID3D12ProtectedResourceSession *s, REFIID riid, void **out) {
  return vdheap_make(dev_of_video(v), desc, s, riid, out);
}
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoProcessor(ID3D12VideoDevice3 *v, UINT mask, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *o, UINT n, const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *in,
                                                          REFIID riid, void **out) {
  return vproc_make(dev_of_video(v), mask, o, n, in, NULL, riid, out);
}
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoProcessor1(ID3D12VideoDevice3 *v, UINT mask, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *o, UINT n, const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *in,
                                                           ID3D12ProtectedResourceSession *s, REFIID riid, void **out) {
  return vproc_make(dev_of_video(v), mask, o, n, in, s, riid, out);
}
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoMotionEstimator(ID3D12VideoDevice3 *v, const D3D12_VIDEO_MOTION_ESTIMATOR_DESC *desc, ID3D12ProtectedResourceSession *s, REFIID riid, void **out) {
  return vme_make(dev_of_video(v), desc, s, riid, out);
}
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoMotionVectorHeap(ID3D12VideoDevice3 *v, const D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC *desc, ID3D12ProtectedResourceSession *s, REFIID riid, void **out) {
  return vmvh_make(dev_of_video(v), desc, s, riid, out);
}
/* (no extension commands: the feature queries say none is there) */
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoExtensionCommand(ID3D12VideoDevice3 *v, const D3D12_VIDEO_EXTENSION_COMMAND_DESC *desc, const void *params, SIZE_T size,
                                                                  ID3D12ProtectedResourceSession *s, REFIID riid, void **out) {
  if (out) *out = NULL;
  return E_INVALIDARG;      /* (no command id is one the device has) */
}
static HRESULT STDMETHODCALLTYPE vdev_ExecuteExtensionCommand(ID3D12VideoDevice3 *v, ID3D12VideoExtensionCommand *cmd, const void *params, SIZE_T size, void *output, SIZE_T output_size) {
  return E_INVALIDARG;      /* (no extension command can have been made) */
}
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoEncoder(ID3D12VideoDevice3 *v, const D3D12_VIDEO_ENCODER_DESC *desc, REFIID riid, void **out) { return venc_make(dev_of_video(v), desc, riid, out); }
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoEncoderHeap(ID3D12VideoDevice3 *v, const D3D12_VIDEO_ENCODER_HEAP_DESC *desc, REFIID riid, void **out) { return veheap_make(dev_of_video(v), desc, riid, out); }

/* ---- the video command lists ------------------------------------------------------------------------------------------------------------------------------------- */

static const GUID *const g_vdlist_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12CommandList, &IID_ID3D12VideoDecodeCommandList, &IID_ID3D12VideoDecodeCommandList1,
                                            &IID_ID3D12VideoDecodeCommandList2, &IID_ID3D12VideoDecodeCommandList3, NULL};
static const GUID *const g_vplist_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12CommandList, &IID_ID3D12VideoProcessCommandList, &IID_ID3D12VideoProcessCommandList1,
                                            &IID_ID3D12VideoProcessCommandList2, &IID_ID3D12VideoProcessCommandList3, NULL};
static const GUID *const g_velist_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12CommandList, &IID_ID3D12VideoEncodeCommandList, &IID_ID3D12VideoEncodeCommandList1,
                                            &IID_ID3D12VideoEncodeCommandList2, &IID_ID3D12VideoEncodeCommandList3, NULL};

static HRESULT video_list_make(Dev *This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out) {
  extern const ID3D12VideoDecodeCommandList3Vtbl vdlist_vtbl;
  extern const ID3D12VideoProcessCommandList3Vtbl vplist_vtbl;
  extern const ID3D12VideoEncodeCommandList3Vtbl velist_vtbl;
  if (out) *out = NULL;
  const int decode = type == D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, encode = type == D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
  CmdList *l = dobj_new(sizeof *l, This, decode ? (const void *)&vdlist_vtbl : encode ? (const void *)&velist_vtbl : (const void *)&vplist_vtbl,
                        decode ? g_vdlist_iids : encode ? g_velist_iids : g_vplist_iids, list_destroy);
  if (!l) return E_OUTOFMEMORY;
  l->type = type;
  list_reset_state(l);
  if (!chunk_new(l)) { dx_Release(OBJ(l)); return E_OUTOFMEMORY; }
  return query_out((IUnknown *)l, riid, out);
}
static HRESULT STDMETHODCALLTYPE vlist_Reset(CmdList *This, ID3D12CommandAllocator *allocator) { return list_Reset(This, allocator, NULL); }
static void STDMETHODCALLTYPE vlist_ClearState(CmdList *This) { (void)This; }      /* (a video list sets no state) */
static void STDMETHODCALLTYPE vlist_BeginQuery(CmdList *l, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
  (void)l; (void)heap; (void)type; (void)index;      /* (decode statistics have no beginning: EndQuery takes them; nothing else is counted on a video queue here) */
}
/* The statistics of the last DecodeFrame recorded go to the query: its status words (still in the stream) are pointed at the query heap. */
static void STDMETHODCALLTYPE vlist_EndQuery(CmdList *l, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
  QHeap *h = (QHeap *)heap;
  if (!h || index >= h->desc.Count || type != D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS || h->desc.Type != D3D12_QUERY_HEAP_TYPE_VIDEO_DECODE_STATISTICS) return;
  if (!l->last_decode_chunk || l->last_decode_chunk > l->nchunks) return;
  Chunk *c = l->chunks[l->last_decode_chunk - 1];
  UINT slot = c->nalloc;
  for (UINT i = 0; i < c->nalloc; ++i)
    if (c->allocs[i] == h->alloc) { slot = i; break; }
  if (slot == c->nalloc) {
    if (c->nalloc >= CHUNK_ALLOCS) return;
    c->allocs[c->nalloc++] = h->alloc;
  }
  const UINT words[2] = {slot, index * 32};
  memcpy(c->bytes + l->last_decode_at, words, sizeof words);
  keep(l, (IUnknown *)heap);
}
static void STDMETHODCALLTYPE vlist_ResolveQueryData(CmdList *l, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource *dst, UINT64 offset) {
  list_ResolveQueryData(l, heap, type, start, count, dst, offset);
}
static void STDMETHODCALLTYPE vlist_SetProtectedResourceSession(CmdList *l, ID3D12ProtectedResourceSession *s) { (void)l; (void)s; }      /* (none can exist: only NULL is given) */
static void STDMETHODCALLTYPE vlist_InitializeExtensionCommand(CmdList *l, ID3D12VideoExtensionCommand *cmd, const void *p, SIZE_T n) { (void)l; (void)cmd; (void)p; (void)n; }
static void STDMETHODCALLTYPE vlist_ExecuteExtensionCommand(CmdList *l, ID3D12VideoExtensionCommand *cmd, const void *p, SIZE_T n) { (void)l; (void)cmd; (void)p; (void)n; }
static void STDMETHODCALLTYPE vlist_Barrier(CmdList *l, UINT32 count, const D3D12_BARRIER_GROUP *groups) { list_Barrier(l, count, groups); }

/* The arguments of a DecodeFrame, in a kernel buffer of the list's (each range 16 aligned): picture parameters, inverse quantization matrices, slice controls. */
static UINT video_args_buffer(CmdList *l, const D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *in, UINT offsets[3], UINT sizes[3]) {
  const void *data[3] = {NULL, NULL, NULL};
  UINT64 total = 0;
  sizes[0] = sizes[1] = sizes[2] = 0;
  for (UINT i = 0; i < in->NumFrameArguments && i < 10; ++i) {
    const D3D12_VIDEO_DECODE_FRAME_ARGUMENT *a = &in->FrameArguments[i];
    if ((UINT)a->Type > D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL || !a->pData || !a->Size || a->Size > (64u << 20)) continue;
    data[a->Type] = a->pData;
    sizes[a->Type] = a->Size;
  }
  for (UINT k = 0; k < 3; ++k) { offsets[k] = (UINT)total; total += KALIGN((UINT64)sizes[k], 16); }
  if (total == 0) total = 16;
  if (l->ntemps == l->cap_temps) {
    const UINT cap = l->cap_temps ? l->cap_temps * 2 : 16;
    UINT *n = mem_grow(l->temps, cap * sizeof *n);
    if (!n) return 0;
    l->temps = n;
    l->cap_temps = cap;
  }
  KmtAllocDesc ad;
  memset(&ad, 0, sizeof ad);
  ad.flags = KA_VERTEX;
  ad.size = total;
  ad.mips = ad.samples = ad.layers = 1;
  UINT alloc = 0;
  if (!KMT_OK(kmt_create_allocation(l->d.dev->kdev, &ad, &alloc, NULL, NULL))) return 0;
  void *p = NULL;
  if (!KMT_OK(kmt_lock(l->d.dev->kdev, alloc, 0, 0, &p)) || !p) { kmt_destroy_allocation(l->d.dev->kdev, alloc); return 0; }
  for (UINT k = 0; k < 3; ++k)
    if (sizes[k]) memcpy((BYTE *)p + offsets[k], data[k], sizes[k]);
  kmt_unlock(l->d.dev->kdev, alloc);
  l->temps[l->ntemps++] = alloc;
  return alloc;
}
/* The luma plane allocation of a planar texture's slice `sub` - a packed YUV texture's one allocation of that slice (0: not one). */
static UINT video_luma(Resource *r, UINT sub) {
  if (!r || sub >= r->layers) return 0;
  if (r->planes) return r->plane_alloc[sub];
  return format_packed_yuv(r->desc.Format) && sub == 0 ? r->alloc : 0;
}

static void video_decode(CmdList *l, ID3D12VideoDecoder *decoder, ID3D12Resource *output, UINT sub, const D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *in) {
  VDec *dec = (VDec *)decoder;
  Resource *out = (Resource *)output;
  if (!dec || !out || !in || l->closed) return;
  VDHeap *heap = (VDHeap *)in->pHeap;
  Resource *bits = (Resource *)in->CompressedBitstream.pBuffer;
  /* (the heap's profile is the decoder's; the output a slice of a texture of the heap's format) */
  if (!heap || !heap->kdec || !guid_eq(&heap->desc.Configuration.DecodeProfile, &dec->desc.Configuration.DecodeProfile) || !video_luma(out, sub) || out->desc.Format != heap->desc.Format ||
      sub >= out->layers || !bits || bits->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || in->CompressedBitstream.Offset > bits->desc.Width ||
      in->CompressedBitstream.Size > bits->desc.Width - in->CompressedBitstream.Offset || in->CompressedBitstream.Offset > 0xFFFFFFFFu || in->CompressedBitstream.Size > 0xFFFFFFFFu ||
      in->ReferenceFrames.NumTexture2Ds > 128)
    return;
  UINT offsets[3], sizes[3];
  const UINT args = video_args_buffer(l, in, offsets, sizes);
  if (!args) return;
  const UINT nkeys = in->ReferenceFrames.NumTexture2Ds;
  ensure_room(l, 128 + 4 * nkeys, 8 + nkeys);
  UINT words[17 + 128];
  words[0] = heap->kdec;
  words[1] = alloc_index(l, args);
  words[2] = offsets[0]; words[3] = sizes[0]; words[4] = offsets[1]; words[5] = sizes[1]; words[6] = offsets[2]; words[7] = sizes[2];
  words[8] = alloc_index(l, bits->alloc);
  words[9] = (UINT)in->CompressedBitstream.Offset;
  words[10] = (UINT)in->CompressedBitstream.Size;
  words[11] = alloc_index(l, video_luma(out, sub));
  /* (a packed format - AYUV, Y410, Y416, YUY2, Y210, Y216 - is one image) */
  words[12] = format_packed_yuv(out->desc.Format) ? 0xFFFFFFFFu : alloc_index(l, out->plane_alloc[out->layers + sub]);
  words[13] = 0xFFFFFFFFu;      /* status: none (EndQuery points it at a query) */
  words[14] = 0;
  words[15] = 1;                /* (the current picture is known by its output) */
  words[16] = nkeys;
  for (UINT i = 0; i < nkeys; ++i) {
    Resource *r = in->ReferenceFrames.ppTexture2Ds ? (Resource *)in->ReferenceFrames.ppTexture2Ds[i] : NULL;
    const UINT luma = video_luma(r, in->ReferenceFrames.pSubresources ? in->ReferenceFrames.pSubresources[i] : 0);
    words[17 + i] = luma ? alloc_index(l, luma) : 0xFFFFFFFFu;
  }
  Chunk *c = chunk_cur(l);
  l->last_decode_chunk = l->nchunks;
  l->last_decode_at = c->size + 4 + 13 * 4;      /* (the header word, then 13 words before the status) */
  packet(l, OP_DECODE_FRAME, words, 17 + nkeys);
  keep(l, (IUnknown *)decoder);
  keep(l, (IUnknown *)in->pHeap);
}
static void STDMETHODCALLTYPE vdlist_DecodeFrame(CmdList *l, ID3D12VideoDecoder *decoder, const D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS *out, const D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *in) {
  /* (conversion is not supported - DECODE_CONVERSION_SUPPORT says so - and a picture asked to be converted is not decoded) */
  if (!out || out->ConversionArguments.Enable) return;
  video_decode(l, decoder, out->pOutputTexture2D, out->OutputSubresource, in);
}
static void STDMETHODCALLTYPE vdlist_DecodeFrame1(CmdList *l, ID3D12VideoDecoder *decoder, const D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS1 *out, const D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *in) {
  /* (and no histograms: DECODE_HISTOGRAM reports no components, so none is asked for) */
  if (!out || out->ConversionArguments.Enable) return;
  for (UINT i = 0; i < 4; ++i)
    if (out->Histograms[i].pBuffer) return;
  video_decode(l, decoder, out->pOutputTexture2D, out->OutputSubresource, in);
}

/* The planes of a picture for the kernel's VideoProcess: a planar texture's slice (both planes), or a 2D RGB texture of one level. 0: not one the processor takes. */
static int video_planes(CmdList *l, Resource *r, UINT sub, UINT *p0, UINT *p1) {
  if (!r) return 0;
  if (format_packed_yuv(r->desc.Format)) {
    /* (a packed YUV texture: one image a slice) */
    const UINT a = video_luma(r, sub);
    if (!a) return 0;
    *p0 = alloc_index(l, a);
    *p1 = 0xFFFFFFFFu;
    return 1;
  }
  if (r->planes) {
    if (sub >= r->layers || r->planes != 2) return 0;      /* (V208 / V408: not the processor's) */
    *p0 = alloc_index(l, r->plane_alloc[sub]);
    *p1 = alloc_index(l, r->plane_alloc[r->layers + sub]);
    return 1;
  }
  if (r->kind != 0 || r->mips != 1 || r->layers != 1 || sub != 0) return 0;
  *p0 = alloc_index(l, r->alloc);
  *p1 = 0xFFFFFFFFu;
  return 1;
}
static UINT float_word(float f) { UINT w; memcpy(&w, &f, 4); return w; }
/* The reference frames a stream gives (`n` of `frames`, nearest in time at `nearest`): that one's planes, or none (0xFFFFFFFF). (Past frames are in time order, the
 * last the nearest; future ones the first.) */
static void video_reference(CmdList *l, UINT n, ID3D12Resource *const *frames, const UINT *subs, UINT nearest, UINT *p0, UINT *p1) {
  *p0 = *p1 = 0xFFFFFFFFu;
  if (!n || !frames || !frames[nearest] || !video_planes(l, (Resource *)frames[nearest], subs ? subs[nearest] : 0, p0, p1)) *p0 = *p1 = 0xFFFFFFFFu;
}
/* What Direct3D 12's stereo format of a stream is in the kernel's terms (TCOS_STEREO_*). */
static UINT video_stereo_of(D3D12_VIDEO_FRAME_STEREO_FORMAT f) {
  switch (f) {
    case D3D12_VIDEO_FRAME_STEREO_FORMAT_HORIZONTAL: return TCOS_STEREO_HORIZONTAL;
    case D3D12_VIDEO_FRAME_STEREO_FORMAT_VERTICAL: return TCOS_STEREO_VERTICAL;
    case D3D12_VIDEO_FRAME_STEREO_FORMAT_SEPARATE: return TCOS_STEREO_SEPARATE;
    default: return TCOS_STEREO_NONE;      /* (NONE, MONO: one view, shown as both) */
  }
}
/* The streams into the output: one VideoProcess of the kernel's, or two with stereo output (the left view into OutputStream[0], the right into [1]). Each stream's
 * deinterlacing (its field type, the mode of its desc, the field RateInfo.OutputIndex names), reference frames, view of a stereo picture, filters (the levels of the
 * filters its desc enables, times their multipliers), luma key and light levels go to the kernel's optional words. A call the processor cannot take records nothing. */
static void video_process(CmdList *l, VProc *p, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *out, UINT count, const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *ins, UINT stride,
                          const D3D12_VIDEO_FIELD_TYPE *field_types) {
  if (!p || !out || !ins || count == 0 || count != p->nin || l->closed) return;
  TcosVideoProcessExtra extra;
  UINT extra_size = sizeof extra;
  if (FAILED(dx_GetPrivateData(OBJ(l), &TCOS_VIDEO_PROCESS_EXTRA_GUID, &extra_size, &extra)) || extra_size != sizeof extra || extra.version != 1) memset(&extra, 0, sizeof extra);
  const UINT views = p->out.EnableStereo ? 2 : 1;
  if (views == 2 && !out->OutputStream[1].pTexture2D) return;
  if (extra.count > VP_MAX_STREAMS) extra.count = VP_MAX_STREAMS;
  for (UINT view = 0; view < views; ++view) {
    ensure_room(l, 4 * (24 + (15 + 25) * VP_MAX_STREAMS) + 64, 4 + 6 * count);
    UINT w[24 + (15 + 25) * VP_MAX_STREAMS];
    UINT x[(25) * VP_MAX_STREAMS];
    UINT n = 0, nx = 0;
    w[n++] = (UINT)vp_format(p->out.Format);
    w[n++] = p->out.ColorSpace;
    w[n++] = (UINT)out->TargetRectangle.left; w[n++] = (UINT)out->TargetRectangle.top; w[n++] = (UINT)out->TargetRectangle.right; w[n++] = (UINT)out->TargetRectangle.bottom;
    for (UINT c = 0; c < 4; ++c) w[n++] = float_word(p->out.BackgroundColor[c]);
    w[n++] = vp_yuv(vp_format(p->out.Format)) ? 1u : 0u;      /* (the background is in the output's colour space) */
    w[n++] = (UINT)p->out.AlphaFillMode;
    w[n++] = p->out.AlphaFillModeSourceStreamIndex;
    if (!video_planes(l, (Resource *)out->OutputStream[view].pTexture2D, out->OutputStream[view].Subresource, &w[n], &w[n + 1])) return;
    n += 2;
    w[n++] = count;
    for (UINT s = 0; s < count; ++s) {
      const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *a = (const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *)((const BYTE *)ins + (SIZE_T)s * stride);
      const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *d = &p->in[s];
      const TcosVideoStreamExtra *e = s < extra.count ? &extra.stream[s] : NULL;
      /* the view: which picture (a separate pair's) and which frame of a packed one; mono output shows the left (base) view */
      const UINT stereo = e ? e->stereo : video_stereo_of(d->StereoFormat);
      const UINT frame = e ? e->view_frame[view] & 1u : view;
      const UINT input = stereo == TCOS_STEREO_SEPARATE ? frame : 0;
      if (stereo == TCOS_STEREO_SEPARATE && !a->InputStream[input].pTexture2D) return;
      const D3D12_VIDEO_PROCESS_INPUT_STREAM *is = &a->InputStream[input];
      w[n++] = (UINT)vp_format(d->Format);
      w[n++] = d->ColorSpace;
      const D3D12_RECT *src = &a->Transform.SourceRectangle, *dst = &a->Transform.DestinationRectangle;
      w[n++] = (UINT)src->left; w[n++] = (UINT)src->top; w[n++] = (UINT)src->right; w[n++] = (UINT)src->bottom;
      w[n++] = (UINT)dst->left; w[n++] = (UINT)dst->top; w[n++] = (UINT)dst->right; w[n++] = (UINT)dst->bottom;
      if (a->Transform.Orientation != D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT && !d->EnableOrientation) return;
      w[n++] = (UINT)a->Transform.Orientation;
      const int blend = d->EnableAlphaBlending && a->AlphaBlending.Enable;
      w[n++] = blend ? 1u : 0u;
      w[n++] = float_word(blend ? a->AlphaBlending.Alpha : 1.0f);
      if (!video_planes(l, (Resource *)is->pTexture2D, is->Subresource, &w[n], &w[n + 1])) return;
      n += 2;
      /* interlacing: the frame's field type (ProcessFrames1's, else the desc's), the desc's mode (CUSTOM: motion adaptive), the field */
      const D3D12_VIDEO_FIELD_TYPE ft = field_types ? *(const D3D12_VIDEO_FIELD_TYPE *)((const BYTE *)&field_types[0] + (SIZE_T)s * stride) : d->FieldType;
      if ((UINT)ft > D3D12_VIDEO_FIELD_TYPE_INTERLACED_BOTTOM_FIELD_FIRST) return;
      x[nx++] = (UINT)ft;
      x[nx++] = e && e->field != TCOS_FIELD_FROM_INDEX ? e->field & 1u : a->RateInfo.OutputIndex & 1u;
      x[nx++] = d->DeinterlaceMode == D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_CUSTOM ? 2u : d->DeinterlaceMode == D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_BOB ? 1u : 0u;
      const D3D12_VIDEO_PROCESS_REFERENCE_SET *rs = &is->ReferenceSet;
      if (ft != D3D12_VIDEO_FIELD_TYPE_NONE && d->DeinterlaceMode == D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_CUSTOM) {
        video_reference(l, rs->NumPastFrames, rs->ppPastFrames, rs->pPastSubresources, rs->NumPastFrames ? rs->NumPastFrames - 1 : 0, &x[nx], &x[nx + 1]);
        video_reference(l, rs->NumFutureFrames, rs->ppFutureFrames, rs->pFutureSubresources, 0, &x[nx + 2], &x[nx + 3]);
      } else {
        x[nx] = x[nx + 1] = x[nx + 2] = x[nx + 3] = 0xFFFFFFFFu;
      }
      nx += 4;
      /* stereo: a packed picture's frame (separate pictures were chosen above), whether it is upside down, the mono offset (right view to the right) */
      x[nx++] = stereo == TCOS_STEREO_SEPARATE ? TCOS_STEREO_NONE : stereo;
      x[nx++] = frame;
      x[nx++] = e && e->flip_frame == frame + 1 ? 1u : 0u;
      x[nx++] = (UINT)(stereo == TCOS_STEREO_MONO_OFFSET && e && views == 2 ? (view == 1 ? e->mono_offset : -e->mono_offset) : 0);   /* (mono output: the picture as it is) */
      /* the filters the desc enables, at their levels times the multipliers; a level outside its range is not taken */
      float values[7] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
      for (UINT f = 0; f < 7; ++f) {
        if (!(d->FilterFlags & (1u << f))) continue;
        const D3D12_VIDEO_PROCESS_FILTER_RANGE *r = &g_vp_filter_ranges[f];
        if (a->FilterLevels[f] < r->Minimum || a->FilterLevels[f] > r->Maximum) return;
        values[f] = (float)a->FilterLevels[f] * r->Multiplier;
      }
      for (UINT f = 0; f < 7; ++f) x[nx++] = float_word(values[f]);
      x[nx++] = d->LumaKey.Enable ? 1u : 0u;
      x[nx++] = float_word(d->LumaKey.Enable ? d->LumaKey.Lower : 0.0f);
      x[nx++] = float_word(d->LumaKey.Enable ? d->LumaKey.Upper : 0.0f);
      for (UINT k = 0; k < 4; ++k) x[nx++] = float_word(e ? e->hdr[k] : 0.0f);
    }
    w[n++] = 0x31585056u;      /* (the kernel's kVideoProcessExtension: the optional words follow) */
    for (UINT k = 0; k < 4; ++k) w[n++] = float_word(extra.display[k]);
    memcpy(&w[n], x, nx * sizeof *x);
    n += nx;
    packet(l, OP_VIDEO_PROCESS, w, n);
  }
  keep(l, (IUnknown *)p);
}
static void STDMETHODCALLTYPE vplist_ProcessFrames(CmdList *l, ID3D12VideoProcessor *proc, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *out, UINT count,
                                                   const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *ins) {
  video_process(l, (VProc *)proc, out, count, ins, sizeof *ins, NULL);
}
static void STDMETHODCALLTYPE vplist_ProcessFrames1(CmdList *l, ID3D12VideoProcessor *proc, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *out, UINT count,
                                                    const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS1 *ins) {
  /* (each frame's own field type: ARGUMENTS1 is ARGUMENTS and FieldType) */
  video_process(l, (VProc *)proc, out, count, (const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *)ins, sizeof *ins, ins ? &ins[0].FieldType : NULL);
}
