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

// Host-side tests for the Intel hardware VP9 decode session (IntelVp9Decoder) against a simulated GPU. The simulated engine plays the decode hardware: for every batch it
// reads the HCP commands the way the engine would, checks that every address in them is mapped in the context's address space and that the buffers do not overlap, that
// the frame state says what the frame header says (compared with what a separate CPU decoder reports for the same frame), that each segment has its state with the quantizer
// and loop filter levels the header works out, that the reference pictures are the surfaces of the frames the reference slots hold, that the bitstream holds the frame behind
// its uncompressed header, and that the probability buffer the engine is given is the one the driver's rules make from the saved contexts (the stand-in "adapts" by leaving
// a pattern of its own in the buffer, which later frames must then find where the rules put it); then it "decodes" by writing the picture the CPU decoder made for the same
// frame into the output surface, tiled as the engine does. The session must hand back exactly the CPU decoder's pictures, in the same order.
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
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/Intel/IntelVp9Session.hpp"
#include "Hardware/VideoCodec/Software/Vp9.hpp"

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

// The packets of an IVF file, back to back in `stream`, with where each one ends.
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

// The CPU decoder's view of a stream: for every frame it decodes (hidden ones too) the description an engine is given and the picture it makes.
struct FrameRecord {
  vp9::OffloadFrame of;                    // (its views are not kept: see the vectors)
  std::vector<uint8_t> data;
  std::vector<uint8_t> plane[3];           // the decoded picture, 8-bit, cropped
  int expected_ref[3] = {-1, -1, -1};      // the decode index of the frames the references name
};

struct Reference final : vp9::IFrameObserver {
  std::vector<uint8_t> stream;
  std::vector<size_t> ends;
  std::vector<FrameRecord> frames;
  Output out;
  int held[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // the decode index of the frame each reference slot holds

  ConstByteView unit(size_t k) const {
    const size_t start = k == 0 ? 0 : ends[k - 1];
    return ConstByteView(stream.data() + start, ends[k] - start);
  }

  void frame_decoded(const vp9::OffloadFrame& f) override {
    FrameRecord r;
    r.of = f;
    r.of.data = ConstByteView();
    for (int p = 0; p < 3; ++p) {
      r.of.plane[p] = ArrayView<uint16_t>();
      for (int y = 0; y < f.plane_height[p]; ++y)
        for (int x = 0; x < f.plane_width[p]; ++x) r.plane[p].push_back(static_cast<uint8_t>(f.plane[p][uint64_t(y) * f.stride[p] + uint64_t(x)]));
    }
    for (int i = 0; i < 3; ++i) r.expected_ref[i] = held[f.ref_slot[i] & 7];
    r.data.assign(f.data.data(), f.data.data() + f.data.size());
    const int id = int(frames.size());
    for (int i = 0; i < 8; ++i)
      if (f.refresh_frame_flags & (1 << i)) held[i] = id;
    frames.push_back(static_cast<FrameRecord&&>(r));
  }
};

static bool make_reference(allocators::IAllocator& a, Reference* ref) {
  Vp9Decoder dec(a);
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
  snprintf(path, sizeof path, "tests/data/vp9_%s.ivf", name);
  return read_ivf(read_file(path), &ref->stream, &ref->ends) && make_reference(a, ref);
}

static uint32_t align8(uint32_t v) { return (v + 7) & ~7u; }

// The pattern the stand-in leaves in the probability buffer after frame k (what it "adapted").
static uint8_t pattern(long k, uint32_t i) { return uint8_t(k * 131 + i * 7 + 1); }

// The decode engine of the simulated GPU.
struct FakeDecoder {
  const Reference* reference = nullptr;
  bool check_content = true;
  std::map<uint64_t, long> painted;               // surface address -> the frame it last held
  std::map<long, uint64_t> address_of;            // frame -> the surface it was painted into
  std::map<long, std::pair<uint32_t, uint32_t>> size_of;
  int frames = 0, problems = 0;
  size_t next = 0;
  // The saved probability contexts as the rules make them, by where each part came from (-1: the defaults, k: the output of frame k).
  struct Ctx { long first = -1, inter = -1; };
  Ctx ctx[4];
  struct Last { bool valid = false, refresh = false, intra = false; uint32_t idx = 0; long frame = -1; } last;
  std::vector<uint8_t> default_first, default_inter, key_inter;   // the defaults, as the first frames that start from them show them

  static uint64_t addr(const std::vector<uint32_t>& d, size_t at) { return (uint64_t{d[at + 1]} << 32) | d[at]; }
  void problem(const char* what) {
    ++problems;
    printf("  fake engine: %s\n", what);
  }

  void run(FakeGpu& gpu, const std::vector<uint32_t>& d) {
    uint32_t codec = 99, pitch = 0, pic[12] = {};
    uint32_t surface_ids = 0;
    uint64_t cur = 0, lines[6] = {}, mv_cur = 0, refs[8] = {}, mv_refs[8] = {}, prob = 0, segment_id = 0, hvd[2] = {}, bitstream = 0;
    bool have_pic = false, have_buf = false;
    struct Segment { uint32_t dw[7]; };
    std::vector<Segment> segments;
    uint32_t bsd_size = 0, bsd_offset = 0, bsd_count = 0;
    size_t i = 0;
    while (i < d.size()) {
      const uint32_t h = d[i];
      if (h == 0) { ++i; continue; }
      if ((h >> 23) == 0x0A) break;
      if ((h >> 29) == 0) { i += (h & 0x3F) + 2; continue; }
      const size_t len = (h & 0xFF) + 2;
      if (i + len > d.size()) { problem("command runs past the batch"); return; }
      switch (h & 0xFFFF0000) {
        case kHcpPipeModeSelect: codec = (d[i + 1] >> 5) & 3; if (len != 6) problem("pipe mode select length"); break;
        case kHcpSurfaceState:
          if (len != 3 || ((d[i + 2] >> 28) != 4)) problem("surface state");
          surface_ids |= 1u << (d[i + 1] >> 28);
          pitch = (d[i + 1] & 0x1FFFF) + 1;
          break;
        case kHcpPipeBufAddrState:
          if (len != 95) { problem("pipe buffer address length"); return; }
          cur = addr(d, i + 1);
          for (int k = 0; k < 6; ++k) lines[k] = addr(d, i + 4 + 3 * k);
          mv_cur = addr(d, i + 31);
          for (int k = 0; k < 8; ++k) { refs[k] = addr(d, i + 37 + 2 * k); mv_refs[k] = addr(d, i + 66 + 2 * k); }
          prob = addr(d, i + 83);
          segment_id = addr(d, i + 86);
          hvd[0] = addr(d, i + 89);
          hvd[1] = addr(d, i + 92);
          for (int k = 22; k < 31; k += 3) if (addr(d, i + k)) problem("an SAO buffer in a VP9 batch");
          have_buf = true;
          break;
        case kHcpIndObjBaseAddrState: bitstream = addr(d, i + 1); if (len != 14) problem("indirect object length"); break;
        case kHcpVp9SegmentState: {
          if (len != 7) { problem("segment state length"); return; }
          Segment s;
          for (int k = 0; k < 7; ++k) s.dw[k] = d[i + k];
          segments.push_back(s);
          break;
        }
        case kHcpVp9PicState: if (len == 12) { for (int k = 0; k < 12; ++k) pic[k] = d[i + k]; have_pic = true; } else problem("picture state length"); break;
        case kHcpBsdObject: bsd_size = d[i + 1]; bsd_offset = d[i + 2]; ++bsd_count; break;
        default: break;
      }
      i += len;
    }
    if (codec != 1 || !have_pic || !have_buf || bsd_count != 1 || segments.empty()) { problem("incomplete VP9 batch"); return; }
    if (surface_ids != 0x1D) problem("surface states are not for the picture and its three references");
    const uint32_t width = (pic[1] & 0xFFFF) + 1, height = (pic[1] >> 16) + 1;
    Nv12TiledLayout layout;
    if (!nv12_tiled_layout(width, height, &layout) || layout.pitch != pitch) { problem("surface layout"); return; }

    // The buffers are mapped and apart.
    const Vp9BufferBytes sizes = vp9_buffer_bytes(width, height);
    struct Range { uint64_t start, size; };
    std::vector<Range> ranges;
    auto add = [&](uint64_t a, uint64_t bytes, const char* name) {
      if (!a) { problem("a buffer is missing"); return; }
      for (uint64_t off = 0; off < bytes; off += 4096) {
        bool ok;
        if (!gpu.ppgtt_bytes(a + off, 1, name, &ok) || !ok) { problem("a buffer in the commands is not mapped"); return; }
      }
      bool ok;
      if (!gpu.ppgtt_bytes(a + bytes - 1, 1, name, &ok) || !ok) { problem("the end of a buffer is not mapped"); return; }
      for (const Range& r : ranges)
        if (a < r.start + r.size && r.start < a + bytes) problem("two buffers overlap");
      ranges.push_back({a, bytes});
    };
    add(cur, layout.size, "current picture");
    const uint64_t line_sizes[6] = {sizes.deblocking_line, sizes.deblocking_tile_line, sizes.deblocking_tile_column, sizes.metadata_line, sizes.metadata_tile_line, sizes.metadata_tile_column};
    for (int k = 0; k < 6; ++k) add(lines[k], line_sizes[k], "line buffer");
    add(mv_cur, sizes.motion_vector, "motion vectors");
    add(mv_refs[0], sizes.motion_vector, "last motion vectors");
    add(prob, sizes.probability, "probabilities");
    add(segment_id, sizes.segment_id, "segment ids");
    add(hvd[0], sizes.hvd_line, "hvd line");
    add(hvd[1], sizes.hvd_tile, "hvd tile");
    add(bitstream, bsd_offset + bsd_size, "bitstream");
    for (int k = 1; k < 8; ++k) if (mv_refs[k]) problem("more than one collocated motion vector buffer");
    for (int k = 3; k < 8; ++k) if (refs[k]) problem("a fifth reference picture");

    const size_t k = next++;
    const FrameRecord* rec = reference && check_content && k < reference->frames.size() ? &reference->frames[k] : nullptr;
    const vp9::OffloadFrame* of = rec ? &rec->of : nullptr;
    if (rec) {
      const bool key = of->info.key_frame, intra_only = of->info.intra_only, intra = key || intra_only;
      if (width != align8(uint32_t(of->info.width)) || height != align8(uint32_t(of->info.height))) problem("frame size");
      // The frame state.
      if (((pic[2] >> 17) & 63) != uint32_t(of->filter_level) || ((pic[2] >> 23) & 7) != uint32_t(of->sharpness_level)) problem("loop filter level or sharpness");
      if (bool((pic[2] >> 15) & 1) != of->error_resilient || bool((pic[2] >> 16) & 1) != of->frame_parallel || bool((pic[2] >> 14) & 1) != of->refresh_frame_context) problem("error resilience, parallel mode or context refresh");
      if (bool(pic[2] & 1) != !key || bool((pic[2] >> 2) & 1) != intra_only || bool((pic[2] >> 3) & 1) != of->hp_mv || ((pic[2] >> 4) & 7) != uint32_t(of->interp_filter)) problem("frame type, intra only, precision or interpolation");
      if (bool((pic[2] >> 1) & 1) != !(of->error_resilient || of->frame_parallel)) problem("probability adaptation flag");
      if (bool((pic[2] >> 26) & 1) != of->seg.enabled || bool((pic[2] >> 30) & 1) != (of->seg.enabled && of->seg.update_map) || bool((pic[2] >> 27) & 1) != (of->seg.enabled && of->seg.update_map)) problem("segmentation flags");
      if (bool((pic[2] >> 29) & 1) != (of->base_qindex == 0 && of->y_dc_delta_q == 0 && of->uv_dc_delta_q == 0 && of->uv_ac_delta_q == 0)) problem("lossless flag");
      if (bool((pic[2] >> 7) & 1) != (of->sign_bias[0] != 0) || bool((pic[2] >> 8) & 1) != (of->sign_bias[1] != 0) || bool((pic[2] >> 9) & 1) != (of->sign_bias[2] != 0)) problem("sign bias");
      if (((pic[3] >> 28) & 15) != 0 || ((pic[3] >> 8) & 3) != uint32_t(of->log2_tile_rows) || (pic[3] & 15) != uint32_t(of->log2_tile_cols)) problem("profile or tiles");
      if ((pic[10] >> 16) != of->compressed_header_bytes || (pic[10] & 0xFFFF) != of->uncompressed_header_bytes) problem("header sizes");
      if (intra && ((pic[2] >> 31) & 1) == 0) problem("an intra frame reads the segment map");
      // The references: the surfaces of the frames the slots hold, at the sizes those frames had.
      for (int r = 0; r < 3; ++r) {
        if (intra) {
          if (refs[r]) problem("an intra frame has reference pictures");
          continue;
        }
        const int want = rec->expected_ref[r];
        auto it = address_of.find(want);
        if (want < 0 || it == address_of.end() || refs[r] != it->second || painted[refs[r]] != want) { problem("a reference picture is not the frame its slot holds"); continue; }
        const uint32_t rw = size_of[want].first, rh = size_of[want].second;
        if ((pic[7 + r] & 0x3FFF) != rw - 1 || ((pic[7 + r] >> 16) & 0x3FFF) != rh - 1) problem("reference size");
        const bool same = rw == uint32_t(of->info.width) && rh == uint32_t(of->info.height);
        if (same && pic[4 + r] != ((1u << 14) << 16 | (1u << 14))) problem("scale of a reference of the same size");
      }
      // Segments.
      const size_t want_segments = of->seg.enabled ? 8 : 1;
      if (segments.size() != want_segments) problem("number of segment states");
      for (size_t s = 0; s < segments.size() && s < want_segments; ++s) {
        const uint32_t* sd = segments[s].dw;
        if (sd[1] != s) problem("segment id");
        if (sd[5] != (uint32_t(of->dequant[s][1]) << 16 | uint32_t(of->dequant[s][0])) || sd[6] != (uint32_t(of->dequant[s][3]) << 16 | uint32_t(of->dequant[s][2]))) problem("segment quantizers");
        if (of->filter_level) {
          const uint32_t a = uint32_t(of->lf_adj_level[s][1][1]) << 24 | uint32_t(of->lf_adj_level[s][1][0]) << 16 | uint32_t(of->lf_adj_level[s][0][1]) << 8 | of->lf_adj_level[s][0][0];
          const uint32_t b = uint32_t(of->lf_adj_level[s][3][1]) << 24 | uint32_t(of->lf_adj_level[s][3][0]) << 16 | uint32_t(of->lf_adj_level[s][2][1]) << 8 | of->lf_adj_level[s][2][0];
          if (sd[3] != a || sd[4] != b) problem("segment loop filter levels");
        } else if (sd[3] || sd[4]) {
          problem("loop filter levels without a loop filter");
        }
        const bool ref_enabled = of->seg.active(int(s), vp9::SEG_LVL_REF_FRAME), skip = of->seg.active(int(s), vp9::SEG_LVL_SKIP);
        const uint32_t ref = intra || !ref_enabled ? 0 : uint32_t(of->seg.feature_data[s][vp9::SEG_LVL_REF_FRAME]) & 3;
        if (sd[2] != (uint32_t(ref_enabled) << 3 | ref << 1 | uint32_t(skip))) problem("segment reference and skip");
      }
      // The frame, behind its uncompressed header.
      if (bsd_offset != of->uncompressed_header_bytes || bsd_size != rec->data.size() - of->uncompressed_header_bytes) problem("bitstream object");
      for (uint32_t off = 0; off < rec->data.size(); ++off) {
        bool ok;
        uint8_t* p1 = gpu.ppgtt_bytes(bitstream + off, 1, "bitstream", &ok);
        if (!ok) { problem("bitstream not mapped"); break; }
        if (*p1 != rec->data[off]) { problem("the bitstream differs from the frame"); break; }
      }
      // The probabilities the engine starts from, as the driver's rules make them.
      if (last.valid && last.refresh) {
        if (last.intra) ctx[last.idx].first = last.frame;
        else ctx[last.idx] = {last.frame, last.frame};
      }
      uint32_t idx = uint32_t(of->frame_context_idx) & 3;
      if (key || intra_only || of->error_resilient) {
        if (key || of->reset_frame_context == 3 || of->error_resilient) { for (Ctx& c : ctx) c = Ctx(); }
        else if (of->reset_frame_context == 2 && intra_only) ctx[idx] = Ctx();
        idx = 0;
      }
      std::vector<uint8_t> buf;
      for (uint32_t off = 0; off < 2048; ++off) {
        bool ok;
        uint8_t* p1 = gpu.ppgtt_bytes(prob + off, 1, "probabilities", &ok);
        if (!ok) { problem("probabilities not mapped"); break; }
        buf.push_back(*p1);
      }
      if (buf.size() == 2048) {
        auto check_part = [&](uint32_t from, uint32_t to, long source, std::vector<uint8_t>* defaults, const char* what) {
          if (source >= 0) {
            for (uint32_t b = from; b < to; ++b)
              if (buf[b] != pattern(source, b)) { problem(what); return; }
          } else if (defaults->empty()) {
            defaults->assign(buf.begin() + from, buf.begin() + to);
          } else if (!std::equal(defaults->begin(), defaults->end(), buf.begin() + from)) {
            problem(what);
          }
        };
        check_part(0, 1667, ctx[idx].first, &default_first, "probabilities: first part");
        if (intra) check_part(1667, 2010, -1, &key_inter, "probabilities: inter part of an intra frame");
        else check_part(1667, 2010, ctx[idx].inter, &default_inter, "probabilities: inter part");
        if (of->seg.enabled && of->seg.update_map) {
          for (int b = 0; b < 7; ++b) if (buf[2010 + b] != of->seg.tree_probs[b]) { problem("segment tree probabilities"); break; }
          for (int b = 0; b < 3; ++b) if (buf[2017 + b] != of->seg.pred_probs[b]) { problem("segment prediction probabilities"); break; }
        }
      }
      last = {true, of->refresh_frame_context, key || intra_only, idx, long(k)};
    }
    // The engine "adapts": the first part of the buffer is its own afterwards.
    for (uint32_t off = 0; off < 2010; ++off) {
      bool ok;
      uint8_t* p1 = gpu.ppgtt_bytes(prob + off, 1, "probabilities", &ok);
      if (ok) *p1 = pattern(long(k), off);
    }
    // "Decode": paint the output picture with the CPU decoder's, tiled.
    uint8_t* page = nullptr;
    uint64_t page_index = ~0ull;
    auto put = [&](uint64_t offset, uint8_t value) {
      if (offset / 4096 != page_index) {
        bool ok;
        page = gpu.ppgtt_bytes(cur + (offset / 4096) * 4096, 4096, "output surface", &ok);
        page_index = offset / 4096;
        if (!ok) page = nullptr;
      }
      if (page) page[offset % 4096] = value;
    };
    for (uint64_t off = 0; off < layout.size; ++off) put(off, 0xEE);
    if (rec) {
      for (int y = 0; y < of->plane_height[0]; ++y)
        for (int x = 0; x < of->plane_width[0]; ++x) put(y_tile_offset(layout.pitch, uint32_t(x), uint32_t(y)), rec->plane[0][size_t(y) * size_t(of->plane_width[0]) + size_t(x)]);
      for (int y = 0; y < of->plane_height[1]; ++y)
        for (int x = 0; x < of->plane_width[1]; ++x) {
          put(y_tile_offset(layout.pitch, uint32_t(2 * x), layout.chroma_offset_rows + uint32_t(y)), rec->plane[1][size_t(y) * size_t(of->plane_width[1]) + size_t(x)]);
          put(y_tile_offset(layout.pitch, uint32_t(2 * x + 1), layout.chroma_offset_rows + uint32_t(y)), rec->plane[2][size_t(y) * size_t(of->plane_width[2]) + size_t(x)]);
        }
      painted[cur] = long(k);
      address_of[long(k)] = cur;
      size_of[long(k)] = {uint32_t(of->info.width), uint32_t(of->info.height)};
    }
    ++frames;
  }
};

struct Rig2 : Rig {
  FakeDecoder engine;
  Rig2() { gpu.on_batch = [this](FakeGpu& g, const std::vector<uint32_t>& d) { engine.run(g, d); }; }
};

static DecoderConfig i420_config() {
  DecoderConfig c;
  c.codec = VideoCodec::Vp9;
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
    printf("    %zu packets in, %zu frames decoded, %zu out\n", ref.ends.size(), ref.frames.size(), ref.out.pts.size());
    Rig2 rig;
    rig.engine.reference = &ref;
    {
      VcsDriver driver(rig.mem);
      CHECK(driver.initialize(rig.config()));
      {
        IntelVp9Decoder session(a, driver, i420_config());
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
        CHECK(!out.format.empty() && session.output_format().width == out.format.front().width && session.output_format().height == out.format.front().height);
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
      IntelVp9Decoder first(a, driver, i420_config());
      CHECK(first.prepare());
      IntelVp9Decoder second(a, driver, i420_config());
      CHECK(!second.prepare());
    }
    IntelVp9Decoder third(a, driver, i420_config());
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
      IntelVp9Decoder session(a, driver, i420_config());
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

// Damaged streams: packets cut short and bits flipped (and a stream that changes size under its references). The engine is never pointed at anything unmapped or freed,
// nothing leaks.
static void test_damaged_streams() {
  TestAllocator a;
  for (const char* name : {"inter", "segments", "tiles", "scaled"}) {
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
          IntelVp9Decoder session(a, driver, i420_config());
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
    CHECK(runs >= 24 * 3);   // (the streams are short)
  }
  CHECK(a.live == 0);
}

// The device: VP9 sessions go to the engine, one at a time, everything else stays on the CPU.
static void test_device() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference_file(a, "inter", &ref));
  Rig2 rig;
  rig.engine.reference = &ref;
  std::vector<uint32_t> regs(0x20000 / 4, 0);
  regs[0x0D88 / 4] = 1;   // the video box answers the power request
  const MmioRegion region(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4);
  IntelMediaVideoEngine::HardwareSetup setup;
  setup.registers = &rig.gpu;
  setup.ggtt_table = rig.config().ggtt_table;
  setup.ggtt_first_page = 100;
  setup.memory = &rig.mem;
  {
    IntelMediaVideoEngine device(a, 0x3E92, region);   // Coffee Lake
    device.set_hardware(setup);
    CHECK(device.initialize());
    CHECK(device.hardware_decode() && (device.info().accelerated_decode_codecs & codec_mask(VideoCodec::Vp9)));
    CHECK(device.engine_decode_codecs() & codec_mask(VideoCodec::Vp9));
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
  }
  CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
  CHECK(a.live == 0);
}

int main() {
  // Streams the engine takes: 8-bit 4:2:0 (profile 0).
  static const char* const taken[] = {"intra", "inter", "tiles", "lossless", "segments"};
  // Streams the CPU keeps: 10- and 12-bit, 4:2:2, 4:4:0, 4:4:4.
  static const char* const kept[] = {"10bit", "12bit_444", "422", "440", "444"};
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
  printf("IntelVp9SessionTest passed\n");
  return 0;
}
