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
#include "PipeWire/NativeServer.hpp"
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
  CHECK(count_type(g, ObjectType::Node) == 1 && count_type(g, ObjectType::Port) == 4 && count_type(g, ObjectType::Device) == 1 && count_type(g, ObjectType::Metadata) == 1);      // (the sink: two playback ports, two monitor ports)
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
  CHECK(count_type(g, ObjectType::Link) == 6 && count_type(g, ObjectType::Port) == 4 + 6);
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
static ExportedNode export_node(NativeClient& c, uint32_t id, const char* media_class, uint32_t direction, const char* latency = nullptr, uint32_t rate = 0,
                                uint32_t channels = 0) {
  ExportedNode e;
  e.id = id;
  c.send(0, 6, [&](pod::Builder& b) {      // create_object
    b.string("client-node"); b.string("PipeWire:Interface:ClientNode"); b.int32(6);
    const uint64_t d = b.begin_struct();
    b.int32(2); b.string("media.class"); b.string(media_class); b.string("application.name"); b.string("pw-test");
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

int main() {
  test_pod();
  test_tagstruct();
  test_convert();
  test_graph();
  test_native_protocol();
  test_pulse_protocol();
  test_windows_service();
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
  printf("PipeWireTest: %d checks, %d failed\n", g_checks.load(), g_failed.load());
  return g_failed ? 1 : 0;
}
