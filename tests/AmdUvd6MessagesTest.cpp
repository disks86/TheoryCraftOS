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

// Host-side tests for the UVD 6 messages (AmdUvd6Messages): the layout of the structure the firmware reads (checked against the offsets worked out from Mesa's structure
// definition by hand - dword counts of the fields in ac_uvd_dec.h - independently of the generated ones the code uses), the create, destroy and decode messages field by
// field, the buffer sizes, the scaling table, the order of the buffers of a job, and a decode message made from a real H.264 stream.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdUvd6Messages.hpp"
#include "Hardware/VideoCodec/Software/H264Parser.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"
#include "Hardware/VideoCodec/Software/Vc1.hpp"

using namespace hardware;
using namespace hardware::amd_uvd6;

#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestHevcStream.inc"   // the H.265 stream the engine's self-test decodes
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestMpeg2Stream.inc"   // and the MPEG-2 one
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestVc1Stream.inc"   // and the two VC-1 ones

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

using Bytes = std::vector<uint8_t>;
static uint32_t word(const Bytes& m, uint32_t offset) { return m[offset] | (m[offset + 1] << 8) | (m[offset + 2] << 16) | (uint32_t{m[offset + 3]} << 24); }
static ByteView view(Bytes& b) { return ByteView(b.data(), b.size()); }

// struct ruvd_msg: four dwords of header, then the union; the decode message's fields in dwords (ac_uvd_dec.h), then 16 reserved dwords and the codec union.
constexpr uint32_t kBody = 16;
constexpr uint32_t dec(uint32_t dword) { return kBody + 4 * dword; }
constexpr uint32_t kDecStreamType = 0, kDecFlags = 1, kDecWidth = 2, kDecHeight = 3, kDecDpbSize = 5, kDecDpbReserved = 7, kDecDbPitch = 9, kDecDbSurfTile = 13, kDecBsdSize = 18,
                   kDecDtPitch = 24, kDecDtTiling = 25, kDecDtArray = 26, kDecDtField = 27, kDecLumaTop = 28, kDecLumaBottom = 29, kDecChromaTop = 30, kDecChromaBottom = 31,
                   kDecDtSurfTile = 32, kDecDtUvSurfTile = 33, kDecWaChromaTop = 34, kDecWaChromaBottom = 35;
constexpr uint32_t kCodec = dec(52);   // 36 dwords of fields + 16 reserved
constexpr uint32_t kH264Size = 1116;
constexpr uint32_t kExtensionSupport = kCodec + 768 * 4;
// struct ruvd_h264 (byte offsets within it)
constexpr uint32_t hProfile = 0, hLevel = 4, hSps = 8, hPps = 12, hChroma = 16, hBdLuma = 17, hBdChroma = 18, hLog2FrameNum = 19, hPocType = 20, hLog2Poc = 21, hNumRef = 22, hQp = 24, hQs = 25,
                   hCqp = 26, hCqp2 = 27, hSliceGroups = 28, hSliceGroupMap = 29, hL0 = 30, hL1 = 31, hScaling4 = 36, hScaling8 = 132, hFrameNum = 260, hFrameNumList = 264,
                   hCurrFoc = 328, hFocList = 336, hDecodedIdx = 464;

static void test_layout() {
  CHECK(message_bytes() == kExtensionSupport + 4 + 64 * 4 + 0 && message_bytes() == 3556 + 0);   // the codec union, the extension flags (4 bytes) and 64 reserved dwords
  CHECK(kCodec == 224 && kExtensionSupport == 3296 && kExtensionSupport + 4 + 256 == 3556);
  CHECK(message_bytes() <= kMessagePageBytes && kFeedbackOffset == kMessagePageBytes && kItTableOffset == 0x1800 && kMessageFeedbackItBytes == 0x1800 + 992);
}

static void test_create_destroy() {
  Bytes m(message_bytes() + 64, 0xEE);
  CHECK(build_create_message(view(m), 0x12345678, 1920, 1088, 15667200));
  CHECK(word(m, 0) == 3556 && word(m, 4) == 0 && word(m, 8) == 0x12345678 && word(m, 12) == 0);
  // create: stream_type, session_flags, asic_id, width, height, dpb_buffer, dpb_size
  CHECK(word(m, 16) == 7 && word(m, 20) == 0 && word(m, 24) == 0 && word(m, 28) == 1920 && word(m, 32) == 1088 && word(m, 36) == 0 && word(m, 40) == 15667200);
  bool cleared = true;
  for (uint32_t i = 44; i < message_bytes(); ++i) cleared = cleared && m[i] == 0;
  CHECK(cleared);
  for (uint32_t i = message_bytes(); i < m.size(); ++i) CHECK(m[i] == 0xEE);   // nothing past the structure is touched
  Bytes d(message_bytes(), 0xEE);
  CHECK(build_destroy_message(view(d), 77) && word(d, 0) == 3556 && word(d, 4) == 2 && word(d, 8) == 77 && word(d, 12) == 0);
  bool zeros = true;
  for (uint32_t i = 16; i < d.size(); ++i) zeros = zeros && d[i] == 0;
  CHECK(zeros);
  Bytes small(message_bytes() - 1, 0xAB);
  CHECK(!build_create_message(view(small), 1, 64, 64, 0) && !build_destroy_message(view(small), 1) && small[0] == 0xAB && small[100] == 0xAB);
  CHECK(!build_create_message(ByteView(), 1, 64, 64, 0));
}

static void test_buffer_sizes() {
  // 1920x1088 at level 4.1 (32768 macroblocks of buffers: 4 frames, +1) with 4 reference frames: five pictures.
  AvcBufferSizes s = avc_buffer_sizes(1920, 1088, 41, 4);
  const uint64_t image = (1920ull * 1088 * 3 / 2 + 1023) / 1024 * 1024;
  CHECK(image == 3133440 && s.references == 5 && s.dpb == image * 5 && s.context == 5ull * (120 * 68 * 192 / 256 * 256));
  // No macroblock context or scaling surface in the picture buffer (the performance stream type on Polaris and newer): the buffer is pictures only.
  CHECK(s.dpb % image == 0);
  // The stream asking for more than the level allows: its own count plus the picture being decoded.
  s = avc_buffer_sizes(1920, 1088, 41, 16);
  CHECK(s.references == 17 && s.dpb == image * 17);
  s = avc_buffer_sizes(320, 240, 30, 1);   // the level allows many small frames: capped at 17
  CHECK(s.references == 17);
  s = avc_buffer_sizes(1280, 720, 31, 2);   // 80 x 46 (aligned up to even) macroblocks: 18000 / 3680 = 4, +1 = 5
  CHECK(s.references == 5 && s.context == 5ull * (80 * 46 * 192 / 256 * 256));
  CHECK(avc_buffer_sizes(64, 64, 0, 0).references == 17);                               // an unknown level: the biggest allowance, no overflow
  // Pictures that are refused give nothing.
  CHECK(avc_buffer_sizes(0, 0, 41, 1).dpb == 0 && avc_buffer_sizes(1930, 1088, 41, 1).dpb == 0 && avc_buffer_sizes(1920, 1080, 41, 1).dpb == 0 &&
        avc_buffer_sizes(8192, 4320, 51, 1).dpb == 0 && avc_buffer_sizes(64, 64, 41, 17).dpb == 0);
  CHECK(avc_buffer_sizes(4096, 2304, 51, 16).dpb != 0);
  CHECK(bitstream_buffer_bytes(1920, 1088) == 1920ull * 1088 * 2);
}

static amd_vcn::AvcPictureState sample_picture() {
  amd_vcn::AvcPictureState p;
  p.width = 1920; p.height = 1088; p.level = 41; p.profile = amd_vcn::kProfileHigh;
  p.sps_flags = 0x85; p.pps_flags = 0x1C1;
  p.chroma_format = 1; p.log2_max_frame_num_minus4 = 4; p.poc_type = 2; p.log2_max_poc_lsb_minus4 = 3; p.num_ref_frames = 4;
  p.pic_init_qp_minus26 = -3; p.chroma_qp_index_offset = -2; p.second_chroma_qp_index_offset = 5;
  p.num_ref_idx_l0_active_minus1 = 3; p.num_ref_idx_l1_active_minus1 = 1;
  for (int l = 0; l < 6; ++l) for (int i = 0; i < 16; ++i) p.scaling_4x4[l][i] = static_cast<uint8_t>(l * 16 + i + 1);
  for (int l = 0; l < 2; ++l) for (int i = 0; i < 64; ++i) p.scaling_8x8[l][i] = static_cast<uint8_t>(200 + l * 20 + i % 20);
  p.frame_num = 9; p.top_poc = -20; p.bottom_poc = -19;
  p.reference_count = 3;
  p.references[0] = {7, 14, 15};
  p.references[1] = {8, -4, -3};
  p.references[2] = {2, 100000, -100000};
  return p;
}

static void test_decode_message() {
  const amd_vcn::AvcPictureState p = sample_picture();
  DecodeBuffers b;
  b.bitstream_bytes = 1001;
  b.dpb_bytes = 15667200;
  b.context_bytes = 7833600;
  b.target = amd_vcn::target_layout(1920, 1088);
  Bytes m(message_bytes() + 32, 0xEE);
  CHECK(build_avc_decode_message(view(m), 5, 42, p, b));
  CHECK(word(m, 0) == 3556 && word(m, 4) == 1 && word(m, 8) == 5 && word(m, 12) == 42);
  CHECK(word(m, dec(kDecStreamType)) == 7 && word(m, dec(kDecFlags)) == 1 && word(m, dec(kDecWidth)) == 1920 && word(m, dec(kDecHeight)) == 1088);
  CHECK(word(m, dec(kDecDpbSize)) == 15667200 && word(m, dec(kDecDpbReserved)) == 7833600 && word(m, dec(kDecDbPitch)) == 1920 && word(m, dec(kDecBsdSize)) == 1024);
  // The decoding target: linear, pitch in bytes, chroma behind the luma, top and bottom alike (a frame), the chroma pitch for the interleaved plane.
  CHECK(word(m, dec(kDecDtPitch)) == 2048 && word(m, dec(kDecDtTiling)) == 0 && word(m, dec(kDecDtArray)) == 0 && word(m, dec(kDecDtField)) == 0);
  CHECK(word(m, dec(kDecLumaTop)) == 0 && word(m, dec(kDecLumaBottom)) == 0 && word(m, dec(kDecChromaTop)) == 2048u * 1088 && word(m, dec(kDecChromaBottom)) == 2048u * 1088);
  CHECK(word(m, dec(kDecDtSurfTile)) == 0 && word(m, dec(kDecDtUvSurfTile)) == 0 && word(m, dec(kDecDbSurfTile)) == 0 && word(m, dec(kDecWaChromaTop)) == 1024 && word(m, dec(kDecWaChromaBottom)) == 0);
  CHECK(m[kExtensionSupport] == 1);
  // The H.264 message.
  CHECK(word(m, kCodec + hProfile) == 2 && word(m, kCodec + hLevel) == 41 && word(m, kCodec + hSps) == 0x85 && word(m, kCodec + hPps) == 0x1C1);
  CHECK(m[kCodec + hChroma] == 1 && m[kCodec + hBdLuma] == 0 && m[kCodec + hBdChroma] == 0 && m[kCodec + hLog2FrameNum] == 4 && m[kCodec + hPocType] == 2 && m[kCodec + hLog2Poc] == 3 && m[kCodec + hNumRef] == 4);
  CHECK(static_cast<int8_t>(m[kCodec + hQp]) == -3 && m[kCodec + hQs] == 0 && static_cast<int8_t>(m[kCodec + hCqp]) == -2 && static_cast<int8_t>(m[kCodec + hCqp2]) == 5);
  CHECK(m[kCodec + hSliceGroups] == 0 && m[kCodec + hSliceGroupMap] == 0 && m[kCodec + hL0] == 3 && m[kCodec + hL1] == 1);
  bool lists = true;
  for (int l = 0; l < 6; ++l) for (int i = 0; i < 16; ++i) lists = lists && m[kCodec + hScaling4 + l * 16 + i] == l * 16 + i + 1;
  for (int l = 0; l < 2; ++l) for (int i = 0; i < 64; ++i) lists = lists && m[kCodec + hScaling8 + l * 64 + i] == 200 + l * 20 + i % 20;
  CHECK(lists);
  CHECK(word(m, kCodec + hFrameNum) == 9 && word(m, kCodec + hDecodedIdx) == 9 && static_cast<int32_t>(word(m, kCodec + hCurrFoc)) == -20 && static_cast<int32_t>(word(m, kCodec + hCurrFoc + 4)) == -19);
  CHECK(word(m, kCodec + hFrameNumList) == 7 && word(m, kCodec + hFrameNumList + 4) == 8 && word(m, kCodec + hFrameNumList + 8) == 2 && word(m, kCodec + hFrameNumList + 12) == 0);
  CHECK(static_cast<int32_t>(word(m, kCodec + hFocList)) == 14 && static_cast<int32_t>(word(m, kCodec + hFocList + 4)) == 15 && static_cast<int32_t>(word(m, kCodec + hFocList + 8)) == -4 &&
        static_cast<int32_t>(word(m, kCodec + hFocList + 20)) == -100000 && word(m, kCodec + hFocList + 24) == 0);
  // Everything the message does not name is zero, and nothing past the structure is touched.
  CHECK(word(m, dec(4)) == 0 && word(m, dec(8)) == 0 && word(m, dec(16)) == 0 && word(m, dec(36)) == 0 && word(m, dec(51)) == 0 && word(m, kCodec + 4 * 270) == 0);
  for (uint32_t i = message_bytes(); i < m.size(); ++i) CHECK(m[i] == 0xEE);
  (void)kH264Size;
  // Pictures and buffers the message cannot describe.
  Bytes big(message_bytes(), 0);
  amd_vcn::AvcPictureState bad = p;
  bad.width = 1930;  CHECK(!build_avc_decode_message(view(big), 1, 1, bad, b));
  bad = p; bad.height = 0;  CHECK(!build_avc_decode_message(view(big), 1, 1, bad, b));
  bad = p; bad.width = 4112;  CHECK(!build_avc_decode_message(view(big), 1, 1, bad, b));
  bad = p; bad.chroma_format = 2;  CHECK(!build_avc_decode_message(view(big), 1, 1, bad, b));
  bad = p; bad.reference_count = 17;  CHECK(!build_avc_decode_message(view(big), 1, 1, bad, b));
  DecodeBuffers bb = b;
  bb.dpb_bytes = 1ull << 32;  CHECK(!build_avc_decode_message(view(big), 1, 1, p, bb));
  bb = b; bb.context_bytes = 1ull << 32;  CHECK(!build_avc_decode_message(view(big), 1, 1, p, bb));
  bb = b; bb.bitstream_bytes = UINT32_MAX;  CHECK(!build_avc_decode_message(view(big), 1, 1, p, bb));
  bb = b; bb.target = amd_vcn::TargetLayout();  CHECK(!build_avc_decode_message(view(big), 1, 1, p, bb));
  Bytes small(message_bytes() - 1, 0xAB);
  CHECK(!build_avc_decode_message(view(small), 1, 1, p, b) && small[0] == 0xAB);
}

static void test_feedback_and_tables() {
  const amd_vcn::AvcPictureState p = sample_picture();
  Bytes buf(kMessageFeedbackItBytes + 16, 0xEE);
  CHECK(prepare_avc_feedback_and_tables(view(buf), p));
  CHECK(word(buf, kFeedbackOffset) == 2048);
  bool zeros = true;
  for (uint32_t i = kFeedbackOffset + 4; i < kItTableOffset; ++i) zeros = zeros && buf[i] == 0;
  CHECK(zeros);
  bool lists = true;
  for (int l = 0; l < 6; ++l) for (int i = 0; i < 16; ++i) lists = lists && buf[kItTableOffset + l * 16 + i] == l * 16 + i + 1;
  for (int l = 0; l < 2; ++l) for (int i = 0; i < 64; ++i) lists = lists && buf[kItTableOffset + 96 + l * 64 + i] == 200 + l * 20 + i % 20;
  CHECK(lists);
  for (uint32_t i = 0; i < kFeedbackOffset; ++i) CHECK(buf[i] == 0xEE);                 // the message page is not touched
  for (uint32_t i = kMessageFeedbackItBytes; i < buf.size(); ++i) CHECK(buf[i] == 0xEE);
  Bytes small(kMessageFeedbackItBytes - 1, 0xAB);
  CHECK(!prepare_avc_feedback_and_tables(view(small), p) && small[kFeedbackOffset] == 0xAB);
}

static void test_job_buffers() {
  JobAddresses a;
  a.session_context = 0x1'0000'0000ull; a.message = 0x1'0100'0000ull; a.dpb = 0x1'0200'0000ull; a.context = 0x1'0300'0000ull; a.bitstream = 0x1'0400'0000ull; a.target = 0x1'0500'0000ull;
  AmdUvd6Ring::Buffer out[kDecodeJobBuffers];
  CHECK(decode_job_buffers(a, ArrayView<AmdUvd6Ring::Buffer>(out)) == 8);
  const uint32_t commands[8] = {0x005, 0x000, 0x001, 0x206, 0x100, 0x002, 0x003, 0x204};
  const uint64_t addresses[8] = {a.session_context, a.message, a.dpb, a.context, a.bitstream, a.target, a.message + 0x1000, a.message + 0x1800};
  for (int i = 0; i < 8; ++i) CHECK(out[i].command == commands[i] && out[i].gpu_address == addresses[i]);
  AmdUvd6Ring::Buffer few[7];
  CHECK(decode_job_buffers(a, ArrayView<AmdUvd6Ring::Buffer>(few)) == 0);
  AmdUvd6Ring::Buffer two[2];
  CHECK(session_job_buffers(a, ArrayView<AmdUvd6Ring::Buffer>(two)) == 2 && two[0].command == 0x005 && two[0].gpu_address == a.session_context && two[1].command == 0 && two[1].gpu_address == a.message);
  CHECK(session_job_buffers(a, ArrayView<AmdUvd6Ring::Buffer>(few, 1)) == 0);
  // The ring can carry such a job: eight buffers and the engine start are 26 register writes, 52 dwords padded to 64.
  uint32_t ib[256];
  CHECK(AmdUvd6Ring::build_indirect_buffer(ArrayView<const AmdUvd6Ring::Buffer>(out), true, ArrayView<uint32_t>(ib)) == 64);
}

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

static bool first_picture(const std::vector<uint8_t>& stream, size_t* end) {
  size_t slices = 0;
  for (size_t i = 0; i + 4 < stream.size(); ++i) {
    if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1) {
      const int type = stream[i + 3] & 31;
      if (type == 1 || type == 5) {
        if (++slices == 2 && stream[i + 4] & 0x80) { *end = i; return true; }
      }
    }
  }
  return false;
}

static void test_from_a_stream() {
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
    const amd_vcn::AvcReference refs[2] = {{7, 14, 15}, {9, -4, -3}};
    amd_vcn::AvcPictureState state;
    CHECK(amd_vcn::make_avc_picture_state(*parser.parameter_sets(), *sps, *pps, first.header, 20, 21, ArrayView<const amd_vcn::AvcReference>(refs), &state));
    DecodeBuffers b;
    b.bitstream_bytes = 1000;
    const AvcBufferSizes sizes = avc_buffer_sizes(state.width, state.height, state.level, state.num_ref_frames);
    CHECK(sizes.dpb != 0);
    b.dpb_bytes = sizes.dpb;
    b.context_bytes = sizes.context;
    b.target = amd_vcn::target_layout(state.width, state.height);
    Bytes m(message_bytes(), 0xEE);
    CHECK(build_avc_decode_message(view(m), 5, 42, state, b));
    CHECK(word(m, dec(kDecWidth)) == uint32_t(sps->mb_w) * 16 && word(m, dec(kDecHeight)) == uint32_t(sps->mb_h) * 16);
    CHECK(word(m, kCodec + hLevel) == uint32_t(sps->level_idc) && word(m, kCodec + hFrameNum) == uint32_t(first.header.frame_num) && word(m, kCodec + hSps) == state.sps_flags);
    CHECK(word(m, kCodec + hFrameNumList) == 7 && word(m, kCodec + hFrameNumList + 4) == 9 && static_cast<int32_t>(word(m, kCodec + hCurrFoc)) == 20);
    // The same picture made into a create message and a feedback/table buffer.
    Bytes c(message_bytes(), 0);
    CHECK(build_create_message(view(c), 5, state.width, state.height, static_cast<uint32_t>(sizes.dpb)) && word(c, 40) == sizes.dpb);
    Bytes fb(kMessageFeedbackItBytes, 0);
    CHECK(prepare_avc_feedback_and_tables(view(fb), state) && word(fb, kFeedbackOffset) == 2048);
  }
  CHECK(alloc.live == 0);
}


// struct ruvd_h265, byte offsets within it, worked out by hand from the structure in ac_uvd_dec.h (two dwords of flags, then 4 + 4 * 6 single bytes, ... ).
constexpr uint32_t gSps = 0, gPps = 4, gChroma = 8, gBitDepthLuma = 9, gLog2Poc = 11, gMaxDecPic = 12, gLog2MinCb = 13, gLog2DiffCb = 14, gLog2MinTb = 15, gLog2DiffTb = 16, gDepthInter = 17,
                   gDepthIntra = 18, gNumStRps = 24, gNumLtSps = 25, gL0Default = 26, gL1Default = 27, gCbQp = 28, gDiffCuQp = 32, gTileCols = 33, gTileRows = 34, gMerge = 35,
                   gColumnWidth = 36, gRowHeight = 74, gInitQp = 116, gDeltaPocs = 117, gCurrIdx = 118, gCurrPoc = 120, gRefPicList = 124, gPocList = 140, gStBefore = 204, gStAfter = 212,
                   gLtCurr = 220, gDc16 = 228, gDc32 = 234, gDirectRefs = 244;
constexpr uint32_t kH265Size = 276;

static void test_hevc_sizes() {
  constexpr uint32_t kRuvdCodecH265 = 0x10;
  CHECK(kStreamTypeH265 == kRuvdCodecH265);
  // 64 x 64: 17 slots of 6144 bytes (64 * 64 * 3 / 2); the context is (19 * 19 * 16 * 17) + 52 KiB.
  HevcBufferSizes s = hevc_buffer_sizes(64, 64, 4);
  CHECK(s.references == 17 && s.dpb == 6144u * 17 && s.context == 19u * 19 * 16 * 17 + 52 * 1024);
  // 1080: the height is aligned to 16 for the slot (1088), the width to 16; 1920 * 1088 * 3 / 2 = 3133440, already a multiple of 256.
  s = hevc_buffer_sizes(1920, 1080, 6);
  CHECK(s.references == 17 && s.dpb == 3133440ull * 17 && s.context == (2175u / 16) * (1343u / 16) * 16 * 17 + 52 * 1024);
  // From 4096 x 2000 up only 8 slots.
  s = hevc_buffer_sizes(4096, 2304, 4);
  CHECK(s.references == 8 && s.dpb == 14155776ull * 8);
  // Refused: not whole 8-sample units, too small, too big, more pictures than slots.
  CHECK(hevc_buffer_sizes(63, 64, 4).dpb == 0 && hevc_buffer_sizes(64, 12, 4).dpb == 0 && hevc_buffer_sizes(4104, 64, 4).dpb == 0 && hevc_buffer_sizes(64, 2312, 4).dpb == 0);
  CHECK(hevc_buffer_sizes(64, 64, 17).dpb != 0 && hevc_buffer_sizes(64, 64, 18).dpb == 0 && hevc_buffer_sizes(4096, 2304, 9).dpb == 0);
}

// Takes every picture the CPU decoder offers, builds the message for it and checks the message against the picture description.
struct HevcRecorder final : h265::IOffload {
  int pictures = 0;
  int refs_seen = 0;
  bool accepts(const h265::Sps&, const h265::Pps&) override { return true; }
  bool decode_picture(const h265::OffloadPicture& p) override {
    const uint32_t k = static_cast<uint32_t>(pictures++);
    const h265::Sps& sps = *p.sps;
    const h265::Pps& pps = *p.pps;
    const HevcBufferSizes sizes = hevc_buffer_sizes(64, 64, 4);
    DecodeBuffers b;
    b.bitstream_bytes = 700;
    b.dpb_bytes = sizes.dpb;
    b.context_bytes = sizes.context;
    b.target = amd_vcn::target_layout(64, 64);
    Bytes m(message_bytes(), 0xCC);
    CHECK(build_hevc_decode_message(view(m), 9, k + 1, p, b));
    CHECK(word(m, 0) == 3556 && word(m, 4) == kMsgDecode && word(m, 8) == 9 && word(m, 12) == k + 1);
    CHECK(word(m, dec(kDecStreamType)) == 0x10 && word(m, dec(kDecFlags)) == 1 && word(m, dec(kDecWidth)) == 64 && word(m, dec(kDecHeight)) == 64);
    CHECK(word(m, dec(kDecDpbSize)) == sizes.dpb && word(m, dec(kDecDpbReserved)) == sizes.context && word(m, dec(kDecBsdSize)) == 768 && word(m, dec(kDecDbPitch)) == 64);
    CHECK(word(m, dec(kDecDtPitch)) == b.target.luma_pitch && word(m, dec(kDecChromaTop)) == b.target.chroma_offset && m[kExtensionSupport] == 1);
    const uint32_t g = kCodec;
    CHECK(((word(m, g + gSps) >> 10) & 1) == 1 && (word(m, g + gSps) & 1) == (sps.scaling_list_enabled ? 1u : 0u) && ((word(m, g + gSps) >> 1) & 1) == (sps.amp ? 1u : 0u));
    CHECK(((word(m, g + gPps) >> 2) & 1) == (pps.sign_data_hiding ? 1u : 0u) && ((word(m, g + gPps) >> 11) & 1) == (pps.tiles_enabled ? 1u : 0u));
    CHECK(m[g + gChroma] == 1 && m[g + gBitDepthLuma] == 0 && m[g + gBitDepthLuma + 1] == 0);
    CHECK(m[g + gLog2Poc] == sps.log2_max_poc_lsb - 4 && m[g + gLog2MinCb] == sps.log2_min_cb - 3 && m[g + gLog2DiffCb] == sps.log2_ctb - sps.log2_min_cb);
    CHECK(m[g + gLog2MinTb] == sps.log2_min_tb - 2 && m[g + gLog2DiffTb] == sps.log2_max_tb - sps.log2_min_tb);
    CHECK(m[g + gDepthInter] == sps.max_th_depth_inter && m[g + gDepthIntra] == sps.max_th_depth_intra && m[g + gNumStRps] == sps.num_st_rps && m[g + gNumLtSps] == sps.num_lt_sps);
    CHECK(m[g + gL0Default] == pps.num_ref_idx_default[0] - 1 && m[g + gL1Default] == pps.num_ref_idx_default[1] - 1);
    CHECK(static_cast<int8_t>(m[g + gCbQp]) == pps.cb_qp_offset && static_cast<int8_t>(m[g + gInitQp]) == pps.init_qp - 26 && m[g + gMerge] == pps.log2_parallel_merge_level - 2);
    CHECK(m[g + gDiffCuQp] == pps.diff_cu_qp_delta_depth && m[g + gTileCols] == 0 && m[g + gTileRows] == 0 && m[g + gDeltaPocs] == p.first->num_delta_pocs_ref_rps_idx);
    CHECK(m[g + gCurrIdx] == p.slot && static_cast<int32_t>(word(m, g + gCurrPoc)) == p.poc);
    for (uint32_t i = 0; i < 16; ++i) {
      const bool used = i < p.ref_count;
      CHECK(m[g + gRefPicList + i] == (used ? p.refs[i].slot : 0x7F));
      CHECK(static_cast<int32_t>(word(m, g + gPocList + 4 * i)) == (used ? p.refs[i].poc : 0));
    }
    for (uint32_t i = 0; i < 8; ++i) {
      CHECK(m[g + gStBefore + i] == (i < p.st_before_count ? p.st_before[i] : 0xFF));
      CHECK(m[g + gStAfter + i] == (i < p.st_after_count ? p.st_after[i] : 0xFF));
      CHECK(m[g + gLtCurr + i] == (i < p.lt_count ? p.lt_curr[i] : 0xFF));
    }
    for (uint32_t l = 0; l < 2; ++l)
      for (uint32_t i = 0; i < 15; ++i) CHECK(m[g + gDirectRefs + l * 15 + i] == p.list[l][i]);
    // The union's remaining bytes stay clear (the structure is cleared before it is filled), and so does everything behind the codec's structure.
    for (uint32_t i = kH265Size; i < 768 * 4; ++i) CHECK(m[g + i] == 0);
    refs_seen += static_cast<int>(p.ref_count);
    // A message too small for the structure is refused, and the picture's slice data is not part of it.
    Bytes small(message_bytes() - 1, 0);
    CHECK(!build_hevc_decode_message(view(small), 9, 1, p, b));
    // Feedback area and scaling table.
    Bytes fb(kMessageFeedbackItBytes, 0xEE);
    CHECK(prepare_hevc_feedback_and_tables(view(fb), sps, pps));
    CHECK(word(fb, kFeedbackOffset) == 2048 && word(fb, kFeedbackOffset + 4) == 0 && fb[kFeedbackOffset + kFeedbackBytes - 1] == 0);
    Bytes short_fb(kMessageFeedbackItBytes - 1, 0);
    CHECK(!prepare_hevc_feedback_and_tables(view(short_fb), sps, pps));
    return true;
  }
};

static void test_hevc_message() {
  TestAllocator alloc;
  {
    H265Decoder cpu(alloc);
    HevcRecorder recorder;
    cpu.set_offload(&recorder);
    const ConstByteView stream(kHevcSelfTestStream);
    for (uint32_t i = 0; i < kHevcSelfTestPictures; ++i) {
      const uint64_t start = i == 0 ? 0 : kHevcSelfTestEnds[i - 1];
      CHECK(cpu.decode(stream.subview(start, kHevcSelfTestEnds[i] - start), int64_t{i} * 1000));
    }
    cpu.flush();
    CHECK(recorder.pictures == int(kHevcSelfTestPictures));
    CHECK(recorder.refs_seen > 0);   // the stream has P and B pictures: some picture named references
  }
  CHECK(alloc.live == 0);
}

// struct ruvd_mpeg2, byte offsets within it, worked out by hand from the structure in ac_uvd_dec.h (three dwords of picture indexes, two flag bytes and two reserved,
// two 64-byte matrices, then single bytes).
constexpr uint32_t mDecoded = 0, mRef = 4, mLoadIntra = 12, mLoadNonIntra = 13, mIntra = 16, mNonIntra = 80, mProfile = 144, mChroma = 145, mType = 146, mFCode = 148, mDcPrecision = 152,
                   mStructure = 153, mTopFirst = 154, mFramePred = 155, mConcealment = 156, mQScale = 157, mVlcFormat = 158, mAlternate = 159;
constexpr uint32_t kMpeg2Size = 160;

static void test_mpeg2_sizes_and_jobs() {
  CHECK(kStreamTypeMpeg2 == 3 && kMpeg2References == 6);
  // 64 x 64: six pictures of 6144 bytes, aligned to 1024 (6144 already is).
  Mpeg2BufferSizes s = mpeg2_buffer_sizes(64, 64);
  CHECK(s.references == 6 && s.dpb == 6144u * 6);
  // 720 x 576: 720 * 576 * 3 / 2 = 622080, rounded up to a multiple of 1024 = 622592.
  s = mpeg2_buffer_sizes(720, 576);
  CHECK(s.dpb == 622592u * 6);
  CHECK(mpeg2_buffer_sizes(63, 64).dpb == 0 && mpeg2_buffer_sizes(64, 8).dpb == 0 && mpeg2_buffer_sizes(4112, 64).dpb == 0);
  // The job: session context, message, picture buffer, bitstream, target, feedback - no context buffer and no scaling table.
  JobAddresses a;
  a.session_context = 0x1000; a.message = 0x2000; a.dpb = 0x3000; a.context = 0x9999; a.bitstream = 0x4000; a.target = 0x5000;
  AmdUvd6Ring::Buffer out[kMpeg2JobBuffers];
  CHECK(mpeg2_job_buffers(a, ArrayView<AmdUvd6Ring::Buffer>(out)) == 6);
  CHECK(out[0].command == 0x005 && out[0].gpu_address == 0x1000 && out[1].command == 0x000 && out[1].gpu_address == 0x2000 && out[2].command == 0x001 && out[2].gpu_address == 0x3000);
  CHECK(out[3].command == 0x100 && out[3].gpu_address == 0x4000 && out[4].command == 0x002 && out[4].gpu_address == 0x5000 && out[5].command == 0x003 && out[5].gpu_address == 0x2000 + kFeedbackOffset);
  AmdUvd6Ring::Buffer few[5];
  CHECK(mpeg2_job_buffers(a, ArrayView<AmdUvd6Ring::Buffer>(few)) == 0);
}

struct Mpeg2Recorder final : mpeg2::IOffload {
  int pictures = 0;
  bool accepts(const mpeg2::SequenceHeader&) override { return true; }
  bool decode_picture(const mpeg2::OffloadPicture& p) override {
    const uint32_t k = static_cast<uint32_t>(pictures++);
    const mpeg2::PictureHeader& pic = *p.pic;
    const Mpeg2BufferSizes sizes = mpeg2_buffer_sizes(64, 64);
    DecodeBuffers b;
    b.bitstream_bytes = 300;
    b.dpb_bytes = sizes.dpb;
    b.context_bytes = 0;
    b.target = amd_vcn::target_layout(64, 64);
    Mpeg2Pictures jobs;
    jobs.current = k + 1;
    jobs.forward = k;        // the latest job (0 for the first picture: Mesa's fallback)
    jobs.backward = k > 1 ? k - 1 : k;
    Bytes m(message_bytes(), 0xCC);
    CHECK(build_mpeg2_decode_message(view(m), 4, k + 1, p, jobs, b));
    CHECK(word(m, 0) == 3556 && word(m, 4) == kMsgDecode && word(m, 8) == 4 && word(m, 12) == k + 1);
    CHECK(word(m, dec(kDecStreamType)) == 3 && word(m, dec(kDecWidth)) == 64 && word(m, dec(kDecHeight)) == 64 && word(m, dec(kDecDpbSize)) == sizes.dpb);
    CHECK(word(m, dec(kDecDpbReserved)) == 0 && word(m, dec(kDecBsdSize)) == 384 && word(m, dec(kDecDbPitch)) == 64 && m[kExtensionSupport] == 1);
    const uint32_t g = kCodec;
    CHECK(word(m, g + mDecoded) == k + 1 && word(m, g + mRef) == jobs.forward && word(m, g + mRef + 4) == jobs.backward);
    CHECK(m[g + mLoadIntra] == 1 && m[g + mLoadNonIntra] == 1);
    for (uint32_t i = 0; i < 64; ++i) {
      CHECK(m[g + mIntra + i] == p.intra_matrix_scan[i]);
      CHECK(m[g + mNonIntra + i] == p.non_intra_matrix_scan[i]);
    }
    CHECK(m[g + mProfile] == 0 && m[g + mChroma] == 1 && m[g + mType] == pic.type);
    for (uint32_t d = 0; d < 2; ++d)
      for (uint32_t c = 0; c < 2; ++c) CHECK(m[g + mFCode + d * 2 + c] == pic.f_code[d][c]);
    CHECK(m[g + mDcPrecision] == pic.intra_dc_precision && m[g + mStructure] == pic.structure && m[g + mTopFirst] == (pic.top_field_first ? 1 : 0));
    CHECK(m[g + mFramePred] == (pic.frame_pred_frame_dct ? 1 : 0) && m[g + mConcealment] == (pic.concealment_motion_vectors ? 1 : 0) && m[g + mQScale] == (pic.q_scale_type ? 1 : 0));
    CHECK(m[g + mVlcFormat] == (pic.intra_vlc_format ? 1 : 0) && m[g + mAlternate] == (pic.alternate_scan ? 1 : 0));
    for (uint32_t i = kMpeg2Size; i < 768 * 4; ++i) CHECK(m[g + i] == 0);
    // References: the last six jobs are in the engine's buffer, the seventh back and a job from the future are not.
    Mpeg2Pictures far = jobs;
    far.current = 10; far.forward = 4; far.backward = 9;
    CHECK(build_mpeg2_decode_message(view(m), 4, 10, p, far, b));
    far.forward = 3;
    CHECK(!build_mpeg2_decode_message(view(m), 4, 10, p, far, b));
    far.forward = 10;
    CHECK(!build_mpeg2_decode_message(view(m), 4, 10, p, far, b));
    far.forward = 9; far.current = 0;
    CHECK(!build_mpeg2_decode_message(view(m), 4, 10, p, far, b));
    Bytes small(message_bytes() - 1, 0);
    CHECK(!build_mpeg2_decode_message(view(small), 4, 1, p, jobs, b));
    Bytes fb(kMessageFeedbackItBytes, 0xEE);
    CHECK(prepare_mpeg2_feedback(view(fb)) && word(fb, kFeedbackOffset) == 2048 && word(fb, kFeedbackOffset + 4) == 0 && fb[kFeedbackOffset + kFeedbackBytes - 1] == 0);
    Bytes short_fb(kMessageFeedbackItBytes - 1, 0);
    CHECK(!prepare_mpeg2_feedback(view(short_fb)));
    return true;
  }
};

static void test_mpeg2_message() {
  TestAllocator alloc;
  {
    Mpeg2Decoder cpu(alloc);
    Mpeg2Recorder recorder;
    cpu.set_offload(&recorder);
    const ConstByteView stream(kMpeg2SelfTestStream);
    for (uint32_t i = 0; i < kMpeg2SelfTestPictures; ++i) {
      const uint64_t start = i == 0 ? 0 : kMpeg2SelfTestEnds[i - 1];
      CHECK(cpu.decode(stream.subview(start, kMpeg2SelfTestEnds[i] - start), int64_t{i} * 1000));
    }
    cpu.flush();
    CHECK(recorder.pictures == int(kMpeg2SelfTestPictures));
  }
  CHECK(alloc.live == 0);
}

// struct ruvd_vc1: six dwords (profile, level, sps flags, pps flags, picture structure, chroma format), 24 bytes, at the start of the codec union.
constexpr uint32_t vProfile = 0, vLevel = 4, vSps = 8, vPps = 12, vStructure = 16, vChroma = 20, kVc1Size = 24;

static void test_vc1_sizes_and_jobs() {
  CHECK(kStreamTypeVc1 == 1 && kVc1References == 5);
  // 64 x 64: five pictures of 6144 bytes, then 128 * 16 macroblocks + 64 * 4 + 128 * 4 + align(4 * 7 * 16, 64) = 2048 + 256 + 512 + 448.
  Vc1BufferSizes s = vc1_buffer_sizes(64, 64);
  CHECK(s.references == 5 && s.dpb == 6144u * 5 + 2048 + 256 + 512 + 448);
  CHECK(vc1_buffer_sizes(63, 64).dpb == 0 && vc1_buffer_sizes(64, 8).dpb == 0 && vc1_buffer_sizes(4112, 64).dpb == 0);
  JobAddresses a;
  a.session_context = 0x1000; a.message = 0x2000; a.dpb = 0x3000; a.bitstream = 0x4000; a.target = 0x5000;
  AmdUvd6Ring::Buffer out[kVc1JobBuffers];
  CHECK(vc1_job_buffers(a, ArrayView<AmdUvd6Ring::Buffer>(out)) == 6 && out[2].command == 0x001 && out[3].command == 0x100 && out[4].command == 0x002 && out[5].command == 0x003);
}

struct Vc1Recorder final : vc1::IOffload {
  int pictures = 0;
  int profile = -1;
  bool accepts(const vc1::SequenceHeader&) override { return true; }
  bool decode_picture(const vc1::OffloadPicture& p) override {
    const uint32_t k = static_cast<uint32_t>(pictures++);
    const vc1::SequenceHeader& seq = *p.seq;
    profile = seq.profile;
    const Vc1BufferSizes sizes = vc1_buffer_sizes(64, 64);
    DecodeBuffers b;
    b.bitstream_bytes = 300;
    b.dpb_bytes = sizes.dpb;
    b.context_bytes = 0;
    b.target = amd_vcn::target_layout(64, 64);
    Bytes m(message_bytes(), 0xCC);
    CHECK(build_vc1_decode_message(view(m), 6, k + 1, p, b));
    CHECK(word(m, 0) == 3556 && word(m, 4) == kMsgDecode && word(m, 8) == 6 && word(m, 12) == k + 1);
    const bool advanced = seq.profile == vc1::kAdvanced, simple = seq.profile == vc1::kSimple;
    CHECK(word(m, dec(kDecStreamType)) == 1 && word(m, dec(kDecDpbSize)) == sizes.dpb && word(m, dec(kDecDpbReserved)) == 0 && word(m, dec(kDecDbPitch)) == 64);
    // The size is in macroblocks for the Simple and Main profiles, in samples for the Advanced one.
    CHECK(word(m, dec(kDecWidth)) == (advanced ? 64u : 4u) && word(m, dec(kDecHeight)) == (advanced ? 64u : 4u));
    const uint32_t g = kCodec;
    CHECK(word(m, g + vProfile) == (advanced ? 2u : simple ? 0u : 1u) && word(m, g + vLevel) == (advanced ? 4u : simple ? 1u : 2u));
    auto bit = [](bool v, uint32_t n) { return v ? 1u << n : 0u; };
    CHECK(word(m, g + vSps) == (bit(seq.postprocflag, 7) | bit(seq.broadcast, 6) | bit(seq.interlace, 5) | bit(seq.tfcntrflag, 4) | bit(seq.finterpflag, 3) | bit(seq.psf, 1)));
    uint32_t pps = bit(seq.range_mapy_flag, 31) | bit(seq.range_mapuv_flag, 27) | bit(seq.multires, 21) | (uint32_t(seq.max_b_frames & 7) << 16) | bit(seq.overlap, 11) |
                   (uint32_t(seq.quantizer_mode & 3) << 9) | bit(seq.panscanflag, 7) | bit(seq.refdist_flag, 6) | bit(seq.vstransform, 0);
    if (!simple) pps |= bit(seq.resync_marker, 20) | bit(seq.rangered, 19) | bit(seq.loop_filter, 5) | bit(seq.fastuvmc, 4) | bit(seq.extended_mv, 3) | bit(seq.extended_dmv, 8) | (uint32_t(seq.dquant & 3) << 1);
    CHECK(word(m, g + vPps) == pps && word(m, g + vStructure) == 0 && word(m, g + vChroma) == 1 && m[kExtensionSupport] == 1);
    for (uint32_t i = kVc1Size; i < 768 * 4; ++i) CHECK(m[g + i] == 0);
    // An interlaced sequence is not taken, nor a message that does not fit.
    vc1::SequenceHeader interlaced = seq;
    interlaced.interlace = true;
    vc1::OffloadPicture copy = p;
    copy.seq = &interlaced;
    CHECK(!build_vc1_decode_message(view(m), 6, 1, copy, b));
    Bytes small(message_bytes() - 1, 0);
    CHECK(!build_vc1_decode_message(view(small), 6, 1, p, b));
    Bytes fb(kMessageFeedbackItBytes, 0xEE);
    CHECK(prepare_vc1_feedback(view(fb)) && word(fb, kFeedbackOffset) == 2048 && word(fb, kFeedbackOffset + 4) == 0);
    return true;
  }
};

static void test_vc1_message() {
  TestAllocator alloc;
  {
    // The Main profile stream (its sequence header is the codec configuration), then the Advanced profile one.
    Vc1Decoder main_cpu(alloc);
    Vc1Recorder main_rec;
    main_cpu.set_offload(&main_rec);
    CHECK(main_cpu.configure(ConstByteView(kVc1SelfTestMainConfig, 4), kVc1SelfTestSize, kVc1SelfTestSize));
    const ConstByteView main_stream(kVc1SelfTestMainStream);
    for (uint32_t i = 0; i < kVc1SelfTestPictures; ++i) {
      const uint64_t start = i == 0 ? 0 : kVc1SelfTestMainEnds[i - 1];
      CHECK(main_cpu.decode(main_stream.subview(start, kVc1SelfTestMainEnds[i] - start), int64_t{i} * 1000));
    }
    main_cpu.flush();
    CHECK(main_rec.pictures > 0 && main_rec.profile == vc1::kMain);
    Vc1Decoder adv_cpu(alloc);
    Vc1Recorder adv_rec;
    adv_cpu.set_offload(&adv_rec);
    const ConstByteView adv_stream(kVc1SelfTestAdvancedStream);
    for (uint32_t i = 0; i < kVc1SelfTestPictures; ++i) {
      const uint64_t start = i == 0 ? 0 : kVc1SelfTestAdvancedEnds[i - 1];
      CHECK(adv_cpu.decode(adv_stream.subview(start, kVc1SelfTestAdvancedEnds[i] - start), int64_t{i} * 1000));
    }
    adv_cpu.flush();
    CHECK(adv_rec.pictures > 0 && adv_rec.profile == vc1::kAdvanced);
  }
  CHECK(alloc.live == 0);
}

// struct ruvd_mpeg4 at the start of the codec union, worked out by hand: decoded_pic_idx 0, ref_pic_idx[2] 4, variant_type 12, profile_and_level 16, verid 17, shape 18, reserved 19,
// width 20, height 22, time increment resolution 24, reserved 26, flags 28, quant_type 32, reserved 33, intra matrix 36, non-intra matrix 100 (64 bytes each), sprite config 164.
static void test_mpeg4() {
  CHECK(kStreamTypeMpeg4 == 4);
  // 32 x 32 is under the 30 MiB the buffer is not allowed to be smaller than. 3840 x 2160: image 3840 * 2160 * 3 / 2 = 12441600 (a multiple of 1024), three of them, then
  // 240 * 136 macroblocks (the height rounded to an even count) * 64 and * 32.
  Mpeg4BufferSizes sizes = mpeg4_buffer_sizes(32, 32);
  CHECK(sizes.references == 3 && sizes.dpb == 30u << 20);
  sizes = mpeg4_buffer_sizes(3840, 2160);
  CHECK(sizes.dpb == 12441600ull * 3 + 240 * 136 * 64 + 240 * 136 * 32);
  // A layer that is not whole macroblocks is taken at the next one (the picture buffer is), a zero or a huge one is not.
  CHECK(mpeg4_buffer_sizes(33, 17).dpb == 30u << 20 && mpeg4_buffer_sizes(0, 32).dpb == 0 && mpeg4_buffer_sizes(32, 0).dpb == 0 && mpeg4_buffer_sizes(16400, 32).dpb == 0);
  JobAddresses a;
  a.session_context = 0x1000; a.message = 0x2000; a.dpb = 0x3000; a.context = 0x9999; a.bitstream = 0x4000; a.target = 0x5000;
  AmdUvd6Ring::Buffer out[kMpeg4JobBuffers];
  CHECK(mpeg4_job_buffers(a, ArrayView<AmdUvd6Ring::Buffer>(out)) == 6 && out[2].command == 0x001 && out[3].command == 0x100 && out[5].command == 0x003);   // (as MPEG-2's)

  mpeg4::Vol vol;
  vol.width = 33;
  vol.height = 17;
  vol.time_increment_resolution = 30000;
  vol.interlaced = true;
  vol.quarter_sample = true;
  vol.quant_type_mpeg = true;
  vol.data_partitioned = true;
  vol.is_object_layer_identifier = true;
  for (uint32_t i = 0; i < 64; ++i) {
    vol.intra_matrix[i] = static_cast<uint8_t>(10 + i);
    vol.non_intra_matrix[i] = static_cast<uint8_t>(100 + i);
  }
  DecodeBuffers b;
  b.bitstream_bytes = 300;
  b.dpb_bytes = 30u << 20;
  b.target = amd_vcn::target_layout(48, 32);
  Mpeg2Pictures jobs;
  jobs.current = 9;
  jobs.forward = 4;
  jobs.backward = 8;
  Bytes m(message_bytes(), 0xCC);
  CHECK(build_mpeg4_decode_message(view(m), 6, 9, vol, jobs, b));
  CHECK(word(m, 0) == 3556 && word(m, 4) == kMsgDecode && word(m, 8) == 6 && word(m, 12) == 9);
  // The decode message gets the macroblock-aligned size, the codec's structure the layer's own.
  CHECK(word(m, dec(kDecStreamType)) == 4 && word(m, dec(kDecWidth)) == 48 && word(m, dec(kDecHeight)) == 32 && word(m, dec(kDecDpbSize)) == 30u << 20 && word(m, dec(kDecDpbReserved)) == 0);
  const uint32_t c = kCodec;
  CHECK(word(m, c + 0) == 9 && word(m, c + 4) == 4 && word(m, c + 8) == 8 && word(m, c + 12) == 0);
  CHECK(m[c + 16] == 0xF0 && m[c + 17] == 5 && m[c + 18] == 0 && (m[c + 20] | (m[c + 21] << 8)) == 33 && (m[c + 22] | (m[c + 23] << 8)) == 17 && (m[c + 24] | (m[c + 25] << 8)) == 30000);
  // Flags: interlaced (2), load matrices (3, 4), quarter sample (5), complexity estimation disabled (6), data partitioning (8), layer identifier (13); not the short header (0), not
  // OBMC disable (1), not the resync marker disable (7), not reversible VLC (9), not scalability (12), not fixed rate (14).
  CHECK(word(m, c + 28) == ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8) | (1u << 13)));
  CHECK(m[c + 32] == 1 && m[c + 36] == 10 && m[c + 36 + 63] == 73 && m[c + 100] == 100 && m[c + 100 + 63] == 163);
  // The sprite configuration stays zero.
  bool zero = true;
  for (uint32_t i = 0; i < 16; ++i) zero = zero && m[c + 164 + i] == 0;
  CHECK(zero);
  // References: none newer than the picture, none more than six jobs back.
  Bytes bad(message_bytes(), 0);
  jobs.forward = 9;
  CHECK(!build_mpeg4_decode_message(view(bad), 6, 9, vol, jobs, b));
  jobs.forward = 2;
  CHECK(!build_mpeg4_decode_message(view(bad), 6, 9, vol, jobs, b));
  jobs.forward = 3;
  CHECK(build_mpeg4_decode_message(view(bad), 6, 9, vol, jobs, b));
  jobs.forward = 0; jobs.backward = 0; jobs.current = 1;
  CHECK(build_mpeg4_decode_message(view(bad), 6, 1, vol, jobs, b));   // (an intra plane at the start names job 0, the latest)
  jobs.current = 0;
  CHECK(!build_mpeg4_decode_message(view(bad), 6, 1, vol, jobs, b));
  Bytes small(100, 0);
  jobs.current = 9; jobs.forward = 4;
  CHECK(!build_mpeg4_decode_message(view(small), 6, 9, vol, jobs, b));
  mpeg4::Vol empty;
  CHECK(!build_mpeg4_decode_message(view(bad), 6, 9, empty, jobs, b));
  Bytes feedback(kMessageFeedbackItBytes, 0xCC);
  CHECK(prepare_mpeg4_feedback(view(feedback)) && word(feedback, kFeedbackOffset) == kFeedbackBytes);
}

int main() {
  test_mpeg4();
  test_vc1_sizes_and_jobs();
  test_vc1_message();
  test_mpeg2_sizes_and_jobs();
  test_mpeg2_message();
  test_hevc_sizes();
  test_hevc_message();
  test_layout();
  test_create_destroy();
  test_buffer_sizes();
  test_decode_message();
  test_feedback_and_tables();
  test_job_buffers();
  test_from_a_stream();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd uvd6 message tests passed\n");
  return 0;
}
