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

int main() {
  test_create_and_destroy();
  test_buffer_sizes();
  test_decode_message_from_a_stream();
  test_scaling_order();
  test_feedback_reader();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdVcnDecodeMessagesTest passed\n");
  return 0;
}
