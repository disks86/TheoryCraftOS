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

// The VDEnc H.265 encoder (IntelHevcVdencEncoder) against a simulated Xe-LP GPU. The simulated engine reads the command batch the way the hardware would: it checks the order and
// length of every command and the fields that matter (the HCP and VDEnc modes, the surfaces, the addresses with their sizes, the picture and slice state, the headers to insert),
// then "encodes" by writing what a valid stream looks like: the headers it was told to insert, then CABAC slice data in which every coding tree block is one 64x64 coding unit
// (a flat intra block without residual in I pictures, a skipped one in B pictures). The CPU decoder decodes the result: the pictures it shows are flat grey and the stream must
// parse, which proves the parameter sets, the slice headers and the picture order the session wrote. The engine also reads the source surface back and the test compares it
// with the input, which proves the tiling.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"
#include "Hardware/VideoCodec/Intel/IntelHevcEncodeCommands.hpp"
#include "Hardware/VideoCodec/Intel/IntelHevcVdencEncoder.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaEngineHw.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/Intel/IntelTiling.hpp"
#include "Hardware/VideoCodec/Software/H265Cabac.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"

using namespace hardware;
using namespace hardware::intel_mfx;

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

// Header words of the commands (the upper 16 bits of dword 0).
constexpr uint32_t kHMfxWait = 0x68000000, kHVdencControl = 0x708B0000;
constexpr uint32_t kHHcpPipeMode = 0x73800000, kHHcpSurface = 0x73810000, kHHcpPipeBuf = 0x73820000, kHHcpIndObj = 0x73830000, kHHcpQm = 0x73840000, kHHcpFqm = 0x73850000,
                   kHHcpPicState = 0x73900000, kHHcpRefIdx = 0x73920000, kHHcpSliceState = 0x73940000, kHHcpPakInsert = 0x73A20000;
constexpr uint32_t kHVdencPipeMode = 0x70800000, kHVdencSrc = 0x70810000, kHVdencRef = 0x70820000, kHVdencDs = 0x70830000, kHVdencPipeBuf = 0x70840000, kHVdencWalker = 0x70870000,
                   kHVdencWeights = 0x70880000, kHVdencCmd2 = 0x70890000, kHVdencCmd1 = 0x708A0000, kHVdFlush = 0x77800000;

class Bits {
 public:
  void put(uint32_t v, uint32_t n) { for (uint32_t i = n; i > 0; --i) bit((v >> (i - 1)) & 1u); }
  void align_zero() { while (fill_) bit(0); }
  void bit(uint32_t b) {
    cur_ = static_cast<uint8_t>((cur_ << 1) | b);
    if (++fill_ == 8) { bytes.push_back(cur_); cur_ = 0; fill_ = 0; }
  }
  std::vector<uint8_t> bytes;
 private:
  uint8_t cur_ = 0;
  uint32_t fill_ = 0;
};

// The CABAC arithmetic encoder of H.265 (9.3.4.2: the encoder of the standard's informative description), with the decoder's probability tables and context numbering.
class CabacEncoder {
 public:
  CabacEncoder(Bits* out, int init_type, int slice_qp) : out_(out) {
    static const uint8_t kInit[3][h265::kContextCount] =
#include "Hardware/VideoCodec/Software/H265CabacInit.inc"
        ;
    const int qp = slice_qp < 0 ? 0 : slice_qp > 51 ? 51 : slice_qp;
    for (int i = 0; i < h265::kContextCount; ++i) {
      const int v = kInit[init_type][i], slope = v >> 4, offset = v & 15;
      const int m = slope * 5 - 45, n = (offset << 3) - 16;
      int pre = ((m * qp) >> 4) + n;
      pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
      state_[i] = pre <= 63 ? static_cast<uint8_t>((63 - pre) << 1) : static_cast<uint8_t>(((pre - 64) << 1) | 1);
    }
  }
  void decision(int ctx, int bin) {
    int st = state_[ctx] >> 1, mps = state_[ctx] & 1;
    const uint32_t lps = h265::kCabacRangeLps[st * 4 + ((range_ >> 6) & 3)];
    range_ -= lps;
    if (bin != mps) {
      low_ += range_;
      range_ = lps;
      if (st == 0) mps = 1 - mps;
      st = h265::kCabacStateNext[st * 2];
    } else {
      st = h265::kCabacStateNext[st * 2 + 1];
    }
    state_[ctx] = static_cast<uint8_t>((st << 1) | mps);
    renorm();
  }
  void bypass(int bin) {
    low_ <<= 1;
    if (bin) low_ += range_;
    if (low_ >= 1024) { put_bit(1); low_ -= 1024; }
    else if (low_ < 512) put_bit(0);
    else { low_ -= 512; ++outstanding_; }
  }
  // end_of_slice_segment_flag. A one flushes the coder; its last bit is the rbsp_stop_one_bit.
  void terminate(int bin) {
    range_ -= 2;
    if (bin) {
      low_ += range_;
      range_ = 2;
      renorm();
      put_bit((low_ >> 9) & 1);
      out_->put(((low_ >> 7) & 3) | 1, 2);
    } else {
      renorm();
    }
  }

 private:
  void put_bit(int b) {
    if (first_) first_ = false; else out_->bit(b);
    for (; outstanding_ > 0; --outstanding_) out_->bit(1 - b);
  }
  void renorm() {
    while (range_ < 256) {
      if (low_ < 256) put_bit(0);
      else if (low_ >= 512) { low_ -= 512; put_bit(1); }
      else { low_ -= 256; ++outstanding_; }
      range_ <<= 1;
      low_ <<= 1;
    }
  }
  Bits* out_;
  uint32_t low_ = 0, range_ = 510;
  int outstanding_ = 0;
  bool first_ = true;
  uint8_t state_[h265::kContextCount] = {};
};

// The slice segment data of a picture of whole 64x64 blocks: flat intra blocks without residual (I), or skipped blocks that merge the zero motion vector (B).
void code_slice_data(Bits* out, bool b_slice, int slice_qp, uint32_t ctbs_x, uint32_t ctbs_y) {
  CabacEncoder cabac(out, b_slice ? 2 : 0, slice_qp);
  const uint32_t count = ctbs_x * ctbs_y;
  for (uint32_t ctb = 0; ctb < count; ++ctb) {
    const uint32_t cx = ctb % ctbs_x, cy = ctb / ctbs_x;
    cabac.decision(h265::kCtxSplitCu, 0);                          // split_cu_flag (neighbours are 64x64 too: context 0)
    if (b_slice) {
      cabac.decision(h265::kCtxSkip + (cx > 0 ? 1 : 0) + (cy > 0 ? 1 : 0), 1);   // cu_skip_flag (neighbours are skipped)
      cabac.decision(h265::kCtxMergeIdx, 0);                       // merge_idx 0
    } else {
      cabac.decision(h265::kCtxPrevIntraLuma, 1);                  // prev_intra_luma_pred_flag
      cabac.bypass(0);                                             // mpm_idx 0
      cabac.decision(h265::kCtxChromaPred, 0);                     // intra_chroma_pred_mode 4
      cabac.decision(h265::kCtxCbfChroma + 0, 0);                  // transform tree (64x64 splits to 32x32 by itself): cbf_cb, cbf_cr
      cabac.decision(h265::kCtxCbfChroma + 0, 0);
      for (int k = 0; k < 4; ++k) {
        cabac.decision(h265::kCtxSplitTransform + 0, 0);           // split_transform_flag of the 32x32 block
        cabac.decision(h265::kCtxCbfLuma + 0, 0);                  // cbf_luma
      }
    }
    cabac.terminate(ctb + 1 == count ? 1 : 0);                     // end_of_slice_segment_flag
  }
  out->align_zero();
}

struct FakeEncoder {
  int pictures = 0, problems = 0, keyframes = 0;
  bool write_stream = true;                       // the picture is whole 64x64 blocks: write real slice data
  std::vector<uint32_t> sizes;
  std::vector<uint8_t> last_luma, last_chroma;    // the source surface as the engine read it (visible part)
  uint32_t last_width = 0, last_height = 0;
  uint32_t previous_recon = 0, previous_ds4 = 0, previous_ds8 = 0, previous_mv = 0;
  void problem(const char* what) { printf("    encoder model: %s\n", what); ++problems; }

  void run(FakeGpu& gpu, const std::vector<uint32_t>& d) {
    size_t i = 0;
    auto expect = [&](uint32_t header, uint32_t length, const char* name) -> const uint32_t* {
      while (i < d.size() && d[i] == 0) ++i;   // (no-ops)
      if (i + length > d.size() || (d[i] & 0xFFFF0000u) != header || (d[i] & 0xFFFu) + 2 != length) {
        char msg[96];
        snprintf(msg, sizeof msg, "expected %s (%u dwords) at dword %zu", name, length, i);
        problem(msg);
        return nullptr;
      }
      const uint32_t* p = &d[i];
      i += length;
      return p;
    };
    auto field = [](uint32_t v, uint32_t lo, uint32_t hi) { return (v >> lo) & (hi - lo >= 31 ? 0xFFFFFFFFu : ((1u << (hi - lo + 1)) - 1u)); };
    auto expect_wait = [&](const char* name) { if (i < d.size() && d[i] == (kHMfxWait | 0x100u)) ++i; else problem(name); };
    while (i < d.size() && (d[i] == 0 || (d[i] >> 23) == 0x26)) i += d[i] == 0 ? 1 : 4;   // the leading flush

    // --- VDEnc and HCP initialisation, the HCP mode.
    const uint32_t* control = expect(kHVdencControl, 2, "VDENC_CONTROL_STATE");
    if (!control || field(control[1], 1, 1) != 1) problem("VDEnc initialisation");
    if (i + 3 <= d.size() && d[i] == 0x738A0001u && d[i + 1] == 1 && d[i + 2] == 0) i += 3; else problem("MI_VD_CONTROL_STATE for the HCP");
    expect_wait("MFX_WAIT before the HCP mode");
    const uint32_t* mode = expect(kHHcpPipeMode, 7, "HCP_PIPE_MODE_SELECT");
    if (!mode) return;
    expect_wait("MFX_WAIT after the HCP mode");
    if (field(mode[1], 0, 0) != 1 || field(mode[1], 5, 7) != 0 || field(mode[1], 10, 10) != 1 || field(mode[1], 2, 2) != 1 || field(mode[1], 11, 11) != 0 || field(mode[1], 15, 16) != 0 ||
        field(mode[1], 13, 14) != 0)
      problem("HCP mode: HEVC, encode, VDEnc, legacy pipe, no RDOQ");

    // --- Surfaces: source (1), reconstruction (0), reference (5).
    const uint32_t* s_src = expect(kHHcpSurface, 5, "source surface");
    const uint32_t* s_rec = expect(kHHcpSurface, 5, "recon surface");
    const uint32_t* s_ref = expect(kHHcpSurface, 5, "reference surface");
    if (!s_src || !s_rec || !s_ref) return;
    if (field(s_src[1], 28, 31) != 1 || field(s_rec[1], 28, 31) != 0 || field(s_ref[1], 28, 31) != 5) problem("surface ids");
    if (field(s_src[2], 27, 31) != 4 || field(s_rec[2], 27, 31) != 4 || field(s_ref[2], 27, 31) != 4) problem("surface format NV12");
    const uint32_t pitch = field(s_src[1], 0, 16) + 1, chroma_rows = field(s_src[2], 0, 14);
    if (field(s_rec[1], 0, 16) + 1 != pitch || field(s_ref[1], 0, 16) + 1 != pitch || field(s_rec[2], 0, 14) != chroma_rows || field(s_src[3], 16, 31) != chroma_rows) problem("surface layout");

    const uint32_t* buf = expect(kHHcpPipeBuf, 121, "HCP_PIPE_BUF_ADDR_STATE");
    const uint32_t* ind = expect(kHHcpIndObj, 29, "HCP_IND_OBJ_BASE_ADDR_STATE");
    if (!buf || !ind) return;
    for (int k = 0; k < 8; ++k) expect(kHHcpFqm, 34, "HCP_FQM_STATE");
    for (int k = 0; k < 20; ++k) expect(kHHcpQm, 18, "HCP_QM_STATE");

    // --- The VDEnc pipeline.
    const uint32_t* vmode = expect(kHVdencPipeMode, 6, "VDENC_PIPE_MODE_SELECT");
    const uint32_t* vsrc = expect(kHVdencSrc, 6, "VDENC_SRC_SURFACE_STATE");
    const uint32_t* vref = expect(kHVdencRef, 6, "VDENC_REF_SURFACE_STATE");
    const uint32_t* vds = expect(kHVdencDs, 10, "VDENC_DS_REF_SURFACE_STATE");
    const uint32_t* vbuf = expect(kHVdencPipeBuf, 71, "VDENC_PIPE_BUF_ADDR_STATE");
    const uint32_t* cmd1 = expect(kHVdencCmd1, 31, "VDENC_CMD1");
    const uint32_t* pic = expect(kHHcpPicState, 41, "HCP_PIC_STATE");
    const uint32_t* cmd2 = expect(kHVdencCmd2, 51, "VDENC_CMD2");
    if (!vmode || !vsrc || !vref || !vds || !vbuf || !cmd1 || !pic || !cmd2) return;
    if (field(vmode[1], 0, 3) != 0 || field(vmode[1], 7, 7) != 1 || field(vmode[1], 12, 14) != 0 || field(vmode[1], 15, 16) != 1 || field(vmode[1], 18, 18) != 0 || field(vmode[3], 24, 27) != 4 ||
        field(vmode[3], 16, 19) != 1)
      problem("VDEnc mode: HEVC, TLB prefetch, 8-bit 4:2:0, low delay");

    // --- Picture geometry and type, from the three places that carry them.
    const uint32_t width = field(cmd2[1], 0, 15) + 1, height = field(cmd2[1], 16, 31) + 1;
    if ((pic[1] & 0x7FF) + 1 != width / 8 || field(pic[1], 16, 26) + 1 != height / 8 || (width & 7) || (height & 7)) problem("picture size");
    if (field(vsrc[2], 4, 17) + 1 != width || field(vsrc[2], 18, 31) + 1 != height) problem("VDEnc source size");
    if (field(pic[2], 2, 3) != 3 || field(pic[2], 0, 1) != 0 || field(pic[2], 6, 7) != 3 || field(pic[2], 4, 5) != 0 || field(pic[2], 29, 31) != 1) problem("coding block sizes");
    if (field(pic[4], 5, 5) != 1 || field(pic[4], 3, 3) != 0 || field(pic[4], 4, 4) != 0 || field(pic[4], 23, 23) != 0 || field(pic[4], 17, 17) != 0 || field(pic[4], 22, 22) != 0)
      problem("coding tools: cu_qp_delta on; SAO, PCM, AMP, tiles, transform skip off");
    if (field(pic[19], 16, 16) != 1 || field(pic[19], 25, 25) != 0 || field(pic[19], 31, 31) != 1) problem("HCP picture flags");
    const bool b_picture = field(cmd2[2], 20, 21) == 3;
    if (!b_picture && field(cmd2[2], 20, 21) != 0) problem("VDEnc picture type");
    const uint32_t qp = field(cmd2[27], 8, 15);
    if (qp < 10 || qp > 51 || field(cmd2[16], 0, 7) != 10 || field(cmd2[16], 8, 15) != 51) problem("VDEnc quantiser");

    // --- Slice level.
    if (b_picture) {
      const uint32_t* l0 = expect(kHHcpRefIdx, 18, "HCP_REF_IDX_STATE list 0");
      const uint32_t* l1 = expect(kHHcpRefIdx, 18, "HCP_REF_IDX_STATE list 1");
      if (l0 && l1) {
        if (field(l0[1], 0, 0) != 0 || field(l1[1], 0, 0) != 1 || field(l0[1], 1, 4) != 0 || field(l1[1], 1, 4) != 0) problem("reference list numbers");
        if (field(l0[2], 8, 10) != 0 || field(l0[2], 0, 7) != 1 || l0[2] != l1[2]) problem("reference list entries");
        if (field(cmd2[3], 0, 7) != 1 || field(cmd2[3], 8, 15) != 1) problem("reference POC distances");
      }
    }
    const uint32_t* slice = expect(kHHcpSliceState, 13, "HCP_SLICE_STATE");
    if (!slice) return;
    if (field(slice[3], 0, 1) != (b_picture ? 0u : 2u) || field(slice[3], 6, 11) != qp || field(slice[3], 2, 2) != 1 || field(slice[1], 0, 25) != 0 || field(slice[2], 0, 26) != 0) problem("slice state");
    if (field(slice[4], 0, 0) != 0 || field(slice[4], 14, 14) != 1 || field(slice[7], 2, 2) != 1 || field(slice[7], 9, 10) != 3 || field(slice[7], 1, 1) != 0 || field(slice[7], 8, 8) != 0)
      problem("slice flags");

    struct Insert { std::vector<uint8_t> bytes; uint32_t bits; bool emulation; uint32_t skip; bool last; };
    std::vector<Insert> inserts;
    while (i < d.size() && (d[i] & 0xFFFF0000u) == kHHcpPakInsert) {
      const uint32_t len = (d[i] & 0xFFF) + 2;
      if (len < 3 || i + len > d.size()) { problem("PAK insert length"); return; }
      Insert in;
      const uint32_t data_dwords = len - 2, last_bits = field(d[i + 1], 8, 13);
      in.bits = (data_dwords - 1) * 32 + last_bits;
      in.emulation = field(d[i + 1], 3, 3) != 0;
      in.skip = field(d[i + 1], 4, 7);
      in.last = field(d[i + 1], 2, 2) != 0;
      for (uint32_t k = 0; k < (in.bits + 7) / 8; ++k) in.bytes.push_back(static_cast<uint8_t>(d[i + 2 + k / 4] >> (8 * (k % 4))));
      inserts.push_back(in);
      i += len;
    }
    if (inserts.size() != (b_picture ? 1u : 4u)) { problem("number of header inserts"); return; }
    if (!inserts.back().last || inserts.back().skip != 6 || !inserts.back().emulation) problem("slice header insert");
    for (size_t k = 0; k + 1 < inserts.size(); ++k) {
      if (inserts[k].last || inserts[k].emulation) problem("parameter set insert");
      if (inserts[k].bytes.size() < 7 || inserts[k].bytes[4] != static_cast<uint8_t>((32 + k) << 1)) problem("parameter set NAL unit type");
    }

    const uint32_t* weights = expect(kHVdencWeights, 3, "VDENC_WEIGHTSOFFSETS_STATE");
    const uint32_t* walker = expect(kHVdencWalker, 27, "VDENC_WALKER_STATE");
    const uint32_t* flush = expect(kHVdFlush, 2, "VD_PIPELINE_FLUSH");
    if (!weights || !walker || !flush) return;
    const uint32_t ctbs_x = (width + 63) / 64, ctbs_y = (height + 63) / 64;
    if (field(walker[2], 16, 25) != ctbs_x || field(walker[2], 0, 9) != ctbs_y || field(walker[5], 0, 15) + 1 != width || field(walker[5], 16, 31) + 1 != height || field(walker[1], 0, 8) != 0 ||
        field(walker[1], 16, 24) != 0)
      problem("walker extent");
    if (field(flush[1], 1, 1) != 1 || field(flush[1], 17, 17) != 1 || field(flush[1], 3, 3) != 1) problem("pipeline flush");
    while (i < d.size() && d[i] == 0) ++i;
    if (i + 4 > d.size() || (d[i] >> 23) != 0x26) { problem("flush after the slice"); return; }
    i += 4;
    const uint32_t* flush2 = expect(kHVdFlush, 2, "VD_PIPELINE_FLUSH for the HCP");
    if (!flush2 || field(flush2[1], 0, 0) != 1 || field(flush2[1], 16, 16) != 1) problem("HCP pipeline flush");
    while (i < d.size() && d[i] == 0) ++i;
    if (i + 4 > d.size() || (d[i] >> 23) != 0x26) { problem("flush before the status store"); return; }
    i += 4;
    uint32_t status_address = 0;
    if (i + 4 <= d.size() && (d[i] >> 23) == 0x24 && d[i + 1] == 0x1C28A0u) { status_address = d[i + 2]; i += 4; } else { problem("MI_STORE_REGISTER_MEM of the bitstream byte count"); return; }
    while (i < d.size() && d[i] == 0) ++i;
    if (i >= d.size() || (d[i] >> 23) != 0x0A) problem("batch end");

    // --- Every address the commands name must be mapped, with the size the engine uses.
    intel_enc::HevcEncodeLayout layout;
    if (!intel_enc::hevc_encode_layout(width, height, &layout)) { problem("layout"); return; }
    const intel_enc::HevcScratchPlan plan = intel_enc::hevc_scratch_plan(layout);
    if (layout.full.pitch != pitch || layout.full.chroma_offset_rows != chroma_rows) problem("surface pitch / chroma offset");
    auto mapped = [&](uint32_t address, uint64_t bytes, const char* what) {
      if (!address) { problem(what); return; }
      bool ok;
      if (!gpu.ppgtt_bytes(address, 1, what, &ok) || !ok || !gpu.ppgtt_bytes(address + bytes - 1, 1, what, &ok) || !ok) problem(what);
    };
    const uint32_t recon = buf[1], source = buf[54];
    mapped(recon, layout.full.size, "recon surface not mapped");
    mapped(source, layout.full.size, "source surface not mapped");
    if (source != vbuf[10]) problem("the two pipes read different sources");
    mapped(buf[4], plan.deblocking_tile_line - plan.deblocking_line, "deblocking line");
    mapped(buf[7], plan.deblocking_tile_column - plan.deblocking_tile_line, "deblocking tile line");
    mapped(buf[10], plan.metadata_line - plan.deblocking_tile_column, "deblocking tile column");
    mapped(buf[13], plan.metadata_tile_line - plan.metadata_line, "metadata line");
    mapped(buf[16], plan.metadata_tile_column - plan.metadata_tile_line, "metadata tile line");
    mapped(buf[19], plan.sao_line - plan.metadata_tile_column, "metadata tile column");
    mapped(buf[22], plan.sao_tile_line - plan.sao_line, "SAO line");
    mapped(buf[25], plan.sao_tile_column - plan.sao_tile_line, "SAO tile line");
    mapped(buf[28], plan.sao_row_store - plan.sao_tile_column, "SAO tile column");
    mapped(buf[31], plan.mv_temporal_bytes, "current motion vector temporal buffer");
    mapped(buf[57], plan.vdenc_row_store - plan.cu_streamout, "CU streamout");
    mapped(buf[60], plan.frame_statistics - plan.lcu_base, "LCU base address buffer");
    mapped(buf[63], 64, "LCU ILDB streamout");
    mapped(buf[95], plan.lcu_ildb - plan.sao_row_store, "SAO row store");
    mapped(buf[98], 576, "frame statistics");
    mapped(buf[101], plan.cu_streamout - plan.sse_row_store, "SSE source pixel row store");
    mapped(vbuf[16], layout.ctbs_x * 256, "VDEnc row store");
    mapped(vbuf[34], 3 * 64 + 1024, "VDEnc statistics");
    mapped(vbuf[46], plan.pak_object_bytes, "PAK object buffer");
    mapped(vbuf[49], layout.ds8.size, "8x downscaled recon");
    mapped(vbuf[52], layout.ds4.size, "4x downscaled recon");
    mapped(vbuf[62], layout.aligned_width / 32 * 128, "VDEnc tile row store");
    mapped(vbuf[65], layout.ctbs_x * layout.ctbs_y * 4, "cumulative CU count");
    if (vbuf[61] != 192) problem("weights histogram offset");
    if (ind[6] != vbuf[46] + plan.pak_object_cu_offset) problem("indirect CU objects are behind the LCU commands");
    mapped(ind[6], plan.pak_object_bytes - plan.pak_object_cu_offset, "indirect CU objects");
    const uint32_t bitstream = ind[9];
    mapped(bitstream, 4096, "bitstream");
    if (ind[12] <= bitstream || ind[12] - bitstream < 4096) problem("bitstream upper bound");
    mapped(status_address, 4, "status");
    if (b_picture) {
      mapped(buf[37], layout.full.size, "reference");
      mapped(buf[66], plan.mv_temporal_bytes, "reference motion vector buffer");
      if (buf[37] != vbuf[22] || buf[37] == recon || buf[37] != previous_recon) problem("reference picture address");
      if (vbuf[1] != previous_ds8 || vbuf[37] != previous_ds4 || buf[66] != previous_mv) problem("reference buffers are the previous picture's");
      mapped(vbuf[1], layout.ds8.size, "8x downscaled reference");
      mapped(vbuf[37], layout.ds4.size, "4x downscaled reference");
    }
    if (problems) return;
    previous_recon = recon;
    previous_ds8 = vbuf[49];
    previous_ds4 = vbuf[52];
    previous_mv = buf[31];

    // The source surface as the engine reads it.
    last_width = 0;
    {
      std::vector<uint8_t> luma, chroma;
      bool ok = true;
      auto sample = [&](uint32_t x, uint32_t row) -> uint8_t {
        bool good;
        uint8_t* p = gpu.ppgtt_bytes(source + y_tile_offset(pitch, x, row), 1, "source sample", &good);
        if (!good || !p) ok = false;
        return ok && p ? *p : 0;
      };
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) luma.push_back(sample(x, y));
      for (uint32_t y = 0; y < height / 2; ++y)
        for (uint32_t x = 0; x < width; ++x) chroma.push_back(sample(x, chroma_rows + y));
      if (ok) { last_luma = luma; last_chroma = chroma; last_width = width; last_height = height; }
    }

    // "Encode": the headers, then the slice data.
    Bits sbits;
    const Insert& sh = inserts.back();
    for (uint32_t k = 0; k < sh.bits; ++k) sbits.bit((sh.bytes[k / 8] >> (7 - k % 8)) & 1u);
    if (write_stream) code_slice_data(&sbits, b_picture, static_cast<int>(qp), layout.ctbs_x, layout.ctbs_y);
    else { sbits.put(0x80, 8); }
    std::vector<uint8_t> out;
    for (size_t k = 0; k + 1 < inserts.size(); ++k) out.insert(out.end(), inserts[k].bytes.begin(), inserts[k].bytes.end());
    // Emulation prevention over the slice header (after the first six bytes) and the slice data.
    uint32_t zeros = 0;
    for (size_t k = 0; k < sbits.bytes.size(); ++k) {
      const uint8_t b = sbits.bytes[k];
      if (k >= 6 && zeros >= 2 && b <= 3) { out.push_back(3); zeros = 0; }
      out.push_back(b);
      zeros = b == 0 ? zeros + 1 : 0;
    }
    for (size_t k = 0; k < out.size(); ++k) {
      bool ok;
      uint8_t* p = gpu.ppgtt_bytes(bitstream + k, 1, "bitstream write", &ok);
      if (!ok || !p) { problem("bitstream overflow"); return; }
      *p = out[k];
    }
    bool ok;
    uint8_t* st = gpu.ppgtt_bytes(status_address, 4, "status write", &ok);
    if (!ok || !st) { problem("status write"); return; }
    const uint32_t n = static_cast<uint32_t>(out.size());
    for (int k = 0; k < 4; ++k) st[k] = static_cast<uint8_t>(n >> (8 * k));
    ++pictures;
    if (!b_picture) ++keyframes;
    sizes.push_back(n);
  }
};

struct Rig2 : Rig {
  FakeEncoder engine;
  explicit Rig2(Generation g = Generation::Gen12) : Rig(g) { gpu.on_batch = [this](FakeGpu& gp, const std::vector<uint32_t>& d) { engine.run(gp, d); }; }
};

void make_i420(VideoFrameBuffer& f, uint32_t w, uint32_t h, uint32_t seed) {
  CHECK(f.allocate({w, h, PixelFormat::I420}));
  ByteView y = f.plane(0), u = f.plane(1), v = f.plane(2);
  for (uint32_t row = 0; row < h; ++row)
    for (uint32_t col = 0; col < w; ++col) y[uint64_t{row} * w + col] = static_cast<uint8_t>(((col * 3 + row * 5 + seed * 17) ^ (row & 7)) & 0xFF);
  const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
  for (uint32_t row = 0; row < ch; ++row)
    for (uint32_t col = 0; col < cw; ++col) {
      u[uint64_t{row} * cw + col] = static_cast<uint8_t>(60 + col * 2 + seed);
      v[uint64_t{row} * cw + col] = static_cast<uint8_t>(200 - row * 3 - seed);
    }
}

uint8_t limited_luma(uint32_t v) { return static_cast<uint8_t>(16 + (v * 219 + 127) / 255); }
uint8_t limited_chroma(int32_t c) { return static_cast<uint8_t>(128 + ((c - 128) * 224 + (c >= 128 ? 127 : -127)) / 255); }

// Encodes `count` pictures through the session, compares the source surface the engine saw with the input, decodes the packets on the CPU and checks the pictures.
void encode_and_check(TestAllocator& a, uint32_t w, uint32_t h, uint32_t count, uint32_t interval, uint32_t quality) {
  printf("  %ux%u, %u pictures, keyframe interval %u, quality %u\n", w, h, count, interval, quality);
  Rig2 rig;
  rig.engine.write_stream = (w % 64 == 0 && h % 64 == 0);
  EncoderConfig cfg;
  cfg.codec = VideoCodec::Hevc;
  cfg.width = w;
  cfg.height = h;
  cfg.input = PixelFormat::I420;
  cfg.keyframe_interval = interval;
  cfg.quality = quality;
  std::vector<std::vector<uint8_t>> packets;
  std::vector<bool> keys;
  VideoFrameBuffer sources[8] = {VideoFrameBuffer(a), VideoFrameBuffer(a), VideoFrameBuffer(a), VideoFrameBuffer(a), VideoFrameBuffer(a), VideoFrameBuffer(a), VideoFrameBuffer(a), VideoFrameBuffer(a)};
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    {
      IntelHevcVdencEncoder enc(a, driver, cfg);
      CHECK(enc.prepare());
      CHECK(enc.quantiser() == 51 - (quality < 1 ? 1 : quality > 100 ? 100 : quality) * 36 / 100);
      for (uint32_t k = 0; k < count; ++k) {
        make_i420(sources[k], w, h, k);
        CHECK(enc.submit(sources[k].frame(int64_t(k) * 1000, false)) == SubmitResult::Accepted);
        VideoPacket p;
        CHECK(enc.receive(&p) && p.pts_us == int64_t(k) * 1000 && p.data.size() > 6);
        keys.push_back(p.keyframe);
        packets.emplace_back();
        for (uint64_t b = 0; b < p.data.size(); ++b) packets.back().push_back(p.data[b]);
        CHECK(!enc.receive(&p));
        // The source surface the engine read is the input, limited range.
        const FakeEncoder& e = rig.engine;
        bool luma_ok = e.last_width != 0, chroma_ok = e.last_width != 0;
        const ConstByteView ry = sources[k].plane(0), ru = sources[k].plane(1), rv = sources[k].plane(2);
        const uint32_t cw = (w + 1) / 2;
        for (uint32_t row = 0; row < h && luma_ok; ++row)
          for (uint32_t col = 0; col < w; ++col)
            if (e.last_luma[uint64_t{row} * e.last_width + col] != limited_luma(ry[uint64_t{row} * w + col])) luma_ok = false;
        for (uint32_t row = 0; row < h / 2 && chroma_ok; ++row)
          for (uint32_t col = 0; col < w / 2; ++col) {
            if (e.last_chroma[uint64_t{row} * e.last_width + 2 * col] != limited_chroma(ru[uint64_t{row} * cw + col])) chroma_ok = false;
            if (e.last_chroma[uint64_t{row} * e.last_width + 2 * col + 1] != limited_chroma(rv[uint64_t{row} * cw + col])) chroma_ok = false;
          }
        CHECK(luma_ok && chroma_ok);
      }
      CHECK(enc.statistics().frames == count && enc.statistics().packets == count && enc.statistics().errors == 0);
    }
    CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
  }
  CHECK(rig.mem.live() == 1);
  CHECK(rig.engine.pictures == int(count));

  // Keyframes follow the interval and carry the parameter sets (VPS first).
  uint32_t expected_keys = 0;
  for (uint32_t k = 0; k < count; ++k) {
    const bool idr = k % (interval < 1 ? 1 : interval) == 0;
    CHECK(keys[k] == idr);
    expected_keys += idr;
    static const uint8_t kVps[6] = {0, 0, 0, 1, 0x40, 0x01};
    static const uint8_t kSlice[6] = {0, 0, 0, 1, 0x02, 0x01};   // TRAIL_R
    CHECK(packets[k].size() > 6 && memcmp(packets[k].data(), idr ? kVps : kSlice, 6) == 0);
  }
  CHECK(rig.engine.keyframes == int(expected_keys));
  if (!rig.engine.write_stream) return;

  // The CPU decodes the stream: flat grey pictures of the right size, one per packet.
  SoftwareVideoCodec soft(a);
  DecoderConfig dc;
  dc.codec = VideoCodec::Hevc;
  dc.output = PixelFormat::I420;
  UniquePointer<IVideoDecoder> dec = soft.create_decoder(dc);
  CHECK(dec != nullptr);
  if (!dec) return;
  uint32_t shown = 0;
  auto check_frame = [&](const VideoFrame& out) {
    CHECK(out.format.width == w && out.format.height == h);
    bool flat = true;
    for (uint32_t row = 0; row < h; ++row)
      for (uint32_t col = 0; col < w; ++col)
        if (out.plane[0][uint64_t{row} * out.stride[0] + col] != 128) flat = false;
    for (uint32_t row = 0; row < h / 2; ++row)
      for (uint32_t col = 0; col < w / 2; ++col)
        if (out.plane[1][uint64_t{row} * out.stride[1] + col] != 128 || out.plane[2][uint64_t{row} * out.stride[2] + col] != 128) flat = false;
    CHECK(flat);
    ++shown;
  };
  for (uint32_t k = 0; k < count; ++k) {
    VideoPacket in;
    in.data = ConstByteView(packets[k].data(), packets[k].size());
    in.pts_us = int64_t(k) * 1000;
    CHECK(dec->submit(in) == SubmitResult::Accepted);
    VideoFrame out;
    while (dec->receive(&out)) check_frame(out);
  }
  dec->flush();
  VideoFrame out;
  while (dec->receive(&out)) check_frame(out);
  CHECK(shown == count);
}

void test_rejections(TestAllocator& a) {
  printf("  configurations and failures\n");
  Rig2 rig;
  rig.engine.write_stream = true;
  VcsDriver driver(rig.mem);
  CHECK(driver.initialize(rig.config()));
  EncoderConfig cfg;
  cfg.codec = VideoCodec::Hevc;
  cfg.width = 64;
  cfg.height = 64;
  cfg.input = PixelFormat::I420;
  {
    EncoderConfig bad = cfg;
    bad.codec = VideoCodec::H264;
    IntelHevcVdencEncoder e(a, driver, bad);
    CHECK(!e.prepare());
  }
  for (uint32_t size : {0u, 8u, 4100u, 65u}) {
    EncoderConfig bad = cfg;
    bad.width = size;
    IntelHevcVdencEncoder e(a, driver, bad);
    CHECK(!e.prepare());
  }
  {
    EncoderConfig bad = cfg;
    bad.input = PixelFormat::Nv12;   // accepted ...
    IntelHevcVdencEncoder e(a, driver, bad);
    CHECK(e.prepare());
    IntelHevcVdencEncoder second(a, driver, cfg);
    CHECK(!second.prepare());        // ... but the engine serves one session at a time
  }
  IntelHevcVdencEncoder e(a, driver, cfg);
  CHECK(e.prepare());
  VideoFrameBuffer f(a), wrong(a);
  make_i420(f, 64, 64, 1);
  make_i420(wrong, 32, 64, 1);
  CHECK(e.submit(wrong.frame()) == SubmitResult::Invalid);
  CHECK(e.submit(f.frame(0, true)) == SubmitResult::Accepted);
  CHECK(e.submit(f.frame(40, false)) == SubmitResult::Full);   // the packet has not been taken
  VideoPacket p;
  CHECK(e.receive(&p) && p.keyframe && !e.receive(&p));
  CHECK(e.statistics().errors == 1 && e.statistics().frames == 1);
  CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
}

// Ice Lake and Coffee Lake have no HEVC encoder here: the session refuses (the media engine would not even offer it).
void test_other_generations(TestAllocator& a) {
  printf("  generations without HEVC encode\n");
  for (Generation g : {Generation::Gen9, Generation::Gen11}) {
    Rig2 rig(g);
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    EncoderConfig cfg;
    cfg.codec = VideoCodec::Hevc;
    cfg.width = 64;
    cfg.height = 64;
    cfg.input = PixelFormat::I420;
    IntelHevcVdencEncoder e(a, driver, cfg);
    CHECK(!e.prepare());
  }
}

void test_hang(TestAllocator& a) {
  printf("  a job that does not finish\n");
  Rig2 rig;
  rig.gpu.on_batch = nullptr;   // the engine never answers
  VcsDriver driver(rig.mem);
  CHECK(driver.initialize(rig.config()));
  EncoderConfig cfg;
  cfg.codec = VideoCodec::Hevc;
  cfg.width = 64;
  cfg.height = 64;
  cfg.input = PixelFormat::I420;
  IntelHevcVdencEncoder e(a, driver, cfg);
  CHECK(e.prepare());
  VideoFrameBuffer f(a);
  make_i420(f, 64, 64, 1);
  // With no model the batch runs but nothing stores a size: the picture is refused, not guessed.
  CHECK(e.submit(f.frame()) == SubmitResult::Invalid);
  VideoPacket p;
  CHECK(!e.receive(&p));
  CHECK(e.statistics().errors == 1 && e.statistics().frames == 0);
}

void test_device(TestAllocator& a) {
  printf("  through the media engine (Tiger Lake)\n");
  Rig2 rig;
  rig.engine.write_stream = true;
  std::vector<uint32_t> regs(0x20000 / 4, 0);
  regs[0x0D50 / 4] = 1;   // the video box answers the power request
  const MmioRegion region(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4);
  IntelMediaVideoEngine::HardwareSetup setup;
  setup.registers = &rig.gpu;
  setup.ggtt_table = rig.config().ggtt_table;
  setup.ggtt_first_page = 100;
  setup.memory = &rig.mem;
  {
    IntelMediaVideoEngine device(a, 0x9A49, region);   // Tiger Lake
    device.set_hardware(setup);
    CHECK(device.initialize());
    CHECK(device.hardware_decode() && device.info().accelerated_encode_codecs == (codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Hevc)));
    EncoderConfig cfg;
    cfg.codec = VideoCodec::Hevc;
    cfg.width = 64;
    cfg.height = 64;
    cfg.input = PixelFormat::I420;
    VideoFrameBuffer f(a);
    make_i420(f, 64, 64, 3);
    {
      UniquePointer<IVideoEncoder> hw = device.create_encoder(cfg);
      CHECK(hw != nullptr && hw->codec() == VideoCodec::Hevc);
      if (hw) {
        CHECK(hw->submit(f.frame()) == SubmitResult::Accepted);
        VideoPacket p;
        CHECK(hw->receive(&p) && p.keyframe && rig.engine.pictures == 1);
      }
    }
    UniquePointer<IVideoEncoder> again = device.create_encoder(cfg);   // free again
    CHECK(again != nullptr);
    if (again) CHECK(again->submit(f.frame()) == SubmitResult::Accepted);
    CHECK(rig.engine.pictures == 2);
  }
  CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
  CHECK(rig.mem.live() == 1);

  // The platforms after Xe-LP decode on the engine but do not encode on it: their VDEnc commands are not the ones written here (the media driver's definitions have an 89-dword
  // pipe buffer command, a 7-dword weights command, a 3-dword walker plus a separate HEVC/VP9 tile and slice state).
  struct Platform { const char* name; uint16_t id; bool media_tile; bool av1; };
  const Platform platforms[] = {{"Meteor Lake", 0x7D55, true, true}, {"Arc Alchemist", 0x56A0, false, true}, {"Lunar Lake", 0x6420, true, true}, {"Arc Battlemage", 0xE20B, true, true}};
  for (const Platform& pl : platforms) {
    printf("  %s\n", pl.name);
    Rig2 rig_pl;
    rig_pl.gpu.xehp = true;
    rig_pl.engine.write_stream = true;
    const uint64_t base = pl.media_tile ? IntelMediaEngineHw::kMediaTileBase : 0;
    std::vector<uint32_t> regs_pl((base + 0x20000) / 4, 0);
    regs_pl[(base + 0x0D50) / 4] = 1;   // the first video box answers the power request
    regs_pl[(base + 0x9140) / 4] = 0x5; // ... and the fuses say both boxes exist
    const MmioRegion region_pl(reinterpret_cast<uint64_t>(regs_pl.data()), regs_pl.size() * 4);
    IntelMediaVideoEngine::HardwareSetup setup_pl;
    setup_pl.registers = &rig_pl.gpu;   // (the simulated GPU stands for the tile's own window)
    setup_pl.ggtt_table = rig_pl.config().ggtt_table;
    setup_pl.ggtt_first_page = 100;
    setup_pl.memory = &rig_pl.mem;
    {
      IntelMediaVideoEngine device(a, pl.id, region_pl);
      device.set_hardware(setup_pl);
      CHECK(device.initialize() && device.hardware_decode());
      const uint32_t decode = device.info().accelerated_decode_codecs;
      CHECK((decode & codec_mask(VideoCodec::H264)) && (decode & codec_mask(VideoCodec::Hevc)) && (decode & codec_mask(VideoCodec::Vp9)) && !(decode & codec_mask(VideoCodec::Mpeg2)));
      CHECK(!!(decode & codec_mask(VideoCodec::Av1)) == pl.av1);
      CHECK(device.info().accelerated_encode_codecs == 0);
      EncoderConfig cfg;
      cfg.codec = VideoCodec::Hevc;
      cfg.width = 64;
      cfg.height = 64;
      cfg.input = PixelFormat::I420;
      UniquePointer<IVideoEncoder> enc = device.create_encoder(cfg);
      if (enc) {
        VideoFrameBuffer f(a);
        make_i420(f, 64, 64, 5);
        enc->submit(f.frame());
      }
      CHECK(rig_pl.engine.pictures == 0);   // (the CPU, if anything)
    }
    CHECK(rig_pl.gpu.violations.empty() && rig_pl.engine.problems == 0);
  }

  // Ice Lake and Coffee Lake offer H.264 only.
  for (Generation g : {Generation::Gen9, Generation::Gen11}) {
    Rig2 other(g);
    std::vector<uint32_t> r2(0x20000 / 4, 0);
    r2[(g == Generation::Gen9 ? 0x0D88 : 0x0D50) / 4] = 1;
    const MmioRegion region2(reinterpret_cast<uint64_t>(r2.data()), r2.size() * 4);
    IntelMediaVideoEngine::HardwareSetup setup2;
    setup2.registers = &other.gpu;
    setup2.ggtt_table = other.config().ggtt_table;
    setup2.ggtt_first_page = 100;
    setup2.memory = &other.mem;
    IntelMediaVideoEngine device(a, g == Generation::Gen11 ? 0x8A52 : 0x3E92, region2);
    device.set_hardware(setup2);
    CHECK(device.initialize());
    CHECK(device.info().accelerated_encode_codecs == codec_mask(VideoCodec::H264));
  }
}

}  // namespace

int main() {
  TestAllocator a;
  encode_and_check(a, 64, 64, 5, 3, 75);        // IPP IPP, one coding tree block
  encode_and_check(a, 128, 64, 4, 2, 40);       // two blocks side by side
  encode_and_check(a, 128, 128, 6, 6, 1);       // 2x2 blocks, lowest quality
  encode_and_check(a, 64, 64, 3, 1, 100);       // every picture a keyframe
  encode_and_check(a, 72, 40, 4, 2, 40);        // partial blocks: commands and source only
  encode_and_check(a, 1920, 1080, 2, 2, 75);    // a real size
  encode_and_check(a, 34, 18, 3, 3, 90);        // odd multiples of two: coded size 40x24
  test_rejections(a);
  test_other_generations(a);
  test_hang(a);
  test_device(a);
  CHECK(a.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("IntelHevcEncoderTest passed\n");
  return 0;
}
