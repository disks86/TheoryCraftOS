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

// Host tests of the HEVC picture encoder (src/Hardware/VideoCodec/Software/H265PictureEncoder) driven as a Direct3D 12 program drives it: the test keeps the decoded picture
// buffer (the descriptors, short and long term, used or not), names the final lists (with list modifications given or left to the encoder), and writes the VPS / SPS / PPS
// itself (write_parameter_sets). Every stream is decoded again by the in-tree H265Decoder (the full NAL path: parameter sets, slice headers, reference picture sets,
// output order) and its pictures must be the encoder's reconstructions sample for sample; the encoder's own pre-filter reconstruction must be what a decoder has
// (drift 0). Sequences: CTB 16 / 32 / 64, minimum coding blocks 8 / 16, transform sizes and depths, AMP, SAO, transform skip, constrained intra prediction, no loop filter
// across slices, every slice layout, QP maps (delta and absolute), long-term references, temporal layers, POC LSB wrap, Main 10, sizes not a multiple of the CTB; and
// the inputs the encoder must refuse.
// `VideoEncodeHevcTest <dir>` also writes <dir>/stream.hevc, recon.yuv, source.yuv (8-bit, yuv420p) and stream10.hevc, recon10.yuv, source10.yuv (Main 10, yuv420p10le)
// for tests/hevc_encode_ffmpeg.sh.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#include "Hardware/VideoCodec/Software/H265.hpp"
#include "Hardware/VideoCodec/Software/H265PictureEncoder.hpp"
#include "Kernel/Dxgkrnl/VideoEncode.hpp"
#include "Kernel/Dxgkrnl/VideoEncodeCodecs.hpp"
#include "Utility/ByteStream.hpp"

using namespace hardware;
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

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

// A smooth random texture to cut moving pictures out of.
struct Texture {
  int w, h;
  std::vector<int> s;
  Texture(int w_, int h_, int detail) : w(w_), h(h_), s(size_t(w_) * h_) {
    std::vector<int> a(s.size());
    for (auto& v : a) v = int(rnd() % 1024);
    for (int pass = 0; pass < detail; ++pass) {
      std::vector<int> b(a.size());
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          int sum = 0, n = 0;
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              sum += a[size_t(std::clamp(y + dy, 0, h - 1)) * w + std::clamp(x + dx, 0, w - 1)];
              ++n;
            }
          b[size_t(y) * w + x] = sum / n;
        }
      a = b;
    }
    for (size_t i = 0; i < s.size(); ++i) s[i] = std::clamp((a[i] - 512) * 4 + 512, 0, 1023);
  }
  int at(int x, int y) const { return s[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; }
};

struct Pic {
  int w = 0, h = 0;
  std::vector<uint16_t> y, u, v;
  void make(int w_, int h_) {
    w = w_;
    h = h_;
    y.assign(size_t(w) * h, 0);
    u.assign(size_t(w / 2) * (h / 2), 0);
    v.assign(size_t(w / 2) * (h / 2), 0);
  }
  EncPlanes16 planes() const {
    return {ArrayView<const uint16_t>(y.data(), y.size()), ArrayView<const uint16_t>(u.data(), u.size()), ArrayView<const uint16_t>(v.data(), v.size()), uint32_t(w),
            uint32_t(h)};
  }
  EncWritablePlanes16 writable() {
    return {ArrayView<uint16_t>(y.data(), y.size()), ArrayView<uint16_t>(u.data(), u.size()), ArrayView<uint16_t>(v.data(), v.size()), uint32_t(w), uint32_t(h)};
  }
};

// Picture t of a moving texture (bit depth bd), the visible part vw x vh (the rest repeats the edge).
static void make_source(const Texture& tex, int t, int bd, int vw, int vh, Pic* p) {
  for (int y = 0; y < p->h; ++y)
    for (int x = 0; x < p->w; ++x) {
      const int sx = std::min(x, vw - 1), sy = std::min(y, vh - 1);
      const int fx = sx * 4 + t * 7, fy = sy * 4 + t * 3;
      const int a = tex.at(fx / 4, fy / 4), b = tex.at(fx / 4 + 1, fy / 4), c = tex.at(fx / 4, fy / 4 + 1), d = tex.at(fx / 4 + 1, fy / 4 + 1);
      const int wx = fx & 3, wy = fy & 3;
      const int v10 = ((4 - wx) * (4 - wy) * a + wx * (4 - wy) * b + (4 - wx) * wy * c + wx * wy * d + 8) / 16;
      p->y[size_t(y) * p->w + x] = uint16_t(bd == 8 ? v10 >> 2 : v10);
    }
  for (int y = 0; y < p->h / 2; ++y)
    for (int x = 0; x < p->w / 2; ++x) {
      const int u = 96 + (std::min(x, vw / 2 - 1) * 2 + t) % 64, v = 160 - (std::min(y, vh / 2 - 1) * 3 + 2 * t) % 48;
      p->u[size_t(y) * (p->w / 2) + x] = uint16_t(bd == 8 ? u : u * 4 + (x & 3));
      p->v[size_t(y) * (p->w / 2) + x] = uint16_t(bd == 8 ? v : v * 4 + (y & 3));
    }
}

static double psnr(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, int w, int h, int stride, int bd) {
  double se = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double d = double(a[size_t(y) * stride + x]) - b[size_t(y) * stride + x];
      se += d * d;
    }
  const double peak = (1 << bd) - 1;
  return se == 0 ? 99.0 : 10 * log10(peak * peak * w * h / se);
}

// One picture of a test sequence, as a program describes it.
struct Step {
  H265EncFrameType type;
  int poc;
  std::vector<int> list0, list1;   // POCs
  bool reference = true;
  int tid = 0;
  std::vector<int> drop;            // POCs that leave the buffer before this picture
  std::vector<int> make_long;       // POCs that become long-term references before this picture
  std::vector<int> unused;          // POCs kept but not used by this picture
  bool give_modification = false;   // give list_entry values (else the encoder works them out)
};

struct Options {
  H265EncSequence seq;
  int vw = 0, vh = 0;               // visible size (0: the coded size)
  int qp = 30;
  H265EncSliceMode slice_mode = H265EncSliceMode::FullFrame;
  uint32_t slice_value = 0;
  int qp_map = 0;                   // 0 none, 1 deltas, 2 absolute
  int max_sub_layers = 1;
  H265EncPrecision precision = H265EncPrecision::Quarter;
  uint32_t pps_id = 0;
};

struct Encoded {
  std::vector<uint8_t> stream;
  std::vector<Pic> recon, source;   // in display (POC) order
  std::vector<int> pocs;
  double min_psnr = 99;
  uint64_t drift = 0, bytes = 0;
  bool ok = true;
};

struct Stored {
  Pic recon;
  int poc;
  bool long_term = false;
};

static Encoded run_sequence(const Options& opt, const std::vector<Step>& steps) {
  Encoded e;
  const H265EncSequence& seq = opt.seq;
  const int W = int(seq.width), H = int(seq.height), bd = seq.bit_depth;
  const int vw = opt.vw ? opt.vw : W, vh = opt.vh ? opt.vh : H;
  Texture tex(W / 2 + 80, H / 2 + 60, 2);
  H265PictureEncoder enc(g_alloc);
  enc.set_drift_check(true);
  std::vector<Stored> dpb;
  ByteBuffer ps(g_alloc);
  CHECK(H265PictureEncoder::write_parameter_sets(seq, uint32_t(vw), uint32_t(vh), opt.pps_id, 93, uint8_t(opt.max_sub_layers), &ps));
  struct Out {
    int poc;
    Pic recon, source;
  };
  std::vector<Out> outs;
  const int ctb = 1 << seq.log2_ctb, ctbs = ((W + ctb - 1) / ctb) * ((H + ctb - 1) / ctb);
  std::vector<int8_t> qmap(static_cast<size_t>(ctbs));
  for (const Step& s : steps) {
    for (int p : s.drop) dpb.erase(std::remove_if(dpb.begin(), dpb.end(), [&](const Stored& d) { return d.poc == p; }), dpb.end());
    for (int p : s.make_long)
      for (Stored& d : dpb)
        if (d.poc == p) d.long_term = true;
    if (s.type == H265EncFrameType::Idr) dpb.clear();
    Pic src;
    src.make(W, H);
    make_source(tex, s.poc, bd, vw, vh, &src);
    std::vector<H265EncReference> desc;
    for (const Stored& d : dpb) {
      H265EncReference r;
      r.picture = d.recon.planes();
      r.poc = d.poc;
      r.long_term = d.long_term;
      r.used_by_current = std::find(s.unused.begin(), s.unused.end(), d.poc) == s.unused.end();
      desc.push_back(r);
    }
    auto index_of = [&](int poc) {
      for (size_t i = 0; i < dpb.size(); ++i)
        if (dpb[i].poc == poc) return uint8_t(i);
      printf("test bug: poc %d not in the buffer\n", poc);
      return uint8_t(255);
    };
    std::vector<uint8_t> l0, l1;
    for (int p : s.list0) l0.push_back(index_of(p));
    for (int p : s.list1) l1.push_back(index_of(p));
    // list_entry values when asked for: the position in RefPicListTemp (before, after, long-term; after, before, long-term)
    std::vector<uint32_t> m0, m1;
    if (s.give_modification) {
      std::vector<int> before, after, lt;
      for (const Stored& d : dpb) {
        if (std::find(s.unused.begin(), s.unused.end(), d.poc) != s.unused.end()) continue;
        if (d.long_term) lt.push_back(d.poc);
        else if (d.poc < s.poc) before.push_back(d.poc);
        else after.push_back(d.poc);
      }
      std::sort(before.begin(), before.end(), std::greater<int>());
      std::sort(after.begin(), after.end());
      const int32_t max_lsb = 1 << seq.log2_max_poc_lsb, msb = s.poc - (s.poc & (max_lsb - 1));
      std::stable_sort(lt.begin(), lt.end(), [&](int a, int b) { return (msb - (a - (a & (max_lsb - 1)))) / max_lsb < (msb - (b - (b & (max_lsb - 1)))) / max_lsb; });
      std::vector<int> t0 = before, t1 = after;
      t0.insert(t0.end(), after.begin(), after.end());
      t0.insert(t0.end(), lt.begin(), lt.end());
      t1.insert(t1.end(), before.begin(), before.end());
      t1.insert(t1.end(), lt.begin(), lt.end());
      for (int p : s.list0) m0.push_back(uint32_t(std::find(t0.begin(), t0.end(), p) - t0.begin()));
      for (int p : s.list1) m1.push_back(uint32_t(std::find(t1.begin(), t1.end(), p) - t1.begin()));
    }
    H265EncPicture pic;
    pic.seq = seq;
    pic.type = s.type;
    pic.pps_id = opt.pps_id;
    pic.poc = s.poc;
    pic.temporal_id = uint8_t(s.tid);
    pic.reference = s.reference;
    pic.dpb = ArrayView<const H265EncReference>(desc.data(), desc.size());
    pic.list[0] = ArrayView<const uint8_t>(l0.data(), l0.size());
    pic.list[1] = ArrayView<const uint8_t>(l1.data(), l1.size());
    pic.modification[0] = ArrayView<const uint32_t>(m0.data(), m0.size());
    pic.modification[1] = ArrayView<const uint32_t>(m1.data(), m1.size());
    pic.qp = int8_t(opt.qp + (s.type == H265EncFrameType::B ? 2 : 0));
    if (opt.qp_map) {
      for (int i = 0; i < ctbs; ++i) qmap[size_t(i)] = int8_t(opt.qp_map == 1 ? (i % 5) * 3 - 6 : 22 + (i * 7) % 20);
      pic.qp_map = ArrayView<const int8_t>(qmap.data(), qmap.size());
      pic.qp_map_absolute = opt.qp_map == 2;
    }
    pic.slice_mode = opt.slice_mode;
    pic.slice_value = opt.slice_value;
    pic.precision = opt.precision;
    Pic rec;
    rec.make(W, H);
    ByteBuffer out(g_alloc);
    List<H265EncSlice> slices(g_alloc);
    EncStatistics st;
    const H265EncResult r = enc.encode(pic, src.planes(), rec.writable(), &out, &slices, &st);
    CHECK(r == H265EncResult::Ok);
    if (r != H265EncResult::Ok) {
      printf("  picture %d: result %d\n", s.poc, int(r));
      e.ok = false;
      return e;
    }
    CHECK(enc.last_drift() == 0);
    if (enc.last_drift()) printf("  picture %d: %llu samples drift\n", s.poc, (unsigned long long)enc.last_drift());
    e.drift += enc.last_drift();
    CHECK(slices.count() == H265PictureEncoder::slice_count(opt.slice_mode, opt.slice_value, uint32_t((W + ctb - 1) / ctb), uint32_t((H + ctb - 1) / ctb)));
    uint64_t at = 0;
    for (const H265EncSlice& sl : slices.view()) {
      CHECK(sl.offset == at);
      CHECK(sl.header_bits >= 6 * 8 && sl.header_bits < sl.size * 8);
      at += sl.size;
    }
    CHECK(at == out.size());
    CHECK(st.units == uint64_t((W / 8) * (H / 8)));
    CHECK(st.intra + st.inter + st.skipped == st.units);
    if (s.type == H265EncFrameType::Idr || s.type == H265EncFrameType::I) CHECK(st.intra == st.units);
    if (s.type == H265EncFrameType::Idr) e.stream.insert(e.stream.end(), ps.view().begin(), ps.view().end());
    e.stream.insert(e.stream.end(), out.view().begin(), out.view().end());
    e.bytes += out.size();
    if (s.reference || s.type == H265EncFrameType::Idr) dpb.push_back({rec, s.poc, false});
    outs.push_back({s.poc, rec, src});
  }
  std::sort(outs.begin(), outs.end(), [](const Out& a, const Out& b) { return a.poc < b.poc; });
  for (Out& o : outs) {
    e.pocs.push_back(o.poc);
    e.min_psnr = std::min(e.min_psnr, psnr(o.recon.y, o.source.y, vw, vh, W, bd));
    e.recon.push_back(o.recon);
    e.source.push_back(o.source);
  }
  return e;
}

// The in-tree decoder's pictures of the stream, in output order, against the reconstructions (cropped to the visible size).
static bool decodes_to_recon(const Encoded& e, int W, int H, int vw, int vh, int bd) {
  H265Decoder dec(g_alloc);
  // the access units: NAL units up to the next slice that starts a picture (first_slice_segment_in_pic_flag)
  std::vector<size_t> starts;
  for (size_t i = 0; i + 4 < e.stream.size(); ++i)
    if (e.stream[i] == 0 && e.stream[i + 1] == 0 && e.stream[i + 2] == 0 && e.stream[i + 3] == 1) starts.push_back(i);
  std::vector<size_t> units;
  for (size_t k = 0; k < starts.size(); ++k) {
    const int type = (e.stream[starts[k] + 4] >> 1) & 0x3F;
    const bool vcl = type < 32, first = vcl && (e.stream[starts[k] + 6] & 0x80);
    if (k == 0 || type == 32 || (first && ((e.stream[starts[k - 1] + 4] >> 1) & 0x3F) < 32)) units.push_back(k);
  }
  size_t n = 0;
  bool same = true;
  VideoFrame f;
  auto take = [&]() {
    while (dec.receive(&f)) {
      if (n >= e.recon.size()) {
        same = false;
        return;
      }
      const Pic& r = e.recon[n];
      if (f.format.width != uint32_t(vw) || f.format.height != uint32_t(vh)) same = false;
      for (int y = 0; y < vh && same; ++y)
        for (int x = 0; x < vw; ++x) {
          const int v = bd == 8 ? f.plane[0][size_t(y) * f.stride[0] + x] : (f.plane[0][size_t(y) * f.stride[0] + 2 * x] | (f.plane[0][size_t(y) * f.stride[0] + 2 * x + 1] << 8)) >> 6;
          if (v != r.y[size_t(y) * W + x]) {
            printf("  picture %zu (poc %d): luma (%d, %d) decoded %d, reconstructed %d\n", n, e.pocs[n], x, y, v, r.y[size_t(y) * W + x]);
            same = false;
            break;
          }
        }
      for (int y = 0; y < vh / 2 && same; ++y)
        for (int x = 0; x < vw / 2; ++x) {
          int u, v;
          if (bd == 8) {
            u = f.plane[1][size_t(y) * f.stride[1] + x];
            v = f.plane[2][size_t(y) * f.stride[2] + x];
          } else {
            const uint8_t* q = f.plane[1] + size_t(y) * f.stride[1] + 4 * x;
            u = (q[0] | (q[1] << 8)) >> 6;
            v = (q[2] | (q[3] << 8)) >> 6;
          }
          if (u != r.u[size_t(y) * (W / 2) + x] || v != r.v[size_t(y) * (W / 2) + x]) {
            printf("  picture %zu (poc %d): chroma (%d, %d) differs\n", n, e.pocs[n], x, y);
            same = false;
            break;
          }
        }
      ++n;
    }
  };
  for (size_t u = 0; u < units.size(); ++u) {
    const size_t from = starts[units[u]], to = u + 1 < units.size() ? starts[units[u + 1]] : e.stream.size();
    CHECK(dec.decode(ConstByteView(e.stream.data() + from, to - from), 0));
    take();
  }
  dec.flush();
  take();
  if (n != e.recon.size()) printf("  decoded %zu pictures of %zu\n", n, e.recon.size());
  return same && n == e.recon.size();
}

static void report(const char* name, const Encoded& e) {
  printf("  %-44s %2zu pictures, %7llu bytes, lowest PSNR %.1f dB\n", name, e.recon.size(), (unsigned long long)e.bytes, e.min_psnr);
}

static void write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(bytes.data(), 1, bytes.size(), f);
  fclose(f);
}
static void write_yuv(const std::string& path, const std::vector<Pic>& pics, int vw, int vh, int bd) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  for (const Pic& p : pics) {
    auto put = [&](const std::vector<uint16_t>& plane, int stride, int w, int h) {
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const uint16_t v = plane[size_t(y) * stride + x];
          if (bd == 8) fputc(v, f);
          else {
            fputc(v & 0xFF, f);
            fputc(v >> 8, f);
          }
        }
    };
    put(p.y, p.w, vw, vh);
    put(p.u, p.w / 2, vw / 2, vh / 2);
    put(p.v, p.w / 2, vw / 2, vh / 2);
  }
  fclose(f);
}

static H265EncSequence base_seq(uint32_t w, uint32_t h) {
  H265EncSequence s;
  s.width = w;
  s.height = h;
  s.log2_ctb = 5;
  s.log2_min_cb = 3;
  s.log2_min_tb = 2;
  s.log2_max_tb = 5;
  s.max_th_depth_inter = 1;
  s.max_th_depth_intra = 1;
  s.log2_max_poc_lsb = 8;
  return s;
}

// IDR P B B P B B P: P pictures refer to the two anchors before them, B pictures to the anchors around them (the first B is a reference of the second).
static std::vector<Step> ipbb(int anchors) {
  std::vector<Step> v;
  v.push_back({H265EncFrameType::Idr, 0, {}, {}});
  int prev = 0, prev2 = -1;
  for (int a = 1; a <= anchors; ++a) {
    const int poc = a * 3;
    Step p{H265EncFrameType::P, poc, {prev}, {}};
    if (prev2 >= 0) p.list0.push_back(prev2);
    if (a > 1) p.drop.push_back(poc - 5);
    if (a > 2) p.drop.push_back(prev2 - 3);
    v.push_back(p);
    v.push_back({H265EncFrameType::B, poc - 2, {prev, poc}, {poc, prev}, true});
    v.push_back({H265EncFrameType::B, poc - 1, {poc - 2, prev}, {poc}, false});
    prev2 = prev;
    prev = poc;
  }
  return v;
}

static void test_sequences(const char* dump) {
  printf("sequences\n");
  // 1. 200x120 (not a multiple of the CTB), CTB 32, SAO, two rows a slice, IPBB with two references
  {
    Options o;
    o.seq = base_seq(200, 120);
    o.seq.sao = true;
    o.slice_mode = H265EncSliceMode::RowsPerSlice;
    o.slice_value = 2;
    const Encoded e = run_sequence(o, ipbb(3));
    CHECK(e.ok && decodes_to_recon(e, 200, 120, 200, 120, 8));
    CHECK(e.min_psnr > 32);
    report("200x120 CTB 32, SAO, IPBB, rows", e);
    if (dump) {
      write_file(std::string(dump) + "/stream.hevc", e.stream);
      write_yuv(std::string(dump) + "/recon.yuv", e.recon, 200, 120, 8);
      write_yuv(std::string(dump) + "/source.yuv", e.source, 200, 120, 8);
    }
  }
  // 2. CTB 64, minimum CB 16, transform blocks 4 .. 16, AMP, transform skip, constrained intra, no filter across slices, CTBs a slice, QP deltas, half-sample vectors
  {
    Options o;
    o.seq = base_seq(192, 128);
    o.seq.log2_ctb = 6;
    o.seq.log2_min_cb = 4;
    o.seq.log2_max_tb = 4;
    o.seq.max_th_depth_inter = 2;
    o.seq.max_th_depth_intra = 0;
    o.seq.amp = true;
    o.seq.transform_skip = true;
    o.seq.constrained_intra_pred = true;
    o.seq.loop_filter_across_slices = false;
    o.vw = 180;
    o.vh = 120;
    o.slice_mode = H265EncSliceMode::CtbsPerSlice;
    o.slice_value = 2;
    o.qp_map = 1;
    o.precision = H265EncPrecision::Half;
    o.pps_id = 5;
    const Encoded e = run_sequence(o, ipbb(2));
    CHECK(e.ok && decodes_to_recon(e, 192, 128, 180, 120, 8));
    report("192x128 CTB 64 / CB 16, AMP TS CIP, QP deltas", e);
  }
  // 3. CTB 16, transform blocks 4 .. 8, POC LSB of 4 bits (wraps), long-term references, list modifications given and worked out, temporal layers,
  //    absolute QP map, three slices, whole-sample vectors
  {
    Options o;
    o.seq = base_seq(64, 48);
    o.seq.log2_ctb = 4;
    o.seq.log2_max_tb = 3;
    o.seq.max_th_depth_inter = 0;
    o.seq.max_th_depth_intra = 0;
    o.seq.log2_max_poc_lsb = 4;
    o.seq.long_term_refs = true;
    o.max_sub_layers = 3;
    o.qp_map = 2;
    o.slice_mode = H265EncSliceMode::SlicesPerFrame;
    o.slice_value = 3;
    o.precision = H265EncPrecision::Full;
    std::vector<Step> v;
    v.push_back({H265EncFrameType::Idr, 0, {}, {}});
    // picture 0 becomes long term at 2 and stays; P pictures refer to it and to the last one
    for (int poc = 1; poc <= 22; ++poc) {
      Step s{H265EncFrameType::P, poc, {poc - 1}, {}};
      if (poc >= 2) {
        s.list0.push_back(0);
        if (poc == 2) s.make_long.push_back(0);
      }
      if (poc >= 3) s.drop.push_back(poc - 2);
      if (poc % 5 == 0 && poc >= 2) {
        // the long-term picture first: a modification (given on even pictures)
        std::swap(s.list0[0], s.list0[1]);
        s.give_modification = poc % 2 == 0;
      }
      if (poc % 7 == 0) {
        s.type = H265EncFrameType::I;
        s.list0.clear();
      }
      v.push_back(s);
    }
    const Encoded e = run_sequence(o, v);
    CHECK(e.ok && decodes_to_recon(e, 64, 48, 64, 48, 8));
    report("64x48 CTB 16, LSB 4, long term, modifications", e);
    if (dump) {
      write_file(std::string(dump) + "/stream_lt.hevc", e.stream);
      write_yuv(std::string(dump) + "/recon_lt.yuv", e.recon, 64, 48, 8);
    }
  }
  // 4. temporal layers: I0 P4 (tid 0), B2 (tid 1), b1 b3 (tid 2, not references)
  {
    Options o;
    o.seq = base_seq(96, 64);
    o.max_sub_layers = 3;
    std::vector<Step> v;
    v.push_back({H265EncFrameType::Idr, 0, {}, {}});
    for (int g = 0; g < 2; ++g) {
      const int b = g * 4;
      v.push_back({H265EncFrameType::P, b + 4, {b}, {}, true, 0, g ? std::vector<int>{b - 4, b - 2} : std::vector<int>{}});
      v.push_back({H265EncFrameType::B, b + 2, {b}, {b + 4}, true, 1});
      v.push_back({H265EncFrameType::B, b + 1, {b, b + 2}, {b + 2, b + 4}, false, 2});
      v.push_back({H265EncFrameType::B, b + 3, {b + 2, b}, {b + 4}, false, 2});
    }
    const Encoded e = run_sequence(o, v);
    CHECK(e.ok && decodes_to_recon(e, 96, 64, 96, 64, 8));
    report("96x64 temporal layers 0 / 1 / 2", e);
  }
  // 5. Main 10, CTB 32, SAO, minimum CB 8 with 4x4 .. 32x32 transforms
  {
    Options o;
    o.seq = base_seq(128, 96);
    o.seq.bit_depth = 10;
    o.seq.sao = true;
    o.qp = 28;
    const Encoded e = run_sequence(o, ipbb(2));
    CHECK(e.ok && decodes_to_recon(e, 128, 96, 128, 96, 10));
    CHECK(e.min_psnr > 32);
    report("128x96 Main 10, SAO, IPBB", e);
    if (dump) {
      write_file(std::string(dump) + "/stream10.hevc", e.stream);
      write_yuv(std::string(dump) + "/recon10.yuv", e.recon, 128, 96, 10);
      write_yuv(std::string(dump) + "/source10.yuv", e.source, 128, 96, 10);
    }
  }
  // 6. Main 10 at a negative QP, CTB 16 (QpBdOffset)
  {
    Options o;
    o.seq = base_seq(48, 32);
    o.seq.bit_depth = 10;
    o.seq.log2_ctb = 4;
    o.seq.log2_max_tb = 4;
    o.qp = -6;
    std::vector<Step> v = {{H265EncFrameType::Idr, 0, {}, {}}, {H265EncFrameType::P, 1, {0}, {}}};
    const Encoded e = run_sequence(o, v);
    CHECK(e.ok && decodes_to_recon(e, 48, 32, 48, 32, 10));
    CHECK(e.min_psnr > 50);
    report("48x32 Main 10 at QP -6", e);
  }
}

// Inputs the encoder refuses.
static void test_refusals() {
  printf("refusals\n");
  H265EncSequence s = base_seq(64, 64);
  CHECK(H265PictureEncoder::sequence_valid(s));
  H265EncSequence b = s;
  b.log2_min_tb = 3;   // not below the minimum coding block
  CHECK(!H265PictureEncoder::sequence_valid(b));
  b = s;
  b.log2_max_tb = 2;
  CHECK(!H265PictureEncoder::sequence_valid(b));
  b = s;
  b.log2_ctb = 7;
  CHECK(!H265PictureEncoder::sequence_valid(b));
  b = s;
  b.width = 60;   // not a multiple of the minimum coding block
  CHECK(!H265PictureEncoder::sequence_valid(b));
  b = s;
  b.bit_depth = 12;
  CHECK(!H265PictureEncoder::sequence_valid(b));
  b = s;
  b.max_th_depth_inter = 4;
  CHECK(!H265PictureEncoder::sequence_valid(b));
  CHECK(H265PictureEncoder::slice_count(H265EncSliceMode::BytesPerSlice, 100, 2, 2) == 0);
  CHECK(H265PictureEncoder::slice_count(H265EncSliceMode::SlicesPerFrame, 5, 2, 2) == 0);
  CHECK(H265PictureEncoder::slice_count(H265EncSliceMode::CtbsPerSlice, 3, 2, 2) == 2);
  CHECK(H265PictureEncoder::slice_count(H265EncSliceMode::RowsPerSlice, 0, 2, 2) == 0);
  ByteBuffer ps(g_alloc);
  CHECK(!H265PictureEncoder::write_parameter_sets(s, 63, 64, 0, 93, 1, &ps));   // odd visible width
  CHECK(!H265PictureEncoder::write_parameter_sets(s, 64, 64, 64, 93, 1, &ps));  // pps id
  CHECK(!H265PictureEncoder::write_parameter_sets(s, 64, 64, 0, 93, 8, &ps));   // sub-layers

  H265PictureEncoder enc(g_alloc);
  Pic src, rec, ref;
  src.make(64, 64);
  rec.make(64, 64);
  ref.make(64, 64);
  auto run = [&](H265EncPicture p) {
    ByteBuffer out(g_alloc);
    List<H265EncSlice> sl(g_alloc);
    EncStatistics st;
    return enc.encode(p, src.planes(), rec.writable(), &out, &sl, &st);
  };
  H265EncPicture p;
  p.seq = s;
  CHECK(run(p) == H265EncResult::Ok);
  H265EncPicture q = p;
  q.poc = 4;   // an IDR picture has order count 0
  CHECK(run(q) == H265EncResult::PictureControlNotSupported);
  q = p;
  q.qp = 52;
  CHECK(run(q) == H265EncResult::PictureControlNotSupported);
  q = p;
  q.slice_mode = H265EncSliceMode::BytesPerSlice;
  q.slice_value = 500;
  CHECK(run(q) == H265EncResult::SubregionLayoutNotSupported);
  q = p;
  std::vector<int8_t> short_map(3);
  q.qp_map = ArrayView<const int8_t>(short_map.data(), short_map.size());
  CHECK(run(q) == H265EncResult::PictureControlNotSupported);
  // P pictures
  H265EncReference r0;
  r0.picture = ref.planes();
  r0.poc = 0;
  std::vector<H265EncReference> dpb = {r0};
  uint8_t l0[2] = {0, 0};
  q = p;
  q.type = H265EncFrameType::P;
  q.poc = 1;
  q.dpb = ArrayView<const H265EncReference>(dpb.data(), dpb.size());
  q.list[0] = ArrayView<const uint8_t>(l0, 1);
  CHECK(run(q) == H265EncResult::Ok);
  H265EncPicture t = q;
  t.list[0] = ArrayView<const uint8_t>();   // a P picture without a list
  CHECK(run(t) == H265EncResult::InvalidReferences);
  t = q;
  uint8_t bad[1] = {3};
  t.list[0] = ArrayView<const uint8_t>(bad, 1);
  CHECK(run(t) == H265EncResult::InvalidReferences);
  t = q;
  std::vector<H265EncReference> unused = dpb;
  unused[0].used_by_current = false;
  t.dpb = ArrayView<const H265EncReference>(unused.data(), unused.size());
  CHECK(run(t) == H265EncResult::InvalidReferences);
  t = q;
  std::vector<H265EncReference> dup = {r0, r0};
  t.dpb = ArrayView<const H265EncReference>(dup.data(), dup.size());
  CHECK(run(t) == H265EncResult::PictureControlNotSupported);   // two pictures of one order count
  t = q;
  std::vector<H265EncReference> lt = dpb;
  lt[0].long_term = true;   // long-term references the sequence does not allow
  t.dpb = ArrayView<const H265EncReference>(lt.data(), lt.size());
  CHECK(run(t) == H265EncResult::PictureControlNotSupported);
  t = q;
  Pic small;
  small.make(32, 32);
  std::vector<H265EncReference> wrong = dpb;
  wrong[0].picture = small.planes();
  t.dpb = ArrayView<const H265EncReference>(wrong.data(), wrong.size());
  CHECK(run(t) == H265EncResult::InvalidReferences);
  t = q;
  uint32_t mods[1] = {1};
  t.modification[0] = ArrayView<const uint32_t>(mods, 1);   // an entry past NumPicTotalCurr
  CHECK(run(t) == H265EncResult::PictureControlNotSupported);
  t = q;
  t.poc = 0;   // the picture's own order count in the buffer
  CHECK(run(t) == H265EncResult::PictureControlNotSupported);
  // a source of the wrong size
  {
    ByteBuffer out(g_alloc);
    List<H265EncSlice> sl(g_alloc);
    CHECK(enc.encode(p, small.planes(), rec.writable(), &out, &sl, nullptr) == H265EncResult::PictureControlNotSupported);
  }
}

// ---- the kernel's EncodeFrame (src/Kernel/Dxgkrnl/VideoEncodeHevc.cpp): the arguments d3d12.dll writes, widened textures; the slices and the reconstruction must be
// what the encoder driven directly makes of the same picture, the metadata resolve to D3D12_VIDEO_ENCODER_OUTPUT_METADATA and a record a slice.

// A picture as a runtime keeps NV12 / P010: widened planes, 4 bytes a pixel (8 bits) or four 16-bit words (10 bits, the sample in the top ten).
struct Widened {
  int w = 0, h = 0, bd = 8;
  std::vector<uint8_t> luma, chroma;
  uint64_t px() const { return bd > 8 ? 8 : 4; }
  void make(int w_, int h_, int bd_) {
    w = w_; h = h_; bd = bd_;
    luma.assign(size_t(w) * h * px(), 0);
    chroma.assign(size_t(w / 2) * (h / 2) * px(), 0);
  }
  void from(const Pic& p) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) put(luma, size_t(y) * w + x, p.y[size_t(y) * w + x], 0);
    for (int y = 0; y < h / 2; ++y)
      for (int x = 0; x < w / 2; ++x) {
        put(chroma, size_t(y) * (w / 2) + x, p.u[size_t(y) * (w / 2) + x], 0);
        put(chroma, size_t(y) * (w / 2) + x, p.v[size_t(y) * (w / 2) + x], 1);
      }
  }
  void put(std::vector<uint8_t>& plane, size_t i, uint16_t v, int c) {
    if (bd > 8) { const uint16_t s = uint16_t(v << 6); plane[i * 8 + c * 2] = uint8_t(s); plane[i * 8 + c * 2 + 1] = uint8_t(s >> 8); }
    else plane[i * 4 + c] = uint8_t(v);
  }
  uint16_t get(const std::vector<uint8_t>& plane, size_t i, int c) const {
    if (bd > 8) return uint16_t((plane[i * 8 + c * 2] | plane[i * 8 + c * 2 + 1] << 8) >> 6);
    return plane[i * 4 + c];
  }
  bool equals(const Pic& p) const {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (get(luma, size_t(y) * w + x, 0) != p.y[size_t(y) * w + x]) return false;
    for (int y = 0; y < h / 2; ++y)
      for (int x = 0; x < w / 2; ++x)
        if (get(chroma, size_t(y) * (w / 2) + x, 0) != p.u[size_t(y) * (w / 2) + x] || get(chroma, size_t(y) * (w / 2) + x, 1) != p.v[size_t(y) * (w / 2) + x]) return false;
    return true;
  }
  dxgk::VideoPictureImages images() const {
    return {{ConstByteView(luma.data(), luma.size()), uint64_t(w) * px(), uint32_t(w), uint32_t(h)},
            {ConstByteView(chroma.data(), chroma.size()), uint64_t(w / 2) * px(), uint32_t(w / 2), uint32_t(h / 2)}};
  }
  dxgk::VideoWritablePlaneImage wl() { return {ByteView(luma.data(), luma.size()), uint64_t(w) * px(), uint32_t(w), uint32_t(h)}; }
  dxgk::VideoWritablePlaneImage wc() { return {ByteView(chroma.data(), chroma.size()), uint64_t(w / 2) * px(), uint32_t(w / 2), uint32_t(h / 2)}; }
};

static void put_word(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }

static void test_kernel_side() {
  for (int bd : {8, 10}) {
    const int W = 96, H = 64;
    H265EncSequence seq = base_seq(W, H);
    seq.bit_depth = uint8_t(bd);
    seq.sao = true;
    seq.log2_ctb = 5;
    Texture tex(W / 2 + 80, H / 2 + 60, 2);
    H265PictureEncoder direct(g_alloc);
    struct Ref { Pic pic; Widened wide; int poc; };
    std::vector<Ref> dpb;
    const H265EncFrameType types[3] = {H265EncFrameType::Idr, H265EncFrameType::P, H265EncFrameType::B};
    const int pocs[3] = {0, 4, 2};
    std::vector<uint8_t> heap(dxgk::kVideoEncodeHeapBytes, 0);
    for (int n = 0; n < 3; ++n) {
      Pic src;
      src.make(W, H);
      make_source(tex, pocs[n], bd, W, H, &src);
      Widened wsrc;
      wsrc.make(W, H, bd);
      wsrc.from(src);
      const int qp = n == 2 ? 32 : n == 1 ? 30 : 28;
      // directly
      std::vector<H265EncReference> desc;
      for (const Ref& r : dpb) desc.push_back({r.pic.planes(), r.poc, false, true});
      std::vector<uint8_t> l0, l1;
      if (n == 1) l0 = {0};
      if (n == 2) { l0 = {0}; l1 = {1}; }
      H265EncPicture pic;
      pic.seq = seq;
      pic.type = types[n];
      pic.poc = pocs[n];
      pic.reference = n < 2;
      pic.dpb = ArrayView<const H265EncReference>(desc.data(), desc.size());
      pic.list[0] = ArrayView<const uint8_t>(l0.data(), l0.size());
      pic.list[1] = ArrayView<const uint8_t>(l1.data(), l1.size());
      pic.qp = int8_t(qp);
      pic.slice_mode = H265EncSliceMode::RowsPerSlice;
      pic.slice_value = 1;
      Pic rec;
      rec.make(W, H);
      ByteBuffer out(g_alloc);
      List<H265EncSlice> slices(g_alloc);
      EncStatistics st;
      CHECK(direct.encode(pic, src.planes(), rec.writable(), &out, &slices, &st) == H265EncResult::Ok);
      // through the kernel
      std::vector<uint32_t> w(dxgk::kVideoEncodeParamWords, 0);
      w[0] = dxgk::kVideoEncodeCodecHevc; w[1] = bd == 10; w[2] = W; w[3] = H; w[4] = 0x4;   // (SAO)
      w[5] = 0; w[6] = 2; w[7] = 0; w[8] = 3; w[9] = 1 | 1 << 8;   // CU 8 .. 32, TU 4 .. 32, depths 1
      w[10] = 1; w[12] = 30; w[13] = 1; w[14] = 28; w[15] = 30; w[16] = 32; w[19] = 51;   // CQP 28 / 30 / 32
      w[31] = 3; w[32] = 1; w[33] = 0;
      w[34] = uint32_t(types[n]); w[35] = 0; w[36] = uint32_t(pocs[n]); w[37] = 0; w[38] = n < 2 ? 1 : 0; w[39] = 4;
      w[40] = uint32_t(dpb.size()); w[41] = uint32_t(l0.size()); w[42] = uint32_t(l1.size());
      w[47] = 64; w[48] = uint32_t(bd);
      std::vector<uint8_t> args;
      for (uint32_t v : w) put_word(args, v);
      for (size_t i = 0; i < dpb.size(); ++i) { put_word(args, uint32_t(i)); put_word(args, 1); put_word(args, 0); put_word(args, uint32_t(dpb[i].poc)); put_word(args, 0); }
      for (uint8_t v : l0) put_word(args, v);
      for (uint8_t v : l1) put_word(args, v);
      std::vector<dxgk::VideoPictureImages> refs;
      for (const Ref& r : dpb) refs.push_back(r.wide.images());
      Widened wrec;
      wrec.make(W, H, bd);
      std::vector<uint8_t> bits(1 << 20, 0), meta(dxgk::kVideoEncodeMetadataHeader + 64 * dxgk::kVideoEncodeSubregionBytes, 0);
      dxgk::VideoEncodeJob job;
      job.params = ConstByteView(args.data(), args.size());
      job.heap = ByteView(heap.data(), heap.size());
      job.input = wsrc.images();
      job.has_reconstruction = true;
      job.reconstruction_luma = wrec.wl();
      job.reconstruction_chroma = wrec.wc();
      job.references = ArrayView<const dxgk::VideoPictureImages>(refs.data(), refs.size());
      job.bitstream = ByteView(bits.data(), bits.size());
      job.metadata = ByteView(meta.data(), meta.size());
      CHECK(dxgk::run_video_encode(g_alloc, job));
      std::vector<uint8_t> resolved(72 + 64 * 24, 0xEE);
      CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size())));
      ByteReader m(ConstByteView(resolved.data(), resolved.size()));
      const uint64_t errors = m.le64(), avg_qp = m.le64();
      m.skip(40);
      const uint64_t written = m.le64(), count = m.le64();
      CHECK(errors == 0);
      CHECK(avg_qp == uint64_t(qp));
      CHECK(written == out.size());
      CHECK(count == slices.count());
      bool same_slices = count == slices.count();
      for (uint64_t i = 0; same_slices && i < count; ++i) same_slices = m.le64() == slices[i].size && m.le64() == 0 && m.le64() == slices[i].header_bits;
      CHECK(same_slices);
      CHECK(written == out.size() && memcmp(bits.data(), out.view().data(), out.size()) == 0);
      CHECK(wrec.equals(rec));
      dpb.push_back({rec, wrec, pocs[n]});
    }
    // what the runtime found wrong goes to the metadata, nothing is coded; a reference outside the textures is refused
    std::vector<uint32_t> w(dxgk::kVideoEncodeParamWords, 0);
    w[0] = dxgk::kVideoEncodeCodecHevc; w[46] = 0x4;
    std::vector<uint8_t> args;
    for (uint32_t v : w) put_word(args, v);
    std::vector<uint8_t> meta(dxgk::kVideoEncodeMetadataHeader + 24, 0), bits(64, 0);
    dxgk::VideoEncodeJob job;
    job.params = ConstByteView(args.data(), args.size());
    job.bitstream = ByteView(bits.data(), bits.size());
    job.metadata = ByteView(meta.data(), meta.size());
    CHECK(dxgk::run_video_encode(g_alloc, job));
    std::vector<uint8_t> resolved(72, 0);
    CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size())));
    CHECK(ByteReader(ConstByteView(resolved.data(), 8)).le64() == 0x4);
    // (truncated arguments: refused as a picture control error, not read past)
    job.params = ConstByteView(args.data(), 100);
    CHECK(dxgk::run_video_encode(g_alloc, job));
    CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size())));
    CHECK(ByteReader(ConstByteView(resolved.data(), 8)).le64() == 0x1);
  }
}

int main(int argc, char** argv) {
  const char* dump = argc > 1 ? argv[1] : nullptr;
  test_refusals();
  test_kernel_side();
  test_sequences(dump);
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
