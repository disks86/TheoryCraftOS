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

// Host-side tests for the Intel hardware AV1 decode session (IntelAv1Decoder) against a simulated Gen12 GPU. The simulated engine plays the decode hardware: for every
// batch it reads the AVP commands the way the engine would, checks that every address in them is mapped in the context's address space and that the buffers do not overlap,
// that the frame state says what the frame header says (compared with what a separate CPU decoder reports for the same frame), that each segment has its state, that the
// reference pictures are the surfaces of the frames the reference slots hold and the collocated motion vectors theirs, that each tile has its state and its bytes in the
// bitstream, that the entropy contexts the engine starts from are the defaults for the frame's quantizer or the tables the primary reference frame ended with (the stand-in
// "adapts" by leaving a pattern of its own in the backward adaptation buffer, which later frames must then find where the rules put it), and that the segmentation map the
// frame reads is the one its primary reference holds; then it "decodes" by writing the picture the CPU decoder made for the same frame into the output surface, tiled as the
// engine does. The session must hand back exactly the CPU decoder's pictures, in the same order.
//
// This does not show that the real engine decodes anything: the commands the stand-in checks follow the reference driver, as far as is known.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <set>
#include <vector>

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"
#include "Hardware/VideoCodec/Intel/IntelAv1Cdf.hpp"
#include "Hardware/VideoCodec/Intel/IntelAv1Session.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/Software/Av1.hpp"
#include "Hardware/VideoCodec/Software/Av1Obu.hpp"

using namespace hardware;
using namespace hardware::intel_mfx;
using namespace hardware::intel_avp;

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

static bool read_ivf(const std::vector<uint8_t>& d, std::vector<uint8_t>* stream, std::vector<size_t>* ends) {
  if (d.size() < 32 || memcmp(d.data(), "DKIF", 4) != 0) return false;
  size_t pos = d[6] | (d[7] << 8);
  while (pos + 12 <= d.size()) {
    const size_t size = d[pos] | (d[pos + 1] << 8) | (d[pos + 2] << 16) | (size_t(d[pos + 3]) << 24);
    if (size > d.size() - pos - 12) return false;
    stream->insert(stream->end(), d.begin() + long(pos + 12), d.begin() + long(pos + 12 + size));
    ends->push_back(stream->size());
    pos += 12 + size;
  }
  return !ends->empty();
}

struct Output {
  std::vector<int64_t> pts;
  std::vector<std::vector<uint8_t>> bytes;
  std::vector<VideoFormat> format;
};

static void drain(IVideoDecoder& dec, Output* out) {
  VideoFrame f;
  while (dec.receive(&f)) {
    std::vector<uint8_t> bytes;
    for (uint32_t p = 0; p < 3; ++p)
      for (uint32_t y = 0; y < f.format.plane_height(p); ++y) bytes.insert(bytes.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + f.format.plane_width(p));
    out->pts.push_back(f.pts_us);
    out->bytes.push_back(bytes);
    out->format.push_back(f.format);
  }
}

static bool feed(IVideoDecoder& dec, ConstByteView au, int64_t pts, Output* out) {
  VideoPacket p;
  p.data = au;
  p.pts_us = pts;
  SubmitResult r = dec.submit(p);
  if (r == SubmitResult::Full) {
    drain(dec, out);
    r = dec.submit(p);
  }
  drain(dec, out);
  return r == SubmitResult::Accepted;
}

// The CPU decoder's view of a stream: for every frame it decodes (hidden ones too) the headers, the tiles and the picture it makes.
struct FrameRecord {
  av1::SequenceHeader seq;
  av1::FrameHeader hdr;
  std::vector<uint8_t> tiles;                 // the tile data back to back
  std::vector<uint32_t> tile_sizes;
  std::vector<uint8_t> plane[3];              // the decoded picture, 8-bit, cropped
  int width = 0, height = 0;
  int expected_ref[7] = {-1, -1, -1, -1, -1, -1, -1};   // the decode index of the frames the references name
};

struct Reference final : av1::IFrameObserver {
  std::vector<uint8_t> stream;
  std::vector<size_t> ends;
  std::vector<FrameRecord> frames;
  Output out;
  int held[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

  ConstByteView unit(size_t k) const {
    const size_t start = k == 0 ? 0 : ends[k - 1];
    return ConstByteView(stream.data() + start, ends[k] - start);
  }

  void frame_decoded(const av1::SequenceHeader& seq, const av1::FrameHeader& hdr, ArrayView<const av1::TileData> tiles, const av1::Picture& pic) override {
    FrameRecord r;
    r.seq = seq;
    r.hdr = hdr;
    r.width = hdr.width[1];
    r.height = hdr.height;
    if (hdr.frame_type == av1::FRAME_TYPE_INTER || hdr.frame_type == av1::FRAME_TYPE_SWITCH)
      for (int i = 0; i < 7; ++i) r.expected_ref[i] = held[hdr.refidx[i] & 7];
    for (const av1::TileData& t : tiles) {
      r.tiles.insert(r.tiles.end(), t.data.data(), t.data.data() + t.data.size());
      r.tile_sizes.push_back(static_cast<uint32_t>(t.data.size()));
    }
    if (seq.hbd == 0 && seq.layout == av1::PIXEL_LAYOUT_I420)
      for (int p = 0; p < 3; ++p)
        for (int y = 0; y < pic.plane_height(p); ++y)
          for (int x = 0; x < pic.plane_width(p); ++x) r.plane[p].push_back(static_cast<uint8_t>(pic.plane(p).at(x, y)));
    const int id = int(frames.size());
    for (int i = 0; i < 8; ++i)
      if (hdr.refresh_frame_flags & (1 << i)) held[i] = id;
    frames.push_back(static_cast<FrameRecord&&>(r));
  }
};

static bool make_reference(allocators::IAllocator& a, Reference* ref) {
  Av1Decoder dec(a);
  dec.set_frame_observer(ref);
  VideoFrame f;
  for (size_t k = 0; k < ref->ends.size(); ++k) {
    dec.decode(ref->unit(k), int64_t(k) * 1000);
    while (dec.receive(&f)) {
      std::vector<uint8_t> bytes;
      for (uint32_t p = 0; p < 3; ++p)
        for (uint32_t y = 0; y < f.format.plane_height(p); ++y) bytes.insert(bytes.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + f.format.plane_width(p));
      ref->out.pts.push_back(f.pts_us);
      ref->out.bytes.push_back(bytes);
      ref->out.format.push_back(f.format);
    }
  }
  return !ref->out.pts.empty();
}

static bool make_reference_file(allocators::IAllocator& a, const char* name, Reference* ref) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/av1_%s.ivf", name);
  return read_ivf(read_file(path), &ref->stream, &ref->ends) && make_reference(a, ref);
}

static uint32_t align8(uint32_t v) { return (v + 7) & ~7u; }

// What the stand-in leaves in the backward adaptation buffer after frame k, and in the segmentation map it writes.
static uint8_t cdf_pattern(long k, uint32_t i) { return uint8_t(k * 37 + i * 13 + 5); }
static uint8_t seg_pattern(long k, uint32_t i) { return uint8_t(k * 91 + i * 3 + 1); }

// The decode engine of the simulated GPU.
struct FakeDecoder {
  const Reference* reference = nullptr;
  bool check_content = true;
  std::map<uint64_t, long> painted;                // surface address -> the frame it last held
  std::map<long, uint64_t> address_of;             // frame -> the surface it was painted into
  std::map<long, uint64_t> mv_of;                  // frame -> its collocated motion vector buffer
  std::map<long, std::vector<uint8_t>> saved_cdf;  // frame -> the tables the rules say it ended with
  std::map<long, long> seg_source;                 // frame -> the frame whose map pattern it holds (-1: none)
  int frames = 0, problems = 0;
  size_t next = 0;

  static uint64_t addr(const std::vector<uint32_t>& d, size_t at) { return (uint64_t{d[at + 1]} << 32) | d[at]; }
  static uint32_t bits(const std::vector<uint32_t>& d, size_t cmd, uint32_t dw, uint32_t lo, uint32_t hi) {
    const uint32_t v = d[cmd + dw] >> lo;
    return hi - lo == 31 ? v : v & ((1u << (hi - lo + 1)) - 1);
  }
  void problem(const char* what) {
    ++problems;
    printf("  fake engine: %s\n", what);
  }
  static bool read_bytes(FakeGpu& gpu, uint64_t at, uint64_t n, std::vector<uint8_t>* out) {
    out->clear();
    for (uint64_t off = 0; off < n;) {
      const uint64_t chunk = std::min<uint64_t>(n - off, 4096 - ((at + off) & 4095));
      bool ok;
      uint8_t* p = gpu.ppgtt_bytes(at + off, chunk, "read", &ok);
      if (!ok) return false;
      out->insert(out->end(), p, p + chunk);
      off += chunk;
    }
    return true;
  }
  static bool write_pattern(FakeGpu& gpu, uint64_t at, uint64_t n, long k, uint8_t (*pattern)(long, uint32_t)) {
    for (uint64_t off = 0; off < n; ++off) {
      bool ok;
      uint8_t* p = gpu.ppgtt_bytes(at + off, 1, "write", &ok);
      if (!ok) return false;
      *p = pattern(k, uint32_t(off));
    }
    return true;
  }

  void run(FakeGpu& gpu, const std::vector<uint32_t>& d) {
    enum { kModeSelect, kSurface, kBufAddr, kIndObj, kSegment, kPic, kInloop, kInter, kTile, kBsd, kOther };
    std::vector<int> order;
    std::vector<size_t> segment_at, tile_at, bsd_at;
    size_t pipe_mode = 0, buf_at = 0, ind_at = 0, pic_at = 0, loop_at = 0, inter_at = 0;
    bool have_mode = false, have_buf = false, have_ind = false, have_pic = false, have_loop = false, have_inter = false, have_flush = false;
    std::set<uint32_t> surface_ids;
    uint32_t pitch = 0, chroma_rows = 0;
    size_t i = 0;
    while (i < d.size()) {
      const uint32_t h = d[i];
      if (h == 0) { ++i; continue; }
      if ((h >> 23) == 0x0A) break;
      if (h == kMfxWait) { order.push_back(kOther); ++i; continue; }
      if ((h >> 29) == 0) { i += (h & 0x3F) + 2; continue; }
      const size_t len = (h & 0xFF) + 2;
      if (i + len > d.size()) { problem("command runs past the batch"); return; }
      if ((h & 0xFFFF0000) == kVdPipelineFlush) { have_flush = true; if (len != 2) problem("pipeline flush length"); i += len; continue; }
      switch (h & 0xFFFF0000) {
        case kAvpPipeModeSelect: have_mode = true; pipe_mode = i; order.push_back(kModeSelect); if (len != kPipeModeSelectDwords) problem("pipe mode select length"); break;
        case kAvpSurfaceState:
          order.push_back(kSurface);
          if (len != kSurfaceStateDwords || bits(d, i, 2, 27, 31) != 4) problem("surface state");
          surface_ids.insert(bits(d, i, 1, 28, 31));
          pitch = bits(d, i, 1, 0, 16) + 1;
          chroma_rows = bits(d, i, 2, 0, 14);
          break;
        case kAvpPipeBufAddrState: have_buf = true; buf_at = i; order.push_back(kBufAddr); if (len != kPipeBufAddrDwords) { problem("pipe buffer address length"); return; } break;
        case kAvpIndObjBaseAddrState: have_ind = true; ind_at = i; order.push_back(kIndObj); if (len != kIndObjDwords) problem("indirect object length"); break;
        case kAvpSegmentState: segment_at.push_back(i); order.push_back(kSegment); if (len != kSegmentStateDwords) { problem("segment state length"); return; } break;
        case kAvpPicState: have_pic = true; pic_at = i; order.push_back(kPic); if (len != kPicStateDwords) { problem("picture state length"); return; } break;
        case kAvpInloopFilterState: have_loop = true; loop_at = i; order.push_back(kInloop); if (len != kInloopFilterDwords) problem("loop filter state length"); break;
        case kAvpInterPredState: have_inter = true; inter_at = i; order.push_back(kInter); if (len != kInterPredDwords) problem("inter prediction state length"); break;
        case kAvpTileCoding: tile_at.push_back(i); order.push_back(kTile); if (len != kTileCodingDwords) { problem("tile coding length"); return; } break;
        case kAvpBsdObject: bsd_at.push_back(i); order.push_back(kBsd); if (len != kBsdObjectDwords) { problem("bsd object length"); return; } break;
        default: problem("an unknown command"); return;
      }
      i += len;
    }
    if (!have_mode || !have_buf || !have_ind || !have_pic || !have_loop || tile_at.empty() || tile_at.size() != bsd_at.size() || segment_at.empty() || !have_flush) { problem("incomplete AV1 batch"); return; }
    if (bits(d, pipe_mode, 1, 5, 7) != 2 || bits(d, pipe_mode, 1, 0, 0) != 0) problem("pipe mode: not AV1 decoding");
    // The order: modes, surfaces, buffers, bitstream, segments, picture, loop filter, (inter prediction), then the tiles in pairs.
    {
      std::vector<int> expect;
      size_t pos = 0;
      auto skip = [&](int kind) { while (pos < order.size() && order[pos] == kind) ++pos; };
      skip(kOther); skip(kModeSelect); skip(kOther); skip(kSurface);
      if (pos >= order.size() || order[pos] != kBufAddr) problem("order: buffer addresses");
      ++pos;
      if (pos >= order.size() || order[pos] != kIndObj) problem("order: indirect object");
      ++pos;
      skip(kSegment);
      if (pos >= order.size() || order[pos] != kPic) problem("order: picture state");
      ++pos;
      if (pos >= order.size() || order[pos] != kInloop) problem("order: loop filter state");
      ++pos;
      skip(kInter);
      for (size_t t = 0; t < tile_at.size(); ++t) {
        if (pos + 1 >= order.size() || order[pos] != kTile || order[pos + 1] != kBsd) { problem("order: tiles"); break; }
        pos += 2;
      }
    }

    // The frame, as the picture state gives it.
    const uint32_t width = bits(d, pic_at, 1, 0, 15) + 1, height = bits(d, pic_at, 1, 16, 31) + 1;
    const bool sb128 = bits(d, pic_at, 2, 7, 8) == 1;
    const uint32_t frame_type = bits(d, pic_at, 3, 16, 17);
    const bool intra = frame_type == 0 || frame_type == 2, key = frame_type == 0;
    const bool intrabc = bits(d, pic_at, 3, 23, 23) != 0;
    const uint32_t tile_columns = bits(d, tile_at[0], 5, 12, 21) + 1, tile_rows = bits(d, tile_at[0], 5, 22, 31) + 1;
    if (tile_columns * tile_rows != tile_at.size()) { problem("number of tiles"); return; }
    const uint32_t sb = sb128 ? 128 : 64;
    Av1Dimensions dims;
    dims.sb_wide = (width + sb - 1) / sb;
    dims.sb_high = (height + sb - 1) / sb;
    dims.sb128 = sb128;
    dims.tile_columns = tile_columns;
    dims.tiles = tile_columns * tile_rows;
    Av1BufferBytes sizes;
    if (!av1_buffer_bytes(dims, &sizes)) { problem("buffer sizes"); return; }

    // The surfaces.
    std::set<uint32_t> want_ids = {kSurfaceRecon};
    if (!intra) for (uint32_t id = kSurfaceIntra; id < kSurfaceIntra + 8; ++id) want_ids.insert(id);
    if (intrabc) want_ids.insert(kSurfaceIntrabc);
    if (surface_ids != want_ids) problem("surface states are not for the picture and its references");
    if (!pitch || pitch % 128 || !chroma_rows || chroma_rows % 32) { problem("surface layout"); return; }

    // The buffers are mapped and apart.
    const size_t b = buf_at;
    const uint64_t current = addr(d, b + 18), intrabc_buffer = addr(d, b + 24), cdf_init = addr(d, b + 27), cdf_bwd = addr(d, b + 30), seg_read = addr(d, b + 33), seg_write = addr(d, b + 36);
    const uint64_t mv_current = addr(d, b + 56);
    uint64_t refs[8], mv_refs[8], lines[kLineCount];
    for (int k = 0; k < 8; ++k) { refs[k] = addr(d, b + 1 + 2 * k); mv_refs[k] = addr(d, b + 39 + 2 * k); }
    static const uint8_t line_dword[kLineCount] = {62, 65, 68, 71, 74, 77, 80, 83, 86, 89, 92, 95, 98, 101, 104, 107, 110, 113, 116, 119, 128, 137, 140, 143, 146, 149, 152, 155, 158, 161, 164, 176, 179};
    for (uint32_t k = 0; k < kLineCount; ++k) lines[k] = addr(d, b + line_dword[k]);
    struct Range { uint64_t start, size; };
    std::vector<Range> ranges;
    auto mapped = [&](uint64_t a, uint64_t bytes, const char* name) {
      if (!a) { problem("a buffer is missing"); return false; }
      for (uint64_t off = 0; off < bytes; off += 4096) {
        bool ok;
        if (!gpu.ppgtt_bytes(a + off, 1, name, &ok) || !ok) { problem("a buffer in the commands is not mapped"); return false; }
      }
      bool ok;
      if (!gpu.ppgtt_bytes(a + bytes - 1, 1, name, &ok) || !ok) { problem("the end of a buffer is not mapped"); return false; }
      return true;
    };
    auto add = [&](uint64_t a, uint64_t bytes, const char* name) {
      if (!mapped(a, bytes, name)) return;
      for (const Range& r : ranges)
        if (a < r.start + r.size && r.start < a + bytes) problem("two buffers overlap");
      ranges.push_back({a, bytes});
    };
    const uint64_t surface_bytes = uint64_t{pitch} * chroma_rows * 3 / 2;   // (at least; the chroma plane is half the luma rows)
    mapped(current, surface_bytes, "current picture");
    ranges.push_back({current, surface_bytes});
    if (intrabc) add(intrabc_buffer, surface_bytes, "intrabc picture"); else if (intrabc_buffer) problem("an intrabc picture without intrabc");
    for (uint32_t k = 0; k < kLineCount; ++k) add(lines[k], sizes.line[k], "line buffer");
    add(cdf_init, intel_av1::kCdfContextBytes, "cdf init");
    add(mv_current, sizes.motion_vector, "motion vectors");
    {
      // The tables the frame leaves, and the map it codes, are in the buffer of its own picture.
      bool writes_cdf = false;
      for (size_t t : tile_at) writes_cdf = writes_cdf || bits(d, t, 4, 31, 31) == 0;
      if (writes_cdf) add(cdf_bwd, intel_av1::kCdfContextBytes, "cdf backward");
      else if (cdf_bwd && !mapped(cdf_bwd, intel_av1::kCdfContextBytes, "cdf backward")) {}
    }
    if (seg_write) add(seg_write, sizes.segment_id, "segment ids");
    if (seg_read) mapped(seg_read, sizes.segment_id, "segment ids read");
    for (int k = 0; k < 8; ++k) {
      if (refs[k] && !intra && !mapped(refs[k], surface_bytes, "reference picture")) {}
      if (mv_refs[k] && !mapped(mv_refs[k], sizes.motion_vector, "reference motion vectors")) {}
    }
    const uint64_t bitstream = addr(d, ind_at + 1);
    uint64_t bitstream_end = 0;
    for (size_t t = 0; t < bsd_at.size(); ++t) bitstream_end = std::max<uint64_t>(bitstream_end, uint64_t{d[bsd_at[t] + 2]} + d[bsd_at[t] + 1]);
    add(bitstream, bitstream_end, "bitstream");
    if (intra && key) for (int k = 0; k < 8; ++k) if (refs[k] || mv_refs[k]) problem("a key frame has reference buffers");

    const size_t k = next++;
    const FrameRecord* rec = reference && check_content && k < reference->frames.size() ? &reference->frames[k] : nullptr;
    if (rec) {
      const av1::FrameHeader& hdr = rec->hdr;
      const av1::SequenceHeader& seq = rec->seq;
      if (width != uint32_t(rec->width) || height != uint32_t(rec->height)) problem("frame size");
      Nv12TiledLayout layout;
      if (!nv12_tiled_layout(align8(uint32_t(seq.max_width)), align8(uint32_t(seq.max_height)), &layout) || layout.pitch != pitch || layout.chroma_offset_rows != chroma_rows) problem("surface layout against the sequence");
      // The frame state.
      if (frame_type != uint32_t(hdr.frame_type) || bits(d, pic_at, 3, 22, 22) != hdr.error_resilient_mode || intrabc != (hdr.allow_intrabc != 0) || bits(d, pic_at, 3, 28, 30) != hdr.primary_ref_frame) problem("frame type, error resilience, intrabc or primary reference");
      if (bits(d, pic_at, 4, 16, 23) != hdr.quant.yac || bits(d, pic_at, 4, 10, 10) != uint32_t(hdr.all_lossless != 0)) problem("quantizer or lossless flag");
      if (bits(d, pic_at, 4, 0, 0) != hdr.segmentation.enabled || bits(d, pic_at, 4, 1, 1) != hdr.segmentation.update_map) problem("segmentation flags");
      if (bits(d, pic_at, 2, 9, 9) != seq.order_hint || bits(d, pic_at, 3, 2, 2) != hdr.warp_motion || bits(d, pic_at, 6, 0, 0) != hdr.hp || bits(d, pic_at, 6, 2, 4) != uint32_t(hdr.subpel_filter_mode)) problem("order hints, warped motion, precision or interpolation");
      if (bits(d, pic_at, 7, 1, 2) != uint32_t(hdr.txfm_mode) || bits(d, pic_at, 7, 4, 4) != hdr.skip_mode_enabled || bits(d, pic_at, 6, 1, 1) != hdr.switchable_comp_refs) problem("transform or reference mode");
      if (bits(d, pic_at, 6, 16, 23) != (seq.order_hint ? hdr.frame_offset : 0u)) problem("order hint");
      const uint32_t uv_dc = bits(d, pic_at, 5, 0, 6);
      if (uv_dc != (uint32_t(hdr.quant.udc_delta) & 127)) problem("chroma quantizer deltas");
      for (int r = 0; r < 7; ++r)
        if (bits(d, pic_at, 8, 3 + 3 * r, 4 + 3 * r) != uint32_t(hdr.gmv[r].type)) { problem("global motion types"); break; }
      // The references: the surfaces of the frames the slots hold, at the sizes those frames had, and the order hints.
      if (!intra) {
        if (refs[0] != current) problem("the intra reference is not the picture itself");
        for (int r = 0; r < 7; ++r) {
          const int want = rec->expected_ref[r];
          auto it = address_of.find(want);
          if (want < 0 || it == address_of.end() || refs[r + 1] != it->second || painted[refs[r + 1]] != want) { problem("a reference picture is not the frame its slot holds"); continue; }
          const FrameRecord& rf = reference->frames[size_t(want)];
          if (bits(d, pic_at, 32 + r, 0, 15) != uint32_t(rf.width) - 1 || bits(d, pic_at, 32 + r, 16, 31) != uint32_t(rf.height) - 1) problem("reference size");
          const uint32_t hint = seq.order_hint ? rf.hdr.frame_offset : 0u;
          if (bits(d, pic_at, 47 + (r + 1) / 4, 8 * ((r + 1) % 4), 8 * ((r + 1) % 4) + 7) != hint) problem("reference order hint");
          if (seq.order_hint) {
            const int dist = av1::get_poc_diff(seq.order_hint_n_bits, int(rf.hdr.frame_offset), int(hdr.frame_offset));
            if (bits(d, pic_at, 6, 8 + r + 1, 8 + r + 1) != uint32_t(dist > 0)) problem("sign bias");
            if (bits(d, pic_at, 7, 24 + r + 1, 24 + r + 1) != uint32_t(dist > 0 || rf.hdr.frame_offset == hdr.frame_offset)) problem("reference side");
          }
          if (bits(d, pic_at, 40 + r, 16, 31) != (uint32_t(rf.width) * 16384 + uint32_t(rec->width) / 2) / uint32_t(rec->width) ||
              bits(d, pic_at, 40 + r, 0, 15) != (uint32_t(rf.height) * 16384 + uint32_t(rec->height) / 2) / uint32_t(rec->height))
            problem("reference scale");
          // The collocated motion vectors are those the reference's own decode left.
          if (mv_refs[r + 1] != mv_of[want]) problem("collocated motion vectors are not the reference's");
          // The order hints it was decoded with, in the inter prediction state.
          if (seq.order_hint) {
            if (!have_inter) { problem("no inter prediction state"); }
            else {
              for (int j = 0; j < 7; ++j) {
                const uint32_t group = uint32_t(r) * 2 + (j >= 4 ? 1 : 0), slot = uint32_t(j >= 4 ? j - 4 : j);
                const int rr = rf.expected_ref[j];
                const uint32_t saved = rr >= 0 ? reference->frames[size_t(rr)].hdr.frame_offset : 0u;
                if (rf.hdr.frame_type == av1::FRAME_TYPE_INTER && rr >= 0 && bits(d, inter_at, 1 + group, 8 * slot, 8 * slot + 7) != saved) { problem("saved order hints"); r = 7; break; }
              }
            }
          }
        }
        if (mv_refs[0] != mv_current) problem("the intra motion vector buffer is not the frame's own");
        if (hdr.skip_mode_allowed && hdr.switchable_comp_refs && seq.order_hint &&
            (bits(d, pic_at, 7, 5, 7) != uint32_t(hdr.skip_mode_refs[0]) + 1 || bits(d, pic_at, 7, 9, 11) != uint32_t(hdr.skip_mode_refs[1]) + 1))
          problem("skip mode frames");
      }
      // Segments.
      const size_t want_segments = hdr.segmentation.enabled ? 8 : 1;
      if (segment_at.size() != want_segments) problem("number of segment states");
      for (size_t s = 0; s < segment_at.size() && s < want_segments; ++s) {
        const av1::SegmentationData& sd = hdr.segmentation.seg_data.d[s];
        if (bits(d, segment_at[s], 1, 0, 2) != s) problem("segment id");
        if (bits(d, segment_at[s], 2, 8, 16) != (uint32_t(int32_t(sd.delta_q)) & 511) || bits(d, segment_at[s], 2, 17, 17) != uint32_t(sd.skip != 0) || bits(d, segment_at[s], 2, 19, 19) != hdr.segmentation.lossless[s]) problem("segment quantizer, skip or lossless");
        const uint32_t qm = hdr.quant.qm && !hdr.segmentation.lossless[s] ? hdr.quant.qm_y : 15u;
        if (bits(d, segment_at[s], 2, 20, 23) != qm) problem("segment quantizer matrix level");
        if (bits(d, segment_at[s], 3, 0, 6) != (uint32_t(int32_t(sd.delta_lf_y_v)) & 127) || bits(d, segment_at[s], 3, 14, 20) != (uint32_t(int32_t(sd.delta_lf_u)) & 127)) problem("segment loop filter deltas");
      }
      // The loop filters.
      if (bits(d, loop_at, 1, 0, 5) != hdr.loopfilter.level_y[0] || bits(d, loop_at, 1, 6, 11) != hdr.loopfilter.level_y[1] || bits(d, loop_at, 1, 12, 17) != hdr.loopfilter.level_u ||
          bits(d, loop_at, 1, 18, 23) != hdr.loopfilter.level_v || bits(d, loop_at, 1, 24, 26) != hdr.loopfilter.sharpness)
        problem("loop filter levels");
      for (int s = 0; s < 4; ++s)
        if (bits(d, loop_at, 5, 6 * s, 6 * s + 5) != hdr.cdef.y_strength[s] || bits(d, loop_at, 7, 6 * s, 6 * s + 5) != hdr.cdef.uv_strength[s]) { problem("cdef strengths"); break; }
      if (bits(d, loop_at, 9, 0, 15) != uint32_t(rec->width) - 1 || bits(d, loop_at, 9, 16, 20) != 8) problem("upscaled width or denominator");
      const uint32_t lr_codes[4] = {0, 3, 1, 2};
      if (bits(d, loop_at, 10, 0, 1) != lr_codes[hdr.restoration.type[0]] || bits(d, loop_at, 10, 2, 3) != lr_codes[hdr.restoration.type[1]] || bits(d, loop_at, 10, 4, 5) != lr_codes[hdr.restoration.type[2]]) problem("loop restoration types");
      // The tiles, and their bytes in the bitstream.
      uint32_t total = 0;
      for (size_t t = 0; t < tile_at.size(); ++t) {
        const uint32_t col = uint32_t(t) % hdr.tiling.cols, row = uint32_t(t) / hdr.tiling.cols;
        if (bits(d, tile_at[t], 1, 0, 11) != t || bits(d, tile_at[t], 2, 0, 9) != hdr.tiling.col_start_sb[col] || bits(d, tile_at[t], 2, 16, 25) != hdr.tiling.row_start_sb[row] ||
            bits(d, tile_at[t], 3, 0, 5) + 1 != uint32_t(hdr.tiling.col_start_sb[col + 1] - hdr.tiling.col_start_sb[col]) || bits(d, tile_at[t], 3, 16, 25) + 1 != uint32_t(hdr.tiling.row_start_sb[row + 1] - hdr.tiling.row_start_sb[row]))
          problem("tile position");
        if (bits(d, tile_at[t], 4, 29, 29) != uint32_t(t + 1 == tile_at.size()) || bits(d, tile_at[t], 4, 27, 27) != uint32_t(t == 0) || bits(d, tile_at[t], 4, 30, 30) != hdr.disable_cdf_update) problem("tile flags");
        const bool update = hdr.refresh_context && t == hdr.tiling.update;
        if (bits(d, tile_at[t], 4, 31, 31) != uint32_t(!update)) problem("which tile updates the contexts");
        if (d[bsd_at[t] + 1] != rec->tile_sizes[t] || d[bsd_at[t] + 2] != total) problem("tile object");
        std::vector<uint8_t> got;
        if (!read_bytes(gpu, bitstream + d[bsd_at[t] + 2], d[bsd_at[t] + 1], &got) || !std::equal(got.begin(), got.end(), rec->tiles.begin() + total)) problem("a tile in the bitstream differs");
        total += rec->tile_sizes[t];
      }
      // The entropy contexts the engine starts from, and what it leaves.
      std::vector<uint8_t> expected(intel_av1::kCdfContextBytes), have;
      bool known = true;
      if (hdr.primary_ref_frame == av1::PRIMARY_REF_NONE || intra) {
        known = intel_av1::build_default_cdf_context(intel_av1::default_context_for(hdr.quant.yac), ByteView(expected.data(), expected.size()));
      } else {
        const int prim = rec->expected_ref[hdr.primary_ref_frame];
        known = prim >= 0 && saved_cdf.count(prim);
        if (known) expected = saved_cdf[prim];
      }
      if (known) {
        if (!read_bytes(gpu, cdf_init, expected.size(), &have) || have != expected) problem("the entropy contexts the frame starts from");
        saved_cdf[long(k)] = hdr.refresh_context ? std::vector<uint8_t>() : expected;
        if (hdr.refresh_context) {
          std::vector<uint8_t> after(intel_av1::kCdfContextBytes);
          for (uint32_t o = 0; o < after.size(); ++o) after[o] = cdf_pattern(long(k), o);
          saved_cdf[long(k)] = after;
        }
      }
      // The segmentation map.
      const bool seg = hdr.segmentation.enabled != 0;
      const bool reads = seg && (hdr.segmentation.temporal || !hdr.segmentation.update_map);
      long source = -1;
      if (seg && hdr.segmentation.update_map) {
        source = long(k);
      } else if (reads && !intra && hdr.primary_ref_frame != av1::PRIMARY_REF_NONE) {
        const int prim = rec->expected_ref[hdr.primary_ref_frame];
        if (prim >= 0 && seg_source.count(prim) && seg_source[prim] >= 0 && reference->frames[size_t(prim)].width == rec->width && reference->frames[size_t(prim)].height == rec->height) source = seg_source[prim];
      }
      const bool in = reads && source >= 0;
      if (bits(d, pic_at, 4, 12, 12) != uint32_t(in) || bits(d, pic_at, 4, 13, 13) != uint32_t(seg && hdr.segmentation.update_map) || bits(d, pic_at, 4, 11, 11) != uint32_t(reads && !in)) problem("segment map flags");
      if (in) {
        std::vector<uint8_t> map;
        if (!read_bytes(gpu, seg_read, 64, &map)) problem("the segment map is not readable");
        else
          for (uint32_t o = 0; o < 64; ++o)
            if (map[o] != seg_pattern(source, o)) { problem("the segment map the frame reads is not its primary reference's"); break; }
      }
      seg_source[long(k)] = source;
      mv_of[long(k)] = mv_current;
    }
    if (!rec) mv_of[long(k)] = mv_current;
    // The engine "adapts" and "codes the map".
    for (size_t t : tile_at)
      if (bits(d, t, 4, 31, 31) == 0 && cdf_bwd && !write_pattern(gpu, cdf_bwd, intel_av1::kCdfContextBytes, long(k), cdf_pattern)) problem("the contexts cannot be written");
    if (seg_write && !write_pattern(gpu, seg_write, 64, long(k), seg_pattern)) problem("the segment map cannot be written");

    // "Decode": paint the output picture with the CPU decoder's, tiled.
    uint8_t* page = nullptr;
    uint64_t page_index = ~0ull;
    auto put = [&](uint64_t offset, uint8_t value) {
      if (offset / 4096 != page_index) {
        bool ok;
        page = gpu.ppgtt_bytes(current + (offset / 4096) * 4096, 4096, "output surface", &ok);
        page_index = offset / 4096;
        if (!ok) page = nullptr;
      }
      if (page) page[offset % 4096] = value;
    };
    if (rec) {
      Nv12TiledLayout layout;
      if (nv12_tiled_layout(align8(uint32_t(rec->seq.max_width)), align8(uint32_t(rec->seq.max_height)), &layout)) {
        for (uint64_t off = 0; off < layout.size; ++off) put(off, 0xEE);
        const uint32_t cw = uint32_t(rec->width + 1) / 2, ch = uint32_t(rec->height + 1) / 2;
        for (int y = 0; y < rec->height; ++y)
          for (int x = 0; x < rec->width; ++x) put(y_tile_offset(layout.pitch, uint32_t(x), uint32_t(y)), rec->plane[0][size_t(y) * size_t(rec->width) + size_t(x)]);
        for (uint32_t y = 0; y < ch; ++y)
          for (uint32_t x = 0; x < cw; ++x) {
            put(y_tile_offset(layout.pitch, 2 * x, layout.chroma_offset_rows + y), rec->plane[1][size_t(y) * cw + x]);
            put(y_tile_offset(layout.pitch, 2 * x + 1, layout.chroma_offset_rows + y), rec->plane[2][size_t(y) * cw + x]);
          }
        painted[current] = long(k);
        address_of[long(k)] = current;
      }
    }
    ++frames;
  }
};

struct Rig2 : Rig {
  FakeDecoder engine;
  Rig2() : Rig(Generation::Gen12) { gpu.on_batch = [this](FakeGpu& g, const std::vector<uint32_t>& d) { engine.run(g, d); }; }
};

static DecoderConfig i420_config() {
  DecoderConfig c;
  c.codec = VideoCodec::Av1;
  c.output = PixelFormat::I420;
  return c;
}

// A stream the engine takes (`taken`) or one the CPU keeps whole.
static void compare_stream(const char* name, bool taken) {
  printf("  %s\n", name);
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference_file(a, name, &ref));
    if (ref.ends.empty()) return;
    printf("    %zu temporal units in, %zu frames decoded, %zu out\n", ref.ends.size(), ref.frames.size(), ref.out.pts.size());
    Rig2 rig;
    rig.engine.reference = &ref;
    {
      VcsDriver driver(rig.mem);
      CHECK(driver.initialize(rig.config()));
      {
        IntelAv1Decoder session(a, driver, i420_config());
        CHECK(session.prepare());
        Output out;
        for (size_t k = 0; k < ref.ends.size(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
        session.flush();
        drain(session, &out);
        CHECK(session.statistics().errors == 0 && session.statistics().frames == ref.out.pts.size());
        CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
        if (taken) CHECK(rig.engine.frames == int(ref.frames.size()) && session.frames_decoded() == ref.frames.size());
        else CHECK(rig.engine.frames == 0 && session.frames_decoded() == 0);
        // The same pictures, in the same order, byte for byte.
        CHECK(out.pts == ref.out.pts);
        for (size_t i = 0; i < out.bytes.size() && i < ref.out.pts.size(); ++i) {
          CHECK(out.format[i] == ref.out.format[i]);
          CHECK(out.bytes[i] == ref.out.bytes[i]);
        }
        CHECK(!out.format.empty() && session.output_format().width == out.format.back().width && session.output_format().height == out.format.back().height);
      }
    }
    CHECK(rig.gpu.violations.empty());
    CHECK(rig.mem.live() == 1);   // only the scratch page that is never freed
  }
  CHECK(a.live == 0);
}

static void test_lease() {
  TestAllocator a;
  Rig2 rig;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    {
      IntelAv1Decoder first(a, driver, i420_config());
      CHECK(first.prepare());
      IntelAv1Decoder second(a, driver, i420_config());
      CHECK(!second.prepare());
    }
    IntelAv1Decoder third(a, driver, i420_config());
    CHECK(third.prepare());
  }
  CHECK(a.live == 0);
}

// The engine stops answering in the middle of a stream.
static void test_hang() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference_file(a, "inter", &ref));
  Rig2 rig;
  rig.engine.reference = &ref;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    int live_with_session = 0;
    {
      IntelAv1Decoder session(a, driver, i420_config());
      CHECK(session.prepare());
      Output out;
      CHECK(feed(session, ref.unit(0), 0, &out));
      CHECK(rig.engine.frames == 1);
      rig.gpu.hang = true;
      CHECK(feed(session, ref.unit(1), 1000, &out));
      CHECK(driver.state() == VcsDriver::State::Wedged && session.statistics().errors >= 1);
      const int frames = rig.engine.frames;
      CHECK(feed(session, ref.unit(2), 2000, &out));
      CHECK(rig.engine.frames == frames);
      session.flush();
      drain(session, &out);
      CHECK(out.bytes.size() >= 3 && out.bytes[0] == ref.out.bytes[0]);
      if (out.bytes.size() >= 3) CHECK(out.bytes[1][0] == 128 && out.bytes[2][0] == 128);
      live_with_session = rig.mem.live();
    }
    CHECK(rig.mem.live() == live_with_session);
  }
  CHECK(rig.gpu.violations.empty());
  CHECK(a.live == 0);
}

// Damaged streams: temporal units cut short and bits flipped. The engine is never pointed at anything unmapped or freed, nothing leaks.
static void test_damaged_streams() {
  TestAllocator a;
  for (const char* name : {"inter", "segments", "tiles", "sb128", "screen"}) {
    Reference ref;
    CHECK(make_reference_file(a, name, &ref));
    int runs = 0;
    for (size_t variant = 0; variant < 24; ++variant) {
      Rig2 rig;
      rig.engine.reference = &ref;
      rig.engine.check_content = false;
      {
        VcsDriver driver(rig.mem);
        CHECK(driver.initialize(rig.config()));
        {
          IntelAv1Decoder session(a, driver, i420_config());
          CHECK(session.prepare());
          Output out;
          for (size_t k = 0; k < ref.ends.size(); ++k) {
            std::vector<uint8_t> au(ref.unit(k).data(), ref.unit(k).data() + ref.unit(k).size());
            if (variant > 0 && k > 0 && !au.empty() && variant % 2 == 1) au.resize(au.size() * (variant + 1) / 25 + 1);
            else if (variant > 0 && k > 0 && !au.empty()) au[(variant * 7919 + k * 104729) % au.size()] ^= uint8_t(1u << (variant % 8));
            VideoPacket p;
            p.data = ConstByteView(au.data(), au.size());
            p.pts_us = int64_t(k) * 1000;
            if (session.submit(p) == SubmitResult::Full) { drain(session, &out); session.submit(p); }
            drain(session, &out);
            ++runs;
          }
          session.flush();
          drain(session, &out);
        }
      }
      CHECK(rig.gpu.violations.empty());
    }
    CHECK(runs >= 24 * 3);
  }
  CHECK(a.live == 0);
}

// The device: an integrated Xe-LP part runs AV1 sessions on the engine, one at a time, everything else stays on the CPU.
static void test_device() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference_file(a, "inter", &ref));
  Rig2 rig;
  rig.engine.reference = &ref;
  std::vector<uint32_t> regs(0x20000 / 4, 0);
  regs[0x0D50 / 4] = 1;   // the first video box answers the power request
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
    CHECK(device.hardware_decode() && device.info().accelerated_decode_codecs == codec_mask(VideoCodec::Av1));
    {
      UniquePointer<IVideoDecoder> hw = device.create_decoder(i420_config());
      CHECK(hw != nullptr);
      Output out;
      for (size_t k = 0; k < ref.ends.size(); ++k) CHECK(feed(*hw, ref.unit(k), int64_t(k) * 1000, &out));
      hw->flush();
      drain(*hw, &out);
      CHECK(out.pts == ref.out.pts && out.bytes == ref.out.bytes);
      CHECK(rig.engine.frames == int(ref.frames.size()));
      // The engine is taken: the next session runs on the CPU and sends nothing to it.
      UniquePointer<IVideoDecoder> sw = device.create_decoder(i420_config());
      CHECK(sw != nullptr);
      Output out2;
      for (size_t k = 0; k < ref.ends.size(); ++k) CHECK(feed(*sw, ref.unit(k), int64_t(k) * 1000, &out2));
      sw->flush();
      drain(*sw, &out2);
      CHECK(out2.bytes == ref.out.bytes && rig.engine.frames == int(ref.frames.size()));
    }
    // Another codec never reaches the engine.
    DecoderConfig h264;
    h264.codec = VideoCodec::H264;
    h264.output = PixelFormat::I420;
    UniquePointer<IVideoDecoder> other = device.create_decoder(h264);
    CHECK(other != nullptr);
  }
  CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
  CHECK(a.live == 0);
}

int main() {
  // Streams the engine takes: 8-bit 4:2:0 without film grain or super-resolution.
  static const char* const taken[] = {"intra", "inter", "tiles", "sb128", "lossless", "qm", "rav1e", "screen", "segments", "svt", "resize"};
  // Streams the CPU keeps: 10- and 12-bit, 4:2:2, 4:4:4, monochrome, film grain, super-resolution.
  static const char* const kept[] = {"10bit", "12bit", "422", "444", "mono", "grain", "superres"};
  for (const char* name : taken)
    if (!getenv("ONLY") || !strcmp(getenv("ONLY"), name)) compare_stream(name, true);
  if (!getenv("ONLY")) {
    for (const char* name : kept) compare_stream(name, false);
    test_lease();
    test_hang();
    test_damaged_streams();
    test_device();
  }
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("IntelAv1SessionTest passed\n");
  return 0;
}
