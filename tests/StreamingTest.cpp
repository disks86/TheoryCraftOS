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

// Host-side tests for the streaming sound decoders: every sound file in tests/data is read through a byte source a piece at a time and
// must give the same sound as decoding the whole file at once - played from the start, and read at scattered places (seeking).
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Media/AudioStreaming.hpp"

using namespace media;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  uint64_t bytes_live = 0, peak = 0;
  struct Header { uint64_t size; uint64_t pad; };
  void* allocate(uint64_t size) override {
    Header* h = static_cast<Header*>(malloc(sizeof(Header) + size));
    if (!h) return nullptr;
    h->size = size;
    ++live;
    bytes_live += size;
    if (bytes_live > peak) peak = bytes_live;
    return h + 1;
  }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override {
    if (!p) return;
    Header* h = static_cast<Header*>(p) - 1;
    --live;
    bytes_live -= h->size;
    free(h);
  }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct File {
  uint8_t* data = nullptr;
  long size = 0;
  explicit File(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = static_cast<uint8_t*>(malloc(size ? size : 1));
    if (fread(data, 1, size, f) != static_cast<size_t>(size)) { free(data); data = nullptr; }
    fclose(f);
  }
  ~File() { free(data); }
  ConstByteView view() const { return ConstByteView(data, size); }
};

// A source that counts what is read from it, to show a stream does not ask for the whole file at once.
struct CountingSource final : IByteSource {
  explicit CountingSource(ConstByteView d) : inner(d) {}
  uint64_t size() const override { return inner.size(); }
  uint64_t read(uint64_t offset, ByteView out) override {
    ++reads;
    if (out.size() > biggest) biggest = out.size();
    return inner.read(offset, out);
  }
  MemoryByteSource inner;
  uint64_t reads = 0, biggest = 0;
};

// Reads a whole stream in pieces of `piece` frames and returns its samples (allocated, to be freed).
static int16_t* read_stream(audio::IPcmSource& s, uint32_t piece, uint64_t* frames) {
  const audio::StreamFormat f = s.format();
  const uint64_t total = s.frame_count();
  int16_t* all = static_cast<int16_t*>(calloc(total * f.channels + 1, 2));
  uint64_t at = 0;
  while (at < total) {
    const uint64_t want = total - at < piece ? total - at : piece;
    const uint32_t got = s.read(at, ByteView(reinterpret_cast<uint8_t*>(all + at * f.channels), want * f.frame_size()));
    if (got == 0) break;
    at += got / f.frame_size();
  }
  *frames = at;
  return all;
}

// How far two sounds may differ (largest sample difference) for the file to count as "the same sound".
// seek_rms_ratio: 0 means that reading at a place must give exactly the sound of decoding the file from the start; otherwise the difference
// (noise substitution in AAC depends on how many noise bands came before, so a decoder that joins in later makes different noise) may be at
// most this fraction of the sound.
static void check_data(const char* name, ConstByteView bytes, int tolerance = 0, double seek_rms_ratio = 0) {
  struct { ConstByteView v; ConstByteView view() const { return v; } long size; const uint8_t* data; } file{bytes, (long)bytes.size(), bytes.data()};
  TestAllocator a;
  {
    AudioData whole(a);
    AudioFileFormat format = AudioFileFormat::Unknown;
    const bool full_ok = decode_audio(file.view(), &whole, &format);
    CountingSource counting(file.view());
    TestAllocator sa;   // what the stream itself uses
    CachedByteSource cached(counting, sa);
    AudioFileFormat detected = AudioFileFormat::Unknown;
    UniquePointer<audio::IPcmSource> stream = open_audio_stream(cached, sa, &detected);
    if (!full_ok) {   // not decodable as a whole: the stream must refuse it too
      CHECK(!stream);
      return;
    }
    if (!stream) { printf("FAILED %s: no stream\n", name); ++g_failed; return; }
    CHECK(detected == format);
    CHECK(stream->format().sample_rate == whole.format().sample_rate && stream->format().channels == whole.format().channels);
    const uint64_t n = whole.frame_count(), ch = whole.format().channels;
    if (stream->frame_count() != n) { printf("FAILED %s: %llu frames, want %llu\n", name, (unsigned long long)stream->frame_count(), (unsigned long long)n); ++g_failed; }
    // From the start to the end, in pieces of different sizes.
    const uint32_t pieces[3] = {1000, 4096 / 4, 33};
    for (uint32_t piece : pieces) {
      uint64_t got = 0;
      int16_t* samples = read_stream(*stream, piece, &got);
      int worst = 0;
      uint64_t bad = 0;
      for (uint64_t i = 0; i < got * ch && i < n * ch; ++i) {
        const int d = abs(int(samples[i]) - int(whole.samples()[i]));
        if (d > worst) worst = d;
        if (d > tolerance) ++bad;
      }
      if (got != n || bad) { printf("FAILED %s: piece %u: %llu of %llu frames read, %llu samples off (worst %d)\n", name, piece, (unsigned long long)got, (unsigned long long)n, (unsigned long long)bad, worst); ++g_failed; }
      free(samples);
    }
    // Scattered places, forwards and backwards.
    uint32_t seed = 12345;
    for (int k = 0; k < 12; ++k) {
      seed = seed * 1664525u + 1013904223u;
      const uint64_t at = (seed >> 8) % n;
      int16_t buf[2 * 600];
      const uint32_t got = stream->read(at, ByteView(reinterpret_cast<uint8_t*>(buf), sizeof buf));
      const uint64_t frames = got / stream->format().frame_size();
      if (frames == 0) { printf("FAILED %s: nothing at frame %llu\n", name, (unsigned long long)at); ++g_failed; continue; }
      uint64_t bad = 0, first_bad = 0;
      int worst = 0;
      double diff_energy = 0, signal_energy = 0;
      for (uint64_t i = 0; i < frames * ch; ++i) {
        const int d = abs(int(buf[i]) - int(whole.samples()[at * ch + i]));
        if (d > tolerance) { if (!bad) first_bad = i / ch; ++bad; }
        if (d > worst) worst = d;
        diff_energy += double(d) * d;
        signal_energy += double(buf[i]) * buf[i];
      }
      if (seek_rms_ratio > 0) bad = diff_energy > seek_rms_ratio * seek_rms_ratio * signal_energy ? bad + 1 : 0;
      if (bad) { printf("FAILED %s: %llu samples off (worst %d, first at +%llu) after seeking to frame %llu\n", name, (unsigned long long)bad, worst, (unsigned long long)first_bad, (unsigned long long)at); ++g_failed; }
    }
    CHECK(stream->read(n, ByteView(reinterpret_cast<uint8_t*>(&seed), 4)) == 0);   // past the end
    // Nothing needs the whole file or the whole sound at once: reads from the file stay small, and memory use is a window.
    if (file.size > 20000) CHECK(counting.biggest <= 524288);
    if (n > 200000) CHECK(sa.peak < 2u << 20 && sa.peak * 4 < n * ch * 2);   // a window, not the whole sound
    stream.reset();
    CHECK(sa.live == 1 || sa.live == 0);
  }
  CHECK(a.live == 0);
}

static void check_stream(const char* name, int tolerance = 0, double seek_rms_ratio = 0) {
  char path[256];
  snprintf(path, sizeof path, "tests/data/%s", name);
  File file(path);
  if (!file.data) { printf("FAILED cannot read %s\n", path); ++g_failed; return; }
  check_data(name, file.view(), tolerance, seek_rms_ratio);
}

// An Ogg Vorbis file repeated `copies` times as one long stream (the fixtures have one page of sound): each copy's pages follow the last one's
// with their positions moved on - `first` is how many samples the first copy gives, `later` how many each next one does (its first packet
// joins the end of the one before) - and only the very last page ends the stream. The header packets that come again are not sound,
// so decoders pass over them.
static void make_long_ogg(ConstByteView one, int copies, uint64_t first, uint64_t later, ByteBuffer* out) {
  auto le32 = [&](const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); };
  for (int c = 0; c < copies; ++c) {
    const uint64_t before = c ? first + uint64_t(c - 1) * later : 0, after = first + uint64_t(c) * later;
    for (uint64_t at = 0; at + 27 <= one.size();) {
      const uint8_t* h = one.data() + at;
      uint64_t total = 0;
      for (uint32_t i = 0; i < h[26]; ++i) total += h[27 + i];
      const uint64_t size = 27 + h[26] + total;
      const uint64_t start = out->size();
      out->append(one.subview(at, size));
      uint8_t* page = const_cast<uint8_t*>(out->view().data()) + start;
      uint64_t g = le32(page + 6) | (uint64_t(le32(page + 10)) << 32);
      if (g != 0xFFFFFFFFFFFFFFFFull) {
        g = g > 0 ? after : before;
        for (int k = 0; k < 8; ++k) page[6 + k] = uint8_t(g >> (8 * k));
      }
      if (c + 1 < copies) page[5] &= ~4;   // not the end yet
      at += size;
    }
  }
}

// A longer sound than the fixtures hold, made here: a few tones and some noise, stereo, so that big files (and the places in them) are tested.
static void make_sound(uint64_t frames, uint32_t rate, AudioData* out) {
  audio::StreamFormat f;
  f.sample_rate = rate;
  f.channels = 2;
  out->create(f, frames);
  uint32_t seed = 7;
  for (uint64_t i = 0; i < frames; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const double t = double(i) / rate;
    const double v = 9000 * __builtin_sin(6.283185 * 440 * t) + 5000 * __builtin_sin(6.283185 * (880 + 300 * __builtin_sin(t)) * t) + double(int(seed >> 20) - 2048);
    out->samples()[i * 2] = int16_t(v);
    out->samples()[i * 2 + 1] = int16_t(0.6 * v + 3000 * __builtin_sin(6.283185 * 220 * t));
  }
}

int main() {
  const char* exact[] = {"pcm8.wav", "pcm16.wav", "pcm16_mono.wav", "pcm24.wav", "pcm32.wav", "f32.wav", "f64.wav", "alaw.wav", "mulaw.wav", "ima.wav",
                         "ima_mono.wav", "adpcm_ms.wav", "adpcm_ms_mono.wav", "s16.aiff", "s24.aiff", "s16.au", "mulaw.au", "f32.au", "sowt.aifc", "u8.aifc", "ulaw.aifc"};
  for (const char* name : exact) check_stream(name);
  const char* lossless[] = {"best.flac", "fast.flac", "s32.flac"};
  for (const char* name : lossless) check_stream(name);
  check_stream("stereo.aac", 0, 0.2);
  for (const char* name : {"mono.ogg", "stereo.ogg"}) check_stream(name);
  for (const char* name : {"opus_silk_nb_mono.opus", "opus_silk_wb_stereo_60.opus", "opus_hybrid_stereo.opus", "opus_celt_mono.opus", "opus_celt_stereo_10.opus", "opus.ogg"}) check_stream(name, 0, 0.2);
  check_stream("mono.m4a", 0, 0.2);
  check_stream("stereo.mov", 0, 0.2);
  check_stream("mp3.mp4");
  const char* mp3[] = {"mono.mp3", "stereo.mp3", "layer1.mp1", "lsf.mp2"};
  for (const char* name : mp3) check_stream(name, 0, 0);
  {   // big files, made here
    TestAllocator a;
    {
      AudioData sound(a);
      make_sound(44100 * 25, 44100, &sound);
      ByteBuffer flac(a), wav(a);
      CHECK(encode_flac(sound, &flac));
      CHECK(encode_wav(sound, &wav));
      check_data("synthetic.flac", flac.view());
      check_data("synthetic.wav", wav.view());
      {
        File aac("tests/data/stereo.aac");
        ByteBuffer many(a);
        for (int i = 0; i < 300 && aac.data; ++i) many.append(aac.view());
        check_data("long stereo.aac", many.view(), 0, 0.2);
      }
      for (const char* base : {"stereo.ogg", "mono.ogg"}) {
        char path[256];
        snprintf(path, sizeof path, "tests/data/%s", base);
        File one(path);
        ByteBuffer many(a);
        if (one.data) {
          // What the file really gives (its last page trims the end), and what each copy adds once they are joined: decoded with the end left open.
          ByteBuffer one_open(a), two_open(a);
          make_long_ogg(one.view(), 1, 0x7FFFFFFFull, 0, &one_open);
          make_long_ogg(one.view(), 2, 0x7FFFFFFFull, 0x7FFFFFFFull, &two_open);
          AudioData raw1(a), raw2(a);
          CHECK(decode_audio(one_open.view(), &raw1) && decode_audio(two_open.view(), &raw2));
          make_long_ogg(one.view(), 400, raw1.frame_count(), raw2.frame_count() - raw1.frame_count(), &many);
        }
        char label[64];
        snprintf(label, sizeof label, "long %s", base);
        check_data(label, many.view());
      }
      // A long MP3: a fixture repeated (the frames follow each other like any others).
      for (const char* base : {"stereo.mp3", "mono.mp3", "lsf.mp2"}) {
        char path[256];
        snprintf(path, sizeof path, "tests/data/%s", base);
        File one(path);
        ByteBuffer many(a);
        for (int i = 0; i < 700 && one.data; ++i) many.append(one.view());
        char label[64];
        snprintf(label, sizeof label, "long %s", base);
        check_data(label, many.view());
      }
    }
    CHECK(a.live == 0);
  }
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("streaming tests passed\n");
  return 0;
}
