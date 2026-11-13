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

// Host tests for the kernel's PipeWire (src/PipeWire): the SPA POD and PulseAudio tagstruct codecs (round trips, every truncation, bit flips), the PCM
// conversion both ways, recording (a fake microphone through the capture hub to record streams, PulseAudio and Windows), the graph over a real AudioMixer and a fake device (native, Linux and Windows streams mixed into one output), the native protocol and the
// PulseAudio protocol driven as their clients drive them (and fed garbage), the Windows service, the AF_UNIX transport, and several threads at once.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "PipeWire/Graph.hpp"
#include "PipeWire/CameraSource.hpp"
#include "PipeWire/ScreenSource.hpp"
#include "PipeWire/VideoWatcher.hpp"
#include "PipeWire/NativeServer.hpp"
#include "PipeWire/VideoConvert.hpp"
#include "PipeWire/VideoDecode.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "PipeWire/Pod.hpp"
#include "PipeWire/PulseServer.hpp"
#include "PipeWire/RecordStream.hpp"
#include "PipeWire/SocketServer.hpp"
#include "Process/MemoryFile.hpp"
#include "PipeWire/Tagstruct.hpp"
#include "Audio/Pan.hpp"
#include "PipeWire/WindowsAudioService.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/ByteStream.hpp"

using namespace pipewire;
using hardware::kAudioFrameSize;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t align) override {
    ++live;
    void* p = nullptr;
    if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size ? size : 1) != 0) return nullptr;
    return p;
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static TestAllocator A;

static std::atomic<int> g_failed{0};
static std::atomic<int> g_checks{0};
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// A device that keeps what it is given and "plays" whatever the test says.
struct FakeDevice final : hardware::IAudioDevice {
  std::mutex m;
  std::vector<int16_t> recorded;
  uint32_t queued = 0;
  uint32_t capacity = 64 * 1024;
  uint8_t vol = 100;
  bool mute = false;
  const char* name() const override { return "Fake speakers"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    std::lock_guard<std::mutex> g(m);
    uint32_t n = pcm.size() < capacity - queued ? uint32_t(pcm.size()) : capacity - queued;
    n -= n % kAudioFrameSize;
    for (uint32_t i = 0; i + 1 < n; i += 2) recorded.push_back(int16_t(pcm[i] | pcm[i + 1] << 8));
    queued += n;
    return n;
  }
  uint32_t queued_bytes() override { std::lock_guard<std::mutex> g(m); return queued; }
  uint32_t free_bytes() override { std::lock_guard<std::mutex> g(m); return capacity - queued; }
  void play_all() { std::lock_guard<std::mutex> g(m); queued = 0; }
  void set_volume(uint8_t v) override { vol = v; }
  uint8_t volume() const override { return vol; }
  void set_muted(bool v) override { mute = v; }
  bool muted() const override { return mute; }
  hardware::AudioStatistics statistics() override { return {}; }
  std::vector<int16_t> take() { std::lock_guard<std::mutex> g(m); std::vector<int16_t> r; r.swap(recorded); return r; }
};

// Runs the mixer until it has nothing left (the device "plays" between rounds).
static void drain_mixer(audio::AudioMixer& mixer, FakeDevice& dev, int rounds = 400) {
  for (int i = 0; i < rounds; ++i) {
    mixer.pump();
    dev.play_all();
  }
}

// A file of the tests' data (empty when it is not there).
static std::vector<uint8_t> read_test_file(const char* path) {
  std::vector<uint8_t> out;
  if (FILE* f = fopen(path, "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
  }
  return out;
}

// The clock the tests move by hand (nanoseconds).
static std::atomic<uint64_t> g_now_ns{1000000000ull};
static uint64_t test_clock() { return g_now_ns.load(); }

static std::vector<uint8_t> s16_constant(uint32_t frames, uint32_t channels, int16_t value) {
  std::vector<uint8_t> b(frames * channels * 2);
  for (uint32_t i = 0; i < frames * channels; ++i) { b[2 * i] = uint8_t(value); b[2 * i + 1] = uint8_t(uint16_t(value) >> 8); }
  return b;
}

// ---- SPA PODs ------------------------------------------------------------------------------------------------------------------------------------

static void walk_pod(pod::Parser p, int depth);
static void walk_value(const pod::Value& v, int depth) {
  bool b; uint32_t u; int32_t i; int64_t l; float f; double d; StringView s; ConstByteView bytes;
  v.get_bool(&b); v.get_id(&u); v.get_int(&i); v.get_long(&l); v.get_float(&f); v.get_double(&d); v.get_string(&s, true); v.get_bytes(&bytes); v.get_fd(&l);
  if (depth > 8) return;
  if (v.type == pod::kStruct) walk_pod(pod::Parser(v.body, depth + 1), depth + 1);
  pod::ObjectReader o;
  if (o.open(v)) {
    uint32_t key, flags; pod::Value inner;
    while (o.next(&key, &flags, &inner)) walk_value(inner, depth + 1);
  }
  pod::ArrayReader a;
  if (a.open(v)) for (uint64_t k = 0; k < a.count() && k < 64; ++k) a.get32(k, &u);
}
static void walk_pod(pod::Parser p, int depth) {
  pod::Value v;
  while (p.next(&v)) walk_value(v, depth);
}

static void test_pod() {
  ByteBuffer buf(A);
  pod::Builder b(buf);
  const uint64_t s = b.begin_struct();
  b.int32(-5);
  b.int64(1234567890123ll);
  b.id(0x103);
  b.boolean(true);
  b.float32(0.5f);
  b.float64(2.25);
  b.string("hello");
  b.string_or_none(nullptr);
  b.fd(3);
  b.fraction(1, 48000);
  b.rectangle(640, 480);
  const uint8_t raw[3] = {1, 2, 3};
  b.bytes(ConstByteView(raw, 3));
  const uint32_t ids[3] = {3, 4, 5};
  b.array_ids(ArrayView<const uint32_t>(ids, 3));
  const uint64_t inner = b.begin_struct();
  b.string("nested");
  b.end(inner);
  build_props_param(b, 0.25f, true, 2);
  build_format_param(b, 4, PcmFormat{SampleFormat::F32Le, 44100, 2});
  const uint64_t ch = b.begin_choice(pod::kChoiceEnum, pod::kInt);
  b.choice_value32(1); b.choice_value32(2);
  b.end(ch);
  b.end(s);
  CHECK(b.ok() && buf.size() % 8 == 0);

  pod::Parser outer(buf.view()), p;
  CHECK(outer.structure(&p));
  int32_t i = 0; int64_t l = 0; uint32_t id = 0; bool flag = false; StringView text;
  CHECK(p.int32(&i) && i == -5);
  CHECK(p.int64(&l) && l == 1234567890123ll);
  CHECK(p.id(&id) && id == 0x103);
  CHECK(p.boolean(&flag) && flag);
  pod::Value v;
  float f = 0; double d = 0;
  CHECK(p.next(&v) && v.get_float(&f) && f == 0.5f);
  CHECK(p.next(&v) && v.get_double(&d) && d == 2.25);
  CHECK(p.string(&text) && text == StringView("hello"));
  bool none = false;
  CHECK(p.next(&v) && v.get_string(&text, true, &none) && none);
  CHECK(p.next(&v) && v.get_fd(&l) && l == 3);
  CHECK(p.next(&v) && v.type == pod::kFraction && v.body.size() == 8);
  CHECK(p.next(&v) && v.type == pod::kRectangle);
  ConstByteView bytes;
  CHECK(p.next(&v) && v.get_bytes(&bytes) && bytes.size() == 3 && bytes[2] == 3);
  pod::ArrayReader a;
  uint32_t e = 0;
  CHECK(p.next(&v) && a.open(v) && a.count() == 3 && a.get32(2, &e) && e == 5 && !a.get32(3, &e));
  pod::Parser nested;
  CHECK(p.structure(&nested) && nested.string(&text) && text == StringView("nested"));
  // the Props object
  pod::ObjectReader o;
  CHECK(p.next(&v) && o.open(v) && o.object_type() == 0x40002 && o.object_id() == 2);
  uint32_t key = 0, flags = 0;
  float volume = 0; bool mute = false; int props = 0;
  while (o.next(&key, &flags, &v)) {
    ++props;
    if (key == 0x10003) v.get_float(&volume);
    if (key == 0x10004) v.get_bool(&mute);
  }
  CHECK(o.ok() && props == 8 && volume == 0.25f && mute);
  CHECK(p.next(&v) && o.open(v) && o.object_type() == 0x40003 && o.object_id() == 4);
  int32_t rate = 0;
  while (o.next(&key, &flags, &v)) if (key == 0x10003) v.get_int(&rate);
  CHECK(rate == 44100);
  CHECK(p.next(&v) && a.open(v) && a.choice_type() == pod::kChoiceEnum && a.count() == 2);
  CHECK(p.at_end() && p.ok());

  // wrong types fail and stay failed
  pod::Parser q(buf.view()), qs;
  CHECK(q.structure(&qs) && !qs.string(&text) && !qs.ok() && !qs.int32(&i));
  // a string without its NUL
  ByteBuffer bad(A);
  bad.put32(4); bad.put32(pod::kString); bad.put32(0x41414141); bad.put32(0);
  pod::Parser bp(bad.view());
  CHECK(bp.next(&v) && !v.get_string(&text));

  // every truncation and many bit flips: nothing is read out of bounds (ASan), nothing loops
  for (uint64_t cut = 0; cut <= buf.size(); ++cut) walk_pod(pod::Parser(buf.view().first(cut)), 0);
  std::mt19937 rng(7);
  std::vector<uint8_t> copy(buf.view().data(), buf.view().data() + buf.size());
  for (int round = 0; round < 3000; ++round) {
    std::vector<uint8_t> m = copy;
    const int flips = 1 + int(rng() % 4);
    for (int k = 0; k < flips; ++k) m[rng() % m.size()] ^= uint8_t(1u << (rng() % 8));
    walk_pod(pod::Parser(ConstByteView(m.data(), m.size())), 0);
  }
  // deep nesting is cut off
  ByteBuffer deep(A);
  pod::Builder db(deep);
  uint64_t frames[40];
  for (int k = 0; k < 40; ++k) frames[k] = db.begin_struct();
  for (int k = 39; k >= 0; --k) db.end(frames[k]);
  pod::Parser dp(deep.view()), cur;
  int levels = 0;
  pod::Parser at = dp;
  while (at.structure(&cur)) { at = cur; ++levels; }
  CHECK(levels < int(pod::kMaxDepth));
}

// ---- tagstruct -----------------------------------------------------------------------------------------------------------------------------------

static void walk_tags(ConstByteView data) {
  // reads whatever is there, tag by tag, with every reader
  for (int kind = 0; kind < 16; ++kind) {
    pulse::TagReader t(data);
    uint32_t u; uint8_t b; uint64_t q; int64_t s; bool flag; StringView str; ConstByteView arb; pulse::SampleSpec ss; pulse::ChannelMap cm; pulse::Cvolume cv;
    uint32_t sec, usec; Properties pl(A); uint8_t enc;
    for (int k = 0; k < 64 && !t.at_end() && t.ok(); ++k) {
      switch ((kind + k) % 15) {
        case 0: t.u32(&u); break; case 1: t.u8(&b); break; case 2: t.u64(&q); break; case 3: t.s64(&s); break; case 4: t.boolean(&flag); break;
        case 5: t.string(&str); break; case 6: t.arbitrary(&arb); break; case 7: t.sample_spec(&ss); break; case 8: t.channel_map(&cm); break;
        case 9: t.cvolume(&cv); break; case 10: t.timeval(&sec, &usec); break; case 11: t.proplist(&pl); break; case 12: t.format_info(&enc, &pl); break;
        case 13: t.usec(&q); break; default: t.volume(&u); break;
      }
    }
  }
}

static void test_tagstruct() {
  ByteBuffer buf(A);
  pulse::TagWriter w(buf);
  Properties props(A);
  props.set("application.name", "paplay");
  props.set("media.name", "tone");
  w.u32(42); w.u8(7); w.u64(1ull << 40); w.s64(-3); w.usec(5000); w.boolean(true); w.boolean(false); w.string("abc"); w.string(static_cast<const char*>(nullptr));
  const uint8_t cookie[4] = {9, 8, 7, 6};
  w.arbitrary(ConstByteView(cookie, 4));
  w.sample_spec(pulse::SampleSpec{pulse::kFloat32Le, 2, 44100});
  pulse::ChannelMap map; map.channels = 2; map.positions[0] = 1; map.positions[1] = 2;
  w.channel_map(map);
  pulse::Cvolume vol; vol.channels = 2; vol.values[0] = 0x8000; vol.values[1] = 0x10000;
  w.cvolume(vol);
  w.volume(0x10000);
  w.timeval(1, 2);
  w.proplist(props);
  w.format_info(1, props);
  CHECK(w.ok());

  pulse::TagReader t(buf.view());
  uint32_t u = 0; uint8_t b = 0; uint64_t q = 0; int64_t s = 0; bool flag = false, is_null = false; StringView str; ConstByteView arb;
  CHECK(t.u32(&u) && u == 42);
  CHECK(t.u8(&b) && b == 7);
  CHECK(t.u64(&q) && q == 1ull << 40);
  CHECK(t.s64(&s) && s == -3);
  CHECK(t.usec(&q) && q == 5000);
  CHECK(t.boolean(&flag) && flag && t.boolean(&flag) && !flag);
  CHECK(t.string(&str, &is_null) && str == StringView("abc") && !is_null);
  CHECK(t.string(&str, &is_null) && is_null);
  CHECK(t.arbitrary(&arb) && arb.size() == 4 && arb[3] == 6);
  pulse::SampleSpec ss;
  CHECK(t.sample_spec(&ss) && ss.format == pulse::kFloat32Le && ss.channels == 2 && ss.rate == 44100 && ss.frame_size() == 8);
  pulse::ChannelMap cm;
  CHECK(t.channel_map(&cm) && cm.channels == 2 && cm.positions[1] == 2);
  pulse::Cvolume cv;
  CHECK(t.cvolume(&cv) && cv.channels == 2 && cv.max() == 0x10000);
  CHECK(t.volume(&u) && u == 0x10000);
  uint32_t sec = 0, usec = 0;
  CHECK(t.timeval(&sec, &usec) && sec == 1 && usec == 2);
  Properties got(A);
  CHECK(t.proplist(&got) && got.count() == 2 && got.get("media.name") == StringView("tone"));
  uint8_t enc = 0;
  CHECK(t.format_info(&enc, &got) && enc == 1 && got.get("application.name") == StringView("paplay"));
  CHECK(t.at_end() && t.ok());
  // wrong tag
  pulse::TagReader t2(buf.view());
  CHECK(!t2.string(&str) && !t2.ok() && !t2.u32(&u));
  // truncation and bit flips
  for (uint64_t cut = 0; cut <= buf.size(); ++cut) walk_tags(buf.view().first(cut));
  std::mt19937 rng(11);
  std::vector<uint8_t> copy(buf.view().data(), buf.view().data() + buf.size());
  for (int round = 0; round < 2000; ++round) {
    std::vector<uint8_t> m = copy;
    m[rng() % m.size()] ^= uint8_t(1u << (rng() % 8));
    walk_tags(ConstByteView(m.data(), m.size()));
  }
  // volumes
  CHECK(pulse_volume_to_linear(pulse::kVolumeNorm) == 1.0f && pulse_volume_to_linear(0) == 0.0f);
  const uint32_t half = linear_to_pulse_volume(0.125f);
  CHECK(half > 0x7F00 && half < 0x8100);      // cube root of 1/8 = 1/2
  CHECK(linear_to_pulse_volume(1.0f) == pulse::kVolumeNorm);
}

// ---- conversion ----------------------------------------------------------------------------------------------------------------------------------

static void test_convert() {
  uint8_t out[64];
  uint32_t used = 0;
  // S16 stereo stays as it is
  const uint8_t s16[8] = {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00};
  CHECK(convert_to_s16(PcmFormat{SampleFormat::S16Le, 48000, 2}, nullptr, ConstByteView(s16, 8), ByteView(out, 64), 2, &used) == 8 && used == 8 && out[0] == 0x10 && out[6] == 0x40);
  // U8: 128 is silence, 255 nearly full scale
  const uint8_t u8[2] = {128, 255};
  CHECK(convert_to_s16(PcmFormat{SampleFormat::U8, 8000, 1}, nullptr, ConstByteView(u8, 2), ByteView(out, 64), 1, &used) == 4 && out[0] == 0 && out[1] == 0 && int16_t(out[2] | out[3] << 8) == 127 * 256);
  // F32: 0.5 -> 16383
  float fl[2] = {0.5f, -1.0f};
  CHECK(convert_to_s16(PcmFormat{SampleFormat::F32Le, 48000, 2}, nullptr, ConstByteView(reinterpret_cast<uint8_t*>(fl), 8), ByteView(out, 64), 2, &used) == 4);
  CHECK(int16_t(out[0] | out[1] << 8) == 16383 && int16_t(out[2] | out[3] << 8) == -32767);
  // a NaN is silence
  float nan = __builtin_nanf("");
  CHECK(convert_to_s16(PcmFormat{SampleFormat::F32Le, 48000, 1}, nullptr, ConstByteView(reinterpret_cast<uint8_t*>(&nan), 4), ByteView(out, 64), 1, &used) == 2 && out[0] == 0 && out[1] == 0);
  // S24 / S32 take their top 16 bits; big endian too
  const uint8_t s24[3] = {0x00, 0x34, 0x12}, s32[4] = {0, 0, 0x34, 0x12}, s16be[2] = {0x12, 0x34};
  CHECK(convert_to_s16(PcmFormat{SampleFormat::S24Le, 48000, 1}, nullptr, ConstByteView(s24, 3), ByteView(out, 64), 1, &used) == 2 && out[0] == 0x34 && out[1] == 0x12);
  CHECK(convert_to_s16(PcmFormat{SampleFormat::S32Le, 48000, 1}, nullptr, ConstByteView(s32, 4), ByteView(out, 64), 1, &used) == 2 && out[0] == 0x34 && out[1] == 0x12);
  CHECK(convert_to_s16(PcmFormat{SampleFormat::S16Be, 48000, 1}, nullptr, ConstByteView(s16be, 2), ByteView(out, 64), 1, &used) == 2 && out[0] == 0x34 && out[1] == 0x12);
  // G.711: the codes for silence decode near 0, the loudest near full scale
  const uint8_t ulaw[2] = {0xFF, 0x80}, alaw[2] = {0xD5, 0xAA};
  CHECK(convert_to_s16(PcmFormat{SampleFormat::Ulaw, 8000, 1}, nullptr, ConstByteView(ulaw, 2), ByteView(out, 64), 1, &used) == 4);
  CHECK(int16_t(out[0] | out[1] << 8) == 0 && int16_t(out[2] | out[3] << 8) > 30000);
  CHECK(convert_to_s16(PcmFormat{SampleFormat::Alaw, 8000, 1}, nullptr, ConstByteView(alaw, 2), ByteView(out, 64), 1, &used) == 4);
  CHECK(abs(int16_t(out[0] | out[1] << 8)) < 16 && int16_t(out[2] | out[3] << 8) > 30000);
  // 5.1 folds to stereo: FL into left, FR into right, centre into both
  int16_t six[6] = {1000, 2000, 3000, 30000, 0, 0};      // FL FR FC LFE RL RR
  CHECK(convert_to_s16(PcmFormat{SampleFormat::S16Le, 48000, 6}, nullptr, ConstByteView(reinterpret_cast<uint8_t*>(six), 12), ByteView(out, 64), 2, &used) == 4 && used == 12);
  const int16_t l = int16_t(out[0] | out[1] << 8), r = int16_t(out[2] | out[3] << 8);
  CHECK(l > 0 && r > l && l < 3000 && r < 3000);      // (the LFE is left out)
  // stereo to mono and partial frames
  CHECK(convert_to_s16(PcmFormat{SampleFormat::S16Le, 48000, 2}, nullptr, ConstByteView(s16, 7), ByteView(out, 64), 1, &used) == 2 && used == 4);
  CHECK(convert_to_s16(PcmFormat{SampleFormat::S16Le, 48000, 2}, nullptr, ConstByteView(s16, 8), ByteView(out, 3), 2, &used) == 0 && used == 0);
  CHECK(convert_to_s16(PcmFormat{SampleFormat::S16Le, 0, 2}, nullptr, ConstByteView(s16, 8), ByteView(out, 64), 2, &used) == 0);
}

// ---- the graph -----------------------------------------------------------------------------------------------------------------------------------

static uint32_t count_type(Graph& g, ObjectType t) {
  uint32_t n = 0;
  g.for_each([&](const GraphObject& o) { n += o.type == t; });
  return n;
}

static void test_graph() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);
  CHECK(g.initialize());
  // (the sink: two playback ports, two monitor ports; the MIDI bridge: an input and an output port)
  CHECK(count_type(g, ObjectType::Node) == 2 && count_type(g, ObjectType::Port) == 6 && count_type(g, ObjectType::Device) == 1 && count_type(g, ObjectType::Metadata) == 1);
  bool sink_ok = false;
  g.visit(g.sink_id(), [&](const GraphObject& o) { sink_ok = o.props.get("media.class") == StringView("Audio/Sink") && o.role == NodeRole::Sink; });
  CHECK(sink_ok);
  g.refresh();
  bool named = false;
  g.visit(g.sink_id(), [&](const GraphObject& o) { named = o.props.get("node.description") == StringView("Fake speakers"); });
  CHECK(named);

  // a Linux client, a Windows client, a native stream
  const uint32_t linux_client = g.add_client(Origin::Linux, 100, "protocol-pulse", "pipewire-pulse");
  const uint32_t windows_client = g.add_client(Origin::Windows, 200, "tcos-winmm", "tcos-winmm");
  CHECK(linux_client && windows_client);
  Properties props(A);
  props.set("application.name", "paplay");
  props.set("media.name", "tone");
  SharedPointer<PlaybackStream> linux_stream, windows_stream;
  const uint32_t ln = g.create_playback(linux_client, PcmFormat{SampleFormat::S16Le, 48000, 2}, props, &linux_stream);
  Properties wprops(A);
  wprops.set("application.name", "game.exe");
  const uint32_t wn = g.create_playback(windows_client, PcmFormat{SampleFormat::F32Le, 48000, 2}, wprops, &windows_stream);
  CHECK(ln && wn && linux_stream && windows_stream);
  SharedPointer<audio::AudioStream> native = mixer.open_stream(audio::StreamFormat(), 32768);
  native->set_label("Media Player");
  const uint64_t before = g.serial();
  g.refresh();
  CHECK(g.serial() > before);
  CHECK(g.playback_count() == 3);
  CHECK(count_type(g, ObjectType::Link) == 6 && count_type(g, ObjectType::Port) == 6 + 6);
  bool native_seen = false;
  g.for_each([&](const GraphObject& o) {
    if (o.native.get() == native.get()) native_seen = o.client == g.native_client_id() && o.props.get("application.name") == StringView("Media Player");
  });
  CHECK(native_seen);
  const uint64_t settled = g.serial();
  g.refresh();
  CHECK(g.serial() == settled);      // nothing changed: no new serial

  // the three play at once: 1000 + 2000 + 0.25 (8191)
  std::vector<uint8_t> a = s16_constant(4800, 2, 1000), n = s16_constant(4800, 2, 2000);
  std::vector<float> f(4800 * 2, 0.25f);
  CHECK(linux_stream->write(ConstByteView(a.data(), a.size())) == a.size());
  CHECK(windows_stream->write(ConstByteView(reinterpret_cast<uint8_t*>(f.data()), f.size() * 4)) == f.size() * 4);
  CHECK(native->write(ConstByteView(n.data(), n.size())) == n.size());
  dev.take();
  mixer.pump();
  std::vector<int16_t> out = dev.take();
  CHECK(out.size() > 100);
  bool summed = out.size() > 100;
  for (size_t i = 0; i < out.size() && i < 2000; ++i) summed &= abs(out[i] - (1000 + 2000 + 8191)) <= 2;
  CHECK(summed);

  // a stream's volume and mute through the graph
  CHECK(g.set_volume(ln, 0.5f, false));
  dev.play_all();
  mixer.pump();
  out = dev.take();
  CHECK(!out.empty() && abs(out[0] - (500 + 2000 + 8191)) <= 3);
  CHECK(g.set_volume(wn, 1.0f, true));
  dev.play_all();
  mixer.pump();
  out = dev.take();
  CHECK(!out.empty() && abs(out[0] - (500 + 2000)) <= 3);
  // the sink's volume is the master volume
  CHECK(g.set_volume(g.sink_id(), 0.3f, false) && mixer.master_volume() == 30 && dev.vol == 30);
  mixer.set_master_volume(70);
  g.refresh();
  float v = 0; bool m = true;
  CHECK(g.get_volume(g.sink_id(), &v, &m) && v > 0.69f && v < 0.71f && !m);
  // pause: nothing from that stream until resumed
  CHECK(g.set_paused(ln, true));
  dev.play_all();
  mixer.pump();
  out = dev.take();
  CHECK(!out.empty() && abs(out[0] - 2000) <= 3);
  CHECK(g.set_paused(ln, false));

  // the Windows client leaves: its node, ports and links go, its stream stops
  g.remove_client(windows_client);
  CHECK(g.playback_count() == 2 && count_type(g, ObjectType::Link) == 4);
  CHECK(windows_stream->stream().closed() && !windows_stream->drained());      // (what the device still holds is not played yet)
  dev.play_all();
  CHECK(windows_stream->drained());
  // destroy with drain: still plays out
  g.destroy_node(ln, true);
  CHECK(linux_stream->stream().closed() && !linux_stream->drained());
  drain_mixer(mixer, dev);
  CHECK(linux_stream->drained());
  g.refresh();
  // the native stream ends: its node goes once the mixer lets go of it
  native->close();
  drain_mixer(mixer, dev);
  g.refresh();
  CHECK(g.playback_count() == 0);
  // a closed graph stream the mixer let go of is not mistaken for a native one
  CHECK(count_type(g, ObjectType::Link) == 0);
  // bad formats and missing clients
  SharedPointer<PlaybackStream> none;
  CHECK(g.create_playback(linux_client, PcmFormat{SampleFormat::S16Le, 1000, 2}, props, &none) == 0 && !none);
  CHECK(g.create_playback(9999, PcmFormat{SampleFormat::S16Le, 48000, 2}, props, &none) == 0);
  // client properties: the graph's own keys stay
  Properties update(A);
  update.set("application.name", "renamed");
  update.set("pipewire.protocol", "forged");
  CHECK(g.update_client(linux_client, update, Properties::Mode::kReplace));
  bool kept = false;
  g.visit(linux_client, [&](const GraphObject& o) { kept = o.props.get("pipewire.protocol") == StringView("protocol-pulse") && o.props.get("application.name") == StringView("renamed"); });
  CHECK(kept);
  // a graph without a mixer still answers
  Graph empty(A, nullptr);
  CHECK(empty.initialize() && empty.create_playback(empty.add_client(Origin::Linux, 1, "x", ""), PcmFormat{}, props, &none) == 0);
}

// ---- the native protocol -------------------------------------------------------------------------------------------------------------------------

struct NativeMessage {
  uint32_t id, opcode, seq, n_fds = 0;
  std::vector<uint8_t> payload;
};

// How a NativeClient reaches the server when not by calling it: a socket (the kernel's transport, SocketServer).
struct NativeTransport {
  virtual void send(ConstByteView bytes) = 0;
  virtual void take(ByteBuffer* out, std::vector<Fd>* fds) = 0;
  virtual ~NativeTransport() = default;
};

struct NativeClient {
  NativeServer& server;
  uint32_t handle;
  uint32_t seq = 0;
  std::vector<uint8_t> in;
  NativeTransport* transport = nullptr;
  NativeClient(NativeServer& s) : server(s), handle(s.open_client(77)) {}
  NativeClient(NativeServer& s, NativeTransport& t) : server(s), handle(0), transport(&t) {}
  template <class F>
  void send(uint32_t id, uint32_t opcode, F&& args) {
    ByteBuffer body(A);
    pod::Builder b(body);
    const uint64_t f = b.begin_struct();
    args(b);
    b.end(f);
    ByteBuffer msg(A);
    msg.put32(id); msg.put32(opcode << 24 | uint32_t(body.size())); msg.put32(++seq); msg.put32(0);
    msg.append(body.view());
    if (transport) transport->send(msg.view());
    else server.receive(handle, msg.view());
  }
  std::vector<Fd> fds;      // what take_with_fds() collected, in order
  std::vector<NativeMessage> take_with_fds() {
    ByteBuffer out(A);
    if (transport) {
      transport->take(&out, &fds);
      return parse(out);
    }
    List<Fd> got(A);
    server.take_output_with_fds(handle, &out, &got);
    for (Fd& f : got) fds.push_back(f);
    return parse(out);
  }
  std::vector<NativeMessage> take() {
    ByteBuffer out(A);
    server.take_output(handle, &out);
    return parse(out);
  }
  std::vector<NativeMessage> parse(ByteBuffer& out) {
    in.insert(in.end(), out.view().data(), out.view().data() + out.size());
    std::vector<NativeMessage> r;
    size_t at = 0;
    while (in.size() - at >= 16) {
      ByteReader h(ConstByteView(in.data() + at, 16));
      NativeMessage m;
      m.id = h.le32();
      const uint32_t w = h.le32();
      m.seq = h.le32();
      m.n_fds = h.le32();
      m.opcode = w >> 24;
      const uint32_t size = w & 0xFFFFFF;
      if (in.size() - at - 16 < size) break;
      m.payload.assign(in.begin() + at + 16, in.begin() + at + 16 + size);
      r.push_back(m);
      at += 16 + size;
    }
    in.erase(in.begin(), in.begin() + at);
    return r;
  }
};

static pod::Parser args_of(const NativeMessage& m) {
  pod::Parser outer(ConstByteView(m.payload.data(), m.payload.size())), p;
  outer.structure(&p);
  return p;
}

// The dict of a message: the Struct after `skip` values.
static std::string dict_value(pod::Parser p, int skip, const char* key) {
  pod::Value v;
  for (int i = 0; i < skip; ++i) p.next(&v);
  pod::Parser d;
  int32_t n = 0;
  if (!p.structure(&d) || !d.int32(&n)) return "<none>";
  for (int32_t i = 0; i < n; ++i) {
    StringView k, val;
    if (!d.string(&k) || !d.string(&val)) return "<bad>";
    if (k == StringView(key)) return std::string(val.data(), val.size());
  }
  return "<missing>";
}

static void test_native_protocol() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);
  CHECK(g.initialize());
  NativeServer server(g, A);
  NativeClient c(server);
  CHECK(c.handle != 0 && server.client_count() == 1);
  c.send(0, 1, [](pod::Builder& b) { b.int32(4); });      // hello
  c.send(1, 2, [](pod::Builder& b) {                      // client update_properties
    const uint64_t d = b.begin_struct();
    b.int32(1); b.string("application.name"); b.string("pw-dump");
    b.end(d);
  });
  c.send(0, 5, [](pod::Builder& b) { b.int32(3); b.int32(2); });      // get_registry, new id 2
  c.send(0, 2, [](pod::Builder& b) { b.int32(0); b.int32(99); });     // sync
  auto msgs = c.take();
  bool core_info = false, client_info = false, done = false;
  uint32_t sink_global = 0, port_global = 0, metadata_global = 0, globals = 0;
  for (const NativeMessage& m : msgs) {
    if (m.id == 0 && m.opcode == 0) core_info = dict_value(args_of(m), 7, "core.name") == "pipewire-0";
    if (m.id == 1 && m.opcode == 0) client_info = dict_value(args_of(m), 2, "application.name") == "pw-dump";
    if (m.id == 2 && m.opcode == 0) {
      ++globals;
      pod::Parser p = args_of(m);
      int32_t id = 0, perms = 0, version = 0;
      StringView type;
      p.int32(&id); p.int32(&perms); p.string(&type); p.int32(&version);
      if (type == StringView("PipeWire:Interface:Node") && dict_value(args_of(m), 4, "media.class") == "Audio/Sink") sink_global = uint32_t(id);
      if (type == StringView("PipeWire:Interface:Port")) port_global = uint32_t(id);
      if (type == StringView("PipeWire:Interface:Metadata")) metadata_global = uint32_t(id);
    }
    if (m.id == 0 && m.opcode == 1) {
      pod::Parser p = args_of(m);
      int32_t id = -1, seq = 0;
      done = p.int32(&id) && p.int32(&seq) && id == 0 && seq == 99;
    }
  }
  CHECK(core_info && client_info && done && sink_global == g.sink_id() && port_global && metadata_global);
  CHECK(globals >= 7);      // core, 2 clients (the kernel's and this one), metadata, device, sink, 2 ports
  // the done comes after every global
  CHECK(!msgs.empty() && msgs.back().id == 0 && msgs.back().opcode == 1);

  // bind the sink, enumerate its Props, change its volume
  c.send(2, 1, [&](pod::Builder& b) { b.int32(int32_t(sink_global)); b.string("PipeWire:Interface:Node"); b.int32(3); b.int32(3); });
  c.send(3, 2, [](pod::Builder& b) { b.int32(5); b.id(2); b.int32(0); b.int32(1); b.none(); });      // enum_params Props
  c.send(3, 2, [](pod::Builder& b) { b.int32(6); b.id(4); b.int32(0); b.int32(1); b.none(); });      // enum_params Format
  msgs = c.take();
  bool bound = false, node_info = false, props_param = false, format_param = false;
  for (const NativeMessage& m : msgs) {
    if (m.id == 0 && m.opcode == 8) { pod::Parser p = args_of(m); int32_t id = 0, global = 0; bound = p.int32(&id) && p.int32(&global) && id == 3 && uint32_t(global) == sink_global; }
    if (m.id == 3 && m.opcode == 0) {
      pod::Parser p = args_of(m);
      int32_t id = 0, max_in = 0, max_out = 0, n_in = 0, n_out = 0; int64_t mask = 0; uint32_t state = 0;
      node_info = p.int32(&id) && p.int32(&max_in) && p.int32(&max_out) && p.int64(&mask) && p.int32(&n_in) && p.int32(&n_out) && p.id(&state) && n_in == 2;
      CHECK(dict_value(args_of(m), 8, "node.name") == "tcos_output.stereo");
    }
    if (m.id == 3 && m.opcode == 1) {
      pod::Parser p = args_of(m);
      int32_t seq = 0, index = 0, next = 0; uint32_t id = 0; pod::Value param;
      if (p.int32(&seq) && p.id(&id) && p.int32(&index) && p.int32(&next) && p.pod(&param)) {
        if (seq == 5 && id == 2) props_param = param.type == pod::kObject;
        if (seq == 6 && id == 4) format_param = param.type == pod::kObject;
      }
    }
  }
  CHECK(bound && node_info && props_param && format_param);
  c.send(3, 3, [](pod::Builder& b) {      // set_param Props { volume: 0.4, mute: false }
    b.id(2); b.int32(0);
    const uint64_t o = b.begin_object(0x40002, 2);
    b.property(0x10003); b.float32(0.4f);
    b.property(0x10004); b.boolean(false);
    b.end(o);
  });
  CHECK(mixer.master_volume() == 40);
  // the change comes back as a new info on the bound node
  server.tick();
  msgs = c.take();
  bool reinfo = false;
  for (const NativeMessage& m : msgs) reinfo |= m.id == 3 && m.opcode == 0;
  CHECK(reinfo);

  // a stream appears and goes: global, then global_remove (and the bound proxy's remove_id)
  const uint32_t client = g.add_client(Origin::Windows, 5, "tcos-winmm", "");
  Properties props(A);
  props.set("application.name", "game.exe");
  SharedPointer<PlaybackStream> s;
  const uint32_t node = g.create_playback(client, PcmFormat{}, props, &s);
  server.tick();
  msgs = c.take();
  int new_globals = 0;
  for (const NativeMessage& m : msgs) new_globals += m.id == 2 && m.opcode == 0;
  CHECK(new_globals == 1 + 1 + 2 + 2);      // client, node, ports, links
  c.send(2, 1, [&](pod::Builder& b) { b.int32(int32_t(node)); b.string("PipeWire:Interface:Node"); b.int32(3); b.int32(4); });
  c.take();
  g.remove_client(client);
  server.tick();
  msgs = c.take();
  int removed = 0;
  bool proxy_removed = false;
  for (const NativeMessage& m : msgs) {
    removed += m.id == 2 && m.opcode == 1;
    if (m.id == 0 && m.opcode == 4) { pod::Parser p = args_of(m); int32_t id = 0; proxy_removed |= p.int32(&id) && id == 4; }
  }
  CHECK(removed == 6 && proxy_removed);

  // bind errors, create_object, unknown objects, the metadata
  c.send(2, 1, [](pod::Builder& b) { b.int32(9999); b.string("PipeWire:Interface:Node"); b.int32(3); b.int32(10); });
  c.send(2, 1, [&](pod::Builder& b) { b.int32(int32_t(port_global)); b.string("PipeWire:Interface:Node"); b.int32(3); b.int32(11); });
  c.send(0, 6, [](pod::Builder& b) {
    b.string("client-node"); b.string("PipeWire:Interface:ClientNode"); b.int32(6);
    const uint64_t d = b.begin_struct(); b.int32(0); b.end(d);
    b.int32(12);
  });
  c.send(2, 1, [&](pod::Builder& b) { b.int32(int32_t(metadata_global)); b.string("PipeWire:Interface:Metadata"); b.int32(3); b.int32(13); });
  msgs = c.take();
  int errors = 0;
  bool default_sink = false;
  for (const NativeMessage& m : msgs) {
    errors += m.id == 0 && m.opcode == 3;
    if (m.id == 13 && m.opcode == 0) {
      pod::Parser p = args_of(m);
      int32_t subject = 0; StringView key, type, value;
      default_sink |= p.int32(&subject) && p.string(&key) && p.string(&type) && p.string(&value) && key == StringView("default.audio.sink") &&
                      value == StringView("{\"name\":\"tcos_output.stereo\"}");
    }
  }
  CHECK(errors == 3 && default_sink);

  // garbage: truncated and flipped messages never crash and never read out of bounds
  std::mt19937 rng(3);
  ByteBuffer valid(A);
  {
    pod::Builder b(valid);
    const uint64_t f = b.begin_struct();
    b.int32(int32_t(sink_global)); b.string("PipeWire:Interface:Node"); b.int32(3); b.int32(50);
    b.end(f);
  }
  for (int round = 0; round < 2000; ++round) {
    NativeClient bad(server);
    if (!bad.handle) break;
    std::vector<uint8_t> msg(16 + valid.size());
    const uint32_t opcode = rng() % 8, id = rng() % 4;
    for (int i = 0; i < 4; ++i) { msg[i] = uint8_t(id >> (8 * i)); msg[4 + i] = uint8_t((opcode << 24 | uint32_t(valid.size())) >> (8 * i)); }
    memcpy(msg.data() + 16, valid.view().data(), valid.size());
    for (int k = 0; k < 3; ++k) msg[16 + rng() % valid.size()] ^= uint8_t(1u << (rng() % 8));
    const size_t cut = rng() % (msg.size() + 1);
    server.receive(bad.handle, ConstByteView(msg.data(), cut));
    if (round % 3 == 0) {
      std::vector<uint8_t> noise(rng() % 200);
      for (uint8_t& x : noise) x = uint8_t(rng());
      server.receive(bad.handle, ConstByteView(noise.data(), noise.size()));
    }
    bad.take();
    server.close_client(bad.handle);
  }
  server.close_client(c.handle);
  CHECK(server.client_count() == 0);
}

// ---- the PulseAudio protocol ---------------------------------------------------------------------------------------------------------------------

struct PulsePacket {
  uint32_t channel;
  std::vector<uint8_t> payload;
};

struct PulseClient {
  PulseServer& server;
  uint32_t handle;
  uint32_t tag = 0;
  std::vector<uint8_t> in;
  PulseClient(PulseServer& s) : server(s), handle(s.open_client(55)) {}
  void packet(uint32_t channel, ConstByteView payload) {
    ByteBuffer d(A);
    const uint32_t words[5] = {uint32_t(payload.size()), channel, 0, 0, 0};
    for (uint32_t w : words) { const uint8_t b[4] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w)}; d.append(ConstByteView(b, 4)); }
    d.append(payload);
    server.receive(handle, d.view());
  }
  template <class F>
  uint32_t command(uint32_t cmd, F&& args) {
    ByteBuffer body(A);
    pulse::TagWriter w(body);
    w.u32(cmd);
    w.u32(++tag);
    args(w);
    packet(0xFFFFFFFFu, body.view());
    return tag;
  }
  std::vector<PulsePacket> take() {
    ByteBuffer out(A);
    server.take_output(handle, &out);
    in.insert(in.end(), out.view().data(), out.view().data() + out.size());
    std::vector<PulsePacket> r;
    size_t at = 0;
    while (in.size() - at >= 20) {
      ByteReader d(ConstByteView(in.data() + at, 20));
      const uint32_t length = d.be32(), channel = d.be32();
      if (in.size() - at - 20 < length) break;
      r.push_back(PulsePacket{channel, std::vector<uint8_t>(in.begin() + at + 20, in.begin() + at + 20 + length)});
      at += 20 + length;
    }
    in.erase(in.begin(), in.begin() + at);
    return r;
  }
};

// The reply to `tag` (its tagstruct after command and tag), or an empty optional-ish flag.
static bool find_reply(const std::vector<PulsePacket>& packets, uint32_t tag, std::vector<uint8_t>* body, uint32_t* command = nullptr) {
  for (const PulsePacket& p : packets) {
    if (p.channel != 0xFFFFFFFFu) continue;
    pulse::TagReader t(ConstByteView(p.payload.data(), p.payload.size()));
    uint32_t cmd = 0, got = 0;
    if (!t.u32(&cmd) || !t.u32(&got) || got != tag) continue;
    if (command) *command = cmd;
    body->assign(p.payload.begin() + 10, p.payload.end());
    return true;
  }
  return false;
}

static uint32_t count_command(const std::vector<PulsePacket>& packets, uint32_t command) {
  uint32_t n = 0;
  for (const PulsePacket& p : packets) {
    pulse::TagReader t(ConstByteView(p.payload.data(), p.payload.size()));
    uint32_t cmd = 0;
    if (p.channel == 0xFFFFFFFFu && t.u32(&cmd) && cmd == command) ++n;
  }
  return n;
}

static void put_create_playback(pulse::TagWriter& w, const pulse::SampleSpec& spec, uint32_t tlength, const char* name) {
  pulse::ChannelMap map; map.channels = spec.channels;
  for (uint8_t i = 0; i < spec.channels; ++i) map.positions[i] = uint8_t(spec.channels == 1 ? 0 : 1 + i);
  pulse::Cvolume vol; vol.channels = spec.channels;
  for (uint8_t i = 0; i < spec.channels; ++i) vol.values[i] = pulse::kVolumeNorm;
  w.sample_spec(spec); w.channel_map(map); w.u32(0xFFFFFFFFu); w.string(static_cast<const char*>(nullptr)); w.u32(0xFFFFFFFFu); w.boolean(false);
  w.u32(tlength); w.u32(0xFFFFFFFFu); w.u32(0xFFFFFFFFu); w.u32(0); w.cvolume(vol);
  for (int i = 0; i < 7; ++i) w.boolean(false);
  Properties props(A);
  props.set("media.name", name);
  w.boolean(false); w.boolean(false); w.proplist(props);      // muted, adjust latency, proplist
  w.boolean(true); w.boolean(false);                          // volume set, early requests
  w.boolean(false); w.boolean(false); w.boolean(false);       // muted set, don't inhibit, fail on suspend
  w.boolean(false); w.boolean(false);                         // relative volume, passthrough
  w.u8(0);                                                    // formats
}

static void test_pulse_protocol() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);
  CHECK(g.initialize());
  PulseServer server(g, A);
  PulseClient c(server);
  CHECK(c.handle != 0);
  // before AUTH only AUTH
  uint32_t early = c.command(20, [](pulse::TagWriter&) {});
  std::vector<uint8_t> body;
  uint32_t cmd = 99;
  CHECK(find_reply(c.take(), early, &body, &cmd) && cmd == 0);
  uint8_t cookie[256] = {};
  uint32_t t = c.command(8, [&](pulse::TagWriter& w) { w.u32(35 | 0x80000000u | 0x40000000u); w.arbitrary(ConstByteView(cookie, 256)); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t version = 0;
    CHECK(r.u32(&version) && version == 35);      // no SHM / memfd flag
  }
  t = c.command(9, [](pulse::TagWriter& w) { Properties p(A); p.set("application.name", "paplay"); w.proplist(p); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  // server info
  t = c.command(20, [](pulse::TagWriter&) {});
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    StringView name, version, user, host, sink, source;
    pulse::SampleSpec ss; uint32_t cookie_value = 0; pulse::ChannelMap map;
    CHECK(r.string(&name) && r.string(&version) && r.string(&user) && r.string(&host) && r.sample_spec(&ss) && r.string(&sink) && r.string(&source) &&
          r.u32(&cookie_value) && r.channel_map(&map) && r.at_end() && sink == StringView("tcos_output.stereo"));
  }
  // sink list: one sink, all fields there
  t = c.command(22, [](pulse::TagWriter&) {});
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t index = 0, module = 0, monitor = 0, flags = 0, base = 0, state = 0, steps = 0, card = 0, ports = 0; StringView name, desc, mname, driver, port;
    pulse::SampleSpec ss; pulse::ChannelMap map; pulse::Cvolume vol; bool mute; uint64_t lat = 0, conf = 0; Properties pl(A); uint8_t formats = 0, enc = 0;
    CHECK(r.u32(&index) && r.string(&name) && r.string(&desc) && r.sample_spec(&ss) && r.channel_map(&map) && r.u32(&module) && r.cvolume(&vol) && r.boolean(&mute) &&
          r.u32(&monitor) && r.string(&mname) && r.usec(&lat) && r.string(&driver) && r.u32(&flags) && r.proplist(&pl) && r.usec(&conf) && r.volume(&base) &&
          r.u32(&state) && r.u32(&steps) && r.u32(&card) && r.u32(&ports) && r.string(&port) && r.u8(&formats) && r.format_info(&enc, &pl) && r.at_end());
    CHECK(index == g.sink_id() && desc == StringView("Audio output") && formats == 1);
  }

  // a playback stream: S16 stereo at 44.1 kHz, 100 ms
  const pulse::SampleSpec spec{pulse::kS16Le, 2, 44100};
  const uint32_t tlength = 17640;
  t = c.command(3, [&](pulse::TagWriter& w) { put_create_playback(w, spec, tlength, "tone"); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  uint32_t channel = 0, node = 0, missing = 0, maxlength = 0, tl = 0, prebuf = 0, minreq = 0;
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    pulse::SampleSpec ss; pulse::ChannelMap map; uint32_t sink = 0; StringView sink_name; bool suspended = true; uint64_t latency = 0; uint8_t enc = 0; Properties fp(A);
    CHECK(r.u32(&channel) && r.u32(&node) && r.u32(&missing) && r.u32(&maxlength) && r.u32(&tl) && r.u32(&prebuf) && r.u32(&minreq) && r.sample_spec(&ss) &&
          r.channel_map(&map) && r.u32(&sink) && r.string(&sink_name) && r.boolean(&suspended) && r.usec(&latency) && r.format_info(&enc, &fp) && r.at_end());
    CHECK(missing == tlength && tl == tlength && !suspended && enc == 1 && sink == g.sink_id());
  }
  bool named = false;
  g.visit(node, [&](const GraphObject& o) { named = o.props.get("media.name") == StringView("tone") && o.props.get("application.name") == StringView("paplay") && o.origin == Origin::Linux; });
  CHECK(named);
  // the audio: 1500 everywhere, as much as was asked for
  std::vector<uint8_t> tone = s16_constant(tlength / 4, 2, 1500);
  c.packet(channel, ConstByteView(tone.data(), tone.size()));
  dev.take();
  mixer.pump();
  std::vector<int16_t> out = dev.take();
  CHECK(!out.empty() && abs(out[out.size() / 2] - 1500) <= 2);
  // as it plays, the server asks for more
  uint32_t requests = 0;
  for (int i = 0; i < 50; ++i) {
    dev.play_all();
    mixer.pump();
    server.tick();
    requests += count_command(c.take(), 61);
  }
  CHECK(requests >= 1);
  // a native stream plays at the same time: the sink-input list shows both kinds
  SharedPointer<audio::AudioStream> native = mixer.open_stream(audio::StreamFormat(), 8192);
  native->set_label("Web Audio");
  server.tick();
  t = c.command(30, [](pulse::TagWriter&) {});
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    int inputs = 0;
    bool saw_native = false;
    while (!r.at_end() && r.ok()) {
      uint32_t index = 0, module = 0, client = 0, sink = 0; StringView name, resample, driver; pulse::SampleSpec ss; pulse::ChannelMap map; pulse::Cvolume vol;
      uint64_t l1 = 0, l2 = 0; bool mute = false, corked = false, has = false, writable = false; Properties pl(A); uint8_t enc = 0;
      if (!(r.u32(&index) && r.string(&name) && r.u32(&module) && r.u32(&client) && r.u32(&sink) && r.sample_spec(&ss) && r.channel_map(&map) && r.cvolume(&vol) &&
            r.usec(&l1) && r.usec(&l2) && r.string(&resample) && r.string(&driver) && r.boolean(&mute) && r.proplist(&pl) && r.boolean(&corked) && r.boolean(&has) &&
            r.boolean(&writable) && r.format_info(&enc, &pl)))
        break;
      ++inputs;
      saw_native |= driver == StringView("tcos-native") && name == StringView("Web Audio");
      CHECK(vol.channels == ss.channels && map.channels == ss.channels);
    }
    CHECK(inputs == 2 && saw_native && r.at_end());
  }
  // volume of the stream (cubic): 0.5 -> 0.125 linear
  t = c.command(37, [&](pulse::TagWriter& w) { w.u32(node); pulse::Cvolume v; v.channels = 2; v.values[0] = v.values[1] = 0x8000; w.cvolume(v); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  float lin = 0; bool mute = true;
  CHECK(g.get_volume(node, &lin, &mute) && lin > 0.12f && lin < 0.13f && !mute);
  // cork, latency, flush, subscribe, drain
  t = c.command(41, [&](pulse::TagWriter& w) { w.u32(channel); w.boolean(true); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2 && g.playback_of(node)->paused());
  t = c.command(41, [&](pulse::TagWriter& w) { w.u32(channel); w.boolean(false); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2 && !g.playback_of(node)->paused());
  t = c.command(14, [&](pulse::TagWriter& w) { w.u32(channel); w.timeval(1, 2); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint64_t sink_us = 0, source_us = 0, under = 0, playing = 0; bool is_playing; uint32_t a1, a2, b1, b2; int64_t wi = 0, ri = 0;
    CHECK(r.usec(&sink_us) && r.usec(&source_us) && r.boolean(&is_playing) && r.timeval(&a1, &a2) && r.timeval(&b1, &b2) && r.s64(&wi) && r.s64(&ri) && r.u64(&under) &&
          r.u64(&playing) && r.at_end() && wi == int64_t(tone.size()) && ri <= wi);
  }
  t = c.command(35, [](pulse::TagWriter& w) { w.u32(0x4 | 0x20); });      // sink inputs and clients
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  native->close();
  drain_mixer(mixer, dev, 50);
  server.tick();
  CHECK(count_command(c.take(), 66) >= 1);      // the native stream's removal (and other changes)
  const uint32_t drain = c.command(12, [&](pulse::TagWriter& w) { w.u32(channel); });
  bool drained = false;
  for (int i = 0; i < 400 && !drained; ++i) {
    dev.play_all();
    mixer.pump();
    server.tick();
    drained = find_reply(c.take(), drain, &body, &cmd) && cmd == 2;
  }
  CHECK(drained);
  // kill-sink-input from another client: the owner is told
  PulseClient other(server);
  t = other.command(8, [&](pulse::TagWriter& w) { w.u32(32); w.arbitrary(ConstByteView(cookie, 256)); });
  other.take();
  t = other.command(49, [&](pulse::TagWriter& w) { w.u32(node); });
  CHECK(find_reply(other.take(), t, &body, &cmd) && cmd == 2);
  server.tick();
  CHECK(count_command(c.take(), 64) == 1 && g.playback_count() == 0);
  // errors: unknown command, bad stream, unsupported format, record streams
  t = c.command(200, [](pulse::TagWriter&) {});
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0);
  t = c.command(4, [](pulse::TagWriter& w) { w.u32(77); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0);
  t = c.command(3, [&](pulse::TagWriter& w) { put_create_playback(w, pulse::SampleSpec{pulse::kS24_32Be, 2, 48000}, 4096, "x"); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0);
  t = c.command(5, [](pulse::TagWriter&) {});
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0);
  // a float stream and a delete
  t = c.command(3, [&](pulse::TagWriter& w) { put_create_playback(w, pulse::SampleSpec{pulse::kFloat32Le, 1, 48000}, 0xFFFFFFFFu, "f"); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  pulse::TagReader r(ConstByteView(body.data(), body.size()));
  uint32_t ch2 = 0;
  CHECK(r.u32(&ch2) && g.playback_count() == 1);
  t = c.command(4, [&](pulse::TagWriter& w) { w.u32(ch2); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2 && g.playback_count() == 0);
  // a version 13 client (fewer fields)
  PulseClient old(server);
  t = old.command(8, [&](pulse::TagWriter& w) { w.u32(13); w.arbitrary(ConstByteView(cookie, 256)); });
  CHECK(find_reply(old.take(), t, &body, &cmd) && cmd == 2);
  t = old.command(22, [](pulse::TagWriter&) {});
  CHECK(find_reply(old.take(), t, &body, &cmd) && cmd == 2);
  // garbage
  std::mt19937 rng(5);
  for (int round = 0; round < 1500; ++round) {
    PulseClient bad(server);
    if (!bad.handle) break;
    ByteBuffer good(A);
    pulse::TagWriter w(good);
    w.u32(round % 2 ? 8 : 3); w.u32(1);
    if (round % 2) { w.u32(35); w.arbitrary(ConstByteView(cookie, 16)); }
    else put_create_playback(w, spec, 4096, "fuzz");
    std::vector<uint8_t> m(good.view().data(), good.view().data() + good.size());
    for (int k = 0; k < 2; ++k) m[rng() % m.size()] ^= uint8_t(1u << (rng() % 8));
    m.resize(rng() % (m.size() + 1));
    if (round % 4 == 1) bad.command(8, [&](pulse::TagWriter& x) { x.u32(35); x.arbitrary(ConstByteView(cookie, 16)); });
    bad.packet(round % 5 == 0 ? rng() % 4 : 0xFFFFFFFFu, ConstByteView(m.data(), m.size()));
    std::vector<uint8_t> noise(rng() % 64);
    for (uint8_t& x : noise) x = uint8_t(rng());
    server.receive(bad.handle, ConstByteView(noise.data(), noise.size()));
    server.tick();
    bad.take();
    server.close_client(bad.handle);
  }
  server.close_client(c.handle);
  server.close_client(other.handle);
  server.close_client(old.handle);
  CHECK(server.client_count() == 0 && g.playback_count() == 0);
}

// ---- recording -----------------------------------------------------------------------------------------------------------------------------------

// A microphone that hands out what the test puts in (48 kHz 16-bit stereo, like the hardware).
struct FakeMicrophone final : hardware::IAudioCapture {
  std::mutex m;
  std::vector<uint8_t> pending;
  uint8_t level = hardware::kAudioMaxVolume;
  bool mute = false;
  const char* name() const override { return "Fake microphone"; }
  void add(uint32_t frames, int16_t left, int16_t right) {
    std::lock_guard<std::mutex> g(m);
    for (uint32_t i = 0; i < frames; ++i) {
      const int16_t v[2] = {left, right};
      for (int16_t x : v) { pending.push_back(uint8_t(x)); pending.push_back(uint8_t(uint16_t(x) >> 8)); }
    }
  }
  uint32_t read(ByteView out) override {
    std::lock_guard<std::mutex> g(m);
    uint32_t n = out.size() < pending.size() ? uint32_t(out.size()) : uint32_t(pending.size());
    n -= n % kAudioFrameSize;
    if (n) memcpy(out.data(), pending.data(), n);
    pending.erase(pending.begin(), pending.begin() + n);
    return n;
  }
  uint32_t available_bytes() override { std::lock_guard<std::mutex> g(m); return uint32_t(pending.size()); }
  void flush() override { std::lock_guard<std::mutex> g(m); pending.clear(); }
  void set_gain(uint8_t g) override { level = g; }
  uint8_t gain() const override { return level; }
  void set_muted(bool v) override { mute = v; }
  bool muted() const override { return mute; }
  hardware::AudioCaptureStatistics statistics() override { return {}; }
};

static int16_t s16_at(const std::vector<uint8_t>& b, size_t sample) { return int16_t(b[2 * sample] | b[2 * sample + 1] << 8); }

static void test_record_convert() {
  // 16-bit stereo into every format, and back through the playback side's decoder: the same values (G.711 within its step)
  static constexpr SampleFormat kAll[] = {SampleFormat::U8, SampleFormat::S16Le, SampleFormat::S16Be, SampleFormat::S24Le, SampleFormat::S24Be,
                                          SampleFormat::S24_32Le, SampleFormat::S32Le, SampleFormat::S32Be, SampleFormat::F32Le, SampleFormat::F32Be,
                                          SampleFormat::F64Le, SampleFormat::Alaw, SampleFormat::Ulaw};
  static constexpr int16_t kValues[] = {0, 1, -1, 100, -100, 1000, -1000, 12345, -12345, 32767, -32768, 255, -256};
  for (SampleFormat f : kAll) {
    const PcmFormat format{f, 48000, 2};
    std::vector<uint8_t> in;
    for (int16_t v : kValues) for (int16_t x : {v, int16_t(-v / 2)}) { in.push_back(uint8_t(x)); in.push_back(uint8_t(uint16_t(x) >> 8)); }
    const uint32_t frames = uint32_t(in.size() / 4);
    std::vector<uint8_t> mid(frames * format.frame_size()), back(frames * 4);
    uint32_t used = 0;
    CHECK(convert_from_s16(ConstByteView(in.data(), in.size()), 2, format, nullptr, ByteView(mid.data(), mid.size()), &used) == mid.size() && used == in.size());
    CHECK(convert_to_s16(format, nullptr, ConstByteView(mid.data(), mid.size()), ByteView(back.data(), back.size()), 2, &used) == back.size());
    int worst = 0;
    for (size_t i = 0; i < in.size() / 2; ++i) {
      const int a = s16_at(in, i), b = s16_at(back, i);
      const int tolerance = f == SampleFormat::U8 ? 256 : (f == SampleFormat::Alaw || f == SampleFormat::Ulaw) ? (abs(a) / 16 + 16) : 1;
      if (abs(a - b) > tolerance) worst = abs(a - b) > worst ? abs(a - b) : worst;
    }
    if (worst) printf("  format %s: off by %d\n", format.name(), worst);
    CHECK(worst == 0);
  }
  // six channels from stereo: each takes its side, the centre both, LFE nothing; mono gets the average
  std::vector<uint8_t> lr = {0xE8, 0x03, 0x18, 0xFC};      // left 1000, right -1000... and a second frame below
  lr.insert(lr.end(), {0xD0, 0x07, 0x00, 0x00});          // left 2000, right 0
  std::vector<uint8_t> six(2 * 6 * 2);
  uint32_t used = 0;
  CHECK(convert_from_s16(ConstByteView(lr.data(), lr.size()), 2, PcmFormat{SampleFormat::S16Le, 48000, 6}, nullptr, ByteView(six.data(), six.size()), &used) == six.size());
  // FL FR FC LFE RL RR
  CHECK(s16_at(six, 0) == 1000 && s16_at(six, 1) == -1000 && s16_at(six, 2) == 0 && s16_at(six, 3) == 0 && s16_at(six, 4) == 1000 && s16_at(six, 5) == -1000);
  CHECK(s16_at(six, 6) == 2000 && s16_at(six, 7) == 0 && s16_at(six, 8) == 1000 && s16_at(six, 9) == 0 && s16_at(six, 10) == 2000);
  std::vector<uint8_t> mono(4);
  CHECK(convert_from_s16(ConstByteView(lr.data(), lr.size()), 2, PcmFormat{SampleFormat::S16Le, 48000, 1}, nullptr, ByteView(mono.data(), mono.size()), &used) == 4);
  CHECK(s16_at(mono, 0) == 0 && s16_at(mono, 1) == 1000);
  // mono input into stereo: both sides
  std::vector<uint8_t> st(8);
  CHECK(convert_from_s16(ConstByteView(lr.data(), 4), 1, PcmFormat{SampleFormat::S16Le, 48000, 2}, nullptr, ByteView(st.data(), st.size()), &used) == 8 && used == 4);
  CHECK(s16_at(st, 0) == 1000 && s16_at(st, 1) == 1000 && s16_at(st, 2) == -1000 && s16_at(st, 3) == -1000);
  // truncation: partial frames are left, a target too small takes what fits, bad arguments do nothing
  for (size_t cut = 0; cut <= lr.size(); ++cut) {
    std::vector<uint8_t> out(24, 0xAA);
    const uint32_t made = convert_from_s16(ConstByteView(lr.data(), cut), 2, PcmFormat{SampleFormat::F32Le, 48000, 3}, nullptr, ByteView(out.data(), out.size()), &used);
    CHECK(used == cut / 4 * 4 && made == cut / 4 * 12);
    for (size_t room = 0; room < 24; ++room) {
      std::vector<uint8_t> small(room ? room : 1, 0xAA);
      const uint32_t m2 = convert_from_s16(ConstByteView(lr.data(), cut), 2, PcmFormat{SampleFormat::F32Le, 48000, 3}, nullptr, ByteView(small.data(), room), &used);
      CHECK(m2 <= room && m2 % 12 == 0 && used == m2 / 12 * 4);
    }
  }
  CHECK(convert_from_s16(ConstByteView(lr.data(), lr.size()), 3, PcmFormat{}, nullptr, ByteView(six.data(), six.size()), &used) == 0 && used == 0);
  CHECK(convert_from_s16(ConstByteView(lr.data(), lr.size()), 2, PcmFormat{SampleFormat::S16Le, 10, 2}, nullptr, ByteView(six.data(), six.size()), &used) == 0);
}

static void put_create_record(pulse::TagWriter& w, const pulse::SampleSpec& spec, uint32_t fragsize, const char* name, bool corked = false) {
  pulse::ChannelMap map; map.channels = spec.channels;
  for (uint8_t i = 0; i < spec.channels; ++i) map.positions[i] = uint8_t(spec.channels == 1 ? 0 : 1 + i);
  pulse::Cvolume vol; vol.channels = spec.channels;
  for (uint8_t i = 0; i < spec.channels; ++i) vol.values[i] = pulse::kVolumeNorm;
  w.sample_spec(spec); w.channel_map(map); w.u32(0xFFFFFFFFu); w.string(static_cast<const char*>(nullptr)); w.u32(0xFFFFFFFFu); w.boolean(corked);
  w.u32(fragsize);
  for (int i = 0; i < 7; ++i) w.boolean(false);
  Properties props(A);
  props.set("media.name", name);
  w.boolean(false); w.boolean(false); w.proplist(props); w.u32(0xFFFFFFFFu);      // peak detect, adjust latency, proplist, direct on input
  w.boolean(false);                                                             // early requests
  w.boolean(false); w.boolean(false);                                           // don't inhibit, fail on suspend
  w.u8(0);                                                                      // formats
  w.cvolume(vol); w.boolean(false); w.boolean(false); w.boolean(false); w.boolean(false); w.boolean(false);      // volume, muted, volume set, muted set, relative, passthrough
}

static void test_record() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  FakeMicrophone mic;
  audio::AudioCaptureHub hub(mic, A);
  CHECK(hub.initialize());
  Graph g(A, &mixer, &hub);
  CHECK(g.initialize());
  CHECK(g.source_id() != 0 && g.has_input());
  // the source: an Audio/Source node with two output ports, the default source
  bool source_ok = false, metadata_ok = false;
  g.visit(g.source_id(), [&](const GraphObject& o) {
    source_ok = o.role == NodeRole::Source && o.output_ports == 2 && o.props.get("media.class") == StringView("Audio/Source") && o.props.get("node.name") == StringView("tcos_input.stereo");
  });
  g.visit(g.metadata_id(), [&](const GraphObject& o) { metadata_ok = o.props.get("default.audio.source").find("tcos_input.stereo") != StringView::npos; });
  CHECK(source_ok && metadata_ok);
  g.refresh();
  g.visit(g.source_id(), [&](const GraphObject& o) { source_ok = o.props.get("node.description") == StringView("Fake microphone") && o.state == kNodeStateIdle; });
  CHECK(source_ok);
  // without a hub there is no source, and no record stream can be made
  {
    Graph none(A, &mixer);
    CHECK(none.initialize() && none.source_id() == 0 && !none.has_input());
    const uint32_t client = none.add_client(Origin::Linux, 1, "protocol-pulse", "");
    SharedPointer<RecordStream> r;
    CHECK(none.create_record(client, PcmFormat{}, Properties(A), &r) == 0 && !r);
  }

  // a record stream: F32 mono at 16 kHz, linked from both sides of the source
  const uint32_t client = g.add_client(Origin::Linux, 77, "protocol-pulse", "pipewire-pulse");
  SharedPointer<RecordStream> rec;
  Properties props(A);
  props.set("application.name", "parecord");
  const uint32_t node = g.create_record(client, PcmFormat{SampleFormat::F32Le, 16000, 1}, props, &rec);
  CHECK(node && rec && g.record_of(node) == rec && !g.playback_of(node) && g.record_count() == 1 && g.playback_count() == 0);
  uint32_t links = 0, ports = 0;
  g.for_each([&](const GraphObject& o) {
    if (o.type == ObjectType::Link && o.input_node == node && o.output_node == g.source_id()) ++links;
    if (o.type == ObjectType::Port && o.node == node && o.direction == kDirectionInput) ++ports;
  });
  bool node_ok = false;
  g.visit(node, [&](const GraphObject& o) { node_ok = o.role == NodeRole::Capture && o.props.get("media.class") == StringView("Stream/Input/Audio") && o.input_ports == 1; });
  CHECK(links == 2 && ports == 1 && node_ok);
  // 100 ms at 48 kHz, left 8000 and right 4000: mono at 16 kHz is 1600 frames of their average (6000)
  mic.add(4800, 8000, 4000);
  hub.pump();
  CHECK(rec->available_bytes() >= 1500 * 4 && rec->available_bytes() <= 1700 * 4);
  std::vector<uint8_t> got(16000 * 4);
  uint32_t n = rec->read(ByteView(got.data(), got.size()));
  CHECK(n % 4 == 0 && n >= 1500 * 4 && rec->read_bytes() == n && rec->available_bytes() == 0);
  float mid = 0;
  memcpy(&mid, got.data() + 800 * 4, 4);
  CHECK(fabsf(mid * 32768.0f - 6000.0f) < 20.0f);
  // a stream's volume and mute are its gain; the source's are the hub's master gain, which the microphone gets
  CHECK(g.set_volume(node, 0.5f, false));
  mic.add(4800, 8000, 4000);
  hub.pump();
  n = rec->read(ByteView(got.data(), got.size()));
  memcpy(&mid, got.data() + 800 * 4, 4);
  CHECK(n > 0 && fabsf(mid * 32768.0f - 3000.0f) < 60.0f);
  float volume = 0; bool muted = true;
  CHECK(g.set_volume(node, 1.4f, false) && g.get_volume(node, &volume, &muted) && volume == 1.0f && !muted);      // (no gain above 1)
  CHECK(g.set_volume(g.source_id(), 0.25f, true) && mic.level == uint8_t(0.25f * hardware::kAudioMaxVolume + 0.5f) && mic.mute && hub.master_muted());
  CHECK(g.set_volume(g.source_id(), 1.0f, false) && mic.level == hardware::kAudioMaxVolume && !mic.mute);
  // paused: hears nothing, and what came meanwhile is not handed out after
  CHECK(g.set_paused(node, true) && rec->paused());
  mic.add(4800, 1000, 1000);
  hub.pump();
  CHECK(rec->available_bytes() == 0 && rec->read(ByteView(got.data(), got.size())) == 0);
  g.refresh();
  bool idle = false;
  g.visit(node, [&](const GraphObject& o) { idle = o.state == kNodeStateIdle; });
  CHECK(idle);
  CHECK(g.set_paused(node, false) && !rec->paused() && rec->available_bytes() == 0);
  mic.add(480, 1000, 1000);
  hub.pump();
  CHECK(rec->available_bytes() > 0);
  rec->flush();
  CHECK(rec->available_bytes() == 0);
  // the client goes: its record node and the hub's stream go too
  g.remove_client(client);
  hub.pump();
  CHECK(g.record_count() == 0 && rec->closed() && hub.stream_count() == 0);
  // the microphone is unplugged: the source is suspended, a stream still opens (and hears nothing)
  hub.set_source(nullptr, SharedPointer<hardware::IAudioDevice>());
  g.refresh();
  bool suspended = false;
  g.visit(g.source_id(), [&](const GraphObject& o) { suspended = o.state == kNodeStateSuspended && o.props.get("node.description") == StringView("Audio input"); });
  CHECK(suspended && !g.has_input());
  hub.set_source(&mic, SharedPointer<hardware::IAudioDevice>());
  g.refresh();      // (the protocols' tick does this)

  // ---- PulseAudio: parecord's commands
  PulseServer server(g, A);
  PulseClient c(server);
  uint8_t cookie[256] = {};
  std::vector<uint8_t> body;
  uint32_t cmd = 0;
  uint32_t t = c.command(8, [&](pulse::TagWriter& w) { w.u32(35); w.arbitrary(ConstByteView(cookie, 256)); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  t = c.command(9, [](pulse::TagWriter& w) { Properties p(A); p.set("application.name", "parecord"); w.proplist(p); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  t = c.command(35, [](pulse::TagWriter& w) { w.u32(0x2 | 0x8); });      // subscribe: sources, source outputs
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  // server info names the default source
  t = c.command(20, [](pulse::TagWriter&) {});
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    StringView name, version, user, host, sink, source;
    pulse::SampleSpec ss;
    CHECK(r.string(&name) && r.string(&version) && r.string(&user) && r.string(&host) && r.sample_spec(&ss) && r.string(&sink) && r.string(&source) &&
          source == StringView("tcos_input.stereo"));
  }
  // the source list and lookups
  t = c.command(24, [](pulse::TagWriter&) {});
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t index = 0, module = 0, monitor = 0, flags = 0, base = 0, state = 0, steps = 0, card = 0, nports = 0; StringView name, desc, mname, driver, port;
    pulse::SampleSpec ss; pulse::ChannelMap map; pulse::Cvolume vol; bool mute; uint64_t lat = 0, conf = 0; Properties pl(A); uint8_t formats = 0, enc = 0;
    CHECK(r.u32(&index) && r.string(&name) && r.string(&desc) && r.sample_spec(&ss) && r.channel_map(&map) && r.u32(&module) && r.cvolume(&vol) && r.boolean(&mute) &&
          r.u32(&monitor) && r.string(&mname) && r.usec(&lat) && r.string(&driver) && r.u32(&flags) && r.proplist(&pl) && r.usec(&conf) && r.volume(&base) &&
          r.u32(&state) && r.u32(&steps) && r.u32(&card) && r.u32(&nports) && r.string(&port) && r.u8(&formats) && r.format_info(&enc, &pl));
    uint32_t second = 0;      // (and the sink's monitor after it)
    CHECK(r.u32(&second) && second == (g.sink_id() | (1u << 24)));
    CHECK(index == g.source_id() && name == StringView("tcos_input.stereo") && desc == StringView("Fake microphone") && monitor == pulse::kInvalidIndex);
  }
  t = c.command(11, [](pulse::TagWriter& w) { w.string("@DEFAULT_SOURCE@"); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t index = 0;
    CHECK(r.u32(&index) && index == g.source_id());
  }
  t = c.command(11, [](pulse::TagWriter& w) { w.string("tcos_output.stereo.monitor"); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);      // (the sink's monitor)
  t = c.command(11, [](pulse::TagWriter& w) { w.string("no.such.source"); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0);
  t = c.command(23, [&](pulse::TagWriter& w) { w.u32(0xFFFFFFFFu); w.string("tcos_input.stereo"); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  // a record stream: S16 stereo at 44.1 kHz, 20 ms fragments
  const pulse::SampleSpec spec{pulse::kS16Le, 2, 44100};
  t = c.command(5, [&](pulse::TagWriter& w) { put_create_record(w, spec, 3528, "voice"); });
  std::vector<PulsePacket> packets = c.take();
  CHECK(find_reply(packets, t, &body, &cmd) && cmd == 2);
  uint32_t channel = 0, index = 0, maxlength = 0, fragsize = 0;
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    pulse::SampleSpec ss; pulse::ChannelMap map; uint32_t source = 0; StringView source_name; bool susp = true; uint64_t latency = 0; uint8_t enc = 0; Properties fp(A);
    CHECK(r.u32(&channel) && r.u32(&index) && r.u32(&maxlength) && r.u32(&fragsize) && r.sample_spec(&ss) && r.channel_map(&map) && r.u32(&source) &&
          r.string(&source_name) && r.boolean(&susp) && r.usec(&latency) && r.format_info(&enc, &fp) && r.at_end());
    CHECK(fragsize == 3528 && maxlength >= fragsize && source == g.source_id() && !susp && enc == 1 && g.record_of(index));
  }
  server.tick();
  packets = c.take();
  CHECK(count_command(packets, 66) >= 1);      // subscription event: a new source output
  // 200 ms of microphone: the client gets about 8820 frames in fragments on its channel, the values the microphone heard
  mic.add(9600, 3000, -3000);
  size_t audio_bytes = 0, fragments = 0;
  bool values_ok = true;
  for (int i = 0; i < 20; ++i) {
    hub.pump();
    server.tick();
    for (const PulsePacket& p : c.take()) {
      if (p.channel != channel) continue;
      ++fragments;
      audio_bytes += p.payload.size();
      if (p.payload.size() != 3528) values_ok = false;
      for (size_t k = 4 * 100; k + 3 < p.payload.size() && k < 4 * 200; k += 4)
        if (abs(s16_at(p.payload, k / 2) - 3000) > 40 || abs(s16_at(p.payload, k / 2 + 1) + 3000) > 40) values_ok = false;
    }
  }
  CHECK(fragments >= 4 && audio_bytes >= 8000 * 4 && audio_bytes <= 8900 * 4 && values_ok);
  // latency: the indices count what was sent
  t = c.command(57, [&](pulse::TagWriter& w) { w.u32(channel); w.timeval(1, 2); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint64_t monitor = 0, source = 0; bool running = false; uint32_t s1, u1, s2, u2; int64_t wi = 0, ri = 0;
    CHECK(r.usec(&monitor) && r.usec(&source) && r.boolean(&running) && r.timeval(&s1, &u1) && r.timeval(&s2, &u2) && r.s64(&wi) && r.s64(&ri) && r.at_end());
    CHECK(running && ri == int64_t(audio_bytes) && wi >= ri);
  }
  // buffer attributes
  t = c.command(73, [&](pulse::TagWriter& w) { w.u32(channel); w.u32(0xFFFFFFFFu); w.u32(10); w.boolean(false); w.boolean(false); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t ml = 0, fs = 0; uint64_t lat = 0;
    CHECK(r.u32(&ml) && r.u32(&fs) && r.usec(&lat) && r.at_end() && fs == 220 * 4);      // (at least 5 ms)
  }
  // source outputs: the stream, with the client's name; its volume and mute (commands 98, 99)
  t = c.command(32, [](pulse::TagWriter&) {});
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t idx = 0, module = 0, owner = 0, source = 0; StringView name, method, driver; pulse::SampleSpec ss; pulse::ChannelMap map; uint64_t l1, l2;
    Properties pl(A); bool corked = true, mute = true, has = false, writable = false; pulse::Cvolume vol; uint8_t enc = 0;
    CHECK(r.u32(&idx) && r.string(&name) && r.u32(&module) && r.u32(&owner) && r.u32(&source) && r.sample_spec(&ss) && r.channel_map(&map) && r.usec(&l1) &&
          r.usec(&l2) && r.string(&method) && r.string(&driver) && r.proplist(&pl) && r.boolean(&corked) && r.cvolume(&vol) && r.boolean(&mute) &&
          r.boolean(&has) && r.boolean(&writable) && r.format_info(&enc, &pl) && r.at_end());
    CHECK(idx == index && name == StringView("voice") && source == g.source_id() && !corked && !mute && has && writable && ss.rate == 44100);
  }
  pulse::Cvolume half; half.channels = 2; half.values[0] = half.values[1] = pulse::kVolumeNorm / 2;
  t = c.command(98, [&](pulse::TagWriter& w) { w.u32(index); w.cvolume(half); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2 && g.get_volume(index, &volume, &muted) && fabsf(volume - 0.125f) < 0.01f);
  t = c.command(99, [&](pulse::TagWriter& w) { w.u32(index); w.boolean(true); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2 && g.get_volume(index, &volume, &muted) && muted);
  t = c.command(98, [&](pulse::TagWriter& w) { w.u32(g.sink_id()); w.cvolume(half); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0);      // (the sink is no source output)
  t = c.command(37, [&](pulse::TagWriter& w) { w.u32(index); w.cvolume(half); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0);      // (nor is a source output a sink input)
  // the source's volume and mute: the hub's master gain
  t = c.command(38, [&](pulse::TagWriter& w) { w.u32(g.source_id()); w.string(static_cast<const char*>(nullptr)); w.cvolume(half); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2 && hub.master_gain() < hardware::kAudioMaxVolume / 4);
  t = c.command(40, [&](pulse::TagWriter& w) { w.u32(0xFFFFFFFFu); w.string("@DEFAULT_SOURCE@"); w.boolean(true); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2 && hub.master_muted());
  t = c.command(40, [&](pulse::TagWriter& w) { w.u32(0xFFFFFFFFu); w.string("tcos_input.stereo"); w.boolean(false); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2 && !hub.master_muted());
  // cork: nothing is sent while corked, and nothing old after
  t = c.command(58, [&](pulse::TagWriter& w) { w.u32(channel); w.boolean(true); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  mic.add(4800, 100, 100);
  hub.pump();
  server.tick();
  size_t while_corked = 0;
  for (const PulsePacket& p : c.take()) if (p.channel == channel) while_corked += p.payload.size();
  CHECK(while_corked == 0);
  t = c.command(58, [&](pulse::TagWriter& w) { w.u32(channel); w.boolean(false); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  hub.pump();
  server.tick();
  for (const PulsePacket& p : c.take()) if (p.channel == channel) while_corked += p.payload.size();
  CHECK(while_corked == 0);
  t = c.command(59, [&](pulse::TagWriter& w) { w.u32(channel); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  t = c.command(47, [&](pulse::TagWriter& w) { w.u32(channel); w.string("call"); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  bool renamed = false;
  g.visit(index, [&](const GraphObject& o) { renamed = o.props.get("media.name") == StringView("call"); });
  CHECK(renamed);
  // playback commands do not reach a record stream, and the other way round
  t = c.command(4, [&](pulse::TagWriter& w) { w.u32(channel); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0 && g.record_of(index));
  // killed from elsewhere (pactl kill-source-output): RECORD_STREAM_KILLED
  PulseClient other(server);
  t = other.command(8, [&](pulse::TagWriter& w) { w.u32(35); w.arbitrary(ConstByteView(cookie, 256)); });
  other.take();
  t = other.command(50, [&](pulse::TagWriter& w) { w.u32(index); });
  CHECK(find_reply(other.take(), t, &body, &cmd) && cmd == 2 && !g.record_of(index));
  server.tick();
  CHECK(count_command(c.take(), 65) == 1);
  // a second stream, then deleted by its client; a mono one too
  t = c.command(5, [&](pulse::TagWriter& w) { put_create_record(w, pulse::SampleSpec{pulse::kFloat32Le, 1, 8000}, 0xFFFFFFFFu, "mono", true); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    CHECK(r.u32(&channel) && r.u32(&index) && r.u32(&maxlength) && r.u32(&fragsize) && fragsize == 400 * 4);      // 50 ms by default
  }
  bool corked_node = false;
  g.visit(index, [&](const GraphObject& o) { corked_node = o.record && o.record->paused(); });
  CHECK(corked_node);
  t = c.command(6, [&](pulse::TagWriter& w) { w.u32(channel); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2 && g.record_count() == 0);
  // bad requests: an unknown source, a channel map that does not fit, direct-on-input
  t = c.command(5, [&](pulse::TagWriter& w) {
    pulse::ChannelMap map; map.channels = 2; map.positions[0] = 1; map.positions[1] = 2;
    w.sample_spec(spec); w.channel_map(map); w.u32(12345); w.string(static_cast<const char*>(nullptr)); w.u32(0xFFFFFFFFu); w.boolean(false); w.u32(0xFFFFFFFFu);
  });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0);
  // garbage: every truncation of a CREATE_RECORD_STREAM and bit flips in it
  ByteBuffer good(A);
  {
    pulse::TagWriter w(good);
    w.u32(5); w.u32(1);
    put_create_record(w, spec, 3528, "fuzz");
  }
  std::mt19937 rng(17);
  for (size_t round = 0; round < good.size() + 600; ++round) {
    PulseClient bad(server);
    if (!bad.handle) break;
    bad.command(8, [&](pulse::TagWriter& x) { x.u32(35); x.arbitrary(ConstByteView(cookie, 16)); });
    std::vector<uint8_t> m(good.view().data(), good.view().data() + good.size());
    if (round < good.size()) m.resize(round);
    else for (int k = 0; k < 3; ++k) m[rng() % m.size()] ^= uint8_t(1u << (rng() % 8));
    bad.packet(0xFFFFFFFFu, ConstByteView(m.data(), m.size()));
    mic.add(480, 1, 1);
    hub.pump();
    server.tick();
    bad.take();
    server.close_client(bad.handle);
  }
  server.close_client(c.handle);
  server.close_client(other.handle);
  hub.pump();
  CHECK(server.client_count() == 0 && g.record_count() == 0 && hub.stream_count() == 0);

  // ---- Windows: the service's capture side
  WindowsAudioService service(g, A);
  uint32_t stream = 0, read = 0;
  CHECK(service.open_capture(20, PcmFormat{SampleFormat::S16Le, 48000, 2}, "recorder.exe", &stream) == 0 && stream && g.record_of(stream));
  bool windows = false;
  g.visit(stream, [&](const GraphObject& o) { windows = o.origin == Origin::Windows && o.role == NodeRole::Capture; });
  CHECK(windows && service.stream_count() == 1);
  mic.add(960, 1234, -1234);
  hub.pump();
  std::vector<uint8_t> buffer(10000);
  CHECK(service.read(21, stream, ByteView(buffer.data(), buffer.size()), &read) != 0);      // not its stream
  CHECK(service.read(20, stream, ByteView(buffer.data(), buffer.size()), &read) == 0 && read == 960 * 4 && s16_at(buffer, 100) == 1234 && s16_at(buffer, 101) == -1234);
  uint32_t taken = 0;
  CHECK(service.write(20, stream, ConstByteView(buffer.data(), 64), &taken) != 0);      // (a capture stream takes no writes)
  // its volume: no pan, the louder side
  uint32_t left = 0, right = 0;
  CHECK(service.set_volume(20, stream, 0x8000, 0x2000) == 0 && service.get_volume(20, stream, &left, &right, &muted) == 0 && left == right && left > 0x7E00 && left < 0x8200);
  // the microphone's master gain (stream 0xFFFFFFFF)
  CHECK(service.set_volume(20, WindowsAudioService::kSourceStream, 0x8000, 0x8000) == 0 && hub.master_gain() > hardware::kAudioMaxVolume / 3 && hub.master_gain() < hardware::kAudioMaxVolume * 2 / 3);
  CHECK(service.set_mute(20, WindowsAudioService::kSourceStream, true) == 0 && hub.master_muted() && service.get_volume(20, WindowsAudioService::kSourceStream, &left, &right, &muted) == 0 && muted);
  CHECK(service.set_mute(20, WindowsAudioService::kSourceStream, false) == 0 && service.set_volume(20, WindowsAudioService::kSourceStream, 0xFFFF, 0xFFFF) == 0);
  CHECK(service.close(20, stream, false) == 0 && service.stream_count() == 0 && g.record_count() == 0);
  CHECK(service.open_capture(20, PcmFormat{SampleFormat::S16Le, 1, 2}, "", &stream) != 0);
  CHECK(service.open_capture(20, PcmFormat{SampleFormat::F32Le, 44100, 2}, "", &stream) == 0);
  service.process_exited(20);
  hub.pump();
  CHECK(service.stream_count() == 0 && g.record_count() == 0 && hub.stream_count() == 0);
  // without a capture hub, no capture stream opens
  {
    Graph none(A, &mixer);
    CHECK(none.initialize());
    WindowsAudioService plain(none, A);
    CHECK(plain.open_capture(20, PcmFormat{}, "", &stream) != 0 && plain.set_volume(20, WindowsAudioService::kSourceStream, 1, 1) != 0);
  }
}

// Recording from several threads at once: the hub pumps a microphone that keeps talking while threads open, read, pause and close record streams (through
// the graph and through the Windows service) and another changes the source's gain. Nothing may race (ASan, UBSan) and every reader hears audio.
static void test_record_threads() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  FakeMicrophone mic;
  audio::AudioCaptureHub hub(mic, A);
  CHECK(hub.initialize());
  Graph g(A, &mixer, &hub);
  CHECK(g.initialize());
  WindowsAudioService service(g, A);
  std::atomic<bool> stop{false};
  std::atomic<int> heard{0}, opened{0};
  std::thread pump([&] {
    while (!stop) {
      mic.add(480, 3000, -3000);
      hub.pump();
      g.refresh();
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  });
  std::thread knob([&] {
    for (int i = 0; !stop; ++i) {
      g.set_volume(g.source_id(), (i % 10) / 10.0f, i % 7 == 0);
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  });
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t)
    readers.emplace_back([&, t] {
      std::vector<uint8_t> buffer(8192);
      const uint32_t client = g.add_client(Origin::Linux, 100 + t, "protocol-pulse", "");
      for (int round = 0; round < 25; ++round) {
        uint32_t node = 0, stream = 0, got = 0;
        SharedPointer<RecordStream> r;
        if (t % 2 == 0) {
          node = g.create_record(client, PcmFormat{t == 0 ? SampleFormat::S16Le : SampleFormat::F32Le, 16000u + 8000u * uint32_t(t), uint32_t(1 + t % 3)}, Properties(A), &r);
          if (node) ++opened;
        } else if (service.open_capture(200 + t, PcmFormat{SampleFormat::S24Le, 44100, 2}, "thread.exe", &stream) == 0) {
          ++opened;
        }
        for (int k = 0; k < 20; ++k) {
          uint32_t n = 0;
          if (r) n = r->read(ByteView(buffer.data(), buffer.size()));
          else if (stream && service.read(200 + t, stream, ByteView(buffer.data(), buffer.size()), &got) == 0) n = got;
          if (n) ++heard;
          if (k == 10 && node) g.set_paused(node, true);
          if (k == 12 && node) g.set_paused(node, false);
          std::this_thread::sleep_for(std::chrono::microseconds(400));
        }
        if (node) g.destroy_node(node);
        if (stream) service.close(200 + t, stream, false);
      }
      g.remove_client(client);
      service.process_exited(200 + t);
    });
  for (std::thread& r : readers) r.join();
  stop = true;
  pump.join();
  knob.join();
  hub.pump();
  CHECK(opened == 100 && heard > 100 && g.record_count() == 0 && service.stream_count() == 0 && hub.stream_count() == 0);
}

// The sink's monitor: what the mixer plays, recorded through the graph, PulseAudio (tcos_output.stereo.monitor, sink | 1 << 24) and the Windows service
// (WASAPI loopback), with and without a microphone.
static void test_monitor() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);      // (no capture hub: the monitor is the only source)
  CHECK(g.initialize() && g.source_id() == 0);
  uint32_t monitor_ports = 0;
  g.for_each([&](const GraphObject& o) {
    if (o.type == ObjectType::Port && o.node == g.sink_id() && o.direction == kDirectionOutput && o.props.get("port.monitor") == StringView("true")) ++monitor_ports;
  });
  CHECK(monitor_ports == 2);
  const uint32_t client = g.add_client(Origin::Linux, 5, "protocol-pulse", "");
  SharedPointer<RecordStream> rec;
  const uint32_t node = g.create_record(client, PcmFormat{SampleFormat::S16Le, 48000, 2}, Properties(A), &rec, true);
  CHECK(node && rec && mixer.tap_count() == 1);
  CHECK(!g.create_record(client, PcmFormat{}, Properties(A), &rec, false));      // (no microphone)
  rec = g.record_of(node);
  bool linked = false;
  g.for_each([&](const GraphObject& o) { if (o.type == ObjectType::Link && o.input_node == node && o.output_node == g.sink_id()) linked = true; });
  CHECK(linked);
  // something plays: the monitor hears it as mixed
  SharedPointer<PlaybackStream> play;
  const uint32_t player = g.create_playback(client, PcmFormat{SampleFormat::S16Le, 48000, 2}, Properties(A), &play);
  std::vector<uint8_t> tone = s16_constant(2400, 2, 1234);
  CHECK(player && play->write(ConstByteView(tone.data(), tone.size())) == tone.size());
  mixer.pump();
  std::vector<uint8_t> got(48000);
  uint32_t n = rec->read(ByteView(got.data(), got.size()));
  CHECK(n == audio::AudioMixer::kChunkFrames * 4 && s16_at(got, 200) == 1234 && s16_at(got, 201) == 1234);      // (one chunk a pump)
  // nothing plays: nothing is heard (as WASAPI loopback)
  drain_mixer(mixer, dev);
  rec->flush();
  mixer.pump();
  CHECK(rec->available_bytes() == 0);
  // closing lets the mixer's tap go
  g.destroy_node(node);
  dev.play_all();
  CHECK(play->write(ConstByteView(tone.data(), tone.size())) > 0);
  mixer.pump();
  CHECK(mixer.tap_count() == 0);

  // PulseAudio: the monitor is a source; with no microphone it is the default one
  PulseServer server(g, A);
  PulseClient c(server);
  uint8_t cookie[256] = {};
  std::vector<uint8_t> body;
  uint32_t cmd = 0;
  uint32_t t = c.command(8, [&](pulse::TagWriter& w) { w.u32(35); w.arbitrary(ConstByteView(cookie, 256)); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  t = c.command(20, [](pulse::TagWriter&) {});
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    StringView name, version, user, host, sink, source;
    pulse::SampleSpec ss;
    CHECK(r.string(&name) && r.string(&version) && r.string(&user) && r.string(&host) && r.sample_spec(&ss) && r.string(&sink) && r.string(&source) &&
          source == StringView("tcos_output.stereo.monitor"));
  }
  const uint32_t monitor_index = g.sink_id() | (1u << 24);
  t = c.command(22, [](pulse::TagWriter&) {});      // the sink names its monitor
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t index = 0, module = 0, monitor = 0; StringView name, desc, mname; pulse::SampleSpec ss; pulse::ChannelMap map; pulse::Cvolume vol; bool mute;
    CHECK(r.u32(&index) && r.string(&name) && r.string(&desc) && r.sample_spec(&ss) && r.channel_map(&map) && r.u32(&module) && r.cvolume(&vol) && r.boolean(&mute) &&
          r.u32(&monitor) && r.string(&mname) && monitor == monitor_index && mname == StringView("tcos_output.stereo.monitor"));
  }
  t = c.command(24, [](pulse::TagWriter&) {});      // the source list: only the monitor
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t index = 0, module = 0, of = 0, flags = 0; StringView name, desc, ofname, driver; pulse::SampleSpec ss; pulse::ChannelMap map; pulse::Cvolume vol; bool mute; uint64_t lat;
    CHECK(r.u32(&index) && r.string(&name) && r.string(&desc) && r.sample_spec(&ss) && r.channel_map(&map) && r.u32(&module) && r.cvolume(&vol) && r.boolean(&mute) &&
          r.u32(&of) && r.string(&ofname) && r.usec(&lat) && r.string(&driver) && r.u32(&flags));
    CHECK(index == monitor_index && name == StringView("tcos_output.stereo.monitor") && of == g.sink_id() && desc.starts_with("Monitor of "));
  }
  for (const char* name : {"@DEFAULT_MONITOR@", "tcos_output.stereo.monitor", "@DEFAULT_SOURCE@"}) {
    t = c.command(11, [&](pulse::TagWriter& w) { w.string(name); });
    CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t index = 0;
    CHECK(r.u32(&index) && index == monitor_index);
  }
  t = c.command(11, [&](pulse::TagWriter& w) { w.string("tcos_input.stereo"); });      // (there is no microphone)
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 0);
  t = c.command(23, [&](pulse::TagWriter& w) { w.u32(monitor_index); w.string(static_cast<const char*>(nullptr)); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  // record the monitor (parec -d @DEFAULT_MONITOR@) while something plays
  const pulse::SampleSpec spec{pulse::kS16Le, 2, 48000};
  t = c.command(5, [&](pulse::TagWriter& w) {
    pulse::TagWriter& x = w;
    pulse::ChannelMap map; map.channels = 2; map.positions[0] = 1; map.positions[1] = 2;
    pulse::Cvolume vol; vol.channels = 2; vol.values[0] = vol.values[1] = pulse::kVolumeNorm;
    x.sample_spec(spec); x.channel_map(map); x.u32(0xFFFFFFFFu); x.string("@DEFAULT_MONITOR@"); x.u32(0xFFFFFFFFu); x.boolean(false); x.u32(960 * 4);
    for (int i = 0; i < 7; ++i) x.boolean(false);
    Properties props(A);
    x.boolean(false); x.boolean(false); x.proplist(props); x.u32(0xFFFFFFFFu); x.boolean(false); x.boolean(false); x.boolean(false);
    x.u8(0); x.cvolume(vol); x.boolean(false); x.boolean(false); x.boolean(false); x.boolean(false); x.boolean(false);
  });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  uint32_t channel = 0, output = 0, maxlength = 0, fragsize = 0, source = 0;
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    pulse::SampleSpec ss; pulse::ChannelMap map; StringView sname;
    CHECK(r.u32(&channel) && r.u32(&output) && r.u32(&maxlength) && r.u32(&fragsize) && r.sample_spec(&ss) && r.channel_map(&map) && r.u32(&source) && r.string(&sname) &&
          source == monitor_index && sname == StringView("tcos_output.stereo.monitor"));
  }
  size_t heard = 0;
  bool value_ok = true;
  for (int i = 0; i < 6; ++i) {
    dev.play_all();
    std::vector<uint8_t> more = s16_constant(960, 2, -777);
    play->write(ConstByteView(more.data(), more.size()));
    mixer.pump();
    server.tick();
    for (const PulsePacket& p : c.take())
      if (p.channel == channel) {
        heard += p.payload.size();
        if (p.payload.size() >= 4 && s16_at(p.payload, 0) != -777 && s16_at(p.payload, 0) != 1234) value_ok = false;
      }
  }
  CHECK(heard >= 960 * 4 * 3 && value_ok);
  t = c.command(32, [](pulse::TagWriter&) {});      // the source output says it records the monitor
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    uint32_t idx = 0, module = 0, owner = 0, from = 0; StringView name;
    CHECK(r.u32(&idx) && r.string(&name) && r.u32(&module) && r.u32(&owner) && r.u32(&from) && idx == output && from == monitor_index);
  }
  t = c.command(38, [&](pulse::TagWriter& w) { pulse::Cvolume v; v.channels = 2; v.values[0] = v.values[1] = 100; w.u32(monitor_index); w.string(static_cast<const char*>(nullptr)); w.cvolume(v); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);      // (accepted; the monitor has no volume of its own)
  server.close_client(c.handle);
  CHECK(g.record_count() == 0);

  // Windows: OPEN_CAPTURE of the monitor (WASAPI loopback) works without a microphone
  WindowsAudioService service(g, A);
  uint32_t stream = 0, read = 0;
  CHECK(service.open_capture(30, PcmFormat{SampleFormat::F32Le, 48000, 2}, "obs.exe", &stream, true) == 0 && stream);
  CHECK(service.open_capture(30, PcmFormat{SampleFormat::F32Le, 48000, 2}, "obs.exe", &read, false) != 0);      // (no microphone)
  dev.play_all();
  play->flush();      // (what the PulseAudio part left queued)
  play->write(ConstByteView(tone.data(), tone.size()));
  mixer.pump();
  std::vector<uint8_t> floats(48000);
  CHECK(service.read(30, stream, ByteView(floats.data(), floats.size()), &read) == 0 && read >= 1000 * 8);
  float v = 0;
  memcpy(&v, floats.data() + 800, 4);
  CHECK(fabsf(v * 32768.0f - 1234.0f) < 2.0f);
  service.process_exited(30);
  g.remove_client(client);
  dev.play_all();
  mixer.pump();
  CHECK(g.record_count() == 0 && mixer.tap_count() == 0);
}

// A native program's record stream (WebRTC's microphone through the desktop) shows up as a source output of the native client, with its gain as volume.
static void test_native_capture() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  FakeMicrophone mic;
  audio::AudioCaptureHub hub(mic, A);
  CHECK(hub.initialize());
  Graph g(A, &mixer, &hub);
  CHECK(g.initialize());
  SharedPointer<audio::CaptureStream> web = hub.open_stream(audio::StreamFormat{16000, 1, audio::SampleType::S16});
  CHECK(web);
  web->set_label("Web Browser");
  g.refresh();
  uint32_t node = 0;
  g.for_each([&](const GraphObject& o) {
    if (o.type == ObjectType::Node && o.role == NodeRole::Capture && o.native_capture.get() == web.get() && o.origin == Origin::Native &&
        o.client == g.native_client_id() && o.props.get("application.name") == StringView("Web Browser") && o.format.rate == 16000)
      node = o.id;
  });
  CHECK(node != 0 && g.record_count() == 1);
  g.refresh();
  CHECK(g.record_count() == 1);      // (not again)
  CHECK(g.set_volume(node, 0.5f, true) && web->muted() && web->gain() == uint8_t(0.5f * hardware::kAudioMaxVolume + 0.5f));
  // the graph's own record streams are not taken for native ones
  const uint32_t client = g.add_client(Origin::Linux, 9, "protocol-pulse", "");
  SharedPointer<RecordStream> rec;
  CHECK(g.create_record(client, PcmFormat{}, Properties(A), &rec) && rec->capture().server_owned());
  g.refresh();
  CHECK(g.record_count() == 2);
  // it ends: its node goes
  web->close();
  g.refresh();
  CHECK(g.record_count() == 1 && !g.visit(node, [](const GraphObject&) {}));
  g.remove_client(client);
  CHECK(g.record_count() == 0);
}

// A playing stream's balance: PulseAudio's per-channel volumes (pavucontrol's balance) become the stream's pan and read back; the mix follows.
static void test_balance() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);
  CHECK(g.initialize());
  PulseServer server(g, A);
  PulseClient c(server);
  uint8_t cookie[256] = {};
  std::vector<uint8_t> body;
  uint32_t cmd = 0;
  uint32_t t = c.command(8, [&](pulse::TagWriter& w) { w.u32(35); w.arbitrary(ConstByteView(cookie, 256)); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  const pulse::SampleSpec spec{pulse::kS16Le, 2, 48000};
  t = c.command(3, [&](pulse::TagWriter& w) { put_create_playback(w, spec, 19200, "balanced"); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  uint32_t channel = 0, node = 0;
  {
    pulse::TagReader r(ConstByteView(body.data(), body.size()));
    CHECK(r.u32(&channel) && r.u32(&node));
  }
  pulse::Cvolume v; v.channels = 2; v.values[0] = pulse::kVolumeNorm; v.values[1] = linear_to_pulse_volume(0.25f);
  t = c.command(37, [&](pulse::TagWriter& w) { w.u32(node); w.cvolume(v); });
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  float l = 0, r = 0; bool mute = true;
  CHECK(g.get_balance(node, &l, &r, &mute) && l == 1.0f && fabsf(r - 0.25f) < 0.01f && !mute);
  t = c.command(29, [&](pulse::TagWriter& w) { w.u32(node); });      // the sink input's volume reads back per channel
  CHECK(find_reply(c.take(), t, &body, &cmd) && cmd == 2);
  {
    pulse::TagReader rd(ConstByteView(body.data(), body.size()));
    uint32_t idx, module, owner, sink; StringView name; pulse::SampleSpec ss; pulse::ChannelMap map; pulse::Cvolume got;
    CHECK(rd.u32(&idx) && rd.string(&name) && rd.u32(&module) && rd.u32(&owner) && rd.u32(&sink) && rd.sample_spec(&ss) && rd.channel_map(&map) && rd.cvolume(&got) &&
          got.channels == 2 && got.values[0] == pulse::kVolumeNorm && got.values[1] == v.values[1]);
  }
  std::vector<uint8_t> tone = s16_constant(4800, 2, 8000);
  c.packet(channel, ConstByteView(tone.data(), tone.size()));
  dev.take();
  mixer.pump();
  std::vector<int16_t> out = dev.take();
  CHECK(out.size() > 400 && out[200] > 7000 && out[201] > 1000 && out[201] < out[200] / 2);      // (louder on the left, still heard on the right)
  // the native protocol's Props show the same two volumes
  ByteBuffer pb(A);
  pod::Builder b(pb);
  build_props_param(b, 0.8f, false, 2, 1.0f, 0.5f);
  CHECK(pb.size() > 0);
  server.close_client(c.handle);
}

// ---- client-node (libpipewire's pw_stream) ------------------------------------------------------------------------------------------------------

// Descriptors in memory: shared memory is a block of this process, an eventfd a counter.
struct FakeFd final : Descriptor {
  std::vector<uint8_t> bytes;
  uint8_t* aligned = nullptr;
  uint64_t size = 0;
  std::atomic<uint64_t> counter{0};
  bool event = false;
  FakeFd(uint64_t n, bool is_event) : bytes(is_event ? 0 : n + 4096), size(is_event ? 0 : n), event(is_event) {
    if (!is_event) aligned = bytes.data() + (4096 - reinterpret_cast<uintptr_t>(bytes.data()) % 4096) % 4096;
  }
  ByteView memory() override { return event ? ByteView() : ByteView(aligned, size); }
  bool signal() override { if (!event) return false; ++counter; return true; }
  uint64_t take_signals() override { return counter.exchange(0); }
};
// A dmabuf: a GPU image whose pixels the server copies out (sync_from_gpu) into memory(); `busy` as the GPU still drawing.
struct FakeGpuFd final : Descriptor {
  std::vector<uint8_t> image, copy;
  bool busy = false;
  int syncs = 0;
  explicit FakeGpuFd(uint64_t n) : image(n, 0), copy(n, 0) {}
  ByteView memory() override { return ByteView(copy.data(), copy.size()); }
  bool signal() override { return false; }
  uint64_t take_signals() override { return 0; }
  bool gpu() const override { return true; }
  bool sync_from_gpu() override {
    ++syncs;
    if (busy) return false;
    copy = image;
    return true;
  }
};
struct FakeMaker final : IDescriptorMaker {
  int made = 0;
  bool fail = false;
  Fd make_memory(uint64_t size, StringView) override { ++made; return fail ? Fd() : Fd::make_as<FakeFd>(A, size, false); }
  Fd make_event() override { ++made; return fail ? Fd() : Fd::make_as<FakeFd>(A, 0, true); }
};

static uint32_t u32_at(ByteView m, uint64_t at) { uint32_t v; memcpy(&v, m.data() + at, 4); return v; }
static void put_u32(ByteView m, uint64_t at, uint32_t v) { memcpy(m.data() + at, &v, 4); }

// The setup a pw_stream goes through, as libpipewire 1.x sends it; returns the ClientNode id, the activation, io and buffer memory and the wake fd.
struct ExportedNode {
  uint32_t id = 0, node = 0, n_buffers = 0, buffer_stride = 0, data_offset = 0, data_size = 0;
  Fd activation, io, buffers, wake;
  bool ok = false;
};

// (latency, rate, channels: what the update's props and EnumFormat say of the stream, as pw_stream sends them; 0 / nullptr: not said)
using Props = std::vector<std::pair<std::string, std::string>>;
static ExportedNode export_node(NativeClient& c, uint32_t id, const char* media_class, uint32_t direction, const char* latency = nullptr, uint32_t rate = 0,
                                uint32_t channels = 0, const Props& extra = {}) {
  ExportedNode e;
  e.id = id;
  c.send(0, 6, [&](pod::Builder& b) {      // create_object
    b.string("client-node"); b.string("PipeWire:Interface:ClientNode"); b.int32(6);
    const uint64_t d = b.begin_struct();
    b.int32(2 + int32_t(extra.size())); b.string("media.class"); b.string(media_class); b.string("application.name"); b.string("pw-test");
    for (const auto& kv : extra) { b.string(StringView(kv.first.c_str())); b.string(StringView(kv.second.c_str())); }
    b.end(d);
    b.int32(int32_t(id));
  });
  std::map<uint32_t, Fd> mems;
  uint64_t next_fd = c.fds.size();
  auto take = [&] {
    std::vector<NativeMessage> msgs = c.take_with_fds();
    std::vector<NativeMessage> mine;
    for (NativeMessage& m : msgs) {
      // the descriptors of this message are the next n_fds collected
      std::vector<Fd> own(c.fds.begin() + long(next_fd), c.fds.begin() + long(next_fd + m.n_fds));
      next_fd += m.n_fds;
      if (m.id == 0 && m.opcode == 6) {      // add_mem: id, type, fd, flags
        int32_t mem = 0; uint32_t type = 0; int64_t fd = -1;
        pod::Parser q = args_of(m); pod::Value v;
        q.int32(&mem); q.id(&type); q.next(&v); v.get_fd(&fd);
        if (type == 2 && fd >= 0 && size_t(fd) < own.size()) mems[uint32_t(mem)] = own[size_t(fd)];
      }
      if (m.id == 0 && m.opcode == 8) { pod::Parser p = args_of(m); int32_t pid = 0, node = 0; if (p.int32(&pid) && p.int32(&node) && uint32_t(pid) == id) e.node = uint32_t(node); }
      if (m.id == id && m.opcode == 0) {      // transport: readfd, writefd, mem, offset, size
        pod::Parser p = args_of(m); pod::Value r, w; int32_t mem = 0, off = 0, size = 0; int64_t rf = -1;
        if (p.next(&r) && p.next(&w) && p.int32(&mem) && p.int32(&off) && p.int32(&size) && r.get_fd(&rf) && rf >= 0 && size_t(rf) < own.size()) {
          e.wake = own[size_t(rf)];
          e.activation = mems[uint32_t(mem)];
          CHECK(size == 2312 && off == 0 && m.n_fds == 2);
        }
      }
      if (m.id == id) mine.push_back(m);
    }
    return mine;
  };
  std::vector<NativeMessage> msgs = take();
  CHECK(e.activation && e.wake);
  // the node's update, then active: the server answers with a PortConfig
  c.send(id, 2, [&](pod::Builder& b) {
    b.int32(3); b.int32(rate ? 1 : 0);
    if (rate) build_format_param(b, 3, PcmFormat{SampleFormat::F32Le, rate, channels});
    const uint64_t i = b.begin_struct();
    b.int32(0); b.int32(1); b.int64(7); b.int64(0);
    b.int32(latency ? 2 : 1); b.string("node.name"); b.string("pw-test-node");
    if (latency) { b.string("node.latency"); b.string(latency); }
    b.int32(0);
    b.end(i);
  });
  c.send(id, 4, [](pod::Builder& b) { b.boolean(true); });
  msgs = take();
  bool port_config = false;
  for (const NativeMessage& m : msgs)
    if (m.opcode == 1) {
      pod::Parser p = args_of(m); uint32_t pid = 0; int32_t flags = 0; pod::Value obj;
      pod::ObjectReader o;
      if (p.id(&pid) && p.int32(&flags) && p.pod(&obj) && o.open(obj) && pid == 11 && o.object_type() == 0x40008) {
        uint32_t key = 0, f = 0, dir = 99, mode = 0; pod::Value v;
        while (o.next(&key, &f, &v)) { if (key == 1) v.get_id(&dir); if (key == 2) v.get_id(&mode); }
        port_config = dir == direction && mode == 2;
      }
    }
  CHECK(port_config && e.node != 0);      // (the node is bound once it is active: its format is known by then)
  // the port appears (its EnumFormat): the server sets the Format
  c.send(id, 3, [&](pod::Builder& b) {
    b.int32(int32_t(direction)); b.int32(0); b.int32(3); b.int32(1);
    build_format_param(b, 3, PcmFormat{SampleFormat::F32Le, 48000, 2});
    b.none();
  });
  msgs = take();
  bool format = false;
  for (const NativeMessage& m : msgs)
    if (m.opcode == 7) { pod::Parser p = args_of(m); int32_t dir = -1, port = -1; uint32_t pid = 0; format = p.int32(&dir) && p.int32(&port) && p.id(&pid) && pid == 4 && dir == int32_t(direction) && port == 0; }
  CHECK(format);
  // the port takes it and says what buffers it wants: the server gives them, the io area and Start
  c.send(id, 3, [&](pod::Builder& b) {
    b.int32(int32_t(direction)); b.int32(0); b.int32(3); b.int32(1);
    const uint64_t o = b.begin_object(0x40004, 5);
    b.property(1); { const uint64_t ch = b.begin_choice(1, pod::kInt); b.choice_value32(4); b.choice_value32(2); b.choice_value32(8); b.end(ch); }
    b.property(3); b.int32(20000);
    b.end(o);
    b.none();
  });
  msgs = take();
  bool mix = false, buffers = false, io = false, start = false;
  for (const NativeMessage& m : msgs) {
    if (m.opcode == 11) mix = true;
    if (m.opcode == 8) {
      pod::Parser p = args_of(m); int32_t dir, port, mixid, flags, n;
      if (p.int32(&dir) && p.int32(&port) && p.int32(&mixid) && p.int32(&flags) && p.int32(&n)) {
        e.n_buffers = uint32_t(n);
        int32_t mem, off, size, metas, datas, dflags, mapoff, maxsize, dataoff; uint32_t dtype;
        if (p.int32(&mem) && p.int32(&off) && p.int32(&size) && p.int32(&metas) && p.int32(&datas) && p.id(&dtype) && p.int32(&dataoff) && p.int32(&dflags) &&
            p.int32(&mapoff) && p.int32(&maxsize)) {
          e.buffers = mems[uint32_t(mem)];
          e.buffer_stride = uint32_t(size);
          e.data_offset = uint32_t(dataoff);
          e.data_size = uint32_t(maxsize);
          buffers = dtype == 1 && datas == 1 && metas == 0 && maxsize >= 20000 && (mixid == (direction == 1 ? -1 : 0));
        }
      }
    }
    if (m.opcode == 9) {
      pod::Parser p = args_of(m); int32_t dir, port, mixid, mem, off, size; uint32_t ioid;
      if (p.int32(&dir) && p.int32(&port) && p.int32(&mixid) && p.id(&ioid) && p.int32(&mem) && p.int32(&off) && p.int32(&size)) {
        e.io = mems[uint32_t(mem)];
        io = ioid == 1 && size == 8;
      }
    }
    if (m.opcode == 4) start = true;
  }
  CHECK(mix && buffers && io && start && e.n_buffers == 4 && e.buffers && e.io);
  e.ok = mix && buffers && io && start && e.buffers && e.io;
  return e;
}

static void test_client_node() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  FakeMicrophone mic;
  audio::AudioCaptureHub hub(mic, A);
  CHECK(hub.initialize());
  Graph g(A, &mixer, &hub);
  CHECK(g.initialize());
  NativeServer server(g, A);
  // without a descriptor maker client-node is refused
  {
    NativeClient c(server);
    c.send(0, 1, [](pod::Builder& b) { b.int32(4); });
    c.take();
    c.send(0, 6, [](pod::Builder& b) {
      b.string("client-node"); b.string("PipeWire:Interface:ClientNode"); b.int32(6);
      const uint64_t d = b.begin_struct(); b.int32(0); b.end(d);
      b.int32(3);
    });
    bool refused = false;
    for (const NativeMessage& m : c.take()) refused |= m.id == 0 && m.opcode == 3;
    CHECK(refused && g.playback_count() == 0);
    server.close_client(c.handle);
  }
  FakeMaker maker;
  server.set_descriptor_maker(&maker);
  NativeClient c(server);
  c.send(0, 1, [](pod::Builder& b) { b.int32(4); });
  c.take_with_fds();

  // ---- playback: the stream fills a buffer each cycle
  ExportedNode play = export_node(c, 3, "Stream/Output/Audio", 1);
  CHECK(play.ok && g.playback_count() == 1 && server.running_nodes() == 0);
  bool named = false;
  g.visit(play.node, [&](const GraphObject& o) { named = o.props.get("application.name") == StringView("pw-test") && o.props.get("client.api") == StringView("pipewire"); });
  CHECK(named);
  if (play.ok) {
    ByteView act = play.activation->memory(), io = play.io->memory(), bufs = play.buffers->memory();
    CHECK(u32_at(act, 0) == 4 && u32_at(act, 544) == 1);      // INACTIVE until the client is ready; server version 1
    server.tick();
    CHECK(play.wake->take_signals() == 0);      // (not triggered while inactive)
    put_u32(act, 0, 3);      // the client added the node to its loop: FINISHED
    server.tick();
    CHECK(server.running_nodes() == 1 && u32_at(act, 0) == 1 && play.wake->take_signals() == 1);      // TRIGGERED, signalled
    CHECK(u32_at(act, 560 + 80 + 4) == 48000 && u32_at(act, 560 + 96) == 1024);      // the clock: 1/48000, a 1024-frame quantum
    // the client fills buffer 2 with 1024 frames of 0.25 / -0.25, says HAVE_DATA and FINISHED
    const uint64_t base = 2ull * play.buffer_stride;
    for (uint32_t i = 0; i < 1024; ++i) {
      const float l = 0.25f, r = -0.25f;
      memcpy(bufs.data() + base + play.data_offset + 8 * i, &l, 4);
      memcpy(bufs.data() + base + play.data_offset + 8 * i + 4, &r, 4);
    }
    put_u32(bufs, base, 0); put_u32(bufs, base + 4, 8192);
    put_u32(io, 0, 2); put_u32(io, 4, 2);
    put_u32(act, 0, 3);
    dev.take();
    server.tick();
    CHECK(u32_at(io, 0) == 1 && u32_at(io, 4) == 2);      // NEED_DATA, buffer 2 handed back
    CHECK(u32_at(act, 0) == 1);      // and the next cycle triggered at once (the stream wants three quanta)
    mixer.pump();
    std::vector<int16_t> out = dev.take();
    CHECK(out.size() > 400 && abs(out[200] - 8191) < 40 && abs(out[201] + 8191) < 40);
    // a cycle without data hands nothing back
    put_u32(io, 0, 1); put_u32(io, 4, 1);
    put_u32(act, 0, 3);
    server.tick();
    CHECK(u32_at(io, 4) == 0xFFFFFFFFu);
    // a cycle that never finishes is given up after a while and triggered again
    for (int i = 0; i < 120; ++i) { server.tick(); mixer.pump(); dev.play_all(); }
    CHECK(play.wake->take_signals() >= 2);
    // set_active(false): Pause, no more cycles
    c.send(3, 4, [](pod::Builder& b) { b.boolean(false); });
    bool paused = false;
    for (const NativeMessage& m : c.take_with_fds()) paused |= m.id == 3 && m.opcode == 4;
    server.tick();
    CHECK(paused && server.running_nodes() == 0);
  }

  // ---- the stream's own rate, channels and latency: a 44.1 kHz mono stream asking for 512-frame cycles gets them (no conversion in the program)
  {
    ExportedNode own = export_node(c, 5, "Stream/Output/Audio", 1, "512/44100", 44100, 1);
    CHECK(own.ok);
    if (own.ok) {
      ByteView act = own.activation->memory();
      CHECK(u32_at(act, 560 + 80 + 4) == 44100 && u32_at(act, 560 + 96) == 512);
      put_u32(act, 0, 3);
      server.tick();
      CHECK(u32_at(act, 0) == 1 && u32_at(act, 560 + 80 + 4) == 44100 && u32_at(act, 560 + 96) == 512);
      CHECK(own.data_size >= 8192 * 4);      // (8192 frames of one channel at least)
      bool mono = false;
      g.visit(own.node, [&](const GraphObject& o) { mono = o.props.get("node.latency") == StringView("512/44100"); });
      CHECK(mono);
      c.send(0, 7, [](pod::Builder& b) { b.int32(5); });      // Core destroy
      c.take_with_fds();
      server.tick();
    }
    // node.latency without a format: at the default 48 kHz; a silly one is held to 64 .. 8192 frames
    ExportedNode small = export_node(c, 6, "Stream/Output/Audio", 1, "1/48000");
    CHECK(small.ok && u32_at(small.activation->memory(), 560 + 96) == 64);
    c.send(0, 7, [](pod::Builder& b) { b.int32(6); });
    c.take_with_fds();
    ExportedNode big = export_node(c, 7, "Stream/Output/Audio", 1, "100000/1000");
    CHECK(big.ok && u32_at(big.activation->memory(), 560 + 96) == 8192);
    c.send(0, 7, [](pod::Builder& b) { b.int32(7); });
    c.take_with_fds();
    ExportedNode bad = export_node(c, 8, "Stream/Output/Audio", 1, "12/0", 300000, 9);      // (refused values: the defaults)
    CHECK(bad.ok && u32_at(bad.activation->memory(), 560 + 96) == 1024 && u32_at(bad.activation->memory(), 560 + 80 + 4) == 48000);
    c.send(0, 7, [](pod::Builder& b) { b.int32(8); });
    c.take_with_fds();
    server.tick();
  }

  // ---- recording: the server fills a buffer from the microphone before each cycle
  ExportedNode rec = export_node(c, 4, "Stream/Input/Audio", 0);
  CHECK(rec.ok && g.record_count() == 1);
  if (rec.ok) {
    ByteView act = rec.activation->memory(), io = rec.io->memory(), bufs = rec.buffers->memory();
    put_u32(act, 0, 3);
    server.tick();
    CHECK(rec.wake->take_signals() == 0);      // (nothing recorded yet)
    mic.add(2400, 16384, -8192);
    hub.pump();
    server.tick();
    CHECK(rec.wake->take_signals() == 1 && u32_at(io, 0) == 2);
    const uint32_t id = u32_at(io, 4);
    CHECK(id < rec.n_buffers);
    if (id < rec.n_buffers) {
      const uint64_t base = uint64_t(id) * rec.buffer_stride;
      float l = 0, r = 0;
      memcpy(&l, bufs.data() + base + rec.data_offset + 8 * 100, 4);
      memcpy(&r, bufs.data() + base + rec.data_offset + 8 * 100 + 4, 4);
      CHECK(u32_at(bufs, base + 4) == 8192 && fabsf(l - 0.5f) < 0.001f && fabsf(r + 0.25f) < 0.001f);
    }
    put_u32(act, 0, 3);
    server.tick();      // (the client took it; another quantum is waiting)
    CHECK(rec.wake->take_signals() == 1 && u32_at(io, 4) != id);
  }

  // ---- garbage: every truncation and bit flips of the client's client-node messages, and Core destroy
  std::mt19937 rng(23);
  for (int round = 0; round < 600; ++round) {
    ByteBuffer body(A);
    pod::Builder b(body);
    const uint64_t f = b.begin_struct();
    const uint32_t op = 2 + uint32_t(round % 3);
    if (op == 2) {
      b.int32(3); b.int32(1); b.raw(ConstByteView());
      const uint64_t i = b.begin_struct(); b.int32(0); b.int32(1); b.int64(7); b.int64(0); b.int32(1); b.string("k"); b.string("v"); b.int32(0); b.end(i);
    } else if (op == 3) {
      b.int32(1); b.int32(0); b.int32(3); b.int32(1);
      const uint64_t o = b.begin_object(0x40004, 5); b.property(3); b.int32(int32_t(rng() % 100000)); b.end(o);
      b.none();
    } else {
      b.boolean(round % 2);
    }
    b.end(f);
    std::vector<uint8_t> m(body.view().data(), body.view().data() + body.size());
    if (round % 2) m.resize(rng() % (m.size() + 1));
    else for (int k = 0; k < 3; ++k) m[rng() % m.size()] ^= uint8_t(1u << (rng() % 8));
    ByteBuffer msg(A);
    msg.put32(3); msg.put32(op << 24 | uint32_t(m.size())); msg.put32(0); msg.put32(0);
    msg.append(ConstByteView(m.data(), m.size()));
    server.receive(c.handle, msg.view());
    server.tick();
    c.take_with_fds();
  }
  c.send(0, 7, [](pod::Builder& b) { b.int32(3); });      // Core destroy of the playback node
  c.take_with_fds();
  CHECK(g.playback_count() == 0 && g.record_count() == 1);
  // the maker fails: refused, nothing left behind
  maker.fail = true;
  c.send(0, 6, [](pod::Builder& b) {
    b.string("client-node"); b.string("PipeWire:Interface:ClientNode"); b.int32(6);
    const uint64_t d = b.begin_struct(); b.int32(1); b.string("media.class"); b.string("Stream/Output/Audio"); b.end(d);
    b.int32(9);
  });
  bool refused = false;
  for (const NativeMessage& m : c.take_with_fds()) refused |= m.id == 0 && m.opcode == 3;
  CHECK(refused && g.playback_count() == 0);
  server.close_client(c.handle);
  CHECK(g.record_count() == 0 && server.client_count() == 0);
}

// ---- the Windows service -------------------------------------------------------------------------------------------------------------------------

static void test_windows_service() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);
  CHECK(g.initialize());
  WindowsAudioService service(g, A);
  uint32_t stream = 0, taken = 0;
  CHECK(service.open(10, PcmFormat{SampleFormat::S16Le, 22050, 1}, "game.exe", &stream) == 0 && stream);
  CHECK(service.write(10, stream, ConstByteView(s16_constant(2205, 1, 4000).data(), 4410), &taken) == 0);
  std::vector<uint8_t> mono = s16_constant(2205, 1, 4000);
  CHECK(service.write(10, stream, ConstByteView(mono.data(), mono.size()), &taken) == 0 && taken == mono.size());
  // another process cannot touch it
  CHECK(service.write(11, stream, ConstByteView(mono.data(), mono.size()), &taken) != 0);
  CHECK(service.close(11, stream, false) != 0);
  bool owned = false;
  g.visit(stream, [&](const GraphObject& o) { owned = o.origin == Origin::Windows && o.props.get("application.name") == StringView("game.exe"); });
  CHECK(owned && service.stream_count() == 1);
  // what the mixer has not taken yet (WASAPI's padding): all of it, then less once the mixer has run
  SharedPointer<PlaybackStream> playback = g.playback_of(stream);
  const uint64_t waiting = playback ? playback->waiting_bytes() : 0;
  CHECK(playback && waiting > 4000 && waiting <= 8820 && waiting <= playback->buffered_bytes());
  dev.take();
  mixer.pump();
  CHECK(playback && playback->waiting_bytes() < waiting);
  std::vector<int16_t> out = dev.take();
  CHECK(out.size() > 202 && abs(out[200] - 2828) <= 6 && abs(out[201] - 2828) <= 6);      // mono to both sides, at the pan's equal-power centre (0.707)
  // a left/right volume pair is the stream's volume and a pan (DirectSound/WASAPI balance), and reads back the same
  uint32_t left = 0, right = 0;
  bool mute = true;
  CHECK(service.set_volume(10, stream, 0xFFFF, 0x8000) == 0);
  CHECK(service.get_volume(10, stream, &left, &right, &mute) == 0 && !mute && left == 0xFFFF && right > 0x7400 && right < 0x8C00);
  CHECK(audio::pan_for_gains(1000, 1000, true) == 0 && audio::pan_for_gains(0, 5, false) == audio::kPanRight && audio::pan_for_gains(5, 0, true) == audio::kPanLeft);
  CHECK(audio::pan_for_gains(32768, 16384, false) < -20 && audio::pan_for_gains(16384, 32768, false) > 20);
  dev.take();
  CHECK(service.write(10, stream, ConstByteView(mono.data(), mono.size()), &taken) == 0);
  mixer.pump();
  out = dev.take();
  CHECK(out.size() > 202 && out[200] > out[201] * 3 / 2 && out[201] > out[200] / 3);      // louder on the left, still heard on the right
  CHECK(service.set_mute(10, stream, true) == 0 && service.get_volume(10, stream, &left, &right, &mute) == 0 && mute && left == 0xFFFF);
  dev.take();
  CHECK(service.write(10, stream, ConstByteView(mono.data(), mono.size()), &taken) == 0);
  mixer.pump();
  out = dev.take();
  CHECK(out.size() > 202 && out[200] == 0 && out[201] == 0);
  CHECK(service.set_mute(11, stream, false) != 0 && service.set_volume(11, stream, 1, 1) != 0 && service.get_volume(11, stream, &left, &right, &mute) != 0);
  // the master volume (stream 0): no balance, the louder side
  CHECK(service.set_volume(10, 0, 0x4000, 0x8000) == 0 && service.get_volume(10, 0, &left, &right, &mute) == 0 && left == right && right > 0x7E00 && right < 0x8200);
  CHECK(service.set_mute(10, 0, true) == 0 && service.get_volume(10, 0, &left, &right, &mute) == 0 && mute);
  CHECK(service.set_mute(10, 0, false) == 0 && service.set_volume(10, 0, 0xFFFF, 0xFFFF) == 0);
  CHECK(service.close(10, stream, true) == 0 && service.stream_count() == 0);
  uint32_t s2 = 0;
  CHECK(service.open(10, PcmFormat{SampleFormat::F32Le, 48000, 2}, "", &s2) == 0);
  service.process_exited(10);
  CHECK(service.stream_count() == 0 && g.playback_count() == 0);
  CHECK(service.open(12, PcmFormat{SampleFormat::S16Le, 100, 2}, "x", &s2) != 0);
}

// A virtual sink from the Windows side: a stream plays into it (not the speakers), another records its monitor, its master volume is its own; and it can
// go while they are open.
static void test_windows_virtual_sink() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);
  CHECK(g.initialize());
  CHECK(g.create_virtual_node(g.native_client_id(), Properties(A), 2, 48000) == 0);      // (no clock yet: none can be made)
  g.set_clock(&test_clock);
  Properties props(A);
  props.set("node.name", "win-null");
  props.set("node.description", "Windows null sink");
  const uint32_t sink = g.create_virtual_node(g.native_client_id(), props, 2, 48000);
  CHECK(sink && g.is_virtual_sink(sink) && !g.is_virtual_sink(g.sink_id()));
  List<uint32_t> sinks(A);
  CHECK(g.virtual_sinks(&sinks) && sinks.count() == 1 && sinks[0] == sink);
  WindowsAudioService service(g, A);
  uint32_t out = 0, in = 0, wrong = 0, taken = 0, got = 0;
  CHECK(service.open(20, PcmFormat{SampleFormat::S16Le, 48000, 2}, "player.exe", &out, g.sink_id()) != 0);      // (the speakers are not a virtual sink)
  CHECK(service.open_capture(20, PcmFormat{SampleFormat::S16Le, 48000, 2}, "rec.exe", &wrong, false, 12345) != 0);
  CHECK(service.open(20, PcmFormat{SampleFormat::S16Le, 48000, 2}, "player.exe", &out, sink) == 0 && out);
  CHECK(service.open_capture(20, PcmFormat{SampleFormat::S16Le, 48000, 2}, "rec.exe", &in, false, sink) == 0 && in);
  const uint32_t speakers_streams = mixer.stream_count();
  CHECK(speakers_streams == 0);      // (into the sink's own mixer)
  std::vector<uint8_t> tone = s16_constant(4800, 2, 6000);
  CHECK(service.write(20, out, ConstByteView(tone.data(), tone.size()), &taken) == 0 && taken == tone.size());
  std::vector<uint8_t> heard(48000 * 4);
  uint64_t total = 0, loud = 0;
  for (int i = 0; i < 40 && loud < 2000; ++i) {
    g_now_ns += 5000000;
    g.refresh();
    CHECK(service.read(20, in, ByteView(heard.data(), heard.size()), &got) == 0);
    for (uint32_t k = 0; k + 1 < got; k += 2) {
      const int16_t v = int16_t(heard[k] | heard[k + 1] << 8);
      loud += abs(v - 6000) < 40;
    }
    total += got;
  }
  CHECK(loud >= 2000);
  dev.take();
  mixer.pump();
  std::vector<int16_t> speakers = dev.take();
  bool silent = true;
  for (int16_t v : speakers) silent &= v == 0;
  CHECK(silent);      // (nothing of it on the speakers)
  // its master volume: stream kVirtualSinkStream | its node
  uint32_t left = 0, right = 0;
  bool mute = true;
  CHECK(service.set_volume(20, WindowsAudioService::kVirtualSinkStream | sink, 0x8000, 0x8000) == 0);
  CHECK(service.get_volume(20, WindowsAudioService::kVirtualSinkStream | sink, &left, &right, &mute) == 0 && !mute && left > 0x7E00 && left < 0x8200);
  CHECK(service.get_volume(20, WindowsAudioService::kVirtualSinkStream | g.sink_id(), &left, &right, &mute) != 0);
  // the sink goes: its streams play and record nothing, and nothing breaks
  g.destroy_node(sink);
  CHECK(!g.is_virtual_sink(sink) && g.virtual_sinks(&sinks) && sinks.count() == 0);
  for (int i = 0; i < 5; ++i) {
    g_now_ns += 5000000;
    g.refresh();
    service.write(20, out, ConstByteView(tone.data(), tone.size()), &taken);
    service.read(20, in, ByteView(heard.data(), heard.size()), &got);
  }
  service.process_exited(20);
  CHECK(service.stream_count() == 0);
}

// ---- the cameras as video sources -------------------------------------------------------------------------------------------------------------------

struct FakeCamera final : hardware::ICamera {
  bool there = true, on = false;
  uint32_t starts = 0, stops = 0, frame = 0;
  const char* name() const override { return "Fake Webcam"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool present() const override { return there; }
  uint32_t mode_count() const override { return 2; }
  bool mode(uint32_t index, hardware::CameraMode* out) const override {
    if (index > 1) return false;
    *out = index == 0 ? hardware::CameraMode{320, 240, 15, hardware::VideoCodec::Raw} : hardware::CameraMode{640, 480, 30, hardware::VideoCodec::Raw};
    return true;
  }
  bool start_mode(uint32_t) override { return start(640, 480, 30); }
  bool start(uint32_t, uint32_t, uint32_t) override {
    if (!there) return false;
    on = true;
    ++starts;
    return true;
  }
  void stop() override { if (on) ++stops; on = false; }
  bool streaming() const override { return on; }
  hardware::VideoFormat frame_format() const override {
    hardware::VideoFormat f;
    if (on) { f.width = 640; f.height = 480; }
    return f;
  }
  bool limited_range() const override { return true; }
  void pump() override {}
  // a picture: luma = the picture's number, chroma = 100 + it
  bool read_frame(const hardware::VideoFrameBuffer& out) override {
    if (!on || out.format().pixel_format != hardware::PixelFormat::I420) return false;
    ++frame;
    out.plane(0).fill(uint8_t(frame));
    out.plane(1).fill(uint8_t(100 + frame));
    out.plane(2).fill(uint8_t(150 + frame));
    return true;
  }
  hardware::CameraStatistics statistics() const override { return {}; }
};

static void test_camera_sources() {
  Graph g(A, nullptr);
  CHECK(g.initialize());
  CameraSources sources(g, A);
  SharedPointer<FakeCamera> cam = SharedPointer<FakeCamera>::make(A);
  SharedPointer<hardware::ICamera> as_camera = SharedPointer<hardware::ICamera>(cam);
  CameraSources::Camera list[1] = {{7, as_camera}};
  uint64_t now = 1000000;
  sources.update(ArrayView<const CameraSources::Camera>(list, 1), now);
  const uint32_t node = sources.node_of(7);
  CHECK(sources.count() == 1 && node && g.find_node("tcos-camera-7") == node && !sources.streaming(7) && cam->starts == 0);
  bool described = false;
  g.visit(node, [&](const GraphObject& o) {
    described = o.props.get("media.class") == StringView("Video/Source") && o.props.get("node.description") == StringView("Fake Webcam") && o.client == g.native_client_id();
  });
  CHECK(described);
  // nobody watches: it stays off
  sources.update(ArrayView<const CameraSources::Camera>(list, 1), now += 20000);
  CHECK(!sources.streaming(7) && cam->starts == 0);
  // a watcher (a program's video input aimed at it): it starts, its pictures go out as I420 640 x 480
  const uint32_t client = g.add_client(Origin::Native, 1, "native", "test");
  Properties wprops(A);
  wprops.set("target.object", "tcos-camera-7");
  SharedPointer<VideoChannel> watched;
  const uint32_t watcher = g.create_video_node(client, wprops, kDirectionInput, &watched);
  CHECK(watcher && watched && g.video_watchers(node) == 1);
  sources.update(ArrayView<const CameraSources::Camera>(list, 1), now += 20000);
  CHECK(sources.streaming(7) && cam->starts == 1 && sources.published(7) == 1);
  if (watched) {
    const VideoFormat f = watched->format();
    CHECK(f.format == 2 && f.width == 640 && f.height == 480 && f.rate_num == 30);
    SharedPointer<VideoFrame> fr = watched->latest();
    CHECK(fr && fr->pixels.size() == 640 * 480 * 3 / 2);
    if (fr) CHECK(fr->pixels.view()[0] == 1 && fr->pixels.view()[640 * 480] == 101 && fr->pixels.view()[640 * 480 + 320 * 240] == 151);
  }
  for (int i = 0; i < 5; ++i) sources.update(ArrayView<const CameraSources::Camera>(list, 1), now += 20000);
  CHECK(sources.published(7) == 6 && watched && watched->frames() == 6);
  // the watcher goes: it keeps streaming a while, then stops
  g.destroy_node(watcher);
  CHECK(g.video_watchers(node) == 0);
  sources.update(ArrayView<const CameraSources::Camera>(list, 1), now += 500000);
  CHECK(sources.streaming(7));
  sources.update(ArrayView<const CameraSources::Camera>(list, 1), now += 2000000);
  CHECK(!sources.streaming(7) && cam->stops == 1);
  // a second watcher starts it again; then the camera is pulled out: its node goes, the watcher sees the channel closed
  SharedPointer<VideoChannel> again;
  const uint32_t second = g.create_video_node(client, wprops, kDirectionInput, &again);
  sources.update(ArrayView<const CameraSources::Camera>(list, 1), now += 20000);
  CHECK(second && sources.streaming(7) && cam->starts == 2);
  cam->there = false;
  sources.update(ArrayView<const CameraSources::Camera>(list, 1), now += 20000);
  CHECK(sources.count() == 0 && g.find_node("tcos-camera-7") == 0 && again && again->closed() && cam->stops == 2);
  // a camera that comes back (a new one) is a new node; one gone from the list goes too
  SharedPointer<FakeCamera> cam2 = SharedPointer<FakeCamera>::make(A);
  CameraSources::Camera list2[1] = {{8, SharedPointer<hardware::ICamera>(cam2)}};
  sources.update(ArrayView<const CameraSources::Camera>(list2, 1), now += 20000);
  CHECK(sources.count() == 1 && sources.node_of(8) && g.find_node("tcos-camera-8"));
  sources.update(ArrayView<const CameraSources::Camera>(), now += 20000);
  CHECK(sources.count() == 0 && g.find_node("tcos-camera-8") == 0);
  g.destroy_node(second);
}

// The screen as a video source: frames only while watched, at most kFps a second, published as BGRx at the screen's size (which may change).
static void test_screen_source() {
  Graph g(A, nullptr);
  CHECK(g.initialize());
  ScreenSource screen(g, A);
  CHECK(screen.start() && screen.node() && g.find_node("tcos-screen") == screen.node());
  uint64_t now = 1000000;
  CHECK(screen.frame_wanted(8, 4, now).empty() && !screen.watched() && !screen.watcher_came());      // (nobody watches)
  const uint32_t client = g.add_client(Origin::Native, 1, "native", "test");
  Properties props(A);
  props.set("target.object", "tcos-screen");
  SharedPointer<VideoChannel> seen;
  const uint32_t watcher = g.create_video_node(client, props, kDirectionInput, &seen);
  CHECK(watcher && seen && screen.watched() && screen.watcher_came() && !screen.watcher_came());
  ArrayView<uint32_t> frame = screen.frame_wanted(8, 4, now);
  CHECK(frame.size() == 32);
  for (uint32_t i = 0; i < frame.size(); ++i) frame[i] = 0xFF000000u | (i << 16) | (i << 8) | i;
  screen.frame_read(8, 4, 77);
  CHECK(screen.published() == 1 && seen->format().format == 8 && seen->format().width == 8 && seen->format().height == 4);
  SharedPointer<VideoFrame> f = seen->latest();
  CHECK(f && f->pixels.size() == 128 && f->pts == 77);
  if (f) CHECK(f->pixels.view()[4 * 5] == 5 && f->pixels.view()[4 * 5 + 2] == 5 && f->pixels.view()[4 * 5 + 3] == 0xFF);      // (B, G, R, A)
  CHECK(screen.frame_wanted(8, 4, now + 50000).empty());      // (too soon: 10 a second at most)
  ArrayView<uint32_t> bigger = screen.frame_wanted(16, 8, now + 100000);
  CHECK(bigger.size() == 128);
  screen.frame_read(16, 8, 78);
  CHECK(screen.published() == 2 && seen->format().width == 16 && seen->latest() && seen->latest()->pixels.size() == 512);
  CHECK(screen.frame_wanted(VideoFormat::kMaxSide + 1, 4, now + 300000).empty());
  // a still screen: the last frame again once a second, never while one is being read
  const uint64_t before = screen.published();
  CHECK(!screen.keep_alive(now + 600000) && screen.published() == before);      // (0.5 s after the last)
  CHECK(screen.keep_alive(now + 1200000) && screen.published() == before + 1 && seen->latest() && seen->latest()->pixels.size() == 512);
  CHECK(!screen.keep_alive(now + 1300000));
  ArrayView<uint32_t> filling = screen.frame_wanted(16, 8, now + 2300000);
  CHECK(!filling.empty() && !screen.keep_alive(now + 2400000));      // (being read)
  screen.frame_read(16, 8, 79);
  CHECK(screen.published() == before + 2);
  // the render thread and the loop at once: every frame that goes out is whole (all its pixels one value)
  {
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> clock{now + 10000000};
    std::thread render([&] {
      for (uint32_t value = 1; value < 400; ++value) {
        ArrayView<uint32_t> f2 = screen.frame_wanted(16, 8, clock.fetch_add(150000));
        if (f2.empty()) continue;
        for (uint32_t& p : f2) p = 0xFF000000u | value;
        screen.frame_read(16, 8, 0);
      }
      stop = true;
    });
    std::thread loop([&] {
      while (!stop.load()) {
        screen.keep_alive(clock.fetch_add(400000));
        std::this_thread::yield();
      }
    });
    int torn = 0, frames = 0;
    uint64_t last = seen->latest() ? seen->latest()->sequence : 0;      // (the frames before the threads were not filled in: not counted)
    while (!stop.load()) {
      SharedPointer<VideoFrame> fr = seen->latest();
      if (fr && fr->sequence != last) {
        last = fr->sequence;
        ++frames;
        const ConstByteView v = fr->pixels.view();
        for (uint64_t k = 4; k < v.size(); k += 4) if (v[k] != v[0]) { ++torn; break; }
      }
      std::this_thread::yield();
    }
    render.join();
    loop.join();
    if (torn || !frames) printf("screen threads: %d frames, %d torn\n", frames, torn);
    CHECK(torn == 0 && frames > 0);
  }
  g.destroy_node(watcher);
  CHECK(!screen.watched() && screen.frame_wanted(8, 4, now + 400000).empty());
}

// ---- encoded video ----------------------------------------------------------------------------------------------------------------------------------

// The software codecs as the graph's decoders (what the kernel's SystemVideoCodecs is to it).
struct SoftwareDecoders final : IVideoDecoders {
  hardware::SoftwareVideoCodec device{A};
  int made = 0;
  bool can_decode(hardware::VideoCodec codec) const override { return device.can_decode(codec); }
  UniquePointer<hardware::IVideoDecoder> create_decoder(const hardware::DecoderConfig& config) override {
    ++made;
    return device.create_decoder(config);
  }
};

// A moving test picture (0xAARRGGBB).
static std::vector<uint32_t> test_picture(uint32_t w, uint32_t h, uint32_t frame) {
  std::vector<uint32_t> px(size_t(w) * h);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t r = (x * 255 / (w - 1) + frame * 8) & 0xFF, g = y * 255 / (h - 1), b = ((x / 8 + y / 8 + frame) & 1) ? 200 : 40;
      px[size_t(y) * w + x] = 0xFF000000u | r << 16 | g << 8 | b;
    }
  return px;
}

// `count` pictures as H.264 access units (Annex B), from the software encoder.
static std::vector<std::vector<uint8_t>> encode_h264(uint32_t w, uint32_t h, uint32_t count, uint32_t keyframe_interval) {
  std::vector<std::vector<uint8_t>> out;
  hardware::SoftwareVideoCodec device(A);
  hardware::EncoderConfig ec;
  ec.codec = hardware::VideoCodec::H264;
  ec.width = w;
  ec.height = h;
  ec.input = hardware::PixelFormat::Bgra8888;
  ec.quality = 90;
  ec.keyframe_interval = keyframe_interval;
  UniquePointer<hardware::IVideoEncoder> enc = device.create_encoder(ec);
  if (!enc) return out;
  for (uint32_t i = 0; i < count; ++i) {
    std::vector<uint32_t> px = test_picture(w, h, i);
    hardware::VideoFrame f;
    f.format = {w, h, hardware::PixelFormat::Bgra8888};
    f.plane[0] = words_as_bytes(ArrayView<const uint32_t>(px.data(), px.size())).data();
    f.stride[0] = w * 4;
    f.pts_us = int64_t(i) * 33333;
    if (enc->submit(f) != hardware::SubmitResult::Accepted) return {};
    hardware::VideoPacket pkt;
    while (enc->receive(&pkt)) out.emplace_back(pkt.data.data(), pkt.data.data() + pkt.data.size());
  }
  enc->flush();
  hardware::VideoPacket pkt;
  while (enc->receive(&pkt)) out.emplace_back(pkt.data.data(), pkt.data.data() + pkt.data.size());
  return out;
}

// The frames of an IVF file (VP8, VP9).
static std::vector<std::vector<uint8_t>> ivf_frames(const std::vector<uint8_t>& file) {
  std::vector<std::vector<uint8_t>> out;
  if (file.size() < 32 || memcmp(file.data(), "DKIF", 4) != 0) return out;
  const size_t header = size_t(file[6]) | size_t(file[7]) << 8;
  for (size_t at = header; at + 12 <= file.size();) {
    const size_t n = size_t(file[at]) | size_t(file[at + 1]) << 8 | size_t(file[at + 2]) << 16 | size_t(file[at + 3]) << 24;
    at += 12;
    if (n > file.size() - at) break;
    out.emplace_back(file.begin() + long(at), file.begin() + long(at + n));
    at += n;
  }
  return out;
}

// The pictures a decoder of the software codecs gives for `packets`, in order, as BGRA bytes (what the graph's decoding must match).
static std::vector<std::vector<uint8_t>> reference_pictures(hardware::VideoCodec codec, const std::vector<std::vector<uint8_t>>& packets, uint32_t* w,
                                                             uint32_t* h) {
  std::vector<std::vector<uint8_t>> out;
  hardware::SoftwareVideoCodec device(A);
  hardware::DecoderConfig config;
  config.codec = codec;
  config.output = hardware::PixelFormat::Bgra8888;
  UniquePointer<hardware::IVideoDecoder> dec = device.create_decoder(config);
  if (!dec) return out;
  auto take = [&] {
    hardware::VideoFrame f;
    while (dec->receive(&f)) {
      *w = f.format.width;
      *h = f.format.height;
      std::vector<uint8_t> px(size_t(f.format.width) * f.format.height * 4);
      for (uint32_t y = 0; y < f.format.height; ++y) memcpy(px.data() + size_t(y) * f.format.width * 4, f.plane[0] + size_t(y) * f.stride[0], f.format.width * 4u);
      out.push_back(std::move(px));
    }
  };
  int64_t pts = 0;
  for (const std::vector<uint8_t>& p : packets) {
    hardware::VideoPacket pkt;
    pkt.data = ConstByteView(p.data(), p.size());
    pkt.pts_us = pts;
    pkt.keyframe = false;
    pts += 33333;
    while (dec->submit(pkt) == hardware::SubmitResult::Full) take();
    take();
  }
  return out;
}

// VideoDecodeSession: H.264 (from the encoder) and VP8 (a file) decoded as a decoder of the codecs would, the codec switched in the middle; without
// decoders only Motion JPEG; truncated and bit-flipped frames are refused or decoded, never read past; decode_video to another format and size.
static void test_video_decode() {
  SoftwareDecoders decoders;
  CHECK(can_decode_subtype(kSubtypeMjpg, nullptr) && !can_decode_subtype(kSubtypeH264, nullptr) && can_decode_subtype(kSubtypeH264, &decoders) &&
        can_decode_subtype(kSubtypeVp9, &decoders) && can_decode_subtype(kSubtypeH265, &decoders) && !can_decode_subtype(0x20003, &decoders) &&
        !can_decode_subtype(VideoFormat::kSubtypeRaw, &decoders));
  hardware::VideoCodec codec;
  CHECK(codec_of_subtype(kSubtypeVc1, &codec) && codec == hardware::VideoCodec::Vc1 && !codec_of_subtype(kSubtypeMjpg, &codec));
  const std::vector<std::vector<uint8_t>> h264 = encode_h264(64, 48, 8, 4);
  CHECK(h264.size() == 8);
  uint32_t rw = 0, rh = 0;
  const std::vector<std::vector<uint8_t>> want = reference_pictures(hardware::VideoCodec::H264, h264, &rw, &rh);
  CHECK(want.size() == 8 && rw == 64 && rh == 48);
  VideoDecodeSession session(A);
  VideoFormat f;
  ConstByteView px;
  bool failed = false;
  CHECK(!session.decode(kSubtypeH264, ConstByteView(h264[0].data(), h264[0].size()), nullptr, &f, &px, &failed) && failed);      // (no decoders)
  session.reset();
  size_t matched = 0;
  for (size_t i = 0; i < h264.size(); ++i) {
    const bool got = session.decode(kSubtypeH264, ConstByteView(h264[i].data(), h264[i].size()), &decoders, &f, &px, &failed);
    CHECK(got && !failed && f.format == 12 && f.width == 64 && f.height == 48 && px.size() == 64 * 48 * 4);
    if (got && i < want.size() && px.size() == want[i].size() && memcmp(px.data(), want[i].data(), px.size()) == 0) ++matched;
    if (got && px.size() == 64 * 48 * 4) {      // (close to the picture encoded)
      const std::vector<uint32_t> src = test_picture(64, 48, uint32_t(i));
      uint64_t err = 0;
      for (size_t k = 0; k < src.size(); ++k)
        for (int c = 0; c < 3; ++c) err += uint64_t(abs(int((src[k] >> (8 * c)) & 0xFF) - int(px[4 * k + size_t(c)])));
      CHECK(err / (src.size() * 3) < 12);
    }
  }
  CHECK(matched == h264.size() && decoders.made == 1);
  // VP8 from a file, then H.264 again (each switch a new decoder)
  const std::vector<std::vector<uint8_t>> vp8 = ivf_frames(read_test_file("tests/data/vp8_inter.ivf"));
  CHECK(vp8.size() >= 4);
  const std::vector<std::vector<uint8_t>> want8 = reference_pictures(hardware::VideoCodec::Vp8, vp8, &rw, &rh);
  CHECK(want8.size() == vp8.size());
  matched = 0;
  for (size_t i = 0; i < vp8.size(); ++i)
    if (session.decode(kSubtypeVp8, ConstByteView(vp8[i].data(), vp8[i].size()), &decoders, &f, &px, &failed) && i < want8.size() &&
        f.width == rw && f.height == rh && px.size() == want8[i].size() && memcmp(px.data(), want8[i].data(), px.size()) == 0)
      ++matched;
  CHECK(matched == vp8.size() && decoders.made == 2);
  CHECK(session.decode(kSubtypeH264, ConstByteView(h264[0].data(), h264[0].size()), &decoders, &f, &px, &failed) && f.width == 64 && decoders.made == 3);
  // Motion JPEG through the same session
  const std::vector<uint8_t> jpeg = read_test_file("tests/data/mjpeg_standard_tables.jpg");
  CHECK(session.decode(kSubtypeMjpg, ConstByteView(jpeg.data(), jpeg.size()), nullptr, &f, &px, &failed) && f.width == 64 && f.height == 48 &&
        px.size() == 64 * 48 * 4);
  // to NV12 at half size: decoded, then converted and scaled
  ByteBuffer out(A);
  VideoFormat from;
  from.subtype = kSubtypeH264;
  VideoFormat nv12;
  nv12.format = 23; nv12.width = 32; nv12.height = 24;
  VideoDecodeSession s2(A);
  CHECK(decode_video(ConstByteView(h264[0].data(), h264[0].size()), from, nv12, s2, &decoders, &out, &failed) && out.size() == nv12.frame_bytes());
  CHECK(can_decode_video(from, 23, &decoders) && !can_decode_video(from, 23, nullptr));
  VideoFormat enc_to = nv12;
  enc_to.subtype = kSubtypeH264;
  CHECK(!decode_video(ConstByteView(h264[1].data(), h264[1].size()), from, enc_to, s2, &decoders, &out, &failed) && failed);
  // truncations and bit flips of a keyframe and of the frames after it: refused or decoded, never past the frame's bytes (ASan)
  std::mt19937 rng(7);
  for (int round = 0; round < 2; ++round) {
    const std::vector<std::vector<uint8_t>>& stream = round == 0 ? h264 : vp8;
    const uint32_t subtype = round == 0 ? kSubtypeH264 : kSubtypeVp8;
    for (size_t cut = 0; cut < stream[0].size(); cut += 1 + stream[0].size() / 40) {
      VideoDecodeSession t(A);
      std::vector<uint8_t> part(stream[0].begin(), stream[0].begin() + long(cut));
      t.decode(subtype, ConstByteView(part.data(), part.size()), &decoders, &f, &px, &failed);
      for (size_t k = 1; k < 3 && k < stream.size(); ++k) t.decode(subtype, ConstByteView(stream[k].data(), stream[k].size()), &decoders, &f, &px, &failed);
    }
    for (int flip = 0; flip < 60; ++flip) {
      VideoDecodeSession t(A);
      for (size_t k = 0; k < 3 && k < stream.size(); ++k) {
        std::vector<uint8_t> bad = stream[k];
        for (int b = 0; b < 3; ++b) bad[rng() % bad.size()] ^= uint8_t(1u << (rng() % 8));
        if (t.decode(subtype, ConstByteView(bad.data(), bad.size()), &decoders, &f, &px, &failed)) CHECK(f.valid() && px.size() == uint64_t{f.width} * f.height * 4);
      }
    }
  }
}

// The kernel's own watcher (the Video Viewer's): the producers listed, one watched by a node of the kernel's (so a camera would start), its frames as
// 0xAARRGGBB words, converted from YUV, encoded video and a producer that went said so.
static void test_video_watcher() {
  Graph g(A, nullptr);
  CHECK(g.initialize());
  ScreenSource screen(g, A);
  CHECK(screen.start());
  const uint32_t client = g.add_client(Origin::Native, 1, "native", "camera-app");
  Properties props(A);
  props.set("node.name", "app-camera");
  props.set("media.class", "Video/Source");
  SharedPointer<VideoChannel> cam;
  const uint32_t camera = g.create_video_node(client, props, kDirectionOutput, &cam);
  CHECK(camera && cam);
  VideoWatcher w(g, A);
  List<uint32_t> ids(A);
  CHECK(w.producers(&ids) && ids.count() == 2 && ids[0] == screen.node() && ids[1] == camera);
  String name(A);
  CHECK(w.name_of(screen.node(), &name) && name.view() == StringView("Screen") && w.name_of(camera, &name) && name.view() == StringView("app-camera"));
  CHECK(!w.watch(client) && w.watch(screen.node()) && g.video_watchers(screen.node()) == 1 && g.video_watchers(camera) == 0);
  uint64_t seq = 0;
  List<uint32_t> px(A);
  uint32_t width = 0, height = 0;
  StringView why;
  CHECK(!w.latest(&seq, &px, &width, &height, &why) && why.empty());      // (nothing yet)
  ArrayView<uint32_t> f = screen.frame_wanted(4, 2, 1000000);
  CHECK(f.size() == 8);
  for (uint32_t i = 0; i < 8; ++i) f[i] = 0xFF102030u + i;
  screen.frame_read(4, 2, 0);
  CHECK(w.latest(&seq, &px, &width, &height, &why) && width == 4 && height == 2 && px.count() == 8 && px[3] == 0xFF102033u && seq == 1);
  CHECK(!w.latest(&seq, &px, &width, &height, &why));      // (not newer)
  // the camera, in I420 (grey: Y 126 is mid grey)
  CHECK(w.watch(camera) && g.video_watchers(screen.node()) == 0 && g.video_watchers(camera) == 1);
  VideoFormat i420;
  i420.format = 2; i420.width = 4; i420.height = 2;
  cam->set_format(i420);
  std::vector<uint8_t> yuv(8 + 2 + 2, 128);
  for (int i = 0; i < 8; ++i) yuv[size_t(i)] = 126;
  CHECK(cam->publish(ConstByteView(yuv.data(), 8), 4, 0) == false);      // (one plane is not enough)
  const VideoPlaneData planes[3] = {{ConstByteView(yuv.data(), 8), 4}, {ConstByteView(yuv.data() + 8, 2), 2}, {ConstByteView(yuv.data() + 10, 2), 2}};
  CHECK(cam->publish(ArrayView<const VideoPlaneData>(planes, 3), 0));
  seq = 0;
  CHECK(w.latest(&seq, &px, &width, &height, &why) && px.count() == 8);
  if (px.count() == 8) CHECK((px[0] >> 24) == 0xFF && abs(int((px[0] >> 16) & 0xFF) - 128) < 4 && abs(int(px[0] & 0xFF) - 128) < 4);
  // Motion JPEG: decoded (a real file, 64 x 48); a broken one said; other encoded video said, not shown
  VideoFormat mjpg;
  mjpg.subtype = 0x20002;
  cam->set_format(mjpg);
  const std::vector<uint8_t> jpeg = read_test_file("tests/data/mjpeg_standard_tables.jpg");
  CHECK(jpeg.size() > 100 && cam->publish(ConstByteView(jpeg.data(), jpeg.size()), 0, 0));
  CHECK(w.latest(&seq, &px, &width, &height, &why) && width == 64 && height == 48 && px.count() == 64 * 48 && (px[0] >> 24) == 0xFF);
  const uint8_t broken[4] = {0xFF, 0xD8, 0xFF, 0xD9};
  CHECK(cam->publish(ConstByteView(broken, 4), 0, 0));
  CHECK(!w.latest(&seq, &px, &width, &height, &why) && why == StringView("A picture that cannot be decoded"));
  // H.264: said, not shown, while the graph has no decoders; decoded once it has (each picture as a decoder of the codecs gives it)
  VideoFormat h264;
  h264.subtype = 0x20001;
  cam->set_format(h264);
  const std::vector<std::vector<uint8_t>> avc = encode_h264(32, 16, 3, 30);
  CHECK(avc.size() == 3 && cam->publish(ConstByteView(avc[0].data(), avc[0].size()), 0, 0));
  CHECK(!w.latest(&seq, &px, &width, &height, &why) && why == StringView("Encoded video this computer cannot decode"));
  SoftwareDecoders decoders;
  g.set_video_decoders(&decoders);
  uint32_t rw = 0, rh = 0;
  const std::vector<std::vector<uint8_t>> want = reference_pictures(hardware::VideoCodec::H264, avc, &rw, &rh);
  CHECK(want.size() == 3);
  for (size_t i = 0; i < avc.size(); ++i) {
    CHECK(cam->publish(ConstByteView(avc[i].data(), avc[i].size()), 0, 0));
    const bool shown = w.latest(&seq, &px, &width, &height, &why);
    CHECK(shown && width == 32 && height == 16 && px.count() == 32 * 16);
    if (shown && i < want.size() && px.count() == 32 * 16) CHECK(memcmp(px.view().data(), want[i].data(), want[i].size()) == 0);
  }
  CHECK(cam->publish(ConstByteView(broken, 4), 0, 0));
  CHECK(!w.latest(&seq, &px, &width, &height, &why) && why == StringView("A picture that cannot be decoded"));
  g.set_video_decoders(nullptr);
  // the producer goes
  g.destroy_node(camera);
  CHECK(!w.latest(&seq, &px, &width, &height, &why) && why == StringView("The source went"));
  CHECK(w.watch(0) && w.watching() == 0);
}

// ---- the AF_UNIX transport -----------------------------------------------------------------------------------------------------------------------

struct SystemSync final : process::ISystemSync {
  std::mutex m;
  std::condition_variable cv;
  static thread_local std::unique_lock<std::mutex>* mine;
  void lock() override { mine = new std::unique_lock<std::mutex>(m); }
  void unlock() override { delete mine; mine = nullptr; }
  bool wait(uint64_t ns) override {
    if (ns == ~uint64_t{0}) { cv.wait(*mine); return true; }
    return cv.wait_for(*mine, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout;
  }
  void notify_all() override { cv.notify_all(); }
};
thread_local std::unique_lock<std::mutex>* SystemSync::mine = nullptr;

static void test_sockets() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  filesystem::RamFileSystem fs(A);
  CHECK(fs.create_directory("/Linux") == filesystem::Status::Ok);
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);
  CHECK(g.initialize());
  NativeServer native(g, A);
  PulseServer pulse(g, A);
  SocketServer server(sockets, A);
  CHECK(listen_standard_sockets(server, native, pulse, &fs, A));
  UniquePointer<filesystem::IFile> file;
  CHECK(fs.open_file("/Linux/run/user/0/pulse/native", filesystem::OpenMode::Read, &file) == filesystem::Status::Ok);
  file.reset();
  // a client connects by path to the pulse socket and authenticates
  uint32_t sock = 0;
  CHECK(sockets.create(process::UnixType::Stream, 9, &sock) == process::UnixStatus::Ok);
  const char* path = "/run/user/0/pulse/native";
  sync.lock();
  const process::UnixStatus st = sockets.connect_locked(sock, process::UnixName::from(process::UnixName::Kind::Path, StringView(path).bytes()), "/Linux/run/user/0/pulse/native", 9);
  sync.unlock();
  CHECK(st == process::UnixStatus::Ok);
  ByteBuffer body(A);
  pulse::TagWriter w(body);
  w.u32(8); w.u32(1); w.u32(35);
  uint8_t cookie[256] = {};
  w.arbitrary(ConstByteView(cookie, 256));
  ByteBuffer pkt(A);
  const uint32_t words[5] = {uint32_t(body.size()), 0xFFFFFFFFu, 0, 0, 0};
  for (uint32_t x : words) { const uint8_t b[4] = {uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x)}; pkt.append(ConstByteView(b, 4)); }
  pkt.append(body.view());
  sync.lock();
  uint32_t sent = 0;
  CHECK(sockets.send_locked(sock, pkt.view(), nullptr, 9, nullptr, &sent) == process::UnixStatus::Ok && sent == pkt.size());
  sync.unlock();
  for (int i = 0; i < 10; ++i) server.pump();
  CHECK(server.connections() == 1 && pulse.client_count() == 1);
  uint8_t reply[64];
  process::UnixSocketManager::Received got;
  sync.lock();
  const process::UnixStatus rs = sockets.receive_locked(sock, ByteView(reply, 64), false, &got, nullptr);
  sync.unlock();
  CHECK(rs == process::UnixStatus::Ok && got.got == 20 + 15 && reply[4] == 0xFF);
  // the native socket by its abstract name
  uint32_t sock2 = 0;
  CHECK(sockets.create(process::UnixType::Stream, 9, &sock2) == process::UnixStatus::Ok);
  sync.lock();
  CHECK(sockets.connect_locked(sock2, process::UnixName::from(process::UnixName::Kind::Abstract, StringView("/run/user/0/pipewire-0").bytes()), StringView(), 9) == process::UnixStatus::Ok);
  sync.unlock();
  for (int i = 0; i < 5; ++i) server.pump();
  CHECK(native.client_count() == 1);
  // the client goes: so does its connection
  sockets.release(sock);
  sockets.release(sock2);
  for (int i = 0; i < 5; ++i) server.pump();
  CHECK(server.connections() == 0 && pulse.client_count() == 0 && native.client_count() == 0);
}

// ---- client-node through the kernel's transport: memfds and eventfds made by SocketServer, passed with SCM_RIGHTS ---------------------------------

// A descriptor the client received: a memfd's block (the same memory the server writes), or an eventfd of the socket manager.
struct ReceivedFd final : Descriptor {
  process::UnixSocketManager& sockets;
  process::UnixInFlight entry;
  process::BlockRef block;
  ReceivedFd(process::UnixSocketManager& m, process::UnixInFlight e) : sockets(m), entry(util::move(e)) {
    if (entry.object && entry.object->memory) {
      uint64_t size = 0;
      block = entry.object->memory->block(&size);
    }
  }
  ~ReceivedFd() { if (entry.kind == process::DescriptorKind::UnixSocket && entry.token) sockets.release(uint32_t(entry.token)); }
  ByteView memory() override { return block ? block.get()->bytes() : ByteView(); }
  bool signal() override {
    sockets.sync().lock();
    const bool ok = sockets.event_add_locked(uint32_t(entry.token), 1) == process::UnixStatus::Ok;
    sockets.sync().unlock();
    return ok;
  }
  uint64_t take_signals() override {
    uint64_t v = 0;
    sockets.sync().lock();
    sockets.event_take_locked(uint32_t(entry.token), &v);
    sockets.sync().unlock();
    return v;
  }
};

struct SocketTransport final : NativeTransport {
  process::UnixSocketManager& sockets;
  SocketServer& server;
  uint32_t sock;
  uint32_t received_events = 0, received_memory = 0;
  SocketTransport(process::UnixSocketManager& m, SocketServer& s, uint32_t id) : sockets(m), server(s), sock(id) {}
  void send(ConstByteView bytes) override {
    sockets.sync().lock();
    uint32_t sent = 0;
    CHECK(sockets.send_locked(sock, bytes, nullptr, 9, nullptr, &sent) == process::UnixStatus::Ok && sent == bytes.size());
    sockets.sync().unlock();
  }
  void take(ByteBuffer* out, std::vector<Fd>* fds) override {
    for (int i = 0; i < 4; ++i) server.pump();
    uint8_t chunk[4096];
    for (;;) {
      process::UnixSocketManager::Received got;
      Queue<process::UnixInFlight> rights(A);
      sockets.sync().lock();
      const process::UnixStatus st = sockets.receive_locked(sock, ByteView(chunk, sizeof chunk), false, &got, &rights);
      sockets.sync().unlock();
      process::UnixInFlight e;
      while (rights.dequeue(&e)) {
        if (e.kind == process::DescriptorKind::UnixSocket) ++received_events;
        else if (e.object && e.object->memory) ++received_memory;
        fds->push_back(Fd::make_as<ReceivedFd>(A, sockets, util::move(e)));
      }
      if (st != process::UnixStatus::Ok || got.got == 0) break;
      out->append(ConstByteView(chunk, got.got));
    }
  }
};

static void test_socket_client_node() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);
  CHECK(g.initialize());
  NativeServer native(g, A);
  PulseServer pulse(g, A);
  const int live_before = A.live.load();
  {
    SocketServer server(sockets, A);
    CHECK(listen_standard_sockets(server, native, pulse, nullptr, A));
    uint32_t sock = 0;
    CHECK(sockets.create(process::UnixType::Stream, 9, &sock) == process::UnixStatus::Ok);
    sync.lock();
    CHECK(sockets.connect_locked(sock, process::UnixName::from(process::UnixName::Kind::Abstract, StringView("/run/user/0/pipewire-0").bytes()), StringView(), 9) ==
          process::UnixStatus::Ok);
    sync.unlock();
    SocketTransport t(sockets, server, sock);
    {
      NativeClient c(native, t);
      c.send(0, 1, [](pod::Builder& b) { b.int32(4); });
      c.take_with_fds();
      ExportedNode play = export_node(c, 3, "Stream/Output/Audio", 1);
      // the activation, the buffers and the io area came as memfds; the wake and done fds as eventfds
      CHECK(play.ok && t.received_memory >= 3 && t.received_events == 2 && g.playback_count() == 1);
      if (play.ok) {
        ByteView act = play.activation->memory(), io = play.io->memory(), bufs = play.buffers->memory();
        CHECK(act.size() >= 2312 && u32_at(act, 0) == 4 && u32_at(act, 544) == 1);      // what the server wrote, seen through the client's memory
        put_u32(act, 0, 3);
        for (int i = 0; i < 3; ++i) server.pump();
        CHECK(native.running_nodes() == 1 && u32_at(act, 0) == 1 && play.wake->take_signals() == 1);      // triggered through the eventfd
        const uint64_t base = 1ull * play.buffer_stride;
        for (uint32_t i = 0; i < 1024; ++i) {
          const float l = 0.5f, r = 0.5f;
          memcpy(bufs.data() + base + play.data_offset + 8 * i, &l, 4);
          memcpy(bufs.data() + base + play.data_offset + 8 * i + 4, &r, 4);
        }
        put_u32(bufs, base, 0); put_u32(bufs, base + 4, 8192);
        put_u32(io, 0, 2); put_u32(io, 4, 1);
        put_u32(act, 0, 3);
        dev.take();
        server.pump();
        CHECK(u32_at(io, 0) == 1 && u32_at(io, 4) == 1);
        mixer.pump();
        std::vector<int16_t> out = dev.take();
        CHECK(out.size() > 400 && abs(out[200] - 16383) < 60);
      }
    }
    // the client goes: its node, connection and every descriptor with them
    sockets.release(sock);
    for (int i = 0; i < 5; ++i) server.pump();
    CHECK(server.connections() == 0 && native.client_count() == 0 && g.playback_count() == 0);
  }
  // nothing is left: every memfd, eventfd and socket was let go
  printf("socket client-node: %d allocations still held\n", A.live.load() - live_before);
  CHECK(A.live.load() <= live_before + 8);
}

// ---- threads -------------------------------------------------------------------------------------------------------------------------------------

static void test_threads() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  Graph g(A, &mixer);
  CHECK(g.initialize());
  NativeServer native(g, A);
  PulseServer pulse(g, A);
  WindowsAudioService service(g, A);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> windows_bytes{0}, pumped{0};
  // the mixer's thread
  std::thread mix([&] { while (!stop) { mixer.pump(); dev.play_all(); dev.take(); ++pumped; std::this_thread::yield(); } });
  // the protocol thread: ticks, a native client that keeps asking for the registry, a pulse client that plays
  std::thread proto([&] {
    NativeClient c(native);
    c.send(0, 1, [](pod::Builder& b) { b.int32(4); });
    c.send(0, 5, [](pod::Builder& b) { b.int32(3); b.int32(2); });
    PulseClient p(pulse);
    uint8_t cookie[256] = {};
    p.command(8, [&](pulse::TagWriter& w) { w.u32(35); w.arbitrary(ConstByteView(cookie, 256)); });
    p.command(3, [&](pulse::TagWriter& w) { put_create_playback(w, pulse::SampleSpec{pulse::kS16Le, 2, 48000}, 9600, "t"); });
    std::vector<uint8_t> chunk = s16_constant(480, 2, 100);
    for (int i = 0; !stop; ++i) {
      native.tick();
      pulse.tick();
      p.packet(0, ConstByteView(chunk.data(), chunk.size()));
      c.send(0, 2, [&](pod::Builder& b) { b.int32(0); b.int32(i); });
      c.take();
      p.take();
    }
    native.close_client(c.handle);
    pulse.close_client(p.handle);
  });
  // Windows programs: open, write, close, over and over, from four threads
  std::vector<std::thread> programs;
  for (int t = 0; t < 4; ++t)
    programs.emplace_back([&, t] {
      std::vector<uint8_t> pcm = s16_constant(1024, 2, int16_t(10 * t));
      for (int round = 0; round < 60; ++round) {
        uint32_t s = 0, taken = 0;
        if (service.open(1000 + t, PcmFormat{SampleFormat::S16Le, 44100, 2}, "w", &s) != 0) continue;
        for (int k = 0; k < 5; ++k) {
          service.write(1000 + t, s, ConstByteView(pcm.data(), pcm.size()), &taken);
          windows_bytes += taken;
        }
        service.close(1000 + t, s, round % 2 == 0);
        if (round % 10 == 9) service.process_exited(1000 + t);
      }
    });
  // native streams come and go on the mixer
  std::thread natives([&] {
    std::vector<uint8_t> pcm = s16_constant(256, 2, 7);
    for (int i = 0; i < 100; ++i) {
      SharedPointer<audio::AudioStream> s = mixer.open_stream(audio::StreamFormat(), 4096);
      if (!s) continue;
      s->set_label("native");
      s->write(ConstByteView(pcm.data(), pcm.size()));
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      s->close();
    }
  });
  for (std::thread& p : programs) p.join();
  natives.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop = true;
  proto.join();
  mix.join();
  CHECK(windows_bytes > 0 && pumped > 0);
  for (int t = 0; t < 4; ++t) service.process_exited(1000 + t);
  drain_mixer(mixer, dev, 50);
  g.refresh();
  CHECK(service.stream_count() == 0 && g.playback_count() == 0);
}

// ---- MIDI: the bus, its messages and UMP -------------------------------------------------------------------------------------------------------------


struct FakeMidiSink final : midi::IMidiSink {
  std::mutex m;
  std::vector<std::array<uint8_t, 3>> got;
  std::vector<std::vector<uint8_t>> sysex;
  std::vector<uint64_t> when;      // (g_now_ns at each message, sysex too)
  void send(uint8_t status, uint8_t d1, uint8_t d2) override { std::lock_guard<std::mutex> g(m); got.push_back({status, d1, d2}); when.push_back(g_now_ns); }
  void send_sysex(ConstByteView x) override {
    std::lock_guard<std::mutex> g(m);
    sysex.emplace_back(x.data(), x.data() + x.size());
    when.push_back(g_now_ns);
  }
  void reset() override {}
  size_t count() { std::lock_guard<std::mutex> g(m); return got.size(); }
};

static void test_midi_bus() {
  // raw bytes: running status, system exclusive skipped, real time inside a message, a cut-short message dropped
  const uint8_t bytes[] = {0x90, 60, 100, 62, 90, 0xF0, 1, 2, 3, 0xF7, 0xB0, 7, 0xF8, 64, 0xC2, 5, 0xE0, 0x10};
  std::vector<MidiMessage> seen;
  const uint32_t n = parse_midi_bytes(ConstByteView(bytes, sizeof bytes), [&](const MidiMessage& m) { seen.push_back(m); });
  CHECK(n == 5 && seen.size() == 5);
  if (seen.size() == 5) {
    CHECK(seen[0].bytes[0] == 0x90 && seen[0].bytes[1] == 60 && seen[0].bytes[2] == 100 && seen[0].size == 3);
    CHECK(seen[1].bytes[0] == 0x90 && seen[1].bytes[1] == 62 && seen[1].bytes[2] == 90);      // (running status)
    CHECK(seen[2].bytes[0] == 0xF8 && seen[2].size == 1);                                    // (clock, in the middle of the controller)
    CHECK(seen[3].bytes[0] == 0xB0 && seen[3].bytes[1] == 7 && seen[3].bytes[2] == 64);
    CHECK(seen[4].bytes[0] == 0xC2 && seen[4].bytes[1] == 5 && seen[4].size == 2);
  }
  CHECK(midi_message_size(0xF0) == 0 && midi_message_size(0x40) == 0 && midi_message_size(0xF2) == 3 && midi_message_size(0xD3) == 2);
  // UMP: MIDI 1.0 channel voice, system, MIDI 2.0 channel voice scaled down; round trips
  MidiMessage m;
  const uint32_t w1[1] = {0x20903C40};
  CHECK(ump_to_midi(ArrayView<const uint32_t>(w1, 1), &m) && m.bytes[0] == 0x90 && m.bytes[1] == 0x3C && m.bytes[2] == 0x40 && m.size == 3);
  CHECK(midi_to_ump(m) == 0x20903C40);
  const uint32_t sys[1] = {0x10F80000};
  CHECK(ump_to_midi(ArrayView<const uint32_t>(sys, 1), &m) && m.bytes[0] == 0xF8 && m.size == 1);
  const uint32_t v2[2] = {0x40913C00, 0xFFFF0000};      // note on, velocity 65535 -> 127
  CHECK(ump_to_midi(ArrayView<const uint32_t>(v2, 2), &m) && m.bytes[0] == 0x91 && m.bytes[1] == 0x3C && m.bytes[2] == 127);
  const uint32_t v2zero[2] = {0x40913C00, 0x00000000};      // (a MIDI 2.0 note on of velocity 0 is still a note on)
  CHECK(ump_to_midi(ArrayView<const uint32_t>(v2zero, 2), &m) && m.bytes[2] == 1);
  const uint32_t bend[2] = {0x40E00000, 0x80000000};      // the middle: 8192
  CHECK(ump_to_midi(ArrayView<const uint32_t>(bend, 2), &m) && m.bytes[1] == 0 && m.bytes[2] == 0x40);
  const uint32_t prog[2] = {0x40C50000, 0x2A000000};
  CHECK(ump_to_midi(ArrayView<const uint32_t>(prog, 2), &m) && m.bytes[0] == 0xC5 && m.bytes[1] == 0x2A && m.size == 2);
  const uint32_t data64[2] = {0x30160102, 0x03040506};      // (a SysEx7 packet: not MIDI 1.0)
  CHECK(!ump_to_midi(ArrayView<const uint32_t>(data64, 2), &m) && ump_packet_words(0x30000000) == 2 && ump_packet_words(0xF0000000) == 4);
  CHECK(!ump_to_midi(ArrayView<const uint32_t>(v2, 1), &m));      // (cut short)
  // every truncation and bit flips of bytes: no crash, never more messages than bytes
  std::mt19937 rng(5);
  for (int round = 0; round < 3000; ++round) {
    std::vector<uint8_t> b(bytes, bytes + sizeof bytes);
    if (round % 2) b.resize(rng() % (b.size() + 1));
    else for (int k = 0; k < 3; ++k) b[rng() % b.size()] ^= uint8_t(1u << (rng() % 8));
    uint32_t count = 0;
    parse_midi_bytes(ConstByteView(b.data(), b.size()), [&](const MidiMessage& x) { ++count; CHECK(x.size >= 1 && x.size <= 3); });
    CHECK(count <= b.size());
    uint32_t words[4] = {uint32_t(rng()), uint32_t(rng()), uint32_t(rng()), uint32_t(rng())};
    MidiMessage u;
    if (ump_to_midi(ArrayView<const uint32_t>(words, 1 + rng() % 4), &u)) CHECK(u.size >= 1 && u.size <= 3 && u.bytes[0] >= 0x80);
  }
  // the bus: a program's notes to the output and the other programs, not back to itself; a keyboard's to the programs only
  MidiBus bus(A);
  FakeMidiSink out;
  bus.set_output(&out);
  SharedPointer<MidiQueue> a = bus.listen(10), b = bus.listen(20);
  CHECK(a && b);
  MidiMessage note;
  note.bytes[0] = 0x90; note.bytes[1] = 64; note.bytes[2] = 80; note.size = 3; note.source = 10;
  bus.publish(note);
  MidiMessage got;
  CHECK(out.count() == 1 && !a->pop(&got) && b->pop(&got) && got.bytes[1] == 64 && got.source == 10);
  bus.send(0x80, 64, 0);
  CHECK(out.count() == 1 && a->pop(&got) && got.bytes[0] == 0x80 && b->pop(&got));
  // a queue holds at most kMaxMessages: the oldest go, counted
  for (uint32_t i = 0; i < MidiQueue::kMaxMessages + 10; ++i) bus.send(0x90, uint8_t(i & 0x7F), 1);
  CHECK(a->waiting() == MidiQueue::kMaxMessages && a->lost() == 10);
  bus.unlisten(a.get());
  bus.send(0x90, 1, 1);
  CHECK(a->waiting() == MidiQueue::kMaxMessages);
  // (a status byte that is not a message is not sent)
  bus.send(0x40, 1, 1);
  CHECK(b->waiting() == MidiQueue::kMaxMessages);
  bus.set_output(nullptr);
}

// The bus from several threads: programs playing, a keyboard, listeners reading; every message a listener hears arrives whole and in its sender's order.
static void test_midi_threads() {
  MidiBus bus(A);
  FakeMidiSink out;
  bus.set_output(&out);
  constexpr int kPlayers = 3, kNotes = 2000;
  std::vector<SharedPointer<MidiQueue>> queues;
  for (int i = 0; i < 4; ++i) queues.push_back(bus.listen(uint32_t(100 + i)));
  std::atomic<bool> done{false};
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int p = 0; p < kPlayers; ++p)
    threads.emplace_back([&, p] {
      for (int i = 0; i < kNotes; ++i) {
        MidiMessage m;
        m.bytes[0] = uint8_t(0x90 | p); m.bytes[1] = uint8_t(i & 0x7F); m.bytes[2] = uint8_t((i >> 7) & 0x7F); m.size = 3;
        m.source = uint32_t(100 + p);
        bus.publish(m);
      }
    });
  threads.emplace_back([&] { for (int i = 0; i < kNotes; ++i) bus.send(0xB0, 1, uint8_t(i & 0x7F)); });
  std::vector<std::thread> readers;
  std::atomic<long> heard{0};
  for (int q = 0; q < 4; ++q)
    readers.emplace_back([&, q] {
      int last[kPlayers];
      for (int& l : last) l = -1;
      MidiMessage m;
      for (;;) {
        const bool finished = done.load();
        while (queues[size_t(q)]->pop(&m)) {
          ++heard;
          if ((m.bytes[0] & 0xF0) != 0x90) continue;
          const int p = m.bytes[0] & 0x0F;
          const int i = m.bytes[1] | m.bytes[2] << 7;
          if (p >= kPlayers || i <= last[p] || m.source == uint32_t(100 + q)) ++bad;
          last[p] = i;
        }
        if (finished) break;
        std::this_thread::yield();
      }
    });
  for (std::thread& t : threads) t.join();
  done = true;
  for (std::thread& t : readers) t.join();
  CHECK(bad == 0 && out.count() == size_t(kPlayers * kNotes) && bus.published() == uint64_t(kPlayers * kNotes));
  long lost = 0;
  for (auto& q : queues) lost += long(q->lost());
  long expected = 0;      // (each listener hears the keyboard and every player but itself)
  for (int q = 0; q < 4; ++q) expected += long((q < kPlayers ? kPlayers - 1 : kPlayers) * kNotes + kNotes);
  CHECK(heard + lost == expected);
  // messages for later from several threads while another pumps the clock: every one reaches the output, once (no lost or doubled message)
  out.got.clear();
  std::atomic<uint64_t> clock{0};
  std::atomic<bool> playing{true};
  std::thread pumper([&] {
    while (playing.load() || bus.next_due()) {
      bus.pump(clock.fetch_add(50) + 50);
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> timed;
  for (int p = 0; p < kPlayers; ++p)
    timed.emplace_back([&, p] {
      for (int i = 0; i < kNotes; ++i) {
        MidiMessage m;
        m.bytes[0] = uint8_t(0x90 | p); m.bytes[1] = uint8_t(i & 0x7F); m.bytes[2] = uint8_t((i >> 7) & 0x7F); m.size = 3;
        m.source = uint32_t(100 + p);
        const uint64_t now = clock.load();
        m.time_ns = now + uint64_t(i % 7) * 30;      // (one in seven now, the others later)
        bus.publish(m, now);
      }
    });
  for (std::thread& t : timed) t.join();
  playing = false;
  pumper.join();
  CHECK(out.count() == size_t(kPlayers * kNotes));
  for (auto& q : queues) { MidiMessage m; while (q->pop(&m)) {} }
  bus.set_output(nullptr);
}

// System exclusive (raw and UMP), the queues' byte limit, and the bus's output on time.
static void test_midi_sysex_timing() {
  // raw: whole with the allocator (a real-time byte inside is its own message, given first), skipped without; broken and unterminated ones dropped
  const uint8_t bytes[] = {0xF0, 0x7E, 0x7F, 0xF8, 0x09, 0x01, 0xF7, 0x90, 1, 2, 0xF0, 1, 2, 0x80, 3, 4, 0xF0, 5, 6};
  std::vector<MidiMessage> seen;
  CHECK(parse_midi_bytes(ConstByteView(bytes, sizeof bytes), [&](const MidiMessage& m) { seen.push_back(m); }, &A) == 4);
  CHECK(seen.size() == 4);
  if (seen.size() == 4) {
    CHECK(seen[0].bytes[0] == 0xF8 && !seen[0].is_sysex());
    const uint8_t want[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
    CHECK(seen[1].is_sysex() && seen[1].wire().size() == 6 && memcmp(seen[1].wire().data(), want, 6) == 0 && seen[1].size == 0);
    CHECK(seen[2].bytes[0] == 0x90 && seen[3].bytes[0] == 0x80 && seen[3].bytes[1] == 3);      // (the sysex the note off broke is gone)
  }
  CHECK(parse_midi_bytes(ConstByteView(bytes, sizeof bytes), [&](const MidiMessage& m) { CHECK(!m.is_sysex()); }) == 3);
  MidiMessage m;
  const uint8_t not_sysex[] = {0xF0, 1, 0x90, 0xF7}, no_end[] = {0xF0, 1, 2}, minimal[] = {0xF0, 0xF7};
  CHECK(!make_sysex(A, ConstByteView(not_sysex, 4), &m) && !make_sysex(A, ConstByteView(no_end, 3), &m) && make_sysex(A, ConstByteView(minimal, 2), &m));
  std::vector<uint8_t> huge(kMaxSysex + 1, 0x11);
  huge.front() = 0xF0; huge.back() = 0xF7;
  CHECK(!make_sysex(A, ConstByteView(huge.data(), huge.size()), &m));
  huge.pop_back(); huge.back() = 0xF7;
  CHECK(make_sysex(A, ConstByteView(huge.data(), huge.size()), &m) && m.wire().size() == kMaxSysex);
  // UMP: every length from empty to 40 data bytes and a long one, round trip through Data Messages (a channel message between the pieces)
  for (uint32_t len : {0u, 1u, 5u, 6u, 7u, 12u, 13u, 40u, 1000u}) {
    std::vector<uint8_t> x(len + 2);
    x.front() = 0xF0; x.back() = 0xF7;
    for (uint32_t k = 0; k < len; ++k) x[k + 1] = uint8_t((k * 7 + len) & 0x7F);
    MidiMessage sx;
    CHECK(make_sysex(A, ConstByteView(x.data(), x.size()), &sx));
    std::vector<uint32_t> words;
    CHECK(write_ump(sx, [&](uint32_t w) { words.push_back(w); }) == words.size() && words.size() == ump_words_for(sx));
    UmpDecoder dec(A);
    MidiMessage back, note;
    int whole = 0, notes = 0;
    for (size_t i = 0; i + 1 < words.size(); i += 2) {
      const uint32_t pk[2] = {words[i], words[i + 1]};
      if (dec.decode(ArrayView<const uint32_t>(pk, 2), &back)) ++whole;
      const uint32_t nw[1] = {0x20903C40};
      if (dec.decode(ArrayView<const uint32_t>(nw, 1), &note)) ++notes;
    }
    CHECK(whole == 1 && notes == int(words.size() / 2) && back.is_sysex() && back.wire().size() == x.size() &&
          memcmp(back.wire().data(), x.data(), x.size()) == 0);
  }
  {
    UmpDecoder dec(A);
    const uint32_t cont[2] = {0x30220102, 0};      // (a continuation without its start)
    CHECK(!dec.decode(ArrayView<const uint32_t>(cont, 2), &m) && !dec.building());
    std::mt19937 rng(9);
    for (int round = 0; round < 5000; ++round) {      // random packets: no crash; what comes out is MIDI 1.0
      uint32_t w[2] = {uint32_t(rng()), uint32_t(rng())};
      if (round % 3 == 0) w[0] = (w[0] & 0x0FFFFFFF) | 0x30000000;
      MidiMessage out;
      if (dec.decode(ArrayView<const uint32_t>(w, 1 + rng() % 2), &out)) {
        if (out.is_sysex()) {
          const ConstByteView v = out.wire();
          CHECK(v.size() >= 2 && v[0] == 0xF0 && v[v.size() - 1] == 0xF7 && v.size() <= kMaxSysex);
          for (uint64_t k = 1; k + 1 < v.size(); ++k) CHECK(v[k] < 0x80);
        } else {
          CHECK(out.size >= 1 && out.size <= 3 && out.bytes[0] >= 0x80);
        }
      }
    }
  }
  // raw bytes with sysex: truncations and bit flips under the allocator
  {
    std::mt19937 rng(11);
    for (int round = 0; round < 3000; ++round) {
      std::vector<uint8_t> b(bytes, bytes + sizeof bytes);
      if (round % 2) b.resize(rng() % (b.size() + 1));
      else for (int k = 0; k < 3; ++k) b[rng() % b.size()] ^= uint8_t(1u << (rng() % 8));
      parse_midi_bytes(ConstByteView(b.data(), b.size()), [&](const MidiMessage& x) {
        if (x.is_sysex()) CHECK(x.wire().size() >= 2 && x.wire()[0] == 0xF0 && x.wire()[x.wire().size() - 1] == 0xF7);
        else CHECK(x.size >= 1 && x.size <= 3);
      }, &A);
    }
  }
  // a queue keeps at most kMaxBytes; pop_if waits, drops and takes
  MidiBus bus(A);
  FakeMidiSink out;
  bus.set_output(&out);
  SharedPointer<MidiQueue> q = bus.listen(50);
  std::vector<uint8_t> big(60000, 0x22);
  big.front() = 0xF0; big.back() = 0xF7;
  for (int i = 0; i < 20; ++i) bus.send_sysex(ConstByteView(big.data(), big.size()));
  CHECK(q->waiting() == MidiQueue::kMaxBytes / 60000 && q->lost() == 20 - MidiQueue::kMaxBytes / 60000);
  CHECK(out.sysex.empty());      // (a keyboard's sysex goes to the programs only)
  CHECK(!q->pop_if([](const MidiMessage&) { return MidiQueue::Choice::Wait; }, &m) && q->waiting() == MidiQueue::kMaxBytes / 60000);
  const uint64_t lost_before = q->lost();
  CHECK(!q->pop_if([](const MidiMessage&) { return MidiQueue::Choice::Drop; }, &m) && q->waiting() == 0 && q->lost() == lost_before + 17);
  // the output on time: later messages wait for pump(); one due now goes behind those already waiting that are not later
  MidiMessage a1, a2, a3;
  a1.bytes[0] = 0x90; a1.bytes[1] = 1; a1.size = 3; a1.source = 7; a1.time_ns = 100;
  a2 = a1; a2.bytes[1] = 2; a2.time_ns = 50;
  a3 = a1; a3.bytes[1] = 3; a3.time_ns = 0;
  bus.publish(a1, 10);
  bus.publish(a2, 10);
  CHECK(out.count() == 0 && bus.next_due() == 50);
  CHECK(q->waiting() == 2);      // (the other programs hear them at once, with their times)
  bus.publish(a3, 10);
  CHECK(out.count() == 1 && out.got[0][1] == 3);
  CHECK(bus.pump(60) == 100 && out.count() == 2 && out.got[1][1] == 2);
  CHECK(bus.pump(100) == 0 && out.count() == 3 && out.got[2][1] == 1 && bus.next_due() == 0);
  MidiMessage sx;
  CHECK(make_sysex(A, ConstByteView(minimal, 2), &sx));
  sx.source = 7;
  bus.publish(sx, 0);
  CHECK(out.sysex.size() == 1 && out.sysex[0].size() == 2);
  // more than kMaxScheduled waiting: the rest go at once
  for (uint32_t i = 0; i < MidiBus::kMaxScheduled + 5; ++i) {
    MidiMessage later = a1;
    later.time_ns = 1000000 + i;
    bus.publish(later, 0);
  }
  CHECK(out.count() == 3 + 5);
  CHECK(bus.pump(~0ull) == 0 && out.count() == 3 + 5 + MidiBus::kMaxScheduled);
  bus.set_output(nullptr);
}

// ---- video: the channel a producer publishes on -------------------------------------------------------------------------------------------------------

static void test_video_channel() {
  VideoChannel ch(A);
  std::vector<uint8_t> rows(3 * 300, 0);
  CHECK(!ch.publish(ConstByteView(rows.data(), rows.size()), 256, 0));      // (no format yet)
  VideoFormat f;
  f.format = 12; f.width = 64; f.height = 3; f.rate_num = 30; f.rate_den = 1;
  CHECK(f.valid() && f.row_bytes() == 256 && f.frame_bytes() == 768);
  ch.set_format(f);
  for (size_t y = 0; y < 3; ++y) for (size_t x = 0; x < 256; ++x) rows[y * 300 + x] = uint8_t(y * 10 + x % 7);
  CHECK(!ch.publish(ConstByteView(rows.data(), 2 * 300 + 255), 300, 0));      // (the last row cut short)
  CHECK(!ch.publish(ConstByteView(rows.data(), rows.size()), 200, 0));        // (a stride shorter than a row)
  CHECK(ch.publish(ConstByteView(rows.data(), 2 * 300 + 256), 300, 77));
  SharedPointer<VideoFrame> frame = ch.latest();
  CHECK(frame && frame->sequence == 1 && frame->pts == 77 && frame->pixels.size() == 768);
  if (frame) CHECK(frame->pixels.view()[256 + 5] == uint8_t(10 + 5 % 7) && frame->pixels.view()[512 + 255] == uint8_t(20 + 255 % 7));
  VideoFormat bad = f;
  bad.format = 1;      // (SPA_VIDEO_FORMAT_ENCODED is not a raw format)
  CHECK(!bad.valid());
  bad.format = 21;     // (v210: not carried)
  CHECK(!bad.valid());
  bad = f; bad.width = 9000;
  CHECK(!bad.valid());
  bad = f; bad.subtype = 0x2000D;      // (bayer)
  CHECK(!bad.valid());
  // the planes of YUV and grey formats (odd sizes round the chroma up)
  VideoFormat yuv;
  yuv.width = 5; yuv.height = 3;
  yuv.format = 2;      // I420
  CHECK(yuv.valid() && yuv.planes() == 3 && yuv.plane(1).row_bytes == 3 && yuv.plane(1).rows == 2 && yuv.plane(2).stride_den == 2 &&
        yuv.frame_bytes() == 15 + 6 + 6 && yuv.plane_offset(2) == 21 && yuv.plane(3).rows == 0);
  yuv.format = 23;     // NV12
  CHECK(yuv.planes() == 2 && yuv.plane(1).row_bytes == 6 && yuv.plane(1).rows == 2 && yuv.frame_bytes() == 15 + 12);
  yuv.format = 4;      // YUY2
  CHECK(yuv.planes() == 1 && yuv.row_bytes() == 12 && yuv.frame_bytes() == 36);
  yuv.format = 18;     // Y42B
  CHECK(yuv.plane(1).row_bytes == 3 && yuv.plane(1).rows == 3);
  yuv.format = 17;     // Y41B
  CHECK(yuv.plane(1).row_bytes == 2 && yuv.plane(1).stride_den == 4);
  yuv.format = 20;     // Y444
  CHECK(yuv.frame_bytes() == 45);
  yuv.format = 25;     // GRAY8
  CHECK(yuv.frame_bytes() == 15);
  yuv.format = 27;     // GRAY16_LE
  CHECK(yuv.frame_bytes() == 30);
  yuv.format = 29;     // RGB16
  CHECK(yuv.frame_bytes() == 30);
  // a planar frame published plane by plane, each at its own stride; one plane short is refused
  VideoChannel planar(A);
  VideoFormat i420;
  i420.format = 2; i420.width = 6; i420.height = 4;
  planar.set_format(i420);
  std::vector<uint8_t> y(8 * 4, 0), u(4 * 2, 0), v(5 * 2, 0);
  for (size_t k = 0; k < y.size(); ++k) y[k] = uint8_t(k);
  for (size_t k = 0; k < u.size(); ++k) u[k] = uint8_t(100 + k);
  for (size_t k = 0; k < v.size(); ++k) v[k] = uint8_t(200 + k);
  const VideoPlaneData parts[3] = {{ConstByteView(y.data(), y.size()), 8}, {ConstByteView(u.data(), u.size()), 4}, {ConstByteView(v.data(), v.size()), 5}};
  CHECK(!planar.publish(ArrayView<const VideoPlaneData>(parts, 2), 0));
  const VideoPlaneData short_v[3] = {parts[0], parts[1], {ConstByteView(v.data(), 5 + 2), 5}};
  CHECK(!planar.publish(ArrayView<const VideoPlaneData>(short_v, 3), 0));
  CHECK(planar.publish(ArrayView<const VideoPlaneData>(parts, 3), 5));
  SharedPointer<VideoFrame> pf = planar.latest();
  CHECK(pf && pf->pixels.size() == 24 + 6 + 6);
  if (pf) {
    const ConstByteView px = pf->pixels.view();
    CHECK(px[6] == 8 && px[23] == 29 && px[24] == 100 && px[27] == 104 && px[30] == 200 && px[33] == 205);
  }
  // encoded: frames of any length up to kMaxEncodedFrame, size unknown allowed
  VideoChannel enc(A);
  VideoFormat h264;
  h264.subtype = 0x20001;
  CHECK(h264.encoded() && h264.valid() && h264.frame_bytes() == 0 && h264.planes() == 0);
  enc.set_format(h264);
  const uint8_t nal[] = {0, 0, 0, 1, 0x65, 0x88, 0x84};
  CHECK(enc.publish(ConstByteView(nal, sizeof nal), 0, 9) && enc.latest() && enc.latest()->pixels.size() == sizeof nal);
  CHECK(!enc.publish(ConstByteView(), 0, 0));
  // random planes against random formats: never a crash, a frame only when every plane fits
  std::mt19937 rng(21);
  std::vector<uint8_t> junk(4096, 0x5A);
  const uint32_t formats[] = {2, 3, 4, 5, 6, 7, 15, 17, 18, 19, 20, 23, 24, 25, 26, 29, 32};
  for (int round = 0; round < 2000; ++round) {
    VideoChannel ch2(A);
    VideoFormat ff;
    ff.format = formats[rng() % (sizeof formats / sizeof formats[0])];
    ff.width = 1 + rng() % 40; ff.height = 1 + rng() % 20;
    ch2.set_format(ff);
    VideoPlaneData pl[4];
    for (VideoPlaneData& d : pl) d = VideoPlaneData{ConstByteView(junk.data(), rng() % junk.size()), uint32_t(rng() % 200)};
    const bool ok = ch2.publish(ArrayView<const VideoPlaneData>(pl, 1 + rng() % 4), 0);
    if (ok) CHECK(ch2.latest() && ch2.latest()->pixels.size() == ff.frame_bytes());
  }
  // the 10- and 12-bit, 4:2:2/4:4:4 semi-planar and other formats' planes
  VideoFormat hb;
  hb.width = 5; hb.height = 3;
  hb.format = 43;      // I420_10LE
  CHECK(hb.planes() == 3 && hb.plane(0).row_bytes == 10 && hb.plane(1).row_bytes == 6 && hb.plane(1).rows == 2 && hb.frame_bytes() == 30 + 12 + 12);
  hb.format = 62;      // P010_10LE
  CHECK(hb.planes() == 2 && hb.plane(1).row_bytes == 12 && hb.frame_bytes() == 30 + 24);
  hb.format = 52;      // NV24: chroma rows twice the luma's, stride too
  CHECK(hb.plane(1).row_bytes == 10 && hb.plane(1).rows == 3 && hb.plane(1).stride_num == 2);
  hb.format = 51;      // NV16
  CHECK(hb.plane(1).row_bytes == 6 && hb.plane(1).rows == 3);
  hb.format = 80;      // xRGB_210LE
  CHECK(hb.frame_bytes() == 60);
  hb.format = 79; hb.subtype = VideoFormat::kSubtypeDsp;      // dsp float video
  CHECK(hb.valid() && !hb.encoded() && hb.frame_bytes() == 5 * 3 * 16);
  hb.format = 12;
  CHECK(!hb.valid());      // (dsp is RGBA_F32 only)
  // conversion: every pair of the 8-bit formats round-trips a grey ramp and pure colours closely
  const uint32_t convertible[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 23, 24, 25, 26, 27, 29, 30, 31, 32, 39, 40, 43, 45, 47,
                                  51, 52, 60, 62, 64, 73, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87};
  VideoFormat rgba;
  rgba.format = 11; rgba.width = 6; rgba.height = 4;
  std::vector<uint8_t> source(6 * 4 * 4);
  for (uint32_t i = 0; i < 24; ++i) {
    const uint8_t v = uint8_t(40 + (i % 6) * 30);      // (a grey per column: survives 4:2:0 too)
    source[4 * i] = source[4 * i + 1] = source[4 * i + 2] = v;
    source[4 * i + 3] = 255;
  }
  for (uint32_t to : convertible) {
    VideoFormat mid = rgba;
    mid.format = to;
    ByteBuffer there(A), back(A);
    CHECK(can_convert_video(11, to) && convert_video(rgba, ConstByteView(source.data(), source.size()), mid, &there) && there.size() == mid.frame_bytes());
    CHECK(convert_video(mid, there.view(), rgba, &back) && back.size() == source.size());
    int worst = 0;
    for (size_t k = 0; k < source.size() && back.size() == source.size(); ++k)
      if (k % 4 != 3) worst = std::max(worst, abs(int(back.view()[k]) - int(source[k])));
    const int allowed = to >= 29 && to <= 32 ? 9 : 4;      // (5 bits a component: steps of 8)
    if (worst > allowed) printf("conversion through format %u: off by %d\n", to, worst);
    CHECK(worst <= allowed);
  }
  {
    VideoFormat i420 = rgba, odd = rgba;
    i420.format = 2;
    odd.width = 5;
    ByteBuffer o(A);
    CHECK(!convert_video(rgba, ConstByteView(source.data(), source.size() - 1), i420, &o));      // (short)
    CHECK(convert_video(rgba, ConstByteView(source.data(), source.size()), odd, &o) && o.size() == 5 * 4 * 4);      // (another size: scaled)
    VideoFormat v210 = rgba;
    v210.format = 21;      // (v210: not carried)
    CHECK(!can_convert_video(21, 11) && !convert_video(v210, ConstByteView(source.data(), source.size()), rgba, &o));
    // a deep format keeps colour through it: red through P010 and through float RGBA
    for (uint32_t deep : {62u, 79u, 85u}) {
      std::vector<uint8_t> red(6 * 4 * 4);
      for (uint32_t i = 0; i < 24; ++i) { red[4 * i] = 200; red[4 * i + 1] = 30; red[4 * i + 2] = 10; red[4 * i + 3] = 255; }
      VideoFormat d = rgba;
      d.format = deep;
      ByteBuffer t(A), b3(A);
      CHECK(convert_video(rgba, ConstByteView(red.data(), red.size()), d, &t) && convert_video(d, t.view(), rgba, &b3));
      if (b3.size() == red.size()) CHECK(abs(int(b3.view()[0]) - 200) <= 6 && abs(int(b3.view()[1]) - 30) <= 6 && b3.view()[2] <= 16);
    }
    // scaling: a ramp of four columns, halved (pairs averaged), doubled (bilinear, the ends kept)
    VideoFormat four = rgba, two = rgba, eight = rgba;
    four.width = 4; four.height = 1; two.width = 2; two.height = 1; eight.width = 8; eight.height = 1;
    std::vector<uint8_t> ramp = {0, 0, 0, 255, 100, 100, 100, 255, 200, 200, 200, 255, 240, 240, 240, 255};
    ByteBuffer half(A), twice(A);
    CHECK(convert_video(four, ConstByteView(ramp.data(), ramp.size()), two, &half) && half.size() == 8);
    if (half.size() == 8) CHECK(half.view()[0] == 50 && half.view()[4] == 220 && half.view()[3] == 255);
    CHECK(convert_video(four, ConstByteView(ramp.data(), ramp.size()), eight, &twice) && twice.size() == 32);
    if (twice.size() == 32) { CHECK(twice.view()[0] == 0 && twice.view()[28] == 240 && twice.view()[8] > 0 && twice.view()[8] < 100); }
    // random sizes both ways, random formats: never out of bounds, the frame the size asked
    std::mt19937 srng(31);
    const uint32_t fmts[] = {2, 4, 12, 15, 23, 29, 43, 52, 62, 79, 80};
    for (int round = 0; round < 300; ++round) {
      VideoFormat from = rgba, to = rgba;
      from.width = 1 + srng() % 33; from.height = 1 + srng() % 17;
      to.width = 1 + srng() % 33; to.height = 1 + srng() % 17;
      from.format = fmts[srng() % 11]; to.format = fmts[srng() % 11];
      std::vector<uint8_t> frame(from.frame_bytes());
      for (uint8_t& v : frame) v = uint8_t(srng());
      ByteBuffer r(A);
      CHECK(convert_video(from, ConstByteView(frame.data(), frame.size()), to, &r) && r.size() == to.frame_bytes());
    }
    // red through NV12 comes back red
    std::vector<uint8_t> red(6 * 4 * 4);
    for (uint32_t i = 0; i < 24; ++i) { red[4 * i] = 200; red[4 * i + 1] = 0; red[4 * i + 2] = 0; red[4 * i + 3] = 255; }
    VideoFormat nv12 = rgba;
    nv12.format = 23;
    ByteBuffer t(A), b2(A);
    CHECK(convert_video(rgba, ConstByteView(red.data(), red.size()), nv12, &t) && convert_video(nv12, t.view(), rgba, &b2));
    if (b2.size() == red.size()) CHECK(abs(int(b2.view()[0]) - 200) <= 6 && b2.view()[1] <= 6 && b2.view()[2] <= 6);
  }
  ch.close();
  CHECK(ch.closed() && !ch.publish(ConstByteView(rows.data(), rows.size()), 300, 0) && ch.frames() == 1);
  // a producer and watchers at once: each frame a watcher takes is whole (every byte its sequence's)
  VideoChannel live(A);
  VideoFormat big;
  big.format = 8; big.width = 320; big.height = 240;
  live.set_format(big);
  std::atomic<bool> done{false};
  std::atomic<int> torn{0}, taken{0};
  std::thread producer([&] {
    std::vector<uint8_t> px(big.frame_bytes());
    for (int i = 1; i <= 300; ++i) {
      memset(px.data(), i & 0xFF, px.size());
      live.publish(ConstByteView(px.data(), px.size()), big.row_bytes(), i);
    }
    done = true;
  });
  std::vector<std::thread> watchers;
  for (int w = 0; w < 3; ++w)
    watchers.emplace_back([&] {
      uint64_t seen = 0;
      while (!done.load() || seen < live.frames()) {
        SharedPointer<VideoFrame> fr = live.latest();
        if (!fr || fr->sequence == seen) { std::this_thread::yield(); continue; }
        if (fr->sequence < seen) ++torn;
        seen = fr->sequence;
        const uint8_t want = uint8_t(fr->pts & 0xFF);
        for (uint8_t byte : fr->pixels.view()) if (byte != want) { ++torn; break; }
        ++taken;
      }
    });
  producer.join();
  for (std::thread& t : watchers) t.join();
  CHECK(torn == 0 && taken > 0 && live.frames() == 300);
}

// ---- client-node: metadata, client-allocated buffers, drivers, MIDI, video, filters, the adapter ------------------------------------------------------


// What the server has told a client node, gathered from its messages.
struct NodeView {
  struct Data { uint32_t type = 0; int32_t data = 0, flags = 0, mapoff = 0, maxsize = 0; };
  struct Buf { uint32_t mem = 0, off = 0, size = 0; std::vector<std::pair<uint32_t, uint32_t>> metas; uint32_t dtype = 0; int32_t data = 0, dflags = 0, mapoff = 0, maxsize = 0; std::vector<Data> datas; };
  struct PortView { std::vector<uint8_t> format; bool use = false; int32_t flags = 0; std::vector<Buf> bufs; Fd io; uint32_t io_off = 0; uint32_t peer = 0; int uses = 0, formats = 0; };
  std::vector<uint32_t> removed;      // (remove_mem)
  uint32_t id = 0, node = 0, activation_mem = 0;
  std::map<uint32_t, Fd> mems;
  Fd activation, wake;
  int config_mode = -1;
  bool config_format = false;
  std::map<std::pair<int, int>, PortView> ports;
  std::map<uint32_t, std::tuple<uint32_t, uint32_t, uint32_t>> set_io;      // area -> mem, offset, size
  int starts = 0, pauses = 0, errors = 0;
  uint64_t next_fd = 0;
  void absorb(NativeClient& c) {
    next_fd = c.fds.size();      // (this take's descriptors are the ones it adds)
    std::vector<NativeMessage> msgs = c.take_with_fds();
    for (NativeMessage& m : msgs) {
      std::vector<Fd> own(c.fds.begin() + long(next_fd), c.fds.begin() + long(next_fd + m.n_fds));
      next_fd += m.n_fds;
      pod::Parser p = args_of(m);
      if (m.id == 0 && m.opcode == 6) {
        int32_t mem = 0; uint32_t type = 0; int64_t fd = -1; pod::Value v;
        p.int32(&mem); p.id(&type); p.next(&v); v.get_fd(&fd);
        if (fd >= 0 && size_t(fd) < own.size()) mems[uint32_t(mem)] = own[size_t(fd)];
      }
      if (m.id == 0 && m.opcode == 7) { int32_t mem = 0; if (p.int32(&mem)) removed.push_back(uint32_t(mem)); }
      if (m.id == 0 && m.opcode == 3) {      // error
        int32_t eid = 0; p.int32(&eid);
        if (uint32_t(eid) == id) ++errors;
      }
      if (m.id == 0 && (m.opcode == 8 || m.opcode == 5)) { int32_t pid = 0, nid = 0; if (p.int32(&pid) && p.int32(&nid) && uint32_t(pid) == id) node = uint32_t(nid); }
      if (m.id != id) continue;
      switch (m.opcode) {
        case 0: {
          pod::Value r, w; int32_t mem = 0, off = 0, size = 0; int64_t rf = -1;
          if (p.next(&r) && p.next(&w) && p.int32(&mem) && p.int32(&off) && p.int32(&size) && r.get_fd(&rf) && rf >= 0 && size_t(rf) < own.size()) {
            wake = own[size_t(rf)];
            activation = mems[uint32_t(mem)];
            activation_mem = uint32_t(mem);
          }
          break;
        }
        case 1: {
          uint32_t pid = 0; int32_t flags = 0; pod::Value obj; pod::ObjectReader o;
          if (p.id(&pid) && p.int32(&flags) && p.pod(&obj) && o.open(obj) && pid == 11) {
            uint32_t key = 0, f = 0, mode = 0; pod::Value v;
            while (o.next(&key, &f, &v)) { if (key == 2) v.get_id(&mode); if (key == 5) config_format = true; }
            config_mode = int(mode);
          }
          break;
        }
        case 2: { uint32_t area = 0; int32_t mem = 0, off = 0, size = 0; if (p.id(&area) && p.int32(&mem) && p.int32(&off) && p.int32(&size)) set_io[area] = {uint32_t(mem), uint32_t(off), uint32_t(size)}; break; }
        case 4: {
          pod::Value obj; pod::ObjectReader o;
          if (p.pod(&obj) && o.open(obj)) { if (o.object_id() == 2) ++starts; if (o.object_id() == 1) ++pauses; }
          break;
        }
        case 7: {
          int32_t dir = 0, port = 0; uint32_t pid = 0; int32_t flags = 0; pod::Value v;
          if (p.int32(&dir) && p.int32(&port) && p.id(&pid) && p.int32(&flags) && p.pod(&v) && pid == 4) {
            ports[{dir, port}].format.assign(v.whole.data(), v.whole.data() + v.whole.size());
            ++ports[{dir, port}].formats;
          }
          break;
        }
        case 8: {
          int32_t dir, port, mix, flags, n;
          if (!p.int32(&dir) || !p.int32(&port) || !p.int32(&mix) || !p.int32(&flags) || !p.int32(&n)) break;
          PortView& pv = ports[{dir, port}];
          pv.use = true; pv.flags = flags; pv.bufs.clear(); ++pv.uses;
          for (int32_t i = 0; i < n; ++i) {
            Buf b; int32_t mem, off, size, nm, nd;
            if (!p.int32(&mem) || !p.int32(&off) || !p.int32(&size) || !p.int32(&nm)) break;
            b.mem = uint32_t(mem); b.off = uint32_t(off); b.size = uint32_t(size);
            for (int32_t k = 0; k < nm; ++k) { uint32_t t = 0; int32_t sz = 0; p.id(&t); p.int32(&sz); b.metas.push_back({t, uint32_t(sz)}); }
            if (!p.int32(&nd)) break;
            bool good = true;
            for (int32_t k = 0; k < nd && good; ++k) {
              Data d;
              good = p.id(&d.type) && p.int32(&d.data) && p.int32(&d.flags) && p.int32(&d.mapoff) && p.int32(&d.maxsize);
              if (good) b.datas.push_back(d);
            }
            if (!good || b.datas.empty()) break;
            b.dtype = b.datas[0].type; b.data = b.datas[0].data; b.dflags = b.datas[0].flags; b.mapoff = b.datas[0].mapoff; b.maxsize = b.datas[0].maxsize;
            pv.bufs.push_back(b);
          }
          break;
        }
        case 9: {
          int32_t dir, port, mix, mem, off, size; uint32_t area;
          if (p.int32(&dir) && p.int32(&port) && p.int32(&mix) && p.id(&area) && p.int32(&mem) && p.int32(&off) && p.int32(&size) && area == 1) {
            ports[{dir, port}].io = mems[uint32_t(mem)];
            ports[{dir, port}].io_off = uint32_t(off);
          }
          break;
        }
        case 11: { int32_t dir, port, mix, peer; if (p.int32(&dir) && p.int32(&port) && p.int32(&mix) && p.int32(&peer)) ports[{dir, port}].peer = uint32_t(peer); break; }
        default: break;
      }
    }
  }
  ByteView buffer(int dir, int port, uint32_t i) {
    Buf& b = ports[{dir, port}].bufs[i];
    return mems[b.mem]->memory().subview(b.off, b.size);
  }
  ByteView io(int dir, int port) { PortView& pv = ports[{dir, port}]; return pv.io->memory().subview(pv.io_off, 8); }
};

static void create_node(NativeClient& c, uint32_t id, const Props& props) {
  c.send(0, 6, [&](pod::Builder& b) {
    b.string("client-node"); b.string("PipeWire:Interface:ClientNode"); b.int32(6);
    const uint64_t d = b.begin_struct();
    b.int32(int32_t(props.size()));
    for (const auto& kv : props) { b.string(StringView(kv.first.c_str())); b.string(StringView(kv.second.c_str())); }
    b.end(d);
    b.int32(int32_t(id));
  });
}
static void node_update(NativeClient& c, uint32_t id) {
  c.send(id, 2, [&](pod::Builder& b) {
    b.int32(3); b.int32(0);
    const uint64_t i = b.begin_struct();
    b.int32(0); b.int32(1); b.int64(7); b.int64(0); b.int32(0); b.int32(0);
    b.end(i);
  });
}
static void set_active(NativeClient& c, uint32_t id, bool on) { c.send(id, 4, [&](pod::Builder& b) { b.boolean(on); }); }
// A port's info: its flags and properties.
static void port_info(pod::Builder& b, uint64_t flags, const Props& props) {
  const uint64_t i = b.begin_struct();
  b.int64(0xF); b.int64(int64_t(flags)); b.int32(0); b.int32(1);
  b.int32(int32_t(props.size()));
  for (const auto& kv : props) { b.string(StringView(kv.first.c_str())); b.string(StringView(kv.second.c_str())); }
  b.int32(0);
  b.end(i);
}
static void enum_control(pod::Builder& b) {
  const uint64_t o = b.begin_object(0x40003, 3);
  b.property(1); b.id(6); b.property(2); b.id(0x60001);
  b.end(o);
}
static void enum_dsp_audio(pod::Builder& b) {
  const uint64_t o = b.begin_object(0x40003, 3);
  b.property(1); b.id(1); b.property(2); b.id(2); b.property(0x10001); b.id(0x203);      // (F32P)
  b.end(o);
}
// A video EnumFormat: BGRA, a size range around `w` x `h`, 25 frames a second.
static void enum_video(pod::Builder& b, uint32_t w, uint32_t h, bool choice_of_formats = false) {
  const uint64_t o = b.begin_object(0x40003, 3);
  b.property(1); b.id(2); b.property(2); b.id(1);
  b.property(0x20001);
  if (choice_of_formats) { const uint64_t ch = b.begin_choice(3, pod::kId); b.choice_value32(12); b.choice_value32(12); b.choice_value32(11); b.end(ch); }
  else b.id(12);
  b.property(0x20003);
  {
    ByteBuffer rect(A);      // a Choice (Range) of Rectangles: default, min, max
    rect.put32(16 + 24); rect.put32(pod::kChoice); rect.put32(1); rect.put32(0); rect.put32(8); rect.put32(pod::kRectangle);
    rect.put32(w); rect.put32(h); rect.put32(1); rect.put32(1); rect.put32(4096); rect.put32(4096);
    b.raw(rect.view());
  }
  b.property(0x20004); b.fraction(25, 1);
  b.end(o);
}
// A video EnumFormat of one raw format (or, with `subtype`, an encoded one) and size.
static void enum_video_fixed(pod::Builder& b, uint32_t format, uint32_t w, uint32_t h, uint32_t subtype = 1) {
  const uint64_t o = b.begin_object(0x40003, 3);
  b.property(1); b.id(2); b.property(2); b.id(subtype);
  if (subtype == 1) { b.property(0x20001); b.id(format); }
  b.property(0x20003); b.rectangle(w, h);
  b.property(0x20004); b.fraction(30, 1);
  b.end(o);
}
// A video EnumFormat of a Choice of raw formats (the first the default) at one size.
static void enum_video_choice(pod::Builder& b, std::initializer_list<uint32_t> formats, uint32_t w, uint32_t h) {
  const uint64_t o = b.begin_object(0x40003, 3);
  b.property(1); b.id(2); b.property(2); b.id(1);
  b.property(0x20001);
  const uint64_t ch = b.begin_choice(3, pod::kId);
  b.choice_value32(*formats.begin());
  for (uint32_t f : formats) b.choice_value32(f);
  b.end(ch);
  if (w && h) { b.property(0x20003); b.rectangle(w, h); }      // (0: any size)
  b.property(0x20004); b.fraction(30, 1);
  b.end(o);
}
static void buffers_param(pod::Builder& b, int32_t count, int32_t size, int32_t stride, uint32_t types, int32_t blocks = 0) {
  const uint64_t o = b.begin_object(0x40004, 5);
  b.property(1); b.int32(count);
  if (blocks) { b.property(2); b.int32(blocks); }
  b.property(3); b.int32(size);
  if (stride) { b.property(4); b.int32(stride); }
  if (types) { b.property(6); const uint64_t ch = b.begin_choice(4, pod::kInt); b.choice_value32(types); b.end(ch); }
  b.end(o);
}
static void meta_param(pod::Builder& b, uint32_t type, int32_t size) {
  const uint64_t o = b.begin_object(0x40005, 6);
  b.property(1); b.id(type); b.property(2); b.int32(size);
  b.end(o);
}
// A Sequence of MIDI controls: raw bytes (SPA_CONTROL_Midi) and UMP words (SPA_CONTROL_UMP).
static std::vector<uint8_t> sequence(const std::vector<std::vector<uint8_t>>& midi, const std::vector<uint32_t>& ump) {
  ByteBuffer body(A);
  body.put32(0); body.put32(0);
  for (const auto& m : midi) {
    body.put32(0); body.put32(2); body.put32(uint32_t(m.size())); body.put32(pod::kBytes);
    body.append(ConstByteView(m.data(), m.size()));
    body.put_zeros((8 - m.size() % 8) % 8);
  }
  for (uint32_t w : ump) { body.put32(0); body.put32(4); body.put32(4); body.put32(pod::kBytes); body.put32(w); body.put32(0); }
  std::vector<uint8_t> out;
  ByteBuffer pod(A);
  pod.put32(uint32_t(body.size())); pod.put32(pod::kSequence); pod.append(body.view());
  out.assign(pod.view().data(), pod.view().data() + pod.size());
  return out;
}
static void put_bytes(ByteView m, uint64_t at, const void* src, size_t n) { memcpy(m.data() + at, src, n); }
// A Sequence of controls at offsets: {offset, control (2 Midi, 4 UMP), the value's bytes}.
struct Control {
  uint32_t offset, type;
  std::vector<uint8_t> value;
};
static std::vector<uint8_t> sequence_at(const std::vector<Control>& controls) {
  ByteBuffer body(A);
  body.put32(0); body.put32(0);
  for (const Control& c : controls) {
    body.put32(c.offset); body.put32(c.type); body.put32(uint32_t(c.value.size())); body.put32(pod::kBytes);
    body.append(ConstByteView(c.value.data(), c.value.size()));
    body.put_zeros((8 - c.value.size() % 8) % 8);
  }
  ByteBuffer pod(A);
  pod.put32(uint32_t(body.size())); pod.put32(pod::kSequence); pod.append(body.view());
  return std::vector<uint8_t>(pod.view().data(), pod.view().data() + pod.size());
}
static std::vector<Control> read_sequence(const uint8_t* d, uint32_t size) {
  std::vector<Control> out;
  uint32_t at = 16;
  while (at + 16 <= size) {
    uint32_t off, type, len; memcpy(&off, d + at, 4); memcpy(&type, d + at + 4, 4); memcpy(&len, d + at + 8, 4);
    if (len > size - at - 16) break;
    out.push_back({off, type, std::vector<uint8_t>(d + at + 16, d + at + 16 + len)});
    at += 16 + ((len + 7) & ~7u);
  }
  return out;
}
static std::vector<uint8_t> words_bytes(const std::vector<uint32_t>& w) {
  std::vector<uint8_t> b(w.size() * 4);
  memcpy(b.data(), w.data(), b.size());
  return b;
}

static void test_client_node_media() {
  FakeDevice dev;
  audio::AudioMixer mixer(dev, A);
  CHECK(mixer.initialize());
  FakeMicrophone mic;
  audio::AudioCaptureHub hub(mic, A);
  CHECK(hub.initialize());
  Graph g(A, &mixer, &hub);
  CHECK(g.initialize());
  NativeServer server(g, A, &test_clock);
  FakeMaker maker;
  server.set_descriptor_maker(&maker);
  FakeMidiSink synth;
  g.midi_bus().set_output(&synth);
  auto tick = [&](uint64_t ms = 5) { g_now_ns += ms * 1000000ull; server.tick(); };

  // ---- MIDI out: a raw node (its port came before it was active: no PortConfig), its Format the port's own, cycles on the server's clock
  NativeClient c(server);
  c.send(0, 1, [](pod::Builder& b) { b.int32(4); });
  c.take_with_fds();
  NodeView mo;
  mo.id = 3;
  create_node(c, 3, {{"media.class", "Stream/Output/Midi"}, {"media.type", "Midi"}, {"application.name", "pw-midiplay"}});
  mo.absorb(c);
  c.send(3, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); enum_control(b); port_info(b, 0, {{"format.dsp", "8 bit raw midi"}}); });
  node_update(c, 3);
  set_active(c, 3, true);
  mo.absorb(c);
  CHECK(mo.config_mode == -1 && mo.node != 0 && !mo.ports[{1, 0}].format.empty());
  CHECK(mo.set_io.count(3) && mo.set_io.count(7) && std::get<0>(mo.set_io[7]) == mo.activation_mem && std::get<1>(mo.set_io[7]) == 560);      // clock, position
  bool midi_node = false;
  g.visit(mo.node, [&](const GraphObject& o) { midi_node = o.role == NodeRole::MidiStream && o.props.get("media.class") == StringView("Stream/Output/Midi"); });
  CHECK(midi_node);
  c.send(3, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(2); enum_control(b); buffers_param(b, 2, 4096, 0, 0); b.none(); });
  mo.absorb(c);
  CHECK(mo.ports[{1, 0}].use && mo.ports[{1, 0}].bufs.size() == 2 && mo.starts == 1 && mo.ports[{1, 0}].peer == g.midi_bridge_id());
  // ---- MIDI in (another node, UMP): hears the first one and keyboards
  NodeView mi;
  mi.id = 4;
  create_node(c, 4, {{"media.class", "Midi/Sink"}, {"application.name", "pw-midirecord"}});
  c.send(4, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(3); b.int32(1); enum_control(b); port_info(b, 0, {{"control.ump", "true"}}); });
  set_active(c, 4, true);
  mi.absorb(c);
  c.send(4, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 4096, 0, 0); b.none(); });
  mi.absorb(c);
  CHECK(mi.node && mi.starts == 1 && mi.config_mode == -1);
  if (mo.starts && mi.starts) {
    put_u32(mo.activation->memory(), 0, 3);
    put_u32(mi.activation->memory(), 0, 3);
    tick();
    CHECK(u32_at(mo.activation->memory(), 0) == 1 && mo.wake->take_signals() == 1);      // (triggered on the clock)
    const std::vector<uint8_t> seq = sequence({{0x90, 60, 100, 62, 90}, {0x80, 60, 0}}, {0x20903E50});
    ByteView buf = mo.buffer(1, 0, 1);
    const uint32_t data_off = uint32_t(mo.ports[{1, 0}].bufs[1].data);
    put_bytes(buf, data_off, seq.data(), seq.size());
    put_u32(buf, 0, 0); put_u32(buf, 4, uint32_t(seq.size()));
    put_u32(mo.io(1, 0), 0, 2); put_u32(mo.io(1, 0), 4, 1);
    put_u32(mo.activation->memory(), 0, 3);
    tick();
    CHECK(synth.count() == 4);
    if (synth.count() == 4) CHECK(synth.got[0][0] == 0x90 && synth.got[1][1] == 62 && synth.got[2][0] == 0x80 && synth.got[3][1] == 0x3E);
    CHECK(u32_at(mo.io(1, 0), 0) == 1 && u32_at(mo.io(1, 0), 4) == 1);      // (handed back)
    g.midi_bus().send(0x90, 70, 30);      // a keyboard
    CHECK(synth.count() == 4);            // (a keyboard's notes are sounded by its own service, not the bus's output)
    // the listener's cycles (each its own buffer): what the bus brought, gathered (the first may have come before the notes did)
    std::vector<MidiMessage> heard;
    for (int i = 0; i < 12; ++i) {
      tick(25);
      if (u32_at(mi.activation->memory(), 0) != 1) continue;
      const uint32_t id = u32_at(mi.io(0, 0), 4);
      CHECK(u32_at(mi.io(0, 0), 0) == 2 && id < 2);
      if (id < 2) {
        ByteView in = mi.buffer(0, 0, id);
        const uint32_t off = uint32_t(mi.ports[{0, 0}].bufs[id].data), size = u32_at(in, 4);
        CHECK(size >= 16 && size < 4096);
        // (the server writes UMP controls to a UMP port)
        const uint8_t* d = in.data() + off;
        for (uint32_t at = 16; at + 24 <= size; at += 24) {
          uint32_t ctl, w; memcpy(&ctl, d + at + 4, 4); memcpy(&w, d + at + 16, 4);
          MidiMessage m; const uint32_t words[1] = {w};
          if (ctl == 4 && ump_to_midi(ArrayView<const uint32_t>(words, 1), &m)) heard.push_back(m);
        }
      }
      put_u32(mi.io(0, 0), 0, 1);
      put_u32(mi.activation->memory(), 0, 3);
    }
    CHECK(heard.size() == 5);
    if (heard.size() == 5) CHECK(heard[0].bytes[1] == 60 && heard[3].bytes[1] == 0x3E && heard[4].bytes[1] == 70);

    // ---- timing and system exclusive: notes at offsets 0, 480 and 960 (0, 10 and 20 ms at 48 kHz), a GM reset in raw bytes at 960, a 13-byte sysex
    // in UMP pieces at 240; the synthesizer hears each at its time, the UMP listener gets them at the same distances and the sysex whole
    synth.got.clear(); synth.sysex.clear(); synth.when.clear();
    std::vector<uint32_t> ump_sx;
    MidiMessage long_sx;
    std::vector<uint8_t> long_bytes = {0xF0, 0x43, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0xF7};
    CHECK(make_sysex(A, ConstByteView(long_bytes.data(), long_bytes.size()), &long_sx));
    write_ump(long_sx, [&](uint32_t w) { ump_sx.push_back(w); });
    CHECK(ump_sx.size() == 6);
    const std::vector<uint8_t> timed = sequence_at({{0, 2, {0x90, 40, 100}},
                                                    {240, 4, words_bytes({ump_sx[0], ump_sx[1]})},
                                                    {300, 4, words_bytes({ump_sx[2], ump_sx[3], ump_sx[4], ump_sx[5]})},
                                                    {480, 2, {0x90, 41, 100}},
                                                    {960, 2, {0x90, 42, 100, 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}}});
    // (the player's next cycle: answered with the sequence)
    for (int i = 0; i < 60 && u32_at(mo.activation->memory(), 0) != 1; ++i) tick(1);
    {
      ByteView buf = mo.buffer(1, 0, 0);
      const uint32_t data_off = uint32_t(mo.ports[{1, 0}].bufs[0].data);
      put_bytes(buf, data_off, timed.data(), timed.size());
      put_u32(buf, 0, 0); put_u32(buf, 4, uint32_t(timed.size()));
      put_u32(mo.io(1, 0), 0, 2); put_u32(mo.io(1, 0), 4, 0);
      put_u32(mo.activation->memory(), 0, 3);
    }
    const uint64_t played = g_now_ns + 1000000ull;      // (taken on the next tick)
    std::vector<std::pair<uint64_t, Control>> got;      // (the listener: its cycle's time, each control)
    for (int i = 0; i < 80; ++i) {
      tick(1);
      if (u32_at(mi.activation->memory(), 0) == 1) {
        const uint32_t id = u32_at(mi.io(0, 0), 4);
        if (u32_at(mi.io(0, 0), 0) == 2 && id < 2) {
          ByteView in = mi.buffer(0, 0, id);
          const uint32_t off = uint32_t(mi.ports[{0, 0}].bufs[id].data), size = u32_at(in, 4);
          for (const Control& c : read_sequence(in.data() + off, size)) got.push_back({g_now_ns.load(), c});
        }
        put_u32(mi.io(0, 0), 0, 1);
        put_u32(mi.activation->memory(), 0, 3);
      }
      if (u32_at(mo.activation->memory(), 0) == 1) put_u32(mo.activation->memory(), 0, 3);      // (the player has nothing more)
    }
    CHECK(synth.got.size() == 3 && synth.sysex.size() == 2 && synth.when.size() == 5);
    if (synth.when.size() == 5 && synth.got.size() == 3 && synth.sysex.size() == 2) {
      // in time order: note 40 at once, the long sysex at 5 ms (its last piece's offset: 300 = 6.25 ms), note 41 at 10 ms, note 42 and the reset at 20 ms
      CHECK(synth.got[0][1] == 40 && synth.got[1][1] == 41 && synth.got[2][1] == 42);
      CHECK(synth.sysex[0] == long_bytes && synth.sysex[1] == std::vector<uint8_t>({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}));
      const int64_t ms[5] = {0, 6, 10, 20, 20};
      for (int k = 0; k < 5; ++k) {
        const int64_t late = int64_t(synth.when[size_t(k)]) - int64_t(played) - ms[k] * 1000000;
        CHECK(late >= 0 && late <= 1000000);      // (within the 1 ms tick)
      }
    }
    // the listener: its controls' times (the cycle's time, a period back, plus the offset) keep the distances
    const double period_ns = 1024.0 * 1e9 / 48000.0;
    std::vector<double> times;
    std::vector<uint32_t> listener_words;
    UmpDecoder dec(A);
    std::vector<std::vector<uint8_t>> listener_sysex;
    for (const auto& [cycle, c] : got) {
      CHECK(c.type == 4 && c.value.size() % 4 == 0);
      std::vector<uint32_t> w(c.value.size() / 4);
      memcpy(w.data(), c.value.data(), c.value.size());
      for (size_t i = 0; i < w.size();) {
        const uint32_t n = ump_packet_words(w[i]);
        if (i + n > w.size()) break;
        MidiMessage x;
        if (dec.decode(ArrayView<const uint32_t>(w.data() + i, n), &x)) {
          times.push_back(double(cycle) - period_ns + c.offset * 1e9 / 48000.0);
          if (x.is_sysex()) listener_sysex.emplace_back(x.wire().data(), x.wire().data() + x.wire().size());
        }
        i += n;
      }
    }
    CHECK(times.size() == 5 && listener_sysex.size() == 2);
    if (times.size() == 5) {
      const double want[5] = {0, 6.25, 10, 20, 20};
      for (int k = 0; k < 5; ++k) CHECK(std::abs((times[size_t(k)] - times[0]) / 1e6 - want[k]) < 1.5);
    }
    if (listener_sysex.size() == 2) CHECK(listener_sysex[0] == long_bytes);
    {
      // a Sequence whose body is not a multiple of 8, its last control's padding past the end: read up to there, no further
      ByteBuffer bad(A);
      bad.put32(27); bad.put32(pod::kSequence); bad.put32(0); bad.put32(0);
      bad.put32(0); bad.put32(2); bad.put32(3); bad.put32(pod::kBytes);
      const uint8_t note[3] = {0x90, 50, 1};
      bad.append(ConstByteView(note, 3));
      const size_t before = synth.got.size();
      for (int i = 0; i < 60 && u32_at(mo.activation->memory(), 0) != 1; ++i) tick(1);
      ByteView b2 = mo.buffer(1, 0, 1);
      put_bytes(b2, uint32_t(mo.ports[{1, 0}].bufs[1].data), bad.view().data(), bad.size());
      put_u32(b2, 0, 0); put_u32(b2, 4, uint32_t(bad.size()));
      put_u32(mo.io(1, 0), 0, 2); put_u32(mo.io(1, 0), 4, 1);
      put_u32(mo.activation->memory(), 0, 3);
      tick();
      CHECK(synth.got.size() == before + 1);
    }
  }
  c.send(0, 7, [](pod::Builder& b) { b.int32(3); });
  c.send(0, 7, [](pod::Builder& b) { b.int32(4); });
  c.take_with_fds();
  CHECK(g.midi_bus().published() >= 3);

  // ---- video: a driver producer with metadata (it runs its own cycles; the server takes what its io says)
  NodeView vp;
  vp.id = 5;
  create_node(c, 5, {{"media.class", "Video/Source"}, {"node.driver", "true"}, {"application.name", "video-src"}, {"node.name", "test-camera"}});
  node_update(c, 5);
  set_active(c, 5, true);
  vp.absorb(c);
  CHECK(vp.config_mode == 1 && !vp.config_format && vp.node);      // (passthrough: the stream's own port)
  c.send(5, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); enum_video(b, 64, 8); port_info(b, 0, {}); });
  vp.absorb(c);
  VideoFormat fixed;
  if (!vp.ports[{1, 0}].format.empty()) {
    pod::Parser fp(ConstByteView(vp.ports[{1, 0}].format.data(), vp.ports[{1, 0}].format.size()));
    pod::Value fv; pod::ObjectReader o;
    if (fp.next(&fv) && o.open(fv)) {
      uint32_t key, fl; pod::Value v;
      while (o.next(&key, &fl, &v)) {
        if (key == 0x20001) v.get_id(&fixed.format);
        if (key == 0x20003 && v.type == pod::kRectangle) { ByteReader r(v.body); fixed.width = r.le32(); fixed.height = r.le32(); }
        if (key == 0x20004 && v.type == pod::kFraction) { ByteReader r(v.body); fixed.rate_num = r.le32(); fixed.rate_den = r.le32(); }
      }
    }
  }
  CHECK(fixed.format == 12 && fixed.width == 64 && fixed.height == 8 && fixed.rate_num == 25);      // (the defaults of its choices)
  c.send(5, 3, [](pod::Builder& b) {
    b.int32(1); b.int32(0); b.int32(1); b.int32(3);
    buffers_param(b, 2, 64 * 8 * 4, 256, (1u << 1) | (1u << 2)); meta_param(b, 1, 32); meta_param(b, 3, 64);
    b.none();
  });
  vp.absorb(c);
  NodeView::PortView& vpp = vp.ports[{1, 0}];
  CHECK(vpp.use && vpp.bufs.size() == 2 && vp.starts == 1);
  if (vpp.bufs.size() == 2) {
    const NodeView::Buf& b0 = vpp.bufs[0];
    CHECK(b0.metas.size() == 2 && b0.metas[0] == std::make_pair(1u, 32u) && b0.metas[1] == std::make_pair(3u, 64u) && b0.dtype == 1 && b0.maxsize >= 2048);
    CHECK(b0.data >= 32 + 64 + 16);      // (the metadata, the chunk, then the data)
  }
  CHECK(u32_at(vp.activation->memory(), 12) == 1);      // (a driver's own trigger is required)
  SharedPointer<VideoChannel> camera = g.video_of(vp.node);
  CHECK(camera && camera->format().width == 64);
  // ---- a watcher (another client): offered the producer's format, given its frames
  NativeClient w(server);
  w.send(0, 1, [](pod::Builder& b) { b.int32(4); });
  w.take_with_fds();
  NodeView vw;
  vw.id = 3;
  create_node(w, 3, {{"media.class", "Stream/Input/Video"}, {"target.object", "test-camera"}});
  node_update(w, 3);
  set_active(w, 3, true);
  vw.absorb(w);
  CHECK(vw.config_mode == 1);
  w.send(3, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(3); b.int32(1); enum_video(b, 320, 240, true); port_info(b, 0, {}); });
  vw.absorb(w);
  CHECK(vw.ports[{0, 0}].format == vpp.format);      // (the producer's format, not its own default)
  w.send(3, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(1); b.int32(2); buffers_param(b, 3, 64 * 8 * 4, 256, 0); meta_param(b, 1, 32); b.none(); });
  vw.absorb(w);
  CHECK(vw.starts == 1 && vw.ports[{0, 0}].bufs.size() == 3);
  if (vp.starts && vw.starts && vpp.bufs.size() == 2) {
    put_u32(vw.activation->memory(), 0, 3);
    tick();
    CHECK(vw.wake->take_signals() == 0);      // (no frame yet)
    // the producer's cycle: a frame in buffer 0 (rows of 256 bytes, each its row number), its header's pts, HAVE_DATA
    ByteView b0 = vp.buffer(1, 0, 0);
    const uint32_t d0 = uint32_t(vpp.bufs[0].data);
    for (uint32_t y = 0; y < 8; ++y) memset(b0.data() + d0 + y * 256, int(y + 1), 256);
    const uint32_t chunk_at = 96;      // (after the header and the damage)
    put_u32(b0, chunk_at, 0); put_u32(b0, chunk_at + 4, 2048); put_u32(b0, chunk_at + 8, 256);
    const int64_t pts = 4242;
    memcpy(b0.data() + 8, &pts, 8);
    put_u32(vp.io(1, 0), 0, 2); put_u32(vp.io(1, 0), 4, 0);
    tick();
    CHECK(camera->frames() == 1 && u32_at(vp.io(1, 0), 0) == 1 && u32_at(vp.io(1, 0), 4) == 0 && vp.wake->take_signals() == 0);
    SharedPointer<VideoFrame> fr = camera->latest();
    CHECK(fr && fr->pts == 4242 && fr->pixels.view()[0] == 1 && fr->pixels.view()[7 * 256] == 8);
    tick(40);      // (the watcher: a new frame, so a cycle)
    CHECK(vw.wake->take_signals() == 1 && u32_at(vw.io(0, 0), 0) == 2);
    const uint32_t id = u32_at(vw.io(0, 0), 4);
    if (id < 3) {
      ByteView in = vw.buffer(0, 0, id);
      const uint32_t off = uint32_t(vw.ports[{0, 0}].bufs[id].data);
      CHECK(in.data()[off] == 1 && in.data()[off + 5 * 256 + 9] == 6 && u32_at(in, 32 + 4) == 2048 && u32_at(in, 32 + 8) == 256);
      uint64_t seq = 99; memcpy(&seq, in.data() + 24, 8);
      CHECK(seq == 0);      // (its first header)
    }
    put_u32(vw.io(0, 0), 0, 1);
    put_u32(vw.activation->memory(), 0, 3);
    tick(40);
    tick(40);
    CHECK(vw.wake->take_signals() == 0);      // (no new frame: no cycle)
  }

  // ---- client-allocated buffers: a port that can allocate (CAN_ALLOC_BUFFERS) and takes memfds gets buffers without data, then gives its own
  NodeView va;
  va.id = 6;
  create_node(c, 6, {{"media.class", "Video/Source"}, {"node.driver", "true"}, {"node.name", "alloc-camera"}});
  node_update(c, 6);
  set_active(c, 6, true);
  c.send(6, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); enum_video(b, 32, 4); port_info(b, 1u << 2, {}); });
  va.absorb(c);
  c.send(6, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(2); buffers_param(b, 2, 512, 128, 1u << 2); meta_param(b, 1, 32); b.none(); });
  va.absorb(c);
  NodeView::PortView& vap = va.ports[{1, 0}];
  CHECK(vap.use && vap.flags == 1 && vap.bufs.size() == 2 && va.starts == 0);
  if (vap.bufs.size() == 2) CHECK(vap.bufs[0].dtype == (1u << 2) && vap.bufs[0].maxsize == 0 && vap.bufs[0].metas.size() == 1);
  auto send_port_buffers = [&](std::vector<Fd> fds, std::vector<std::array<int32_t, 4>> datas) {      // type, fd index, mapoffset, maxsize
    ByteBuffer body(A);
    pod::Builder b(body);
    const uint64_t f = b.begin_struct();
    b.int32(1); b.int32(0); b.int32(-1); b.int32(int32_t(datas.size()));
    for (const auto& d : datas) { b.int32(1); b.id(uint32_t(d[0])); b.fd(d[1]); b.int32(3); b.int32(d[2]); b.int32(d[3]); }
    b.end(f);
    ByteBuffer msg(A);
    msg.put32(6); msg.put32(6u << 24 | uint32_t(body.size())); msg.put32(++c.seq); msg.put32(uint32_t(fds.size()));
    msg.append(body.view());
    List<Fd> list(A);
    for (Fd& fd : fds) list.push_back(fd);
    server.receive_with_fds(c.handle, msg.view(), list);
  };
  Fd own0 = Fd::make_as<FakeFd>(A, 4096, false), own1 = Fd::make_as<FakeFd>(A, 4096, false);
  // refused: a descriptor that did not come, a buffer outside its memfd, a DMA-BUF
  send_port_buffers({own0}, {{2, 0, 0, 512}, {2, 1, 0, 512}});
  va.absorb(c);
  send_port_buffers({own0, own1}, {{2, 0, 0, 512}, {2, 1, 3700, 512}});
  va.absorb(c);
  send_port_buffers({own0, own1}, {{3, 0, 0, 512}, {3, 1, 0, 512}});
  va.absorb(c);
  CHECK(va.errors == 3 && va.starts == 0);
  send_port_buffers({own0, own1}, {{2, 0, 0, 512}, {2, 1, 1024, 512}});
  va.absorb(c);
  CHECK(va.errors == 3 && va.starts == 1);
  if (va.starts && vap.bufs.size() == 2) {
    // the client's frame is in its own memory (buffer 1: at 1024 of its second memfd)
    ByteView mine = own1->memory();
    for (uint32_t y = 0; y < 4; ++y) memset(mine.data() + 1024 + y * 128, int(0x40 + y), 128);
    ByteView part1 = va.buffer(1, 0, 1);
    put_u32(part1, 32, 0); put_u32(part1, 36, 512); put_u32(part1, 40, 128);
    put_u32(va.io(1, 0), 0, 2); put_u32(va.io(1, 0), 4, 1);
    tick();
    SharedPointer<VideoChannel> ch = g.video_of(va.node);
    SharedPointer<VideoFrame> fr = ch ? ch->latest() : SharedPointer<VideoFrame>();
    CHECK(fr && fr->format.width == 32 && fr->pixels.view()[0] == 0x40 && fr->pixels.view()[3 * 128 + 127] == 0x43);
  }
  // DMA-BUFs: a camera whose buffers are GPU images; each frame is copied out of the image before it is read, and one the GPU still draws is not taken
  {
    NodeView vd;
    vd.id = 9;
    create_node(c, 9, {{"media.class", "Video/Source"}, {"node.driver", "true"}, {"node.name", "gpu-camera"}});
    node_update(c, 9);
    set_active(c, 9, true);
    c.send(9, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); enum_video(b, 32, 4); port_info(b, 1u << 2, {}); });
    vd.absorb(c);
    c.send(9, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(2); buffers_param(b, 2, 512, 128, 1u << 3); meta_param(b, 1, 32); b.none(); });
    vd.absorb(c);
    NodeView::PortView& vdp = vd.ports[{1, 0}];
    CHECK(vdp.use && vdp.flags == 1 && vdp.bufs.size() == 2 && vdp.bufs[0].dtype == (1u << 3));
    SharedPointer<FakeGpuFd> g0 = SharedPointer<FakeGpuFd>::make(A, 512), g1 = SharedPointer<FakeGpuFd>::make(A, 512);
    auto send_gpu_buffers = [&](std::vector<Fd> fds, int32_t type) {
      ByteBuffer body(A);
      pod::Builder b(body);
      const uint64_t f = b.begin_struct();
      b.int32(1); b.int32(0); b.int32(-1); b.int32(2);
      for (int k = 0; k < 2; ++k) { b.int32(1); b.id(uint32_t(type)); b.fd(k); b.int32(3); b.int32(0); b.int32(512); }
      b.end(f);
      ByteBuffer msg(A);
      msg.put32(9); msg.put32(6u << 24 | uint32_t(body.size())); msg.put32(++c.seq); msg.put32(2);
      msg.append(body.view());
      List<Fd> list(A);
      for (Fd& fd : fds) list.push_back(fd);
      server.receive_with_fds(c.handle, msg.view(), list);
    };
    send_gpu_buffers({Fd(g0), Fd(g1)}, 3);
    vd.absorb(c);
    CHECK(vd.errors == 0 && vd.starts == 1);
    if (vd.starts) {
      for (uint32_t y = 0; y < 4; ++y) memset(g1->image.data() + y * 128, int(0x60 + y), 128);
      ByteView part1 = vd.buffer(1, 0, 1);
      put_u32(part1, 32, 0); put_u32(part1, 36, 512); put_u32(part1, 40, 128);
      g1->busy = true;      // (still being drawn: not taken, handed back)
      put_u32(vd.io(1, 0), 0, 2); put_u32(vd.io(1, 0), 4, 1);
      tick();
      SharedPointer<VideoChannel> ch = g.video_of(vd.node);
      CHECK(ch && ch->frames() == 0 && g1->syncs == 1 && u32_at(vd.io(1, 0), 0) == 1);
      g1->busy = false;
      put_u32(vd.io(1, 0), 0, 2); put_u32(vd.io(1, 0), 4, 1);
      tick();
      SharedPointer<VideoFrame> fr = ch ? ch->latest() : SharedPointer<VideoFrame>();
      CHECK(g1->syncs == 2 && fr && fr->pixels.view()[0] == 0x60 && fr->pixels.view()[3 * 128 + 127] == 0x63);
    }
    c.send(0, 7, [](pod::Builder& b) { b.int32(9); });
    c.take_with_fds();
  }
  // a port_buffers for a port that was not asked to allocate is refused
  {
    ByteBuffer body(A);
    pod::Builder b(body);
    const uint64_t f = b.begin_struct();
    b.int32(1); b.int32(0); b.int32(-1); b.int32(2);
    b.end(f);
    ByteBuffer msg(A);
    msg.put32(5); msg.put32(6u << 24 | uint32_t(body.size())); msg.put32(++c.seq); msg.put32(0);
    msg.append(body.view());
    server.receive(c.handle, msg.view());
    vp.absorb(c);
    CHECK(vp.errors == 1);
  }

  // ---- planes and a format change: an NV12 camera with a block a plane, a watcher with them one after another; then the camera says I420 at another
  // size: both are given the new Format, their old memory is removed and new buffers come (I420: three blocks); then an MJPEG camera and its watcher
  {
    NodeView yc;
    yc.id = 7;
    create_node(c, 7, {{"media.class", "Video/Source"}, {"node.driver", "true"}, {"node.name", "yuv-camera"}});
    node_update(c, 7);
    set_active(c, 7, true);
    c.send(7, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); enum_video_fixed(b, 23, 8, 4); port_info(b, 0, {}); });
    yc.absorb(c);
    c.send(7, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(2); buffers_param(b, 2, 64, 8, 0, 2); meta_param(b, 1, 32); b.none(); });
    yc.absorb(c);
    NodeView::PortView& ycp = yc.ports[{1, 0}];
    CHECK(ycp.use && ycp.bufs.size() == 2 && yc.starts == 1);
    if (ycp.bufs.size() == 2) CHECK(ycp.bufs[0].datas.size() == 2 && ycp.bufs[0].datas[1].data >= ycp.bufs[0].datas[0].data + 32);
    NodeView yw;
    yw.id = 4;
    create_node(w, 4, {{"media.class", "Stream/Input/Video"}, {"target.object", "yuv-camera"}});
    node_update(w, 4);
    set_active(w, 4, true);
    w.send(4, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(3); b.int32(1); enum_video_choice(b, {23, 2}, 0, 0); port_info(b, 0, {}); });
    yw.absorb(w);
    CHECK(yw.ports[{0, 0}].format == ycp.format);
    w.send(4, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 0, 0, 0); b.none(); });
    yw.absorb(w);
    NodeView::PortView& ywp = yw.ports[{0, 0}];
    CHECK(yw.starts == 1 && ywp.bufs.size() == 2 && ywp.bufs[0].datas.size() == 1 && ywp.bufs[0].maxsize >= 48);
    // a frame: luma in block 0 (each byte its index), chroma in block 1 (100 + index), each its own chunk
    auto produce = [&](NodeView& cam, uint32_t buffer, const std::vector<std::vector<uint8_t>>& planes, const std::vector<uint32_t>& strides) {
      NodeView::PortView& cp = cam.ports[{1, 0}];
      ByteView part = cam.buffer(1, 0, buffer);
      const NodeView::Buf& bf = cp.bufs[buffer];
      uint32_t meta_bytes = 0;
      for (const auto& m : bf.metas) meta_bytes += (m.second + 7) & ~7u;
      for (size_t k = 0; k < planes.size() && k < bf.datas.size(); ++k) {
        memcpy(part.data() + bf.datas[k].data, planes[k].data(), planes[k].size());
        put_u32(part, meta_bytes + 16 * uint32_t(k), 0); put_u32(part, meta_bytes + 16 * uint32_t(k) + 4, uint32_t(planes[k].size()));
        put_u32(part, meta_bytes + 16 * uint32_t(k) + 8, strides[k]);
      }
      put_u32(cam.io(1, 0), 0, 2); put_u32(cam.io(1, 0), 4, buffer);
    };
    auto watched = [&](NodeView& view) -> std::vector<uint8_t> {      // (the watcher's buffer now, by its chunk; empty when none came)
      if (u32_at(view.io(0, 0), 0) != 2) return {};
      const uint32_t id = u32_at(view.io(0, 0), 4);
      NodeView::PortView& vp2 = view.ports[{0, 0}];
      if (id >= vp2.bufs.size()) return {};
      ByteView part = view.buffer(0, 0, id);
      uint32_t meta_bytes = 0;
      for (const auto& m : vp2.bufs[id].metas) meta_bytes += (m.second + 7) & ~7u;
      const uint32_t size = u32_at(part, meta_bytes + 4);
      std::vector<uint8_t> out(part.data() + vp2.bufs[id].data, part.data() + vp2.bufs[id].data + size);
      put_u32(view.io(0, 0), 0, 1);
      put_u32(view.activation->memory(), 0, 3);
      return out;
    };
    std::vector<uint8_t> luma(32), chroma(16);
    for (size_t k = 0; k < 32; ++k) luma[k] = uint8_t(k);
    for (size_t k = 0; k < 16; ++k) chroma[k] = uint8_t(100 + k);
    if (yc.starts && yw.starts && ycp.bufs.size() == 2) {
      put_u32(yw.activation->memory(), 0, 3);
      produce(yc, 0, {luma, chroma}, {8, 8});
      tick();
      tick(40);
      const std::vector<uint8_t> got = watched(yw);
      CHECK(got.size() == 48 && got[31] == 31 && got[32] == 100 && got[47] == 115);
    }
    // the camera changes to I420 6 x 4
    const uint32_t old_camera_mem = ycp.bufs.empty() ? 0 : ycp.bufs[0].mem, old_watcher_mem = ywp.bufs.empty() ? 0 : ywp.bufs[0].mem;
    c.send(7, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(1); enum_video_fixed(b, 2, 6, 4); b.none(); });
    yc.absorb(c);
    CHECK(ycp.formats == 2 && ycp.uses == 1 && std::count(yc.removed.begin(), yc.removed.end(), old_camera_mem) == 1);
    c.send(7, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(2); buffers_param(b, 2, 64, 6, 0, 3); meta_param(b, 1, 32); b.none(); });
    yc.absorb(c);
    CHECK(ycp.uses == 2 && ycp.bufs.size() == 2 && ycp.bufs[0].mem == old_camera_mem && yc.starts == 1);      // (the removed id, again: as the pool does)
    if (ycp.bufs.size() == 2) CHECK(ycp.bufs[0].datas.size() == 3);
    tick();
    yw.absorb(w);
    CHECK(ywp.formats == 2 && std::count(yw.removed.begin(), yw.removed.end(), old_watcher_mem) == 1);
    {
      VideoFormat now_format;
      pod::Parser fp(ConstByteView(ywp.format.data(), ywp.format.size()));
      pod::Value fv; pod::ObjectReader o;
      if (fp.next(&fv) && o.open(fv)) {
        uint32_t key, fl; pod::Value v;
        while (o.next(&key, &fl, &v)) {
          if (key == 0x20001) v.get_id(&now_format.format);
          if (key == 0x20003 && v.type == pod::kRectangle) { ByteReader r(v.body); now_format.width = r.le32(); now_format.height = r.le32(); }
        }
      }
      CHECK(now_format.format == 2 && now_format.width == 6 && now_format.height == 4);
    }
    w.send(4, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 0, 0, 0); b.none(); });
    yw.absorb(w);
    CHECK(ywp.uses == 2 && ywp.bufs.size() == 2 && ywp.bufs[0].mem == old_watcher_mem);
    if (ycp.bufs.size() == 2 && ywp.bufs.size() == 2) {
      std::vector<uint8_t> y6(24), u3(6), v3(6);
      for (size_t k = 0; k < 24; ++k) y6[k] = uint8_t(k);
      for (size_t k = 0; k < 6; ++k) { u3[k] = uint8_t(50 + k); v3[k] = uint8_t(80 + k); }
      put_u32(yw.activation->memory(), 0, 3);
      produce(yc, 1, {y6, u3, v3}, {6, 3, 3});
      tick();
      tick(40);
      const std::vector<uint8_t> got = watched(yw);
      CHECK(got.size() == 36 && got[23] == 23 && got[24] == 50 && got[30] == 80 && got[35] == 85);
      SharedPointer<VideoChannel> ych = g.video_of(yc.node);
      CHECK(ych && ych->format().format == 2 && ych->latest() && ych->latest()->pixels.size() == 36);
    }
    // MJPEG: whole compressed frames of any length
    NodeView mc;
    mc.id = 8;
    create_node(c, 8, {{"media.class", "Video/Source"}, {"node.driver", "true"}, {"node.name", "mjpeg-camera"}});
    node_update(c, 8);
    set_active(c, 8, true);
    c.send(8, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); enum_video_fixed(b, 0, 16, 16, 0x20002); port_info(b, 0, {}); });
    mc.absorb(c);
    c.send(8, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 1000, 0, 0); b.none(); });
    mc.absorb(c);
    NodeView mw;
    mw.id = 5;
    create_node(w, 5, {{"media.class", "Stream/Input/Video"}, {"target.object", "mjpeg-camera"}});
    node_update(w, 5);
    set_active(w, 5, true);
    w.send(5, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(3); b.int32(1); enum_video_fixed(b, 0, 16, 16, 0x20002); port_info(b, 0, {}); });
    mw.absorb(w);
    w.send(5, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 1000, 0, 0); b.none(); });
    mw.absorb(w);
    CHECK(mc.starts == 1 && mw.starts == 1 && mw.ports[{0, 0}].format == mc.ports[{1, 0}].format);
    if (mc.starts && mw.starts) {
      std::vector<uint8_t> jpeg(300);
      for (size_t k = 0; k < jpeg.size(); ++k) jpeg[k] = uint8_t(k * 3);
      jpeg[0] = 0xFF; jpeg[1] = 0xD8;
      put_u32(mw.activation->memory(), 0, 3);
      produce(mc, 0, {jpeg}, {0});
      tick();
      tick(40);
      CHECK(watched(mw) == jpeg);
    }
    // a real Motion JPEG camera (64 x 48) and a watcher that takes raw BGRA only: offered BGRA, given each picture decoded
    NodeView rc;
    rc.id = 10;
    create_node(c, 10, {{"media.class", "Video/Source"}, {"node.driver", "true"}, {"node.name", "real-mjpeg"}});
    node_update(c, 10);
    set_active(c, 10, true);
    c.send(10, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); enum_video_fixed(b, 0, 64, 48, 0x20002); port_info(b, 0, {}); });
    rc.absorb(c);
    c.send(10, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 8192, 0, 0); b.none(); });
    rc.absorb(c);
    NodeView rw;
    rw.id = 7;
    create_node(w, 7, {{"media.class", "Stream/Input/Video"}, {"target.object", "real-mjpeg"}});
    node_update(w, 7);
    set_active(w, 7, true);
    w.send(7, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(3); b.int32(1); enum_video_choice(b, {12}, 64, 48); port_info(b, 0, {}); });
    rw.absorb(w);
    w.send(7, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 0, 0, 0); b.none(); });
    rw.absorb(w);
    CHECK(rc.starts == 1 && rw.starts == 1 && rw.ports[{0, 0}].bufs.size() == 2);
    const std::vector<uint8_t> real = read_test_file("tests/data/mjpeg_standard_tables.jpg");
    if (rc.starts && rw.starts && real.size() > 100) {
      put_u32(rw.activation->memory(), 0, 3);
      produce(rc, 0, {real}, {0});
      tick();
      tick(40);
      const std::vector<uint8_t> got = watched(rw);
      CHECK(got.size() == 64 * 48 * 4 && got[3] == 0xFF);
      hardware::JpegDecoder dec(A);
      std::vector<uint32_t> want(64 * 48);
      CHECK(dec.decode(ConstByteView(real.data(), real.size())) && dec.output_bgra(want.data(), 64));
      if (got.size() == 64 * 48 * 4) CHECK(memcmp(got.data(), want.data(), got.size()) == 0);
    }
    c.send(0, 7, [](pod::Builder& b) { b.int32(10); });
    w.send(0, 7, [](pod::Builder& b) { b.int32(7); });
    // an H.264 camera (64 x 48) and a watcher that takes raw BGRA only: not offered raw while the graph has no decoders (it is given the H.264);
    // with them, offered BGRA and given each picture decoded (what a decoder of the codecs gives), at the size it asks for
    {
      SoftwareDecoders decoders;
      const std::vector<std::vector<uint8_t>> avc = encode_h264(64, 48, 4, 30);
      uint32_t aw = 0, ah = 0;
      const std::vector<std::vector<uint8_t>> want = reference_pictures(hardware::VideoCodec::H264, avc, &aw, &ah);
      CHECK(avc.size() == 4 && want.size() == 4);
      for (int with = 0; with < 2; ++with) {
        g.set_video_decoders(with ? &decoders : nullptr);
        NodeView hc;
        hc.id = 11;
        create_node(c, 11, {{"media.class", "Video/Source"}, {"node.driver", "true"}, {"node.name", "h264-camera"}});
        node_update(c, 11);
        set_active(c, 11, true);
        c.send(11, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); enum_video_fixed(b, 0, 64, 48, 0x20001); port_info(b, 0, {}); });
        hc.absorb(c);
        c.send(11, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 65536, 0, 0); b.none(); });
        hc.absorb(c);
        NodeView hw;
        hw.id = 9;
        create_node(w, 9, {{"media.class", "Stream/Input/Video"}, {"target.object", "h264-camera"}});
        node_update(w, 9);
        set_active(w, 9, true);
        w.send(9, 3, [with](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(3); b.int32(1); enum_video_choice(b, {12}, with ? 32 : 0, with ? 24 : 0); port_info(b, 0, {}); });
        hw.absorb(w);
        VideoFormat offered;
        {
          pod::Parser fp(ConstByteView(hw.ports[{0, 0}].format.data(), hw.ports[{0, 0}].format.size()));
          pod::Value fv; pod::ObjectReader o;
          if (fp.next(&fv) && o.open(fv)) {
            uint32_t key, fl; pod::Value v;
            while (o.next(&key, &fl, &v)) {
              if (key == 2) v.get_id(&offered.subtype);
              if (key == 0x20001) v.get_id(&offered.format);
              if (key == 0x20003 && v.body.size() >= 8) { memcpy(&offered.width, v.body.data(), 4); memcpy(&offered.height, v.body.data() + 4, 4); }
            }
          }
        }
        if (!with) {
          CHECK(offered.subtype == 0x20001);      // (as the producer sends it)
        } else {
          CHECK(offered.subtype == VideoFormat::kSubtypeRaw && offered.format == 12 && offered.width == 32 && offered.height == 24);
          w.send(9, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 0, 0, 0); b.none(); });
          hw.absorb(w);
          CHECK(hc.starts == 1 && hw.starts == 1);
          if (hc.starts && hw.starts) {
            for (size_t i = 0; i < avc.size(); ++i) {
              put_u32(hw.activation->memory(), 0, 3);
              produce(hc, 0, {avc[i]}, {0});
              tick();
              tick(40);
              const std::vector<uint8_t> got = watched(hw);
              CHECK(got.size() == 32 * 24 * 4);
              if (got.size() == 32 * 24 * 4 && i < want.size()) {      // (the reference picture halved: each pixel the average of its 2 x 2)
                int worst = 0;
                for (uint32_t y = 0; y < 24; ++y)
                  for (uint32_t x = 0; x < 32; ++x)
                    for (int ch = 0; ch < 3; ++ch) {
                      int sum = 0;
                      for (uint32_t dy = 0; dy < 2; ++dy)
                        for (uint32_t dx = 0; dx < 2; ++dx) sum += want[i][((2 * y + dy) * 64 + 2 * x + dx) * 4 + uint32_t(ch)];
                      worst = std::max(worst, abs((sum + 2) / 4 - int(got[(y * 32 + x) * 4 + uint32_t(ch)])));
                    }
                CHECK(worst <= 1);
              }
            }
          }
        }
        c.send(0, 7, [](pod::Builder& b) { b.int32(11); });
        w.send(0, 7, [](pod::Builder& b) { b.int32(9); });
        tick();
      }
      g.set_video_decoders(nullptr);
    }
    // a watcher that takes BGRA only, of the I420 camera: offered BGRA, given each frame converted (BT.601 limited: Y 16 black, 235 white)
    NodeView cw;
    cw.id = 6;
    create_node(w, 6, {{"media.class", "Stream/Input/Video"}, {"target.object", "yuv-camera"}});
    node_update(w, 6);
    set_active(w, 6, true);
    w.send(6, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(3); b.int32(1); enum_video_choice(b, {12}, 6, 4); port_info(b, 0, {}); });
    cw.absorb(w);
    {
      VideoFormat offered;
      pod::Parser fp(ConstByteView(cw.ports[{0, 0}].format.data(), cw.ports[{0, 0}].format.size()));
      pod::Value fv; pod::ObjectReader o;
      if (fp.next(&fv) && o.open(fv)) {
        uint32_t key, fl; pod::Value v;
        while (o.next(&key, &fl, &v)) if (key == 0x20001) v.get_id(&offered.format);
      }
      CHECK(offered.format == 12);
    }
    w.send(6, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 0, 0, 0); b.none(); });
    cw.absorb(w);
    CHECK(cw.starts == 1 && cw.ports[{0, 0}].bufs.size() == 2);
    if (cw.starts && ycp.bufs.size() == 2) {
      // white on the left half, black on the right, no colour
      std::vector<uint8_t> y6(24), u3(6, 128), v3(6, 128);
      for (size_t k = 0; k < 24; ++k) y6[k] = (k % 6) < 3 ? 235 : 16;
      put_u32(cw.activation->memory(), 0, 3);
      produce(yc, 0, {y6, u3, v3}, {6, 3, 3});
      tick();
      tick(40);
      const std::vector<uint8_t> got = watched(cw);
      CHECK(got.size() == 6 * 4 * 4);
      if (got.size() == 96) CHECK(got[0] == 255 && got[1] == 255 && got[2] == 255 && got[3] == 255 && got[5 * 4] == 0 && got[5 * 4 + 2] == 0 && got[23 * 4 + 3] == 255);
    }
    // a watcher of one fixed size, half the camera's: offered that size, given each frame scaled (each pixel two by two averaged)
    NodeView sw;
    sw.id = 8;
    create_node(w, 8, {{"media.class", "Stream/Input/Video"}, {"target.object", "yuv-camera"}});
    node_update(w, 8);
    set_active(w, 8, true);
    w.send(8, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(3); b.int32(1); enum_video_choice(b, {12}, 3, 2); port_info(b, 0, {}); });
    sw.absorb(w);
    {
      uint32_t ow = 0, oh = 0;
      pod::Parser fp(ConstByteView(sw.ports[{0, 0}].format.data(), sw.ports[{0, 0}].format.size()));
      pod::Value fv; pod::ObjectReader o;
      if (fp.next(&fv) && o.open(fv)) {
        uint32_t key, fl; pod::Value v;
        while (o.next(&key, &fl, &v)) if (key == 0x20003 && v.type == pod::kRectangle) { ByteReader r(v.body); ow = r.le32(); oh = r.le32(); }
      }
      CHECK(ow == 3 && oh == 2);
    }
    w.send(8, 3, [](pod::Builder& b) { b.int32(0); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 0, 0, 0); b.none(); });
    sw.absorb(w);
    CHECK(sw.starts == 1);
    if (sw.starts && ycp.bufs.size() == 2) {
      std::vector<uint8_t> y6(24), u3(6, 128), v3(6, 128);
      for (size_t k = 0; k < 24; ++k) y6[k] = (k % 6) < 2 ? 235 : 16;      // (white in the first two columns: the first scaled column white)
      put_u32(sw.activation->memory(), 0, 3);
      produce(yc, 1, {y6, u3, v3}, {6, 3, 3});
      tick();
      tick(40);
      const std::vector<uint8_t> got = watched(sw);
      CHECK(got.size() == 3 * 2 * 4);
      if (got.size() == 24) CHECK(got[0] == 255 && got[4] == 0 && got[12] == 255);
    }
    w.send(0, 7, [](pod::Builder& b) { b.int32(8); });
    w.send(0, 7, [](pod::Builder& b) { b.int32(6); });
    c.send(0, 7, [](pod::Builder& b) { b.int32(7); });
    c.send(0, 7, [](pod::Builder& b) { b.int32(8); });
    w.send(0, 7, [](pod::Builder& b) { b.int32(4); });
    w.send(0, 7, [](pod::Builder& b) { b.int32(5); });
    c.take_with_fds();
    w.take_with_fds();
  }

  // ---- an audio driver follows the server's clock: its position is an activation of the server's, and it is triggered as any other
  {
    NodeView ad;
    ad.id = 7;
    create_node(c, 7, {{"media.class", "Stream/Output/Audio"}, {"node.driver", "true"}});
    node_update(c, 7);
    set_active(c, 7, true);
    ad.absorb(c);
    CHECK(ad.config_mode == 2 && ad.set_io.count(7) && std::get<0>(ad.set_io[7]) != ad.activation_mem && ad.mems.count(std::get<0>(ad.set_io[7])));
    c.send(7, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); build_format_param(b, 3, PcmFormat{SampleFormat::F32Le, 48000, 2}); b.none(); });
    c.send(7, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(1); b.int32(1); buffers_param(b, 2, 32768, 8, 0); b.none(); });
    ad.absorb(c);
    CHECK(ad.starts == 1);
    put_u32(ad.activation->memory(), 0, 3);
    tick();
    CHECK(ad.wake->take_signals() == 1 && u32_at(ad.mems[std::get<0>(ad.set_io[7])]->memory(), 560 + 96) == 1024);
    c.send(0, 7, [](pod::Builder& b) { b.int32(7); });
    c.take_with_fds();
  }

  // ---- an adapter's port that says no EnumFormat (as pwnodetest does) still gets the adapter's format, buffers and Start
  {
    NodeView an;
    an.id = 9;
    create_node(c, 9, {{"media.class", "Stream/Output/Audio"}});
    node_update(c, 9);
    set_active(c, 9, true);
    c.send(9, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(0); b.none(); });
    c.send(9, 3, [](pod::Builder& b) { b.int32(1); b.int32(0); b.int32(3); b.int32(1); buffers_param(b, 3, 8192, 0, 0); b.none(); });
    an.absorb(c);
    CHECK(an.config_mode == 2 && !an.ports[{1, 0}].format.empty() && an.ports[{1, 0}].bufs.size() == 3 && an.starts == 1);
    c.send(0, 7, [](pod::Builder& b) { b.int32(9); });
    c.take_with_fds();
  }

  // ---- a pw_filter with two float outputs: what they play is put together into one stereo stream
  {
    NodeView fl;
    fl.id = 8;
    create_node(c, 8, {{"media.class", "Stream/Output/Audio"}, {"media.type", "Audio"}});
    for (int port = 0; port < 2; ++port)
      c.send(8, 3, [&](pod::Builder& b) { b.int32(1); b.int32(port); b.int32(3); b.int32(1); enum_dsp_audio(b); port_info(b, 0, {{"format.dsp", "32 bit float mono audio"}}); });
    node_update(c, 8);
    set_active(c, 8, true);
    for (int port = 0; port < 2; ++port)
      c.send(8, 3, [&](pod::Builder& b) { b.int32(1); b.int32(port); b.int32(1); b.int32(1); buffers_param(b, 2, 32768, 4, 0); b.none(); });
    fl.absorb(c);
    CHECK(fl.config_mode == -1 && fl.starts == 1 && fl.ports[{1, 0}].bufs.size() == 2 && fl.ports[{1, 1}].bufs.size() == 2);
    if (fl.starts == 1 && fl.ports[{1, 1}].bufs.size() == 2) {
      put_u32(fl.activation->memory(), 0, 3);
      tick();
      CHECK(fl.wake->take_signals() == 1);
      for (int port = 0; port < 2; ++port) {
        ByteView buf = fl.buffer(1, port, 0);
        const uint32_t off = uint32_t(fl.ports[{1, port}].bufs[0].data);
        const float v = port == 0 ? 0.25f : -0.25f;
        for (uint32_t i = 0; i < 1024; ++i) memcpy(buf.data() + off + 4 * i, &v, 4);
        put_u32(buf, 0, 0); put_u32(buf, 4, 4096);
        put_u32(fl.io(1, port), 0, 2); put_u32(fl.io(1, port), 4, 0);
      }
      put_u32(fl.activation->memory(), 0, 3);
      dev.take();
      tick();
      mixer.pump();
      std::vector<int16_t> out = dev.take();
      CHECK(out.size() > 400 && abs(out[200] - 8191) < 40 && abs(out[201] + 8191) < 40);
    }
    c.send(0, 7, [](pod::Builder& b) { b.int32(8); });
    c.take_with_fds();
  }

  // ---- an effect: a pw_filter with two float inputs and two float outputs is one node that records from the microphone and plays to the speakers;
  // and one with six outputs (5.1) is folded into the speakers' two
  {
    NodeView fx;
    fx.id = 9;
    create_node(c, 9, {{"media.class", "Audio/Filter"}, {"media.type", "Audio"}, {"node.name", "tcos-effect"}});
    for (int dir = 0; dir < 2; ++dir)
      for (int port = 0; port < 2; ++port)
        c.send(9, 3, [&](pod::Builder& b) { b.int32(dir); b.int32(port); b.int32(3); b.int32(1); enum_dsp_audio(b); port_info(b, 0, {{"format.dsp", "32 bit float mono audio"}}); });
    node_update(c, 9);
    set_active(c, 9, true);
    for (int dir = 0; dir < 2; ++dir)
      for (int port = 0; port < 2; ++port)
        c.send(9, 3, [&](pod::Builder& b) { b.int32(dir); b.int32(port); b.int32(1); b.int32(1); buffers_param(b, 2, 32768, 4, 0); b.none(); });
    fx.absorb(c);
    CHECK(fx.starts == 1 && fx.ports[{0, 1}].bufs.size() == 2 && fx.ports[{1, 1}].bufs.size() == 2);
    uint32_t in_ports = 0, out_ports = 0;
    std::string fx_class;
    g.visit(fx.node, [&](const GraphObject& o) { in_ports = o.input_ports; out_ports = o.output_ports; fx_class = std::string(o.props.get("media.class").data(), o.props.get("media.class").size()); });
    CHECK(in_ports == 2 && out_ports == 2 && fx_class == "Audio/Filter" && g.playback_of(fx.node) && g.record_of(fx.node));
    if (fx.starts == 1 && fx.ports[{0, 1}].bufs.size() == 2) {
      mic.add(4800, 8000, -4000);
      hub.pump();
      put_u32(fx.activation->memory(), 0, 3);
      tick();
      CHECK(fx.wake->take_signals() == 1);
      // its inputs: the microphone's left and right, a port each
      for (int port = 0; port < 2; ++port) {
        CHECK(u32_at(fx.io(0, port), 0) == 2);
        const uint32_t id = u32_at(fx.io(0, port), 4);
        if (id >= 2) continue;
        ByteView buf = fx.buffer(0, port, id);
        const uint32_t off = uint32_t(fx.ports[{0, port}].bufs[id].data);
        CHECK(u32_at(buf, 4) == 4096);
        float v = 0; memcpy(&v, buf.data() + off + 4 * 500, 4);
        CHECK(std::abs(v - (port == 0 ? 8000.f : -4000.f) / 32768.f) < 0.01f);
      }
      // its outputs: the effect's (half of the input) to the speakers
      for (int port = 0; port < 2; ++port) {
        ByteView buf = fx.buffer(1, port, 0);
        const uint32_t off = uint32_t(fx.ports[{1, port}].bufs[0].data);
        const float v = port == 0 ? 0.125f : -0.0625f;
        for (uint32_t i = 0; i < 1024; ++i) memcpy(buf.data() + off + 4 * i, &v, 4);
        put_u32(buf, 0, 0); put_u32(buf, 4, 4096);
        put_u32(fx.io(1, port), 0, 2); put_u32(fx.io(1, port), 4, 0);
      }
      put_u32(fx.activation->memory(), 0, 3);
      dev.take();
      tick();
      mixer.pump();
      std::vector<int16_t> out = dev.take();
      CHECK(out.size() > 400 && abs(out[200] - 4096) < 40 && abs(out[201] + 2048) < 40);
    }
    c.send(0, 7, [](pod::Builder& b) { b.int32(9); });
    c.take_with_fds();
    g.refresh();
    CHECK(!g.playback_of(fx.node) && !g.record_of(fx.node));

    NodeView sx;
    sx.id = 10;
    create_node(c, 10, {{"media.class", "Stream/Output/Audio"}, {"media.type", "Audio"}});
    for (int port = 0; port < 6; ++port)
      c.send(10, 3, [&](pod::Builder& b) { b.int32(1); b.int32(port); b.int32(3); b.int32(1); enum_dsp_audio(b); port_info(b, 0, {{"format.dsp", "32 bit float mono audio"}}); });
    node_update(c, 10);
    set_active(c, 10, true);
    for (int port = 0; port < 6; ++port)
      c.send(10, 3, [&](pod::Builder& b) { b.int32(1); b.int32(port); b.int32(1); b.int32(1); buffers_param(b, 2, 32768, 4, 0); b.none(); });
    sx.absorb(c);
    CHECK(sx.starts == 1 && sx.ports[{1, 5}].bufs.size() == 2);
    SharedPointer<PlaybackStream> six = g.playback_of(sx.node);
    CHECK(six && six->format().channels == 6);
    if (sx.starts == 1 && sx.ports[{1, 5}].bufs.size() == 2) {
      put_u32(sx.activation->memory(), 0, 3);
      tick();
      // FL only: the left speaker alone
      for (int port = 0; port < 6; ++port) {
        ByteView buf = sx.buffer(1, port, 0);
        const uint32_t off = uint32_t(sx.ports[{1, port}].bufs[0].data);
        const float v = port == 0 ? 0.5f : 0.f;
        for (uint32_t i = 0; i < 1024; ++i) memcpy(buf.data() + off + 4 * i, &v, 4);
        put_u32(buf, 0, 0); put_u32(buf, 4, 4096);
        put_u32(sx.io(1, port), 0, 2); put_u32(sx.io(1, port), 4, 0);
      }
      put_u32(sx.activation->memory(), 0, 3);
      dev.take();
      tick();
      mixer.pump();
      std::vector<int16_t> out = dev.take();
      CHECK(out.size() > 400 && out[200] > 4000 && abs(out[201]) < 40);
    }
    c.send(0, 7, [](pod::Builder& b) { b.int32(10); });
    c.take_with_fds();
  }

  // ---- the adapter: a virtual sink (support.null-audio-sink); a stream plays into it, another records what plays there
  {
    auto adapter = [&](NativeClient& cl, uint32_t id, const Props& props) {
      cl.send(0, 6, [&](pod::Builder& b) {
        b.string("adapter"); b.string("PipeWire:Interface:Node"); b.int32(3);
        const uint64_t d = b.begin_struct();
        b.int32(int32_t(props.size()));
        for (const auto& kv : props) { b.string(StringView(kv.first.c_str())); b.string(StringView(kv.second.c_str())); }
        b.end(d);
        b.int32(int32_t(id));
      });
    };
    NodeView ns;
    ns.id = 30;
    adapter(c, 30, {{"factory.name", "support.null-audio-sink"}, {"node.name", "my-sink"}, {"media.class", "Audio/Sink"}, {"audio.position", "FL,FR"}});
    ns.absorb(c);
    CHECK(ns.node && server.virtual_sinks() == 1);
    bool virtual_ok = false;
    g.visit(ns.node, [&](const GraphObject& o) { virtual_ok = o.role == NodeRole::Virtual && o.input_ports == 2 && o.output_ports == 2 && o.props.get("media.class") == StringView("Audio/Sink"); });
    CHECK(virtual_ok && g.find_node("my-sink") == ns.node);
    NodeView refused;
    refused.id = 31;
    adapter(c, 31, {{"factory.name", "api.alsa.pcm.sink"}});
    refused.absorb(c);
    CHECK(refused.errors == 1 && server.virtual_sinks() == 1);
    const uint32_t speakers_before = mixer.stream_count();
    ExportedNode into = export_node(c, 32, "Stream/Output/Audio", 1, nullptr, 0, 0, {{"target.object", "my-sink"}});
    ExportedNode from = export_node(c, 33, "Stream/Input/Audio", 0, nullptr, 0, 0, {{"target.object", "my-sink"}});
    CHECK(into.ok && from.ok && mixer.stream_count() == speakers_before);      // (not to the speakers: to the sink's own mixer)
    bool linked = false, recording = false;
    g.visit(into.node, [&](const GraphObject& o) { linked = o.role == NodeRole::Playback && o.target == ns.node && o.props.get("target.object") == StringView("my-sink"); });
    g.visit(from.node, [&](const GraphObject& o) { recording = o.role == NodeRole::Capture && o.target == ns.node && o.monitor; });
    CHECK(linked && recording);
    if (into.ok && from.ok) {
      put_u32(into.activation->memory(), 0, 3);
      put_u32(from.activation->memory(), 0, 3);
      bool heard = false;
      for (int round = 0; round < 60 && !heard; ++round) {
        tick(10);
        ByteView a = into.activation->memory();
        if (u32_at(a, 0) == 1) {      // its cycle: 1024 frames of 0.3 / -0.3
          const uint32_t id = round % 2;
          const uint64_t base = uint64_t(id) * into.buffer_stride;
          ByteView bufs = into.buffers->memory();
          for (uint32_t i = 0; i < 1024; ++i) {
            const float l = 0.3f, r = -0.3f;
            memcpy(bufs.data() + base + into.data_offset + 8 * i, &l, 4);
            memcpy(bufs.data() + base + into.data_offset + 8 * i + 4, &r, 4);
          }
          put_u32(bufs, base, 0); put_u32(bufs, base + 4, 8192);
          put_u32(into.io->memory(), 0, 2); put_u32(into.io->memory(), 4, id);
          put_u32(a, 0, 3);
        }
        ByteView fa = from.activation->memory();
        if (u32_at(fa, 0) == 1) {
          const uint32_t id = u32_at(from.io->memory(), 0);
          (void)id;
          const uint32_t bid = u32_at(from.io->memory(), 4);
          if (bid < from.n_buffers) {
            float l = 0, r = 0;
            const uint64_t base = uint64_t(bid) * from.buffer_stride + from.data_offset;
            memcpy(&l, from.buffers->memory().data() + base + 8 * 512, 4);
            memcpy(&r, from.buffers->memory().data() + base + 8 * 512 + 4, 4);
            heard = fabsf(l - 0.3f) < 0.001f && fabsf(r + 0.3f) < 0.001f;
          }
          put_u32(from.io->memory(), 0, 1);
          put_u32(fa, 0, 3);
        }
      }
      CHECK(heard);
    }
    // destroying the sink: its streams make and hear silence, nothing breaks
    c.send(0, 7, [](pod::Builder& b) { b.int32(30); });
    c.take_with_fds();
    CHECK(server.virtual_sinks() == 0 && g.find_node("my-sink") == 0);
    for (int i = 0; i < 5; ++i) tick(10);
    c.send(0, 7, [](pod::Builder& b) { b.int32(32); });
    c.send(0, 7, [](pod::Builder& b) { b.int32(33); });
    c.take_with_fds();
    // a lingering one stays when its client goes (the kernel's then), one that does not goes
    NativeClient l(server);
    l.send(0, 1, [](pod::Builder& b) { b.int32(4); });
    l.take_with_fds();
    adapter(l, 3, {{"factory.name", "support.null-audio-sink"}, {"node.name", "kept"}, {"object.linger", "true"}, {"audio.channels", "1"}});
    adapter(l, 4, {{"factory.name", "support.null-audio-sink"}, {"node.name", "gone"}});
    l.take_with_fds();
    CHECK(server.virtual_sinks() == 2);
    server.close_client(l.handle);
    const uint32_t kept = g.find_node("kept");
    uint32_t owner = 99;
    g.visit(kept, [&](const GraphObject& o) { owner = o.client; });
    CHECK(kept && owner == g.native_client_id() && g.find_node("gone") == 0 && server.virtual_sinks() == 1);
  }

  // ---- garbage: every truncation and bit flips of the new messages (port_update with metadata and info, port_buffers)
  std::mt19937 rng(41);
  for (int round = 0; round < 900; ++round) {
    ByteBuffer body(A);
    pod::Builder b(body);
    const uint64_t f = b.begin_struct();
    const uint32_t op = round % 3 == 0 ? 6u : 3u;
    if (op == 3) {
      b.int32(round % 2); b.int32(int32_t(rng() % 3)); b.int32(3); b.int32(3);
      if (round % 3 == 1) {
        static const uint32_t raw[] = {2, 4, 12, 17, 18, 20, 23, 25};
        if (round % 4 == 1) enum_video(b, 1 + rng() % 64, 1 + rng() % 64);
        else if (round % 4 == 3) enum_video_fixed(b, 0, rng() % 64, rng() % 64, 0x20001 + rng() % 16);
        else enum_video_fixed(b, raw[rng() % 8], 1 + rng() % 64, 1 + rng() % 64);
      } else {
        enum_control(b);
      }
      buffers_param(b, int32_t(rng() % 20), int32_t(rng() % 100000), int32_t(rng() % 4096), uint32_t(rng() % 32), int32_t(rng() % 6));
      meta_param(b, rng() % 10, int32_t(rng() % 200000));
      port_info(b, rng() % 8, {{"control.ump", "true"}});
    } else {
      b.int32(1); b.int32(0); b.int32(-1); b.int32(2);
      for (int k = 0; k < 2; ++k) {
        const int datas = 1 + int(rng() % 3);
        b.int32(datas);
        for (int d = 0; d < datas; ++d) { b.id(2); b.fd(k); b.int32(3); b.int32(int32_t(rng() % 8192)); b.int32(int32_t(rng() % 8192)); }
      }
    }
    b.end(f);
    std::vector<uint8_t> m(body.view().data(), body.view().data() + body.size());
    if (round % 2) m.resize(rng() % (m.size() + 1));
    else for (int k = 0; k < 3; ++k) m[rng() % m.size()] ^= uint8_t(1u << (rng() % 8));
    ByteBuffer msg(A);
    const uint32_t target = 5 + uint32_t(round % 2);
    msg.put32(target); msg.put32(op << 24 | uint32_t(m.size())); msg.put32(0); msg.put32(op == 6 ? 2u : 0u);
    msg.append(ConstByteView(m.data(), m.size()));
    List<Fd> fds(A);
    if (op == 6) { fds.push_back(Fd::make_as<FakeFd>(A, 4096, false)); fds.push_back(Fd::make_as<FakeFd>(A, 64, false)); }
    server.receive_with_fds(c.handle, msg.view(), fds);
    tick();
    c.take_with_fds();
  }
  server.close_client(c.handle);
  server.close_client(w.handle);
  CHECK(server.client_count() == 0);
  g.midi_bus().set_output(nullptr);
}

// ---- the transport takes descriptors from clients -----------------------------------------------------------------------------------------------------

// A protocol that keeps what it was given (bytes and descriptors).
struct CaptureProtocol final : IProtocolServer {
  std::vector<uint8_t> bytes;
  std::vector<Fd> fds;
  uint32_t open_client(uint64_t) override { return 1; }
  void receive(uint32_t, ConstByteView b) override { bytes.insert(bytes.end(), b.data(), b.data() + b.size()); }
  void receive_with_fds(uint32_t client, ConstByteView b, List<Fd>& in) override {
    for (Fd& f : in) fds.push_back(f);
    in.clear();
    receive(client, b);
  }
  bool take_output(uint32_t, ByteBuffer*) override { return false; }
  void close_client(uint32_t) override {}
  void tick() override {}
  uint64_t tick_interval_us() const override { return 10000; }
};

static void test_socket_fds_in() {
  SystemSync sync;
  process::UnixSocketManager sockets(sync, A);
  CaptureProtocol proto;
  const int live_before = A.live.load();
  {
    SocketServer server(sockets, A);
    CHECK(server.listen(proto, "/run/user/0/capture-test", StringView(), nullptr));
    uint32_t sock = 0;
    CHECK(sockets.create(process::UnixType::Stream, 9, &sock) == process::UnixStatus::Ok);
    sync.lock();
    CHECK(sockets.connect_locked(sock, process::UnixName::from(process::UnixName::Kind::Abstract, StringView("/run/user/0/capture-test").bytes()), StringView(), 9) ==
          process::UnixStatus::Ok);
    sync.unlock();
    server.pump();
    // a memfd the client wrote into, and an eventfd
    SharedPointer<process::OpenObject> object = SharedPointer<process::OpenObject>::make(A, A);
    UniquePointer<process::MemoryFile> file = UniquePointer<process::MemoryFile>::make(A, A, true);
    CHECK(file->resize(6000) == filesystem::Status::Ok);
    uint64_t written = 0;
    const char text[] = "client bytes";
    CHECK(file->write(5000, ConstByteView(reinterpret_cast<const uint8_t*>(text), sizeof text), &written) == filesystem::Status::Ok);
    object->memory = file.get();
    object->file = util::move(file);
    object->readable = object->writable = true;
    uint32_t event = 0;
    CHECK(sockets.create_event(0, 0, false, &event) == process::UnixStatus::Ok);
    Queue<process::UnixInFlight> rights(A);
    process::UnixInFlight mem;
    mem.kind = process::DescriptorKind::File;
    mem.object = object;
    rights.enqueue(util::move(mem));
    process::UnixInFlight ev;
    ev.kind = process::DescriptorKind::UnixSocket;
    ev.token = event;
    rights.enqueue(util::move(ev));
    sync.lock();
    uint32_t sent = 0;
    const uint8_t hello[4] = {1, 2, 3, 4};
    CHECK(sockets.send_locked(sock, ConstByteView(hello, 4), nullptr, 9, &rights, &sent) == process::UnixStatus::Ok && sent == 4);
    sync.unlock();
    for (int i = 0; i < 3; ++i) server.pump();
    CHECK(proto.bytes.size() == 4 && proto.fds.size() == 2);
    if (proto.fds.size() == 2) {
      ByteView m = proto.fds[0]->memory();
      CHECK(m.size() == 6000 && memcmp(m.data() + 5000, text, sizeof text) == 0);      // (the file's bytes, as big as the file)
      CHECK(proto.fds[1]->memory().empty() && proto.fds[1]->signal());
      sync.lock();
      uint64_t value = 0;
      CHECK(sockets.event_take_locked(event, &value) == process::UnixStatus::Ok && value == 1);      // (the same eventfd)
      sync.unlock();
    }
    proto.fds.clear();
    object = SharedPointer<process::OpenObject>();
    sockets.release(event);
    sockets.release(sock);
    for (int i = 0; i < 3; ++i) server.pump();
  }
  CHECK(A.live.load() <= live_before + 4);
}

int main() {
  test_pod();
  test_tagstruct();
  test_convert();
  test_graph();
  test_native_protocol();
  test_pulse_protocol();
  test_windows_service();
  test_windows_virtual_sink();
  test_camera_sources();
  test_screen_source();
  test_video_decode();
  test_video_watcher();
  test_record_convert();
  test_record();
  test_record_threads();
  test_monitor();
  test_native_capture();
  test_balance();
  test_client_node();
  test_sockets();
  test_socket_client_node();
  test_threads();
  test_midi_bus();
  test_midi_threads();
  test_midi_sysex_timing();
  test_video_channel();
  test_client_node_media();
  test_socket_fds_in();
  printf("PipeWireTest: %d checks, %d failed\n", g_checks.load(), g_failed.load());
  return g_failed ? 1 : 0;
}
