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
#include "Kernel/Dxgkrnl/VideoEncodeCodecs.hpp"
#include "Hardware/VideoCodec/Software/H264PictureEncoder.hpp"
#include "Hardware/VideoCodec/Software/MotionEstimator.hpp"
#include "Hardware/VideoCodec/Software/VideoRateControl.hpp"
#include "Math/FloatMath.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"

namespace dxgk {

namespace {

// D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAGS
constexpr uint64_t kErrorPictureControl = 0x1, kErrorSubregionLayout = 0x2, kErrorReferences = 0x4, kErrorMetadataSource = 0x10;
constexpr uint32_t kRateControlStateMagic = 0x43524354u;   // 'TCRC'
constexpr uint32_t kStoreMagic = 0x34484354u, kSlotMagic = 0x53524354u;   // 'TCH4', 'TCRS'
// D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAGS
constexpr uint32_t kConfigConstrainedIntra = 0x1, kConfigTransform8x8 = 0x2, kConfigCabac = 0x4;

bool image_fits(ConstByteView memory, uint64_t pitch, uint32_t image_width, uint32_t image_height, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > image_width || height > image_height || pitch < uint64_t{image_width} * 4) return false;
  return memory.size() >= pitch * (height - 1) + uint64_t{width} * 4;
}
bool plane_fits(const VideoPlaneImage& p, uint32_t w, uint32_t h) { return image_fits(p.memory, p.pitch, p.width, p.height, w, h); }
bool plane_fits(const VideoWritablePlaneImage& p, uint32_t w, uint32_t h) { return image_fits(ConstByteView(p.memory), p.pitch, p.width, p.height, w, h); }
inline uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }

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
  // plane 0 luma, 1 Cb, 2 Cr: where it starts and its width
  uint64_t plane_offset(int p) const { return p == 0 ? 0 : p == 1 ? luma() : luma() * 5 / 4; }
  uint32_t plane_width(int p) const { return p == 0 ? width : width / 2; }
  hardware::H264EncPlanes planes() const {
    const ConstByteView v = bytes.view();
    return {v.subview(0, luma()), v.subview(luma(), luma() / 4), v.subview(luma() * 5 / 4, luma() / 4), width, height};
  }
  hardware::H264EncWritablePlanes writable() {
    const ByteView v = bytes.writable_view();
    return {v.subview(0, luma()), v.subview(luma(), luma() / 4), v.subview(luma() * 5 / 4, luma() / 4), width, height};
  }
  // From the widened planes: their top-left rw x rh (even, at most the picture's size), the last column and row repeated beyond.
  void read(const VideoPictureImages& in, uint32_t rw, uint32_t rh) {
    ByteView v = bytes.writable_view();
    for (uint32_t y = 0; y < height; ++y) {
      const uint64_t row = in.luma.pitch * umin(y, rh - 1);
      for (uint32_t x = 0; x < width; ++x) v[uint64_t{y} * width + x] = in.luma.memory[row + uint64_t{umin(x, rw - 1)} * 4];
    }
    const uint32_t cw = width / 2, ch = height / 2;
    for (uint32_t y = 0; y < ch; ++y) {
      const uint64_t row = in.chroma.pitch * umin(y, rh / 2 - 1);
      for (uint32_t x = 0; x < cw; ++x) {
        const uint64_t at = row + uint64_t{umin(x, rw / 2 - 1)} * 4;
        v[luma() + uint64_t{y} * cw + x] = in.chroma.memory[at];
        v[luma() * 5 / 4 + uint64_t{y} * cw + x] = in.chroma.memory[at + 1];
      }
    }
  }
  // Into widened planes, their top-left rw x rh: (Y, 0, 0, 255) and (U, V, 0, 255).
  void write(const VideoWritablePlaneImage& luma_image, const VideoWritablePlaneImage& chroma_image, uint32_t rw, uint32_t rh) const {
    const ConstByteView v = bytes.view();
    for (uint32_t y = 0; y < rh; ++y)
      for (uint32_t x = 0; x < rw; ++x) {
        ByteView px = luma_image.memory.subview(luma_image.pitch * y + uint64_t{x} * 4, 4);
        px[0] = v[uint64_t{y} * width + x];
        px[1] = px[2] = 0;
        px[3] = 0xFF;
      }
    const uint32_t cw = width / 2;
    for (uint32_t y = 0; y < rh / 2; ++y)
      for (uint32_t x = 0; x < rw / 2; ++x) {
        ByteView px = chroma_image.memory.subview(chroma_image.pitch * y + uint64_t{x} * 4, 4);
        px[0] = v[luma() + uint64_t{y} * cw + x];
        px[1] = v[luma() * 5 / 4 + uint64_t{y} * cw + x];
        px[2] = 0;
        px[3] = 0xFF;
      }
  }
};

// The samples of a picture outside its top-left tw x th (what a texture of that size has no room for), in a fixed order: luma, Cb, Cr, each first the columns right of
// tw on every row, then the rows below th left of tw. fn(plane, x, y).
template <typename Fn>
void for_each_strip_sample(const CompactPicture& pic, uint32_t tw, uint32_t th, Fn&& fn) {
  for (int p = 0; p < 3; ++p) {
    const uint32_t pw = p == 0 ? pic.width : pic.width / 2, ph = p == 0 ? pic.height : pic.height / 2;
    const uint32_t cw = p == 0 ? tw : tw / 2, chh = p == 0 ? th : th / 2;
    for (uint32_t y = 0; y < ph; ++y)
      for (uint32_t x = cw; x < pw; ++x) fn(p, x, y);
    for (uint32_t y = chh; y < ph; ++y)
      for (uint32_t x = 0; x < cw; ++x) fn(p, x, y);
  }
}
uint64_t strip_samples(const CompactPicture& pic, uint32_t tw, uint32_t th) {
  uint64_t n = 0;
  for_each_strip_sample(pic, tw, th, [&](int, uint32_t, uint32_t) { ++n; });
  return n;
}

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

// ---- the H.264 reference store in the encoder heap (VideoEncode.hpp)

struct SlotInfo {
  bool valid = false;
  uint64_t key = 0;
  int32_t poc = 0;
  uint32_t mb_w = 0, mb_h = 0, tex_w = 0, tex_h = 0, clean_rows = 0, strip_count = 0, has_motion = 0;
  uint64_t stamp = 0;
};
class ReferenceStore {
 public:
  // The store of `heap` (past the rate controller's state), if it has room for pictures of mb_w x mb_h macroblocks; made empty if it was never set up for its size.
  bool open(ByteView heap, uint32_t mb_w, uint32_t mb_h) {
    if (heap.size() < uint64_t{kVideoEncodeHeapBytes} + kH264StoreHeader + kH264StoreSlots * 8) return false;
    region_ = heap.subview(kVideoEncodeHeapBytes, heap.size() - kVideoEncodeHeapBytes);
    slot_bytes_ = (region_.size() - kH264StoreHeader) / kH264StoreSlots / 8 * 8;
    if (slot_bytes_ < h264_slot_bytes(mb_w * 16, mb_h * 16)) return false;
    ByteReader r(region_.subview(0, kH264StoreHeader));
    const uint32_t magic = r.le32(), count = r.le32();
    const uint64_t bytes = r.le64();
    stamp_ = r.le64();
    if (magic != kStoreMagic || count != kH264StoreSlots || bytes != slot_bytes_) {
      region_.fill(0);
      ByteWriter w(region_.subview(0, kH264StoreHeader));
      w.le32(kStoreMagic);
      w.le32(kH264StoreSlots);
      w.le64(slot_bytes_);
      w.le64(0);
      stamp_ = 0;
    }
    open_ = true;
    return true;
  }
  bool is_open() const { return open_; }
  ByteView slot(uint32_t i) { return region_.subview(kH264StoreHeader + uint64_t{i} * slot_bytes_, slot_bytes_); }
  SlotInfo info(uint32_t i) {
    SlotInfo s;
    ByteReader r(ConstByteView(slot(i)).subview(0, kH264SlotHeader));
    const uint32_t magic = r.le32();
    s.valid = r.le32() == 1 && magic == kSlotMagic;
    s.key = r.le64();
    s.poc = static_cast<int32_t>(r.le32());
    s.mb_w = r.le32();
    s.mb_h = r.le32();
    s.tex_w = r.le32();
    s.tex_h = r.le32();
    s.clean_rows = r.le32();
    s.strip_count = r.le32();
    s.has_motion = r.le32();
    s.stamp = r.le64();
    return s;
  }
  // The slot holding the picture of texture `key` with order count `poc`, of this size (-1: none).
  int find(uint64_t key, int32_t poc, uint32_t mb_w, uint32_t mb_h) {
    if (!open_) return -1;
    for (uint32_t i = 0; i < kH264StoreSlots; ++i) {
      const SlotInfo s = info(i);
      if (s.valid && s.key == key && s.poc == poc && s.mb_w == mb_w && s.mb_h == mb_h) return static_cast<int>(i);
    }
    return -1;
  }
  // Where the picture of texture `key` goes: its own slot, else an empty one, else one whose texture is not among `live` (the references the program still has), else the
  // oldest.
  uint32_t choose(uint64_t key, ArrayView<const uint64_t> live) {
    int empty = -1, dead = -1, oldest = 0;
    uint64_t oldest_stamp = ~uint64_t{0};
    for (uint32_t i = 0; i < kH264StoreSlots; ++i) {
      const SlotInfo s = info(i);
      if (s.valid && s.key == key) return i;
      if (!s.valid) {
        if (empty < 0) empty = static_cast<int>(i);
        continue;
      }
      bool in_use = false;
      for (const uint64_t k : live) in_use = in_use || k == s.key;
      if (!in_use && dead < 0) dead = static_cast<int>(i);
      if (s.stamp < oldest_stamp) {
        oldest_stamp = s.stamp;
        oldest = static_cast<int>(i);
      }
    }
    return static_cast<uint32_t>(empty >= 0 ? empty : dead >= 0 ? dead : oldest);
  }
  void write_header(uint32_t i, const SlotInfo& s) {
    ByteWriter w(slot(i).subview(0, kH264SlotHeader));
    w.le32(kSlotMagic);
    w.le32(s.valid ? 1 : 0);
    w.le64(s.key);
    w.le32(static_cast<uint32_t>(s.poc));
    w.le32(s.mb_w);
    w.le32(s.mb_h);
    w.le32(s.tex_w);
    w.le32(s.tex_h);
    w.le32(s.clean_rows);
    w.le32(s.strip_count);
    w.le32(s.has_motion);
    w.le64(++stamp_);
    ByteWriter h(region_.subview(16, 8));
    h.le64(stamp_);
  }

 private:
  ByteView region_;
  uint64_t slot_bytes_ = 0, stamp_ = 0;
  bool open_ = false;
};

// One macroblock's motion as the store keeps it (kH264MotionBytes): intra, then for each 8x8 block the co-located motion direct prediction reads - the list 0 motion, else
// the list 1 motion - as a reference index, the identity of its picture and the vector.
void put_motion(ByteWriter& w, const hardware::H264EncMbMotion& m) {
  w.u8(m.intra);
  w.zeros(3);
  for (int b = 0; b < 4; ++b) {
    const int list = m.ref[0][b] >= 0 ? 0 : 1;
    const bool used = !m.intra && m.ref[list][b] >= 0;
    w.u8(static_cast<uint8_t>(used ? m.ref[list][b] : -1));
    w.le32(used ? m.ident[list][b] : 0);
    w.le16(static_cast<uint16_t>(used ? m.mv[list][b][0] : 0));
    w.le16(static_cast<uint16_t>(used ? m.mv[list][b][1] : 0));
  }
}
hardware::H264EncMbMotion get_motion(ByteReader& r) {
  hardware::H264EncMbMotion m;
  m.intra = r.u8() ? 1 : 0;
  r.skip(3);
  for (int b = 0; b < 4; ++b) {
    m.ref[0][b] = static_cast<int8_t>(r.u8());
    m.ident[0][b] = r.le32();
    m.mv[0][b][0] = static_cast<int16_t>(r.le16());
    m.mv[0][b][1] = static_cast<int16_t>(r.le16());
    if (m.intra) m.ref[0][b] = -1;
  }
  return m;
}

struct Outcome {
  uint64_t errors = 0;
  uint64_t written = 0;
  hardware::H264EncStatistics stats;
  uint64_t macroblocks = 1;
  uint32_t optional = 0;   // the optional metadata written (PSNR)
};

// Sums of squared errors and sample counts of a part of a picture: luma, Cb, Cr.
struct ErrorSums {
  uint64_t sse[3] = {0, 0, 0};
  uint64_t luma = 0, chroma = 0;
};
// The errors of the luma rectangle (x0, y0) .. (x1, y1) (even, exclusive) of `rec` against `src`.
void add_errors(const CompactPicture& src, const CompactPicture& rec, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, ErrorSums* e) {
  const ConstByteView a = src.bytes.view(), b = rec.bytes.view();
  for (int p = 0; p < 3; ++p) {
    const uint32_t s = p == 0 ? 1 : 2, pw = src.plane_width(p);
    const uint64_t base = src.plane_offset(p);
    for (uint32_t y = y0 / s; y < y1 / s; ++y)
      for (uint32_t x = x0 / s; x < x1 / s; ++x) {
        const int d = int{a[base + uint64_t{y} * pw + x]} - b[base + uint64_t{y} * pw + x];
        e->sse[p] += static_cast<uint64_t>(d * d);
      }
  }
  e->luma += uint64_t{x1 - x0} * (y1 - y0);
  e->chroma += uint64_t{(x1 - x0) / 2} * ((y1 - y0) / 2);
}
void put_errors(ByteWriter& w, const ErrorSums& e) {
  for (const uint64_t v : e.sse) w.le64(v);
  w.le64(e.luma);
  w.le64(e.chroma);
}

void write_metadata(ByteView metadata, const Outcome& o, ArrayView<const hardware::H264EncSlice> slices, const CompactPicture* src, const CompactPicture* rec, uint32_t width,
                    uint32_t height, uint32_t mb_w) {
  ByteWriter w(metadata);
  const bool optional = !o.errors && o.optional != 0;
  w.le32(kVideoEncodeMetadataMagic);
  w.le32(optional ? 2 : 1);
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
  if (!optional || !src || !rec) return;
  w.le32(kVideoEncodeMetadataOptionalMagic);
  w.le32(o.optional);
  ErrorSums frame;
  add_errors(*src, *rec, 0, 0, width, height, &frame);
  put_errors(w, frame);
  // each slice: its macroblocks, the visible part of them
  for (const hardware::H264EncSlice& s : slices) {
    ErrorSums e;
    for (uint32_t a = s.first_mb; a < s.first_mb + s.mb_count; ++a) {
      const uint32_t x0 = (a % mb_w) * 16, y0 = (a / mb_w) * 16;
      if (x0 >= width || y0 >= height) continue;
      add_errors(*src, *rec, x0, y0, umin(x0 + 16, width), umin(y0 + 16, height), &e);
    }
    put_errors(w, e);
  }
}

// PSNR in dB of a sum of squared errors over `count` samples (100 for no error).
float psnr_of(uint64_t sse, uint64_t count) {
  if (count == 0) return 0.0f;
  if (sse == 0) return 100.0f;
  const double ratio = 255.0 * 255.0 * static_cast<double>(count) / static_cast<double>(sse);
  return static_cast<float>(10.0 * fmath::ln(ratio) / fmath::ln(10.0));
}
void put_psnr(ByteWriter& w, ByteReader& r) {
  uint64_t sse[3];
  for (uint64_t& v : sse) v = r.le64();
  const uint64_t luma = r.le64(), chroma = r.le64();
  for (int p = 0; p < 3; ++p) {
    const float f = psnr_of(sse[p], p == 0 ? luma : chroma);
    w.le32(__builtin_bit_cast(uint32_t, f));   // (D3D12's resolved layout is IEEE single precision floats)
  }
}

}  // namespace

// (shared with the HEVC and AV1 paths: VideoEncodeCodecs.hpp)
void load_rate_control_state(ConstByteView heap, hardware::RateControlState* state) { load_state(heap, state); }
void store_rate_control_state(ByteView heap, const hardware::RateControlState& state) { store_state(heap, state); }

bool run_video_encode(allocators::IAllocator& allocator, const VideoEncodeJob& job) {
  // (HEVC and AV1 have argument layouts of their own: VideoEncodeCodecs.hpp)
  {
    ByteReader c(job.params);
    const uint32_t codec = c.le32();
    if (c.ok() && codec == kVideoEncodeCodecHevc) return run_video_encode_hevc(allocator, job);
    if (c.ok() && codec == kVideoEncodeCodecAv1) return run_video_encode_av1(allocator, job);
  }
  Outcome o;
  List<hardware::H264EncSlice> slices(allocator);
  uint32_t width = 0, height = 0, mb_w = 1;
  const CompactPicture* psnr_src = nullptr;
  const CompactPicture* psnr_rec = nullptr;
  auto finish = [&](uint64_t errors) {
    o.errors = errors;
    if (errors) o.written = 0;
    const uint64_t need = errors ? kVideoEncodeMetadataHeader
                                 : kVideoEncodeMetadataHeader + slices.count() * kVideoEncodeSubregionBytes +
                                       (o.optional ? 8 + kVideoEncodePsnrRecordBytes * (1 + slices.count()) : 0);
    if (job.metadata.size() >= need) write_metadata(job.metadata, o, slices.view(), psnr_src, psnr_rec, width, height, mb_w);
    return true;
  };
  ByteReader r(job.params);
  uint32_t w[kVideoEncodeParamWords];
  for (uint32_t& v : w) v = r.le32();
  if (!r.ok()) return finish(kErrorPictureControl);
  if (w[48]) return finish(w[48] & (kErrorPictureControl | kErrorSubregionLayout | kErrorReferences | 0x8));   // (what the runtime found wrong)
  const uint32_t codec = w[0], profile = w[1], config = w[4], direct = w[5], deblock = w[6];
  width = w[2];
  height = w[3];
  const uint32_t ndesc = w[41], n0 = w[42], n1 = w[43], nmark = w[44], nmod0 = w[45], nmod1 = w[46], nqp = w[47], max_subregions = w[49];
  const uint32_t nunchanged = w[53], nhints = w[55];
  if (ndesc > kVideoEncodeMaxReferences || n0 > kVideoEncodeMaxReferences || n1 > kVideoEncodeMaxReferences || nmark > 64 || nmod0 > 64 || nmod1 > 64 || nqp > (1u << 20) ||
      nunchanged > (1u << 20) || nhints > (1u << 20))
    return finish(kErrorPictureControl);
  for (uint32_t i = 59; i < kVideoEncodeParamWords; ++i)
    if (w[i] != 0) return finish(kErrorPictureControl);
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
  const ConstByteView unchanged_bytes = r.take(nunchanged);
  r.skip((4 - nunchanged % 4) % 4);
  List<hardware::H264EncHint> hints(allocator);
  if (!hints.reserve(nhints)) return false;
  for (uint32_t i = 0; i < nhints && r.ok(); ++i) {
    hardware::H264EncHint h;
    h.ref[0] = static_cast<int8_t>(r.u8());
    h.ref[1] = static_cast<int8_t>(r.u8());
    r.skip(2);
    for (int l = 0; l < 2; ++l)
      for (int c = 0; c < 2; ++c) h.mv[l][c] = static_cast<int16_t>(r.le16());
    if (!hints.push_back(h)) return false;
  }
  if (!r.ok() || !r.at_end()) return finish(kErrorPictureControl);

  // the configuration: H.264 Main or High; constrained intra prediction, CABAC, the 8x8 transform (High), direct prediction, filter modes 0 .. 2 (what the support queries
  // report); intra constrained slices are not done
  if (codec != 0 || profile > 1 || (config & ~(kConfigConstrainedIntra | kConfigTransform8x8 | kConfigCabac)) || ((config & kConfigTransform8x8) && profile != 1) || direct > 2 ||
      deblock > 2 || (w[39] & 2))
    return finish(kErrorPictureControl);
  if (width < 16 || height < 16 || (width & 1) || (height & 1) || width > 4096 || height > 4096 || !plane_fits(job.input.luma, width, height) ||
      !plane_fits(job.input.chroma, width / 2, height / 2))
    return finish(kErrorPictureControl);
  const uint32_t coded_w = (width + 15) / 16 * 16, coded_h = (height + 15) / 16 * 16;
  // (the reconstruction's texture holds the target size at least, the coded size as far as it reaches)
  const uint32_t rec_w = job.has_reconstruction ? umin(job.reconstruction_luma.width, coded_w) & ~1u : 0, rec_h = job.has_reconstruction ? umin(job.reconstruction_luma.height, coded_h) & ~1u : 0;
  if (job.has_reconstruction && (rec_w < width || rec_h < height || !plane_fits(job.reconstruction_luma, rec_w, rec_h) || !plane_fits(job.reconstruction_chroma, rec_w / 2, rec_h / 2)))
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
  pic.mb_width = coded_w / 16;
  pic.mb_height = coded_h / 16;
  mb_w = pic.mb_width;
  pic.constrained_intra_pred = (config & kConfigConstrainedIntra) != 0;
  pic.entropy = (config & kConfigCabac) ? hardware::H264EncEntropy::Cabac : hardware::H264EncEntropy::Cavlc;
  pic.transform_8x8 = (config & kConfigTransform8x8) != 0;
  pic.direct = static_cast<hardware::H264EncDirect>(direct);
  pic.disable_deblocking_filter_idc = static_cast<uint8_t>(deblock);
  if (w[7] > 2 || w[8] > 12 || w[9] > 12) return finish(kErrorPictureControl);
  pic.pic_order_cnt_type = static_cast<uint8_t>(w[7]);
  pic.log2_max_frame_num = static_cast<uint8_t>(w[8] + 4);
  pic.log2_max_poc_lsb = static_cast<uint8_t>(w[9] + 4);
  if (w[34] > 3) return finish(kErrorPictureControl);
  pic.type = static_cast<hardware::H264EncFrameType>(w[34]);
  const bool inter = pic.type == hardware::H264EncFrameType::P || pic.type == hardware::H264EncFrameType::B;
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
  // intra refresh (row based): the rows of this picture's part of the wave
  const uint64_t mbs = uint64_t{pic.mb_width} * pic.mb_height;
  o.macroblocks = mbs;
  if (w[50] > 1) return finish(kErrorPictureControl);
  if (w[50] == 1) {
    const uint32_t duration = w[51], index = w[52];
    if (duration == 0 || duration > pic.mb_height || index >= duration) return finish(kErrorPictureControl);
    if (inter) {
      pic.refresh = true;
      pic.refresh_begin = static_cast<uint32_t>(uint64_t{index} * pic.mb_height / duration);
      pic.refresh_end = static_cast<uint32_t>(uint64_t{index + 1} * pic.mb_height / duration);
    }
  }
  // the QP map: deltas (ENABLE_DELTA_QP) or absolute (ABSOLUTE_QP_MAP mode), one a macroblock
  const bool absolute = rc.mode == hardware::RateControlMode::AbsoluteQpMap;
  if (absolute || (rc.flags & hardware::kRateControlDeltaQp)) {
    if (nqp != mbs) return finish(kErrorPictureControl);
    pic.qp_map = ArrayView<const int8_t>(reinterpret_cast<const int8_t*>(qp_bytes.data()), qp_bytes.size());   // (bytes reinterpreted as signed: same size and alignment)
    pic.qp_map_absolute = absolute;
  }
  // dirty regions: the unchanged macroblocks, copies of a list 0 picture
  if (nunchanged) {
    if (nunchanged != mbs || !inter || w[54] >= n0) return finish(kErrorPictureControl);
    pic.unchanged = unchanged_bytes;
    pic.unchanged_ref = w[54];
  }
  // motion hints
  if (nhints) {
    if (nhints != mbs || !inter || w[56] > 2) return finish(kErrorPictureControl);
    for (const hardware::H264EncHint& h : hints.view())
      for (int l = 0; l < 2; ++l)
        if (h.ref[l] >= static_cast<int>(l == 0 ? n0 : n1)) return finish(kErrorPictureControl);
    pic.hints = ArrayView<const hardware::H264EncHint>(hints.view().data(), hints.count());
    pic.hint_mode = static_cast<hardware::H264EncHintMode>(w[56]);
    pic.hint_limit = w[57];
  }
  // optional metadata: frame and subregion PSNR (the per-block maps are not offered)
  if (w[58] & ~(kVideoEncodeOptionalFramePsnr | kVideoEncodeOptionalSubregionPsnr)) return finish(kErrorPictureControl);
  o.optional = w[58];

  // the pictures: the source in whole macroblocks (its last column and row repeated)
  CompactPicture source(allocator), recon(allocator);
  if (!source.make(coded_w, coded_h) || !recon.make(coded_w, coded_h)) return false;
  source.read(job.input, width, height);
  ReferenceStore store;
  if (!job.heap.empty()) (void)store.open(job.heap, pic.mb_width, pic.mb_height);
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
  const uint64_t frame_bytes = uint64_t{coded_w} * coded_h * 3 / 2, luma_bytes = uint64_t{coded_w} * coded_h;
  ByteBuffer ref_bytes(allocator);
  if (!ref_bytes.put_zeros(frame_bytes * slots)) return false;
  struct RefExtra {
    uint32_t exact_w = 0, exact_h = 0, clean_rows = 0;
    int store_slot = -1;
  } extra[kVideoEncodeMaxReferences * 2];
  for (uint32_t k = 0; k < slots; ++k) {
    const uint32_t t = texture_of_slot[k];
    const VideoPictureImages& img = job.references[t];
    const uint32_t tw = umin(img.luma.width, coded_w) & ~1u, th = umin(img.luma.height, coded_h) & ~1u;
    if (tw < width || th < height || !plane_fits(img.luma, tw, th) || !plane_fits(img.chroma, tw / 2, th / 2)) return finish(kErrorReferences);
    CompactPicture c(allocator);
    if (!c.make(coded_w, coded_h)) return false;
    c.read(img, tw, th);
    // (the order count of the picture in the texture: any descriptor naming it)
    int32_t poc = 0;
    for (uint32_t d = 0; d < ndesc; ++d)
      if (desc[d].texture == t) poc = static_cast<int32_t>(desc[d].poc);
    const uint64_t key = t < job.reference_keys.size() ? job.reference_keys[t] : 0;
    const int s = key ? store.find(key, poc, pic.mb_width, pic.mb_height) : -1;
    extra[k].store_slot = s;
    extra[k].clean_rows = 0;
    if (s >= 0) {
      const SlotInfo si = store.info(static_cast<uint32_t>(s));
      extra[k].clean_rows = si.clean_rows;
      // the samples the texture has no room for, as they were coded
      if (si.tex_w == tw && si.tex_h == th && si.strip_count == strip_samples(c, tw, th)) {
        ByteReader sr(ConstByteView(store.slot(static_cast<uint32_t>(s))).subview(kH264SlotHeader + mbs * kH264MotionBytes));
        ByteView v = c.bytes.writable_view();
        for_each_strip_sample(c, tw, th, [&](int p, uint32_t x, uint32_t y) { v[c.plane_offset(p) + uint64_t{y} * c.plane_width(p) + x] = sr.u8(); });
      } else if (tw < coded_w || th < coded_h) {
        extra[k].exact_w = tw;
        extra[k].exact_h = th;
      }
    } else if (tw < coded_w || th < coded_h) {
      extra[k].exact_w = tw;   // (not known beyond the texture: predictions keep inside it)
      extra[k].exact_h = th;
    }
    ref_bytes.writable_view().subview(frame_bytes * k, frame_bytes).copy_from(c.bytes.view());
  }
  // the co-located picture's motion (RefPicList1[0] of a B picture with direct prediction)
  List<hardware::H264EncMbMotion> colocated(allocator);
  hardware::H264EncReference entries[2][kVideoEncodeMaxReferences];
  for (int l = 0; l < 2; ++l)
    for (uint32_t i = 0; i < (l == 0 ? n0 : n1); ++i) {
      const Descriptor& d = desc[list_desc[l][i]];
      uint32_t k = 0;
      while (texture_of_slot[k] != d.texture) ++k;
      const ConstByteView f = ref_bytes.view().subview(frame_bytes * k, frame_bytes);
      hardware::H264EncReference& e = entries[l][i];
      e.picture = {f.subview(0, luma_bytes), f.subview(luma_bytes, luma_bytes / 4), f.subview(luma_bytes * 5 / 4, luma_bytes / 4), coded_w, coded_h};
      e.identity = d.poc;   // (order counts tell the pictures of the buffer apart)
      e.poc = static_cast<int32_t>(d.poc);
      e.long_term = d.long_term != 0;
      e.clean_rows = extra[k].clean_rows;
      e.exact_width = extra[k].exact_w;
      e.exact_height = extra[k].exact_h;
      if (l == 1 && i == 0 && pic.type == hardware::H264EncFrameType::B && pic.direct != hardware::H264EncDirect::None && extra[k].store_slot >= 0 &&
          store.info(static_cast<uint32_t>(extra[k].store_slot)).has_motion) {
        if (!colocated.reserve(mbs)) return false;
        ByteReader mr(ConstByteView(store.slot(static_cast<uint32_t>(extra[k].store_slot))).subview(kH264SlotHeader, mbs * kH264MotionBytes));
        for (uint64_t a = 0; a < mbs; ++a)
          if (!colocated.push_back(get_motion(mr))) return false;
        e.motion = ArrayView<const hardware::H264EncMbMotion>(colocated.view().data(), colocated.count());
      }
    }
  pic.list[0] = ArrayView<const hardware::H264EncReference>(entries[0], n0);
  pic.list[1] = ArrayView<const hardware::H264EncReference>(entries[1], n1);
  // what this picture leaves for later ones: its motion, if it is a reference that can be kept
  const bool keep = store.is_open() && job.has_reconstruction && job.reconstruction_key != 0 && (pic.reference || pic.type == hardware::H264EncFrameType::Idr);
  List<hardware::H264EncMbMotion> motion(allocator);
  if (keep) {
    if (!motion.reserve(mbs)) return false;
    for (uint64_t a = 0; a < mbs; ++a)
      if (!motion.push_back(hardware::H264EncMbMotion())) return false;
  }
  hardware::H264EncOutputs outputs;
  outputs.motion = ArrayView<hardware::H264EncMbMotion>(motion.view().data(), motion.count());

  // rate control: the QP, coded again while the picture is too big (for the controller, or for the bitstream buffer)
  hardware::RateControlState state;
  if (!job.heap.empty()) load_state(job.heap, &state);
  const hardware::RateControlPicture kind = pic.type == hardware::H264EncFrameType::B ? hardware::RateControlPicture::B
                                            : pic.type == hardware::H264EncFrameType::P ? hardware::RateControlPicture::P : hardware::RateControlPicture::Intra;
  uint32_t qp = hardware::rate_control_qp(rc, state, kind, uint64_t{coded_w} * coded_h);
  hardware::H264PictureEncoder encoder(allocator);
  ByteBuffer out(allocator);
  for (uint32_t attempt = 0;; ++attempt) {
    out.clear();
    pic.qp = static_cast<uint8_t>(qp > 51 ? 51 : qp);
    const hardware::H264EncResult result = encoder.encode(pic, source.planes(), recon.writable(), &out, &slices, &o.stats, outputs);
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
  // (slices by bytes: their count is known now)
  if (slices.count() > max_subregions || job.metadata.size() < kVideoEncodeMetadataHeader + slices.count() * kVideoEncodeSubregionBytes) return finish(kErrorSubregionLayout);
  if (o.optional && job.metadata.size() < kVideoEncodeMetadataHeader + slices.count() * kVideoEncodeSubregionBytes + 8 + kVideoEncodePsnrRecordBytes * (1 + slices.count()))
    return finish(kErrorPictureControl);
  hardware::rate_control_update(rc, state, kind, qp, out.size() * 8);
  if (!job.heap.empty()) store_state(job.heap, state);
  job.bitstream.subview(0, out.size()).copy_from(out.view());
  if (job.has_reconstruction) recon.write(job.reconstruction_luma, job.reconstruction_chroma, rec_w, rec_h);
  // the reference store: this picture's motion and the samples its texture has no room for
  if (keep) {
    const uint64_t strips = strip_samples(recon, rec_w, rec_h);
    if (kH264SlotHeader + mbs * kH264MotionBytes + strips <= store.slot(0).size()) {
      const uint32_t s = store.choose(job.reconstruction_key, job.reference_keys);
      ByteView slot = store.slot(s);
      ByteWriter mw(slot.subview(kH264SlotHeader, mbs * kH264MotionBytes));
      for (const hardware::H264EncMbMotion& m : motion.view()) put_motion(mw, m);
      ByteWriter sw(slot.subview(kH264SlotHeader + mbs * kH264MotionBytes, strips));
      const ConstByteView v = recon.bytes.view();
      for_each_strip_sample(recon, rec_w, rec_h, [&](int p, uint32_t x, uint32_t y) { sw.u8(v[recon.plane_offset(p) + uint64_t{y} * recon.plane_width(p) + x]); });
      SlotInfo si;
      si.valid = true;
      si.key = job.reconstruction_key;
      si.poc = static_cast<int32_t>(pic.poc);
      si.mb_w = pic.mb_width;
      si.mb_h = pic.mb_height;
      si.tex_w = rec_w;
      si.tex_h = rec_h;
      si.clean_rows = o.stats.clean_rows;
      si.strip_count = static_cast<uint32_t>(strips);
      si.has_motion = 1;
      store.write_header(s, si);
    }
  }
  o.written = out.size();
  psnr_src = &source;
  psnr_rec = &recon;
  return finish(0);
}

bool resolve_video_encode_metadata(ConstByteView metadata, ByteView out, ByteView frame_psnr, ByteView subregion_psnr) {
  ByteReader r(metadata);
  const uint32_t magic = r.le32(), version = r.le32();
  ByteWriter w(out);
  if (r.ok() && magic == kVideoEncodeMetadataMagic && version == kVideoEncodeMetadataVersionAv1) return resolve_av1_encode_metadata(metadata, out);
  if (!r.ok() || magic != kVideoEncodeMetadataMagic || (version != 1 && version != 2)) {
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
  // the optional metadata: PSNR of the frame and of each slice
  ByteReader o(metadata.subview(kVideoEncodeMetadataHeader + count * kVideoEncodeSubregionBytes));
  uint32_t optional = 0;
  if (version == 2 && o.le32() == kVideoEncodeMetadataOptionalMagic) optional = o.le32();
  const bool has = o.ok() && optional != 0 && o.remaining() >= kVideoEncodePsnrRecordBytes * (1 + count);
  if (!frame_psnr.empty()) {
    if (frame_psnr.size() < 12) return false;
    ByteWriter fw(frame_psnr);
    ByteReader fr(has && (optional & kVideoEncodeOptionalFramePsnr) ? o.rest().first(kVideoEncodePsnrRecordBytes) : ConstByteView());
    if (fr.remaining() == kVideoEncodePsnrRecordBytes) put_psnr(fw, fr);
    else fw.zeros(12);
  }
  if (!subregion_psnr.empty()) {
    if (subregion_psnr.size() < 12 * count) return false;
    ByteWriter sw(subregion_psnr);
    if (has && (optional & kVideoEncodeOptionalSubregionPsnr)) {
      o.skip(kVideoEncodePsnrRecordBytes);
      for (uint64_t i = 0; i < count; ++i) put_psnr(sw, o);
    } else {
      sw.zeros(12 * count);
    }
  }
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

bool resolve_motion_vectors(ConstByteView heap, uint32_t width, uint32_t height, const VideoWritablePlaneImage& image, uint32_t x, uint32_t y, uint32_t texel) {
  if (texel != 4 && texel != 8) return false;
  ByteReader r(heap);
  const uint32_t magic = r.le32(), block = r.le32(), cols = r.le32(), rows = r.le32();
  if (!r.ok() || magic != kMotionVectorHeapMagic || (block != 8 && block != 16) || cols != (width + block - 1) / block || rows != (height + block - 1) / block) return false;
  if (uint64_t{x} + cols > image.width || uint64_t{y} + rows > image.height || image.pitch < uint64_t{image.width} * texel) return false;
  if (image.memory.size() < image.pitch * (uint64_t{y} + rows - 1) + (uint64_t{x} + cols) * texel) return false;
  for (uint32_t row = 0; row < rows; ++row)
    for (uint32_t col = 0; col < cols; ++col) {
      const uint16_t mx = r.le16(), my = r.le16();
      r.le32();
      ByteWriter px(image.memory.subview(image.pitch * (y + row) + (uint64_t{x} + col) * texel, texel));
      px.le16(mx);
      px.le16(my);
      if (texel == 8) {
        px.le16(0);
        px.le16(1);   // (an integer format's missing alpha is 1)
      }
    }
  return r.ok();
}

}  // namespace dxgk
