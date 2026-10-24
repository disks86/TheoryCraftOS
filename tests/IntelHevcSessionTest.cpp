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

// Host-side tests for the Intel hardware H.265 decode session (IntelHevcDecoder) against a simulated GPU. The simulated engine plays the decode hardware: for every batch
// it reads the HCP commands the way the engine would, checks that every address in them is mapped in the context's address space and that the buffers do not overlap, that
// the picture state agrees with the surface, that every slice segment has its state, its reference index lists and (when the stream weights predictions) its weights, that
// each slice's position, type and the chain to the next one agree with what the slice NAL units say (parsed here from their own bits), that the bitstream objects name the
// NAL units as the stream has them and that the slice data starts behind a slice header that ends in its alignment bits, that the reference frame store holds pictures the
// engine has decoded and that the order count differences of the reference lists are consistent among all the pictures; then it "decodes" by writing the picture a separate
// CPU decoder made for the same access unit into the output surface, tiled as the engine does. The session must hand back exactly that decoder's pictures, in the same
// order, and the stream's own picture hashes must confirm them.
//
// This does not show that the real engine decodes anything: the commands the stand-in checks follow the reference driver, as far as is known.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include "Crypto/Crypto.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"
#include "Hardware/VideoCodec/Intel/IntelHevcSession.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"

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

struct Md5Hasher final : IPlaneHasher {
  void md5(const uint8_t* plane, uint32_t width, uint32_t height, uint32_t stride, uint8_t out[16]) override {
    crypto::Hasher h(crypto::HashKind::Md5);
    for (uint32_t y = 0; y < height; ++y) h.update(ConstByteView(plane + size_t(y) * stride, width));
    h.finish(out);
  }
};
static Md5Hasher g_md5;

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

// Splits an Annex B stream into units that each end before the next picture's first slice (the parameter sets in front of a picture go with the picture before it).
static std::vector<std::pair<size_t, size_t>> split_pictures(const std::vector<uint8_t>& d) {
  std::vector<size_t> starts;
  for (size_t i = 0; i + 3 <= d.size(); ++i)
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { starts.push_back(i); i += 2; }
  std::vector<std::pair<size_t, size_t>> units;
  size_t unit_begin = 0;
  bool have_slice = false;
  for (size_t k = 0; k < starts.size(); ++k) {
    const size_t at = starts[k];
    if (at + 5 > d.size()) break;
    const int type = (d[at + 3] >> 1) & 0x3F;
    const bool slice = type < 32;
    const bool first = slice && (d[at + 5] & 0x80);
    if (first && have_slice) {
      size_t cut = at;
      size_t j = k;
      while (j > 0) {
        const int pt = (d[starts[j - 1] + 3] >> 1) & 0x3F;
        if (pt >= 32 && pt <= 39) { cut = starts[j - 1]; --j; } else break;
      }
      if (cut > unit_begin) units.push_back({unit_begin, cut});
      unit_begin = cut;
      have_slice = false;
    }
    if (slice) have_slice = true;
  }
  units.push_back({unit_begin, d.size()});
  return units;
}

// The slice NAL units of an access unit (header bytes on, trailing zero bytes off): what the engine's bitstream must hold behind start codes.
static std::vector<std::vector<uint8_t>> slice_nals(ConstByteView au) {
  std::vector<std::vector<uint8_t>> nals;
  for (uint64_t i = 0; i + 3 <= au.size();) {
    if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
      uint64_t j = i + 3;
      while (j + 3 <= au.size() && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)) ++j;
      uint64_t end = j + 3 <= au.size() ? j : au.size();
      while (end > i + 3 && au[end - 1] == 0) --end;
      const int type = (au[i + 3] >> 1) & 0x3F;
      if (type < 32) nals.emplace_back(au.data() + i + 3, au.data() + end);
      i = j;
    } else {
      ++i;
    }
  }
  return nals;
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

// The CPU decoder's view of a stream: its units, and its pictures by presentation time.
struct Reference {
  std::vector<uint8_t> file;
  std::vector<std::pair<size_t, size_t>> units;
  Output out;
  std::map<int64_t, size_t> index;   // pts -> position in out
  ConstByteView unit(size_t k) const { return ConstByteView(file.data() + units[k].first, units[k].second - units[k].first); }
};

static bool make_reference_from(allocators::IAllocator& a, Reference* ref) {
  if (ref->file.empty()) return false;
  ref->units = split_pictures(ref->file);
  H265Decoder dec(a);
  dec.set_plane_hasher(&g_md5);
  VideoFrame f;
  auto take = [&] {
    while (dec.receive(&f)) {
      std::vector<uint8_t> bytes;
      for (uint32_t p = 0; p < 3; ++p)
        for (uint32_t y = 0; y < f.format.plane_height(p); ++y) bytes.insert(bytes.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + f.format.plane_width(p));
      ref->index[f.pts_us] = ref->out.pts.size();
      ref->out.pts.push_back(f.pts_us);
      ref->out.bytes.push_back(bytes);
      ref->out.format.push_back(f.format);
    }
  };
  for (size_t k = 0; k < ref->units.size(); ++k) {
    dec.decode(ref->unit(k), int64_t(k) * 1000);
    take();
  }
  dec.flush();
  take();
  const auto stats = dec.hash_statistics();
  return !ref->out.pts.empty() && stats.checked == ref->out.pts.size() && stats.mismatched == 0;
}

static bool make_reference(allocators::IAllocator& a, const char* name, Reference* ref) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/hevc_%s.h265", name);
  ref->file = read_file(path);
  return make_reference_from(a, ref);
}

static std::vector<uint8_t> unescape(const uint8_t* d, size_t n) {
  std::vector<uint8_t> out;
  int zeros = 0;
  for (size_t i = 0; i < n; ++i) {
    if (zeros >= 2 && d[i] == 3) { zeros = 0; continue; }
    out.push_back(d[i]);
    zeros = d[i] == 0 ? zeros + 1 : 0;
  }
  return out;
}

struct Bits {
  const std::vector<uint8_t>& d;
  size_t pos;
  uint32_t u(uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i, ++pos) v = (v << 1) | (pos / 8 < d.size() ? (d[pos / 8] >> (7 - pos % 8)) & 1 : 0);
    return v;
  }
  uint32_t ue() {
    uint32_t zeros = 0;
    while (zeros < 32 && u(1) == 0) ++zeros;
    return zeros == 0 ? 0 : ((1u << zeros) - 1) + u(zeros);
  }
};

static uint32_t ceil_log2(uint32_t v) {
  uint32_t n = 0;
  while ((1u << n) < v) ++n;
  return n;
}

// The decode engine of the simulated GPU.
struct FakeDecoder {
  const Reference* reference = nullptr;
  bool check_content = true;
  std::map<uint64_t, long> painted;               // surface address -> the picture (by position among the decoded ones) it last held
  struct Edge { long cur, ref; int delta; };
  std::vector<Edge> edges;                        // order count of `cur` minus that of `ref`
  int pictures = 0, batches = 0, problems = 0, max_slices = 0;
  size_t next = 0, slices_done = 0;
  long current = -1;
  bool weighted_p = false, weighted_b = false, saw_long_term = false, saw_tiles = false;
  // What the PPS NAL units seen so far say (dependent slice segments, extra slice header bits).
  bool pps_dependent = false;
  uint32_t pps_extra_bits = 0;

  static uint64_t addr(const std::vector<uint32_t>& d, size_t at) { return (uint64_t{d[at + 1]} << 32) | d[at]; }
  void problem(const char* what) {
    ++problems;
    printf("  fake engine: %s\n", what);
  }

  void note_parameter_sets(ConstByteView au) {
    for (uint64_t i = 0; i + 3 <= au.size();) {
      if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
        uint64_t j = i + 3;
        while (j + 3 <= au.size() && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)) ++j;
        const uint64_t end = j + 3 <= au.size() ? j : au.size();
        if (((au[i + 3] >> 1) & 0x3F) == 34) {
          const std::vector<uint8_t> clean = unescape(au.data() + i + 3 + 2, end - (i + 3 + 2));
          Bits b{clean, 0};
          b.ue();
          b.ue();
          pps_dependent = b.u(1);
          b.u(1);
          pps_extra_bits = b.u(3);
        }
        i = j;
      } else {
        ++i;
      }
    }
  }

  // The potentials of the order counts must exist: every difference agrees with every other path between the same two pictures.
  bool consistent() const {
    std::map<long, std::vector<std::pair<long, int>>> graph;
    for (const Edge& e : edges) {
      graph[e.cur].push_back({e.ref, -e.delta});
      graph[e.ref].push_back({e.cur, e.delta});
    }
    std::map<long, long> value;
    for (const auto& [start, unused] : graph) {
      (void)unused;
      if (value.count(start)) continue;
      value[start] = 0;
      std::vector<long> stack{start};
      while (!stack.empty()) {
        const long n = stack.back();
        stack.pop_back();
        for (const auto& [m, d] : graph[n]) {
          const long want = value[n] + d;
          auto it = value.find(m);
          if (it == value.end()) { value[m] = want; stack.push_back(m); }
          else if (it->second != want) return false;
        }
      }
    }
    return true;
  }

  void run(FakeGpu& gpu, const std::vector<uint32_t>& d) {
    ++batches;
    uint32_t codec = 99, surf_pitch = 0, surf_format = 0, surf_chroma = 0, qm_count = 0;
    uint64_t cur = 0, lines[9] = {}, mv_cur = 0, refs[8] = {}, mv_refs[8] = {}, bitstream = 0, bitstream_upper = 0;
    uint32_t pic[19] = {}, tile[13] = {};
    bool have_pic = false, have_tile = false, have_buf = false, have_ind = false;
    struct RefList { bool present = false; uint32_t count = 0; uint32_t entry[16] = {}; };
    struct Slice {
      uint32_t state[9] = {};
      RefList list[2];
      bool weights[2] = {false, false};
      uint32_t size = 0, offset = 0;
      bool done = false;
    };
    std::vector<Slice> slices;
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
        case kHcpSurfaceState: surf_pitch = (d[i + 1] & 0x1FFFF) + 1; surf_format = d[i + 2] >> 28; surf_chroma = d[i + 2] & 0xFFFF; break;
        case kHcpPipeBufAddrState:
          if (len != 95) { problem("pipe buffer address length"); return; }
          cur = addr(d, i + 1);
          for (int k = 0; k < 9; ++k) lines[k] = addr(d, i + 4 + 3 * k);
          mv_cur = addr(d, i + 31);
          for (int k = 0; k < 8; ++k) { refs[k] = addr(d, i + 37 + 2 * k); mv_refs[k] = addr(d, i + 66 + 2 * k); }
          have_buf = true;
          break;
        case kHcpIndObjBaseAddrState: bitstream = addr(d, i + 1); bitstream_upper = addr(d, i + 4); have_ind = len == 14; break;
        case kHcpQmState: ++qm_count; break;
        case kHcpPicState: if (len == 19) { for (int k = 0; k < 19; ++k) pic[k] = d[i + k]; have_pic = true; } break;
        case kHcpTileState: if (len == 13) { for (int k = 0; k < 13; ++k) tile[k] = d[i + k]; have_tile = true; } break;
        case kHcpSliceState: {
          if (len != 9) { problem("slice state length"); return; }
          Slice s;
          for (int k = 0; k < 9; ++k) s.state[k] = d[i + k];
          slices.push_back(s);
          break;
        }
        case kHcpRefIdxState: {
          if (slices.empty() || len != 18) { problem("reference index state outside a slice or of the wrong length"); return; }
          const uint32_t list = d[i + 1] & 1;
          RefList& r = slices.back().list[list];
          if (r.present) problem("a reference list twice");
          r.present = true;
          r.count = ((d[i + 1] >> 1) & 0xF) + 1;
          for (int k = 0; k < 16; ++k) r.entry[k] = d[i + 2 + k];
          break;
        }
        case kHcpWeightoffset:
          if (slices.empty() || len != 34) { problem("weights outside a slice or of the wrong length"); return; }
          slices.back().weights[d[i + 1] & 1] = true;
          break;
        case kHcpBsdObject:
          if (slices.empty() || len != 3 || slices.back().done) { problem("bitstream object without a slice state"); return; }
          slices.back().size = d[i + 1];
          slices.back().offset = d[i + 2];
          slices.back().done = true;
          break;
        default: break;
      }
      i += len;
    }
    if (codec != 0 || !have_pic || !have_buf || !have_ind || slices.empty() || qm_count != 20) { problem("incomplete HEVC batch"); return; }
    for (const Slice& s : slices) if (!s.done) { problem("a slice without a bitstream object"); return; }
    if (surf_format != 4) problem("surface format");
    const uint32_t log2_min_cb = (pic[2] & 3) + 3, log2_ctb = ((pic[2] >> 2) & 3) + 3;
    const uint32_t width = ((pic[1] & 0xFFFF) + 1) << log2_min_cb, height = ((pic[1] >> 16) + 1) << log2_min_cb;
    const uint32_t width_ctbs = (width + (1u << log2_ctb) - 1) >> log2_ctb, height_ctbs = (height + (1u << log2_ctb) - 1) >> log2_ctb;
    if (log2_ctb < 4 || log2_ctb > 6) problem("coding tree block size");
    Nv12TiledLayout layout;
    if (!nv12_tiled_layout(width, height, &layout) || layout.pitch != surf_pitch || layout.chroma_offset_rows != surf_chroma) { problem("surface layout"); return; }
    const bool tiles = (pic[4] >> 17) & 1;
    if (tiles != have_tile) problem("tile state present only with tiles");
    if (have_tile) {
      saw_tiles = true;
      const uint32_t cols = ((tile[1] >> 5) & 0x1F) + 1, rows = (tile[1] & 0x1F) + 1;
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&tile[2]);
      if (cols > 19 || rows > 21 || bytes[cols] != width_ctbs || bytes[20 + rows] != height_ctbs) problem("tile positions do not end at the picture's size");
    }
    weighted_p |= (pic[4] >> 19) & 1;
    weighted_b |= (pic[4] >> 18) & 1;

    // Every buffer is mapped, none overlaps another.
    const HevcBufferBytes sizes = hevc_buffer_bytes(width, height, log2_ctb);
    struct Range { uint64_t start, size; const char* name; };
    std::vector<Range> ranges;
    std::set<uint64_t> seen;
    auto add = [&](uint64_t a, uint64_t bytes, const char* name) {
      if (!a) return;
      for (uint64_t off = 0; off < bytes; off += 4096) {   // (the stand-in looks at one page at a time)
        bool ok;
        if (!gpu.ppgtt_bytes(a + off, 1, name, &ok) || !ok) { problem("a buffer in the commands is not mapped"); return; }
      }
      bool ok;
      if (bytes && (!gpu.ppgtt_bytes(a + bytes - 1, 1, name, &ok) || !ok)) { problem("the end of a buffer in the commands is not mapped"); return; }
      if (seen.insert(a).second) ranges.push_back({a, bytes, name});
    };
    add(cur, layout.size, "current picture");
    const uint64_t line_sizes[9] = {sizes.deblocking_line, sizes.deblocking_tile_line, sizes.deblocking_tile_column, sizes.metadata_line, sizes.metadata_tile_line,
                                    sizes.metadata_tile_column, sizes.sao_line, sizes.sao_tile_line, sizes.sao_tile_column};
    for (int k = 0; k < 9; ++k) {
      if (!lines[k]) problem("a line or column buffer is missing");
      add(lines[k], line_sizes[k], "line buffer");
    }
    if (!mv_cur) problem("no motion vector buffer for the picture");
    add(mv_cur, sizes.motion_vector, "motion vector buffer");
    uint32_t used_refs = 0;
    for (int k = 0; k < 8; ++k) {
      if (!refs[k]) { if (mv_refs[k]) problem("a motion vector buffer without a reference picture"); continue; }
      ++used_refs;
      if (!mv_refs[k]) problem("a reference picture without its motion vector buffer");
      if (refs[k] == cur) problem("a picture refers to itself");
      add(refs[k], layout.size, "reference picture");
      add(mv_refs[k], sizes.motion_vector, "reference motion vectors");
    }
    add(bitstream, bitstream_upper > bitstream ? 1 : 1, "bitstream");
    if (bitstream_upper <= bitstream) problem("bitstream upper bound");
    for (size_t x = 0; x < ranges.size(); ++x)
      for (size_t y = x + 1; y < ranges.size(); ++y)
        if (ranges[x].start < ranges[y].start + ranges[y].size && ranges[y].start < ranges[x].start + ranges[x].size) problem("two buffers overlap");

    // Which picture this batch belongs to (a picture with many slices comes in several batches).
    const bool new_picture = slices_done == 0;
    long k = -1;
    if (new_picture) {
      current = long(next++);
      if (!check_content || size_t(current) >= reference->units.size()) { current = -1; }
    }
    k = current;
    const bool checks = reference && check_content && k >= 0;
    std::vector<std::vector<uint8_t>> nals;
    if (checks) {
      nals = slice_nals(reference->unit(size_t(k)));
      if (new_picture) note_parameter_sets(reference->unit(size_t(k)));
      if (slices_done + slices.size() > nals.size()) { problem("more slices than the access unit has"); return; }
    }
    if (checks) {
      max_slices = int(slices.size()) > max_slices ? int(slices.size()) : max_slices;
      for (size_t s = 0; s < slices.size(); ++s) {
        const Slice& sl = slices[s];
        const std::vector<uint8_t>& nal = nals[slices_done + s];
        const uint32_t type = sl.state[3] & 3;
        const bool last = (sl.state[3] >> 2) & 1, dependent = (sl.state[3] >> 4) & 1, tmvp = (sl.state[3] >> 5) & 1;
        const uint32_t hor = sl.state[1] & 0xFFFF, ver = sl.state[1] >> 16, next_hor = sl.state[2] & 0xFFFF, next_ver = sl.state[2] >> 16;
        // The slice NAL unit as stored, at the place the object names.
        std::vector<uint8_t> hw;
        for (uint32_t off = 0; off < sl.size; ++off) {
          bool ok;
          uint8_t* p1 = gpu.ppgtt_bytes(bitstream + sl.offset + off, 1, "bitstream", &ok);
          if (!ok) { problem("bitstream not mapped"); return; }
          hw.push_back(*p1);
        }
        bool same = hw.size() >= nal.size() && memcmp(hw.data(), nal.data(), nal.size()) == 0;
        for (size_t q = nal.size(); same && q < hw.size(); ++q) same = hw[q] == 0;
        if (!same) problem("a bitstream object does not hold the slice NAL unit");
        // The header bytes: the slice data starts behind the alignment bits.
        const uint32_t hb = sl.state[5];
        if (hb < 3 || hb >= nal.size() || (nal[hb - 1] == 0)) problem("slice data offset");
        // What the NAL unit says about itself.
        const std::vector<uint8_t> clean = unescape(nal.data() + 2, nal.size() - 2);
        Bits b{clean, 0};
        const bool first = b.u(1);
        const int nal_type = (nal[0] >> 1) & 0x3F;
        if (nal_type >= 16 && nal_type <= 23) b.u(1);
        b.ue();
        bool dep = false;
        uint32_t address = 0;
        if (!first) {
          if (pps_dependent) dep = b.u(1);
          address = b.u(ceil_log2(width_ctbs * height_ctbs));
        }
        if (address != ver * width_ctbs + hor || first != (address == 0) || dep != dependent) problem("slice position or dependency differs from the NAL unit's");
        if (!dep) {
          b.u(pps_extra_bits);
          const uint32_t nal_slice_type = b.ue();
          if (nal_slice_type != type) problem("slice type differs from the NAL unit's");
        }
        // The chain of slices.
        const bool really_last = slices_done + s + 1 == nals.size();
        if (last != really_last) problem("last slice flag");
        if (!last) {
          const std::vector<uint8_t>& nn = nals[slices_done + s + 1];
          const std::vector<uint8_t> cn = unescape(nn.data() + 2, nn.size() - 2);
          Bits nb{cn, 0};
          const bool nfirst = nb.u(1);
          if ((((nn[0] >> 1) & 0x3F) >= 16) && (((nn[0] >> 1) & 0x3F) <= 23)) nb.u(1);
          nb.ue();
          bool ndep = false;
          uint32_t naddr = 0;
          if (!nfirst) {
            if (pps_dependent) ndep = nb.u(1);
            naddr = nb.u(ceil_log2(width_ctbs * height_ctbs));
          }
          (void)ndep;
          if (next_ver * width_ctbs + next_hor != naddr) problem("next slice position");
        } else if (next_hor || next_ver) {
          problem("the last slice names a next position");
        }
        // Reference lists.
        const uint32_t want_lists = type == 0 ? 2 : type == 1 ? 1 : 0;
        for (uint32_t l = 0; l < 2; ++l)
          if (sl.list[l].present != (l < want_lists)) problem("reference lists of the slice type");
        const bool weighted = (type == 1 && ((pic[4] >> 19) & 1)) || (type == 0 && ((pic[4] >> 18) & 1));
        for (uint32_t l = 0; l < 2; ++l)
          if (sl.weights[l] != (weighted && l < want_lists)) problem("prediction weights of the slice");
        bool low_delay = want_lists > 0;
        for (uint32_t l = 0; l < want_lists; ++l) {
          const RefList& r = sl.list[l];
          if (r.count == 0 || r.count > 15) { problem("reference list size"); continue; }
          for (uint32_t e = 0; e < r.count; ++e) {
            const uint32_t entry = r.entry[e];
            const uint32_t fs = (entry >> 8) & 0xF;
            const int delta = int8_t(entry & 0xFF);
            if (fs >= 8 || !refs[fs] || !(entry >> 15 & 1) || (entry >> 14 & 1)) { problem("reference list entry"); continue; }
            if (entry >> 13 & 1) saw_long_term = true;
            auto it = painted.find(refs[fs]);
            if (it == painted.end()) problem("a reference picture was never decoded");
            else edges.push_back({k, it->second, delta});
            if (delta < 0) low_delay = false;
          }
        }
        if (bool((sl.state[4] >> 14) & 1) != low_delay) problem("low delay flag");
        const uint32_t coll = (sl.state[4] >> 26) & 0xF;
        if (type != 2 && tmvp && !refs[coll]) problem("collocated picture is not in the frame store");
        if (type == 2 && bool((sl.state[4] >> 14) & 1)) problem("low delay on an intra slice");
      }
    }
    slices_done += slices.size();
    if (checks && slices_done == nals.size()) slices_done = 0;
    else if (!checks) slices_done = 0;

    // "Decode": paint the output picture with the CPU decoder's, tiled.
    if (!new_picture) return;
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
    if (reference && k >= 0) {
      auto it = reference->index.find(int64_t(k) * 1000);
      if (it == reference->index.end()) { problem("no CPU picture to stand in for the decode"); return; }
      const std::vector<uint8_t>& yuv = reference->out.bytes[it->second];
      const VideoFormat& fmt = reference->out.format[it->second];
      const uint32_t cw = fmt.chroma_width(), ch = fmt.chroma_height();
      if (fmt.width > width || fmt.height > height) { problem("CPU picture is bigger than the surface"); return; }
      for (uint32_t y = 0; y < fmt.height; ++y)
        for (uint32_t x = 0; x < fmt.width; ++x) put(y_tile_offset(layout.pitch, x, y), yuv[uint64_t{y} * fmt.width + x]);
      const uint8_t* u = yuv.data() + uint64_t{fmt.width} * fmt.height;
      const uint8_t* v = u + uint64_t{cw} * ch;
      for (uint32_t y = 0; y < ch; ++y)
        for (uint32_t x = 0; x < cw; ++x) {
          put(y_tile_offset(layout.pitch, 2 * x, layout.chroma_offset_rows + y), u[uint64_t{y} * cw + x]);
          put(y_tile_offset(layout.pitch, 2 * x + 1, layout.chroma_offset_rows + y), v[uint64_t{y} * cw + x]);
        }
    }
    painted[cur] = k;
    ++pictures;
  }
};

struct Rig2 : Rig {
  FakeDecoder engine;
  Rig2() { gpu.on_batch = [this](FakeGpu& g, const std::vector<uint32_t>& d) { engine.run(g, d); }; }
};

static DecoderConfig i420_config() {
  DecoderConfig c;
  c.codec = VideoCodec::Hevc;
  c.output = PixelFormat::I420;
  return c;
}

static void compare_stream(const char* name, uint64_t batch_limit = 0, int min_batches_per_picture = 0) {
  printf("  %s%s\n", name, batch_limit ? " (small batches)" : "");
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, name, &ref));
    if (ref.units.empty()) return;
    printf("    %zu pictures in, %zu out\n", ref.units.size(), ref.out.pts.size());
    Rig2 rig;
    rig.engine.reference = &ref;
    {
      VcsDriver driver(rig.mem);
      CHECK(driver.initialize(rig.config()));
      {
        IntelHevcDecoder session(a, driver, i420_config());
        session.set_plane_hasher(&g_md5);
        if (batch_limit) session.limit_batch_words(batch_limit);
        CHECK(session.prepare());
        Output out;
        for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
        session.flush();
        drain(session, &out);
        CHECK(session.statistics().errors == 0 && session.statistics().frames == ref.out.pts.size());
        CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
        CHECK(rig.engine.pictures == int(ref.units.size()) && session.pictures_decoded() == ref.units.size());
        if (min_batches_per_picture) CHECK(rig.engine.batches >= min_batches_per_picture * rig.engine.pictures);
        CHECK(rig.engine.consistent());
        // The same pictures, in the same order, byte for byte; and the stream's own hashes agree with every one of the engine's pictures.
        CHECK(out.pts == ref.out.pts);
        for (size_t i = 0; i < out.bytes.size() && i < ref.out.pts.size(); ++i) {
          CHECK(out.format[i] == ref.out.format[i]);
          CHECK(out.bytes[i] == ref.out.bytes[i]);
        }
        CHECK(session.hash_statistics().checked == ref.units.size() && session.hash_statistics().mismatched == 0);
        CHECK(!out.format.empty() && session.output_format().width == out.format.front().width && session.output_format().height == out.format.front().height);
      }
    }
    CHECK(rig.gpu.violations.empty());
    CHECK(rig.mem.live() == 1);   // only the scratch page that is never freed
  }
  CHECK(a.live == 0);
}

static void test_lease_and_config() {
  TestAllocator a;
  Rig2 rig;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    {
      IntelHevcDecoder first(a, driver, i420_config());
      CHECK(first.prepare());
      IntelHevcDecoder second(a, driver, i420_config());
      CHECK(!second.prepare());
    }
    DecoderConfig junk = i420_config();
    const uint8_t bytes[4] = {'j', 'u', 'n', 'k'};
    junk.codec_config = ConstByteView(bytes, 4);
    IntelHevcDecoder bad(a, driver, junk);
    CHECK(!bad.prepare());   // not an hvcC record
    IntelHevcDecoder good(a, driver, i420_config());
    CHECK(good.prepare());   // the lease was not kept by the session that failed
  }
  CHECK(a.live == 0);
}

// The engine stops answering in the middle of a stream.
static void test_hang() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference(a, "ipp", &ref));
  Rig2 rig;
  rig.engine.reference = &ref;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    int live_with_session = 0;
    {
      IntelHevcDecoder session(a, driver, i420_config());
      CHECK(session.prepare());
      Output out;
      CHECK(feed(session, ref.unit(0), 0, &out));
      CHECK(rig.engine.pictures == 1);
      rig.gpu.hang = true;
      CHECK(feed(session, ref.unit(1), 1000, &out));
      CHECK(driver.state() == VcsDriver::State::Wedged && session.statistics().errors >= 1);
      const int pictures = rig.engine.pictures;
      CHECK(feed(session, ref.unit(2), 2000, &out));
      CHECK(rig.engine.pictures == pictures);
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

// Damaged streams: units cut short and bits flipped. The engine is never pointed at anything unmapped or freed, nothing leaks.
static void test_damaged_streams() {
  TestAllocator a;
  for (const char* name : {"bframes", "slices"}) {
    Reference ref;
    CHECK(make_reference(a, name, &ref));
    int runs = 0;
    for (size_t variant = 0; variant < 24; ++variant) {
      Rig2 rig;
      rig.engine.reference = &ref;
      rig.engine.check_content = false;
      {
        VcsDriver driver(rig.mem);
        CHECK(driver.initialize(rig.config()));
        {
          IntelHevcDecoder session(a, driver, i420_config());
          CHECK(session.prepare());
          Output out;
          for (size_t k = 0; k < ref.units.size(); ++k) {
            std::vector<uint8_t> au(ref.unit(k).data(), ref.unit(k).data() + ref.unit(k).size());
            if (k > 0 && variant % 2 == 1) au.resize(au.size() * (variant + 1) / 25 + 1);
            else if (k > 0) au[(variant * 7919 + k * 104729) % au.size()] ^= uint8_t(1u << (variant % 8));
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
    CHECK(runs > 100);
  }
  CHECK(a.live == 0);
}

// The device: H.265 sessions go to the engine, one at a time, everything else stays on the CPU.
static void test_device() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference(a, "bframes", &ref));
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
    CHECK(device.hardware_decode() && (device.info().accelerated_decode_codecs & codec_mask(VideoCodec::Hevc)));
    CHECK(device.engine_decode_codecs() & codec_mask(VideoCodec::Hevc));
    {
      UniquePointer<IVideoDecoder> hw = device.create_decoder(i420_config());
      CHECK(hw != nullptr);
      Output out;
      for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(*hw, ref.unit(k), int64_t(k) * 1000, &out));
      hw->flush();
      drain(*hw, &out);
      CHECK(out.pts == ref.out.pts && out.bytes == ref.out.bytes);
      CHECK(rig.engine.pictures == int(ref.units.size()));
      // The engine is taken: the next session runs on the CPU and sends nothing to it.
      UniquePointer<IVideoDecoder> sw = device.create_decoder(i420_config());
      CHECK(sw != nullptr);
      Output out2;
      for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(*sw, ref.unit(k), int64_t(k) * 1000, &out2));
      sw->flush();
      drain(*sw, &out2);
      CHECK(out2.bytes == ref.out.bytes && rig.engine.pictures == int(ref.units.size()));
    }
  }
  CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
  CHECK(a.live == 0);
}

int main() {
  static const char* const streams[] = {"intra", "ipp", "bframes", "ctu64", "scaling", "oddsize", "wpp", "weighted", "slices", "tools", "maxref", "qcif", "cutree_off", "highrate", "cra", "cip", "lossless"};
  for (const char* name : streams)
    if (!getenv("ONLY") || !strcmp(getenv("ONLY"), name)) compare_stream(name);
  if (!getenv("ONLY")) {
    compare_stream("slices", 700, 2);   // (a batch holds the state and one slice or two)
    test_lease_and_config();
    test_hang();
    test_damaged_streams();
    test_device();
  }
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("IntelHevcSessionTest passed\n");
  return 0;
}
