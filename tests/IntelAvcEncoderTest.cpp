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

// The VDEnc H.264 encoder (IntelAvcVdencEncoder) against a simulated Gen9.5, Gen11 and Xe-LP GPU. The simulated engine reads the command batch the way the hardware would: it checks the order
// and length of every command and the fields that matter (codec and mode, the surfaces, the addresses, the picture and slice state, the headers to insert), then "encodes" by
// writing what a valid stream looks like: the headers it was told to insert, then slice data of I_PCM macroblocks read from the source surface (I pictures) or one skip run (P
// pictures), with the emulation prevention bytes the engine adds. The CPU decoder decodes the result: the pictures it shows must be the source pictures (in limited range) and
// the stream must parse, which proves the parameter sets and slice headers the session wrote and the source surface it tiled.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"
#include "Hardware/VideoCodec/Intel/IntelAvcVdencEncoder.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/Intel/IntelTiling.hpp"
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

// Header words of the commands (dword 0 without the length).
constexpr uint32_t kHMfxPipeMode = 0x70000000, kHMfxSurface = 0x70010000, kHMfxPipeBuf = 0x70020000, kHMfxIndObj = 0x70030000, kHMfxBsp = 0x70040000, kHMfxQm = 0x70070000,
                   kHMfxFqm = 0x70080000, kHPakInsert = 0x70480000, kHAvcImg = 0x71000000, kHAvcSlice = 0x71030000, kHAvcRefIdx = 0x71040000;
constexpr uint32_t kHMfxWait = 0x68000000, kHVdencControl = 0x708B0000;
constexpr uint32_t kHVdencPipeMode = 0x70800000, kHVdencSrc = 0x70810000, kHVdencRef = 0x70820000, kHVdencDs = 0x70830000, kHVdencPipeBuf = 0x70840000, kHVdencImg = 0x70850000,
                   kHVdencConstQpt = 0x70860000, kHVdencWalker = 0x70870000, kHVdFlush = 0x77800000;

class Bits {
 public:
  void put(uint32_t v, uint32_t n) { for (uint32_t i = n; i > 0; --i) bit((v >> (i - 1)) & 1u); }
  void ue(uint32_t v) {
    const uint32_t x = v + 1;
    uint32_t len = 0;
    while ((x >> len) > 1) ++len;
    put(0, len);
    put(x, len + 1);
  }
  void align_zero() { while (fill_) bit(0); }
  void trailing() { bit(1); align_zero(); }
  void bit(uint32_t b) {
    cur_ = static_cast<uint8_t>((cur_ << 1) | b);
    if (++fill_ == 8) { bytes.push_back(cur_); cur_ = 0; fill_ = 0; }
  }
  uint32_t fill() const { return fill_; }
  std::vector<uint8_t> bytes;
 private:
  uint8_t cur_ = 0;
  uint32_t fill_ = 0;
};

struct FakeEncoder {
  Generation gen = Generation::Gen9;
  int pictures = 0, problems = 0, keyframes = 0;
  std::vector<uint32_t> sizes;
  void problem(const char* what) { printf("    encoder model: %s\n", what); ++problems; }

  void run(FakeGpu& gpu, const std::vector<uint32_t>& d) {
    size_t i = 0;
    auto expect = [&](uint32_t header, uint32_t length, const char* name) -> const uint32_t* {
      // the next command must have this header (ignoring its length field) and total length
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
    auto field = [](uint32_t v, uint32_t lo, uint32_t hi) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1u); };
    while (i < d.size() && (d[i] == 0 || (d[i] >> 23) == 0x26)) i += d[i] == 0 ? 1 : 4;   // the leading flush

    // The command sizes of the generation (Gen11 and Xe-LP grew four VDEnc commands; Xe-LP starts with the VDEnc control state, both wait for the MFX pipe around its mode).
    const bool gen9 = gen == Generation::Gen9, gen12 = gen == Generation::Gen12;
    const uint32_t vdenc_mode_dw = gen12 ? 6 : 2, vdenc_ds_dw = gen9 ? 6 : 10, vdenc_buf_dw = gen9 ? 37 : gen12 ? 71 : 62, walker_dw = gen9 ? 4 : gen12 ? 27 : 10;
    auto expect_wait = [&](const char* name) {
      if (i < d.size() && (d[i] & ~0x100u) == kHMfxWait && (d[i] & 0x100u)) ++i; else problem(name);
    };
    if (gen12) {
      const uint32_t* control = expect(kHVdencControl, 2, "VDENC_CONTROL_STATE");
      if (!control || field(control[1], 1, 1) != 1) problem("VDEnc initialisation");
    }
    if (!gen9) expect_wait("MFX_WAIT before the pipe mode");
    const uint32_t* mode = expect(kHMfxPipeMode, 5, "MFX_PIPE_MODE_SELECT");
    if (!mode) return;
    if (!gen9) expect_wait("MFX_WAIT after the pipe mode");
    if (field(mode[1], 0, 3) != 2 || field(mode[1], 4, 4) != 1 || field(mode[1], 13, 13) != 1 || field(mode[1], 7, 7) != 1) problem("pipe mode: AVC, encode, VDEnc, scaled surface");
    const bool deblocking = field(mode[1], 9, 9) == 1 && field(mode[1], 8, 8) == 0;
    const uint32_t* s0 = expect(kHMfxSurface, 6, "recon surface");
    const uint32_t* s1 = expect(kHMfxSurface, 6, "source surface");
    const uint32_t* s2 = expect(kHMfxSurface, 6, "ds surface");
    if (!s0 || !s1 || !s2) return;
    if (field(s0[1], 0, 3) != 0 || field(s1[1], 0, 3) != 4 || field(s2[1], 0, 3) != 5) problem("surface ids");
    const uint32_t width = field(s1[2], 4, 17) + 1, height = field(s1[2], 18, 31) + 1;
    if (field(s0[2], 4, 17) + 1 != width || field(s0[2], 18, 31) + 1 != height) problem("recon and source differ in size");
    if (field(s2[2], 4, 17) + 1 < 48 || field(s2[2], 18, 31) + 1 < 48) problem("ds surface size");
    Nv12TiledLayout layout;
    if (!nv12_tiled_layout(width, height, &layout) || field(s1[3], 3, 19) + 1 != layout.pitch || field(s1[3], 27, 27) != 1 || field(s1[3], 28, 31) != 4 || field(s1[3], 1, 1) != 1 ||
        field(s1[4], 0, 14) != layout.chroma_offset_rows)
      problem("source surface layout");

    const uint32_t* buf = expect(kHMfxPipeBuf, 68, "MFX_PIPE_BUF_ADDR_STATE");
    const uint32_t* ind = expect(kHMfxIndObj, 26, "MFX_IND_OBJ_BASE_ADDR_STATE");
    const uint32_t* bsp = expect(kHMfxBsp, 10, "MFX_BSP_BUF_BASE_ADDR_STATE");
    if (!buf || !ind || !bsp) return;
    const uint32_t* vmode = expect(kHVdencPipeMode, vdenc_mode_dw, "VDENC_PIPE_MODE_SELECT");
    if (gen12 && vmode && (field(vmode[1], 7, 7) != 1 || field(vmode[3], 24, 27) != 7)) problem("Xe-LP TLB prefetch of the source");
    expect(kHVdencSrc, 6, "VDENC_SRC_SURFACE_STATE");
    expect(kHVdencRef, 6, "VDENC_REF_SURFACE_STATE");
    const uint32_t* ds_state = expect(kHVdencDs, vdenc_ds_dw, "VDENC_DS_REF_SURFACE_STATE");
    if (ds_state && !gen9 && (ds_state[6] != 0 || ds_state[7] != 0 || ds_state[8] != 0 || ds_state[9] != 0)) problem("second DS surface unused");
    const uint32_t* vbuf = expect(kHVdencPipeBuf, vdenc_buf_dw, "VDENC_PIPE_BUF_ADDR_STATE");
    const uint32_t* cqpt = expect(kHVdencConstQpt, 61, "VDENC_CONST_QPT_STATE");
    const uint32_t* img = expect(kHAvcImg, 21, "MFX_AVC_IMG_STATE");
    const uint32_t* vimg = expect(kHVdencImg, 35, "VDENC_IMG_STATE");
    if (!vmode || !vbuf || !cqpt || !img || !vimg) return;
    if (field(vmode[1], 0, 3) != 2) problem("VDEnc standard");
    for (int k = 0; k < 4; ++k) expect(kHMfxQm, 18, "MFX_QM_STATE");
    for (int k = 0; k < 4; ++k) expect(kHMfxFqm, 34, "MFX_FQM_STATE");

    const uint32_t mbs_w = field(img[2], 0, 7) + 1, mbs_h = field(img[2], 16, 23) + 1;
    if (mbs_w * 16 != width || mbs_h * 16 != height || field(img[1], 0, 15) != mbs_w * mbs_h) problem("MFX image state size");
    if (field(vimg[3], 16, 31) != mbs_w || field(vimg[5], 0, 15) + 1 != mbs_h) problem("VDEnc image state size");
    const bool p_picture = field(vimg[5], 29, 30) == 1;
    const uint32_t qp = field(vimg[14], 0, 7);
    if (qp > 51 || field(img[3], 13, 13) != 1) problem("image state qp / rho domain");
    // The skip thresholds exist for P pictures only.
    uint32_t skip_thresholds = 0;
    for (int k = 12; k <= 24; ++k) skip_thresholds |= cqpt[k];
    if (p_picture != (skip_thresholds != 0)) problem("constant QP table for the picture type");

    uint32_t ref_entry = 0xFF;
    if (p_picture) {
      const uint32_t* ref = expect(kHAvcRefIdx, 10, "MFX_AVC_REF_IDX_STATE");
      if (ref) ref_entry = ref[2] & 0xFF;
      if (ref_entry == 0x80 || (ref_entry & 0x20) == 0) problem("reference list entry");
    }
    const uint32_t* slice = expect(kHAvcSlice, 11, "MFX_AVC_SLICE_STATE");
    if (!slice) return;
    if (field(slice[1], 0, 3) != (p_picture ? 0u : 2u) || field(slice[3], 16, 21) != qp || field(slice[6], 19, 19) != 1) problem("slice state");
    if (field(slice[5], 24, 31) != 0 || field(slice[5], 16, 24) != mbs_h || field(slice[4], 0, 14) != 0) problem("slice extent");
    if (field(slice[3], 27, 28) != (deblocking ? 0u : 1u)) problem("deblocking control");
    if (p_picture && field(slice[2], 16, 21) != 1) problem("slice reference count");

    // The headers.
    struct Insert { std::vector<uint8_t> bytes; uint32_t bits; bool emulation; uint32_t skip; bool slice_header; bool last; };
    std::vector<Insert> inserts;
    while (i < d.size() && (d[i] & 0xFFFF0000u) == kHPakInsert) {
      const uint32_t len = (d[i] & 0xFFF) + 2;
      if (len < 3 || i + len > d.size()) { problem("PAK insert length"); return; }
      Insert in;
      const uint32_t data_dwords = len - 2, last_bits = field(d[i + 1], 8, 13);
      in.bits = (data_dwords - 1) * 32 + last_bits;
      in.emulation = field(d[i + 1], 3, 3) != 0;
      in.skip = field(d[i + 1], 4, 7);
      in.slice_header = field(d[i + 1], 14, 14) != 0;
      in.last = field(d[i + 1], 2, 2) != 0;
      for (uint32_t k = 0; k < (in.bits + 7) / 8; ++k) in.bytes.push_back(static_cast<uint8_t>(d[i + 2 + k / 4] >> (8 * (k % 4))));
      inserts.push_back(in);
      i += len;
    }
    if (inserts.empty() || !inserts.back().slice_header || !inserts.back().last || inserts.back().skip != 4 || !inserts.back().emulation) problem("slice header insert");
    for (size_t k = 0; k + 1 < inserts.size(); ++k)
      if (inserts[k].slice_header || inserts[k].last || inserts[k].emulation) problem("parameter set insert");
    if (inserts.size() != (p_picture ? 1u : 3u)) problem("number of header inserts");

    const uint32_t* walker = expect(kHVdencWalker, walker_dw, "VDENC_WALKER_STATE");
    const uint32_t* flush = expect(kHVdFlush, 2, "VD_PIPELINE_FLUSH");
    if (!walker || !flush) return;
    if (field(walker[2], 0, 9) != mbs_h) problem("walker extent");
    if (!gen9 && field(walker[5], 0, 15) + 1 != width) problem("walker width");
    if (!gen9) for (uint32_t k = 6; k < 10; ++k) if (walker[k] != 0) problem("walker tile offsets unused");   // (later Xe-LP dwords carry driver defaults)
    if (field(flush[1], 1, 1) != 1 || field(flush[1], 17, 17) != 1) problem("pipeline flush");
    while (i < d.size() && d[i] == 0) ++i;
    if (i >= d.size() || (d[i] >> 23) != 0x26) { problem("flush before the status store"); return; }
    i += 4;
    uint32_t status_address = 0;
    if (i + 4 <= d.size() && (d[i] >> 23) == 0x24 && d[i + 1] == (gen9 ? 0x128A0u : 0x1C08A0u)) { status_address = d[i + 2]; i += 4; } else { problem("MI_STORE_REGISTER_MEM of the bitstream byte count"); return; }
    while (i < d.size() && d[i] == 0) ++i;
    if (i >= d.size() || (d[i] >> 23) != 0x0A) problem("batch end");

    // Every address the commands name must be mapped.
    auto mapped = [&](uint32_t address, uint64_t bytes, const char* what) {
      if (!address) { problem(what); return; }
      bool ok;
      if (!gpu.ppgtt_bytes(address, 1, what, &ok) || !ok || !gpu.ppgtt_bytes(address + bytes - 1, 1, what, &ok) || !ok) problem(what);
    };
    const uint32_t recon = deblocking ? buf[4] : buf[1];
    const uint32_t source = buf[7];
    mapped(recon, layout.size, "recon surface not mapped");
    mapped(source, layout.size, "source surface not mapped");
    mapped(buf[13], mbs_w * 64, "intra row store");
    mapped(buf[16], mbs_w * 4 * 64, "deblocking row store");
    mapped(bsp[1], mbs_w * 128, "BSD/MPC row store");
    mapped(buf[62], 48 * 128, "downscaled recon");
    mapped(vbuf[10], layout.size, "VDEnc source");
    mapped(vbuf[16], mbs_w * 64, "VDEnc row store");
    mapped(vbuf[34], 80, "VDEnc statistics");
    if (source != vbuf[10]) problem("the two pipes read different sources");
    if (p_picture) {
      mapped(vbuf[22], layout.size, "VDEnc reference");
      mapped(vbuf[1], 48 * 128, "VDEnc ds reference");
      const uint32_t store = (ref_entry >> 1) & 0xF;
      if (buf[19 + 2 * store] != vbuf[22] || vbuf[22] == recon) problem("reference picture address");
    }
    const uint32_t bitstream = ind[21];
    mapped(bitstream, 4096, "bitstream");
    mapped(status_address, 4, "status");
    if (problems) return;

    // "Encode": the headers, then the slice data.
    Bits sbits;
    const Insert& sh = inserts.back();
    for (uint32_t k = 0; k < sh.bits; ++k) sbits.bit((sh.bytes[k / 8] >> (7 - k % 8)) & 1u);
    if (p_picture) {
      sbits.ue(mbs_w * mbs_h);   // mb_skip_run: every macroblock is skipped
      sbits.trailing();
    } else {
      for (uint32_t my = 0; my < mbs_h; ++my)
        for (uint32_t mx = 0; mx < mbs_w; ++mx) {
          sbits.ue(25);          // I_PCM
          sbits.align_zero();
          auto sample = [&](uint32_t x, uint32_t row) -> uint8_t {
            bool ok;
            uint8_t* p = gpu.ppgtt_bytes(source + y_tile_offset(layout.pitch, x, row), 1, "source sample", &ok);
            return ok && p ? *p : 0;
          };
          for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x) sbits.put(sample(mx * 16 + x, my * 16 + y), 8);
          for (uint32_t c = 0; c < 2; ++c)
            for (uint32_t y = 0; y < 8; ++y)
              for (uint32_t x = 0; x < 8; ++x) sbits.put(sample((mx * 8 + x) * 2 + c, layout.chroma_offset_rows + my * 8 + y), 8);
        }
      sbits.trailing();
    }
    std::vector<uint8_t> out;
    for (size_t k = 0; k + 1 < inserts.size(); ++k) out.insert(out.end(), inserts[k].bytes.begin(), inserts[k].bytes.end());
    // Emulation prevention over the slice header (after the first four bytes) and the slice data.
    uint32_t zeros = 0;
    for (size_t k = 0; k < sbits.bytes.size(); ++k) {
      const uint8_t b = sbits.bytes[k];
      if (k >= 4 && zeros >= 2 && b <= 3) { out.push_back(3); zeros = 0; }
      out.push_back(b);
      zeros = b == 0 ? zeros + 1 : 0;
    }
    // Write the bitstream and the byte count.
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
    // The reconstruction is the source (PCM loses nothing; the skip run repeats the reference).
    if (!p_picture) {
      for (uint64_t off = 0; off < layout.size; ++off) {
        uint8_t* from = gpu.ppgtt_bytes(source + off, 1, "recon copy", &ok);
        uint8_t* to = gpu.ppgtt_bytes(recon + off, 1, "recon copy", &ok);
        if (from && to) *to = *from;
      }
    }
    ++pictures;
    if (!p_picture) ++keyframes;
    sizes.push_back(n);
  }
};

struct Rig2 : Rig {
  FakeEncoder engine;
  explicit Rig2(Generation g = Generation::Gen9) : Rig(g) { engine.gen = g; gpu.on_batch = [this](FakeGpu& gp, const std::vector<uint32_t>& d) { engine.run(gp, d); }; }
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

// Encodes `count` pictures through the session, decodes the packets on the CPU and compares.
void encode_and_check(TestAllocator& a, uint32_t w, uint32_t h, uint32_t count, uint32_t interval, uint32_t quality, Generation g = Generation::Gen9) {
  printf("  %ux%u, %u pictures, keyframe interval %u, quality %u%s\n", w, h, count, interval, quality, g == Generation::Gen12 ? " (Xe-LP)" : g == Generation::Gen11 ? " (Ice Lake)" : "");
  Rig2 rig(g);
  EncoderConfig cfg;
  cfg.codec = VideoCodec::H264;
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
      IntelAvcVdencEncoder enc(a, driver, cfg);
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
      }
      CHECK(enc.statistics().frames == count && enc.statistics().packets == count && enc.statistics().errors == 0);
    }
    CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
  }
  CHECK(rig.mem.live() == 1);
  CHECK(rig.engine.pictures == int(count));

  // The stream: keyframes follow the interval and carry the parameter sets; the CPU decodes it, and the pictures are the sources.
  uint32_t expected_keys = 0;
  for (uint32_t k = 0; k < count; ++k) {
    const bool idr = k % (interval < 1 ? 1 : interval) == 0;
    CHECK(keys[k] == idr);
    expected_keys += idr;
    static const uint8_t kSps[5] = {0, 0, 0, 1, 0x67};
    const bool has_sps = packets[k].size() > 5 && memcmp(packets[k].data(), kSps, 5) == 0;
    CHECK(has_sps == idr);
  }
  CHECK(rig.engine.keyframes == int(expected_keys));
  SoftwareVideoCodec soft(a);
  DecoderConfig dc;
  dc.codec = VideoCodec::H264;
  dc.output = PixelFormat::I420;
  UniquePointer<IVideoDecoder> dec = soft.create_decoder(dc);
  CHECK(dec != nullptr);
  if (!dec) return;
  uint32_t shown = 0;
  for (uint32_t k = 0; k < count; ++k) {
    VideoPacket in;
    in.data = ConstByteView(packets[k].data(), packets[k].size());
    in.pts_us = int64_t(k) * 1000;
    CHECK(dec->submit(in) == SubmitResult::Accepted);
    VideoFrame out;
    while (dec->receive(&out)) {
      CHECK(out.format.width == w && out.format.height == h);
      // The picture that was last coded as a keyframe is what a skipped picture repeats.
      const uint32_t key_index = k - (k % (interval < 1 ? 1 : interval));
      const VideoFrameBuffer& ref = sources[key_index];
      const ConstByteView ry = ref.plane(0), ru = ref.plane(1), rv = ref.plane(2);
      const uint32_t cw = (w + 1) / 2;
      bool luma_ok = true, chroma_ok = true;
      for (uint32_t row = 0; row < h; ++row)
        for (uint32_t col = 0; col < w; ++col)
          if (out.plane[0][uint64_t{row} * out.stride[0] + col] != limited_luma(ry[uint64_t{row} * w + col])) luma_ok = false;
      for (uint32_t row = 0; row < (h + 1) / 2; ++row)
        for (uint32_t col = 0; col < cw; ++col) {
          if (out.plane[1][uint64_t{row} * out.stride[1] + col] != limited_chroma(ru[uint64_t{row} * cw + col])) chroma_ok = false;
          if (out.plane[2][uint64_t{row} * out.stride[2] + col] != limited_chroma(rv[uint64_t{row} * cw + col])) chroma_ok = false;
        }
      CHECK(luma_ok && chroma_ok);
      ++shown;
    }
  }
  dec->flush();
  VideoFrame out;
  while (dec->receive(&out)) ++shown;
  CHECK(shown == count);
}

void test_rejections(TestAllocator& a) {
  printf("  configurations and failures\n");
  Rig2 rig;
  VcsDriver driver(rig.mem);
  CHECK(driver.initialize(rig.config()));
  EncoderConfig cfg;
  cfg.codec = VideoCodec::H264;
  cfg.width = 64;
  cfg.height = 48;
  cfg.input = PixelFormat::I420;
  {
    EncoderConfig bad = cfg;
    bad.codec = VideoCodec::Hevc;
    IntelAvcVdencEncoder e(a, driver, bad);
    CHECK(!e.prepare());
  }
  for (uint32_t size : {0u, 8u, 4100u}) {
    EncoderConfig bad = cfg;
    bad.width = size;
    IntelAvcVdencEncoder e(a, driver, bad);
    CHECK(!e.prepare());
  }
  {
    EncoderConfig bad = cfg;
    bad.input = PixelFormat::Nv12;   // accepted ...
    IntelAvcVdencEncoder e(a, driver, bad);
    CHECK(e.prepare());
    IntelAvcVdencEncoder second(a, driver, cfg);
    CHECK(!second.prepare());        // ... but the engine serves one session at a time
  }
  IntelAvcVdencEncoder e(a, driver, cfg);
  CHECK(e.prepare());
  VideoFrameBuffer f(a), wrong(a), yuv(a);
  make_i420(f, 64, 48, 1);
  make_i420(wrong, 32, 48, 1);
  CHECK(e.submit(wrong.frame()) == SubmitResult::Invalid);
  CHECK(e.submit(f.frame(0, true)) == SubmitResult::Accepted);
  CHECK(e.submit(f.frame(40, false)) == SubmitResult::Full);   // the packet has not been taken
  VideoPacket p;
  CHECK(e.receive(&p) && p.keyframe && !e.receive(&p));
  CHECK(e.statistics().errors == 1 && e.statistics().frames == 1);
  CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
}

void test_hang(TestAllocator& a) {
  printf("  a job that does not finish\n");
  Rig2 rig;
  rig.gpu.on_batch = nullptr;   // the engine never answers
  VcsDriver driver(rig.mem);
  CHECK(driver.initialize(rig.config()));
  EncoderConfig cfg;
  cfg.codec = VideoCodec::H264;
  cfg.width = 64;
  cfg.height = 48;
  cfg.input = PixelFormat::I420;
  IntelAvcVdencEncoder e(a, driver, cfg);
  CHECK(e.prepare());
  VideoFrameBuffer f(a);
  make_i420(f, 64, 48, 1);
  // With no model the batch runs but nothing stores a size: the picture is refused, not guessed.
  CHECK(e.submit(f.frame()) == SubmitResult::Invalid);
  VideoPacket p;
  CHECK(!e.receive(&p));
  CHECK(e.statistics().errors == 1 && e.statistics().frames == 0);
}

void test_device(TestAllocator& a, Generation g = Generation::Gen9) {
  const char* name = g == Generation::Gen12 ? "Tiger Lake" : g == Generation::Gen11 ? "Ice Lake" : "Coffee Lake";
  printf("  through the media engine (%s)\n", name);
  Rig2 rig(g);
  std::vector<uint32_t> regs(0x20000 / 4, 0);
  regs[(g == Generation::Gen9 ? 0x0D88 : 0x0D50) / 4] = 1;   // the video box answers the power request
  const MmioRegion region(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4);
  IntelMediaVideoEngine::HardwareSetup setup;
  setup.registers = &rig.gpu;
  setup.ggtt_table = rig.config().ggtt_table;
  setup.ggtt_first_page = 100;
  setup.memory = &rig.mem;
  {
    IntelMediaVideoEngine device(a, g == Generation::Gen12 ? 0x9A49 : g == Generation::Gen11 ? 0x8A52 : 0x3E92, region);   // Tiger Lake / Ice Lake / Coffee Lake
    device.set_hardware(setup);
    CHECK(device.initialize());
    CHECK(device.hardware_decode() && device.info().accelerated_encode_codecs == codec_mask(VideoCodec::H264));
    EncoderConfig cfg;
    cfg.codec = VideoCodec::H264;
    cfg.width = 48;
    cfg.height = 32;
    cfg.input = PixelFormat::I420;
    VideoFrameBuffer f(a);
    make_i420(f, 48, 32, 3);
    {
      UniquePointer<IVideoEncoder> hw = device.create_encoder(cfg);
      CHECK(hw != nullptr);
      CHECK(hw->submit(f.frame()) == SubmitResult::Accepted);
      VideoPacket p;
      CHECK(hw->receive(&p) && p.keyframe && rig.engine.pictures == 1);
      // The engine is taken: the next session is the CPU's and sends nothing to it.
      UniquePointer<IVideoEncoder> sw = device.create_encoder(cfg);
      CHECK(sw != nullptr);
      CHECK(sw->submit(f.frame()) == SubmitResult::Accepted);
      CHECK(sw->receive(&p) && p.data.size() > 20 && rig.engine.pictures == 1);
      // Other codecs are the CPU's.
      EncoderConfig mj = cfg;
      mj.codec = VideoCodec::Mjpeg;
      CHECK(device.create_encoder(mj) != nullptr && rig.engine.pictures == 1);
    }
    UniquePointer<IVideoEncoder> again = device.create_encoder(cfg);   // free again
    CHECK(again != nullptr);
    CHECK(again->submit(f.frame()) == SubmitResult::Accepted);
    CHECK(rig.engine.pictures == 2);
  }
  CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
  CHECK(rig.mem.live() == 1);
}

// A Tiger Lake whose first video box does not answer (the Gen9 power request register is all this one has): the CPU encodes.
void test_no_video_box(TestAllocator& a) {
  printf("  a video box that does not answer\n");
  Rig2 rig(Generation::Gen12);
  std::vector<uint32_t> regs(0x20000 / 4, 0);
  regs[0x0D88 / 4] = 1;
  const MmioRegion region(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4);
  IntelMediaVideoEngine::HardwareSetup setup;
  setup.registers = &rig.gpu;
  setup.ggtt_table = rig.config().ggtt_table;
  setup.ggtt_first_page = 100;
  setup.memory = &rig.mem;
  IntelMediaVideoEngine tiger(a, 0x9A49, region);
  tiger.set_hardware(setup);
  CHECK(tiger.initialize() && tiger.info().accelerated_encode_codecs == 0);
}

}  // namespace

int main() {
  TestAllocator a;
  encode_and_check(a, 64, 48, 5, 3, 75);     // IPP IPP
  encode_and_check(a, 72, 40, 4, 2, 40);     // a size that is not a macroblock multiple: cropped
  encode_and_check(a, 32, 32, 3, 1, 100);    // every picture a keyframe
  encode_and_check(a, 96, 64, 6, 6, 1);
  for (Generation g : {Generation::Gen11, Generation::Gen12}) {
    encode_and_check(a, 64, 48, 5, 3, 75, g);
    encode_and_check(a, 72, 40, 4, 2, 40, g);
    encode_and_check(a, 96, 64, 6, 6, 1, g);
  }
  test_rejections(a);
  test_hang(a);
  test_device(a);
  test_device(a, Generation::Gen11);
  test_device(a, Generation::Gen12);
  test_no_video_box(a);
  CHECK(a.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("IntelAvcEncoderTest passed\n");
  return 0;
}
