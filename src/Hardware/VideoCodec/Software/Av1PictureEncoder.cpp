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

// The AV1 picture encoder (Av1PictureEncoder.hpp).
//
// The encoder runs on the AV1 decoder's own state (Av1Core.hpp): its FrameContext / TaskContext / TileState, the above and left BlockContexts, the reference
// motion vector rows (RefmvsFrame::find / splat_mv) and its prediction code (Core::recon_b_intra / recon_b_inter called for blocks marked skip, which predict
// without reading coefficients). So every context the syntax depends on and every prediction is the decoder's, computed the way decode_b computes them; the
// encoder adds the residual (its forward transform, quantization, and the decoder's dequantization and inverse transform). The frame header is made by
// writing it as the program will and parsing it with the decoder's ObuParser (so the derived fields are the parser's).
//
// Coding runs in two steps: the decisions (partition by rate-distortion against the parent block, intra mode by prediction error, motion search with the
// block motion estimator's vectors as candidates, quantization) write a log of symbols - for each the CDF (as an offset into a CdfContext), its size and the
// value - and the log is then entropy coded per tile from the default CDFs (primary_ref_frame is always PRIMARY_REF_NONE). Logging first lets the encoder
// try every CDEF strength index per 64x64 block: the frame is decoded once with every block on index j (the in-loop filters run as in a decoder), each block
// takes the index with the smallest error, and the log is coded once more with those. The reconstruction a program gets is the in-tree decoder's
// (Core::decode_frame_external) of the final tile data.
#include "Hardware/VideoCodec/Software/Av1PictureEncoder.hpp"
#include "Hardware/VideoCodec/Software/Av1Core.hpp"
#include "Hardware/VideoCodec/Software/Av1EncBitstream.hpp"
#include "Hardware/VideoCodec/Software/Av1Itx.hpp"
#include "Hardware/VideoCodec/Software/Av1Mc.hpp"
#include "Hardware/VideoCodec/Software/MotionEstimator.hpp"

namespace hardware {
namespace av1 {

namespace {

#include "Hardware/VideoCodec/Software/Av1Tables.inc"
#include "Hardware/VideoCodec/Software/Av1Scan.inc"

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int iclip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int iabs(int v) { return v < 0 ? -v : v; }
inline unsigned umin(unsigned a, unsigned b) { return a < b ? a : b; }
inline int ulog2(unsigned v) { return 31 ^ __builtin_clz(v); }

// dav1d's set_ctx (Av1Decode.cpp): 1 << log2n entries from `off`.
template <class T, uint64_t N>
inline void set_ctx_log2(T (&arr)[N], int off, int log2n, int value) {
  ArrayView<T>(arr).subview(static_cast<uint64_t>(off), uint64_t{1} << log2n).fill(static_cast<T>(value));
}
// Av1Recon.cpp's set_ctx: n entries.
inline void set_ctx_n(uint8_t (&ctx)[32], int start, int n, uint8_t value) {
  if (n <= 0) return;
  ArrayView<uint8_t>(ctx).subview(static_cast<uint64_t>(start), static_cast<uint64_t>(n)).fill(value);
}
inline ArrayView<uint8_t> ctx_view(uint8_t (&ctx)[32], int start) { return ArrayView<uint8_t>(ctx).subview(static_cast<uint64_t>(start)); }

// ---- the decoder's context helpers (copied from Av1Decode.cpp / Av1Recon.cpp, where they are file local; dav1d's env.h / recon_tmpl.c) -----------

int get_intra_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  if (have_left) {
    if (have_top) {
      const int ctx = l.intra[yb4] + a.intra[xb4];
      return ctx + (ctx == 2);
    }
    return l.intra[yb4] * 2;
  }
  return have_top ? a.intra[xb4] * 2 : 0;
}
int get_tx_ctx(const BlockContext& a, const BlockContext& l, const TxfmInfo& max_tx, int yb4, int xb4) {
  return (l.tx_intra[yb4] >= max_tx.lh) + (a.tx_intra[xb4] >= max_tx.lw);
}
int get_partition_ctx(const BlockContext& a, const BlockContext& l, int bl, int yb8, int xb8) {
  return ((a.partition[xb8] >> (4 - bl)) & 1) + (((l.partition[yb8] >> (4 - bl)) & 1) << 1);
}
unsigned gather_left_partition_prob(ArrayView<const uint16_t> in, int bl) {
  unsigned out = static_cast<unsigned>(in[PARTITION_H - 1] - in[PARTITION_H]);
  out += static_cast<unsigned>(in[PARTITION_SPLIT - 1] - in[PARTITION_T_LEFT_SPLIT]);
  if (bl != BL_128X128) out += static_cast<unsigned>(in[PARTITION_H4 - 1] - in[PARTITION_H4]);
  return out;
}
unsigned gather_top_partition_prob(ArrayView<const uint16_t> in, int bl) {
  unsigned out = static_cast<unsigned>(in[PARTITION_V - 1] - in[PARTITION_T_TOP_SPLIT]);
  out += in[PARTITION_T_LEFT_SPLIT - 1];
  if (bl != BL_128X128) out += static_cast<unsigned>(in[PARTITION_V4 - 1] - in[PARTITION_T_RIGHT_SPLIT]);
  return out;
}
int get_filter_ctx(const BlockContext& a, const BlockContext& l, int comp, int dir, int ref, int yb4, int xb4) {
  const int a_filter = (a.ref[0][xb4] == ref || a.ref[1][xb4] == ref) ? static_cast<int>(a.filter[dir][xb4]) : static_cast<int>(N_SWITCHABLE_FILTERS);
  const int l_filter = (l.ref[0][yb4] == ref || l.ref[1][yb4] == ref) ? static_cast<int>(l.filter[dir][yb4]) : static_cast<int>(N_SWITCHABLE_FILTERS);
  if (a_filter == l_filter) return comp * 4 + a_filter;
  if (a_filter == N_SWITCHABLE_FILTERS) return comp * 4 + l_filter;
  if (l_filter == N_SWITCHABLE_FILTERS) return comp * 4 + a_filter;
  return comp * 4 + N_SWITCHABLE_FILTERS;
}
int get_ref_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[2] = {0, 0};
  if (have_top && !a.intra[xb4]) {
    cnt[a.ref[0][xb4] >= 4]++;
    if (a.comp_type[xb4]) cnt[a.ref[1][xb4] >= 4]++;
  }
  if (have_left && !l.intra[yb4]) {
    cnt[l.ref[0][yb4] >= 4]++;
    if (l.comp_type[yb4]) cnt[l.ref[1][yb4] >= 4]++;
  }
  return cnt[0] == cnt[1] ? 1 : cnt[0] < cnt[1] ? 0 : 2;
}
int get_fwd_ref_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[4] = {0, 0, 0, 0};
  if (have_top && !a.intra[xb4]) {
    if (a.ref[0][xb4] < 4) cnt[a.ref[0][xb4]]++;
    if (a.comp_type[xb4] && a.ref[1][xb4] < 4) cnt[a.ref[1][xb4]]++;
  }
  if (have_left && !l.intra[yb4]) {
    if (l.ref[0][yb4] < 4) cnt[l.ref[0][yb4]]++;
    if (l.comp_type[yb4] && l.ref[1][yb4] < 4) cnt[l.ref[1][yb4]]++;
  }
  cnt[0] += cnt[1];
  cnt[2] += cnt[3];
  return cnt[0] == cnt[2] ? 1 : cnt[0] < cnt[2] ? 0 : 2;
}
int get_fwd_ref_1_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[2] = {0, 0};
  if (have_top && !a.intra[xb4]) {
    if (a.ref[0][xb4] < 2) cnt[a.ref[0][xb4]]++;
    if (a.comp_type[xb4] && a.ref[1][xb4] < 2) cnt[a.ref[1][xb4]]++;
  }
  if (have_left && !l.intra[yb4]) {
    if (l.ref[0][yb4] < 2) cnt[l.ref[0][yb4]]++;
    if (l.comp_type[yb4] && l.ref[1][yb4] < 2) cnt[l.ref[1][yb4]]++;
  }
  return cnt[0] == cnt[1] ? 1 : cnt[0] < cnt[1] ? 0 : 2;
}
int get_fwd_ref_2_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[2] = {0, 0};
  if (have_top && !a.intra[xb4]) {
    if ((static_cast<unsigned>(a.ref[0][xb4]) ^ 2u) < 2) cnt[a.ref[0][xb4] - 2]++;
    if (a.comp_type[xb4] && (static_cast<unsigned>(a.ref[1][xb4]) ^ 2u) < 2) cnt[a.ref[1][xb4] - 2]++;
  }
  if (have_left && !l.intra[yb4]) {
    if ((static_cast<unsigned>(l.ref[0][yb4]) ^ 2u) < 2) cnt[l.ref[0][yb4] - 2]++;
    if (l.comp_type[yb4] && (static_cast<unsigned>(l.ref[1][yb4]) ^ 2u) < 2) cnt[l.ref[1][yb4] - 2]++;
  }
  return cnt[0] == cnt[1] ? 1 : cnt[0] < cnt[1] ? 0 : 2;
}
int get_bwd_ref_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[3] = {0, 0, 0};
  if (have_top && !a.intra[xb4]) {
    if (a.ref[0][xb4] >= 4) cnt[a.ref[0][xb4] - 4]++;
    if (a.comp_type[xb4] && a.ref[1][xb4] >= 4) cnt[a.ref[1][xb4] - 4]++;
  }
  if (have_left && !l.intra[yb4]) {
    if (l.ref[0][yb4] >= 4) cnt[l.ref[0][yb4] - 4]++;
    if (l.comp_type[yb4] && l.ref[1][yb4] >= 4) cnt[l.ref[1][yb4] - 4]++;
  }
  cnt[1] += cnt[0];
  return cnt[2] == cnt[1] ? 1 : cnt[1] < cnt[2] ? 0 : 2;
}
int get_bwd_ref_1_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[3] = {0, 0, 0};
  if (have_top && !a.intra[xb4]) {
    if (a.ref[0][xb4] >= 4) cnt[a.ref[0][xb4] - 4]++;
    if (a.comp_type[xb4] && a.ref[1][xb4] >= 4) cnt[a.ref[1][xb4] - 4]++;
  }
  if (have_left && !l.intra[yb4]) {
    if (l.ref[0][yb4] >= 4) cnt[l.ref[0][yb4] - 4]++;
    if (l.comp_type[yb4] && l.ref[1][yb4] >= 4) cnt[l.ref[1][yb4] - 4]++;
  }
  return cnt[0] == cnt[1] ? 1 : cnt[0] < cnt[1] ? 0 : 2;
}
int get_drl_context(ArrayView<const RefmvsCandidate> ref_mv_stack, int ref_idx) {
  if (ref_mv_stack[static_cast<uint64_t>(ref_idx)].weight >= 640) return ref_mv_stack[static_cast<uint64_t>(ref_idx) + 1].weight < 640;
  return ref_mv_stack[static_cast<uint64_t>(ref_idx) + 1].weight < 640 ? 2 : 0;
}
void fix_mv_precision(const FrameHeader& hdr, Mv& mv) {
  if (hdr.force_integer_mv) {
    mv.x = static_cast<int16_t>(static_cast<unsigned>(mv.x - (mv.x >> 15) + 3) & ~7u);
    mv.y = static_cast<int16_t>(static_cast<unsigned>(mv.y - (mv.y >> 15) + 3) & ~7u);
  } else if (!hdr.hp) {
    mv.x = static_cast<int16_t>(static_cast<unsigned>(mv.x - (mv.x >> 15)) & ~1u);
    mv.y = static_cast<int16_t>(static_cast<unsigned>(mv.y - (mv.y >> 15)) & ~1u);
  }
}

ArrayView<const uint16_t> scan_for(int tx) {
  switch (tx) {
    case TX_4X4: return ArrayView<const uint16_t>(k_scan_4x4);
    case TX_8X8: return ArrayView<const uint16_t>(k_scan_8x8);
    case TX_16X16: return ArrayView<const uint16_t>(k_scan_16x16);
    case TX_32X32:
    case TX_64X64: return ArrayView<const uint16_t>(k_scan_32x32);
    default: return ArrayView<const uint16_t>();
  }
}
bool any_not_marker(ArrayView<const uint8_t> a, unsigned n) {
  for (uint8_t v : a.first(n))
    if (v != 0x40) return true;
  return false;
}
unsigned or_bytes(ArrayView<const uint8_t> a, unsigned n) {
  unsigned r = 0;
  for (uint8_t v : a.first(n)) r |= v;
  return r;
}
unsigned sum_sign_bits(ArrayView<const uint8_t> a, unsigned n) {
  unsigned r = 0;
  for (uint8_t v : a.first(n)) r += v >> 6;
  return r;
}
unsigned get_skip_ctx(const TxfmInfo& t_dim, BlockSize bs, ArrayView<const uint8_t> a, ArrayView<const uint8_t> l, bool chroma) {
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  if (chroma) {
    const int not_one_blk = b_dim[2] - (!!b_dim[2]) > t_dim.lw || b_dim[3] - (!!b_dim[3]) > t_dim.lh;   // (4:2:0)
    const unsigned ca = any_not_marker(a, 1u << t_dim.lw);
    const unsigned cl = any_not_marker(l, 1u << t_dim.lh);
    return 7 + not_one_blk * 3 + ca + cl;
  }
  if (b_dim[2] == t_dim.lw && b_dim[3] == t_dim.lh) return 0;
  const unsigned la = or_bytes(a, 1u << t_dim.lw);
  const unsigned ll = or_bytes(l, 1u << t_dim.lh);
  return k_skip_ctx[umin(la & 0x3F, 4)][umin(ll & 0x3F, 4)];
}
unsigned get_dc_sign_ctx(const TxfmInfo& t_dim, ArrayView<const uint8_t> a, ArrayView<const uint8_t> l) {
  const int s = static_cast<int>(sum_sign_bits(a, t_dim.w) + sum_sign_bits(l, t_dim.h)) - static_cast<int>(t_dim.w) - static_cast<int>(t_dim.h);
  return (s != 0) + (s > 0);
}
unsigned get_lo_ctx(ArrayView<const uint8_t> levels, unsigned idx, unsigned& hi_mag, const uint8_t (*ctx_offsets)[5], unsigned x, unsigned y, unsigned stride) {
  unsigned mag = levels[idx + 0 * stride + 1] + levels[idx + 1 * stride + 0];
  mag += levels[idx + 1 * stride + 1];
  hi_mag = mag;
  mag += levels[idx + 0 * stride + 2] + levels[idx + 2 * stride + 0];
  const unsigned offset = ctx_offsets[umin(y, 4)][umin(x, 4)];
  return offset + (mag > 512 ? 4 : (mag + 64) >> 7);
}

// The CDFs as words (CdfContext is a structure of uint16_t arrays and its padding: a log entry names a CDF by its word offset). The one place the context is
// viewed as words: it is an object of the encoder or the caller that lives across the call, so the view does not outlive it.
ArrayView<uint16_t> cdf_words(CdfContext& c) { return ArrayView<uint16_t>(reinterpret_cast<uint16_t*>(&c), sizeof(CdfContext) / sizeof(uint16_t)); }
uint32_t cdf_offset(const CdfContext& base, ArrayView<const uint16_t> cdf) {
  return static_cast<uint32_t>(cdf.data() - reinterpret_cast<const uint16_t*>(&base));
}
template <uint64_t N>
ArrayView<const uint16_t> V(const uint16_t (&a)[N]) {
  return ArrayView<const uint16_t>(a);
}

// 256 * log2(x) for x >= 1 (piecewise linear between powers of two).
uint32_t log2_256(uint32_t x) {
  if (x == 0) x = 1;
  const int k = ulog2(x);
  const uint32_t frac = k >= 8 ? (x >> (k - 8)) & 255 : (x << (8 - k)) & 255;
  return static_cast<uint32_t>(k) * 256 + frac;
}

// ---- the symbol log ---------------------------------------------------------------------------------------------------------------------------------
enum SymKind : uint8_t { kSymAdapt = 0, kSymBoolProb = 1, kSymEqui = 2, kSymCdef = 3, kSymGatherTop = 4, kSymGatherLeft = 5 };
struct Sym {
  uint32_t a = 0;     // kSymAdapt / kSymGather*: the CDF's word offset; kSymBoolProb: the probability; kSymCdef: the 64x64 block (raster index)
  uint16_t value = 0;
  uint8_t kind = 0;
  uint8_t n = 0;      // kSymAdapt: n_symbols; kSymCdef: bits; kSymGather*: the block level
};

// Where a block's symbols go: costs (always, in 1/256 bits, from the frame's default CDFs) and, when logging, the log.
struct Sink {
  const CdfContext* cdf = nullptr;
  List<Sym>* log = nullptr;    // null: costs only
  uint64_t bits = 0;
  bool ok = true;
  void sym(ArrayView<const uint16_t> c, unsigned n, unsigned v) {
    const unsigned fl = v > 0 ? c[v - 1] : 32768u;
    const unsigned fh = v < n ? c[v] : 0u;
    const unsigned p = fl > fh ? fl - fh : 1;
    bits += 15 * 256 - log2_256(p);
    if (log) {
      Sym s;
      s.a = cdf_offset(*cdf, c);
      s.value = static_cast<uint16_t>(v);
      s.kind = kSymAdapt;
      s.n = static_cast<uint8_t>(n);
      ok = log->push_back(s) && ok;
    }
  }
  void bool_prob(unsigned f, unsigned v) {
    const unsigned p = v ? f : 32768u - f;
    bits += 15 * 256 - log2_256(p ? p : 1);
    if (log) {
      Sym s;
      s.a = f;
      s.value = static_cast<uint16_t>(v);
      s.kind = kSymBoolProb;
      ok = log->push_back(s) && ok;
    }
  }
  // A frame edge's partition bit: its probability is gathered from the partition CDF as it is when the bit is coded (it adapts with the symbols before it).
  void gather(ArrayView<const uint16_t> partition_cdf, int bl, bool top, unsigned v) {
    bool_prob(top ? gather_top_partition_prob(partition_cdf, bl) : gather_left_partition_prob(partition_cdf, bl), v);
    if (log) {
      Sym& s = (*log)[log->count() - 1];
      s.a = cdf_offset(*cdf, partition_cdf);
      s.kind = top ? kSymGatherTop : kSymGatherLeft;
      s.n = static_cast<uint8_t>(bl);
    }
  }
  void equi(unsigned v) {
    bits += 256;
    if (log) {
      Sym s;
      s.value = static_cast<uint16_t>(v & 1);
      s.kind = kSymEqui;
      ok = log->push_back(s) && ok;
    }
  }
  void equi_bits(unsigned v, unsigned n) {
    for (unsigned i = n; i-- > 0;) equi((v >> i) & 1);
  }
  void cdef(unsigned bits_n, uint32_t block) {
    bits += 256 * bits_n;
    if (log) {
      Sym s;
      s.a = block;
      s.kind = kSymCdef;
      s.n = static_cast<uint8_t>(bits_n);
      ok = log->push_back(s) && ok;
    }
  }
  void golomb(unsigned x) {
    const unsigned v = x + 1;
    const int len = ulog2(v);
    for (int i = 0; i < len; ++i) equi(0);
    equi(1);
    for (int i = len - 1; i >= 0; --i) equi((v >> i) & 1);
  }
  // The coefficient range token 3 .. 15 (Msac::hi_tok's inverse).
  void hi_tok(ArrayView<const uint16_t> c, unsigned level) {
    unsigned rem = level - 3;
    for (int k = 0; k < 4; ++k) {
      const unsigned s = rem < 3 ? rem : 3;
      sym(c, 3, s);
      if (s < 3) break;
      rem -= 3;
    }
  }
};

// ---- header geometry ----------------------------------------------------------------------------------------------------------------------------------
int tile_log2(int sz, int tgt) {
  int k = 0;
  while ((sz << k) < tgt) ++k;
  return k;
}
struct TileGeometry {
  int sbw = 0, sbh = 0, max_tile_width_sb = 64, max_tile_area_sb = 0;
  int min_log2_cols = 0, max_log2_cols = 0, max_log2_rows = 0, min_log2_tiles = 0;
  TileGeometry(uint32_t width, uint32_t height) {
    sbw = static_cast<int>((width + 63) >> 6);
    sbh = static_cast<int>((height + 63) >> 6);
    max_tile_area_sb = 4096 * 2304 >> 12;
    min_log2_cols = tile_log2(max_tile_width_sb, sbw);
    max_log2_cols = tile_log2(1, imin(sbw, MAX_TILE_COLS));
    max_log2_rows = tile_log2(1, imin(sbh, MAX_TILE_ROWS));
    min_log2_tiles = imax(tile_log2(max_tile_area_sb, sbw * sbh), min_log2_cols);
  }
  // The largest tile height of an explicit layout whose widest tile is `widest` superblocks (the parser's max_tile_height_sb).
  int max_height_sb(int widest) const {
    int area = sbw * sbh;
    if (min_log2_tiles) area >>= min_log2_tiles + 1;
    return imax(area / imax(widest, 1), 1);
  }
};

constexpr int8_t kDefaultRefDeltas[8] = {1, 0, 0, 0, -1, 0, -1, -1};

int bits_for(uint32_t v) {
  int n = 1;
  while (n < 16 && (v >> n) != 0) ++n;
  return n;
}
// seq_level_idx by the picture size (Annex A MaxPicSize; 31: no constraint).
unsigned level_for(uint32_t w, uint32_t h) {
  const uint64_t s = uint64_t{w} * h;
  static const struct { uint64_t pic; uint32_t maxw, maxh; unsigned idx; } kLevels[] = {
      {147456, 2048, 1152, 0}, {278784, 2816, 1584, 1}, {665856, 4352, 2448, 4}, {1065024, 5504, 3096, 5},
      {2359296, 6144, 3456, 8}, {8912896, 8192, 4352, 12}, {35651584, 16384, 8704, 16}};
  for (const auto& l : kLevels)
    if (s <= l.pic && w <= l.maxw && h <= l.maxh) return l.idx;
  return 31;
}

void write_sequence_payload(const Av1EncSequence& seq, HeaderBits& b) {
  b.put(0, 3);   // seq_profile 0
  b.bit(0);      // still_picture
  b.bit(0);      // reduced_still_picture_header
  b.bit(0);      // timing_info_present_flag
  b.bit(0);      // initial_display_delay_present_flag
  b.put(0, 5);   // operating_points_cnt_minus_1
  b.put(0, 12);  // operating_point_idc[0]
  const unsigned level = level_for(seq.width, seq.height);
  b.put(level, 5);
  if (level > 7) b.bit(0);   // seq_tier
  const int wb = bits_for(seq.width - 1), hb = bits_for(seq.height - 1);
  b.put(static_cast<uint32_t>(wb - 1), 4);
  b.put(static_cast<uint32_t>(hb - 1), 4);
  b.put(seq.width - 1, wb);
  b.put(seq.height - 1, hb);
  b.bit(0);   // frame_id_numbers_present_flag
  b.bit(0);   // use_128x128_superblock
  b.bit(0);   // enable_filter_intra
  b.bit(seq.enable_intra_edge_filter);
  b.bit(0);   // enable_interintra_compound
  b.bit(0);   // enable_masked_compound
  b.bit(0);   // enable_warped_motion
  b.bit(0);   // enable_dual_filter
  b.bit(seq.enable_order_hint);
  if (seq.enable_order_hint) {
    b.bit(0);   // enable_jnt_comp
    b.bit(0);   // enable_ref_frame_mvs
  }
  b.bit(0);   // seq_choose_screen_content_tools
  b.bit(0);   // seq_force_screen_content_tools
  if (seq.enable_order_hint) b.put(static_cast<uint32_t>(seq.order_hint_bits - 1), 3);
  b.bit(0);   // enable_superres
  b.bit(seq.enable_cdef);
  b.bit(0);   // enable_restoration
  b.bit(seq.bit_depth == 10);   // high_bitdepth
  b.bit(0);   // mono_chrome
  b.bit(0);   // color_description_present_flag
  b.bit(0);   // color_range
  b.put(0, 2);   // chroma_sample_position
  b.bit(0);   // separate_uv_delta_q
  b.bit(0);   // film_grain_params_present
  b.trailing();
}

bool is_intra_type(Av1EncFrameType t) { return t == Av1EncFrameType::Key || t == Av1EncFrameType::IntraOnly; }

void write_delta_q(HeaderBits& b, int v) {
  b.bit(v != 0);
  if (v) b.su(v, 7);
}

// uncompressed_header() as the header comment of Av1PictureEncoder.hpp lists it.
void write_frame_header_payload(const Av1EncPicture& p, const Av1EncPostEncode& post, HeaderBits& b) {
  const Av1EncSequence& seq = p.seq;
  const bool intra = is_intra_type(p.type);
  const bool key = p.type == Av1EncFrameType::Key, sw = p.type == Av1EncFrameType::Switch;
  const bool er = key || sw || p.error_resilient;
  const uint32_t hint_mask = (1u << seq.order_hint_bits) - 1;
  b.bit(0);   // show_existing_frame
  b.put(static_cast<uint32_t>(p.type), 2);
  b.bit(1);   // show_frame
  if (!key && !sw) b.bit(er);
  b.bit(p.disable_cdf_update);
  if (!sw) b.bit(0);   // frame_size_override_flag
  if (seq.enable_order_hint) b.put(p.order_hint & hint_mask, seq.order_hint_bits);
  if (!er && !intra) b.put(post.primary_ref_frame, 3);
  const uint8_t refresh = key || sw ? 0xFF : p.refresh_frame_flags;
  if (intra) {
    if (!key) b.put(refresh, 8);
    if (refresh != 0xFF && er && seq.enable_order_hint)
      for (const Av1EncReference& s : p.slots) b.put(s.valid ? s.order_hint & hint_mask : 0, seq.order_hint_bits);
    b.bit(0);   // render_and_frame_size_different
  } else {
    if (!sw) b.put(refresh, 8);
    if (er && seq.enable_order_hint)
      for (const Av1EncReference& s : p.slots) b.put(s.valid ? s.order_hint & hint_mask : 0, seq.order_hint_bits);
    if (seq.enable_order_hint) b.bit(0);   // frame_refs_short_signaling
    for (uint8_t idx : post.ref_frame_idx) b.put(idx, 3);
    if (sw) {
      b.put(seq.width - 1, bits_for(seq.width - 1));
      b.put(seq.height - 1, bits_for(seq.height - 1));
    }
    b.bit(0);   // render_and_frame_size_different
    b.bit(p.allow_high_precision_mv);
    if (p.filter == Av1EncFilter::Switchable) {
      b.bit(1);
    } else {
      b.bit(0);
      b.put(static_cast<uint32_t>(p.filter), 2);
    }
    b.bit(0);   // is_motion_mode_switchable
  }
  if (!p.disable_cdf_update) b.bit(p.disable_frame_end_update_cdf);

  // tile_info()
  const TileGeometry g(seq.width, seq.height);
  const Av1EncTiles& t = post.tiles;
  b.bit(t.uniform);
  int log2_cols, log2_rows;
  if (t.uniform) {
    for (int i = g.min_log2_cols; i < static_cast<int>(t.cols_log2); ++i) b.bit(1);
    if (static_cast<int>(t.cols_log2) < g.max_log2_cols) b.bit(0);
    const int min_log2_rows = imax(g.min_log2_tiles - static_cast<int>(t.cols_log2), 0);
    for (int i = min_log2_rows; i < static_cast<int>(t.rows_log2); ++i) b.bit(1);
    if (static_cast<int>(t.rows_log2) < g.max_log2_rows) b.bit(0);
    log2_cols = static_cast<int>(t.cols_log2);
    log2_rows = static_cast<int>(t.rows_log2);
  } else {
    int start = 0, widest = 0;
    for (uint32_t i = 0; i < t.cols; ++i) {
      const int maxw = imin(g.sbw - start, g.max_tile_width_sb);
      b.ns(static_cast<unsigned>(maxw), t.col_width_sb[i] - 1);
      start += static_cast<int>(t.col_width_sb[i]);
      widest = imax(widest, static_cast<int>(t.col_width_sb[i]));
    }
    const int maxh_all = g.max_height_sb(widest);
    start = 0;
    for (uint32_t i = 0; i < t.rows; ++i) {
      const int maxh = imin(g.sbh - start, maxh_all);
      b.ns(static_cast<unsigned>(maxh), t.row_height_sb[i] - 1);
      start += static_cast<int>(t.row_height_sb[i]);
    }
    log2_cols = tile_log2(1, static_cast<int>(t.cols));
    log2_rows = tile_log2(1, static_cast<int>(t.rows));
  }
  if (log2_cols || log2_rows) {
    b.put(t.context_update_tile_id, log2_cols + log2_rows);
    b.put(3, 2);   // tile_size_bytes_minus_1: TileSizeBytes 4
  }

  // quantization_params()
  b.put(post.base_q_idx, 8);
  write_delta_q(b, p.delta_q_y_dc);
  write_delta_q(b, p.delta_q_u_dc);
  write_delta_q(b, p.delta_q_u_ac);
  b.bit(0);   // using_qmatrix
  b.bit(0);   // segmentation_enabled
  if (post.base_q_idx) {
    b.bit(post.delta_q_present);
    if (post.delta_q_present) {
      b.put(post.delta_q_res, 2);
      b.bit(0);   // delta_lf_present
    }
  }
  // loop_filter_params()
  const Av1EncLoopFilter& lf = p.loop_filter;
  b.put(lf.level[0], 6);
  b.put(lf.level[1], 6);
  if (lf.level[0] || lf.level[1]) {
    b.put(lf.level[2], 6);
    b.put(lf.level[3], 6);
  }
  b.put(lf.sharpness, 3);
  b.bit(lf.delta_enabled);
  if (lf.delta_enabled) {
    b.bit(lf.delta_update);
    if (lf.delta_update) {
      for (int i = 0; i < 8; ++i) {
        const bool upd = lf.ref_deltas[i] != kDefaultRefDeltas[i];
        b.bit(upd);
        if (upd) b.su(lf.ref_deltas[i], 7);
      }
      for (int i = 0; i < 2; ++i) {
        const bool upd = lf.mode_deltas[i] != 0;
        b.bit(upd);
        if (upd) b.su(lf.mode_deltas[i], 7);
      }
    }
  }
  // cdef_params()
  if (seq.enable_cdef) {
    b.put(p.cdef.damping_minus3, 2);
    b.put(p.cdef.bits, 2);
    for (int i = 0; i < (1 << p.cdef.bits); ++i) {
      b.put((static_cast<uint32_t>(p.cdef.y_pri[i]) << 2) | p.cdef.y_sec[i], 6);
      b.put((static_cast<uint32_t>(p.cdef.uv_pri[i]) << 2) | p.cdef.uv_sec[i], 6);
    }
  }
  b.bit(p.tx_mode == Av1EncTxMode::Select);   // tx_mode_select
  if (!intra) b.bit(0);                        // reference_select
  b.bit(p.reduced_tx_set);
  if (!intra)
    for (int i = 0; i < 7; ++i) b.bit(0);   // is_global
}

// ---- the forward transform ------------------------------------------------------------------------------------------------------------------------------
// The encoder's forward transforms are the decoder's inverse ones transposed: the basis vectors are measured from Av1Itx's inverse transform (an impulse per
// frequency into a mid-grey 12-bit block), normalised to unit length (1 << 14), and the gain of the 2D transform taken from the DC impulse. They are separable
// and orthogonal like the inverse transforms, so a projection on them gives the coefficients the inverse turns back into the residual.
struct TxBasis {
  bool ready = false;
  int n = 0, nf = 0;
  int16_t h[32][64] = {};   // horizontal frequency u, sample x
  int16_t v[32][64] = {};   // vertical frequency v, sample y
  int64_t gain = 0;         // the 2D gain * 65536
};
// A transform type's horizontal and vertical kinds (true: ADST).
bool hor_adst(int txtp) { return txtp == DCT_ADST || txtp == ADST_ADST; }
bool ver_adst(int txtp) { return txtp == ADST_DCT || txtp == ADST_ADST; }

uint64_t isqrt64(uint64_t v) {
  uint64_t r = 0, bit = uint64_t{1} << 62;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

}  // namespace

// ---- the frame encoder ------------------------------------------------------------------------------------------------------------------------------------

class FrameEncoder {
 public:
  explicit FrameEncoder(allocators::IAllocator& allocator);
  Av1EncResult encode(const Av1EncPicture& pic, const EncPlanes16& source, const EncWritablePlanes16& recon, ByteBuffer* out, List<Av1EncTile>* tiles,
                      Av1EncPostEncode* post, EncStatistics* stats);
  uint64_t drift() const { return drift_; }

 private:
  // A block's coefficients of one plane, in the inverse transform's layout (coef[x * sh + y]).
  struct PlaneCoefs {
    int32_t level[32 * 32];   // signed levels
    int eob = -1;             // the last scan position with a level, -1: none
    int tx = 0, txtp = 0;
  };
  struct Decision {
    bool intra = true;
    uint8_t y_mode = DC_PRED, uv_mode = DC_PRED;
    int ref = 0;                 // 0 .. 6
    int inter_mode = NEARESTMV, drl = 0;
    Mv mv{}, ref_mv{};
    int filter = 0;              // the switchable filter chosen (FilterMode)
    bool has_subpel_filter = true;
    bool skip = false;
    int qidx = 0;
    PlaneCoefs coef[3];
    uint64_t sse = 0, bits = 0, cost = 0;
    uint8_t res_ctx[3] = {0x40, 0x40, 0x40};
  };
  struct Snapshot {
    explicit Snapshot(allocators::IAllocator& a) : pixels(a), mvs(a) {}
    BlockContext a{}, l{};
    OwnedArray<Pixel> pixels;
    OwnedArray<RefmvsBlock> mvs;
    uint64_t log_size = 0;
    int last_qidx = 0;
    const DequantTables* dq = nullptr;
    bool sb_delta_pending = false, sb_cdef_coded = false;
    EncStatistics stats;
  };

  Av1EncResult setup(const Av1EncPicture& pic, const EncPlanes16& source, Av1EncPostEncode* post);
  bool make_headers(const Av1EncPicture& pic, const Av1EncPostEncode& post);
  bool init_frame_context(const Av1EncPicture& pic);
  bool encode_tile(int tile_row, int tile_col);
  bool encode_sb(BlockLevel bl, const EdgeNode& node, uint64_t* cost);
  bool encode_b(BlockLevel bl, BlockSize bs, int edge_flags, uint64_t* cost);
  void take_snapshot(Snapshot& s, BlockSize bs);
  void restore_snapshot(const Snapshot& s, BlockSize bs);

  // decisions of one block
  bool eval_intra(BlockSize bs, int edge_flags, Decision& d);
  bool eval_inter(BlockSize bs, int edge_flags, Decision& d);
  bool predict(BlockSize bs, int edge_flags, const Decision& d);
  bool residual(BlockSize bs, Decision& d);
  void emit_block(Sink& sink, BlockSize bs, Decision& d, bool code_delta, bool code_cdef);
  unsigned emit_coefs(Sink& sink, ArrayView<const uint8_t> a, ArrayView<const uint8_t> l, BlockSize bs, bool intra, int plane, const Decision& d);
  void update_contexts(BlockSize bs, const Decision& d);
  int block_qidx() const;
  const DequantTables& dq_for(int qidx);
  uint64_t sse_block(BlockSize bs, bool luma, bool chroma) const;
  uint64_t lambda() const { return lambda_; }

  // motion search
  uint32_t sad_int(int bx_px, int by_px, int w, int h, int ref, int mvx8, int mvy8) const;
  uint32_t sad_sub(int bx_px, int by_px, int w, int h, int ref, Mv mv, int filter2d);
  uint32_t mv_bits(Mv mv, Mv ref) const;

  // transforms
  bool basis(int tx, int txtp, const TxBasis** out);
  bool quantize(int tx, int txtp, int plane, bool intra, PixelView pred, int px, int py, int qidx, PlaneCoefs& c);
  bool reconstruct(int tx, int plane, PixelView dst, int qidx, PlaneCoefs& c);

  // coding
  bool code_tiles(ArrayView<const int8_t> cdef_choice, ByteBuffer* data, List<Av1EncTile>* tiles);
  bool decode(ConstByteView data, ArrayView<const Av1EncTile> tiles, RefSlot* state);
  bool choose_cdef(ArrayView<int8_t> choice);

  allocators::IAllocator& allocator_;
  Core core_;
  FrameContext f_;
  UniquePointer<TaskContext> t_;
  UniquePointer<TileState> ts_;
  SequenceHeader seq_{};
  FrameHeader hdr_{};
  SharedPointer<Picture> slot_pic_[8];
  RefSlot refs_[8];
  SharedPointer<Picture> src_;          // the source, edges repeated to the frame's 8x8 size
  OwnedArray<uint8_t> luma8_;           // the source's luma at 8 bits, for the block motion estimator
  OwnedArray<MotionBlockResult> me_[8]; // the estimator's 16x16 vectors per slot
  int me_cols_ = 0, me_rows_ = 0;
  OwnedArray<TxBasis> bases_;
  CdfContext cost_cdf_{};
  CdfContext code_cdf_{};
  List<Sym> log_;
  List<uint64_t> tile_log_end_;
  OwnedArray<int16_t> sb_qidx_;
  int sb_cols_ = 0, sb_rows_ = 0;
  bool sb_delta_pending_ = false, sb_cdef_coded_ = false;
  int sb_target_qidx_ = 0;
  uint32_t cur_sb_ = 0;
  uint64_t lambda_ = 0;
  DequantTables dq_cache_{};
  int dq_cache_qidx_ = -1;
  Decision cand_[3];
  UniquePointer<Snapshot> snaps_[5];
  EncStatistics stats_;
  const Av1EncPicture* pic_ = nullptr;
  bool inter_frame_ = false;
  bool cdef_active_ = false;
  uint64_t drift_ = 0;
  int cur_edge_flags_ = 0;
  // scratch (kept out of the stack)
  int32_t res_[64 * 64] = {};
  int64_t tmp_[32 * 64] = {};
  Pixel probe_[64 * 64] = {};
  int32_t probe_coef_[32 * 32] = {};
  OwnedArray<int8_t> cdef_choice_;
  ByteBuffer tile_bytes_;
  List<Av1EncTile> tile_list_;
};

FrameEncoder::FrameEncoder(allocators::IAllocator& allocator)
    : allocator_(allocator),
      core_(allocator),
      f_(allocator),
      luma8_(allocator),
      me_{OwnedArray<MotionBlockResult>(allocator), OwnedArray<MotionBlockResult>(allocator), OwnedArray<MotionBlockResult>(allocator),
          OwnedArray<MotionBlockResult>(allocator), OwnedArray<MotionBlockResult>(allocator), OwnedArray<MotionBlockResult>(allocator),
          OwnedArray<MotionBlockResult>(allocator), OwnedArray<MotionBlockResult>(allocator)},
      bases_(allocator),
      log_(allocator),
      tile_log_end_(allocator),
      sb_qidx_(allocator),
      cdef_choice_(allocator),
      tile_bytes_(allocator),
      tile_list_(allocator) {}

// ---- set-up -------------------------------------------------------------------------------------------------------------------------------------------------

bool FrameEncoder::make_headers(const Av1EncPicture& pic, const Av1EncPostEncode& post) {
  ByteBuffer obus(allocator_);
  HeaderBits b(allocator_);
  write_sequence_payload(pic.seq, b);
  if (!b.ok() || !append_obu(&obus, OBU_SEQ_HDR, b.bytes())) return false;
  b.clear();
  write_frame_header_payload(pic, post, b);
  b.trailing();
  if (!b.ok() || !append_obu(&obus, OBU_FRAME_HDR, b.bytes())) return false;
  ObuParser parser(allocator_);
  ConstByteView rest = obus.view();
  bool have_frame = false;
  while (!rest.empty()) {
    uint64_t used = 0;
    ObuEvent ev;
    if (!parser.parse_obu(rest, used, ev)) return false;
    rest = rest.subview(used);
    if (ev.kind == ObuEventKind::SequenceHeader) {
      // the slots as the program's decoder holds them: what the frame header reads of them is their size, order hint and type
      for (int i = 0; i < 8; ++i) {
        const Av1EncReference& s = pic.slots[i];
        if (!s.valid) continue;
        FrameHeader h{};
        h.frame_type = static_cast<FrameType>(s.frame_type);
        h.width[0] = h.width[1] = static_cast<int>(pic.seq.width);
        h.height = static_cast<int>(pic.seq.height);
        h.render_width = h.width[1];
        h.render_height = h.height;
        h.frame_offset = static_cast<uint8_t>(s.order_hint & ((1u << pic.seq.order_hint_bits) - 1));
        h.show_frame = 1;
        h.loopfilter.mode_ref_deltas.ref_delta[0] = 1;
        for (int k = 0; k < 7; ++k) h.gmv[k] = default_wm_params();
        parser.ref_update(1u << i, h);
      }
    } else if (ev.kind == ObuEventKind::FrameHeader) {
      have_frame = true;
    }
  }
  if (!have_frame || !parser.sequence_header() || !parser.frame_header()) return false;
  seq_ = *parser.sequence_header();
  hdr_ = *parser.frame_header();
  return true;
}

bool FrameEncoder::init_frame_context(const Av1EncPicture& pic) {
  FrameContext& f = f_;
  f.seq_hdr_store = seq_;
  f.frame_hdr_store = hdr_;
  f.seq_hdr = &f.seq_hdr_store;
  f.frame_hdr = &f.frame_hdr_store;
  const SequenceHeader& seq = f.seq_hdr_store;
  const FrameHeader& hdr = f.frame_hdr_store;
  const int bpc = 8 + 2 * seq.hbd;
  f.layout = seq.layout;
  f.ss_hor = 1;
  f.ss_ver = 1;
  f.bitdepth_max = (1 << bpc) - 1;
  if (!t_) t_ = UniquePointer<TaskContext>::make(allocator_);
  if (!ts_) ts_ = UniquePointer<TileState>::make(allocator_);
  if (!t_ || !ts_) return false;
  for (int i = 0; i < 7; ++i) {
    f.svc[i][0] = f.svc[i][1] = ScalableMotionParams{0, 0};
    f.gmv_warp_allowed[i] = 0;
    f.refpoc[i] = 0;
    f.refp[i].reset();
  }
  if (inter_frame_)
    for (int i = 0; i < 7; ++i) f.refp[i] = slot_pic_[hdr.refidx[i]];
  f.cur = SharedPointer<Picture>::make(allocator_, allocator_);
  if (!f.cur || !f.cur->allocate(seq.layout, bpc, hdr.width[0], hdr.height)) return false;
  f.sr_cur = f.cur;
  f.w4 = (hdr.width[0] + 3) >> 2;
  f.h4 = (hdr.height + 3) >> 2;
  f.bw = ((hdr.width[0] + 7) >> 3) << 1;
  f.bh = ((hdr.height + 7) >> 3) << 1;
  f.sb128w = (f.bw + 31) >> 5;
  f.sb128h = (f.bh + 31) >> 5;
  f.sb_shift = 4;
  f.sb_step = 16;
  f.sbh = (f.bh + f.sb_step - 1) >> f.sb_shift;
  f.b4_stride = (f.bw + 31) & ~31;
  f.sr_sb128w = (hdr.width[1] + 127) >> 7;
  f.mvs.reset();
  if (inter_frame_) {
    f.mvs = SharedPointer<MotionVectorStore>::make(allocator_, allocator_);
    if (!f.mvs || !f.mvs->allocate(hdr.width[0], hdr.height)) return false;
    for (int i = 0; i < 7; ++i) f.refpoc[i] = f.refp[i]->hdr.frame_offset;
    const MotionVectorStore* ref_stores[7] = {};
    if (!f.rf.init_frame(seq, hdr, ArrayView<const uint8_t>(f.refpoc), f.mvs.get(), ArrayView<const MotionVectorStore* const>(ref_stores))) return false;
  }
  f.cur_segmap.reset();
  f.prev_segmap.reset();
  if (!f.a.reset(static_cast<uint64_t>(f.sb128w) * hdr.tiling.rows)) return false;
  for (BlockContext& c : f.a.view()) reset_block_context(c, is_key_or_intra(hdr));
  const uint64_t edge_size = static_cast<uint64_t>(f.sbh) * static_cast<uint64_t>(f.sb128w) * 128;
  for (int p = 0; p < 3; ++p)
    if (!f.ipred_edge[p].reset(edge_size)) return false;
  init_quant_tables(seq, hdr, hdr.quant.yac, f.dq);
  for (int i = 0; i < N_RECT_TX_SIZES; ++i)
    for (int pl = 0; pl < 3; ++pl) f.qm[i][pl] = ArrayView<const uint8_t>();
  f.cur->hdr = hdr;
  (void)pic;
  return true;
}

Av1EncResult FrameEncoder::setup(const Av1EncPicture& pic, const EncPlanes16& source, Av1EncPostEncode* post) {
  const Av1EncSequence& seq = pic.seq;
  // the frame description
  if (seq.width < 16 || seq.height < 16 || seq.width > 8192 || seq.height > 8192 || (seq.width & 1) || (seq.height & 1) ||
      (seq.bit_depth != 8 && seq.bit_depth != 10) || seq.order_hint_bits < 1 || seq.order_hint_bits > 8)
    return Av1EncResult::PictureControlNotSupported;
  if (static_cast<unsigned>(pic.type) > 3 || pic.base_q_idx == 0 || pic.tx_mode == Av1EncTxMode::Only4x4 || static_cast<unsigned>(pic.tx_mode) > 2 ||
      static_cast<unsigned>(pic.filter) > 4 || static_cast<unsigned>(pic.precision) > 2)
    return Av1EncResult::PictureControlNotSupported;
  const int8_t deltas[5] = {pic.delta_q_y_dc, pic.delta_q_u_dc, pic.delta_q_u_ac, pic.delta_q_v_dc, pic.delta_q_v_ac};
  for (int8_t d : deltas)
    if (d < -64 || d > 63) return Av1EncResult::PictureControlNotSupported;
  // (separate_uv_delta_q is 0 in the sequence header: V's deltas are U's)
  if (pic.delta_q_v_dc != pic.delta_q_u_dc || pic.delta_q_v_ac != pic.delta_q_u_ac) return Av1EncResult::PictureControlNotSupported;
  const Av1EncLoopFilter& lf = pic.loop_filter;
  for (uint8_t l : lf.level)
    if (l > 63) return Av1EncResult::PictureControlNotSupported;
  if (lf.sharpness > 7) return Av1EncResult::PictureControlNotSupported;
  for (int8_t d : lf.ref_deltas)
    if (d < -63 || d > 63) return Av1EncResult::PictureControlNotSupported;
  for (int8_t d : lf.mode_deltas)
    if (d < -63 || d > 63) return Av1EncResult::PictureControlNotSupported;
  if (seq.enable_cdef) {
    const Av1EncCdef& c = pic.cdef;
    if (c.damping_minus3 > 3 || c.bits > 3) return Av1EncResult::PictureControlNotSupported;
    for (int i = 0; i < (1 << c.bits); ++i)
      if (c.y_pri[i] > 15 || c.uv_pri[i] > 15 || c.y_sec[i] > 3 || c.uv_sec[i] > 3) return Av1EncResult::PictureControlNotSupported;
  }
  const bool key = pic.type == Av1EncFrameType::Key, sw = pic.type == Av1EncFrameType::Switch;
  if ((key || sw) && pic.refresh_frame_flags != 0xFF) return Av1EncResult::PictureControlNotSupported;
  if (pic.type == Av1EncFrameType::IntraOnly && pic.refresh_frame_flags == 0xFF) return Av1EncResult::PictureControlNotSupported;
  if (!source.fits() || source.width != seq.width || source.height != seq.height) return Av1EncResult::PictureControlNotSupported;
  inter_frame_ = !is_intra_type(pic.type);
  // the references
  for (SharedPointer<Picture>& p : slot_pic_) p.reset();
  for (RefSlot& r : refs_) r = RefSlot{};
  for (int i = 0; i < 8; ++i) {
    const Av1EncReference& s = pic.slots[i];
    if (!s.valid) continue;
    if (!s.picture.fits() || s.picture.width != seq.width || s.picture.height != seq.height) {
      if (inter_frame_) {
        for (uint8_t idx : pic.ref_frame_idx)
          if (idx == i) return Av1EncResult::InvalidReferences;
      }
      continue;
    }
  }
  if (inter_frame_)
    for (uint8_t idx : pic.ref_frame_idx)
      if (idx > 7 || !pic.slots[idx].valid || !pic.slots[idx].picture.fits() || pic.slots[idx].picture.width != seq.width ||
          pic.slots[idx].picture.height != seq.height || static_cast<unsigned>(pic.slots[idx].frame_type) > 3)
        return Av1EncResult::InvalidReferences;
  // the tiles and the values the header will carry
  Av1EncPostEncode pe;
  pe.tiles = pic.tiles;
  if (!Av1PictureEncoder::tile_layout(seq.width, seq.height, &pe.tiles)) return Av1EncResult::SubregionLayoutNotSupported;
  const uint32_t sbs = ((seq.width + 63) >> 6) * ((seq.height + 63) >> 6);
  if (!pic.qp_map.empty() && pic.qp_map.size() != sbs) return Av1EncResult::PictureControlNotSupported;
  pe.base_q_idx = pic.base_q_idx;
  pe.delta_q_present = !pic.qp_map.empty();
  pe.delta_q_res = 0;
  pe.primary_ref_frame = kAv1PrimaryRefNone;
  for (int i = 0; i < 7; ++i) pe.ref_frame_idx[i] = inter_frame_ ? pic.ref_frame_idx[i] : 0;
  *post = pe;
  if (!make_headers(pic, pe)) return Av1EncResult::PictureControlNotSupported;

  // the slot pictures
  const int bpc = seq.bit_depth;
  for (int i = 0; i < 8; ++i) {
    const Av1EncReference& s = pic.slots[i];
    if (!s.valid || !s.picture.fits() || s.picture.width != seq.width || s.picture.height != seq.height) continue;
    SharedPointer<Picture> p = SharedPointer<Picture>::make(allocator_, allocator_);
    if (!p || !p->allocate(PIXEL_LAYOUT_I420, bpc, static_cast<int>(seq.width), static_cast<int>(seq.height))) return Av1EncResult::NoMemory;
    const ArrayView<const uint16_t> src[3] = {s.picture.y, s.picture.u, s.picture.v};
    for (int pl = 0; pl < 3; ++pl) {
      const int w = p->plane_width(pl), h = p->plane_height(pl);
      const PixelView dst = p->plane(pl);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) dst.at(x, y) = static_cast<Pixel>(src[pl][static_cast<uint64_t>(y) * static_cast<uint64_t>(w) + static_cast<uint64_t>(x)] & ((1u << bpc) - 1));
    }
    FrameHeader h{};
    h.frame_type = static_cast<FrameType>(s.frame_type);
    h.width[0] = h.width[1] = static_cast<int>(seq.width);
    h.height = static_cast<int>(seq.height);
    h.render_width = h.width[0];
    h.render_height = h.height;
    h.frame_offset = static_cast<uint8_t>(s.order_hint & ((1u << seq.order_hint_bits) - 1));
    h.show_frame = 1;
    p->hdr = h;
    slot_pic_[i] = p;
    refs_[i].pic = p;
  }
  if (!init_frame_context(pic)) return Av1EncResult::NoMemory;

  // the source at the 8x8 size of the frame (edges repeated)
  src_ = SharedPointer<Picture>::make(allocator_, allocator_);
  if (!src_ || !src_->allocate(PIXEL_LAYOUT_I420, bpc, f_.bw * 4, f_.bh * 4)) return Av1EncResult::NoMemory;
  {
    const ArrayView<const uint16_t> src[3] = {source.y, source.u, source.v};
    for (int pl = 0; pl < 3; ++pl) {
      const int sw_ = pl ? static_cast<int>(source.chroma_width()) : static_cast<int>(source.width);
      const int sh_ = pl ? static_cast<int>(source.chroma_height()) : static_cast<int>(source.height);
      const int w = pl ? f_.bw * 2 : f_.bw * 4, h = pl ? f_.bh * 2 : f_.bh * 4;
      const PixelView dst = src_->plane(pl);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          dst.at(x, y) = static_cast<Pixel>(src[pl][static_cast<uint64_t>(imin(y, sh_ - 1)) * static_cast<uint64_t>(sw_) + static_cast<uint64_t>(imin(x, sw_ - 1))] &
                                            ((1u << bpc) - 1));
    }
  }
  // per superblock quantizers
  sb_cols_ = static_cast<int>((seq.width + 63) >> 6);
  sb_rows_ = static_cast<int>((seq.height + 63) >> 6);
  if (!sb_qidx_.reset(static_cast<uint64_t>(sb_cols_) * static_cast<uint64_t>(sb_rows_))) return Av1EncResult::NoMemory;
  for (uint64_t i = 0; i < sb_qidx_.size(); ++i) {
    int q = pic.base_q_idx;
    if (!pic.qp_map.empty()) q = pic.qp_map_absolute ? pic.qp_map[i] : pic.base_q_idx + pic.qp_map[i];
    sb_qidx_[i] = static_cast<int16_t>(iclip(q, 1, 255));
  }
  cdf_load_default(cost_cdf_, cdf_qcat(hdr_.quant.yac));
  const int ac = k_dq_tbl[seq_.hbd][hdr_.quant.yac][1];
  lambda_ = static_cast<uint64_t>(ac) * static_cast<uint64_t>(ac) * 37 / 4096 / (seq_.hbd ? 16 : 1);
  if (lambda_ == 0) lambda_ = 1;
  cdef_active_ = false;
  if (seq_.cdef)
    for (int i = 0; i < (1 << hdr_.cdef.n_bits); ++i) cdef_active_ = cdef_active_ || hdr_.cdef.y_strength[i] || hdr_.cdef.uv_strength[i];

  // the block motion estimator's vectors, one set per slot the frame refers to
  if (inter_frame_) {
    const uint64_t luma = uint64_t{seq.width} * seq.height;
    const int shift = bpc - 8;
    me_cols_ = static_cast<int>((seq.width + 15) / 16);
    me_rows_ = static_cast<int>((seq.height + 15) / 16);
    if (!luma8_.reset(luma)) return Av1EncResult::NoMemory;
    for (uint64_t i = 0; i < luma; ++i) luma8_[i] = static_cast<uint8_t>(source.y[i] >> shift);
    bool done[8] = {};
    for (uint8_t idx : pic.ref_frame_idx) done[idx] = true;
    for (int s = 0; s < 8; ++s) {
      if (!done[s]) continue;
      ByteBuffer ref8(allocator_);
      if (!ref8.put_zeros(luma)) return Av1EncResult::NoMemory;
      ByteView rv = ref8.writable_view();
      for (uint64_t i = 0; i < luma; ++i) rv[i] = static_cast<uint8_t>(pic.slots[s].picture.y[i] >> shift);
      if (!me_[s].reset(static_cast<uint64_t>(me_cols_) * static_cast<uint64_t>(me_rows_))) return Av1EncResult::NoMemory;
      const LumaPlane cur{ConstByteView(luma8_.view()), static_cast<int>(seq.width), static_cast<int>(seq.height), static_cast<int>(seq.width)};
      const LumaPlane ref{ref8.view(), static_cast<int>(seq.width), static_cast<int>(seq.height), static_cast<int>(seq.width)};
      if (!estimate_motion(cur, ref, 16, ArrayView<const MotionVector>(), me_[s].view())) return Av1EncResult::NoMemory;
    }
  }
  if (!bases_.empty() || bases_.reset(5 * 4)) {
  } else {
    return Av1EncResult::NoMemory;
  }
  for (int i = 0; i < 5; ++i)
    if (!snaps_[i]) {
      snaps_[i] = UniquePointer<Snapshot>::make(allocator_, allocator_);
      if (!snaps_[i]) return Av1EncResult::NoMemory;
      const int n = i == 0 ? 8 : 128 >> i;   // (by BlockLevel: BL_64X64 is 1)
      if (!snaps_[i]->pixels.reset(static_cast<uint64_t>(n) * n * 3 / 2) || !snaps_[i]->mvs.reset(static_cast<uint64_t>(n / 4) * (n / 4)))
        return Av1EncResult::NoMemory;
    }
  return Av1EncResult::Ok;
}

// ---- blocks -------------------------------------------------------------------------------------------------------------------------------------------------

int FrameEncoder::block_qidx() const { return sb_delta_pending_ && hdr_.delta.q.present ? sb_target_qidx_ : ts_->last_qidx; }

const DequantTables& FrameEncoder::dq_for(int qidx) {
  if (qidx == hdr_.quant.yac) return f_.dq;
  if (dq_cache_qidx_ != qidx) {
    init_quant_tables(seq_, hdr_, qidx, dq_cache_);
    dq_cache_qidx_ = qidx;
  }
  return dq_cache_;
}

uint64_t FrameEncoder::sse_block(BlockSize bs, bool luma, bool chroma) const {
  const TaskContext& t = *t_;
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  uint64_t sse = 0;
  for (int pl = luma ? 0 : 1; pl < (chroma ? 3 : 1); ++pl) {
    const int ss = pl ? 1 : 0;
    const int w = (b_dim[0] * 4) >> ss, h = (b_dim[1] * 4) >> ss;
    const int x0 = (t.bx * 4) >> ss, y0 = (t.by * 4) >> ss;
    const PixelView a = f_.cur->plane(pl), s = src_->plane(pl);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int d = static_cast<int>(a.at(x0 + x, y0 + y)) - static_cast<int>(s.at(x0 + x, y0 + y));
        sse += static_cast<uint64_t>(d * d);
      }
  }
  return sse;
}

void FrameEncoder::take_snapshot(Snapshot& s, BlockSize bs) {
  const TaskContext& t = *t_;
  s.a = *t.a;
  s.l = t.l;
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  const int w = b_dim[0] * 4, h = b_dim[1] * 4;
  uint64_t k = 0;
  for (int pl = 0; pl < 3; ++pl) {
    const int ss = pl ? 1 : 0;
    const PixelView p = f_.cur->plane(pl);
    for (int y = 0; y < (h >> ss); ++y)
      for (int x = 0; x < (w >> ss); ++x) s.pixels[k++] = p.at(((t.bx * 4) >> ss) + x, ((t.by * 4) >> ss) + y);
  }
  if (inter_frame_) {
    k = 0;
    for (int y = 0; y < b_dim[1]; ++y) {
      const ArrayView<RefmvsBlock> row = f_.rf.row(t.rt, (t.by & 31) + 5 + y);
      for (int x = 0; x < b_dim[0]; ++x) s.mvs[k++] = row[static_cast<uint64_t>(t.bx + x)];
    }
  }
  s.log_size = log_.count();
  s.last_qidx = ts_->last_qidx;
  s.dq = ts_->dq;
  s.sb_delta_pending = sb_delta_pending_;
  s.sb_cdef_coded = sb_cdef_coded_;
  s.stats = stats_;
}

void FrameEncoder::restore_snapshot(const Snapshot& s, BlockSize bs) {
  TaskContext& t = *t_;
  *t.a = s.a;
  t.l = s.l;
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  const int w = b_dim[0] * 4, h = b_dim[1] * 4;
  uint64_t k = 0;
  for (int pl = 0; pl < 3; ++pl) {
    const int ss = pl ? 1 : 0;
    const PixelView p = f_.cur->plane(pl);
    for (int y = 0; y < (h >> ss); ++y)
      for (int x = 0; x < (w >> ss); ++x) p.at(((t.bx * 4) >> ss) + x, ((t.by * 4) >> ss) + y) = s.pixels[k++];
  }
  if (inter_frame_) {
    k = 0;
    for (int y = 0; y < b_dim[1]; ++y) {
      const ArrayView<RefmvsBlock> row = f_.rf.row(t.rt, (t.by & 31) + 5 + y);
      for (int x = 0; x < b_dim[0]; ++x) row[static_cast<uint64_t>(t.bx + x)] = s.mvs[k++];
    }
  }
  while (log_.count() > s.log_size) (void)log_.remove_at(log_.count() - 1);
  ts_->last_qidx = s.last_qidx;
  ts_->dq = s.dq;
  sb_delta_pending_ = s.sb_delta_pending;
  sb_cdef_coded_ = s.sb_cdef_coded;
  stats_ = s.stats;
}

bool FrameEncoder::encode_sb(BlockLevel bl, const EdgeNode& node, uint64_t* cost) {
  FrameContext& f = f_;
  TaskContext& t = *t_;
  const int hsz = 16 >> bl;
  const bool have_h_split = f.bw > t.bx + hsz;
  const bool have_v_split = f.bh > t.by + hsz;
  *cost = 0;
  if (!have_h_split && !have_v_split) {
    if (bl >= BL_8X8) return false;
    return encode_sb(static_cast<BlockLevel>(bl + 1), k_intra_edge_tree.split(node, 0), cost);
  }
  const int bx8 = (t.bx & 31) >> 1, by8 = (t.by & 31) >> 1;
  const int ctx = get_partition_ctx(*t.a, t.l, bl, by8, bx8);
  const ArrayView<const uint16_t> pc = V(cost_cdf_.m.partition[bl][ctx]);
  const unsigned n_parts = k_partition_type_count[bl];
  BlockPartition bp = PARTITION_SPLIT;
  const BlockLevel next = static_cast<BlockLevel>(bl + 1);
  auto split4 = [&](uint64_t* c) {
    uint64_t sum = 0, part = 0;
    if (!encode_sb(next, k_intra_edge_tree.split(node, 0), &part)) return false;
    sum += part;
    t.bx += hsz;
    if (t.bx < f.bw) {   // (have_h_split)
      if (!encode_sb(next, k_intra_edge_tree.split(node, 1), &part)) return false;
      sum += part;
    }
    t.bx -= hsz;
    t.by += hsz;
    if (t.by < f.bh) {
      if (!encode_sb(next, k_intra_edge_tree.split(node, 2), &part)) return false;
      sum += part;
      t.bx += hsz;
      if (t.bx < f.bw) {
        if (!encode_sb(next, k_intra_edge_tree.split(node, 3), &part)) return false;
        sum += part;
      }
      t.bx -= hsz;
    }
    t.by -= hsz;
    *c = sum;
    return true;
  };
  if (have_h_split && have_v_split) {
    const BlockSize bs_none = static_cast<BlockSize>(k_block_sizes[bl][PARTITION_NONE][0]);
    if (bl == BL_8X8) {
      // (no 4x4 blocks: an 8x8 block is not split)
      Sink s{&cost_cdf_, &log_};
      s.sym(pc, n_parts, PARTITION_NONE);
      if (!s.ok) return false;
      uint64_t c = 0;
      if (!encode_b(bl, bs_none, node.o, &c)) return false;
      *cost = c + ((s.bits * lambda()) >> 8);
      bp = PARTITION_NONE;
    } else {
      Snapshot& snap = *snaps_[bl];
      take_snapshot(snap, bs_none);
      Sink s{&cost_cdf_, &log_};
      s.sym(pc, n_parts, PARTITION_NONE);
      uint64_t c_none = 0;
      if (!s.ok || !encode_b(bl, bs_none, node.o, &c_none)) return false;
      c_none += (s.bits * lambda()) >> 8;
      restore_snapshot(snap, bs_none);
      Sink s2{&cost_cdf_, &log_};
      s2.sym(pc, n_parts, PARTITION_SPLIT);
      uint64_t c_split = 0;
      if (!s2.ok || !split4(&c_split)) return false;
      c_split += (s2.bits * lambda()) >> 8;
      if (c_none <= c_split) {
        restore_snapshot(snap, bs_none);
        Sink s3{&cost_cdf_, &log_};
        s3.sym(pc, n_parts, PARTITION_NONE);
        if (!s3.ok || !encode_b(bl, bs_none, node.o, &c_none)) return false;
        *cost = c_none;
        bp = PARTITION_NONE;
      } else {
        *cost = c_split;
        bp = PARTITION_SPLIT;
      }
    }
  } else if (have_h_split) {
    Sink s{&cost_cdf_, &log_};
    s.gather(pc, bl, true, 1);
    if (!s.ok || bl >= BL_8X8) return false;
    uint64_t c = 0;
    if (!split4(&c)) return false;
    *cost = c;
  } else {
    Sink s{&cost_cdf_, &log_};
    s.gather(pc, bl, false, 1);
    if (!s.ok || bl >= BL_8X8) return false;
    uint64_t c = 0;
    if (!split4(&c)) return false;
    *cost = c;
  }
  if (bp != PARTITION_SPLIT || bl == BL_8X8) {
    set_ctx_log2(t.a->partition, bx8, 4 - bl, k_al_part_ctx[0][bl][bp]);
    set_ctx_log2(t.l.partition, by8, 4 - bl, k_al_part_ctx[1][bl][bp]);
  }
  return true;
}

bool FrameEncoder::predict(BlockSize bs, int edge_flags, const Decision& d) {
  TaskContext& t = *t_;
  const BlockContext a = *t.a, l = t.l;   // (prediction of a "skipped" block resets the coefficient contexts: they are the block's to set)
  Av1Block b{};
  b.bs = static_cast<uint8_t>(bs);
  b.skip = 1;
  b.seg_id = 0;
  bool ok;
  if (d.intra) {
    b.intra = 1;
    b.y_mode = d.y_mode;
    b.uv_mode = d.uv_mode;
    b.y_angle = b.uv_angle = 0;
    b.tx = k_max_txfm_size_for_bs[bs][0];
    b.uvtx = k_max_txfm_size_for_bs[bs][PIXEL_LAYOUT_I420];
    ok = core_.recon_b_intra(t, bs, edge_flags, b);
  } else {
    b.intra = 0;
    b.ref[0] = static_cast<int8_t>(d.ref);
    b.ref[1] = -1;
    b.mv[0] = d.mv;
    b.comp_type = COMP_INTER_NONE;
    b.inter_mode = static_cast<uint8_t>(d.inter_mode);
    b.motion_mode = MM_TRANSLATION;
    b.interintra_type = INTER_INTRA_NONE;
    const int filt = d.has_subpel_filter ? (hdr_.subpel_filter_mode == FILTER_SWITCHABLE ? d.filter : static_cast<int>(hdr_.subpel_filter_mode))
                                         : (hdr_.subpel_filter_mode == FILTER_SWITCHABLE ? 0 : static_cast<int>(hdr_.subpel_filter_mode));
    b.filter2d = k_filter_2d[filt][filt];
    b.max_ytx = k_max_txfm_size_for_bs[bs][0];
    b.uvtx = k_max_txfm_size_for_bs[bs][PIXEL_LAYOUT_I420];
    ok = core_.recon_b_inter(t, bs, b);
  }
  *t.a = a;
  t.l = l;
  return ok;
}

bool FrameEncoder::basis(int tx, int txtp, const TxBasis** out) {
  TxBasis& b = bases_[static_cast<uint64_t>(tx) * 4 + static_cast<uint64_t>(txtp)];
  *out = &b;
  if (b.ready) return true;
  const int n = 4 << tx, sh = imin(n, 32);
  b.n = n;
  b.nf = sh;
  Pixel (&block)[64 * 64] = probe_;
  int32_t (&coef)[32 * 32] = probe_coef_;
  const PixelView view{ArrayView<Pixel>(block), 0, n};
  const int eob_all = static_cast<int>(itx_coef_count(tx)) - 1;
  // the residual of one coefficient `value` at (u, v) of transform type tt, as block - 2048
  auto impulse = [&](int tt, int u, int v, int value) {
    ArrayView<Pixel>(block).fill(2048);
    ArrayView<int32_t>(coef).fill(0);
    coef[u * sh + v] = value;
    return inv_txfm_add(view, ArrayView<int32_t>(coef), tx, tt, eob_all, 4095);
  };
  auto peak = [&]() {
    int m = 0;
    for (int i = 0; i < n * n; ++i) m = imax(m, iabs(static_cast<int>(block[i]) - 2048));
    return m;
  };
  // an amplitude that gives residuals of about 900
  auto amplitude = [&](int tt) {
    int a = 1024;
    if (!impulse(tt, 0, 0, a)) return 0;
    const int p = peak();
    a = static_cast<int>(static_cast<int64_t>(a) * 900 / imax(p, 1));
    return iclip(a, 1, (1 << 19) - 1);
  };
  for (int dir = 0; dir < 2; ++dir) {
    const bool adst = dir == 0 ? hor_adst(txtp) : ver_adst(txtp);
    const int probe = dir == 0 ? (adst ? DCT_ADST : DCT_DCT) : (adst ? ADST_DCT : DCT_DCT);
    const int a = amplitude(probe);
    if (!a) return false;
    for (int f = 0; f < sh; ++f) {
      if (!impulse(probe, dir == 0 ? f : 0, dir == 0 ? 0 : f, a)) return false;
      int64_t raw[64];
      for (int i = 0; i < n; ++i) {
        int64_t sum = 0;
        for (int j = 0; j < n; ++j) sum += dir == 0 ? static_cast<int>(block[j * n + i]) - 2048 : static_cast<int>(block[i * n + j]) - 2048;
        raw[i] = sum;
      }
      uint64_t norm2 = 0;
      for (int i = 0; i < n; ++i) norm2 += static_cast<uint64_t>(raw[i] * raw[i]);
      const int64_t norm = static_cast<int64_t>(isqrt64(norm2));
      for (int i = 0; i < n; ++i) {
        const int64_t q = norm ? (raw[i] * 16384 + (raw[i] >= 0 ? norm / 2 : -norm / 2)) / norm : 0;
        (dir == 0 ? b.h[f][i] : b.v[f][i]) = static_cast<int16_t>(q);
      }
    }
  }
  const int a = amplitude(txtp);
  if (!a || !impulse(txtp, 0, 0, a)) return false;
  uint64_t e = 0;
  for (int i = 0; i < n * n; ++i) {
    const int64_t r = static_cast<int>(block[i]) - 2048;
    e += static_cast<uint64_t>(r * r);
  }
  b.gain = static_cast<int64_t>((isqrt64(e << 32) + static_cast<uint64_t>(a) / 2) / static_cast<uint64_t>(a));
  if (b.gain <= 0) return false;
  b.ready = true;
  return true;
}

// The levels of the residual (source - prediction) of one transform block at (px, py) of plane `plane`.
bool FrameEncoder::quantize(int tx, int txtp, int plane, bool intra, PixelView pred, int px, int py, int qidx, PlaneCoefs& c) {
  const TxBasis* bp = nullptr;
  if (!basis(tx, txtp, &bp)) return false;
  const TxBasis& b = *bp;
  const int n = b.n, nf = b.nf;
  const PixelView src = src_->plane(plane);
  int32_t (&res)[64 * 64] = res_;
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) res[y * n + x] = static_cast<int>(src.at(px + x, py + y)) - static_cast<int>(pred.at(x, y));
  // T[u][y] = sum_x res[y][x] h[u][x]
  int64_t (&tmp)[32 * 64] = tmp_;
  for (int u = 0; u < nf; ++u)
    for (int y = 0; y < n; ++y) {
      int64_t s = 0;
      for (int x = 0; x < n; ++x) s += static_cast<int64_t>(res[y * n + x]) * b.h[u][x];
      tmp[u * 64 + y] = s;
    }
  const TxfmInfo& t_dim = k_txfm_dimensions[tx];
  const int dq_shift = imax(0, t_dim.ctx - 2);
  const DequantTables& dq = dq_for(qidx);
  const int64_t rnd = intra ? 104 : 80;   // the dead zone: levels are rounded from 0.41 (intra) or 0.31 (inter) up
  for (int u = 0; u < nf; ++u)
    for (int v = 0; v < nf; ++v) {
      int64_t s = 0;
      for (int y = 0; y < n; ++y) s += tmp[u * 64 + y] * b.v[v][y];
      const int64_t q = (u == 0 && v == 0) ? dq.q[0][plane][0] : dq.q[0][plane][1];
      const int64_t den = (b.gain * q) << 12;
      const int64_t num = (s < 0 ? -s : s) << dq_shift;
      int64_t level = (num + (den * rnd >> 8)) / den;
      if (level > 0xFFFFF) level = 0xFFFFF;
      c.level[u * nf + v] = static_cast<int32_t>(s < 0 ? -level : level);
    }
  const ArrayView<const uint16_t> scan = scan_for(tx);
  c.eob = -1;
  for (int i = nf * nf - 1; i >= 0; --i)
    if (c.level[scan[static_cast<uint64_t>(i)]]) {
      c.eob = i;
      break;
    }
  c.tx = tx;
  c.txtp = txtp;
  return true;
}

// Dequantizes as decode_coefs does and adds the inverse transform to dst.
bool FrameEncoder::reconstruct(int tx, int plane, PixelView dst, int qidx, PlaneCoefs& c) {
  if (c.eob < 0) return true;
  const TxfmInfo& t_dim = k_txfm_dimensions[tx];
  const int sh = imin(4 << tx, 32);
  const int dq_shift = imax(0, t_dim.ctx - 2);
  const DequantTables& dq = dq_for(qidx);
  const unsigned cf_max = ~(~127U << f_.cur->bpc());
  const ArrayView<int32_t> cfv(t_->cf);   // (TaskContext::cf: all zero between transform blocks, inv_txfm_add clears it)
  for (int i = 0; i < sh * sh; ++i) {
    const int32_t lv = c.level[i];
    if (!lv) continue;
    const unsigned tok = static_cast<unsigned>(lv < 0 ? -lv : lv);
    const unsigned q = i == 0 ? dq.q[0][plane][0] : dq.q[0][plane][1];
    const int sign = lv < 0;
    unsigned v;
    if (tok >= 15) {
      v = ((q * tok) & 0xffffff) >> dq_shift;
      v = umin(v, cf_max + static_cast<unsigned>(sign));
    } else {
      v = (q * tok) >> dq_shift;
    }
    cfv[static_cast<uint64_t>(i)] = sign ? -static_cast<int32_t>(v) : static_cast<int32_t>(v);
  }
  return inv_txfm_add(dst, cfv, tx, c.txtp, c.eob, f_.bitdepth_max);
}

// The residual of a predicted block (in f.cur): levels of every plane, the block reconstructed in f.cur, its error and the skip flag.
bool FrameEncoder::residual(BlockSize bs, Decision& d) {
  TaskContext& t = *t_;
  const int ytx = k_max_txfm_size_for_bs[bs][0], uvtx = k_max_txfm_size_for_bs[bs][PIXEL_LAYOUT_I420];
  const int qidx = d.qidx;
  const int x = t.bx * 4, y = t.by * 4;
  const PixelView ydst = f_.cur->plane(0).offset(x, y);
  // luma: DCT_DCT
  if (!quantize(ytx, DCT_DCT, 0, d.intra, ydst, x, y, qidx, d.coef[0])) return false;
  // chroma: the type the decoder infers
  const TxfmInfo& uvt = k_txfm_dimensions[uvtx];
  const int uvtp = d.intra ? (uvt.max + 1 >= TX_64X64 ? static_cast<int>(DCT_DCT) : static_cast<int>(k_txtp_from_uvmode[d.uv_mode])) : static_cast<int>(DCT_DCT);
  for (int pl = 1; pl < 3; ++pl)
    if (!quantize(uvtx, uvtp, pl, d.intra, f_.cur->plane(pl).offset(x / 2, y / 2), x / 2, y / 2, qidx, d.coef[pl])) return false;
  d.skip = d.coef[0].eob < 0 && d.coef[1].eob < 0 && d.coef[2].eob < 0;
  if (!d.skip) {
    if (!reconstruct(ytx, 0, ydst, qidx, d.coef[0])) return false;
    for (int pl = 1; pl < 3; ++pl)
      if (!reconstruct(uvtx, pl, f_.cur->plane(pl).offset(x / 2, y / 2), qidx, d.coef[pl])) return false;
  }
  d.sse = sse_block(bs, true, true);
  return true;
}

unsigned FrameEncoder::emit_coefs(Sink& sink, ArrayView<const uint8_t> a, ArrayView<const uint8_t> l, BlockSize bs, bool intra, int plane, const Decision& d) {
  const PlaneCoefs& c = d.coef[plane];
  const bool chroma = plane != 0;
  const int tx = c.tx;
  const TxfmInfo& t_dim = k_txfm_dimensions[tx];
  CdfCoefContext& cc = cost_cdf_.coef;
  const unsigned sctx = get_skip_ctx(t_dim, bs, a, l, chroma);
  sink.sym(V(cc.skip[t_dim.ctx][sctx]), 1, c.eob < 0);
  if (c.eob < 0) return 0x40;
  // transform type (luma: DCT_DCT, coded where the decoder reads one)
  if (!chroma && t_dim.max + intra < TX_64X64 && hdr_.segmentation.qidx[0]) {
    CdfModeContext& m = cost_cdf_.m;
    if (intra) {
      if (hdr_.reduced_txtp_set || t_dim.min == TX_16X16) sink.sym(V(m.txtp_intra2[t_dim.min][d.y_mode]), 4, 1);
      else sink.sym(V(m.txtp_intra1[t_dim.min][d.y_mode]), 6, 1);
    } else {
      if (hdr_.reduced_txtp_set || t_dim.max == TX_32X32) sink.sym(V(m.txtp_inter3[t_dim.min]), 1, 1);
      else if (t_dim.min == TX_16X16) sink.sym(V(m.txtp_inter2), 11, 3);
      else sink.sym(V(m.txtp_inter1[t_dim.min]), 15, 7);
    }
  }
  // end of block
  const int slw = imin(t_dim.lw, TX_32X32), slh = imin(t_dim.lh, TX_32X32);
  const int tx2dszctx = slw + slh;
  const int eob = c.eob;
  unsigned eob_sym;
  int eob_bin = -1;
  if (eob == 0) eob_sym = 0;
  else if (eob == 1) eob_sym = 1;
  else {
    eob_bin = ulog2(static_cast<unsigned>(eob)) - 1;
    eob_sym = static_cast<unsigned>(eob_bin + 2);
  }
  switch (tx2dszctx) {
    case 0: sink.sym(V(cc.eob_bin_16[chroma][0]), 4, eob_sym); break;
    case 1: sink.sym(V(cc.eob_bin_32[chroma][0]), 5, eob_sym); break;
    case 2: sink.sym(V(cc.eob_bin_64[chroma][0]), 6, eob_sym); break;
    case 3: sink.sym(V(cc.eob_bin_128[chroma][0]), 7, eob_sym); break;
    case 4: sink.sym(V(cc.eob_bin_256[chroma][0]), 8, eob_sym); break;
    case 5: sink.sym(V(cc.eob_bin_512[chroma]), 9, eob_sym); break;
    default: sink.sym(V(cc.eob_bin_1024[chroma]), 10, eob_sym); break;
  }
  if (eob_bin >= 0) {
    sink.sym(V(cc.eob_hi_bit[t_dim.ctx][chroma][eob_bin]), 1, (static_cast<unsigned>(eob) >> eob_bin) & 1);
    sink.equi_bits(static_cast<unsigned>(eob) & ((1u << eob_bin) - 1), static_cast<unsigned>(eob_bin));
  }
  // base and range tokens (decode_tokens in reverse)
  auto& eob_cdf = cc.eob_base_tok[t_dim.ctx][chroma];
  auto& hi_cdf = cc.br_tok[imin(t_dim.ctx, 3)][chroma];
  auto& lo_cdf = cc.base_tok[t_dim.ctx][chroma];
  const ArrayView<const uint16_t> scan = scan_for(tx);
  auto absl = [&](unsigned rc) { const int32_t v = c.level[rc]; return static_cast<unsigned>(v < 0 ? -v : v); };
  if (eob == 0) {
    const unsigned lv = umin(absl(0), 15);
    sink.sym(V(eob_cdf[0]), 2, umin(lv, 3) - 1);
    if (lv >= 3) sink.hi_tok(V(hi_cdf[0]), lv);
  } else {
    const unsigned nonsquare_tx = tx >= RTX_4X8;
    const uint8_t(*lo_ctx_offsets)[5] = k_lo_ctx_offsets[nonsquare_tx + (tx & nonsquare_tx)];
    const unsigned stride = 4u << slh, shift = static_cast<unsigned>(slh) + 2, mask = (4u << slh) - 1;
    uint8_t levels[32 * 34];
    ArrayView<uint8_t> lv_view(levels);
    lv_view.first(stride * ((4u << slw) + 2)).fill(0);
    unsigned ctx = 1 + (eob > (2 << tx2dszctx)) + (eob > (4 << tx2dszctx));
    unsigned rc = scan[static_cast<uint64_t>(eob)];
    unsigned x = rc >> shift, y = rc & mask;
    unsigned lv = umin(absl(rc), 15);
    sink.sym(V(eob_cdf[ctx]), 2, umin(lv, 3) - 1);
    if (lv >= 3) {
      ctx = (x | y) > 1 ? 14 : 7;
      sink.hi_tok(V(hi_cdf[ctx]), lv);
      levels[rc] = static_cast<uint8_t>(lv + (3 << 6));
    } else {
      levels[rc] = static_cast<uint8_t>(lv * 0x41);
    }
    for (int i = eob - 1; i > 0; --i) {
      const unsigned rc_i = scan[static_cast<uint64_t>(i)];
      x = rc_i >> shift;
      y = rc_i & mask;
      unsigned mag = 0;
      ctx = get_lo_ctx(lv_view, rc_i, mag, lo_ctx_offsets, x, y, stride);
      y |= x;
      lv = umin(absl(rc_i), 15);
      sink.sym(V(lo_cdf[ctx]), 3, umin(lv, 3));
      if (lv >= 3) {
        mag &= 63;
        ctx = (y > 1 ? 14 : 7) + (mag > 12 ? 6 : (mag + 1) >> 1);
        sink.hi_tok(V(hi_cdf[ctx]), lv);
        levels[rc_i] = static_cast<uint8_t>(lv + (3 << 6));
      } else {
        levels[rc_i] = static_cast<uint8_t>(lv * 0x41);
      }
    }
    lv = umin(absl(0), 15);
    sink.sym(V(lo_cdf[0]), 3, umin(lv, 3));
    if (lv >= 3) {
      unsigned mag = levels[0 * stride + 1] + levels[1 * stride + 0] + levels[1 * stride + 1];
      mag &= 63;
      ctx = mag > 12 ? 6 : (mag + 1) >> 1;
      sink.hi_tok(V(hi_cdf[ctx]), lv);
    }
  }
  // signs and the Golomb remainders, in scan order
  unsigned cul_level = 0, dc_sign_level;
  const unsigned dc = absl(0);
  if (!dc) {
    dc_sign_level = 1 << 6;
  } else {
    const unsigned dc_sign_ctx = get_dc_sign_ctx(t_dim, a, l);
    const unsigned sign = c.level[0] < 0;
    sink.sym(V(cc.dc_sign[chroma][dc_sign_ctx]), 1, sign);
    dc_sign_level = sign ? 0 : (2 << 6);
    if (dc >= 15) sink.golomb(dc - 15);
    cul_level += dc;
  }
  for (int i = 1; i <= eob; ++i) {
    const unsigned rc = scan[static_cast<uint64_t>(i)];
    const unsigned lvl = absl(rc);
    if (!lvl) continue;
    sink.equi(c.level[rc] < 0);
    if (lvl >= 15) sink.golomb(lvl - 15);
    cul_level += lvl;
  }
  return umin(cul_level, 63) | dc_sign_level;
}

uint32_t FrameEncoder::mv_bits(Mv mv, Mv ref) const {
  const int dx = iabs(mv.x - ref.x), dy = iabs(mv.y - ref.y);
  return static_cast<uint32_t>((dx ? 256 + 2 * log2_256(static_cast<uint32_t>(dx)) : 0) + (dy ? 256 + 2 * log2_256(static_cast<uint32_t>(dy)) : 0) + 256);
}

void FrameEncoder::emit_block(Sink& sink, BlockSize bs, Decision& d, bool code_delta, bool code_cdef) {
  TaskContext& t = *t_;
  const TileState& ts = *ts_;
  BlockContext& a = *t.a;
  BlockContext& l = t.l;
  CdfModeContext& cdf = cost_cdf_.m;
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  const int bx4 = t.bx & 31, by4 = t.by & 31;
  const int cbx4 = bx4 >> 1, cby4 = by4 >> 1;
  const bool have_left = t.bx > ts.tiling.col_start, have_top = t.by > ts.tiling.row_start;
  sink.sym(V(cdf.skip[a.skip[bx4] + l.skip[by4]]), 1, d.skip);
  if (!d.skip && code_cdef) sink.cdef(hdr_.cdef.n_bits, cur_sb_);
  if (code_delta) {
    const int delta = (d.qidx - ts.last_qidx) >> hdr_.delta.q.res_log2;
    const unsigned mag = static_cast<unsigned>(iabs(delta));
    sink.sym(V(cdf.delta_q), 3, umin(mag, 3));
    if (mag >= 3) {
      const int n_bits = ulog2(mag - 1);
      sink.equi_bits(static_cast<unsigned>(n_bits - 1), 3);
      sink.equi_bits(mag - 1 - (1u << n_bits), static_cast<unsigned>(n_bits));
    }
    if (mag) sink.equi(delta < 0);
  }
  if (inter_frame_) sink.sym(V(cdf.intra[get_intra_ctx(a, l, by4, bx4, have_top, have_left)]), 1, !d.intra);
  const int ytx = k_max_txfm_size_for_bs[bs][0];
  const TxfmInfo& t_dim = k_txfm_dimensions[ytx];
  if (d.intra) {
    if (inter_frame_) sink.sym(V(cdf.y_mode[k_ymode_size_context[bs]]), N_INTRA_PRED_MODES - 1, d.y_mode);
    else sink.sym(V(cost_cdf_.kfym[k_intra_mode_context[a.mode[bx4]]][k_intra_mode_context[l.mode[by4]]]), N_INTRA_PRED_MODES - 1, d.y_mode);
    if (d.y_mode >= VERT_PRED && d.y_mode <= VERT_LEFT_PRED) sink.sym(V(cdf.angle_delta[d.y_mode - VERT_PRED]), 6, 3);
    const bool cfl_allowed = (cfl_allowed_mask & (1u << bs)) != 0;
    sink.sym(V(cdf.uv_mode[cfl_allowed][d.y_mode]), N_UV_INTRA_PRED_MODES - 1 - !cfl_allowed, d.uv_mode);
    if (d.uv_mode >= VERT_PRED && d.uv_mode <= VERT_LEFT_PRED) sink.sym(V(cdf.angle_delta[d.uv_mode - VERT_PRED]), 6, 3);
    if (hdr_.txfm_mode == TX_SWITCHABLE && t_dim.max > TX_4X4) {
      const int tctx = get_tx_ctx(a, l, t_dim, by4, bx4);
      sink.sym(V(cdf.txsz[t_dim.max - 1][tctx]), static_cast<unsigned>(imin(t_dim.max, 2)), 0);
    }
  } else {
    const int r = d.ref;
    const int ctx1 = get_ref_ctx(a, l, by4, bx4, have_top, have_left);
    sink.sym(V(cdf.ref[0][ctx1]), 1, r >= 4);
    if (r >= 4) {
      const int ctx2 = get_bwd_ref_ctx(a, l, by4, bx4, have_top, have_left);
      sink.sym(V(cdf.ref[1][ctx2]), 1, r == 6);
      if (r != 6) sink.sym(V(cdf.ref[5][get_bwd_ref_1_ctx(a, l, by4, bx4, have_top, have_left)]), 1, r - 4);
    } else {
      const int ctx2 = get_fwd_ref_ctx(a, l, by4, bx4, have_top, have_left);
      sink.sym(V(cdf.ref[2][ctx2]), 1, r >= 2);
      if (r >= 2) sink.sym(V(cdf.ref[4][get_fwd_ref_2_ctx(a, l, by4, bx4, have_top, have_left)]), 1, r - 2);
      else sink.sym(V(cdf.ref[3][get_fwd_ref_1_ctx(a, l, by4, bx4, have_top, have_left)]), 1, r);
    }
    // (the stack the decoder makes for this reference: the decision was made with it)
    RefmvsCandidate mvstack[8] = {};
    int n_mvs = 0, ctx = 0;
    RefmvsRefPair rp;
    rp.ref[0] = static_cast<int8_t>(r + 1);
    rp.ref[1] = -1;
    f_.rf.find(t.rt, ArrayView<RefmvsCandidate>(mvstack), n_mvs, ctx, rp, bs, (cur_edge_flags_ & EDGE_I444_TOP_HAS_RIGHT) != 0, t.by, t.bx);
    const ArrayView<const RefmvsCandidate> mvs(mvstack);
    if (d.inter_mode == NEWMV) {
      sink.sym(V(cdf.newmv_mode[ctx & 7]), 1, 0);
      if (n_mvs > 1) {
        sink.sym(V(cdf.drl_bit[get_drl_context(mvs, 0)]), 1, d.drl >= 1);
        if (d.drl >= 1 && n_mvs > 2) sink.sym(V(cdf.drl_bit[get_drl_context(mvs, 1)]), 1, d.drl >= 2);
      }
      // the vector residual
      const int dy = d.mv.y - d.ref_mv.y, dx = d.mv.x - d.ref_mv.x;
      const unsigned joint = (dy ? static_cast<unsigned>(MV_JOINT_V) : 0u) | (dx ? static_cast<unsigned>(MV_JOINT_H) : 0u);
      sink.sym(V(cost_cdf_.mv.joint), N_MV_JOINTS - 1, joint);
      const int comps[2] = {dy, dx};
      for (int k = 0; k < 2; ++k) {
        const int diff = comps[k];
        if (!diff) continue;
        CdfMvComponent& mc = cost_cdf_.mv.comp[k];
        const unsigned sign = diff < 0;
        const unsigned v = static_cast<unsigned>(iabs(diff)) - 1;
        const unsigned up = v >> 3, fp = (v >> 1) & 3, hp = v & 1;
        const unsigned cl = up < 2 ? 0 : static_cast<unsigned>(ulog2(up));
        sink.sym(V(mc.sign), 1, sign);
        sink.sym(V(mc.classes), 10, cl);
        if (cl == 0) {
          sink.sym(V(mc.class0), 1, up);
          sink.sym(V(mc.class0_fp[up]), 3, fp);
          if (hdr_.hp) sink.sym(V(mc.class0_hp), 1, hp);
        } else {
          for (unsigned b = 0; b < cl; ++b) sink.sym(V(mc.classN[b]), 1, (up >> b) & 1);
          sink.sym(V(mc.classN_fp), 3, fp);
          if (hdr_.hp) sink.sym(V(mc.classN_hp), 1, hp);
        }
      }
    } else {
      sink.sym(V(cdf.newmv_mode[ctx & 7]), 1, 1);
      sink.sym(V(cdf.globalmv_mode[(ctx >> 3) & 1]), 1, d.inter_mode != GLOBALMV);
      if (d.inter_mode != GLOBALMV) {
        sink.sym(V(cdf.refmv_mode[(ctx >> 4) & 15]), 1, d.inter_mode == NEARMV);
        if (d.inter_mode == NEARMV && n_mvs > 2) {
          sink.sym(V(cdf.drl_bit[get_drl_context(mvs, 1)]), 1, d.drl >= 2);
          if (d.drl >= 2 && n_mvs > 3) sink.sym(V(cdf.drl_bit[get_drl_context(mvs, 2)]), 1, d.drl >= 3);
        }
      }
    }
    if (hdr_.subpel_filter_mode == FILTER_SWITCHABLE && d.has_subpel_filter)
      sink.sym(V(cdf.filter[0][get_filter_ctx(a, l, 0, 0, r, by4, bx4)]), N_SWITCHABLE_FILTERS - 1, static_cast<unsigned>(d.filter));
    if (hdr_.txfm_mode == TX_SWITCHABLE && !d.skip) {
      const int cat = 2 * (TX_64X64 - t_dim.max) - 0;
      const int ca = a.tx[bx4] < t_dim.lw, cl = l.tx[by4] < t_dim.lh;
      sink.sym(V(cdf.txpart[cat][ca + cl]), 1, 0);
    }
  }
  if (!d.skip) {
    d.res_ctx[0] = static_cast<uint8_t>(emit_coefs(sink, ctx_view(a.lcoef, bx4), ctx_view(l.lcoef, by4), bs, d.intra, 0, d));
    for (int pl = 0; pl < 2; ++pl)
      d.res_ctx[1 + pl] = static_cast<uint8_t>(emit_coefs(sink, ctx_view(a.ccoef[pl], cbx4), ctx_view(l.ccoef[pl], cby4), bs, d.intra, 1 + pl, d));
  }
  (void)b_dim;
}

void FrameEncoder::update_contexts(BlockSize bs, const Decision& d) {
  TaskContext& t = *t_;
  FrameContext& f = f_;
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  const int bx4 = t.bx & 31, by4 = t.by & 31, cbx4 = bx4 >> 1, cby4 = by4 >> 1;
  const int bw4 = b_dim[0], bh4 = b_dim[1];
  const int cbw4 = (bw4 + 1) >> 1, cbh4 = (bh4 + 1) >> 1;
  const TxfmInfo& t_dim = k_txfm_dimensions[k_max_txfm_size_for_bs[bs][0]];
  const TxfmInfo& uv_dim = k_txfm_dimensions[k_max_txfm_size_for_bs[bs][PIXEL_LAYOUT_I420]];
  // coefficients
  if (d.skip) {
    set_ctx_n(t.a->lcoef, bx4, 1 << t_dim.lw, 0x40);
    set_ctx_n(t.l.lcoef, by4, 1 << t_dim.lh, 0x40);
    for (int pl = 0; pl < 2; ++pl) {
      set_ctx_n(t.a->ccoef[pl], cbx4, 1 << uv_dim.lw, 0x40);
      set_ctx_n(t.l.ccoef[pl], cby4, 1 << uv_dim.lh, 0x40);
    }
  } else {
    set_ctx_n(t.a->lcoef, bx4, imin(t_dim.w, f.bw - t.bx), d.res_ctx[0]);
    set_ctx_n(t.l.lcoef, by4, imin(t_dim.h, f.bh - t.by), d.res_ctx[0]);
    const int ctw = imin(uv_dim.w, (f.bw - t.bx + 1) >> 1), cth = imin(uv_dim.h, (f.bh - t.by + 1) >> 1);
    for (int pl = 0; pl < 2; ++pl) {
      set_ctx_n(t.a->ccoef[pl], cbx4, ctw, d.res_ctx[1 + pl]);
      set_ctx_n(t.l.ccoef[pl], cby4, cth, d.res_ctx[1 + pl]);
    }
  }
  if (d.intra) {
    for (int i = 0; i < 2; ++i) {
      BlockContext& edge = i ? t.l : *t.a;
      const int off = i ? by4 : bx4;
      const int n = b_dim[2 + i];
      const int t_lsz = i ? t_dim.lh : t_dim.lw;
      set_ctx_log2(edge.tx_intra, off, n, t_lsz);
      set_ctx_log2(edge.tx, off, n, t_lsz);
      set_ctx_log2(edge.mode, off, n, d.y_mode);
      set_ctx_log2(edge.pal_sz, off, n, 0);
      set_ctx_log2(edge.seg_pred, off, n, 0);
      set_ctx_log2(edge.skip_mode, off, n, 0);
      set_ctx_log2(edge.intra, off, n, 1);
      set_ctx_log2(edge.skip, off, n, d.skip);
      set_ctx_log2(t.pal_sz_uv[i], off, n, 0);
      if (inter_frame_) {
        set_ctx_log2(edge.comp_type, off, n, COMP_INTER_NONE);
        set_ctx_log2(edge.ref[0], off, n, -1);
        set_ctx_log2(edge.ref[1], off, n, -1);
        set_ctx_log2(edge.filter[0], off, n, N_SWITCHABLE_FILTERS);
        set_ctx_log2(edge.filter[1], off, n, N_SWITCHABLE_FILTERS);
      }
    }
    set_ctx_log2(t.a->uvmode, cbx4, ulog2(static_cast<unsigned>(cbw4)), d.uv_mode);
    set_ctx_log2(t.l.uvmode, cby4, ulog2(static_cast<unsigned>(cbh4)), d.uv_mode);
    if (inter_frame_) {
      RefmvsBlock tmpl{};
      tmpl.ref.ref[0] = 0;
      tmpl.ref.ref[1] = -1;
      tmpl.mv.mv[0].n = INVALID_MV;
      tmpl.bs = static_cast<uint8_t>(bs);
      tmpl.mf = 0;
      f.rf.splat_mv(t.rt, tmpl, t.by, t.bx, bw4, bh4);
    }
  } else {
    RefmvsBlock tmpl{};
    tmpl.ref.ref[0] = static_cast<int8_t>(d.ref + 1);
    tmpl.ref.ref[1] = -1;
    tmpl.mv.mv[0] = d.mv;
    tmpl.bs = static_cast<uint8_t>(bs);
    tmpl.mf = static_cast<uint8_t>((d.inter_mode == GLOBALMV && imin(bw4, bh4) >= 2) | ((d.inter_mode == NEWMV) * 2));
    f.rf.splat_mv(t.rt, tmpl, t.by, t.bx, bw4, bh4);
    const int filt = hdr_.subpel_filter_mode == FILTER_SWITCHABLE ? (d.has_subpel_filter ? d.filter : 0) : static_cast<int>(hdr_.subpel_filter_mode);
    for (int i = 0; i < 2; ++i) {
      BlockContext& edge = i ? t.l : *t.a;
      const int off = i ? by4 : bx4;
      const int n = b_dim[2 + i];
      set_ctx_log2(edge.seg_pred, off, n, 0);
      set_ctx_log2(edge.skip_mode, off, n, 0);
      set_ctx_log2(edge.intra, off, n, 0);
      set_ctx_log2(edge.skip, off, n, d.skip);
      set_ctx_log2(edge.pal_sz, off, n, 0);
      set_ctx_log2(t.pal_sz_uv[i], off, n, 0);
      set_ctx_log2(edge.tx_intra, off, n, b_dim[2 + i]);
      set_ctx_log2(edge.comp_type, off, n, COMP_INTER_NONE);
      set_ctx_log2(edge.filter[0], off, n, filt);
      set_ctx_log2(edge.filter[1], off, n, filt);
      set_ctx_log2(edge.mode, off, n, d.inter_mode);
      set_ctx_log2(edge.ref[0], off, n, d.ref);
      set_ctx_log2(edge.ref[1], off, n, -1);
    }
    set_ctx_log2(t.a->uvmode, cbx4, ulog2(static_cast<unsigned>(cbw4)), DC_PRED);
    set_ctx_log2(t.l.uvmode, cby4, ulog2(static_cast<unsigned>(cbh4)), DC_PRED);
    if (hdr_.txfm_mode == TX_SWITCHABLE) {
      // (read_vartx_tree: a skipped block sets the block's size; an unsplit transform its own, the same for square blocks)
      set_ctx_log2(t.a->tx, bx4, b_dim[2], d.skip ? b_dim[2] : t_dim.lw);
      set_ctx_log2(t.l.tx, by4, b_dim[3], d.skip ? b_dim[3] : t_dim.lh);
    }
  }
}

// ---- mode decisions -----------------------------------------------------------------------------------------------------------------------------------------

bool FrameEncoder::eval_intra(BlockSize bs, int edge_flags, Decision& d) {
  const TaskContext& t = *t_;
  d.intra = true;
  d.qidx = block_qidx();
  // the luma mode with the smallest prediction error
  uint64_t best = ~uint64_t{0};
  uint8_t best_mode = DC_PRED;
  for (uint8_t m = DC_PRED; m < N_INTRA_PRED_MODES; ++m) {
    d.y_mode = m;
    d.uv_mode = DC_PRED;
    if (!predict(bs, edge_flags, d)) return false;
    const uint64_t e = sse_block(bs, true, false) + (m == DC_PRED ? 0 : lambda() * 2);
    if (e < best) {
      best = e;
      best_mode = m;
    }
  }
  d.y_mode = best_mode;
  // chroma: DC, the luma mode or smooth
  const uint8_t uv_cands[3] = {DC_PRED, best_mode, SMOOTH_PRED};
  best = ~uint64_t{0};
  uint8_t best_uv = DC_PRED;
  for (uint8_t m : uv_cands) {
    d.uv_mode = m;
    if (!predict(bs, edge_flags, d)) return false;
    const uint64_t e = sse_block(bs, false, true);
    if (e < best) {
      best = e;
      best_uv = m;
    }
  }
  d.uv_mode = best_uv;
  if (!predict(bs, edge_flags, d) || !residual(bs, d)) return false;
  (void)t;
  return true;
}

uint32_t FrameEncoder::sad_int(int bx_px, int by_px, int w, int h, int ref, int mvx8, int mvy8) const {
  const Picture& rp = *f_.refp[ref];
  const PixelView r = rp.plane(0), s = src_->plane(0);
  const int rw = rp.width(), rh = rp.height();
  const int ox = bx_px + (mvx8 >> 3), oy = by_px + (mvy8 >> 3);
  uint32_t sad = 0;
  for (int y = 0; y < h; ++y) {
    const int yy = iclip(oy + y, 0, rh - 1);
    for (int x = 0; x < w; ++x) sad += static_cast<uint32_t>(iabs(static_cast<int>(s.at(bx_px + x, by_px + y)) - static_cast<int>(r.at(iclip(ox + x, 0, rw - 1), yy))));
  }
  return sad;
}

// The luma prediction of a w x h block for `mv` (mc_block's unscaled path) against the source.
uint32_t FrameEncoder::sad_sub(int bx_px, int by_px, int w, int h, int ref, Mv mv, int filter2d) {
  const Picture& rp = *f_.refp[ref];
  TaskContext& t = *t_;
  const int mx = mv.x & 7, my = mv.y & 7;
  const int dx = bx_px + (mv.x >> 3), dy = by_px + (mv.y >> 3);
  const int rw = rp.width(), rh = rp.height();
  const PixelView plane = rp.plane(0);
  PixelView refv = plane;
  if (dx < !!mx * 3 || dy < !!my * 3 || dx + w + !!mx * 4 > rw || dy + h + !!my * 4 > rh) {
    const PixelView emu{ArrayView<Pixel>(t.scratch.emu_edge), 0, 192};
    emu_edge(w + !!mx * 7, h + !!my * 7, rw, rh, dx - !!mx * 3, dy - !!my * 3, emu, plane);
    refv = emu.offset(!!mx * 3, !!my * 3);
  } else {
    refv = plane.offset(dx, dy);
  }
  const PixelView dst{ArrayView<Pixel>(t.scratch.interintra), 0, 64};
  mc(static_cast<Filter2d>(filter2d), dst, refv, w, h, mx << 1, my << 1, f_.bitdepth_max);
  const PixelView s = src_->plane(0);
  uint32_t sad = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) sad += static_cast<uint32_t>(iabs(static_cast<int>(s.at(bx_px + x, by_px + y)) - static_cast<int>(dst.at(x, y))));
  return sad;
}

bool FrameEncoder::eval_inter(BlockSize bs, int edge_flags, Decision& d) {
  TaskContext& t = *t_;
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  const int w = b_dim[0] * 4, h = b_dim[1] * 4;
  const int px = t.bx * 4, py = t.by * 4;
  const Av1EncPicture& pic = *pic_;
  const bool switchable = hdr_.subpel_filter_mode == FILTER_SWITCHABLE;
  const int base_filter = switchable ? 0 : static_cast<int>(hdr_.subpel_filter_mode);
  const uint64_t sad_lambda = isqrt64(lambda() * 64) + 1;   // ~ sqrt(lambda), per 1/8 bit
  uint64_t best_cost = ~uint64_t{0};
  struct Best { int ref, inter_mode, drl, filter; Mv mv, ref_mv; bool has_subpel_filter; } best{};
  bool any = false;
  bool seen[8] = {};
  for (int r = 0; r < 7; ++r) {
    const int slot = pic.ref_frame_idx[r];
    if (seen[slot]) continue;
    seen[slot] = true;
    RefmvsCandidate mvstack[8] = {};
    int n_mvs = 0, ctx = 0;
    RefmvsRefPair rp;
    rp.ref[0] = static_cast<int8_t>(r + 1);
    rp.ref[1] = -1;
    f_.rf.find(t.rt, ArrayView<RefmvsCandidate>(mvstack), n_mvs, ctx, rp, bs, (edge_flags & EDGE_I444_TOP_HAS_RIGHT) != 0, t.by, t.bx);
    // the vector each mode gives
    struct Cand { int mode, drl; Mv mv, ref; };
    Cand cands[8];
    int nc = 0;
    {
      Mv m = mvstack[0].mv.mv[0];
      fix_mv_precision(hdr_, m);
      cands[nc++] = {NEARESTMV, 0, m, m};
      for (int k = 1; k <= 3; ++k) {
        if (k > 1 && n_mvs <= k) break;
        Mv mm = mvstack[k].mv.mv[0];
        if (k < NEAR_DRL) fix_mv_precision(hdr_, mm);
        cands[nc++] = {NEARMV, k, mm, mm};
      }
      Mv z{};
      z.n = 0;
      cands[nc++] = {GLOBALMV, 0, z, z};
    }
    // NEWMV: the search
    Mv ref_mv = mvstack[0].mv.mv[0];
    if (n_mvs <= 1) fix_mv_precision(hdr_, ref_mv);
    const int me_col = imin((px + w / 2) / 16, me_cols_ - 1), me_row = imin((py + h / 2) / 16, me_rows_ - 1);
    const MotionBlockResult& me = me_[slot][static_cast<uint64_t>(me_row) * static_cast<uint64_t>(me_cols_) + static_cast<uint64_t>(me_col)];
    int starts[6][2] = {{0, 0}, {me.vector.x * 2, me.vector.y * 2}, {ref_mv.x, ref_mv.y}, {cands[0].mv.x, cands[0].mv.y}, {0, 0}, {0, 0}};
    int ns = 4;
    if (n_mvs > 1) {
      starts[ns][0] = mvstack[1].mv.mv[0].x;
      starts[ns][1] = mvstack[1].mv.mv[0].y;
      ++ns;
    }
    int bxi = 0, byi = 0;
    uint64_t bsad = ~uint64_t{0};
    for (int i = 0; i < ns; ++i) {
      const int ix = (starts[i][0] + 4) >> 3 << 3, iy = (starts[i][1] + 4) >> 3 << 3;
      if (iabs(ix) > 8 * 256 || iabs(iy) > 8 * 256) continue;
      const uint64_t c = sad_int(px, py, w, h, r, ix, iy);
      if (c < bsad) {
        bsad = c;
        bxi = ix;
        byi = iy;
      }
    }
    // whole samples around the best start, then half and quarter samples
    const int range = pic.precision == Av1EncPrecision::Full ? 4 : 3;
    {
      const int cx = bxi, cy = byi;
      for (int y = -range; y <= range; ++y)
        for (int x = -range; x <= range; ++x) {
          const uint64_t c = sad_int(px, py, w, h, r, cx + x * 8, cy + y * 8);
          if (c < bsad) {
            bsad = c;
            bxi = cx + x * 8;
            byi = cy + y * 8;
          }
        }
    }
    Mv nm;
    nm.x = static_cast<int16_t>(bxi);
    nm.y = static_cast<int16_t>(byi);
    const int f2d = k_filter_2d[base_filter][base_filter];
    uint64_t bsub = sad_sub(px, py, w, h, r, nm, f2d);
    const int steps = pic.precision == Av1EncPrecision::Full ? 0 : pic.precision == Av1EncPrecision::Half ? 1 : 2;
    for (int s = 0; s < steps; ++s) {
      const int st = s == 0 ? 4 : 2;
      const Mv c0 = nm;
      for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
          if (!x && !y) continue;
          Mv m;
          m.x = static_cast<int16_t>(c0.x + x * st);
          m.y = static_cast<int16_t>(c0.y + y * st);
          const uint64_t c = sad_sub(px, py, w, h, r, m, f2d);
          if (c < bsub) {
            bsub = c;
            nm = m;
          }
        }
    }
    // the reference vector of NEWMV: the stack entry (drl) nearest the found vector
    int ndrl = 0;
    Mv nref = ref_mv;
    if (n_mvs > 1) {
      uint32_t bb = mv_bits(nm, mvstack[0].mv.mv[0]);
      for (int k = 1; k < imin(n_mvs, 3); ++k) {
        const uint32_t bk = mv_bits(nm, mvstack[k].mv.mv[0]) + 256 * static_cast<uint32_t>(k);
        if (bk < bb) {
          bb = bk;
          ndrl = k;
        }
      }
      nref = mvstack[ndrl].mv.mv[0];
    }
    // without high precision the residual is even
    Mv fin = nm;
    if (!hdr_.hp) {
      const int ddx = fin.x - nref.x, ddy = fin.y - nref.y;
      fin.x = static_cast<int16_t>(nref.x + ((ddx + (ddx > 0 ? 1 : 0)) & ~1));
      fin.y = static_cast<int16_t>(nref.y + ((ddy + (ddy > 0 ? 1 : 0)) & ~1));
    }
    if (iabs(fin.x - nref.x) < (1 << 13) && iabs(fin.y - nref.y) < (1 << 13)) cands[nc++] = {NEWMV, ndrl, fin, nref};
    // the candidate with the smallest error and rate
    for (int i = 0; i < nc; ++i) {
      const Cand& c = cands[i];
      if (iabs(c.mv.x) > 8 * 1024 || iabs(c.mv.y) > 8 * 1024) continue;
      const bool subpel = !(c.mode == GLOBALMV);
      int filt = base_filter;
      uint64_t e;
      if (switchable && subpel) {
        uint64_t be = ~uint64_t{0};
        for (int fl = 0; fl < 3; ++fl) {
          const uint64_t s = sad_sub(px, py, w, h, r, c.mv, k_filter_2d[fl][fl]);
          if (s < be) {
            be = s;
            filt = fl;
          }
        }
        e = be;
      } else {
        e = sad_sub(px, py, w, h, r, c.mv, k_filter_2d[base_filter][base_filter]);
      }
      const uint64_t bits = (c.mode == NEWMV ? mv_bits(c.mv, c.ref) + 512 : 256 + 128 * static_cast<uint64_t>(c.drl)) + 256 * static_cast<uint64_t>(r > 0);
      const uint64_t cost = e * 8 + ((bits * sad_lambda) >> 8);
      if (cost < best_cost) {
        best_cost = cost;
        best.ref = r;
        best.inter_mode = c.mode;
        best.drl = c.drl;
        best.mv = c.mv;
        best.ref_mv = c.ref;
        best.filter = filt;
        best.has_subpel_filter = subpel;
        any = true;
      }
    }
  }
  if (!any) return false;
  d.intra = false;
  d.ref = best.ref;
  d.inter_mode = best.inter_mode;
  d.drl = best.drl;
  d.mv = best.mv;
  d.ref_mv = best.ref_mv;
  d.filter = best.filter;
  d.has_subpel_filter = best.has_subpel_filter;
  d.qidx = block_qidx();
  return predict(bs, edge_flags, d) && residual(bs, d);
}

bool FrameEncoder::encode_b(BlockLevel bl, BlockSize bs, int edge_flags, uint64_t* cost) {
  TaskContext& t = *t_;
  TileState& ts = *ts_;
  (void)bl;
  cur_edge_flags_ = edge_flags;
  const bool origin = sb_delta_pending_;
  const bool delta_present = hdr_.delta.q.present != 0;
  // the candidates: intra, and inter in inter frames; each costed with the symbols it would write
  Decision* best = nullptr;
  Decision& di = cand_[0];
  if (!eval_intra(bs, edge_flags, di)) return false;
  auto price = [&](Decision& d) {
    Sink s{&cost_cdf_, nullptr};
    const bool code_delta = origin && delta_present && (bs != BS_64x64 || !d.skip);
    emit_block(s, bs, d, code_delta, !sb_cdef_coded_);
    d.bits = s.bits;
    d.cost = d.sse + ((s.bits * lambda()) >> 8);
  };
  price(di);
  best = &di;
  if (inter_frame_) {
    Decision& dn = cand_[1];
    if (!eval_inter(bs, edge_flags, dn)) return false;
    price(dn);
    if (dn.cost < best->cost) best = &dn;
  }
  Decision& d = *best;
  // commit: predict and reconstruct again (the trials left the last candidate's samples), log the symbols, update the contexts
  if (!predict(bs, edge_flags, d) || !residual(bs, d)) return false;
  const bool code_delta = origin && delta_present && (bs != BS_64x64 || !d.skip);
  const bool code_cdef = !sb_cdef_coded_ && !d.skip;
  Sink s{&cost_cdf_, &log_};
  emit_block(s, bs, d, code_delta, !sb_cdef_coded_);
  if (!s.ok) return false;
  if (code_cdef) sb_cdef_coded_ = true;
  if (code_delta) {
    ts.last_qidx = iclip(ts.last_qidx + (((d.qidx - ts.last_qidx) >> hdr_.delta.q.res_log2) << hdr_.delta.q.res_log2), 1, 255);
  }
  sb_delta_pending_ = false;
  update_contexts(bs, d);
  // statistics: 8x8 units
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  const uint64_t units = static_cast<uint64_t>(b_dim[0]) * b_dim[1] / 4;
  stats_.units += units;
  stats_.qp_sum += units * static_cast<uint64_t>(d.qidx);
  if (d.intra) {
    stats_.intra += units;
  } else {
    stats_.inter += units;
    if (d.skip) stats_.skipped += units;
    stats_.vector_x_sum += static_cast<int64_t>(units) * (d.mv.x / 2);
    stats_.vector_y_sum += static_cast<int64_t>(units) * (d.mv.y / 2);
  }
  *cost = d.sse + ((s.bits * lambda()) >> 8);
  (void)t;
  return true;
}

bool FrameEncoder::encode_tile(int tile_row, int tile_col) {
  FrameContext& f = f_;
  TaskContext& t = *t_;
  TileState& ts = *ts_;
  const FrameHeader& hdr = *f.frame_hdr;
  t.f = &f;
  t.ts = &ts;
  const int col_sb_start = hdr.tiling.col_start_sb[tile_col], col_sb_end = hdr.tiling.col_start_sb[tile_col + 1];
  const int row_sb_start = hdr.tiling.row_start_sb[tile_row], row_sb_end = hdr.tiling.row_start_sb[tile_row + 1];
  ts.last_qidx = hdr.quant.yac;
  for (int8_t& v : ts.last_delta_lf) v = 0;
  ts.dq = &f.dq;
  ts.tiling.row = tile_row;
  ts.tiling.col = tile_col;
  ts.tiling.col_start = col_sb_start << f.sb_shift;
  ts.tiling.col_end = imin(col_sb_end << f.sb_shift, f.bw);
  ts.tiling.row_start = row_sb_start << f.sb_shift;
  ts.tiling.row_end = imin(row_sb_end << f.sb_shift, f.bh);
  const int sbh_end = imin(row_sb_end, f.sbh);
  const int col_sb128_start = col_sb_start >> 1;
  for (int sby = row_sb_start; sby < sbh_end; ++sby) {
    t.by = sby << 4;
    if (inter_frame_) f.rf.tile_sbrow_init(t.rt, ts.tiling.col_start, ts.tiling.col_end, ts.tiling.row_start, ts.tiling.row_end, sby);
    reset_block_context(t.l, is_key_or_intra(hdr));
    ArrayView<uint8_t>(t.pal_sz_uv[1]).fill(0);
    const EdgeNode& root = k_intra_edge_tree.root(BL_64X64);
    for (t.bx = ts.tiling.col_start, t.a = &f.a[static_cast<uint64_t>(col_sb128_start + tile_row * f.sb128w)]; t.bx < ts.tiling.col_end; t.bx += f.sb_step) {
      const int sbx = t.bx >> 4;
      cur_sb_ = static_cast<uint32_t>(sby * sb_cols_ + sbx);
      sb_target_qidx_ = sb_qidx_[cur_sb_];
      sb_delta_pending_ = true;
      sb_cdef_coded_ = false;
      uint64_t c = 0;
      if (!encode_sb(BL_64X64, root, &c)) return false;
      if (t.bx & 16) t.a++;
    }
    core_.backup_ipred_edge(t);
  }
  return true;
}

// ---- coding and decoding ------------------------------------------------------------------------------------------------------------------------------------

bool FrameEncoder::code_tiles(ArrayView<const int8_t> cdef_choice, ByteBuffer* data, List<Av1EncTile>* tiles) {
  data->clear();
  tiles->clear();
  SymbolWriter w(allocator_);
  uint64_t start = 0;
  const bool adapt = !hdr_.disable_cdf_update;
  for (uint64_t end : tile_log_end_.view()) {
    cdf_load_default(code_cdf_, cdf_qcat(hdr_.quant.yac));
    const ArrayView<uint16_t> words = cdf_words(code_cdf_);
    w.reset();
    for (uint64_t i = start; i < end; ++i) {
      const Sym& s = log_[i];
      switch (s.kind) {
        case kSymAdapt: w.symbol(words.subview(s.a, uint64_t{s.n} + 1), s.n, s.value, adapt); break;
        case kSymBoolProb: w.bool_prob(s.a, s.value); break;
        case kSymEqui: w.bool_equi(s.value); break;
        case kSymGatherTop:
        case kSymGatherLeft: {
          const ArrayView<const uint16_t> pc = words.subview(s.a, uint64_t{k_partition_type_count[s.n]} + 1);
          w.bool_prob(s.kind == kSymGatherTop ? gather_top_partition_prob(pc, s.n) : gather_left_partition_prob(pc, s.n), s.value);
          break;
        }
        default: {
          const int8_t v = cdef_choice[s.a];
          for (unsigned b = s.n; b-- > 0;) w.bool_equi((static_cast<unsigned>(v) >> b) & 1);
          break;
        }
      }
    }
    const uint64_t at = data->size();
    if (!w.ok() || !w.finish(data)) return false;
    Av1EncTile tile;
    tile.offset = at;
    tile.size = data->size() - at;
    if (!tiles->push_back(tile)) return false;
    start = end;
  }
  return true;
}

bool FrameEncoder::decode(ConstByteView data, ArrayView<const Av1EncTile> tiles, RefSlot* state) {
  OwnedArray<TileData> td(allocator_);
  if (!td.reset(tiles.size())) return false;
  for (uint64_t i = 0; i < tiles.size(); ++i) {
    td[i].data = data.subview(tiles[i].offset, tiles[i].size);
    td[i].tile_row = static_cast<uint16_t>(i / hdr_.tiling.cols);
    td[i].tile_col = static_cast<uint16_t>(i % hdr_.tiling.cols);
  }
  return core_.decode_frame_external(seq_, hdr_, ArrayView<const TileData>(td.view()), ArrayView<const RefSlot>(refs_), state);
}

// Decodes the frame with every 64x64 block on each strength index in turn and gives each block the index with the smallest error.
bool FrameEncoder::choose_cdef(ArrayView<int8_t> choice) {
  const int n = 1 << hdr_.cdef.n_bits;
  OwnedArray<uint64_t> best(allocator_);
  OwnedArray<int8_t> trial(allocator_);
  if (!best.reset(choice.size()) || !trial.reset(choice.size())) return false;
  for (uint64_t& b : best.view()) b = ~uint64_t{0};
  for (int j = 0; j < n; ++j) {
    trial.view().fill(static_cast<int8_t>(j));
    if (!code_tiles(ArrayView<const int8_t>(trial.view()), &tile_bytes_, &tile_list_)) return false;
    RefSlot st;
    if (!decode(tile_bytes_.view(), tile_list_.view(), &st) || !st.pic) return false;
    for (int sby = 0; sby < sb_rows_; ++sby)
      for (int sbx = 0; sbx < sb_cols_; ++sbx) {
        uint64_t e = 0;
        for (int pl = 0; pl < 3; ++pl) {
          const int ss = pl ? 1 : 0;
          const int pw = st.pic->plane_width(pl), ph = st.pic->plane_height(pl);
          const int x0 = (sbx * 64) >> ss, y0 = (sby * 64) >> ss;
          const int x1 = imin(x0 + (64 >> ss), pw), y1 = imin(y0 + (64 >> ss), ph);
          const PixelView a = st.pic->plane(pl), s = src_->plane(pl);
          for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
              const int d = static_cast<int>(a.at(x, y)) - static_cast<int>(s.at(x, y));
              e += static_cast<uint64_t>(d * d);
            }
        }
        const uint64_t k = static_cast<uint64_t>(sby) * static_cast<uint64_t>(sb_cols_) + static_cast<uint64_t>(sbx);
        if (e < best[k]) {
          best[k] = e;
          choice[k] = static_cast<int8_t>(j);
        }
      }
  }
  return true;
}

Av1EncResult FrameEncoder::encode(const Av1EncPicture& pic, const EncPlanes16& source, const EncWritablePlanes16& recon, ByteBuffer* out,
                                  List<Av1EncTile>* tiles, Av1EncPostEncode* post, EncStatistics* stats) {
  pic_ = &pic;
  stats_ = EncStatistics();
  drift_ = 0;
  if (!recon.fits() || recon.width != pic.seq.width || recon.height != pic.seq.height) return Av1EncResult::PictureControlNotSupported;
  const Av1EncResult r = setup(pic, source, post);
  if (r != Av1EncResult::Ok) return r;
  log_.clear();
  tile_log_end_.clear();
  for (int tr = 0; tr < hdr_.tiling.rows; ++tr)
    for (int tc = 0; tc < hdr_.tiling.cols; ++tc) {
      if (!encode_tile(tr, tc)) return Av1EncResult::PictureControlNotSupported;
      if (!tile_log_end_.push_back(log_.count())) return Av1EncResult::NoMemory;
    }
  // the CDEF strength index of every 64x64 block
  if (!cdef_choice_.reset(static_cast<uint64_t>(sb_cols_) * static_cast<uint64_t>(sb_rows_))) return Av1EncResult::NoMemory;
  if (cdef_active_ && hdr_.cdef.n_bits > 0 && !choose_cdef(cdef_choice_.view())) return Av1EncResult::NoMemory;
  if (!code_tiles(ArrayView<const int8_t>(cdef_choice_.view()), &tile_bytes_, &tile_list_)) return Av1EncResult::NoMemory;
  RefSlot st;
  if (!decode(tile_bytes_.view(), tile_list_.view(), &st) || !st.pic) return Av1EncResult::PictureControlNotSupported;
  // (without in-loop filters the decoder's picture is the encoder's own: a difference is an encoder bug, counted for the tests)
  const bool filters_off = !hdr_.loopfilter.level_y[0] && !hdr_.loopfilter.level_y[1] && !cdef_active_;
  const ArrayView<uint16_t> rp[3] = {recon.y, recon.u, recon.v};
  for (int pl = 0; pl < 3; ++pl) {
    const int w = pl ? static_cast<int>(recon.chroma_width()) : static_cast<int>(recon.width);
    const int h = pl ? static_cast<int>(recon.chroma_height()) : static_cast<int>(recon.height);
    const PixelView d = st.pic->plane(pl), own = f_.cur->plane(pl);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const Pixel v = d.at(x, y);
        rp[pl][static_cast<uint64_t>(y) * static_cast<uint64_t>(w) + static_cast<uint64_t>(x)] = v;
        if (filters_off && own.at(x, y) != v) ++drift_;
      }
  }
  const uint64_t base = out->size();
  if (!out->append(tile_bytes_.view())) return Av1EncResult::NoMemory;
  tiles->clear();
  for (const Av1EncTile& tl : tile_list_.view()) {
    Av1EncTile o = tl;
    o.offset += base;
    if (!tiles->push_back(o)) return Av1EncResult::NoMemory;
  }
  if (stats) *stats = stats_;
  return Av1EncResult::Ok;
}

}  // namespace av1

// ---- Av1PictureEncoder --------------------------------------------------------------------------------------------------------------------------------------

Av1PictureEncoder::Av1PictureEncoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
Av1PictureEncoder::~Av1PictureEncoder() {}

bool Av1PictureEncoder::tile_layout(uint32_t width, uint32_t height, Av1EncTiles* t) {
  if (width == 0 || height == 0 || width > 65536 || height > 65536) return false;
  const av1::TileGeometry g(width, height);
  if (t->uniform) {
    if (t->cols == 0 || t->rows == 0 || t->cols > kAv1EncMaxTileCols || t->rows > kAv1EncMaxTileRows) return false;
    int lc = av1::tile_log2(1, static_cast<int>(t->cols));
    lc = av1::imin(av1::imax(lc, g.min_log2_cols), g.max_log2_cols);
    const int tile_w = 1 + ((g.sbw - 1) >> lc);
    int cols = 0;
    for (int sbx = 0; sbx < g.sbw; sbx += tile_w) t->col_width_sb[cols++] = static_cast<uint32_t>(av1::imin(tile_w, g.sbw - sbx));
    const int min_log2_rows = av1::imax(g.min_log2_tiles - lc, 0);
    int lr = av1::tile_log2(1, static_cast<int>(t->rows));
    lr = av1::imin(av1::imax(lr, min_log2_rows), g.max_log2_rows);
    const int tile_h = 1 + ((g.sbh - 1) >> lr);
    int rows = 0;
    for (int sby = 0; sby < g.sbh; sby += tile_h) t->row_height_sb[rows++] = static_cast<uint32_t>(av1::imin(tile_h, g.sbh - sby));
    t->cols_log2 = static_cast<uint32_t>(lc);
    t->rows_log2 = static_cast<uint32_t>(lr);
    t->cols = static_cast<uint32_t>(cols);
    t->rows = static_cast<uint32_t>(rows);
    for (uint32_t i = t->cols; i < kAv1EncMaxTileCols; ++i) t->col_width_sb[i] = 0;
    for (uint32_t i = t->rows; i < kAv1EncMaxTileRows; ++i) t->row_height_sb[i] = 0;
  } else {
    if (t->cols == 0 || t->rows == 0 || t->cols > kAv1EncMaxTileCols || t->rows > kAv1EncMaxTileRows) return false;
    int sum = 0, widest = 0;
    for (uint32_t i = 0; i < t->cols; ++i) {
      const int wsb = static_cast<int>(t->col_width_sb[i]);
      if (wsb < 1 || wsb > g.max_tile_width_sb || wsb > g.sbw - sum) return false;
      sum += wsb;
      widest = av1::imax(widest, wsb);
    }
    if (sum != g.sbw) return false;
    const int maxh = g.max_height_sb(widest);
    sum = 0;
    for (uint32_t i = 0; i < t->rows; ++i) {
      const int hsb = static_cast<int>(t->row_height_sb[i]);
      if (hsb < 1 || hsb > maxh || hsb > g.sbh - sum) return false;
      sum += hsb;
    }
    if (sum != g.sbh) return false;
    // (the parser's minimum number of tiles: an explicit layout must have at least that many)
    const int lc = av1::tile_log2(1, static_cast<int>(t->cols)), lr = av1::tile_log2(1, static_cast<int>(t->rows));
    if (lc < g.min_log2_cols || lc + lr < g.min_log2_tiles) return false;
    t->cols_log2 = static_cast<uint32_t>(lc);
    t->rows_log2 = static_cast<uint32_t>(lr);
  }
  if (t->context_update_tile_id >= t->cols * t->rows) return false;
  return true;
}

Av1EncResult Av1PictureEncoder::encode(const Av1EncPicture& picture, const EncPlanes16& source, const EncWritablePlanes16& reconstruction, ByteBuffer* out,
                                       List<Av1EncTile>* tiles, Av1EncPostEncode* post, EncStatistics* statistics) {
  if (!out || !tiles || !post) return Av1EncResult::PictureControlNotSupported;
  if (!core_) {
    core_ = UniquePointer<av1::FrameEncoder>::make(allocator_, allocator_);
    if (!core_) return Av1EncResult::NoMemory;
  }
  return core_->encode(picture, source, reconstruction, out, tiles, post, statistics);
}

uint64_t Av1PictureEncoder::last_drift() const { return core_ ? core_->drift() : 0; }

bool Av1PictureEncoder::write_sequence_header(const Av1EncSequence& seq, ByteBuffer* out) {
  if (!out || seq.width == 0 || seq.height == 0 || seq.width > 65536 || seq.height > 65536 || seq.order_hint_bits < 1 || seq.order_hint_bits > 8) return false;
  av1::HeaderBits b(out->allocator());
  av1::write_sequence_payload(seq, b);
  return b.ok() && av1::append_obu(out, av1::OBU_TD, ConstByteView()) && av1::append_obu(out, av1::OBU_SEQ_HDR, b.bytes());
}

bool Av1PictureEncoder::write_frame_obu(const Av1EncPicture& picture, const Av1EncPostEncode& post, ConstByteView tile_data, ArrayView<const Av1EncTile> tiles,
                                        ByteBuffer* out) {
  const uint64_t count = uint64_t{post.tiles.cols} * post.tiles.rows;
  if (!out || tiles.size() != count || count == 0 || picture.seq.order_hint_bits < 1 || picture.seq.order_hint_bits > 8) return false;
  av1::HeaderBits b(out->allocator());
  av1::write_frame_header_payload(picture, post, b);
  b.byte_align();
  if (count > 1) b.bit(0);   // tile_start_and_end_present_flag
  b.byte_align();
  if (!b.ok()) return false;
  ByteBuffer payload(out->allocator());
  if (!payload.append(b.bytes())) return false;
  for (uint64_t i = 0; i < count; ++i) {
    ConstByteView tile;
    if (!tile_data.try_subview(tiles[i].offset, tiles[i].size, &tile) || tile.empty()) return false;
    if (i + 1 < count && !payload.put32(static_cast<uint32_t>(tile.size() - 1))) return false;
    if (!payload.append(tile)) return false;
  }
  return av1::append_obu(out, av1::OBU_FRAME, payload.view());
}

}  // namespace hardware
