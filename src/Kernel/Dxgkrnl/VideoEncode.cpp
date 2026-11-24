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

#include "Kernel/Dxgkrnl/VideoEncode.hpp"
#include "Hardware/VideoCodec/Software/H264PictureEncoder.hpp"
#include "Hardware/VideoCodec/Software/MotionEstimator.hpp"
#include "Hardware/VideoCodec/Software/VideoRateControl.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"

namespace dxgk {

namespace {

// D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAGS
constexpr uint64_t kErrorPictureControl = 0x1, kErrorSubregionLayout = 0x2, kErrorReferences = 0x4, kErrorMetadataSource = 0x10;
constexpr uint32_t kRateControlStateMagic = 0x43524354u;   // 'TCRC'

bool image_fits(ConstByteView memory, uint64_t pitch, uint32_t image_width, uint32_t image_height, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > image_width || height > image_height || pitch < uint64_t{image_width} * 4) return false;
  return memory.size() >= pitch * (height - 1) + uint64_t{width} * 4;
}
bool plane_fits(const VideoPlaneImage& p, uint32_t w, uint32_t h) { return image_fits(p.memory, p.pitch, p.width, p.height, w, h); }
bool plane_fits(const VideoWritablePlaneImage& p, uint32_t w, uint32_t h) { return image_fits(ConstByteView(p.memory), p.pitch, p.width, p.height, w, h); }

// A picture as the encoder takes it: three planes in one buffer.
struct CompactPicture {
  explicit CompactPicture(allocators::IAllocator& a) : bytes(a) {}
  ByteBuffer bytes;
  uint32_t width = 0, height = 0;
  bool make(uint32_t w, uint32_t h) {
    width = w;
    height = h;
    bytes.clear();
    return bytes.put_zeros(uint64_t{w} * h * 3 / 2);
  }
  uint64_t luma() const { return uint64_t{width} * height; }
  hardware::H264EncPlanes planes() const {
    const ConstByteView v = bytes.view();
    return {v.subview(0, luma()), v.subview(luma(), luma() / 4), v.subview(luma() * 5 / 4, luma() / 4), width, height};
  }
  hardware::H264EncWritablePlanes writable() {
    const ByteView v = bytes.writable_view();
    return {v.subview(0, luma()), v.subview(luma(), luma() / 4), v.subview(luma() * 5 / 4, luma() / 4), width, height};
  }
  // From the widened planes (the top-left width x height).
  void read(const VideoPictureImages& in) {
    ByteView v = bytes.writable_view();
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) v[uint64_t{y} * width + x] = in.luma.memory[in.luma.pitch * y + uint64_t{x} * 4];
    const uint32_t cw = width / 2, ch = height / 2;
    for (uint32_t y = 0; y < ch; ++y)
      for (uint32_t x = 0; x < cw; ++x) {
        const uint64_t at = in.chroma.pitch * y + uint64_t{x} * 4;
        v[luma() + uint64_t{y} * cw + x] = in.chroma.memory[at];
        v[luma() * 5 / 4 + uint64_t{y} * cw + x] = in.chroma.memory[at + 1];
      }
  }
  // Into widened planes: (Y, 0, 0, 255) and (U, V, 0, 255).
  void write(const VideoWritablePlaneImage& luma_image, const VideoWritablePlaneImage& chroma_image) const {
    const ConstByteView v = bytes.view();
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        ByteView px = luma_image.memory.subview(luma_image.pitch * y + uint64_t{x} * 4, 4);
        px[0] = v[uint64_t{y} * width + x];
        px[1] = px[2] = 0;
        px[3] = 0xFF;
      }
    const uint32_t cw = width / 2, ch = height / 2;
    for (uint32_t y = 0; y < ch; ++y)
      for (uint32_t x = 0; x < cw; ++x) {
        ByteView px = chroma_image.memory.subview(chroma_image.pitch * y + uint64_t{x} * 4, 4);
        px[0] = v[luma() + uint64_t{y} * cw + x];
        px[1] = v[luma() * 5 / 4 + uint64_t{y} * cw + x];
        px[2] = 0;
        px[3] = 0xFF;
      }
  }
};

struct Descriptor {
  uint32_t texture = 0, long_term = 0, long_term_index = 0, poc = 0, frame_number = 0, temporal_layer = 0;
};

void load_state(ConstByteView heap, hardware::RateControlState* s) {
  *s = hardware::RateControlState();
  ByteReader r(heap);
  if (r.le32() != kRateControlStateMagic) return;
  hardware::RateControlState t;
  t.pictures = r.le64();
  t.fullness = static_cast<int64_t>(r.le64());
  t.rate_error = static_cast<int64_t>(r.le64());
  t.coded_bits = r.le64();
  for (uint64_t& c : t.complexity) c = r.le64();
  for (uint32_t& q : t.last_qp) q = r.le32();
  t.started = r.le32();
  if (r.ok()) *s = t;
}
void store_state(ByteView heap, const hardware::RateControlState& s) {
  ByteWriter w(heap);
  w.le32(kRateControlStateMagic);
  w.le64(s.pictures);
  w.le64(static_cast<uint64_t>(s.fullness));
  w.le64(static_cast<uint64_t>(s.rate_error));
  w.le64(s.coded_bits);
  for (uint64_t c : s.complexity) w.le64(c);
  for (uint32_t q : s.last_qp) w.le32(q);
  w.le32(s.started);
}

struct Outcome {
  uint64_t errors = 0;
  uint64_t written = 0;
  hardware::H264EncStatistics stats;
  uint64_t macroblocks = 1;
};

void write_metadata(ByteView metadata, const Outcome& o, ArrayView<const hardware::H264EncSlice> slices) {
  ByteWriter w(metadata);
  w.le32(kVideoEncodeMetadataMagic);
  w.le32(1);
  w.le64(o.errors);
  const uint64_t inter = o.stats.inter;
  w.le64(o.errors ? 0 : o.stats.qp_sum / o.macroblocks);   // AverageQP
  w.le64(o.stats.intra);
  w.le64(o.stats.inter);
  w.le64(o.stats.skipped);
  w.le64(inter ? static_cast<uint64_t>(o.stats.vector_x_sum / static_cast<int64_t>(inter)) : 0);
  w.le64(inter ? static_cast<uint64_t>(o.stats.vector_y_sum / static_cast<int64_t>(inter)) : 0);
  w.le64(o.written);
  w.le64(o.errors ? 0 : slices.size());
  if (o.errors) return;
  for (const hardware::H264EncSlice& s : slices) {
    w.le64(s.size);
    w.le64(0);   // bStartOffset: no padding before a slice
    w.le64(s.header_bits);
  }
}

}  // namespace

bool run_video_encode(allocators::IAllocator& allocator, const VideoEncodeJob& job) {
  Outcome o;
  List<hardware::H264EncSlice> slices(allocator);
  auto finish = [&](uint64_t errors) {
    o.errors = errors;
    if (errors) o.written = 0;
    if (job.metadata.size() >= kVideoEncodeMetadataHeader + (errors ? 0 : slices.count() * kVideoEncodeSubregionBytes)) write_metadata(job.metadata, o, slices.view());
    return true;
  };
  ByteReader r(job.params);
  uint32_t w[kVideoEncodeParamWords];
  for (uint32_t& v : w) v = r.le32();
  if (!r.ok()) return finish(kErrorPictureControl);
  if (w[48]) return finish(w[48] & (kErrorPictureControl | kErrorSubregionLayout | kErrorReferences | 0x8));   // (what the runtime found wrong)
  const uint32_t codec = w[0], profile = w[1], width = w[2], height = w[3], config = w[4], direct = w[5], deblock = w[6];
  const uint32_t ndesc = w[41], n0 = w[42], n1 = w[43], nmark = w[44], nmod0 = w[45], nmod1 = w[46], nqp = w[47], max_subregions = w[49];
  if (ndesc > kVideoEncodeMaxReferences || n0 > kVideoEncodeMaxReferences || n1 > kVideoEncodeMaxReferences || nmark > 64 || nmod0 > 64 || nmod1 > 64 || nqp > (1u << 20))
    return finish(kErrorPictureControl);
  Descriptor desc[kVideoEncodeMaxReferences];
  for (uint32_t i = 0; i < ndesc; ++i) desc[i] = {r.le32(), r.le32(), r.le32(), r.le32(), r.le32(), r.le32()};
  uint32_t list_desc[2][kVideoEncodeMaxReferences];
  for (uint32_t i = 0; i < n0; ++i) list_desc[0][i] = r.le32();
  for (uint32_t i = 0; i < n1; ++i) list_desc[1][i] = r.le32();
  List<hardware::H264EncMarking> marking(allocator);
  List<hardware::H264EncListModification> mods[2] = {List<hardware::H264EncListModification>(allocator), List<hardware::H264EncListModification>(allocator)};
  for (uint32_t i = 0; i < nmark; ++i) {
    hardware::H264EncMarking m;
    m.operation = static_cast<uint8_t>(r.le32());
    m.difference_of_pic_nums_minus1 = r.le32();
    m.long_term_pic_num = r.le32();
    m.long_term_frame_idx = r.le32();
    m.max_long_term_frame_idx_plus1 = r.le32();
    if (!marking.push_back(m)) return false;
  }
  for (int l = 0; l < 2; ++l)
    for (uint32_t i = 0; i < (l == 0 ? nmod0 : nmod1); ++i) {
      hardware::H264EncListModification m;
      m.idc = static_cast<uint8_t>(r.le32());
      m.abs_diff_pic_num_minus1 = r.le32();
      m.long_term_pic_num = r.le32();
      if (!mods[l].push_back(m)) return false;
    }
  const ConstByteView qp_bytes = r.take(nqp);
  r.skip((4 - nqp % 4) % 4);
  if (!r.ok() || !r.at_end()) return finish(kErrorPictureControl);

  // the configuration: H.264, Main or High as Main, CAVLC, no 8x8 transform, no direct prediction, filter modes 0 .. 2 (what the encoder's support queries report)
  if (codec != 0 || profile > 1 || (config & ~0x1u) || direct != 0 || deblock > 2 || (w[39] & 2)) return finish(kErrorPictureControl);
  if (width == 0 || height == 0 || width % 16 || height % 16 || width > 4096 || height > 4096 || !plane_fits(job.input.luma, width, height) ||
      !plane_fits(job.input.chroma, width / 2, height / 2))
    return finish(kErrorPictureControl);
  if (job.has_reconstruction && (!plane_fits(job.reconstruction_luma, width, height) || !plane_fits(job.reconstruction_chroma, width / 2, height / 2)))
    return finish(kErrorReferences);
  hardware::RateControlConfig rc;
  rc.mode = static_cast<hardware::RateControlMode>(w[10] > 4 ? 0xFF : w[10]);
  rc.flags = w[11];
  rc.fps_numerator = w[12];
  rc.fps_denominator = w[13];
  rc.qp_intra = w[14];
  rc.qp_p = w[15];
  rc.qp_b = w[16];
  rc.initial_qp = w[17];
  rc.min_qp = w[18];
  rc.max_qp = w[19];
  rc.target_bitrate = w[20] | uint64_t{w[21]} << 32;
  rc.peak_bitrate = w[22] | uint64_t{w[23]} << 32;
  rc.vbv_capacity = w[24] | uint64_t{w[25]} << 32;
  rc.vbv_initial_fullness = w[26] | uint64_t{w[27]} << 32;
  rc.max_frame_bits = w[28] | uint64_t{w[29]} << 32;
  rc.quality_target = w[30];
  // (frame analysis - a second pass - is not done: the support queries do not offer it)
  if (w[10] > 4 || !hardware::rate_control_valid(rc) || (rc.flags & ~0x3Du)) return finish(kErrorPictureControl);

  hardware::H264EncPicture pic;
  pic.mb_width = width / 16;
  pic.mb_height = height / 16;
  pic.constrained_intra_pred = (config & 0x1) != 0;
  pic.disable_deblocking_filter_idc = static_cast<uint8_t>(deblock);
  if (w[7] > 2 || w[8] > 12 || w[9] > 12) return finish(kErrorPictureControl);
  pic.pic_order_cnt_type = static_cast<uint8_t>(w[7]);
  pic.log2_max_frame_num = static_cast<uint8_t>(w[8] + 4);
  pic.log2_max_poc_lsb = static_cast<uint8_t>(w[9] + 4);
  if (w[34] > 3) return finish(kErrorPictureControl);
  pic.type = static_cast<hardware::H264EncFrameType>(w[34]);
  pic.idr_pic_id = w[35];
  pic.pic_parameter_set_id = w[36];
  pic.poc = w[37];
  pic.frame_num = w[38];
  pic.reference = (w[39] & 1) != 0;
  pic.adaptive_ref_pic_marking = w[40] != 0;
  pic.marking = ArrayView<const hardware::H264EncMarking>(marking.view().data(), marking.count());
  pic.modification[0] = ArrayView<const hardware::H264EncListModification>(mods[0].view().data(), mods[0].count());
  pic.modification[1] = ArrayView<const hardware::H264EncListModification>(mods[1].view().data(), mods[1].count());
  if (w[31] > 4) return finish(kErrorSubregionLayout);
  pic.slice_mode = static_cast<hardware::H264EncSliceMode>(w[31]);
  pic.slice_value = w[32];
  pic.precision = w[33] == 1 ? hardware::H264EncPrecision::Full : w[33] == 2 ? hardware::H264EncPrecision::Half : hardware::H264EncPrecision::Quarter;
  const uint32_t slice_count = hardware::H264PictureEncoder::slice_count(pic.slice_mode, pic.slice_value, pic.mb_width, pic.mb_height);
  if (slice_count == 0 || slice_count > max_subregions || job.metadata.size() < kVideoEncodeMetadataHeader + uint64_t{slice_count} * kVideoEncodeSubregionBytes)
    return finish(kErrorSubregionLayout);
  // the QP map: deltas (ENABLE_DELTA_QP) or absolute (ABSOLUTE_QP_MAP mode), one a macroblock
  const uint64_t mbs = uint64_t{pic.mb_width} * pic.mb_height;
  o.macroblocks = mbs;
  const bool absolute = rc.mode == hardware::RateControlMode::AbsoluteQpMap;
  if (absolute || (rc.flags & hardware::kRateControlDeltaQp)) {
    if (nqp != mbs) return finish(kErrorPictureControl);
    pic.qp_map = ArrayView<const int8_t>(reinterpret_cast<const int8_t*>(qp_bytes.data()), qp_bytes.size());   // (bytes reinterpreted as signed: same size and alignment)
    pic.qp_map_absolute = absolute;
  }

  // the pictures
  CompactPicture source(allocator), recon(allocator);
  if (!source.make(width, height) || !recon.make(width, height)) return false;
  source.read(job.input);
  // (each reference texture once, in one buffer: the list entries point into it)
  uint32_t texture_of_slot[kVideoEncodeMaxReferences * 2];
  uint32_t slots = 0;
  for (int l = 0; l < 2; ++l)
    for (uint32_t i = 0; i < (l == 0 ? n0 : n1); ++i) {
      if (list_desc[l][i] >= ndesc) return finish(kErrorReferences);
      const uint32_t t = desc[list_desc[l][i]].texture;
      if (t >= job.references.size()) return finish(kErrorReferences);
      bool seen = false;
      for (uint32_t k = 0; k < slots; ++k) seen = seen || texture_of_slot[k] == t;
      if (!seen) texture_of_slot[slots++] = t;
    }
  const uint64_t frame_bytes = uint64_t{width} * height * 3 / 2, luma_bytes = uint64_t{width} * height;
  ByteBuffer ref_bytes(allocator);
  if (!ref_bytes.put_zeros(frame_bytes * slots)) return false;
  for (uint32_t k = 0; k < slots; ++k) {
    const VideoPictureImages& img = job.references[texture_of_slot[k]];
    if (!plane_fits(img.luma, width, height) || !plane_fits(img.chroma, width / 2, height / 2)) return finish(kErrorReferences);
    CompactPicture c(allocator);
    if (!c.make(width, height)) return false;
    c.read(img);
    ref_bytes.writable_view().subview(frame_bytes * k, frame_bytes).copy_from(c.bytes.view());
  }
  hardware::H264EncReference entries[2][kVideoEncodeMaxReferences];
  for (int l = 0; l < 2; ++l)
    for (uint32_t i = 0; i < (l == 0 ? n0 : n1); ++i) {
      const uint32_t t = desc[list_desc[l][i]].texture;
      uint32_t k = 0;
      while (texture_of_slot[k] != t) ++k;
      const ConstByteView f = ref_bytes.view().subview(frame_bytes * k, frame_bytes);
      entries[l][i] = {{f.subview(0, luma_bytes), f.subview(luma_bytes, luma_bytes / 4), f.subview(luma_bytes * 5 / 4, luma_bytes / 4), width, height}, t};
    }
  pic.list[0] = ArrayView<const hardware::H264EncReference>(entries[0], n0);
  pic.list[1] = ArrayView<const hardware::H264EncReference>(entries[1], n1);

  // rate control: the QP, coded again while the picture is too big (for the controller, or for the bitstream buffer)
  hardware::RateControlState state;
  if (!job.heap.empty()) load_state(job.heap, &state);
  const hardware::RateControlPicture kind = pic.type == hardware::H264EncFrameType::B ? hardware::RateControlPicture::B
                                            : pic.type == hardware::H264EncFrameType::P ? hardware::RateControlPicture::P : hardware::RateControlPicture::Intra;
  uint32_t qp = hardware::rate_control_qp(rc, state, kind, uint64_t{width} * height);
  hardware::H264PictureEncoder encoder(allocator);
  ByteBuffer out(allocator);
  for (uint32_t attempt = 0;; ++attempt) {
    out.clear();
    pic.qp = static_cast<uint8_t>(qp > 51 ? 51 : qp);
    const hardware::H264EncResult result = encoder.encode(pic, source.planes(), recon.writable(), &out, &slices, &o.stats);
    switch (result) {
      case hardware::H264EncResult::Ok: break;
      case hardware::H264EncResult::NoMemory: return false;
      case hardware::H264EncResult::SubregionLayoutNotSupported: return finish(kErrorSubregionLayout);
      case hardware::H264EncResult::InvalidReferences: return finish(kErrorReferences);
      default: return finish(kErrorPictureControl);
    }
    const bool fits = out.size() <= job.bitstream.size();
    uint32_t retry = qp;
    const bool accepted = hardware::rate_control_accept(rc, state, kind, qp, out.size() * 8, &retry);
    if (fits && (accepted || attempt >= hardware::kRateControlMaxRetries)) break;
    if (!fits) retry = retry > qp ? retry : qp + 4;
    if (qp >= 51 || attempt >= 2 * hardware::kRateControlMaxRetries) {
      if (!fits) return finish(kErrorPictureControl);   // (not even QP 51 fits the program's bitstream buffer)
      break;
    }
    qp = retry > 51 ? 51 : retry;
  }
  hardware::rate_control_update(rc, state, kind, qp, out.size() * 8);
  if (!job.heap.empty()) store_state(job.heap, state);
  job.bitstream.subview(0, out.size()).copy_from(out.view());
  if (job.has_reconstruction) recon.write(job.reconstruction_luma, job.reconstruction_chroma);
  o.written = out.size();
  return finish(0);
}

bool resolve_video_encode_metadata(ConstByteView metadata, ByteView out) {
  ByteReader r(metadata);
  const uint32_t magic = r.le32(), version = r.le32();
  ByteWriter w(out);
  if (!r.ok() || magic != kVideoEncodeMetadataMagic || version != 1) {
    if (out.size() < 72) return false;
    w.le64(kErrorMetadataSource);
    w.zeros(64);
    return true;
  }
  const ConstByteView head = r.take(72);
  if (!r.ok()) return false;
  const uint64_t count = ByteReader(head.subview(64, 8)).le64();
  if (count > (metadata.size() - kVideoEncodeMetadataHeader) / kVideoEncodeSubregionBytes) return false;
  const uint64_t bytes = 72 + count * kVideoEncodeSubregionBytes;
  if (out.size() < bytes) return false;
  out.subview(0, bytes).copy_from(metadata.subview(8, bytes));
  return true;
}

bool run_motion_estimation(allocators::IAllocator& allocator, const VideoPlaneImage& current, const VideoPlaneImage& reference, uint32_t width, uint32_t height, uint32_t block,
                           ConstByteView hints, ByteView heap) {
  if ((block != 8 && block != 16) || !plane_fits(current, width, height) || !plane_fits(reference, width, height) || heap.size() < motion_vector_heap_bytes(width, height, block))
    return false;
  ByteBuffer planes(allocator);
  if (!planes.put_zeros(uint64_t{width} * height * 2)) return false;
  ByteView v = planes.writable_view();
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      v[uint64_t{y} * width + x] = current.memory[current.pitch * y + uint64_t{x} * 4];
      v[uint64_t{width} * height + uint64_t{y} * width + x] = reference.memory[reference.pitch * y + uint64_t{x} * 4];
    }
  const uint32_t cols = (width + block - 1) / block, rows = (height + block - 1) / block;
  const uint64_t count = uint64_t{cols} * rows;
  List<hardware::MotionBlockResult> results(allocator);
  List<hardware::MotionVector> hint_vectors(allocator);
  if (!results.reserve(count)) return false;
  for (uint64_t i = 0; i < count; ++i)
    if (!results.push_back(hardware::MotionBlockResult())) return false;
  // (a hint heap of the same layout lends its vectors as candidates)
  ByteReader h(hints);
  if (!hints.empty() && h.le32() == kMotionVectorHeapMagic && h.le32() == block && h.le32() == cols && h.le32() == rows && hints.size() >= motion_vector_heap_bytes(width, height, block)) {
    for (uint64_t i = 0; i < count; ++i) {
      hardware::MotionVector mv;
      mv.x = static_cast<int16_t>(h.le16());
      mv.y = static_cast<int16_t>(h.le16());
      h.le32();
      if (!hint_vectors.push_back(mv)) return false;
    }
  }
  const hardware::LumaPlane cur{ConstByteView(v).subview(0, uint64_t{width} * height), static_cast<int>(width), static_cast<int>(height), static_cast<int>(width)};
  const hardware::LumaPlane ref{ConstByteView(v).subview(uint64_t{width} * height, uint64_t{width} * height), static_cast<int>(width), static_cast<int>(height), static_cast<int>(width)};
  if (!hardware::estimate_motion(cur, ref, static_cast<int>(block), ArrayView<const hardware::MotionVector>(hint_vectors.view().data(), hint_vectors.count()), results.view()))
    return false;
  ByteWriter w(heap);
  w.le32(kMotionVectorHeapMagic);
  w.le32(block);
  w.le32(cols);
  w.le32(rows);
  for (const hardware::MotionBlockResult& m : results.view()) {
    w.le16(static_cast<uint16_t>(m.vector.x));
    w.le16(static_cast<uint16_t>(m.vector.y));
    w.le32(m.sad);
  }
  return w.ok();
}

bool resolve_motion_vectors(ConstByteView heap, uint32_t width, uint32_t height, const VideoWritablePlaneImage& image, uint32_t x, uint32_t y) {
  ByteReader r(heap);
  const uint32_t magic = r.le32(), block = r.le32(), cols = r.le32(), rows = r.le32();
  if (!r.ok() || magic != kMotionVectorHeapMagic || (block != 8 && block != 16) || cols != (width + block - 1) / block || rows != (height + block - 1) / block) return false;
  if (uint64_t{x} + cols > image.width || uint64_t{y} + rows > image.height || image.pitch < uint64_t{image.width} * 8) return false;
  if (image.memory.size() < image.pitch * (uint64_t{y} + rows - 1) + (uint64_t{x} + cols) * 8) return false;
  for (uint32_t row = 0; row < rows; ++row)
    for (uint32_t col = 0; col < cols; ++col) {
      const uint16_t mx = r.le16(), my = r.le16();
      r.le32();
      ByteWriter px(image.memory.subview(image.pitch * (y + row) + (uint64_t{x} + col) * 8, 8));
      px.le16(mx);
      px.le16(my);
      px.le16(0);
      px.le16(1);   // (an integer format's missing alpha is 1)
    }
  return r.ok();
}

}  // namespace dxgk
