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

// Host-side tests for AVI reading and writing and the video player (over the real software codecs, a fake
// clock and a fake audio device).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Hardware/VideoCodec/ColorConvert.hpp"
#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Video/AviFile.hpp"
#include "Video/AviWriter.hpp"
#include "Video/VideoPlayer.hpp"

using namespace hardware;
using namespace video;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)
static bool near(int a, int b, int t) { return a >= b - t && a <= b + t; }

struct FakeClock final : IClock {
  uint64_t now = 0;
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return now; }
};

struct NoPci final : IPciBus {
  const char* name() const override { return "no pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t) const override { return 0; }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

struct FakeDevice final : IAudioDevice {
  uint32_t queued = 0;
  const char* name() const override { return "fake audio"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    uint32_t n = pcm.size() < 16384 - queued ? (uint32_t)pcm.size() : 16384 - queued;
    n -= n % kAudioFrameSize;
    queued += n;
    return n;
  }
  uint32_t queued_bytes() override { return queued; }
  uint32_t free_bytes() override { return 16384 - queued; }
  void play_all() { queued = 0; }
  void set_volume(uint8_t) override {}
  uint8_t volume() const override { return 80; }
  void set_muted(bool) override {}
  bool muted() const override { return false; }
  AudioStatistics statistics() override { return {}; }
};

// Frame i of the test clip: a flat colour whose red level is 10 * i (so a frame can be told by its colour), a
// little detail so the JPEG has something to code.
static void make_frame(VideoFrameBuffer& b, uint32_t w, uint32_t h, uint32_t i) {
  CHECK(b.allocate({w, h, PixelFormat::Bgra8888}));
  uint32_t* p = reinterpret_cast<uint32_t*>(b.plane(0).data());
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) p[y * w + x] = 0xFF000000 | ((10 * i + 5) << 16) | (80 << 8) | (40 + (x / 8 + y / 8) % 2 * 20);
}

static int red_of_picture(const graphics::Surface* s) { return s ? (int)((s->row(s->height / 2)[s->width / 2] >> 16) & 0xFF) : -1; }

// Builds a Motion JPEG clip of `frames` frames at 10 fps, optionally with 8 kHz mono audio (a ramp).
static bool make_clip(TestAllocator& a, AviWriter& writer, uint32_t frames, bool with_audio, VideoCodec codec = VideoCodec::Mjpeg) {
  const uint32_t w = 32, h = 24;
  audio::StreamFormat audio_format{8000, 1, audio::SampleType::U8};
  if (!writer.begin(codec, w, h, 10, 1, PixelFormat::I420, with_audio ? &audio_format : nullptr)) return false;
  SoftwareVideoCodec device(a);
  EncoderConfig ec;
  ec.codec = codec; ec.width = w; ec.height = h; ec.input = codec == VideoCodec::Raw ? PixelFormat::I420 : PixelFormat::Bgra8888; ec.quality = 90;
  UniquePointer<IVideoEncoder> enc = device.create_encoder(ec);
  if (!enc) return false;
  for (uint32_t i = 0; i < frames; ++i) {
    VideoFrameBuffer bgra(a), yuv(a);
    make_frame(bgra, w, h, i);
    VideoFrame frame = bgra.frame(i * 100000);
    if (codec == VideoCodec::Raw) {
      if (!yuv.allocate({w, h, PixelFormat::I420}) || !convert_frame(bgra.frame(), yuv)) return false;
      frame = yuv.frame(i * 100000);
    }
    VideoPacket packet;
    if (enc->submit(frame) != SubmitResult::Accepted || !enc->receive(&packet) || !writer.add_frame(packet.data, true)) return false;
    if (with_audio) {
      uint8_t samples[800];  // 100 ms
      for (uint32_t k = 0; k < 800; ++k) samples[k] = (uint8_t)(128 + (k % 64) - 32);
      if (!writer.add_audio(ConstByteView(samples, 800))) return false;
    }
  }
  return writer.finish();
}

static void test_avi_roundtrip() {
  TestAllocator a;
  {
    AviWriter writer(a);
    CHECK(make_clip(a, writer, 7, true));
    AviFile avi(a);
    CHECK(avi.open(writer.file()));
    CHECK(avi.video().present && avi.video().supported && avi.video().codec == VideoCodec::Mjpeg);
    CHECK(avi.video().width == 32 && avi.video().height == 24 && avi.video().rate == 10 && avi.video().scale == 1 && avi.frame_count() == 7);
    CHECK(avi.audio().present && avi.audio().format.sample_rate == 8000 && avi.audio().format.channels == 1 && avi.audio().format.type == audio::SampleType::U8);
    CHECK(avi.audio().bytes == 7 * 800 && avi.all_keyframes());
    CHECK(avi.frame_time_us(0) == 0 && avi.frame_time_us(3) == 300000 && avi.duration_us() == 700000);
    CHECK(avi.frame_at_time(-5) == 0 && avi.frame_at_time(299999) == 2 && avi.frame_at_time(300000) == 3 && avi.frame_at_time(99999999) == 6);
    VideoPacket packet;
    CHECK(avi.frame(2, &packet) && packet.pts_us == 200000 && packet.keyframe && packet.data.size() > 100 && packet.data[0] == 0xFF && packet.data[1] == 0xD8);
    CHECK(!avi.frame(7, &packet));
    uint8_t audio_out[7 * 800];
    CHECK(avi.copy_audio(ByteView(audio_out, sizeof audio_out)) == 7 * 800);
    CHECK(audio_out[0] == 96 && audio_out[799] == 127 && audio_out[800] == 96);   // the chunks, one after the other
    CHECK(avi.copy_audio(ByteView(audio_out, 1000)) == 1000);

    // No audio.
    AviWriter silent(a);
    CHECK(make_clip(a, silent, 3, false));
    AviFile avi2(a);
    CHECK(avi2.open(silent.file()) && !avi2.audio().present && avi2.frame_count() == 3);

    // Raw frames.
    AviWriter raw(a);
    CHECK(make_clip(a, raw, 4, false, VideoCodec::Raw));
    AviFile avi3(a);
    CHECK(avi3.open(raw.file()) && avi3.video().codec == VideoCodec::Raw && avi3.video().raw_format == PixelFormat::I420);
    CHECK(avi3.frame(0, &packet) && packet.data.size() == VideoFormat({32, 24, PixelFormat::I420}).frame_bytes());

    // A file cut short loses the frames it lacks; one that is not an AVI is refused.
    AviFile cut(a);
    const ConstByteView whole = writer.file();
    uint64_t chunk0_end = 0;
    for (uint64_t i = 12; i + 8 < whole.size(); ++i)
      if (memcmp(whole.data() + i, "00dc", 4) == 0) { chunk0_end = i; break; }
    CHECK(chunk0_end > 0);
    CHECK(cut.open(whole.first(whole.size() * 2 / 3)) && cut.frame_count() < 7 && cut.frame_count() >= 1);
    CHECK(!cut.open(whole.first(chunk0_end - 4)));   // not even the first frame
    CHECK(!cut.open(ConstByteView()));
    uint8_t junk[64] = {'R', 'I', 'F', 'F'};
    CHECK(!cut.open(ConstByteView(junk, 64)));
    CHECK(!AviWriter(a).begin(VideoCodec::H264, 16, 16, 30, 1));
    CHECK(!AviWriter(a).begin(VideoCodec::Mjpeg, 0, 16, 30, 1));
  }
  CHECK(a.live == 0);
}

static void test_avi_keyframes() {
  TestAllocator a;
  {
    // A raw clip relabelled "H264": only the frames the index flags are keyframes.
    AviWriter w(a);
    CHECK(w.begin(VideoCodec::Raw, 16, 16, 25, 1, PixelFormat::I420));
    uint8_t frame[16 * 16 * 3 / 2];
    memset(frame, 100, sizeof frame);
    for (int i = 0; i < 10; ++i) CHECK(w.add_frame(ConstByteView(frame, sizeof frame), i % 4 == 0));
    CHECK(w.finish());
    uint8_t* copy = static_cast<uint8_t*>(malloc(w.file().size()));
    memcpy(copy, w.file().data(), w.file().size());
    int patched = 0;
    for (uint64_t i = 0; i + 4 < w.file().size(); ++i)
      if (memcmp(copy + i, "I420", 4) == 0) { memcpy(copy + i, "H264", 4); ++patched; }
    CHECK(patched == 2);
    AviFile avi(a);
    CHECK(avi.open(ConstByteView(copy, w.file().size())));
    CHECK(avi.video().codec == VideoCodec::H264 && avi.video().supported && !avi.all_keyframes());
    VideoPacket packet;
    for (uint32_t i = 0; i < 10; ++i) CHECK(avi.frame(i, &packet) && packet.keyframe == (i % 4 == 0));
    CHECK(avi.keyframe_at_or_before(0) == 0 && avi.keyframe_at_or_before(3) == 0 && avi.keyframe_at_or_before(4) == 4 && avi.keyframe_at_or_before(9) == 8);
    CHECK(avi.keyframe_at_or_before(99) == 8);
    // An unknown fourcc: parsed, but not supported.
    for (uint64_t i = 0; i + 4 < w.file().size(); ++i)
      if (memcmp(copy + i, "H264", 4) == 0) memcpy(copy + i, "ZZZZ", 4);
    CHECK(avi.open(ConstByteView(copy, w.file().size())) && !avi.video().supported);
    free(copy);
  }
  CHECK(a.live == 0);
}

static void test_player_no_audio() {
  TestAllocator a;
  {
    NoPci pci;
    hardware::SystemVideoCodecs codecs(pci, a);
    codecs.initialize();
    FakeClock clock;
    AviWriter writer(a);
    CHECK(make_clip(a, writer, 20, false));   // 2 seconds at 10 fps
    VideoPlayer player(clock, codecs, a);
    CHECK(player.state() == VideoState::Empty && player.picture() == nullptr && !player.play());
    CHECK(player.open(writer.file()) == OpenResult::Ok);
    CHECK(player.state() == VideoState::Stopped && player.frame_width() == 32 && player.frame_height() == 24 && player.duration_us() == 2000000);
    CHECK(player.picture() && near(red_of_picture(player.picture()), 5, 6) && player.current_frame() == 0 && !player.has_audio());
    const uint32_t serial0 = player.picture_serial();

    CHECK(player.play() && player.playing());
    clock.now += 350000;
    player.update();
    CHECK(player.current_frame() == 3 && near(red_of_picture(player.picture()), 35, 6) && player.picture_serial() != serial0);
    CHECK(near((int)(player.position_us() / 1000), 350, 1));
    const uint32_t serial3 = player.picture_serial();
    clock.now += 20000;   // still frame 3
    player.update();
    CHECK(player.picture_serial() == serial3);
    // Falling behind skips frames rather than showing them all late.
    clock.now += 1000000;
    player.update();
    CHECK(player.current_frame() == 13 && player.frames_skipped() > 0 && near(red_of_picture(player.picture()), 135, 6));

    // Pause freezes the clock.
    player.pause();
    CHECK(player.state() == VideoState::Paused);
    const int64_t at = player.position_us();
    clock.now += 700000;
    player.update();
    CHECK(player.position_us() == at && player.current_frame() == 13);
    player.play();
    clock.now += 100000;
    player.update();
    CHECK(player.current_frame() == 14);

    // Seek, playing: the picture jumps and time carries on from there.
    player.seek(1500000);
    CHECK(player.current_frame() == 15 && near(red_of_picture(player.picture()), 155, 6));
    clock.now += 200000;
    player.update();
    CHECK(player.current_frame() == 17);
    // Seek while stopped shows the frame too.
    player.stop();
    CHECK(player.state() == VideoState::Stopped && player.current_frame() == 0 && player.position_us() == 0);
    player.seek(900000);
    CHECK(player.current_frame() == 9 && player.state() == VideoState::Stopped && player.position_us() == 900000);
    player.seek(-1);
    CHECK(player.current_frame() == 0);

    // Playing to the end stops on the last frame; play restarts.
    player.seek(1700000);
    player.play();
    clock.now += 600000;
    player.update();
    CHECK(player.state() == VideoState::Stopped && player.current_frame() == 19 && player.position_us() == 2000000);
    CHECK(player.play() && player.current_frame() == 0);
    clock.now += 100000;
    player.update();
    CHECK(player.current_frame() == 1);

    // Looping goes round.
    player.set_looping(true);
    clock.now += 2500000;
    player.update();
    CHECK(player.playing() && player.current_frame() == 0 && player.position_us() < 100000);

    player.close();
    CHECK(player.state() == VideoState::Empty && player.picture() == nullptr);
  }
  CHECK(a.live == 0);
}

static void test_player_formats() {
  TestAllocator a;
  {
    NoPci pci;
    hardware::SystemVideoCodecs codecs(pci, a);
    codecs.initialize();
    FakeClock clock;
    VideoPlayer player(clock, codecs, a);

    AviWriter raw(a);
    CHECK(make_clip(a, raw, 5, false, VideoCodec::Raw));
    CHECK(player.open(raw.file()) == OpenResult::Ok && near(red_of_picture(player.picture()), 5, 3));
    player.seek(300000);
    CHECK(player.current_frame() == 3 && near(red_of_picture(player.picture()), 35, 3));

    // H.264 has no decoder here: refused, and the player stays empty.
    uint8_t* copy = static_cast<uint8_t*>(malloc(raw.file().size()));
    memcpy(copy, raw.file().data(), raw.file().size());
    for (uint64_t i = 0; i + 4 < raw.file().size(); ++i)
      if (memcmp(copy + i, "I420", 4) == 0) memcpy(copy + i, "H264", 4);
    CHECK(player.open(ConstByteView(copy, raw.file().size())) == OpenResult::NoDecoder && player.state() == VideoState::Empty);
    free(copy);
    uint8_t junk[40] = {1, 2, 3};
    CHECK(player.open(ConstByteView(junk, 40)) == OpenResult::NotAvi && player.state() == VideoState::Empty);
    CHECK(!player.play());
  }
  CHECK(a.live == 0);
}

static void test_player_with_audio() {
  TestAllocator a;
  FakeDevice dev;
  audio::AudioMixer mixer(dev, a);
  CHECK(mixer.initialize());
  {
    NoPci pci;
    hardware::SystemVideoCodecs codecs(pci, a);
    codecs.initialize();
    FakeClock clock;   // never advances: the audio is the clock
    AviWriter writer(a);
    CHECK(make_clip(a, writer, 20, true));
    VideoPlayer player(clock, codecs, a, &mixer);
    CHECK(player.open(writer.file()) == OpenResult::Ok && player.has_audio());
    CHECK(player.audio()->duration() == 20 * 800 && player.audio()->sample_rate() == 8000);
    auto step = [&](int rounds) { for (int i = 0; i < rounds; ++i) { player.update(); mixer.pump(); dev.play_all(); } };

    CHECK(player.play());
    step(3);
    CHECK(player.audio()->playing());
    const int64_t position = player.position_us();
    CHECK(position > 0);   // the sound moved although the clock did not
    CHECK(player.current_frame() == (uint32_t)(position / 100000) || player.current_frame() + 1 == (uint32_t)(position / 100000) + 1);
    step(10);
    CHECK(player.position_us() > position);

    // Pause pauses the sound; seeking moves the sound.
    player.pause();
    const int64_t paused = player.position_us();
    step(5);
    CHECK(player.position_us() == paused && !player.audio()->playing());
    player.seek(1200000);
    CHECK(player.current_frame() == 12 && player.audio()->position() == 12 * 800);
    player.play();
    for (int i = 0; i < 80 && player.playing(); ++i) step(1);
    CHECK(player.state() == VideoState::Stopped && player.current_frame() == 19);

    player.close();
    mixer.pump();
    CHECK(mixer.stream_count() == 0);
  }
}

int main() {
  test_avi_roundtrip();
  test_avi_keyframes();
  test_player_no_audio();
  test_player_formats();
  test_player_with_audio();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("video player tests passed\n");
  return 0;
}
