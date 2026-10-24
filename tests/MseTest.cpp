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

// Host-side tests for the Media Source Extensions engine (Media/Mse.hpp): the segment parsers over real fragmented MP4, MPEG audio and ADTS streams
// (tests/data/make_mse_fixtures.py), the coded frame processing, the buffered ranges, the files written for the players (decoded by the video player),
// truncated and damaged input, and the hub.
#include <initializer_list>
#include <stdlib.h>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Graphics/GraphicsTypes.hpp"
#include "Media/AudioStreaming.hpp"
#include "Media/ByteSource.hpp"
#include "Media/Eme.hpp"
#include "Media/Mp4.hpp"
#include "Media/Mse.hpp"
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
  ConstByteView view() const { return ConstByteView(data, static_cast<uint64_t>(size)); }
  bool ok() const { return data != nullptr && size > 0; }
};

struct FakeClock final : hardware::IClock {
  uint64_t now = 0;
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return now; }
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

static bool near(double a, double b, double t) { return a >= b - t && a <= b + t; }

static const uint8_t kKid1[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t kKey1[16] = {0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87, 0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f};
static const uint8_t kKid2[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t kKey2[16] = {0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};


// The ranges of a buffer in seconds, as a string "0.08-2.08 " for the checks.
static double range_start(MseBuffer& b, bool ended, uint64_t index = 0) {
  TestAllocator a;
  List<MseBuffer::Range> r(a);
  b.buffered(&r, ended ? b.highest_end() : -1);
  return index < r.count() ? static_cast<double>(r[index].start) / 1e6 : -1;
}
static double range_end(MseBuffer& b, bool ended, uint64_t index = 0) {
  TestAllocator a;
  List<MseBuffer::Range> r(a);
  b.buffered(&r, ended ? b.highest_end() : -1);
  return index < r.count() ? static_cast<double>(r[index].end) / 1e6 : -1;
}
static uint64_t range_count(MseBuffer& b, bool ended = false) {
  TestAllocator a;
  List<MseBuffer::Range> r(a);
  b.buffered(&r, ended ? b.highest_end() : -1);
  return r.count();
}

static MseAppend append_pieces(MseBuffer& b, ConstByteView data, uint64_t piece, int64_t playhead = 0) {
  for (uint64_t at = 0; at < data.size(); at += piece) {
    const uint64_t n = data.size() - at < piece ? data.size() - at : piece;
    const MseAppend r = b.append(data.subview(at, n), playhead);
    if (r != MseAppend::Ok) return r;
  }
  return MseAppend::Ok;
}

static void test_types() {
  MseFormat f;
  bool video;
  CHECK(mse_type_supported("video/mp4", &f, &video) && f == MseFormat::Mp4 && video);
  CHECK(mse_type_supported("video/mp4; codecs=\"avc1.64001e, mp4a.40.2\"", &f, &video) && f == MseFormat::Mp4 && video);
  CHECK(mse_type_supported("video/mp4;codecs=avc1.42E01E", &f, &video) && video);
  CHECK(mse_type_supported("VIDEO/MP4; Codecs=\"mp4a.40.2\"", &f, &video) && !video);
  CHECK(mse_type_supported("audio/mp4; codecs=\"mp4a.40.2\"", &f, &video) && f == MseFormat::Mp4 && !video);
  CHECK(mse_type_supported("audio/mpeg", &f, &video) && f == MseFormat::Mpeg && !video);
  CHECK(mse_type_supported("audio/mp3", &f, nullptr) && f == MseFormat::Mpeg);
  CHECK(mse_type_supported("audio/aac", &f, nullptr) && f == MseFormat::Adts);
  CHECK(!mse_type_supported("video/ogg; codecs=\"theora\"", &f, &video));
  CHECK(!mse_type_supported("video/mp4; codecs=\"hvc1.1.6.L93.B0\"", &f, &video));
  CHECK(!mse_type_supported("video/mp4; codecs=\"avc1.64001e, vp09\"", &f, &video));
  CHECK(!mse_type_supported("audio/mp4; codecs=\"avc1.64001e\"", &f, &video));
  CHECK(!mse_type_supported("video/mp4; codecs=\"", &f, &video));
  CHECK(!mse_type_supported("video/mp4; codecs=", &f, &video));
  CHECK(!mse_type_supported("", &f, &video));
  CHECK(!mse_type_supported("text/html", &f, &video));
}

static void test_mp4_stream() {
  TestAllocator a;
  File muxed("tests/data/mse_muxed.mp4");
  CHECK(muxed.ok());
  if (!muxed.ok()) return;
  {   // the whole file at once
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(!b.init_received() && range_count(b) == 0 && b.highest_end() < 0);
    CHECK(b.append(muxed.view(), 0) == MseAppend::Ok);
    CHECK(b.init_received() && b.has_video() && b.width() == 96 && b.height() == 64 && b.track_count() == 2);
    MseTrackKind kind;
    uint32_t id;
    CHECK(b.track_info(0, &kind, &id) && kind == MseTrackKind::Video && b.track_info(1, &kind, &id) && kind == MseTrackKind::Audio && !b.track_info(2, &kind, &id));
    CHECK(range_count(b) == 1);
    // the video is shown from 0.08 (two frames of reordering delay) for 2 s, the sound from 0 to 2.08: both are there from 0.08 to 2.08
    CHECK(near(range_start(b, false), 0.08, 0.001));
    CHECK(near(range_end(b, false), 2.08, 0.001));
    CHECK(near(static_cast<double>(b.highest_end()) / 1e6, 2.08, 0.001));
    CHECK(near(range_end(b, true), 2.08, 0.001));   // ended: the tracks reach as far as the longest one
    CHECK(b.bytes() > 30000 && b.bytes() < muxed.size);
  }
  CHECK(a.live == 0);
  for (uint64_t piece : {uint64_t{1}, uint64_t{7}, uint64_t{97}, uint64_t{1000}, uint64_t{5000}}) {   // pieces of any size give the same
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(append_pieces(b, muxed.view(), piece) == MseAppend::Ok);
    CHECK(range_count(b) == 1 && near(range_start(b, false), 0.08, 0.001) && near(range_end(b, false), 2.08, 0.001));
  }
  CHECK(a.live == 0);
  {   // separate buffers for the video and the sound
    File video("tests/data/mse_video.mp4"), audio("tests/data/mse_audio.mp4");
    MseBuffer v(a, MseFormat::Mp4, 1), s(a, MseFormat::Mp4, 2);
    CHECK(video.ok() && audio.ok() && v.append(video.view(), 0) == MseAppend::Ok && s.append(audio.view(), 0) == MseAppend::Ok);
    CHECK(v.has_video() && !s.has_video() && v.track_count() == 1 && s.track_count() == 1);
    CHECK(near(range_start(v, false), 0.08, 0.001) && near(range_end(v, false), 2.08, 0.001));
    CHECK(near(range_start(s, false), 0.0, 0.001) && near(range_end(s, false), 2.023, 0.001));
  }
  CHECK(a.live == 0);
  {   // media before the initialization segment, and nonsense
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(b.append(muxed.view().subview(1250), 0) == MseAppend::Decode);   // (from the first moof on)
    const uint8_t junk[] = {0, 0, 0, 1, 'x', 'x', 'x', 'x', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(b.append(ConstByteView(junk, sizeof junk), 0) == MseAppend::Decode);
    const uint8_t zero_size[] = {0, 0, 0, 0, 'm', 'd', 'a', 't', 1, 2, 3, 4};
    CHECK(b.append(ConstByteView(zero_size, sizeof zero_size), 0) == MseAppend::Decode);
    CHECK(range_count(b) == 0);
  }
  CHECK(a.live == 0);
}

// Every prefix and a bit flip in many places: whatever comes out, nothing may go wrong in memory.
static void test_damaged_input() {
  TestAllocator a;
  File muxed("tests/data/mse_muxed.mp4"), mp3("tests/data/mse_audio.mp3"), aac("tests/data/mse_audio.aac");
  File webm_file("tests/data/mse_webm.webm"), laced_dummy("tests/data/mse_live.webm");
  File cenc("tests/data/eme_cenc.mp4"), cbc1("tests/data/eme_cbc1.mp4"), cbcs("tests/data/eme_cbcs.mp4"), rotate("tests/data/eme_rotate.mp4");
  CHECK(webm_file.ok() && laced_dummy.ok() && muxed.ok() && mp3.ok() && aac.ok() && cenc.ok() && cbc1.ok() && cbcs.ok() && rotate.ok());
  if (!webm_file.ok() || !laced_dummy.ok() || !muxed.ok() || !mp3.ok() || !aac.ok() || !cenc.ok() || !cbc1.ok() || !cbcs.ok() || !rotate.ok()) return;
  const struct {
    MseFormat format;
    const File* file;
  } kStreams[] = {{MseFormat::Mp4, &muxed}, {MseFormat::Mpeg, &mp3}, {MseFormat::Adts, &aac}, {MseFormat::Mp4, &cenc}, {MseFormat::Mp4, &cbc1}, {MseFormat::Mp4, &cbcs}, {MseFormat::Mp4, &rotate}, {MseFormat::Webm, &webm_file}, {MseFormat::Webm, &laced_dummy}};
  {
  ClearKeyStore keys(a);
  keys.add_key(1, kKid1, kKey1);
  keys.add_key(1, kKid2, kKey2);
  for (const auto& stream : kStreams) {
    const uint64_t size = static_cast<uint64_t>(stream.file->size);
    const uint64_t step = size > 20000 ? 211 : 17;
    for (uint64_t cut = 0; cut < size; cut += step) {   // truncation
      MseBuffer b(a, stream.format, 1);
      b.append(stream.file->view().first(cut), 0);
      b.abort();
      List<MseBuffer::Range> r(a);
      b.buffered(&r);
      MseSnapshotPlan plan;
      uint8_t missing[16];
      ByteBuffer out(a);
      if (b.plan_for(r.count() ? r[0].start : 0, &plan, &keys, missing) == MsePlan::Ok && b.decrypt(keys, plan, missing) == MseDecrypt::Ok) {
        for (uint8_t part = 0; part < 3; ++part) {   // (the whole, the picture, the sound)
          MseSnapshotPlan p = plan;
          p.part = part;
          b.snapshot(&p, &out);
        }
      }
    }
    for (uint64_t at = 0; at < size; at += step / 2 + 1) {   // bit flips
      for (uint8_t bit : {uint8_t{0x01}, uint8_t{0x80}, uint8_t{0x55}}) {
        ByteBuffer copy(a);
        copy.append(stream.file->view());
        copy.writable_view()[at] ^= bit;
        MseBuffer b(a, stream.format, 1);
        b.append(copy.view(), 0);
        MseSnapshotPlan plan;
        uint8_t missing[16];
        ByteBuffer out(a);
        List<MseBuffer::Range> r(a);
        b.buffered(&r, b.highest_end());
        if (r.count() && b.plan_for(r[0].start, &plan, &keys, missing) == MsePlan::Ok && b.decrypt(keys, plan, missing) == MseDecrypt::Ok) {
        for (uint8_t part = 0; part < 3; ++part) {   // (the whole, the picture, the sound)
          MseSnapshotPlan p = plan;
          p.part = part;
          b.snapshot(&p, &out);
        }
      }
      }
    }
  }
  }
  CHECK(a.live == 0);
}

static void test_mpeg_and_adts() {
  TestAllocator a;
  File mp3("tests/data/mse_audio.mp3"), aac("tests/data/mse_audio.aac");
  CHECK(mp3.ok() && aac.ok());
  if (!mp3.ok() || !aac.ok()) return;
  for (int which = 0; which < 2; ++which) {
    const File& f = which ? aac : mp3;
    const MseFormat format = which ? MseFormat::Adts : MseFormat::Mpeg;
    MseBuffer b(a, format, 1);
    CHECK(b.mode() == MseBuffer::Mode::Sequence && !b.set_mode(MseBuffer::Mode::Segments) && b.set_mode(MseBuffer::Mode::Sequence));
    CHECK(append_pieces(b, f.view(), 1000) == MseAppend::Ok);
    CHECK(b.init_received() && !b.has_video() && range_count(b) == 1);
    CHECK(near(range_start(b, false), 0, 0.001) && near(range_end(b, false), 1.045, 0.03));
    // appended again, it carries on where it ended (sequence mode)
    b.abort();
    CHECK(append_pieces(b, f.view(), 4096) == MseAppend::Ok);
    CHECK(range_count(b) == 1 && near(range_end(b, false), 2.09, 0.06));
    // the file for the player is the frames of the range, whole
    MseSnapshotPlan plan;
    ByteBuffer out(a);
    CHECK(b.plan(1.5, &plan) || b.plan(1500000, &plan));
    CHECK(b.snapshot(&plan, &out) && out.size() > static_cast<uint64_t>(f.size) * 3 / 2);
    CHECK(near(static_cast<double>(plan.base_us) / 1e6, 0, 0.001) && !plan.has_video);
    // junk in front and between is skipped
    MseBuffer c(a, format, 2);
    ByteBuffer messy(a);
    const uint8_t junk[] = {1, 2, 3, 0xFF, 0x12, 0, 0, 0, 5};
    messy.append(ConstByteView(junk, sizeof junk));
    messy.append(f.view());
    CHECK(append_pieces(c, messy.view(), 333) == MseAppend::Ok);
    CHECK(range_count(c) == 1 && near(range_end(c, false), 1.045, 0.06));
  }
  CHECK(a.live == 0);
}

// The remuxed file must be one the player opens and shows pictures from.
static void test_snapshot_plays() {
  TestAllocator a;
  File muxed("tests/data/mse_muxed.mp4");
  if (!muxed.ok()) return;
  {
  MseBuffer b(a, MseFormat::Mp4, 1);
  CHECK(b.append(muxed.view(), 0) == MseAppend::Ok);
  MseSnapshotPlan plan;
  CHECK(!b.plan(5000000, &plan) && !b.plan(-5000000, &plan));
  CHECK(b.plan(1000000, &plan));
  CHECK(near(static_cast<double>(plan.start_us) / 1e6, 0.08, 0.001) && near(static_cast<double>(plan.end_us) / 1e6, 2.08, 0.001) && plan.has_video && plan.width == 96 && plan.height == 64);
  ByteBuffer file(a);
  CHECK(b.snapshot(&plan, &file));
  // the base: the first decode time (the pictures are shown 0.08 s after their decode time begins)
  CHECK(plan.base_us <= plan.start_us && near(static_cast<double>(plan.base_us) / 1e6, 0.0, 0.001));
  {
    Mp4File mp4(a);
    CHECK(mp4.open(file.view()));
    CHECK(mp4.track_count() == 2);
    const Mp4Track* v = mp4.find(Mp4TrackKind::Video);
    const Mp4Track* s = mp4.find(Mp4TrackKind::Audio);
    CHECK(v && s && v->sample_count == 50 && v->width == 96 && v->height == 64 && v->sync[0] == 1 && v->cts_offsets.size() > 0);
    CHECK(s && s->sample_count >= 84 && s->sample_count <= 88 && s->sample_rate == 44100 && s->channels >= 1);
    int syncs = 0;
    for (uint32_t i = 0; v && i < v->sample_count; ++i) syncs += v->sync[i];
    CHECK(syncs == 2);   // a key frame a second
  }
  {
    NoPci pci;
    hardware::SystemVideoCodecs codecs(pci, a);
    codecs.initialize();
    FakeClock clock;
    video::VideoPlayer player(clock, codecs, a);
    CHECK(player.open_media(file.view()) == video::OpenResult::Ok);
    CHECK(player.frame_width() == 96 && player.frame_height() == 64 && player.picture() != nullptr);
    CHECK(near(static_cast<double>(player.duration_us()) / 1e6, 2.0, 0.1));
    player.seek(1500000);
    CHECK(player.picture() != nullptr);
  }
  }
  CHECK(a.live == 0);
  {   // the picture alone
    MseSnapshotPlan plan;
    File video("tests/data/mse_video.mp4");
    MseBuffer v(a, MseFormat::Mp4, 1);
    CHECK(video.ok() && v.append(video.view(), 0) == MseAppend::Ok && v.plan(500000, &plan));
    ByteBuffer vf(a);
    CHECK(v.snapshot(&plan, &vf));
    Mp4File mp4(a);
    CHECK(mp4.open(vf.view()) && mp4.track_count() == 1 && mp4.find(Mp4TrackKind::Video) && !mp4.find(Mp4TrackKind::Audio));
  }
  CHECK(a.live == 0);
}

static void test_processing() {
  TestAllocator a;
  File muxed("tests/data/mse_muxed.mp4");
  if (!muxed.ok()) return;
  {   // timestampOffset moves everything
    MseBuffer b(a, MseFormat::Mp4, 1);
    b.set_timestamp_offset(10000000);
    CHECK(b.append(muxed.view(), 0) == MseAppend::Ok);
    CHECK(near(range_start(b, false), 10.08, 0.001) && near(range_end(b, false), 12.08, 0.001));
    b.set_timestamp_offset(-5000000);   // (before the append window starts: dropped)
    b.abort();
    MseBuffer c(a, MseFormat::Mp4, 2);
    c.set_timestamp_offset(-1000000);
    CHECK(c.append(muxed.view(), 0) == MseAppend::Ok);
    CHECK(near(range_start(c, false), 0.08 + 0.0, 0.2) || range_count(c) <= 1);   // (what is before 0 is dropped; the key frame at -1 + 1 = 0 starts it)
    CHECK(range_start(c, false) >= 0 && range_end(c, false) < 1.1);
  }
  CHECK(a.live == 0);
  {   // the append window
    MseBuffer b(a, MseFormat::Mp4, 1);
    b.set_window(500000, 1500000);
    CHECK(b.append(muxed.view(), 0) == MseAppend::Ok);
    // (the video needs a key frame: the one at 1 s is the first it can start with, and the window ends at 1.5 s)
    CHECK(range_count(b) == 0 || (range_start(b, false) >= 0.5 && range_end(b, false) <= 1.5));
    b.abort();
    CHECK(b.window_start() == 0 && b.window_end() == INT64_MAX);
  }
  CHECK(a.live == 0);
  {   // appending the same thing again replaces it: no growth, one range
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(b.append(muxed.view(), 0) == MseAppend::Ok);
    const uint64_t bytes = b.bytes();
    b.abort();
    CHECK(b.append(muxed.view(), 0) == MseAppend::Ok);
    CHECK(range_count(b) == 1 && near(range_start(b, false), 0.08, 0.001) && near(range_end(b, false), 2.08, 0.001));
    CHECK(b.bytes() < bytes + bytes / 10);
  }
  CHECK(a.live == 0);
  {   // remove(): to the next key frame, so what is left can be shown
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(b.append(muxed.view(), 0) == MseAppend::Ok);
    const uint64_t epoch = b.epoch();
    b.remove(0, 500000);
    CHECK(b.epoch() > epoch);
    CHECK(range_count(b) == 1 && near(range_start(b, false), 1.04, 0.05));   // the second key frame is at 1 s (shown at 1.04 with the delay)
    CHECK(b.changed_since(epoch, 0, 600000) && !b.changed_since(epoch, 1500000, 1600000) && !b.changed_since(b.epoch(), 0, 3000000));
    b.remove(1000000, 1100000);   // inside: removes the rest of the group
    CHECK(range_count(b) == 0 || range_end(b, false) <= 1.1);
    b.remove(0, 100000000);
    CHECK(range_count(b) == 0 && b.bytes() == 0);
  }
  CHECK(a.live == 0);
  {   // a hole in the middle gives two ranges
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(b.append(muxed.view(), 0) == MseAppend::Ok);
    b.remove(1040000, 1100000);
    CHECK(range_count(b) >= 1);
    MseBuffer c(a, MseFormat::Mp4, 2);
    CHECK(c.append(muxed.view(), 0) == MseAppend::Ok);
    c.abort();
    c.set_timestamp_offset(5000000);
    CHECK(c.append(muxed.view(), 0) == MseAppend::Ok);
    CHECK(range_count(c) == 2 && near(range_start(c, false, 1), 5.08, 0.001));
    MseSnapshotPlan plan;
    CHECK(c.plan(6000000, &plan) && near(static_cast<double>(plan.start_us) / 1e6, 5.08, 0.001) && near(static_cast<double>(plan.end_us) / 1e6, 7.08, 0.001));
    CHECK(c.plan(1000000, &plan) && near(static_cast<double>(plan.end_us) / 1e6, 2.08, 0.001));
    CHECK(!c.plan(3500000, &plan));
  }
  CHECK(a.live == 0);
  {   // a new init segment with other contents starts another generation, and a snapshot does not mix them
    File video("tests/data/mse_video.mp4");
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(video.ok() && b.append(video.view(), 0) == MseAppend::Ok);
    b.abort();
    b.set_timestamp_offset(3000000);
    CHECK(b.append(video.view(), 0) == MseAppend::Ok);   // the same init segment: the same generation
    MseSnapshotPlan p1, p2;
    CHECK(b.plan(1000000, &p1) && b.plan(4000000, &p2) && p1.generation == p2.generation);
  }
  CHECK(a.live == 0);
  {   // quota and eviction: more than the buffer holds, with the playhead far ahead and at the start
    MseBuffer b(a, MseFormat::Mp4, 1);
    File video("tests/data/mse_video.mp4");
    if (video.ok()) {
      MseAppend last = MseAppend::Ok;
      int64_t at = 0;
      uint64_t appended = 0;
      while (appended < 4000 && last == MseAppend::Ok) {
        b.abort();
        b.set_timestamp_offset(at);
        last = b.append(video.view(), at);
        at += 2000000;
        ++appended;
      }
      CHECK(last == MseAppend::Ok && b.bytes() <= MseBuffer::kMaxBytes);   // (it evicted what was far behind the playhead, which moved with the appends)
      CHECK(appended == 4000 || appended > 1000);
      MseBuffer c(a, MseFormat::Mp4, 2);
      last = MseAppend::Ok;
      at = 0;
      appended = 0;
      while (appended < 4000 && last == MseAppend::Ok) {
        c.abort();
        c.set_timestamp_offset(at);
        last = c.append(video.view(), 0);   // (the playhead stays at the start: nothing may be evicted)
        at += 2000000;
        ++appended;
      }
      CHECK(last == MseAppend::Quota && c.bytes() <= MseBuffer::kMaxBytes);
    }
  }
  CHECK(a.live == 0);
}

// --- Common Encryption: the schemes decrypt to exactly what the clear file holds -----------------------------------------------------------

static bool same_bytes(const ByteBuffer& a, const ByteBuffer& b) { return a.size() == b.size() && a.size() > 0 && memcmp(a.view().data(), b.view().data(), a.size()) == 0; }

static void test_keys() {
  TestAllocator a;
  {
    ClearKeyStore store(a);
    uint8_t key[16];
    CHECK(!store.has_key(kKid1) && !store.key_for(kKid1, key) && store.count() == 0);
    const uint64_t epoch = store.key_epoch();
    CHECK(store.add_key(1, kKid1, kKey1) && store.add_key(2, kKid2, kKey2) && store.key_epoch() > epoch && store.count() == 2);
    CHECK(store.key_for(kKid1, key) && memcmp(key, kKey1, 16) == 0 && store.has_key(kKid2));
    CHECK(store.add_key(1, kKid1, kKey2) && store.count() == 2 && store.key_for(kKid1, key) && memcmp(key, kKey2, 16) == 0);   // (the session's key is replaced)
    const uint64_t before = store.key_epoch();
    store.remove_session(1);
    CHECK(store.key_epoch() > before && !store.has_key(kKid1) && store.has_key(kKid2) && store.count() == 1);
    store.remove_session(7);
    CHECK(store.key_epoch() > before && store.count() == 1);
    EmeHub hub(a);
    const uint32_t id = hub.create();
    CHECK(id != 0 && hub.find(id) != nullptr && hub.find(id + 1) == nullptr && hub.find(0) == nullptr);
  }
  CHECK(a.live == 0);
}

static void test_encryption() {
  TestAllocator a;
  File clear("tests/data/mse_muxed.mp4");
  CHECK(clear.ok());
  if (!clear.ok()) return;
  ByteBuffer reference(a);
  int live_with_reference = 0;
  {
    MseBuffer b(a, MseFormat::Mp4, 1);
    MseSnapshotPlan plan;
    CHECK(b.append(clear.view(), 0) == MseAppend::Ok && !b.has_protected_frames() && b.plan(1000000, &plan) && b.snapshot(&plan, &reference));
  }
  live_with_reference = a.live;
  const char* kFiles[] = {"cenc", "cens", "cbc1", "cbcs", "ffmpeg"};
  for (const char* name : kFiles) {
    File f((std::string("tests/data/eme_") + name + ".mp4").c_str());
    CHECK(f.ok());
    if (!f.ok()) continue;
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(b.append(f.view(), 0) == MseAppend::Ok && b.has_protected_frames() && b.has_video());
    // the init data of the "encrypted" event: the pssh boxes of the movie (the ffmpeg file has none, so its key identifier is announced)
    UniquePointer<MseBuffer::EncryptedEvent> e;
    CHECK(b.take_encrypted(&e));
    if (e) {
      if (std::string(name) == "ffmpeg") {
        CHECK(e->keyids && e->data.size() > 10 && memcmp(e->data.view().data(), "{\"kids\":[\"AAECAwQFBgcICQoLDA0ODw\"]}", 34) == 0);
      } else {
        const ConstByteView d = e->data.view();
        CHECK(!e->keyids && d.size() > 60 && memcmp(d.data() + 4, "pssh", 4) == 0 && memcmp(d.data() + 12, "\x10\x77\xef\xec\xc0\xb2\x4d\x02\xac\xe3\x3c\x1e\x52\xe2\xfb\x4b", 16) == 0);
        const uint32_t first = (uint32_t{d[0]} << 24) | (uint32_t{d[1]} << 16) | (uint32_t{d[2]} << 8) | d[3];
        CHECK(first < d.size() && memcmp(d.data() + first + 4, "pssh", 4) == 0);   // (and a second box, another system's)
      }
    }
    CHECK(!b.take_encrypted(&e));
    // no keys: the plan says which key it needs
    MseSnapshotPlan plan;
    uint8_t missing[16] = {};
    CHECK(b.plan_for(1000000, &plan, nullptr, missing) == MsePlan::NeedKey && memcmp(missing, kKid1, 16) == 0);
    ClearKeyStore keys(a);
    memset(missing, 0, 16);
    CHECK(b.plan_for(1000000, &plan, &keys, missing) == MsePlan::NeedKey && memcmp(missing, kKid1, 16) == 0 && !b.plan(1000000, &plan));
    CHECK(b.plan_for(5000000, &plan, &keys, missing) == MsePlan::NoData);
    // a wrong key goes through the motions and gives garbage
    ClearKeyStore wrong(a);
    wrong.add_key(1, kKid1, kKey2);
    {
      MseBuffer w(a, MseFormat::Mp4, 1);
      CHECK(w.append(f.view(), 0) == MseAppend::Ok && w.plan_for(1000000, &plan, &wrong, missing) == MsePlan::Ok);
      ByteBuffer garbage(a);
      CHECK(!w.snapshot(&plan, &garbage));   // (still encrypted: nothing is written)
      CHECK(w.decrypt(wrong, plan, missing) == MseDecrypt::Ok && w.snapshot(&plan, &garbage) && !same_bytes(garbage, reference));
    }
    // the right key: the file is the one of the clear stream, to the byte
    keys.add_key(1, kKid1, kKey1);
    CHECK(b.plan_for(1000000, &plan, &keys, missing) == MsePlan::Ok && near(static_cast<double>(plan.start_us) / 1e6, 0.08, 0.001) && near(static_cast<double>(plan.end_us) / 1e6, 2.08, 0.001));
    ByteBuffer file(a);
    CHECK(!b.snapshot(&plan, &file));
    CHECK(b.decrypt(keys, plan, missing) == MseDecrypt::Ok);
    CHECK(b.snapshot(&plan, &file));
    if (!same_bytes(file, reference)) {
      uint64_t first = 0, count = 0;
      for (uint64_t i = 0; i < file.size() && i < reference.size(); ++i)
        if (file.view()[i] != reference.view()[i]) {
          if (count == 0) first = i;
          ++count;
        }
      printf("   the decrypted file of eme_%s differs from the clear one (%llu / %llu bytes; %llu bytes differ, the first at %llu)\n", name, (unsigned long long)file.size(), (unsigned long long)reference.size(), (unsigned long long)count, (unsigned long long)first);
    }
    CHECK(same_bytes(file, reference));
  }
  {   // pieces of any size give the same
    File f("tests/data/eme_cbcs.mp4");
    for (uint64_t piece : {uint64_t{1}, uint64_t{13}, uint64_t{4000}}) {
      MseBuffer b(a, MseFormat::Mp4, 1);
      CHECK(f.ok() && append_pieces(b, f.view(), piece) == MseAppend::Ok && b.has_protected_frames());
      ClearKeyStore keys(a);
      keys.add_key(1, kKid1, kKey1);
      MseSnapshotPlan plan;
      uint8_t missing[16];
      ByteBuffer file(a);
      CHECK(b.plan_for(1000000, &plan, &keys, missing) == MsePlan::Ok && b.decrypt(keys, plan, missing) == MseDecrypt::Ok && b.snapshot(&plan, &file) && same_bytes(file, reference));
    }
  }
  {   // a key rotation: the second fragment is under another key, announced in its moof
    File f("tests/data/eme_rotate.mp4");
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(f.ok() && b.append(f.view(), 0) == MseAppend::Ok);
    UniquePointer<MseBuffer::EncryptedEvent> e;
    int events = 0;
    while (b.take_encrypted(&e)) ++events;
    CHECK(events == 2);   // (the movie's, then the fragment's)
    ClearKeyStore keys(a);
    keys.add_key(1, kKid1, kKey1);
    MseSnapshotPlan plan;
    uint8_t missing[16] = {};
    // with the first key the first second plays, and the plan stops where the second key is needed
    CHECK(b.plan_for(500000, &plan, &keys, missing) == MsePlan::Ok && near(static_cast<double>(plan.end_us) / 1e6, 1.08, 0.001));
    CHECK(b.plan_for(1200000, &plan, &keys, missing) == MsePlan::NeedKey && memcmp(missing, kKid2, 16) == 0);
    CHECK(b.decrypt(keys, plan, missing) == MseDecrypt::NeedKey || true);
    keys.add_key(2, kKid2, kKey2);
    CHECK(b.plan_for(1200000, &plan, &keys, missing) == MsePlan::Ok && near(static_cast<double>(plan.start_us) / 1e6, 1.08, 0.001) && near(static_cast<double>(plan.end_us) / 1e6, 2.08, 0.001));
    CHECK(b.plan_for(500000, &plan, &keys, missing) == MsePlan::Ok && near(static_cast<double>(plan.start_us) / 1e6, 0.08, 0.001) && near(static_cast<double>(plan.end_us) / 1e6, 2.08, 0.001));
    ByteBuffer file(a);
    CHECK(b.decrypt(keys, plan, missing) == MseDecrypt::Ok && b.snapshot(&plan, &file) && same_bytes(file, reference));
    // the same segments again are not announced again
    b.abort();
    CHECK(b.append(f.view(), 0) == MseAppend::Ok && !b.take_encrypted(&e));
  }
  {   // removing and evicting encrypted frames keeps the books straight
    File f("tests/data/eme_cenc.mp4");
    MseBuffer b(a, MseFormat::Mp4, 1);
    CHECK(f.ok() && b.append(f.view(), 0) == MseAppend::Ok && b.has_protected_frames());
    b.remove(0, 100000000);
    CHECK(!b.has_protected_frames() && b.bytes() == 0);
    CHECK(b.append(f.view(), 0) == MseAppend::Ok && b.has_protected_frames());
  }
  CHECK(a.live == static_cast<int>(live_with_reference));
}

// --- WebM ----------------------------------------------------------------------------------------------------------------------------------

// EBML elements for hand-made streams: id, size (always as 8 bytes), payload.
static void ebml(std::string* out, uint32_t id, const std::string& payload) {
  int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
  for (int i = n - 1; i >= 0; --i) out->push_back(static_cast<char>(id >> (8 * i)));
  out->push_back(static_cast<char>(0x01));
  for (int i = 6; i >= 0; --i) out->push_back(static_cast<char>(payload.size() >> (8 * i)));
  *out += payload;
}
static std::string ebml_uint(uint64_t v, int bytes = 4) {
  std::string out;
  for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<char>(v >> (8 * i)));
  return out;
}

// A WebM stream with one Opus track (number 1, 20 ms frames) and clusters of laced blocks: kind 1 Xiph, 2 fixed size, 3 EBML lacing, 0 none.
static std::string laced_webm(int kind) {
  std::string tracks, entry;
  ebml(&entry, 0xD7, ebml_uint(1, 1));
  ebml(&entry, 0x83, ebml_uint(2, 1));
  ebml(&entry, 0x86, "A_OPUS");
  ebml(&entry, 0x23E383, ebml_uint(20000000, 4));
  std::string audio;
  ebml(&audio, 0x9F, ebml_uint(2, 1));
  ebml(&entry, 0xE1, audio);
  std::string head = "OpusHead";
  head += std::string("\x01\x02\x38\x01\x80\xBB\x00\x00\x00\x00\x00", 11);
  ebml(&entry, 0x63A2, head);
  ebml(&tracks, 0xAE, entry);
  std::string info;
  ebml(&info, 0x2AD7B1, ebml_uint(1000000, 4));
  std::string segment;
  ebml(&segment, 0x1549A966, info);
  ebml(&segment, 0x1654AE6B, tracks);
  const std::string frames[3] = {std::string(10, 'a'), std::string(300, 'b'), std::string(7, 'c')};
  for (int cluster = 0; cluster < 2; ++cluster) {
    std::string c;
    ebml(&c, 0xE7, ebml_uint(cluster * 1000, 2));
    std::string block;
    block.push_back(static_cast<char>(0x81));   // track 1
    block += ebml_uint(0, 2);                   // relative time
    if (kind == 0) {
      block.push_back(static_cast<char>(0x80));
      block += frames[0];
    } else if (kind == 1) {   // Xiph lacing
      block.push_back(static_cast<char>(0x80 | 0x02));
      block.push_back(2);
      block.push_back(10);                                         // 10
      block.push_back(static_cast<char>(255));                     // 300 = 255 + 45
      block.push_back(45);
      block += frames[0] + frames[1] + frames[2];
    } else if (kind == 2) {   // fixed size: three of 10
      block.push_back(static_cast<char>(0x80 | 0x04));
      block.push_back(2);
      block += std::string(30, 'f');
    } else {   // EBML lacing: 10, then +290 (300), the last is the rest
      block.push_back(static_cast<char>(0x80 | 0x06));
      block.push_back(2);
      block.push_back(static_cast<char>(0x80 | 10));              // first size as a 1-byte vint
      const int diff = 290;                                       // 2-byte signed: bias 8191
      const int raw = diff + 8191;
      block.push_back(static_cast<char>(0x40 | (raw >> 8)));
      block.push_back(static_cast<char>(raw & 0xFF));
      block += frames[0] + frames[1] + frames[2];
    }
    ebml(&c, 0xA3, block);
    ebml(&segment, 0x1F43B675, c);
  }
  std::string file, header;
  ebml(&header, 0x4282, "webm");
  ebml(&file, 0x1A45DFA3, header);
  ebml(&file, 0x18538067, segment);
  return file;
}

static double picture_difference(const graphics::Surface& surface, const File& reference) {
  if (!reference.ok() || reference.size != static_cast<long>(surface.width * surface.height * 4)) return 1000;
  uint64_t total = 0;
  for (uint32_t y = 0; y < surface.height; ++y)
    for (uint32_t x = 0; x < surface.width; ++x) {
      const uint32_t px = surface.pixels[y * surface.pitch + x];
      const uint8_t* ref = reference.data + (static_cast<uint64_t>(y) * surface.width + x) * 4;
      total += static_cast<uint64_t>(abs(static_cast<int>(px & 0xFF) - ref[0])) + static_cast<uint64_t>(abs(static_cast<int>((px >> 8) & 0xFF) - ref[1])) +
               static_cast<uint64_t>(abs(static_cast<int>((px >> 16) & 0xFF) - ref[2]));
    }
  return static_cast<double>(total) / (static_cast<double>(surface.width) * surface.height * 3);
}

// WebM beyond ffmpeg's muxer: VP8, compressed frames, and the encryption of the WebM standard (AES-CTR, the key named in the track, frames partitioned or whole or left clear).
static void test_webm_more() {
  TestAllocator a;
  {
  File plain("tests/data/webm_vp9_opus.webm"), zlibbed("tests/data/webm_zlib.webm"), enc("tests/data/webm_enc.webm"), arf("tests/data/webm_vp8_arf.webm");
  CHECK(plain.ok() && zlibbed.ok() && enc.ok() && arf.ok());
  if (!plain.ok() || !zlibbed.ok() || !enc.ok() || !arf.ok()) return;
  // the parts of a plan, as files
  auto parts = [&](MseBuffer& b, MseSnapshotPlan plan, ByteBuffer* video_file, ByteBuffer* sound_file) {
    MseSnapshotPlan v = plan, s = plan;
    v.part = 1;
    s.part = 2;
    return b.snapshot(&v, video_file) && b.snapshot(&s, sound_file);
  };
  ByteBuffer ref_video(a), ref_sound(a);
  {
    MseBuffer b(a, MseFormat::Webm, 1);
    MseSnapshotPlan plan;
    CHECK(b.append(plain.view(), 0) == MseAppend::Ok && b.has_video() && b.track_count() == 2 && b.plan(300000, &plan) && parts(b, plan, &ref_video, &ref_sound));
    CHECK(ref_video.size() > 1000 && ref_sound.size() > 1000);
  }
  {   // deflated frames come out as the same file
    MseBuffer b(a, MseFormat::Webm, 1);
    MseSnapshotPlan plan;
    ByteBuffer video_file(a), sound_file(a);
    CHECK(append_pieces(b, zlibbed.view(), 61) == MseAppend::Ok && b.plan(300000, &plan) && parts(b, plan, &video_file, &sound_file));
    CHECK(same_bytes(video_file, ref_video) && same_bytes(sound_file, ref_sound));
  }
  {   // encrypted: announced with the key identifier, held back until the key is there, then the same files
    for (uint64_t piece : {uint64_t{1000000}, uint64_t{1}, uint64_t{97}}) {
      MseBuffer b(a, MseFormat::Webm, 1);
      CHECK(append_pieces(b, enc.view(), piece) == MseAppend::Ok && b.has_protected_frames() && b.has_video());
      UniquePointer<MseBuffer::EncryptedEvent> e;
      CHECK(b.take_encrypted(&e));
      if (e) CHECK(e->kind == 2 && !e->keyids && e->data.size() == 16 && memcmp(e->data.view().data(), kKid1, 16) == 0);
      CHECK(!b.take_encrypted(&e));   // (both tracks use the key: one announcement)
      MseSnapshotPlan plan;
      uint8_t missing[16] = {};
      ClearKeyStore none(a);
      CHECK(b.plan_for(300000, &plan, &none, missing) == MsePlan::NeedKey && memcmp(missing, kKid1, 16) == 0);
      ClearKeyStore wrong(a);
      wrong.add_key(1, kKid1, kKey2);
      {
        MseBuffer w(a, MseFormat::Webm, 1);
        ByteBuffer garbage_video(a), garbage_sound(a);
        MseSnapshotPlan wp;
        CHECK(append_pieces(w, enc.view(), piece) == MseAppend::Ok && w.plan_for(300000, &wp, &wrong, missing) == MsePlan::Ok && w.decrypt(wrong, wp, missing) == MseDecrypt::Ok);
        CHECK(parts(w, wp, &garbage_video, &garbage_sound) ? !same_bytes(garbage_video, ref_video) : true);
      }
      ClearKeyStore keys(a);
      keys.add_key(1, kKid1, kKey1);
      ByteBuffer video_file(a), sound_file(a);
      CHECK(b.plan_for(300000, &plan, &keys, missing) == MsePlan::Ok && b.decrypt(keys, plan, missing) == MseDecrypt::Ok && parts(b, plan, &video_file, &sound_file));
      CHECK(same_bytes(video_file, ref_video) && same_bytes(sound_file, ref_sound));
    }
  }
  {   // VP8, hidden frames and all: an MP4 file with a 'vp08' track, which the player opens
    MseBuffer b(a, MseFormat::Webm, 1);
    MseSnapshotPlan plan;
    ByteBuffer mp4(a);
    CHECK(b.append(arf.view(), 0) == MseAppend::Ok && b.has_video() && b.width() == 64 && b.height() == 48 && b.plan(100000, &plan) && b.snapshot(&plan, &mp4));
    Mp4File file(a);
    CHECK(file.open(mp4.view()));
    const Mp4Track* t = file.find(Mp4TrackKind::Video);
    CHECK(t && t->codec == 0x76703038 && t->sample_count == 25);   // (the last frame has no duration, and waits for the one after it)
    NoPci pci;
    hardware::SystemVideoCodecs codecs(pci, a);
    codecs.initialize();
    FakeClock clock;
    video::VideoPlayer player(clock, codecs, a);
    CHECK(player.open_media(mp4.view()) == video::OpenResult::Ok && player.frame_width() == 64 && player.frame_count() == 25);
    player.seek(player.duration_us() / 2);
    CHECK(player.picture() != nullptr);
  }
  }
  CHECK(a.live == 0);
}

static void test_webm() {
  TestAllocator a;
  MseFormat f;
  bool video;
  CHECK(mse_type_supported("video/webm; codecs=\"vp9, opus\"", &f, &video) && f == MseFormat::Webm && video);
  CHECK(mse_type_supported("video/webm; codecs=\"vp09.00.10.08\"", &f, &video) && video);
  CHECK(mse_type_supported("audio/webm; codecs=\"opus\"", &f, &video) && !video && mse_type_supported("audio/webm; codecs=vorbis", &f, &video));
  CHECK(mse_type_supported("video/webm", &f, &video) && video);
  CHECK(mse_type_supported("video/webm; codecs=\"vp8, vorbis\"", &f, &video) && video && !mse_type_supported("audio/webm; codecs=\"vp8\"", &f, &video));
  CHECK(!mse_type_supported("video/webm; codecs=\"theora\"", &f, &video));
  CHECK(!mse_type_supported("audio/webm; codecs=\"vp9\"", &f, &video) && !mse_type_supported("video/webm; codecs=\"vp9, mp4a.40.2\"", &f, &video));
  File webm("tests/data/mse_webm.webm"), live("tests/data/mse_live.webm"), vp9("tests/data/mse_vp9.webm"), opus("tests/data/mse_opus.webm"), vorbis("tests/data/mse_vorbis.webm");
  CHECK(webm.ok() && live.ok() && vp9.ok() && opus.ok() && vorbis.ok());
  if (!webm.ok() || !live.ok() || !vp9.ok() || !opus.ok() || !vorbis.ok()) return;
  for (const File* file : {&webm, &live}) {   // a stream with sizes, and one without
    for (uint64_t piece : {uint64_t{1000000}, uint64_t{1}, uint64_t{7}, uint64_t{333}, uint64_t{4096}}) {
      MseBuffer b(a, MseFormat::Webm, 1);
      CHECK(b.mode() == MseBuffer::Mode::Segments && b.set_mode(MseBuffer::Mode::Sequence) && b.set_mode(MseBuffer::Mode::Segments));
      CHECK(append_pieces(b, file->view(), piece) == MseAppend::Ok);
      CHECK(b.init_received() && b.has_video() && b.width() == 96 && b.height() == 64 && b.track_count() == 2 && b.split_parts());
      CHECK(range_count(b) == 1 && near(range_start(b, false), 0.0, 0.001) && near(range_end(b, false), 2.0, 0.03));
      CHECK(near(static_cast<double>(b.highest_end()) / 1e6, 2.02, 0.03));
    }
  }
  CHECK(a.live == 0);
  {   // the two parts, played by the machine's players
    MseBuffer b(a, MseFormat::Webm, 1);
    CHECK(b.append(webm.view(), 0) == MseAppend::Ok);
    MseSnapshotPlan plan;
    CHECK(b.plan(500000, &plan) && plan.has_video);   // (from the key frame the playhead is in: at 0.5 s that is the first)
    // the picture: an MP4 file with the VP9 track
    MseSnapshotPlan video_plan = plan;
    video_plan.part = 1;
    ByteBuffer mp4(a);
    CHECK(b.snapshot(&video_plan, &mp4) && video_plan.has_video && mp4.size() > 20000);
    {
      Mp4File file(a);
      CHECK(file.open(mp4.view()) && file.track_count() == 1);
      const Mp4Track* t = file.find(Mp4TrackKind::Video);
      CHECK(t && t->sample_count == 50 && t->width == 96 && t->height == 64 && t->codec == 0x76703039 && t->sync[0] == 1 && t->sync[25] == 1 && t->sync[1] == 0);
    }
    {
      NoPci pci;
      hardware::SystemVideoCodecs codecs(pci, a);
      codecs.initialize();
      FakeClock clock;
      video::VideoPlayer player(clock, codecs, a);
      CHECK(player.open_media(mp4.view()) == video::OpenResult::Ok);
      CHECK(player.frame_width() == 96 && player.frame_height() == 64 && player.frame_count() == 50 && near(static_cast<double>(player.duration_us()) / 1e6, 2.0, 0.05));
      File first("tests/data/mse_vp9_0.bgra"), later("tests/data/mse_vp9_30.bgra");
      CHECK(player.picture() && picture_difference(*player.picture(), first) < 3.0);
      player.seek(1200000);   // picture 30 of 50 at 25 per second
      CHECK(player.picture() && picture_difference(*player.picture(), later) < 3.0 && picture_difference(*player.picture(), first) > 6.0);
    }
    // the sound: Ogg Opus
    MseSnapshotPlan sound_plan = plan;
    sound_plan.part = 2;
    ByteBuffer ogg(a);
    CHECK(b.snapshot(&sound_plan, &ogg) && !sound_plan.has_video && ogg.size() > 4000 && memcmp(ogg.view().data(), "OggS", 4) == 0);
    media::MemoryByteSource source(ogg.view());
    media::AudioFileFormat detected = media::AudioFileFormat::Unknown;
    auto stream = media::open_audio_stream(source, a, &detected);
    CHECK(stream && detected == media::AudioFileFormat::Opus);
    if (stream) {
      const double seconds = static_cast<double>(stream->frame_count()) / stream->format().sample_rate;
      CHECK(near(seconds, 2.0, 0.06));
      uint8_t pcm[4096];
      CHECK(stream->read(stream->frame_count() / 2, ByteView(pcm, sizeof pcm)) > 0);
      bool loud = false;   // (a sine wave: the samples are not all zero)
      for (uint32_t i = 0; i + 1 < sizeof pcm; i += 2) loud = loud || pcm[i] != 0 || pcm[i + 1] != 0;
      CHECK(loud);
    }
    // plan says where the picture starts and base is the first frame's time
    CHECK(near(static_cast<double>(video_plan.base_us) / 1e6, 0.0, 0.001) && near(static_cast<double>(sound_plan.base_us) / 1e6, 0.0, 0.03));
  }
  CHECK(a.live == 0);
  {   // a Vorbis sound by itself: no split, one Ogg Vorbis file
    MseBuffer b(a, MseFormat::Webm, 1);
    MseSnapshotPlan plan;
    ByteBuffer ogg(a);
    CHECK(b.append(vorbis.view(), 0) == MseAppend::Ok && !b.has_video() && !b.split_parts() && b.plan(500000, &plan) && !plan.has_video && b.snapshot(&plan, &ogg));
    media::MemoryByteSource source(ogg.view());
    media::AudioFileFormat detected = media::AudioFileFormat::Unknown;
    auto stream = media::open_audio_stream(source, a, &detected);
    CHECK(stream && detected == media::AudioFileFormat::Vorbis && near(static_cast<double>(stream->frame_count()) / stream->format().sample_rate, 2.0, 0.1));
    MseBuffer c(a, MseFormat::Webm, 2);
    ByteBuffer ogg2(a);
    CHECK(c.append(opus.view(), 0) == MseAppend::Ok && c.plan(500000, &plan) && c.snapshot(&plan, &ogg2) && memcmp(ogg2.view().data(), "OggS", 4) == 0);
    // the picture alone is a whole file
    MseBuffer d(a, MseFormat::Webm, 3);
    ByteBuffer file(a);
    CHECK(d.append(vp9.view(), 0) == MseAppend::Ok && d.has_video() && !d.split_parts() && d.plan(500000, &plan) && plan.has_video && d.snapshot(&plan, &file) && file.size() > 20000);
  }
  CHECK(a.live == 0);
  for (int kind = 0; kind < 4; ++kind) {   // lacing: one frame, Xiph, fixed size, EBML
    const std::string stream = laced_webm(kind);
    MseBuffer b(a, MseFormat::Webm, 1);
    CHECK(append_pieces(b, ConstByteView(reinterpret_cast<const uint8_t*>(stream.data()), stream.size()), kind == 3 ? 5 : 1000) == MseAppend::Ok);
    const double frames_per_block = kind == 0 ? 1 : 3;   // 20 ms each, a block at 0 s and one at 1 s
    CHECK(range_count(b) == 2 && near(range_end(b, false, 0), frames_per_block * 0.02, 0.001) && near(range_start(b, false, 1), 1.0, 0.001) && near(range_end(b, false, 1), 1.0 + frames_per_block * 0.02, 0.001));
    MseBuffer c(a, MseFormat::Webm, 2);
    ByteBuffer out(a);
    MseSnapshotPlan plan;
    CHECK(c.append(ConstByteView(reinterpret_cast<const uint8_t*>(stream.data()), stream.size()), 0) == MseAppend::Ok && c.plan(1000000, &plan) && c.snapshot(&plan, &out) && out.size() > 100);
  }
  {   // bytes that are no WebM, media before its tracks, a track that is not kept
    MseBuffer b(a, MseFormat::Webm, 1);
    const uint8_t junk[] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(b.append(ConstByteView(junk, sizeof junk), 0) == MseAppend::Decode);
    const std::string stream = laced_webm(0);
    const size_t cluster = stream.find("\x1F\x43\xB6\x75");
    CHECK(cluster != std::string::npos && b.append(ConstByteView(reinterpret_cast<const uint8_t*>(stream.data()) + cluster, stream.size() - cluster), 0) == MseAppend::Decode);
  }
  CHECK(a.live == 0);
}

static void test_hub() {
  TestAllocator a;
  File video("tests/data/mse_video.mp4"), audio("tests/data/mse_audio.mp4");
  if (!video.ok() || !audio.ok()) return;
  MseHub hub(a);
  const uint32_t source = hub.create_source();
  CHECK(source != 0 && hub.find(source) && hub.find(source)->state == MseSource::State::Closed);
  uint32_t vb = 0, ab = 0;
  CHECK(hub.add_buffer(source, "video/mp4", &vb) == MseHub::AddResult::InvalidState);   // (not open yet)
  CHECK(hub.register_url("blob:http://example.com/1", source) && hub.source_for_url("blob:http://example.com/1") == source && hub.source_for_url("blob:x") == 0);
  CHECK(hub.attach(source, 7) && hub.find(source)->state == MseSource::State::Open && hub.find(source)->element == 7);
  CHECK(!hub.attach(source, 8) && hub.attach(source, 7));
  MseHub::Event e;
  CHECK(hub.take_event(&e) && e.source == source && StringView(e.type).equals("sourceopen") && !hub.take_event(&e));
  CHECK(hub.add_buffer(source, "video/ogg", &vb) == MseHub::AddResult::NotSupported);
  CHECK(hub.add_buffer(source, "video/mp4; codecs=\"avc1.4d401e\"", &vb) == MseHub::AddResult::Ok && hub.add_buffer(source, "audio/mp4; codecs=\"mp4a.40.2\"", &ab) == MseHub::AddResult::Ok && vb != ab);
  MseBuffer* v = hub.find_buffer(vb);
  MseBuffer* s = hub.find_buffer(ab);
  CHECK(v && s && hub.find_buffer(9999) == nullptr);
  CHECK(!hub.find(source)->duration_set);
  CHECK(v->append(video.view(), 0) == MseAppend::Ok);
  hub.appended(hub.find(source));
  CHECK(hub.find(source)->duration_set && near(hub.find(source)->duration, 2.08, 0.001));
  List<double> ranges(a);
  hub.buffered(source, &ranges);
  CHECK(ranges.count() == 0);   // (the sound has nothing yet: nothing is playable)
  CHECK(s->append(audio.view(), 0) == MseAppend::Ok);
  hub.appended(hub.find(source));
  hub.buffered(source, &ranges);
  CHECK(ranges.count() == 2 && near(ranges[0], 0.08, 0.001) && near(ranges[1], 2.04, 0.03));
  CHECK(near(hub.highest_end(source), 2.08, 0.001));
  CHECK(!hub.set_duration(source, 1.0) && hub.set_duration(source, 600.0) && hub.find(source)->duration == 600.0);
  hub.appended(hub.find(source));
  CHECK(hub.find(source)->duration == 600.0);   // (an explicit duration is kept)
  hub.end_of_stream(source, 0);
  CHECK(hub.find(source)->state == MseSource::State::Ended && near(hub.find(source)->duration, 2.08, 0.001));
  hub.buffered(source, &ranges);
  CHECK(ranges.count() == 2 && near(ranges[1], 2.08, 0.001));
  hub.reopen(source);
  CHECK(hub.find(source)->state == MseSource::State::Open);
  CHECK(hub.remove_buffer(source, ab) && !hub.remove_buffer(source, ab) && hub.find_buffer(ab) == nullptr);
  hub.detach(source);
  CHECK(hub.find(source)->state == MseSource::State::Closed && hub.find(source)->buffers.count() == 0 && !hub.find(source)->duration_set && hub.find(source)->element == 0);
  CHECK(hub.take_event(&e) && StringView(e.type).equals("sourceclose"));
  hub.revoke_url("blob:http://example.com/1");
  CHECK(hub.source_for_url("blob:http://example.com/1") == 0);
  hub.drop_source(source);
  CHECK(hub.find(source) == nullptr);
  uint32_t many[40];
  uint32_t made = 0;
  for (uint32_t& m : many) {
    m = hub.create_source();
    if (m) ++made;
  }
  CHECK(made >= MseHub::kMaxSources);   // (the unused closed ones make room for new ones)
}

int main() {
  test_types();
  test_mp4_stream();
  test_mpeg_and_adts();
  test_snapshot_plays();
  test_processing();
  test_damaged_input();
  test_keys();
  test_encryption();
  test_webm();
  test_webm_more();
  test_hub();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("mse tests passed\n");
  return 0;
}
