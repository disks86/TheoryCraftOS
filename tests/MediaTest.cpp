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
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
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
  char kind[16], name[96];
  unsigned x, y, z;
  unsigned long long hash;
  int count = 0;
  while (fscanf(m, "%15s %95s %u %u %u %llx", kind, name, &x, &y, &z, &hash) == 6) {
    char path[160];
    snprintf(path, sizeof path, "tests/data/%s", name);
    File file(path);
    if (!file.data) { printf("missing fixture %s\n", path); ++g_failed; continue; }
    ++count;
    bool ok = false;
    if (!strcmp(kind, "image")) {
      Image img(a);
      ok = decode_image(file.view(), &img) && img.width() == x && img.height() == y && img.frame_count() == z && image_hash(img) == hash;
    } else {
      AudioData au(a);
      ok = decode_audio(file.view(), &au) && au.format().sample_rate == x && au.format().channels == y && au.frame_count() == z && audio_hash(au) == hash;
    }
    if (!ok) { printf("FAILED fixture %s\n", name); ++g_failed; }
  }
  fclose(m);
  CHECK(count >= 52);
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
                                                             {ImageFormat::Tga, true, true}, {ImageFormat::Pnm, true, false}, {ImageFormat::Tiff, true, true}};
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

// Damaged and random input must be refused or decoded, never crash or read out of bounds (the sanitizers watch).
static void test_fuzz() {
  TestAllocator a;
  const char* names[] = {"rgb.png", "rgba16.png", "anim.apng", "rgb.jpg", "progressive.jpg", "rle8.bmp", "rgba32.bmp", "anim.gif", "interlaced.gif", "rgba.qoi", "rgb.ppm",
                         "rgba.pam", "rgb.tga", "rgb_lzw.tif", "tiled.tif", "planar.tif", "pcm16.wav", "ima.wav", "adpcm_ms.wav", "s24.aiff", "ulaw.aifc", "s16.au", "best.flac", "s32.flac"};
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
  for (int round = 0; round < 200; ++round) {
    uint8_t noise[300];
    for (uint8_t& b : noise) b = static_cast<uint8_t>(rnd());
    static const char* const sigs[] = {"\x89PNG\r\n\x1a\n", "\xFF\xD8\xFF\xE0", "BM", "GIF89a", "qoif", "P6\n", "RIFF\x24\x00\x00\x00WAVEfmt ", "FORM\x00\x00\x00\x40" "AIFF", ".snd", "fLaC", "YUV4MPEG2 W8 H8\n"};
    const char* sig = sigs[round % 11];
    memcpy(noise, sig, strlen(sig) < 16 ? strlen(sig) : 16);
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
}

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

int main() {
  test_fixtures();
  test_image_roundtrip();
  test_zlib();
  test_audio_roundtrip();
  test_y4m();
  test_acceleration_and_fallback();
  test_media_front_door_audio_video();
  test_player_media();
  test_fuzz();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("media tests passed\n");
  return 0;
}
