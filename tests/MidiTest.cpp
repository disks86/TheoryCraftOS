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

// Host-side tests for MIDI: reading Standard MIDI Files (timing, tempo changes, running status, SMPTE, RIFF wrapper, damaged files), the
// software synthesizer (pitch, loudness, release, pedal, bend, drums, polyphony), the PCM source over a song (chunked reads, seeking, the
// hardware path), and the MPU-401 output against a fake interface.
#include <deque>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/MidiDevice/Mpu401.hpp"
#include "Media/AudioStreaming.hpp"
#include "Midi/MidiFile.hpp"
#include "Midi/MidiStream.hpp"
#include "Midi/SoftwareSynth.hpp"

using namespace midi;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  struct Header { uint64_t size; uint64_t pad; };
  void* allocate(uint64_t size) override {
    Header* h = static_cast<Header*>(malloc(sizeof(Header) + size));
    if (!h) return nullptr;
    h->size = size;
    ++live;
    return h + 1;
  }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override {
    if (!p) return;
    --live;
    free(static_cast<Header*>(p) - 1);
  }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// --- Building files ---------------------------------------------------------------------------------

typedef std::vector<uint8_t> Bytes;
static void put16(Bytes& b, uint32_t v) { b.push_back(static_cast<uint8_t>(v >> 8)); b.push_back(static_cast<uint8_t>(v)); }
static void put32(Bytes& b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }
static void varlen(Bytes& b, uint32_t v) {
  uint8_t tmp[5];
  int n = 0;
  tmp[n++] = v & 0x7F;
  while (v >>= 7) tmp[n++] = static_cast<uint8_t>((v & 0x7F) | 0x80);
  while (n) b.push_back(tmp[--n]);
}
struct Track {
  Bytes b;
  Track& at(uint32_t delta) { varlen(b, delta); return *this; }
  Track& raw(std::initializer_list<uint8_t> bytes) { b.insert(b.end(), bytes.begin(), bytes.end()); return *this; }
  Track& tempo(uint32_t us) { return raw({0xFF, 0x51, 3, static_cast<uint8_t>(us >> 16), static_cast<uint8_t>(us >> 8), static_cast<uint8_t>(us)}); }
  Track& end() { return at(0).raw({0xFF, 0x2F, 0}); }
};
static Bytes smf(uint16_t format, uint16_t division, std::initializer_list<Track> tracks) {
  Bytes f;
  const char head[] = "MThd";
  f.insert(f.end(), head, head + 4);
  put32(f, 6);
  put16(f, format);
  put16(f, static_cast<uint32_t>(tracks.size()));
  put16(f, division);
  for (const Track& t : tracks) {
    const char tag[] = "MTrk";
    f.insert(f.end(), tag, tag + 4);
    put32(f, static_cast<uint32_t>(t.b.size()));
    f.insert(f.end(), t.b.begin(), t.b.end());
  }
  return f;
}
static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

// --- Reading files ----------------------------------------------------------------------------------

static void test_parse() {
  TestAllocator a;
  {
    // Format 0, 480 ticks per quarter note at the default 120 bpm: a quarter note lasts 500 ms.
    Track t;
    t.at(0).raw({0xC0, 5}).at(0).raw({0x90, 60, 100}).at(480).raw({0x80, 60, 0}).end();
    const Bytes file = smf(0, 480, {t});
    Sequence s(a);
    CHECK(looks_like_midi(view(file)) && s.parse(view(file)));
    CHECK(s.format() == 0 && s.track_count() == 1 && s.events().count() == 3);
    CHECK(s.events()[0].status == 0xC0 && s.events()[0].data1 == 5 && s.events()[0].data2 == 0);
    CHECK(s.events()[1].status == 0x90 && s.events()[1].data1 == 60 && s.events()[1].data2 == 100 && s.events()[1].time_us == 0);
    CHECK(s.events()[2].status == 0x80 && s.events()[2].time_us == 500000 && s.duration_us() == 500000);
  }
  {
    // Format 1: the tempo track slows the song down; the notes of the other track are merged in time order.
    Track tempo, notes;
    tempo.at(0).tempo(500000).at(480).tempo(1000000).end();   // after one quarter note: 60 bpm
    notes.at(0).raw({0x90, 60, 90}).at(480).raw({0x80, 60, 0}).at(480).raw({0x90, 62, 90}).at(480).raw({0x80, 62, 0}).end();
    const Bytes file = smf(1, 480, {tempo, notes});
    Sequence s(a);
    CHECK(s.parse(view(file)) && s.format() == 1 && s.track_count() == 2 && s.events().count() == 4);
    CHECK(s.events()[1].time_us == 500000);              // tick 480: still at 120 bpm
    CHECK(s.events()[2].time_us == 1500000);            // tick 960: 500 ms + a 1 s quarter note
    CHECK(s.events()[3].time_us == 2500000);
  }
  {
    // Running status, a note on with velocity 0 for note off, system exclusive and other meta events are skipped.
    Track t;
    t.at(0).raw({0xFF, 0x03, 4, 'n', 'a', 'm', 'e'}).at(0).raw({0xF0, 3, 0x7E, 0x09, 0xF7});
    t.at(0).raw({0x93, 64, 80}).at(240).raw({64, 0}).at(240).raw({67, 70}).at(240).raw({0xE3, 0, 0x50}).end();
    const Bytes file = smf(0, 480, {t});
    Sequence s(a);
    CHECK(s.parse(view(file)) && s.events().count() == 4);
    CHECK(s.events()[0].status == 0x93 && s.events()[1].status == 0x93 && s.events()[1].data1 == 64 && s.events()[1].data2 == 0 && s.events()[1].time_us == 250000);
    CHECK(s.events()[2].status == 0x93 && s.events()[2].data1 == 67 && s.events()[3].status == 0xE3 && s.events()[3].data2 == 0x50);
  }
  {
    // SMPTE timing: 25 frames a second, 40 ticks a frame - 1000 ticks are one second (tempo events do not count).
    Track t;
    t.at(0).tempo(2000000).at(0).raw({0x90, 60, 100}).at(1000).raw({0x80, 60, 0}).end();
    const Bytes file = smf(0, static_cast<uint16_t>((0xE7 << 8) | 40), {t});
    Sequence s(a);
    CHECK(s.parse(view(file)) && s.events().count() == 2 && s.events()[1].time_us == 1000000);
  }
  {
    // Format 2: the songs follow each other.
    Track one, two;
    one.at(0).raw({0x90, 60, 100}).at(480).raw({0x80, 60, 0}).end();
    two.at(0).raw({0x90, 62, 100}).at(480).raw({0x80, 62, 0}).end();
    const Bytes file = smf(2, 480, {one, two});
    Sequence s(a);
    CHECK(s.parse(view(file)) && s.events().count() == 4 && s.events()[2].time_us == 500000 && s.events()[3].time_us == 1000000);
  }
  {
    // In a RIFF "RMID" wrapper.
    Track t;
    t.at(0).raw({0x90, 60, 100}).at(96).raw({0x80, 60, 0}).end();
    const Bytes inner = smf(0, 96, {t});
    Bytes riff = {'R', 'I', 'F', 'F'};
    const uint32_t body = 4 + 8 + static_cast<uint32_t>(inner.size());
    for (int i = 0; i < 4; ++i) riff.push_back(static_cast<uint8_t>(body >> (8 * i)));
    const char tag[] = "RMIDdata";
    riff.insert(riff.end(), tag, tag + 8);
    for (int i = 0; i < 4; ++i) riff.push_back(static_cast<uint8_t>(inner.size() >> (8 * i)));
    riff.insert(riff.end(), inner.begin(), inner.end());
    Sequence s(a);
    CHECK(looks_like_midi(view(riff)) && s.parse(view(riff)) && s.events().count() == 2 && s.events()[1].time_us == 500000);
  }
  {
    // Damaged files: nothing, garbage, a header without tracks, a track cut short (keeps what came before the damage).
    Sequence s(a);
    CHECK(!s.parse(ConstByteView()) && !looks_like_midi(ConstByteView()));
    const Bytes junk(100, 0x55);
    CHECK(!s.parse(view(junk)));
    CHECK(!s.parse(view(smf(0, 480, {})))) ;
    Track bad;
    bad.at(0).raw({0x90, 60, 100}).at(10).raw({0x80});   // the last message is cut off
    Bytes file = smf(0, 480, {bad});
    CHECK(s.parse(view(file)) && s.events().count() == 1);
    Track empty;
    empty.end();
    CHECK(!s.parse(view(smf(0, 480, {empty}))));
    Track zero_division;
    zero_division.at(0).raw({0x90, 60, 1}).end();
    CHECK(!s.parse(view(smf(0, 0, {zero_division}))));
    file = smf(0, 480, {bad});
    file.resize(file.size() / 2);   // the chunk claims more than the file holds
    s.parse(view(file));            // (must not read outside the data)
  }
  CHECK(a.live == 0);
}

// --- The synthesizer --------------------------------------------------------------------------------

static void note_on(IMidiSink& s, int ch, int note, int vel) { s.send(static_cast<uint8_t>(0x90 | ch), static_cast<uint8_t>(note), static_cast<uint8_t>(vel)); }
static void note_off(IMidiSink& s, int ch, int note) { s.send(static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(note), 0); }

struct Rendered {
  std::vector<int16_t> left, right;
  int peak() const {
    int p = 0;
    for (size_t i = 0; i < left.size(); ++i) {
      p = abs(left[i]) > p ? abs(left[i]) : p;
      p = abs(right[i]) > p ? abs(right[i]) : p;
    }
    return p;
  }
};
static Rendered render(SoftwareSynth& synth, uint32_t frames) {
  std::vector<int16_t> out(frames * 2);
  synth.render(out.data(), frames);
  Rendered r;
  for (uint32_t i = 0; i < frames; ++i) {
    r.left.push_back(out[i * 2]);
    r.right.push_back(out[i * 2 + 1]);
  }
  return r;
}
// The pitch of a render, from rising zero crossings between two times.
static double pitch_of(const Rendered& r, uint32_t from, uint32_t to) {
  int crossings = 0;
  for (uint32_t i = from + 1; i < to; ++i)
    if (r.left[i - 1] < 0 && r.left[i] >= 0) ++crossings;
  return crossings * 48000.0 / (to - from);
}

static void test_synth() {
  TestAllocator a;
  {
    SoftwareSynth synth(a);
    CHECK(synth.initialize());
    // Nothing is playing: silence.
    CHECK(render(synth, 480).peak() == 0 && synth.active_voices() == 0);

    // A4 on a flute: 440 Hz, whatever the loudness.
    synth.send(0xC0, 73, 0);
    note_on(synth, 0, 69, 100);
    Rendered r = render(synth, 24000);
    const double hz = pitch_of(r, 6000, 18000);
    CHECK(hz > 425 && hz < 455);
    CHECK(r.peak() > 1500 && r.peak() < 32000 && synth.active_voices() == 1);
    // The octave above is twice as high.
    note_off(synth, 0, 69);
    render(synth, 48000);
    CHECK(synth.active_voices() == 0);
    note_on(synth, 0, 81, 100);
    r = render(synth, 24000);
    const double hz2 = pitch_of(r, 6000, 18000);
    CHECK(hz2 > 1.9 * hz && hz2 < 2.1 * hz);
    synth.reset();

    // Velocity: harder is louder.
    synth.send(0xC0, 73, 0);
    note_on(synth, 0, 60, 30);
    const int soft = render(synth, 12000).peak();
    synth.reset();
    synth.send(0xC0, 73, 0);
    note_on(synth, 0, 60, 120);
    const int hard = render(synth, 12000).peak();
    CHECK(soft > 100 && hard > soft * 2);
    synth.reset();

    // The note dies away after note off (release), and the voice is freed.
    synth.send(0xC0, 73, 0);
    note_on(synth, 0, 64, 100);
    render(synth, 4800);
    note_off(synth, 0, 64);
    render(synth, 48000);
    CHECK(synth.active_voices() == 0 && render(synth, 1000).peak() == 0);

    // The sustain pedal holds a released note until the pedal is up.
    synth.send(0xB0, kControllerSustain, 127);
    note_on(synth, 0, 64, 100);
    render(synth, 4800);
    note_off(synth, 0, 64);
    render(synth, 24000);
    CHECK(synth.active_voices() == 1 && render(synth, 1000).peak() > 500);
    synth.send(0xB0, kControllerSustain, 0);
    render(synth, 48000);
    CHECK(synth.active_voices() == 0);

    // Volume 0 is silent; pan hard left leaves the right side silent.
    synth.send(0xB0, kControllerVolume, 0);
    note_on(synth, 0, 64, 100);
    CHECK(render(synth, 4800).peak() == 0);
    synth.reset();
    synth.send(0xC0, 73, 0);
    synth.send(0xB0, kControllerPan, 0);
    note_on(synth, 0, 64, 100);
    r = render(synth, 4800);
    int right_peak = 0;
    for (int16_t v : r.right) right_peak = abs(v) > right_peak ? abs(v) : right_peak;
    CHECK(right_peak <= 2 && r.peak() > 500);
    synth.reset();

    // Pitch bend: full up with the default range of two semitones raises the pitch by about 12.2 %.
    synth.send(0xC0, 73, 0);
    note_on(synth, 0, 69, 100);
    synth.send(0xE0, 0x7F, 0x7F);
    r = render(synth, 24000);
    const double bent = pitch_of(r, 6000, 18000);
    CHECK(bent > 440 * 1.09 && bent < 440 * 1.15);
    synth.reset();

    // Drums on channel 10 sound and then are gone by themselves, and a note off does not matter.
    note_on(synth, 9, 36, 110);
    CHECK(render(synth, 2400).peak() > 2000);
    note_off(synth, 9, 36);
    render(synth, 48000);
    CHECK(synth.active_voices() == 0);
    note_on(synth, 9, 38, 110);
    note_on(synth, 9, 42, 100);
    CHECK(render(synth, 2400).peak() > 1000);
    synth.reset();

    // All notes off releases, all sound off cuts.
    note_on(synth, 0, 60, 100);
    note_on(synth, 0, 64, 100);
    synth.send(0xB0, kControllerAllSoundOff, 0);
    CHECK(synth.active_voices() == 0);
    note_on(synth, 0, 60, 100);
    synth.send(0xB0, kControllerAllNotesOff, 0);
    render(synth, 96000);
    CHECK(synth.active_voices() == 0);

    // More notes than voices: the oldest give way, nothing breaks, and the sound stays inside the 16-bit range.
    synth.reset();
    synth.send(0xC0, 0, 0);
    for (int n = 0; n < 80; ++n) note_on(synth, n % 8, 30 + n, 127);
    r = render(synth, 4800);
    CHECK(synth.active_voices() <= SoftwareSynth::kMaxVoices && r.peak() <= 32767 && r.peak() > 1000);
    // Messages that are none of its business are ignored.
    synth.send(0x00, 1, 2);
    synth.send(0xF8, 0, 0);
    synth.send(0xA0, 60, 10);
    synth.send(0xD0, 10, 0);
  }
  CHECK(a.live == 0);
}

// --- Playing a song ---------------------------------------------------------------------------------

struct Recorder final : IMidiSink {
  struct Message { uint8_t status, d1, d2; uint64_t at_frame; };
  std::vector<Message> messages;
  uint64_t frame = 0;   // where the test is reading, to show when things were sent
  int resets = 0;
  void send(uint8_t s, uint8_t a, uint8_t b) override { messages.push_back({s, a, b, frame}); }
  void reset() override { ++resets; messages.push_back({0xFF, 0, 0, frame}); }
  int count(uint8_t status, uint8_t d1 = 0xFF) const {
    int n = 0;
    for (const Message& m : messages) n += (status == 0xFF ? m.status == status : (m.status & 0xF0) == status) && (d1 == 0xFF || m.d1 == d1);
    return n;
  }
};

static Bytes song() {
  // Instrument 5 on channel 0, a volume change; notes C4 at 0 s and E4 at 1 s, ending at 2 s (480 ticks per quarter, 120 bpm: 960 ticks a second).
  Track t;
  t.at(0).raw({0xC0, 73}).at(0).raw({0xB0, 7, 110}).at(0).raw({0x90, 60, 100}).at(960).raw({0x80, 60, 0}).at(0).raw({0x90, 64, 100}).at(960).raw({0x80, 64, 0}).end();
  return smf(0, 480, {t});
}

static void test_stream() {
  TestAllocator a;
  {
    const Bytes file = song();
    media::MemoryByteSource bytes(view(file));
    UniquePointer<audio::IPcmSource> src = open_midi_stream(bytes, a);
    CHECK(src);
    audio::StreamFormat f = src->format();
    CHECK(f.sample_rate == 48000 && f.channels == 2 && f.type == audio::SampleType::S16);
    CHECK(src->frame_count() >= 2 * 48000 + 48000 && src->frame_count() <= 2 * 48000 + 4 * 48000);

    // One big read and many small ones give the same sound.
    const uint32_t total = 3 * 48000;
    std::vector<int16_t> whole(total * 2), pieces(total * 2);
    CHECK(src->read(0, ByteView(reinterpret_cast<uint8_t*>(whole.data()), total * 4)) == total * 4);
    uint32_t at = 0;
    const uint32_t sizes[] = {1, 100, 1023, 4096, 777, 48000, 5};
    for (int i = 0; at < total; ++i) {
      uint32_t n = sizes[i % 7];
      if (n > total - at) n = total - at;
      CHECK(src->read(at, ByteView(reinterpret_cast<uint8_t*>(pieces.data() + at * 2), n * 4)) == n * 4);
      at += n;
    }
    CHECK(whole == pieces);
    int peak = 0;
    for (int16_t v : whole) peak = abs(v) > peak ? abs(v) : peak;
    CHECK(peak > 1500);
    // Silent when the song and its tail are over; nothing past the end.
    std::vector<int16_t> late(480 * 2);
    CHECK(src->read(src->frame_count() - 480, ByteView(reinterpret_cast<uint8_t*>(late.data()), 480 * 4)) == 480 * 4);
    int late_peak = 0;
    for (int16_t v : late) late_peak = abs(v) > late_peak ? abs(v) : late_peak;
    CHECK(late_peak == 0);
    CHECK(src->read(src->frame_count(), ByteView(reinterpret_cast<uint8_t*>(late.data()), 16)) == 0);

    // Seeking back and forth works, and the instrument set at the start is still in force after a seek (the second note is a flute too).
    std::vector<int16_t> second(48000 * 2), again(48000 * 2);
    CHECK(src->read(47000, ByteView(reinterpret_cast<uint8_t*>(second.data()), 48000 * 2)) == 48000 * 2);
    CHECK(src->read(47000, ByteView(reinterpret_cast<uint8_t*>(again.data()), 48000 * 2)) == 48000 * 2);
    CHECK(second == again);
    int seek_peak = 0;
    for (int16_t v : second) seek_peak = abs(v) > seek_peak ? abs(v) : seek_peak;
    CHECK(seek_peak > 1000);

    // Other files are refused.
    const Bytes junk(300, 7);
    media::MemoryByteSource not_midi(view(junk));
    CHECK(!open_midi_stream(not_midi, a));
    // The audio opener knows MIDI by what the file holds.
    media::AudioFileFormat detected = media::AudioFileFormat::Unknown;
    media::MemoryByteSource again_bytes(view(file));
    UniquePointer<audio::IPcmSource> through = media::open_audio_stream(again_bytes, a, &detected);
    CHECK(detected == media::AudioFileFormat::Midi && through && through->frame_count() == src->frame_count());
    CHECK(media::detect_audio_format(view(file)) == media::AudioFileFormat::Midi);
  }
  CHECK(a.live == 0);
}

static void test_hardware_stream() {
  TestAllocator a;
  {
    Recorder hw;
    const Bytes file = song();
    media::MemoryByteSource bytes(view(file));
    UniquePointer<audio::IPcmSource> src = open_midi_stream(bytes, a, &hw);
    CHECK(src && src->frame_count() >= 2 * 48000);
    // The source is silent; the messages go out as the player reads past their times.
    std::vector<int16_t> out(1000 * 2, 1);
    uint64_t frame = 0;
    bool silent = true;
    while (frame + 1000 <= src->frame_count()) {
      hw.frame = frame;
      CHECK(src->read(frame, ByteView(reinterpret_cast<uint8_t*>(out.data()), 1000 * 4)) == 1000 * 4);
      for (int16_t v : out) silent = silent && v == 0;
      frame += 1000;
    }
    CHECK(silent);
    CHECK(hw.count(0xC0, 73) == 1 && hw.count(0xB0, 7) == 1 && hw.count(0x90, 60) == 1 && hw.count(0x90, 64) == 1 && hw.count(0x80, 60) == 1);
    for (const Recorder::Message& m : hw.messages) {
      if (m.status == 0x90 && m.d1 == 64) CHECK(m.at_frame >= 47000 && m.at_frame <= 48000);   // the second note, at one second
      if (m.status == 0x90 && m.d1 == 60) CHECK(m.at_frame == 0);
    }
    // A pause silences the hardware (the player rewinds and re-reads: that is a seek, which sets the song up again but plays no notes).
    const size_t before = hw.messages.size();
    src->playback_paused();
    CHECK(hw.messages.size() == before + 32 && hw.count(0xB0, kControllerAllNotesOff) == 16);
    hw.messages.clear();
    CHECK(src->read(48000 + 10, ByteView(reinterpret_cast<uint8_t*>(out.data()), 1000 * 4)) == 1000 * 4);
    CHECK(hw.resets == 1 && hw.count(0xC0, 73) == 1 && hw.count(0xB0, 7) == 1 && hw.count(0x90) == 0);
    hw.messages.clear();
    src.reset();   // closing the song silences the hardware too
    CHECK(hw.count(0xB0, kControllerAllSoundOff) == 16 && hw.count(0xB0, kControllerAllNotesOff) == 16);
  }
  CHECK(a.live == 0);
}

// --- The MPU-401 ------------------------------------------------------------------------------------

struct FakeMpu final : hardware::IMidiPortIo {
  explicit FakeMpu(uint16_t base_port) : base(base_port) {}
  uint16_t base;
  bool present = true, answers_reset = true, busy_forever = false;
  std::vector<uint8_t> data, commands;
  std::deque<uint8_t> input;
  int reads = 0, writes = 0;
  uint8_t read8(uint16_t port) override {
    ++reads;
    if (!present) return 0xFF;
    if (port == base + 1) return static_cast<uint8_t>(0x3F | (input.empty() ? 0x80 : 0) | (busy_forever ? 0x40 : 0));
    if (port == base && !input.empty()) {
      const uint8_t v = input.front();
      input.pop_front();
      return v;
    }
    return 0xFF;
  }
  void write8(uint16_t port, uint8_t value) override {
    ++writes;
    if (!present) return;
    if (port == base + 1) {
      commands.push_back(value);
      if ((value == 0xFF && answers_reset) || value == 0x3F) input.push_back(0xFE);
    } else if (port == base) {
      data.push_back(value);
    }
  }
};

static void test_mpu401() {
  {
    // Nothing there: the address floats high, and nothing is written to it.
    FakeMpu none(0x330);
    none.present = false;
    hardware::Mpu401Output out(none, 0x330);
    CHECK(!out.initialize() && !out.present() && none.writes == 0);
    out.send(0x90, 60, 100);   // (ignored)
    CHECK(none.writes == 0);
  }
  {
    // An interface that never answers the reset is not used.
    FakeMpu mute(0x330);
    mute.answers_reset = false;
    hardware::Mpu401Output out(mute, 0x330);
    CHECK(!out.initialize() && !out.present());
  }
  {
    // One that never gets ready to take a byte does not hang the player either.
    FakeMpu busy(0x330);
    busy.busy_forever = true;
    hardware::Mpu401Output out(busy, 0x330);
    CHECK(!out.initialize());
  }
  {
    FakeMpu mpu(0x330);
    mpu.input.push_back(0x99);   // old input is thrown away
    hardware::Mpu401Output out(mpu, 0x330);
    CHECK(out.initialize() && out.present() && out.base() == 0x330);
    CHECK(mpu.commands.size() == 2 && mpu.commands[0] == 0xFF && mpu.commands[1] == 0x3F && mpu.data.empty());   // reset, then UART mode
    out.send(0x90, 60, 100);
    out.send(0xC1, 5, 0);       // program change: two bytes
    out.send(0xE2, 0, 0x40);
    out.send(0xF0, 1, 2);       // system messages are not sent
    out.send(0x10, 1, 2);
    const uint8_t expected[] = {0x90, 60, 100, 0xC1, 5, 0xE2, 0, 0x40};
    CHECK(mpu.data.size() == sizeof expected && memcmp(mpu.data.data(), expected, sizeof expected) == 0);
    mpu.data.clear();
    out.reset();
    int resets = 0, silences = 0;
    for (size_t i = 0; i + 2 < mpu.data.size();) {
      const uint8_t s = mpu.data[i];
      const size_t n = (s & 0xF0) == 0xC0 || (s & 0xF0) == 0xD0 ? 2 : 3;
      if ((s & 0xF0) == 0xB0 && mpu.data[i + 1] == 121) ++resets;
      if ((s & 0xF0) == 0xB0 && (mpu.data[i + 1] == 120 || mpu.data[i + 1] == 123)) ++silences;
      i += n;
    }
    CHECK(resets == 16 && silences == 32);
  }
}

int main() {
  test_parse();
  test_synth();
  test_stream();
  test_hardware_stream();
  test_mpu401();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("midi tests passed\n");
  return 0;
}
