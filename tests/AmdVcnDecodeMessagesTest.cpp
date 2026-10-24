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


// Host-side tests for the VCN decode messages: the layout of each message is checked against offsets written out here by hand from Mesa's structure definitions
// (independently of the generated AmdVcnMessageLayout.inc the builders use), the values against a real stream's parameter sets, and the buffer sizes against
// worked examples of Mesa's formulas. Builders are run on every too-small buffer; the feedback reader on every truncation and bit flip.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdVcnDecodeMessages.hpp"
#include "Hardware/VideoCodec/Software/H264Parser.hpp"

using namespace hardware;
using namespace hardware::amd_vcn;

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

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path, "rb");
  if (!f) return bytes;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(f);
  return bytes;
}

static uint32_t word(const std::vector<uint8_t>& m, uint32_t offset) {
  return m[offset] | (m[offset + 1] << 8) | (m[offset + 2] << 16) | (uint32_t{m[offset + 3]} << 24);
}

// The first access unit of an Annex B file that holds a slice, with the parameter sets in front of it.
static bool first_picture(const std::vector<uint8_t>& stream, size_t* end) {
  size_t slices = 0;
  for (size_t i = 0; i + 4 < stream.size(); ++i) {
    if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1) {
      const int type = stream[i + 3] & 31;
      if (type == 1 || type == 5) {
        if (++slices == 2 && stream[i + 4] & 0x80) { *end = i; return true; }   // the second slice NAL that starts a picture
      }
    }
  }
  return false;
}

static void test_create_and_destroy() {
  std::vector<uint8_t> m(create_message_bytes(), 0xEE);
  CHECK(create_message_bytes() == 56 && destroy_message_bytes() == 40);
  CHECK(build_create_message(ByteView(m.data(), m.size()), 0x12345678, 1920, 1088));
  // rvcn_dec_message_header_t: header_size, total_size, num_buffers, msg_type, stream_handle, feedback number, index[0] {id, offset, size, filled}
  CHECK(word(m, 0) == 40 && word(m, 4) == 56 && word(m, 8) == 1 && word(m, 12) == 0 && word(m, 16) == 0x12345678 && word(m, 20) == 0);
  CHECK(word(m, 24) == 1 && word(m, 28) == 40 && word(m, 32) == 16 && word(m, 36) == 0);
  // rvcn_dec_message_create_t: stream_type (H264_PERF = 7), session_flags, width, height
  CHECK(word(m, 40) == 7 && word(m, 44) == 0 && word(m, 48) == 1920 && word(m, 52) == 1088);

  std::vector<uint8_t> d(destroy_message_bytes(), 0xEE);
  CHECK(build_destroy_message(ByteView(d.data(), d.size()), 77));
  CHECK(word(d, 0) == 40 && word(d, 4) == 24 && word(d, 8) == 0 && word(d, 12) == 2 && word(d, 16) == 77 && word(d, 20) == 0);
  for (uint32_t i = 24; i < 40; ++i) CHECK(d[i] == 0);   // the unused index is clear

  // Too small a buffer is refused and nothing is written.
  for (uint32_t size = 0; size < create_message_bytes(); ++size) {
    std::vector<uint8_t> small(size, 0xAB);
    CHECK(!build_create_message(ByteView(small.data(), small.size()), 1, 64, 64));
    for (uint8_t b : small) CHECK(b == 0xAB);
  }
  for (uint32_t size = 0; size < destroy_message_bytes(); ++size) {
    std::vector<uint8_t> small(size, 0xAB);
    CHECK(!build_destroy_message(ByteView(small.data(), small.size()), 1));
  }
}

static void test_buffer_sizes() {
  // 1920x1080 is coded as 1920x1088 (120 x 68 macroblocks), level 4.1 allows 32768 macroblocks = 4 frames; four reference frames asked for: five pictures.
  AvcBufferSizes s = avc_buffer_sizes(1920, 1088, 41, 4);
  CHECK(s.references == 5);
  const uint64_t image = ((1920ull * 1088 * 3 / 2) + 1023) / 1024 * 1024;   // 3133440, a multiple of 1024 already
  CHECK(image == 3133440);
  CHECK(s.dpb == image * 5);
  CHECK(s.context == 5ull * ((120ull * 68 * 192 + 255) / 256 * 256));
  // Few references asked for, a level that allows many small frames: the level wins, capped at 17 pictures.
  s = avc_buffer_sizes(320, 240, 41, 1);
  CHECK(s.references == 17);
  s = avc_buffer_sizes(320, 240, 30, 1);   // 8100 / (20 * 16) = 25, +1, capped at 17
  CHECK(s.references == 17);
  // Unknown levels get the biggest allowance, never an overflow.
  CHECK(avc_buffer_sizes(64, 64, 0, 0).references == 17);
  // Sizes that are refused give nothing.
  CHECK(avc_buffer_sizes(0, 0, 41, 1).dpb == 0 && avc_buffer_sizes(1921, 1088, 41, 1).dpb == 0 && avc_buffer_sizes(8192, 4320, 51, 1).dpb == 0 && avc_buffer_sizes(64, 64, 41, 17).dpb == 0);
  CHECK(avc_buffer_sizes(4096, 2304, 51, 16).dpb != 0 && avc_buffer_sizes(4096, 2304, 51, 16).dpb < (1ull << 32) * 4);

  const TargetLayout t = target_layout(1920, 1088);
  CHECK(t.luma_pitch == 2048 && t.chroma_pitch == 2048 && t.uv_pitch_texels == 1024);
  CHECK(t.chroma_offset == 2048ull * 1088 && t.bytes >= t.chroma_offset + 2048ull * 544 && t.bytes % 4096 == 0);
  const TargetLayout odd = target_layout(176, 144);
  CHECK(odd.luma_pitch == 256 && odd.chroma_offset == 256ull * 160 && odd.bytes >= odd.chroma_offset + 256ull * 72);
  CHECK(target_layout(0, 16).bytes == 0 && target_layout(100, 100).bytes == 0 && target_layout(8192, 16).bytes == 0);
  CHECK(bitstream_buffer_bytes(1920, 1088) >= 1920ull * 1088 * 2 && bitstream_buffer_bytes(1920, 1088) % 4096 == 0 && bitstream_buffer_bytes(15, 16) == 0);
}

static void test_decode_message_from_a_stream() {
  const std::vector<uint8_t> stream = read_file("tests/data/h264_bframes.h264");
  size_t end = 0;
  CHECK(!stream.empty() && first_picture(stream, &end));
  if (stream.empty() || !end) return;
  TestAllocator alloc;
  {
    H264StreamParser parser(alloc);
    List<H264Slice> slices(alloc);
    CHECK(parser.parse(ConstByteView(stream.data(), end), &slices) && !slices.empty());
    const H264Slice& first = slices[0];
    const h264::Sps* sps = parser.sps_for(first.header);
    const h264::Pps* pps = parser.pps_for(first.header);
    CHECK(sps && pps);
    if (!sps || !pps) return;
    AvcReference refs[2] = {{7, 14, 15}, {9, -4, -3}};
    AvcPictureState state;
    CHECK(make_avc_picture_state(*parser.parameter_sets(), *sps, *pps, first.header, 20, 21, ArrayView<const AvcReference>(refs), &state));
    CHECK(state.width == uint32_t(sps->mb_w) * 16 && state.height == uint32_t(sps->mb_h) * 16 && state.level == uint32_t(sps->level_idc));
    CHECK(state.reference_count == 2 && state.frame_num == uint32_t(first.header.frame_num) && state.top_poc == 20 && state.bottom_poc == 21);
    CHECK(state.sps_flags & (1u << 7));   // the extension-support bit Mesa always sets
    CHECK(((state.pps_flags >> 8) & 1) == (pps->cabac ? 1u : 0u) && ((state.pps_flags >> 0) & 1) == (pps->transform_8x8 ? 1u : 0u));
    // With no scaling matrix in the stream the lists are flat 16, in either order.
    if (!sps->scaling_matrix_present && !pps->scaling_matrix_present) {
      for (int i = 0; i < 6; ++i) for (int k = 0; k < 16; ++k) CHECK(state.scaling_4x4[i][k] == 16);
      for (int i = 0; i < 2; ++i) for (int k = 0; k < 64; ++k) CHECK(state.scaling_8x8[i][k] == 16);
    }

    DecodeBuffers b;
    b.bitstream_bytes = 1000;
    const AvcBufferSizes sizes = avc_buffer_sizes(state.width, state.height, state.level, state.num_ref_frames);
    b.dpb_bytes = sizes.dpb;
    b.context_bytes = sizes.context;
    b.target = target_layout(state.width, state.height);
    std::vector<uint8_t> m(decode_message_bytes(), 0xEE);
    CHECK(decode_message_bytes() == 56 + 188 + 1116);
    CHECK(build_avc_decode_message(ByteView(m.data(), m.size()), 5, 42, state, b));
    // header: total_size is the size up to the AVC message (Mesa does not count the codec message), two buffers, decode type
    CHECK(word(m, 0) == 40 && word(m, 4) == 244 && word(m, 8) == 2 && word(m, 12) == 1 && word(m, 16) == 5 && word(m, 20) == 42);
    CHECK(word(m, 24) == 2 && word(m, 28) == 56 && word(m, 32) == 188 && word(m, 36) == 0);   // index[0]: the decode message
    CHECK(word(m, 40) == 6 && word(m, 44) == 244 && word(m, 48) == 1116 && word(m, 52) == 0);   // the codec's index
    // rvcn_dec_message_decode_t at 56
    CHECK(word(m, 56) == 7 && word(m, 60) == 0 && word(m, 64) == state.width && word(m, 68) == state.height);
    CHECK(word(m, 72) == 1024 && word(m, 76) == uint32_t(b.dpb_bytes) && word(m, 80) == uint32_t(b.target.bytes) && word(m, 84) == 0 && word(m, 88) == 0);
    CHECK(word(m, 92) == uint32_t(b.context_bytes) && word(m, 96) == 128 * 1024);
    CHECK(word(m, 56 + 18 * 4) == ((state.width + 31) / 32 * 32));   // db_pitch
    CHECK(word(m, 56 + 25 * 4) == b.target.luma_pitch && word(m, 56 + 26 * 4) == b.target.uv_pitch_texels);
    CHECK(word(m, 56 + 29 * 4) == 0 && word(m, 56 + 28 * 4) == 0);   // linear
    CHECK(word(m, 56 + 34 * 4) == 0 && word(m, 56 + 36 * 4) == uint32_t(b.target.chroma_offset) && word(m, 56 + 37 * 4) == uint32_t(b.target.chroma_offset));
    // rvcn_dec_message_avc_t at 244
    CHECK(word(m, 244) == (sps->profile_idc == 100 ? 2u : sps->profile_idc == 66 ? 0u : 1u) && word(m, 248) == state.level);
    CHECK(word(m, 244 + 260) == state.frame_num && word(m, 244 + 464) == state.frame_num);
    CHECK(word(m, 244 + 264) == 7 && word(m, 244 + 268) == 9);                          // frame_num_list
    CHECK(int32_t(word(m, 244 + 328)) == 20 && int32_t(word(m, 244 + 332)) == 21);      // curr_field_order_cnt_list
    CHECK(int32_t(word(m, 244 + 336)) == 14 && int32_t(word(m, 244 + 340)) == 15 && int32_t(word(m, 244 + 344)) == -4 && int32_t(word(m, 244 + 348)) == -3);
    CHECK(word(m, 244 + 272) == 0 && word(m, 244 + 352) == 0);                          // unused entries are clear
    CHECK(m[244 + 17] == sps->log2_max_frame_num - 4 && m[244 + 20] == sps->poc_type && m[244 + 22] == sps->max_num_ref_frames);

    // Too small: refused, and nothing written.
    for (uint32_t size : {0u, 1u, 55u, 56u, 243u, 244u, 1359u}) {
      std::vector<uint8_t> small(size, 0xAB);
      CHECK(!build_avc_decode_message(ByteView(small.data(), small.size()), 5, 42, state, b));
      for (uint8_t v : small) CHECK(v == 0xAB);
    }
    // Sizes that do not fit the message's 32-bit fields are refused.
    DecodeBuffers huge = b;
    huge.dpb_bytes = 1ull << 32;
    CHECK(!build_avc_decode_message(ByteView(m.data(), m.size()), 5, 42, state, huge));

    // The scaling table and the feedback header.
    std::vector<uint8_t> page(kMessageFeedbackItBytes, 0xEE);
    CHECK(prepare_feedback_and_tables(ByteView(page.data(), page.size()), state));
    CHECK(word(page, kFeedbackOffset) == 44 && word(page, kFeedbackOffset + 4) == 44 && word(page, kFeedbackOffset + 8) == 0 && word(page, kFeedbackOffset + 16) == 0);
    CHECK(page[kItTableOffset] == state.scaling_4x4[0][0] && page[kItTableOffset + 96] == state.scaling_8x8[0][0] && page[kItTableOffset + 96 + 64] == state.scaling_8x8[1][0]);
    CHECK(!prepare_feedback_and_tables(ByteView(page.data(), page.size() - 1), state));
  }
  CHECK(alloc.live == 0);
}

static void test_scaling_order() {
  // A matrix that is not flat comes out in zigzag order: entry k of the bitstream order is the raster position kZigzag[k]. The first four of the 4x4 zigzag are
  // raster positions 0, 1, 4, 8.
  uint8_t w4[6][16], w8[2][64], z4[6][16], z8[2][64];
  for (int i = 0; i < 6; ++i) for (int k = 0; k < 16; ++k) w4[i][k] = uint8_t(k + 1);
  for (int i = 0; i < 2; ++i) for (int k = 0; k < 64; ++k) w8[i][k] = uint8_t(k + 1);
  h264::ParameterSets::scaling_to_bitstream_order(w4, w8, z4, z8);
  CHECK(z4[0][0] == 1 && z4[0][1] == 2 && z4[0][2] == 5 && z4[0][3] == 9);
  CHECK(z8[0][0] == 1 && z8[0][1] == 2 && z8[0][2] == 9 && z8[0][3] == 17);
  // A permutation: every value appears once.
  for (int i = 0; i < 6; ++i) { int seen = 0; for (int k = 0; k < 16; ++k) seen |= 1 << (z4[i][k] - 1); CHECK(seen == 0xFFFF); }
}

static void test_feedback_reader() {
  std::vector<uint8_t> page(kMessageFeedbackItBytes, 0);
  auto put = [&](uint32_t offset, uint32_t v) { for (int i = 0; i < 4; ++i) page[kFeedbackOffset + offset + i] = uint8_t(v >> (8 * i)); };
  put(0, 44); put(4, 44); put(12, 42); put(16, 0); put(24, 0);
  Feedback f = read_feedback(ConstByteView(page.data(), page.size()));
  CHECK(f.valid && f.feedback_number == 42 && f.status == 0 && f.error_bits == 0);
  put(16, 7); put(24, 0x80);
  f = read_feedback(ConstByteView(page.data(), page.size()));
  CHECK(f.valid && f.status == 7 && f.error_bits == 0x80);
  // Every truncation is refused without reading past the end; every bit flip is read without fault, and a flipped header size is not valid.
  for (size_t size = 0; size < page.size(); ++size) CHECK(!read_feedback(ConstByteView(page.data(), size)).valid);
  for (size_t byte = 0; byte < 64; ++byte)
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> flipped = page;
      flipped[kFeedbackOffset + byte] ^= uint8_t(1 << bit);
      const Feedback g = read_feedback(ConstByteView(flipped.data(), flipped.size()));
      if (byte < 4) CHECK(!g.valid);
    }
}


// The H.265 messages: offsets written out by hand from rvcn_dec_message_hevc_t (276 bytes), values from a made-up picture whose fields are all distinct.
static void test_hevc_messages() {
  CHECK(hevc_decode_message_bytes() == 56 + 188 + 276);
  uint8_t created[16];
  (void)created;
  std::vector<uint8_t> c(create_message_bytes(), 0);
  CHECK(build_create_message(ByteView(c.data(), c.size()), 3, 1920, 1080, kStreamTypeH265) && word(c, 40) == 0x10 && word(c, 48) == 1920 && word(c, 52) == 1080);

  h265::Sps sps;
  h265::Pps pps;
  h265::SliceHeader first;
  sps.width = 1920;
  sps.height = 1080;
  sps.max_sub_layers = 2;
  sps.max_dec_pic_buffering[1] = 5;
  sps.log2_max_poc_lsb = 8;
  sps.log2_min_cb = 3;
  sps.log2_ctb = 6;
  sps.log2_min_tb = 2;
  sps.log2_max_tb = 5;
  sps.max_th_depth_inter = 3;
  sps.max_th_depth_intra = 2;
  sps.amp = sps.sao = sps.temporal_mvp = sps.strong_intra_smoothing = true;
  sps.pcm = true;
  sps.pcm_bit_depth_luma = 7;
  sps.pcm_bit_depth_chroma = 6;
  sps.log2_min_pcm = 4;
  sps.log2_max_pcm = 5;
  sps.num_st_rps = 9;
  sps.num_lt_sps = 2;
  sps.pic_w_ctb = 30;
  sps.pic_h_ctb = 17;
  pps.sign_data_hiding = true;
  pps.cu_qp_delta_enabled = true;
  pps.diff_cu_qp_delta_depth = 2;
  pps.cb_qp_offset = -3;
  pps.cr_qp_offset = 4;
  pps.init_qp = 30;
  pps.num_ref_idx_default[0] = 3;
  pps.num_ref_idx_default[1] = 2;
  pps.beta_offset_div2 = -2;
  pps.tc_offset_div2 = 1;
  pps.log2_parallel_merge_level = 4;
  pps.tiles_enabled = true;
  pps.uniform_spacing = true;
  pps.num_tile_cols = 3;
  pps.num_tile_rows = 2;
  pps.num_extra_slice_header_bits = 1;
  first.num_delta_pocs_ref_rps_idx = 5;

  h265::OffloadPicture pic;
  pic.sps = &sps;
  pic.pps = &pps;
  pic.first = &first;
  pic.poc = -7;
  pic.slot = 4;
  pic.ref_count = 2;
  pic.refs[0] = {2, 8, false};
  pic.refs[1] = {9, -16, true};
  pic.st_before_count = 1;
  pic.st_before[0] = 1;
  pic.st_after_count = 1;
  pic.st_after[0] = 0;
  pic.lt_count = 0;
  for (auto& l : pic.list) for (uint8_t& e : l) e = 0xFF;
  pic.list[0][0] = 1;
  pic.list[1][0] = 0;

  DecodeBuffers b;
  b.bitstream_bytes = 1000;
  const HevcBufferSizes sizes = hevc_buffer_sizes(1920, 1080, 5);
  b.dpb_bytes = sizes.dpb;
  b.context_bytes = sizes.context;
  b.target = target_layout(1920, 1080);
  std::vector<uint8_t> m(hevc_decode_message_bytes(), 0xEE);
  CHECK(build_hevc_decode_message(ByteView(m.data(), m.size()), 5, 42, pic, b));
  CHECK(word(m, 0) == 40 && word(m, 4) == 244 && word(m, 8) == 2 && word(m, 12) == 1 && word(m, 16) == 5 && word(m, 20) == 42);
  CHECK(word(m, 24) == 2 && word(m, 28) == 56 && word(m, 32) == 188);
  CHECK(word(m, 40) == 13 && word(m, 44) == 244 && word(m, 48) == 276);
  CHECK(word(m, 56) == 0x10 && word(m, 64) == 1920 && word(m, 68) == 1080 && word(m, 72) == 1024 && word(m, 76) == uint32_t(sizes.dpb) && word(m, 92) == uint32_t(sizes.context));
  CHECK(word(m, 56 + 100) == 2048 && word(m, 56 + 104) == 1024 && word(m, 56 + 144) == uint32_t(b.target.chroma_offset));
  const uint32_t h = 244;
  // sps_info_flags: amp 1, sao 2, pcm 3, temporal mvp 6, strong intra 7, and 10 (reference lists in use)
  CHECK(word(m, h) == ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 6) | (1u << 7) | (1u << 10)));
  // pps_info_flags: sign hiding 2, cu_qp_delta 6, tiles 11, uniform spacing 13, loop filter across tiles 14 (the default is on)
  CHECK(word(m, h + 4) == ((1u << 2) | (1u << 6) | (1u << 11) | (1u << 13) | (1u << 14)));
  CHECK(m[h + 8] == 1 && m[h + 9] == 0 && m[h + 10] == 0 && m[h + 11] == 4);
  CHECK(m[h + 12] == 4 && m[h + 13] == 0 && m[h + 14] == 3 && m[h + 15] == 0);
  CHECK(m[h + 16] == 3 && m[h + 17] == 3 && m[h + 18] == 2 && m[h + 19] == 6);
  CHECK(m[h + 20] == 5 && m[h + 21] == 1 && m[h + 22] == 1 && m[h + 23] == 1);
  CHECK(m[h + 24] == 9 && m[h + 25] == 2 && m[h + 26] == 2 && m[h + 27] == 1);
  CHECK(int8_t(m[h + 28]) == -3 && int8_t(m[h + 29]) == 4 && int8_t(m[h + 30]) == -2 && int8_t(m[h + 31]) == 1);
  CHECK(m[h + 32] == 2 && m[h + 33] == 2 && m[h + 34] == 1 && m[h + 35] == 2);
  // uniform tiles of 30 x 17 CTBs: columns 10, 10, (10); rows 8, (9)
  CHECK(m[h + 36] == 9 && m[h + 37] == 0 && m[h + 38] == 9 && m[h + 39] == 0 && m[h + 40] == 0);
  CHECK(m[h + 74] == 7 && m[h + 75] == 0 && m[h + 76] == 0);
  CHECK(int8_t(m[h + 116]) == 4 && m[h + 117] == 5 && m[h + 118] == 4 && m[h + 119] == 0 && int32_t(word(m, h + 120)) == -7);
  CHECK(m[h + 124] == 2 && m[h + 125] == 9 && m[h + 126] == 0x7F && m[h + 139] == 0x7F);
  CHECK(int32_t(word(m, h + 140)) == 8 && int32_t(word(m, h + 144)) == -16 && word(m, h + 148) == 0);
  CHECK(m[h + 204] == 1 && m[h + 205] == 0xFF && m[h + 212] == 0 && m[h + 213] == 0xFF && m[h + 220] == 0xFF);
  CHECK(m[h + 244] == 1 && m[h + 245] == 0xFF && m[h + 259] == 0 && m[h + 260] == 0xFF);
  for (uint32_t i = 236; i < 244; ++i) CHECK(m[h + i] == 0);
  // Too small: refused, and nothing written.
  for (uint32_t size : {0u, 55u, 243u, 244u, 519u}) {
    std::vector<uint8_t> small(size, 0xAB);
    CHECK(!build_hevc_decode_message(ByteView(small.data(), small.size()), 5, 42, pic, b));
    for (uint8_t v : small) CHECK(v == 0xAB);
  }
  // Pictures the engine does not take: 10 bits, 4:2:2.
  h265::Sps ten = sps;
  ten.bit_depth_luma = 10;
  pic.sps = &ten;
  CHECK(!build_hevc_decode_message(ByteView(m.data(), m.size()), 5, 42, pic, b));
  pic.sps = &sps;

  // Buffer sizes (Mesa's formulas): 17 pictures of align(align(1920,32) * 1088 * 3 / 2, 256), and the context of 16 bytes per 16x16 block of the padded picture per picture.
  CHECK(sizes.references == 17 && sizes.dpb == 17ull * ((1920ull * 1088 * 3 / 2 + 255) / 256 * 256));
  CHECK(sizes.context == (2175ull / 16) * (1343ull / 16) * 16 * 17 + 52 * 1024);
  CHECK(hevc_buffer_sizes(4096, 2304, 5).references == 8 && hevc_buffer_sizes(4096, 2304, 9).dpb == 0 && hevc_buffer_sizes(1920, 1080, 18).dpb == 0);
  CHECK(hevc_buffer_sizes(100, 100, 1).dpb == 0 && hevc_buffer_sizes(8, 8, 1).dpb == 0 && hevc_buffer_sizes(8192, 4320, 1).dpb == 0 && hevc_buffer_sizes(1928, 1080, 1).dpb != 0);

  // The scaling table: lists in coding (up-right diagonal) order, 4x4 at 0, 8x8 at 96, 16x16 at 480, 32x32 (matrices 0 and 3) at 864; DC values in the message.
  sps.scaling_list_enabled = true;
  for (int i = 0; i < 16; ++i) sps.scaling.list[0][0][i] = static_cast<uint8_t>(i);
  for (int i = 0; i < 64; ++i) {
    sps.scaling.list[1][5][i] = static_cast<uint8_t>(i);
    sps.scaling.list[2][2][i] = static_cast<uint8_t>(100 + i);
    sps.scaling.list[3][3][i] = static_cast<uint8_t>(150 + i);
  }
  sps.scaling.dc[2][2] = 33;
  sps.scaling.dc[3][3] = 44;
  std::vector<uint8_t> t(kMessageFeedbackItBytes, 0xEE);
  CHECK(prepare_hevc_feedback_and_tables(ByteView(t.data(), t.size()), sps, pps));
  static const uint8_t kDiagonal4[16] = {0, 4, 1, 8, 5, 2, 12, 9, 6, 3, 13, 10, 7, 14, 11, 15};
  for (int k = 0; k < 16; ++k) CHECK(t[kItTableOffset + k] == kDiagonal4[k]);
  // 8x8: raster index of the first positions of the diagonal scan: 0, 8, 1, 16, 9, 2
  static const uint8_t kDiagonal8[6] = {0, 8, 1, 16, 9, 2};
  for (int k = 0; k < 6; ++k) {
    CHECK(t[kItTableOffset + 96 + 5 * 64 + k] == kDiagonal8[k]);
    CHECK(t[kItTableOffset + 480 + 2 * 64 + k] == 100 + kDiagonal8[k]);
    CHECK(t[kItTableOffset + 864 + 64 + k] == 150 + kDiagonal8[k]);
  }
  CHECK(t[kItTableOffset + 96 + 5 * 64 + 63] == 63 && t[kItTableOffset + 864 + 64 + 63] == 150 + 63);
  CHECK(word(t, kFeedbackOffset) == 44 && word(t, kFeedbackOffset + 4) == 44);
  pic.sps = &sps;
  std::vector<uint8_t> m2(hevc_decode_message_bytes(), 0);
  CHECK(build_hevc_decode_message(ByteView(m2.data(), m2.size()), 5, 42, pic, b) && m2[h + 228 + 2] == 33 && m2[h + 234 + 1] == 44 && (word(m2, h) & 1) == 1);
  // A picture parameter set's own lists win over the sequence's.
  pps.scaling_list_present = true;
  pps.scaling.set_default();
  CHECK(prepare_hevc_feedback_and_tables(ByteView(t.data(), t.size()), sps, pps) && t[kItTableOffset + 5] == 16);
  // Without scaling lists the table is clear.
  sps.scaling_list_enabled = false;
  CHECK(prepare_hevc_feedback_and_tables(ByteView(t.data(), t.size()), sps, pps));
  for (uint32_t i = 0; i < kItTableBytes; ++i) CHECK(t[kItTableOffset + i] == 0);
}

// The AV1 messages: offsets written out by hand from rvcn_dec_message_av1_t (3576 bytes), values from a made-up frame whose fields are all distinct.
static void test_av1_messages() {
  CHECK(av1_decode_message_bytes() == 56 + 188 + 3576 && av1_message_buffer_bytes() == 0x1800 + 20736);
  av1::SequenceHeader seq = {};
  av1::FrameHeader hdr = {};
  seq.profile = 0;
  seq.max_width = 128;
  seq.max_height = 96;
  seq.layout = av1::PIXEL_LAYOUT_I420;
  seq.sb128 = 1;
  seq.order_hint = 1;
  seq.order_hint_n_bits = 7;
  seq.dual_filter = 1;
  seq.filter_intra = 1;
  seq.ref_frame_mvs = 1;
  hdr.frame_type = av1::FRAME_TYPE_INTER;
  hdr.width[0] = hdr.width[1] = 120;
  hdr.height = 90;
  hdr.frame_offset = 77;
  hdr.frame_id = 5;
  hdr.show_frame = 1;
  hdr.refresh_context = 1;
  hdr.hp = 1;
  hdr.use_ref_frame_mvs = 1;
  hdr.primary_ref_frame = 2;
  hdr.subpel_filter_mode = av1::FILTER_8TAP_SHARP;
  hdr.loopfilter.level_y[0] = 11;
  hdr.loopfilter.level_y[1] = 12;
  hdr.loopfilter.level_u = 13;
  hdr.loopfilter.level_v = 14;
  hdr.loopfilter.sharpness = 3;
  hdr.loopfilter.mode_ref_deltas.ref_delta[0] = 1;
  hdr.loopfilter.mode_ref_deltas.ref_delta[7] = -1;
  hdr.loopfilter.mode_ref_deltas.mode_delta[1] = -2;
  hdr.quant.yac = 100;
  hdr.quant.ydc_delta = -1;
  hdr.quant.udc_delta = 2;
  hdr.quant.vdc_delta = -3;
  hdr.quant.uac_delta = 4;
  hdr.quant.vac_delta = -5;
  hdr.delta.q.res_log2 = 2;
  hdr.delta.lf.res_log2 = 1;
  hdr.tiling.cols = 2;
  hdr.tiling.rows = 1;
  hdr.tiling.col_start_sb[0] = 0;
  hdr.tiling.col_start_sb[1] = 1;
  hdr.tiling.col_start_sb[2] = 2;
  hdr.tiling.row_start_sb[1] = 1;
  hdr.tiling.update = 1;
  hdr.txfm_mode = av1::TX_SWITCHABLE;
  hdr.switchable_comp_refs = 1;
  hdr.refidx[0] = 3;
  hdr.refidx[6] = 7;
  hdr.cdef.damping = 5;
  hdr.cdef.n_bits = 2;
  hdr.cdef.y_strength[1] = 0x2B;
  hdr.cdef.uv_strength[2] = 0x11;
  hdr.restoration.type[0] = av1::RESTORATION_SGRPROJ;
  hdr.restoration.type[1] = av1::RESTORATION_SWITCHABLE;
  hdr.restoration.unit_size[0] = 7;
  hdr.restoration.unit_size[1] = 6;
  hdr.segmentation.enabled = 1;
  for (auto& d : hdr.segmentation.seg_data.d) d.ref = -1;   // (the parser marks an unused reference feature so)
  hdr.segmentation.seg_data.d[2].delta_q = -9;
  hdr.segmentation.seg_data.d[2].ref = 4;
  hdr.segmentation.seg_data.d[1].ref = -1;
  hdr.segmentation.seg_data.preskip = 1;
  hdr.segmentation.seg_data.last_active_segid = 2;
  hdr.segmentation.lossless[3] = 1;
  hdr.gmv[2].type = av1::WM_TYPE_ROT_ZOOM;
  hdr.gmv[2].matrix[1] = 1234;
  const uint8_t tile0[3] = {1, 2, 3}, tile1[5] = {4, 5, 6, 7, 8};
  av1::TileData tiles[2];
  tiles[0].data = ConstByteView(tile0);
  tiles[1].data = ConstByteView(tile1);
  tiles[1].tile_col = 1;
  av1::OffloadFrame frame;
  frame.seq = &seq;
  frame.hdr = &hdr;
  frame.tiles = ArrayView<const av1::TileData>(tiles, 2);
  frame.width = 120;
  frame.height = 90;
  Av1Slots slots;
  slots.current = 4;
  for (uint32_t i = 0; i < 8; ++i) slots.ref_frame_map[i] = static_cast<uint8_t>(i == 3 ? 8 : i == 7 ? 6 : kAv1NoPicture);
  const Av1BufferSizes sizes = av1_buffer_sizes(128, 96);
  DecodeBuffers b;
  b.bitstream_bytes = 8;
  b.dpb_bytes = sizes.dpb;
  b.context_bytes = sizes.context;
  b.target = target_layout(120, 96);
  std::vector<uint8_t> m(av1_decode_message_bytes(), 0xEE);
  CHECK(build_av1_decode_message(ByteView(m.data(), m.size()), 5, 42, frame, slots, b));
  CHECK(word(m, 0) == 40 && word(m, 4) == 244 && word(m, 8) == 2 && word(m, 12) == 1 && word(m, 16) == 5 && word(m, 20) == 42);
  CHECK(word(m, 40) == 0x11 && word(m, 44) == 244 && word(m, 48) == 3576);
  CHECK(word(m, 56) == 0x13 && word(m, 64) == 120 && word(m, 68) == 90 && word(m, 56 + 72) == 128 && word(m, 56 + 76) == 128 && word(m, 56 + 164) == b.target.chroma_pitch);
  const uint32_t a = 244;
  // flags: show_frame 0, refresh_context 2, hp 5, filter_intra 9, dual_filter 14, order_hint 15, use_ref_frame_mvs 17, segmentation 25
  CHECK(word(m, a) == ((1u << 0) | (1u << 2) | (1u << 5) | (1u << 9) | (1u << 14) | (1u << 15) | (1u << 17) | (1u << 25) | (1u << 30)));   // (30: no reference slot is refreshed)
  CHECK(word(m, a + 4) == 5 && word(m, a + 8) == 77);
  CHECK(m[a + 12] == 0 && m[a + 13] == 0 && m[a + 14] == 1 && m[a + 15] == 2 && m[a + 16] == 4 && m[a + 17] == 1 && m[a + 18] == 2);
  CHECK(m[a + 19] == 11 && m[a + 20] == 12 && m[a + 21] == 13 && m[a + 22] == 14 && m[a + 23] == 3);
  CHECK(int8_t(m[a + 24]) == 1 && int8_t(m[a + 31]) == -1 && int8_t(m[a + 33]) == -2);
  CHECK(m[a + 34] == 100 && int8_t(m[a + 35]) == -1 && int8_t(m[a + 36]) == 2 && int8_t(m[a + 37]) == -3 && int8_t(m[a + 38]) == 4 && int8_t(m[a + 39]) == -5);
  CHECK(m[a + 40] == 0xFF && m[a + 41] == 0xFF && m[a + 42] == 0xFF);   // no quantizer matrix: level 15 with the 0xF0 Mesa adds
  CHECK(m[a + 43] == 4 && m[a + 44] == 2 && m[a + 45] == 2 && m[a + 46] == 1 && m[a + 47] == 2 && m[a + 48] == 2 && m[a + 49] == 1);
  CHECK(word(m, a + 52) == 0xFF && word(m, a + 56) == 1);
  CHECK(word(m, a + 60) == 0 && word(m, a + 64) == 1 && word(m, a + 68) == 2 && word(m, a + 72) == 0);
  CHECK(word(m, a + 320) == 0 && word(m, a + 324) == 1 && word(m, a + 580) == 128 && word(m, a + 584) == 96);
  CHECK(word(m, a + 588) == 120 && word(m, a + 592) == 90 && word(m, a + 596) == 120 && m[a + 600] == 8 && m[a + 601] == 7);
  CHECK(m[a + 602] == 0x7F && m[a + 605] == 8 && m[a + 609] == 6 && m[a + 606] == 0x7F);
  CHECK(m[a + 644] == 8 && m[a + 650] == 6 && m[a + 645] == 0x7F);   // frame_refs[i] = ref_frame_map[refidx[i]]: refidx[0] = 3, refidx[6] = 7
  CHECK(m[a + 658] == 0 && m[a + 659] == 0);
  CHECK(int32_t(word(m, a + 660 + (2 * 8 + 0) * 4)) == -9 && int32_t(word(m, a + 660 + (2 * 8 + 5) * 4)) == 4);
  CHECK(m[a + 916 + 2] == ((1u << 0) | (1u << 5)) && m[a + 916 + 1] == 0 && m[a + 916 + 3] == 0);
  CHECK(m[a + 924] == 5 && m[a + 925] == 2 && m[a + 926 + 2] == 0x2B && m[a + 958 + 4] == 0x11);
  CHECK(m[a + 990] == 2 && m[a + 991] == 3 && m[a + 992] == 0 && m[a + 993] == 2 && m[a + 994] == 1 && m[a + 995] == 0);   // sgrproj = 2, switchable = 3; unit sizes 2^7, 2^6
  CHECK(m[a + 996] == 0 && m[a + 998] == 0 && m[a + 1000] == 1 && m[a + 1001] == 2 && m[a + 1002] == (1u << 3));
  CHECK(word(m, a + 1004) == 0);   // no film grain
  CHECK(word(m, a + 1176 + 3 * 44) == 2 && int32_t(word(m, a + 1176 + 3 * 44 + 4 + 4)) == 1234);   // global_motion[i + 1]
  CHECK(word(m, a + 1528) == 0 && word(m, a + 1532) == 3 && word(m, a + 1536) == 3 && word(m, a + 1540) == 5 && word(m, a + 1544) == 0);

  // Refused: 10 bits, film grain, super-resolution, tile tables that do not fit; too small a buffer writes nothing.
  av1::SequenceHeader ten = seq;
  ten.hbd = 1;
  frame.seq = &ten;
  CHECK(!build_av1_decode_message(ByteView(m.data(), m.size()), 5, 42, frame, slots, b));
  frame.seq = &seq;
  hdr.film_grain.present = 1;
  CHECK(!build_av1_decode_message(ByteView(m.data(), m.size()), 5, 42, frame, slots, b));
  hdr.film_grain.present = 0;
  frame.tiles = ArrayView<const av1::TileData>(tiles, 1);
  CHECK(!build_av1_decode_message(ByteView(m.data(), m.size()), 5, 42, frame, slots, b));   // the header says two tiles
  frame.tiles = ArrayView<const av1::TileData>(tiles, 2);
  for (uint32_t size : {0u, 243u, 244u, 3000u, 3819u}) {
    std::vector<uint8_t> small(size, 0xAB);
    CHECK(!build_av1_decode_message(ByteView(small.data(), small.size()), 5, 42, frame, slots, b));
    for (uint8_t v : small) CHECK(v == 0xAB);
  }
  CHECK(build_av1_decode_message(ByteView(m.data(), m.size()), 5, 42, frame, slots, b));

  // Buffer sizes (Mesa's formulas): the context does not depend on the picture, the picture buffer is nine pictures of 64 aligned size, with the 10-bit half more.
  CHECK(sizes.references == 9 && sizes.dpb == 128ull * 128 * 3 / 2 * 9 * 3 / 2);
  CHECK(av1_buffer_sizes(4096, 2304).context == sizes.context && sizes.context > 30000000 && sizes.context < 60000000);
  CHECK(av1_buffer_sizes(0, 10).dpb == 0 && av1_buffer_sizes(10, 0).dpb == 0 && av1_buffer_sizes(4097, 16).dpb == 0 && av1_buffer_sizes(16, 2305).dpb == 0);

  // The feedback header and the segmentation table.
  std::vector<uint8_t> t(av1_message_buffer_bytes(), 0xEE);
  CHECK(prepare_av1_feedback_and_tables(ByteView(t.data(), t.size()), hdr));
  CHECK(word(t, kFeedbackOffset) == 44 && word(t, kFeedbackOffset + 4) == 44 && word(t, kFeedbackOffset + 8) == 0);
  CHECK(t[kItTableOffset + (2 * 8 + 0) * 2] == uint8_t(-9) && t[kItTableOffset + (2 * 8 + 0) * 2 + 1] == 0xFF && t[kItTableOffset + 128 + 2] == ((1u << 0) | (1u << 5)));
  CHECK(t[kItTableOffset + 256] == 0 && t[av1_message_buffer_bytes() - 1] == 0);   // the film grain part is clear
  {
    std::vector<uint8_t> small(av1_message_buffer_bytes() - 1, 0xAB);
    CHECK(!prepare_av1_feedback_and_tables(ByteView(small.data(), small.size()), hdr));
  }

  // The initial contexts: runs of non-zero bytes, in order, inside four contexts of 22528 bytes; everything outside is zero.
  CHECK(av1_context_bytes() == 4 * 22528 && av1_context_run_count() > 100);
  uint64_t covered = 0, previous_end = 0;
  for (uint32_t i = 0; i < av1_context_run_count(); ++i) {
    uint32_t offset = 0;
    ConstByteView bytes;
    CHECK(av1_context_run(i, &offset, &bytes) && !bytes.empty() && offset >= previous_end && uint64_t{offset} + bytes.size() <= av1_context_bytes());
    previous_end = uint64_t{offset} + bytes.size();
    covered += bytes.size();
    CHECK(bytes[0] != 0 && bytes[bytes.size() - 1] != 0);
  }
  CHECK(covered > 30000 && covered < av1_context_bytes());
  uint32_t o = 0;
  ConstByteView bytes;
  CHECK(!av1_context_run(av1_context_run_count(), &o, &bytes));
}

// MPEG-2: the offsets are Mesa's rvcn_dec_message_mpeg2_vld_t, written out here by hand: three indexes of 32 bits, the matrices at 16 and 80, the single bytes from 144.
static void test_mpeg2_messages() {
  // 1920x1080 is coded as 1920x1088: image 1920 x 1088 x 1.5 = 3133440 (a multiple of 1024), six of them.
  Mpeg2BufferSizes sizes = mpeg2_buffer_sizes(1920, 1088);
  CHECK(sizes.references == 6 && sizes.dpb == 3133440ull * 6);
  // 720x576 PAL: 45 x 36 macroblocks; the width is rounded up to 736 for the pitch.
  CHECK(mpeg2_buffer_sizes(720, 576).dpb == ((736ull * 576 * 3 / 2 + 1023) / 1024 * 1024) * 6);
  // A width that is not a multiple of 32 is rounded up: 16 x 16 macroblock picture 48 x 16 -> pitch 64.
  CHECK(mpeg2_buffer_sizes(48, 16).dpb == ((64ull * 16 * 3 / 2 + 1023) / 1024 * 1024) * 6);
  CHECK(mpeg2_buffer_sizes(0, 0).dpb == 0 && mpeg2_buffer_sizes(8192, 4320).dpb == 0 && mpeg2_buffer_sizes(100, 100).dpb == 0);

  mpeg2::SequenceHeader seq;
  seq.valid = true;
  seq.mb_width = 120;
  seq.mb_height = 68;
  seq.chroma_format = 1;
  mpeg2::PictureHeader pic;
  pic.type = mpeg2::kPictureB;
  pic.structure = mpeg2::kFramePicture;
  pic.f_code[0][0] = 3;
  pic.f_code[0][1] = 2;
  pic.f_code[1][0] = 4;
  pic.f_code[1][1] = 1;
  pic.intra_dc_precision = 2;
  pic.top_field_first = true;
  pic.q_scale_type = true;
  pic.alternate_scan = true;
  mpeg2::OffloadPicture picture;
  picture.seq = &seq;
  picture.pic = &pic;
  for (int i = 0; i < 64; ++i) {
    picture.intra_matrix_scan[i] = uint8_t(i + 1);
    picture.non_intra_matrix_scan[i] = uint8_t(200 - i);
  }
  DecodeBuffers buffers;
  buffers.bitstream_bytes = 1000;
  buffers.dpb_bytes = sizes.dpb;
  buffers.target = target_layout(1920, 1088);
  CHECK(mpeg2_decode_message_bytes() == 244 + 160);
  Mpeg2Slots slots;
  slots.current = 2;
  slots.forward = 5;
  slots.backward = 1;
  std::vector<uint8_t> m(4096, 0xAA);
  CHECK(build_mpeg2_decode_message(ByteView(m.data(), m.size()), 9, 77, picture, slots, buffers));
  // Header, indexes: decode message (id 2) at 56, the MPEG-2 message (id 10) at 244.
  CHECK(word(m, 0) == 40 && word(m, 4) == 244 && word(m, 8) == 2 && word(m, 12) == 1 && word(m, 16) == 9 && word(m, 20) == 77);
  CHECK(word(m, 24) == 2 && word(m, 28) == 56 && word(m, 32) == 188 && word(m, 40) == 10 && word(m, 44) == 244 && word(m, 48) == 160);
  CHECK(word(m, 56) == 3);   // stream type MPEG-2
  CHECK(word(m, 56 + 8) == 1920 && word(m, 56 + 12) == 1088 && word(m, 56 + 20) == sizes.dpb && word(m, 56 + 36) == 0);
  const uint32_t h = 244;
  CHECK(word(m, h) == 2 && word(m, h + 4) == 5 && word(m, h + 8) == 1);
  CHECK(m[h + 12] == 1 && m[h + 13] == 1);
  for (int i = 0; i < 64; ++i) CHECK(m[h + 16 + i] == i + 1 && m[h + 80 + i] == 200 - i);
  CHECK(m[h + 144] == 0 && m[h + 145] == 1 && m[h + 146] == 3);
  CHECK(m[h + 148] == 3 && m[h + 149] == 2 && m[h + 150] == 4 && m[h + 151] == 1);
  CHECK(m[h + 152] == 2 && m[h + 153] == 3 && m[h + 154] == 1 && m[h + 155] == 0 && m[h + 156] == 0 && m[h + 157] == 1 && m[h + 158] == 0 && m[h + 159] == 1);
  CHECK(m[mpeg2_decode_message_bytes()] == 0xAA);   // nothing written past the message
  // Refused: too small a buffer (every size), another chroma format, slots out of range, no headers, a picture size the engine does not take.
  for (uint32_t n = 0; n < mpeg2_decode_message_bytes(); ++n) CHECK(!build_mpeg2_decode_message(ByteView(m.data(), n), 9, 77, picture, slots, buffers));
  seq.chroma_format = 2;
  CHECK(!build_mpeg2_decode_message(ByteView(m.data(), m.size()), 9, 77, picture, slots, buffers));
  seq.chroma_format = 1;
  Mpeg2Slots bad = slots;
  bad.forward = 6;
  CHECK(!build_mpeg2_decode_message(ByteView(m.data(), m.size()), 9, 77, picture, bad, buffers));
  bad = slots;
  bad.current = 6;
  CHECK(!build_mpeg2_decode_message(ByteView(m.data(), m.size()), 9, 77, picture, bad, buffers));
  mpeg2::OffloadPicture empty;
  CHECK(!build_mpeg2_decode_message(ByteView(m.data(), m.size()), 9, 77, empty, slots, buffers));
  seq.mb_width = 300;
  CHECK(!build_mpeg2_decode_message(ByteView(m.data(), m.size()), 9, 77, picture, slots, buffers));
  seq.mb_width = 120;
  pic.type = 4;
  CHECK(!build_mpeg2_decode_message(ByteView(m.data(), m.size()), 9, 77, picture, slots, buffers));
  pic.type = mpeg2::kPictureB;
  // The feedback header, and no more.
  std::vector<uint8_t> fb(kMessageFeedbackItBytes, 0xAA);
  CHECK(prepare_mpeg2_feedback(ByteView(fb.data(), fb.size())));
  CHECK(word(fb, kFeedbackOffset) == 44 && word(fb, kFeedbackOffset + 4) == 44 && word(fb, kFeedbackOffset + 8) == 0 && fb[0] == 0xAA && fb[kItTableOffset] == 0xAA);
  CHECK(!prepare_mpeg2_feedback(ByteView(fb.data(), kMessageFeedbackItBytes - 1)));
}

int main() {
  test_create_and_destroy();
  test_buffer_sizes();
  test_decode_message_from_a_stream();
  test_scaling_order();
  test_feedback_reader();
  test_hevc_messages();
  test_av1_messages();
  test_mpeg2_messages();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdVcnDecodeMessagesTest passed\n");
  return 0;
}
