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

// The H.265 parameter sets and slice headers of the hardware encoder (IntelHevcHeaders): written for several sizes, parsed back by an independent bit reader that follows the
// specification (Annex B -> emulation prevention removed -> profile_tier_level, VPS, SPS, PPS and slice header syntax), and fed to the CPU H.265 decoder, which must accept
// the parameter sets and report the cropped size.
#include <stdio.h>
#include <stdlib.h>
#include <vector>

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "Hardware/VideoCodec/Intel/IntelHevcHeaders.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"

using namespace hardware;
using namespace hardware::intel_enc;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

namespace {

// Reads an RBSP (emulation prevention bytes already removed).
class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& d) : d_(d) {}
  uint32_t u(uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const size_t byte = pos_ >> 3;
      if (byte >= d_.size()) { overrun = true; return 0; }
      v = (v << 1) | ((d_[byte] >> (7 - (pos_ & 7))) & 1u);
      ++pos_;
    }
    return v;
  }
  uint32_t ue() {
    uint32_t zeros = 0;
    while (!overrun && u(1) == 0) { if (++zeros > 31) { overrun = true; return 0; } }
    return zeros ? ((1u << zeros) - 1) + u(zeros) : 0;
  }
  int32_t se() { const uint32_t k = ue(); return (k & 1) ? int32_t((k + 1) / 2) : -int32_t(k / 2); }
  size_t pos() const { return pos_; }
  // rbsp_trailing_bits at the end: a one, then zeros up to the end of the data.
  bool trailing_ok() {
    if (u(1) != 1) return false;
    while (pos_ & 7) if (u(1) != 0) return false;
    return pos_ == d_.size() * 8;
  }
  bool overrun = false;
 private:
  const std::vector<uint8_t>& d_;
  size_t pos_ = 0;
};

struct Nal {
  uint32_t type = 0, layer = 0, tid_plus1 = 0;
  std::vector<uint8_t> rbsp;
  bool epb_ok = true;
};

// Splits one NAL unit off `v` starting at 00 00 00 01 (the unit ends at the buffer end) and removes the emulation prevention bytes.
Nal parse_nal(const std::vector<uint8_t>& v, size_t start, size_t end, bool* ok) {
  Nal n;
  *ok = end - start >= 6 && v[start] == 0 && v[start + 1] == 0 && v[start + 2] == 0 && v[start + 3] == 1;
  if (!*ok) return n;
  const uint8_t h0 = v[start + 4], h1 = v[start + 5];
  *ok = (h0 & 0x80) == 0;
  n.type = (h0 >> 1) & 0x3F;
  n.layer = ((h0 & 1) << 5) | (h1 >> 3);
  n.tid_plus1 = h1 & 7;
  uint32_t zeros = 0;
  for (size_t i = start + 6; i < end; ++i) {
    if (zeros >= 2 && v[i] <= 3) {
      if (v[i] != 3) n.epb_ok = false;   // 00 00 0x (x < 3) must never appear
      else { zeros = 0; continue; }
    }
    zeros = v[i] == 0 ? zeros + 1 : 0;
    n.rbsp.push_back(v[i]);
  }
  return n;
}

struct Ptl { uint32_t space, tier, profile, compat, progressive, interlaced, nonpacked, frame_only, reserved_hi, reserved_lo, level; };
Ptl read_ptl(Reader& r) {
  Ptl p;
  p.space = r.u(2); p.tier = r.u(1); p.profile = r.u(5); p.compat = r.u(32);
  p.progressive = r.u(1); p.interlaced = r.u(1); p.nonpacked = r.u(1); p.frame_only = r.u(1);
  p.reserved_hi = r.u(32); p.reserved_lo = r.u(12);   // 44 bits
  p.level = r.u(8);
  return p;
}
bool ptl_ok(const Ptl& p, uint32_t level) {
  return p.space == 0 && p.tier == 0 && p.profile == 1 && (p.compat & (0x80000000u >> 1)) && (p.compat & (0x80000000u >> 2)) && p.progressive == 1 && p.interlaced == 0 &&
         p.nonpacked == 0 && p.frame_only == 1 && p.reserved_hi == 0 && p.reserved_lo == 0 && p.level == level;
}

// Appends in `all` and returns the units' start offsets.
struct Stream {
  std::vector<uint8_t> bytes;
  size_t vps = 0, sps = 0, pps = 0, end = 0;
};

Stream build(allocators::IAllocator& a, const HevcStreamHeaders& s, bool* ok) {
  Stream st;
  ByteBuffer buf(a);
  uint32_t b1 = 0, b2 = 0, b3 = 0;
  *ok = write_hevc_vps(s, &buf, &b1);
  st.sps = buf.size();
  *ok = *ok && write_hevc_sps(s, &buf, &b2);
  st.pps = buf.size();
  *ok = *ok && write_hevc_pps(s, &buf, &b3);
  st.end = buf.size();
  *ok = *ok && st.sps * 8 == b1 && st.pps - st.sps == b2 / 8 && st.end - st.pps == b3 / 8 && b1 % 8 == 0 && b2 % 8 == 0 && b3 % 8 == 0;
  for (uint64_t i = 0; i < buf.size(); ++i) st.bytes.push_back(buf.view()[i]);
  return st;
}

void check_parameter_sets(allocators::IAllocator& a, const HevcStreamHeaders& s) {
  bool ok = false;
  const Stream st = build(a, s, &ok);
  CHECK(ok);
  if (!ok) return;
  const uint32_t cw = (s.width + 7) & ~7u, ch = (s.height + 7) & ~7u;

  // VPS
  Nal vps = parse_nal(st.bytes, 0, st.sps, &ok);
  CHECK(ok && vps.type == 32 && vps.layer == 0 && vps.tid_plus1 == 1 && vps.epb_ok);
  {
    Reader r(vps.rbsp);
    CHECK(r.u(4) == 0 && r.u(1) == 1 && r.u(1) == 1 && r.u(6) == 0 && r.u(3) == 0 && r.u(1) == 1 && r.u(16) == 0xFFFF);
    CHECK(ptl_ok(read_ptl(r), s.level_idc));
    CHECK(r.u(1) == 1 && r.ue() == 1 && r.ue() == 0 && r.ue() == 0);
    CHECK(r.u(6) == 0 && r.ue() == 0 && r.u(1) == 0 && r.u(1) == 0);
    CHECK(r.trailing_ok() && !r.overrun);
  }
  // SPS
  Nal sps = parse_nal(st.bytes, st.sps, st.pps, &ok);
  CHECK(ok && sps.type == 33 && sps.layer == 0 && sps.tid_plus1 == 1 && sps.epb_ok);
  {
    Reader r(sps.rbsp);
    CHECK(r.u(4) == 0 && r.u(3) == 0 && r.u(1) == 1);
    CHECK(ptl_ok(read_ptl(r), s.level_idc));
    CHECK(r.ue() == 0 && r.ue() == 1);
    const uint32_t w = r.ue(), h = r.ue();
    CHECK(w == cw && h == ch);
    uint32_t l = 0, rr = 0, t = 0, bb = 0;
    if (r.u(1)) { l = r.ue(); rr = r.ue(); t = r.ue(); bb = r.ue(); }
    CHECK(w - 2 * (l + rr) == s.width && h - 2 * (t + bb) == s.height);   // SubWidthC = SubHeightC = 2
    CHECK(((cw != s.width || ch != s.height) ? 1 : 0) == ((l | rr | t | bb) != 0 || cw != s.width || ch != s.height ? 1 : 0));
    CHECK(r.ue() == 0 && r.ue() == 0);
    CHECK(r.ue() + 4 == s.log2_max_poc_lsb);
    CHECK(r.u(1) == 1 && r.ue() == 1 && r.ue() == 0 && r.ue() == 0);   // dpb size 2, no reordering, no latency limit
    CHECK(r.ue() + 3 == kHevcLog2MinCb);
    const uint32_t diff_cb = r.ue();
    CHECK(3 + diff_cb == kHevcLog2Ctb && diff_cb == 3);
    CHECK(r.ue() + 2 == kHevcLog2MinTb);
    CHECK(r.ue() + kHevcLog2MinTb == kHevcLog2MaxTb);
    CHECK(r.ue() == kHevcMaxTransformDepthInter && r.ue() == kHevcMaxTransformDepthIntra);
    CHECK(r.u(1) == 0 && r.u(1) == 0 && r.u(1) == 0 && r.u(1) == 0);   // scaling lists, AMP, SAO, PCM
    CHECK(r.ue() == 0 && r.u(1) == 0 && r.u(1) == 0 && r.u(1) == 1 && r.u(1) == 0 && r.u(1) == 0);   // no RPS, no long term, no TMVP, strong intra smoothing, no VUI, no extension
    CHECK(r.trailing_ok() && !r.overrun);
  }
  // PPS
  Nal pps = parse_nal(st.bytes, st.pps, st.end, &ok);
  CHECK(ok && pps.type == 34 && pps.layer == 0 && pps.tid_plus1 == 1 && pps.epb_ok);
  {
    Reader r(pps.rbsp);
    CHECK(r.ue() == 0 && r.ue() == 0 && r.u(1) == 0 && r.u(1) == 0 && r.u(3) == 0 && r.u(1) == 0 && r.u(1) == 0);
    CHECK(r.ue() == 0 && r.ue() == 0 && r.se() == 0 && r.u(1) == 0 && r.u(1) == 0);
    CHECK(r.u(1) == 1);                          // cu_qp_delta_enabled_flag
    CHECK(r.ue() == kHevcDiffCuQpDeltaDepth);
    CHECK(r.se() == s.cb_qp_offset && r.se() == s.cr_qp_offset);
    CHECK(r.u(1) == 0 && r.u(1) == 0 && r.u(1) == 0 && r.u(1) == 0 && r.u(1) == 0 && r.u(1) == 0);   // chroma offsets present, weighted x2, transquant bypass, tiles, WPP
    CHECK(r.u(1) == 1);                          // loop filter across slices
    CHECK(r.u(1) == 1 && r.u(1) == 0 && r.u(1) == 0 && r.se() == 0 && r.se() == 0);   // deblocking control, no override, enabled, offsets
    CHECK(r.u(1) == 0 && r.u(1) == 0 && r.ue() + 2 == kHevcLog2ParMrgLevel && r.u(1) == 0 && r.u(1) == 0);
    CHECK(r.trailing_ok() && !r.overrun);
  }
}

void check_slice(allocators::IAllocator& a, const HevcStreamHeaders& s, const HevcSliceHeader& h) {
  ByteBuffer buf(a);
  buf.put8(0xAA);   // existing content stays
  uint32_t bits = 0;
  CHECK(write_hevc_slice_header(s, h, &buf, &bits));
  CHECK(bits % 8 == 0 && buf.size() == 1 + bits / 8 && buf.view()[0] == 0xAA);
  std::vector<uint8_t> v;
  for (uint64_t i = 1; i < buf.size(); ++i) v.push_back(buf.view()[i]);
  CHECK(v.size() > 6 && v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 1);
  CHECK(v[4] == uint8_t((h.idr ? 19 : 1) << 1) && v[5] == 1);
  Reader r([&]() -> const std::vector<uint8_t>& { static std::vector<uint8_t> body; body.assign(v.begin() + 6, v.end()); return body; }());
  CHECK(r.u(1) == 1);                                   // first_slice_segment_in_pic_flag
  if (h.idr) CHECK(r.u(1) == 0);                        // no_output_of_prior_pics_flag
  CHECK(r.ue() == 0);                                   // slice_pic_parameter_set_id
  const bool p = h.p_slice && !h.idr;
  CHECK(r.ue() == (p ? 1u : 2u));
  if (!h.idr) {
    CHECK(r.u(s.log2_max_poc_lsb) == h.poc_lsb);
    CHECK(r.u(1) == 0);                                 // short_term_ref_pic_set_sps_flag
    CHECK(r.ue() == 1 && r.ue() == 0 && r.ue() == 0 && r.u(1) == 1);
  }
  if (p) CHECK(r.u(1) == 0 && r.ue() == 0);
  CHECK(r.se() == h.qp - 26);
  CHECK(r.u(1) == 1);                                   // slice_loop_filter_across_slices_enabled_flag
  // byte_alignment(): a one then zeros up to the byte boundary, and nothing after.
  CHECK(r.u(1) == 1);
  while (r.pos() & 7) CHECK(r.u(1) == 0);
  CHECK(r.pos() == v.size() * 8 - 48 && !r.overrun);
}

}  // namespace

int main() {
  TestAllocator a;
  const uint32_t sizes[][2] = {{64, 48}, {72, 40}, {1920, 1080}, {16, 16}, {8192, 8192}, {18, 34}, {176, 144}};
  for (const auto& sz : sizes) {
    HevcStreamHeaders s;
    s.width = sz[0];
    s.height = sz[1];
    check_parameter_sets(a, s);
    s.log2_max_poc_lsb = 16; s.level_idc = 93; s.cb_qp_offset = -12; s.cr_qp_offset = 7;
    check_parameter_sets(a, s);
    s.log2_max_poc_lsb = 4; s.cb_qp_offset = 12; s.cr_qp_offset = -3;
    check_parameter_sets(a, s);
    for (int qp : {0, 1, 26, 37, 51}) {
      HevcSliceHeader h;
      h.qp = qp;
      h.idr = true;
      check_slice(a, s, h);
      h.idr = false; h.p_slice = true; h.poc_lsb = 5 % (1u << s.log2_max_poc_lsb);
      check_slice(a, s, h);
      h.poc_lsb = (1u << s.log2_max_poc_lsb) - 1;
      check_slice(a, s, h);
      h.p_slice = false;   // a non-IDR I slice
      check_slice(a, s, h);
    }
  }

  // The CPU decoder accepts the parameter sets (and an empty slice) and reports the cropped size.
  for (const auto& sz : sizes) {
    if (sz[0] > 2000) continue;
    HevcStreamHeaders s;
    s.width = sz[0];
    s.height = sz[1];
    bool ok = false;
    const Stream st = build(a, s, &ok);
    CHECK(ok);
    H265Decoder dec(a);
    dec.decode(ConstByteView(st.bytes.data(), st.bytes.size()), 0);
    ByteBuffer slice(a);
    uint32_t bits = 0;
    HevcSliceHeader h;
    h.idr = true;
    CHECK(write_hevc_slice_header(s, h, &slice, &bits));
    for (int i = 0; i < 8; ++i) slice.put8(0x80);   // garbage slice data: must not crash
    dec.decode(slice.view(), 0);
    VideoFrame f;
    while (dec.receive(&f)) {}
    dec.flush();
    while (dec.receive(&f)) {}
    const VideoFormat fmt = dec.output_format();
    CHECK(fmt.width == s.width && fmt.height == s.height);
  }

  // Invalid parameters fail and leave the buffer alone; appending after existing content works.
  {
    ByteBuffer buf(a);
    buf.put32(0x11223344);
    uint32_t bits = 0;
    HevcStreamHeaders s;
    s.width = 64; s.height = 48;
    HevcStreamHeaders bad[8];
    for (auto& b : bad) b = s;
    bad[0].width = 15; bad[1].height = 8193; bad[2].log2_max_poc_lsb = 3; bad[3].log2_max_poc_lsb = 17; bad[4].cb_qp_offset = 13; bad[5].cr_qp_offset = -13;
    bad[6].width = 63; bad[7].height = 0;
    for (const auto& b : bad) {
      CHECK(!write_hevc_vps(b, &buf, &bits) && !write_hevc_sps(b, &buf, &bits) && !write_hevc_pps(b, &buf, &bits));
      HevcSliceHeader h;
      CHECK(!write_hevc_slice_header(b, h, &buf, &bits));
    }
    HevcSliceHeader h;
    h.qp = 52; CHECK(!write_hevc_slice_header(s, h, &buf, &bits));
    h.qp = -1; CHECK(!write_hevc_slice_header(s, h, &buf, &bits));
    h.qp = 26; h.deblocking = false; CHECK(!write_hevc_slice_header(s, h, &buf, &bits));
    h.deblocking = true; h.idr = false; h.poc_lsb = 1u << s.log2_max_poc_lsb; CHECK(!write_hevc_slice_header(s, h, &buf, &bits));
    CHECK(buf.size() == 4);
    CHECK(write_hevc_sps(s, &buf, &bits) && buf.size() == 4 + bits / 8 && buf.view()[0] == 0x44);
  }
  CHECK(a.live == 0);
  if (g_failed) { printf("IntelHevcHeadersTest: %d failure(s)\n", g_failed); return 1; }
  printf("IntelHevcHeadersTest passed\n");
  return 0;
}
