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

// Host-side tests for the Intel video engine command encoders (Gen8/9 AVC decode): header words and field placement checked against values worked out by
// hand from the reference layouts, command lengths, the order of a whole picture's batch built from a real H.264 stream, rejection of unusable input, and
// the Y-tiled surface layout with a round trip through the detiler.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/VideoCodec/Intel/IntelAvcDecode.hpp"
#include "Hardware/VideoCodec/Intel/IntelTiling.hpp"
#include "Hardware/VideoCodec/Software/H264Encoder.hpp"
#include "Utility/ByteBuffer.hpp"

using namespace hardware;
using namespace hardware::intel_mfx;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// Runs a writer over a vector and returns the dwords.
template <class F>
static std::vector<uint32_t> emit(F&& f, uint64_t capacity = 512) {
  std::vector<uint32_t> buf(capacity);
  DwordWriter w(ArrayView<uint32_t>(buf.data(), buf.size()));
  f(w);
  CHECK(w.ok());
  buf.resize(w.count());
  return buf;
}

static void test_headers_and_fields() {
  CHECK(kMfxPipeModeSelect == 0x70000000 && kMfxSurfaceState == 0x70010000 && kMfxPipeBufAddrState == 0x70020000);
  CHECK(kMfxIndObjBaseAddrState == 0x70030000 && kMfxBspBufBaseAddrState == 0x70040000 && kMfxQmState == 0x70070000);
  CHECK(kMfxAvcImgState == 0x71000000 && kMfxAvcDirectmodeState == 0x71020000 && kMfxAvcSliceState == 0x71030000);
  CHECK(kMfxAvcRefIdxState == 0x71040000 && kMfxAvcWeightoffsetState == 0x71050000 && kMfdAvcPicidState == 0x71250000 && kMfdAvcBsdObject == 0x71280000);

  auto d = emit([](DwordWriter& w) { emit_pipe_mode_select(w, true, false); });
  CHECK(d.size() == 5 && d[0] == 0x70000003 && d[1] == 0x00020202 && d[2] == 0 && d[3] == 0 && d[4] == 0);
  d = emit([](DwordWriter& w) { emit_pipe_mode_select(w, false, true); });
  CHECK(d[1] == 0x00020102);

  AvcPicture p;
  p.width_in_mbs = 4; p.height_in_mbs = 3;
  p.chroma_qp_index_offset = 2; p.second_chroma_qp_index_offset = -1;
  p.weighted_pred = true; p.weighted_bipred_idc = 1;
  p.entropy_coding_cabac = true; p.direct_8x8_inference = true; p.transform_8x8_mode = true;
  d = emit([&](DwordWriter& w) { emit_avc_img_state(w, p); });
  CHECK(d.size() == 17 && d[0] == 0x7100000F && d[1] == 11 && d[2] == 0x00020003 && d[3] == 0x1F021400 && d[4] == 0x0000049C);
  for (size_t i = 5; i < 17; ++i) CHECK(d[i] == 0);
  p.reference_picture = false; p.constrained_intra_pred = true;
  d = emit([&](DwordWriter& w) { emit_avc_img_state(w, p); });
  CHECK(d[4] == (0x49C | 0x40 | 0x20));

  AvcSlice s;
  s.slice_type = 1; s.first_mb = 5; s.next_first_mb = 9; s.last_slice = false;
  s.num_ref_idx_l0 = 2; s.num_ref_idx_l1 = 1; s.luma_log2_weight_denom = 3; s.chroma_log2_weight_denom = 2;
  s.direct_spatial_mv_pred = true; s.disable_deblocking_filter_idc = 2; s.cabac_init_idc = 1;
  s.slice_qp = 30; s.slice_alpha_c0_offset_div2 = -3; s.slice_beta_offset_div2 = 2;
  d = emit([&](DwordWriter& w) { emit_avc_slice_state(w, p, s); });
  CHECK(d.size() == 11 && d[0] == 0x71030009 && d[1] == 1 && d[2] == 0x01020203 && d[3] == 0x311E020D && d[4] == 0x01010005 && d[5] == 0x00020001 && d[6] == 0);
  s.last_slice = true;
  d = emit([&](DwordWriter& w) { emit_avc_slice_state(w, p, s); });
  CHECK(d[5] == 0x00030000 && d[6] == (1u << 19));  // the last slice ends at row 3 of the picture

  s.data_size = 100; s.data_offset = 8; s.first_mb_bit_offset = 77;
  d = emit([&](DwordWriter& w) { emit_avc_bsd_object(w, s); });
  CHECK(d.size() == 6 && d[0] == 0x71280004 && d[1] == 100 && d[2] == 8 && d[3] == 0 && d[4] == 0x0009008D && d[5] == 0);

  d = emit([&](DwordWriter& w) { emit_avc_phantom_slice(w, p, true, 9); });
  CHECK(d.size() == 17 && d[0] == 0x71030009 && d[5] == 0x00020001 && d[11] == 0x71280004);
  d = emit([&](DwordWriter& w) { emit_avc_phantom_slice(w, p, false, 0); });
  CHECK(d[4] == ((3u << 24) | 12u) && d[5] == 0);

  CHECK(avc_ref_idx_entry(3, false) == 0x26 && avc_ref_idx_entry(3, true) == 0x66 && avc_ref_idx_entry(0, false) == 0x20);

  // Sizes of the fixed-length commands.
  AvcBuffers b;
  b.current_picture = 0x100000; b.bitstream = 0x200000;
  CHECK(emit([&](DwordWriter& w) { emit_pipe_buf_addr_state(w, b); }).size() == 61);
  CHECK(emit([&](DwordWriter& w) { emit_ind_obj_base_addr_state(w, 0x200000, 2); }).size() == 26);
  CHECK(emit([&](DwordWriter& w) { emit_bsp_buf_base_addr_state(w, b); }).size() == 10);
  CHECK(emit([&](DwordWriter& w) { emit_avc_directmode_state(w, b); }).size() == 71);
  const uint8_t entries[32] = {0x20, 0x22};
  CHECK(emit([&](DwordWriter& w) { emit_avc_ref_idx_state(w, 1, entries); }).size() == 10);
  int16_t table[192] = {};
  CHECK(emit([&](DwordWriter& w) { emit_avc_weightoffset_state(w, 0, table); }).size() == 98);
  CHECK(emit([&](DwordWriter& w) { emit_avc_picid_state(w); }).size() == 10);
  const uint8_t qm[48] = {1, 2, 3};
  d = emit([&](DwordWriter& w) { emit_qm_state(w, kQmAvc4x4Inter, ConstByteView(qm, 48)); });
  CHECK(d.size() == 18 && d[0] == 0x70070010 && d[1] == 1 && d[2] == 0x00030201 && d[13] == 0 && d[17] == 0);

  // The pipe buffer command puts the picture in the pre- or post-deblocking slot, and a 64-bit address as low then high.
  b.current_picture = 0x1'2345'6000ull; b.deblocking_enabled = true; b.mocs = 2;
  d = emit([&](DwordWriter& w) { emit_pipe_buf_addr_state(w, b); });
  CHECK(d[1] == 0 && d[2] == 0 && d[3] == 2 && d[4] == 0x23456000 && d[5] == 1 && d[6] == 2);
  b.deblocking_enabled = false;
  d = emit([&](DwordWriter& w) { emit_pipe_buf_addr_state(w, b); });
  CHECK(d[1] == 0x23456000 && d[2] == 1 && d[4] == 0 && d[5] == 0);

  // The end marker: an odd count gets it directly, an even count a no-op first, so the batch is a whole number of quadwords.
  d = emit([](DwordWriter& w) { w.emit(7); emit_batch_end(w); });
  CHECK(d.size() == 2 && d[1] == kMiBatchBufferEnd);
  d = emit([](DwordWriter& w) { w.emit(7); w.emit(7); emit_batch_end(w); });
  CHECK(d.size() == 4 && d[2] == kMiNoop && d[3] == kMiBatchBufferEnd);
  d = emit([](DwordWriter& w) { emit_mi_flush(w); });
  CHECK(d.size() == 4 && d[0] == 0x13000082 && d[1] == 0 && d[2] == 0 && d[3] == 0);

  // Overflow is remembered, not fatal.
  std::vector<uint32_t> tiny(3);
  DwordWriter w(ArrayView<uint32_t>(tiny.data(), tiny.size()));
  emit_pipe_mode_select(w, true, false);
  CHECK(!w.ok() && w.count() == 3);
}

static void test_layout_and_tiling() {
  Nv12TiledLayout l;
  CHECK(nv12_tiled_layout(1920, 1080, &l) && l.pitch == 1920 && l.aligned_height == 1088 && l.chroma_offset_rows == 1088 && l.size == 1920ull * (1088 + 544));
  CHECK(nv12_tiled_layout(64, 48, &l) && l.pitch == 128 && l.aligned_height == 64 && l.size == 128ull * (64 + 32) && l.size % 4096 == 0);
  CHECK(nv12_tiled_layout(130, 17, &l) && l.pitch == 256 && l.aligned_height == 32 && l.size == 256ull * (32 + 32));
  CHECK(!nv12_tiled_layout(0, 16, &l) && !nv12_tiled_layout(16, 0, &l) && !nv12_tiled_layout(16385, 16, &l) && !nv12_tiled_layout(16, 16385, &l));

  CHECK(y_tile_offset(256, 0, 0) == 0 && y_tile_offset(256, 16, 0) == 512 && y_tile_offset(256, 0, 1) == 16);
  CHECK(y_tile_offset(256, 127, 31) == 4095 && y_tile_offset(256, 128, 0) == 4096 && y_tile_offset(256, 0, 32) == 8192);
  // A bijection onto the surface: every byte has its own place.
  std::vector<uint8_t> seen(256 * 64, 0);
  for (uint32_t row = 0; row < 64; ++row)
    for (uint32_t x = 0; x < 256; ++x) {
      const uint64_t o = y_tile_offset(256, x, row);
      CHECK(o < seen.size() && !seen[o]);
      if (o < seen.size()) seen[o] = 1;
    }

  // Round trip: tile a picture the way the engine would (here: our own writer), detile, compare.
  TestAllocator a;
  {
    const uint32_t cw = 50, ch = 34;
    CHECK(nv12_tiled_layout(64, 48, &l));
    std::vector<uint8_t> tiled(l.size, 0xEE);
    auto luma = [](uint32_t x, uint32_t y) { return static_cast<uint8_t>(x * 3 + y * 7); };
    auto cb = [](uint32_t x, uint32_t y) { return static_cast<uint8_t>(100 + x + 2 * y); };
    auto cr = [](uint32_t x, uint32_t y) { return static_cast<uint8_t>(200 - x - y); };
    for (uint32_t y = 0; y < 48; ++y)
      for (uint32_t x = 0; x < 64; ++x) tiled[y_tile_offset(l.pitch, x, y)] = luma(x, y);
    for (uint32_t y = 0; y < 24; ++y)
      for (uint32_t x = 0; x < 32; ++x) {
        tiled[y_tile_offset(l.pitch, 2 * x, l.chroma_offset_rows + y)] = cb(x, y);
        tiled[y_tile_offset(l.pitch, 2 * x + 1, l.chroma_offset_rows + y)] = cr(x, y);
      }
    VideoFrameBuffer out(a);
    CHECK(out.allocate({cw, ch, PixelFormat::I420}));
    CHECK(detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size()), l, cw, ch, out));
    bool same = true;
    for (uint32_t y = 0; y < ch; ++y)
      for (uint32_t x = 0; x < cw; ++x) same &= out.plane(0)[uint64_t{y} * cw + x] == luma(x, y);
    for (uint32_t y = 0; y < (ch + 1) / 2; ++y)
      for (uint32_t x = 0; x < (cw + 1) / 2; ++x) {
        same &= out.plane(1)[uint64_t{y} * ((cw + 1) / 2) + x] == cb(x, y);
        same &= out.plane(2)[uint64_t{y} * ((cw + 1) / 2) + x] == cr(x, y);
      }
    CHECK(same);
    // A cropped picture, starting inside the surface (the offsets are even, as the chroma planes are half size).
    {
      VideoFrameBuffer crop(a);
      CHECK(crop.allocate({40, 30, PixelFormat::I420}));
      CHECK(detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size()), l, 40, 30, crop, 6, 4));
      bool same_crop = true;
      for (uint32_t y = 0; y < 30; ++y)
        for (uint32_t x = 0; x < 40; ++x) same_crop &= crop.plane(0)[uint64_t{y} * 40 + x] == luma(x + 6, y + 4);
      for (uint32_t y = 0; y < 15; ++y)
        for (uint32_t x = 0; x < 20; ++x) {
          same_crop &= crop.plane(1)[uint64_t{y} * 20 + x] == cb(x + 3, y + 2);
          same_crop &= crop.plane(2)[uint64_t{y} * 20 + x] == cr(x + 3, y + 2);
        }
      CHECK(same_crop);
      CHECK(!detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size()), l, 40, 30, crop, 5, 4));    // odd origin
      CHECK(!detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size()), l, 40, 30, crop, 26, 4));   // runs past the surface
      CHECK(!detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size()), l, 40, 30, crop, 0, 36));
      CHECK(!detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size()), l, 40, 30, crop, 0xFFFFFFFEu, 0));
    }
    // Refusals: surface too small, wrong output format or size.
    CHECK(!detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size() - 1), l, cw, ch, out));
    CHECK(!detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size()), l, cw + 1, ch, out));
    CHECK(!detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size()), l, 65, ch, out));
    VideoFrameBuffer wrong(a);
    CHECK(wrong.allocate({cw, ch, PixelFormat::Nv12}));
    CHECK(!detile_nv12_to_i420(ConstByteView(tiled.data(), tiled.size()), l, cw, ch, wrong));
  }
  CHECK(a.live == 0);
}

// A walker over a command stream: names each command by its header and checks that the length field adds up.
static bool walk(const std::vector<uint32_t>& d, std::vector<uint32_t>* names) {
  size_t i = 0;
  while (i < d.size()) {
    const uint32_t h = d[i];
    size_t len;
    uint32_t name = h;
    if (h == kMiNoop) len = 1;
    else if ((h >> 23) == 0x0A) len = 1;
    else {
      len = ((h >> 29) == 0 ? (h & 0x3F) : (h & 0xFF)) + 2;  // the length field of an MI command is 6 bits; bit 7 is a flag
      name = h & 0xFFFF0000;
      if ((h >> 29) == 0) name = h & 0xFFFFFF00;  // MI commands keep their flags out of the name
    }
    if (i + len > d.size()) return false;
    names->push_back(name);
    i += len;
  }
  return true;
}

static void test_picture_batches() {
  TestAllocator a;
  {
  H264Encoder enc(a);
  CHECK(enc.configure(64, 48, 25, 1, 85, 3));
  ByteBuffer stream(a);
  VideoFrameBuffer picture(a);
  std::vector<uint64_t> ends;
  for (int t = 0; t < 2; ++t) {
    CHECK(picture.allocate({64, 48, PixelFormat::I420}));
    for (uint32_t i = 0; i < 64 * 48; ++i) picture.plane(0)[i] = static_cast<uint8_t>(i * 5 + t * 9);
    for (uint32_t i = 0; i < 32 * 24; ++i) { picture.plane(1)[i] = 120; picture.plane(2)[i] = static_cast<uint8_t>(130 + t); }
    bool key;
    CHECK(enc.encode(picture.frame(), false, &stream, &key));
    ends.push_back(stream.size());
  }
  H264StreamParser parser(a);
  List<H264Slice> slices0(a), slices1(a);
  CHECK(parser.parse(stream.view().first(ends[0]), &slices0));
  const ConstByteView au0 = stream.view().first(ends[0]);
  const ConstByteView au1 = stream.view().subview(ends[0], ends[1] - ends[0]);
  CHECK(parser.parse(au1, &slices1));
  CHECK(slices0.count() == 1 && slices1.count() == 1);

  AvcPictureJob job;
  job.sets = parser.parameter_sets();
  job.sps = parser.sps_for(slices0[0].header);
  job.pps = parser.pps_for(slices0[0].header);
  job.slices = ArrayView<const H264Slice>(&slices0[0], 1);
  job.access_unit = au0;
  job.bitstream_address = 0x40000;
  job.buffers.current_picture = 0x100000; job.buffers.intra_row_store = 0x200000; job.buffers.deblocking_row_store = 0x201000;
  job.buffers.bsd_mpc_row_store = 0x202000; job.buffers.mpr_row_store = 0x203000; job.buffers.direct_mv_current = 0x300000;

  std::vector<uint32_t> out(2048);
  uint64_t n = 0;
  CHECK(build_avc_decode_batch(job, ArrayView<uint32_t>(out.data(), out.size()), &n) == AvcBatchError::None);
  const std::vector<uint32_t> idr(out.begin(), out.begin() + n);
  std::vector<uint32_t> names;
  CHECK(walk(idr, &names));
  std::vector<uint32_t> order;
  for (uint32_t name : names) if (name != kMiNoop) order.push_back(name);  // a no-op may pad the batch end
  const std::vector<uint32_t> expect_idr = {0x13000000, kMfxPipeModeSelect, kMfxSurfaceState, kMfxPipeBufAddrState, kMfxBspBufBaseAddrState,
                                            kMfxQmState, kMfxQmState, kMfdAvcPicidState, kMfxAvcImgState, kMfxIndObjBaseAddrState,
                                            kMfxAvcDirectmodeState, kMfxAvcSliceState, kMfdAvcBsdObject, kMiBatchBufferEnd};
  if (order != expect_idr) { for (uint32_t v : order) printf(" %08x", v); printf("\n"); }
  CHECK(order == expect_idr);
  CHECK(n % 2 == 0 && idr.back() == kMiBatchBufferEnd);
  CHECK(idr[0] == 0x13000082 && idr[4] == 0x70000003);  // the pipe mode select follows the 4-dword flush
  // The picture state carries the stream's size and the slice object points at the slice.
  size_t at = 0;
  for (size_t i = 0; i + 1 < idr.size(); ++i) if (idr[i] == 0x7100000F) at = i;
  CHECK(at && idr[at + 1] == 4 * 3 - 1 && idr[at + 2] == ((2u << 16) | 3u));
  for (size_t i = 0; i + 5 < idr.size(); ++i)
    if (idr[i] == 0x71280004) {
      CHECK(idr[i + 1] == slices0[0].nal_size && idr[i + 2] == slices0[0].nal_offset && (idr[i + 4] >> 16) == (slices0[0].header_bits >> 3) &&
            (idr[i + 4] & 7) == (slices0[0].header_bits & 7) && (idr[i + 4] & (1u << 3)));
    }

  // The P picture needs its reference list: frame store 0 holds the IDR picture.
  AvcPictureJob pjob = job;
  pjob.slices = ArrayView<const H264Slice>(&slices1[0], 1);
  pjob.access_unit = au1;
  pjob.sps = parser.sps_for(slices1[0].header);
  pjob.pps = parser.pps_for(slices1[0].header);
  pjob.frames[0].used = true; pjob.frames[0].picture = 0x500000; pjob.frames[0].direct_mv = 0x600000; pjob.frames[0].poc[0] = 0; pjob.frames[0].poc[1] = 0;
  static const uint8_t list0[1][kMaxAvcRefIdx] = {{0}};
  pjob.ref_list0 = list0;
  pjob.current_poc[0] = pjob.current_poc[1] = 2;
  const AvcBatchError perr = build_avc_decode_batch(pjob, ArrayView<uint32_t>(out.data(), out.size()), &n);
  if (perr != AvcBatchError::None) printf("P batch error %d (slice type %d)\n", (int)perr, slices1[0].header.type);
  CHECK(perr == AvcBatchError::None);
  std::vector<uint32_t> p(out.begin(), out.begin() + n), pnames;
  CHECK(walk(p, &pnames));
  bool saw_ref = false;
  for (size_t i = 0; i + 2 < p.size(); ++i)
    if (p[i] == 0x71040008) {  // MFX_AVC_REF_IDX_STATE, list 0, first entry: frame store 0, frame, short term
      saw_ref = true;
      CHECK(p[i + 1] == 0 && (p[i + 2] & 0xFF) == 0x20 && (p[i + 2] >> 8) == 0xFFFFFF);
    }
  CHECK(saw_ref);

  // Unusable input is refused, never half-built.
  AvcPictureJob bad = pjob;
  bad.frames[0].used = false;
  CHECK(build_avc_decode_batch(bad, ArrayView<uint32_t>(out.data(), out.size()), &n) == AvcBatchError::BadInput);
  bad = pjob; bad.ref_list0 = nullptr;
  CHECK(build_avc_decode_batch(bad, ArrayView<uint32_t>(out.data(), out.size()), &n) == AvcBatchError::BadInput);
  bad = pjob; bad.frames[0].direct_mv = 0;
  CHECK(build_avc_decode_batch(bad, ArrayView<uint32_t>(out.data(), out.size()), &n) == AvcBatchError::BadInput);
  bad = pjob; bad.bitstream_address = 0;
  CHECK(build_avc_decode_batch(bad, ArrayView<uint32_t>(out.data(), out.size()), &n) == AvcBatchError::BadInput);
  bad = pjob; bad.access_unit = au1.first(4);
  CHECK(build_avc_decode_batch(bad, ArrayView<uint32_t>(out.data(), out.size()), &n) == AvcBatchError::BadInput);
  bad = pjob; bad.slices = ArrayView<const H264Slice>();
  CHECK(build_avc_decode_batch(bad, ArrayView<uint32_t>(out.data(), out.size()), &n) == AvcBatchError::BadInput);
  static const uint8_t bad_list[1][kMaxAvcRefIdx] = {{5}};
  bad = pjob; bad.ref_list0 = bad_list;
  CHECK(build_avc_decode_batch(bad, ArrayView<uint32_t>(out.data(), out.size()), &n) == AvcBatchError::BadInput);

  // Any output buffer that is too small gives NoRoom (and never writes past its end).
  CHECK(build_avc_decode_batch(pjob, ArrayView<uint32_t>(out.data(), out.size()), &n) == AvcBatchError::None);
  const uint64_t needed = n;
  for (uint64_t cap = 0; cap < needed; ++cap) {
    std::vector<uint32_t> small(cap + 1, 0xDEADBEEF);
    uint64_t m = 0;
    CHECK(build_avc_decode_batch(pjob, ArrayView<uint32_t>(small.data(), cap), &m) == AvcBatchError::NoRoom);
    CHECK(small[cap] == 0xDEADBEEF);
  }
  }
  CHECK(a.live == 0);
}

// Damaged streams may parse into odd slices; the builder must answer with a batch or an error, never crash.
static void test_batch_robustness() {
  TestAllocator a;
  {
  H264Encoder enc(a);
  CHECK(enc.configure(48, 32, 25, 1, 80, 1));
  ByteBuffer stream(a);
  VideoFrameBuffer picture(a);
  CHECK(picture.allocate({48, 32, PixelFormat::I420}));
  for (uint32_t i = 0; i < 48 * 32; ++i) picture.plane(0)[i] = static_cast<uint8_t>(i * 3);
  for (uint32_t i = 0; i < 24 * 16; ++i) { picture.plane(1)[i] = 128; picture.plane(2)[i] = 128; }
  bool key;
  CHECK(enc.encode(picture.frame(), false, &stream, &key));
  const uint64_t size = stream.size();
  std::vector<uint32_t> out(4096);
  for (uint64_t byte = 0; byte < size; ++byte)
    for (int bit = 0; bit < 8; bit += 2) {
      ByteBuffer copy(a);
      CHECK(copy.append(stream.view()));
      copy.writable_view()[byte] ^= static_cast<uint8_t>(1u << bit);
      H264StreamParser parser(a);
      List<H264Slice> slices(a);
      if (!parser.parse(copy.view(), &slices) || slices.count() == 0) continue;
      AvcPictureJob job;
      job.sets = parser.parameter_sets();
      job.sps = parser.sps_for(slices[0].header);
      job.pps = parser.pps_for(slices[0].header);
      job.slices = ArrayView<const H264Slice>(&slices[0], slices.count());
      job.access_unit = copy.view();
      job.bitstream_address = 0x1000;
      job.buffers.current_picture = 0x2000;
      uint64_t n = 0;
      build_avc_decode_batch(job, ArrayView<uint32_t>(out.data(), out.size()), &n);
      CHECK(n <= out.size());
    }
  }
  CHECK(a.live == 0);
}

int main() {
  test_headers_and_fields();
  test_layout_and_tiling();
  test_picture_batches();
  test_batch_robustness();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("intel mfx tests passed\n");
  return 0;
}
