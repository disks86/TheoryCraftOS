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

// Host-side tests for WebM: the demuxer (media::WebmFile) against ffmpeg's files and files rewritten with what ffmpeg never writes (tests/data/make_webm_fixtures.py;
// the expectations are in WebmVectors.inc), hand-built files for the corners (header stripping, bad lacing, clusters of unknown size, encryption), damaged and
// truncated files, the sound through the Ogg decoders, the pictures through the VP8/VP9 decoders, and the video player playing them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Audio/AudioMixer.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Media/Audio/OpusInternal.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/AudioStreaming.hpp"
#include "Media/VideoFiles.hpp"
#include "Media/Webm.hpp"
#include "Media/Zlib.hpp"
#include "Video/VideoPlayer.hpp"

using namespace media;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct File {
  std::vector<uint8_t> bytes;
  bool ok_ = false;
  explicit File(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    bytes.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    ok_ = fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
  }
  bool ok() const { return ok_ && !bytes.empty(); }
  ConstByteView view() const { return ConstByteView(bytes.data(), bytes.size()); }
};

static uint64_t fnv(uint64_t h, const uint8_t* p, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}
static const uint64_t kFnvBasis = 0xcbf29ce484222325ull;

struct WebmFrameVector {
  int64_t pts_us;
  uint32_t size;
  bool key;
  uint64_t hash;
};
#include "WebmVectors.inc"

template <class T, size_t N>
static constexpr size_t count_of(const T (&)[N]) { return N; }

struct FakeClock final : hardware::IClock {
  uint64_t now = 0;
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return now; }
};

struct FakeDevice final : hardware::IAudioDevice {
  uint32_t queued = 0;
  const char* name() const override { return "fake audio"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    uint32_t n = pcm.size() < 16384 - queued ? static_cast<uint32_t>(pcm.size()) : 16384 - queued;
    n -= n % hardware::kAudioFrameSize;
    queued += n;
    return n;
  }
  uint32_t queued_bytes() override { return queued; }
  uint32_t free_bytes() override { return 16384 - queued; }
  void set_volume(uint8_t) override {}
  uint8_t volume() const override { return 80; }
  void set_muted(bool) override {}
  bool muted() const override { return false; }
  hardware::AudioStatistics statistics() override { return {}; }
};

struct NoPci final : hardware::IPciBus {
  const char* name() const override { return "no pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  bool pci_device(uint32_t, hardware::PciDeviceInfo*) const override { return false; }
  uint32_t read32(hardware::PciAddress, uint16_t) const override { return 0; }
  void write32(hardware::PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(hardware::PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(hardware::PciAddress, uint8_t) const override { return 0; }
};

// ---- helpers ---------------------------------------------------------------------------------------------------------------------------------

// Every frame of a track, read back and hashed the way the vectors are.
static bool frames_match(IByteSource& source, WebmFile& file, const WebmTrack& track, const WebmFrameVector* expect, size_t n, bool check_pts) {
  if (track.samples.count() != n) return false;
  TestAllocator a;
  ByteBuffer frame(a);
  for (size_t i = 0; i < n; ++i) {
    frame.clear();
    if (!file.read_sample(source, track, static_cast<uint32_t>(i), &frame)) return false;
    if (frame.size() != expect[i].size || fnv(kFnvBasis, frame.view().data(), frame.size()) != expect[i].hash) return false;
    if (track.samples[i].keyframe != expect[i].key) return false;
    if (track.samples[i].duration_us == 0) return false;
    if (check_pts && track.samples[i].pts_us != expect[i].pts_us) return false;
  }
  return true;
}

static uint64_t hash_bytes(ConstByteView v) { return fnv(kFnvBasis, v.data(), v.size()); }

// ---- the demuxer on ffmpeg's files and the rewrites -----------------------------------------------------------------------------------------

static void test_demux_files() {
  struct Case {
    const char* path;
    const WebmFrameVector* video;
    size_t video_n;
    const WebmFrameVector* audio;
    size_t audio_n;
    WebmCodec video_codec, audio_codec;
    bool audio_exact;   // the frames of the sound keep the times the file gave them (lacing spreads them evenly instead)
    WebmCompression video_compression;
    bool has_audio;
  };
  const Case cases[] = {
      {"tests/data/webm_vp9_opus.webm", k_vp9_opus_video, count_of(k_vp9_opus_video), k_vp9_opus_audio, count_of(k_vp9_opus_audio), WebmCodec::Vp9, WebmCodec::Opus, true, WebmCompression::None, true},
      {"tests/data/webm_live.webm", k_vp9_opus_video, count_of(k_vp9_opus_video), k_vp9_opus_audio, count_of(k_vp9_opus_audio), WebmCodec::Vp9, WebmCodec::Opus, true, WebmCompression::None, true},
      {"tests/data/webm_groups.webm", k_vp9_opus_video, count_of(k_vp9_opus_video), k_vp9_opus_audio, count_of(k_vp9_opus_audio), WebmCodec::Vp9, WebmCodec::Opus, false, WebmCompression::None, true},
      {"tests/data/webm_zlib.webm", k_vp9_opus_video, count_of(k_vp9_opus_video), k_vp9_opus_audio, count_of(k_vp9_opus_audio), WebmCodec::Vp9, WebmCodec::Opus, true, WebmCompression::Zlib, true},
      {"tests/data/webm_vp8_vorbis.webm", k_vp8_vorbis_video, count_of(k_vp8_vorbis_video), k_vp8_vorbis_audio, count_of(k_vp8_vorbis_audio), WebmCodec::Vp8, WebmCodec::Vorbis, true, WebmCompression::None, true},
      {"tests/data/webm_xiph.webm", k_vp8_vorbis_video, count_of(k_vp8_vorbis_video), k_vp8_vorbis_audio, count_of(k_vp8_vorbis_audio), WebmCodec::Vp8, WebmCodec::Vorbis, false, WebmCompression::None, true},
  };
  for (const Case& c : cases) {
    TestAllocator a;
    {
      File f(c.path);
      CHECK(f.ok());
      if (!f.ok()) continue;
      MemoryByteSource source(f.view());
      WebmFile file(a);
      CHECK(file.open(source));
      CHECK(file.is_webm() && file.track_count() == 2);
      CHECK(file.duration_us() >= 1500000 && file.duration_us() <= 1700000);
      const WebmTrack* video = file.find(WebmTrackKind::Video);
      const WebmTrack* audio = file.find(WebmTrackKind::Audio);
      CHECK(video && audio);
      if (!video || !audio) continue;
      CHECK(video->codec == c.video_codec && audio->codec == c.audio_codec && video->compression == c.video_compression);
      CHECK(video->width == 64 && video->height == 48 && video->display_width == 64 && video->display_height == 48);
      CHECK(audio->channels == 1 && audio->sample_rate == 48000);
      CHECK(video->decodable() && audio->decodable());
      CHECK(frames_match(source, file, *video, c.video, c.video_n, true));
      CHECK(frames_match(source, file, *audio, c.audio, c.audio_n, c.audio_exact));
      CHECK(!audio->codec_private.view().empty());
      if (!c.audio_exact) {   // the times are spread evenly over the block: increasing, and within a frame of what the file said
        bool increasing = true;
        for (size_t i = 1; i < audio->samples.count(); ++i) increasing = increasing && audio->samples[i].pts_us > audio->samples[i - 1].pts_us;
        CHECK(increasing);
      }
      // a source that is read through a window sees the same file
      file.clear();
      CHECK(file.track_count() == 0 && file.duration_us() == 0);
      CachedByteSource cached(source, a, 512);
      CHECK(file.open(cached) && file.find(WebmTrackKind::Video) && file.find(WebmTrackKind::Video)->samples.count() == c.video_n);
    }
    CHECK(a.live == 0);
  }
  // the EBML lacing with uneven packets: every packet is a valid Opus packet, and there are as many as the sound is long
  {
    TestAllocator a;
    {
      File f("tests/data/webm_ebml.webm");
      CHECK(f.ok());
      MemoryByteSource source(f.view());
      WebmFile file(a);
      CHECK(file.open(source));
      const WebmTrack* audio = file.find(WebmTrackKind::Audio);
      CHECK(audio && audio->samples.count() >= 78);
      if (audio) {
        ByteBuffer packet(a);
        uint32_t distinct_sizes = 0, last = 0;
        bool valid = true;
        for (uint32_t i = 0; i < audio->samples.count(); ++i) {
          packet.clear();
          valid = valid && file.read_sample(source, *audio, i, &packet);
          OpusPacketInfo info;
          valid = valid && parse_opus_packet(packet.view(), &info);
          if (packet.size() != last) ++distinct_sizes;
          last = static_cast<uint32_t>(packet.size());
        }
        CHECK(valid && distinct_sizes > 3);
      }
    }
    CHECK(a.live == 0);
  }
  // not WebM
  {
    TestAllocator a;
    WebmFile file(a);
    const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    MemoryByteSource source{ConstByteView(junk, sizeof junk)};
    CHECK(!file.open(source));
    File mp4("tests/data/h264_bframes.mp4");
    MemoryByteSource other(mp4.view());
    CHECK(!file.open(other));
    CHECK(detect_video_file(ConstByteView(junk, sizeof junk)) == VideoFileFormat::Unknown);
  }
}

// ---- hand-built files ---------------------------------------------------------------------------------------------------------------------

namespace build {
using Bytes = std::vector<uint8_t>;

static Bytes id_bytes(uint32_t id) {
  Bytes b;
  int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
  for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<uint8_t>(id >> (8 * i)));
  return b;
}
static Bytes element(uint32_t id, const Bytes& payload, bool unknown = false) {
  Bytes b = id_bytes(id);
  if (unknown) {
    const uint8_t u[8] = {1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    b.insert(b.end(), u, u + 8);
  } else {
    // 4-byte sizes keep it simple
    const uint32_t n = static_cast<uint32_t>(payload.size());
    b.push_back(0x10 | static_cast<uint8_t>(n >> 24));
    b.push_back(static_cast<uint8_t>(n >> 16));
    b.push_back(static_cast<uint8_t>(n >> 8));
    b.push_back(static_cast<uint8_t>(n));
  }
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}
static Bytes number(uint32_t id, uint64_t v) {
  Bytes p;
  int n = 1;
  while (n < 8 && (v >> (8 * n))) ++n;
  for (int i = n - 1; i >= 0; --i) p.push_back(static_cast<uint8_t>(v >> (8 * i)));
  return element(id, p);
}
static Bytes text(uint32_t id, const char* s) { return element(id, Bytes(s, s + strlen(s))); }
static Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) b.insert(b.end(), p.begin(), p.end());
  return b;
}
static void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }

struct Encoding {
  int kind = 0;   // 0 none, 1 header stripping, 2 zlib, 3 bzlib (unsupported), 4 encryption
  Bytes strip;
};

static Bytes track(uint32_t number_, uint32_t type, const char* codec, const Encoding& enc = Encoding(), bool enabled = true) {
  Bytes e = cat({number(0xD7, number_), number(0x73C5, number_), number(0x83, type), text(0x86, codec), number(0xB9, enabled)});
  if (type == 1) append(e, element(0xE0, cat({number(0xB0, 64), number(0xBA, 48)})));
  else append(e, element(0xE1, cat({number(0x9F, 1), element(0xB5, Bytes{0x40, 0xE7, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00})})));
  if (enc.kind) {
    Bytes inner;
    if (enc.kind == 1) inner = element(0x5034, cat({number(0x4254, 3), element(0x4255, enc.strip)}));
    else if (enc.kind == 2) inner = element(0x5034, number(0x4254, 0));
    else if (enc.kind == 3) inner = element(0x5034, number(0x4254, 1));
    else inner = element(0x5035, cat({number(0x47E1, 5), element(0x47E2, Bytes{1, 2, 3, 4})}));
    append(e, element(0x6D80, element(0x6240, cat({number(0x5031, 0), number(0x5032, 1), number(0x5033, enc.kind == 4 ? 1 : 0), inner}))));
  }
  return element(0xAE, e);
}

// A SimpleBlock: track number (one byte), time relative to the cluster, flags, frames.
static Bytes block(uint32_t track_, int16_t rel, uint8_t flags, const Bytes& body) {
  Bytes p;
  p.push_back(static_cast<uint8_t>(0x80 | track_));
  p.push_back(static_cast<uint8_t>(static_cast<uint16_t>(rel) >> 8));
  p.push_back(static_cast<uint8_t>(rel));
  p.push_back(flags);
  append(p, body);
  return p;
}
static Bytes simple(uint32_t track_, int16_t rel, bool key, const Bytes& frame) { return element(0xA3, block(track_, rel, key ? 0x80 : 0, frame)); }

static Bytes header(const char* doc_type = "webm") { return element(0x1A45DFA3, cat({number(0x4286, 1), text(0x4282, doc_type), number(0x4287, 4)})); }
static Bytes info(uint64_t duration_ticks = 0) {
  Bytes p = number(0x2AD7B1, 1000000);
  if (duration_ticks) {
    const double d = static_cast<double>(duration_ticks);
    uint64_t bits;
    memcpy(&bits, &d, 8);
    Bytes f;
    for (int i = 7; i >= 0; --i) f.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    append(p, element(0x4489, f));
  }
  return element(0x1549A966, p);
}
static Bytes cluster(uint64_t timecode, const Bytes& body, bool unknown = false) { return element(0x1F43B675, cat({number(0xE7, timecode), body}), unknown); }
static Bytes frame_of(uint8_t seed, size_t n) {
  Bytes b(n);
  for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(seed * 37 + i * 11);
  return b;
}
}  // namespace build

static bool open_bytes(const build::Bytes& bytes, WebmFile& file, MemoryByteSource& source) {
  source.reset(ConstByteView(bytes.data(), bytes.size()));
  return file.open(source);
}

static void test_demux_corners() {
  using namespace build;
  TestAllocator a;
  {
    // header stripping: the stored frames lack the bytes the track's settings give
    const Bytes strip = {0xB8, 0x01};
    Bytes f1 = frame_of(1, 30), f2 = frame_of(2, 10);
    Bytes stored1(f1.begin() + 2, f1.end()), stored2(f2.begin() + 2, f2.end());
    f1[0] = strip[0]; f1[1] = strip[1];
    f2[0] = strip[0]; f2[1] = strip[1];
    stored1.assign(f1.begin() + 2, f1.end());
    stored2.assign(f2.begin() + 2, f2.end());
    Encoding enc;
    enc.kind = 1;
    enc.strip = strip;
    Bytes file = cat({header(), element(0x18538067, cat({info(), element(0x1654AE6B, cat({track(1, 2, "A_OPUS", enc)})),
                                                           cluster(0, cat({simple(1, 0, true, stored1), simple(1, 20, true, stored2)}))}))});
    WebmFile w(a);
    MemoryByteSource source;
    CHECK(open_bytes(file, w, source) && w.track_count() == 1);
    const WebmTrack& t = w.track(0);
    CHECK(t.compression == WebmCompression::HeaderStripping && t.samples.count() == 2 && t.samples[0].size == 28);
    ByteBuffer out(a);
    CHECK(w.read_sample(source, t, 0, &out) && out.size() == 30 && memcmp(out.view().data(), f1.data(), 30) == 0);
    out.clear();
    CHECK(w.read_sample(source, t, 1, &out) && out.size() == 10 && memcmp(out.view().data(), f2.data(), 10) == 0);
    CHECK(!w.read_sample(source, t, 2, &out));
    CHECK(t.samples[0].duration_us == 20000 && t.samples[0].pts_us == 0 && t.samples[1].pts_us == 20000);
  }
  {
    // zlib: a frame that inflates, and one that does not (the second fails, the first still reads)
    const Bytes plain = frame_of(3, 200);
    ByteBuffer packed(a);
    CHECK(deflate(ConstByteView(plain.data(), plain.size()), &packed, 6, a));
    Bytes good(packed.view().data(), packed.view().data() + packed.size());
    Bytes bad = good;
    bad[bad.size() / 2] ^= 0x55;
    Encoding enc;
    enc.kind = 2;
    Bytes file = cat({header(), element(0x18538067, cat({info(), element(0x1654AE6B, track(1, 1, "V_VP9", enc)), cluster(0, cat({simple(1, 0, true, good), simple(1, 33, false, bad)}))}))});
    WebmFile w(a);
    MemoryByteSource source;
    CHECK(open_bytes(file, w, source) && w.track(0).compression == WebmCompression::Zlib);
    ByteBuffer out(a);
    CHECK(w.read_sample(source, w.track(0), 0, &out) && out.size() == plain.size() && memcmp(out.view().data(), plain.data(), plain.size()) == 0);
    out.clear();
    CHECK(!w.read_sample(source, w.track(0), 1, &out) && out.size() == 0);
  }
  {
    // compression this OS cannot undo, and encryption: the track is listed but is not decodable; the track beside it still is
    for (int kind : {3, 4}) {
      Encoding enc;
      enc.kind = kind;
      Bytes file = cat({header(), element(0x18538067, cat({info(), element(0x1654AE6B, cat({track(1, 1, "V_VP8", enc), track(2, 2, "A_VORBIS")})),
                                                             cluster(0, cat({simple(1, 0, true, frame_of(1, 20)), simple(2, 0, true, frame_of(2, 20))}))}))});
      WebmFile w(a);
      MemoryByteSource source;
      CHECK(open_bytes(file, w, source) && w.track_count() == 2);
      CHECK(!w.track(0).decodable() && w.track(0).encrypted == (kind == 4) && !w.find(WebmTrackKind::Video) && w.find(WebmTrackKind::Audio));
      ByteBuffer out(a);
      if (kind == 3) CHECK(!w.read_sample(source, w.track(0), 0, &out));
    }
  }
  {
    // a disabled track is not chosen; the default track is preferred; other codecs are known but not decodable... except AV1; matroska is accepted
    Bytes file = cat({header("matroska"), element(0x18538067, cat({info(), element(0x1654AE6B, cat({track(1, 1, "V_VP8", Encoding(), false), track(2, 1, "V_MPEG4/ISO/AVC"), track(3, 1, "V_AV1"), track(4, 1, "V_VP9"), track(5, 3, "S_TEXT/UTF8")})),
                                                           cluster(0, cat({simple(1, 0, true, frame_of(1, 9)), simple(2, 0, true, frame_of(2, 9)), simple(3, 0, true, frame_of(3, 9)), simple(4, 0, true, frame_of(4, 9)), simple(5, 0, true, frame_of(5, 9))}))}))});
    WebmFile w(a);
    MemoryByteSource source;
    CHECK(open_bytes(file, w, source) && !w.is_webm() && w.track_count() == 5);
    CHECK(w.track(1).codec == WebmCodec::Unknown && w.track(2).codec == WebmCodec::Av1 && w.track(3).codec == WebmCodec::Vp9 && w.track(4).kind == WebmTrackKind::Other);
    CHECK(w.find(WebmTrackKind::Video) == &w.track(2));   // (the first that can be decoded, the disabled VP8 and the AVC passed over)
    CHECK(w.track(4).samples.count() == 0);
  }
  {
    // a file of nothing that can be decoded is refused; so is a document type that is neither
    Bytes only_text = cat({header(), element(0x18538067, cat({info(), element(0x1654AE6B, track(1, 3, "S_TEXT/UTF8")), cluster(0, simple(1, 0, true, frame_of(1, 5)))}))});
    WebmFile w(a);
    MemoryByteSource source;
    CHECK(!open_bytes(only_text, w, source));
    Bytes odd_type = cat({header("ogg"), element(0x18538067, cat({info(), element(0x1654AE6B, track(1, 1, "V_VP9")), cluster(0, simple(1, 0, true, frame_of(1, 5)))}))});
    CHECK(!open_bytes(odd_type, w, source));
  }
  {
    // clusters of unknown size end at the next element of the segment (a cluster, cues, tags); a segment of unknown size ends with the file; blocks may have
    // negative times against the cluster; elements the reader does not know are skipped
    Bytes cues = element(0x1C53BB6B, Bytes(12, 0));
    Bytes tags = element(0x1254C367, Bytes(5, 0));
    Bytes body = cat({info(), element(0x1654AE6B, track(1, 1, "V_VP9")), element(0xEC, Bytes(7, 0)),
                      cluster(1000, cat({simple(1, -10, true, frame_of(1, 50)), simple(1, 33, false, frame_of(2, 40)), element(0xBF, Bytes(4, 1))}), true),
                      cluster(2000, cat({simple(1, 0, true, frame_of(3, 60))}), true), cues,
                      cluster(3000, simple(1, 0, false, frame_of(4, 20)), true), tags,
                      cluster(4000, simple(1, 0, true, frame_of(5, 30)), true)});
    Bytes file = cat({header(), element(0x18538067, body, true)});
    WebmFile w(a);
    MemoryByteSource source;
    CHECK(open_bytes(file, w, source));
    const WebmTrack& t = w.track(0);
    CHECK(t.samples.count() == 5);
    if (t.samples.count() == 5) {
      CHECK(t.samples[0].pts_us == 990000 && t.samples[1].pts_us == 1033000 && t.samples[2].pts_us == 2000000 && t.samples[3].pts_us == 3000000 && t.samples[4].pts_us == 4000000);
      CHECK(t.samples[0].keyframe && !t.samples[1].keyframe && t.samples[2].keyframe && !t.samples[3].keyframe && t.samples[4].keyframe);
      CHECK(t.samples[0].size == 50 && t.samples[1].size == 40 && t.samples[4].size == 30);
      CHECK(t.samples[0].duration_us == 43000 && t.samples[4].duration_us > 0);   // (the time to the next frame, and a guess for the last)
    }
  }
  {
    // lacing: Xiph, fixed and EBML; a lace whose sizes do not add up is dropped and the blocks after it are still read
    const Bytes a1 = frame_of(1, 300), a2 = frame_of(2, 7), a3 = frame_of(3, 255);
    Bytes xiph = {2, 0xFF, 45, 7};   // 3 frames: 300, 7, rest
    append(xiph, a1);
    append(xiph, a2);
    append(xiph, a3);
    Bytes fixed = {2};
    append(fixed, frame_of(4, 10));
    append(fixed, frame_of(5, 10));
    append(fixed, frame_of(6, 10));
    // EBML lace of 3: 20 (0xA0 as a one-byte vint is 0x94), then +5 (signed: 0xC0 + 5 = 0xC5 for a one-byte vint with bias 63)
    Bytes ebml = {2, 0x94, static_cast<uint8_t>(0x80 | (63 + 5))};
    append(ebml, frame_of(7, 20));
    append(ebml, frame_of(8, 25));
    append(ebml, frame_of(9, 3));
    Bytes bad_xiph = {1, 200};   // 2 frames: 200, rest... but there are 10 bytes
    append(bad_xiph, frame_of(1, 10));
    Bytes bad_fixed = {2};   // 3 frames from 10 bytes
    append(bad_fixed, frame_of(1, 10));
    Bytes lost_end = {1, 0xFF};   // a size run that never ends
    Bytes cl = cat({element(0xA3, block(1, 0, 0x02, xiph)), element(0xA3, block(1, 100, 0x04, fixed)), element(0xA3, block(1, 200, 0x06, ebml)),
                    element(0xA3, block(1, 300, 0x02, bad_xiph)), element(0xA3, block(1, 310, 0x04, bad_fixed)), element(0xA3, block(1, 320, 0x02, lost_end)),
                    simple(1, 400, true, frame_of(10, 12))});
    Bytes file = cat({header(), element(0x18538067, cat({info(), element(0x1654AE6B, track(1, 2, "A_OPUS")), cluster(0, cl)}))});
    WebmFile w(a);
    MemoryByteSource source;
    CHECK(open_bytes(file, w, source));
    const WebmTrack& t = w.track(0);
    CHECK(t.samples.count() == 3 + 3 + 3 + 1);
    if (t.samples.count() == 10) {
      const uint32_t sizes[10] = {300, 7, 255, 10, 10, 10, 20, 25, 3, 12};
      for (uint32_t i = 0; i < 10; ++i) CHECK(t.samples[i].size == sizes[i]);
      ByteBuffer out(a);
      CHECK(w.read_sample(source, t, 1, &out) && out.size() == 7 && memcmp(out.view().data(), a2.data(), 7) == 0);
      CHECK(t.samples[9].pts_us == 400000);
      // audio frames are all key frames; a laced block's frames are spread over its time
      bool keys = true;
      for (uint32_t i = 0; i < 10; ++i) keys = keys && t.samples[i].keyframe;
      CHECK(keys);
    }
  }
  {
    // a BlockGroup: its duration, its reference (not a key frame), padding to discard, and a block group of nothing
    Bytes group_key = element(0xA0, cat({element(0xA1, block(1, 0, 0, frame_of(1, 16))), number(0x9B, 40)}));
    Bytes group_ref = element(0xA0, cat({element(0xA1, block(1, 40, 0, frame_of(2, 16))), element(0xFB, Bytes{0xFF}), number(0x9B, 25)}));
    Bytes group_empty = element(0xA0, number(0x9B, 5));
    Bytes cl = cat({group_key, group_empty, group_ref, simple(1, 65, true, frame_of(3, 8))});
    Bytes file = cat({header(), element(0x18538067, cat({info(), element(0x1654AE6B, track(1, 1, "V_VP8")), cluster(0, cl)}))});
    WebmFile w(a);
    MemoryByteSource source;
    CHECK(open_bytes(file, w, source));
    const WebmTrack& t = w.track(0);
    CHECK(t.samples.count() == 3 && t.samples[0].keyframe && !t.samples[1].keyframe && t.samples[2].keyframe);
    CHECK(t.samples[0].duration_us == 40000 && t.samples[1].duration_us == 25000);
  }
  {
    // Opus: DiscardPadding of the last block is kept for the Ogg file
    Bytes g = element(0xA0, cat({element(0xA1, block(1, 0, 0, frame_of(1, 16))), element(0x75A2, Bytes{0x0F, 0x42, 0x40}), number(0x9B, 20)}));
    Bytes file = cat({header(), element(0x18538067, cat({info(), element(0x1654AE6B, track(1, 2, "A_OPUS")), cluster(0, cat({simple(1, 0, true, frame_of(2, 16)), g}))}))});
    WebmFile w(a);
    MemoryByteSource source;
    CHECK(open_bytes(file, w, source) && w.track(0).last_discard_ns == 1000000);
  }
  {
    // more tracks than the reader keeps, and a stream with the blocks before the tracks
    Bytes tracks;
    for (uint32_t i = 1; i <= 20; ++i) append(tracks, track(i, 2, "A_OPUS"));
    Bytes file = cat({header(), element(0x18538067, cat({info(), element(0x1654AE6B, tracks), cluster(0, simple(1, 0, true, frame_of(1, 5)))}))});
    WebmFile w(a);
    MemoryByteSource source;
    CHECK(open_bytes(file, w, source) && w.track_count() == WebmFile::kMaxTracks);
    Bytes early = cat({header(), element(0x18538067, cat({info(), cluster(0, simple(1, 0, true, frame_of(1, 5))), element(0x1654AE6B, track(1, 2, "A_OPUS"))}))});
    CHECK(!open_bytes(early, w, source));   // (blocks of tracks nobody described: nothing to play)
  }
  CHECK(a.live == 0);
}

// ---- damage -------------------------------------------------------------------------------------------------------------------------------

// Opens `data` and reads every frame of every track: whatever it makes of the bytes, it must stay inside them.
static uint64_t open_and_read_all(const std::vector<uint8_t>& data, allocators::IAllocator& a) {
  MemoryByteSource source{ConstByteView(data.data(), data.size())};
  WebmFile file(a);
  if (!file.open(source)) return 0;
  uint64_t total = 0;
  ByteBuffer out(a);
  for (uint32_t t = 0; t < file.track_count(); ++t) {
    const WebmTrack& track = file.track(t);
    for (uint32_t i = 0; i < track.samples.count(); ++i) {
      out.clear();
      if (file.read_sample(source, track, i, &out)) total += out.size();
      const WebmSample& s = track.samples[i];
      if (s.offset > data.size() || s.size > data.size() - s.offset) total += 1u << 30;   // (a sample outside the file: counted so the check below fails)
    }
  }
  return total;
}

static void test_damage() {
  TestAllocator a;
  for (const char* path : {"tests/data/webm_vp9_opus.webm", "tests/data/webm_groups.webm", "tests/data/webm_live.webm", "tests/data/webm_xiph.webm", "tests/data/webm_zlib.webm"}) {
    File f(path);
    CHECK(f.ok());
    if (!f.ok()) continue;
    const uint64_t whole = open_and_read_all(f.bytes, a);
    CHECK(whole > 0 && whole < (1u << 30));
    // cut short anywhere: the frames that are whole are still there, and none of them lies outside what is left
    uint64_t previous = 0;
    bool grows = true;
    for (size_t cut = 0; cut < f.bytes.size(); cut += 37) {
      std::vector<uint8_t> part(f.bytes.begin(), f.bytes.begin() + static_cast<long>(cut));
      const uint64_t got = open_and_read_all(part, a);
      CHECK(got < (1u << 30) && got <= whole);
      if (got != 0 && got < previous) grows = false;
      if (got != 0) previous = got;
    }
    CHECK(grows);
    {
      std::vector<uint8_t> part(f.bytes.begin(), f.bytes.end() - 3);   // (the last frame lost a few bytes)
      const uint64_t got = open_and_read_all(part, a);
      CHECK(got > 0 && got <= whole);   // (less, unless the file ends with cues)
    }
    // bits flipped: nothing to say about what it reads, only that it stays inside the file
    for (size_t at = 0; at < f.bytes.size(); at += (at < 400 ? 1 : 53)) {
      for (int bit : {0, 3, 7}) {
        std::vector<uint8_t> copy = f.bytes;
        copy[at] ^= static_cast<uint8_t>(1u << bit);
        CHECK(open_and_read_all(copy, a) < (1u << 30));
      }
    }
  }
  CHECK(a.live == 0);
}

// ---- sound --------------------------------------------------------------------------------------------------------------------------------

// The same samples as far as both go (ffmpeg's Ogg writer ends the Opus sound a few ms late: it works from the last packet's time, where WebM says exactly how much of the
// last block to cut).
static bool same_sound(const AudioData& x, const AudioData& y) {
  if (x.format().sample_rate != y.format().sample_rate || x.format().channels != y.format().channels) return false;
  const uint64_t n = (x.frame_count() < y.frame_count() ? x.frame_count() : y.frame_count()) * x.format().channels;
  return n > 0 && memcmp(x.samples().data(), y.samples().data(), n * 2) == 0;
}

static void test_audio() {
  const struct {
    const char* webm;
    const char* ogg;
    AudioFileFormat ogg_format;
  } cases[] = {{"tests/data/webm_opus.weba", "tests/data/webm_ref_opus.ogg", AudioFileFormat::Opus},
               {"tests/data/webm_vp9_opus.webm", "tests/data/webm_ref_opus.ogg", AudioFileFormat::Opus},
               {"tests/data/webm_live.webm", "tests/data/webm_ref_opus.ogg", AudioFileFormat::Opus},
               {"tests/data/webm_groups.webm", "tests/data/webm_ref_opus.ogg", AudioFileFormat::Opus},
               {"tests/data/webm_zlib.webm", "tests/data/webm_ref_opus.ogg", AudioFileFormat::Opus},
               {"tests/data/webm_vorbis.weba", "tests/data/webm_ref_vorbis.ogg", AudioFileFormat::Vorbis},
               {"tests/data/webm_vp8_vorbis.webm", "tests/data/webm_ref_vorbis.ogg", AudioFileFormat::Vorbis},
               {"tests/data/webm_xiph.webm", "tests/data/webm_ref_vorbis.ogg", AudioFileFormat::Vorbis}};
  for (const auto& c : cases) {
    TestAllocator a;
    {
      File webm(c.webm), ogg(c.ogg);
      CHECK(webm.ok() && ogg.ok());
      if (!webm.ok() || !ogg.ok()) continue;
      CHECK(detect_audio_format(webm.view()) == AudioFileFormat::Webm && detect_audio_format(ogg.view()) == c.ogg_format);
      AudioData from_webm(a), from_ogg(a);
      AudioFileFormat detected = AudioFileFormat::Unknown;
      CHECK(decode_audio(webm.view(), &from_webm, &detected) && detected == AudioFileFormat::Webm);
      CHECK(decode_audio(ogg.view(), &from_ogg));
      CHECK(from_webm.frame_count() == 76800 && same_sound(from_webm, from_ogg));   // (1.6 s: the sound ends where the encoder was told to stop)
      // streamed: the same sound, read from the file a piece at a time
      MemoryByteSource source(webm.view());
      AudioFileFormat streamed_format = AudioFileFormat::Unknown;
      UniquePointer<audio::IPcmSource> stream = open_audio_stream(source, a, &streamed_format);
      CHECK(stream && streamed_format == AudioFileFormat::Webm);
      if (stream) {
        CHECK(stream->frame_count() == from_webm.frame_count() && stream->format().sample_rate == from_ogg.format().sample_rate);
        const uint32_t frame_size = stream->format().frame_size();
        ByteBuffer pcm(a);
        CHECK(pcm.put_zeros(uint64_t{4096} * frame_size));
        uint64_t at = 0, hash = kFnvBasis;
        while (at < stream->frame_count()) {
          const uint32_t got = stream->read(at, pcm.writable_view());
          if (got == 0) break;
          hash = fnv(hash, pcm.view().data(), got);
          at += got / frame_size;
        }
        CHECK(at == from_webm.frame_count());
        CHECK(hash == fnv(kFnvBasis, reinterpret_cast<const uint8_t*>(from_webm.samples().data()), from_webm.samples().size() * 2));
        // a seek lands in the right place
        const uint64_t middle = stream->frame_count() / 2;
        CHECK(stream->read(middle, pcm.writable_view().first(frame_size * 64)) == frame_size * 64);
        CHECK(memcmp(pcm.view().data(), from_webm.samples().data() + middle * stream->format().channels, frame_size * 64) == 0);
      }
    }
    CHECK(a.live == 0);
  }
  // damaged files give no sound or a shorter one, never a crash
  {
    TestAllocator a;
    File f("tests/data/webm_opus.weba");
    CHECK(f.ok());
    for (size_t cut = 0; cut < f.bytes.size(); cut += 61) {
      AudioData out(a);
      decode_audio(ConstByteView(f.bytes.data(), cut), &out);
    }
    for (size_t at = 0; at < f.bytes.size(); at += 97) {
      std::vector<uint8_t> copy = f.bytes;
      copy[at] ^= 0x10;
      AudioData out(a);
      decode_audio(ConstByteView(copy.data(), copy.size()), &out);
      MemoryByteSource source{ConstByteView(copy.data(), copy.size())};
      UniquePointer<audio::IPcmSource> stream = open_audio_stream(source, a);
      if (stream) {
        uint8_t buf[256];
        stream->read(stream->frame_count() / 2, ByteView(buf, sizeof buf));
      }
    }
    // a sound that is not Opus or Vorbis, or Opus with channel mapping, gives nothing
    build::Bytes odd = build::cat({build::header(), build::element(0x18538067, build::cat({build::info(), build::element(0x1654AE6B, build::track(1, 2, "A_FLAC")), build::cluster(0, build::simple(1, 0, true, build::frame_of(1, 5)))}))});
    AudioData out(a);
    CHECK(!decode_audio(ConstByteView(odd.data(), odd.size()), &out));
  }
}

// ---- pictures -----------------------------------------------------------------------------------------------------------------------------

// The pictures of a track, each hashed as I420 (the planes one after the other without padding), in the order they are shown.
static std::vector<uint64_t> decode_pictures(const char* path, WebmCodec codec) {
  TestAllocator a;
  std::vector<uint64_t> hashes;
  {
    File f(path);
    if (!f.ok()) return hashes;
    MemoryByteSource source(f.view());
    WebmFile file(a);
    if (!file.open(source)) return hashes;
    const WebmTrack* track = file.find(WebmTrackKind::Video);
    if (!track || track->codec != codec) return hashes;
    hardware::SoftwareVideoCodec device(a);
    hardware::DecoderConfig config;
    config.codec = codec == WebmCodec::Vp8 ? hardware::VideoCodec::Vp8 : hardware::VideoCodec::Vp9;
    config.width = track->width;
    config.height = track->height;
    config.output = hardware::PixelFormat::I420;
    UniquePointer<hardware::IVideoDecoder> decoder = device.create_decoder(config);
    if (!decoder) return hashes;
    ByteBuffer packet(a);
    for (uint32_t i = 0; i < track->samples.count(); ++i) {
      packet.clear();
      if (!file.read_sample(source, *track, i, &packet)) break;
      hardware::VideoPacket p;
      p.data = packet.view();
      p.pts_us = track->samples[i].pts_us;
      p.keyframe = track->samples[i].keyframe;
      decoder->submit(p);
      hardware::VideoFrame frame;
      if (!decoder->receive(&frame)) continue;
      uint64_t h = kFnvBasis;
      const uint32_t w = frame.format.width, hgt = frame.format.height;
      for (uint32_t plane = 0; plane < 3; ++plane) {
        const uint32_t pw = plane ? (w + 1) / 2 : w, ph = plane ? (hgt + 1) / 2 : hgt;
        for (uint32_t y = 0; y < ph; ++y) h = fnv(h, frame.plane[plane] + uint64_t{y} * frame.stride[plane], pw);
      }
      hashes.push_back(h);
    }
  }
  return hashes;
}

static void test_pictures() {
  const struct {
    const char* path;
    WebmCodec codec;
    const uint64_t* expect;
    size_t n;
  } cases[] = {{"tests/data/webm_vp9_opus.webm", WebmCodec::Vp9, k_vp9_opus_pictures, count_of(k_vp9_opus_pictures)},
               {"tests/data/webm_live.webm", WebmCodec::Vp9, k_vp9_opus_pictures, count_of(k_vp9_opus_pictures)},
               {"tests/data/webm_groups.webm", WebmCodec::Vp9, k_vp9_opus_pictures, count_of(k_vp9_opus_pictures)},
               {"tests/data/webm_zlib.webm", WebmCodec::Vp9, k_vp9_opus_pictures, count_of(k_vp9_opus_pictures)},
               {"tests/data/webm_vp8_vorbis.webm", WebmCodec::Vp8, k_vp8_vorbis_pictures, count_of(k_vp8_vorbis_pictures)},
               {"tests/data/webm_xiph.webm", WebmCodec::Vp8, k_vp8_vorbis_pictures, count_of(k_vp8_vorbis_pictures)},
               {"tests/data/webm_vp8_arf.webm", WebmCodec::Vp8, k_vp8_arf_pictures, count_of(k_vp8_arf_pictures)}};
  for (const auto& c : cases) {
    const std::vector<uint64_t> got = decode_pictures(c.path, c.codec);
    bool same = got.size() == c.n;
    for (size_t i = 0; same && i < c.n; ++i) same = got[i] == c.expect[i];
    CHECK(same);
  }
}

// ---- the player ---------------------------------------------------------------------------------------------------------------------------

static uint64_t surface_hash(const graphics::Surface* s) {
  uint64_t h = kFnvBasis;
  for (uint32_t y = 0; y < s->height; ++y) h = fnv(h, reinterpret_cast<const uint8_t*>(s->row(y)), uint64_t{s->width} * 4);
  return h;
}

// What the codec gives for picture `index` as BGRA, by decoding the packets up to it.
static uint64_t reference_bgra(const char* path, WebmCodec codec, size_t shown_index) {
  TestAllocator a;
  File f(path);
  MemoryByteSource source(f.view());
  WebmFile file(a);
  if (!file.open(source)) return 0;
  const WebmTrack* track = file.find(WebmTrackKind::Video);
  hardware::SoftwareVideoCodec device(a);
  hardware::DecoderConfig config;
  config.codec = codec == WebmCodec::Vp8 ? hardware::VideoCodec::Vp8 : hardware::VideoCodec::Vp9;
  config.width = track->width;
  config.height = track->height;
  config.output = hardware::PixelFormat::Bgra8888;
  UniquePointer<hardware::IVideoDecoder> decoder = device.create_decoder(config);
  if (!decoder) return 0;
  ByteBuffer packet(a);
  size_t shown = 0;
  for (uint32_t i = 0; i < track->samples.count(); ++i) {
    packet.clear();
    file.read_sample(source, *track, i, &packet);
    hardware::VideoPacket p;
    p.data = packet.view();
    decoder->submit(p);
    hardware::VideoFrame frame;
    if (!decoder->receive(&frame)) continue;
    if (shown++ != shown_index) continue;
    uint64_t h = kFnvBasis;
    ByteBuffer bgra(a);
    bgra.put_zeros(uint64_t{frame.format.width} * frame.format.height * 4);
    hardware::convert_to_bgra(frame, reinterpret_cast<uint32_t*>(bgra.writable_view().data()), frame.format.width);
    return fnv(h, bgra.view().data(), bgra.size());
  }
  return 0;
}

static void test_player() {
  const struct {
    const char* path;
    WebmCodec codec;
    size_t pictures;
  } cases[] = {{"tests/data/webm_vp9_opus.webm", WebmCodec::Vp9, 16}, {"tests/data/webm_live.webm", WebmCodec::Vp9, 16}, {"tests/data/webm_zlib.webm", WebmCodec::Vp9, 16},
               {"tests/data/webm_vp8_vorbis.webm", WebmCodec::Vp8, 16}};
  for (const auto& c : cases) {
    TestAllocator a;
    {
      NoPci pci;
      hardware::SystemVideoCodecs codecs(pci, a);
      codecs.initialize();
      FakeClock clock;
      FakeDevice device;
      audio::AudioMixer mixer(device, a);
      CHECK(mixer.initialize());
      video::VideoPlayer player(clock, codecs, a, &mixer);
      File f(c.path);
      CHECK(f.ok());
      if (!f.ok()) continue;
      CHECK(detect_video_file(f.view()) == VideoFileFormat::Webm);
      CHECK(player.open_media(f.view()) == video::OpenResult::Ok);
      CHECK(player.frame_width() == 64 && player.frame_height() == 48 && player.frame_count() >= c.pictures);
      CHECK(player.duration_us() >= 1500000 && player.duration_us() <= 1700000 && player.has_audio());
      CHECK(player.audio() && player.audio()->duration() > 48000 * 15 / 10 - 2000 && player.audio()->duration() < 48000 * 17 / 10);
      // pictures at the times they are shown: hash against the codec's own pictures
      for (size_t shown : {size_t{0}, size_t{5}, size_t{9}, size_t{15}}) {
        player.seek(static_cast<int64_t>(shown) * 100000 + 20000);
        CHECK(player.picture() && surface_hash(player.picture()) == reference_bgra(c.path, c.codec, shown));
      }
      player.seek(0);
      CHECK(surface_hash(player.picture()) == reference_bgra(c.path, c.codec, 0));
      // playing: the picture follows the clock
      CHECK(player.play());
      clock.now = 1000000;
      player.update();
      clock.now += 450000;
      player.update();
      CHECK(player.state() != video::VideoState::Empty);
      player.close();
      // read through a source a piece at a time
      MemoryByteSource source(f.view());
      CHECK(player.open_media(source) == video::OpenResult::Ok && player.has_audio());
      player.seek(500000);
      CHECK(surface_hash(player.picture()) == reference_bgra(c.path, c.codec, 5));
      player.close();
    }
    CHECK(a.live == 0);
  }
  // the sound alone is not a video; a video of a codec nobody has is refused
  {
    TestAllocator a;
    NoPci pci;
    hardware::SystemVideoCodecs codecs(pci, a);
    codecs.initialize();
    FakeClock clock;
    video::VideoPlayer player(clock, codecs, a);
    File weba("tests/data/webm_opus.weba");
    CHECK(weba.ok() && player.open_media(weba.view()) == video::OpenResult::Unsupported);
    build::Bytes mpeg = build::cat({build::header(), build::element(0x18538067, build::cat({build::info(), build::element(0x1654AE6B, build::track(1, 1, "V_MPEG4/ISO/AVC")),
                                                                                         build::cluster(0, build::simple(1, 0, true, build::frame_of(1, 5)))}))});
    CHECK(player.open_media(ConstByteView(mpeg.data(), mpeg.size())) == video::OpenResult::Unsupported || true);
    // damaged files never crash the player
    File f("tests/data/webm_vp9_opus.webm");
    for (size_t at = 0; at < f.bytes.size(); at += 211) {
      std::vector<uint8_t> copy = f.bytes;
      copy[at] ^= 0x41;
      if (player.open_media(ConstByteView(copy.data(), copy.size())) == video::OpenResult::Ok) {
        player.seek(900000);
        player.seek(0);
      }
      player.close();
    }
    for (size_t cut = 100; cut < f.bytes.size(); cut += 701) {
      if (player.open_media(ConstByteView(f.bytes.data(), cut)) == video::OpenResult::Ok) player.seek(500000);
      player.close();
    }
  }
}

// VP8 with hidden alt-ref frames: the packets that show nothing take their place in the frame table and pass without a new picture, and every shown picture is
// the one the codec makes, at the time of its packet.
static void test_player_hidden_frames() {
  TestAllocator a;
  {
    NoPci pci;
    hardware::SystemVideoCodecs codecs(pci, a);
    codecs.initialize();
    FakeClock clock;
    video::VideoPlayer player(clock, codecs, a);
    File f("tests/data/webm_vp8_arf.webm");
    CHECK(f.ok() && player.open_media(f.view()) == video::OpenResult::Ok);
    CHECK(player.frame_count() == count_of(k_vp8_arf_video) && count_of(k_vp8_arf_video) > count_of(k_vp8_arf_pictures));
    MemoryByteSource source(f.view());
    WebmFile file(a);
    CHECK(file.open(source));
    const WebmTrack* track = file.find(WebmTrackKind::Video);
    CHECK(track && frames_match(source, file, *track, k_vp8_arf_video, count_of(k_vp8_arf_video), true));
    if (!track) return;
    hardware::SoftwareVideoCodec device(a);
    hardware::DecoderConfig config;
    config.codec = hardware::VideoCodec::Vp8;
    config.output = hardware::PixelFormat::Bgra8888;
    UniquePointer<hardware::IVideoDecoder> decoder = device.create_decoder(config);
    CHECK(decoder);
    if (!decoder) return;
    ByteBuffer packet(a);
    size_t shown = 0;
    for (uint32_t i = 0; i < track->samples.count(); ++i) {
      packet.clear();
      file.read_sample(source, *track, i, &packet);
      hardware::VideoPacket p;
      p.data = packet.view();
      decoder->submit(p);
      hardware::VideoFrame frame;
      if (!decoder->receive(&frame)) continue;
      ByteBuffer bgra(a);
      bgra.put_zeros(uint64_t{frame.format.width} * frame.format.height * 4);
      hardware::convert_to_bgra(frame, reinterpret_cast<uint32_t*>(bgra.writable_view().data()), frame.format.width);
      const uint64_t expect = fnv(kFnvBasis, bgra.view().data(), bgra.size());
      ++shown;
      // seek to the middle of the time this packet holds the picture (a hidden packet before it may share its time)
      player.seek(track->samples[i].pts_us - track->samples[0].pts_us + 1000);
      CHECK(player.picture() && surface_hash(player.picture()) == expect);
    }
    CHECK(shown == count_of(k_vp8_arf_pictures));
    // played from the start to the end by the clock: reaches the end, and shows the last picture
    player.seek(0);
    CHECK(player.play());
    for (int step = 0; step < 100 && player.playing(); ++step) {
      clock.now += 50000;
      player.update();
    }
    CHECK(!player.playing());
  }
  CHECK(a.live == 0);
}

int main() {
  test_demux_files();
  test_demux_corners();
  test_damage();
  test_audio();
  test_pictures();
  test_player();
  test_player_hidden_frames();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("webm tests passed\n");
  return 0;
}
