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
// conversion, the graph over a real AudioMixer and a fake device (native, Linux and Windows streams mixed into one output), the native protocol and the
// PulseAudio protocol driven as their clients drive them (and fed garbage), the Windows service, the AF_UNIX transport, and several threads at once.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include "PipeWire/SocketServer.hpp"
#include "PipeWire/Tagstruct.hpp"
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
  CHECK(count_type(g, ObjectType::Node) == 1 && count_type(g, ObjectType::Port) == 2 && count_type(g, ObjectType::Device) == 1 && count_type(g, ObjectType::Metadata) == 1);
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
  CHECK(count_type(g, ObjectType::Link) == 6 && count_type(g, ObjectType::Port) == 2 + 6);
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
  uint32_t id, opcode, seq;
  std::vector<uint8_t> payload;
};

struct NativeClient {
  NativeServer& server;
  uint32_t handle;
  uint32_t seq = 0;
  std::vector<uint8_t> in;
  NativeClient(NativeServer& s) : server(s), handle(s.open_client(77)) {}
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
    server.receive(handle, msg.view());
  }
  std::vector<NativeMessage> take() {
    ByteBuffer out(A);
    server.take_output(handle, &out);
    in.insert(in.end(), out.view().data(), out.view().data() + out.size());
    std::vector<NativeMessage> r;
    size_t at = 0;
    while (in.size() - at >= 16) {
      ByteReader h(ConstByteView(in.data() + at, 16));
      NativeMessage m;
      m.id = h.le32();
      const uint32_t w = h.le32();
      m.seq = h.le32();
      h.le32();
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
  dev.take();
  mixer.pump();
  std::vector<int16_t> out = dev.take();
  CHECK(out.size() > 202 && abs(out[200] - 2828) <= 6 && abs(out[201] - 2828) <= 6);      // mono to both sides, at the pan's equal-power centre (0.707)
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
  test_sockets();
  test_threads();
  printf("PipeWireTest: %d checks, %d failed\n", g_checks.load(), g_failed.load());
  return g_failed ? 1 : 0;
}
