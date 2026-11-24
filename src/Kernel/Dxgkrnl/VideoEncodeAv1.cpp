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

// The AV1 EncodeFrame (VideoEncodeCodecs.hpp: the arguments and the metadata; Av1PictureEncoder.hpp: what is coded) and the resolve of its metadata.
#include "Hardware/VideoCodec/Software/Av1PictureEncoder.hpp"
#include "Kernel/Dxgkrnl/VideoEncodeCodecs.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {
namespace av1 {
#include "Hardware/VideoCodec/Software/Av1Tables.inc"
}  // namespace av1
}  // namespace hardware

namespace dxgk {

namespace {

// D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAGS the encoder takes (Av1PictureEncoder.hpp says what the headers then are).
constexpr uint32_t kAv1IntraEdgeFilter = 0x4, kAv1Cdef = 0x1000, kAv1OrderHint = 0x8000, kAv1LoopFilterDeltas = 0x40000, kAv1QuantizationDeltas = 0x80000,
                   kAv1ReducedTxSet = 0x200000, kAv1HighPrecisionMv = 0x800000;
constexpr uint32_t kAv1Features = kAv1IntraEdgeFilter | kAv1Cdef | kAv1OrderHint | kAv1LoopFilterDeltas | kAv1QuantizationDeltas | kAv1ReducedTxSet | kAv1HighPrecisionMv;
// D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAGS it takes: error resilient mode, CDF updates off, frame end CDF update off, the reduced transform set, high precision vectors.
constexpr uint32_t kPicErrorResilient = 0x1, kPicDisableCdfUpdate = 0x2, kPicDisableFrameEndUpdateCdf = 0x100, kPicReducedTxSet = 0x1000, kPicHighPrecisionMv = 0x4000;
constexpr uint32_t kAv1PictureFlags = kPicErrorResilient | kPicDisableCdfUpdate | kPicDisableFrameEndUpdateCdf | kPicReducedTxSet | kPicHighPrecisionMv;
// D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE
constexpr uint32_t kLayoutFullFrame = 0, kLayoutUniformGrid = 5, kLayoutConfigurableGrid = 6;
constexpr uint32_t kNoTexture = 0xFFFFFFFFu;

// The rate controller works in H.264's QP scale (a quantiser step doubling every 6); AV1's quantizer indices are put on it by their 8-bit AC quantizer
// (dav1d's dq_tbl): 16 x the H.264 step at QP p is about kStep16[p % 6] << (p / 6), and AV1's step at index q is ac(q) / 8, which is the H.264 step at about the
// QP whose 16 x step is 2 x ac(q).
constexpr uint32_t kStep16[6] = {10, 11, 13, 14, 16, 18};
uint32_t step16(uint32_t qp) { return kStep16[qp % 6] << (qp / 6); }
uint32_t qp_of_qindex(uint32_t q) {
  const uint32_t target = 2u * hardware::av1::k_dq_tbl[0][q > 255 ? 255 : q][1];
  uint32_t best = 0;
  for (uint32_t qp = 1; qp <= 51; ++qp) {
    const uint32_t a = step16(qp), b = step16(best);
    if ((a > target ? a - target : target - a) < (b > target ? b - target : target - b)) best = qp;
  }
  return best;
}
uint32_t qindex_of_qp(uint32_t qp) {
  // (the smallest index on that QP, or the first above it)
  for (uint32_t q = 1; q <= 255; ++q)
    if (qp_of_qindex(q) >= qp) return q;
  return 255;
}

struct Slot {
  uint32_t texture = kNoTexture, order_hint = 0, frame_type = 0, picture_index = 0;
};

void write_tiles(ByteWriter& w, const hardware::Av1EncTiles& t) {
  w.le64(t.rows);
  w.le64(t.cols);
  for (uint32_t i = 0; i < 64; ++i) w.le64(i < t.rows ? t.row_height_sb[i] : 0);
  for (uint32_t i = 0; i < 64; ++i) w.le64(i < t.cols ? t.col_width_sb[i] : 0);
  w.le64(t.context_update_tile_id);
}

// What the arguments said of the loop filter, its deltas, the quantizer and CDEF, as D3D12 has them (copied into the post-encode values).
struct HeaderValues {
  uint32_t lf[18] = {};      // levels [0], [1], U, V, sharpness, DeltaEnabled, UpdateRefDelta, RefDeltas[8], UpdateModeDelta, ModeDeltas[2]
  uint32_t lf_delta[3] = {};
  uint32_t quant[10] = {};
  uint32_t quant_delta[2] = {};
  uint32_t cdef[34] = {};
};
int64_t s64(uint32_t v) { return static_cast<int32_t>(v); }

void write_post_encode(ByteWriter& w, const HeaderValues& h, const hardware::Av1EncPostEncode& post) {
  w.le64(0);   // CompoundPredictionType: single reference
  // LoopFilter
  for (int i = 0; i < 7; ++i) w.le64(h.lf[i]);
  for (int i = 7; i < 15; ++i) w.le64(static_cast<uint64_t>(s64(h.lf[i])));
  w.le64(h.lf[15]);
  for (int i = 16; i < 18; ++i) w.le64(static_cast<uint64_t>(s64(h.lf[i])));
  // LoopFilterDelta
  for (uint32_t v : h.lf_delta) w.le64(v);
  // Quantization: the index coded, the deltas as given
  w.le64(post.base_q_idx);
  for (int i = 1; i < 6; ++i) w.le64(static_cast<uint64_t>(s64(h.quant[i])));
  for (int i = 6; i < 10; ++i) w.le64(h.quant[i]);
  // QuantizationDelta
  w.le64(post.delta_q_present ? 1 : 0);
  w.le64(post.delta_q_res);
  // CDEF
  for (uint32_t v : h.cdef) w.le64(v);
  // SegmentationConfig: none
  w.zeros(608);
  w.le64(post.primary_ref_frame);
  for (uint8_t r : post.ref_frame_idx) w.le64(r);
}

void write_metadata(ByteView metadata, uint64_t errors, uint64_t written, const hardware::EncStatistics& stats, ArrayView<const hardware::Av1EncTile> tiles,
                    const HeaderValues& h, const hardware::Av1EncPostEncode& post) {
  ByteWriter w(metadata);
  w.le32(kVideoEncodeMetadataMagic);
  w.le32(kVideoEncodeMetadataVersionAv1);
  write_output_metadata(w, errors, written, stats, tiles.size());
  if (errors) {
    w.zeros(kVideoEncodeAv1TilesBytes + kVideoEncodeAv1PostEncodeBytes);
    return;
  }
  for (const hardware::Av1EncTile& t : tiles) {
    w.le64(t.size);
    w.le64(0);   // bStartOffset: the tiles follow each other
    w.le64(0);   // bHeaderSize: the program writes the tile group headers
  }
  write_tiles(w, post.tiles);
  write_post_encode(w, h, post);
}

hardware::Av1EncPrecision precision_of(uint32_t mode) {
  return mode == 1 ? hardware::Av1EncPrecision::Full : mode == 2 ? hardware::Av1EncPrecision::Half : hardware::Av1EncPrecision::Quarter;
}

}  // namespace

bool run_video_encode_av1(allocators::IAllocator& allocator, const VideoEncodeJob& job) {
  List<hardware::Av1EncTile> tiles(allocator);
  List<int16_t> qp_map(allocator);
  hardware::EncStatistics stats;
  hardware::Av1EncPostEncode post;
  HeaderValues h;
  uint64_t written = 0;
  auto finish = [&](uint64_t errors) {
    const uint64_t need = kVideoEncodeMetadataHeader + (errors ? 0 : tiles.count() * kVideoEncodeSubregionBytes) + kVideoEncodeAv1TilesBytes + kVideoEncodeAv1PostEncodeBytes;
    if (job.metadata.size() >= need) write_metadata(job.metadata, errors, errors ? 0 : written, stats, tiles.view(), h, post);
    return true;
  };
  ByteReader r(job.params);
  uint32_t w[kVideoEncodeParamWords];
  for (uint32_t& v : w) v = r.le32();
  if (!r.ok() || w[0] != kVideoEncodeCodecAv1) return finish(kEncodeErrorPictureControl);
  if (w[46]) return finish(w[46] & (kEncodeErrorPictureControl | kEncodeErrorSubregionLayout | kEncodeErrorReferences | kEncodeErrorReconfiguration));
  uint32_t ref_idx[7];
  for (uint32_t& v : ref_idx) v = r.le32();
  Slot slots[8];
  for (Slot& s : slots) s = {r.le32(), r.le32(), r.le32(), r.le32()};
  uint32_t restoration[6];
  for (uint32_t& v : restoration) v = r.le32();
  for (uint32_t& v : h.lf) v = r.le32();
  for (uint32_t& v : h.lf_delta) v = r.le32();
  for (uint32_t& v : h.quant) v = r.le32();
  for (uint32_t& v : h.quant_delta) v = r.le32();
  for (uint32_t& v : h.cdef) v = r.le32();
  const uint32_t mode = w[7], rows = w[8], cols = w[9], nqp = w[45], max_tiles = w[47], depth = w[6];
  if (rows == 0 || cols == 0 || rows > hardware::kAv1EncMaxTileRows || cols > hardware::kAv1EncMaxTileCols || nqp > (1u << 20)) return finish(kEncodeErrorSubregionLayout);
  uint32_t heights[64] = {}, widths[64] = {};
  if (mode == kLayoutConfigurableGrid) {
    for (uint32_t i = 0; i < rows; ++i) heights[i] = r.le32();
    for (uint32_t i = 0; i < cols; ++i) widths[i] = r.le32();
  }
  const ConstByteView qp_bytes = r.take(uint64_t{nqp} * 2);
  r.skip(nqp % 2 ? 2 : 0);
  if (!r.ok() || !r.at_end()) return finish(kEncodeErrorPictureControl);

  // the sequence: Main, 8 or 10 bits, the features the encoder has
  hardware::Av1EncPicture pic;
  hardware::Av1EncSequence& seq = pic.seq;
  const uint32_t features = w[4];
  if (w[1] != 0 || (depth != 8 && depth != 10) || (features & ~kAv1Features) || w[5] > 7) return finish(kEncodeErrorPictureControl);
  seq.width = w[2];
  seq.height = w[3];
  seq.bit_depth = static_cast<uint8_t>(depth);
  seq.enable_order_hint = (features & kAv1OrderHint) != 0;
  seq.order_hint_bits = static_cast<uint8_t>(w[5] + 1);
  seq.enable_cdef = (features & kAv1Cdef) != 0;
  seq.enable_intra_edge_filter = (features & kAv1IntraEdgeFilter) != 0;
  if (seq.width < 16 || seq.height < 16 || seq.width > 8192 || seq.height > 8192 || seq.width % 2 || seq.height % 2 || !planes_fit(job.input, seq.width, seq.height, depth))
    return finish(kEncodeErrorPictureControl);
  if (job.has_reconstruction && !planes_fit(job.reconstruction_luma, job.reconstruction_chroma, seq.width, seq.height, depth)) return finish(kEncodeErrorReferences);

  // the frame: what the picture control asks for, within what the configuration enabled
  const uint32_t flags = w[35];
  if (w[34] > 3 || (flags & ~kAv1PictureFlags) || w[36] != 0 || w[37] > 4 || w[38] == 0 || w[38] > 2 || (w[42] & 0xFFFF) > 1 || (w[42] >> 16) > 1 || w[44] > 0xFF)
    return finish(kEncodeErrorPictureControl);
  if (((flags & kPicReducedTxSet) && !(features & kAv1ReducedTxSet)) || ((flags & kPicHighPrecisionMv) && !(features & kAv1HighPrecisionMv)))
    return finish(kEncodeErrorPictureControl);
  if (restoration[0] || restoration[1] || restoration[2]) return finish(kEncodeErrorPictureControl);   // (loop restoration is not done)
  pic.type = static_cast<hardware::Av1EncFrameType>(w[34]);
  pic.error_resilient = (flags & kPicErrorResilient) != 0;
  pic.disable_cdf_update = (flags & kPicDisableCdfUpdate) != 0;
  pic.disable_frame_end_update_cdf = (flags & kPicDisableFrameEndUpdateCdf) != 0;
  pic.reduced_tx_set = (flags & kPicReducedTxSet) != 0;
  pic.allow_high_precision_mv = (flags & kPicHighPrecisionMv) != 0;
  pic.filter = static_cast<hardware::Av1EncFilter>(w[37]);
  pic.tx_mode = static_cast<hardware::Av1EncTxMode>(w[38]);
  pic.order_hint = w[40];
  pic.refresh_frame_flags = static_cast<uint8_t>(w[44]);
  for (int i = 0; i < 7; ++i) {
    if (ref_idx[i] > 7) return finish(kEncodeErrorReferences);
    pic.ref_frame_idx[i] = static_cast<uint8_t>(ref_idx[i]);
  }
  // the loop filter, its deltas (present only where the configuration has them; delta LF is not done) and the quantizer's deltas
  hardware::Av1EncLoopFilter& lf = pic.loop_filter;
  for (int i = 0; i < 4; ++i) lf.level[i] = static_cast<uint8_t>(h.lf[i] > 63 ? 0xFF : h.lf[i]);
  lf.sharpness = static_cast<uint8_t>(h.lf[4] > 7 ? 0xFF : h.lf[4]);
  lf.delta_enabled = h.lf[5] != 0;
  lf.delta_update = h.lf[6] != 0 || h.lf[15] != 0;
  for (int i = 0; i < 8; ++i) {
    const int64_t d = s64(h.lf[7 + i]);
    if (d < -63 || d > 63) return finish(kEncodeErrorPictureControl);
    lf.ref_deltas[i] = static_cast<int8_t>(d);
  }
  for (int i = 0; i < 2; ++i) {
    const int64_t d = s64(h.lf[16 + i]);
    if (d < -63 || d > 63) return finish(kEncodeErrorPictureControl);
    lf.mode_deltas[i] = static_cast<int8_t>(d);
  }
  if (lf.level[0] > 63 || lf.level[1] > 63 || lf.level[2] > 63 || lf.level[3] > 63 || lf.sharpness > 7 || (lf.delta_enabled && !(features & kAv1LoopFilterDeltas)) ||
      h.lf_delta[0] != 0)
    return finish(kEncodeErrorPictureControl);
  int8_t* const deltas[5] = {&pic.delta_q_y_dc, &pic.delta_q_u_dc, &pic.delta_q_u_ac, &pic.delta_q_v_dc, &pic.delta_q_v_ac};
  for (int i = 0; i < 5; ++i) {
    const int64_t d = s64(h.quant[1 + i]);
    if (d < -64 || d > 63 || (d != 0 && !(features & kAv1QuantizationDeltas))) return finish(kEncodeErrorPictureControl);
    *deltas[i] = static_cast<int8_t>(d);
  }
  // (separate_uv_delta_q is 0 in the sequence header: V's deltas are U's; quantizer matrices are not done)
  if (pic.delta_q_v_dc != pic.delta_q_u_dc || pic.delta_q_v_ac != pic.delta_q_u_ac || h.quant[6] != 0) return finish(kEncodeErrorPictureControl);
  if (seq.enable_cdef) {
    hardware::Av1EncCdef& c = pic.cdef;
    if (h.cdef[0] > 3 || h.cdef[1] > 3) return finish(kEncodeErrorPictureControl);
    c.bits = static_cast<uint8_t>(h.cdef[0]);
    c.damping_minus3 = static_cast<uint8_t>(h.cdef[1]);
    for (int i = 0; i < 8; ++i) {
      if (h.cdef[2 + i] > 15 || h.cdef[10 + i] > 15 || h.cdef[18 + i] > 3 || h.cdef[26 + i] > 3) return finish(kEncodeErrorPictureControl);
      c.y_pri[i] = static_cast<uint8_t>(h.cdef[2 + i]);
      c.uv_pri[i] = static_cast<uint8_t>(h.cdef[10 + i]);
      c.y_sec[i] = static_cast<uint8_t>(h.cdef[18 + i]);
      c.uv_sec[i] = static_cast<uint8_t>(h.cdef[26 + i]);
    }
  } else {
    for (uint32_t& v : h.cdef) v = 0;   // (no CDEF: the post-encode values say none)
  }
  pic.precision = precision_of(w[33]);

  // the tiles
  hardware::Av1EncTiles& t = pic.tiles;
  if (mode == kLayoutFullFrame) {
    t.uniform = true;
    t.cols = t.rows = 1;
  } else if (mode == kLayoutUniformGrid) {
    t.uniform = true;
    t.cols = cols;
    t.rows = rows;
  } else if (mode == kLayoutConfigurableGrid) {
    t.uniform = false;
    t.cols = cols;
    t.rows = rows;
    for (uint32_t i = 0; i < rows; ++i) t.row_height_sb[i] = heights[i];
    for (uint32_t i = 0; i < cols; ++i) t.col_width_sb[i] = widths[i];
  } else {
    return finish(kEncodeErrorSubregionLayout);
  }
  t.context_update_tile_id = w[31];
  if (!hardware::Av1PictureEncoder::tile_layout(seq.width, seq.height, &t) || t.context_update_tile_id >= t.cols * t.rows || t.cols * t.rows > max_tiles ||
      job.metadata.size() < kVideoEncodeMetadataHeader + uint64_t{t.cols * t.rows} * kVideoEncodeSubregionBytes + kVideoEncodeAv1TilesBytes + kVideoEncodeAv1PostEncodeBytes)
    return finish(kEncodeErrorSubregionLayout);

  // rate control, on H.264's QP scale (the configuration's indices put on it)
  hardware::RateControlConfig rc;
  if (!rate_control_from_words(ArrayView<const uint32_t>(w), &rc) || (rc.flags & ~0x3Du)) return finish(kEncodeErrorPictureControl);
  const uint32_t cqp_index[3] = {rc.qp_intra, rc.qp_p, rc.qp_b};
  if (rc.qp_intra > 255 || rc.qp_p > 255 || rc.qp_b > 255 || rc.initial_qp > 255 || rc.min_qp > 255 || rc.max_qp > 255 || rc.quality_target > 255) return finish(kEncodeErrorPictureControl);
  const uint32_t min_index = (rc.flags & hardware::kRateControlQpRange) ? (rc.min_qp ? rc.min_qp : 1) : 1;
  const uint32_t max_index = (rc.flags & hardware::kRateControlQpRange) ? rc.max_qp : 255;
  rc.qp_intra = qp_of_qindex(rc.qp_intra);
  rc.qp_p = qp_of_qindex(rc.qp_p);
  rc.qp_b = qp_of_qindex(rc.qp_b);
  rc.initial_qp = qp_of_qindex(rc.initial_qp);
  rc.min_qp = qp_of_qindex(rc.min_qp);
  rc.max_qp = qp_of_qindex(rc.max_qp);
  if (rc.quality_target) rc.quality_target = qp_of_qindex(rc.quality_target) ? qp_of_qindex(rc.quality_target) : 1;
  const bool intra = pic.type == hardware::Av1EncFrameType::Key || pic.type == hardware::Av1EncFrameType::IntraOnly;
  if (!hardware::rate_control_valid(rc) || min_index > max_index || (rc.mode == hardware::RateControlMode::Cqp && cqp_index[intra ? 0 : 1] == 0))
    return finish(kEncodeErrorPictureControl);   // (index 0 is lossless, which is not done)
  const uint64_t sb_cols = (seq.width + 63) / 64, sb_rows = (seq.height + 63) / 64;
  const bool absolute = rc.mode == hardware::RateControlMode::AbsoluteQpMap;
  if (absolute || (rc.flags & hardware::kRateControlDeltaQp)) {
    if (nqp != sb_cols * sb_rows) return finish(kEncodeErrorPictureControl);
    ByteReader q(qp_bytes);
    for (uint32_t i = 0; i < nqp; ++i)
      if (!qp_map.push_back(static_cast<int16_t>(q.le16()))) return false;
    pic.qp_map = qp_map.view();
    pic.qp_map_absolute = absolute;
  }

  // the pictures: the source, and the slots' textures (each once)
  Picture16 source(allocator), recon(allocator);
  if (!source.make(seq.width, seq.height) || !recon.make(seq.width, seq.height)) return false;
  source.read(job.input, depth);
  UniquePointer<Picture16> textures[8];
  uint32_t texture_index[8];
  uint32_t ntextures = 0;
  for (int i = 0; i < 8; ++i) {
    const Slot& s = slots[i];
    if (s.texture == kNoTexture) continue;
    if (s.texture >= job.references.size() || s.frame_type > 3) return finish(kEncodeErrorReferences);
    if (!planes_fit(job.references[s.texture], seq.width, seq.height, depth)) {
      if (intra) continue;   // (a slot of another size: a frame that refers to nothing does not care)
      return finish(kEncodeErrorReferences);
    }
    uint32_t k = 0;
    while (k < ntextures && texture_index[k] != s.texture) ++k;
    if (k == ntextures) {
      textures[k] = UniquePointer<Picture16>::make(allocator, allocator);
      if (!textures[k] || !textures[k]->make(seq.width, seq.height)) return false;
      textures[k]->read(job.references[s.texture], depth);
      texture_index[k] = s.texture;
      ++ntextures;
    }
    pic.slots[i].valid = true;
    pic.slots[i].picture = textures[k]->planes();
    pic.slots[i].order_hint = s.order_hint;
    pic.slots[i].frame_type = static_cast<hardware::Av1EncFrameType>(s.frame_type);
  }
  if (!intra)
    for (uint8_t idx : pic.ref_frame_idx)
      if (!pic.slots[idx].valid) return finish(kEncodeErrorReferences);

  // rate control: the index, coded again while the frame is too big (for the controller, or for the bitstream buffer)
  hardware::RateControlState state;
  if (!job.heap.empty()) load_rate_control_state(job.heap, &state);
  const hardware::RateControlPicture kind = intra ? hardware::RateControlPicture::Intra : hardware::RateControlPicture::P;
  uint32_t qp = hardware::rate_control_qp(rc, state, kind, uint64_t{seq.width} * seq.height);
  auto index_of = [&](uint32_t attempt, uint32_t at) {
    uint32_t q = rc.mode == hardware::RateControlMode::Cqp && attempt == 0 ? cqp_index[intra ? 0 : 1] : qindex_of_qp(at);
    if (q < min_index) q = min_index;
    if (q > max_index) q = max_index;
    return q;
  };
  UniquePointer<hardware::Av1PictureEncoder> encoder = UniquePointer<hardware::Av1PictureEncoder>::make(allocator, allocator);
  if (!encoder) return false;
  ByteBuffer out(allocator);
  uint32_t index = 0;
  for (uint32_t attempt = 0;; ++attempt) {
    out.clear();
    stats = hardware::EncStatistics();
    index = index_of(attempt, qp);
    pic.base_q_idx = static_cast<uint8_t>(index);
    switch (encoder->encode(pic, source.planes(), recon.writable(), &out, &tiles, &post, &stats)) {
      case hardware::Av1EncResult::Ok: break;
      case hardware::Av1EncResult::NoMemory: return false;
      case hardware::Av1EncResult::SubregionLayoutNotSupported: tiles.clear(); return finish(kEncodeErrorSubregionLayout);
      case hardware::Av1EncResult::InvalidReferences: tiles.clear(); return finish(kEncodeErrorReferences);
      default: tiles.clear(); return finish(kEncodeErrorPictureControl);
    }
    const bool fits = out.size() <= job.bitstream.size();
    uint32_t retry = qp;
    const bool accepted = hardware::rate_control_accept(rc, state, kind, qp, out.size() * 8, &retry);
    if (fits && (accepted || attempt >= hardware::kRateControlMaxRetries)) break;
    if (!fits) retry = retry > qp ? retry : qp + 4;
    if (index >= max_index || attempt >= 2 * hardware::kRateControlMaxRetries) {
      if (!fits) { tiles.clear(); return finish(kEncodeErrorPictureControl); }   // (not even the largest index fits the program's bitstream buffer)
      break;
    }
    qp = retry > 51 ? 51 : retry;
    if (qp <= qp_of_qindex(index)) qp = qp_of_qindex(index) + 1;   // (always coarser than the attempt before)
  }
  hardware::rate_control_update(rc, state, kind, qp_of_qindex(index), out.size() * 8);
  if (!job.heap.empty()) store_rate_control_state(job.heap, state);
  job.bitstream.subview(0, out.size()).copy_from(out.view());
  if (job.has_reconstruction) recon.write(job.reconstruction_luma, job.reconstruction_chroma, depth);
  written = out.size();
  return finish(0);
}

bool resolve_av1_encode_metadata(ConstByteView metadata, ByteView out) {
  if (metadata.size() < kVideoEncodeMetadataHeader) return false;
  const uint64_t count = ByteReader(metadata.subview(8 + 64, 8)).le64();
  const uint64_t tail = kVideoEncodeAv1TilesBytes + kVideoEncodeAv1PostEncodeBytes;
  if (metadata.size() < kVideoEncodeMetadataHeader + tail || count > (metadata.size() - kVideoEncodeMetadataHeader - tail) / kVideoEncodeSubregionBytes) return false;
  const uint64_t bytes = kVideoEncodeOutputMetadataBytes + count * kVideoEncodeSubregionBytes + tail;
  if (out.size() < bytes) return false;
  out.subview(0, bytes).copy_from(metadata.subview(8, bytes));
  return true;
}

}  // namespace dxgk
