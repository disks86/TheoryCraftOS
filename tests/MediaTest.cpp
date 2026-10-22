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

// Host-side tests for the media codecs: every decoder against fixtures whose expected output came from independent
// decoders (tests/data/make_fixtures.py), encoder round trips, damaged and random input, the acceleration-first
// codec front door with its CPU fallback, and the video player reading GIF/Y4M/compressed-audio files.
#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Media/Audio/AacInternal.hpp"
#include "Media/Mp4.hpp"
#include "Media/MediaCodecs.hpp"
#include "Media/Zlib.hpp"
#include "Video/AviWriter.hpp"
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

static uint64_t fnv(uint64_t h, const uint8_t* p, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}
static const uint64_t kFnvBasis = 0xcbf29ce484222325ull;

// The same hash make_fixtures.py computes: RGBA bytes with the colour zeroed where the pixel is transparent.
static uint64_t image_hash(const Image& image) {
  uint64_t h = kFnvBasis;
  for (uint32_t f = 0; f < image.frame_count(); ++f)
    for (uint32_t p : image.frame(f)) {
      const uint8_t px[4] = {static_cast<uint8_t>(p >> 16), static_cast<uint8_t>(p >> 8), static_cast<uint8_t>(p), static_cast<uint8_t>(p >> 24)};
      const uint8_t z[4] = {0, 0, 0, 0};
      h = fnv(h, px[3] ? px : z, 4);
    }
  return h;
}
static uint64_t audio_hash(const AudioData& a) { return fnv(kFnvBasis, a.bytes().data(), a.bytes().size()); }

static void test_fixtures() {
  TestAllocator a;
  FILE* m = fopen("tests/data/manifest.txt", "r");
  CHECK(m != nullptr);
  if (!m) return;
  char kind[16], name[96], reference[96] = "";
  unsigned x, y, z;
  unsigned long long hash = 0;
  int count = 0;
  while (fscanf(m, "%15s %95s %u %u %u", kind, name, &x, &y, &z) == 5) {
    if (!strcmp(kind, "audiotol")) {
      if (fscanf(m, "%95s", reference) != 1) break;
    } else if (fscanf(m, "%llx", &hash) != 1) {
      break;
    }
    char path[160];
    snprintf(path, sizeof path, "tests/data/%s", name);
    File file(path);
    if (!file.data) { printf("missing fixture %s\n", path); ++g_failed; continue; }
    ++count;
    bool ok = false;
    if (!strcmp(kind, "image")) {
      Image img(a);
      ok = decode_image(file.view(), &img) && img.width() == x && img.height() == y && img.frame_count() == z && image_hash(img) == hash;
    } else if (!strcmp(kind, "audiotol")) {
      // Lossy audio: the samples must be within a couple of steps of the reference decoder's, in the same count.
      char ref_path[160];
      snprintf(ref_path, sizeof ref_path, "tests/data/%s", reference);
      File ref(ref_path);
      AudioData au(a);
      // (ffmpeg leaves out a last MPEG audio frame it cannot confirm by a following header, and does not cut MP4 audio at its edit list's
      // end: the lengths may differ by a frame or two. The samples both have must agree.)
      ok = ref.data && decode_audio(file.view(), &au) && au.format().sample_rate == x && au.format().channels == y && au.frame_count() + 2304 >= z && au.frame_count() <= z + 2304;
      const uint64_t common = ok ? (au.frame_count() < z ? au.frame_count() : z) * y : 0;
      for (uint64_t i = 0; ok && i < common; ++i) {
        const int d = au.samples()[i] - static_cast<int16_t>(ref.data[i * 2] | ref.data[i * 2 + 1] << 8);
        ok = d >= -2 && d <= 2;
      }
    } else {
      AudioData au(a);
      ok = decode_audio(file.view(), &au) && au.format().sample_rate == x && au.format().channels == y && au.frame_count() == z && audio_hash(au) == hash;
    }
    if (!ok) { printf("FAILED fixture %s\n", name); ++g_failed; }
  }
  fclose(m);
  CHECK(count >= 79);
  {  // Ogg Opus shares the container with Vorbis but is a different codec: detected as Opus, decoded at 48 kHz.
    File opus("tests/data/opus.ogg");
    AudioData au(a);
    AudioFileFormat f = AudioFileFormat::Vorbis;
    CHECK(opus.data && decode_audio(opus.view(), &au, &f) && f == AudioFileFormat::Opus && au.format().sample_rate == 48000 && au.format().channels == 2);
  }
  CHECK(a.live == 0);
}

// A test picture with smooth gradients, a hard-edged disc and some transparency.
static void make_picture(Image* img, uint32_t w, uint32_t h, uint32_t shift, bool alpha) {
  CHECK(img->create(w, h, 1));
  ArrayView<uint32_t> px = img->frame(0);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t r = (x * 255 / (w - 1) + shift * 40) & 255, g = y * 255 / (h - 1), b = ((x + y) * 3) & 255;
      const int dx = static_cast<int>(x) - static_cast<int>(w / 2), dy = static_cast<int>(y) - static_cast<int>(h / 2);
      if (dx * dx + dy * dy < static_cast<int>(w * h / 30)) { r = 240; g = 230; b = 20; }
      const uint32_t av = alpha ? (x < w / 4 ? 0 : 255 - (y * 150 / h)) : 255;
      px[y * w + x] = av << 24 | r << 16 | g << 8 | b;
    }
  img->set_has_alpha(alpha);
}

static bool same_pixels(const Image& a, const Image& b, bool compare_color_of_transparent = false) {
  if (a.width() != b.width() || a.height() != b.height() || a.frame_count() != b.frame_count()) return false;
  for (uint32_t f = 0; f < a.frame_count(); ++f)
    for (uint64_t i = 0; i < a.frame(f).size(); ++i) {
      const uint32_t p = a.frame(f)[i], q = b.frame(f)[i];
      if (p != q && (compare_color_of_transparent || (p >> 24) != 0 || (q >> 24) != 0)) return false;
    }
  return true;
}

static double psnr(const Image& a, const Image& b) {
  double se = 0;
  uint64_t n = 0;
  for (uint64_t i = 0; i < a.frame(0).size(); ++i)
    for (int s = 0; s <= 16; s += 8) {
      const double d = static_cast<int>((a.frame(0)[i] >> s) & 255) - static_cast<int>((b.frame(0)[i] >> s) & 255);
      se += d * d;
      ++n;
    }
  if (se == 0) return 99;
  return 10 * log10(255.0 * 255.0 / (se / n));
}

static void test_image_roundtrip() {
  TestAllocator a;
  Image src(a), dst(a);
  make_picture(&src, 45, 33, 0, true);
  Image opaque(a);
  make_picture(&opaque, 45, 33, 1, false);
  struct { ImageFormat f; bool exact; bool alpha; } cases[] = {{ImageFormat::Png, true, true}, {ImageFormat::Bmp, true, true}, {ImageFormat::Qoi, true, true},
                                                             {ImageFormat::Tga, true, true}, {ImageFormat::Pnm, true, false}, {ImageFormat::Tiff, true, true}, {ImageFormat::Webp, true, true}};
  for (const auto& c : cases) {
    const Image& in = c.alpha ? src : opaque;
    ByteBuffer out(a);
    ImageFormat detected = ImageFormat::Unknown;
    CHECK(encode_image(in, c.f, 80, &out));
    CHECK(decode_image(out.view(), &dst, &detected) && detected == c.f && same_pixels(in, dst));
  }
  // Lossy: JPEG at high quality is close; GIF is a 216-colour dither, so only roughly close.
  ByteBuffer jpg(a), gif(a);
  CHECK(encode_image(opaque, ImageFormat::Jpeg, 92, &jpg) && decode_image(jpg.view(), &dst) && dst.width() == 45 && dst.height() == 33);
  CHECK(psnr(opaque, dst) > 24);  // 4:2:0 chroma smears the saturated disc edge, so a modest bar
  CHECK(encode_image(opaque, ImageFormat::Gif, 80, &gif) && decode_image(gif.view(), &dst) && dst.width() == 45 && psnr(opaque, dst) > 15);
  // Animation through PNG (APNG) and GIF keeps the frames and delays.
  Image anim(a);
  CHECK(anim.create(24, 16, 3));
  for (uint32_t f = 0; f < 3; ++f) {
    for (uint64_t i = 0; i < anim.frame(f).size(); ++i) anim.frame(f)[i] = 0xFF000000u | ((f * 90 + i * 3) & 255) << 16 | (i & 255) << 8 | 77;
    anim.set_delay_ms(f, 40 + 30 * f);
  }
  anim.set_loop_count(2);
  ByteBuffer apng(a);
  CHECK(encode_image(anim, ImageFormat::Png, 60, &apng));
  CHECK(detect_video_file(apng.view()) == VideoFileFormat::Apng);
  CHECK(decode_image(apng.view(), &dst) && dst.frame_count() == 3 && same_pixels(anim, dst) && dst.delay_ms(2) == 100 && dst.loop_count() == 2);
  ByteBuffer agif(a);
  CHECK(encode_image(anim, ImageFormat::Gif, 60, &agif) && detect_video_file(agif.view()) == VideoFileFormat::Gif);
  CHECK(decode_image(agif.view(), &dst) && dst.frame_count() == 3 && dst.delay_ms(1) == 70 && dst.loop_count() == 2);
}

static void test_zlib() {
  TestAllocator a;
  uint8_t data[20000];
  uint32_t seed = 12345;
  for (uint32_t i = 0; i < sizeof data; ++i) {
    seed = seed * 1103515245 + 12345;
    data[i] = (i % 400 < 250) ? static_cast<uint8_t>(i / 7) : static_cast<uint8_t>(seed >> 16);
  }
  for (uint32_t level : {0u, 1u, 6u, 9u}) {
    ByteBuffer comp(a), back(a);
    CHECK(deflate(ConstByteView(data, sizeof data), &comp, level, a));
    CHECK(inflate(comp.view(), &back, 1u << 20));
    CHECK(back.size() == sizeof data && memcmp(back.view().data(), data, sizeof data) == 0);
    if (level >= 1) CHECK(comp.size() < sizeof data);
  }
  ByteBuffer bomb(a);
  uint8_t zeros[100000] = {0};
  ByteBuffer comp(a);
  CHECK(deflate(ConstByteView(zeros, sizeof zeros), &comp, 6, a));
  CHECK(!inflate(comp.view(), &bomb, 1000));  // the output limit holds
}

static void make_tone(AudioData* au, uint32_t rate, uint32_t channels, uint32_t frames) {
  audio::StreamFormat f;
  f.sample_rate = rate;
  f.channels = static_cast<uint8_t>(channels);
  CHECK(au->create(f, frames));
  uint32_t seed = 99;
  for (uint32_t i = 0; i < frames; ++i)
    for (uint32_t c = 0; c < channels; ++c) {
      seed = seed * 1664525 + 1013904223;
      au->samples()[i * channels + c] = static_cast<int16_t>(((i * (7 + 3 * c)) % 20000) - 10000 + static_cast<int>((seed >> 20) & 255) - 128);
    }
}

static void test_audio_roundtrip() {
  TestAllocator a;
  for (uint32_t channels : {1u, 2u})
    for (AudioFileFormat f : {AudioFileFormat::Wav, AudioFileFormat::Aiff, AudioFileFormat::Au, AudioFileFormat::Flac}) {
      AudioData src(a), dst(a);
      make_tone(&src, 22050, channels, 9001);
      ByteBuffer out(a);
      AudioFileFormat detected = AudioFileFormat::Unknown;
      CHECK(encode_audio(src, f, &out));
      CHECK(decode_audio(out.view(), &dst, &detected) && detected == f);
      CHECK(dst.frame_count() == src.frame_count() && dst.format().channels == channels && dst.format().sample_rate == 22050);
      CHECK(dst.frame_count() == src.frame_count() && memcmp(dst.bytes().data(), src.bytes().data(), src.bytes().size()) == 0);
    }
  // FLAC has to cope with silence, a single sample and full-scale noise.
  AudioData silent(a), one(a), loud(a), back(a);
  make_tone(&silent, 8000, 2, 5000);
  for (int16_t& s : silent.samples()) s = 0;
  make_tone(&one, 8000, 1, 1);
  make_tone(&loud, 8000, 2, 6000);
  uint32_t seed = 5;
  for (int16_t& s : loud.samples()) { seed = seed * 1664525 + 1013904223; s = static_cast<int16_t>(seed >> 16); }
  for (AudioData* s : {&silent, &one, &loud}) {
    ByteBuffer out(a);
    CHECK(encode_flac(*s, &out) && decode_flac(out.view(), &back) && back.frame_count() == s->frame_count() && memcmp(back.bytes().data(), s->bytes().data(), s->bytes().size()) == 0);
  }
  CHECK(decode_mulaw(0xFF) == 0 && decode_alaw(0xD5) == 8 && decode_mulaw(0x00) == -32124);
}

// Music-like test sound: a few tones with vibrato and an envelope, some noise, and a left/right balance that makes the pair correlated.
static void make_music(AudioData* au, uint32_t rate, uint32_t channels, uint32_t frames) {
  audio::StreamFormat f;
  f.sample_rate = rate;
  f.channels = static_cast<uint8_t>(channels);
  CHECK(au->create(f, frames));
  uint32_t seed = 7;
  for (uint32_t i = 0; i < frames; ++i) {
    const double t = (double)i / rate;
    const double env = 0.6 + 0.4 * sin(2 * 3.14159265 * 2.0 * t);
    seed = seed * 1664525 + 1013904223;
    const double noise = ((seed >> 16) & 0xFF) / 255.0 - 0.5;
    const double a = sin(2 * 3.14159265 * 440 * t + 3 * sin(2 * 3.14159265 * 5 * t)), b = sin(2 * 3.14159265 * 1250 * t), c = sin(2 * 3.14159265 * (rate / 8.0) * t);
    for (uint32_t ch = 0; ch < channels; ++ch) {
      const double v = env * (6000 * a + (ch ? 1500 : 3000) * b + 800 * c) + 400 * noise;
      au->samples()[i * channels + ch] = (int16_t)v;
    }
  }
}

// The decoded sound lags the original by the transform delay; the signal-to-noise ratio is over the part where both exist.
static double aac_snr(const AudioData& src, const AudioData& dec) {
  const uint64_t delay = AacEncoder::delay_frames();
  const uint32_t ch = src.format().channels;
  double sig = 0, noise = 0;
  for (uint64_t i = 2048; i + delay + 2048 < src.frame_count() && i + delay < dec.frame_count(); ++i)
    for (uint32_t c = 0; c < ch; ++c) {
      const double x = src.samples()[i * ch + c], y = dec.samples()[(i + delay) * ch + c];
      sig += x * x;
      noise += (x - y) * (x - y);
    }
  return noise == 0 ? 99 : 10 * log10(sig / noise);
}

static void test_aac_encode() {
  TestAllocator a;
  struct Case { uint32_t rate, channels, bitrate; double min_snr; };
  const Case cases[] = {{44100, 2, 128000, 26}, {44100, 1, 64000, 19}, {48000, 2, 96000, 21}, {22050, 2, 48000, 22}, {8000, 1, 24000, 26}, {96000, 2, 256000, 25}, {32000, 1, 48000, 20}};
  for (const Case& c : cases) {
    AudioData src(a), dec(a);
    make_music(&src, c.rate, c.channels, c.rate * 3);
    ByteBuffer out(a);
    CHECK(encode_aac(src, c.bitrate, &out));
    CHECK(looks_like_adts(out.view()));
    if (const char* dir = getenv("AAC_DUMP")) {  // for checking the streams with other decoders
      char path[256];
      snprintf(path, sizeof path, "%s/aac_%u_%u.aac", dir, c.rate, c.channels);
      if (FILE* f = fopen(path, "wb")) { fwrite(out.view().data(), 1, out.size(), f); fclose(f); }
      snprintf(path, sizeof path, "%s/aac_%u_%u.raw", dir, c.rate, c.channels);
      if (FILE* f = fopen(path, "wb")) { fwrite(src.bytes().data(), 1, src.bytes().size(), f); fclose(f); }
    }
    AudioFileFormat detected = AudioFileFormat::Unknown;
    CHECK(decode_audio(out.view(), &dec, &detected) && detected == AudioFileFormat::Aac);
    CHECK(dec.format().channels == c.channels && dec.format().sample_rate == c.rate);
    CHECK(dec.frame_count() == ((src.frame_count() + 1023) / 1024 + 1) * 1024);
    const double snr = aac_snr(src, dec);
    const double bps = (double)out.size() * 8 * c.rate / (double)dec.frame_count();  // over the blocks, the priming block included
    printf("  aac %u Hz x%u at %u: SNR %.1f dB, %.0f bit/s\n", c.rate, c.channels, c.bitrate, snr, bps);
    CHECK(snr >= c.min_snr);
    CHECK(bps < c.bitrate * 1.06 && bps > c.bitrate * 0.9);  // the average holds the target
  }
  {  // the default rate through encode_audio(), and the resulting stream detected and played back
    AudioData src(a), dec(a);
    make_music(&src, 44100, 2, 44100);
    ByteBuffer out(a);
    CHECK(encode_audio(src, AudioFileFormat::Aac, &out) && decode_audio(out.view(), &dec));
    CHECK(aac_snr(src, dec) > 24);
    CHECK(kEncodableAudioFormats & audio_format_bit(AudioFileFormat::Aac));
  }
  {  // silence, a very short sound and full-scale noise are all valid input
    AudioData silent(a), one(a), loud(a), dec(a);
    make_music(&silent, 44100, 2, 5000);
    for (int16_t& v : silent.samples()) v = 0;
    make_music(&one, 44100, 1, 1);
    make_music(&loud, 44100, 2, 30000);
    uint32_t seed = 3;
    for (int16_t& v : loud.samples()) { seed = seed * 1664525 + 1013904223; v = (int16_t)(seed >> 16); }
    for (AudioData* s : {&silent, &one, &loud}) {
      ByteBuffer out(a);
      CHECK(encode_aac(*s, 128000, &out) && decode_aac(out.view(), &dec) && dec.frame_count() >= s->frame_count());
    }
    ByteBuffer out(a);
    CHECK(encode_aac(silent, 128000, &out) && out.size() < 5 * 9 * 8);  // a handful of bytes per block
    CHECK(decode_aac(out.view(), &dec));
    bool all_zero = true;
    for (int16_t v : dec.samples()) all_zero = all_zero && v == 0;
    CHECK(all_zero);
  }
  {  // what cannot be encoded
    AudioData odd_rate(a), empty(a);
    make_music(&odd_rate, 12345, 1, 4000);
    ByteBuffer out(a);
    CHECK(!encode_aac(odd_rate, 64000, &out) && out.size() == 0);
    CHECK(!encode_aac(empty, 64000, &out));
  }
  {  // blocks and the AudioSpecificConfig
    AacEncoder enc(a);
    CHECK(!enc.init(44100, 3, 0) && !enc.init(12345, 2, 0));
    CHECK(enc.init(44100, 2, 128000) && enc.sampling_index() == 4 && enc.channels() == 2);
    ByteBuffer cfg(a), blk(a);
    CHECK(enc.audio_specific_config(&cfg) && cfg.size() == 2);
    uint32_t object = 0, index = 0, chans = 0;
    CHECK(parse_audio_specific_config(cfg.view(), &object, &index, &chans) && object == 2 && index == 4 && chans == 2);
    int16_t pcm[2048] = {};
    CHECK(!enc.encode_block(ArrayView<const int16_t>(pcm, 100), &blk));  // not a whole block
    CHECK(enc.encode_block(ArrayView<const int16_t>(pcm, 2048), &blk) && blk.size() > 0);
    AacDecoder dec(a);
    int16_t out_pcm[2048];
    CHECK(dec.init(4) && dec.decode_block(blk.view(), out_pcm) && dec.channels() == 2);
  }
  CHECK(a.live == 0);
}

// Damaged and random input must be refused or decoded, never crash or read out of bounds (the sanitizers watch).
static void test_fuzz() {
  TestAllocator a;
  const char* names[] = {"rgb.png", "rgba16.png", "anim.apng", "rgb.jpg", "progressive.jpg", "rle8.bmp", "rgba32.bmp", "anim.gif", "interlaced.gif", "rgba.qoi", "rgb.ppm",
                         "rgba.pam", "rgb.tga", "rgb_lzw.tif", "tiled.tif", "planar.tif", "big.webp", "pal.webp", "lossy.webp", "lossy_alpha.webp", "anim.webp", "anim_lossy.webp", "pcm16.wav", "ima.wav", "adpcm_ms.wav", "s24.aiff", "ulaw.aifc", "s16.au", "best.flac", "s32.flac", "stereo.ogg", "mono.ogg", "opus_silk_wb_mono.opus", "opus_silk_wb_stereo_60.opus", "opus_hybrid_stereo.opus", "opus_celt_stereo_10.opus", "opus.ogg", "stereo.mp3", "mono.mp3", "lsf.mp2", "layer1.mp1", "stereo.aac", "mono.m4a", "stereo.mov", "mp3.mp4"};
  uint32_t seed = 2024;
  auto rnd = [&seed]() { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (const char* name : names) {
    char path[160];
    snprintf(path, sizeof path, "tests/data/%s", name);
    File file(path);
    CHECK(file.data != nullptr);
    if (!file.data) continue;
    Image img(a);
    AudioData au(a);
    for (int round = 0; round < 60; ++round) {
      uint8_t* copy = static_cast<uint8_t*>(malloc(file.size));
      memcpy(copy, file.data, file.size);
      long len = file.size;
      switch (round % 4) {
        case 0: len = rnd() % file.size; break;                                     // cut short
        case 1: for (int k = 0; k < 4; ++k) copy[rnd() % file.size] ^= 1 << (rnd() % 8); break;  // bit flips
        case 2: for (int k = 0; k < 12; ++k) copy[rnd() % file.size] = static_cast<uint8_t>(rnd()); break;
        default: for (int k = 0; k < 3; ++k) copy[rnd() % (file.size < 64 ? file.size : 64)] = static_cast<uint8_t>(rnd()); break;  // damaged headers
      }
      ConstByteView view(copy, len);
      decode_image(view, &img);
      decode_audio(view, &au);
      decode_y4m(view, &img);
      free(copy);
    }
  }
  // Pure noise behind each signature.
  for (int round = 0; round < 300; ++round) {
    uint8_t noise[300];
    for (uint8_t& b : noise) b = static_cast<uint8_t>(rnd());
    struct Sig { const char* bytes; unsigned size; };
#define SIG(x) {x, sizeof(x) - 1}
    static const Sig sigs[] = {SIG("\x89PNG\r\n\x1a\n"), SIG("\xFF\xD8\xFF\xE0"), SIG("BM"), SIG("GIF89a"), SIG("qoif"), SIG("P6\n"), SIG("RIFF\x24\x00\x00\x00WAVEfmt "), SIG("FORM\x00\x00\x00\x40" "AIFF"), SIG(".snd"), SIG("fLaC"), SIG("YUV4MPEG2 W8 H8\n"), SIG("\xFF\xFB\x90\x00"), SIG("\xFF\xFB\x92\x40"), SIG("II*\x00\x08\x00\x00\x00"), SIG("RIFF\x40\x00\x00\x00WEBPVP8L"), SIG("RIFF\x40\x00\x00\x00WEBPVP8 "), SIG("RIFF\x40\x00\x00\x00WEBPVP8X\x0a\x00\x00\x00\x10\x00\x00\x00")};
#undef SIG
    const Sig& sig = sigs[round % (sizeof sigs / sizeof sigs[0])];
    memcpy(noise, sig.bytes, sig.size < 24 ? sig.size : 24);
    Image img(a);
    AudioData au(a);
    decode_image(ConstByteView(noise, sizeof noise), &img);
    decode_audio(ConstByteView(noise, sizeof noise), &au);
    decode_y4m(ConstByteView(noise, sizeof noise), &img);
  }
  CHECK(a.live == 0);
}

// ---- video: Y4M, the player, and the acceleration-first front door ----


static void test_y4m() {
  TestAllocator a;
  Image clip(a), back(a);
  CHECK(clip.create(33, 21, 5));  // odd sizes exercise the chroma rounding
  for (uint32_t f = 0; f < 5; ++f) {
    for (uint32_t y = 0; y < 21; ++y)
      for (uint32_t x = 0; x < 33; ++x) clip.frame(f)[y * 33 + x] = 0xFF000000u | ((x * 6 + f * 20) & 255) << 16 | (y * 10 & 255) << 8 | 128;
    clip.set_delay_ms(f, 40);
  }
  ByteBuffer y4m(a);
  CHECK(encode_y4m(clip, &y4m) && detect_video_file(y4m.view()) == VideoFileFormat::Y4m);
  CHECK(decode_y4m(y4m.view(), &back) && back.width() == 33 && back.height() == 21 && back.frame_count() == 5 && back.total_duration_ms() == 200);
  CHECK(psnr(clip, back) > 28);
  // A truncated last frame is dropped; interlaced and unknown chroma are refused.
  CHECK(decode_y4m(y4m.view().first(y4m.size() - 100), &back) && back.frame_count() == 4);
  const char bad[] = "YUV4MPEG2 W4 H4 F25:1 It C420\nFRAME\n";
  CHECK(!decode_y4m(ConstByteView(reinterpret_cast<const uint8_t*>(bad), sizeof bad - 1), &back));
  // 30000/1001 fps: delays alternate 33/34 ms so that the running time stays exact.
  const char head[] = "YUV4MPEG2 W2 H2 F30000:1001 Ip C444\n";
  ByteBuffer ntsc(a);
  ntsc.append(ConstByteView(reinterpret_cast<const uint8_t*>(head), sizeof head - 1));
  for (int f = 0; f < 30; ++f) {
    ntsc.append(ConstByteView(reinterpret_cast<const uint8_t*>("FRAME\n"), 6));
    ntsc.put_zeros(12);
  }
  CHECK(decode_y4m(ntsc.view(), &back) && back.frame_count() == 30 && back.total_duration_ms() == 1001);
}

// A byte source over memory that records how much is read from it at a time.
struct CountingSource final : media::IByteSource {
  explicit CountingSource(ConstByteView d) : inner(d) {}
  uint64_t size() const override { return inner.size(); }
  uint64_t read(uint64_t offset, ByteView out) override {
    ++reads;
    const uint64_t got = inner.read(offset, out);
    bytes += got;
    if (got > biggest) biggest = got;
    return got;
  }
  media::MemoryByteSource inner;
  uint64_t reads = 0, bytes = 0, biggest = 0;
};

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

// A codec device that claims to be a hardware engine for Motion JPEG (it runs the software codec underneath) and counts its sessions;
// with `refuse` set it turns every session down, as a busy or broken engine would.
struct FakeEngine final : hardware::IVideoCodecDevice {
  hardware::SoftwareVideoCodec inner;
  hardware::VideoCodecInfo info_;
  int decoders = 0, encoders = 0;
  bool refuse = false;
  explicit FakeEngine(allocators::IAllocator& a) : inner(a, true) {
    info_ = inner.info();
    info_.accelerated = true;
  }
  const char* name() const override { return "fake engine"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  const hardware::VideoCodecInfo& info() const override { return info_; }
  UniquePointer<hardware::IVideoDecoder> create_decoder(const hardware::DecoderConfig& c) override {
    if (refuse) return UniquePointer<hardware::IVideoDecoder>();
    ++decoders;
    return inner.create_decoder(c);
  }
  UniquePointer<hardware::IVideoEncoder> create_encoder(const hardware::EncoderConfig& c) override {
    if (refuse) return UniquePointer<hardware::IVideoEncoder>();
    ++encoders;
    return inner.create_encoder(c);
  }
};

struct EngineProxy final : hardware::IVideoCodecDevice {  // lets the test keep a pointer to the engine the system owns
  FakeEngine* engine;
  explicit EngineProxy(FakeEngine* e) : engine(e) {}
  const char* name() const override { return engine->name(); }
  bool initialize() override { return true; }
  void shutdown() override {}
  const hardware::VideoCodecInfo& info() const override { return engine->info(); }
  UniquePointer<hardware::IVideoDecoder> create_decoder(const hardware::DecoderConfig& c) override { return engine->create_decoder(c); }
  UniquePointer<hardware::IVideoEncoder> create_encoder(const hardware::EncoderConfig& c) override { return engine->create_encoder(c); }
};

static void test_acceleration_and_fallback() {
  TestAllocator a;
  NoPci pci;
  hardware::SystemVideoCodecs codecs(pci, a);
  codecs.initialize();
  MediaCodecs plain(&codecs), none(nullptr);
  CHECK(!plain.jpeg_accelerated() && !none.jpeg_accelerated());  // only the CPU codec is on this machine

  File jpg("tests/data/rgb.jpg");
  File png("tests/data/rgb.png");
  Image cpu(a), got(a);
  CodecPath path = CodecPath::Hardware;
  CHECK(plain.decode_image(jpg.view(), &cpu, nullptr, &path) && path == CodecPath::Cpu);

  FakeEngine engine(a);
  CHECK(codecs.add_device(UniquePointer<hardware::IVideoCodecDevice>::make_as<EngineProxy>(a, &engine)));
  CHECK(plain.jpeg_accelerated() && plain.jpeg_encode_accelerated());
  ImageFormat detected = ImageFormat::Unknown;
  CHECK(plain.decode_image(jpg.view(), &got, &detected, &path) && path == CodecPath::Hardware && detected == ImageFormat::Jpeg && engine.decoders == 1);
  CHECK(got.width() == 40 && got.height() == 30 && psnr(cpu, got) > 30);  // the same picture (another IDCT/upsampler may differ slightly)
  CHECK(plain.decode_image(png.view(), &got, nullptr, &path) && path == CodecPath::Cpu && engine.decoders == 1);  // PNG has no engine
  ByteBuffer out(a);
  CHECK(plain.encode_image(cpu, ImageFormat::Jpeg, 85, &out, &path) && path == CodecPath::Hardware && engine.encoders == 1);
  CHECK(decode_image(out.view(), &got) && got.width() == 40);

  engine.refuse = true;  // a broken engine: the CPU takes over and the call still succeeds
  out.clear();
  CHECK(plain.decode_image(jpg.view(), &got, nullptr, &path) && path == CodecPath::Cpu && same_pixels(cpu, got, true));
  CHECK(plain.encode_image(cpu, ImageFormat::Jpeg, 85, &out, &path) && path == CodecPath::Cpu && out.size() > 100);
  engine.refuse = false;
  uint8_t junk[64] = {0xFF, 0xD8, 0xFF};
  CHECK(!plain.decode_image(ConstByteView(junk, sizeof junk), &got) && got.empty());  // garbage fails on both paths

  codecs.shutdown();
}

static void test_media_front_door_audio_video() {
  TestAllocator a;
  MediaCodecs codecs;
  File wav("tests/data/ima.wav"), gif("tests/data/anim.gif");
  AudioData au(a);
  Image clip(a);
  AudioFileFormat af;
  VideoFileFormat vf;
  CodecPath path = CodecPath::Hardware;
  CHECK(codecs.decode_audio(wav.view(), &au, &af, &path) && af == AudioFileFormat::Wav && path == CodecPath::Cpu && au.frame_count() > 2000);
  CHECK(codecs.decode_video(gif.view(), &clip, &vf) && vf == VideoFileFormat::Gif && clip.frame_count() == 4);
  CHECK(!codecs.decode_video(wav.view(), &clip, &vf) && vf == VideoFileFormat::Unknown);
  File webp("tests/data/anim.webp");
  CHECK(codecs.decode_video(webp.view(), &clip, &vf) && vf == VideoFileFormat::AnimatedWebp && clip.frame_count() == 5 && clip.delay_ms(1) == 90 && clip.loop_count() == 3);
  File still_webp("tests/data/lossy.webp");
  CHECK(detect_video_file(still_webp.view()) == VideoFileFormat::Unknown);
}

// (the H.264 tests follow the player test)
static int player_red(const graphics::Surface* s) { return s ? static_cast<int>((s->row(s->height / 2)[s->width / 2] >> 16) & 0xFF) : -1; }

static void test_player_media() {
  TestAllocator a;
  {
  NoPci pci;
  hardware::SystemVideoCodecs codecs(pci, a);
  codecs.initialize();
  FakeClock clock;
  video::VideoPlayer player(clock, codecs, a);

  // A 3-frame GIF-style clip with different delays: 100, 300 and 200 ms; every frame a different flat colour.
  Image clip(a);
  CHECK(clip.create(16, 8, 3));
  const uint32_t reds[3] = {200, 100, 30};
  const uint32_t delays[3] = {100, 300, 200};
  for (uint32_t f = 0; f < 3; ++f) {
    for (uint32_t& p : clip.frame(f)) p = 0xFF000000u | reds[f] << 16 | 0x40 << 8 | 0x20;
    clip.set_delay_ms(f, delays[f]);
  }
  ByteBuffer apng(a);
  CHECK(encode_image(clip, ImageFormat::Png, 60, &apng));
  CHECK(player.open_media(apng.view()) == video::OpenResult::Ok);
  CHECK(player.frame_count() == 3 && player.duration_us() == 600000 && player.frame_width() == 16 && player.frame_height() == 8 && !player.has_audio());
  CHECK(player_red(player.picture()) == 200);
  clock.now = 1000000;
  CHECK(player.play());
  clock.now += 99000;
  player.update();
  CHECK(player_red(player.picture()) == 200 && player.current_frame() == 0);
  clock.now += 2000;  // 101 ms: the second frame
  player.update();
  CHECK(player_red(player.picture()) == 100 && player.current_frame() == 1);
  clock.now += 300000;  // 401 ms: the third
  player.update();
  CHECK(player_red(player.picture()) == 30 && player.current_frame() == 2 && player.playing());
  clock.now += 300000;  // past the end
  player.update();
  CHECK(!player.playing() && player.position_us() == 600000 && player_red(player.picture()) == 30);
  player.seek(150000);
  CHECK(player.current_frame() == 1 && player_red(player.picture()) == 100);
  player.stop();
  CHECK(player.current_frame() == 0);

  // Y4M, and a still picture as a one-frame video.
  Image y(a);
  CHECK(y.create(8, 8, 2));
  y.set_delay_ms(0, 50);
  y.set_delay_ms(1, 50);
  for (uint32_t f = 0; f < 2; ++f)
    for (uint32_t& p : y.frame(f)) p = f ? 0xFF2020F0u : 0xFFF02020u;
  ByteBuffer y4m(a);
  CHECK(encode_y4m(y, &y4m) && player.open_media(y4m.view()) == video::OpenResult::Ok && player.frame_count() == 2 && player.duration_us() == 100000);
  // A longer Y4M clip read frame by frame from a source: the same pictures and times as when it is decoded whole.
  {
    Image clip(a), whole(a);
    CHECK(clip.create(33, 21, 40));
    for (uint32_t f = 0; f < 40; ++f) {
      clip.set_delay_ms(f, 40);
      for (uint32_t i = 0; i < clip.frame(f).size(); ++i) clip.frame(f)[i] = 0xFF000000u | ((f * 6 + i) & 255) << 16 | ((i * 3) & 255) << 8 | 90;
    }
    ByteBuffer file(a);
    CHECK(encode_y4m(clip, &file) && decode_y4m(file.view(), &whole) && whole.frame_count() == 40);
    CountingSource source(file.view());
    CHECK(player.open_media(source) == video::OpenResult::Ok && player.frame_count() == 40 && player.frame_width() == 33 && player.duration_us() == 1600000);
    CHECK(source.bytes < file.size() / 5);
    const uint32_t picks[5] = {0, 17, 39, 3, 4};
    for (uint32_t f : picks) {
      player.seek(int64_t{f} * 40000 + 7000);
      const graphics::Surface* sf = player.picture();
      bool same = player.current_frame() == f;
      for (uint32_t y = 0; y < 21 && same; ++y)
        for (uint32_t x = 0; x < 33; ++x)
          if (sf->row(y)[x] != whole.frame(f)[y * 33 + x]) same = false;
      CHECK(same);
    }
    CHECK(source.bytes < file.size() / 5 * 2);
    player.close();
  }
  File still("tests/data/rgb.png");
  CHECK(player.open_media(still.view()) == video::OpenResult::Ok && player.frame_count() == 1 && player.frame_width() == 40);
  const uint8_t junk[40] = {1, 2, 3};
  CHECK(player.open_media(ConstByteView(junk, sizeof junk)) == video::OpenResult::Unsupported && player.picture() == nullptr);
  player.close();

  // An AVI whose soundtrack is mu-law (a PCM clip with the sound's format tag changed): it plays, decoded to 16 bits.
  {
    video::AviWriter writer(a);
    audio::StreamFormat pcm{8000, 1, audio::SampleType::U8};
    CHECK(writer.begin(hardware::VideoCodec::Raw, 16, 8, 10, 1, hardware::PixelFormat::I420, &pcm));
    uint8_t frame[16 * 8 * 3 / 2] = {0}, samples[800];
    for (uint32_t k = 0; k < 800; ++k) samples[k] = static_cast<uint8_t>(k * 3);
    for (int f = 0; f < 4; ++f) CHECK(writer.add_frame(ConstByteView(frame, sizeof frame)) && writer.add_audio(ConstByteView(samples, 800)));
    CHECK(writer.finish());
    uint8_t* file = static_cast<uint8_t*>(malloc(writer.file().size()));
    memcpy(file, writer.file().data(), writer.file().size());
    const uint8_t wave[8] = {1, 0, 1, 0, 0x40, 0x1F, 0, 0};  // WAVEFORMATEX: PCM, mono, 8000 Hz
    bool patched = false;
    for (uint64_t i = 0; !patched && i + 8 <= writer.file().size(); ++i)
      if (memcmp(file + i, wave, 8) == 0) { file[i] = 7; patched = true; }
    CHECK(patched);
    FakeClock clock;
    FakeDevice device;
    audio::AudioMixer mixer(device, a);
    CHECK(mixer.initialize());
    video::VideoPlayer with_sound(clock, codecs, a, &mixer);
    CHECK(with_sound.open_media(ConstByteView(file, writer.file().size())) == video::OpenResult::Ok);
    CHECK(with_sound.file().audio().tag == 7 && with_sound.file().audio().format.type == audio::SampleType::S16 && with_sound.file().audio().bytes == 3200);
    CHECK(with_sound.has_audio() && with_sound.audio()->duration() == 3200);
    with_sound.close();
    free(file);
  }
  codecs.shutdown();
  }
  CHECK(a.live == 0);
}


// ---- H.264 ----

// Feeds a whole stream to a decoder session and hashes the pictures that come out, the way make_video_fixtures.py hashes
// ffmpeg's: the cropped I420 frames one after the other. With `bgra` set the hashes are those of the converted pictures instead.
struct H264Run {
  uint64_t hash = kFnvBasis;
  uint32_t frames = 0, width = 0, height = 0;
  uint64_t frame_hash[64];
  int64_t last_pts = 0;
};

static void h264_take(hardware::IVideoDecoder& dec, H264Run* run, bool bgra) {
  hardware::VideoFrame f;
  while (dec.receive(&f)) {
    const uint32_t w = f.format.width, h = f.format.height;
    run->width = w;
    run->height = h;
    uint64_t fh = kFnvBasis;
    if (bgra) {
      CHECK(f.format.pixel_format == hardware::PixelFormat::Bgra8888);
      for (uint32_t y = 0; y < h; ++y) fh = fnv(fh, f.plane[0] + uint64_t{y} * f.stride[0], uint64_t{w} * 4);
    } else {
      CHECK(f.format.pixel_format == hardware::PixelFormat::I420);
      for (uint32_t p = 0; p < 3; ++p)
        for (uint32_t y = 0; y < (p ? h / 2 : h); ++y) {
          run->hash = fnv(run->hash, f.plane[p] + uint64_t{y} * f.stride[p], p ? w / 2 : w);
        }
    }
    if (run->frames < 64) run->frame_hash[run->frames] = fh;
    ++run->frames;
  }
}

static void h264_feed(hardware::IVideoDecoder& dec, ConstByteView packet, H264Run* run, bool bgra) {
  hardware::VideoPacket p;
  p.data = packet;
  hardware::SubmitResult r = dec.submit(p);
  if (r == hardware::SubmitResult::Full) {
    h264_take(dec, run, bgra);
    r = dec.submit(p);
  }
  h264_take(dec, run, bgra);
}

// Splits an Annex B stream into access units: a slice with first_mb_in_slice 0, or parameter sets / SEI / a delimiter after a slice, starts one.
static bool next_access_unit(ConstByteView d, uint64_t* pos, ConstByteView* au) {
  auto start_at = [&](uint64_t k) { return k + 3 <= d.size() && d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1; };
  uint64_t i = *pos;
  while (i < d.size() && !start_at(i)) ++i;
  if (i >= d.size()) return false;
  const uint64_t begin = i;
  bool has_slice = false;
  while (i < d.size()) {
    const uint64_t hdr = i + 3;
    if (hdr >= d.size()) { i = d.size(); break; }
    const int t = d[hdr] & 31;
    const bool slice = t == 1 || t == 5;
    const bool first = slice && hdr + 1 < d.size() && (d[hdr + 1] & 0x80);
    if (i != begin && ((has_slice && (t == 6 || t == 7 || t == 8 || t == 9)) || (slice && first && has_slice))) break;
    has_slice = has_slice || slice;
    uint64_t j = hdr + 1;
    while (j < d.size() && !start_at(j)) ++j;
    i = j;
  }
  *au = d.subview(begin, i - begin);
  *pos = i;
  return true;
}

// Decodes a fixture (Annex B file, MP4 or AVI) through the CPU codec device; false if the file could not be opened.
static bool h264_decode_file(allocators::IAllocator& a, ConstByteView data, H264Run* run, bool bgra) {
  hardware::SoftwareVideoCodec device(a);
  hardware::DecoderConfig config;
  config.codec = hardware::VideoCodec::H264;
  config.output = bgra ? hardware::PixelFormat::Bgra8888 : hardware::PixelFormat::I420;
  const VideoFileFormat format = detect_video_file(data);
  if (format == VideoFileFormat::Mp4) {
    Mp4File mp4(a);
    if (!mp4.open(data)) return false;
    const Mp4Track* t = mp4.find(Mp4TrackKind::Video);
    if (!t) return false;
    config.codec_config = t->config;
    UniquePointer<hardware::IVideoDecoder> dec = device.create_decoder(config);
    if (!dec) return false;
    for (uint32_t i = 0; i < t->sample_count; ++i) h264_feed(*dec, mp4.sample(*t, i), run, bgra);
    dec->flush();
    h264_take(*dec, run, bgra);
    return true;
  }
  UniquePointer<hardware::IVideoDecoder> dec = device.create_decoder(config);
  if (!dec) return false;
  if (format == VideoFileFormat::Avi) {
    video::AviFile avi(a);
    if (!avi.open(data) || avi.video().codec != hardware::VideoCodec::H264) return false;
    for (uint32_t i = 0; i < avi.frame_count(); ++i) {
      hardware::VideoPacket p;
      if (avi.frame(i, &p)) h264_feed(*dec, p.data, run, bgra);
    }
  } else {
    uint64_t pos = 0;
    ConstByteView au;
    while (next_access_unit(data, &pos, &au)) h264_feed(*dec, au, run, bgra);
  }
  dec->flush();
  h264_take(*dec, run, bgra);
  return true;
}

static void test_h264() {
  TestAllocator a;
  FILE* m = fopen("tests/data/manifest_video.txt", "r");
  CHECK(m != nullptr);
  if (!m) return;
  char kind[16], name[96];
  unsigned w, h, frames;
  unsigned long long hash;
  int count = 0;
  while (fscanf(m, "%15s %95s %u %u %u %llx", kind, name, &w, &h, &frames, &hash) == 6) {
    char path[160];
    snprintf(path, sizeof path, "tests/data/%s", name);
    File file(path);
    if (!file.data) { printf("missing fixture %s\n", path); ++g_failed; continue; }
    H264Run run;
    CHECK(h264_decode_file(a, file.view(), &run, false));
    if (run.frames != frames || run.width != w || run.height != h || run.hash != hash) {
      printf("H.264 fixture %s: got %u frames %ux%u hash %016llx, wanted %u frames %ux%u hash %016llx\n", name, run.frames, run.width, run.height,
             static_cast<unsigned long long>(run.hash), frames, w, h, hash);
      ++g_failed;
    }
    ++count;
  }
  fclose(m);
  CHECK(count >= 10);
  CHECK(a.live == 0);

  // Refused or damaged input: not an H.264 stream, a stream without parameter sets, pictures before the first IDR.
  {
    hardware::SoftwareVideoCodec device(a);
    hardware::DecoderConfig config;
    config.codec = hardware::VideoCodec::H264;
    UniquePointer<hardware::IVideoDecoder> dec = device.create_decoder(config);
    CHECK(dec != nullptr);
    hardware::VideoPacket junk;
    const uint8_t noise[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    junk.data = ConstByteView(noise, sizeof noise);
    CHECK(dec->submit(junk) == hardware::SubmitResult::Invalid);
    File file("tests/data/h264_cavlc.h264");
    uint64_t pos = 0;
    ConstByteView sps_au, p_au, p2_au;
    CHECK(next_access_unit(file.view(), &pos, &sps_au) && next_access_unit(file.view(), &pos, &p_au) && next_access_unit(file.view(), &pos, &p2_au));
    // The P pictures alone (no sequence parameter set seen) give nothing.
    hardware::VideoPacket pk;
    pk.data = p_au;
    hardware::VideoFrame f;
    dec->submit(pk);
    CHECK(!dec->receive(&f));
    CHECK(dec->output_format().width == 0);
    // After the IDR picture and a seek (reset) they do not decode either, until the next IDR.
    pk.data = sps_au;
    CHECK(dec->submit(pk) == hardware::SubmitResult::Accepted);
    dec->flush();
    CHECK(dec->receive(&f) && f.format.width == 90 && f.format.height == 50 && f.keyframe);
    CHECK(dec->output_format().width == 90 && dec->output_format().height == 50);
    dec->reset();
    pk.data = p2_au;
    dec->submit(pk);
    dec->flush();
    CHECK(!dec->receive(&f));
    CHECK(dec->statistics().frames == 1);
  }
  CHECK(a.live == 0);
}

// Mutated copies of the H.264 fixtures must neither crash nor leak.
static void test_h264_fuzz() {
  TestAllocator a;
  const char* names[] = {"h264_pcm_cavlc.h264", "h264_pcm_cabac.h264", "h264_cavlc.h264", "h264_bframes.h264", "h264_cqm.h264", "h264_noise.h264", "h264_ltr.h264", "h264_bframes.mp4", "h264.avi"};
  uint32_t seed = 77;
  auto rnd = [&seed]() { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (const char* name : names) {
    char path[160];
    snprintf(path, sizeof path, "tests/data/%s", name);
    File file(path);
    CHECK(file.data != nullptr);
    if (!file.data) continue;
    for (int round = 0; round < 25; ++round) {
      uint8_t* copy = static_cast<uint8_t*>(malloc(file.size));
      memcpy(copy, file.data, file.size);
      long len = file.size;
      switch (round % 4) {
        case 0: len = rnd() % file.size; break;
        case 1: for (int k = 0; k < 4; ++k) copy[rnd() % file.size] ^= 1 << (rnd() % 8); break;
        case 2: for (int k = 0; k < 12; ++k) copy[rnd() % file.size] = static_cast<uint8_t>(rnd()); break;
        default: for (int k = 0; k < 3; ++k) copy[rnd() % (file.size < 400 ? file.size : 400)] = static_cast<uint8_t>(rnd()); break;
      }
      H264Run run;
      h264_decode_file(a, ConstByteView(copy, len), &run, round & 1);
      free(copy);
    }
  }
  CHECK(a.live == 0);
}

// The player on H.264: an MP4 with B frames (frames decoded out of order and shown in order) and an AVI.
static void test_h264_player() {
  TestAllocator a;
  {
  NoPci pci;
  hardware::SystemVideoCodecs codecs(pci, a);
  codecs.initialize();
  FakeClock clock;
  video::VideoPlayer player(clock, codecs, a);
  const struct { const char* name; uint32_t frames; int64_t frame_us; } cases[] = {{"h264_bframes.mp4", 15, 40000}, {"h264.avi", 8, 40000}};
  for (const auto& c : cases) {
    char path[160];
    snprintf(path, sizeof path, "tests/data/%s", c.name);
    File file(path);
    CHECK(file.data != nullptr);
    if (!file.data) continue;
    H264Run expect;
    CHECK(h264_decode_file(a, file.view(), &expect, true) && expect.frames == c.frames);
    CHECK(player.open_media(file.view()) == video::OpenResult::Ok);
    CHECK(player.frame_count() == c.frames && player.frame_width() == expect.width && player.frame_height() == expect.height);
    CHECK(player.duration_us() >= int64_t{c.frames} * c.frame_us - 1000 && player.duration_us() <= int64_t{c.frames} * c.frame_us + 1000);
    auto picture_hash = [&]() {
      const graphics::Surface* s = player.picture();
      uint64_t h = kFnvBasis;
      for (uint32_t y = 0; y < s->height; ++y) h = fnv(h, reinterpret_cast<const uint8_t*>(s->row(y)), uint64_t{s->width} * 4);
      return h;
    };
    CHECK(picture_hash() == expect.frame_hash[0]);
    // Seeking forwards, backwards and far: each shows the right picture.
    const uint32_t order[] = {3, 4, 5, 2, 7, 1, 14 % c.frames, 0, c.frames - 1, 6, 6, 5};
    for (uint32_t f : order) {
      player.seek(int64_t{f} * c.frame_us + 5000);
      CHECK(player.current_frame() == f);
      if (picture_hash() != expect.frame_hash[f]) { printf("%s: wrong picture at frame %u\n", c.name, f); ++g_failed; }
    }
    // Playing: the pictures follow the clock.
    player.stop();
    clock.now = 5000000;
    CHECK(player.play());
    for (uint32_t f = 1; f < c.frames; ++f) {
      clock.now += c.frame_us;
      player.update();
      CHECK(player.current_frame() == f);
      if (picture_hash() != expect.frame_hash[f]) { printf("%s: wrong picture while playing, frame %u\n", c.name, f); ++g_failed; }
    }
    clock.now += 100000;
    player.update();
    CHECK(!player.playing());
    player.close();
  }
  // The same files read through a source, a piece at a time: the same pictures.
  for (const auto& c : cases) {
    char path[160];
    snprintf(path, sizeof path, "tests/data/%s", c.name);
    File file(path);
    if (!file.data) continue;
    H264Run expect;
    CHECK(h264_decode_file(a, file.view(), &expect, true));
    CountingSource source(file.view());
    CHECK(player.open_media(source) == video::OpenResult::Ok && player.frame_count() == c.frames);
    const uint32_t order[] = {3, 4, 2, 7, 0, c.frames - 1, 5};
    for (uint32_t f : order) {
      player.seek(int64_t{f} * c.frame_us + 5000);
      const graphics::Surface* sf = player.picture();
      uint64_t h = kFnvBasis;
      for (uint32_t y = 0; y < sf->height; ++y) h = fnv(h, reinterpret_cast<const uint8_t*>(sf->row(y)), uint64_t{sf->width} * 4);
      CHECK(player.current_frame() == f && h == expect.frame_hash[f]);
    }
    CHECK(source.reads > 3);
    player.close();
  }
  // The MP4's AAC soundtrack plays through a mixer: 0.6 s at 22.05 kHz, give or take the codec's priming.
  {
    File mp4("tests/data/h264_bframes.mp4");
    FakeDevice device;
    audio::AudioMixer mixer(device, a);
    CHECK(mp4.data != nullptr && mixer.initialize());
    video::VideoPlayer with_sound(clock, codecs, a, &mixer);
    CHECK(with_sound.open_media(mp4.view()) == video::OpenResult::Ok && with_sound.has_audio());
    CHECK(with_sound.audio()->duration() > 22050 * 55 / 100 && with_sound.audio()->duration() < 22050 * 65 / 100);
    with_sound.seek(240000);
    CHECK(with_sound.current_frame() == 6);
    with_sound.close();
    CountingSource source(mp4.view());
    CHECK(with_sound.open_media(source) == video::OpenResult::Ok && with_sound.has_audio() && with_sound.audio()->duration() > 22050 * 55 / 100);
    with_sound.close();
  }
  // An MP4 without H.264 video (an audio file) is not a video.
  File m4a("tests/data/mono.m4a");
  CHECK(m4a.data != nullptr && player.open_media(m4a.view()) == video::OpenResult::Unsupported);
  player.close();
  }
  CHECK(a.live == 0);
}

int main() {
  test_fixtures();
  test_image_roundtrip();
  test_zlib();
  test_audio_roundtrip();
  test_aac_encode();
  test_y4m();
  test_acceleration_and_fallback();
  test_media_front_door_audio_video();
  test_player_media();
  test_h264();
  test_h264_player();
  test_fuzz();
  test_h264_fuzz();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("media tests passed\n");
  return 0;
}
