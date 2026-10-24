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


// Host-side tests for the VCN MPEG-2 decode session (AmdVcnMpeg2Decoder) against a simulated engine: the ring controller of FakeAmdVcn.hpp and a stand-in for the engine's
// firmware. The stand-in reads the messages the way the firmware does (offsets written out here by hand from Mesa's structures), checks that the buffers it is given are
// inside the work memory and do not overlap, that the bitstream holds the picture's slices behind their start codes, that the matrices and the picture coding parameters
// are the stream's, and that the pictures the message names as references are the ones it holds in the slots the message names; then it "decodes" by writing the picture a
// separate CPU decoder made at the same position in the stream into the target surface, as linear NV12 (for a field picture only the lines of its field). The session must
// hand back exactly that decoder's pictures, in the same order.
//
// This does not show that the real engine decodes anything: the message contents the stand-in checks are the contents Mesa sends, as far as they are known.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <vector>
#include "FakeAmdVcn.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnMpeg2Decoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVideoEngine.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"

using namespace hardware;
using namespace fakevcn;

// The stream the engine's self-test decodes (kMpeg2SelfTestStream, kMpeg2SelfTestEnds, ...).
namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestMpeg2Stream.inc"
}

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

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

// Splits an elementary stream into units that each end before the next picture's headers (a sequence header, a group of pictures header or a picture header that
// follows slices starts a unit).
static std::vector<std::pair<size_t, size_t>> split_pictures(const std::vector<uint8_t>& d) {
  std::vector<std::pair<size_t, size_t>> units;
  size_t begin = 0;
  bool slices = false;
  for (size_t i = 0; i + 4 <= d.size(); ++i) {
    if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
    const int code = d[i + 3];
    if ((code == 0xB3 || code == 0xB8 || code == 0x00) && slices) {
      units.push_back({begin, i});
      begin = i;
      slices = false;
    }
    if (code >= 0x01 && code <= 0xAF) slices = true;
  }
  units.push_back({begin, d.size()});
  return units;
}

struct Output {
  std::vector<int64_t> pts;
  std::vector<std::vector<uint8_t>> bytes;
  std::vector<VideoFormat> format;
};

static void take(Mpeg2Decoder& dec, Output* out) {
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

// What the CPU decoder says about each picture it decodes (a field is a picture): the description an engine is given, the picture it makes, and which earlier pictures it
// predicts from (by position in the stream).
struct RefPicture {
  int type = 0, structure = 0, f_code[2][2] = {{0, 0}, {0, 0}};
  int intra_dc_precision = 0;
  bool top_field_first = false, frame_pred_frame_dct = false, concealment_motion_vectors = false, q_scale_type = false, intra_vlc_format = false, alternate_scan = false, second_field = false;
  uint8_t intra_matrix[64] = {}, non_intra_matrix[64] = {};
  int forward = -1, backward = -1;
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> plane[3];
  uint32_t stride[3] = {0, 0, 0};
  std::vector<uint8_t> bits;   // the slices, each behind its start code (filled in from the unit, as the CPU decoder does not keep them)
};

class Observer final : public mpeg2::IFrameObserver {
 public:
  std::vector<RefPicture> pictures;
  void picture_decoded(const mpeg2::OffloadPicture& o) override {
    RefPicture r;
    r.type = o.pic->type;
    r.structure = o.pic->structure;
    for (int d = 0; d < 2; ++d)
      for (int c = 0; c < 2; ++c) r.f_code[d][c] = o.pic->f_code[d][c];
    r.intra_dc_precision = o.pic->intra_dc_precision;
    r.top_field_first = o.pic->top_field_first;
    r.frame_pred_frame_dct = o.pic->frame_pred_frame_dct;
    r.concealment_motion_vectors = o.pic->concealment_motion_vectors;
    r.q_scale_type = o.pic->q_scale_type;
    r.intra_vlc_format = o.pic->intra_vlc_format;
    r.alternate_scan = o.pic->alternate_scan;
    r.second_field = o.second_field;
    memcpy(r.intra_matrix, o.seq->intra_matrix, 64);
    memcpy(r.non_intra_matrix, o.seq->non_intra_matrix, 64);
    r.width = uint32_t(o.seq->mb_width) * 16;
    r.height = uint32_t(o.seq->mb_height) * 16;
    if (o.forward_slot >= 0) r.forward = owner_[o.forward_slot];
    if (o.backward_slot >= 0) r.backward = owner_[o.backward_slot];
    for (int c = 0; c < 3; ++c) {
      const uint32_t h = c == 0 ? r.height : r.height / 2;
      r.stride[c] = uint32_t(o.stride[c]);
      r.plane[c].assign(o.plane[c].data(), o.plane[c].data() + uint64_t{r.stride[c]} * h);
    }
    owner_[o.slot] = int(pictures.size());
    pictures.push_back(std::move(r));
  }

 private:
  int owner_[mpeg2::kPoolSize] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};

// The CPU decoder's view of a stream: its units, its pictures in coding order and its output.
struct Reference {
  std::vector<uint8_t> file;
  std::vector<std::pair<size_t, size_t>> units;
  Output out;
  Observer observer;
  ConstByteView unit(size_t k) const { return ConstByteView(file.data() + units[k].first, units[k].second - units[k].first); }
};

static bool make_reference_from(allocators::IAllocator& a, Reference* ref) {
  if (ref->file.empty()) return false;
  ref->units = split_pictures(ref->file);
  Mpeg2Decoder dec(a);
  dec.set_frame_observer(&ref->observer);
  for (size_t k = 0; k < ref->units.size(); ++k) {
    const size_t before = ref->observer.pictures.size();
    dec.decode(ref->unit(k), int64_t(k) * 1000);
    take(dec, &ref->out);
    // The slices of the unit: every start code from 01 to AF to the next start code (a unit holds one picture).
    const ConstByteView u = ref->unit(k);
    std::vector<uint8_t> bits;
    for (size_t i = 0; i + 4 <= u.size(); ++i) {
      if (u[i] || u[i + 1] || u[i + 2] != 1 || u[i + 3] < 1 || u[i + 3] > 0xAF) continue;
      size_t end = i + 3;
      while (end + 3 <= u.size() && !(u[end] == 0 && u[end + 1] == 0 && u[end + 2] == 1)) ++end;
      if (end + 3 > u.size()) end = u.size();
      bits.insert(bits.end(), u.data() + i, u.data() + end);
      i = end - 1;
    }
    if (ref->observer.pictures.size() == before + 1) ref->observer.pictures.back().bits = bits;
  }
  dec.flush();
  take(dec, &ref->out);
  return !ref->out.pts.empty() && ref->observer.pictures.size() >= ref->units.size();
}

static bool make_reference(allocators::IAllocator& a, const char* name, Reference* ref) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/mpeg2_%s.m2v", name);
  ref->file = read_file(path);
  return make_reference_from(a, ref);
}

static uint32_t word(const uint8_t* p, uint32_t offset) { return p[offset] | (p[offset + 1] << 8) | (p[offset + 2] << 16) | (uint32_t{p[offset + 3]} << 24); }

// The scan the standard says (zigzag), by position, made here rather than taken from the decoder.
static void zigzag(uint8_t* position) {
  int i = 0, x = 0, y = 0, up = 1;
  while (i < 64) {
    position[i++] = uint8_t(y * 8 + x);
    if (up) {
      if (x == 7) { ++y; up = 0; }
      else if (y == 0) { ++x; up = 0; }
      else { ++x; --y; }
    } else {
      if (y == 7) { ++x; up = 1; }
      else if (x == 0) { ++y; up = 1; }
      else { --x; ++y; }
    }
  }
}

struct FakeMapper final : IRegisterMapper {
  uint8_t* host = nullptr;
  uint64_t physical_base = 0, size = 0;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    if (physical < physical_base || length > size || physical - physical_base > size - length) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(host + (physical - physical_base)), length);
    return true;
  }
};

struct Rig {
  static constexpr uint64_t kArena = 40ull << 20;
  static constexpr uint64_t kGpuBase = 0x4'0000'0000ull, kPhysicalBase = 0xE000'0000ull;
  std::vector<uint8_t> arena, ring_page;
  FakeMapper mapper;
  AmdVcnWorkMemory memory;
  Vcn vcn;
  FakeClock clock;
  std::unique_ptr<AmdVcn1Ring> ring;
  AmdVcnWorkMemory::Piece ib, fence;

  const Reference* reference = nullptr;
  bool write_pictures = true;      // the engine really decodes (off: it leaves the target as it was)
  bool check_content = true;       // compare the bitstream and the message with the stream's pictures (off for damaged streams)
  std::map<uint32_t, uint64_t> buffer;
  int creates = 0, destroys = 0, decodes = 0, problems = 0;
  uint32_t session_decodes = 0;    // pictures decoded since the last create: the stream's own count
  std::vector<uint32_t> stream_handles;
  int slot_content[6];             // which picture (by position in the stream) each slot of the engine's picture buffer holds

  explicit Rig(uint64_t arena_bytes = kArena) : arena(arena_bytes), ring_page(AmdVcn1::kRingBytes) {
    for (int& s : slot_content) s = -1;
    mapper.host = arena.data();
    mapper.physical_base = kPhysicalBase;
    mapper.size = arena.size();
    memory.configure(&mapper, kGpuBase, kPhysicalBase, arena.size());
    memory.allocate(4096, 4096, &ib);
    memory.allocate(4096, 4096, &fence);
    memory.keep();
    vcn.windows = {{kGpuBase, arena.data(), arena.size()}};
    vcn.windows.push_back({0x5'0000'0000ull, ring_page.data(), ring_page.size()});
    vcn.ring_gpu = 0x5'0000'0000ull;
    AmdVcn1Ring::Memory m;
    m.ib_gpu_address = ib.gpu_address;
    m.ib = ib.cpu;
    m.fence_gpu_address = fence.gpu_address;
    m.fence = fence.cpu;
    m.ring = MmioRegion(reinterpret_cast<uint64_t>(ring_page.data()), ring_page.size());
    ring.reset(new AmdVcn1Ring(vcn, clock, m));
    vcn.on_command = [this](uint32_t c, uint64_t a) { on_command(c, a); };
    vcn.on_engine = [this] { on_engine(); };
  }

  void problem(const char* what) {
    ++problems;
    printf("  fake engine: %s\n", what);
  }

  void on_command(uint32_t command, uint64_t address) {
    buffer[command] = address;
    if (command != 0) return;
    const uint8_t* m = vcn.translate(address, 40);
    if (!m) { problem("message outside the work memory"); return; }
    const uint32_t type = word(m, 12);
    if (type == 0) {
      ++creates;
      session_decodes = 0;
      for (int& s : slot_content) s = -1;
      stream_handles.push_back(word(m, 16));
      if (word(m, 0) != 40 || word(m, 4) != 56 || word(m, 8) != 1 || word(m, 24) != 1 || word(m, 28) != 40 || word(m, 32) != 16) problem("bad create header");
      const uint8_t* c = vcn.translate(address + 40, 16);
      if (!c || word(c, 0) != 3) problem("create: stream type is not MPEG-2");
    } else if (type == 2) {
      ++destroys;
      if (word(m, 0) != 40 || word(m, 8) != 0) problem("bad destroy header");
    }
  }

  struct Range { const char* name; uint64_t start, size; };

  static uint64_t align_to(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

  void on_engine() {
    const uint64_t msg_addr = buffer[0];
    constexpr uint32_t kMessageBytes = 244 + 160;
    uint8_t m[kMessageBytes];
    if (!vcn.translate(msg_addr, sizeof m)) { problem("decode message outside the work memory"); return; }
    memcpy(m, vcn.translate(msg_addr, sizeof m), sizeof m);
    if (word(m, 12) != 1) { problem("engine started on a message that is not a decode"); return; }
    stream_handles.push_back(word(m, 16));
    ++decodes;
    const uint32_t k = session_decodes++;
    // Header and indexes: the decode message at 56 (188 bytes), the MPEG-2 message (id 10) at 244 (160 bytes).
    if (word(m, 0) != 40 || word(m, 4) != 244 || word(m, 8) != 2 || word(m, 24) != 2 || word(m, 28) != 56 || word(m, 32) != 188 || word(m, 40) != 10 || word(m, 44) != 244 ||
        word(m, 48) != 160)
      problem("bad decode header");
    if (word(m, 20) != k + 1) problem("feedback number is not the picture count");
    const uint32_t dec = 56, mp = 244;
    const uint32_t width = word(m, dec + 8), height = word(m, dec + 12), bsd = word(m, dec + 16), dpb = word(m, dec + 20), dt_size = word(m, dec + 24);
    if (word(m, dec) != 3 || width % 16 || height % 16 || !width || !height) problem("bad decode message");
    if (word(m, dec + 36) != 0 || word(m, dec + 40) != 128 * 1024) problem("context sizes");
    // Every buffer is inside the work memory, and none overlaps another.
    const uint64_t fb = buffer[3];
    if (fb != msg_addr + 0x1000) problem("feedback not where Mesa puts it");
    const Range ranges[] = {{"message", msg_addr, 0x1000 + 2048 + 992}, {"session", buffer[5], 128 * 1024}, {"dpb", buffer[1], dpb}, {"bitstream", buffer[0x100], bsd}, {"target", buffer[2], dt_size}};
    for (size_t i = 0; i < 5; ++i) {
      if (!ranges[i].size || !vcn.translate(ranges[i].start, ranges[i].size)) { char t[80]; snprintf(t, sizeof t, "%s buffer outside the work memory", ranges[i].name); problem(t); }
      for (size_t j = i + 1; j < 5; ++j)
        if (ranges[i].start < ranges[j].start + ranges[j].size && ranges[j].start < ranges[i].start + ranges[i].size) { char t[80]; snprintf(t, sizeof t, "%s and %s overlap", ranges[i].name, ranges[j].name); problem(t); }
    }
    // The picture buffer: six pictures of the size Mesa works out.
    const uint64_t luma = align_to(width, 32) * align_to(height, 16);
    const uint64_t image = align_to(luma + luma / 2, 1024);
    if (dpb < image * 6 || dpb >= image * 6 + 4096) problem("dpb size is not six pictures (to the page)");
    // The pictures it names: slots in range; the references are in the slots they are said to be in.
    const uint32_t cur = word(m, mp), fwd = word(m, mp + 4), bwd = word(m, mp + 8);
    if (cur >= 6 || fwd >= 6 || bwd >= 6) { problem("slot out of range"); return; }
    const RefPicture* pic = reference && check_content && k < reference->observer.pictures.size() ? &reference->observer.pictures[k] : nullptr;
    if (pic) {
      if (pic->forward >= 0 && slot_content[fwd] != pic->forward) problem("the forward reference is not the picture in its slot");
      if (pic->backward >= 0 && slot_content[bwd] != pic->backward) problem("the backward reference is not the picture in its slot");
      if (pic->type == 2 && bwd != fwd && !pic->second_field) problem("a predicted picture should name its one reference twice");
      if (pic->type != 1 && !pic->second_field && (cur == fwd || (pic->type == 3 && cur == bwd))) problem("the picture is decoded over its own reference");
      // The coding parameters and the matrices.
      if (m[mp + 146] != pic->type || m[mp + 153] != pic->structure || m[mp + 152] != pic->intra_dc_precision || m[mp + 154] != pic->top_field_first ||
          m[mp + 155] != pic->frame_pred_frame_dct || m[mp + 156] != pic->concealment_motion_vectors || m[mp + 157] != pic->q_scale_type || m[mp + 158] != pic->intra_vlc_format ||
          m[mp + 159] != pic->alternate_scan || m[mp + 145] != 1)
        problem("picture coding parameters differ from the stream's");
      for (int d = 0; d < 2; ++d)
        for (int c = 0; c < 2; ++c)
          if (m[mp + 148 + d * 2 + c] != pic->f_code[d][c]) problem("f_code differs");
      if (m[mp + 12] != 1 || m[mp + 13] != 1) problem("matrices not loaded");
      uint8_t scan[64];
      zigzag(scan);
      if (!pic->alternate_scan) {
        for (int i = 0; i < 64; ++i)
          if (m[mp + 16 + i] != pic->intra_matrix[scan[i]] || m[mp + 80 + i] != pic->non_intra_matrix[scan[i]]) { problem("quantiser matrix differs"); break; }
      } else {
        // (the alternate scan is not made here: the same weights in some order)
        int a[256] = {}, b[256] = {}, c[256] = {}, e[256] = {};
        for (int i = 0; i < 64; ++i) { ++a[m[mp + 16 + i]]; ++b[pic->intra_matrix[i]]; ++c[m[mp + 80 + i]]; ++e[pic->non_intra_matrix[i]]; }
        if (memcmp(a, b, sizeof a) || memcmp(c, e, sizeof c)) problem("quantiser matrix differs (alternate scan)");
      }
      // The bitstream: every slice behind its start code, then zeros up to bsd_size.
      const uint8_t* bits = vcn.translate(buffer[0x100], bsd);
      if (bits && (pic->bits.size() > bsd || bsd % 128 != 0 || memcmp(bits, pic->bits.data(), pic->bits.size()) != 0)) problem("bitstream differs from the picture's slices");
      for (uint64_t i = pic->bits.size(); bits && i < bsd; ++i) if (bits[i] != 0) { problem("bitstream padding is not zero"); break; }
    }
    const uint8_t* f = vcn.translate(fb, 44);
    if (!f || word(f, 0) != 44 || word(f, 4) != 44 || word(f, 8) != 0) problem("feedback header not initialised");
    // "Decode": the CPU decoder's picture at the same place in the stream, NV12, linear, with the pitch and chroma offset the message names. A field picture's lines only.
    const uint32_t pitch = word(m, dec + 100), uv_pitch_texels = word(m, dec + 104), chroma_offset = word(m, dec + 144);
    if (word(m, dec + 116) != 0 || word(m, dec + 112) != 0) problem("target is not linear");
    if (pic && write_pictures) {
      uint8_t* target = vcn.translate(buffer[2], dt_size);
      if (!target) { problem("no target"); return; }
      if (pic->width > width || pic->height > height || uint64_t{pitch} * height > chroma_offset || chroma_offset + uint64_t{pitch} * (height / 2) > dt_size || uv_pitch_texels * 2 != pitch) {
        problem("target layout does not fit");
        return;
      }
      const bool field = pic->structure != 3;
      if (!field || !pic->second_field) memset(target, 0xEE, dt_size);   // (what the lines of the other field hold is nobody's business)
      const uint32_t first = field && pic->structure == 2 ? 1 : 0, step = field ? 2 : 1;
      for (uint32_t y = first; y < pic->height; y += step) memcpy(target + uint64_t{pitch} * y, pic->plane[0].data() + uint64_t{y} * pic->stride[0], pic->width);
      for (uint32_t y = first; y < pic->height / 2; y += step)
        for (uint32_t x = 0; x < pic->width / 2; ++x) {
          target[chroma_offset + uint64_t{pitch} * y + 2 * x] = pic->plane[1][uint64_t{y} * pic->stride[1] + x];
          target[chroma_offset + uint64_t{pitch} * y + 2 * x + 1] = pic->plane[2][uint64_t{y} * pic->stride[2] + x];
        }
    }
    slot_content[cur] = int(k);
  }
};

static DecoderConfig i420_config() {
  DecoderConfig c;
  c.codec = VideoCodec::Mpeg2;
  c.output = PixelFormat::I420;
  return c;
}

static AmdVcnMpeg2Decoder::Engine engine_of(Rig& rig, uint32_t handle = 7) {
  AmdVcnMpeg2Decoder::Engine e;
  e.ring = rig.ring.get();
  e.memory = &rig.memory;
  e.stream_handle = handle;
  e.job_timeout_ms = 20;
  return e;
}

static void compare_stream(const char* name) {
  printf("  %s\n", name);
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, name, &ref));
    if (ref.units.empty()) return;
    printf("    %zu units in, %zu pictures decoded, %zu out\n", ref.units.size(), ref.observer.pictures.size(), ref.out.pts.size());
    Rig rig;
    rig.reference = &ref;
    {
      AmdVcnMpeg2Decoder session(a, engine_of(rig), i420_config());
      CHECK(session.prepare());
      Output out;
      for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
      session.flush();
      drain(session, &out);
      CHECK(session.statistics().errors == 0 && session.statistics().frames == ref.out.pts.size());
      CHECK(rig.problems == 0 && rig.vcn.protocol_errors == 0);
      CHECK(rig.creates == 1 && rig.decodes == int(ref.observer.pictures.size()) && rig.destroys == 0 && session.pictures_decoded() == ref.observer.pictures.size());
      // The same pictures, in the same order, byte for byte.
      CHECK(out.pts == ref.out.pts);
      for (size_t i = 0; i < out.bytes.size() && i < ref.out.pts.size(); ++i) {
        CHECK(out.format[i] == ref.out.format[i]);
        CHECK(out.bytes[i] == ref.out.bytes[i]);
      }
      CHECK(!out.format.empty() && session.output_format().width == out.format.front().width && session.output_format().height == out.format.front().height);
    }
    CHECK(rig.destroys == 1 && rig.problems == 0);
    CHECK(rig.stream_handles.front() == 7 && rig.stream_handles.back() == 7);
    CHECK(rig.memory.free_bytes() == Rig::kArena - 2 * 4096);
  }
  CHECK(a.live == 0);
}

// A work memory too small for the sequence: the CPU decodes it whole, and the engine is never touched.
static void test_cpu_fallback() {
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, "ibp", &ref));
    Rig rig(160 * 1024);
    AmdVcnMpeg2Decoder session(a, engine_of(rig), i420_config());
    CHECK(session.prepare());
    Output out;
    for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
    session.flush();
    drain(session, &out);
    CHECK(out.pts == ref.out.pts && out.bytes == ref.out.bytes);
    CHECK(rig.creates == 0 && rig.decodes == 0 && rig.vcn.writes.empty() && !session.session_open() && session.statistics().errors == 0);
  }
  CHECK(a.live == 0);
}

static void test_one_session_at_a_time() {
  TestAllocator a;
  {
    Rig rig;
    AmdVcnMpeg2Decoder first(a, engine_of(rig), i420_config());
    CHECK(first.prepare());
    AmdVcnMpeg2Decoder second(a, engine_of(rig, 8), i420_config());
    CHECK(!second.prepare());
  }
  {
    Rig rig;
    DecoderConfig junk = i420_config();
    const uint8_t bytes[4] = {'j', 'u', 'n', 'k'};
    junk.codec_config = ConstByteView(bytes, 4);
    AmdVcnMpeg2Decoder bad(a, engine_of(rig), junk);
    CHECK(!bad.prepare());   // MPEG-2 has no configuration record
  }
  CHECK(a.live == 0);
}

static void test_wedged_engine() {
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, "ipp", &ref));
    Rig rig;
    rig.reference = &ref;
    AmdVcnMpeg2Decoder session(a, engine_of(rig), i420_config());
    CHECK(session.prepare());
    Output out;
    CHECK(feed(session, ref.unit(0), 0, &out));
    CHECK(rig.decodes == 1 && !rig.ring->wedged());
    // The engine stops answering: the next picture times out and is shown grey; the pictures after it are grey too, without the engine being touched again.
    rig.vcn.stall_fence = true;
    CHECK(feed(session, ref.unit(1), 1000, &out));
    CHECK(rig.ring->wedged() && session.statistics().errors >= 1);
    const size_t writes = rig.vcn.writes.size();
    CHECK(feed(session, ref.unit(2), 2000, &out));
    CHECK(rig.vcn.writes.size() == writes);
    session.flush();
    drain(session, &out);
    CHECK(out.bytes.size() >= 3 && out.bytes[0] == ref.out.bytes[0]);
    if (out.bytes.size() >= 3) {
      CHECK(out.bytes[1][0] == 128 && out.bytes[1].back() == 128 && out.bytes[2][0] == 128);
    }
  }
  CHECK(a.live == 0);
}

// Damaged streams: units cut short and bits flipped. The session must stay inside its buffers (the sanitizers say so), never hang, and give everything back.
static void test_damaged_streams() {
  TestAllocator a;
  for (const char* name : {"ibp", "syn_fields"}) {
    Reference ref;
    CHECK(make_reference(a, name, &ref));
    int runs = 0;
    for (size_t variant = 0; variant < 24; ++variant) {
      Rig rig;
      rig.reference = &ref;
      rig.check_content = false;
      {
        AmdVcnMpeg2Decoder session(a, engine_of(rig), i420_config());
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
      CHECK(rig.vcn.protocol_errors == 0);
    }
    CHECK(runs > 100);
  }
  CHECK(a.live == 0);
}

// The engine's own MPEG-2 set-up and self-test, against the same simulated card: the stand-in firmware is given the CPU's pictures for the self-test stream.
static void test_engine_self_test(bool engine_works, bool engine_hangs) {
  TestAllocator a;
  {
    Reference ref;
    ref.file.assign(kMpeg2SelfTestStream, kMpeg2SelfTestStream + sizeof kMpeg2SelfTestStream);
    CHECK(make_reference_from(a, &ref));
    CHECK(ref.units.size() == kMpeg2SelfTestPictures);
    for (size_t k = 0; k < ref.units.size() && k < kMpeg2SelfTestPictures; ++k) CHECK(ref.units[k].second == kMpeg2SelfTestEnds[k]);   // the same split the test makes

    Rig rig;
    rig.reference = &ref;
    rig.write_pictures = engine_works;
    rig.vcn.stall_fence = engine_hangs;
    AmdVcnVideoEngine engine(a, 0x15DD);
    const MmioRegion ring_window(reinterpret_cast<uint64_t>(rig.ring_page.data()), rig.ring_page.size());
    CHECK(engine.set_up_decode(rig.vcn, rig.clock, ring_window, rig.mapper, Rig::kGpuBase, Rig::kPhysicalBase, rig.arena.size()) == nullptr);
    const bool enabled = engine.enable_mpeg2_decode();
    CHECK(enabled == (engine_works && !engine_hangs) && engine.mpeg2_ready() == enabled);
    CHECK(engine.info().accelerated_decode_codecs == (enabled ? codec_mask(VideoCodec::Mpeg2) : 0u));
    CHECK(rig.problems == 0 || !engine_works);
    if (enabled) {
      CHECK(rig.creates == 1 && rig.destroys == 1 && rig.decodes == int(kMpeg2SelfTestPictures));   // the test opened and closed its own session
      // MPEG-2 sessions are the engine's now; other codecs stay with the CPU.
      UniquePointer<IVideoDecoder> session = engine.create_decoder(i420_config());
      CHECK(static_cast<bool>(session));
      DecoderConfig vp9;
      vp9.codec = VideoCodec::Vp9;
      UniquePointer<IVideoDecoder> software = engine.create_decoder(vp9);
      CHECK(static_cast<bool>(software));
      Output out;
      CHECK(feed(*session, ref.unit(0), 0, &out));
      CHECK(rig.creates == 2 && rig.decodes == int(kMpeg2SelfTestPictures) + 1);   // it ran on the engine
      // The engine serves one session at a time: while the first is open, a second one is the CPU's.
      UniquePointer<IVideoDecoder> second = engine.create_decoder(i420_config());
      CHECK(static_cast<bool>(second));
      CHECK(feed(*second, ref.unit(0), 0, &out));
      CHECK(rig.creates == 2 && rig.decodes == int(kMpeg2SelfTestPictures) + 1 && rig.problems == 0);
      session.reset();
      second.reset();
      software.reset();
      engine.shutdown();
      CHECK(!engine.mpeg2_ready() && engine.info().accelerated_decode_codecs == 0);
    } else {
      // Not trusted: nothing accelerated, and MPEG-2 is still served (by the CPU).
      UniquePointer<IVideoDecoder> session = engine.create_decoder(i420_config());
      CHECK(static_cast<bool>(session));
      CHECK(strstr(engine.engine_note(), engine_hangs ? "engine stopped" : "MPEG-2 self-test failed") != nullptr);
    }
  }
  CHECK(a.live == 0);
}

int main() {
  static const char* const streams[] = {"intra", "ipp", "ibp", "interlaced", "interlaced_bff", "alt", "dc10", "highrate", "lowrate", "matrix", "oddsize", "syn_frames", "syn_fields", "syn_fields_bff"};
  for (const char* name : streams) compare_stream(name);
  test_engine_self_test(true, false);
  test_engine_self_test(false, false);
  test_engine_self_test(true, true);
  test_cpu_fallback();
  test_one_session_at_a_time();
  test_wedged_engine();
  test_damaged_streams();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("AmdVcnMpeg2DecoderTest passed\n");
  return 0;
}
