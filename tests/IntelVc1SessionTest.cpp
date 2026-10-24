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

// Host-side tests for the Intel hardware VC-1 decode session (IntelVc1Decoder) against a simulated GPU. The simulated engine plays the decode hardware: for every batch it
// reads the commands the way the engine would, checks that every address in them is mapped in the context's address space, that the picture state says what the stream's
// headers say (compared with what a separate CPU decoder reports for the same stream, worked out here from the reference driver's rules), that the bit planes, the
// intensity compensation, the direct motion vector buffers and the references are right, and that each unit's first macroblock is where the stream has it (with the
// emulation prevention bytes taken out); then it "decodes" by writing the picture the CPU decoder made at the same position in the stream into the output surface, tiled as
// the engine does. The session must hand back exactly the CPU decoder's pictures, in the same order.
//
// This does not show that the real engine decodes anything: the commands the stand-in checks follow the reference driver, as far as is known.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <vector>

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/Intel/IntelVc1Session.hpp"
#include "Hardware/VideoCodec/Software/Vc1.hpp"

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

static uint32_t le32(const std::vector<uint8_t>& d, size_t at) { return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24; }

// A test stream: its configuration (Simple and Main profile) and one packet per frame.
struct Stream {
  bool advanced = false;
  uint8_t struct_c[4] = {};
  uint32_t width = 0, height = 0;
  std::vector<std::vector<uint8_t>> packets;
};

static bool load_rcv(const std::vector<uint8_t>& d, Stream* s) {
  if (d.size() < 36 || (le32(d, 0) >> 24) != 0xC5 || le32(d, 4) != 4) return false;
  memcpy(s->struct_c, &d[8], 4);
  s->height = le32(d, 12);
  s->width = le32(d, 16);
  size_t pos = 36;
  while (pos + 8 <= d.size()) {
    const size_t n = d[pos] | d[pos + 1] << 8 | d[pos + 2] << 16;
    if (n > d.size() - pos - 8) return false;
    s->packets.emplace_back(d.begin() + long(pos + 8), d.begin() + long(pos + 8 + n));
    pos += 8 + n;
  }
  return !s->packets.empty();
}

static bool load_advanced(const std::vector<uint8_t>& d, Stream* s) {
  s->advanced = true;
  size_t begin = 0;
  bool in_frame = false, started = false;
  for (size_t i = 0; i + 4 <= d.size(); ++i) {
    if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
    const int code = d[i + 3];
    if (code != 0x0F && code != 0x0E && code != 0x0D && code != 0x0B) continue;
    if (code != 0x0B && (in_frame || !started)) {
      if (started) s->packets.emplace_back(d.begin() + long(begin), d.begin() + long(i));
      begin = i;
    }
    started = true;
    in_frame = code == 0x0D || code == 0x0B;
  }
  if (started) s->packets.emplace_back(d.begin() + long(begin), d.end());
  return !s->packets.empty();
}

static bool load(const char* name, Stream* s) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/vc1_%s.rcv", name);
  std::vector<uint8_t> d = read_file(path);
  if (!d.empty()) return load_rcv(d, s);
  snprintf(path, sizeof path, "tests/data/vc1_%s.vc1", name);
  d = read_file(path);
  return !d.empty() && load_advanced(d, s);
}

static ConstByteView packet_of(const Stream& s, size_t k) { return ConstByteView(s.packets[k].data(), s.packets[k].size()); }
static ConstByteView config_of(const Stream& s) { return s.advanced ? ConstByteView() : ConstByteView(s.struct_c, 4); }

struct Output {
  std::vector<int64_t> pts;
  std::vector<std::vector<uint8_t>> bytes;
  std::vector<VideoFormat> format;
};

static void take_frame(const VideoFrame& f, Output* out) {
  std::vector<uint8_t> bytes;
  for (uint32_t p = 0; p < 3; ++p)
    for (uint32_t y = 0; y < f.format.plane_height(p); ++y) bytes.insert(bytes.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + f.format.plane_width(p));
  out->pts.push_back(f.pts_us);
  out->bytes.push_back(bytes);
  out->format.push_back(f.format);
}

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



static void take(Vc1Decoder& dec, Output* out) {
  VideoFrame f;
  while (dec.receive(&f)) take_frame(f, out);
}

// The bits of a unit without its emulation prevention bytes (00 00 03 followed by 0-3).
static std::vector<uint8_t> unescape(const uint8_t* d, size_t n) {
  std::vector<uint8_t> out;
  int zeros = 0;
  for (size_t i = 0; i < n; ++i) {
    if (zeros >= 2 && d[i] == 3 && i + 1 < n && d[i + 1] < 4) { zeros = 0; continue; }
    out.push_back(d[i]);
    zeros = d[i] == 0 ? zeros + 1 : 0;
  }
  return out;
}

// What the CPU decoder says about each picture it decodes: the description an engine is given, the picture it makes, and which earlier pictures it predicts from (by
// position among the pictures the engine decodes).
struct RefPicture {
  vc1::SequenceHeader seq;
  vc1::PictureHeader pic;
  int forward = -1, backward = -1;
  bool backward_is_p = false;
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> plane[3];
  uint32_t stride[3] = {0, 0, 0};
  std::vector<std::vector<uint8_t>> units;
  std::vector<vc1::UnitInfo> info;
  std::vector<uint8_t> skip, mv_type, direct, acpred, overflags;
  bool ic = false;
  int ic_scale = 0, ic_shift = 0;
};

class Observer final : public vc1::IFrameObserver {
 public:
  explicit Observer(bool advanced) : advanced_(advanced) {}
  std::vector<RefPicture> pictures;
  int skipped = 0;
  void picture_decoded(const vc1::OffloadPicture& o) override {
    if (o.pic->skipped) {
      ++skipped;
      owner_[o.slot] = o.anchor_slot[1] >= 0 ? owner_[o.anchor_slot[1]] : -1;
      is_p_[o.slot] = false;
      return;
    }
    RefPicture r;
    r.seq = *o.seq;
    r.pic = *o.pic;
    r.width = uint32_t(o.seq->mb_width) * 16;
    r.height = uint32_t(o.seq->mb_height) * 16;
    if (o.forward_slot >= 0) r.forward = owner_[o.forward_slot];
    if (o.backward_slot >= 0) { r.backward = owner_[o.backward_slot]; r.backward_is_p = is_p_[o.backward_slot]; }
    for (int c = 0; c < 3; ++c) {
      const uint32_t h = c == 0 ? r.height : r.height / 2;
      r.stride[c] = uint32_t(o.stride[c]);
      r.plane[c].assign(o.plane[c].data(), o.plane[c].data() + uint64_t{r.stride[c]} * h);
    }
    for (const ConstByteView& u : o.units) r.units.emplace_back(u.data(), u.data() + u.size());
    for (const vc1::UnitInfo& i : o.unit_info) r.info.push_back(i);
    r.skip.assign(o.skip_plane.data(), o.skip_plane.data() + o.skip_plane.size());
    r.mv_type.assign(o.mv_type_plane.data(), o.mv_type_plane.data() + o.mv_type_plane.size());
    r.direct.assign(o.direct_plane.data(), o.direct_plane.data() + o.direct_plane.size());
    r.acpred.assign(o.acpred_plane.data(), o.acpred_plane.data() + o.acpred_plane.size());
    r.overflags.assign(o.overflags_plane.data(), o.overflags_plane.data() + o.overflags_plane.size());
    r.ic = o.ic_forward;
    r.ic_scale = o.ic_scale;
    r.ic_shift = o.ic_shift;
    owner_[o.slot] = int(pictures.size());
    is_p_[o.slot] = o.pic->type == vc1::kPictureP;
    pictures.push_back(std::move(r));
  }

 private:
  bool advanced_;
  int owner_[vc1::kPoolSize] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  bool is_p_[vc1::kPoolSize] = {};
};

struct Reference {
  Stream stream;
  Output out;
  std::unique_ptr<Observer> observer;
  size_t units() const { return stream.packets.size(); }
  ConstByteView unit(size_t k) const { return packet_of(stream, k); }
};

static bool make_reference_from(allocators::IAllocator& a, Reference* ref) {
  if (ref->stream.packets.empty()) return false;
  ref->observer.reset(new Observer(ref->stream.advanced));
  Vc1Decoder dec(a);
  if (!ref->stream.advanced && !dec.configure(config_of(ref->stream), ref->stream.width, ref->stream.height)) return false;
  dec.set_frame_observer(ref->observer.get());
  for (size_t k = 0; k < ref->units(); ++k) {
    dec.decode(ref->unit(k), int64_t(k) * 1000);
    take(dec, &ref->out);
  }
  dec.flush();
  take(dec, &ref->out);
  return !ref->out.pts.empty() && ref->observer->pictures.size() + size_t(ref->observer->skipped) == ref->units();
}

static bool make_reference(allocators::IAllocator& a, const char* name, Reference* ref) { return load(name, &ref->stream) && make_reference_from(a, ref); }

// The decode engine of the simulated GPU.
struct FakeDecoder {
  const Reference* reference = nullptr;
  bool check_content = true;
  std::map<uint64_t, uint64_t> hash_to_picture;   // content hash of a painted surface -> the picture (by position among the decoded ones)
  std::map<uint64_t, uint64_t> dmv_of;            // surface address -> the direct motion vector buffer a P picture wrote with it
  int pictures = 0, problems = 0;
  size_t next = 0;

  static uint64_t addr(const std::vector<uint32_t>& d, size_t at) { return (uint64_t{d[at + 1]} << 32) | d[at]; }
  void problem(const char* what) {
    ++problems;
    printf("  fake engine: %s\n", what);
  }

  // Which picture a surface holds: -1 for an address of nothing, -2 for a surface nobody painted.
  long picture_in(FakeGpu& gpu, uint64_t surface, uint64_t bytes) {
    if (!surface) return -1;
    uint64_t h = 1469598103934665603ull;
    for (uint64_t off = 0; off < bytes; off += 4096) {
      bool ok;
      uint8_t* p = gpu.ppgtt_bytes(surface + off, 4096, "reference surface", &ok);
      if (!ok) return -3;
      for (uint32_t k = 0; k < 4096; ++k) h = (h ^ p[k]) * 1099511628211ull;
    }
    auto it = hash_to_picture.find(h);
    return it == hash_to_picture.end() ? -2 : long(it->second);
  }

  void run(FakeGpu& gpu, const std::vector<uint32_t>& d) {
    uint64_t out_pre = 0, out_post = 0, refs[16] = {}, bitstream = 0, bsd_mpc = 0, bitplane = 0, dmv_write = 0, dmv_read = 0;
    uint32_t format = 99, mode_flags = 0, surf_w = 0, surf_h = 0, surf_pitch = 0, surf_chroma = 0;
    uint32_t pic[6] = {}, pred[6] = {};
    bool have_pic = false, have_pred = false, have_dmv = false;
    struct Unit { uint32_t size, offset, bit, row, next_row; };
    std::vector<Unit> units;
    size_t i = 0;
    while (i < d.size()) {
      const uint32_t h = d[i];
      if (h == 0) { ++i; continue; }
      if ((h >> 23) == 0x0A) break;
      if ((h >> 29) == 0) { i += (h & 0x3F) + 2; continue; }
      const size_t len = (h & 0xFF) + 2;
      if (i + len > d.size()) { problem("command runs past the batch"); return; }
      switch (h & 0xFFFF0000) {
        case kMfxPipeModeSelect: format = d[i + 1] & 0xF; mode_flags = d[i + 1]; break;
        case kMfxSurfaceState:
          surf_w = ((d[i + 2] >> 4) & 0x3FFF) + 1;
          surf_h = ((d[i + 2] >> 18) & 0x3FFF) + 1;
          surf_pitch = ((d[i + 3] >> 3) & 0x1FFFF) + 1;
          surf_chroma = d[i + 4] & 0xFFFF;
          break;
        case kMfxPipeBufAddrState:
          out_pre = addr(d, i + 1);
          out_post = addr(d, i + 4);
          for (int k = 0; k < 16; ++k) refs[k] = addr(d, i + 19 + 2 * k);
          break;
        case kMfxBspBufBaseAddrState: bsd_mpc = addr(d, i + 1); bitplane = addr(d, i + 7); break;
        case kMfxIndObjBaseAddrState: bitstream = addr(d, i + 1); break;
        case kMfdVc1LongPicState: for (int k = 0; k < 5; ++k) pic[k] = d[i + 1 + k]; have_pic = true; break;
        case kMfxVc1PredPipeState: for (int k = 0; k < 5; ++k) pred[k] = d[i + 1 + k]; have_pred = true; break;
        case kMfxVc1DirectmodeState: dmv_write = addr(d, i + 1); dmv_read = addr(d, i + 4); have_dmv = true; break;
        case kMfdVc1BsdObject: units.push_back({d[i + 1], d[i + 2], d[i + 4] & 7, d[i + 3] >> 16, d[i + 3] & 0xFFFF}); break;
        default: break;
      }
      i += len;
    }
    if (format != kFormatVc1 || !have_pic || !have_pred || !have_dmv || units.empty()) { problem("incomplete VC-1 batch"); return; }
    const uint32_t width_mbs = (pic[0] & 0xFFFF) + 1, height_mbs = ((pic[0] >> 16) & 0x3FFF) + 1;
    const bool loop_filter = (pic[1] >> 5) & 1;
    const uint64_t out = loop_filter ? out_post : out_pre;
    if (!out || (loop_filter ? out_pre : out_post) != 0 || bool((mode_flags >> 9) & 1) != loop_filter || bool((mode_flags >> 8) & 1) == loop_filter) problem("output surface selection");
    if (surf_w != width_mbs * 16 || surf_h != height_mbs * 16) problem("surface size");
    const uint32_t ptype = (pic[2] >> 26) & 7;
    for (uint64_t a : {out, bsd_mpc, bitstream}) {
      bool ok;
      if (!gpu.ppgtt_bytes(a, 1, "command address", &ok) || !ok) problem("an address in the commands is not mapped");
    }
    Nv12TiledLayout layout;
    if (!nv12_tiled_layout(width_mbs * 16, height_mbs * 16, &layout) || layout.pitch != surf_pitch || layout.chroma_offset_rows != surf_chroma) { problem("surface layout"); return; }
    const size_t k = next++;
    const RefPicture* rp = reference && check_content && k < reference->observer->pictures.size() ? &reference->observer->pictures[k] : nullptr;
    if (rp) {
      const vc1::SequenceHeader& s = rp->seq;
      const vc1::PictureHeader& p = rp->pic;
      const bool adv = s.profile == vc1::kAdvanced;
      const bool intra = p.type == vc1::kPictureI || p.type == vc1::kPictureBI;
      if (ptype != uint32_t(p.type) || width_mbs != uint32_t(s.mb_width) || height_mbs != uint32_t(s.mb_height)) problem("picture type or size differ from the stream's");
      if (bool(pic[1] >> 0 & 1) != adv || bool((pic[1] >> 3) & 1) || (pic[2] >> 24 & 3) != 0 || ((pic[1] >> 24) & 0xFF) != (width_mbs + 1) / 2 - 1) problem("profile, field flag or bit plane row size");
      if (bool((pic[1] >> 14) & 1) != (s.quantizer_mode == 0) || int((pic[1] >> 13) & 1) != p.rnd || bool((pic[1] >> 12) & 1) != s.resync_marker || loop_filter != s.loop_filter) problem("quantizer mode, rounding, sync marker or loop filter");
      // Overlap: the rules of the reference driver.
      bool overlap = false;
      if (s.overlap) {
        if (adv) {
          if (p.type == vc1::kPictureP && p.pq >= 9) overlap = true;
          if (intra) overlap = p.pq >= 9 || p.condover == vc1::kCondOverAll || p.condover == vc1::kCondOverSelect;
        } else {
          overlap = p.pq >= 9 && p.type != vc1::kPictureB;
        }
      }
      if (bool((pic[1] >> 4) & 1) != overlap) problem("overlap flag");
      if (int((pic[2] >> 8) & 31) != p.pq || int((pic[3] >> 1) & 1) != int(p.halfpq) || int(pic[3] & 1) != int(p.pquantizer)) problem("picture quantizer");
      static const uint8_t scale[21] = {128, 85, 170, 64, 192, 51, 102, 153, 204, 43, 215, 37, 74, 111, 148, 185, 222, 32, 96, 160, 224};
      if (p.type == vc1::kPictureB && int(pic[2] & 0xFF) != scale[p.bfraction_index]) problem("B fraction scale");
      if (!intra) {
        const int mode = p.mv_mode == vc1::kMvIntensityComp ? p.mv_mode2 : p.mv_mode;
        const uint32_t unified = mode == vc1::kMv1Mv ? 1 : mode == vc1::kMv1MvHpel ? 2 : mode == vc1::kMv1MvHpelBilin ? 3 : 0;
        const uint32_t interp = ((mode == vc1::kMv1MvHpelBilin) ? 8u : 0u) | (s.fastuvmc ? 1u : 0u);
        if (((pic[3] >> 28) & 3) != unified || ((pic[1] >> 8) & 15) != interp || bool((pic[3] >> 26) & 1) != s.fastuvmc || int((pic[3] >> 8) & 3) != p.mvrange) problem("motion vector mode or range");
        const bool vst = s.vstransform;
        const int tt = vst ? p.ttfrm : vc1::kTt8x8;
        const uint32_t level = tt == vc1::kTt8x4 ? 1 : tt == vc1::kTt4x8 ? 2 : tt == vc1::kTt4x4 ? 3 : 0;
        if (((pic[4] >> 12) & 3) != level || bool((pic[4] >> 11) & 1) != (vst ? p.ttmbf : true)) problem("transform type");
        if (int((pic[4] >> 6) & 3) != p.c_ac_table_index) problem("AC coding set of an inter picture");
      } else if (int((pic[4] >> 6) & 3) != p.y_ac_table_index) {
        problem("AC coding set of an intra picture");
      }
      if (int((pic[4] >> 4) & 3) != p.c_ac_table_index || int((pic[4] >> 3) & 1) != p.dc_table_index || int(pic[4] & 7) != p.cbptab || int((pic[4] >> 20) & 7) != p.mv_table_index) problem("tables");
      // The alternative quantizer.
      uint32_t config = 0, mask = 0, alt_pq = uint32_t(p.altpq);
      if (s.dquant == 2) { config = 1; mask = 0xF; }
      else if (s.dquant == 1) {
        if (!p.dquantfrm) alt_pq = 0;
        else {
          config = 1;
          if (p.dqprofile == vc1::kDqAllMbs) config = p.dqbilevel ? 3 : 2;
          else if (p.dqprofile == vc1::kDqFourEdges) mask = 0xF;
          else if (p.dqprofile == vc1::kDqDoubleEdges) mask = p.dqsbedge == 3 ? 0x9 : (0x3u << p.dqsbedge);
          else mask = 1u << p.dqsbedge;
        }
      }
      if (((pic[3] >> 4) & 15) != mask || ((pic[3] >> 2) & 3) != config || ((pic[2] >> 16) & 31) != alt_pq) problem("dquant");
      // Which bit planes the picture has, which are in the macroblock layer, and the buffer.
      const bool present = !rp->skip.empty() || !rp->mv_type.empty() || !rp->direct.empty() || !rp->acpred.empty() || !rp->overflags.empty();
      if (bool(pic[4] >> 31) != present || (bitplane != 0) != present) problem("bit plane use");
      if (bool((pic[4] >> 28) & 1) != p.skip_raw && (p.type == vc1::kPictureP || p.type == vc1::kPictureB)) problem("raw skip flag");
      if (present) {
        const uint32_t row_bytes = (width_mbs + 1) / 2;
        for (uint32_t y = 0; y < height_mbs; ++y)
          for (uint32_t x = 0; x < width_mbs; ++x) {
            uint32_t v = 0;
            const std::vector<uint8_t>* pl[3];
            if (p.type == vc1::kPictureP) { pl[0] = &rp->direct; pl[1] = &rp->skip; pl[2] = &rp->mv_type; }
            else if (p.type == vc1::kPictureB) { pl[0] = &rp->direct; pl[1] = &rp->skip; pl[2] = &rp->overflags; }
            else { pl[0] = &rp->overflags; pl[1] = &rp->acpred; pl[2] = &rp->overflags; }
            if (intra) pl[0] = &rp->mv_type;   // (no field transform: empty)
            if (p.type == vc1::kPictureB) pl[2] = &rp->mv_type;   // (no forward plane: empty)
            for (uint32_t b = 0; b < 3; ++b)
              if (!pl[b]->empty()) v |= uint32_t((*pl[b])[uint64_t{y} * width_mbs + x] & 1) << b;
            bool ok;
            uint8_t* byte = gpu.ppgtt_bytes(bitplane + uint64_t{y} * row_bytes + x / 2, 1, "bit plane", &ok);
            if (!ok) { problem("bit plane not mapped"); break; }
            if (((*byte >> (4 * (x & 1))) & 15) != v) { problem("bit plane contents differ"); y = height_mbs; break; }
          }
      }
      // Intensity compensation of the forward reference.
      const bool ic = rp->ic && (p.type == vc1::kPictureP || p.type == vc1::kPictureB);
      if (bool((pred[0] >> 10) & 1) != ic || (ic && (int(pred[1] & 0xFF) != rp->ic_scale || int((pred[1] >> 16) & 0xFF) != rp->ic_shift)) || ((pred[0] >> 4) & 15) != 0) problem("intensity compensation");
      // The references: the pictures painted for the pictures this one predicts from.
      auto in_ref = [&](uint64_t a) { return picture_in(gpu, a, layout.size); };
      if (p.type == vc1::kPictureP && (in_ref(refs[0]) != rp->forward || in_ref(refs[1]) != rp->forward)) problem("forward reference is not the picture it should be");
      if (p.type == vc1::kPictureB && (in_ref(refs[0]) != rp->forward || in_ref(refs[1]) != rp->backward)) problem("B references are not the pictures they should be");
      for (int q = 2; q < 16; ++q)
        if (refs[q] != refs[q & 1]) { problem("reference array does not repeat forward, backward"); break; }
      // The direct motion vectors: a P picture writes its buffer, a B picture reads the one of a backward reference that was a P picture.
      if (p.type == vc1::kPictureP) { if (!dmv_write || dmv_read) problem("P picture motion vector buffers"); }
      else if (p.type == vc1::kPictureB) {
        const bool valid = bool((pic[1] >> 15) & 1);
        if (valid != rp->backward_is_p || dmv_write || (valid && dmv_read != dmv_of[refs[1]]) || (!valid && dmv_read)) problem("B picture motion vector buffers");
      } else if (dmv_write || dmv_read) problem("an intra picture has motion vector buffers");
      // The units: where each one's first macroblock is, in the stream without its prevention bytes.
      const size_t n = rp->units.size();
      if (units.size() != n) { problem("unit count differs"); return; }
      uint64_t base = 0;
      for (size_t u = 0; u < n; ++u) {
        std::vector<uint8_t> stored = rp->units[u];
        size_t skip = 0;
        if (stored.size() >= 4 && stored[0] == 0 && stored[1] == 0 && stored[2] == 1) skip = 4;
        const std::vector<uint8_t> payload(stored.begin() + long(skip), stored.end());
        const std::vector<uint8_t> clean = unescape(payload.data(), payload.size());
        const uint32_t mb_bits = rp->info[u].mb_bit_offset;
        // The engine's data: from its offset to the unit's end (as far as the buffer's content goes).
        std::vector<uint8_t> hw;
        for (uint32_t off = 0; off < units[u].size; ++off) {
          bool ok;
          uint8_t* p1 = gpu.ppgtt_bytes(bitstream + units[u].offset + off, 1, "bitstream", &ok);
          if (!ok) { problem("bitstream not mapped"); return; }
          hw.push_back(*p1);
        }
        const std::vector<uint8_t> hw_clean = unescape(hw.data(), hw.size());
        if (int(units[u].row) != rp->info[u].start_row || units[u].next_row != (u + 1 < n ? uint32_t(rp->info[u + 1].start_row) : height_mbs)) problem("unit rows differ");
        // Same bits from the first macroblock on: the engine's byte holds the macroblock at bit `bit`; the stream's at mb_bits.
        bool same = (mb_bits >> 3) <= clean.size();
        if (same) {
          const size_t want = clean.size() - (mb_bits >> 3);
          same = hw_clean.size() >= want && units[u].bit == (mb_bits & 7);
          for (size_t q = 0; same && q < want; ++q) same = hw_clean[q] == clean[(mb_bits >> 3) + q];
        }
        if (!same) problem("unit data or first macroblock position differs");
        base += stored.size() - skip;
      }
      (void)base;
    }
    // "Decode": paint the output picture with the CPU decoder's, tiled.
    uint8_t* page = nullptr;
    uint64_t page_index = ~0ull;
    auto put = [&](uint64_t offset, uint8_t value) {
      if (offset / 4096 != page_index) {
        bool ok;
        page = gpu.ppgtt_bytes(out + (offset / 4096) * 4096, 4096, "output surface", &ok);
        page_index = offset / 4096;
        if (!ok) page = nullptr;
      }
      if (page) page[offset % 4096] = value;
    };
    for (uint64_t off = 0; off < layout.size; ++off) put(off, 0xEE);
    if (rp) {
      for (uint32_t y = 0; y < rp->height; ++y)
        for (uint32_t x = 0; x < rp->width; ++x) put(y_tile_offset(layout.pitch, x, y), rp->plane[0][uint64_t{y} * rp->stride[0] + x]);
      for (uint32_t y = 0; y < rp->height / 2; ++y)
        for (uint32_t x = 0; x < rp->width / 2; ++x) {
          put(y_tile_offset(layout.pitch, 2 * x, layout.chroma_offset_rows + y), rp->plane[1][uint64_t{y} * rp->stride[1] + x]);
          put(y_tile_offset(layout.pitch, 2 * x + 1, layout.chroma_offset_rows + y), rp->plane[2][uint64_t{y} * rp->stride[2] + x]);
        }
    }
    {
      uint64_t h = 1469598103934665603ull;
      for (uint64_t off = 0; off < layout.size; off += 4096) {
        bool ok;
        uint8_t* p2 = gpu.ppgtt_bytes(out + off, 4096, "output surface", &ok);
        if (!ok) break;
        for (uint32_t q = 0; q < 4096; ++q) h = (h ^ p2[q]) * 1099511628211ull;
      }
      hash_to_picture[h] = k;
    }
    if (ptype == vc1::kPictureP) dmv_of[out] = dmv_write;
    ++pictures;
  }
};

struct Rig2 : Rig {
  FakeDecoder engine;
  Rig2() { gpu.on_batch = [this](FakeGpu& g, const std::vector<uint32_t>& d) { engine.run(g, d); }; }
};

static DecoderConfig config_for(const Stream& s) {
  DecoderConfig c;
  c.codec = VideoCodec::Vc1;
  c.output = PixelFormat::I420;
  c.width = s.width;
  c.height = s.height;
  c.codec_config = config_of(s);
  return c;
}

static void compare_stream(const char* name) {
  printf("  %s\n", name);
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, name, &ref));
    if (ref.units() == 0) return;
    printf("    %zu units in, %zu pictures decoded by the engine, %zu out\n", ref.units(), ref.observer->pictures.size(), ref.out.pts.size());
    Rig2 rig;
    rig.engine.reference = &ref;
    {
      VcsDriver driver(rig.mem);
      CHECK(driver.initialize(rig.config()));
      {
        IntelVc1Decoder session(a, driver, config_for(ref.stream));
        CHECK(session.prepare());
        Output out;
        for (size_t k = 0; k < ref.units(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
        session.flush();
        drain(session, &out);
        CHECK(session.statistics().errors == 0 && session.statistics().frames == ref.out.pts.size());
        CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
        CHECK(rig.engine.pictures == int(ref.observer->pictures.size()));
        CHECK(session.pictures_decoded() == ref.observer->pictures.size() + size_t(ref.observer->skipped));
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

static void test_lease_and_config() {
  TestAllocator a;
  Stream main_profile;
  CHECK(load("sm_ipp", &main_profile));
  Rig2 rig;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    {
      IntelVc1Decoder first(a, driver, config_for(main_profile));
      CHECK(first.prepare());
      IntelVc1Decoder second(a, driver, config_for(main_profile));
      CHECK(!second.prepare());
    }
    DecoderConfig junk = config_for(main_profile);
    uint8_t bytes[4];
    memcpy(bytes, main_profile.struct_c, 4);
    bytes[0] = uint8_t((bytes[0] & 0x3F) | 0xC0);   // the Complex profile
    junk.codec_config = ConstByteView(bytes, 4);
    IntelVc1Decoder bad(a, driver, junk);
    CHECK(!bad.prepare());
    DecoderConfig shorter = config_for(main_profile);
    shorter.codec_config = ConstByteView(main_profile.struct_c, 3);
    IntelVc1Decoder bad2(a, driver, shorter);
    CHECK(!bad2.prepare());
    IntelVc1Decoder good(a, driver, config_for(main_profile));
    CHECK(good.prepare());   // the lease was not kept by the sessions that failed
  }
  CHECK(a.live == 0);
}

// The engine stops answering in the middle of a stream.
static void test_hang() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference(a, "sm_ipp", &ref));
  Rig2 rig;
  rig.engine.reference = &ref;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    int live_with_session = 0;
    {
      IntelVc1Decoder session(a, driver, config_for(ref.stream));
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
  for (const char* name : {"sm_ipb_f", "adv_ipb", "adv_skip"}) {
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
          IntelVc1Decoder session(a, driver, config_for(ref.stream));
          CHECK(session.prepare());
          Output out;
          for (size_t k = 0; k < ref.units(); ++k) {
            std::vector<uint8_t> au = ref.stream.packets[k];
            if (k > 0 && !au.empty() && variant % 2 == 1) au.resize(au.size() * (variant + 1) / 25 + 1);
            else if (k > 0 && !au.empty()) au[(variant * 7919 + k * 104729) % au.size()] ^= uint8_t(1u << (variant % 8));
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

// The device: VC-1 sessions go to the engine, one at a time, everything else stays on the CPU.
static void test_device() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference(a, "sm_ipb", &ref));
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
    CHECK(device.hardware_decode() && (device.info().accelerated_decode_codecs & codec_mask(VideoCodec::Vc1)));
    CHECK(device.engine_decode_codecs() & codec_mask(VideoCodec::Vc1) && !(device.engine_encode_codecs() & codec_mask(VideoCodec::Vc1)));
    {
      UniquePointer<IVideoDecoder> hw = device.create_decoder(config_for(ref.stream));
      CHECK(hw != nullptr);
      Output out;
      for (size_t k = 0; k < ref.units(); ++k) CHECK(feed(*hw, ref.unit(k), int64_t(k) * 1000, &out));
      hw->flush();
      drain(*hw, &out);
      CHECK(out.pts == ref.out.pts && out.bytes == ref.out.bytes);
      CHECK(rig.engine.pictures == int(ref.observer->pictures.size()));
      // The engine is taken: the next session runs on the CPU and sends nothing to it.
      UniquePointer<IVideoDecoder> sw = device.create_decoder(config_for(ref.stream));
      CHECK(sw != nullptr);
      Output out2;
      for (size_t k = 0; k < ref.units(); ++k) CHECK(feed(*sw, ref.unit(k), int64_t(k) * 1000, &out2));
      sw->flush();
      drain(*sw, &out2);
      CHECK(out2.bytes == ref.out.bytes && rig.engine.pictures == int(ref.observer->pictures.size()));
    }
  }
  CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
  CHECK(a.live == 0);
}

int main() {
  static const char* const streams[] = {"simple_intra", "sm_intra", "sm_intra_lf", "sm_intra_q", "sm_intra_nu", "adv_intra", "adv_intra_lf", "adv_intra_odd", "sp_ipp", "sm_ipp", "sm_ipp_q", "sm_ipp_nu",
                                        "sm_ipb", "sm_ipb_f", "adv_ipp", "adv_ipb", "adv_ipp_odd", "adv_skip"};
  for (const char* name : streams)
    if (!getenv("ONLY") || !strcmp(getenv("ONLY"), name)) compare_stream(name);
  test_lease_and_config();
  test_hang();
  test_damaged_streams();
  test_device();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("IntelVc1SessionTest passed\n");
  return 0;
}
