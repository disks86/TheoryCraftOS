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


// Host-side tests for the VCE 3.4 task commands (AmdVce3Messages): each block's size, id and fields at the dwords they must be at (counted by hand from Mesa's radeon_vce_52.c,
// independently of the code under test), the session setup made from the parameters, the buffer layout arithmetic, the offsets between encode tasks, the feedback, and the
// writer's bounds. The field order of the blocks is also checked against Mesa's sources by scripts/check_vce3_messages.py.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdVce3Messages.hpp"

using namespace hardware;
using namespace hardware::amd_vce3;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

using Dwords = std::vector<uint32_t>;
static ArrayView<uint32_t> view(Dwords& d) { return ArrayView<uint32_t>(d.data(), d.size()); }

static EncoderParameters params64() {
  EncoderParameters p;
  p.width = 64;
  p.height = 64;
  p.profile_idc = 66;
  p.level = 40;
  p.max_references = 1;
  p.rate_control = RateControlMethod::Constant;
  p.target_bitrate = 500000;
  p.peak_bitrate = 600000;
  p.frame_rate_num = 30;
  p.frame_rate_den = 1;
  p.gop_size = 12;
  p.quant_i_frames = 24;
  p.quant_p_frames = 26;
  p.quant_b_frames = 28;
  p.vbv_buffer_size = 100000;
  return p;
}

static void test_writer() {
  Dwords buf(16, 0xAAAAAAAA);
  TaskWriter w(view(buf));
  w.begin(0x11);
  w.put(7);
  w.put_address(0x12'3456'7890ull);
  w.end();
  CHECK(w.size() == 5 && !w.overflow() && buf[0] == 20 && buf[1] == 0x11 && buf[2] == 7 && buf[3] == 0x12 && buf[4] == 0x34567890 && buf[5] == 0xAAAAAAAA);   // (high dword first)
  // Too small: nothing past the end is written, overflow is reported.
  Dwords small(3, 0xAAAAAAAA);
  TaskWriter s(view(small));
  s.begin(1);
  s.put(2);
  s.put(3);
  s.end();
  CHECK(s.overflow() && small.size() == 3);
  // A block begun inside another, an end with none open, and patching outside what was written, are all errors.
  Dwords more(16);
  TaskWriter n(view(more));
  n.begin(1);
  n.begin(2);
  CHECK(n.overflow());
  TaskWriter e(view(more));
  e.end();
  CHECK(e.overflow());
  TaskWriter pw(view(more));
  pw.patch(5, 1);
  CHECK(pw.overflow());
}

static void test_small_blocks() {
  Dwords buf(64, 0);
  {
    TaskWriter w(view(buf));
    write_session(w, 0x77);
    CHECK(w.size() == 3 && buf[0] == 12 && buf[1] == 0x00000001 && buf[2] == 0x77);
  }
  {
    TaskWriter w(view(buf));
    write_task_info(w, TaskOperation::Config, 0, 0xffffffff, 0);
    // size 8 dwords = 32 bytes, id 2, then offset_of_next_task_info, task_operation, reference_picture_dependency, collocate_flag_dependency, feedback_index, video_bitstream_ring_index
    const uint32_t expect[] = {32, 0x2, 0xffffffff, 2, 0, 0, 0xffffffff, 0};
    CHECK(w.size() == 8);
    for (int i = 0; i < 8; ++i) CHECK(buf[i] == expect[i]);
  }
  {
    TaskWriter w(view(buf));
    write_feedback(w, 0xAB'CDEF'0000ull, 1);
    const uint32_t expect[] = {20, 0x05000005, 0xAB, 0xCDEF0000, 1};
    CHECK(w.size() == 5);
    for (int i = 0; i < 5; ++i) CHECK(buf[i] == expect[i]);
  }
  {
    TaskWriter w(view(buf));
    write_destroy(w, 0x1000, 1);
    // task info (destroy), feedback, the destroy block of two dwords
    CHECK(w.size() == 8 + 5 + 2 && buf[3] == 1 && buf[8] == 20 && buf[8 + 1] == 0x05000005 && buf[13] == 8 && buf[14] == 0x02000001);
  }
}

static void test_setup() {
  const EncoderSetup s = make_setup(params64());
  CHECK(s.rc.rc_method == 3 && s.rc.target_bitrate == 500000 && s.rc.peak_bitrate == 600000 && s.rc.frame_rate_num == 30 && s.rc.frame_rate_den == 1 && s.rc.gop_size == 12);
  CHECK(s.rc.quant_i_frames == 24 && s.rc.quant_p_frames == 26 && s.rc.quant_b_frames == 28 && s.rc.max_qp == 51 && s.rc.min_qp == 0 && s.rc.vbv_buffer_size == 100000 && s.rc.fill_data_enable == 0);
  CHECK(s.me.motion_est_quarter_pixel == 1 && s.me.enc_disable_sub_mode == 254 && s.me.lsmvert == 2 && s.me.enc_ime2_search_range_x == 4 && s.me.enc_ime2_search_range_y == 4);
  CHECK(s.me.enc_ime_decimation_search == 1 && s.me.motion_est_half_pixel == 1 && s.me.enc_search_range_x == 16 && s.me.enc_search_range_y == 16 && s.me.enc_search1_range_x == 16 && s.me.enc_search1_range_y == 16);
  CHECK(s.pc.enc_crop_right_offset == 0 && s.pc.enc_crop_bottom_offset == 0 && s.pc.enc_num_mbs_per_slice == 16 && s.pc.enc_b_pic_pattern == 0 && s.pc.enc_number_of_reference_frames == 1);
  CHECK(s.pc.enc_max_num_ref_frames == 2 && s.pc.enc_num_default_active_ref_l0 == 1 && s.pc.enc_num_default_active_ref_l1 == 1 && s.pc.enc_cabac_enable == 0 && s.pc.enc_constraint_set_flags == 0x40);
  CHECK(s.feedback_ring_size == 1 && s.config_extension_perf_logging == 3 && s.addrmode_arraymode_disrdo_distwoinstants == 0x01000201);
  // A size that is not a whole macroblock is cropped in the padding, in units of two samples; more references mean a longer B pattern and more reference frames.
  EncoderParameters odd = params64();
  odd.width = 66;
  odd.height = 50;
  odd.max_references = 3;
  odd.cabac = true;
  const EncoderSetup o = make_setup(odd);
  CHECK(o.pc.enc_crop_right_offset == (80 - 66) / 2 && o.pc.enc_crop_bottom_offset == (64 - 50) / 2 && o.pc.enc_num_mbs_per_slice == 5 * 4);
  CHECK(o.pc.enc_b_pic_pattern == 2 && o.pc.enc_number_of_reference_frames == 1 && o.pc.enc_max_num_ref_frames == 4 && o.pc.enc_cabac_enable == 1);
  CHECK(parameters_ok(params64()));
  EncoderParameters bad = params64();
  bad.width = 8;
  CHECK(!parameters_ok(bad));
  bad = params64(); bad.width = 4098; CHECK(!parameters_ok(bad));
  bad = params64(); bad.height = 63; CHECK(!parameters_ok(bad));
  bad = params64(); bad.profile_idc = 99; CHECK(!parameters_ok(bad));
  bad = params64(); bad.level = 9; CHECK(!parameters_ok(bad));
  bad = params64(); bad.level = 53; CHECK(!parameters_ok(bad));
  bad = params64(); bad.max_references = 17; CHECK(!parameters_ok(bad));
  bad = params64(); bad.frame_rate_den = 0; CHECK(!parameters_ok(bad));
  bad = params64(); bad.gop_size = 0; CHECK(!parameters_ok(bad));
  bad = params64(); bad.quant_p_frames = 52; CHECK(!parameters_ok(bad));
  bad = params64(); bad.rate_control = static_cast<RateControlMethod>(6); CHECK(!parameters_ok(bad));
}

static void test_layout() {
  const PictureLayout l = picture_layout(64, 64);
  CHECK(l.pitch == 256 && l.vpitch == 64 && l.frame_bytes == 256 * 96);
  const PictureLayout odd = picture_layout(1920, 1080);
  CHECK(odd.pitch == 2048 && odd.vpitch == 1088 && odd.frame_bytes == 2048u * 1632);
  uint32_t luma = 0, chroma = 0;
  frame_offset(l, 0, &luma, &chroma);
  CHECK(luma == 0 && chroma == 256 * 64);
  frame_offset(l, 3, &luma, &chroma);
  CHECK(luma == 3 * 24576 && chroma == 3 * 24576 + 16384);
  // The number of pictures the level allows (macroblocks of the level over the picture's, at most 16).
  CHECK(cpb_slot_count(64, 64, 40) == 16 && cpb_slot_count(1920, 1080, 40) == 4 && cpb_slot_count(1920, 1080, 41) == 4 && cpb_slot_count(1920, 1080, 42) == 4 && cpb_slot_count(1280, 720, 31) == 5);
  CHECK(cpb_slot_count(64, 64, 10) == 16 && cpb_slot_count(176, 144, 10) == 4 && cpb_slot_count(1920, 1080, 52) == 16 && cpb_slot_count(1920, 1080, 51) == 16);
  CHECK(cpb_slot_count(4096, 2304, 52) == 184320 / (256 * 144));
  // Rows aligned to 32 and a half more for the chroma, once for each slot.
  CHECK(cpb_bytes(64, 64, 40) == uint64_t{256} * 64 * 3 / 2 * 16);
  CHECK(cpb_bytes(1920, 1080, 40) == uint64_t{2048} * 1088 * 3 / 2 * 4);
}

static void test_create_and_config() {
  const EncoderSetup s = make_setup(params64());
  const PictureLayout l = picture_layout(64, 64);
  Dwords buf(256, 0);
  {
    TaskWriter w(view(buf));
    write_create(w, s, l);
    // task info (create), then the create block: size, id, circular buffer, profile, level, pic struct restriction, width, height, luma pitch, chroma pitch, height in quad words,
    // the address mode word, the four pre-encode fields.
    CHECK(w.size() == 8 + 16 && !w.overflow() && buf[0] == 32 && buf[3] == 0);
    const uint32_t expect[] = {64, 0x01000001, 0, 66, 40, 0, 64, 64, 256, 256, 8, 0x01000201, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) CHECK(buf[8 + i] == expect[i]);
  }
  {
    TaskWriter w(view(buf));
    write_config(w, s);
    // task info (config, feedback index all ones) 8, rate control 28, config extension 3, motion estimation 26, rdo 19, pic control 29
    CHECK(w.size() == 8 + 28 + 3 + 26 + 19 + 29 && !w.overflow());
    CHECK(buf[0] == 32 && buf[3] == 2 && buf[6] == 0xffffffff);
    uint32_t at = 8;
    CHECK(buf[at] == 112 && buf[at + 1] == 0x04000005 && buf[at + 2] == 3 && buf[at + 3] == 500000 && buf[at + 4] == 600000 && buf[at + 5] == 30 && buf[at + 6] == 12);   // method, target, peak, rate, gop
    CHECK(buf[at + 7] == 24 && buf[at + 8] == 26 && buf[at + 9] == 28 && buf[at + 10] == 100000 && buf[at + 11] == 1);                                               // quants, vbv size, rate denominator
    CHECK(buf[at + 2 + 17] == 51);   // max_qp is the 18th field
    at += 28;
    CHECK(buf[at] == 12 && buf[at + 1] == 0x04000001 && buf[at + 2] == 3);
    at += 3;
    CHECK(buf[at] == 104 && buf[at + 1] == 0x04000007 && buf[at + 2] == 1 && buf[at + 3] == 1 && buf[at + 4] == 1 && buf[at + 7] == 2 && buf[at + 2 + 13] == 254);   // decimation, half pel, quarter pel; lsmvert; sub modes
    at += 26;
    CHECK(buf[at] == 76 && buf[at + 1] == 0x04000008);
    for (int i = 2; i < 19; ++i) CHECK(buf[at + i] == 0);
    at += 19;
    CHECK(buf[at] == 116 && buf[at + 1] == 0x04000002 && buf[at + 2 + 10] == 16 && buf[at + 2 + 18] == 0x40 && buf[at + 2 + 22] == 2 && buf[at + 2 + 21] == 1);   // mbs per slice, constraint flags, max refs, refs
    CHECK(at + 29 == 8 + 28 + 3 + 26 + 19 + 29);
  }
}

// The position, counted from the block's size dword, of the n-th (1-based) payload dword.
static uint32_t pos(uint32_t n) { return n + 1; }

static EncodeTask idr_task() {
  EncodeTask t;
  t.picture_type = PictureType::IDR;
  t.frame_num = 0;
  t.frame_num_cnt = 1;
  t.pic_order_cnt = 0;
  t.i_remain = 11;
  t.p_remain = 0;
  t.current_luma_offset = 0;
  t.current_chroma_offset = 256 * 64;
  t.input_luma_gpu_address = 0x4'0000'1000ull;
  t.input_chroma_gpu_address = 0x4'0000'5000ull;
  t.cpb_gpu_address = 0x4'1000'0000ull;
  t.bitstream_gpu_address = 0x4'2000'0000ull;
  t.bitstream_bytes = 0x10000;
  t.feedback_gpu_address = 0x4'3000'0000ull;
  return t;
}

static void test_encode() {
  const EncoderSetup s = make_setup(params64());
  const PictureLayout l = picture_layout(64, 64);
  Dwords buf(256, 0xEE);
  TaskWriter w(view(buf));
  write_encode(w, s, l, idr_task(), 0x55);
  CHECK(!w.overflow() && w.size() == 3 + 8 + 4 + 5 + 98 + 5);   // session, task info, context buffer, bitstream buffer, the encode block, feedback
  // session
  CHECK(buf[0] == 12 && buf[1] == 1 && buf[2] == 0x55);
  // task info: encode, no dependency, feedback index 0, bitstream ring index 0
  CHECK(buf[3] == 32 && buf[4] == 2 && buf[6] == 3 && buf[7] == 0 && buf[9] == 0 && buf[10] == 0);
  // context buffer, bitstream buffer
  CHECK(buf[11] == 16 && buf[12] == 0x05000001 && buf[13] == 4 && buf[14] == 0x10000000);
  CHECK(buf[15] == 20 && buf[16] == 0x05000004 && buf[17] == 4 && buf[18] == 0x20000000 && buf[19] == 0x10000);
  // the encode block
  const uint32_t b = 20;
  CHECK(buf[b] == 98 * 4 && buf[b + 1] == 0x03000001);
  CHECK(buf[b + pos(1)] == 0x11 && buf[b + pos(2)] == 0 && buf[b + pos(3)] == 0x10000 && buf[b + pos(4)] == 0 && buf[b + pos(5)] == 0 && buf[b + pos(6)] == 0 && buf[b + pos(7)] == 0);
  CHECK(buf[b + pos(8)] == 4 && buf[b + pos(9)] == 0x1000 && buf[b + pos(10)] == 4 && buf[b + pos(11)] == 0x5000);   // the input pictures' addresses
  CHECK(buf[b + pos(12)] == 64 && buf[b + pos(13)] == 256 && buf[b + pos(14)] == 256 && buf[b + pos(15)] == 0x00010000 && buf[b + pos(16)] == 0);
  CHECK(buf[b + pos(17)] == 3 && buf[b + pos(18)] == 1 && buf[b + pos(19)] == 0 && buf[b + pos(20)] == 0 && buf[b + pos(21)] == 1 && buf[b + pos(22)] == 0);   // IDR, flag, id, mgs, reference, layer
  CHECK(buf[b + pos(23)] == 0 && buf[b + pos(24)] == 0 && buf[b + pos(25)] == 0 && buf[b + pos(26)] == 0 && buf[b + pos(27)] == 0);                         // override, l0/l1 minus one, modification op/num
  for (uint32_t i = 28; i <= 53; ++i) CHECK(buf[b + pos(i)] == 0);   // three more modifications and four markings
  // L0[0]: the structure then five (none for an IDR), L0[1]: six, L1[0]: the structure then five
  CHECK(buf[b + pos(54)] == 0 && buf[b + pos(55)] == 0 && buf[b + pos(56)] == 0 && buf[b + pos(57)] == 0 && buf[b + pos(58)] == 0xffffffff && buf[b + pos(59)] == 0xffffffff);
  CHECK(buf[b + pos(60)] == 0 && buf[b + pos(61)] == 0 && buf[b + pos(62)] == 0 && buf[b + pos(63)] == 0 && buf[b + pos(64)] == 0xffffffff && buf[b + pos(65)] == 0xffffffff);
  CHECK(buf[b + pos(66)] == 0 && buf[b + pos(67)] == 0 && buf[b + pos(68)] == 0 && buf[b + pos(69)] == 0 && buf[b + pos(70)] == 0xffffffff && buf[b + pos(71)] == 0xffffffff);
  CHECK(buf[b + pos(72)] == 0 && buf[b + pos(73)] == 256 * 64);                                      // the picture's place in the buffer
  for (uint32_t i = 74; i <= 78; ++i) CHECK(buf[b + pos(i)] == 0);                                     // collocated and the base pictures
  CHECK(buf[b + pos(79)] == 0 && buf[b + pos(80)] == 0 && buf[b + pos(81)] == 0 && buf[b + pos(82)] == 11 && buf[b + pos(83)] == 0);   // picture count less one, frame number, order count, remaining I and P
  for (uint32_t i = 84; i <= 96; ++i) CHECK(buf[b + pos(i)] == 0);
  // the feedback block after it
  const uint32_t f = b + 98;
  CHECK(buf[f] == 20 && buf[f + 1] == 0x05000005 && buf[f + 2] == 4 && buf[f + 3] == 0x30000000 && buf[f + 4] == 1);
  CHECK(buf[f + 5] == 0xEE);   // (nothing past the end)
}

static void test_encode_p_and_b() {
  const EncoderSetup s = make_setup(params64());
  const PictureLayout l = picture_layout(64, 64);
  Dwords buf(256, 0);
  // A P picture whose reference is the previous frame: no modification; the L0 entry is the reference's. insertHeaders is off after the first picture.
  EncodeTask p = idr_task();
  p.picture_type = PictureType::P;
  p.frame_num = 1;
  p.frame_num_cnt = 2;
  p.pic_order_cnt = 2;
  p.ref_idx_l0 = 0;
  p.not_referenced = false;
  p.l0 = {PictureType::IDR, 0, 0, 0, 256 * 64};
  p.current_luma_offset = 24576;
  p.current_chroma_offset = 24576 + 256 * 64;
  {
    TaskWriter w(view(buf));
    write_encode(w, s, l, p, 1);
    const uint32_t b = 20;
    CHECK(buf[b + pos(1)] == 0 && buf[b + pos(17)] == 0 && buf[b + pos(18)] == 0 && buf[b + pos(21)] == 1);       // headers off, P, not IDR, referenced
    CHECK(buf[b + pos(26)] == 0 && buf[b + pos(27)] == 0);                                                           // distance 1: no modification
    CHECK(buf[b + pos(54)] == 0 && buf[b + pos(55)] == 3 && buf[b + pos(56)] == 0 && buf[b + pos(57)] == 0 && buf[b + pos(58)] == 0 && buf[b + pos(59)] == 256 * 64);   // L0[0] = the IDR in slot 0
    CHECK(buf[b + pos(66)] == 0 && buf[b + pos(70)] == 0xffffffff);                                                  // no L1
    CHECK(buf[b + pos(72)] == 24576 && buf[b + pos(73)] == 24576 + 256 * 64 && buf[b + pos(79)] == 1 && buf[b + pos(80)] == 1 && buf[b + pos(81)] == 2);
  }
  // A P picture that refers back three frames: the modification says so (operation 1, distance - 1); a non-referenced picture says so too.
  p.frame_num = 5;
  p.ref_idx_l0 = 2;
  p.not_referenced = true;
  {
    TaskWriter w(view(buf));
    write_encode(w, s, l, p, 1);
    const uint32_t b = 20;
    CHECK(buf[b + pos(21)] == 0 && buf[b + pos(26)] == 1 && buf[b + pos(27)] == 2);
  }
  // A B picture has both lists.
  EncodeTask bt = p;
  bt.picture_type = PictureType::B;
  bt.frame_num = 3;
  bt.ref_idx_l0 = 2;
  bt.l0 = {PictureType::P, 2, 4, 24576, 24576 + 16384};
  bt.l1 = {PictureType::P, 4, 8, 2 * 24576, 2 * 24576 + 16384};
  {
    TaskWriter w(view(buf));
    write_encode(w, s, l, bt, 1);
    const uint32_t b = 20;
    CHECK(buf[b + pos(17)] == 1 && buf[b + pos(26)] == 0);   // B; no modification (only P pictures get one)
    CHECK(buf[b + pos(55)] == 0 && buf[b + pos(56)] == 2 && buf[b + pos(57)] == 4 && buf[b + pos(58)] == 24576 && buf[b + pos(59)] == 24576 + 16384);
    CHECK(buf[b + pos(66)] == 0 && buf[b + pos(67)] == 0 && buf[b + pos(68)] == 4 && buf[b + pos(69)] == 8 && buf[b + pos(70)] == 2 * 24576 && buf[b + pos(71)] == 2 * 24576 + 16384);
  }
  // A bitstream ring index other than the first: the address is pulled back by that many buffers (the firmware adds the index times the size).
  EncodeTask second = idr_task();
  second.bitstream_index = 1;
  {
    TaskWriter w(view(buf));
    write_encode(w, s, l, second, 1);
    CHECK(buf[10] == 1 && buf[17] == 4 && buf[18] == 0x20000000 - 0x10000 && buf[19] == 0x10000);
  }
}

static void test_task_chain() {
  // Two encode tasks in one job: the first task info's offset_of_next_task_info becomes the distance, in bytes... of dwords counted the way Mesa does (the distance in dwords + 3).
  const EncoderSetup s = make_setup(params64());
  const PictureLayout l = picture_layout(64, 64);
  Dwords buf(512, 0);
  TaskWriter w(view(buf));
  EncodeTask a = idr_task();
  write_encode(w, s, l, a, 1);
  const uint32_t first_fields = 2 + 2 + 1;   // session (3 dwords) then task info: its size dword, id, then the fields start at 3 + 2
  const uint32_t first_start = w.size();
  EncodeTask b = idr_task();
  b.picture_type = PictureType::P;
  b.frame_num = 1;
  b.bitstream_index = 1;
  write_encode(w, s, l, b, 1);
  (void)first_start;
  // Mesa: task_info_idx is the index of the offset dword (3 + 2 = 5 for the first, since the session block is 3 dwords and the task info begins with its size and id);
  // when the next encode task info is written, with cdw at the index of its own offset dword, the first's offset becomes cdw - task_info_idx + 3.
  const uint32_t first_offset_at = 3 + 2, second_offset_at = (3 + 8 + 4 + 5 + 98 + 5) + 3 + 2;   // (in dwords from the start of the job)
  CHECK(buf[first_offset_at] == second_offset_at - first_offset_at + 3);
  CHECK(buf[second_offset_at] == 0xffffffff);   // the last task points nowhere
  (void)first_fields;
}

static void test_feedback() {
  Dwords fb(16, 0);
  CHECK(!read_feedback(ArrayView<const uint32_t>(fb.data(), fb.size())).has_bitstream);
  fb[1] = 1; fb[4] = 5000; fb[9] = 200;
  const Feedback f = read_feedback(ArrayView<const uint32_t>(fb.data(), fb.size()));
  CHECK(f.has_bitstream && f.bytes == 4800);
  fb[4] = 100;   // an end before the start is no size
  CHECK(read_feedback(ArrayView<const uint32_t>(fb.data(), fb.size())).bytes == 0);
  CHECK(!read_feedback(ArrayView<const uint32_t>(fb.data(), 9)).has_bitstream);
}

int main() {
  test_writer();
  test_small_blocks();
  test_setup();
  test_layout();
  test_create_and_config();
  test_encode();
  test_encode_p_and_b();
  test_task_chain();
  test_feedback();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd vce3 message tests passed\n");
  return 0;
}
