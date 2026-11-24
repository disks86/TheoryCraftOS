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
 * alpha blending and fill; no deinterlacing, filters, luma keying, stereo or auto processing (the feature queries say so).
 * Video encoding and motion estimation are encode.inc's (included after this file); this file hands their feature queries and creators over.
 * Not here: extension commands (none are enumerated), protected sessions, decode histograms and decode-time conversion (reported as not supported), timestamps on video lists. */

/* ---- the profiles ------------------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct VideoProfile { const GUID *guid; UINT kernel; DXGI_FORMAT formats[2]; UINT nformats; } VideoProfile;
/* (`kernel` is hardware::DxvaProfile) */
static const VideoProfile g_video_profiles[] = {
    {&D3D12_VIDEO_DECODE_PROFILE_H264, 0, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_MPEG2, 1, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_MPEG1_AND_MPEG2, 2, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_VP9, 3, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_VP9_10BIT_PROFILE2, 4, {DXGI_FORMAT_P010}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN, 5, {DXGI_FORMAT_NV12}, 1},
    {&D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0, 6, {DXGI_FORMAT_NV12, DXGI_FORMAT_P010}, 2},
};
#define VIDEO_PROFILES (sizeof g_video_profiles / sizeof g_video_profiles[0])
enum { ESC_CREATE_VIDEO_DECODER = 15, ESC_DESTROY_VIDEO_DECODER = 16, ESC_QUERY_VIDEO_DECODE_SUPPORT = 17 };

static const VideoProfile *video_profile(const GUID *g) {
  for (UINT i = 0; g && i < VIDEO_PROFILES; ++i)
    if (guid_eq(g, g_video_profiles[i].guid)) return &g_video_profiles[i];
  return NULL;
}
/* Whether the kernel decodes the configuration into `format` at that size (progressive, not encrypted). */
static int video_decode_supported(Dev *d, const D3D12_VIDEO_DECODE_CONFIGURATION *c, DXGI_FORMAT format, UINT width, UINT height) {
  const VideoProfile *p = video_profile(&c->DecodeProfile);
  if (!p || c->BitstreamEncryption != D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE || c->InterlaceType != D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE) return 0;
  int known = 0;
  for (UINT i = 0; i < p->nformats; ++i) known |= p->formats[i] == format;
  if (!known || width == 0 || height == 0) return 0;
  UINT request[5] = {ESC_QUERY_VIDEO_DECODE_SUPPORT, p->kernel, format == DXGI_FORMAT_P010 ? 1u : 0u, width, height}, reply[1] = {0};
  return KMT_OK(dev_escape(d, request, 5, reply, 1)) && reply[0] == 1;
}

/* ---- the video processor's formats and colour spaces (Hardware/VideoCodec/VideoProcess.hpp) ----------------------------------------------------------------------- */

enum { VP_NV12, VP_P010, VP_P016, VP_BGRA8, VP_BGRX8, VP_RGBA8, VP_RGB10A2, VP_RGBA16F };
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
    default: return -1;
  }
}
/* (the standard dynamic range spaces, gamma 2.2 and 2.4: YCbCr for the YUV formats, RGB for the others - what vp_color_space_supported takes; no PQ or HLG) */
static int vp_space_ok(DXGI_FORMAT f, DXGI_COLOR_SPACE_TYPE s) {
  const int v = vp_format(f);
  if (v < 0) return 0;
  if (v <= VP_P016)
    return s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 || s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 ||
           s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709 || s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020 ||
           s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709 ||
           s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020 || s == DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020;
  return s == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 || s == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 || s == DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 ||
         s == DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020 || s == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020 || s == DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709 ||
         s == DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020;
}
#define VP_MAX_STREAMS 16      /* (the kernel's kMaxVideoProcessStreams) */
#define VP_FEATURES (D3D12_VIDEO_PROCESS_FEATURE_FLAG_ALPHA_FILL | D3D12_VIDEO_PROCESS_FEATURE_FLAG_ROTATION | D3D12_VIDEO_PROCESS_FEATURE_FLAG_FLIP | D3D12_VIDEO_PROCESS_FEATURE_FLAG_ALPHA_BLENDING)

static int vp_input_ok(const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *in) {
  return vp_space_ok(in->Format, in->ColorSpace) && in->FieldType == D3D12_VIDEO_FIELD_TYPE_NONE && in->StereoFormat == D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE &&
         in->DeinterlaceMode == D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE && in->FilterFlags == D3D12_VIDEO_PROCESS_FILTER_FLAG_NONE && !in->LumaKey.Enable &&
         in->NumPastFrames == 0 && in->NumFutureFrames == 0 && !in->EnableAutoProcessing;
}
static int vp_output_ok(const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *out) {
  return vp_space_ok(out->Format, out->ColorSpace) && !out->EnableStereo && (UINT)out->AlphaFillMode <= D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_SOURCE_STREAM;
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
      f->ConfigurationFlags = D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_NONE;
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
      f->FormatCount = f->Configuration.BitstreamEncryption == D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE && f->Configuration.InterlaceType == D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE ? p->nformats : 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODE_FORMATS: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS);
      D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS *f = data;
      const VideoProfile *p = video_profile(&f->Configuration.DecodeProfile);
      const UINT n = p && f->Configuration.BitstreamEncryption == D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE && f->Configuration.InterlaceType == D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE ? p->nformats : 0;
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
      f->MemoryPoolL1Size = (UINT64)h->DecodeWidth * h->DecodeHeight * 3 / 2 * (h->MaxDecodePictureBufferCount + 1) * (h->Format == DXGI_FORMAT_P010 ? 2 : 1);
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_DECODER_HEAP_SIZE1: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE1);
      D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE1 *f = data;
      const D3D12_VIDEO_DECODER_HEAP_DESC *h = &f->VideoDecoderHeapDesc;
      if (f->Protected || !video_decode_supported(d, &h->Configuration, h->Format, h->DecodeWidth, h->DecodeHeight)) return E_INVALIDARG;
      f->MemoryPoolL0Size = 0;
      f->MemoryPoolL1Size = (UINT64)h->DecodeWidth * h->DecodeHeight * 3 / 2 * (h->MaxDecodePictureBufferCount + 1) * (h->Format == DXGI_FORMAT_P010 ? 2 : 1);
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_PROCESS_SUPPORT: {
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      const int ok = vp_space_ok(f->InputSample.Format.Format, f->InputSample.Format.ColorSpace) && vp_space_ok(f->OutputFormat.Format, f->OutputFormat.ColorSpace) &&
                     f->InputSample.Width && f->InputSample.Height && f->InputSample.Width <= KMAX_TEXTURE_DIMENSION && f->InputSample.Height <= KMAX_TEXTURE_DIMENSION &&
                     f->InputFieldType == D3D12_VIDEO_FIELD_TYPE_NONE && f->InputStereoFormat == D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE &&
                     f->OutputStereoFormat == D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
      f->SupportFlags = ok ? D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED : D3D12_VIDEO_PROCESS_SUPPORT_FLAG_NONE;
      memset(&f->ScaleSupport, 0, sizeof f->ScaleSupport);
      if (ok) {
        f->ScaleSupport.OutputSizeRange.MaxWidth = f->ScaleSupport.OutputSizeRange.MaxHeight = KMAX_TEXTURE_DIMENSION;
        f->ScaleSupport.OutputSizeRange.MinWidth = f->ScaleSupport.OutputSizeRange.MinHeight = 1;
      }
      f->FeatureSupport = ok ? VP_FEATURES : D3D12_VIDEO_PROCESS_FEATURE_FLAG_NONE;
      f->DeinterlaceSupport = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE;
      f->AutoProcessingSupport = D3D12_VIDEO_PROCESS_AUTO_PROCESSING_FLAG_NONE;
      f->FilterSupport = D3D12_VIDEO_PROCESS_FILTER_FLAG_NONE;
      memset(f->FilterRangeSupport, 0, sizeof f->FilterRangeSupport);
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
      /* (no deinterlacing or temporal filters: no past or future frames) */
      VIDEO_FEATURE(D3D12_FEATURE_DATA_VIDEO_PROCESS_REFERENCE_INFO);
      D3D12_FEATURE_DATA_VIDEO_PROCESS_REFERENCE_INFO *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->PastFrames = f->FutureFrames = 0;
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
  if (!p || desc->Configuration.BitstreamEncryption != D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE || desc->Configuration.InterlaceType != D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE)
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
  UINT request[6] = {ESC_CREATE_VIDEO_DECODER, p->kernel, desc->DecodeWidth, desc->DecodeHeight, surfaces, desc->Format == DXGI_FORMAT_P010 ? 1u : 0u}, reply[1] = {0};
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
/* The luma plane allocation of a planar texture's slice `sub` (0: not one). */
static UINT video_luma(Resource *r, UINT sub) { return r && r->planes && sub < r->layers ? r->plane_alloc[sub] : 0; }

static void video_decode(CmdList *l, ID3D12VideoDecoder *decoder, ID3D12Resource *output, UINT sub, const D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *in) {
  VDec *dec = (VDec *)decoder;
  Resource *out = (Resource *)output;
  if (!dec || !out || !in || l->closed) return;
  VDHeap *heap = (VDHeap *)in->pHeap;
  Resource *bits = (Resource *)in->CompressedBitstream.pBuffer;
  /* (the heap's profile is the decoder's; the output a slice of a texture of the heap's format) */
  if (!heap || !heap->kdec || !guid_eq(&heap->desc.Configuration.DecodeProfile, &dec->desc.Configuration.DecodeProfile) || !out->planes || out->desc.Format != heap->desc.Format ||
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
  words[11] = alloc_index(l, out->plane_alloc[sub]);
  words[12] = alloc_index(l, out->plane_alloc[out->layers + sub]);
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
  if (r->planes) {
    if (sub >= r->layers) return 0;
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
static void video_process(CmdList *l, VProc *p, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *out, UINT count, const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *ins, UINT stride,
                          const D3D12_VIDEO_FIELD_TYPE *field) {
  if (!p || !out || !ins || count == 0 || count != p->nin || l->closed) return;
  if (field && *field != D3D12_VIDEO_FIELD_TYPE_NONE) return;
  ensure_room(l, 256 + 64 * count, 4 + 2 * count);
  UINT w[16 + 15 * VP_MAX_STREAMS];
  UINT n = 0;
  w[n++] = (UINT)vp_format(p->out.Format);
  w[n++] = p->out.ColorSpace;
  w[n++] = (UINT)out->TargetRectangle.left; w[n++] = (UINT)out->TargetRectangle.top; w[n++] = (UINT)out->TargetRectangle.right; w[n++] = (UINT)out->TargetRectangle.bottom;
  for (UINT c = 0; c < 4; ++c) w[n++] = float_word(p->out.BackgroundColor[c]);
  w[n++] = vp_format(p->out.Format) <= VP_P016 ? 1u : 0u;      /* (the background is in the output's colour space) */
  w[n++] = (UINT)p->out.AlphaFillMode;
  w[n++] = p->out.AlphaFillModeSourceStreamIndex;
  if (!video_planes(l, (Resource *)out->OutputStream[0].pTexture2D, out->OutputStream[0].Subresource, &w[n], &w[n + 1])) return;
  n += 2;
  w[n++] = count;
  for (UINT s = 0; s < count; ++s) {
    const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *a = (const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *)((const BYTE *)ins + (SIZE_T)s * stride);
    const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *d = &p->in[s];
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
    if (!video_planes(l, (Resource *)a->InputStream[0].pTexture2D, a->InputStream[0].Subresource, &w[n], &w[n + 1])) return;
    n += 2;
  }
  packet(l, OP_VIDEO_PROCESS, w, n);
  keep(l, (IUnknown *)p);
}
static void STDMETHODCALLTYPE vplist_ProcessFrames(CmdList *l, ID3D12VideoProcessor *proc, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *out, UINT count,
                                                   const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *ins) {
  video_process(l, (VProc *)proc, out, count, ins, sizeof *ins, NULL);
}
static void STDMETHODCALLTYPE vplist_ProcessFrames1(CmdList *l, ID3D12VideoProcessor *proc, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *out, UINT count,
                                                    const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS1 *ins) {
  /* (the field type of each stream: progressive frames only) */
  for (UINT s = 0; ins && s < count; ++s)
    if (ins[s].FieldType != D3D12_VIDEO_FIELD_TYPE_NONE) return;
  video_process(l, (VProc *)proc, out, count, (const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *)ins, sizeof *ins, NULL);
}
