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

/* d3d12.dll: video encoding and motion estimation (ID3D12VideoMotionEstimator, ID3D12VideoMotionVectorHeap, ID3D12VideoEncoder, ID3D12VideoEncoderHeap, the encode command
 * list, the encoder's feature queries). Included by d3d12.c after video.inc.
 *
 * Both run in the kernel on the CPU (src/Kernel/Dxgkrnl/VideoEncode.hpp: the stream's EstimateMotion / ResolveMotionVectors / EncodeFrame / ResolveEncoderMetadata).
 * A motion vector heap and an encoder heap are kernel buffers: the vectors, and the rate controller's state from picture to picture.
 * The encoder is H.264 (Main, and High coded as Main is): CAVLC, Intra16x16 / 16x16 inter partitions with quarter-sample vectors, I / P / B pictures with any references
 * the program's lists name (long-term ones too, the marking and list modifications written as given), constrained intra prediction, the in-loop filter modes 0 - 2, slices
 * by macroblocks, rows or count, the rate control modes CQP, CBR, VBR, QVBR and an absolute QP map, with QP ranges, initial QP, VBV sizes, frame size limits and QP deltas.
 * The program writes the sequence and picture parameter sets (as H264PictureEncoder.hpp says they must read); EncodeFrame writes the slice NAL units.
 * What it does not do (the queries say so): CABAC, the 8x8 transform, direct prediction, intra refresh, slices by bytes, frame analysis, HEVC and AV1, protected content. */

/* ---- the kernel's ops and layouts (src/Kernel/Dxgkrnl/CommandStream.hpp, VideoEncode.hpp) --------------------------------------------------------------------------- */

enum { OP_ESTIMATE_MOTION = OP_VIDEO_PROCESS + 1, OP_RESOLVE_MOTION_VECTORS, OP_ENCODE_FRAME, OP_RESOLVE_ENCODER_METADATA };
#define VENC_PARAM_WORDS 50
#define VENC_METADATA_HEADER (8 + 72)
#define VENC_SUBREGION_BYTES 24
#define VENC_HEAP_BYTES 256
#define VENC_MAX_REFERENCES 16
#define VENC_MAX_SIZE 4096
#define VENC_ALIGN 16
#define MV_HEAP_HEADER 16
#define MV_ENTRY_BYTES 8

static UINT64 mv_heap_bytes(UINT w, UINT h, UINT block) { return MV_HEAP_HEADER + (UINT64)((w + block - 1) / block) * ((h + block - 1) / block) * MV_ENTRY_BYTES; }

/* A kernel buffer of `bytes`, zeroed (0: none). */
static UINT kbuffer_new(Dev *d, UINT64 bytes) {
  KmtAllocDesc ad;
  memset(&ad, 0, sizeof ad);
  ad.flags = KA_VERTEX;
  ad.size = bytes;
  ad.mips = ad.samples = ad.layers = 1;
  UINT alloc = 0;
  if (!KMT_OK(kmt_create_allocation(d->kdev, &ad, &alloc, NULL, NULL))) return 0;
  void *p = NULL;
  if (!KMT_OK(kmt_lock(d->kdev, alloc, 0, 0, &p)) || !p) { kmt_destroy_allocation(d->kdev, alloc); return 0; }
  memset(p, 0, (size_t)bytes);
  kmt_unlock(d->kdev, alloc);
  return alloc;
}
/* A buffer of the list's holding `bytes` of `data` until it is reset (0: no memory). */
static UINT list_temp_buffer(CmdList *l, const void *data, UINT bytes) {
  if (l->ntemps == l->cap_temps) {
    const UINT cap = l->cap_temps ? l->cap_temps * 2 : 16;
    UINT *n = mem_grow(l->temps, cap * sizeof *n);
    if (!n) return 0;
    l->temps = n;
    l->cap_temps = cap;
  }
  const UINT alloc = kbuffer_new(l->d.dev, bytes ? bytes : 16);
  if (!alloc) return 0;
  void *p = NULL;
  if (!KMT_OK(kmt_lock(l->d.dev->kdev, alloc, 0, 0, &p)) || !p) { kmt_destroy_allocation(l->d.dev->kdev, alloc); return 0; }
  memcpy(p, data, bytes);
  kmt_unlock(l->d.dev->kdev, alloc);
  l->temps[l->ntemps++] = alloc;
  return alloc;
}
/* An NV12 texture's planes at subresource `sub` (its array slice): 0 if it is not one. */
static int nv12_planes(Resource *r, UINT sub, UINT *luma, UINT *chroma) {
  if (!r || !r->planes || !r->fmt || r->fmt->dxgi != DXGI_FORMAT_NV12 || sub >= r->layers) return 0;
  *luma = r->plane_alloc[sub];
  *chroma = r->plane_alloc[r->layers + sub];
  return 1;
}

/* ---- motion estimation ------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct VMe { DObj d; D3D12_VIDEO_MOTION_ESTIMATOR_DESC desc; } VMe;
typedef struct VMvHeap { DObj d; D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC desc; UINT alloc; UINT64 bytes; } VMvHeap;
static const GUID *const g_vme_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12VideoMotionEstimator, NULL};
static const GUID *const g_vmvh_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12VideoMotionVectorHeap, NULL};

/* What the estimator takes: NV12, 8x8 or 16x16 blocks, quarter-sample vectors, pictures of 16 x 16 .. the kernel's largest texture. */
#define ME_MIN_SIZE 16
static int me_size_range_ok(const D3D12_VIDEO_SIZE_RANGE *r) {
  return r->MinWidth >= ME_MIN_SIZE && r->MinHeight >= ME_MIN_SIZE && r->MinWidth <= r->MaxWidth && r->MinHeight <= r->MaxHeight && r->MaxWidth <= KMAX_TEXTURE_DIMENSION &&
         r->MaxHeight <= KMAX_TEXTURE_DIMENSION;
}
static int me_desc_ok(DXGI_FORMAT f, D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE b, D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION p, const D3D12_VIDEO_SIZE_RANGE *r) {
  return f == DXGI_FORMAT_NV12 && (b == D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_8X8 || b == D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_16X16) &&
         p == D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL && me_size_range_ok(r);
}
static UINT me_block(D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE b) { return b == D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_8X8 ? 8 : 16; }

static void vmvh_destroy(DxObj *o) {
  VMvHeap *h = (VMvHeap *)o;
  if (h->alloc) kmt_destroy_allocation(h->d.dev->kdev, h->alloc);
  dobj_child_destroy(o);
}
static D3D12_VIDEO_MOTION_ESTIMATOR_DESC *STDMETHODCALLTYPE vme_GetDesc(VMe *This, D3D12_VIDEO_MOTION_ESTIMATOR_DESC *ret) { *ret = This->desc; return ret; }
static D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC *STDMETHODCALLTYPE vmvh_GetDesc(VMvHeap *This, D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC *ret) { *ret = This->desc; return ret; }
/* (made without a protected session: there is none to give) */
static HRESULT STDMETHODCALLTYPE vme_GetProtectedResourceSession(VMe *This, REFIID riid, void **out) { if (!out) return E_INVALIDARG; *out = NULL; return DXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE vmvh_GetProtectedResourceSession(VMvHeap *This, REFIID riid, void **out) { if (!out) return E_INVALIDARG; *out = NULL; return DXGI_ERROR_NOT_FOUND; }

static HRESULT vme_make(Dev *d, const D3D12_VIDEO_MOTION_ESTIMATOR_DESC *desc, ID3D12ProtectedResourceSession *session, REFIID riid, void **out) {
  extern const ID3D12VideoMotionEstimatorVtbl vme_vtbl;
  if (out) *out = NULL;
  /* (no protected sessions exist: D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_PROTECTED_RESOURCES says so) */
  if (!desc || !out || desc->NodeMask > 1 || session || !me_desc_ok(desc->InputFormat, desc->BlockSize, desc->Precision, &desc->SizeRange)) return E_INVALIDARG;
  VMe *v = dobj_new(sizeof *v, d, &vme_vtbl, g_vme_iids, dobj_child_destroy);
  if (!v) return E_OUTOFMEMORY;
  v->desc = *desc;
  return query_out((IUnknown *)v, riid, out);
}
static HRESULT vmvh_make(Dev *d, const D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC *desc, ID3D12ProtectedResourceSession *session, REFIID riid, void **out) {
  extern const ID3D12VideoMotionVectorHeapVtbl vmvh_vtbl;
  if (out) *out = NULL;
  if (!desc || !out || desc->NodeMask > 1 || session || !me_desc_ok(desc->InputFormat, desc->BlockSize, desc->Precision, &desc->SizeRange)) return E_INVALIDARG;
  VMvHeap *h = dobj_new(sizeof *h, d, &vmvh_vtbl, g_vmvh_iids, vmvh_destroy);
  if (!h) return E_OUTOFMEMORY;
  h->desc = *desc;
  h->bytes = mv_heap_bytes(desc->SizeRange.MaxWidth, desc->SizeRange.MaxHeight, me_block(desc->BlockSize));
  h->alloc = kbuffer_new(d, h->bytes);
  if (!h->alloc) { dx_Release(OBJ(h)); return E_OUTOFMEMORY; }
  return query_out((IUnknown *)h, riid, out);
}

static void STDMETHODCALLTYPE velist_EstimateMotion(CmdList *l, ID3D12VideoMotionEstimator *estimator, const D3D12_VIDEO_MOTION_ESTIMATOR_OUTPUT *out, const D3D12_VIDEO_MOTION_ESTIMATOR_INPUT *in) {
  VMe *me = (VMe *)estimator;
  if (!me || !out || !in || l->closed) return;
  VMvHeap *heap = (VMvHeap *)out->pMotionVectorHeap, *hint = (VMvHeap *)in->pHintMotionVectorHeap;
  Resource *cur = (Resource *)in->pInputTexture2D, *ref = (Resource *)in->pReferenceTexture2D;
  UINT cl, cc, rl, rc;
  /* (the heaps of the estimator's format, block size and precision; the two pictures NV12 of one size inside the heap's range) */
  if (!heap || heap->desc.BlockSize != me->desc.BlockSize || heap->desc.InputFormat != me->desc.InputFormat || heap->desc.Precision != me->desc.Precision) return;
  if (hint && (hint == heap || hint->desc.BlockSize != heap->desc.BlockSize || hint->desc.SizeRange.MaxWidth < heap->desc.SizeRange.MaxWidth ||
               hint->desc.SizeRange.MaxHeight < heap->desc.SizeRange.MaxHeight))
    return;
  if (!nv12_planes(cur, in->InputSubresourceIndex, &cl, &cc) || !nv12_planes(ref, in->ReferenceSubresourceIndex, &rl, &rc)) return;
  const UINT w = (UINT)cur->desc.Width, h = cur->desc.Height;
  if (ref->desc.Width != cur->desc.Width || ref->desc.Height != h || w < me->desc.SizeRange.MinWidth || h < me->desc.SizeRange.MinHeight || w > me->desc.SizeRange.MaxWidth ||
      h > me->desc.SizeRange.MaxHeight || w > heap->desc.SizeRange.MaxWidth || h > heap->desc.SizeRange.MaxHeight)
    return;
  ensure_room(l, 128, 6);
  const UINT words[9] = {me_block(me->desc.BlockSize), w, h, alloc_index(l, cl), alloc_index(l, rl), hint ? alloc_index(l, hint->alloc) : 0xFFFFFFFFu, 0, alloc_index(l, heap->alloc), 0};
  packet(l, OP_ESTIMATE_MOTION, words, 9);
  keep(l, (IUnknown *)estimator);
  keep(l, (IUnknown *)heap);
  if (hint) keep(l, (IUnknown *)hint);
}
static void STDMETHODCALLTYPE velist_ResolveMotionVectorHeap(CmdList *l, const D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_OUTPUT *out, const D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_INPUT *in) {
  if (!out || !in || l->closed) return;
  VMvHeap *heap = (VMvHeap *)in->pMotionVectorHeap;
  Resource *tex = (Resource *)out->pMotionVectorTexture2D;
  /* (an R16G16_SINT 2D texture of one level: a pixel a block, at the coordinate) */
  if (!heap || !tex || !tex->fmt || tex->fmt->dxgi != DXGI_FORMAT_R16G16_SINT || tex->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || tex->mips != 1 || tex->layers != 1 ||
      out->MotionVectorCoordinate.Z != 0 || out->MotionVectorCoordinate.SubresourceIndex != 0 || out->MotionVectorCoordinate.X > 0xFFFFFFFFu || in->PixelWidth == 0 || in->PixelHeight == 0 ||
      in->PixelWidth > heap->desc.SizeRange.MaxWidth || in->PixelHeight > heap->desc.SizeRange.MaxHeight)
    return;
  ensure_room(l, 128, 4);
  const UINT words[7] = {alloc_index(l, heap->alloc), 0, in->PixelWidth, in->PixelHeight, alloc_index(l, tex->alloc), (UINT)out->MotionVectorCoordinate.X, out->MotionVectorCoordinate.Y};
  packet(l, OP_RESOLVE_MOTION_VECTORS, words, 7);
  keep(l, (IUnknown *)heap);
}

/* ---- the encoder's capabilities ---------------------------------------------------------------------------------------------------------------------------------- */

static int venc_profile(const D3D12_VIDEO_ENCODER_PROFILE_DESC *p, D3D12_VIDEO_ENCODER_PROFILE_H264 *out) {
  if (p->DataSize != sizeof(D3D12_VIDEO_ENCODER_PROFILE_H264) || !p->pH264Profile) return 0;
  if (*p->pH264Profile != D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN && *p->pH264Profile != D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH) return 0;
  if (out) *out = *p->pH264Profile;
  return 1;
}
static int venc_level(const D3D12_VIDEO_ENCODER_LEVEL_SETTING *l, D3D12_VIDEO_ENCODER_LEVELS_H264 *out) {
  if (l->DataSize != sizeof(D3D12_VIDEO_ENCODER_LEVELS_H264) || !l->pH264LevelSetting || (UINT)*l->pH264LevelSetting > D3D12_VIDEO_ENCODER_LEVELS_H264_62) return 0;
  if (out) *out = *l->pH264LevelSetting;
  return 1;
}
static int venc_resolution_ok(const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *r) {
  return r->Width >= VENC_ALIGN && r->Height >= VENC_ALIGN && r->Width <= VENC_MAX_SIZE && r->Height <= VENC_MAX_SIZE && r->Width % VENC_ALIGN == 0 && r->Height % VENC_ALIGN == 0;
}
/* The configurations the kernel codes: constrained intra prediction or not, no CABAC / 8x8 transform / intra constrained slices, no direct prediction, filter modes 0 - 2. */
static int venc_config(const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION *c, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 *out) {
  if (c->DataSize != sizeof(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264) || !c->pH264Config) return 0;
  const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 *h = c->pH264Config;
  if ((h->ConfigurationFlags & ~D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_CONSTRAINED_INTRAPREDICTION) ||
      h->DirectModeConfig != D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED ||
      (UINT)h->DisableDeblockingFilterConfig > D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_2_DISABLE_SLICE_BOUNDARIES_BLOCKS)
    return 0;
  if (out) *out = *h;
  return 1;
}
#define VENC_RC_FLAGS (D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_DELTA_QP | D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE | D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_INITIAL_QP | \
                       D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_MAX_FRAME_SIZE | D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES)
/* The rate control parameters as the kernel's words 10 .. 30 (VideoEncode.hpp). 0 if the mode, flags or parameters are not ones it takes. */
static int venc_rate_control(const D3D12_VIDEO_ENCODER_RATE_CONTROL *rc, UINT *w) {
  memset(w + 10, 0, 21 * sizeof *w);
  if ((UINT)rc->Mode > D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR || (rc->Flags & ~VENC_RC_FLAGS)) return 0;
  w[10] = rc->Mode;
  w[11] = rc->Flags;
  w[12] = rc->TargetFrameRate.Numerator;
  w[13] = rc->TargetFrameRate.Denominator;
  w[19] = 51;
  const UINT qp_range = (rc->Flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE) != 0;
#define RC_U64(at, v) do { w[at] = (UINT)(v); w[(at) + 1] = (UINT)((UINT64)(v) >> 32); } while (0)
  switch (rc->Mode) {
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_ABSOLUTE_QP_MAP: return rc->ConfigParams.DataSize == 0 || rc->ConfigParams.pConfiguration_CQP == NULL;
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP: {
      const D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP *c = rc->ConfigParams.pConfiguration_CQP;
      if (rc->ConfigParams.DataSize != sizeof *c || !c || c->ConstantQP_FullIntracodedFrame > 51 || c->ConstantQP_InterPredictedFrame_PrevRefOnly > 51 ||
          c->ConstantQP_InterPredictedFrame_BiDirectionalRef > 51)
        return 0;
      w[14] = c->ConstantQP_FullIntracodedFrame;
      w[15] = c->ConstantQP_InterPredictedFrame_PrevRefOnly;
      w[16] = c->ConstantQP_InterPredictedFrame_BiDirectionalRef;
      return 1;
    }
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR: {
      const D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR *c = rc->ConfigParams.pConfiguration_CBR;
      if (rc->ConfigParams.DataSize != sizeof *c || !c) return 0;
      w[17] = c->InitialQP; w[18] = qp_range ? c->MinQP : 0; w[19] = qp_range ? c->MaxQP : 51;
      RC_U64(20, c->TargetBitRate); RC_U64(24, c->VBVCapacity); RC_U64(26, c->InitialVBVFullness); RC_U64(28, c->MaxFrameBitSize);
      break;
    }
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_VBR: {
      const D3D12_VIDEO_ENCODER_RATE_CONTROL_VBR *c = rc->ConfigParams.pConfiguration_VBR;
      if (rc->ConfigParams.DataSize != sizeof *c || !c) return 0;
      w[17] = c->InitialQP; w[18] = qp_range ? c->MinQP : 0; w[19] = qp_range ? c->MaxQP : 51;
      RC_U64(20, c->TargetAvgBitRate); RC_U64(22, c->PeakBitRate); RC_U64(24, c->VBVCapacity); RC_U64(26, c->InitialVBVFullness); RC_U64(28, c->MaxFrameBitSize);
      break;
    }
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR: {
      const D3D12_VIDEO_ENCODER_RATE_CONTROL_QVBR *c = rc->ConfigParams.pConfiguration_QVBR;
      if (rc->ConfigParams.DataSize != sizeof *c || !c) return 0;
      w[17] = c->InitialQP; w[18] = qp_range ? c->MinQP : 0; w[19] = qp_range ? c->MaxQP : 51;
      RC_U64(20, c->TargetAvgBitRate); RC_U64(22, c->PeakBitRate); RC_U64(28, c->MaxFrameBitSize);
      w[30] = c->ConstantQualityTarget;
      break;
    }
  }
#undef RC_U64
  /* (what the kernel's rate_control_valid checks, so that the query answers what EncodeFrame does) */
  const UINT64 target = w[20] | (UINT64)w[21] << 32, peak = w[22] | (UINT64)w[23] << 32, cap = w[24] | (UINT64)w[25] << 32, init = w[26] | (UINT64)w[27] << 32;
  const UINT64 max_rate = (UINT64)1 << 38;
  if (!w[12] || !w[13] || w[12] > (1u << 24) || w[13] > (1u << 24) || !target || target > max_rate) return 0;
  if (rc->Mode != D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR && (peak < target || peak > max_rate)) return 0;
  if (rc->Mode == D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR && (w[30] == 0 || w[30] > 51)) return 0;
  if (qp_range && (w[18] > w[19] || w[19] > 51)) return 0;
  if ((rc->Flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_INITIAL_QP) && w[17] > 51) return 0;
  if ((rc->Flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES) && (cap > max_rate * 16 || init > cap)) return 0;
  if ((rc->Flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_MAX_FRAME_SIZE) && !(w[28] | w[29])) return 0;
  return 1;
}
static int venc_gop(const D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE *g, UINT *w) {
  if (g->DataSize != sizeof(D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264) || !g->pH264GroupOfPictures) return 0;
  const D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 *h = g->pH264GroupOfPictures;
  /* (picture order count types 0 and 2; B pictures - a P period above 1 - need type 0, as output order is decoding order with type 2) */
  if ((h->pic_order_cnt_type != 0 && h->pic_order_cnt_type != 2) || h->log2_max_frame_num_minus4 > 12 || h->log2_max_pic_order_cnt_lsb_minus4 > 12) return 0;
  if (h->pic_order_cnt_type == 2 && h->PPicturePeriod > 1) return 0;
  if (w) { w[7] = h->pic_order_cnt_type; w[8] = h->log2_max_frame_num_minus4; w[9] = h->log2_max_pic_order_cnt_lsb_minus4; }
  return 1;
}
/* How many slices a layout makes of a picture (H264PictureEncoder::slice_count); 0: not one the encoder takes. */
static UINT venc_slices(D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE mode, const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA *data, UINT w, UINT h) {
  const UINT mbw = w / 16, mbh = h / 16, mbs = mbw * mbh;
  if (!mbs) return 0;
  if (mode == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) return 1;
  if (!data || data->DataSize != sizeof(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES) || !data->pSlicesPartition_H264) return 0;
  const UINT v = data->pSlicesPartition_H264->NumberOfCodingUnitsPerSlice;
  switch (mode) {
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_SQUARE_UNITS_PER_SUBREGION_ROW_UNALIGNED: return v ? (mbs + v - 1) / v : 0;
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION: return v ? (mbh + v - 1) / v : 0;
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME: return v && v <= mbs ? v : 0;
    default: return 0;   /* (bytes a slice, grids, automatic: not done) */
  }
}
static int venc_mode_ok(D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE m) {
  return m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME || m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_SQUARE_UNITS_PER_SUBREGION_ROW_UNALIGNED ||
         m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION || m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME;
}
static UINT64 venc_metadata_bytes(UINT w, UINT h) { return VENC_METADATA_HEADER + (UINT64)(w / 16) * (h / 16) * VENC_SUBREGION_BYTES; }
/* The smallest level whose frame size and macroblock rate take the picture (Table A-1). */
static D3D12_VIDEO_ENCODER_LEVELS_H264 venc_suggested_level(UINT w, UINT h, const DXGI_RATIONAL *rate) {
  static const struct { D3D12_VIDEO_ENCODER_LEVELS_H264 level; UINT fs; UINT mbps; } levels[] = {
      {D3D12_VIDEO_ENCODER_LEVELS_H264_1, 99, 1485}, {D3D12_VIDEO_ENCODER_LEVELS_H264_11, 396, 3000}, {D3D12_VIDEO_ENCODER_LEVELS_H264_12, 396, 6000},
      {D3D12_VIDEO_ENCODER_LEVELS_H264_13, 396, 11880}, {D3D12_VIDEO_ENCODER_LEVELS_H264_2, 396, 11880}, {D3D12_VIDEO_ENCODER_LEVELS_H264_21, 792, 19800},
      {D3D12_VIDEO_ENCODER_LEVELS_H264_22, 1620, 20250}, {D3D12_VIDEO_ENCODER_LEVELS_H264_3, 1620, 40500}, {D3D12_VIDEO_ENCODER_LEVELS_H264_31, 3600, 108000},
      {D3D12_VIDEO_ENCODER_LEVELS_H264_32, 5120, 216000}, {D3D12_VIDEO_ENCODER_LEVELS_H264_4, 8192, 245760}, {D3D12_VIDEO_ENCODER_LEVELS_H264_41, 8192, 245760},
      {D3D12_VIDEO_ENCODER_LEVELS_H264_42, 8704, 522240}, {D3D12_VIDEO_ENCODER_LEVELS_H264_5, 22080, 589824}, {D3D12_VIDEO_ENCODER_LEVELS_H264_51, 36864, 983040},
      {D3D12_VIDEO_ENCODER_LEVELS_H264_52, 36864, 2073600}, {D3D12_VIDEO_ENCODER_LEVELS_H264_6, 139264, 4177920}, {D3D12_VIDEO_ENCODER_LEVELS_H264_61, 139264, 8355840},
      {D3D12_VIDEO_ENCODER_LEVELS_H264_62, 139264, 16711680}};
  const UINT64 mbs = (UINT64)(w / 16) * (h / 16);
  const UINT64 mbps = rate && rate->Denominator ? mbs * rate->Numerator / rate->Denominator : mbs * 30;
  for (UINT i = 0; i < sizeof levels / sizeof levels[0]; ++i)
    if (mbs <= levels[i].fs && mbps <= levels[i].mbps) return levels[i].level;
  return D3D12_VIDEO_ENCODER_LEVELS_H264_62;
}
#define VENC_SUPPORT_FLAGS (D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_RECONFIGURATION_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RESOLUTION_RECONFIGURATION_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_VBV_SIZE_CONFIG_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_DELTA_QP_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_SUBREGION_LAYOUT_RECONFIGURATION_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_ADJUSTABLE_QP_RANGE_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_INITIAL_QP_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_MAX_FRAME_SIZE_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_SEQUENCE_GOP_RECONFIGURATION_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_MOTION_ESTIMATION_PRECISION_MODE_LIMIT_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_READABLE_RECONSTRUCTED_PICTURE_LAYOUT_AVAILABLE_)

/* D3D12_FEATURE_VIDEO_ENCODER_SUPPORT and SUPPORT1 (their common part; `layout` is SUPPORT1's subregion data, NULL for SUPPORT). */
static HRESULT venc_support(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT *f, const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA *layout) {
  if (f->NodeIndex) return E_INVALIDARG;
  UINT v = 0, w[VENC_PARAM_WORDS];
  f->SupportFlags = D3D12_VIDEO_ENCODER_SUPPORT_FLAG_NONE;
  if (f->Codec != D3D12_VIDEO_ENCODER_CODEC_H264) { f->ValidationFlags = D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_NOT_SUPPORTED; return S_OK; }
  if (f->InputFormat != DXGI_FORMAT_NV12) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INPUT_FORMAT_NOT_SUPPORTED;
  if (!venc_config(&f->CodecConfiguration, NULL)) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED;
  if (!venc_gop(&f->CodecGopSequence, NULL)) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_GOP_STRUCTURE_NOT_SUPPORTED;
  if ((UINT)f->RateControl.Mode > D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_MODE_NOT_SUPPORTED;
  else if (!venc_rate_control(&f->RateControl, w)) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_CONFIGURATION_NOT_SUPPORTED;
  if (f->IntraRefresh != D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INTRA_REFRESH_MODE_NOT_SUPPORTED;
  if (!venc_mode_ok(f->SubregionFrameEncoding)) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_MODE_NOT_SUPPORTED;
  if (!f->ResolutionsListCount || !f->pResolutionList) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST;
  for (UINT i = 0; f->pResolutionList && i < f->ResolutionsListCount; ++i) {
    const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *r = &f->pResolutionList[i];
    if (!venc_resolution_ok(r)) { v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST; continue; }
    if (layout && venc_mode_ok(f->SubregionFrameEncoding) && !venc_slices(f->SubregionFrameEncoding, layout, r->Width, r->Height))
      v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_MODE_NOT_SUPPORTED;
    if (f->pResolutionDependentSupport) {
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS *lim = &f->pResolutionDependentSupport[i];
      lim->MaxSubregionsNumber = (r->Width / 16) * (r->Height / 16);
      lim->MaxIntraRefreshFrameDuration = 0;
      lim->SubregionBlockPixelsSize = 16;
      lim->QPMapRegionPixelsSize = 16;
    }
  }
  f->MaxReferenceFramesInDPB = VENC_MAX_REFERENCES;
  f->ValidationFlags = (D3D12_VIDEO_ENCODER_VALIDATION_FLAGS)v;
  if (v) return S_OK;
  f->SupportFlags = VENC_SUPPORT_FLAGS;
  if (f->SuggestedProfile.DataSize == sizeof(D3D12_VIDEO_ENCODER_PROFILE_H264) && f->SuggestedProfile.pH264Profile) *f->SuggestedProfile.pH264Profile = D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;
  if (f->SuggestedLevel.DataSize == sizeof(D3D12_VIDEO_ENCODER_LEVELS_H264) && f->SuggestedLevel.pH264LevelSetting) {
    UINT mw = 0, mh = 0;
    for (UINT i = 0; i < f->ResolutionsListCount; ++i)
      if ((UINT64)f->pResolutionList[i].Width * f->pResolutionList[i].Height > (UINT64)mw * mh) { mw = f->pResolutionList[i].Width; mh = f->pResolutionList[i].Height; }
    *f->SuggestedLevel.pH264LevelSetting = venc_suggested_level(mw, mh, &f->RateControl.TargetFrameRate);
  }
  return S_OK;
}

/* The encoder's part of vdev_CheckFeatureSupport (video.inc): the motion estimator's and encoder's queries. S_FALSE: not one of them. */
static HRESULT venc_feature(Dev *d, D3D12_FEATURE_VIDEO feature, void *data, UINT size) {
#define ENC_FEATURE(type) do { if (size != sizeof(type)) return E_INVALIDARG; } while (0)
  (void)d;
  switch (feature) {
    case D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR);
      D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      const int ok = f->InputFormat == DXGI_FORMAT_NV12;
      f->BlockSizeFlags = ok ? D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_8X8 | D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_16X16 : D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_FLAG_NONE;
      f->PrecisionFlags = ok ? D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_FLAG_QUARTER_PEL : D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_FLAG_NONE;
      f->SizeRange.MaxWidth = f->SizeRange.MaxHeight = ok ? KMAX_TEXTURE_DIMENSION : 0;
      f->SizeRange.MinWidth = f->SizeRange.MinHeight = ok ? ME_MIN_SIZE : 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR_SIZE: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_SIZE);
      D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_SIZE *f = data;
      if (f->NodeIndex || f->Protected || !me_desc_ok(f->InputFormat, f->BlockSize, f->Precision, &f->SizeRange)) return E_INVALIDARG;
      f->MotionVectorHeapMemoryPoolL0Size = 0;
      f->MotionVectorHeapMemoryPoolL1Size = mv_heap_bytes(f->SizeRange.MaxWidth, f->SizeRange.MaxHeight, me_block(f->BlockSize));
      f->MotionEstimatorMemoryPoolL0Size = f->MotionEstimatorMemoryPoolL1Size = 0;      /* (the estimator keeps nothing) */
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_CODEC: {
      /* (H.264 only: there is no HEVC or AV1 encoder) */
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = f->Codec == D3D12_VIDEO_ENCODER_CODEC_H264;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = FALSE;
      if (f->Codec != D3D12_VIDEO_ENCODER_CODEC_H264) return S_OK;
      if (f->Profile.DataSize != sizeof(D3D12_VIDEO_ENCODER_PROFILE_H264) || !f->Profile.pH264Profile || f->MinSupportedLevel.DataSize != sizeof(D3D12_VIDEO_ENCODER_LEVELS_H264) ||
          !f->MinSupportedLevel.pH264LevelSetting || f->MaxSupportedLevel.DataSize != sizeof(D3D12_VIDEO_ENCODER_LEVELS_H264) || !f->MaxSupportedLevel.pH264LevelSetting)
        return E_INVALIDARG;
      f->IsSupported = venc_profile(&f->Profile, NULL);
      if (f->IsSupported) {
        *f->MinSupportedLevel.pH264LevelSetting = D3D12_VIDEO_ENCODER_LEVELS_H264_1;
        *f->MaxSupportedLevel.pH264LevelSetting = D3D12_VIDEO_ENCODER_LEVELS_H264_62;
      }
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->ResolutionRatiosCount = 0;      /* (any width and height in the range, no ratio asked for) */
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION *f = data;
      if (f->NodeIndex || f->ResolutionRatiosCount != 0) return E_INVALIDARG;
      f->IsSupported = f->Codec == D3D12_VIDEO_ENCODER_CODEC_H264;
      /* (whole macroblocks: the program's parameter sets crop what it shows; the reconstructions are the coded size) */
      f->MinResolutionSupported.Width = f->MinResolutionSupported.Height = f->IsSupported ? VENC_ALIGN : 0;
      f->MaxResolutionSupported.Width = f->MaxResolutionSupported.Height = f->IsSupported ? VENC_MAX_SIZE : 0;
      f->ResolutionWidthMultipleRequirement = f->ResolutionHeightMultipleRequirement = f->IsSupported ? VENC_ALIGN : 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = f->Codec == D3D12_VIDEO_ENCODER_CODEC_H264 && venc_profile(&f->Profile, NULL) && f->Format == DXGI_FORMAT_NV12;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_RATE_CONTROL_MODE: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_MODE);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_MODE *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = f->Codec == D3D12_VIDEO_ENCODER_CODEC_H264 && (UINT)f->RateControlMode <= D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_INTRA_REFRESH_MODE: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_INTRA_REFRESH_MODE);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_INTRA_REFRESH_MODE *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = f->Codec == D3D12_VIDEO_ENCODER_CODEC_H264 && venc_profile(&f->Profile, NULL) && venc_level(&f->Level, NULL) &&
                       f->IntraRefreshMode == D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = f->Codec == D3D12_VIDEO_ENCODER_CODEC_H264 && venc_profile(&f->Profile, NULL) && venc_level(&f->Level, NULL) && venc_mode_ok(f->SubregionMode);
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE *f = data;
      const D3D12_VIDEO_ENCODER_HEAP_DESC *h = &f->HeapDesc;
      int ok = h->NodeMask <= 1 && h->Flags == D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE && h->EncodeCodec == D3D12_VIDEO_ENCODER_CODEC_H264 && venc_profile(&h->EncodeProfile, NULL) &&
               venc_level(&h->EncodeLevel, NULL) && h->ResolutionsListCount && h->pResolutionList;
      for (UINT i = 0; ok && i < h->ResolutionsListCount; ++i) ok = venc_resolution_ok(&h->pResolutionList[i]);
      f->IsSupported = ok;
      f->MemoryPoolL0Size = 0;
      f->MemoryPoolL1Size = ok ? VENC_HEAP_BYTES : 0;      /* (the rate controller's state: the pictures are the program's) */
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = FALSE;
      if (f->Codec != D3D12_VIDEO_ENCODER_CODEC_H264) return S_OK;
      if (f->CodecSupportLimits.DataSize != sizeof(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264) || !f->CodecSupportLimits.pH264Support) return E_INVALIDARG;
      f->IsSupported = venc_profile(&f->Profile, NULL);
      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264 *s = f->CodecSupportLimits.pH264Support;
      /* (B pictures and long-term references together: nothing in the encoder depends on direct prediction, which it does not use) */
      s->SupportFlags = f->IsSupported ? D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CONSTRAINED_INTRAPREDICTION_SUPPORT | D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_BFRAME_LTR_COMBINED_SUPPORT
                                       : D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_NONE;
      s->DisableDeblockingFilterSupportedModes = f->IsSupported ? D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_FLAG_0_ALL_LUMA_CHROMA_SLICE_BLOCK_EDGES_ALWAYS_FILTERED |
                                                                      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_FLAG_1_DISABLE_ALL_SLICE_BLOCK_EDGES |
                                                                      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_FLAG_2_DISABLE_SLICE_BOUNDARIES_BLOCKS
                                                                : D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_FLAG_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = FALSE;
      if (f->Codec != D3D12_VIDEO_ENCODER_CODEC_H264) return S_OK;
      if (f->PictureSupport.DataSize != sizeof(D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_H264) || !f->PictureSupport.pH264Support) return E_INVALIDARG;
      f->IsSupported = venc_profile(&f->Profile, NULL);
      D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_H264 *s = f->PictureSupport.pH264Support;
      memset(s, 0, sizeof *s);
      if (f->IsSupported) {
        /* (the encoder searches every reference of the lists it is given, up to the slice syntax's 16; these are what it is meant for) */
        s->MaxL0ReferencesForP = 4;
        s->MaxL0ReferencesForB = 4;
        s->MaxL1ReferencesForB = 2;
        s->MaxLongTermReferences = VENC_MAX_REFERENCES;
        s->MaxDPBCapacity = VENC_MAX_REFERENCES;
      }
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_SUPPORT: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT);
      return venc_support(data, NULL);
    }
    case D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1 *f = data;
      /* (SUPPORT1 is SUPPORT with the subregion data and the quality levels after it) */
      const HRESULT hr = venc_support((D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT *)f, &f->SubregionFrameEncodingData);
      f->MaxQualityVsSpeed = 0;
      return hr;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS: {
      ENC_FEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = f->Codec == D3D12_VIDEO_ENCODER_CODEC_H264 && venc_profile(&f->Profile, NULL) && f->InputFormat == DXGI_FORMAT_NV12 && venc_resolution_ok(&f->PictureTargetResolution);
      f->CompressedBitstreamBufferAccessAlignment = f->IsSupported ? 1 : 0;
      f->EncoderMetadataBufferAccessAlignment = f->IsSupported ? 1 : 0;
      f->MaxEncoderOutputMetadataBufferSize = f->IsSupported ? (UINT)venc_metadata_bytes(f->PictureTargetResolution.Width, f->PictureTargetResolution.Height) : 0;
      return S_OK;
    }
    default: return S_FALSE;
  }
#undef ENC_FEATURE
}

/* ---- encoders and their heaps ------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct VEnc {
  DObj d;
  D3D12_VIDEO_ENCODER_DESC desc;
  D3D12_VIDEO_ENCODER_PROFILE_H264 profile;
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 config;
} VEnc;
typedef struct VEncHeap {
  DObj d;
  D3D12_VIDEO_ENCODER_HEAP_DESC desc;
  D3D12_VIDEO_ENCODER_PROFILE_H264 profile;
  D3D12_VIDEO_ENCODER_LEVELS_H264 level;
  D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *resolutions;
  UINT alloc;      /* the rate controller's state */
} VEncHeap;
static const GUID *const g_venc_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12VideoEncoder, NULL};
static const GUID *const g_veheap_iids[] = {&IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable, &IID_ID3D12VideoEncoderHeap, NULL};

static void veheap_destroy(DxObj *o) {
  VEncHeap *h = (VEncHeap *)o;
  if (h->alloc) kmt_destroy_allocation(h->d.dev->kdev, h->alloc);
  mem_free(h->resolutions);
  dobj_child_destroy(o);
}

static HRESULT venc_make(Dev *d, const D3D12_VIDEO_ENCODER_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12VideoEncoderVtbl venc_vtbl;
  if (out) *out = NULL;
  if (!desc || !out || desc->NodeMask > 1 || desc->Flags != D3D12_VIDEO_ENCODER_FLAG_NONE) return E_INVALIDARG;
  if (desc->EncodeCodec != D3D12_VIDEO_ENCODER_CODEC_H264) return (UINT)desc->EncodeCodec <= D3D12_VIDEO_ENCODER_CODEC_AV1 ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
  D3D12_VIDEO_ENCODER_PROFILE_H264 profile;
  D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 config;
  if (!venc_profile(&desc->EncodeProfile, &profile) || desc->InputFormat != DXGI_FORMAT_NV12 || !venc_config(&desc->CodecConfiguration, &config) ||
      (UINT)desc->MaxMotionEstimationPrecision > D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_EIGHTH_PIXEL)
    return E_INVALIDARG;
  VEnc *e = dobj_new(sizeof *e, d, &venc_vtbl, g_venc_iids, dobj_child_destroy);
  if (!e) return E_OUTOFMEMORY;
  e->desc = *desc;
  e->profile = profile;
  e->config = config;
  e->desc.EncodeProfile.pH264Profile = &e->profile;
  e->desc.CodecConfiguration.pH264Config = &e->config;
  return query_out((IUnknown *)e, riid, out);
}
static HRESULT veheap_make(Dev *d, const D3D12_VIDEO_ENCODER_HEAP_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12VideoEncoderHeapVtbl veheap_vtbl;
  if (out) *out = NULL;
  if (!desc || !out || desc->NodeMask > 1 || desc->Flags != D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE) return E_INVALIDARG;
  if (desc->EncodeCodec != D3D12_VIDEO_ENCODER_CODEC_H264) return (UINT)desc->EncodeCodec <= D3D12_VIDEO_ENCODER_CODEC_AV1 ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
  D3D12_VIDEO_ENCODER_PROFILE_H264 profile;
  D3D12_VIDEO_ENCODER_LEVELS_H264 level;
  if (!venc_profile(&desc->EncodeProfile, &profile) || !venc_level(&desc->EncodeLevel, &level) || !desc->ResolutionsListCount || desc->ResolutionsListCount > 256 || !desc->pResolutionList)
    return E_INVALIDARG;
  for (UINT i = 0; i < desc->ResolutionsListCount; ++i)
    if (!venc_resolution_ok(&desc->pResolutionList[i])) return E_INVALIDARG;
  VEncHeap *h = dobj_new(sizeof *h, d, &veheap_vtbl, g_veheap_iids, veheap_destroy);
  if (!h) return E_OUTOFMEMORY;
  h->desc = *desc;
  h->profile = profile;
  h->level = level;
  h->desc.EncodeProfile.pH264Profile = &h->profile;
  h->desc.EncodeLevel.pH264LevelSetting = &h->level;
  h->resolutions = mem_alloc(sizeof *h->resolutions * desc->ResolutionsListCount);
  h->alloc = kbuffer_new(d, VENC_HEAP_BYTES);
  if (!h->resolutions || !h->alloc) { dx_Release(OBJ(h)); return E_OUTOFMEMORY; }
  memcpy(h->resolutions, desc->pResolutionList, sizeof *h->resolutions * desc->ResolutionsListCount);
  h->desc.pResolutionList = h->resolutions;
  return query_out((IUnknown *)h, riid, out);
}

static UINT STDMETHODCALLTYPE venc_GetNodeMask(VEnc *This) { return This->desc.NodeMask; }
static D3D12_VIDEO_ENCODER_FLAGS STDMETHODCALLTYPE venc_GetEncoderFlags(VEnc *This) { return This->desc.Flags; }
static D3D12_VIDEO_ENCODER_CODEC STDMETHODCALLTYPE venc_GetCodec(VEnc *This) { return This->desc.EncodeCodec; }
static HRESULT STDMETHODCALLTYPE venc_GetCodecProfile(VEnc *This, D3D12_VIDEO_ENCODER_PROFILE_DESC dst) {
  if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_PROFILE_H264) || !dst.pH264Profile) return E_INVALIDARG;
  *dst.pH264Profile = This->profile;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE venc_GetCodecConfiguration(VEnc *This, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION dst) {
  if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264) || !dst.pH264Config) return E_INVALIDARG;
  *dst.pH264Config = This->config;
  return S_OK;
}
static DXGI_FORMAT STDMETHODCALLTYPE venc_GetInputFormat(VEnc *This) { return This->desc.InputFormat; }
static D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE STDMETHODCALLTYPE venc_GetMaxMotionEstimationPrecision(VEnc *This) { return This->desc.MaxMotionEstimationPrecision; }
static UINT STDMETHODCALLTYPE veheap_GetNodeMask(VEncHeap *This) { return This->desc.NodeMask; }
static D3D12_VIDEO_ENCODER_HEAP_FLAGS STDMETHODCALLTYPE veheap_GetEncoderHeapFlags(VEncHeap *This) { return This->desc.Flags; }
static D3D12_VIDEO_ENCODER_CODEC STDMETHODCALLTYPE veheap_GetCodec(VEncHeap *This) { return This->desc.EncodeCodec; }
static HRESULT STDMETHODCALLTYPE veheap_GetCodecProfile(VEncHeap *This, D3D12_VIDEO_ENCODER_PROFILE_DESC dst) {
  if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_PROFILE_H264) || !dst.pH264Profile) return E_INVALIDARG;
  *dst.pH264Profile = This->profile;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE veheap_GetCodecLevel(VEncHeap *This, D3D12_VIDEO_ENCODER_LEVEL_SETTING dst) {
  if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_LEVELS_H264) || !dst.pH264LevelSetting) return E_INVALIDARG;
  *dst.pH264LevelSetting = This->level;
  return S_OK;
}
static UINT STDMETHODCALLTYPE veheap_GetResolutionListCount(VEncHeap *This) { return This->desc.ResolutionsListCount; }
static HRESULT STDMETHODCALLTYPE veheap_GetResolutionList(VEncHeap *This, const UINT count, D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *list) {
  if (!list || count != This->desc.ResolutionsListCount) return E_INVALIDARG;
  memcpy(list, This->resolutions, sizeof *list * count);
  return S_OK;
}

/* ---- the encode command list ------------------------------------------------------------------------------------------------------------------------------------- */

static int venc_same_profile(const VEnc *e, const VEncHeap *h) { return e->desc.EncodeCodec == h->desc.EncodeCodec && e->profile == h->profile; }

static void STDMETHODCALLTYPE velist_EncodeFrame(CmdList *l, ID3D12VideoEncoder *encoder, ID3D12VideoEncoderHeap *heap, const D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS *in,
                                                 const D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS *out) {
  VEnc *e = (VEnc *)encoder;
  VEncHeap *h = (VEncHeap *)heap;
  if (!e || !h || !in || !out || l->closed) return;
  Resource *input = (Resource *)in->pInputFrame, *bits = (Resource *)out->Bitstream.pBuffer, *meta = (Resource *)out->EncoderOutputMetadata.pBuffer;
  Resource *recon = (Resource *)out->ReconstructedPicture.pReconstructedPicture;
  UINT il, ic, rl = 0, rc = 0;
  /* (what is not there to write into or read from makes no packet: the metadata could not say anything) */
  if (!bits || bits->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || out->Bitstream.FrameStartOffset > bits->desc.Width || out->Bitstream.FrameStartOffset > 0xFFFFFFFFu) return;
  if (!meta || meta->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || out->EncoderOutputMetadata.Offset > meta->desc.Width || out->EncoderOutputMetadata.Offset > 0xFFFFFFFFu) return;
  if (!nv12_planes(input, in->InputFrameSubresource, &il, &ic)) return;
  if (recon && !nv12_planes(recon, out->ReconstructedPicture.ReconstructedPictureSubresource, &rl, &rc)) return;
  const D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC *seq = &in->SequenceControlDesc;
  const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC *pc = &in->PictureControlDesc;
  const D3D12_VIDEO_ENCODE_REFERENCE_FRAMES *rf = &pc->ReferenceFrames;
  if (rf->NumTexture2Ds > VENC_MAX_REFERENCES || (rf->NumTexture2Ds && !rf->ppTexture2Ds)) return;
  UINT refs[2 * VENC_MAX_REFERENCES];
  for (UINT i = 0; i < rf->NumTexture2Ds; ++i)
    if (!nv12_planes((Resource *)rf->ppTexture2Ds[i], rf->pSubresources ? rf->pSubresources[i] : 0, &refs[2 * i], &refs[2 * i + 1])) return;

  /* the kernel's arguments (VideoEncode.hpp); what is wrong with them goes to the metadata as errors (word 48) */
  UINT w[VENC_PARAM_WORDS];
  memset(w, 0, sizeof w);
  UINT errors = 0;
  const UINT width = seq->PictureTargetResolution.Width, height = seq->PictureTargetResolution.Height;
  int listed = 0;
  for (UINT i = 0; i < h->desc.ResolutionsListCount; ++i) listed |= h->resolutions[i].Width == width && h->resolutions[i].Height == height;
  if (!venc_same_profile(e, h) || !listed || input->desc.Width < width || input->desc.Height < height) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
  if (seq->IntraRefreshConfig.Mode != D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
  w[0] = 0;
  w[1] = e->profile == D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH ? 1 : 0;
  w[2] = width;
  w[3] = height;
  w[4] = e->config.ConfigurationFlags;
  w[5] = e->config.DirectModeConfig;
  w[6] = e->config.DisableDeblockingFilterConfig;
  if (!venc_gop(&seq->CodecGopSequence, w)) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
  if (!venc_rate_control(&seq->RateControl, w)) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
  w[31] = seq->SelectedLayoutMode;
  if (seq->SelectedLayoutMode != D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) {
    const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA *ld = &seq->FrameSubregionsLayoutData;
    if (ld->DataSize != sizeof(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES) || !ld->pSlicesPartition_H264) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_SUBREGION_LAYOUT_CONFIGURATION_NOT_SUPPORTED;
    else w[32] = ld->pSlicesPartition_H264->NumberOfCodingUnitsPerSlice;
  }
  w[33] = e->desc.MaxMotionEstimationPrecision;
  const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 *p = pc->PictureControlCodecData.pH264PicData;
  if (pc->PictureControlCodecData.DataSize != sizeof *p || !p) { errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED; p = NULL; }
  UINT nd = 0, n0 = 0, n1 = 0, nm = 0, nm0 = 0, nm1 = 0, nq = 0;
  if (p) {
    w[34] = p->FrameType;
    w[35] = p->idr_pic_id;
    w[36] = p->pic_parameter_set_id;
    w[37] = p->PictureOrderCountNumber;
    w[38] = p->FrameDecodingOrderNumber;
    w[39] = ((pc->Flags & D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE) ? 1 : 0) |
            ((p->Flags & D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_FLAG_REQUEST_INTRA_CONSTRAINED_SLICES) ? 2 : 0);
    w[40] = p->adaptive_ref_pic_marking_mode_flag;
    nd = p->ReferenceFramesReconPictureDescriptorsCount;
    n0 = p->List0ReferenceFramesCount;
    n1 = p->List1ReferenceFramesCount;
    nm = p->adaptive_ref_pic_marking_mode_flag ? p->RefPicMarkingOperationsCommandsCount : 0;
    nm0 = p->List0RefPicModificationsCount;
    nm1 = p->List1RefPicModificationsCount;
    nq = p->QPMapValuesCount;
    if (nd > VENC_MAX_REFERENCES || n0 > VENC_MAX_REFERENCES || n1 > VENC_MAX_REFERENCES || (nd && !p->pReferenceFramesReconPictureDescriptors) || (n0 && !p->pList0ReferenceFrames) ||
        (n1 && !p->pList1ReferenceFrames)) {
      errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_INVALID_REFERENCE_PICTURES;
      nd = n0 = n1 = 0;
    }
    if (nm > 64 || nm0 > 64 || nm1 > 64 || (nm && !p->pRefPicMarkingOperationsCommands) || (nm0 && !p->pList0RefPicModifications) || (nm1 && !p->pList1RefPicModifications) ||
        nq > (1u << 20) || (nq && !p->pRateControlQPMap)) {
      errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
      nm = nm0 = nm1 = nq = 0;
    }
    for (UINT i = 0; i < nd; ++i)
      if (p->pReferenceFramesReconPictureDescriptors[i].ReconstructedPictureResourceIndex >= rf->NumTexture2Ds) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_INVALID_REFERENCE_PICTURES;
  }
  w[41] = nd; w[42] = n0; w[43] = n1; w[44] = nm; w[45] = nm0; w[46] = nm1; w[47] = nq;
  w[48] = errors;
  w[49] = meta->desc.Width - out->EncoderOutputMetadata.Offset >= VENC_METADATA_HEADER ? (UINT)((meta->desc.Width - out->EncoderOutputMetadata.Offset - VENC_METADATA_HEADER) / VENC_SUBREGION_BYTES) : 0;
  const UINT words = VENC_PARAM_WORDS + 6 * nd + n0 + n1 + 5 * nm + 3 * (nm0 + nm1);
  const UINT bytes = words * 4 + (nq + 3) / 4 * 4;
  UINT *blob = mem_alloc(bytes);
  if (!blob) return;
  memcpy(blob, w, sizeof w);
  UINT at = VENC_PARAM_WORDS;
  for (UINT i = 0; i < nd; ++i) {
    const D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264 *r = &p->pReferenceFramesReconPictureDescriptors[i];
    blob[at++] = r->ReconstructedPictureResourceIndex; blob[at++] = (UINT)r->IsLongTermReference; blob[at++] = r->LongTermPictureIdx;
    blob[at++] = r->PictureOrderCountNumber; blob[at++] = r->FrameDecodingOrderNumber; blob[at++] = r->TemporalLayerIndex;
  }
  for (UINT i = 0; i < n0; ++i) blob[at++] = p->pList0ReferenceFrames[i];
  for (UINT i = 0; i < n1; ++i) blob[at++] = p->pList1ReferenceFrames[i];
  for (UINT i = 0; i < nm; ++i) {
    const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_REFERENCE_PICTURE_MARKING_OPERATION *m = &p->pRefPicMarkingOperationsCommands[i];
    blob[at++] = m->memory_management_control_operation; blob[at++] = m->difference_of_pic_nums_minus1; blob[at++] = m->long_term_pic_num;
    blob[at++] = m->long_term_frame_idx; blob[at++] = m->max_long_term_frame_idx_plus1;
  }
  for (UINT k = 0; k < 2; ++k)
    for (UINT i = 0; i < (k == 0 ? nm0 : nm1); ++i) {
      const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_REFERENCE_PICTURE_LIST_MODIFICATION_OPERATION *m = k == 0 ? &p->pList0RefPicModifications[i] : &p->pList1RefPicModifications[i];
      blob[at++] = m->modification_of_pic_nums_idc; blob[at++] = m->abs_diff_pic_num_minus1; blob[at++] = m->long_term_pic_num;
    }
  if (nq) memcpy(blob + at, p->pRateControlQPMap, nq);
  const UINT args = list_temp_buffer(l, blob, bytes);
  mem_free(blob);
  if (!args) return;
  ensure_room(l, 128 + 8 * rf->NumTexture2Ds, 12 + 2 * rf->NumTexture2Ds);
  UINT pw[14 + 2 * VENC_MAX_REFERENCES];
  pw[0] = alloc_index(l, args); pw[1] = 0; pw[2] = bytes;
  pw[3] = alloc_index(l, h->alloc); pw[4] = 0;
  pw[5] = alloc_index(l, il); pw[6] = alloc_index(l, ic);
  pw[7] = recon ? alloc_index(l, rl) : 0xFFFFFFFFu; pw[8] = recon ? alloc_index(l, rc) : 0xFFFFFFFFu;
  pw[9] = alloc_index(l, bits->alloc); pw[10] = (UINT)out->Bitstream.FrameStartOffset;
  pw[11] = alloc_index(l, meta->alloc); pw[12] = (UINT)out->EncoderOutputMetadata.Offset;
  pw[13] = rf->NumTexture2Ds;
  for (UINT i = 0; i < 2 * rf->NumTexture2Ds; ++i) pw[14 + i] = alloc_index(l, refs[i]);
  packet(l, OP_ENCODE_FRAME, pw, 14 + 2 * rf->NumTexture2Ds);
  keep(l, (IUnknown *)encoder);
  keep(l, (IUnknown *)heap);
}
static void STDMETHODCALLTYPE velist_ResolveEncoderOutputMetadata(CmdList *l, const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS *in,
                                                                  const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS *out) {
  if (!in || !out || l->closed) return;
  Resource *src = (Resource *)in->HWLayoutMetadata.pBuffer, *dst = (Resource *)out->ResolvedLayoutMetadata.pBuffer;
  if (!src || !dst || src == dst || src->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || dst->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
      in->HWLayoutMetadata.Offset > src->desc.Width || out->ResolvedLayoutMetadata.Offset > dst->desc.Width || in->HWLayoutMetadata.Offset > 0xFFFFFFFFu ||
      out->ResolvedLayoutMetadata.Offset > 0xFFFFFFFFu || in->EncoderCodec != D3D12_VIDEO_ENCODER_CODEC_H264)
    return;
  ensure_room(l, 64, 4);
  const UINT words[4] = {alloc_index(l, src->alloc), (UINT)in->HWLayoutMetadata.Offset, alloc_index(l, dst->alloc), (UINT)out->ResolvedLayoutMetadata.Offset};
  packet(l, OP_RESOLVE_ENCODER_METADATA, words, 4);
}
