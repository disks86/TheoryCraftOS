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

/* Part of d3d11.c: Direct3D 11's video - ID3D11VideoDevice .. 2 (a face of the device), ID3D11VideoContext .. 3 (a face of the immediate context; a deferred context has
 * none, as on Windows), decoders, video processor enumerators and processors and the three kinds of video views - over Direct3D 12's video below (d3d12.dll video.inc:
 * the kernel decodes the DXVA buffers and processes video on the CPU).
 *
 * Decoding. A decoder is a Direct3D 12 decoder and decoder heap (the heap is the kernel's decoder, which keeps the pictures the stream refers to). GetDecoderBuffer hands
 * out memory of the decoder's for each DXVA buffer type; SubmitDecoderBuffers gathers what the program put there (the slices of several calls one after the other, the
 * slice controls' offsets moved with them); DecoderEndFrame sends the picture as one DecodeFrame on a VIDEO_DECODE queue of the device's, after what the immediate context
 * recorded (the list is sent first, and the queue runs the decode before it returns), so draws and copies before and after see the textures in order. The DXVA surface
 * indices of the parameters are the array slices of the texture the output view is of (as ffmpeg's d3d11va and Media Foundation give them): the reference list is every
 * slice of that texture, so index n is slice n. The status of each picture (DecoderExtension, DXVA_STATUS_REPORTING_FUNCTION) comes from Direct3D 12's decode statistics.
 * Profiles: what Direct3D 12 below decodes (H.264, MPEG-2, MPEG-1/2, VP9 profile 0 and 10-bit profile 2, HEVC Main, AV1 profile 0); all are VLD (bitstream) decoders, so
 * the buffers of the older acceleration modes (macroblock control, residual difference, deblocking, motion vectors, film grain) are not taken.
 *
 * Video processing. VideoProcessorBlt is one ProcessFrames on a VIDEO_PROCESS queue, ordered with the immediate context's work the same way. A processor keeps the
 * Direct3D 11 state (the output's and each stream's) and makes the Direct3D 12 processor of the formats and colour spaces it is blitted with (a few are kept). What it
 * does is what the kernel's processor does: colour space and format conversion (NV12, P010, P016, BGRA8, BGRX8, RGBA8, RGB10A2, RGBA16F; BT.601 / BT.709 / BT.2020,
 * studio and full range), bilinear scaling, rotation and mirroring, planar alpha, alpha fill, the background colour. Not: deinterlacing (progressive frames only:
 * one rate conversion with no past or future frames), filters, luma keys, palettes, stereo, constriction, pixel aspect ratio conversion, HDR tone mapping (the metadata
 * is kept and given back) - the caps say so, and a Blt that asks for them is E_INVALIDARG.
 *
 * Content protection: there is none on this device. No crypto type, key exchange or protection capability is reported, and crypto sessions and authenticated channels
 * cannot be made (see video_crypto_*): the context's methods that take one have nothing valid to be given. */

#define VIDEO_STREAMS 16u            /* the video processor's input streams (Direct3D 12's and the kernel's limit) */
#define VIDEO_SURFACES 128u          /* the slices of a decoder's output texture its pictures may name (the kernel's kMaxVideoDecodeSurfaces) */
#define VIDEO_STATUS_KEPT 64u        /* the status reports of decoded pictures kept until DecoderExtension takes them */
#define VIDEO_BUFFER_TYPES 9u        /* D3D11_VIDEO_DECODER_BUFFER_TYPE: PICTURE_PARAMETERS .. FILM_GRAIN */
#define VIDEO_PROCESSOR_CACHE 4u     /* Direct3D 12 processors a Direct3D 11 one keeps */
#define VIDEO_HDR_BYTES 128u         /* HDR metadata kept (DXGI_HDR_METADATA_HDR10 or HDR10PLUS) */

/* ---- the codecs ------------------------------------------------------------------------------------------------------------------------------------------------ */

enum { VSTATUS_NONE, VSTATUS_H264, VSTATUS_HEVC, VSTATUS_VPX, VSTATUS_AV1 };
#pragma pack(push, 1)
/* (the HEVC status report of the DXVA HEVC specification, which dxva.h does not have) */
typedef struct VideoStatusHevc { USHORT StatusReportFeedbackNumber; UCHAR CurrPic; UCHAR bBufType, bStatus, bReserved8Bits; USHORT wNumMbsAffected; } VideoStatusHevc;
#pragma pack(pop)

/* What Direct3D 11 says of a profile beyond what Direct3D 12 below does with it: ConfigBitstreamRaw (2: H.264's short slice format; 1: the others' bitstream format - HEVC,
 * VP9 and AV1 have short slices only, and ffmpeg takes 1 for them), the slice control entry and where its offset into the bitstream is, the status report, and where
 * the picture parameters have the feedback number and the current picture (STATUS_NONE: MPEG-2's DXVA has no status reports). */
typedef struct VideoCodec {
  const GUID *profile;
  UINT bitstream_raw;
  UINT slice_bytes, slice_location;
  UINT status;
  UINT pp_bytes, pp_feedback, pp_curr;
} VideoCodec;
static const VideoCodec g_video_codecs[] = {
    {&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 2, sizeof(DXVA_Slice_H264_Short), offsetof(DXVA_Slice_H264_Short, BSNALunitDataLocation), VSTATUS_H264,
     sizeof(DXVA_PicParams_H264), offsetof(DXVA_PicParams_H264, StatusReportFeedbackNumber), offsetof(DXVA_PicParams_H264, CurrPic)},
    {&D3D11_DECODER_PROFILE_MPEG2_VLD, 1, sizeof(DXVA_SliceInfo), offsetof(DXVA_SliceInfo, dwSliceDataLocation), VSTATUS_NONE, sizeof(DXVA_PictureParameters), 0, 0},
    {&D3D11_DECODER_PROFILE_MPEG2and1_VLD, 1, sizeof(DXVA_SliceInfo), offsetof(DXVA_SliceInfo, dwSliceDataLocation), VSTATUS_NONE, sizeof(DXVA_PictureParameters), 0, 0},
    {&D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0, 1, sizeof(DXVA_Slice_VPx_Short), offsetof(DXVA_Slice_VPx_Short, BSNALunitDataLocation), VSTATUS_VPX, sizeof(DXVA_PicParams_VP9),
     offsetof(DXVA_PicParams_VP9, StatusReportFeedbackNumber), offsetof(DXVA_PicParams_VP9, CurrPic)},
    {&D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2, 1, sizeof(DXVA_Slice_VPx_Short), offsetof(DXVA_Slice_VPx_Short, BSNALunitDataLocation), VSTATUS_VPX,
     sizeof(DXVA_PicParams_VP9), offsetof(DXVA_PicParams_VP9, StatusReportFeedbackNumber), offsetof(DXVA_PicParams_VP9, CurrPic)},
    {&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, 1, sizeof(DXVA_Slice_HEVC_Short), offsetof(DXVA_Slice_HEVC_Short, BSNALunitDataLocation), VSTATUS_HEVC, sizeof(DXVA_PicParams_HEVC),
     offsetof(DXVA_PicParams_HEVC, StatusReportFeedbackNumber), offsetof(DXVA_PicParams_HEVC, CurrPic)},
    {&DXVA_ModeAV1_VLD_Profile0, 1, sizeof(DXVA_Tile_AV1), offsetof(DXVA_Tile_AV1, DataOffset), VSTATUS_AV1, sizeof(DXVA_PicParams_AV1),
     offsetof(DXVA_PicParams_AV1, StatusReportFeedbackNumber), offsetof(DXVA_PicParams_AV1, CurrPicTextureIndex)},
};
#define VIDEO_CODECS (sizeof g_video_codecs / sizeof g_video_codecs[0])

static const VideoCodec *video_codec(const GUID *profile) {
  for (UINT i = 0; profile && i < VIDEO_CODECS; ++i)
    if (guid_eq(profile, g_video_codecs[i].profile)) return &g_video_codecs[i];
  return NULL;
}
static UINT video_status_bytes(UINT status) {
  switch (status) {
    case VSTATUS_H264: return sizeof(DXVA_Status_H264);
    case VSTATUS_HEVC: return sizeof(VideoStatusHevc);
    case VSTATUS_VPX: return sizeof(DXVA_Status_VPx);
    case VSTATUS_AV1: return sizeof(DXVA_Status_AV1);
    default: return 0;
  }
}

/* ---- the device's video state ------------------------------------------------------------------------------------------------------------------------------------ */

/* A queue of Direct3D 12's video work: the queue, its allocator and the one list recorded into it (closed between uses). */
typedef struct VideoQueue { ID3D12CommandQueue *queue; ID3D12CommandAllocator *allocator; ID3D12CommandList *list; } VideoQueue;
typedef struct VideoState {
  ID3D12VideoDevice *v12;
  VideoQueue decode, process;
  ID3D12QueryHeap *statistics;          /* a decode's statistics (one query), and the buffer they are read back through */
  ID3D12Resource *statistics_readback;
} VideoState;

static void video_queue_release(VideoQueue *q) {
  if (q->list) ID3D12CommandList_Release(q->list);
  if (q->allocator) ID3D12CommandAllocator_Release(q->allocator);
  if (q->queue) ID3D12CommandQueue_Release(q->queue);
  memset(q, 0, sizeof *q);
}
/* (dev_destroy) */
static void video_shutdown(Dev *d) {
  VideoState *v = d->video_state;
  if (!v) return;
  video_queue_release(&v->decode);
  video_queue_release(&v->process);
  if (v->statistics) ID3D12QueryHeap_Release(v->statistics);
  if (v->statistics_readback) ID3D12Resource_Release(v->statistics_readback);
  if (v->v12) ID3D12VideoDevice_Release(v->v12);
  mem_free(v);
  d->video_state = NULL;
}
/* The Direct3D 12 video device below (made the first time; NULL when Direct3D 12 has none). Under the device's lock. */
static VideoState *video_state(Dev *d) {
  if (d->video_state) return d->video_state;
  VideoState *v = mem_alloc(sizeof *v);
  if (!v) return NULL;
  if (FAILED(ID3D12Device_QueryInterface(d->d12, &IID_ID3D12VideoDevice, (void **)&v->v12)) || !v->v12) { mem_free(v); return NULL; }
  d->video_state = v;
  return v;
}
/* A queue of `type` (VIDEO_DECODE or VIDEO_PROCESS) with its list reset for recording. FALSE when Direct3D 12 refused one. */
static BOOL video_queue_begin(Dev *d, VideoQueue *q, D3D12_COMMAND_LIST_TYPE type, REFIID list_iid) {
  if (!q->queue) {
    const D3D12_COMMAND_QUEUE_DESC qd = {type, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (FAILED(ID3D12Device_CreateCommandQueue(d->d12, &qd, &IID_ID3D12CommandQueue, (void **)&q->queue)) ||
        FAILED(ID3D12Device_CreateCommandAllocator(d->d12, type, &IID_ID3D12CommandAllocator, (void **)&q->allocator)) ||
        FAILED(ID3D12Device_CreateCommandList(d->d12, 0, type, q->allocator, NULL, list_iid, (void **)&q->list))) {
      video_queue_release(q);
      return FALSE;
    }
    return TRUE;      /* (a list is made open) */
  }
  if (FAILED(ID3D12CommandAllocator_Reset(q->allocator))) return FALSE;
  /* (the video lists' Reset takes the allocator only; both kinds have it in the same place) */
  return SUCCEEDED(((ID3D12VideoDecodeCommandList *)q->list)->lpVtbl->Reset((ID3D12VideoDecodeCommandList *)q->list, q->allocator));
}
/* Closes the list and runs it after what the immediate context recorded: that list is sent first, and the queue runs this one before it returns. */
static void video_queue_run(Dev *d, VideoQueue *q) {
  ((ID3D12VideoDecodeCommandList *)q->list)->lpVtbl->Close((ID3D12VideoDecodeCommandList *)q->list);
  flush(d);
  ID3D12CommandList *lists[1] = {q->list};
  ID3D12CommandQueue_ExecuteCommandLists(q->queue, 1, lists);
}

/* ---- gathered bytes ------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct Bytes { BYTE *data; SIZE_T size, capacity; } Bytes;
static void bytes_free(Bytes *b) { mem_free(b->data); memset(b, 0, sizeof *b); }
static BOOL bytes_append(Bytes *b, const BYTE *data, SIZE_T n) {
  if (n > ((SIZE_T)1 << 30) - b->size) return FALSE;      /* (a picture of more than a gigabyte: not one) */
  if (b->size + n > b->capacity) {
    SIZE_T cap = b->capacity ? b->capacity : 4096;
    while (cap < b->size + n) cap *= 2;
    BYTE *grown = mem_grow(b->data, cap);
    if (!grown) return FALSE;
    b->data = grown;
    b->capacity = cap;
  }
  memcpy(b->data + b->size, data, n);
  b->size += n;
  return TRUE;
}
static BOOL bytes_set(Bytes *b, const BYTE *data, SIZE_T n) { b->size = 0; return bytes_append(b, data, n); }

/* ---- the objects ---------------------------------------------------------------------------------------------------------------------------------------------------- */

enum { VVIEW_DECODER_OUTPUT, VVIEW_PROCESSOR_INPUT, VVIEW_PROCESSOR_OUTPUT };
/* A decoder output view, video processor input view or video processor output view: a texture (the runtime's reference), the subresource and the format it is read as. */
typedef struct VideoView {
  Child c;
  Tex *tex;
  UINT kind;
  UINT slice, mip;
  DXGI_FORMAT format;
  union { D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC dov; D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC piv; D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC pov; } desc;
} VideoView;

/* A decoded picture's status, as DecoderExtension reports it. */
typedef struct VideoStatus { UINT feedback; BYTE curr, field; UINT width, height; BYTE status; USHORT macroblocks; } VideoStatus;

typedef struct VideoDecoder {
  Child c;
  D3D11_VIDEO_DECODER_DESC desc;
  D3D11_VIDEO_DECODER_CONFIG config;
  const VideoCodec *codec;
  ID3D12VideoDecoder *dec12;
  ID3D12VideoDecoderHeap *heap12;
  BYTE *buffers[VIDEO_BUFFER_TYPES];      /* what GetDecoderBuffer hands out, by type (made the first time) */
  UINT sizes[VIDEO_BUFFER_TYPES];
  BOOL held[VIDEO_BUFFER_TYPES];          /* handed out, and not given back with ReleaseDecoderBuffer yet */
  /* the picture between DecoderBeginFrame and DecoderEndFrame: its output (a reference; NULL: none begun) and what SubmitDecoderBuffers gave */
  VideoView *output;
  Bytes picture, matrix, slices, bits;
  BOOL have_matrix;
  UINT64 bits_rebase;                     /* what the slices' offsets of the last bitstream submitted are moved by (where it began in `bits`, less its DataOffset) */
  BOOL have_bits;
  VideoStatus status[VIDEO_STATUS_KEPT];  /* the newest last */
  UINT status_count;
} VideoDecoder;

typedef struct VideoEnum { Child c; D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc; } VideoEnum;

/* A stream's state (VideoProcessorSetStream*), as Direct3D 11 keeps it in the processor. */
typedef struct VideoStreamState {
  D3D11_VIDEO_FRAME_FORMAT frame_format;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE space;
  DXGI_COLOR_SPACE_TYPE space1;
  BOOL use_space1;                        /* the last colour space set was VideoProcessorSetStreamColorSpace1's */
  D3D11_VIDEO_PROCESSOR_OUTPUT_RATE rate;
  BOOL repeat;
  DXGI_RATIONAL custom_rate;
  BOOL source_enable, dest_enable;
  RECT source, dest;
  BOOL alpha_enable;
  FLOAT alpha;
  UINT palette_count;
  UINT palette[256];
  BOOL aspect_enable;
  DXGI_RATIONAL aspect_source, aspect_dest;
  BOOL luma_enable;
  FLOAT luma_lower, luma_upper;
  BOOL stereo_enable, left_frame0, base_frame0;
  D3D11_VIDEO_PROCESSOR_STEREO_FORMAT stereo_format;
  D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE flip_mode;
  int mono_offset;
  BOOL auto_processing;
  BOOL filter_enable[8];
  int filter_level[8];
  BOOL rotation_enable;
  D3D11_VIDEO_PROCESSOR_ROTATION rotation;
  BOOL mirror_enable, flip_horizontal, flip_vertical;
  DXGI_HDR_METADATA_TYPE hdr_type;
  UINT hdr_size;
  BYTE hdr[VIDEO_HDR_BYTES];
} VideoStreamState;

typedef struct VideoProcessorCache {
  ID3D12VideoProcessor *processor;
  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC out;
  D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC in[VIDEO_STREAMS];
  UINT count, used;
} VideoProcessorCache;

typedef struct VideoProcessor {
  Child c;
  VideoEnum *enumerator;                  /* (the runtime's reference) */
  UINT rate_index;
  /* the output's state (VideoProcessorSetOutput*) */
  BOOL target_enable;
  RECT target;
  BOOL background_ycbcr;
  D3D11_VIDEO_COLOR background;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE space;
  DXGI_COLOR_SPACE_TYPE space1;
  BOOL use_space1;
  D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE fill;
  UINT fill_stream;
  BOOL constriction;
  SIZE constriction_size;
  BOOL stereo;
  BOOL shader_usage;
  DXGI_HDR_METADATA_TYPE hdr_type;
  UINT hdr_size;
  BYTE hdr[VIDEO_HDR_BYTES];
  VideoStreamState streams[VIDEO_STREAMS];
  VideoProcessorCache cache[VIDEO_PROCESSOR_CACHE];
  UINT clock;
} VideoProcessor;

extern const ID3D11VideoDecoderVtbl vdec_vtbl;
extern const ID3D11VideoProcessorEnumerator1Vtbl venum_vtbl;
extern const ID3D11VideoProcessorVtbl vproc_vtbl;
extern const ID3D11VideoDecoderOutputViewVtbl dov_vtbl;
extern const ID3D11VideoProcessorInputViewVtbl piv_vtbl;
extern const ID3D11VideoProcessorOutputViewVtbl pov_vtbl;
extern const ID3D11VideoDevice2Vtbl vdev_vtbl;
extern const ID3D11VideoContext3Vtbl vctx_vtbl;

static VideoDecoder *vdec_of(ID3D11VideoDecoder *p) { return p && ((DxObj *)p)->lpVtbl == (const void *)&vdec_vtbl ? (VideoDecoder *)p : NULL; }
static VideoEnum *venum_of(ID3D11VideoProcessorEnumerator *p) { return p && ((DxObj *)p)->lpVtbl == (const void *)&venum_vtbl ? (VideoEnum *)p : NULL; }
static VideoProcessor *vproc_of(ID3D11VideoProcessor *p) { return p && ((DxObj *)p)->lpVtbl == (const void *)&vproc_vtbl ? (VideoProcessor *)p : NULL; }
static VideoView *vview_of(void *p, UINT kind) {
  static const void *const tables[3] = {&dov_vtbl, &piv_vtbl, &pov_vtbl};
  return p && ((DxObj *)p)->lpVtbl == tables[kind] ? (VideoView *)p : NULL;
}
static Dev *dev_of_video(ID3D11VideoDevice2 *p) { return (Dev *)((BYTE *)p - offsetof(Dev, video)); }
static Ctx *ctx_of_video(ID3D11VideoContext3 *p) { return (Ctx *)((BYTE *)p - offsetof(Ctx, video)); }

/* ---- the views --------------------------------------------------------------------------------------------------------------------------------------------------- */

static void vview_destroy(DxObj *o) {
  VideoView *v = (VideoView *)o;
  unhold(v->tex);
  child_destroy(o);
}
static void STDMETHODCALLTYPE vview_GetResource(VideoView *v, ID3D11Resource **out) {
  if (!out) return;
  dx_AddRef(OBJ(v->tex));
  *out = (ID3D11Resource *)v->tex;
}
static void STDMETHODCALLTYPE dov_GetDesc(VideoView *v, D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC *desc) { if (desc) *desc = v->desc.dov; }
static void STDMETHODCALLTYPE piv_GetDesc(VideoView *v, D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *desc) { if (desc) *desc = v->desc.piv; }
static void STDMETHODCALLTYPE pov_GetDesc(VideoView *v, D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *desc) { if (desc) *desc = v->desc.pov; }

static VideoView *vview_new(Dev *d, Tex *t, UINT kind) {
  static const GUID *const dov_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11VideoDecoderOutputView, NULL};
  static const GUID *const piv_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11VideoProcessorInputView, NULL};
  static const GUID *const pov_iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11View, &IID_ID3D11VideoProcessorOutputView, NULL};
  static const void *const tables[3] = {&dov_vtbl, &piv_vtbl, &pov_vtbl};
  static const GUID *const *const iids[3] = {dov_iids, piv_iids, pov_iids};
  VideoView *v = child_new(sizeof *v, d, tables[kind], iids[kind], vview_destroy);
  if (!v) return NULL;
  hold(t);
  v->tex = t;
  v->kind = kind;
  return v;
}

/* ---- the video processor's formats and colour spaces ------------------------------------------------------------------------------------------------------------ */

static BOOL vp_yuv(DXGI_FORMAT f) { return f == DXGI_FORMAT_NV12 || f == DXGI_FORMAT_P010 || f == DXGI_FORMAT_P016; }
/* The formats the processor reads and writes (the kernel's VpFormat). */
static BOOL vp_format_ok(DXGI_FORMAT f) {
  return vp_yuv(f) || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM ||
         f == DXGI_FORMAT_R16G16B16A16_FLOAT;
}
/* A Direct3D 11 colour space for a picture of `format`, as DXGI's (FALSE: xvYCC, which the processor does not have). YCbCr: the matrix (BT.601 or BT.709) and the
 * nominal range (16-235 when undefined); RGB: full or studio range. */
static BOOL vp_space_of(const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *s, DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE *out) {
  if (vp_yuv(format)) {
    if (s->YCbCr_xvYCC) return FALSE;
    const BOOL full = s->Nominal_Range == D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    *out = s->YCbCr_Matrix ? (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709)
                           : (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
  } else *out = s->RGB_Range ? DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  return TRUE;
}
/* The YCbCr matrix of a colour space (Kr, Kb) and whether its range is full. */
static void vp_matrix(DXGI_COLOR_SPACE_TYPE s, FLOAT *kr, FLOAT *kb, BOOL *full) {
  switch (s) {
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601: case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601: case DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601:
      *kr = 0.299f; *kb = 0.114f; break;
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020: case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020: case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020:
      *kr = 0.2627f; *kb = 0.0593f; break;
    default: *kr = 0.2126f; *kb = 0.0722f; break;
  }
  *full = s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 || s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601 || s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 ||
          s == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 || s == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 || s == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ||
          s == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020;
}
static FLOAT clamp01(FLOAT v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
/* The background colour as Direct3D 12 takes it, in the output's colour space: as it is when it is of the output's kind (YCbCr for a YUV output, RGB for an RGB one),
 * converted otherwise - an RGB colour to YCbCr with the output's matrix and range; a YCbCr colour (read with the matrix and range of the processor's output colour space)
 * to RGB in the output's range. */
static void vp_background(const VideoProcessor *p, DXGI_COLOR_SPACE_TYPE out_space, BOOL out_yuv, FLOAT bg[4]) {
  const D3D11_VIDEO_COLOR *c = &p->background;
  FLOAT kr, kb;
  BOOL full;
  if (p->background_ycbcr == out_yuv) {
    if (out_yuv) { bg[0] = c->YCbCr.Y; bg[1] = c->YCbCr.Cb; bg[2] = c->YCbCr.Cr; bg[3] = c->YCbCr.A; }
    else { bg[0] = c->RGBA.R; bg[1] = c->RGBA.G; bg[2] = c->RGBA.B; bg[3] = c->RGBA.A; }
    return;
  }
  if (out_yuv) {
    vp_matrix(out_space, &kr, &kb, &full);
    const FLOAT r = c->RGBA.R, g = c->RGBA.G, b = c->RGBA.B, kg = 1.0f - kr - kb;
    const FLOAT y = kr * r + kg * g + kb * b, cb = (b - y) / (2.0f * (1.0f - kb)), cr = (r - y) / (2.0f * (1.0f - kr));
    bg[0] = full ? y : (16.0f + 219.0f * y) / 255.0f;
    bg[1] = full ? cb + 128.0f / 255.0f : (128.0f + 224.0f * cb) / 255.0f;
    bg[2] = full ? cr + 128.0f / 255.0f : (128.0f + 224.0f * cr) / 255.0f;
    bg[3] = c->RGBA.A;
  } else {
    DXGI_COLOR_SPACE_TYPE in_space = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    vp_space_of(&p->space, DXGI_FORMAT_NV12, &in_space);
    vp_matrix(in_space, &kr, &kb, &full);
    const FLOAT y = full ? c->YCbCr.Y : (c->YCbCr.Y * 255.0f - 16.0f) / 219.0f;
    const FLOAT cb = full ? c->YCbCr.Cb - 128.0f / 255.0f : (c->YCbCr.Cb * 255.0f - 128.0f) / 224.0f;
    const FLOAT cr = full ? c->YCbCr.Cr - 128.0f / 255.0f : (c->YCbCr.Cr * 255.0f - 128.0f) / 224.0f;
    const FLOAT kg = 1.0f - kr - kb;
    const FLOAT r = clamp01(y + 2.0f * (1.0f - kr) * cr), b = clamp01(y + 2.0f * (1.0f - kb) * cb);
    const FLOAT g = clamp01((y - kr * r - kb * b) / kg);
    BOOL out_full;
    FLOAT unused_r, unused_b;
    vp_matrix(out_space, &unused_r, &unused_b, &out_full);
    const FLOAT rgb[3] = {r, g, b};
    for (UINT k = 0; k < 3; ++k) bg[k] = out_full ? rgb[k] : (16.0f + 219.0f * rgb[k]) / 255.0f;
    bg[3] = c->YCbCr.A;
  }
}
/* A stream's rotation (clockwise) and mirroring as Direct3D 12's orientation. The mirroring is done first, then the rotation; each orientation of Direct3D 12 is the
 * rotation first and then a horizontal flip. The eight are the 2 x 2 matrices of quarter turns and flips (x to the right, y down). */
static D3D12_VIDEO_PROCESS_ORIENTATION vp_orientation(const VideoStreamState *s) {
  typedef struct { int a, b, c, d; } M;      /* (x', y') = (a x + b y, c x + d y) */
  const M identity = {1, 0, 0, 1}, quarter = {0, -1, 1, 0}, flip_h = {-1, 0, 0, 1}, flip_v = {1, 0, 0, -1};
#define MUL(p, q) ((M){(p).a * (q).a + (p).b * (q).c, (p).a * (q).b + (p).b * (q).d, (p).c * (q).a + (p).d * (q).c, (p).c * (q).b + (p).d * (q).d})
  M m = identity;
  if (s->mirror_enable && s->flip_horizontal) m = MUL(flip_h, m);
  if (s->mirror_enable && s->flip_vertical) m = MUL(flip_v, m);
  const UINT turns = s->rotation_enable ? (UINT)s->rotation & 3u : 0;
  for (UINT k = 0; k < turns; ++k) m = MUL(quarter, m);
  const M r1 = quarter, r2 = MUL(quarter, quarter), r3 = MUL(quarter, MUL(quarter, quarter));
  const M table[8] = {identity, flip_h, r1, MUL(flip_h, r1), r2, flip_v, r3, MUL(flip_h, r3)};
#undef MUL
  for (UINT k = 0; k < 8; ++k)
    if (table[k].a == m.a && table[k].b == m.b && table[k].c == m.c && table[k].d == m.d) return (D3D12_VIDEO_PROCESS_ORIENTATION)k;
  return D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT;      /* (not reached: the eight are all there are) */
}

/* ---- ID3D11VideoDevice: decoders --------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE vdev_QueryInterface(ID3D11VideoDevice2 *p, REFIID riid, void **out) { return dev_QueryInterface(dev_of_video(p), riid, out); }
static ULONG STDMETHODCALLTYPE vdev_AddRef(ID3D11VideoDevice2 *p) { return dx_AddRef(OBJ(dev_of_video(p))); }
static ULONG STDMETHODCALLTYPE vdev_Release(ID3D11VideoDevice2 *p) { return dx_Release(OBJ(dev_of_video(p))); }
static HRESULT STDMETHODCALLTYPE vdev_SetPrivateData(ID3D11VideoDevice2 *p, REFGUID guid, UINT size, const void *data) { return dx_SetPrivateData(OBJ(dev_of_video(p)), guid, size, data); }
static HRESULT STDMETHODCALLTYPE vdev_SetPrivateDataInterface(ID3D11VideoDevice2 *p, REFGUID guid, const IUnknown *object) {
  return dx_SetPrivateDataInterface(OBJ(dev_of_video(p)), guid, object);
}

/* The profiles Direct3D 12 below decodes that this runtime knows (all of them), in its order. */
static UINT video_profiles(Dev *d, GUID *out, UINT capacity) {
  VideoState *v = video_state(d);
  if (!v) return 0;
  D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT count = {0, 0};
  if (FAILED(ID3D12VideoDevice_CheckFeatureSupport(v->v12, D3D12_FEATURE_VIDEO_DECODE_PROFILE_COUNT, &count, sizeof count)) || !count.ProfileCount || count.ProfileCount > 64) return 0;
  GUID all[64];
  D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES list = {0, count.ProfileCount, all};
  if (FAILED(ID3D12VideoDevice_CheckFeatureSupport(v->v12, D3D12_FEATURE_VIDEO_DECODE_PROFILES, &list, sizeof list))) return 0;
  UINT n = 0;
  for (UINT i = 0; i < count.ProfileCount; ++i) {
    if (!video_codec(&all[i])) continue;
    if (out && n < capacity) out[n] = all[i];
    ++n;
  }
  return n;
}
static UINT STDMETHODCALLTYPE vdev_GetVideoDecoderProfileCount(ID3D11VideoDevice2 *p) {
  Dev *d = dev_of_video(p);
  EnterCriticalSection(&d->lock);
  const UINT n = video_profiles(d, NULL, 0);
  LeaveCriticalSection(&d->lock);
  return n;
}
static HRESULT STDMETHODCALLTYPE vdev_GetVideoDecoderProfile(ID3D11VideoDevice2 *p, UINT index, GUID *profile) {
  Dev *d = dev_of_video(p);
  GUID all[64];
  if (!profile) return E_INVALIDARG;
  EnterCriticalSection(&d->lock);
  const UINT n = video_profiles(d, all, 64);
  LeaveCriticalSection(&d->lock);
  if (index >= n || index >= 64) return E_INVALIDARG;
  *profile = all[index];
  return S_OK;
}
/* Whether Direct3D 12 below decodes `profile` into `format` (any size; NULL: no such profile). */
static BOOL video_profile_format(Dev *d, const GUID *profile, DXGI_FORMAT format, BOOL *known) {
  *known = FALSE;
  VideoState *v = video_state(d);
  if (!v || !video_codec(profile)) return FALSE;
  D3D12_FEATURE_DATA_VIDEO_DECODE_FORMAT_COUNT count;
  memset(&count, 0, sizeof count);
  count.Configuration.DecodeProfile = *profile;
  if (FAILED(ID3D12VideoDevice_CheckFeatureSupport(v->v12, D3D12_FEATURE_VIDEO_DECODE_FORMAT_COUNT, &count, sizeof count))) return FALSE;
  *known = TRUE;
  if (!count.FormatCount || count.FormatCount > 16) return FALSE;
  DXGI_FORMAT formats[16];
  D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS list;
  memset(&list, 0, sizeof list);
  list.Configuration = count.Configuration;
  list.FormatCount = count.FormatCount;
  list.pOutputFormats = formats;
  if (FAILED(ID3D12VideoDevice_CheckFeatureSupport(v->v12, D3D12_FEATURE_VIDEO_DECODE_FORMATS, &list, sizeof list))) return FALSE;
  for (UINT i = 0; i < count.FormatCount; ++i) if (formats[i] == format) return TRUE;
  return FALSE;
}
static HRESULT STDMETHODCALLTYPE vdev_CheckVideoDecoderFormat(ID3D11VideoDevice2 *p, const GUID *profile, DXGI_FORMAT format, BOOL *supported) {
  Dev *d = dev_of_video(p);
  if (!profile || !supported) return E_INVALIDARG;
  BOOL known;
  EnterCriticalSection(&d->lock);
  *supported = video_profile_format(d, profile, format, &known);
  LeaveCriticalSection(&d->lock);
  return known ? S_OK : E_INVALIDARG;
}
/* Whether Direct3D 12 below decodes a description (its profile into its format at its size), progressive and not encrypted (what every configuration here is). */
static BOOL video_desc_supported(Dev *d, const D3D11_VIDEO_DECODER_DESC *desc) {
  VideoState *v = video_state(d);
  if (!v || !video_codec(&desc->Guid) || !desc->SampleWidth || !desc->SampleHeight) return FALSE;
  D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT s;
  memset(&s, 0, sizeof s);
  s.Configuration.DecodeProfile = desc->Guid;
  s.Configuration.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
  s.Configuration.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
  s.Width = desc->SampleWidth;
  s.Height = desc->SampleHeight;
  s.DecodeFormat = desc->OutputFormat;
  return SUCCEEDED(ID3D12VideoDevice_CheckFeatureSupport(v->v12, D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &s, sizeof s)) && (s.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED);
}
/* The one configuration of a profile: bitstream (VLD) decoding without encryption, the DXVA slice format of ConfigBitstreamRaw (VideoCodec), the values of
 * ConfigResidDiffAccelerator, ConfigHostInverseScan and ConfigSpecificIDCT that Windows' drivers give for VLD decoders. */
static void video_config(const VideoCodec *codec, D3D11_VIDEO_DECODER_CONFIG *c) {
  memset(c, 0, sizeof *c);
  c->guidConfigBitstreamEncryption = DXVA_NoEncrypt;
  c->guidConfigMBcontrolEncryption = DXVA_NoEncrypt;
  c->guidConfigResidDiffEncryption = DXVA_NoEncrypt;
  c->ConfigBitstreamRaw = codec->bitstream_raw;
  c->ConfigResidDiffAccelerator = 1;
  c->ConfigHostInverseScan = 1;
  c->ConfigSpecificIDCT = 2;
  c->ConfigMinRenderTargetBuffCount = 0;      /* (no surfaces beyond the stream's references: the decoder writes the output directly) */
}
static HRESULT STDMETHODCALLTYPE vdev_GetVideoDecoderConfigCount(ID3D11VideoDevice2 *p, const D3D11_VIDEO_DECODER_DESC *desc, UINT *count) {
  Dev *d = dev_of_video(p);
  if (count) *count = 0;
  if (!desc || !count || !video_codec(&desc->Guid)) return E_INVALIDARG;
  EnterCriticalSection(&d->lock);
  *count = video_desc_supported(d, desc) ? 1 : 0;
  LeaveCriticalSection(&d->lock);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE vdev_GetVideoDecoderConfig(ID3D11VideoDevice2 *p, const D3D11_VIDEO_DECODER_DESC *desc, UINT index, D3D11_VIDEO_DECODER_CONFIG *config) {
  Dev *d = dev_of_video(p);
  if (!desc || !config) return E_INVALIDARG;
  const VideoCodec *codec = video_codec(&desc->Guid);
  if (!codec) return E_INVALIDARG;
  EnterCriticalSection(&d->lock);
  const BOOL ok = video_desc_supported(d, desc);
  LeaveCriticalSection(&d->lock);
  if (!ok || index != 0) return E_INVALIDARG;
  video_config(codec, config);
  return S_OK;
}

static void vdec_frame_clear(VideoDecoder *v) {
  if (v->output) { unhold(v->output); v->output = NULL; }
  v->picture.size = v->matrix.size = v->slices.size = v->bits.size = 0;
  v->have_matrix = v->have_bits = FALSE;
  v->bits_rebase = 0;
}
static void vdec_destroy(DxObj *o) {
  VideoDecoder *v = (VideoDecoder *)o;
  vdec_frame_clear(v);
  for (UINT i = 0; i < VIDEO_BUFFER_TYPES; ++i) mem_free(v->buffers[i]);
  bytes_free(&v->picture);
  bytes_free(&v->matrix);
  bytes_free(&v->slices);
  bytes_free(&v->bits);
  if (v->heap12) ID3D12VideoDecoderHeap_Release(v->heap12);
  if (v->dec12) ID3D12VideoDecoder_Release(v->dec12);
  child_destroy(o);
}
/* A decoder of a description and configuration: the configuration must be the profile's (the encryption GUIDs DXVA_NoEncrypt - no encryption is had - and its
 * ConfigBitstreamRaw); the other fields describe the older acceleration modes and the surfaces, which a VLD decoder does not take from them. */
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoDecoder(ID3D11VideoDevice2 *p, const D3D11_VIDEO_DECODER_DESC *desc, const D3D11_VIDEO_DECODER_CONFIG *config, ID3D11VideoDecoder **out) {
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11VideoDecoder, NULL};
  Dev *d = dev_of_video(p);
  if (out) *out = NULL;
  if (!desc || !config || !out) return E_INVALIDARG;
  const VideoCodec *codec = video_codec(&desc->Guid);
  if (!codec) return E_INVALIDARG;
  if (!guid_eq(&config->guidConfigBitstreamEncryption, &DXVA_NoEncrypt) || !guid_eq(&config->guidConfigMBcontrolEncryption, &DXVA_NoEncrypt) ||
      !guid_eq(&config->guidConfigResidDiffEncryption, &DXVA_NoEncrypt) || config->ConfigBitstreamRaw != codec->bitstream_raw)
    return E_INVALIDARG;
  EnterCriticalSection(&d->lock);
  HRESULT hr = video_desc_supported(d, desc) ? S_OK : E_INVALIDARG;
  VideoDecoder *v = NULL;
  if (SUCCEEDED(hr)) {
    v = child_new(sizeof *v, d, &vdec_vtbl, iids, vdec_destroy);
    if (!v) hr = E_OUTOFMEMORY;
  }
  if (SUCCEEDED(hr)) {
    v->desc = *desc;
    v->config = *config;
    v->codec = codec;
    VideoState *vs = d->video_state;
    D3D12_VIDEO_DECODER_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.Configuration.DecodeProfile = desc->Guid;
    dd.Configuration.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
    dd.Configuration.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
    hr = ID3D12VideoDevice_CreateVideoDecoder(vs->v12, &dd, &IID_ID3D12VideoDecoder, (void **)&v->dec12);
    if (SUCCEEDED(hr)) {
      D3D12_VIDEO_DECODER_HEAP_DESC hd;
      memset(&hd, 0, sizeof hd);
      hd.Configuration = dd.Configuration;
      hd.DecodeWidth = desc->SampleWidth;
      hd.DecodeHeight = desc->SampleHeight;
      hd.Format = desc->OutputFormat;
      hd.FrameRate.Denominator = 1;
      hd.MaxDecodePictureBufferCount = VIDEO_SURFACES - 1;      /* (every slice of an output texture may be a reference: see video.inc's head) */
      hr = ID3D12VideoDevice_CreateVideoDecoderHeap(vs->v12, &hd, &IID_ID3D12VideoDecoderHeap, (void **)&v->heap12);
    }
  }
  LeaveCriticalSection(&d->lock);
  if (FAILED(hr)) { if (v) dx_Release(OBJ(v)); return hr; }
  *out = (ID3D11VideoDecoder *)v;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE vdec_GetCreationParameters(VideoDecoder *v, D3D11_VIDEO_DECODER_DESC *desc, D3D11_VIDEO_DECODER_CONFIG *config) {
  if (!desc || !config) return E_INVALIDARG;
  *desc = v->desc;
  *config = v->config;
  return S_OK;
}
/* The decoder's handle: what an authenticated channel's D3D11_AUTHENTICATED_CONFIGURE_CRYPTO_SESSION takes to name it. This runtime's number for the decoder (no
 * authenticated channel can be made here, so nothing else takes it). */
static HRESULT STDMETHODCALLTYPE vdec_GetDriverHandle(VideoDecoder *v, HANDLE *handle) {
  if (!handle) return E_INVALIDARG;
  *handle = (HANDLE)(ULONG_PTR)v->c.id;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE vdev_CreateVideoDecoderOutputView(ID3D11VideoDevice2 *p, ID3D11Resource *resource, const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC *desc,
                                                                   ID3D11VideoDecoderOutputView **out) {
  extern const ID3D11Texture2D1Vtbl tex_vtbl;
  Dev *d = dev_of_video(p);
  if (out) *out = NULL;
  Tex *t = tex_of_resource(resource);
  if (!t || ((DxObj *)t)->lpVtbl != (const void *)&tex_vtbl || !desc || !out) return E_INVALIDARG;
  if (!(t->desc.BindFlags & D3D11_BIND_DECODER) || desc->ViewDimension != D3D11_VDOV_DIMENSION_TEXTURE2D || desc->Texture2D.ArraySlice >= t->desc.ArraySize) return E_INVALIDARG;
  BOOL known;
  EnterCriticalSection(&d->lock);
  const BOOL ok = video_profile_format(d, &desc->DecodeProfile, t->desc.Format, &known);
  LeaveCriticalSection(&d->lock);
  if (!ok) return E_INVALIDARG;
  VideoView *v = vview_new(d, t, VVIEW_DECODER_OUTPUT);
  if (!v) return E_OUTOFMEMORY;
  v->desc.dov = *desc;
  v->slice = desc->Texture2D.ArraySlice;
  v->format = t->desc.Format;
  *out = (ID3D11VideoDecoderOutputView *)v;
  return S_OK;
}

/* ---- ID3D11VideoDevice: video processors ------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE venum_GetVideoProcessorContentDesc(VideoEnum *e, D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc) {
  if (!desc) return E_INVALIDARG;
  *desc = e->desc;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE venum_CheckVideoProcessorFormat(VideoEnum *e, DXGI_FORMAT format, UINT *flags) {
  (void)e;
  if (!flags) return E_INVALIDARG;
  *flags = vp_format_ok(format) ? D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT | D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT : 0;
  return S_OK;
}
/* What the processor does (see video.inc's head): range and matrix conversions; alpha fill, rotation, planar alpha, mirroring, the shader usage hint. Progressive input
 * only: one rate conversion. */
static HRESULT STDMETHODCALLTYPE venum_GetVideoProcessorCaps(VideoEnum *e, D3D11_VIDEO_PROCESSOR_CAPS *caps) {
  (void)e;
  if (!caps) return E_INVALIDARG;
  memset(caps, 0, sizeof *caps);
  caps->DeviceCaps = D3D11_VIDEO_PROCESSOR_DEVICE_CAPS_RGB_RANGE_CONVERSION | D3D11_VIDEO_PROCESSOR_DEVICE_CAPS_YCbCr_MATRIX_CONVERSION | D3D11_VIDEO_PROCESSOR_DEVICE_CAPS_NOMINAL_RANGE;
  caps->FeatureCaps = D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ALPHA_FILL | D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION | D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ALPHA_STREAM |
                      D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_MIRROR | D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_SHADER_USAGE;
  caps->RateConversionCapsCount = 1;
  caps->MaxInputStreams = VIDEO_STREAMS;
  caps->MaxStreamStates = VIDEO_STREAMS;
  return S_OK;
}
/* The one rate conversion: frames as they come, no past or future frames, no deinterlacing, inverse telecine or frame rate conversion, no custom rates. */
static HRESULT STDMETHODCALLTYPE venum_GetVideoProcessorRateConversionCaps(VideoEnum *e, UINT index, D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *caps) {
  (void)e;
  if (!caps || index != 0) return E_INVALIDARG;
  memset(caps, 0, sizeof *caps);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE venum_GetVideoProcessorCustomRate(VideoEnum *e, UINT type_index, UINT rate_index, D3D11_VIDEO_PROCESSOR_CUSTOM_RATE *rate) {
  (void)e; (void)type_index; (void)rate_index;
  if (rate) memset(rate, 0, sizeof *rate);
  return E_INVALIDARG;      /* (the rate conversion has no custom rates: CustomRateCount 0) */
}
static HRESULT STDMETHODCALLTYPE venum_GetVideoProcessorFilterRange(VideoEnum *e, D3D11_VIDEO_PROCESSOR_FILTER filter, D3D11_VIDEO_PROCESSOR_FILTER_RANGE *range) {
  (void)e; (void)filter;
  if (range) memset(range, 0, sizeof *range);
  return E_INVALIDARG;      /* (no filters: FilterCaps 0) */
}
/* Whether a conversion is had: what Direct3D 12's VIDEO_PROCESS_SUPPORT answers for the formats and colour spaces (the content's size). */
static HRESULT STDMETHODCALLTYPE venum_CheckVideoProcessorFormatConversion(VideoEnum *e, DXGI_FORMAT in_format, DXGI_COLOR_SPACE_TYPE in_space, DXGI_FORMAT out_format,
                                                                           DXGI_COLOR_SPACE_TYPE out_space, BOOL *supported) {
  Dev *d = e->c.dev;
  if (!supported) return E_INVALIDARG;
  *supported = FALSE;
  EnterCriticalSection(&d->lock);
  VideoState *v = video_state(d);
  if (v && vp_format_ok(in_format) && vp_format_ok(out_format)) {
    D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT s;
    memset(&s, 0, sizeof s);
    s.InputSample.Width = e->desc.InputWidth;
    s.InputSample.Height = e->desc.InputHeight;
    s.InputSample.Format.Format = in_format;
    s.InputSample.Format.ColorSpace = in_space;
    s.InputFieldType = D3D12_VIDEO_FIELD_TYPE_NONE;
    s.InputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    s.InputFrameRate = e->desc.InputFrameRate;
    s.OutputFormat.Format = out_format;
    s.OutputFormat.ColorSpace = out_space;
    s.OutputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    s.OutputFrameRate = e->desc.OutputFrameRate;
    *supported = SUCCEEDED(ID3D12VideoDevice_CheckFeatureSupport(v->v12, D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &s, sizeof s)) &&
                 (s.SupportFlags & D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED);
  }
  LeaveCriticalSection(&d->lock);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoProcessorEnumerator(ID3D11VideoDevice2 *p, const D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc, ID3D11VideoProcessorEnumerator **out) {
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11VideoProcessorEnumerator, &IID_ID3D11VideoProcessorEnumerator1, NULL};
  Dev *d = dev_of_video(p);
  if (out) *out = NULL;
  if (!desc || !out || (UINT)desc->InputFrameFormat > D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST || (UINT)desc->Usage > D3D11_VIDEO_USAGE_OPTIMAL_QUALITY ||
      !desc->InputWidth || !desc->InputHeight || !desc->OutputWidth || !desc->OutputHeight || desc->InputWidth > 4096 || desc->InputHeight > 4096 || desc->OutputWidth > 4096 ||
      desc->OutputHeight > 4096)
    return E_INVALIDARG;
  EnterCriticalSection(&d->lock);
  const BOOL have = video_state(d) != NULL;
  LeaveCriticalSection(&d->lock);
  if (!have) return E_INVALIDARG;
  VideoEnum *e = child_new(sizeof *e, d, &venum_vtbl, iids, child_destroy);
  if (!e) return E_OUTOFMEMORY;
  e->desc = *desc;
  *out = (ID3D11VideoProcessorEnumerator *)e;
  return S_OK;
}

static void vproc_destroy(DxObj *o) {
  VideoProcessor *p = (VideoProcessor *)o;
  for (UINT i = 0; i < VIDEO_PROCESSOR_CACHE; ++i) if (p->cache[i].processor) ID3D12VideoProcessor_Release(p->cache[i].processor);
  unhold(p->enumerator);
  child_destroy(o);
}
static void STDMETHODCALLTYPE vproc_GetContentDesc(VideoProcessor *p, D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc) { if (desc) *desc = p->enumerator->desc; }
static void STDMETHODCALLTYPE vproc_GetRateConversionCaps(VideoProcessor *p, D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *caps) {
  if (caps) venum_GetVideoProcessorRateConversionCaps(p->enumerator, p->rate_index, caps);
}
/* A processor with Direct3D 11's defaults: no target rectangle (all of the output), a black background, BT.601 studio-range YCbCr and full-range RGB, opaque alpha
 * fill; each stream progressive, at the normal rate, all of its source into all of the output, opaque, no rotation or mirroring, auto processing on (there is none to do). */
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoProcessor(ID3D11VideoDevice2 *p, ID3D11VideoProcessorEnumerator *enumerator, UINT rate_index, ID3D11VideoProcessor **out) {
  static const GUID *const iids[] = {&IID_ID3D11DeviceChild, &IID_ID3D11VideoProcessor, NULL};
  Dev *d = dev_of_video(p);
  VideoEnum *e = venum_of(enumerator);
  if (out) *out = NULL;
  if (!e || e->c.dev != d || !out || rate_index != 0) return E_INVALIDARG;
  VideoProcessor *v = child_new(sizeof *v, d, &vproc_vtbl, iids, vproc_destroy);
  if (!v) return E_OUTOFMEMORY;
  hold(e);
  v->enumerator = e;
  v->rate_index = rate_index;
  v->background.RGBA.A = 1.0f;
  v->space1 = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  v->fill = D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE;
  for (UINT s = 0; s < VIDEO_STREAMS; ++s) {
    VideoStreamState *st = &v->streams[s];
    st->frame_format = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    st->space1 = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    st->rate = D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL;
    st->alpha = 1.0f;
    st->auto_processing = TRUE;
    st->aspect_source.Numerator = st->aspect_source.Denominator = st->aspect_dest.Numerator = st->aspect_dest.Denominator = 1;
  }
  *out = (ID3D11VideoProcessor *)v;
  return S_OK;
}

/* A texture the processor reads or writes: a 2D one (not an array or a cube, not multisampled, not the CPU's), of a format it has. */
static BOOL vp_texture_ok(const Tex *t) {
  extern const ID3D11Texture2D1Vtbl tex_vtbl;
  return ((const DxObj *)t)->lpVtbl == (const void *)&tex_vtbl && t->kind != 3 && t->desc.SampleDesc.Count == 1 && t->desc.Usage != D3D11_USAGE_STAGING && !t->depth;
}
/* An input view: a level and slice of a texture, read as the texture's format (FourCC 0) or as the format the FourCC names, which must be the texture's. */
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoProcessorInputView(ID3D11VideoDevice2 *p, ID3D11Resource *resource, ID3D11VideoProcessorEnumerator *enumerator,
                                                                    const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *desc, ID3D11VideoProcessorInputView **out) {
  Dev *d = dev_of_video(p);
  Tex *t = tex_of_resource(resource);
  VideoEnum *e = venum_of(enumerator);
  if (out) *out = NULL;
  if (!t || !e || !desc || !out || !vp_texture_ok(t) || !vp_format_ok(t->desc.Format)) return E_INVALIDARG;
  if (desc->FourCC) {
#define VIDEO_FOURCC(a, b, c, e) ((UINT)(BYTE)(a) | (UINT)(BYTE)(b) << 8 | (UINT)(BYTE)(c) << 16 | (UINT)(BYTE)(e) << 24)
    static const struct { UINT fourcc; DXGI_FORMAT format; } names[] = {{VIDEO_FOURCC('N', 'V', '1', '2'), DXGI_FORMAT_NV12}, {VIDEO_FOURCC('P', '0', '1', '0'), DXGI_FORMAT_P010},
                                                                       {VIDEO_FOURCC('P', '0', '1', '6'), DXGI_FORMAT_P016}};
#undef VIDEO_FOURCC
    BOOL match = FALSE;
    for (UINT i = 0; i < sizeof names / sizeof names[0]; ++i) match |= names[i].fourcc == desc->FourCC && names[i].format == t->desc.Format;
    if (!match) return E_INVALIDARG;
  }
  if (desc->ViewDimension != D3D11_VPIV_DIMENSION_TEXTURE2D || desc->Texture2D.MipSlice >= t->desc.MipLevels || desc->Texture2D.ArraySlice >= t->desc.ArraySize) return E_INVALIDARG;
  VideoView *v = vview_new(d, t, VVIEW_PROCESSOR_INPUT);
  if (!v) return E_OUTOFMEMORY;
  v->desc.piv = *desc;
  v->slice = desc->Texture2D.ArraySlice;
  v->mip = desc->Texture2D.MipSlice;
  v->format = t->desc.Format;
  *out = (ID3D11VideoProcessorInputView *)v;
  return S_OK;
}
/* An output view: a level of a render target (of one slice, or of the first slice of a 2D array view: without stereo only that one is written). */
static HRESULT STDMETHODCALLTYPE vdev_CreateVideoProcessorOutputView(ID3D11VideoDevice2 *p, ID3D11Resource *resource, ID3D11VideoProcessorEnumerator *enumerator,
                                                                     const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *desc, ID3D11VideoProcessorOutputView **out) {
  Dev *d = dev_of_video(p);
  Tex *t = tex_of_resource(resource);
  VideoEnum *e = venum_of(enumerator);
  if (out) *out = NULL;
  if (!t || !e || !desc || !out || !vp_texture_ok(t) || !vp_format_ok(t->desc.Format) || !(t->desc.BindFlags & D3D11_BIND_RENDER_TARGET)) return E_INVALIDARG;
  UINT mip, slice;
  if (desc->ViewDimension == D3D11_VPOV_DIMENSION_TEXTURE2D) {
    mip = desc->Texture2D.MipSlice;
    slice = 0;
  } else if (desc->ViewDimension == D3D11_VPOV_DIMENSION_TEXTURE2DARRAY) {
    mip = desc->Texture2DArray.MipSlice;
    slice = desc->Texture2DArray.FirstArraySlice;
    if (!desc->Texture2DArray.ArraySize || slice >= t->desc.ArraySize || desc->Texture2DArray.ArraySize > t->desc.ArraySize - slice) return E_INVALIDARG;
  } else return E_INVALIDARG;
  if (mip >= t->desc.MipLevels) return E_INVALIDARG;
  VideoView *v = vview_new(d, t, VVIEW_PROCESSOR_OUTPUT);
  if (!v) return E_OUTOFMEMORY;
  v->desc.pov = *desc;
  v->slice = slice;
  v->mip = mip;
  v->format = t->desc.Format;
  *out = (ID3D11VideoProcessorOutputView *)v;
  return S_OK;
}

/* ---- ID3D11VideoDevice: content protection (none) ----------------------------------------------------------------------------------------------------------------- */

/* The device has no content protection: no crypto type (a crypto type asked about is not one it has), and for no encryption (no crypto type) no capability, key
 * exchange or protected memory. */
static HRESULT STDMETHODCALLTYPE vdev_GetContentProtectionCaps(ID3D11VideoDevice2 *p, const GUID *crypto_type, const GUID *profile, D3D11_VIDEO_CONTENT_PROTECTION_CAPS *caps) {
  (void)p;
  if (!caps) return E_INVALIDARG;
  memset(caps, 0, sizeof *caps);
  if (crypto_type && !guid_eq(crypto_type, &DXVA_NoEncrypt)) return E_INVALIDARG;
  if (profile && !video_codec(profile)) return E_INVALIDARG;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE vdev_CheckCryptoKeyExchange(ID3D11VideoDevice2 *p, const GUID *crypto_type, const GUID *profile, UINT index, GUID *key_exchange) {
  (void)p; (void)crypto_type; (void)profile; (void)index;
  if (key_exchange) memset(key_exchange, 0, sizeof *key_exchange);
  return E_INVALIDARG;      /* (KeyExchangeTypeCount is 0) */
}
static HRESULT STDMETHODCALLTYPE vdev_CreateCryptoSession(ID3D11VideoDevice2 *p, const GUID *crypto_type, const GUID *profile, const GUID *key_exchange, ID3D11CryptoSession **out) {
  (void)p; (void)crypto_type; (void)profile; (void)key_exchange;
  if (out) *out = NULL;
  return E_INVALIDARG;      /* (no crypto type is one the device has) */
}
/* Authenticated channels: the driver's (software or hardware) are the hardware's, which has none; Windows' own (D3D11_AUTHENTICATED_CHANNEL_D3D11) is one the runtime
 * implements with a certificate Microsoft signs, which this runtime cannot have. No channel is made. */
static HRESULT STDMETHODCALLTYPE vdev_CreateAuthenticatedChannel(ID3D11VideoDevice2 *p, D3D11_AUTHENTICATED_CHANNEL_TYPE type, ID3D11AuthenticatedChannel **out) {
  (void)p;
  if (out) *out = NULL;
  if (!out || type < D3D11_AUTHENTICATED_CHANNEL_D3D11 || type > D3D11_AUTHENTICATED_CHANNEL_DRIVER_HARDWARE) return E_INVALIDARG;
  return DXGI_ERROR_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE vdev_GetCryptoSessionPrivateDataSize(ID3D11VideoDevice2 *p, const GUID *crypto_type, const GUID *profile, const GUID *key_exchange, UINT *input,
                                                                      UINT *output) {
  (void)p; (void)crypto_type; (void)profile; (void)key_exchange;
  if (input) *input = 0;
  if (output) *output = 0;
  return E_INVALIDARG;      /* (no crypto type, no key exchange) */
}
static HRESULT STDMETHODCALLTYPE vdev_NegotiateCryptoSessionKeyExchangeMT(ID3D11VideoDevice2 *p, ID3D11CryptoSession *session, D3D11_CRYPTO_SESSION_KEY_EXCHANGE_FLAGS flags,
                                                                         UINT size, void *data) {
  (void)p; (void)session; (void)flags; (void)size; (void)data;
  return E_INVALIDARG;      /* (no crypto session can have been made) */
}

/* ---- ID3D11VideoDevice1 .. 2: decoder capabilities, downsampling, features --------------------------------------------------------------------------------------- */

/* What the decoder does for a profile at a size: nothing of the downsampling kinds; D3D11_VIDEO_DECODER_CAPS_UNSUPPORTED for a profile, size or crypto type it does not
 * decode. */
static HRESULT STDMETHODCALLTYPE vdev_GetVideoDecoderCaps(ID3D11VideoDevice2 *p, const GUID *profile, UINT width, UINT height, const DXGI_RATIONAL *rate, UINT bit_rate,
                                                          const GUID *crypto_type, UINT *caps) {
  Dev *d = dev_of_video(p);
  (void)rate; (void)bit_rate;
  if (!profile || !caps) return E_INVALIDARG;
  *caps = 0;
  const VideoCodec *codec = video_codec(profile);
  if (!codec) return E_INVALIDARG;
  BOOL any = FALSE;
  EnterCriticalSection(&d->lock);
  for (UINT f = 0; f < 2 && !any; ++f) {
    const D3D11_VIDEO_DECODER_DESC desc = {*profile, width, height, f ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12};
    any = video_desc_supported(d, &desc);
  }
  LeaveCriticalSection(&d->lock);
  if (!any || (crypto_type && !guid_eq(crypto_type, &DXVA_NoEncrypt))) *caps = D3D11_VIDEO_DECODER_CAPS_UNSUPPORTED;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE vdev_CheckVideoDecoderDownsampling(ID3D11VideoDevice2 *p, const D3D11_VIDEO_DECODER_DESC *in_desc, DXGI_COLOR_SPACE_TYPE in_space,
                                                                    const D3D11_VIDEO_DECODER_CONFIG *in_config, const DXGI_RATIONAL *rate, const D3D11_VIDEO_SAMPLE_DESC *out_desc,
                                                                    BOOL *supported, BOOL *real_time) {
  (void)p; (void)in_space; (void)rate;
  if (!in_desc || !in_config || !out_desc || !supported || !real_time) return E_INVALIDARG;
  *supported = FALSE;      /* (the decoder does not downsample: GetVideoDecoderCaps has no DOWNSAMPLE) */
  *real_time = FALSE;
  return video_codec(&in_desc->Guid) ? S_OK : E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE vdev_RecommendVideoDecoderDownsampleParameters(ID3D11VideoDevice2 *p, const D3D11_VIDEO_DECODER_DESC *in_desc, DXGI_COLOR_SPACE_TYPE in_space,
                                                                                const D3D11_VIDEO_DECODER_CONFIG *in_config, const DXGI_RATIONAL *rate,
                                                                                D3D11_VIDEO_SAMPLE_DESC *recommended) {
  (void)p; (void)in_desc; (void)in_space; (void)in_config; (void)rate;
  if (recommended) memset(recommended, 0, sizeof *recommended);
  return E_INVALIDARG;      /* (there is no downsampling to recommend parameters of) */
}
/* D3D11_FEATURE_VIDEO_DECODER_HISTOGRAM: no histograms (Direct3D 12 below reports no components). */
static HRESULT STDMETHODCALLTYPE vdev_CheckFeatureSupport(ID3D11VideoDevice2 *p, D3D11_FEATURE_VIDEO feature, void *data, UINT size) {
  (void)p;
  if (!data || feature != D3D11_FEATURE_VIDEO_DECODER_HISTOGRAM || size != sizeof(D3D11_FEATURE_DATA_VIDEO_DECODER_HISTOGRAM)) return E_INVALIDARG;
  D3D11_FEATURE_DATA_VIDEO_DECODER_HISTOGRAM *h = data;
  if (!video_codec(&h->DecoderDesc.Guid)) return E_INVALIDARG;
  h->Components = D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAG_NONE;
  h->BinCount = 0;
  h->CounterBitDepth = 0;
  return S_OK;
}

/* ---- ID3D11VideoContext: decoding --------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE vctx_QueryInterface(ID3D11VideoContext3 *p, REFIID riid, void **out) { return ctx_QueryInterface(ctx_of_video(p), riid, out); }
static ULONG STDMETHODCALLTYPE vctx_AddRef(ID3D11VideoContext3 *p) { return ctx_AddRef(ctx_of_video(p)); }
static ULONG STDMETHODCALLTYPE vctx_Release(ID3D11VideoContext3 *p) { return ctx_Release(ctx_of_video(p)); }
static void STDMETHODCALLTYPE vctx_GetDevice(ID3D11VideoContext3 *p, ID3D11Device **out) { ctx_GetDevice(ctx_of_video(p), out); }
static HRESULT STDMETHODCALLTYPE vctx_GetPrivateData(ID3D11VideoContext3 *p, REFGUID guid, UINT *size, void *data) { return dx_GetPrivateData(OBJ(ctx_of_video(p)), guid, size, data); }
static HRESULT STDMETHODCALLTYPE vctx_SetPrivateData(ID3D11VideoContext3 *p, REFGUID guid, UINT size, const void *data) { return dx_SetPrivateData(OBJ(ctx_of_video(p)), guid, size, data); }
static HRESULT STDMETHODCALLTYPE vctx_SetPrivateDataInterface(ID3D11VideoContext3 *p, REFGUID guid, const IUnknown *object) {
  return dx_SetPrivateDataInterface(OBJ(ctx_of_video(p)), guid, object);
}

/* The memory a decoder hands out for a buffer type, as large as a picture of its size may need (the bitstream: a picture's worth of samples twice over, at least 2 MiB;
 * the slice controls: an entry for each macroblock). */
static UINT video_buffer_size(const VideoDecoder *v, UINT type) {
  const UINT64 w = v->desc.SampleWidth, h = v->desc.SampleHeight, mbs = ((w + 15) / 16) * ((h + 15) / 16);
  UINT64 n;
  switch (type) {
    case D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS: n = 4096; break;
    case D3D11_VIDEO_DECODER_BUFFER_MACROBLOCK_CONTROL: n = mbs * 64; break;
    case D3D11_VIDEO_DECODER_BUFFER_RESIDUAL_DIFFERENCE: n = mbs * 6 * 64 * 2; break;
    case D3D11_VIDEO_DECODER_BUFFER_DEBLOCKING_CONTROL: n = mbs * 16; break;
    case D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX: n = 4096; break;
    case D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL: n = (mbs > 4096 ? mbs : 4096) * 64; break;
    case D3D11_VIDEO_DECODER_BUFFER_BITSTREAM: n = w * h * 3; if (n < (2u << 20)) n = 2u << 20; break;
    case D3D11_VIDEO_DECODER_BUFFER_MOTION_VECTOR: n = mbs * 16 * 8; break;
    default: n = 4096; break;      /* (FILM_GRAIN) */
  }
  if (n < 4096) n = 4096;
  return n > (256u << 20) ? (256u << 20) : (UINT)n;
}
static HRESULT STDMETHODCALLTYPE vctx_GetDecoderBuffer(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, D3D11_VIDEO_DECODER_BUFFER_TYPE type, UINT *size, void **data) {
  Ctx *c = ctx_of_video(p);
  VideoDecoder *v = vdec_of(decoder);
  if (size) *size = 0;
  if (data) *data = NULL;
  if (!v || !size || !data || (UINT)type >= VIDEO_BUFFER_TYPES) return E_INVALIDARG;
  HRESULT hr = S_OK;
  LOCK(c);
  if (v->held[type]) hr = DXGI_ERROR_INVALID_CALL;      /* (handed out already) */
  else {
    if (!v->buffers[type]) {
      v->sizes[type] = video_buffer_size(v, type);
      v->buffers[type] = mem_alloc(v->sizes[type]);
      if (!v->buffers[type]) hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
      v->held[type] = TRUE;
      *size = v->sizes[type];
      *data = v->buffers[type];
    }
  }
  UNLOCK(c);
  return hr;
}
static HRESULT STDMETHODCALLTYPE vctx_ReleaseDecoderBuffer(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, D3D11_VIDEO_DECODER_BUFFER_TYPE type) {
  Ctx *c = ctx_of_video(p);
  VideoDecoder *v = vdec_of(decoder);
  if (!v || (UINT)type >= VIDEO_BUFFER_TYPES) return E_INVALIDARG;
  LOCK(c);
  const BOOL held = v->held[type];
  v->held[type] = FALSE;
  UNLOCK(c);
  return held ? S_OK : DXGI_ERROR_INVALID_CALL;
}

/* A picture into a decoder output view. No content key: there is no encryption. */
static HRESULT vdec_begin(Ctx *c, VideoDecoder *v, ID3D11VideoDecoderOutputView *view, UINT key_size, const void *key) {
  VideoView *o = vview_of(view, VVIEW_DECODER_OUTPUT);
  if (!v || !o || o->c.dev != v->c.dev || key_size || key) return E_INVALIDARG;
  /* (the view is of the decoder's profile, its texture of the decoder's format and at least its size, every slice of it a surface index) */
  if (!guid_eq(&o->desc.dov.DecodeProfile, &v->desc.Guid) || o->tex->desc.Format != v->desc.OutputFormat || o->tex->desc.Width < v->desc.SampleWidth ||
      o->tex->desc.Height < v->desc.SampleHeight || o->tex->desc.ArraySize > VIDEO_SURFACES)
    return E_INVALIDARG;
  HRESULT hr = S_OK;
  LOCK(c);
  if (v->output) hr = DXGI_ERROR_INVALID_CALL;      /* (a picture is begun and not ended) */
  else {
    hold(o);
    v->output = o;
    v->picture.size = v->matrix.size = v->slices.size = v->bits.size = 0;
    v->have_matrix = v->have_bits = FALSE;
  }
  UNLOCK(c);
  return hr;
}
static HRESULT STDMETHODCALLTYPE vctx_DecoderBeginFrame(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, ID3D11VideoDecoderOutputView *view, UINT key_size, const void *key) {
  return vdec_begin(ctx_of_video(p), vdec_of(decoder), view, key_size, key);
}
/* (with histograms: there are none - D3D11_FEATURE_VIDEO_DECODER_HISTOGRAM reports no components) */
static HRESULT STDMETHODCALLTYPE vctx_DecoderBeginFrame1(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, ID3D11VideoDecoderOutputView *view, UINT key_size, const void *key,
                                                         UINT histograms, const UINT *offsets, ID3D11Buffer *const *buffers) {
  (void)offsets; (void)buffers;
  if (histograms) return E_INVALIDARG;
  return vdec_begin(ctx_of_video(p), vdec_of(decoder), view, key_size, key);
}

/* One buffer of a SubmitDecoderBuffers call, whichever of the three descriptions it came in. */
typedef struct VideoSubmitted { UINT type, offset, size; BOOL encrypted; } VideoSubmitted;
/* Gathers what a SubmitDecoderBuffers call gives for the picture begun: the picture parameters and quantization matrices (the last given), the slice controls and the
 * bitstream one after the other - a slice control's offset into the bitstream moved by where that call's bitstream went. Each type at most once a call; only the
 * types a VLD decoder takes; nothing encrypted (there is no encryption). Under the device's lock. */
static HRESULT vdec_submit(VideoDecoder *v, const VideoSubmitted *b, UINT count) {
  if (!v->output) return DXGI_ERROR_INVALID_CALL;      /* (no picture begun) */
  UINT seen = 0;
  for (UINT i = 0; i < count; ++i) {
    const UINT t = b[i].type;
    if (t >= VIDEO_BUFFER_TYPES || (seen >> t & 1) || b[i].encrypted) return E_INVALIDARG;
    seen |= 1u << t;
    if (t != D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS && t != D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX && t != D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL &&
        t != D3D11_VIDEO_DECODER_BUFFER_BITSTREAM)
      return E_INVALIDARG;      /* (the older acceleration modes' buffers: no profile here is one of those) */
    if (v->held[t] || !v->buffers[t] || b[i].offset > v->sizes[t] || b[i].size > v->sizes[t] - b[i].offset) return v->held[t] ? DXGI_ERROR_INVALID_CALL : E_INVALIDARG;
  }
  /* (the bitstream first: the slice controls of the same call are of it) */
  for (UINT i = 0; i < count; ++i) {
    if (b[i].type != D3D11_VIDEO_DECODER_BUFFER_BITSTREAM) continue;
    v->bits_rebase = (UINT64)v->bits.size - b[i].offset;      /* (an offset into the buffer, less the DataOffset, plus where its bytes go: modulo 2^64) */
    if (!bytes_append(&v->bits, v->buffers[b[i].type] + b[i].offset, b[i].size)) return E_OUTOFMEMORY;
    v->have_bits = TRUE;
  }
  for (UINT i = 0; i < count; ++i) {
    const BYTE *data = v->buffers[b[i].type] + b[i].offset;
    switch (b[i].type) {
      case D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS: if (!bytes_set(&v->picture, data, b[i].size)) return E_OUTOFMEMORY; break;
      case D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX: if (!bytes_set(&v->matrix, data, b[i].size)) return E_OUTOFMEMORY; v->have_matrix = TRUE; break;
      case D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL: {
        const SIZE_T at = v->slices.size;
        if (!bytes_append(&v->slices, data, b[i].size)) return E_OUTOFMEMORY;
        /* (each whole entry's offset into the bitstream, moved; an entry that pointed before the buffer's data keeps a value the decoder refuses) */
        for (SIZE_T e = at; e + v->codec->slice_bytes <= v->slices.size; e += v->codec->slice_bytes) {
          UINT location;
          memcpy(&location, v->slices.data + e + v->codec->slice_location, 4);
          const UINT64 moved = (UINT64)location + v->bits_rebase;
          location = moved > 0xFFFFFFFFull ? 0xFFFFFFFFu : (UINT)moved;
          memcpy(v->slices.data + e + v->codec->slice_location, &location, 4);
        }
        break;
      }
      default: break;
    }
  }
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE vctx_SubmitDecoderBuffers(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, UINT count, const D3D11_VIDEO_DECODER_BUFFER_DESC *descs) {
  Ctx *c = ctx_of_video(p);
  VideoDecoder *v = vdec_of(decoder);
  VideoSubmitted b[VIDEO_BUFFER_TYPES];
  if (!v || (count && !descs) || count > VIDEO_BUFFER_TYPES) return E_INVALIDARG;
  for (UINT i = 0; i < count; ++i) {
    b[i].type = descs[i].BufferType;
    b[i].offset = descs[i].DataOffset;
    b[i].size = descs[i].DataSize;
    b[i].encrypted = descs[i].pIV || descs[i].IVSize || descs[i].PartialEncryption;
  }
  LOCK(c);
  const HRESULT hr = vdec_submit(v, b, count);
  UNLOCK(c);
  return hr;
}
static HRESULT STDMETHODCALLTYPE vctx_SubmitDecoderBuffers1(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, UINT count, const D3D11_VIDEO_DECODER_BUFFER_DESC1 *descs) {
  Ctx *c = ctx_of_video(p);
  VideoDecoder *v = vdec_of(decoder);
  VideoSubmitted b[VIDEO_BUFFER_TYPES];
  if (!v || (count && !descs) || count > VIDEO_BUFFER_TYPES) return E_INVALIDARG;
  for (UINT i = 0; i < count; ++i) {
    b[i].type = descs[i].BufferType;
    b[i].offset = descs[i].DataOffset;
    b[i].size = descs[i].DataSize;
    b[i].encrypted = descs[i].pIV || descs[i].IVSize || descs[i].pSubSampleMappingBlock || descs[i].SubSampleMappingCount;
  }
  LOCK(c);
  const HRESULT hr = vdec_submit(v, b, count);
  UNLOCK(c);
  return hr;
}
static HRESULT STDMETHODCALLTYPE vctx_SubmitDecoderBuffers2(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, UINT count, const D3D11_VIDEO_DECODER_BUFFER_DESC2 *descs) {
  Ctx *c = ctx_of_video(p);
  VideoDecoder *v = vdec_of(decoder);
  VideoSubmitted b[VIDEO_BUFFER_TYPES];
  if (!v || (count && !descs) || count > VIDEO_BUFFER_TYPES) return E_INVALIDARG;
  for (UINT i = 0; i < count; ++i) {
    b[i].type = descs[i].BufferType;
    b[i].offset = descs[i].DataOffset;
    b[i].size = descs[i].DataSize;
    b[i].encrypted = descs[i].pIV || descs[i].IVSize || descs[i].pSubSampleMappingBlock || descs[i].SubSampleMappingCount || descs[i].cBlocksStripeEncrypted;
  }
  LOCK(c);
  const HRESULT hr = vdec_submit(v, b, count);
  UNLOCK(c);
  return hr;
}

/* The picture's status report, from its picture parameters and Direct3D 12's decode statistics. */
static void vdec_status_push(VideoDecoder *v, const D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS *stats) {
  const VideoCodec *k = v->codec;
  if (k->status == VSTATUS_NONE) return;
  VideoStatus s;
  memset(&s, 0, sizeof s);
  const BYTE *pp = v->picture.data;
  if (v->picture.size >= k->pp_feedback + 4) memcpy(&s.feedback, pp + k->pp_feedback, 4);
  if (v->picture.size > k->pp_curr) s.curr = pp[k->pp_curr];
  if (k->status == VSTATUS_H264 && v->picture.size >= offsetof(DXVA_PicParams_H264, wBitFields) + 2) {
    USHORT bits;
    memcpy(&bits, pp + offsetof(DXVA_PicParams_H264, wBitFields), 2);
    s.field = (BYTE)(bits & 1);      /* (field_pic_flag) */
  }
  if (k->status == VSTATUS_AV1 && v->picture.size >= 8) { memcpy(&s.width, pp, 4); memcpy(&s.height, pp + 4, 4); }
  s.status = stats->Status > 4 ? 4 : (BYTE)stats->Status;
  s.macroblocks = stats->NumMacroblocksAffected >= 0xFFFF ? 0xFFFF : (USHORT)stats->NumMacroblocksAffected;
  if (v->status_count == VIDEO_STATUS_KEPT) { memmove(v->status, v->status + 1, sizeof v->status[0] * (VIDEO_STATUS_KEPT - 1)); --v->status_count; }
  v->status[v->status_count++] = s;
}
/* Sends the picture: one DecodeFrame of the gathered buffers into the output view's slice, the references every slice of its texture (the DXVA indices are the slices),
 * after what the immediate context recorded. A picture that could not be decoded is in its status report; one that was not described (no picture parameters or no
 * bitstream) is E_INVALIDARG. */
static HRESULT STDMETHODCALLTYPE vctx_DecoderEndFrame(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder) {
  Ctx *c = ctx_of_video(p);
  VideoDecoder *v = vdec_of(decoder);
  if (!v) return E_INVALIDARG;
  Dev *d = c->dev;
  LOCK(c);
  if (!v->output) { UNLOCK(c); return DXGI_ERROR_INVALID_CALL; }
  HRESULT hr = v->picture.size && v->have_bits ? S_OK : E_INVALIDARG;
  VideoState *vs = d->video_state;
  Tex *t = v->output->tex;
  ID3D12Resource *upload = NULL;
  BYTE *memory = NULL;
  UINT64 upload_size = 0;
  if (SUCCEEDED(hr)) {
    upload = take_upload(d, v->bits.size ? v->bits.size : 16, &memory, &upload_size);
    if (!upload) hr = E_OUTOFMEMORY;
    else {
      memcpy(memory, v->bits.data, v->bits.size);
      ID3D12Resource_Unmap(upload, 0, NULL);      /* (marks it changed: d3d12.dll sends it with the list) */
    }
  }
  if (SUCCEEDED(hr) && !vs->statistics) {
    const D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_VIDEO_DECODE_STATISTICS, 1, 0};
    if (FAILED(ID3D12Device_CreateQueryHeap(d->d12, &qd, &IID_ID3D12QueryHeap, (void **)&vs->statistics))) vs->statistics = NULL;
    else if (!(vs->statistics_readback = make_buffer(d, D3D12_HEAP_TYPE_READBACK, sizeof(D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS)))) hr = E_OUTOFMEMORY;
  }
  if (SUCCEEDED(hr) && !video_queue_begin(d, &vs->decode, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, &IID_ID3D12VideoDecodeCommandList)) hr = E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) {
    ID3D12VideoDecodeCommandList *list = (ID3D12VideoDecodeCommandList *)vs->decode.list;
    ID3D12Resource *refs[VIDEO_SURFACES];
    UINT subs[VIDEO_SURFACES];
    for (UINT i = 0; i < t->desc.ArraySize; ++i) { refs[i] = t->res; subs[i] = i; }
    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS in;
    memset(&in, 0, sizeof in);
    in.FrameArguments[in.NumFrameArguments++] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, (UINT)v->picture.size, v->picture.data};
    if (v->have_matrix && v->matrix.size)
      in.FrameArguments[in.NumFrameArguments++] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, (UINT)v->matrix.size, v->matrix.data};
    if (v->slices.size)
      in.FrameArguments[in.NumFrameArguments++] = (D3D12_VIDEO_DECODE_FRAME_ARGUMENT){D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, (UINT)v->slices.size, v->slices.data};
    in.ReferenceFrames.NumTexture2Ds = t->desc.ArraySize;
    in.ReferenceFrames.ppTexture2Ds = refs;
    in.ReferenceFrames.pSubresources = subs;
    in.CompressedBitstream.pBuffer = upload;
    in.CompressedBitstream.Offset = 0;
    in.CompressedBitstream.Size = v->bits.size;
    in.pHeap = v->heap12;
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS out;
    memset(&out, 0, sizeof out);
    out.pOutputTexture2D = t->res;
    out.OutputSubresource = v->output->slice;
    ID3D12VideoDecodeCommandList_DecodeFrame(list, v->dec12, &out, &in);
    if (vs->statistics) {
      ID3D12VideoDecodeCommandList_EndQuery(list, vs->statistics, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, 0);
      ID3D12VideoDecodeCommandList_ResolveQueryData(list, vs->statistics, D3D12_QUERY_TYPE_VIDEO_DECODE_STATISTICS, 0, 1, vs->statistics_readback, 0);
    }
    tex_sync(d, t);
    video_queue_run(d, &vs->decode);
    ++t->version;      /* (a render target view's own copy of it is loaded again) */
    D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS stats;
    memset(&stats, 0, sizeof stats);
    stats.NumMacroblocksAffected = 0xFFFF;      /* (no estimate when there are no statistics) */
    const BYTE *read = NULL;
    if (vs->statistics && SUCCEEDED(ID3D12Resource_Map(vs->statistics_readback, 0, NULL, (void **)&read)) && read) {
      memcpy(&stats, read, sizeof stats);
      ID3D12Resource_Unmap(vs->statistics_readback, 0, NULL);
    }
    vdec_status_push(v, &stats);
  }
  if (upload) spare_keep(d, (Spare){upload, NULL, upload_size, 0});      /* (the decode has run: the memory may be used again) */
  vdec_frame_clear(v);
  UNLOCK(c);
  return hr;
}

/* The decoder's extension functions: status reporting (DXVA_STATUS_REPORTING_FUNCTION), which hands out the reports of the pictures decoded since the last call - the
 * newest first, as many as the output holds, the rest of it zeros (all zeros when there are none) - and drops those it gave. E_FAIL when the output does not hold one
 * report, or the profile has none (MPEG-2). No other function is had. */
static HRESULT STDMETHODCALLTYPE vctx_DecoderExtension(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, const D3D11_VIDEO_DECODER_EXTENSION *ext) {
  Ctx *c = ctx_of_video(p);
  VideoDecoder *v = vdec_of(decoder);
  if (!v || !ext) return E_INVALIDARG;
  if (ext->Function != DXVA_STATUS_REPORTING_FUNCTION) return E_INVALIDARG;
  const UINT one = video_status_bytes(v->codec->status);
  if (!one || !ext->pPrivateOutputData || ext->PrivateOutputDataSize < one) return E_FAIL;
  LOCK(c);
  BYTE *outp = ext->pPrivateOutputData;
  memset(outp, 0, ext->PrivateOutputDataSize);
  const UINT room = ext->PrivateOutputDataSize / one, n = room < v->status_count ? room : v->status_count;
  for (UINT i = 0; i < n; ++i) {
    const VideoStatus *s = &v->status[v->status_count - 1 - i];
    BYTE *at = outp + (SIZE_T)i * one;
    switch (v->codec->status) {
      case VSTATUS_H264: {
        DXVA_Status_H264 r;
        memset(&r, 0, sizeof r);
        r.StatusReportFeedbackNumber = s->feedback;
        r.CurrPic.bPicEntry = s->curr;
        r.field_pic_flag = s->field;
        r.bDXVA_Func = DXVA_PICTURE_DECODING_FUNCTION;
        r.bBufType = 0xFF;      /* (not a particular buffer) */
        r.bStatus = s->status;
        r.wNumMbsAffected = s->macroblocks;
        memcpy(at, &r, sizeof r);
        break;
      }
      case VSTATUS_HEVC: {
        VideoStatusHevc r = {(USHORT)s->feedback, s->curr, 0xFF, s->status, 0, s->macroblocks};
        memcpy(at, &r, sizeof r);
        break;
      }
      case VSTATUS_VPX: {
        DXVA_Status_VPx r;
        memset(&r, 0, sizeof r);
        r.StatusReportFeedbackNumber = s->feedback;
        r.CurrPic.bPicEntry = s->curr;
        r.bBufType = 0xFF;
        r.bStatus = s->status;
        r.wNumMbsAffected = s->macroblocks;
        memcpy(at, &r, sizeof r);
        break;
      }
      case VSTATUS_AV1: {
        DXVA_Status_AV1 r;
        memset(&r, 0, sizeof r);
        r.StatusReportFeedbackNumber = s->feedback;
        r.CurrPic.Index = s->curr;
        r.CurrPic.width = s->width;
        r.CurrPic.height = s->height;
        r.BufType = 0xFF;
        r.Status = s->status;
        r.NumMbsAffected = s->macroblocks;
        memcpy(at, &r, sizeof r);
        break;
      }
      default: break;
    }
  }
  v->status_count -= n;
  UNLOCK(c);
  return S_OK;
}
/* Downsampling while decoding: the decoder has none (CheckVideoDecoderDownsampling says so). */
static HRESULT STDMETHODCALLTYPE vctx_DecoderEnableDownsampling(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, DXGI_COLOR_SPACE_TYPE in_space,
                                                                const D3D11_VIDEO_SAMPLE_DESC *out_desc, UINT reference_frames) {
  (void)p; (void)decoder; (void)in_space; (void)out_desc; (void)reference_frames;
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE vctx_DecoderUpdateDownsampling(ID3D11VideoContext3 *p, ID3D11VideoDecoder *decoder, const D3D11_VIDEO_SAMPLE_DESC *out_desc) {
  (void)p; (void)decoder; (void)out_desc;
  return E_INVALIDARG;      /* (downsampling was not enabled: it cannot be) */
}

/* ---- ID3D11VideoContext: the processor's output state --------------------------------------------------------------------------------------------------------------- */

#define VPROC_OR_RETURN(name) VideoProcessor *name = vproc_of(processor); if (!name) return
static void STDMETHODCALLTYPE vctx_VideoProcessorSetOutputTargetRect(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL enable, const RECT *rect) {
  (void)p;
  VPROC_OR_RETURN(v);
  v->target_enable = enable;
  if (enable && rect) v->target = *rect;
  if (!enable) memset(&v->target, 0, sizeof v->target);
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetOutputTargetRect(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL *enabled, RECT *rect) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (enabled) *enabled = v->target_enable;
  if (rect) *rect = v->target;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetOutputBackgroundColor(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL ycbcr, const D3D11_VIDEO_COLOR *colour) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (!colour) return;
  v->background_ycbcr = ycbcr;
  v->background = *colour;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetOutputBackgroundColor(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL *ycbcr, D3D11_VIDEO_COLOR *colour) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (ycbcr) *ycbcr = v->background_ycbcr;
  if (colour) *colour = v->background;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetOutputColorSpace(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *space) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (!space) return;
  v->space = *space;
  v->use_space1 = FALSE;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetOutputColorSpace(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, D3D11_VIDEO_PROCESSOR_COLOR_SPACE *space) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (space) *space = v->space;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetOutputColorSpace1(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, DXGI_COLOR_SPACE_TYPE space) {
  (void)p;
  VPROC_OR_RETURN(v);
  v->space1 = space;
  v->use_space1 = TRUE;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetOutputColorSpace1(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, DXGI_COLOR_SPACE_TYPE *space) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (space) *space = v->space1;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetOutputAlphaFillMode(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE mode, UINT stream) {
  (void)p;
  VPROC_OR_RETURN(v);
  if ((UINT)mode > D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_SOURCE_STREAM || (mode == D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_SOURCE_STREAM && stream >= VIDEO_STREAMS)) return;
  v->fill = mode;
  v->fill_stream = mode == D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_SOURCE_STREAM ? stream : 0;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetOutputAlphaFillMode(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE *mode, UINT *stream) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (mode) *mode = v->fill;
  if (stream) *stream = v->fill_stream;
}
/* (kept; a Blt with constriction on is refused: FeatureCaps has no CONSTRICTION) */
static void STDMETHODCALLTYPE vctx_VideoProcessorSetOutputConstriction(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL enable, SIZE size) {
  (void)p;
  VPROC_OR_RETURN(v);
  v->constriction = enable;
  v->constriction_size = enable ? size : (SIZE){0, 0};
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetOutputConstriction(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL *enabled, SIZE *size) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (enabled) *enabled = v->constriction;
  if (size) *size = v->constriction_size;
}
/* (kept; a Blt with stereo on is refused: FeatureCaps has no STEREO) */
static void STDMETHODCALLTYPE vctx_VideoProcessorSetOutputStereoMode(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL enable) {
  (void)p;
  VPROC_OR_RETURN(v);
  v->stereo = enable;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetOutputStereoMode(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL *enabled) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (enabled) *enabled = v->stereo;
}
/* (no extensions: no GUID is one the processor knows) */
static HRESULT STDMETHODCALLTYPE vctx_VideoProcessorSetOutputExtension(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, const GUID *guid, UINT size, void *data) {
  (void)p; (void)processor; (void)guid; (void)size; (void)data;
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE vctx_VideoProcessorGetOutputExtension(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, const GUID *guid, UINT size, void *data) {
  (void)p; (void)processor; (void)guid; (void)size; (void)data;
  return E_INVALIDARG;
}
/* (the output is always one shaders may read: a hint the processor keeps) */
static void STDMETHODCALLTYPE vctx_VideoProcessorSetOutputShaderUsage(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL usage) {
  (void)p;
  VPROC_OR_RETURN(v);
  v->shader_usage = usage;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetOutputShaderUsage(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, BOOL *usage) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (usage) *usage = v->shader_usage;
}
/* HDR metadata: kept and given back (no tone mapping is done with it). A type and the size of its structure (NONE: none). */
static BOOL hdr_size_ok(DXGI_HDR_METADATA_TYPE type, UINT size, const void *data) {
  switch (type) {
    case DXGI_HDR_METADATA_TYPE_NONE: return size == 0;
    case DXGI_HDR_METADATA_TYPE_HDR10: return size == sizeof(DXGI_HDR_METADATA_HDR10) && data;
    case DXGI_HDR_METADATA_TYPE_HDR10PLUS: return size && size <= VIDEO_HDR_BYTES && data;
    default: return FALSE;
  }
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetOutputHDRMetaData(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, DXGI_HDR_METADATA_TYPE type, UINT size, const void *data) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (!hdr_size_ok(type, size, data)) return;
  v->hdr_type = type;
  v->hdr_size = size;
  if (size) memcpy(v->hdr, data, size);
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetOutputHDRMetaData(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, DXGI_HDR_METADATA_TYPE *type, UINT size, void *data) {
  (void)p;
  VPROC_OR_RETURN(v);
  if (type) *type = v->hdr_type;
  if (data && size >= v->hdr_size) memcpy(data, v->hdr, v->hdr_size);
}

/* ---- ID3D11VideoContext: the streams' state ----------------------------------------------------------------------------------------------------------------------- */

#define STREAM_OR_RETURN(name) VideoProcessor *proc_ = vproc_of(processor); if (!proc_ || stream >= VIDEO_STREAMS) return; VideoStreamState *name = &proc_->streams[stream]
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamFrameFormat(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, D3D11_VIDEO_FRAME_FORMAT format) {
  (void)p;
  STREAM_OR_RETURN(s);
  if ((UINT)format <= D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST) s->frame_format = format;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamFrameFormat(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, D3D11_VIDEO_FRAME_FORMAT *format) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (format) *format = s->frame_format;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamColorSpace(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *space) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (!space) return;
  s->space = *space;
  s->use_space1 = FALSE;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamColorSpace(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, D3D11_VIDEO_PROCESSOR_COLOR_SPACE *space) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (space) *space = s->space;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamColorSpace1(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, DXGI_COLOR_SPACE_TYPE space) {
  (void)p;
  STREAM_OR_RETURN(s);
  s->space1 = space;
  s->use_space1 = TRUE;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamColorSpace1(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, DXGI_COLOR_SPACE_TYPE *space) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (space) *space = s->space1;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamOutputRate(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE rate,
                                                                    BOOL repeat, const DXGI_RATIONAL *custom) {
  (void)p;
  STREAM_OR_RETURN(s);
  if ((UINT)rate > D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_CUSTOM || (rate == D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_CUSTOM && !custom)) return;
  s->rate = rate;
  s->repeat = repeat;
  if (custom) s->custom_rate = *custom; else memset(&s->custom_rate, 0, sizeof s->custom_rate);
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamOutputRate(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE *rate,
                                                                    BOOL *repeat, DXGI_RATIONAL *custom) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (rate) *rate = s->rate;
  if (repeat) *repeat = s->repeat;
  if (custom) *custom = s->custom_rate;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamSourceRect(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL enable, const RECT *rect) {
  (void)p;
  STREAM_OR_RETURN(s);
  s->source_enable = enable;
  if (enable && rect) s->source = *rect;
  if (!enable) memset(&s->source, 0, sizeof s->source);
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamSourceRect(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL *enabled, RECT *rect) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enabled) *enabled = s->source_enable;
  if (rect) *rect = s->source;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamDestRect(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL enable, const RECT *rect) {
  (void)p;
  STREAM_OR_RETURN(s);
  s->dest_enable = enable;
  if (enable && rect) s->dest = *rect;
  if (!enable) memset(&s->dest, 0, sizeof s->dest);
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamDestRect(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL *enabled, RECT *rect) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enabled) *enabled = s->dest_enable;
  if (rect) *rect = s->dest;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamAlpha(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL enable, FLOAT alpha) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enable && !(alpha >= 0.0f && alpha <= 1.0f)) return;      /* (planar alpha is 0 .. 1) */
  s->alpha_enable = enable;
  s->alpha = enable ? alpha : 1.0f;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamAlpha(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL *enabled, FLOAT *alpha) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enabled) *enabled = s->alpha_enable;
  if (alpha) *alpha = s->alpha;
}
/* (a palette is for the palettized formats - AI44, IA44, P8, A8P8 -, which the processor does not read: kept and given back) */
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamPalette(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, UINT count, const UINT *entries) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (count > 256 || (count && !entries)) return;
  s->palette_count = count;
  if (count) memcpy(s->palette, entries, sizeof *entries * count);
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamPalette(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, UINT count, UINT *entries) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (!entries) return;
  for (UINT i = 0; i < count && i < 256; ++i) entries[i] = i < s->palette_count ? s->palette[i] : 0;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamPixelAspectRatio(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL enable,
                                                                          const DXGI_RATIONAL *source, const DXGI_RATIONAL *dest) {
  (void)p;
  STREAM_OR_RETURN(s);
  s->aspect_enable = enable;
  const DXGI_RATIONAL one = {1, 1};
  s->aspect_source = enable && source ? *source : one;
  s->aspect_dest = enable && dest ? *dest : one;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamPixelAspectRatio(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL *enabled,
                                                                          DXGI_RATIONAL *source, DXGI_RATIONAL *dest) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enabled) *enabled = s->aspect_enable;
  if (source) *source = s->aspect_source;
  if (dest) *dest = s->aspect_dest;
}
/* (kept; a Blt with a luma key on is refused: FeatureCaps has no LUMA_KEY) */
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamLumaKey(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL enable, FLOAT lower, FLOAT upper) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enable && !(lower >= 0.0f && lower <= upper && upper <= 1.0f)) return;
  s->luma_enable = enable;
  s->luma_lower = enable ? lower : 0.0f;
  s->luma_upper = enable ? upper : 0.0f;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamLumaKey(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL *enabled, FLOAT *lower, FLOAT *upper) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enabled) *enabled = s->luma_enable;
  if (lower) *lower = s->luma_lower;
  if (upper) *upper = s->luma_upper;
}
/* (kept; a Blt of a stereo format other than mono is refused: there is no stereo) */
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamStereoFormat(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL enable,
                                                                      D3D11_VIDEO_PROCESSOR_STEREO_FORMAT format, BOOL left_frame0, BOOL base_frame0,
                                                                      D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE flip, int mono_offset) {
  (void)p;
  STREAM_OR_RETURN(s);
  if ((UINT)format > D3D11_VIDEO_PROCESSOR_STEREO_FORMAT_CHECKERBOARD || (UINT)flip > D3D11_VIDEO_PROCESSOR_STEREO_FLIP_FRAME1) return;
  s->stereo_enable = enable;
  s->stereo_format = format;
  s->left_frame0 = left_frame0;
  s->base_frame0 = base_frame0;
  s->flip_mode = flip;
  s->mono_offset = mono_offset;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamStereoFormat(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL *enabled,
                                                                      D3D11_VIDEO_PROCESSOR_STEREO_FORMAT *format, BOOL *left_frame0, BOOL *base_frame0,
                                                                      D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE *flip, int *mono_offset) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enabled) *enabled = s->stereo_enable;
  if (format) *format = s->stereo_format;
  if (left_frame0) *left_frame0 = s->left_frame0;
  if (base_frame0) *base_frame0 = s->base_frame0;
  if (flip) *flip = s->flip_mode;
  if (mono_offset) *mono_offset = s->mono_offset;
}
/* (the processor has no automatic processing - AutoStreamCaps 0 -: on or off, nothing is done) */
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamAutoProcessingMode(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL enable) {
  (void)p;
  STREAM_OR_RETURN(s);
  s->auto_processing = enable;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamAutoProcessingMode(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL *enabled) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enabled) *enabled = s->auto_processing;
}
/* (kept; a Blt with a filter on is refused: FilterCaps 0) */
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamFilter(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, D3D11_VIDEO_PROCESSOR_FILTER filter, BOOL enable,
                                                                int level) {
  (void)p;
  STREAM_OR_RETURN(s);
  if ((UINT)filter > D3D11_VIDEO_PROCESSOR_FILTER_STEREO_ADJUSTMENT) return;
  s->filter_enable[filter] = enable;
  s->filter_level[filter] = level;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamFilter(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, D3D11_VIDEO_PROCESSOR_FILTER filter, BOOL *enabled,
                                                                int *level) {
  (void)p;
  STREAM_OR_RETURN(s);
  if ((UINT)filter > D3D11_VIDEO_PROCESSOR_FILTER_STEREO_ADJUSTMENT) return;
  if (enabled) *enabled = s->filter_enable[filter];
  if (level) *level = s->filter_level[filter];
}
static HRESULT STDMETHODCALLTYPE vctx_VideoProcessorSetStreamExtension(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, const GUID *guid, UINT size, void *data) {
  (void)p; (void)processor; (void)stream; (void)guid; (void)size; (void)data;
  return E_INVALIDARG;      /* (no extensions) */
}
static HRESULT STDMETHODCALLTYPE vctx_VideoProcessorGetStreamExtension(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, const GUID *guid, UINT size, void *data) {
  (void)p; (void)processor; (void)stream; (void)guid; (void)size; (void)data;
  return E_INVALIDARG;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamRotation(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL enable, D3D11_VIDEO_PROCESSOR_ROTATION rotation) {
  (void)p;
  STREAM_OR_RETURN(s);
  if ((UINT)rotation > D3D11_VIDEO_PROCESSOR_ROTATION_270) return;
  s->rotation_enable = enable;
  s->rotation = enable ? rotation : D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamRotation(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL *enabled, D3D11_VIDEO_PROCESSOR_ROTATION *rotation) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enabled) *enabled = s->rotation_enable;
  if (rotation) *rotation = s->rotation;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamMirror(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL enable, BOOL horizontal, BOOL vertical) {
  (void)p;
  STREAM_OR_RETURN(s);
  s->mirror_enable = enable;
  s->flip_horizontal = enable && horizontal;
  s->flip_vertical = enable && vertical;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamMirror(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, BOOL *enabled, BOOL *horizontal, BOOL *vertical) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (enabled) *enabled = s->mirror_enable;
  if (horizontal) *horizontal = s->flip_horizontal;
  if (vertical) *vertical = s->flip_vertical;
}
static void STDMETHODCALLTYPE vctx_VideoProcessorSetStreamHDRMetaData(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, DXGI_HDR_METADATA_TYPE type, UINT size,
                                                                     const void *data) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (!hdr_size_ok(type, size, data)) return;
  s->hdr_type = type;
  s->hdr_size = size;
  if (size) memcpy(s->hdr, data, size);
}
static void STDMETHODCALLTYPE vctx_VideoProcessorGetStreamHDRMetaData(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT stream, DXGI_HDR_METADATA_TYPE *type, UINT size,
                                                                     void *data) {
  (void)p;
  STREAM_OR_RETURN(s);
  if (type) *type = s->hdr_type;
  if (data && size >= s->hdr_size) memcpy(data, s->hdr, s->hdr_size);
}
/* No multiplane overlay below: no behaviour to hint at. */
static HRESULT STDMETHODCALLTYPE vctx_VideoProcessorGetBehaviorHints(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, UINT width, UINT height, DXGI_FORMAT format, UINT count,
                                                                    const D3D11_VIDEO_PROCESSOR_STREAM_BEHAVIOR_HINT *streams, UINT *hints) {
  (void)p; (void)width; (void)height;
  if (!vproc_of(processor) || !hints || (count && !streams) || count > VIDEO_STREAMS || !vp_format_ok(format)) return E_INVALIDARG;
  *hints = 0;
  return S_OK;
}

/* ---- VideoProcessorBlt ----------------------------------------------------------------------------------------------------------------------------------------------- */

/* Whether the processor below takes a subresource of a texture as it is: a planar texture's slice, or a 2D texture of one level (not an array, not drawn through a target
 * of its own). Any other is processed by way of a copy (vp_scratch). */
static BOOL vp_direct(const Tex *t) { return t->planar || (t->desc.MipLevels == 1 && t->desc.ArraySize == 1 && t->kind == 0 && !t->shadowed && t->stored == t->desc.Format); }
/* A texture of one level and slice the size of a subresource of `t` (in its format), holding what the subresource has when `load`. */
static HRESULT vp_scratch(Dev *d, Tex *t, UINT sub, BOOL load, Tex **out) {
  *out = NULL;
  const UINT level = sub % t->desc.MipLevels, w = mip_size(t->desc.Width, level), h = mip_size(t->desc.Height, level);
  const D3D11_TEXTURE2D_DESC desc = {w, h, 1, 1, t->desc.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  Tex *s = NULL;
  HRESULT hr = tex_make(d, &desc, 0, NULL, &s);
  if (FAILED(hr)) return hr;
  if (load) {
    const SIZE_T bytes = (SIZE_T)tex_pitch(t, w) * h;
    BYTE *rows = mem_alloc(bytes);
    if (!rows) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = tex_read(d, t, sub, 0, rows);
    if (SUCCEEDED(hr)) hr = tex_write(d, s, 0, 0, 0, 0, w, h, rows, tex_pitch(t, w));
    mem_free(rows);
  }
  if (FAILED(hr)) { dx_Release(OBJ(s)); return hr; }
  *out = s;
  return S_OK;
}
/* What a scratch texture holds into the subresource it stands for. */
static void vp_scratch_store(Dev *d, Tex *s, Tex *t, UINT sub) {
  const UINT w = s->desc.Width, h = s->desc.Height;
  BYTE *rows = mem_alloc((SIZE_T)tex_pitch(t, w) * h);
  if (rows && SUCCEEDED(tex_read(d, s, 0, 0, rows))) tex_write(d, t, sub, 0, 0, 0, w, h, rows, tex_pitch(t, w));
  mem_free(rows);
}
/* The size of the picture a video view names (a level's; a planar texture's whole). */
static void vp_view_size(const VideoView *v, UINT *w, UINT *h) {
  *w = mip_size(v->tex->desc.Width, v->mip);
  *h = mip_size(v->tex->desc.Height, v->mip);
}
/* A rectangle clipped to (0, 0, w, h) (an empty one stays empty). */
static RECT vp_clip(RECT r, UINT w, UINT h) {
  if (r.left < 0) r.left = 0;
  if (r.top < 0) r.top = 0;
  if (r.right > (LONG)w) r.right = (LONG)w;
  if (r.bottom > (LONG)h) r.bottom = (LONG)h;
  if (r.right < r.left) r.right = r.left;
  if (r.bottom < r.top) r.bottom = r.top;
  return r;
}
/* The Direct3D 12 processor of the formats, colour spaces and features of a Blt: one kept from an earlier Blt, or a new one (the least recently used goes). NULL when
 * Direct3D 12 refuses the combination. */
static ID3D12VideoProcessor *vp_processor(Dev *d, VideoProcessor *p, const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *out, const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *in, UINT count) {
  ++p->clock;
  for (UINT i = 0; i < VIDEO_PROCESSOR_CACHE; ++i) {
    VideoProcessorCache *k = &p->cache[i];
    if (k->processor && k->count == count && memcmp(&k->out, out, sizeof *out) == 0 && memcmp(k->in, in, sizeof *in * count) == 0) { k->used = p->clock; return k->processor; }
  }
  ID3D12VideoProcessor *made = NULL;
  if (FAILED(ID3D12VideoDevice_CreateVideoProcessor(d->video_state->v12, 0, out, count, in, &IID_ID3D12VideoProcessor, (void **)&made)) || !made) return NULL;
  VideoProcessorCache *slot = &p->cache[0];
  for (UINT i = 1; i < VIDEO_PROCESSOR_CACHE; ++i) if (!p->cache[i].processor || (slot->processor && p->cache[i].used < slot->used)) slot = &p->cache[i];
  if (slot->processor) ID3D12VideoProcessor_Release(slot->processor);
  slot->processor = made;
  slot->out = *out;
  memset(slot->in, 0, sizeof slot->in);
  memcpy(slot->in, in, sizeof *in * count);
  slot->count = count;
  slot->used = p->clock;
  return made;
}

/* The enabled streams into the output view: one ProcessFrames on a VIDEO_PROCESS queue after what the immediate context recorded. Each stream's source rectangle (all of
 * its picture when not enabled; clipped to it) goes to its destination rectangle (all of the output when not enabled) with its rotation and mirroring and its planar
 * alpha, over the background inside the target rectangle (all of the output when not enabled). E_INVALIDARG for what the processor does not do (see video.inc's head):
 * past or future frames, interlaced frames, stereo, luma keys, filters, constriction, a custom output rate, pixel aspect ratios that differ, xvYCC. */
static HRESULT STDMETHODCALLTYPE vctx_VideoProcessorBlt(ID3D11VideoContext3 *p, ID3D11VideoProcessor *processor, ID3D11VideoProcessorOutputView *view, UINT frame, UINT count,
                                                         const D3D11_VIDEO_PROCESSOR_STREAM *streams) {
  Ctx *c = ctx_of_video(p);
  Dev *d = c->dev;
  VideoProcessor *v = vproc_of(processor);
  VideoView *ov = vview_of(view, VVIEW_PROCESSOR_OUTPUT);
  (void)frame;
  if (!v || !ov || v->c.dev != d || ov->c.dev != d || !count || count > VIDEO_STREAMS || !streams) return E_INVALIDARG;
  if (v->stereo || v->constriction) return E_INVALIDARG;
  const DXGI_FORMAT out_format = ov->format;
  const BOOL out_yuv = vp_yuv(out_format);
  DXGI_COLOR_SPACE_TYPE out_space;
  if (v->use_space1) out_space = v->space1;
  else if (!vp_space_of(&v->space, out_format, &out_space)) return E_INVALIDARG;
  UINT ow, oh;
  vp_view_size(ov, &ow, &oh);

  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC od;
  memset(&od, 0, sizeof od);
  od.Format = out_format;
  od.ColorSpace = out_space;
  od.AlphaFillMode = (D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE)v->fill;      /* (the two enumerations are the same) */
  vp_background(v, out_space, out_yuv, od.BackgroundColor);
  od.FrameRate = v->enumerator->desc.OutputFrameRate;
  D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC id[VIDEO_STREAMS];
  D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS ia[VIDEO_STREAMS];
  VideoView *inputs[VIDEO_STREAMS];
  UINT n = 0;
  BOOL fill_found = v->fill != D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_SOURCE_STREAM;
  memset(id, 0, sizeof id);
  memset(ia, 0, sizeof ia);
  for (UINT i = 0; i < count; ++i) {
    const D3D11_VIDEO_PROCESSOR_STREAM *st = &streams[i];
    if (!st->Enable) continue;
    VideoView *iv = vview_of(st->pInputSurface, VVIEW_PROCESSOR_INPUT);
    const VideoStreamState *s = &v->streams[i];
    if (!iv || iv->c.dev != d || st->PastFrames || st->FutureFrames || st->ppPastSurfaces || st->ppFutureSurfaces || st->pInputSurfaceRight || st->ppPastSurfacesRight ||
        st->ppFutureSurfacesRight)
      return E_INVALIDARG;
    if (s->frame_format != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE || s->luma_enable || (s->stereo_enable && s->stereo_format != D3D11_VIDEO_PROCESSOR_STEREO_FORMAT_MONO) ||
        s->rate == D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_CUSTOM)
      return E_INVALIDARG;
    for (UINT f = 0; f < 8; ++f) if (s->filter_enable[f]) return E_INVALIDARG;
    if (s->aspect_enable && (UINT64)s->aspect_source.Numerator * s->aspect_dest.Denominator != (UINT64)s->aspect_dest.Numerator * s->aspect_source.Denominator) return E_INVALIDARG;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *in = &id[n];
    in->Format = iv->format;
    if (s->use_space1) in->ColorSpace = s->space1;
    else if (!vp_space_of(&s->space, iv->format, &in->ColorSpace)) return E_INVALIDARG;
    in->SourceAspectRatio = s->aspect_source;
    in->DestinationAspectRatio = s->aspect_dest;
    in->FrameRate = v->enumerator->desc.InputFrameRate;
    UINT iw, ih;
    vp_view_size(iv, &iw, &ih);
    in->SourceSizeRange = (D3D12_VIDEO_SIZE_RANGE){iw, ih, 1, 1};
    in->DestinationSizeRange = (D3D12_VIDEO_SIZE_RANGE){ow, oh, 1, 1};
    const D3D12_VIDEO_PROCESS_ORIENTATION orientation = vp_orientation(s);
    in->EnableOrientation = orientation != D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT;
    in->EnableAlphaBlending = s->alpha_enable;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *a = &ia[n];
    const RECT whole_in = {0, 0, (LONG)iw, (LONG)ih}, whole_out = {0, 0, (LONG)ow, (LONG)oh};
    const RECT src = vp_clip(s->source_enable ? s->source : whole_in, iw, ih);
    const RECT dst = s->dest_enable ? s->dest : whole_out;
    a->Transform.SourceRectangle = (D3D12_RECT){src.left, src.top, src.right, src.bottom};
    a->Transform.DestinationRectangle = (D3D12_RECT){dst.left, dst.top, dst.right, dst.bottom};
    a->Transform.Orientation = orientation;
    a->RateInfo.OutputIndex = st->OutputIndex;
    a->RateInfo.InputFrameOrField = st->InputFrameOrField;
    a->AlphaBlending.Enable = s->alpha_enable;
    a->AlphaBlending.Alpha = s->alpha_enable ? s->alpha : 1.0f;
    if (v->fill == D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_SOURCE_STREAM && v->fill_stream == i) { od.AlphaFillModeSourceStreamIndex = n; fill_found = TRUE; }
    inputs[n++] = iv;
  }
  if (!fill_found) return E_INVALIDARG;      /* (the stream whose alpha fills the output is not one of those drawn) */

  HRESULT hr = S_OK;
  LOCK(c);
  VideoState *vs = video_state(d);
  Tex *out_scratch = NULL, *in_scratch[VIDEO_STREAMS];
  memset(in_scratch, 0, sizeof in_scratch);
  if (!vs) hr = E_INVALIDARG;
  const UINT ninputs = n;
  /* (no stream drawn: the target is filled with the background - a stream of a small picture of the output's format with nothing of it drawn) */
  Tex *nothing = NULL;
  if (SUCCEEDED(hr) && n == 0) {
    const D3D11_TEXTURE2D_DESC small = {2, 2, 1, 1, out_format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    hr = tex_make(d, &small, 0, NULL, &nothing);
    id[0].Format = out_format;
    id[0].ColorSpace = out_space;
    id[0].SourceAspectRatio = id[0].DestinationAspectRatio = (DXGI_RATIONAL){1, 1};
    ia[0].AlphaBlending.Alpha = 1.0f;
    n = 1;
  }
  Tex *out_tex = ov->tex;
  const UINT out_sub = ov->mip + ov->slice * out_tex->desc.MipLevels;
  if (SUCCEEDED(hr) && !vp_direct(out_tex)) hr = vp_scratch(d, out_tex, out_sub, TRUE, &out_scratch);
  for (UINT k = 0; SUCCEEDED(hr) && k < ninputs; ++k) {
    Tex *t = inputs[k]->tex;
    tex_sync(d, t);
    if (!vp_direct(t)) hr = vp_scratch(d, t, inputs[k]->mip + inputs[k]->slice * t->desc.MipLevels, TRUE, &in_scratch[k]);
  }
  ID3D12VideoProcessor *p12 = NULL;
  if (SUCCEEDED(hr) && !(p12 = vp_processor(d, v, &od, id, n))) hr = E_INVALIDARG;
  if (SUCCEEDED(hr) && !video_queue_begin(d, &vs->process, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, &IID_ID3D12VideoProcessCommandList)) hr = E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) {
    for (UINT k = 0; k < n; ++k) {
      if (k < ninputs) {
        Tex *t = in_scratch[k] ? in_scratch[k] : inputs[k]->tex;
        ia[k].InputStream[0].pTexture2D = t->res;
        ia[k].InputStream[0].Subresource = in_scratch[k] ? 0 : t->planar ? inputs[k]->slice : 0;
      } else {
        /* (the background-only stream: empty rectangles of a picture of its own) */
        ia[k].InputStream[0].pTexture2D = nothing->res;
        ia[k].InputStream[0].Subresource = 0;
      }
    }
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS oa;
    memset(&oa, 0, sizeof oa);
    Tex *target = out_scratch ? out_scratch : out_tex;
    oa.OutputStream[0].pTexture2D = target->res;
    oa.OutputStream[0].Subresource = out_scratch ? 0 : target->planar ? ov->slice : 0;
    const RECT whole = {0, 0, (LONG)ow, (LONG)oh};
    const RECT tr = vp_clip(v->target_enable ? v->target : whole, ow, oh);
    oa.TargetRectangle = (D3D12_RECT){tr.left, tr.top, tr.right, tr.bottom};
    tex_sync(d, out_tex);
    ID3D12VideoProcessCommandList_ProcessFrames((ID3D12VideoProcessCommandList *)vs->process.list, p12, &oa, n, ia);
    video_queue_run(d, &vs->process);
    if (out_scratch) vp_scratch_store(d, out_scratch, out_tex, out_sub);
    ++out_tex->version;      /* (a render target view's own copy of it is loaded again) */
  }
  if (out_scratch) dx_Release(OBJ(out_scratch));
  if (nothing) dx_Release(OBJ(nothing));
  for (UINT k = 0; k < VIDEO_STREAMS; ++k) if (in_scratch[k]) dx_Release(OBJ(in_scratch[k]));
  UNLOCK(c);
  return hr;
}

/* ---- ID3D11VideoContext: content protection (none) ------------------------------------------------------------------------------------------------------------------ */

/* No crypto session or authenticated channel can be made (ID3D11VideoDevice's creators refuse), so what these are given is not one: the call is invalid. The methods
 * without a result do nothing. */
static HRESULT STDMETHODCALLTYPE vctx_NegotiateCryptoSessionKeyExchange(ID3D11VideoContext3 *p, ID3D11CryptoSession *session, UINT size, void *data) {
  (void)p; (void)session; (void)size; (void)data;
  return E_INVALIDARG;
}
static void STDMETHODCALLTYPE vctx_EncryptionBlt(ID3D11VideoContext3 *p, ID3D11CryptoSession *session, ID3D11Texture2D *source, ID3D11Texture2D *dest, UINT iv_size, void *iv) {
  (void)p; (void)session; (void)source; (void)dest; (void)iv_size; (void)iv;
}
static void STDMETHODCALLTYPE vctx_DecryptionBlt(ID3D11VideoContext3 *p, ID3D11CryptoSession *session, ID3D11Texture2D *source, ID3D11Texture2D *dest,
                                                 D3D11_ENCRYPTED_BLOCK_INFO *info, UINT key_size, const void *key, UINT iv_size, void *iv) {
  (void)p; (void)session; (void)source; (void)dest; (void)info; (void)key_size; (void)key; (void)iv_size; (void)iv;
}
static void STDMETHODCALLTYPE vctx_StartSessionKeyRefresh(ID3D11VideoContext3 *p, ID3D11CryptoSession *session, UINT size, void *number) {
  (void)p; (void)session;
  if (number && size) memset(number, 0, size);
}
static void STDMETHODCALLTYPE vctx_FinishSessionKeyRefresh(ID3D11VideoContext3 *p, ID3D11CryptoSession *session) { (void)p; (void)session; }
static HRESULT STDMETHODCALLTYPE vctx_GetEncryptionBltKey(ID3D11VideoContext3 *p, ID3D11CryptoSession *session, UINT size, void *key) {
  (void)p; (void)session; (void)size; (void)key;
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE vctx_NegotiateAuthenticatedChannelKeyExchange(ID3D11VideoContext3 *p, ID3D11AuthenticatedChannel *channel, UINT size, void *data) {
  (void)p; (void)channel; (void)size; (void)data;
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE vctx_QueryAuthenticatedChannel(ID3D11VideoContext3 *p, ID3D11AuthenticatedChannel *channel, UINT in_size, const void *in, UINT out_size, void *out) {
  (void)p; (void)channel; (void)in_size; (void)in;
  if (out && out_size) memset(out, 0, out_size);
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE vctx_ConfigureAuthenticatedChannel(ID3D11VideoContext3 *p, ID3D11AuthenticatedChannel *channel, UINT in_size, const void *in,
                                                                    D3D11_AUTHENTICATED_CONFIGURE_OUTPUT *out) {
  (void)p; (void)channel; (void)in_size; (void)in;
  if (out) memset(out, 0, sizeof *out);
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE vctx_GetDataForNewHardwareKey(ID3D11VideoContext3 *p, ID3D11CryptoSession *session, UINT size, const void *input, UINT64 *output) {
  (void)p; (void)session; (void)size; (void)input;
  if (output) *output = 0;
  return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE vctx_CheckCryptoSessionStatus(ID3D11VideoContext3 *p, ID3D11CryptoSession *session, D3D11_CRYPTO_SESSION_STATUS *status) {
  (void)p; (void)session;
  if (status) *status = D3D11_CRYPTO_SESSION_STATUS_OK;
  return E_INVALIDARG;
}
