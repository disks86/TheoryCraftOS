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

// Host tests of the AV1 picture encoder (src/Hardware/VideoCodec/Software/Av1PictureEncoder), driven the way a Direct3D 12 program drives its AV1 encoder: the test
// keeps the eight reference slots, names each frame's type, references and refresh flags, and writes the OBUs (sequence header, frame header with the values the
// encoder reports after coding, tile group) itself. Every stream is decoded again by the in-tree decoder (Av1Decoder, the whole OBU path) and every frame must
// equal the encoder's reconstruction sample for sample (8 and 10 bits): key, inter frames of several references, intra-only, error resilient and switch frames,
// tile layouts (uniform, explicit), TX_MODE_LARGEST / SELECT, every interpolation filter and switchable filters, high precision vectors, the reduced transform
// set, CDEF (strength indices chosen by the encoder), loop filter levels and deltas, QP maps (delta q), sizes that are not multiples of 64, CDF updates off.
// Also: the symbol coder against the decoder's Msac, inputs that are refused. `VideoEncodeAv1Test <dir>` writes <dir>/av1_8.ivf + av1_8.yuv (yuv420p) and
// <dir>/av1_10.ivf + av1_10.yuv (yuv420p10le), the reconstructions in order, and av1_8_src.yuv / av1_10_src.yuv, for tests/av1_encode_ffmpeg.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include "Hardware/VideoCodec/Software/Av1.hpp"
#include "Hardware/VideoCodec/Software/Av1Core.hpp"
#include "Hardware/VideoCodec/Software/Av1EncBitstream.hpp"
#include "Hardware/VideoCodec/Software/Av1Msac.hpp"
#include "Hardware/VideoCodec/Software/Av1PictureEncoder.hpp"
#include "Kernel/Dxgkrnl/VideoEncode.hpp"
#include "Kernel/Dxgkrnl/VideoEncodeCodecs.hpp"
#include "Utility/ByteStream.hpp"

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

static uint32_t g_seed = 4321;
static uint32_t rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

// ---- the symbol coder ------------------------------------------------------------------------------------------------------------------------------------
static void test_symbol_coder() {
  for (int round = 0; round < 40; ++round) {
    const bool adapt = round % 3 != 0;
    struct Op { int kind; unsigned n, v, f; };
    std::vector<Op> ops;
    const int count = 200 + int(rnd() % 3000);
    for (int i = 0; i < count; ++i) {
      Op o{int(rnd() % 4), 0, 0, 0};
      if (o.kind == 0) { o.n = 1 + rnd() % 15; o.v = rnd() % (o.n + 1); }
      else if (o.kind == 1) { o.n = 1; o.v = rnd() & 1; }
      else if (o.kind == 2) { o.v = rnd() & 1; o.f = 1 + rnd() % 32767; }
      else o.v = rnd() & 1;
      ops.push_back(o);
    }
    // sixteen CDFs per size, random but valid (decreasing inverse CDFs)
    auto make = [&](std::vector<std::vector<uint16_t>>& cdfs) {
      cdfs.assign(16 * 16, {});
      uint32_t s = 99 + uint32_t(round);
      for (unsigned n = 1; n < 16; ++n)
        for (int k = 0; k < 16; ++k) {
          std::vector<uint16_t>& c = cdfs[n * 16 + k];
          c.assign(n + 2, 0);
          std::vector<unsigned> cuts;
          for (unsigned i = 0; i < n; ++i) { s = s * 1103515245u + 12345u; cuts.push_back(1 + (s >> 8) % 32766); }
          std::sort(cuts.begin(), cuts.end(), std::greater<unsigned>());
          for (unsigned i = 0; i < n; ++i) c[i] = uint16_t(cuts[i]);
          c[n] = 0;   // counter
        }
    };
    std::vector<std::vector<uint16_t>> enc_cdfs, dec_cdfs;
    make(enc_cdfs);
    make(dec_cdfs);
    av1::SymbolWriter w(g_alloc);
    w.reset();
    for (size_t i = 0; i < ops.size(); ++i) {
      const Op& o = ops[i];
      if (o.kind <= 1) w.symbol(ArrayView<uint16_t>(enc_cdfs[o.n * 16 + i % 16].data(), o.n + 1), o.n, o.v, adapt);
      else if (o.kind == 2) w.bool_prob(o.f, o.v);
      else w.bool_equi(o.v);
    }
    ByteBuffer bytes(g_alloc);
    CHECK(w.finish(&bytes));
    av1::Msac m(bytes.view(), !adapt);
    bool same = true;
    for (size_t i = 0; i < ops.size() && same; ++i) {
      const Op& o = ops[i];
      unsigned v;
      if (o.kind <= 1) v = m.symbol_adapt(ArrayView<uint16_t>(dec_cdfs[o.n * 16 + i % 16].data(), o.n + 1), o.n);
      else if (o.kind == 2) v = m.decode_bool(o.f);
      else v = m.bool_equi();
      if (v != o.v) { printf("  symbol %zu of round %d: %u, wanted %u\n", i, round, v, o.v); same = false; }
    }
    CHECK(same);
    CHECK(enc_cdfs == dec_cdfs);
  }
}

// ---- pictures ---------------------------------------------------------------------------------------------------------------------------------------------
struct Planes {
  uint32_t w = 0, h = 0;
  std::vector<uint16_t> y, u, v;
  void make(uint32_t w_, uint32_t h_) {
    w = w_; h = h_;
    y.assign(size_t(w) * h, 0);
    u.assign(size_t((w + 1) / 2) * ((h + 1) / 2), 0);
    v.assign(u.size(), 0);
  }
  EncPlanes16 view() const {
    return {ArrayView<const uint16_t>(y.data(), y.size()), ArrayView<const uint16_t>(u.data(), u.size()), ArrayView<const uint16_t>(v.data(), v.size()), w, h};
  }
  EncWritablePlanes16 wview() { return {ArrayView<uint16_t>(y.data(), y.size()), ArrayView<uint16_t>(u.data(), u.size()), ArrayView<uint16_t>(v.data(), v.size()), w, h}; }
};

struct Texture {
  int w, h;
  std::vector<int> s;
  Texture(int w_, int h_) : w(w_), h(h_), s(size_t(w_) * h_) {
    std::vector<int> a(s.size());
    for (auto& v : a) v = int(rnd() % 1024);
    for (int pass = 0; pass < 3; ++pass) {
      std::vector<int> b(a.size());
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          int sum = 0, n = 0;
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) { sum += a[size_t(std::clamp(y + dy, 0, h - 1)) * w + std::clamp(x + dx, 0, w - 1)]; ++n; }
          b[size_t(y) * w + x] = sum / n;
        }
      a = b;
    }
    for (size_t i = 0; i < s.size(); ++i) s[i] = std::clamp((a[i] - 512) * 4 + 512, 0, 1023);
  }
  int at(int x, int y) const { return s[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; }
};

// Picture t: the texture moving by (3, 1) samples a picture (luma), a gradient in chroma, a bright square moving the other way.
static void make_source(const Texture& tex, int t, Planes* p, uint32_t w, uint32_t h, int bit_depth) {
  p->make(w, h);
  const int shift = 10 - bit_depth;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      int v = tex.at(int(x) + 3 * t + 20, int(y) + t + 10);
      const int sx = 40 + 2 * int(w) / 3 - 4 * t, sy = int(h) / 3;
      if (int(x) >= sx && int(x) < sx + 24 && int(y) >= sy && int(y) < sy + 20) v = 900;
      p->y[size_t(y) * w + x] = uint16_t(v >> shift);
    }
  const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
  for (uint32_t y = 0; y < ch; ++y)
    for (uint32_t x = 0; x < cw; ++x) {
      p->u[size_t(y) * cw + x] = uint16_t((300 + tex.at(int(x) * 2 + 5 * t, int(y) * 2) / 4 + int(x) * 2) >> shift);
      p->v[size_t(y) * cw + x] = uint16_t((700 - tex.at(int(y) * 2, int(x) * 2 + t) / 4 - int(y)) >> shift);
    }
}

static double psnr(const Planes& a, const Planes& b, int bit_depth) {
  double se = 0;
  size_t n = 0;
  for (const auto* pa : {&a.y, &a.u, &a.v}) {
    const auto& pb = pa == &a.y ? b.y : pa == &a.u ? b.u : b.v;
    for (size_t i = 0; i < pa->size(); ++i) { const double d = double((*pa)[i]) - double(pb[i]); se += d * d; ++n; }
  }
  const double peak = double((1 << bit_depth) - 1);
  return se == 0 ? 99.0 : 10 * log10(peak * peak * double(n) / se);
}

// ---- driving the encoder like a program ------------------------------------------------------------------------------------------------------------------
struct Step {
  Av1EncFrameType type = Av1EncFrameType::Key;
  uint8_t refresh = 0xFF;
  uint8_t refs[7] = {0, 0, 0, 0, 0, 0, 0};
  bool error_resilient = false, disable_cdf_update = false, disable_frame_end_update = false;
  int qp_delta = 0;     // added to the options' base_q_idx
};
struct Options {
  uint32_t width = 128, height = 96;
  int bit_depth = 8;
  int base_q = 120;
  Av1EncTiles tiles{};
  Av1EncTxMode tx_mode = Av1EncTxMode::Largest;
  Av1EncFilter filter = Av1EncFilter::EightTap;
  bool hp = false, reduced_tx = false, cdef = false, edge_filter = false, order_hint = true;
  uint8_t order_bits = 7;
  Av1EncLoopFilter lf{};
  int8_t dq_ydc = 0, dq_udc = 0, dq_uac = 0;
  bool qp_map = false;
  Av1EncPrecision precision = Av1EncPrecision::Quarter;
};
struct Encoded {
  std::vector<std::vector<uint8_t>> units;   // temporal units
  std::vector<Planes> recon, source;
  double min_psnr = 99;
  size_t bytes = 0;
  bool ok = true;
};

struct Collector final : av1::IFrameObserver {
  std::vector<Planes> frames;
  void frame_decoded(const av1::SequenceHeader&, const av1::FrameHeader&, ArrayView<const av1::TileData>, const av1::Picture& pic) override {
    Planes p;
    p.make(uint32_t(pic.width()), uint32_t(pic.height()));
    for (int pl = 0; pl < 3; ++pl) {
      std::vector<uint16_t>& dst = pl == 0 ? p.y : pl == 1 ? p.u : p.v;
      const int w = pic.plane_width(pl), h = pic.plane_height(pl);
      const av1::PixelView v = pic.plane(pl);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) dst[size_t(y) * w + x] = v.at(x, y);
    }
    frames.push_back(std::move(p));
  }
};

static Encoded run_sequence(const char* name, const Options& opt, const std::vector<Step>& steps) {
  Encoded e;
  Texture tex(int(opt.width) + 200, int(opt.height) + 100);
  Av1PictureEncoder enc(g_alloc);
  struct Slot { bool valid = false; Planes pic; uint32_t hint = 0; Av1EncFrameType type = Av1EncFrameType::Key; } slots[8];
  Av1EncSequence seq;
  seq.width = opt.width;
  seq.height = opt.height;
  seq.bit_depth = uint8_t(opt.bit_depth);
  seq.enable_order_hint = opt.order_hint;
  seq.order_hint_bits = opt.order_bits;
  seq.enable_cdef = opt.cdef;
  seq.enable_intra_edge_filter = opt.edge_filter;
  const uint32_t sbs = ((opt.width + 63) / 64) * ((opt.height + 63) / 64);
  std::vector<int16_t> qp_map(sbs);
  for (uint32_t i = 0; i < sbs; ++i) qp_map[i] = int16_t(int(i % 5) * 12 - 24);
  for (size_t i = 0; i < steps.size(); ++i) {
    const Step& st = steps[i];
    Planes src, rec;
    make_source(tex, int(i), &src, opt.width, opt.height, opt.bit_depth);
    rec.make(opt.width, opt.height);
    Av1EncPicture pic;
    pic.seq = seq;
    pic.type = st.type;
    pic.error_resilient = st.error_resilient;
    pic.disable_cdf_update = st.disable_cdf_update;
    pic.disable_frame_end_update_cdf = st.disable_frame_end_update;
    pic.allow_high_precision_mv = opt.hp;
    pic.reduced_tx_set = opt.reduced_tx;
    pic.filter = opt.filter;
    pic.tx_mode = opt.tx_mode;
    pic.order_hint = uint32_t(i);
    pic.refresh_frame_flags = st.refresh;
    for (int k = 0; k < 8; ++k) {
      pic.slots[k].valid = slots[k].valid;
      if (slots[k].valid) {
        pic.slots[k].picture = slots[k].pic.view();
        pic.slots[k].order_hint = slots[k].hint;
        pic.slots[k].frame_type = slots[k].type;
      }
    }
    for (int k = 0; k < 7; ++k) pic.ref_frame_idx[k] = st.refs[k];
    pic.base_q_idx = uint8_t(std::clamp(opt.base_q + st.qp_delta, 1, 255));
    pic.delta_q_y_dc = opt.dq_ydc;
    pic.delta_q_u_dc = pic.delta_q_v_dc = opt.dq_udc;
    pic.delta_q_u_ac = pic.delta_q_v_ac = opt.dq_uac;
    if (opt.qp_map) pic.qp_map = ArrayView<const int16_t>(qp_map.data(), qp_map.size());
    pic.loop_filter = opt.lf;
    pic.cdef.damping_minus3 = 2;
    pic.cdef.bits = 2;
    const uint8_t ypri[4] = {0, 4, 8, 12}, ysec[4] = {0, 1, 2, 3}, uvpri[4] = {0, 2, 6, 10}, uvsec[4] = {0, 0, 1, 2};
    for (int k = 0; k < 4; ++k) { pic.cdef.y_pri[k] = ypri[k]; pic.cdef.y_sec[k] = ysec[k]; pic.cdef.uv_pri[k] = uvpri[k]; pic.cdef.uv_sec[k] = uvsec[k]; }
    pic.tiles = opt.tiles;
    pic.precision = opt.precision;
    ByteBuffer data(g_alloc);
    List<Av1EncTile> tiles(g_alloc);
    Av1EncPostEncode post;
    EncStatistics stats;
    const Av1EncResult r = enc.encode(pic, src.view(), rec.wview(), &data, &tiles, &post, &stats);
    CHECK(r == Av1EncResult::Ok);
    if (r != Av1EncResult::Ok) { printf("  %s frame %zu: encode result %d\n", name, i, int(r)); e.ok = false; return e; }
    CHECK(post.primary_ref_frame == kAv1PrimaryRefNone);
    CHECK(post.base_q_idx == pic.base_q_idx);
    CHECK(stats.units == uint64_t((opt.width + 7) / 8) * ((opt.height + 7) / 8));
    const bool filters_off = !opt.lf.level[0] && !opt.lf.level[1] && !opt.cdef;
    if (filters_off) CHECK(enc.last_drift() == 0);
    if (filters_off && enc.last_drift()) printf("  %s frame %zu: %llu samples drift\n", name, i, (unsigned long long)enc.last_drift());
    ByteBuffer tu(g_alloc);
    CHECK(av1::append_obu(&tu, 2 /* OBU_TD */, ConstByteView()));
    if (i == 0) {
      ByteBuffer sh(g_alloc);
      CHECK(Av1PictureEncoder::write_sequence_header(seq, &sh));
      CHECK(tu.append(sh.view().subview(2)));   // (write_sequence_header starts with its own temporal delimiter)
    }
    CHECK(Av1PictureEncoder::write_frame_obu(pic, post, data.view(), tiles.view(), &tu));
    e.units.emplace_back(tu.view().data(), tu.view().data() + tu.size());
    e.bytes += tu.size();
    e.recon.push_back(rec);
    e.source.push_back(src);
    e.min_psnr = std::min(e.min_psnr, psnr(src, rec, opt.bit_depth));
    for (int k = 0; k < 8; ++k)
      if (post.tiles.cols * post.tiles.rows == 0) {}
    for (int k = 0; k < 8; ++k)
      if ((st.type == Av1EncFrameType::Key || st.type == Av1EncFrameType::Switch ? 0xFF : st.refresh) & (1 << k)) {
        slots[k].valid = true;
        slots[k].pic = rec;
        slots[k].hint = uint32_t(i) & ((1u << opt.order_bits) - 1);
        slots[k].type = st.type;
      }
  }
  // the in-tree decoder, the whole OBU path
  Av1Decoder dec(g_alloc);
  Collector col;
  dec.set_frame_observer(&col);
  for (size_t i = 0; i < e.units.size(); ++i) {
    const bool ok = dec.decode(ConstByteView(e.units[i].data(), e.units[i].size()), int64_t(i));
    CHECK(ok);
    if (!ok) { printf("  %s: unit %zu does not decode\n", name, i); e.ok = false; }
    VideoFrame f;
    (void)dec.receive(&f);
  }
  CHECK(col.frames.size() == e.recon.size());
  size_t bad = 0;
  for (size_t i = 0; i < std::min(col.frames.size(), e.recon.size()); ++i) {
    const bool same = col.frames[i].y == e.recon[i].y && col.frames[i].u == e.recon[i].u && col.frames[i].v == e.recon[i].v;
    if (!same) {
      ++bad;
      size_t diff = 0;
      for (size_t k = 0; k < e.recon[i].y.size(); ++k) diff += col.frames[i].y[k] != e.recon[i].y[k];
      printf("  %s: frame %zu differs from the reconstruction (%zu luma samples)\n", name, i, diff);
    }
  }
  CHECK(bad == 0);
  if (bad || col.frames.size() != e.recon.size()) e.ok = false;
  printf("  %-34s %zu frames, %zu bytes, lowest PSNR %.2f dB\n", name, e.recon.size(), e.bytes, e.min_psnr);
  return e;
}

static Step key() { return Step{}; }
static Step inter(uint8_t refresh, std::initializer_list<uint8_t> refs) {
  Step s;
  s.type = Av1EncFrameType::Inter;
  s.refresh = refresh;
  int k = 0;
  for (uint8_t r : refs) s.refs[k++] = r;
  for (; k < 7; ++k) s.refs[k] = s.refs[0];
  return s;
}

static void write_file(const std::string& path, const void* data, size_t size) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(data, 1, size, f);
  fclose(f);
}
static void put_le(std::vector<uint8_t>& b, uint64_t v, int n) { for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i))); }
static void dump(const std::string& dir, const char* stem, const Options& opt, const Encoded& e) {
  std::vector<uint8_t> ivf;
  ivf.insert(ivf.end(), {'D', 'K', 'I', 'F'});
  put_le(ivf, 0, 2);
  put_le(ivf, 32, 2);
  ivf.insert(ivf.end(), {'A', 'V', '0', '1'});
  put_le(ivf, opt.width, 2);
  put_le(ivf, opt.height, 2);
  put_le(ivf, 30, 4);
  put_le(ivf, 1, 4);
  put_le(ivf, e.units.size(), 4);
  put_le(ivf, 0, 4);
  for (size_t i = 0; i < e.units.size(); ++i) {
    put_le(ivf, e.units[i].size(), 4);
    put_le(ivf, i, 8);
    ivf.insert(ivf.end(), e.units[i].begin(), e.units[i].end());
  }
  write_file(dir + "/" + stem + ".ivf", ivf.data(), ivf.size());
  for (int which = 0; which < 2; ++which) {
    std::vector<uint8_t> raw;
    for (const Planes& p : which ? e.source : e.recon)
      for (const auto* pl : {&p.y, &p.u, &p.v})
        for (uint16_t s : *pl) {
          if (opt.bit_depth == 8) raw.push_back(uint8_t(s));
          else put_le(raw, s, 2);
        }
    write_file(dir + "/" + stem + (which ? "_src.yuv" : ".yuv"), raw.data(), raw.size());
  }
}

// ---- the sequences ----------------------------------------------------------------------------------------------------------------------------------------
static void test_sequences(const char* dir) {
  // 8 bits, 200x120 (not a multiple of 64): key, inter frames of one and of several slots, an intra-only frame, an error resilient frame, a switch frame;
  // 2x2 uniform tiles, TX_MODE_SELECT, switchable filters, high precision vectors, CDEF, loop filter with deltas
  {
    Options o;
    o.width = 200;
    o.height = 120;
    o.tiles.uniform = true;
    o.tiles.cols = 2;
    o.tiles.rows = 2;
    o.tiles.context_update_tile_id = 3;
    o.tx_mode = Av1EncTxMode::Select;
    o.filter = Av1EncFilter::Switchable;
    o.hp = true;
    o.cdef = true;
    o.lf.level[0] = 10; o.lf.level[1] = 8; o.lf.level[2] = 4; o.lf.level[3] = 5;
    o.lf.sharpness = 2;
    o.lf.delta_enabled = true;
    o.lf.delta_update = true;
    o.lf.ref_deltas[1] = 2;
    o.lf.mode_deltas[1] = -1;
    Step ir;
    ir.type = Av1EncFrameType::IntraOnly;
    ir.refresh = 0x04;
    Step er = inter(0x08, {1, 0, 2, 1, 0, 0, 1});
    er.error_resilient = true;
    Step sw;
    sw.type = Av1EncFrameType::Switch;
    for (int k = 0; k < 7; ++k) sw.refs[k] = uint8_t(k % 4);
    Step nocdf = inter(0x01, {0, 3});
    nocdf.disable_cdf_update = true;
    Step noend = inter(0x02, {1, 0});
    noend.disable_frame_end_update = true;
    const std::vector<Step> steps = {key(), inter(0x01, {0}), inter(0x02, {0, 1}), ir, inter(0x01, {0, 1, 2, 0, 1, 2, 0}), er, inter(0x01, {3, 0}), sw,
                                     nocdf, noend, inter(0x10, {0, 1, 2, 3})};
    const Encoded e = run_sequence("8-bit 200x120 tools", o, steps);
    CHECK(e.ok);
    CHECK(e.min_psnr > 30);
    if (dir) dump(dir, "av1_8", o, e);
  }
  // 10 bits, 128x96: explicit tiles, TX_MODE_LARGEST, the smooth filter, quarter samples, QP map (delta q), no in-loop filters (the reconstruction is the
  // encoder's own: checked for drift), CDF updates off in one frame
  {
    Options o;
    o.width = 128;
    o.height = 96;
    o.bit_depth = 10;
    o.tiles.uniform = false;
    o.tiles.cols = 2;
    o.tiles.rows = 1;
    o.tiles.col_width_sb[0] = 1;
    o.tiles.col_width_sb[1] = 1;
    o.tiles.row_height_sb[0] = 2;
    o.tiles.context_update_tile_id = 1;
    o.filter = Av1EncFilter::Smooth;
    o.qp_map = true;
    Step nocdf = inter(0x02, {0});
    nocdf.disable_cdf_update = true;
    const std::vector<Step> steps = {key(), inter(0x01, {0}), inter(0x02, {0, 1}), nocdf, inter(0x01, {1, 0})};
    const Encoded e = run_sequence("10-bit 128x96 explicit tiles", o, steps);
    CHECK(e.ok);
    CHECK(e.min_psnr > 30);
    if (dir) dump(dir, "av1_10", o, e);
  }
  // the other filters, the reduced transform set, DC / AC quantizer deltas, the intra edge filter, half sample search, 8 bits, CDEF on 10 bits
  {
    const Av1EncFilter filters[3] = {Av1EncFilter::Sharp, Av1EncFilter::Bilinear, Av1EncFilter::EightTap};
    for (int k = 0; k < 3; ++k) {
      Options o;
      o.width = 96 + 32 * k;
      o.height = 64;
      o.bit_depth = k == 2 ? 10 : 8;
      o.tiles.uniform = true;
      o.tiles.cols = o.tiles.rows = 1;
      o.filter = filters[k];
      o.reduced_tx = true;
      o.dq_ydc = -4;
      o.dq_udc = 3;
      o.dq_uac = -2;
      o.edge_filter = true;
      o.cdef = k == 2;
      o.lf.level[0] = o.lf.level[1] = uint8_t(6 * k);
      o.precision = k == 0 ? Av1EncPrecision::Half : Av1EncPrecision::Quarter;
      o.hp = k == 1;
      o.base_q = 60 + 60 * k;
      char name[64];
      snprintf(name, sizeof name, "filter %d, reduced set, deltas", int(filters[k]));
      const Encoded e = run_sequence(name, o, {key(), inter(0x01, {0}), inter(0x03, {0}), inter(0x01, {1, 0})});
      CHECK(e.ok);
    }
  }
  // order hints off, a high quantizer
  {
    Options o;
    o.width = 64;
    o.height = 64;
    o.order_hint = false;
    o.base_q = 250;
    o.tiles.uniform = true;
    o.tiles.cols = o.tiles.rows = 1;
    const Encoded e = run_sequence("no order hints, qindex 250", o, {key(), inter(0x01, {0}), inter(0x01, {0})});
    CHECK(e.ok);
  }
}

// ---- refusals --------------------------------------------------------------------------------------------------------------------------------------------
static void test_refusals() {
  Av1PictureEncoder enc(g_alloc);
  Planes src, rec;
  src.make(64, 64);
  rec.make(64, 64);
  Av1EncPicture good;
  good.seq.width = good.seq.height = 64;
  ByteBuffer data(g_alloc);
  List<Av1EncTile> tiles(g_alloc);
  Av1EncPostEncode post;
  auto run = [&](const Av1EncPicture& p) { data.clear(); return enc.encode(p, src.view(), rec.wview(), &data, &tiles, &post, nullptr); };
  CHECK(run(good) == Av1EncResult::Ok);
  Av1EncPicture p = good;
  p.base_q_idx = 0;
  CHECK(run(p) == Av1EncResult::PictureControlNotSupported);   // lossless
  p = good;
  p.tx_mode = Av1EncTxMode::Only4x4;
  CHECK(run(p) == Av1EncResult::PictureControlNotSupported);
  p = good;
  p.delta_q_v_ac = 5;
  CHECK(run(p) == Av1EncResult::PictureControlNotSupported);   // (no separate_uv_delta_q)
  p = good;
  p.refresh_frame_flags = 0x0F;
  CHECK(run(p) == Av1EncResult::PictureControlNotSupported);   // a shown key frame refreshes every slot
  p = good;
  p.type = Av1EncFrameType::IntraOnly;
  CHECK(run(p) == Av1EncResult::PictureControlNotSupported);   // an intra-only frame cannot refresh every slot
  p = good;
  p.type = Av1EncFrameType::Inter;
  p.refresh_frame_flags = 1;
  CHECK(run(p) == Av1EncResult::InvalidReferences);            // empty slots
  p.slots[0].valid = true;
  p.slots[0].picture = rec.view();
  CHECK(run(p) == Av1EncResult::Ok);
  Planes small;
  small.make(32, 32);
  p.slots[0].picture = small.view();
  CHECK(run(p) == Av1EncResult::InvalidReferences);            // a reference of another size
  p = good;
  p.seq.width = 63;
  CHECK(run(p) == Av1EncResult::PictureControlNotSupported);
  p = good;
  p.tiles.uniform = false;
  p.tiles.cols = 2;
  p.tiles.col_width_sb[0] = 1;
  p.tiles.col_width_sb[1] = 1;
  p.tiles.rows = 1;
  p.tiles.row_height_sb[0] = 1;
  CHECK(run(p) == Av1EncResult::SubregionLayoutNotSupported);  // two columns of a one superblock wide frame
  p = good;
  p.tiles.context_update_tile_id = 1;
  CHECK(run(p) == Av1EncResult::SubregionLayoutNotSupported);
  p = good;
  p.seq.enable_cdef = true;
  p.cdef.y_pri[0] = 16;
  CHECK(run(p) == Av1EncResult::PictureControlNotSupported);
  p = good;
  p.loop_filter.level[0] = 64;
  CHECK(run(p) == Av1EncResult::PictureControlNotSupported);
  std::vector<int16_t> map(3);
  p = good;
  p.qp_map = ArrayView<const int16_t>(map.data(), map.size());
  CHECK(run(p) == Av1EncResult::PictureControlNotSupported);   // one value per superblock
  // the uniform layout: 4 columns of a 5 superblock wide frame are 3 of 2, 2, 1
  Av1EncTiles t;
  t.cols = 4;
  t.rows = 1;
  CHECK(Av1PictureEncoder::tile_layout(320, 64, &t));
  CHECK(t.cols == 3 && t.cols_log2 == 2 && t.col_width_sb[0] == 2 && t.col_width_sb[2] == 1);
  // the OBU writers refuse what they cannot write
  ByteBuffer out(g_alloc);
  Av1EncTile one{0, 10};
  CHECK(!Av1PictureEncoder::write_frame_obu(good, post, ConstByteView(), ArrayView<const Av1EncTile>(&one, 1), &out));
}

// ---- the kernel's EncodeFrame (src/Kernel/Dxgkrnl/VideoEncodeAv1.cpp): the arguments d3d12.dll writes, widened textures; the tiles and the reconstruction must be what
// the encoder driven directly makes of the same frame, the metadata resolve to D3D12_VIDEO_ENCODER_OUTPUT_METADATA, a record a tile, the tile layout and the
// post-encode values.
struct Widened {
  uint32_t w = 0, h = 0;
  int bd = 8;
  std::vector<uint8_t> luma, chroma;
  uint64_t px() const { return bd > 8 ? 8 : 4; }
  void make(uint32_t w_, uint32_t h_, int bd_) { w = w_; h = h_; bd = bd_; luma.assign(size_t(w) * h * px(), 0); chroma.assign(size_t(w / 2) * (h / 2) * px(), 0); }
  void put(std::vector<uint8_t>& plane, size_t i, uint16_t v, int c) {
    if (bd > 8) { const uint16_t s = uint16_t(v << 6); plane[i * 8 + c * 2] = uint8_t(s); plane[i * 8 + c * 2 + 1] = uint8_t(s >> 8); }
    else plane[i * 4 + c] = uint8_t(v);
  }
  uint16_t get(const std::vector<uint8_t>& plane, size_t i, int c) const { return bd > 8 ? uint16_t((plane[i * 8 + c * 2] | plane[i * 8 + c * 2 + 1] << 8) >> 6) : plane[i * 4 + c]; }
  void from(const Planes& p) {
    for (size_t i = 0; i < size_t(w) * h; ++i) put(luma, i, p.y[i], 0);
    for (size_t i = 0; i < size_t(w / 2) * (h / 2); ++i) { put(chroma, i, p.u[i], 0); put(chroma, i, p.v[i], 1); }
  }
  bool equals(const Planes& p) const {
    for (size_t i = 0; i < size_t(w) * h; ++i) if (get(luma, i, 0) != p.y[i]) return false;
    for (size_t i = 0; i < size_t(w / 2) * (h / 2); ++i) if (get(chroma, i, 0) != p.u[i] || get(chroma, i, 1) != p.v[i]) return false;
    return true;
  }
  dxgk::VideoPictureImages images() const {
    return {{ConstByteView(luma.data(), luma.size()), uint64_t(w) * px(), w, h}, {ConstByteView(chroma.data(), chroma.size()), uint64_t(w / 2) * px(), w / 2, h / 2}};
  }
  dxgk::VideoWritablePlaneImage wl() { return {ByteView(luma.data(), luma.size()), uint64_t(w) * px(), w, h}; }
  dxgk::VideoWritablePlaneImage wc() { return {ByteView(chroma.data(), chroma.size()), uint64_t(w / 2) * px(), w / 2, h / 2}; }
};
static void put_word(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }

static void test_kernel_side() {
  for (int bd : {8, 10}) {
    const uint32_t W = 160, H = 96;
    Texture tex(int(W) + 120, int(H) + 60);
    Av1PictureEncoder direct(g_alloc);
    Planes slot_pic[8];
    Widened slot_wide[8];
    bool slot_valid[8] = {};
    uint32_t slot_hint[8] = {};
    std::vector<uint8_t> heap(dxgk::kVideoEncodeHeapBytes, 0);
    for (int n = 0; n < 3; ++n) {
      Planes src;
      make_source(tex, n, &src, W, H, bd);
      Widened wsrc;
      wsrc.make(W, H, bd);
      wsrc.from(src);
      const bool key = n == 0;
      const uint32_t qindex = key ? 100 : 130;
      // directly
      Av1EncPicture pic;
      pic.seq.width = W; pic.seq.height = H; pic.seq.bit_depth = uint8_t(bd);
      pic.seq.enable_order_hint = true; pic.seq.order_hint_bits = 7; pic.seq.enable_cdef = true; pic.seq.enable_intra_edge_filter = true;
      pic.type = key ? Av1EncFrameType::Key : Av1EncFrameType::Inter;
      pic.allow_high_precision_mv = !key;
      pic.filter = n == 2 ? Av1EncFilter::Switchable : Av1EncFilter::EightTap;
      pic.tx_mode = n == 1 ? Av1EncTxMode::Select : Av1EncTxMode::Largest;
      pic.order_hint = uint32_t(n);
      pic.refresh_frame_flags = key ? 0xFF : uint8_t(1u << n);
      for (int i = 0; i < 8; ++i)
        if (slot_valid[i]) {
          pic.slots[i].valid = true;
          pic.slots[i].picture = slot_pic[i].view();
          pic.slots[i].order_hint = slot_hint[i];
          pic.slots[i].frame_type = slot_hint[i] == 0 ? Av1EncFrameType::Key : Av1EncFrameType::Inter;
        }
      const uint8_t refs[7] = {uint8_t(n == 2 ? 1 : 0), 0, 0, 0, 0, 0, 0};
      for (int i = 0; i < 7; ++i) pic.ref_frame_idx[i] = refs[i];
      pic.base_q_idx = uint8_t(qindex);
      pic.loop_filter.level[0] = 10; pic.loop_filter.level[1] = 8; pic.loop_filter.level[2] = 4; pic.loop_filter.level[3] = 5; pic.loop_filter.sharpness = 1;
      pic.cdef.bits = 1; pic.cdef.damping_minus3 = 2; pic.cdef.y_pri[1] = 4; pic.cdef.y_sec[1] = 1; pic.cdef.uv_pri[1] = 2;
      pic.tiles.uniform = true; pic.tiles.cols = 2; pic.tiles.rows = 1;
      CHECK(Av1PictureEncoder::tile_layout(W, H, &pic.tiles));
      Planes rec;
      rec.make(W, H);
      ByteBuffer out(g_alloc);
      List<Av1EncTile> tiles(g_alloc);
      Av1EncPostEncode post;
      EncStatistics st;
      CHECK(direct.encode(pic, src.view(), rec.wview(), &out, &tiles, &post, &st) == Av1EncResult::Ok);
      // through the kernel
      std::vector<uint32_t> w(dxgk::kVideoEncodeParamWords, 0);
      w[0] = dxgk::kVideoEncodeCodecAv1; w[1] = 0; w[2] = W; w[3] = H; w[4] = 0x4 | 0x1000 | 0x8000 | 0x800000; w[5] = 6; w[6] = uint32_t(bd);
      w[7] = 5; w[8] = 1; w[9] = 2;   // a uniform grid of 2 x 1
      w[10] = 1; w[12] = 30; w[13] = 1; w[14] = 100; w[15] = 130; w[16] = 150; w[19] = 255;
      w[34] = key ? 0 : 1; w[35] = key ? 0 : 0x4000; w[37] = uint32_t(pic.filter); w[38] = uint32_t(pic.tx_mode); w[39] = 8; w[40] = uint32_t(n);
      w[44] = pic.refresh_frame_flags; w[47] = 64;
      std::vector<uint8_t> args;
      for (uint32_t v : w) put_word(args, v);
      for (uint8_t r : refs) put_word(args, r);
      std::vector<dxgk::VideoPictureImages> textures;
      for (int i = 0; i < 8; ++i) {
        if (slot_valid[i]) textures.push_back(slot_wide[i].images());
        put_word(args, slot_valid[i] ? uint32_t(textures.size() - 1) : 0xFFFFFFFFu);
        put_word(args, slot_hint[i]);
        put_word(args, slot_hint[i] == 0 ? 0 : 1);
        put_word(args, 0);
      }
      for (int i = 0; i < 6; ++i) put_word(args, 0);   // restoration
      const uint32_t lf[18] = {10, 8, 4, 5, 1, 0, 0, 1, 0, 0, 0, uint32_t(-1), 0, uint32_t(-1), uint32_t(-1), 0, 0, 0};
      for (uint32_t v : lf) put_word(args, v);
      for (int i = 0; i < 3; ++i) put_word(args, 0);
      for (int i = 0; i < 10; ++i) put_word(args, i == 0 ? 77 : 0);   // (BaseQIndex is the rate controller's)
      for (int i = 0; i < 2; ++i) put_word(args, 0);
      uint32_t cdef[34] = {1, 2};
      cdef[2 + 1] = 4; cdef[10 + 1] = 2; cdef[18 + 1] = 1;
      for (uint32_t v : cdef) put_word(args, v);
      Widened wrec;
      wrec.make(W, H, bd);
      const size_t meta_bytes = dxgk::kVideoEncodeMetadataHeader + 64 * dxgk::kVideoEncodeSubregionBytes + dxgk::kVideoEncodeAv1TilesBytes + dxgk::kVideoEncodeAv1PostEncodeBytes;
      std::vector<uint8_t> bits(1 << 20, 0), meta(meta_bytes, 0);
      dxgk::VideoEncodeJob job;
      job.params = ConstByteView(args.data(), args.size());
      job.heap = ByteView(heap.data(), heap.size());
      job.input = wsrc.images();
      job.has_reconstruction = true;
      job.reconstruction_luma = wrec.wl();
      job.reconstruction_chroma = wrec.wc();
      job.references = ArrayView<const dxgk::VideoPictureImages>(textures.data(), textures.size());
      job.bitstream = ByteView(bits.data(), bits.size());
      job.metadata = ByteView(meta.data(), meta.size());
      CHECK(dxgk::run_video_encode(g_alloc, job));
      std::vector<uint8_t> resolved(meta_bytes, 0xEE);
      CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size())));
      ByteReader m(ConstByteView(resolved.data(), resolved.size()));
      const uint64_t errors = m.le64(), avg_qp = m.le64();
      m.skip(40);
      const uint64_t written = m.le64(), count = m.le64();
      CHECK(errors == 0);
      if (errors) printf("  frame %d (%d bits): errors %llx\n", n, bd, (unsigned long long)errors);
      CHECK(avg_qp == qindex);
      CHECK(written == out.size() && memcmp(bits.data(), out.view().data(), out.size()) == 0);
      CHECK(count == tiles.count() && count == 2);
      for (uint64_t i = 0; i < count && i < tiles.count(); ++i) {
        CHECK(m.le64() == tiles[i].size);
        CHECK(m.le64() == 0);
        CHECK(m.le64() == 0);
      }
      CHECK(m.le64() == 1);   // RowCount
      CHECK(m.le64() == 2);   // ColCount
      CHECK(m.le64() == 2);   // RowHeights[0]: 96 lines are 2 superblocks
      m.skip(63 * 8);
      CHECK(m.le64() == 2);   // ColWidths[0]: 160 samples are 3 superblocks, cut 2 + 1
      CHECK(m.le64() == 1);
      m.skip(62 * 8);
      CHECK(m.le64() == 0);   // ContextUpdateTileId
      CHECK(m.le64() == 0);   // CompoundPredictionType
      CHECK(m.le64() == 10 && m.le64() == 8 && m.le64() == 4 && m.le64() == 5 && m.le64() == 1);   // the loop filter as given
      m.skip(8 * 13 + 24);
      CHECK(m.le64() == qindex);   // BaseQIndex: what was coded
      m.skip(9 * 8);
      CHECK(m.le64() == 0);        // DeltaQPresent
      m.skip(8);
      CHECK(m.le64() == 1 && m.le64() == 2);   // CDEF bits, damping
      m.skip(32 * 8 + 608);
      CHECK(m.le64() == 7);        // PrimaryRefFrame: none
      CHECK(m.le64() == refs[0]);
      CHECK(m.remaining() >= 6 * 8);
      CHECK(wrec.equals(rec));
      for (int i = 0; i < 8; ++i)
        if (pic.refresh_frame_flags & (1u << i)) { slot_pic[i] = rec; slot_wide[i] = wrec; slot_valid[i] = true; slot_hint[i] = uint32_t(n); }
    }
  }
  // what the runtime found wrong, truncated arguments: the metadata says why, nothing is read past
  std::vector<uint32_t> w(dxgk::kVideoEncodeParamWords, 0);
  w[0] = dxgk::kVideoEncodeCodecAv1; w[46] = 0x2;
  std::vector<uint8_t> args;
  for (uint32_t v : w) put_word(args, v);
  const size_t meta_bytes = dxgk::kVideoEncodeMetadataHeader + dxgk::kVideoEncodeAv1TilesBytes + dxgk::kVideoEncodeAv1PostEncodeBytes;
  std::vector<uint8_t> meta(meta_bytes, 0), bits(64, 0), resolved(meta_bytes, 0);
  dxgk::VideoEncodeJob job;
  job.params = ConstByteView(args.data(), args.size());
  job.bitstream = ByteView(bits.data(), bits.size());
  job.metadata = ByteView(meta.data(), meta.size());
  CHECK(dxgk::run_video_encode(g_alloc, job));
  CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size())));
  CHECK(ByteReader(ConstByteView(resolved.data(), 8)).le64() == 0x2);
  CHECK(!dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), 100)));   // (no room for the AV1 layout)
  job.params = ConstByteView(args.data(), 120);
  CHECK(dxgk::run_video_encode(g_alloc, job));
  CHECK(dxgk::resolve_video_encode_metadata(ConstByteView(meta.data(), meta.size()), ByteView(resolved.data(), resolved.size())));
  CHECK(ByteReader(ConstByteView(resolved.data(), 8)).le64() == 0x1);
}

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : nullptr;
  test_symbol_coder();
  test_refusals();
  test_kernel_side();
  test_sequences(dir);
  printf("VideoEncodeAv1Test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
