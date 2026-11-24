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

// The HEVC EncodeFrame (VideoEncodeCodecs.hpp: the arguments; H265PictureEncoder.hpp: what is coded).
#include "Hardware/VideoCodec/Software/H265PictureEncoder.hpp"
#include "Kernel/Dxgkrnl/VideoEncodeCodecs.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/UniquePointer.hpp"

namespace dxgk {

namespace {

// D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAGS the encoder takes: loop filter across slices off, SAO, long-term references, AMP (allowed; 2Nx2N is what it codes),
// transform skip (allowed; never chosen), constrained intra prediction. ALLOW_REQUEST_INTRA_CONSTRAINED_SLICES (0x2) is not done.
constexpr uint32_t kHevcDisableLoopFilterAcrossSlices = 0x1, kHevcSao = 0x4, kHevcLongTerm = 0x8, kHevcAmp = 0x10, kHevcTransformSkip = 0x20, kHevcConstrainedIntra = 0x40;
constexpr uint32_t kHevcConfigFlags = kHevcDisableLoopFilterAcrossSlices | kHevcSao | kHevcLongTerm | kHevcAmp | kHevcTransformSkip | kHevcConstrainedIntra;

struct Descriptor {
  uint32_t texture = 0, used = 0, long_term = 0, poc = 0, temporal = 0;
};

void write_metadata(ByteView metadata, uint64_t errors, uint64_t written, const hardware::EncStatistics& stats, ArrayView<const hardware::H265EncSlice> slices) {
  ByteWriter w(metadata);
  w.le32(kVideoEncodeMetadataMagic);
  w.le32(1);
  write_output_metadata(w, errors, written, stats, slices.size());
  if (errors) return;
  for (const hardware::H265EncSlice& s : slices) {
    w.le64(s.size);
    w.le64(0);   // bStartOffset: no padding before a slice
    w.le64(s.header_bits);
  }
}

hardware::H265EncPrecision precision_of(uint32_t mode) {
  // (D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE: maximum, quarter and eighth are quarter samples, the finest the encoder searches)
  return mode == 1 ? hardware::H265EncPrecision::Full : mode == 2 ? hardware::H265EncPrecision::Half : hardware::H265EncPrecision::Quarter;
}

}  // namespace

bool run_video_encode_hevc(allocators::IAllocator& allocator, const VideoEncodeJob& job) {
  List<hardware::H265EncSlice> slices(allocator);
  List<int8_t> qp_map(allocator);
  hardware::EncStatistics stats;
  uint64_t written = 0;
  auto finish = [&](uint64_t errors) {
    const uint64_t need = kVideoEncodeMetadataHeader + (errors ? 0 : slices.count() * kVideoEncodeSubregionBytes);
    if (job.metadata.size() >= need) write_metadata(job.metadata, errors, errors ? 0 : written, stats, slices.view());
    return true;
  };
  ByteReader r(job.params);
  uint32_t w[kVideoEncodeParamWords];
  for (uint32_t& v : w) v = r.le32();
  if (!r.ok() || w[0] != kVideoEncodeCodecHevc) return finish(kEncodeErrorPictureControl);
  if (w[46]) return finish(w[46] & (kEncodeErrorPictureControl | kEncodeErrorSubregionLayout | kEncodeErrorReferences | kEncodeErrorReconfiguration));
  const uint32_t ndesc = w[40], n0 = w[41], n1 = w[42], nmod0 = w[43], nmod1 = w[44], nqp = w[45], max_subregions = w[47], depth = w[48];
  if (ndesc > hardware::kH265EncMaxReferences || n0 > 15 || n1 > 15 || nmod0 > 15 || nmod1 > 15 || nqp > (1u << 20)) return finish(kEncodeErrorPictureControl);
  Descriptor desc[hardware::kH265EncMaxReferences];
  for (uint32_t i = 0; i < ndesc; ++i) desc[i] = {r.le32(), r.le32(), r.le32(), r.le32(), r.le32()};
  uint8_t lists[2][15];
  uint32_t list_word[2][15];   // (descriptor indices, checked below)
  for (uint32_t i = 0; i < n0; ++i) list_word[0][i] = r.le32();
  for (uint32_t i = 0; i < n1; ++i) list_word[1][i] = r.le32();
  uint32_t mods[2][15];
  for (uint32_t i = 0; i < nmod0; ++i) mods[0][i] = r.le32();
  for (uint32_t i = 0; i < nmod1; ++i) mods[1][i] = r.le32();
  const ConstByteView qp_bytes = r.take(nqp);
  r.skip((4 - nqp % 4) % 4);
  if (!r.ok() || !r.at_end()) return finish(kEncodeErrorPictureControl);
  for (int l = 0; l < 2; ++l)
    for (uint32_t i = 0; i < (l == 0 ? n0 : n1); ++i) {
      if (list_word[l][i] >= ndesc) return finish(kEncodeErrorReferences);
      lists[l][i] = static_cast<uint8_t>(list_word[l][i]);
    }

  // the sequence: Main (8 bits) or Main 10 (10 bits), the configuration's sizes
  hardware::H265EncSequence seq;
  if (w[1] > 1 || depth != (w[1] == 1 ? 10u : 8u) || (w[4] & ~kHevcConfigFlags) || w[5] > 3 || w[6] > 3 || w[7] > 3 || w[8] > 3 || w[39] > 12)
    return finish(kEncodeErrorPictureControl);
  seq.width = w[2];
  seq.height = w[3];
  seq.bit_depth = static_cast<uint8_t>(depth);
  seq.log2_min_cb = static_cast<uint8_t>(w[5] + 3);
  seq.log2_ctb = static_cast<uint8_t>(w[6] + 3);
  seq.log2_min_tb = static_cast<uint8_t>(w[7] + 2);
  seq.log2_max_tb = static_cast<uint8_t>(w[8] + 2);
  seq.max_th_depth_inter = static_cast<uint8_t>(w[9] & 0xFF);
  seq.max_th_depth_intra = static_cast<uint8_t>((w[9] >> 8) & 0xFF);
  seq.log2_max_poc_lsb = static_cast<uint8_t>(w[39] + 4);
  seq.loop_filter_across_slices = (w[4] & kHevcDisableLoopFilterAcrossSlices) == 0;
  seq.sao = (w[4] & kHevcSao) != 0;
  seq.long_term_refs = (w[4] & kHevcLongTerm) != 0;
  seq.amp = (w[4] & kHevcAmp) != 0;
  seq.transform_skip = (w[4] & kHevcTransformSkip) != 0;
  seq.constrained_intra_pred = (w[4] & kHevcConstrainedIntra) != 0;
  if (!hardware::H265PictureEncoder::sequence_valid(seq) || !planes_fit(job.input, seq.width, seq.height, depth)) return finish(kEncodeErrorPictureControl);
  if (job.has_reconstruction && !planes_fit(job.reconstruction_luma, job.reconstruction_chroma, seq.width, seq.height, depth)) return finish(kEncodeErrorReferences);
  hardware::RateControlConfig rc;
  // (frame analysis - a second pass - is not done: the support queries do not offer it)
  if (!rate_control_from_words(ArrayView<const uint32_t>(w), &rc) || !hardware::rate_control_valid(rc) || (rc.flags & ~0x3Du)) return finish(kEncodeErrorPictureControl);

  // the picture
  hardware::H265EncPicture pic;
  pic.seq = seq;
  if (w[34] > 3 || (w[38] & 2) || w[35] > 63 || w[37] > 6) return finish(kEncodeErrorPictureControl);
  pic.type = static_cast<hardware::H265EncFrameType>(w[34]);
  pic.pps_id = w[35];
  pic.poc = static_cast<int32_t>(w[36]);
  pic.temporal_id = static_cast<uint8_t>(w[37]);
  pic.reference = (w[38] & 1) != 0;
  if (w[31] > 4) return finish(kEncodeErrorSubregionLayout);
  pic.slice_mode = static_cast<hardware::H265EncSliceMode>(w[31]);
  pic.slice_value = w[32];
  pic.precision = precision_of(w[33]);
  const uint32_t ctb = 1u << seq.log2_ctb, ctb_w = (seq.width + ctb - 1) / ctb, ctb_h = (seq.height + ctb - 1) / ctb;
  const uint32_t slice_count = hardware::H265PictureEncoder::slice_count(pic.slice_mode, pic.slice_value, ctb_w, ctb_h);
  if (slice_count == 0 || slice_count > max_subregions || job.metadata.size() < kVideoEncodeMetadataHeader + uint64_t{slice_count} * kVideoEncodeSubregionBytes)
    return finish(kEncodeErrorSubregionLayout);
  const bool absolute = rc.mode == hardware::RateControlMode::AbsoluteQpMap;
  if (absolute || (rc.flags & hardware::kRateControlDeltaQp)) {
    if (nqp != uint64_t{ctb_w} * ctb_h) return finish(kEncodeErrorPictureControl);
    for (uint8_t b : qp_bytes)
      if (!qp_map.push_back(static_cast<int8_t>(b))) return false;
    pic.qp_map = qp_map.view();
    pic.qp_map_absolute = absolute;
  }
  pic.modification[0] = ArrayView<const uint32_t>(mods[0], nmod0);
  pic.modification[1] = ArrayView<const uint32_t>(mods[1], nmod1);
  pic.list[0] = ArrayView<const uint8_t>(lists[0], n0);
  pic.list[1] = ArrayView<const uint8_t>(lists[1], n1);

  // the pictures: the source, and each reference texture once
  Picture16 source(allocator), recon(allocator);
  if (!source.make(seq.width, seq.height) || !recon.make(seq.width, seq.height)) return false;
  source.read(job.input, depth);
  UniquePointer<Picture16> textures[hardware::kH265EncMaxReferences];
  uint32_t texture_index[hardware::kH265EncMaxReferences];
  uint32_t ntextures = 0;
  hardware::H265EncReference dpb[hardware::kH265EncMaxReferences];
  for (uint32_t i = 0; i < ndesc; ++i) {
    const uint32_t t = desc[i].texture;
    if (t >= job.references.size() || !planes_fit(job.references[t], seq.width, seq.height, depth)) return finish(kEncodeErrorReferences);
    uint32_t k = 0;
    while (k < ntextures && texture_index[k] != t) ++k;
    if (k == ntextures) {
      textures[k] = UniquePointer<Picture16>::make(allocator, allocator);
      if (!textures[k] || !textures[k]->make(seq.width, seq.height)) return false;
      textures[k]->read(job.references[t], depth);
      texture_index[k] = t;
      ++ntextures;
    }
    dpb[i].picture = textures[k]->planes();
    dpb[i].poc = static_cast<int32_t>(desc[i].poc);
    dpb[i].long_term = desc[i].long_term != 0;
    dpb[i].used_by_current = desc[i].used != 0;
  }
  pic.dpb = ArrayView<const hardware::H265EncReference>(dpb, ndesc);

  // rate control: the QP, coded again while the picture is too big (for the controller, or for the bitstream buffer)
  hardware::RateControlState state;
  if (!job.heap.empty()) load_rate_control_state(job.heap, &state);
  const hardware::RateControlPicture kind = pic.type == hardware::H265EncFrameType::B ? hardware::RateControlPicture::B
                                            : pic.type == hardware::H265EncFrameType::P ? hardware::RateControlPicture::P : hardware::RateControlPicture::Intra;
  uint32_t qp = hardware::rate_control_qp(rc, state, kind, uint64_t{seq.width} * seq.height);
  UniquePointer<hardware::H265PictureEncoder> encoder = UniquePointer<hardware::H265PictureEncoder>::make(allocator, allocator);
  if (!encoder) return false;
  ByteBuffer out(allocator);
  for (uint32_t attempt = 0;; ++attempt) {
    out.clear();
    stats = hardware::EncStatistics();
    pic.qp = static_cast<int8_t>(qp > 51 ? 51 : qp);
    switch (encoder->encode(pic, source.planes(), recon.writable(), &out, &slices, &stats)) {
      case hardware::H265EncResult::Ok: break;
      case hardware::H265EncResult::NoMemory: return false;
      case hardware::H265EncResult::SubregionLayoutNotSupported: slices.clear(); return finish(kEncodeErrorSubregionLayout);
      case hardware::H265EncResult::InvalidReferences: slices.clear(); return finish(kEncodeErrorReferences);
      default: slices.clear(); return finish(kEncodeErrorPictureControl);
    }
    const bool fits = out.size() <= job.bitstream.size();
    uint32_t retry = qp;
    const bool accepted = hardware::rate_control_accept(rc, state, kind, qp, out.size() * 8, &retry);
    if (fits && (accepted || attempt >= hardware::kRateControlMaxRetries)) break;
    if (!fits) retry = retry > qp ? retry : qp + 4;
    if (qp >= 51 || attempt >= 2 * hardware::kRateControlMaxRetries) {
      if (!fits) { slices.clear(); return finish(kEncodeErrorPictureControl); }   // (not even QP 51 fits the program's bitstream buffer)
      break;
    }
    qp = retry > 51 ? 51 : retry;
  }
  hardware::rate_control_update(rc, state, kind, qp, out.size() * 8);
  if (!job.heap.empty()) store_rate_control_state(job.heap, state);
  job.bitstream.subview(0, out.size()).copy_from(out.view());
  if (job.has_reconstruction) recon.write(job.reconstruction_luma, job.reconstruction_chroma, depth);
  written = out.size();
  return finish(0);
}

}  // namespace dxgk
