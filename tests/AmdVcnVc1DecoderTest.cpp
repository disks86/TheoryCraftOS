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


// Host-side tests for the VCN VC-1 decode session (AmdVcnVc1Decoder) against a simulated engine: the ring controller of FakeAmdVcn.hpp and a stand-in for the engine's
// firmware. The stand-in reads the messages the way the firmware does (offsets written out here by hand from Mesa's structures), checks that the buffers it is given are
// inside the work memory and do not overlap, that the bitstream holds the picture's frame (and slices, each behind its start code for the Advanced profile), that the
// profile and the sequence flags are the stream's, and that the pictures the message names as references are the ones it holds in the slots the message names; then it
// "decodes" by writing the picture a separate CPU decoder made at the same position in the stream into the target surface, as linear NV12. The session must hand back
// exactly that decoder's pictures, in the same order.
//
// This does not show that the real engine decodes anything: the message contents the stand-in checks are the contents Mesa sends, as far as they are known.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <vector>
#include "FakeAmdVcn.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVc1Decoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVideoEngine.hpp"
#include "Hardware/VideoCodec/Software/Vc1.hpp"

using namespace hardware;
using namespace fakevcn;

// The streams the engine's self-test decodes (kVc1SelfTestMainStream, ...).
namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestVc1Stream.inc"
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

// The streams of the engine's self-test (the arrays hold the packets one after the other).
static void load_self_test(bool advanced, Stream* s) {
  s->advanced = advanced;
  s->width = s->height = kVc1SelfTestSize;
  if (!advanced) memcpy(s->struct_c, kVc1SelfTestMainConfig, 4);
  const uint8_t* bytes = advanced ? kVc1SelfTestAdvancedStream : kVc1SelfTestMainStream;
  const uint64_t* ends = advanced ? kVc1SelfTestAdvancedEnds : kVc1SelfTestMainEnds;
  uint64_t start = 0;
  for (uint32_t i = 0; i < kVc1SelfTestPictures; ++i) {
    s->packets.emplace_back(bytes + start, bytes + ends[i]);
    start = ends[i];
  }
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

static void take(Vc1Decoder& dec, Output* out) {
  VideoFrame f;
  while (dec.receive(&f)) take_frame(f, out);
}

static void drain(IVideoDecoder& dec, Output* out) {
  VideoFrame f;
  while (dec.receive(&f)) take_frame(f, out);
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

// What the CPU decoder says about each picture it decodes: the description an engine is given, the picture it makes, and which earlier pictures it predicts from (by
// position among the pictures the engine decodes: a skipped picture is not one, it is the picture it repeats).
struct RefPicture {
  vc1::SequenceHeader seq;
  int type = 0;
  int forward = -1, backward = -1;
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> plane[3];
  uint32_t stride[3] = {0, 0, 0};
  std::vector<uint8_t> bits;   // what the engine's bitstream buffer should hold
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
      return;
    }
    RefPicture r;
    r.seq = *o.seq;
    r.type = o.pic->type;
    r.width = uint32_t(o.seq->mb_width) * 16;
    r.height = uint32_t(o.seq->mb_height) * 16;
    if (o.forward_slot >= 0) r.forward = owner_[o.forward_slot];
    if (o.backward_slot >= 0) r.backward = owner_[o.backward_slot];
    for (int c = 0; c < 3; ++c) {
      const uint32_t h = c == 0 ? r.height : r.height / 2;
      r.stride[c] = uint32_t(o.stride[c]);
      r.plane[c].assign(o.plane[c].data(), o.plane[c].data() + uint64_t{r.stride[c]} * h);
    }
    bool first = true;
    for (const ConstByteView& u : o.units) {
      const bool coded = u.size() >= 4 && u[0] == 0 && u[1] == 0 && u[2] == 1;
      if (first && advanced_ && !coded) r.bits.insert(r.bits.end(), {0x00, 0x00, 0x01, 0x0D});
      r.bits.insert(r.bits.end(), u.data(), u.data() + u.size());
      first = false;
    }
    owner_[o.slot] = int(pictures.size());
    pictures.push_back(std::move(r));
  }

 private:
  bool advanced_;
  int owner_[vc1::kPoolSize] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};

// The CPU decoder's view of a stream: its pictures in coding order and its output.
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

static uint32_t word(const uint8_t* p, uint32_t offset) { return p[offset] | (p[offset + 1] << 8) | (p[offset + 2] << 16) | (uint32_t{p[offset + 3]} << 24); }
static uint32_t half(const uint8_t* p, uint32_t offset) { return p[offset] | (p[offset + 1] << 8); }

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

  std::vector<const Reference*> references;   // the stream of each session in turn (the first create gets the first)
  bool write_pictures = true;      // the engine really decodes (off: it leaves the target as it was)
  bool check_content = true;       // compare the bitstream and the message with the stream's pictures (off for damaged streams)
  std::map<uint32_t, uint64_t> buffer;
  int creates = 0, destroys = 0, decodes = 0, problems = 0;
  uint32_t session_decodes = 0;    // pictures decoded since the last create: the stream's own count
  std::vector<uint32_t> stream_handles;
  int slot_content[5];             // which picture (by position among the decoded ones) each slot of the engine's picture buffer holds

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
      if (!c || word(c, 0) != 1) problem("create: stream type is not VC-1");
    } else if (type == 2) {
      ++destroys;
      if (word(m, 0) != 40 || word(m, 8) != 0) problem("bad destroy header");
    }
  }

  struct Range { const char* name; uint64_t start, size; };

  static uint64_t align_to(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

  void on_engine() {
    const uint64_t msg_addr = buffer[0];
    constexpr uint32_t kMessageBytes = 244 + 36;
    uint8_t m[kMessageBytes];
    if (!vcn.translate(msg_addr, sizeof m)) { problem("decode message outside the work memory"); return; }
    memcpy(m, vcn.translate(msg_addr, sizeof m), sizeof m);
    if (word(m, 12) != 1) { problem("engine started on a message that is not a decode"); return; }
    stream_handles.push_back(word(m, 16));
    ++decodes;
    const uint32_t k = session_decodes++;
    // Header and indexes: the decode message at 56 (188 bytes), the VC-1 message (id 7) at 244 (36 bytes).
    if (word(m, 0) != 40 || word(m, 4) != 244 || word(m, 8) != 2 || word(m, 24) != 2 || word(m, 28) != 56 || word(m, 32) != 188 || word(m, 40) != 7 || word(m, 44) != 244 ||
        word(m, 48) != 36)
      problem("bad decode header");
    if (word(m, 20) != k + 1) problem("feedback number is not the picture count");
    const Reference* reference = creates >= 1 && size_t(creates) <= references.size() ? references[size_t(creates) - 1] : nullptr;
    const uint32_t dec = 56, vp = 244;
    const uint32_t width = word(m, dec + 8), height = word(m, dec + 12), bsd = word(m, dec + 16), dpb = word(m, dec + 20), dt_size = word(m, dec + 24);
    if (word(m, dec) != 1 || !width || !height) problem("bad decode message");
    if (word(m, dec + 36) != 0 || word(m, dec + 40) != 128 * 1024) problem("context sizes");
    const RefPicture* pic = reference && check_content && k < reference->observer->pictures.size() ? &reference->observer->pictures[k] : nullptr;
    // The size is in macroblocks for the Simple and Main profiles, in samples for the Advanced one.
    const bool advanced = reference ? reference->stream.advanced : width > 64;
    const uint32_t w = advanced ? width : width * 16, h = advanced ? height : height * 16;
    if (w % 16 || h % 16) problem("picture size is not a whole number of macroblocks");
    // Every buffer is inside the work memory, and none overlaps another.
    const uint64_t fb = buffer[3];
    if (fb != msg_addr + 0x1000) problem("feedback not where Mesa puts it");
    const Range ranges[] = {{"message", msg_addr, 0x1000 + 2048 + 992}, {"session", buffer[5], 128 * 1024}, {"dpb", buffer[1], dpb}, {"bitstream", buffer[0x100], bsd}, {"target", buffer[2], dt_size}};
    for (size_t i = 0; i < 5; ++i) {
      if (!ranges[i].size || !vcn.translate(ranges[i].start, ranges[i].size)) { char t[80]; snprintf(t, sizeof t, "%s buffer outside the work memory", ranges[i].name); problem(t); }
      for (size_t j = i + 1; j < 5; ++j)
        if (ranges[i].start < ranges[j].start + ranges[j].size && ranges[j].start < ranges[i].start + ranges[i].size) { char t[80]; snprintf(t, sizeof t, "%s and %s overlap", ranges[i].name, ranges[j].name); problem(t); }
    }
    // The picture buffer: Mesa's sum for VC-1 (five pictures of the size, the context, intra transform, deblocking and bit plane areas).
    const uint64_t luma = align_to(w, 32) * align_to(h, 16);
    const uint64_t image = align_to(luma + luma / 2, 1024);
    const uint64_t wmb = w / 16, hmb = align_to(h / 16, 2);
    const uint64_t expected_dpb = image * 5 + wmb * hmb * 128 + wmb * 64 + wmb * 128 + align_to((wmb > hmb ? wmb : hmb) * 7 * 16, 64);
    if (dpb < expected_dpb || dpb >= expected_dpb + 4096) problem("dpb size is not Mesa's (to the page)");
    // The pictures it names: slots in range; the references are in the slots they are said to be in.
    const uint32_t cur = half(m, vp + 24), deblocked = half(m, vp + 26), fwd = half(m, vp + 28), bwd = half(m, vp + 30);
    if (cur >= 5 || fwd >= 5 || bwd >= 5) { problem("slot out of range"); return; }
    if (deblocked != cur) problem("the deblocked picture is not the decoded one");
    if (word(m, vp + 20) != 1 || word(m, vp + 16) != 0 || word(m, vp + 32) != 0) problem("chroma format, structure or cached flag");
    if (pic) {
      if (pic->forward >= 0 && slot_content[fwd] != pic->forward) problem("the forward reference is not the picture in its slot");
      if (pic->backward >= 0 && slot_content[bwd] != pic->backward) problem("the backward reference is not the picture in its slot");
      if (pic->type == vc1::kPictureP && bwd != fwd) problem("a predicted picture should name its one reference twice");
      if (pic->type != vc1::kPictureI && pic->type != vc1::kPictureBI && cur == fwd) problem("the picture is decoded over its own reference");
      if ((pic->type == vc1::kPictureB) && cur == bwd) problem("the B picture is decoded over its backward reference");
      // The profile and the flags, worked out here from the sequence header.
      const vc1::SequenceHeader& s = pic->seq;
      const uint32_t profile = s.profile == vc1::kSimple ? 0 : s.profile == vc1::kMain ? 1 : 2;
      if (word(m, vp) != profile || word(m, vp + 4) != (profile == 0 ? 1u : profile == 1 ? 2u : 4u)) problem("profile or level");
      uint32_t sps = (s.postprocflag << 7) | (s.broadcast << 6) | (s.interlace << 5) | (s.tfcntrflag << 4) | (s.finterpflag << 3) | (s.psf << 1);
      uint32_t pps = (uint32_t(s.max_b_frames) << 16) | (s.overlap << 11) | (uint32_t(s.quantizer_mode) << 9) | (s.vstransform << 0) | (s.multires << 21);
      if (profile != 0) pps |= (s.resync_marker << 20) | (s.rangered << 19) | (s.loop_filter << 5) | (s.fastuvmc << 4) | (s.extended_mv << 3) | (uint32_t(s.dquant) << 1);
      if (word(m, vp + 8) != sps) problem("sps flags differ from the stream's");
      if (word(m, vp + 12) != pps) problem("pps flags differ from the stream's");
      // The bitstream: the frame, then zeros up to bsd_size.
      const uint8_t* bits = vcn.translate(buffer[0x100], bsd);
      if (bits && (pic->bits.size() > bsd || bsd % 128 != 0 || memcmp(bits, pic->bits.data(), pic->bits.size()) != 0)) problem("bitstream differs from the picture's frame");
      for (uint64_t i = pic->bits.size(); bits && i < bsd; ++i) if (bits[i] != 0) { problem("bitstream padding is not zero"); break; }
    }
    const uint8_t* f = vcn.translate(fb, 44);
    if (!f || word(f, 0) != 44 || word(f, 4) != 44 || word(f, 8) != 0) problem("feedback header not initialised");
    // "Decode": the CPU decoder's picture at the same place in the stream, NV12, linear, with the pitch and chroma offset the message names.
    const uint32_t pitch = word(m, dec + 100), uv_pitch_texels = word(m, dec + 104), chroma_offset = word(m, dec + 144);
    if (word(m, dec + 116) != 0 || word(m, dec + 112) != 0) problem("target is not linear");
    if (pic && write_pictures) {
      uint8_t* target = vcn.translate(buffer[2], dt_size);
      if (!target) { problem("no target"); return; }
      if (pic->width > w || pic->height > h || uint64_t{pitch} * h > chroma_offset || chroma_offset + uint64_t{pitch} * (h / 2) > dt_size || uv_pitch_texels * 2 != pitch) {
        problem("target layout does not fit");
        return;
      }
      memset(target, 0xEE, dt_size);
      for (uint32_t y = 0; y < pic->height; ++y) memcpy(target + uint64_t{pitch} * y, pic->plane[0].data() + uint64_t{y} * pic->stride[0], pic->width);
      for (uint32_t y = 0; y < pic->height / 2; ++y)
        for (uint32_t x = 0; x < pic->width / 2; ++x) {
          target[chroma_offset + uint64_t{pitch} * y + 2 * x] = pic->plane[1][uint64_t{y} * pic->stride[1] + x];
          target[chroma_offset + uint64_t{pitch} * y + 2 * x + 1] = pic->plane[2][uint64_t{y} * pic->stride[2] + x];
        }
    }
    slot_content[cur] = int(k);
  }
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

static AmdVcnVc1Decoder::Engine engine_of(Rig& rig, uint32_t handle = 7) {
  AmdVcnVc1Decoder::Engine e;
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
    if (ref.units() == 0) return;
    printf("    %zu units in, %zu pictures decoded by the engine, %zu out\n", ref.units(), ref.observer->pictures.size(), ref.out.pts.size());
    Rig rig;
    rig.references.push_back(&ref);
    {
      AmdVcnVc1Decoder session(a, engine_of(rig), config_for(ref.stream));
      CHECK(session.prepare());
      Output out;
      for (size_t k = 0; k < ref.units(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
      session.flush();
      drain(session, &out);
      CHECK(session.statistics().errors == 0 && session.statistics().frames == ref.out.pts.size());
      CHECK(rig.problems == 0 && rig.vcn.protocol_errors == 0);
      CHECK(rig.creates == 1 && rig.decodes == int(ref.observer->pictures.size()) && rig.destroys == 0 && session.pictures_decoded() == ref.observer->pictures.size());
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
    CHECK(make_reference(a, "sm_ipb", &ref));
    Rig rig(160 * 1024);
    AmdVcnVc1Decoder session(a, engine_of(rig), config_for(ref.stream));
    CHECK(session.prepare());
    Output out;
    for (size_t k = 0; k < ref.units(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
    session.flush();
    drain(session, &out);
    CHECK(out.pts == ref.out.pts && out.bytes == ref.out.bytes);
    CHECK(rig.creates == 0 && rig.decodes == 0 && rig.vcn.writes.empty() && !session.session_open() && session.statistics().errors == 0);
  }
  CHECK(a.live == 0);
}

static void test_one_session_at_a_time() {
  TestAllocator a;
  Stream main_profile;
  CHECK(load("sm_ipp", &main_profile));
  {
    Rig rig;
    AmdVcnVc1Decoder first(a, engine_of(rig), config_for(main_profile));
    CHECK(first.prepare());
    AmdVcnVc1Decoder second(a, engine_of(rig, 8), config_for(main_profile));
    CHECK(!second.prepare());
  }
  {
    // A configuration record the decoder does not take: the Complex profile, a record that is too short.
    Rig rig;
    DecoderConfig junk = config_for(main_profile);
    uint8_t bytes[4];
    memcpy(bytes, main_profile.struct_c, 4);
    bytes[0] = uint8_t((bytes[0] & 0x3F) | 0xC0);
    junk.codec_config = ConstByteView(bytes, 4);
    AmdVcnVc1Decoder bad(a, engine_of(rig), junk);
    CHECK(!bad.prepare());
    DecoderConfig shorter = config_for(main_profile);
    shorter.codec_config = ConstByteView(main_profile.struct_c, 3);
    AmdVcnVc1Decoder bad2(a, engine_of(rig), shorter);
    CHECK(!bad2.prepare());
    // ... and the lease was not kept by the sessions that failed.
    AmdVcnVc1Decoder good(a, engine_of(rig), config_for(main_profile));
    CHECK(good.prepare());
  }
  CHECK(a.live == 0);
}

static void test_wedged_engine() {
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, "sm_ipp", &ref));
    Rig rig;
    rig.references.push_back(&ref);
    AmdVcnVc1Decoder session(a, engine_of(rig), config_for(ref.stream));
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
  for (const char* name : {"sm_ipb_f", "adv_ipb", "adv_skip"}) {
    Reference ref;
    CHECK(make_reference(a, name, &ref));
    int runs = 0;
    for (size_t variant = 0; variant < 24; ++variant) {
      Rig rig;
      rig.references.push_back(&ref);
      rig.check_content = false;
      {
        AmdVcnVc1Decoder session(a, engine_of(rig), config_for(ref.stream));
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
      CHECK(rig.vcn.protocol_errors == 0);
    }
    CHECK(runs > 100);
  }
  CHECK(a.live == 0);
}

// The engine's own VC-1 set-up and self-test, against the same simulated card: the stand-in firmware is given the CPU's pictures for the two self-test streams.
static void test_engine_self_test(bool engine_works, bool engine_hangs) {
  TestAllocator a;
  {
    Reference main_ref, adv_ref;
    load_self_test(false, &main_ref.stream);
    load_self_test(true, &adv_ref.stream);
    CHECK(make_reference_from(a, &main_ref) && make_reference_from(a, &adv_ref));
    CHECK(main_ref.units() == kVc1SelfTestPictures && adv_ref.units() == kVc1SelfTestPictures);

    Rig rig;
    rig.references = {&main_ref, &adv_ref};
    rig.write_pictures = engine_works;
    rig.vcn.stall_fence = engine_hangs;
    AmdVcnVideoEngine engine(a, 0x15DD);
    const MmioRegion ring_window(reinterpret_cast<uint64_t>(rig.ring_page.data()), rig.ring_page.size());
    CHECK(engine.set_up_decode(rig.vcn, rig.clock, ring_window, rig.mapper, Rig::kGpuBase, Rig::kPhysicalBase, rig.arena.size()) == nullptr);
    const bool enabled = engine.enable_vc1_decode();
    CHECK(enabled == (engine_works && !engine_hangs) && engine.vc1_ready() == enabled);
    CHECK(engine.info().accelerated_decode_codecs == (enabled ? codec_mask(VideoCodec::Vc1) : 0u));
    CHECK(rig.problems == 0 || !engine_works);
    if (enabled) {
      CHECK(rig.creates == 2 && rig.destroys == 2 && rig.decodes == int(2 * kVc1SelfTestPictures));   // the test opened and closed a session for each stream
      // VC-1 sessions are the engine's now; other codecs stay with the CPU.
      Reference stream;
      CHECK(make_reference(a, "sm_ipp", &stream));
      rig.references = {&main_ref, &adv_ref, &stream};
      UniquePointer<IVideoDecoder> session = engine.create_decoder(config_for(stream.stream));
      CHECK(static_cast<bool>(session));
      DecoderConfig vp9;
      vp9.codec = VideoCodec::Vp9;
      UniquePointer<IVideoDecoder> software = engine.create_decoder(vp9);
      CHECK(static_cast<bool>(software));
      Output out;
      CHECK(feed(*session, stream.unit(0), 0, &out));
      CHECK(rig.creates == 3 && rig.decodes == int(2 * kVc1SelfTestPictures) + 1);   // it ran on the engine
      // The engine serves one session at a time: while the first is open, a second one is the CPU's.
      UniquePointer<IVideoDecoder> second = engine.create_decoder(config_for(stream.stream));
      CHECK(static_cast<bool>(second));
      CHECK(feed(*second, stream.unit(0), 0, &out));
      CHECK(rig.creates == 3 && rig.decodes == int(2 * kVc1SelfTestPictures) + 1 && rig.problems == 0);
      session.reset();
      second.reset();
      software.reset();
      engine.shutdown();
      CHECK(!engine.vc1_ready() && engine.info().accelerated_decode_codecs == 0);
    } else {
      // Not trusted: nothing accelerated, and VC-1 is still served (by the CPU).
      Stream s;
      CHECK(load("sm_ipp", &s));
      UniquePointer<IVideoDecoder> session = engine.create_decoder(config_for(s));
      CHECK(static_cast<bool>(session));
      CHECK(strstr(engine.engine_note(), engine_hangs ? "engine stopped" : "VC-1 self-test failed") != nullptr);
    }
  }
  CHECK(a.live == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  static const char* const streams[] = {"simple_intra", "sm_intra", "sm_intra_lf", "sm_intra_q", "sm_intra_nu", "adv_intra", "adv_intra_lf", "adv_intra_odd", "sp_ipp", "sm_ipp", "sm_ipp_q", "sm_ipp_nu",
                                        "sm_ipb", "sm_ipb_f", "adv_ipp", "adv_ipb", "adv_ipp_odd", "adv_skip"};
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
  printf("AmdVcnVc1DecoderTest passed\n");
  return 0;
}
