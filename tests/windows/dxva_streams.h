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

/* dxva_streams.h: elementary video streams turned into the DXVA buffers a program hands a decoder, filled the way ffmpeg's libavcodec/dxva2_h264.c and dxva2_mpeg2.c fill
 * them - for the tests of Direct3D 11's and Direct3D 12's video decoding (tests/windows/d3d11videotest.c, d3d12videotest.c). It does not name an API: picture by picture,
 * in decoding order, it gives the picture parameters, the inverse quantization matrices, the slice controls and the bitstream (ready to be submitted as they are), the
 * surface index the picture goes into and the surface indices of the references it may read. The program maps the indices to its surfaces: in Direct3D 11 an index is
 * an array slice of the output texture; Direct3D 12 takes a list of reference textures, in which an index is a position.
 *
 *   H.264: Annex B byte streams of frames (frame_mbs_only_flag 1; no slice groups, no gaps in frame_num): SPS, PPS, POC types 0, 1 and 2, reference marking by the
 *          sliding window and by memory management control operations 1 .. 6, scaling lists with the fall-back rules; DXVA_PicParams_H264, DXVA_Qmatrix_H264 and the
 *          short slice format (DXVA_Slice_H264_Short, ConfigBitstreamRaw 2), each slice NAL unit with a 00 00 01 start code, the buffer padded to 128 bytes.
 *   MPEG-1 / MPEG-2: video elementary streams, frame and field pictures; DXVA_PictureParameters, DXVA_QmatrixData (always sent), DXVA_SliceInfo, the slices back to
 *          back from their start codes, the buffer padded to 128 bytes.
 *
 * Surfaces: indices 0 .. surfaces - 1, taken in turn, skipping those the stream's references are in (the DPB; MPEG-2's two anchors and a frame whose second field is to
 * come). A picture's surface is not used again before `surfaces` pictures later at the soonest less the references: a program that keeps decoded pictures for display
 * copies them out (or gives more surfaces). `display_key` sorts the pictures into display order.
 *
 * Static functions and types prefixed dxva_ / Dxva only; uses the C library (malloc, free, memcpy, memset).
 *
 *   DxvaStream s;
 *   if (dxva_stream_open(&s, DXVA_CODEC_H264, data, size, 17) == 0) {
 *     DxvaPicture p;
 *     while (dxva_stream_next(&s, &p) == 1) { ... submit p.picture_parameters, p.qmatrix, p.slice_control, p.bitstream into surface p.surface ... }
 *     dxva_stream_close(&s);
 *   }
 * dxva_stream_next answers 1 for a picture (its buffers stay valid until the next call), 0 at the end of the stream, -1 for a stream it does not take (s.error says why). */
#ifndef TCOS_DXVA_STREAMS_H
#define TCOS_DXVA_STREAMS_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <dxva.h>

enum { DXVA_CODEC_H264 = 1, DXVA_CODEC_MPEG2 = 2 };
#define DXVA_STREAM_MAX_SURFACES 128u

typedef struct DxvaPicture {
  unsigned surface;                        /* where the picture is decoded to (CurrPic.Index7Bits / wDecodedPictureIndex) */
  unsigned ref_count;                      /* the surfaces its references are in */
  unsigned refs[16];
  const void *picture_parameters;          /* DXVA_PicParams_H264 / DXVA_PictureParameters */
  unsigned picture_parameters_size;
  const void *qmatrix;                     /* DXVA_Qmatrix_H264 / DXVA_QmatrixData */
  unsigned qmatrix_size;
  const void *slice_control;               /* DXVA_Slice_H264_Short[] / DXVA_SliceInfo[] */
  unsigned slice_control_size, slice_count;
  const unsigned char *bitstream;
  unsigned bitstream_size;
  long long display_key;                   /* sorts the pictures into display order (a field picture: both fields of a frame have the frame's) */
  int is_reference;                        /* (H.264: nal_ref_idc != 0; MPEG-2: an I or P picture) */
  char type;                               /* 'I', 'P' or 'B' (H.264: the lowest of the slices' types, I < P < B) */
  int field;                               /* MPEG-2: 0 a frame picture, 1 the first field, 2 the second field */
} DxvaPicture;

typedef struct DxvaH264State DxvaH264State;
typedef struct DxvaMpeg2State DxvaMpeg2State;
typedef struct DxvaStream {
  int codec;
  const unsigned char *data;
  size_t size, pos;
  unsigned surfaces, next_surface;
  unsigned width, height;                   /* the coded size (whole macroblocks), known after the first picture */
  unsigned crop_left, crop_right, crop_top, crop_bottom;      /* what of it is shown (H.264's frame cropping; MPEG-2: the picture size) */
  unsigned feedback;                        /* (StatusReportFeedbackNumber, from 1) */
  const char *error;
  /* the buffers of the picture being assembled */
  unsigned char *pp, *qm, *sc, *bs;
  size_t pp_size, qm_size, sc_size, sc_cap, bs_size, bs_cap;
  unsigned slices;
  DxvaH264State *h264;
  DxvaMpeg2State *mpeg2;
} DxvaStream;

/* ---- common ------------------------------------------------------------------------------------------------------------------------------------------------------- */

static int dxva_grow(unsigned char **p, size_t *cap, size_t need) {
  if (need <= *cap) return 1;
  size_t n = *cap ? *cap : 4096;
  while (n < need) n *= 2;
  unsigned char *q = (unsigned char *)realloc(*p, n);
  if (!q) return 0;
  *p = q;
  *cap = n;
  return 1;
}
static int dxva_append(unsigned char **p, size_t *size, size_t *cap, const void *data, size_t n) {
  if (!dxva_grow(p, cap, *size + n)) return 0;
  if (n) memcpy(*p + *size, data, n);
  *size += n;
  return 1;
}
/* The next start code (00 00 01) at or after `from`: its offset, or `size`. */
static size_t dxva_find_start(const unsigned char *d, size_t size, size_t from) {
  for (size_t i = from; i + 2 < size; ++i)
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
  return size;
}
/* A surface for the next picture: the next in turn that holds none of `busy`. */
static unsigned dxva_pick_surface(DxvaStream *s, const unsigned *busy, unsigned busy_count) {
  for (unsigned tries = 0; tries < s->surfaces; ++tries) {
    const unsigned c = s->next_surface;
    s->next_surface = (s->next_surface + 1) % s->surfaces;
    int taken = 0;
    for (unsigned i = 0; i < busy_count; ++i) taken |= busy[i] == c;
    if (!taken) return c;
  }
  return ~0u;
}
/* The picture's slices padded with zeros to 128 bytes (as ffmpeg's commit_bitstream_and_slice_buffer does); H.264's counts the padding in its last slice (`last_size`:
 * where that slice control keeps its size in bytes; ~0: not counted, as dxva2_mpeg2.c has it). */
static int dxva_pad(DxvaStream *s, size_t last_size_offset) {
  static const unsigned char zeros[128] = {0};
  const size_t padding = (128 - (s->bs_size & 127)) & 127;
  if (!padding || !s->slices) return 1;
  if (!dxva_append(&s->bs, &s->bs_size, &s->bs_cap, zeros, padding)) return 0;
  if (last_size_offset == (size_t)~(size_t)0) return 1;
  unsigned v;
  memcpy(&v, s->sc + last_size_offset, 4);
  v += (unsigned)padding;
  memcpy(s->sc + last_size_offset, &v, 4);
  return 1;
}

/* ---- H.264 ------------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct DxvaBits { const unsigned char *d; size_t bits, pos; int bad; } DxvaBits;
static unsigned dxva_u(DxvaBits *b, unsigned n) {
  unsigned v = 0;
  for (unsigned i = 0; i < n; ++i, ++b->pos) {
    if (b->pos >= b->bits) { b->bad = 1; v <<= 1; continue; }
    v = (v << 1) | ((b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u);
  }
  return v;
}
static unsigned dxva_ue(DxvaBits *b) {
  unsigned zeros = 0;
  while (!dxva_u(b, 1)) { if (++zeros > 31 || b->bad) { b->bad = 1; return 0; } }
  return zeros ? (1u << zeros) - 1 + dxva_u(b, zeros) : 0;
}
static int dxva_se(DxvaBits *b) {
  const unsigned k = dxva_ue(b);
  return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
}
/* Whether there is more of the RBSP before its stop bit (the last 1 bit). */
static int dxva_more_rbsp(const DxvaBits *b) {
  size_t last = b->bits;
  while (last > 0 && !((b->d[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1)) --last;
  return last > 0 && b->pos < last - 1;
}

static const unsigned char g_dxva_default4_intra[16] = {6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42};
static const unsigned char g_dxva_default4_inter[16] = {10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34};
static const unsigned char g_dxva_default8_intra[64] = {6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23, 25,
                                                        25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31,
                                                        31, 31, 31, 31, 31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42};
static const unsigned char g_dxva_default8_inter[64] = {9,  13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21, 21, 21, 22,
                                                        22, 22, 22, 22, 22, 22, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27,
                                                        27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35};

typedef struct DxvaH264Sps {
  int valid;
  unsigned profile_idc, level_idc, chroma_format_idc, separate_colour_plane, bit_depth_luma, bit_depth_chroma;
  unsigned log2_max_frame_num, poc_type, log2_max_poc_lsb, delta_pic_order_always_zero, num_ref_frames_in_poc_cycle;
  int offset_for_non_ref_pic, offset_for_top_to_bottom_field, offset_for_ref_frame[256];
  unsigned max_num_ref_frames, gaps_allowed, width_mbs, height_map_units, frame_mbs_only, mb_aff, direct_8x8;
  unsigned crop_left, crop_right, crop_top, crop_bottom;
  int has_matrix;
  unsigned char sl4[6][16], sl8[6][64];      /* (in zigzag order, as coded) */
} DxvaH264Sps;
typedef struct DxvaH264Pps {
  int valid;
  unsigned sps_id, cabac, pic_order_present, num_slice_groups_minus1, slice_group_map_type, ref_count[2], weighted_pred, weighted_bipred_idc;
  unsigned deblocking_control_present, constrained_intra_pred, redundant_pic_cnt_present, transform_8x8;
  int init_qp, init_qs, chroma_qp_offset[2];
  unsigned char sl4[6][16], sl8[6][64];
} DxvaH264Pps;
typedef struct DxvaH264Ref { unsigned surface, frame_num, long_idx; int long_term; int poc[2]; } DxvaH264Ref;
typedef struct DxvaH264Mmco { unsigned op, a, b; } DxvaH264Mmco;

struct DxvaH264State {
  DxvaH264Sps sps[32];
  DxvaH264Pps pps[256];
  DxvaH264Ref shortr[16];                   /* the short-term references, the latest first (ffmpeg's short_ref) */
  unsigned short_count;
  DxvaH264Ref longr[16];                    /* the long-term references by LongTermFrameIdx */
  int long_used[16];
  int max_long_idx;                         /* (-1: no long-term frame index) */
  /* the picture order count state of the previous (reference) picture */
  int prev_poc_msb, prev_poc_lsb;
  unsigned prev_frame_num, prev_frame_num_offset, prev_ref_frame_num;
  long long period;                         /* (display_key: increases at an IDR or a picture with MMCO 5) */
  /* the picture being assembled */
  int pending;
  const DxvaH264Sps *sps_cur;
  const DxvaH264Pps *pps_cur;
  unsigned nal_ref_idc, idr, frame_num, long_term_reference, adaptive, mmco_count;
  DxvaH264Mmco mmco[66];
  int poc[2], poc_lsb, poc_msb;
  unsigned frame_num_offset;
  char type;
  unsigned surface;
};

/* The RBSP of a NAL unit (its emulation prevention bytes taken out), from the byte after its header. */
static size_t dxva_rbsp(const unsigned char *nal, size_t size, unsigned char *out) {
  size_t n = 0, zeros = 0;
  for (size_t i = 1; i < size; ++i) {
    if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
    zeros = nal[i] == 0 ? zeros + 1 : 0;
    out[n++] = nal[i];
  }
  return n;
}
/* scaling_list(): `list` in coded (zigzag) order; FALSE when the default list is to be used. */
static int dxva_scaling_list(DxvaBits *b, unsigned char *list, unsigned n) {
  int last = 8, next = 8;
  for (unsigned j = 0; j < n; ++j) {
    if (next != 0) {
      next = (last + dxva_se(b) + 256) % 256;
      if (j == 0 && next == 0) return 0;
    }
    list[j] = (unsigned char)(next == 0 ? last : next);
    last = list[j];
  }
  return 1;
}
/* The scaling lists of an SPS (fall-back rule A) or a PPS (rule B over `sps` when the SPS has lists, else rule A). `count`: 6 4x4 lists and 2 (or 6) 8x8 ones. */
static void dxva_scaling_matrices(DxvaBits *b, unsigned char sl4[6][16], unsigned char sl8[6][64], unsigned count8, const DxvaH264Sps *sps) {
  const int rule_b = sps && sps->has_matrix;
  for (unsigned i = 0; i < 6; ++i) {
    if (dxva_u(b, 1)) {
      if (!dxva_scaling_list(b, sl4[i], 16)) memcpy(sl4[i], i < 3 ? g_dxva_default4_intra : g_dxva_default4_inter, 16);
    } else if (i == 0 || i == 3) memcpy(sl4[i], rule_b ? sps->sl4[i] : i == 0 ? g_dxva_default4_intra : g_dxva_default4_inter, 16);
    else memcpy(sl4[i], sl4[i - 1], 16);
  }
  for (unsigned i = 0; i < count8; ++i) {
    if (dxva_u(b, 1)) {
      if (!dxva_scaling_list(b, sl8[i], 64)) memcpy(sl8[i], i % 2 == 0 ? g_dxva_default8_intra : g_dxva_default8_inter, 64);
    } else if (i < 2) memcpy(sl8[i], rule_b ? sps->sl8[i] : i == 0 ? g_dxva_default8_intra : g_dxva_default8_inter, 64);
    else memcpy(sl8[i], sl8[i - 2], 64);
  }
}
static void dxva_flat(unsigned char sl4[6][16], unsigned char sl8[6][64]) {
  memset(sl4, 16, 6 * 16);
  memset(sl8, 16, 6 * 64);
}

static int dxva_h264_sps(DxvaStream *s, DxvaBits *b) {
  DxvaH264Sps sps;
  memset(&sps, 0, sizeof sps);
  sps.profile_idc = dxva_u(b, 8);
  dxva_u(b, 8);
  sps.level_idc = dxva_u(b, 8);
  const unsigned id = dxva_ue(b);
  if (id >= 32) { s->error = "an SPS id above 31"; return 0; }
  sps.chroma_format_idc = 1;
  sps.bit_depth_luma = sps.bit_depth_chroma = 8;
  dxva_flat(sps.sl4, sps.sl8);
  const unsigned p = sps.profile_idc;
  if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 || p == 139 || p == 134 || p == 135) {
    sps.chroma_format_idc = dxva_ue(b);
    if (sps.chroma_format_idc == 3) sps.separate_colour_plane = dxva_u(b, 1);
    sps.bit_depth_luma = 8 + dxva_ue(b);
    sps.bit_depth_chroma = 8 + dxva_ue(b);
    dxva_u(b, 1);      /* qpprime_y_zero_transform_bypass_flag */
    if (dxva_u(b, 1)) {
      sps.has_matrix = 1;
      dxva_scaling_matrices(b, sps.sl4, sps.sl8, sps.chroma_format_idc == 3 ? 6 : 2, NULL);
    }
  }
  sps.log2_max_frame_num = 4 + dxva_ue(b);
  sps.poc_type = dxva_ue(b);
  if (sps.poc_type == 0) sps.log2_max_poc_lsb = 4 + dxva_ue(b);
  else if (sps.poc_type == 1) {
    sps.delta_pic_order_always_zero = dxva_u(b, 1);
    sps.offset_for_non_ref_pic = dxva_se(b);
    sps.offset_for_top_to_bottom_field = dxva_se(b);
    sps.num_ref_frames_in_poc_cycle = dxva_ue(b);
    if (sps.num_ref_frames_in_poc_cycle > 255) { s->error = "a POC cycle of more than 255 frames"; return 0; }
    for (unsigned i = 0; i < sps.num_ref_frames_in_poc_cycle; ++i) sps.offset_for_ref_frame[i] = dxva_se(b);
  }
  sps.max_num_ref_frames = dxva_ue(b);
  sps.gaps_allowed = dxva_u(b, 1);
  sps.width_mbs = dxva_ue(b) + 1;
  sps.height_map_units = dxva_ue(b) + 1;
  sps.frame_mbs_only = dxva_u(b, 1);
  if (!sps.frame_mbs_only) sps.mb_aff = dxva_u(b, 1);
  sps.direct_8x8 = dxva_u(b, 1);
  if (dxva_u(b, 1)) {
    sps.crop_left = dxva_ue(b);
    sps.crop_right = dxva_ue(b);
    sps.crop_top = dxva_ue(b);
    sps.crop_bottom = dxva_ue(b);
  }
  if (b->bad || sps.log2_max_frame_num > 16 || sps.log2_max_poc_lsb > 16 || sps.max_num_ref_frames > 16) { s->error = "an SPS that does not parse"; return 0; }
  sps.valid = 1;
  s->h264->sps[id] = sps;
  return 1;
}
static int dxva_h264_pps(DxvaStream *s, DxvaBits *b) {
  DxvaH264Pps pps;
  memset(&pps, 0, sizeof pps);
  const unsigned id = dxva_ue(b);
  pps.sps_id = dxva_ue(b);
  if (id >= 256 || pps.sps_id >= 32 || !s->h264->sps[pps.sps_id].valid) { s->error = "a PPS of no SPS"; return 0; }
  const DxvaH264Sps *sps = &s->h264->sps[pps.sps_id];
  pps.cabac = dxva_u(b, 1);
  pps.pic_order_present = dxva_u(b, 1);
  pps.num_slice_groups_minus1 = dxva_ue(b);
  if (pps.num_slice_groups_minus1) { s->error = "slice groups (FMO)"; return 0; }
  pps.ref_count[0] = dxva_ue(b) + 1;
  pps.ref_count[1] = dxva_ue(b) + 1;
  pps.weighted_pred = dxva_u(b, 1);
  pps.weighted_bipred_idc = dxva_u(b, 2);
  pps.init_qp = 26 + dxva_se(b);
  pps.init_qs = 26 + dxva_se(b);
  pps.chroma_qp_offset[0] = pps.chroma_qp_offset[1] = dxva_se(b);
  pps.deblocking_control_present = dxva_u(b, 1);
  pps.constrained_intra_pred = dxva_u(b, 1);
  pps.redundant_pic_cnt_present = dxva_u(b, 1);
  memcpy(pps.sl4, sps->sl4, sizeof pps.sl4);
  memcpy(pps.sl8, sps->sl8, sizeof pps.sl8);
  if (dxva_more_rbsp(b)) {
    pps.transform_8x8 = dxva_u(b, 1);
    if (dxva_u(b, 1)) dxva_scaling_matrices(b, pps.sl4, pps.sl8, pps.transform_8x8 ? (sps->chroma_format_idc == 3 ? 6 : 2) : 0, sps);
    pps.chroma_qp_offset[1] = dxva_se(b);
  }
  if (b->bad || pps.ref_count[0] > 32 || pps.ref_count[1] > 32) { s->error = "a PPS that does not parse"; return 0; }
  pps.valid = 1;
  s->h264->pps[id] = pps;
  return 1;
}

/* The picture parameters of the picture whose first slice header was just read (ffmpeg's fill_picture_parameters), the scaling lists (fill_scaling_lists), and its
 * surface. The references are the DPB before the picture's own marking. */
static int dxva_h264_begin_picture(DxvaStream *s) {
  DxvaH264State *h = s->h264;
  const DxvaH264Sps *sps = h->sps_cur;
  const DxvaH264Pps *pps = h->pps_cur;
  unsigned busy[32], nbusy = 0;
  for (unsigned i = 0; i < h->short_count; ++i) busy[nbusy++] = h->shortr[i].surface;
  for (unsigned i = 0; i < 16; ++i) if (h->long_used[i]) busy[nbusy++] = h->longr[i].surface;
  h->surface = dxva_pick_surface(s, busy, nbusy);
  if (h->surface == ~0u) { s->error = "no free surface (give more)"; return 0; }
  DXVA_PicParams_H264 pp;
  memset(&pp, 0, sizeof pp);
  pp.CurrPic.bPicEntry = (UCHAR)h->surface;
  unsigned j = 0;
  for (unsigned i = 0; i < 16; ++i) {
    const DxvaH264Ref *r = NULL;
    if (j < h->short_count) r = &h->shortr[j++];
    else {
      while (!r && j < h->short_count + 16) {
        const unsigned k = j++ - h->short_count;
        if (h->long_used[k]) r = &h->longr[k];
      }
    }
    if (r) {
      pp.RefFrameList[i].bPicEntry = (UCHAR)(r->surface | (r->long_term ? 0x80 : 0));
      pp.FieldOrderCntList[i][0] = r->poc[0];
      pp.FieldOrderCntList[i][1] = r->poc[1];
      pp.FrameNumList[i] = (USHORT)(r->long_term ? r->long_idx : r->frame_num);
      pp.UsedForReferenceFlags |= 3u << (2 * i);
    } else pp.RefFrameList[i].bPicEntry = 0xFF;
  }
  pp.wFrameWidthInMbsMinus1 = (USHORT)(sps->width_mbs - 1);
  pp.wFrameHeightInMbsMinus1 = (USHORT)(sps->height_map_units * (2 - sps->frame_mbs_only) - 1);
  pp.num_ref_frames = (UCHAR)sps->max_num_ref_frames;
  pp.wBitFields = (USHORT)((0u << 0) | (0u << 1) | (sps->separate_colour_plane << 2) | (sps->chroma_format_idc << 4) | ((h->nal_ref_idc != 0) << 6) |
                           (pps->constrained_intra_pred << 7) | (pps->weighted_pred << 8) | (pps->weighted_bipred_idc << 9) | (1u << 11) | (sps->frame_mbs_only << 12) |
                           (pps->transform_8x8 << 13) | ((sps->level_idc >= 31) << 14) | (1u << 15));
  pp.bit_depth_luma_minus8 = (UCHAR)(sps->bit_depth_luma - 8);
  pp.bit_depth_chroma_minus8 = (UCHAR)(sps->bit_depth_chroma - 8);
  pp.Reserved16Bits = 3;
  pp.StatusReportFeedbackNumber = ++s->feedback;
  pp.CurrFieldOrderCnt[0] = h->poc[0];
  pp.CurrFieldOrderCnt[1] = h->poc[1];
  pp.pic_init_qs_minus26 = (CHAR)(pps->init_qs - 26);
  pp.chroma_qp_index_offset = (CHAR)pps->chroma_qp_offset[0];
  pp.second_chroma_qp_index_offset = (CHAR)pps->chroma_qp_offset[1];
  pp.ContinuationFlag = 1;
  pp.pic_init_qp_minus26 = (CHAR)(pps->init_qp - 26);
  pp.num_ref_idx_l0_active_minus1 = (UCHAR)(pps->ref_count[0] - 1);
  pp.num_ref_idx_l1_active_minus1 = (UCHAR)(pps->ref_count[1] - 1);
  pp.frame_num = (USHORT)h->frame_num;
  pp.log2_max_frame_num_minus4 = (UCHAR)(sps->log2_max_frame_num - 4);
  pp.pic_order_cnt_type = (UCHAR)sps->poc_type;
  if (sps->poc_type == 0) pp.log2_max_pic_order_cnt_lsb_minus4 = (UCHAR)(sps->log2_max_poc_lsb - 4);
  else if (sps->poc_type == 1) pp.delta_pic_order_always_zero_flag = (UCHAR)sps->delta_pic_order_always_zero;
  pp.direct_8x8_inference_flag = (UCHAR)sps->direct_8x8;
  pp.entropy_coding_mode_flag = (UCHAR)pps->cabac;
  pp.pic_order_present_flag = (UCHAR)pps->pic_order_present;
  pp.num_slice_groups_minus1 = (UCHAR)pps->num_slice_groups_minus1;
  pp.slice_group_map_type = (UCHAR)pps->slice_group_map_type;
  pp.deblocking_filter_control_present_flag = (UCHAR)pps->deblocking_control_present;
  pp.redundant_pic_cnt_present_flag = (UCHAR)pps->redundant_pic_cnt_present;
  s->pp_size = 0;
  size_t cap = 0;
  free(s->pp);
  s->pp = NULL;
  if (!dxva_append(&s->pp, &s->pp_size, &cap, &pp, sizeof pp)) return 0;
  DXVA_Qmatrix_H264 qm;
  memcpy(qm.bScalingLists4x4, pps->sl4, sizeof qm.bScalingLists4x4);
  memcpy(qm.bScalingLists8x8[0], pps->sl8[0], 64);
  memcpy(qm.bScalingLists8x8[1], pps->sl8[1], 64);
  s->qm_size = 0;
  cap = 0;
  free(s->qm);
  s->qm = NULL;
  if (!dxva_append(&s->qm, &s->qm_size, &cap, &qm, sizeof qm)) return 0;
  s->sc_size = s->bs_size = 0;
  s->slices = 0;
  s->width = sps->width_mbs * 16;
  s->height = sps->height_map_units * (2 - sps->frame_mbs_only) * 16;
  const unsigned cx = sps->chroma_format_idc == 3 ? 1 : 2, cy = (sps->chroma_format_idc == 1 ? 2 : 1) * (2 - sps->frame_mbs_only);
  s->crop_left = sps->crop_left * cx;
  s->crop_right = sps->crop_right * cx;
  s->crop_top = sps->crop_top * cy;
  s->crop_bottom = sps->crop_bottom * cy;
  h->type = 'I';
  h->pending = 1;
  return 1;
}

/* The picture order count of the picture (8.2.1), for frames. */
static void dxva_h264_poc(DxvaH264State *h, int delta_bottom, const int delta[2]) {
  const DxvaH264Sps *sps = h->sps_cur;
  const unsigned max_frame_num = 1u << sps->log2_max_frame_num;
  if (sps->poc_type == 0) {
    const int max_lsb = 1 << sps->log2_max_poc_lsb;
    int prev_msb = h->prev_poc_msb, prev_lsb = h->prev_poc_lsb;
    if (h->idr) prev_msb = prev_lsb = 0;
    int msb;
    if (h->poc_lsb < prev_lsb && prev_lsb - h->poc_lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
    else if (h->poc_lsb > prev_lsb && h->poc_lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
    else msb = prev_msb;
    h->poc_msb = msb;
    h->poc[0] = msb + h->poc_lsb;
    h->poc[1] = h->poc[0] + delta_bottom;
    return;
  }
  unsigned offset = h->idr ? 0 : h->prev_frame_num > h->frame_num ? h->prev_frame_num_offset + max_frame_num : h->prev_frame_num_offset;
  h->frame_num_offset = offset;
  if (sps->poc_type == 1) {
    unsigned abs_frame = sps->num_ref_frames_in_poc_cycle ? offset + h->frame_num : 0;
    if (!h->nal_ref_idc && abs_frame > 0) --abs_frame;
    int expected = 0;
    if (abs_frame > 0) {
      int delta_cycle = 0;
      for (unsigned i = 0; i < sps->num_ref_frames_in_poc_cycle; ++i) delta_cycle += sps->offset_for_ref_frame[i];
      const unsigned cycle = (abs_frame - 1) / sps->num_ref_frames_in_poc_cycle, in_cycle = (abs_frame - 1) % sps->num_ref_frames_in_poc_cycle;
      expected = (int)cycle * delta_cycle;
      for (unsigned i = 0; i <= in_cycle; ++i) expected += sps->offset_for_ref_frame[i];
    }
    if (!h->nal_ref_idc) expected += sps->offset_for_non_ref_pic;
    h->poc[0] = expected + delta[0];
    h->poc[1] = h->poc[0] + sps->offset_for_top_to_bottom_field + delta[1];
  } else {
    const int t = h->idr ? 0 : (int)(2 * (offset + h->frame_num)) - (h->nal_ref_idc ? 0 : 1);
    h->poc[0] = h->poc[1] = t;
  }
}

/* A reference's PicNum among the short-term ones (FrameNumWrap: frames only). */
static int dxva_h264_pic_num(const DxvaH264State *h, const DxvaH264Ref *r) {
  const unsigned max_frame_num = 1u << h->sps_cur->log2_max_frame_num;
  return r->frame_num > h->frame_num ? (int)r->frame_num - (int)max_frame_num : (int)r->frame_num;
}
static void dxva_h264_remove_short(DxvaH264State *h, unsigned i) {
  memmove(&h->shortr[i], &h->shortr[i + 1], sizeof h->shortr[0] * (h->short_count - i - 1));
  --h->short_count;
}
/* The reference marking of the picture just decoded (8.2.5), then the state the next picture's order count starts from. */
static void dxva_h264_end_picture(DxvaH264State *h) {
  const DxvaH264Sps *sps = h->sps_cur;
  int mmco5 = 0, made_long = 0;
  DxvaH264Ref cur;
  memset(&cur, 0, sizeof cur);
  cur.surface = h->surface;
  cur.frame_num = h->frame_num;
  cur.poc[0] = h->poc[0];
  cur.poc[1] = h->poc[1];
  if (h->nal_ref_idc) {
    if (h->idr) {
      h->short_count = 0;
      memset(h->long_used, 0, sizeof h->long_used);
      if (h->long_term_reference) { cur.long_term = 1; cur.long_idx = 0; h->longr[0] = cur; h->long_used[0] = 1; h->max_long_idx = 0; made_long = 1; }
      else h->max_long_idx = -1;
    } else if (h->adaptive) {
      for (unsigned m = 0; m < h->mmco_count; ++m) {
        const DxvaH264Mmco *op = &h->mmco[m];
        const int pic_num_x = (int)h->frame_num - (int)(op->a + 1);
        switch (op->op) {
          case 1:
            for (unsigned i = 0; i < h->short_count; ++i) if (dxva_h264_pic_num(h, &h->shortr[i]) == pic_num_x) { dxva_h264_remove_short(h, i); break; }
            break;
          case 2: if (op->a < 16) h->long_used[op->a] = 0; break;
          case 3:
            for (unsigned i = 0; i < h->short_count; ++i) {
              if (dxva_h264_pic_num(h, &h->shortr[i]) != pic_num_x || op->b >= 16) continue;
              DxvaH264Ref r = h->shortr[i];
              dxva_h264_remove_short(h, i);
              r.long_term = 1;
              r.long_idx = op->b;
              h->longr[op->b] = r;
              h->long_used[op->b] = 1;
              break;
            }
            break;
          case 4:
            h->max_long_idx = (int)op->a - 1;
            for (int i = 0; i < 16; ++i) if (i > h->max_long_idx) h->long_used[i] = 0;
            break;
          case 5:
            h->short_count = 0;
            memset(h->long_used, 0, sizeof h->long_used);
            h->max_long_idx = -1;
            mmco5 = 1;
            break;
          case 6:
            if (op->a < 16) { cur.long_term = 1; cur.long_idx = op->a; h->longr[op->a] = cur; h->long_used[op->a] = 1; made_long = 1; }
            break;
          default: break;
        }
      }
    } else {
      unsigned longs = 0;
      for (unsigned i = 0; i < 16; ++i) longs += h->long_used[i] != 0;
      const unsigned max_refs = sps->max_num_ref_frames ? sps->max_num_ref_frames : 1;
      if (h->short_count + longs >= max_refs && h->short_count) {
        unsigned oldest = 0;
        for (unsigned i = 1; i < h->short_count; ++i) if (dxva_h264_pic_num(h, &h->shortr[i]) < dxva_h264_pic_num(h, &h->shortr[oldest])) oldest = i;
        dxva_h264_remove_short(h, oldest);
      }
    }
    if (mmco5) {
      /* (the picture is frame_num 0 from now on, its order counts relative to itself) */
      const int t = cur.poc[0] < cur.poc[1] ? cur.poc[0] : cur.poc[1];
      cur.poc[0] -= t;
      cur.poc[1] -= t;
      cur.frame_num = 0;
      if (made_long) { h->longr[cur.long_idx].poc[0] = cur.poc[0]; h->longr[cur.long_idx].poc[1] = cur.poc[1]; h->longr[cur.long_idx].frame_num = 0; }
    }
    if (!made_long && h->short_count < 16) {
      memmove(&h->shortr[1], &h->shortr[0], sizeof h->shortr[0] * h->short_count);
      h->shortr[0] = cur;
      ++h->short_count;
    }
    h->prev_poc_msb = mmco5 ? 0 : h->poc_msb;
    h->prev_poc_lsb = mmco5 ? cur.poc[0] : h->poc_lsb;
    h->prev_ref_frame_num = mmco5 ? 0 : h->frame_num;
  }
  h->prev_frame_num = mmco5 ? 0 : h->frame_num;
  h->prev_frame_num_offset = mmco5 ? 0 : h->frame_num_offset;
  h->pending = 0;
  if (mmco5) ++h->period;
}

/* A slice NAL unit: its header (the first slice of a picture begins one), its bytes with a start code into the bitstream, its short slice control. 0: the stream is not
 * one this takes; 2: it begins a new picture while one is pending (nothing done: the caller hands the pending one out first). */
static int dxva_h264_slice(DxvaStream *s, const unsigned char *nal, size_t size, unsigned char *rbsp) {
  DxvaH264State *h = s->h264;
  const unsigned nal_ref_idc = (nal[0] >> 5) & 3, nal_type = nal[0] & 31;
  const size_t n = dxva_rbsp(nal, size, rbsp);
  DxvaBits b = {rbsp, n * 8, 0, 0};
  const unsigned first_mb = dxva_ue(&b), slice_type = dxva_ue(&b) % 5, pps_id = dxva_ue(&b);
  if (b.bad || pps_id >= 256 || !h->pps[pps_id].valid) { s->error = "a slice of no PPS"; return 0; }
  if (first_mb == 0 && h->pending) return 2;
  const DxvaH264Pps *pps = &h->pps[pps_id];
  const DxvaH264Sps *sps = &h->sps[pps->sps_id];
  if (first_mb == 0) {
    if (!sps->frame_mbs_only) { s->error = "field or MBAFF pictures"; return 0; }
    if (sps->separate_colour_plane) dxva_u(&b, 2);
    h->sps_cur = sps;
    h->pps_cur = pps;
    h->nal_ref_idc = nal_ref_idc;
    h->idr = nal_type == 5;
    h->frame_num = dxva_u(&b, sps->log2_max_frame_num);
    if (h->idr) dxva_ue(&b);
    int delta_bottom = 0, delta[2] = {0, 0};
    if (sps->poc_type == 0) {
      h->poc_lsb = (int)dxva_u(&b, sps->log2_max_poc_lsb);
      if (pps->pic_order_present) delta_bottom = dxva_se(&b);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
      delta[0] = dxva_se(&b);
      if (pps->pic_order_present) delta[1] = dxva_se(&b);
    }
    if (pps->redundant_pic_cnt_present) dxva_ue(&b);
    const int is_b = slice_type == 1, is_p = slice_type == 0 || slice_type == 3;
    if (is_b) dxva_u(&b, 1);
    unsigned refs[2] = {pps->ref_count[0], pps->ref_count[1]};
    if (is_p || is_b) {
      if (dxva_u(&b, 1)) {
        refs[0] = dxva_ue(&b) + 1;
        if (is_b) refs[1] = dxva_ue(&b) + 1;
      }
    }
    for (unsigned list = 0; list < (is_b ? 2u : is_p ? 1u : 0u); ++list) {
      if (!dxva_u(&b, 1)) continue;
      for (unsigned guard = 0; guard < 64; ++guard) {
        const unsigned idc = dxva_ue(&b);
        if (idc == 3 || b.bad) break;
        dxva_ue(&b);
      }
    }
    if ((pps->weighted_pred && is_p) || (pps->weighted_bipred_idc == 1 && is_b)) {
      const unsigned chroma = sps->separate_colour_plane ? 0 : sps->chroma_format_idc;
      dxva_ue(&b);
      if (chroma) dxva_ue(&b);
      for (unsigned list = 0; list < (is_b ? 2u : 1u); ++list)
        for (unsigned i = 0; i < refs[list] && i < 32; ++i) {
          if (dxva_u(&b, 1)) { dxva_se(&b); dxva_se(&b); }
          if (chroma && dxva_u(&b, 1)) for (int k = 0; k < 4; ++k) dxva_se(&b);
        }
    }
    h->long_term_reference = h->adaptive = h->mmco_count = 0;
    if (nal_ref_idc) {
      if (h->idr) {
        dxva_u(&b, 1);
        h->long_term_reference = dxva_u(&b, 1);
      } else if ((h->adaptive = dxva_u(&b, 1)) != 0) {
        for (;;) {
          const unsigned op = dxva_ue(&b);
          if (op == 0 || b.bad || h->mmco_count == 66) break;
          DxvaH264Mmco m = {op, 0, 0};
          if (op == 1 || op == 3) m.a = dxva_ue(&b);
          if (op == 2) m.a = dxva_ue(&b);
          if (op == 3 || op == 6) { if (op == 3) m.b = dxva_ue(&b); else m.a = dxva_ue(&b); }
          if (op == 4) m.a = dxva_ue(&b);
          h->mmco[h->mmco_count++] = m;
        }
      }
    }
    if (b.bad) { s->error = "a slice header that does not parse"; return 0; }
    /* (no gaps in frame_num: the decoder is not given frames that do not exist) */
    const unsigned max_frame_num = 1u << sps->log2_max_frame_num;
    if (!h->idr && h->frame_num != h->prev_ref_frame_num && h->frame_num != (h->prev_ref_frame_num + 1) % max_frame_num) { s->error = "a gap in frame_num"; return 0; }
    if (h->idr) ++h->period;
    dxva_h264_poc(h, delta_bottom, delta);
    if (!dxva_h264_begin_picture(s)) return 0;
  }
  /* (the slice's type: an inter slice clears IntraPicFlag) */
  if (slice_type != 2 && slice_type != 4) {
    USHORT bits;
    memcpy(&bits, s->pp + offsetof(DXVA_PicParams_H264, wBitFields), 2);
    bits &= (USHORT)~(1u << 15);
    memcpy(s->pp + offsetof(DXVA_PicParams_H264, wBitFields), &bits, 2);
  }
  const char t = slice_type == 1 ? 'B' : (slice_type == 0 || slice_type == 3) ? 'P' : 'I';
  if (t == 'B' || (t == 'P' && h->type == 'I')) h->type = t;
  static const unsigned char start[3] = {0, 0, 1};
  DXVA_Slice_H264_Short sc;
  memset(&sc, 0, sizeof sc);
  sc.BSNALunitDataLocation = (UINT)s->bs_size;
  sc.SliceBytesInBuffer = (UINT)(3 + size);
  if (!dxva_append(&s->bs, &s->bs_size, &s->bs_cap, start, 3) || !dxva_append(&s->bs, &s->bs_size, &s->bs_cap, nal, size) ||
      !dxva_append(&s->sc, &s->sc_size, &s->sc_cap, &sc, sizeof sc))
    return 0;
  ++s->slices;
  return 1;
}
/* Hands out the pending picture (its marking done). */
static int dxva_h264_finish(DxvaStream *s, DxvaPicture *p) {
  DxvaH264State *h = s->h264;
  if (!dxva_pad(s, (s->slices - 1) * sizeof(DXVA_Slice_H264_Short) + offsetof(DXVA_Slice_H264_Short, SliceBytesInBuffer))) return -1;
  memset(p, 0, sizeof *p);
  p->surface = h->surface;
  for (unsigned i = 0; i < h->short_count; ++i) p->refs[p->ref_count++] = h->shortr[i].surface;
  for (unsigned i = 0; i < 16 && p->ref_count < 16; ++i) if (h->long_used[i]) p->refs[p->ref_count++] = h->longr[i].surface;
  p->is_reference = h->nal_ref_idc != 0;
  p->type = h->type;
  const long long period = h->period;
  const int poc = h->poc[0] < h->poc[1] ? h->poc[0] : h->poc[1];
  dxva_h264_end_picture(h);
  /* (an MMCO 5 picture belongs to the period it begins, at its order count made relative to itself: 0) */
  p->display_key = h->period != period ? h->period * (1LL << 33) + (1LL << 32) : period * (1LL << 33) + (1LL << 32) + poc;
  p->picture_parameters = s->pp;
  p->picture_parameters_size = (unsigned)s->pp_size;
  p->qmatrix = s->qm;
  p->qmatrix_size = (unsigned)s->qm_size;
  p->slice_control = s->sc;
  p->slice_control_size = (unsigned)s->sc_size;
  p->slice_count = s->slices;
  p->bitstream = s->bs;
  p->bitstream_size = (unsigned)s->bs_size;
  return 1;
}
static int dxva_h264_next(DxvaStream *s, DxvaPicture *p) {
  DxvaH264State *h = s->h264;
  while (s->pos < s->size) {
    const size_t at = dxva_find_start(s->data, s->size, s->pos);
    if (at >= s->size) { s->pos = s->size; break; }
    const size_t begin = at + 3;
    size_t end = dxva_find_start(s->data, s->size, begin);
    const size_t next = end;
    while (end > begin && s->data[end - 1] == 0) --end;      /* (the zero bytes before the next start code are not the NAL unit's) */
    if (end <= begin) { s->pos = next; continue; }
    const unsigned char *nal = s->data + begin;
    const size_t size = end - begin;
    unsigned char *rbsp = (unsigned char *)malloc(size);
    if (!rbsp) return -1;
    const unsigned type = nal[0] & 31;
    int r = 1;
    if (type == 7 || type == 8) {
      if (h->pending) { free(rbsp); return dxva_h264_finish(s, p); }      /* (a parameter set ends the picture: hand it out before taking the set) */
      const size_t n = dxva_rbsp(nal, size, rbsp);
      DxvaBits b = {rbsp, n * 8, 0, 0};
      r = type == 7 ? dxva_h264_sps(s, &b) : dxva_h264_pps(s, &b);
    } else if (type == 1 || type == 5) {
      r = dxva_h264_slice(s, nal, size, rbsp);
      if (r == 2) { free(rbsp); return dxva_h264_finish(s, p); }
    } else if ((type == 9 || type == 10 || type == 11) && h->pending) {
      free(rbsp);
      s->pos = next;
      return dxva_h264_finish(s, p);      /* (an access unit delimiter, end of sequence or end of stream ends the picture) */
    }
    free(rbsp);
    if (!r) return -1;
    s->pos = next;
  }
  if (h->pending) return dxva_h264_finish(s, p);
  return 0;
}

/* ---- MPEG-1 / MPEG-2 --------------------------------------------------------------------------------------------------------------------------------------------- */

static const unsigned char g_dxva_m2_default_intra[64] = {8,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26,
                                                          27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38,
                                                          37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};
struct DxvaMpeg2State {
  int width, height, mpeg1, progressive_sequence;
  unsigned char intra[64], non_intra[64];      /* (in the order the stream has them: zigzag) */
  int older, newer, open_frame;                /* the surfaces of the two anchors and of a frame whose second field is to come (-1: none) */
  int first_field_open, first_structure;
  long long gop;
  /* the picture being assembled */
  int pending, type, structure, second_field;
  unsigned fcodes, pce, mb_width, frame_mb_height, temporal_reference;
  unsigned surface;
};

static unsigned dxva_bits_at(const unsigned char *d, size_t size, size_t *pos, unsigned n) {
  unsigned v = 0;
  for (unsigned i = 0; i < n; ++i, ++*pos) v = (v << 1) | (*pos / 8 < size ? (d[*pos / 8] >> (7 - *pos % 8)) & 1u : 0u);
  return v;
}

/* The picture parameters and matrices of the picture (ffmpeg's fill_picture_parameters and fill_quantization_matrices), once its structure is known (its first slice). */
static int dxva_mpeg2_begin_picture(DxvaStream *s) {
  DxvaMpeg2State *m = s->mpeg2;
  const int field = m->structure != 3;
  m->second_field = field && m->first_field_open && m->first_structure != m->structure;
  m->first_field_open = field && !m->second_field;
  m->first_structure = m->structure;
  if (m->second_field && m->open_frame >= 0) m->surface = (unsigned)m->open_frame;
  else {
    unsigned busy[3], n = 0;
    if (m->older >= 0) busy[n++] = (unsigned)m->older;
    if (m->newer >= 0) busy[n++] = (unsigned)m->newer;
    if (m->open_frame >= 0) busy[n++] = (unsigned)m->open_frame;
    m->surface = dxva_pick_surface(s, busy, n);
    if (m->surface == ~0u) { s->error = "no free surface (give more)"; return 0; }
  }
  const int fwd = m->type == 3 ? m->older : m->newer, bwd = m->newer;
  DXVA_PictureParameters pp;
  memset(&pp, 0, sizeof pp);
  pp.wDecodedPictureIndex = (WORD)m->surface;
  pp.wForwardRefPictureIndex = (WORD)(m->type != 1 && fwd >= 0 ? (unsigned)fwd : 0xFFFF);
  pp.wBackwardRefPictureIndex = (WORD)(m->type == 3 && bwd >= 0 ? (unsigned)bwd : 0xFFFF);
  pp.wPicWidthInMBminus1 = (WORD)(m->mb_width - 1);
  pp.wPicHeightInMBminus1 = (WORD)((m->frame_mb_height >> (field ? 1 : 0)) - 1);
  pp.bMacroblockWidthMinus1 = 15;
  pp.bMacroblockHeightMinus1 = 15;
  pp.bBlockWidthMinus1 = 7;
  pp.bBlockHeightMinus1 = 7;
  pp.bBPPminus1 = 7;
  pp.bPicStructure = (BYTE)m->structure;
  pp.bSecondField = (BYTE)m->second_field;
  pp.bPicIntra = m->type == 1;
  pp.bPicBackwardPrediction = m->type == 3;
  pp.bChromaFormat = 1;
  pp.bPicScanFixed = 1;
  pp.bPicScanMethod = (m->pce & 0x40) ? 1 : 0;      /* (alternate_scan) */
  pp.wBitstreamFcodes = (WORD)m->fcodes;
  pp.wBitstreamPCEelements = (WORD)(m->mpeg1 ? 0 : m->pce);
  size_t cap = 0;
  free(s->pp);
  s->pp = NULL;
  s->pp_size = 0;
  if (!dxva_append(&s->pp, &s->pp_size, &cap, &pp, sizeof pp)) return 0;
  DXVA_QmatrixData qm;
  memset(&qm, 0, sizeof qm);
  for (int k = 0; k < 4; ++k) {
    qm.bNewQmatrix[k] = 1;
    for (int i = 0; i < 64; ++i) qm.Qmatrix[k][i] = (k & 1) ? m->non_intra[i] : m->intra[i];
  }
  cap = 0;
  free(s->qm);
  s->qm = NULL;
  s->qm_size = 0;
  if (!dxva_append(&s->qm, &s->qm_size, &cap, &qm, sizeof qm)) return 0;
  s->sc_size = s->bs_size = 0;
  s->slices = 0;
  s->width = m->mb_width * 16;
  s->height = m->frame_mb_height * 16;
  s->crop_left = s->crop_top = 0;
  s->crop_right = s->width - (unsigned)m->width;
  s->crop_bottom = s->height - (unsigned)m->height;
  return 1;
}
static int dxva_mpeg2_slice(DxvaStream *s, size_t at, size_t end) {
  DxvaMpeg2State *m = s->mpeg2;
  DXVA_SliceInfo si;
  memset(&si, 0, sizeof si);
  const size_t size = end - at;
  si.wVerticalPosition = (WORD)(s->data[at + 3] - 1);
  si.dwSliceBitsInBuffer = (DWORD)(8 * size);
  si.dwSliceDataLocation = (DWORD)s->bs_size;
  size_t pos = (at + 4) * 8;
  si.wQuantizerScaleCode = (WORD)dxva_bits_at(s->data, s->size, &pos, 5);
  while (dxva_bits_at(s->data, s->size, &pos, 1)) dxva_bits_at(s->data, s->size, &pos, 8);
  si.wMBbitOffset = (WORD)(pos - at * 8);
  const unsigned rows = m->frame_mb_height >> (m->structure != 3 ? 1 : 0);
  if (s->slices) {
    /* (the previous slice's macroblocks: up to this one's row) */
    DXVA_SliceInfo prev;
    memcpy(&prev, s->sc + (s->slices - 1) * sizeof prev, sizeof prev);
    prev.wNumberMBsInSlice = (WORD)((si.wVerticalPosition - prev.wVerticalPosition) * m->mb_width);
    memcpy(s->sc + (s->slices - 1) * sizeof prev, &prev, sizeof prev);
  }
  si.wNumberMBsInSlice = (WORD)(rows * m->mb_width - si.wVerticalPosition * m->mb_width);
  if (!dxva_append(&s->bs, &s->bs_size, &s->bs_cap, s->data + at, size) || !dxva_append(&s->sc, &s->sc_size, &s->sc_cap, &si, sizeof si)) return 0;
  ++s->slices;
  return 1;
}
static int dxva_mpeg2_finish(DxvaStream *s, DxvaPicture *p) {
  DxvaMpeg2State *m = s->mpeg2;
  if (!dxva_pad(s, (size_t)~(size_t)0)) return -1;
  memset(p, 0, sizeof *p);
  p->surface = m->surface;
  const int fwd = m->type == 3 ? m->older : m->newer, bwd = m->newer;
  if (m->type != 1 && fwd >= 0) p->refs[p->ref_count++] = (unsigned)fwd;
  if (m->type == 3 && bwd >= 0 && bwd != fwd) p->refs[p->ref_count++] = (unsigned)bwd;
  const int field = m->structure != 3;
  if (field && m->second_field && m->open_frame >= 0 && m->type != 1 && m->type != 3) {
    /* (a second field of a P frame may refer to the first) */
    int have = 0;
    for (unsigned i = 0; i < p->ref_count; ++i) have |= p->refs[i] == (unsigned)m->open_frame;
    if (!have) p->refs[p->ref_count++] = (unsigned)m->open_frame;
  }
  p->is_reference = m->type != 3;
  p->type = m->type == 1 ? 'I' : m->type == 2 ? 'P' : 'B';
  p->field = field ? (m->second_field ? 2 : 1) : 0;
  p->display_key = m->gop * 1024 + m->temporal_reference;
  p->picture_parameters = s->pp;
  p->picture_parameters_size = (unsigned)s->pp_size;
  p->qmatrix = s->qm;
  p->qmatrix_size = (unsigned)s->qm_size;
  p->slice_control = s->sc;
  p->slice_control_size = (unsigned)s->sc_size;
  p->slice_count = s->slices;
  p->bitstream = s->bs;
  p->bitstream_size = (unsigned)s->bs_size;
  /* (ffmpeg's last_picture / next_picture: the anchors move when a frame is complete) */
  if (field && !m->second_field) m->open_frame = (int)m->surface;
  else m->open_frame = -1;
  if (m->type != 3 && (!field || m->second_field)) { m->older = m->newer; m->newer = (int)m->surface; }
  m->pending = 0;
  return 1;
}
static int dxva_mpeg2_next(DxvaStream *s, DxvaPicture *p) {
  DxvaMpeg2State *m = s->mpeg2;
  while (s->pos < s->size) {
    const size_t at = dxva_find_start(s->data, s->size, s->pos);
    if (at + 3 >= s->size) { s->pos = s->size; break; }
    const size_t end = dxva_find_start(s->data, s->size, at + 3);
    const unsigned code = s->data[at + 3];
    size_t pos = (at + 4) * 8;
    const unsigned char *d = s->data;
    if (code >= 0x01 && code <= 0xAF) {
      if (m->pending == 1) { if (!dxva_mpeg2_begin_picture(s)) return -1; m->pending = 2; }
      if (m->pending == 2 && !dxva_mpeg2_slice(s, at, end)) return -1;
      s->pos = end;
      continue;
    }
    /* (any other start code ends the picture's slices) */
    if (m->pending == 2) return dxva_mpeg2_finish(s, p);
    if (code == 0xB3) {
      m->width = (int)dxva_bits_at(d, s->size, &pos, 12);
      m->height = (int)dxva_bits_at(d, s->size, &pos, 12);
      dxva_bits_at(d, s->size, &pos, 4 + 4 + 18 + 1 + 10 + 1);
      if (dxva_bits_at(d, s->size, &pos, 1)) for (int i = 0; i < 64; ++i) m->intra[i] = (unsigned char)dxva_bits_at(d, s->size, &pos, 8);
      else memcpy(m->intra, g_dxva_m2_default_intra, 64);
      if (dxva_bits_at(d, s->size, &pos, 1)) for (int i = 0; i < 64; ++i) m->non_intra[i] = (unsigned char)dxva_bits_at(d, s->size, &pos, 8);
      else memset(m->non_intra, 16, 64);
      m->mpeg1 = 1;
      m->progressive_sequence = 1;
    } else if (code == 0xB8) {
      ++m->gop;
    } else if (code == 0xB5) {
      const unsigned id = dxva_bits_at(d, s->size, &pos, 4);
      if (id == 1) {
        m->mpeg1 = 0;
        dxva_bits_at(d, s->size, &pos, 8);
        m->progressive_sequence = dxva_bits_at(d, s->size, &pos, 1) != 0;
      } else if (id == 3) {
        if (dxva_bits_at(d, s->size, &pos, 1)) for (int i = 0; i < 64; ++i) m->intra[i] = (unsigned char)dxva_bits_at(d, s->size, &pos, 8);
        if (dxva_bits_at(d, s->size, &pos, 1)) for (int i = 0; i < 64; ++i) m->non_intra[i] = (unsigned char)dxva_bits_at(d, s->size, &pos, 8);
      } else if (id == 8 && m->pending == 1) {
        unsigned f = 0;
        for (int i = 0; i < 4; ++i) f = (f << 4) | dxva_bits_at(d, s->size, &pos, 4);
        m->fcodes = f;
        const unsigned bits = dxva_bits_at(d, s->size, &pos, 13);      /* (intra_dc_precision .. progressive_frame) */
        m->pce = bits << 3;
        m->structure = (int)((bits >> 9) & 3);
      }
    } else if (code == 0x00) {
      m->pending = 1;
      m->temporal_reference = dxva_bits_at(d, s->size, &pos, 10);
      m->type = (int)dxva_bits_at(d, s->size, &pos, 3);
      dxva_bits_at(d, s->size, &pos, 16);
      unsigned fwd = 15, bwd = 15;
      if (m->type == 2 || m->type == 3) { dxva_bits_at(d, s->size, &pos, 1); fwd = dxva_bits_at(d, s->size, &pos, 3); }
      if (m->type == 3) { dxva_bits_at(d, s->size, &pos, 1); bwd = dxva_bits_at(d, s->size, &pos, 3); }
      m->structure = 3;
      m->pce = 0;
      m->fcodes = m->mpeg1 ? (fwd << 12 | fwd << 8 | bwd << 4 | bwd) : 0xFFFF;
      m->mb_width = (unsigned)(m->width + 15) / 16;
      m->frame_mb_height = (unsigned)(m->progressive_sequence ? (m->height + 15) / 16 : 2 * ((m->height + 31) / 32));
      if (m->type < 1 || m->type > 3 || !m->width || !m->height) { s->error = "a picture that is not I, P or B, or before a sequence header"; return -1; }
    }
    s->pos = end;
  }
  if (m->pending == 2) return dxva_mpeg2_finish(s, p);
  return 0;
}

/* ---- the stream ---------------------------------------------------------------------------------------------------------------------------------------------- */

static int dxva_stream_open(DxvaStream *s, int codec, const unsigned char *data, size_t size, unsigned surfaces) {
  memset(s, 0, sizeof *s);
  if (!data || !surfaces || surfaces > DXVA_STREAM_MAX_SURFACES || (codec != DXVA_CODEC_H264 && codec != DXVA_CODEC_MPEG2)) return -1;
  s->codec = codec;
  s->data = data;
  s->size = size;
  s->surfaces = surfaces;
  if (codec == DXVA_CODEC_H264) {
    s->h264 = (DxvaH264State *)calloc(1, sizeof *s->h264);
    if (!s->h264) return -1;
    s->h264->max_long_idx = -1;
  } else {
    s->mpeg2 = (DxvaMpeg2State *)calloc(1, sizeof *s->mpeg2);
    if (!s->mpeg2) return -1;
    s->mpeg2->older = s->mpeg2->newer = s->mpeg2->open_frame = -1;
  }
  return 0;
}
static int dxva_stream_next(DxvaStream *s, DxvaPicture *p) {
  if (s->error) return -1;
  return s->codec == DXVA_CODEC_H264 ? dxva_h264_next(s, p) : dxva_mpeg2_next(s, p);
}
static void dxva_stream_close(DxvaStream *s) {
  free(s->pp);
  free(s->qm);
  free(s->sc);
  free(s->bs);
  free(s->h264);
  free(s->mpeg2);
  memset(s, 0, sizeof *s);
}

#endif
