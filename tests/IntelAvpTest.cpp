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

// Host-side tests for the Gen12 AV1 command encoders (AVP): header words, command lengths and field placement checked against values worked out by hand from the reference
// layouts, the engine's buffer sizes, the global motion parameter conversion, and the default entropy contexts.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/VideoCodec/Intel/IntelAv1Cdf.hpp"
#include "Hardware/VideoCodec/Intel/IntelAvpCommands.hpp"

using namespace hardware;
using namespace hardware::intel_avp;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

template <class F>
static std::vector<uint32_t> emit(F&& f, uint64_t capacity = 512) {
  std::vector<uint32_t> buf(capacity);
  DwordWriter w(ArrayView<uint32_t>(buf.data(), buf.size()));
  f(w);
  CHECK(w.ok());
  buf.resize(w.count());
  return buf;
}

static void test_headers() {
  CHECK(kAvpPipeModeSelect == 0x71800000 && kAvpSurfaceState == 0x71810000 && kAvpPipeBufAddrState == 0x71820000 && kAvpIndObjBaseAddrState == 0x71830000);
  CHECK(kAvpInterPredState == 0x71920000 && kAvpTileCoding == 0x71950000 && kAvpBsdObject == 0x71A00000);
  CHECK(kAvpPicState == 0x71B00000 && kAvpSegmentState == 0x71B20000 && kAvpInloopFilterState == 0x71B30000);
  CHECK(kMfxWait == 0x68000100 && kVdPipelineFlush == 0x77800000);
  CHECK(emit([](DwordWriter& w) { emit_mfx_wait(w); }) == std::vector<uint32_t>({0x68000100}));
  CHECK(emit([](DwordWriter& w) { emit_vd_pipeline_flush(w); }) == std::vector<uint32_t>({0x77800000, (1u << 4) | (1u << 5) | (1u << 20)}));
}

static void test_small_commands() {
  const std::vector<uint32_t> mode = emit([](DwordWriter& w) { emit_avp_pipe_mode_select(w); });
  CHECK(mode.size() == kPipeModeSelectDwords && mode[0] == (kAvpPipeModeSelect | 4) && mode[1] == (2u << 5) && mode[2] == 0 && mode[5] == 0);
  intel_mfx::Nv12TiledLayout layout;
  layout.pitch = 256;
  layout.chroma_offset_rows = 64;
  const std::vector<uint32_t> surface = emit([&](DwordWriter& w) { emit_avp_surface_state(w, layout, kSurfaceLast + 3); });
  CHECK(surface.size() == 5 && surface[0] == (kAvpSurfaceState | 3) && surface[1] == ((10u << 28) | 255) && surface[2] == ((4u << 27) | 64) && surface[3] == 0 && surface[4] == 0);
  const std::vector<uint32_t> ind = emit([](DwordWriter& w) { emit_avp_ind_obj_base_addr_state(w, 0x1'2000'0000ull, 5000, 3); });
  CHECK(ind.size() == 6 && ind[0] == (kAvpIndObjBaseAddrState | 4) && ind[1] == 0x20000000 && ind[2] == 1 && ind[3] == 6 && ind[4] == 0x20002000 && ind[5] == 1);
  const std::vector<uint32_t> bsd = emit([](DwordWriter& w) { emit_avp_bsd_object(w, 1234, 77); });
  CHECK(bsd == std::vector<uint32_t>({kAvpBsdObject | 1, 1234, 77}));
  AvpSegment s;
  s.id = 5;
  s.feature_mask = 0xA1;
  s.delta_q = -3;
  s.skip = true;
  s.lossless = true;
  s.qm_level_y = 7;
  s.qm_level_u = 8;
  s.qm_level_v = 9;
  s.delta_lf_y_vertical = -1;
  s.delta_lf_u = 5;
  s.reference_frame = 6;
  const std::vector<uint32_t> seg = emit([&](DwordWriter& w) { emit_avp_segment_state(w, s); });
  CHECK(seg.size() == 4 && seg[0] == (kAvpSegmentState | 2) && seg[1] == 5);
  CHECK(seg[2] == (0xA1u | (509u << 8) | (1u << 17) | (1u << 19) | (7u << 20) | (8u << 24) | (9u << 28)));
  CHECK(seg[3] == (127u | (5u << 14) | (6u << 28)));
  AvpTile t;
  t.id = 5;
  t.group_num = 5;
  t.column_sb = 9;
  t.row_sb = 4;
  t.width_sb = 3;
  t.height_sb = 2;
  t.last_of_row = true;
  t.last_of_frame = true;
  t.first_of_group = false;
  t.disable_frame_context_update = true;
  t.columns = 4;
  t.rows = 2;
  const std::vector<uint32_t> tile = emit([&](DwordWriter& w) { emit_avp_tile_coding(w, t); });
  CHECK(tile.size() == 6 && tile[0] == (kAvpTileCoding | 4) && tile[1] == (5u | (5u << 12)) && tile[2] == (9u | (4u << 16)) && tile[3] == (2u | (1u << 16)));
  CHECK(tile[4] == ((1u << 26) | (1u << 29) | (1u << 31)) && tile[5] == (1u | (3u << 12) | (1u << 22)));
}

static void test_pic_state() {
  AvpPicture p;
  p.width = 1920;
  p.height = 1080;
  p.sb128 = true;
  p.order_hint_enabled = true;
  p.order_hint_bits = 7;
  p.dual_filter = true;
  p.warped_motion = true;
  p.cdef = true;
  p.frame_type = 1;
  p.primary_ref_frame = 3;
  p.segmentation_enabled = true;
  p.segmentation_last_active = 5;
  p.base_qindex = 100;
  p.y_dc_delta_q = -2;
  p.u_ac_delta_q = 7;
  p.interp_filter = 4;
  p.sign_bias_mask = 0x84;
  p.order_hint = 33;
  p.skip_mode_present = true;
  p.skip_mode_frame[0] = 2;
  p.skip_mode_frame[1] = 5;
  p.ref_frame_side = 0xC0;
  p.global_motion_type[0] = 2;
  p.global_motion_type[6] = 3;
  p.global_motion_invalid[2] = true;
  p.global_motion_words[0] = 0x11111111;
  p.global_motion_words[20] = 0x22222222;
  p.reference_width[3] = 640;
  p.reference_height[3] = 360;
  p.reference_scale_vertical[3] = 5461;
  p.reference_scale_horizontal[3] = 5462;
  p.reference_order_hint[7] = 0x44;
  const std::vector<uint32_t> d = emit([&](DwordWriter& w) { emit_avp_pic_state(w, p); });
  CHECK(d.size() == kPicStateDwords && d[0] == (kAvpPicState | 49));
  CHECK(d[1] == (1079u << 16 | 1919));
  CHECK(d[2] == ((1u << 0) | (1u << 7) | (1u << 9) | (6u << 10) | (1u << 18)));
  CHECK(d[3] == ((1u << 2) | (1u << 4) | (1u << 16) | (3u << 28)));
  CHECK(d[4] == (1u | (5u << 4) | (100u << 16) | (126u << 24)));
  CHECK(d[5] == (7u << 8));
  CHECK(d[6] == ((4u << 2) | (0x84u << 8) | (33u << 16)));
  CHECK(d[7] == ((1u << 4) | (2u << 5) | (5u << 9) | (0xC0u << 24)));
  CHECK(d[8] == ((2u << 3) | (3u << 21) | (1u << 27)));
  CHECK(d[9] == 0x11111111 && d[29] == 0x22222222);
  CHECK(d[30] == 0x76543210u);
  CHECK(d[31 + 3] == (359u << 16 | 639) && d[39 + 3] == (5462u << 16 | 5461));
  CHECK(d[47 + 1] == (0x44u << 24));
}

static void test_global_motion() {
  const int32_t identity[6] = {0, 0, 1 << 16, 0, 0, 1 << 16};
  const int32_t zoom[6] = {16384, -8192, (1 << 16) + 512, 64, -64, (1 << 16) + 512};
  const AvpGlobalMotion none = av1_global_motion(0, identity, true);
  CHECK(none.words[0] == 0 && none.words[1] == 0 && none.words[2] == 0);
  // Rotation and zoom: the 1.0 and a bit of precision go, the translation loses ten bits; the fourth and fifth terms follow the second and third.
  const AvpGlobalMotion rz = av1_global_motion(2, zoom, true);
  CHECK(rz.words[0] == ((16384u >> 10) | (uint32_t(-8192 >> 10) << 16)));
  CHECK(rz.words[1] == (256u | (32u << 16)));
  CHECK(rz.words[2] == ((uint32_t(-32) & 0xFFFF) | (256u << 16)));
  // Translation alone: thirteen bits (fourteen when the vectors are not high precision).
  const AvpGlobalMotion tr = av1_global_motion(1, zoom, false);
  CHECK(tr.words[0] == ((16384u >> 14) | (uint32_t(-8192 >> 14) << 16)));
}

static void test_pipe_buffers() {
  AvpBuffers b;
  b.mocs = 5;
  b.current = 0x1'0000'1000ull;
  b.reference[1] = 0x2000;
  b.reference[7] = 0x3000;
  b.cdf_init = 0x4000;
  b.cdf_backward = 0x5000;
  b.segment_read = 0x6000;
  b.segment_write = 0x7000;
  b.motion_vector_collocated[0] = 0x8000;
  b.motion_vector_collocated[7] = 0x9000;
  b.motion_vector_current = 0xA000;
  b.line[kBsdLine] = 0xB000;
  b.line[kDeblockLineY] = 0xC000;
  b.line[kCdefTopLeft] = 0xD000;
  b.line[kLrTileColumnV] = 0xE000;
  b.line[kBlockStreamout] = 0xF000;
  const std::vector<uint32_t> d = emit([&](DwordWriter& w) { emit_avp_pipe_buf_addr_state(w, b); }, 256);
  CHECK(d.size() == kPipeBufAddrDwords && d[0] == (kAvpPipeBufAddrState | 186));
  CHECK(d[3] == 0x2000 && d[15] == 0x3000 && d[17] == 10);
  CHECK(d[18] == 0x1000 && d[19] == 1 && d[20] == 10);
  CHECK(d[27] == 0x4000 && d[29] == 10 && d[30] == 0x5000 && d[32] == 10 && d[33] == 0x6000 && d[36] == 0x7000);
  CHECK(d[39] == 0x8000 && d[53] == 0x9000 && d[55] == 10 && d[56] == 0xA000 && d[58] == 10);
  CHECK(d[62] == 0xB000 && d[64] == 10 && d[92] == 0xC000 && d[146] == 0xD000 && d[164] == 0xE000 && d[179] == 0xF000 && d[181] == 10);
  CHECK(d[1] == 0 && d[21] == 0 && d[22] == 0 && d[23] == 0 && d[24] == 0 && d[175] == 0);
}

static void test_inter_pred_and_loop_filter() {
  AvpInterPred p;
  for (uint32_t r = 0; r < 7; ++r)
    for (uint32_t j = 0; j < 7; ++j) p.saved_order_hint[r][j] = 10 * r + j;
  p.motion_field_mask = 0x55;
  const std::vector<uint32_t> d = emit([&](DwordWriter& w) { emit_avp_inter_pred_state(w, p); });
  CHECK(d.size() == kInterPredDwords && d[0] == (kAvpInterPredState | 13));
  CHECK(d[1] == (0u | (1u << 8) | (2u << 16) | (3u << 24)) && d[2] == (4u | (5u << 8) | (6u << 16) | (0x55u << 24)));
  CHECK(d[13] == (60u | (61u << 8) | (62u << 16) | (63u << 24)) && d[14] == (64u | (65u << 8) | (66u << 16)));
  AvpInloop l;
  l.level_y_vertical = 20;
  l.level_y_horizontal = 21;
  l.level_u = 22;
  l.level_v = 23;
  l.sharpness = 5;
  l.mode_ref_delta_enabled = true;
  l.ref_delta[0] = 1;
  l.ref_delta[5] = -1;
  l.mode_delta[1] = -2;
  l.cdef_damping_minus3 = 2;
  l.cdef_bits = 3;
  l.cdef_y_strength[4] = 42;
  l.cdef_uv_strength[7] = 21;
  l.upscaled_width = 640;
  l.restoration_type[0] = 3;
  l.restoration_type[2] = 1;
  l.restoration_unit_size_luma = 2;
  l.restoration_same_unit_size_chroma = true;
  const std::vector<uint32_t> f = emit([&](DwordWriter& w) { emit_avp_inloop_filter_state(w, l); });
  CHECK(f.size() == kInloopFilterDwords && f[0] == (kAvpInloopFilterState | 13));
  CHECK(f[1] == (20u | (21u << 6) | (22u << 12) | (23u << 18) | (5u << 24) | (1u << 27)));
  CHECK(f[2] == 1u && f[3] == (127u << 8) && f[4] == (126u << 8));
  CHECK(f[5] == ((3u << 28) | (2u << 30)) && f[6] == 42u && f[7] == 0 && f[8] == (21u << 18));
  CHECK(f[9] == (639u | (8u << 16)) && f[10] == (3u | (1u << 4) | (2u << 8) | (1u << 10)));
}

static void test_sizes() {
  Av1Dimensions dims;
  dims.sb_wide = 30;
  dims.sb_high = 17;
  dims.sb128 = false;
  dims.tile_columns = 2;
  dims.tiles = 4;
  Av1BufferBytes bytes;
  CHECK(av1_buffer_bytes(dims, &bytes));
  CHECK(bytes.segment_id == 2u * 510 * 64 && bytes.motion_vector == 4u * 510 * 64);
  CHECK(bytes.line[kBsdLine] == 64u * 2 * 64 && bytes.line[kBsdTileLine] == 30u * 2 * 64);
  CHECK(bytes.line[kDeblockTileColumnY] == 17u * 8 * 64 && bytes.line[kLrTileLineY] == 2u * 7 * 64 && bytes.line[kCdefTopLeft] == 4u * 64);
  CHECK(bytes.line[kCdefTileColumn] == (17u * 8 + 1) * 64 + 4096 && bytes.line[kLrMetaTileColumn] == (17u + 1) * 64 + 4096);
  CHECK(bytes.line[kFrameStatus] == 64 && bytes.line[kBlockStreamout] == 64);
  dims.sb128 = true;
  CHECK(av1_buffer_bytes(dims, &bytes) && bytes.segment_id == 8u * 510 * 64 && bytes.line[kBsdLine] == 32u * 4 * 64);
  Av1Dimensions bad;
  CHECK(!av1_buffer_bytes(bad, &bytes));
  bad.sb_wide = 2000;
  bad.sb_high = 1;
  CHECK(!av1_buffer_bytes(bad, &bytes));
}

static void test_default_contexts() {
  CHECK(intel_av1::default_context_for(0) == 0 && intel_av1::default_context_for(20) == 0 && intel_av1::default_context_for(21) == 1 && intel_av1::default_context_for(60) == 1 &&
        intel_av1::default_context_for(61) == 2 && intel_av1::default_context_for(120) == 2 && intel_av1::default_context_for(121) == 3 && intel_av1::default_context_for(255) == 3);
  std::vector<uint8_t> a(intel_av1::kCdfContextBytes), b(intel_av1::kCdfContextBytes), shortbuf(100);
  CHECK(intel_av1::build_default_cdf_context(0, ByteView(a.data(), a.size())) && intel_av1::build_default_cdf_context(3, ByteView(b.data(), b.size())));
  CHECK(a != b);
  size_t nonzero = 0;
  for (uint8_t v : a) nonzero += v != 0;
  CHECK(nonzero > 5000);
  CHECK(!intel_av1::build_default_cdf_context(4, ByteView(a.data(), a.size())) && !intel_av1::build_default_cdf_context(0, ByteView(shortbuf.data(), shortbuf.size())));
  // Deterministic.
  std::vector<uint8_t> again(intel_av1::kCdfContextBytes);
  CHECK(intel_av1::build_default_cdf_context(0, ByteView(again.data(), again.size())) && again == a);
}

int main() {
  test_headers();
  test_small_commands();
  test_pic_state();
  test_global_motion();
  test_pipe_buffers();
  test_inter_pred_and_loop_filter();
  test_sizes();
  test_default_contexts();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("IntelAvpTest passed\n");
  return 0;
}
