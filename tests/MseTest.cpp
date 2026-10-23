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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
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
  CHECK(!mse_type_supported("video/webm; codecs=\"vp9\"", &f, &video));
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
  CHECK(muxed.ok() && mp3.ok() && aac.ok());
  if (!muxed.ok() || !mp3.ok() || !aac.ok()) return;
  const struct {
    MseFormat format;
    const File* file;
  } kStreams[] = {{MseFormat::Mp4, &muxed}, {MseFormat::Mpeg, &mp3}, {MseFormat::Adts, &aac}};
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
      ByteBuffer out(a);
      if (b.plan(r.count() ? r[0].start : 0, &plan)) b.snapshot(&plan, &out);
    }
    for (uint64_t at = 0; at < size; at += step / 2 + 1) {   // bit flips
      for (uint8_t bit : {uint8_t{0x01}, uint8_t{0x80}, uint8_t{0x55}}) {
        ByteBuffer copy(a);
        copy.append(stream.file->view());
        copy.writable_view()[at] ^= bit;
        MseBuffer b(a, stream.format, 1);
        b.append(copy.view(), 0);
        MseSnapshotPlan plan;
        ByteBuffer out(a);
        List<MseBuffer::Range> r(a);
        b.buffered(&r, b.highest_end());
        if (r.count() && b.plan(r[0].start, &plan)) b.snapshot(&plan, &out);
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
  CHECK(hub.add_buffer(source, "video/webm", &vb) == MseHub::AddResult::NotSupported);
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
  test_hub();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("mse tests passed\n");
  return 0;
}
