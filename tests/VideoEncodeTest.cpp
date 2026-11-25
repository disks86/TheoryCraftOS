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
// Host tests of what Direct3D 12's video encoding and motion estimation run on (src/Hardware/VideoCodec/Software): the block motion estimator (known whole and quarter
// sample shifts of a textured picture give exactly those vectors), the H.264 picture encoder driven as a D3D12 program drives it (the test keeps the decoded picture buffer
// and writes the parameter sets) with every picture decoded again by the in-tree decoder (H264Decoder::decode_external, references named by the test) and compared with the
// encoder's reconstruction sample for sample - I/P/B, several references, long-term references with marking and list modification, the slice layouts, the in-loop filter
// modes, constrained intra prediction, QP maps - and the rate controller (CBR / VBR / QVBR reach their rates). Encoder inputs that are not valid are refused.
// `VideoEncodeTest <dir>` also writes <dir>/stream.h264 and <dir>/recon.yuv (the reconstruction in display order, I420) for tests/h264_encode_ffmpeg.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/H264PictureEncoder.hpp"
#include "Hardware/VideoCodec/Software/MotionEstimator.hpp"
#include "Hardware/VideoCodec/Software/VideoRateControl.hpp"
#include "Kernel/Dxgkrnl/VideoEncode.hpp"

using namespace hardware;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al + al);
  }
  void deallocate(void* p) override { free(p); }
};
static TestAllocator g_alloc;

static uint32_t g_seed = 12345;
static uint32_t rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

// A smooth random texture (noise blurred twice), big enough to cut moving pictures out of.
struct Texture {
  int w, h;
  std::vector<uint8_t> s;
  Texture(int w_, int h_, int detail) : w(w_), h(h_), s(size_t(w_) * h_) {
    std::vector<int> a(s.size());
    for (auto& v : a) v = int(rnd() % 256);
    for (int pass = 0; pass < detail; ++pass) {
      std::vector<int> b(a.size());
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          int sum = 0, n = 0;
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              const int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
              sum += a[size_t(yy) * w + xx];
              ++n;
            }
          b[size_t(y) * w + x] = sum / n;
        }
      a = b;
    }
    // stretch the contrast back
    for (size_t i = 0; i < s.size(); ++i) s[i] = uint8_t(std::clamp((a[i] - 128) * 4 + 128, 0, 255));
  }
  uint8_t at(int x, int y) const { return s[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; }
};

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// Motion estimation

static void test_motion_estimation() {
  Texture t(160, 128, 2);
  const int W = 96, H = 64;
  std::vector<uint8_t> ref(W * H), cur(W * H);
  // whole samples: current(x, y) = reference(x + 5, y - 3)
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      ref[size_t(y) * W + x] = t.at(x + 20, y + 20);
      cur[size_t(y) * W + x] = t.at(x + 25, y + 17);
    }
  for (int block : {8, 16}) {
    const int cols = (W + block - 1) / block, rows = (H + block - 1) / block;
    std::vector<MotionBlockResult> out(size_t(cols) * rows);
    const LumaPlane c{ConstByteView(cur.data(), cur.size()), W, H, W}, r{ConstByteView(ref.data(), ref.size()), W, H, W};
    CHECK(estimate_motion(c, r, block, {}, ArrayView<MotionBlockResult>(out.data(), out.size())));
    int exact = 0, interior = 0;
    for (int by = 0; by < rows; ++by)
      for (int bx = 0; bx < cols; ++bx) {
        // (blocks whose match lies inside the reference)
        if (bx * block + 5 < 0 || (bx + 1) * block + 5 > W || by * block - 3 < 0) continue;
        ++interior;
        const MotionBlockResult& m = out[size_t(by) * cols + bx];
        exact += m.vector.x == 20 && m.vector.y == -12 && m.sad == 0;
      }
    printf("motion estimation %dx%d, whole shift: %d of %d inner blocks exact\n", block, block, exact, interior);
    CHECK(interior > 0 && exact == interior);
  }
  // quarter samples: current is the reference interpolated at (+1/4, +1/2), as H.264 interpolates
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) ref[size_t(y) * W + x] = t.at(x + 30, y + 30);
  const LumaPlane r{ConstByteView(ref.data(), ref.size()), W, H, W};
  for (int y = 0; y < H; y += 16)
    for (int x = 0; x < W; x += 16) {
      uint8_t blk[256];
      h264_luma_prediction(r, x - 2, y + 1, 1, 2, 16, 16, ByteView(blk, 256));
      for (int yy = 0; yy < 16; ++yy) memcpy(&cur[size_t(y + yy) * W + x], blk + yy * 16, 16);
    }
  {
    const int cols = W / 16, rows = H / 16;
    std::vector<MotionBlockResult> out(size_t(cols) * rows);
    std::vector<MotionVector> hints(out.size());
    for (auto& h : hints) h = {-8, 4};
    const LumaPlane c{ConstByteView(cur.data(), cur.size()), W, H, W};
    CHECK(estimate_motion(c, r, 16, ArrayView<const MotionVector>(hints.data(), hints.size()), ArrayView<MotionBlockResult>(out.data(), out.size())));
    int exact = 0, interior = 0;
    for (int by = 0; by < rows; ++by)
      for (int bx = 0; bx < cols; ++bx) {
        if (bx == 0 || by == 0 || by + 1 == rows) continue;   // (the edge blocks' matches reach outside, where the edge is repeated)
        ++interior;
        const MotionBlockResult& m = out[size_t(by) * cols + bx];
        exact += m.vector.x == -7 && m.vector.y == 6 && m.sad == 0;
      }
    printf("motion estimation 16x16, quarter shift (-1.75, 1.5): %d of %d inner blocks exact\n", exact, interior);
    CHECK(interior > 0 && exact == interior);
  }
  // sizes that do not fit are refused
  std::vector<MotionBlockResult> small(2);
  const LumaPlane c{ConstByteView(cur.data(), cur.size()), W, H, W};
  CHECK(!estimate_motion(c, r, 16, {}, ArrayView<MotionBlockResult>(small.data(), small.size())));
  CHECK(!estimate_motion(c, r, 4, {}, ArrayView<MotionBlockResult>(small.data(), small.size())));
  const LumaPlane short_plane{ConstByteView(cur.data(), 100), W, H, W};
  std::vector<MotionBlockResult> out(64);
  CHECK(!estimate_motion(short_plane, r, 16, {}, ArrayView<MotionBlockResult>(out.data(), out.size())));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The program's side of H.264 encoding: parameter sets, the decoded picture buffer, decoding back

struct Bits {
  std::vector<uint8_t> b;
  int n = 0;
  void bit(unsigned v) { if (n % 8 == 0) b.push_back(0); if (v & 1) b.back() |= uint8_t(0x80 >> (n % 8)); ++n; }
  void put(unsigned v, int len) { for (int i = len - 1; i >= 0; --i) bit(v >> i); }
  void ue(unsigned v) { unsigned x = v + 1; int len = 0; while ((x >> (len + 1)) != 0) ++len; put(0, len); put(x, len + 1); }
  void se(int v) { ue(v > 0 ? unsigned(2 * v - 1) : unsigned(-2 * v)); }
  void trailing() { bit(1); while (n % 8) bit(0); }
};
static void nal(std::vector<uint8_t>& out, int ref_idc, int type, const std::vector<uint8_t>& rbsp) {
  const uint8_t start[4] = {0, 0, 0, 1};
  out.insert(out.end(), start, start + 4);
  out.push_back(uint8_t((ref_idc << 5) | type));
  int zeros = 0;
  for (uint8_t v : rbsp) {
    if (zeros >= 2 && v <= 3) { out.push_back(3); zeros = 0; }
    out.push_back(v);
    zeros = v == 0 ? zeros + 1 : 0;
  }
}

struct Sequence {
  int width, height;          // the visible size
  int mbw, mbh;
  bool high = false, constrained = false, cabac = false, t8x8 = false;
  int poc_type = 2, log2_frame_num = 4, log2_poc_lsb = 6;
  std::vector<uint8_t> headers;
  void write_headers() {
    headers.clear();
    Bits s;
    s.put(high ? 100 : 77, 8);
    s.put(0, 8);
    s.put(31, 8);
    s.ue(0);
    if (high) { s.ue(1); s.ue(0); s.ue(0); s.bit(0); s.bit(0); }
    s.ue(unsigned(log2_frame_num - 4));
    s.ue(unsigned(poc_type));
    if (poc_type == 0) s.ue(unsigned(log2_poc_lsb - 4));
    s.ue(16);     // max_num_ref_frames
    s.bit(0);
    s.ue(unsigned(mbw - 1));
    s.ue(unsigned(mbh - 1));
    s.bit(1);     // frame_mbs_only_flag
    s.bit(1);     // direct_8x8_inference_flag
    const int cr = (mbw * 16 - width) / 2, cb = (mbh * 16 - height) / 2;
    if (cr || cb) { s.bit(1); s.ue(0); s.ue(unsigned(cr)); s.ue(0); s.ue(unsigned(cb)); } else s.bit(0);
    s.bit(0);     // vui
    s.trailing();
    nal(headers, 3, 7, s.b);
    Bits p;
    p.ue(0); p.ue(0);
    p.bit(cabac ? 1 : 0);     // entropy_coding_mode_flag
    p.bit(0); p.ue(0); p.ue(0); p.ue(0); p.bit(0); p.put(0, 2);
    p.se(0); p.se(0); p.se(0);
    p.bit(1);     // deblocking_filter_control_present_flag
    p.bit(constrained ? 1 : 0);
    p.bit(0);
    if (high) { p.bit(t8x8 ? 1 : 0); p.bit(0); p.se(0); }
    p.trailing();
    nal(headers, 3, 8, p.b);
  }
};

struct Picture {
  int width = 0, height = 0;
  std::vector<uint8_t> y, u, v;
  void make(int w, int h) { width = w; height = h; y.assign(size_t(w) * h, 0); u.assign(size_t(w) * h / 4, 0); v.assign(size_t(w) * h / 4, 0); }
  H264EncPlanes planes() const { return {ConstByteView(y.data(), y.size()), ConstByteView(u.data(), u.size()), ConstByteView(v.data(), v.size()), uint32_t(width), uint32_t(height)}; }
  H264EncWritablePlanes writable() { return {ByteView(y.data(), y.size()), ByteView(u.data(), u.size()), ByteView(v.data(), v.size()), uint32_t(width), uint32_t(height)}; }
};

// Frame t of a scene: the texture moving 1.5 samples right and 0.75 down a frame over a still background (the lowest quarter of the picture, macroblock aligned),
// chroma the texture too.
static void make_source(const Texture& tex, int t, Picture* p, int visible_w, int visible_h) {
  const int still = (visible_h * 3 / 4) & ~15;
  for (int y = 0; y < p->height; ++y)
    for (int x = 0; x < p->width; ++x) {
      const int sx = std::min(x, visible_w - 1), sy = std::min(y, visible_h - 1);
      const int tt = sy >= still ? 0 : t;
      const int fx = sx * 4 + tt * 6, fy = sy * 4 + tt * 3;   // quarter samples
      const int a = tex.at(fx / 4, fy / 4), b = tex.at(fx / 4 + 1, fy / 4), c = tex.at(fx / 4, fy / 4 + 1), d = tex.at(fx / 4 + 1, fy / 4 + 1);
      const int wx = fx & 3, wy = fy & 3;
      p->y[size_t(y) * p->width + x] = uint8_t(((4 - wx) * (4 - wy) * a + wx * (4 - wy) * b + (4 - wx) * wy * c + wx * wy * d + 8) / 16);
    }
  // (chroma: the texture again, moving with it, at half resolution and contrast)
  for (int y = 0; y < p->height / 2; ++y)
    for (int x = 0; x < p->width / 2; ++x) {
      const int sx = std::min(2 * x, visible_w - 1), sy = std::min(2 * y, visible_h - 1);
      const int tt = sy >= still ? 0 : t;
      const int fx = sx * 4 + tt * 6, fy = sy * 4 + tt * 3;
      p->u[size_t(y) * (p->width / 2) + x] = uint8_t(64 + tex.at(fx / 4 + 40, fy / 4) / 2);
      p->v[size_t(y) * (p->width / 2) + x] = uint8_t(192 - tex.at(fx / 4, fy / 4 + 40) / 2);
    }
}

static double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h, int stride) {
  double se = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) { const double d = double(a[size_t(y) * stride + x]) - b[size_t(y) * stride + x]; se += d * d; }
  return se == 0 ? 99.0 : 10 * log10(255.0 * 255.0 * w * h / se);
}

// One stored picture of the program's buffer.
struct Stored {
  Picture recon;
  std::vector<H264EncMbMotion> motion;
  uint32_t clean_rows = 0xFFFFFFFFu;
  uint32_t key = 0;
  int frame_num = 0, poc = 0;
  bool long_term = false;
  int long_term_idx = 0;
};

// What a picture of a test sequence is.
struct Step {
  H264EncFrameType type;
  int display;                         // its place in display order (the POC is 2 x this)
  std::vector<int> list0 = {}, list1 = {};       // indices into the stored pictures
  bool reference = true;
  std::vector<H264EncMarking> marking = {};
  std::vector<H264EncListModification> mod0 = {};
  int long_term_store = -1;            // the marking makes the current picture long term with this index
  std::vector<int> drop = {};               // stored pictures that the marking drops (or that leave the window)
};

struct Encoded {
  std::vector<uint8_t> stream;         // parameter sets + every slice
  std::vector<std::pair<int, Picture>> recon, source;   // (display index, picture)
  int mismatches = 0, decoded = 0;
  double psnr_min = 99;
  uint64_t bits = 0;
};

struct Options {
  H264EncSliceMode slices = H264EncSliceMode::FullFrame;
  uint32_t slice_value = 0;
  uint8_t deblocking = 0;
  uint8_t qp = 28;
  bool qp_map = false;
  RateControlConfig* rc = nullptr;
  H264EncPrecision precision = H264EncPrecision::Quarter;
  H264EncDirect direct = H264EncDirect::None;
  int refresh_period = 0;            // intra refresh: the waves' length in pictures (0: none)
  bool unchanged = false;            // every third macroblock of P pictures marked unchanged from list 0 entry 0
  bool hints = false;                // hints: the true motion of the scene, into list 0 entry 0
  const char* dump = nullptr;        // a name: the stream and the reconstruction go to <g_dump>/<name>.h264 / .yuv for tests/h264_encode_ffmpeg.sh
};
static const char* g_dump = nullptr;
struct Totals {
  uint64_t skipped = 0, intra = 0, inter = 0, slices = 0, direct = 0;
};

static Encoded run_sequence(Sequence seq, const std::vector<Step>& steps, const Options& opt, Totals* totals = nullptr) {
  Encoded e;
  seq.mbw = (seq.width + 15) / 16;
  seq.mbh = (seq.height + 15) / 16;
  seq.write_headers();
  e.stream = seq.headers;
  const int W = seq.mbw * 16, H = seq.mbh * 16;
  Texture tex(W + 128, H + 128, 2);
  H264PictureEncoder enc(g_alloc);
  H264Decoder dec(g_alloc);
  std::vector<Stored> dpb;
  uint32_t next_key = 1, idr_id = 0;
  int frame_num = 0;
  RateControlState rcs;
  for (const Step& st : steps) {
    Picture src;
    src.make(W, H);
    make_source(tex, st.display, &src, seq.width, seq.height);
    Stored cur;
    cur.recon.make(W, H);
    const bool idr = st.type == H264EncFrameType::Idr;
    if (idr) { frame_num = 0; dpb.clear(); }
    cur.frame_num = frame_num;
    cur.poc = 2 * st.display;
    std::vector<H264EncReference> l0, l1;
    auto entry = [&](const Stored& s) {
      H264EncReference r{s.recon.planes(), s.key, 0, false, {}, 0xFFFFFFFFu};
      r.poc = s.poc;
      r.long_term = s.long_term;
      r.motion = ArrayView<const H264EncMbMotion>(s.motion.data(), s.motion.size());
      r.clean_rows = s.clean_rows;
      return r;
    };
    for (int i : st.list0) l0.push_back(entry(dpb[size_t(i)]));
    for (int i : st.list1) l1.push_back(entry(dpb[size_t(i)]));
    H264EncPicture p;
    p.mb_width = uint32_t(seq.mbw);
    p.mb_height = uint32_t(seq.mbh);
    p.constrained_intra_pred = seq.constrained;
    p.disable_deblocking_filter_idc = opt.deblocking;
    p.log2_max_frame_num = uint8_t(seq.log2_frame_num);
    p.pic_order_cnt_type = uint8_t(seq.poc_type);
    p.log2_max_poc_lsb = uint8_t(seq.log2_poc_lsb);
    p.type = st.type;
    p.idr_pic_id = idr_id;
    p.frame_num = uint32_t(frame_num);
    p.poc = uint32_t(cur.poc);
    p.reference = st.reference;
    p.adaptive_ref_pic_marking = !st.marking.empty();
    p.marking = ArrayView<const H264EncMarking>(st.marking.data(), st.marking.size());
    p.list[0] = ArrayView<const H264EncReference>(l0.data(), l0.size());
    p.list[1] = ArrayView<const H264EncReference>(l1.data(), l1.size());
    p.modification[0] = ArrayView<const H264EncListModification>(st.mod0.data(), st.mod0.size());
    p.slice_mode = opt.slices;
    p.slice_value = opt.slice_value;
    p.precision = opt.precision;
    p.entropy = seq.cabac ? H264EncEntropy::Cabac : H264EncEntropy::Cavlc;
    p.transform_8x8 = seq.t8x8;
    p.direct = opt.direct;
    const size_t step_index = size_t(&st - steps.data());
    if (opt.refresh_period > 0 && st.type != H264EncFrameType::Idr && st.type != H264EncFrameType::I) {
      const int index = int((step_index - 1) % size_t(opt.refresh_period));
      p.refresh = true;
      p.refresh_begin = uint32_t(index * seq.mbh / opt.refresh_period);
      p.refresh_end = uint32_t((index + 1) * seq.mbh / opt.refresh_period);
      if (p.refresh_end <= p.refresh_begin) p.refresh = false;
    }
    std::vector<uint8_t> unchanged;
    if (opt.unchanged && st.type == H264EncFrameType::P) {
      for (int i = 0; i < seq.mbw * seq.mbh; ++i) unchanged.push_back(i % 3 == 0);
      p.unchanged = ArrayView<const uint8_t>(unchanged.data(), unchanged.size());
    }
    std::vector<H264EncHint> hints;
    if (opt.hints && !l0.empty()) {
      // (the scene moves 1.5 samples right and 0.75 down a picture: the hint is the true motion from list 0 entry 0)
      const int dt = st.display - dpb[size_t(st.list0[0])].poc / 2;
      for (int i = 0; i < seq.mbw * seq.mbh; ++i) {
        H264EncHint h;
        h.ref[0] = 0;
        h.mv[0][0] = int16_t(6 * dt);
        h.mv[0][1] = int16_t(3 * dt);
        hints.push_back(h);
      }
      p.hints = ArrayView<const H264EncHint>(hints.data(), hints.size());
      p.hint_mode = H264EncHintMode::StartHintLimited;
      p.hint_limit = 2;
    }
    cur.motion.assign(size_t(seq.mbw) * seq.mbh, H264EncMbMotion());
    std::vector<H264EncMbReport> report(size_t(seq.mbw) * seq.mbh);
    H264EncOutputs outputs{ArrayView<H264EncMbMotion>(cur.motion.data(), cur.motion.size()), ArrayView<H264EncMbReport>(report.data(), report.size())};
    H264EncStatistics stats;
    std::vector<int8_t> map;
    if (opt.qp_map) {
      for (int i = 0; i < seq.mbw * seq.mbh; ++i) map.push_back(int8_t((i % 3) * 4 - 4));
      p.qp_map = ArrayView<const int8_t>(map.data(), map.size());
    }
    const RateControlPicture kind = st.type == H264EncFrameType::B ? RateControlPicture::B : st.type == H264EncFrameType::P ? RateControlPicture::P : RateControlPicture::Intra;
    uint32_t qp = opt.rc ? rate_control_qp(*opt.rc, rcs, kind, uint64_t(W) * H) : opt.qp;
    ByteBuffer out(g_alloc);
    List<H264EncSlice> slices(g_alloc);
    for (uint32_t attempt = 0;; ++attempt) {
      out.clear();
      p.qp = uint8_t(qp);
      const H264EncResult r = enc.encode(p, src.planes(), cur.recon.writable(), &out, &slices, &stats, outputs);
      CHECK(r == H264EncResult::Ok);
      if (r != H264EncResult::Ok) return e;
      uint32_t retry = qp;
      if (!opt.rc || attempt >= kRateControlMaxRetries || rate_control_accept(*opt.rc, rcs, kind, qp, out.size() * 8, &retry)) break;
      qp = retry;
    }
    if (opt.rc) rate_control_update(*opt.rc, rcs, kind, qp, out.size() * 8);
    e.bits += out.size() * 8;
    if (opt.slices != H264EncSliceMode::BytesPerSlice) CHECK(slices.count() == H264PictureEncoder::slice_count(opt.slices, opt.slice_value, uint32_t(seq.mbw), uint32_t(seq.mbh)));
    uint64_t at = 0;
    uint32_t next_mb = 0;
    for (const H264EncSlice& s : slices.view()) {
      CHECK(s.offset == at && s.size > 5 && s.header_bits > 40 && s.header_bits < s.size * 8 && s.first_mb == next_mb && s.mb_count > 0);
      if (opt.slices == H264EncSliceMode::BytesPerSlice && s.mb_count > 1) CHECK(s.size <= opt.slice_value);
      at += s.size;
      next_mb += s.mb_count;
    }
    CHECK(at == out.size() && next_mb == uint32_t(seq.mbw * seq.mbh));
    cur.clean_rows = stats.clean_rows;
    // the report: the QPs, and the bits adding up to about the slice data
    uint64_t report_bits = 0;
    for (const H264EncMbReport& r : report) { CHECK(r.qp <= 51); report_bits += r.bits; }
    CHECK(report_bits <= out.size() * 8);
    if (opt.unchanged && st.type == H264EncFrameType::P)
      for (int i = 0; i < seq.mbw * seq.mbh; i += 3) CHECK(!cur.motion[size_t(i)].intra && cur.motion[size_t(i)].ref[0][0] == 0 && cur.motion[size_t(i)].mv[0][0][0] == 0 && cur.motion[size_t(i)].mv[0][0][1] == 0);
    if (p.refresh)
      for (uint32_t y = p.refresh_begin; y < p.refresh_end; ++y)
        for (int x = 0; x < seq.mbw; ++x) CHECK(cur.motion[size_t(y) * seq.mbw + x].intra);
    if (totals) {
      totals->skipped += stats.skipped; totals->intra += stats.intra; totals->inter += stats.inter; totals->slices += slices.count(); totals->direct += stats.direct;
    }
    // decode it again with the program's references
    std::vector<uint8_t> au = seq.headers;
    au.insert(au.end(), out.view().data(), out.view().data() + out.size());
    e.stream.insert(e.stream.end(), out.view().data(), out.view().data() + out.size());
    std::vector<H264Decoder::ExternalReference> refs;
    for (const Stored& s : dpb) {
      H264Decoder::ExternalReference r;
      r.key = s.key;
      r.long_term = s.long_term;
      r.frame_num = s.long_term ? s.long_term_idx : s.frame_num;
      r.top_poc = r.bottom_poc = s.poc;
      refs.push_back(r);
    }
    cur.key = next_key++;
    H264Decoder::ExternalPicture ep;
    ep.key = cur.key;
    ep.top_poc = ep.bottom_poc = cur.poc;
    ep.references = ArrayView<const H264Decoder::ExternalReference>(refs.data(), refs.size());
    VideoFrame f;
    const auto status = dec.decode_external(ConstByteView(au.data(), au.size()), ep, &f);
    CHECK(status == H264Decoder::ExternalStatus::Decoded);
    if (status == H264Decoder::ExternalStatus::Decoded) {
      ++e.decoded;
      bool same = int(f.format.width) == W && int(f.format.height) == H;
      for (int y = 0; same && y < H; ++y) same = memcmp(f.plane[0] + size_t(y) * f.stride[0], &cur.recon.y[size_t(y) * W], size_t(W)) == 0;
      for (int y = 0; same && y < H / 2; ++y)
        same = memcmp(f.plane[1] + size_t(y) * f.stride[1], &cur.recon.u[size_t(y) * W / 2], size_t(W / 2)) == 0 && memcmp(f.plane[2] + size_t(y) * f.stride[2], &cur.recon.v[size_t(y) * W / 2], size_t(W / 2)) == 0;
      if (!same) ++e.mismatches;
    }
    e.psnr_min = std::min(e.psnr_min, psnr(src.y, cur.recon.y, seq.width, seq.height, W));
    e.recon.push_back({st.display, cur.recon});
    e.source.push_back({st.display, src});
    // the program's buffer: marking, then the picture itself
    if (idr) ++idr_id;
    std::vector<int> drop = st.drop;
    std::sort(drop.rbegin(), drop.rend());
    for (int d : drop) dpb.erase(dpb.begin() + d);
    if (st.reference || idr) {
      if (st.long_term_store >= 0) { cur.long_term = true; cur.long_term_idx = st.long_term_store; }
      dpb.push_back(cur);
      frame_num = (frame_num + 1) % (1 << seq.log2_frame_num);
    }
  }
  if (g_dump && opt.dump) {
    const std::string base = std::string(g_dump) + "/" + opt.dump;
    FILE* f = fopen((base + ".h264").c_str(), "wb");
    if (f) { fwrite(e.stream.data(), 1, e.stream.size(), f); fclose(f); }
    for (int which = 0; which < 2; ++which) {
      auto pics = which == 0 ? e.recon : e.source;
      std::sort(pics.begin(), pics.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      f = fopen((base + (which == 0 ? ".recon.yuv" : ".source.yuv")).c_str(), "wb");
      for (const auto& r : pics) {
        const Picture& pp = r.second;
        for (int y = 0; f && y < seq.height; ++y) fwrite(&pp.y[size_t(y) * pp.width], 1, size_t(seq.width), f);
        for (int y = 0; f && y < (seq.height + 1) / 2; ++y) fwrite(&pp.u[size_t(y) * pp.width / 2], 1, size_t((seq.width + 1) / 2), f);
        for (int y = 0; f && y < (seq.height + 1) / 2; ++y) fwrite(&pp.v[size_t(y) * pp.width / 2], 1, size_t((seq.width + 1) / 2), f);
      }
      if (f) fclose(f);
    }
    f = fopen((base + ".size").c_str(), "wb");
    if (f) { fprintf(f, "%dx%d %zu\n", seq.width, seq.height, e.recon.size()); fclose(f); }
  }
  return e;
}

static void report(const char* name, const Encoded& e, size_t pictures) {
  printf("%s: %d of %zu pictures decode to the reconstruction (%d differ), %llu bytes, PSNR >= %.1f dB\n", name, e.decoded - e.mismatches, pictures, e.mismatches,
         (unsigned long long)(e.bits / 8), e.psnr_min);
  CHECK(e.decoded == int(pictures) && e.mismatches == 0);
}

static void test_encoder() {
  // 1. IDR + P, one slice, filter on, picture order count type 2, a visible size that is not whole macroblocks (cropped)
  {
    Sequence s{};
    s.width = 120; s.height = 72;
    Encoded e = run_sequence(s, {{H264EncFrameType::Idr, 0, {}, {}}, {H264EncFrameType::P, 1, {0}, {}}, {H264EncFrameType::P, 2, {1, 0}, {}}, {H264EncFrameType::P, 3, {2, 1, 0}, {}},
                                 {H264EncFrameType::I, 4, {}, {}}, {H264EncFrameType::P, 5, {4, 3, 2, 1}, {}}},
                             Options{});
    report("IDR P P P I P, several references", e, 6);
    CHECK(e.psnr_min > 30);
  }
  // 2. IDR P B B P B with POC type 0, slices of 7 macroblocks, filter not across slices
  std::vector<Step> gop = {{H264EncFrameType::Idr, 0, {}, {}},
                           {H264EncFrameType::P, 3, {0}, {}},
                           {H264EncFrameType::B, 1, {0, 1}, {1, 0}, false},
                           {H264EncFrameType::B, 2, {0, 1}, {1, 0}, false},
                           {H264EncFrameType::P, 6, {1, 0}, {}},
                           {H264EncFrameType::B, 4, {1, 0}, {2, 1}, false},
                           {H264EncFrameType::B, 5, {1, 0}, {2, 1}, false}};
  Sequence sb{};
  sb.width = 96; sb.height = 64; sb.poc_type = 0;
  {
    Options o;
    o.slices = H264EncSliceMode::MacroblocksPerSlice;
    o.slice_value = 7;
    o.deblocking = 2;
    Encoded e = run_sequence(sb, gop, o);
    report("IDR P B B P B B, 7 macroblocks a slice, idc 2", e, gop.size());
  }
  // 3. the same, rows a slice, filter off, High profile headers, a QP map of deltas
  {
    Options o;
    o.slices = H264EncSliceMode::RowsPerSlice;
    o.slice_value = 2;
    o.deblocking = 1;
    o.qp_map = true;
    Sequence s = sb;
    s.high = true;
    Encoded e = run_sequence(s, gop, o);
    report("IDR P B B P B B, 2 rows a slice, idc 1, QP deltas, High", e, gop.size());
  }
  // 4. three slices a picture, constrained intra prediction, half-sample precision
  {
    Options o;
    o.slices = H264EncSliceMode::SlicesPerFrame;
    o.slice_value = 3;
    o.precision = H264EncPrecision::Half;
    o.dump = "cavlc_slices";
    Sequence s = sb;
    s.constrained = true;
    Encoded e = run_sequence(s, gop, o);
    report("3 slices, constrained intra, half samples", e, gop.size());
  }
  // 5. long-term references: the second picture marks itself long term (MMCO 6), later pictures reach it through a list modification
  {
    Sequence s{};
    s.width = 64; s.height = 48; s.poc_type = 0;
    H264EncMarking to_long{6, 0, 0, 0, 0}, max_idx{4, 0, 0, 0, 1};
    std::vector<Step> steps = {{H264EncFrameType::Idr, 0, {}, {}},
                               {H264EncFrameType::P, 1, {0}, {}, true, {max_idx, to_long}, {}, 0},
                               {H264EncFrameType::P, 2, {0, 1}, {}},            // default order: short term (0) first, then the long-term one
                               {H264EncFrameType::P, 3, {1, 2}, {}, true, {}, {{2, 0, 0}}},   // the long-term one moved to the front (long_term_pic_num 0)
                               {H264EncFrameType::B, 4, {2, 1}, {2}, false, {}, {{2, 0, 0}}}};
    // (stored after 1: [IDR, LT], after 2: [IDR, LT, P2], step 3's lists: index 1 = LT, 2 = P2; default list0 would be P2, IDR, LT)
    steps[3].list0 = {1, 2, 0};
    steps[3].mod0 = {{2, 0, 0}};
    steps[4] = {H264EncFrameType::B, 3, {2, 0, 3}, {3}, false};
    steps[3].display = 4;
    Encoded e = run_sequence(s, steps, Options{});
    report("long-term reference: MMCO 4 + 6, list modification", e, steps.size());
  }
  // 6. rate control: CBR, VBR and QVBR over 30 pictures of IPPP...
  for (RateControlMode mode : {RateControlMode::Cbr, RateControlMode::Vbr, RateControlMode::Qvbr}) {
    Sequence s{};
    s.width = 96; s.height = 64;
    std::vector<Step> steps;
    for (int i = 0; i < 30; ++i) steps.push_back(i == 0 ? Step{H264EncFrameType::Idr, 0, {}, {}} : Step{H264EncFrameType::P, i, {std::min(i, 3) - 1}, {}, true, {}, {}, -1, i >= 3 ? std::vector<int>{0} : std::vector<int>{}});
    RateControlConfig rc;
    rc.mode = mode;
    rc.fps_numerator = 30;
    rc.fps_denominator = 1;
    rc.target_bitrate = 300000;
    rc.peak_bitrate = 600000;
    rc.quality_target = 20;
    rc.flags = kRateControlQpRange;
    rc.min_qp = 10;
    rc.max_qp = 51;
    CHECK(rate_control_valid(rc));
    Options o;
    o.rc = &rc;
    Encoded e = run_sequence(s, steps, o);
    const double rate = double(e.bits) * 30 / 30;
    printf("rate control %s: %.0f bits/s for a target of 300000\n", mode == RateControlMode::Cbr ? "CBR" : mode == RateControlMode::Vbr ? "VBR" : "QVBR", rate);
    CHECK(e.mismatches == 0 && e.decoded == 30);
    CHECK(rate > 300000 * 0.75 && rate < 300000 * 1.2);
  }
  // CQP: the QP of each picture's type
  {
    RateControlConfig rc;
    rc.mode = RateControlMode::Cqp;
    rc.qp_intra = 20; rc.qp_p = 24; rc.qp_b = 30;
    RateControlState st;
    CHECK(rate_control_qp(rc, st, RateControlPicture::Intra, 100) == 20 && rate_control_qp(rc, st, RateControlPicture::P, 100) == 24 && rate_control_qp(rc, st, RateControlPicture::B, 100) == 30);
    rc.qp_b = 60;
    CHECK(!rate_control_valid(rc));
    RateControlConfig bad;
    bad.mode = RateControlMode::Cbr;
    CHECK(!rate_control_valid(bad));       // (no rate)
    bad.target_bitrate = 1000;
    bad.fps_numerator = 0;
    CHECK(!rate_control_valid(bad));
  }
}

// CABAC, the 8x8 transform and Intra8x8, direct prediction (spatial, temporal) with B_Skip, intra refresh, slices by bytes, cropped sizes, unchanged macroblocks and
// motion hints: each sequence decoded again picture by picture (and dumped for tests/h264_encode_ffmpeg.sh).
static void test_new_tools() {
  std::vector<Step> gop = {{H264EncFrameType::Idr, 0, {}, {}},
                           {H264EncFrameType::P, 3, {0}, {}},
                           {H264EncFrameType::B, 1, {0, 1}, {1, 0}, false},
                           {H264EncFrameType::B, 2, {0, 1}, {1, 0}, false},
                           {H264EncFrameType::P, 6, {1, 0}, {}},
                           {H264EncFrameType::B, 4, {1, 0}, {2, 1}, false},
                           {H264EncFrameType::B, 5, {1, 0}, {2, 1}, false}};
  Sequence base{};
  base.width = 96; base.height = 64; base.poc_type = 0; base.high = true;
  struct Case {
    const char* name;
    bool cabac, t8x8, constrained;
    H264EncDirect direct;
    int width, height;
    H264EncSliceMode slices;
    uint32_t slice_value;
    uint8_t deblocking;
  };
  const Case cases[] = {
      {"cabac", true, false, false, H264EncDirect::None, 96, 64, H264EncSliceMode::RowsPerSlice, 2, 0},
      {"cavlc_8x8", false, true, false, H264EncDirect::None, 96, 64, H264EncSliceMode::FullFrame, 0, 0},
      {"cabac_8x8", true, true, false, H264EncDirect::None, 96, 64, H264EncSliceMode::MacroblocksPerSlice, 9, 2},
      {"cavlc_spatial", false, false, false, H264EncDirect::Spatial, 96, 64, H264EncSliceMode::FullFrame, 0, 0},
      {"cabac_8x8_spatial", true, true, false, H264EncDirect::Spatial, 96, 64, H264EncSliceMode::FullFrame, 0, 0},
      {"cavlc_8x8_temporal", false, true, false, H264EncDirect::Temporal, 96, 64, H264EncSliceMode::SlicesPerFrame, 2, 0},
      {"cabac_temporal_odd", true, false, true, H264EncDirect::Temporal, 100, 60, H264EncSliceMode::FullFrame, 0, 0},
      {"cabac_8x8_spatial_odd", true, true, true, H264EncDirect::Spatial, 118, 46, H264EncSliceMode::FullFrame, 0, 1},
      {"bytes_cavlc", false, true, false, H264EncDirect::Spatial, 96, 64, H264EncSliceMode::BytesPerSlice, 160, 0},
      {"bytes_cabac", true, true, false, H264EncDirect::Temporal, 96, 64, H264EncSliceMode::BytesPerSlice, 160, 2},
  };
  for (const Case& c : cases) {
    Sequence s = base;
    s.cabac = c.cabac; s.t8x8 = c.t8x8; s.constrained = c.constrained; s.width = c.width; s.height = c.height;
    Options o;
    o.direct = c.direct;
    o.slices = c.slices;
    o.slice_value = c.slice_value;
    o.deblocking = c.deblocking;
    o.dump = c.name;
    Totals t;
    Encoded e = run_sequence(s, gop, o, &t);
    report(c.name, e, gop.size());
    printf("  %llu slices, %llu intra, %llu inter, %llu skipped, %llu direct macroblocks\n", (unsigned long long)t.slices, (unsigned long long)t.intra,
           (unsigned long long)t.inter, (unsigned long long)t.skipped, (unsigned long long)t.direct);
    CHECK(e.psnr_min > 30);
    if (c.slices == H264EncSliceMode::BytesPerSlice) CHECK(t.slices > gop.size());
    CHECK(t.skipped > 0);
    if (c.direct != H264EncDirect::None) CHECK(t.direct > 0);
  }
  // CABAC and CAVLC with the 8x8 transform at a low QP (big levels: escapes, the CAVLC level limit) and a high one
  for (uint8_t qp : {6, 46}) {
    Sequence s = base;
    s.cabac = qp == 6; s.t8x8 = true;
    Options o;
    o.qp = qp;
    o.direct = H264EncDirect::Spatial;
    Encoded e = run_sequence(s, gop, o);
    report(qp == 6 ? "CABAC 8x8 at QP 6" : "CAVLC 8x8 at QP 46", e, gop.size());
  }
  // intra refresh: an IDR picture, then waves of 4 P pictures refreshing a quarter of the rows each
  {
    Sequence s = base;
    s.cabac = true; s.t8x8 = true; s.width = 96; s.height = 128;
    std::vector<Step> steps;
    for (int i = 0; i < 10; ++i) steps.push_back(i == 0 ? Step{H264EncFrameType::Idr, 0, {}, {}} : Step{H264EncFrameType::P, i, {std::min(i, 2) - 1}, {}, true, {}, {}, -1, i >= 2 ? std::vector<int>{0} : std::vector<int>{}});
    Options o;
    o.refresh_period = 4;
    o.dump = "refresh";
    Encoded e = run_sequence(s, steps, o);
    report("intra refresh, waves of 4 pictures", e, steps.size());
  }
  // unchanged macroblocks (dirty regions) and motion hints
  {
    Sequence s = base;
    s.cabac = true;
    std::vector<Step> steps;
    for (int i = 0; i < 5; ++i) steps.push_back(i == 0 ? Step{H264EncFrameType::Idr, 0, {}, {}} : Step{H264EncFrameType::P, i, {std::min(i, 2) - 1}, {}, true, {}, {}, -1, i >= 2 ? std::vector<int>{0} : std::vector<int>{}});
    Options o;
    o.unchanged = true;
    o.dump = "unchanged";
    report("unchanged macroblocks", run_sequence(s, steps, o), steps.size());
    Options h;
    h.hints = true;
    h.dump = "hints";
    Encoded e = run_sequence(s, steps, h);
    report("motion hints, limited to 2 samples", e, steps.size());
    CHECK(e.psnr_min > 30);
  }
}

// Pictures the encoder must refuse.
static void test_refusals() {
  H264PictureEncoder enc(g_alloc);
  Picture src, rec, small;
  src.make(32, 32);
  rec.make(32, 32);
  small.make(16, 16);
  ByteBuffer out(g_alloc);
  List<H264EncSlice> slices(g_alloc);
  H264EncPicture p;
  p.mb_width = 2;
  p.mb_height = 2;
  CHECK(enc.encode(p, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::Ok);
  H264EncPicture q = p;
  q.type = H264EncFrameType::P;   // no references
  CHECK(enc.encode(q, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::PictureControlNotSupported);
  H264EncReference r{small.planes(), 1, 0, false, {}, 0xFFFFFFFFu};
  q.list[0] = ArrayView<const H264EncReference>(&r, 1);
  CHECK(enc.encode(q, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::InvalidReferences);
  q = p;
  q.qp = 52;
  CHECK(enc.encode(q, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::PictureControlNotSupported);
  q = p;
  q.disable_deblocking_filter_idc = 3;
  CHECK(enc.encode(q, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::PictureControlNotSupported);
  q = p;
  q.slice_mode = H264EncSliceMode::SlicesPerFrame;
  q.slice_value = 5;   // more slices than macroblocks
  CHECK(enc.encode(q, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::SubregionLayoutNotSupported);
  q.slice_mode = H264EncSliceMode::BytesPerSlice;
  q.slice_value = 0;   // (no byte limit)
  CHECK(enc.encode(q, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::SubregionLayoutNotSupported);
  q = p;
  q.type = H264EncFrameType::B;
  q.pic_order_cnt_type = 2;
  H264EncReference rr{src.planes(), 1, 0, false, {}, 0xFFFFFFFFu};
  q.list[0] = q.list[1] = ArrayView<const H264EncReference>(&rr, 1);
  CHECK(enc.encode(q, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::PictureControlNotSupported);
  q = p;
  q.mb_width = 3;   // the planes are 2 macroblocks wide
  CHECK(enc.encode(q, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::InvalidReferences);
  int8_t map[3] = {0, 0, 0};
  q = p;
  q.qp_map = ArrayView<const int8_t>(map, 3);   // not one per macroblock
  CHECK(enc.encode(q, src.planes(), rec.writable(), &out, &slices, nullptr) == H264EncResult::PictureControlNotSupported);
}

// The kernel's side (Kernel/Dxgkrnl/VideoEncode.hpp): EncodeFrame's arguments parsed from the words a runtime writes, the metadata it writes and its resolve, and arguments
// cut short or with bits flipped (each must give a picture or an error in the metadata, never a fault); the motion vector heap and its resolve.
static void put_words(std::vector<uint8_t>& b, std::initializer_list<uint32_t> w) {
  for (uint32_t v : w) for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
static void test_kernel_side() {
  const uint32_t W = 32, H = 32;
  // widened planes: 4 bytes a pixel
  std::vector<uint8_t> in_l(W * H * 4), in_c(W / 2 * H / 2 * 4), rec_l(W * H * 4), rec_c(W / 2 * H / 2 * 4);
  for (uint32_t i = 0; i < W * H; ++i) in_l[i * 4] = uint8_t((i * 7) ^ (i >> 5));
  for (uint32_t i = 0; i < W * H / 4; ++i) { in_c[i * 4] = 120; in_c[i * 4 + 1] = 130; }
  std::vector<uint8_t> params;
  std::vector<uint32_t> w(dxgk::kVideoEncodeParamWords, 0);
  w[2] = W; w[3] = H; w[6] = 0; w[7] = 0; w[8] = 0; w[9] = 2;
  w[10] = 1; w[14] = 26; w[15] = 28; w[16] = 30; w[19] = 51;
  w[34] = 3; w[39] = 1; w[49] = 8;
  for (uint32_t v : w) put_words(params, {v});
  std::vector<uint8_t> heap(dxgk::kVideoEncodeHeapBytes), bits(1 << 16), meta(dxgk::kVideoEncodeMetadataHeader + 8 * dxgk::kVideoEncodeSubregionBytes), resolved(1024);
  auto job_for = [&](const std::vector<uint8_t>& p) {
    dxgk::VideoEncodeJob job;
    job.params = ConstByteView(p.data(), p.size());
    job.heap = ByteView(heap.data(), heap.size());
    job.input = {{ConstByteView(in_l.data(), in_l.size()), W * 4, W, H}, {ConstByteView(in_c.data(), in_c.size()), W / 2 * 4, W / 2, H / 2}};
    job.has_reconstruction = true;
    job.reconstruction_luma = {ByteView(rec_l.data(), rec_l.size()), W * 4, W, H};
    job.reconstruction_chroma = {ByteView(rec_c.data(), rec_c.size()), W / 2 * 4, W / 2, H / 2};
    job.bitstream = ByteView(bits.data(), bits.size());
    job.metadata = ByteView(meta.data(), meta.size());
    return job;
  };
  auto u64 = [](const std::vector<uint8_t>& b, size_t at) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | b[at + size_t(i)]; return v; };
  CHECK(dxgk::run_video_encode(g_alloc, job_for(params)));
  CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size())));
  const uint64_t written = u64(resolved, 56), count = u64(resolved, 64);
  CHECK(u64(resolved, 0) == 0 && written > 0 && count == 1 && u64(resolved, 72) == written && bits[0] == 0 && bits[3] == 1 && (bits[4] & 0x1F) == 5);
  CHECK(u64(resolved, 16) == 4);   // (an IDR picture: all four macroblocks intra)
  CHECK(rec_l[3] == 0xFF && rec_c[3] == 0xFF);   // (the reconstruction's pixels are (Y, 0, 0, 255), (U, V, 0, 255))
  // decoded again, the reconstruction
  {
    H264Decoder dec(g_alloc);
    Sequence seq{};
    seq.width = W; seq.height = H; seq.mbw = 2; seq.mbh = 2; seq.poc_type = 0; seq.log2_poc_lsb = 6;
    seq.write_headers();
    std::vector<uint8_t> au = seq.headers;
    au.insert(au.end(), bits.begin(), bits.begin() + long(written));
    VideoFrame f;
    H264Decoder::ExternalPicture ep;
    ep.key = 1;
    CHECK(dec.decode_external(ConstByteView(au.data(), au.size()), ep, &f) == H264Decoder::ExternalStatus::Decoded);
    bool same = true;
    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) same = same && f.plane[0][size_t(y) * f.stride[0] + x] == rec_l[(y * W + x) * 4];
    for (uint32_t y = 0; y < H / 2; ++y) for (uint32_t x = 0; x < W / 2; ++x) same = same && f.plane[1][size_t(y) * f.stride[1] + x] == rec_c[(y * W / 2 + x) * 4] && f.plane[2][size_t(y) * f.stride[2] + x] == rec_c[(y * W / 2 + x) * 4 + 1];
    CHECK(same);
  }
  // errors the runtime found, and a P picture with a reference index past the textures: the metadata says so, nothing is written
  std::vector<uint8_t> p2 = params;
  p2[48 * 4] = 0x2;
  CHECK(dxgk::run_video_encode(g_alloc, job_for(p2)));
  CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size())) && u64(resolved, 0) == 2 && u64(resolved, 56) == 0);
  std::vector<uint8_t> p3;
  std::vector<uint32_t> w3 = w;
  w3[34] = 1; w3[41] = 1; w3[42] = 1;
  for (uint32_t v : w3) put_words(p3, {v});
  put_words(p3, {5, 0, 0, 0, 0, 0});   // (a descriptor naming texture 5)
  put_words(p3, {0});                  // (list 0: that descriptor)
  CHECK(dxgk::run_video_encode(g_alloc, job_for(p3)));
  CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size())) && u64(resolved, 0) == 4);
  // the resolve of something EncodeFrame did not write
  std::vector<uint8_t> junk(200, 0x5A);
  CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(junk.data(), junk.size()), ByteView(resolved.data(), resolved.size())) && u64(resolved, 0) == 0x10);
  CHECK(!dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), 16)));
  // cut short at every length, and bits flipped
  int survived = 0;
  for (size_t n = 0; n < p3.size(); ++n) {
    std::vector<uint8_t> cut(p3.begin(), p3.begin() + long(n));
    survived += dxgk::run_video_encode(g_alloc, job_for(cut));
  }
  g_seed = 99;
  for (int i = 0; i < 400; ++i) {
    std::vector<uint8_t> flipped = (i & 1) ? params : p3;
    for (int k = 0; k < 3; ++k) { const size_t at = rnd() % flipped.size(); flipped[at] = uint8_t(flipped[at] ^ (1u << (rnd() % 8))); }
    survived += dxgk::run_video_encode(g_alloc, job_for(flipped));
    (void)dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size()));
  }
  printf("kernel encode arguments: %d of %zu damaged argument buffers handled\n", survived, p3.size() + 400);
  CHECK(survived == int(p3.size() + 400));
  // motion estimation through the kernel's planes and heap
  std::vector<uint8_t> cur(64 * 32 * 4), ref(64 * 32 * 4);
  Texture t(128, 96, 2);
  for (int y = 0; y < 32; ++y) for (int x = 0; x < 64; ++x) { ref[size_t(y * 64 + x) * 4] = t.at(x + 10, y + 10); cur[size_t(y * 64 + x) * 4] = t.at(x + 12, y + 11); }
  std::vector<uint8_t> mvheap(dxgk::motion_vector_heap_bytes(64, 32, 16));
  const dxgk::VideoPlaneImage c{ConstByteView(cur.data(), cur.size()), 64 * 4, 64, 32}, r{ConstByteView(ref.data(), ref.size()), 64 * 4, 64, 32};
  CHECK(dxgk::run_motion_estimation(g_alloc, c, r, 64, 32, 16, {}, ByteView(mvheap.data(), mvheap.size())));
  std::vector<uint8_t> img(4 * 2 * 8);
  CHECK(dxgk::resolve_motion_vectors(ConstByteView(mvheap.data(), mvheap.size()), 64, 32, {ByteView(img.data(), img.size()), 4 * 8, 4, 2}, 0, 0));
  const int16_t mx = int16_t(img[0] | img[1] << 8), my = int16_t(img[2] | img[3] << 8);
  CHECK(mx == 8 && my == 4 && img[6] == 1);
  CHECK(!dxgk::resolve_motion_vectors(ConstByteView(mvheap.data(), mvheap.size()), 64, 32, {ByteView(img.data(), img.size()), 4 * 8, 4, 2}, 1, 0));   // (does not fit at x 1)
  CHECK(!dxgk::resolve_motion_vectors(ConstByteView(junk.data(), junk.size()), 64, 32, {ByteView(img.data(), img.size()), 4 * 8, 4, 2}, 0, 0));
  {
    // (into an R16G16Sint image: the vector alone, 4 bytes a pixel, as the 8-byte one has it)
    std::vector<uint8_t> narrow(4 * 2 * 4, 0xEE);
    CHECK(dxgk::resolve_motion_vectors(ConstByteView(mvheap.data(), mvheap.size()), 64, 32, {ByteView(narrow.data(), narrow.size()), 4 * 4, 4, 2}, 0, 0, 4));
    bool same = true;
    for (uint32_t i = 0; i < 8; ++i) same = same && narrow[i * 4] == img[i * 8] && narrow[i * 4 + 1] == img[i * 8 + 1] && narrow[i * 4 + 2] == img[i * 8 + 2] && narrow[i * 4 + 3] == img[i * 8 + 3];
    CHECK(same);
    CHECK(!dxgk::resolve_motion_vectors(ConstByteView(mvheap.data(), mvheap.size()), 64, 32, {ByteView(narrow.data(), narrow.size()), 4 * 4, 4, 2}, 0, 0, 3));
  }
  CHECK(!dxgk::run_motion_estimation(g_alloc, c, r, 64, 32, 16, {}, ByteView(mvheap.data(), 20)));
}

// The kernel's encoder on pictures of a size that is not whole macroblocks, in textures of exactly that size (the reconstruction's last columns and rows kept in the
// encoder heap's reference store), CABAC, the 8x8 transform and temporal direct prediction; then again without the store (a heap of the rate controller's state only:
// predictions keep inside the textures). Each picture decoded again and compared with the texture's reconstruction; the PSNR metadata resolved; slices by bytes.
struct KernelTexture {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> l, c;
  void make(uint32_t w_, uint32_t h_) { w = w_; h = h_; l.assign(size_t(w) * h * 4, 0); c.assign(size_t(w / 2) * (h / 2) * 4, 0); }
  dxgk::VideoPictureImages images() const { return {{ConstByteView(l.data(), l.size()), w * 4, w, h}, {ConstByteView(c.data(), c.size()), w / 2 * 4, w / 2, h / 2}}; }
};
static void test_kernel_cropped(bool with_store, bool moving) {
  const uint32_t W = 40, H = 24;   // (coded as 48 x 32)
  Texture tex(200, 160, 2);
  KernelTexture input, pics[3];
  input.make(W, H);
  for (KernelTexture& t : pics) t.make(W, H);
  std::vector<uint8_t> heap(with_store ? dxgk::video_encode_heap_bytes(W, H) : dxgk::kVideoEncodeHeapBytes), bits(1 << 16);
  std::vector<uint8_t> meta(dxgk::video_encode_metadata_bytes(6, 0x18)), resolved(1024), fpsnr(12), spsnr(12 * 6);
  H264Decoder dec(g_alloc);
  Sequence seq{};
  seq.width = int(W); seq.height = int(H); seq.mbw = 3; seq.mbh = 2; seq.poc_type = 0; seq.log2_poc_lsb = 6; seq.high = true; seq.cabac = true; seq.t8x8 = true;
  seq.write_headers();
  // decode order: IDR (POC 0) into texture 0, P (POC 4, from 0) into 1, B (POC 2, from 0 and 1, not a reference) into 2
  struct Pic { uint32_t type, poc, frame_num, ref; std::vector<uint32_t> l0, l1; };
  const Pic order[3] = {{3, 0, 0, 1, {}, {}}, {1, 4, 1, 1, {0}, {}}, {2, 2, 2, 0, {0}, {1}}};
  int same = 0;
  uint64_t direct_pictures = 0;
  for (int i = 0; i < 3; ++i) {
    const Pic& pc = order[i];
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) input.l[(y * W + x) * 4] = moving ? tex.at(int(x + pc.poc / 2), int(y + pc.poc / 2)) : tex.at(int(x), int(y));
    for (uint32_t y = 0; y < H / 2; ++y)
      for (uint32_t x = 0; x < W / 2; ++x) { input.c[(y * W / 2 + x) * 4] = uint8_t(100 + tex.at(int(x + pc.poc / 4), int(y)) / 4); input.c[(y * W / 2 + x) * 4 + 1] = 140; }
    std::vector<uint32_t> w(dxgk::kVideoEncodeParamWords, 0);
    w[1] = 1; w[2] = W; w[3] = H; w[4] = 0x2 | 0x4; w[5] = 1; w[7] = 0; w[9] = 2;
    w[10] = 1; w[14] = 24; w[15] = 26; w[16] = 28; w[19] = 51;
    w[31] = i == 2 ? 1 : 0; w[32] = i == 2 ? 60 : 0;   // (the B picture in slices of 60 bytes)
    w[34] = pc.type; w[37] = pc.poc; w[38] = pc.frame_num; w[39] = pc.ref; w[49] = 6;
    w[41] = i; w[42] = uint32_t(pc.l0.size()); w[43] = uint32_t(pc.l1.size());
    w[58] = 0x18;
    std::vector<uint8_t> params;
    for (uint32_t v : w) put_words(params, {v});
    for (int d = 0; d < i; ++d) put_words(params, {uint32_t(d), 0, 0, order[d].poc, order[d].frame_num, 0});   // (descriptor d: texture d)
    for (uint32_t d : pc.l0) put_words(params, {d});
    for (uint32_t d : pc.l1) put_words(params, {d});
    dxgk::VideoPictureImages refs[2];
    uint64_t keys[2];
    for (int d = 0; d < i; ++d) { refs[d] = pics[d].images(); keys[d] = 100 + uint64_t(d); }
    dxgk::VideoEncodeJob job;
    job.params = ConstByteView(params.data(), params.size());
    job.heap = ByteView(heap.data(), heap.size());
    job.input = input.images();
    job.has_reconstruction = true;
    job.reconstruction_luma = {ByteView(pics[i].l.data(), pics[i].l.size()), W * 4, W, H};
    job.reconstruction_chroma = {ByteView(pics[i].c.data(), pics[i].c.size()), W / 2 * 4, W / 2, H / 2};
    job.references = ArrayView<const dxgk::VideoPictureImages>(refs, size_t(i));
    job.reference_keys = ArrayView<const uint64_t>(keys, size_t(i));
    job.reconstruction_key = 100 + uint64_t(i);
    job.bitstream = ByteView(bits.data(), bits.size());
    job.metadata = ByteView(meta.data(), meta.size());
    CHECK(dxgk::run_video_encode(g_alloc, job));
    CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size()), ByteView(fpsnr.data(), fpsnr.size()),
                                              ByteView(spsnr.data(), spsnr.size())));
    auto u64 = [&](size_t at) { uint64_t v = 0; for (int k = 7; k >= 0; --k) v = (v << 8) | resolved[at + size_t(k)]; return v; };
    const uint64_t written = u64(56), count = u64(64);
    CHECK(u64(0) == 0 && written > 0 && count >= 1);
    if (i == 2 && moving) CHECK(count > 1);   // (slices by bytes)
    uint64_t total = 0;
    for (uint64_t s = 0; s < count; ++s) total += u64(72 + size_t(s) * 24);
    CHECK(total == written);
    float py, pu, pv;
    memcpy(&py, &fpsnr[0], 4); memcpy(&pu, &fpsnr[4], 4); memcpy(&pv, &fpsnr[8], 4);
    CHECK(py > 25 && py < 100 && pu > 25 && pv > 25);
    float sy;
    memcpy(&sy, &spsnr[0], 4);
    CHECK(sy > 20);
    // decoded again: the visible part is the texture's reconstruction
    std::vector<uint8_t> au = seq.headers;
    au.insert(au.end(), bits.begin(), bits.begin() + long(written));
    std::vector<H264Decoder::ExternalReference> drefs;
    for (int d = 0; d < i; ++d) {
      if (!order[d].ref) continue;
      H264Decoder::ExternalReference er;
      er.key = uint32_t(d + 1);
      er.frame_num = int(order[d].frame_num);
      er.top_poc = er.bottom_poc = int(order[d].poc);
      drefs.push_back(er);
    }
    H264Decoder::ExternalPicture ep;
    ep.key = uint32_t(i + 1);
    ep.top_poc = ep.bottom_poc = int(pc.poc);
    ep.references = ArrayView<const H264Decoder::ExternalReference>(drefs.data(), drefs.size());
    VideoFrame f;
    CHECK(dec.decode_external(ConstByteView(au.data(), au.size()), ep, &f) == H264Decoder::ExternalStatus::Decoded);
    bool ok = f.format.width == 48 && f.format.height == 32;
    for (uint32_t y = 0; ok && y < H; ++y) for (uint32_t x = 0; ok && x < W; ++x) ok = f.plane[0][size_t(y) * f.stride[0] + x] == pics[i].l[(y * W + x) * 4];
    for (uint32_t y = 0; ok && y < H / 2; ++y)
      for (uint32_t x = 0; ok && x < W / 2; ++x) ok = f.plane[1][size_t(y) * f.stride[1] + x] == pics[i].c[(y * W / 2 + x) * 4] && f.plane[2][size_t(y) * f.stride[2] + x] == pics[i].c[(y * W / 2 + x) * 4 + 1];
    same += ok;
    // (B_Direct / B_Skip in the B picture: the metadata's skipped count; direct needs the store)
    if (i == 2) direct_pictures = u64(32);
  }
  printf("kernel encoder, %s 40x24 in textures of 40x24 %s: %d of 3 pictures decode to the textures' reconstructions; %llu skipped in the B picture\n",
         moving ? "moving" : "still", with_store ? "with the reference store" : "without a store", same, (unsigned long long)direct_pictures);
  CHECK(same == 3);
  // (a still scene: B_Skip by temporal direct prediction needs the co-located picture's motion, which only the store has)
  if (!moving) CHECK(with_store ? direct_pictures > 0 : direct_pictures == 0);
}

int main(int argc, char** argv) {
  test_motion_estimation();
  g_dump = argc > 1 ? argv[1] : nullptr;
  test_encoder();
  test_new_tools();
  test_refusals();
  test_kernel_side();
  for (int moving = 0; moving < 2; ++moving) {
    test_kernel_cropped(true, moving != 0);
    test_kernel_cropped(false, moving != 0);
  }
  printf("VideoEncodeTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
