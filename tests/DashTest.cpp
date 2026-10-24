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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Media/Dash.hpp"

using namespace media;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static TestAllocator A;

static std::string read_file(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return std::string();
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

// Serves the files of a directory as http://example.com/ ; the clock runs by the bytes sent at `link_bps`.
struct FakeFetcher final : IDashFetcher {
  std::string dir;
  uint64_t clock = 1000000, link_bps = 100000000;
  int64_t wall = 0;
  std::vector<std::string> log;
  bool fail_all = false;
  std::string override_name, override_body;   // a manifest served instead of the file
  bool fetch(StringView url, DashRange range, ByteBuffer* out) override {
    std::string u(url.data(), url.size());
    log.push_back(range.present ? u + " " + std::to_string(range.first) + "-" + std::to_string(range.last) : u);
    if (fail_all) return false;
    const std::string prefix = "http://example.com/";
    if (u.compare(0, prefix.size(), prefix) != 0) return false;
    std::string name = u.substr(prefix.size());
    std::string body = name == override_name ? override_body : read_file(dir + "/" + name);
    if (body.empty()) return false;
    if (range.present) {
      if (range.first >= body.size()) return false;
      uint64_t last = range.last >= body.size() ? body.size() - 1 : range.last;
      body = body.substr(range.first, last - range.first + 1);
    }
    clock += body.size() * 8000 / link_bps + 1;
    return out->append(ConstByteView(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
  }
  uint64_t now_ms() override { return clock; }
  int64_t wall_clock_s() override { return wall; }
};

static bool near(double a, double b, double e = 0.001) { return a - b < e && b - a < e; }

static void test_xml() {
  auto root = parse_xml(A, "<?xml version=\"1.0\"?><!-- c --><a:r xmlns:a='u' x='1&amp;2' a:y=\"q\"><b>t&lt;ext&#65;</b><c/><![CDATA[<raw>]]></a:r>");
  CHECK(root && root->name.view().equals("r") && root->attribute("x").equals("1&2") && root->attribute("y").equals("q"));
  if (root) {
    CHECK(root->children.count() == 2 && root->child("b") && root->child("b")->text.view().equals("t<extA") && root->child("c"));
    CHECK(root->text.view().equals("<raw>"));
  }
  CHECK(!parse_xml(A, "<a><b></a>"));
  CHECK(!parse_xml(A, "<a>"));
  CHECK(!parse_xml(A, "<a/><b/>"));
  CHECK(!parse_xml(A, "text"));
  CHECK(!parse_xml(A, "<a x=1/>"));
  CHECK(!parse_xml(A, ""));
  std::string deep;
  for (int i = 0; i < 200; ++i) deep += "<a>";
  CHECK(!parse_xml(A, deep.c_str()));
}

static void test_values() {
  int64_t v;
  CHECK(parse_iso_duration("PT1H2M3.5S", &v) && v == 3723500000);
  CHECK(parse_iso_duration("P1DT0S", &v) && v == 86400000000);
  CHECK(parse_iso_duration("PT0.5S", &v) && v == 500000);
  CHECK(!parse_iso_duration("1H", &v) && !parse_iso_duration("PT", &v) && !parse_iso_duration("PT5", &v) && !parse_iso_duration("PTxS", &v));
  CHECK(parse_iso_datetime("1970-01-01T00:00:00Z", &v) && v == 0);
  CHECK(parse_iso_datetime("2020-02-29T12:30:15Z", &v) && v == 1582979415);
  CHECK(parse_iso_datetime("2020-02-29T12:30:15.250+01:00", &v) && v == 1582979415 - 3600);
  CHECK(!parse_iso_datetime("2020-13-01T00:00:00Z", &v) && !parse_iso_datetime("garbage", &v));
  String s(A);
  CHECK(dash_expand("seg-$RepresentationID$-$Number%05d$-$Bandwidth$-$Time$-$$.m4s", "v1", 500, 7, 90000, &s) && std::string(s.c_str()) == "seg-v1-00007-500-90000-$.m4s");
  s.clear();
  CHECK(!dash_expand("a$Number", "x", 0, 0, 0, &s) && !dash_expand("$Bogus$", "x", 0, 0, 0, &s) && !dash_expand("$Number%5x$", "x", 0, 0, 0, &s));
}

static void test_mpd() {
  const std::string mpd = read_file("tests/data/dash/manifest.mpd");
  DashManifest m(A);
  CHECK(!mpd.empty() && parse_mpd(A, mpd.c_str(), "http://example.com/manifest.mpd", &m));
  CHECK(!m.dynamic && m.duration_us == 4000000 && m.min_buffer_us == 2000000 && m.periods.count() == 1);
  if (m.periods.count() == 1) {
    const DashPeriod& p = *m.periods[0];
    CHECK(p.sets.count() == 2 && p.sets[0]->content_type.view().equals("video") && p.sets[1]->content_type.view().equals("audio"));
    CHECK(p.sets[0]->representations.count() == 2 && p.sets[0]->representations[0]->bandwidth == 150000 && p.sets[0]->representations[1]->bandwidth == 400000);
    const DashRepresentation& hi = *p.sets[0]->representations[1];
    CHECK(hi.width == 64 && hi.codecs.view().equals("avc1.f4000c") && hi.mime_type.view().equals("video/mp4") && hi.base_url.view().equals("http://example.com/manifest.mpd"));
    DashSegments segs(A);
    CHECK(dash_segments(A, m, p, 4000000, hi, -1, nullptr, &segs));
    CHECK(segs.list.count() == 4 && segs.list[0].start_us == 0 && segs.list[3].start_us == 3000000 && segs.list[0].duration_us == 1000000 && segs.list[2].number == 3);
    CHECK(std::string(segs.init_url.c_str()) == "http://example.com/init-stream1.m4s" || std::string(segs.init_url.c_str()) == "http://example.com/init-stream0.m4s");
    String url(A);
    CHECK(dash_media_url(A, hi, segs.list[1], &url) && std::string(url.c_str()).find("chunk-stream") != std::string::npos && std::string(url.c_str()).find("-00002.m4s") != std::string::npos);
    // audio: an irregular timeline whose entries continue each other
    const DashRepresentation& audio = *p.sets[1]->representations[0];
    DashSegments asegs(A);
    CHECK(dash_segments(A, m, p, 4000000, audio, -1, nullptr, &asegs) && asegs.list.count() == 4);
    if (asegs.list.count() == 4) CHECK(near(asegs.list[1].start_us / 1e6, 44032 / 44100.0, 0.00001) && near(asegs.list[3].start_us / 1e6, (44032 * 2 + 45056) / 44100.0, 0.00001));
  }
  CHECK(!parse_mpd(A, "<html/>", "http://example.com/m.mpd", &m));
  DashManifest empty(A);
  CHECK(!parse_mpd(A, "<MPD></MPD>", "http://example.com/m.mpd", &empty));
}

static void test_template_variants() {
  // a number template with a duration, a base URL chain, two periods, offsets
  const char* mpd =
      "<MPD type='static' mediaPresentationDuration='PT10S' minBufferTime='PT1S'><BaseURL>media/</BaseURL>"
      "<Period id='a' start='PT0S' duration='PT6S'><BaseURL>p1/</BaseURL><AdaptationSet mimeType='video/mp4' codecs='avc1.42c00d'>"
      "<SegmentTemplate timescale='1000' duration='2500' startNumber='0' media='s$Number$.m4s' initialization='i.mp4'/>"
      "<Representation id='r' bandwidth='1000'><BaseURL>r/</BaseURL></Representation></AdaptationSet></Period>"
      "<Period id='b' start='PT6S'><AdaptationSet mimeType='video/mp4' codecs='avc1.42c00d'>"
      "<SegmentTemplate timescale='1000' duration='2000' presentationTimeOffset='500' media='t$Time$.m4s' initialization='i2.mp4'/>"
      "<Representation id='r2' bandwidth='1000'/></AdaptationSet></Period></MPD>";
  DashManifest m(A);
  CHECK(parse_mpd(A, mpd, "http://example.com/dir/x.mpd", &m) && m.periods.count() == 2);
  if (m.periods.count() != 2) return;
  const DashRepresentation& r1 = *m.periods[0]->sets[0]->representations[0];
  CHECK(r1.base_url.view().equals("http://example.com/dir/media/p1/r/"));
  DashSegments s1(A);
  CHECK(dash_segments(A, m, *m.periods[0], 6000000, r1, -1, nullptr, &s1) && s1.list.count() == 3);
  if (s1.list.count() == 3) CHECK(s1.list[0].number == 0 && s1.list[2].number == 2 && s1.list[2].duration_us == 1000000 && s1.list[1].start_us == 2500000);
  CHECK(std::string(s1.init_url.c_str()) == "http://example.com/dir/media/p1/r/i.mp4");
  DashSegments s2(A);
  const DashRepresentation& r2 = *m.periods[1]->sets[0]->representations[0];
  CHECK(dash_segments(A, m, *m.periods[1], 10000000, r2, -1, nullptr, &s2) && s2.list.count() == 2);
  CHECK(s2.timestamp_offset_us == 6000000 - 500000);
  if (s2.list.count() == 2) {
    String url(A);
    CHECK(dash_media_url(A, r2, s2.list[1], &url) && std::string(url.c_str()) == "http://example.com/dir/media/t2500.m4s");
    CHECK(s2.list[0].start_us == 6000000);
  }
  // a dynamic presentation with a time shift buffer: only what is available
  const char* live =
      "<MPD type='dynamic' availabilityStartTime='2020-01-01T00:00:00Z' minimumUpdatePeriod='PT2S' timeShiftBufferDepth='PT6S' minBufferTime='PT1S'>"
      "<Period start='PT0S'><AdaptationSet mimeType='video/mp4' codecs='avc1.42c00d'><SegmentTemplate timescale='1' duration='2' media='$Number$.m4s'/>"
      "<Representation id='r' bandwidth='1'/></AdaptationSet></Period></MPD>";
  DashManifest lm(A);
  CHECK(parse_mpd(A, live, "http://example.com/live.mpd", &lm) && lm.dynamic && lm.min_update_us == 2000000 && lm.time_shift_depth_us == 6000000);
  DashSegments ls(A);
  CHECK(dash_segments(A, lm, *lm.periods[0], -1, *lm.periods[0]->sets[0]->representations[0], 21000000, nullptr, &ls));
  CHECK(ls.list.count() >= 3 && ls.list.count() <= 5 && ls.list[ls.list.count() - 1].start_us + ls.list[ls.list.count() - 1].duration_us <= 21000000);
  // a segment list with ranges
  const char* single = "tests/data/dash_single/manifest.mpd";
  const std::string text = read_file(single);
  DashManifest sm(A);
  CHECK(parse_mpd(A, text.c_str(), "http://example.com/manifest.mpd", &sm));
  DashSegments ss(A);
  const DashRepresentation& sr = *sm.periods[0]->sets[0]->representations[0];
  CHECK(dash_segments(A, sm, *sm.periods[0], 4000000, sr, -1, nullptr, &ss) && ss.list.count() == 4 && ss.init_range.present && ss.init_range.first == 0);
  if (ss.list.count() == 4) {
    CHECK(ss.list[1].range.present && ss.list[1].range.first == 7482 && ss.list[1].start_us == 1000000);
    String url(A);
    CHECK(dash_media_url(A, sr, ss.list[1], &url) && std::string(url.c_str()) == "http://example.com/manifest-stream0.mp4");
  }
}

static void test_sidx() {
  // version 0, 2 references, timescale 1000
  const uint8_t box[] = {0, 0, 0, 44, 's', 'i', 'd', 'x', 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 3, 0xE8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2,
                         0, 0, 0, 100, 0, 0, 7, 0xD0, 0x80, 0, 0, 0, 0, 0, 0, 50, 0, 0, 3, 0xE8, 0x80, 0, 0, 0};
  uint64_t scale, earliest;
  List<DashSidxEntry> e(A);
  CHECK(parse_sidx(A, ConstByteView(box, sizeof box), 1000, &scale, &earliest, &e) && scale == 1000 && earliest == 0 && e.count() == 2);
  if (e.count() == 2) CHECK(e[0].offset == 1000 && e[0].size == 100 && e[0].duration == 2000 && e[1].offset == 1100 && e[1].size == 50);
  for (size_t cut = 0; cut < sizeof box; ++cut) {
    List<DashSidxEntry> t(A);
    CHECK(!parse_sidx(A, ConstByteView(box, cut), 0, &scale, &earliest, &t));
  }
}

// Plays the fixture with the playhead moving in real time; returns the client's state.
struct Run {
  MseHub hub;
  FakeFetcher fetcher;
  uint32_t source = 0;
  Run() : hub(A) {}
};

static void play(Run& r, DashClient& client, int64_t until_us, int64_t step_us = 250000, int64_t start_us = 0) {
  for (int64_t t = start_us; t <= until_us && client.error() == 0; t += step_us) client.tick(t);
}

static double buffered_end(Run& r) {
  List<double> ranges(A);
  r.hub.buffered(r.source, &ranges);
  return ranges.count() >= 2 ? ranges[ranges.count() - 1] : -1;
}

static void test_client() {
  {   // a static presentation on a fast link: the best quality, everything fetched, the stream ends
    Run r;
    r.fetcher.dir = "tests/data/dash";
    r.source = r.hub.create_source();
    CHECK(r.hub.attach(r.source, 1));
    DashClient client(A, r.hub, r.source, r.fetcher);
    const std::string mpd = read_file("tests/data/dash/manifest.mpd");
    CHECK(client.start("http://example.com/manifest.mpd", ConstByteView(reinterpret_cast<const uint8_t*>(mpd.data()), mpd.size())));
    CHECK(client.stream_count() == 2 && r.hub.find(r.source)->buffers.count() == 2);
    play(r, client, 6000000);
    CHECK(client.error() == 0 && client.ended() && r.hub.find(r.source)->state == MseSource::State::Ended);
    CHECK(client.chosen_bandwidth(0) == 400000 && client.chosen_bandwidth(1) == 48000);
    CHECK(near(buffered_end(r), 4.0, 0.1));
    CHECK(client.segments_fetched() == 8 && client.switches() <= 1);
    CHECK(near(r.hub.find(r.source)->duration, 4.0, 0.1));
  }
  {   // a slow link: the lowest quality
    Run r;
    r.fetcher.dir = "tests/data/dash";
    r.fetcher.link_bps = 100000;
    r.source = r.hub.create_source();
    r.hub.attach(r.source, 1);
    DashClient client(A, r.hub, r.source, r.fetcher);
    const std::string mpd = read_file("tests/data/dash/manifest.mpd");
    CHECK(client.start("http://example.com/manifest.mpd", ConstByteView(reinterpret_cast<const uint8_t*>(mpd.data()), mpd.size())));
    play(r, client, 6000000);
    CHECK(client.error() == 0 && client.chosen_bandwidth(0) == 150000);
    CHECK(client.throughput_bps() > 0 && client.throughput_bps() < 400000);
  }
  {   // seeking: the player jumps to the segment that has the playhead
    Run r;
    r.fetcher.dir = "tests/data/dash";
    r.source = r.hub.create_source();
    r.hub.attach(r.source, 1);
    DashClient client(A, r.hub, r.source, r.fetcher);
    const std::string mpd = read_file("tests/data/dash/manifest.mpd");
    CHECK(client.start("http://example.com/manifest.mpd", ConstByteView(reinterpret_cast<const uint8_t*>(mpd.data()), mpd.size())));
    r.fetcher.log.clear();
    client.tick(2500000);
    bool second_from_start = false;
    for (const auto& l : r.fetcher.log) second_from_start = second_from_start || l.find("00001.m4s") != std::string::npos;
    CHECK(!second_from_start);
    bool third = false;
    for (const auto& l : r.fetcher.log) third = third || l.find("00003.m4s") != std::string::npos;
    CHECK(third);
    play(r, client, 6000000, 250000, 2500000);
    CHECK(client.error() == 0);
  }
  {   // single file with byte ranges
    Run r;
    r.fetcher.dir = "tests/data/dash_single";
    r.source = r.hub.create_source();
    r.hub.attach(r.source, 1);
    DashClient client(A, r.hub, r.source, r.fetcher);
    const std::string mpd = read_file("tests/data/dash_single/manifest.mpd");
    CHECK(client.start("http://example.com/manifest.mpd", ConstByteView(reinterpret_cast<const uint8_t*>(mpd.data()), mpd.size())));
    play(r, client, 6000000);
    CHECK(client.error() == 0 && client.ended() && near(buffered_end(r), 4.0, 0.1));
    bool ranged = false;
    for (const auto& l : r.fetcher.log) ranged = ranged || l.find("manifest-stream0.mp4 831-7481") != std::string::npos;
    CHECK(ranged);
  }
  {   // errors: a segment that cannot be had ends the stream with a network error; a manifest that is not one is refused
    Run r;
    r.fetcher.dir = "tests/data/dash";
    r.source = r.hub.create_source();
    r.hub.attach(r.source, 1);
    DashClient client(A, r.hub, r.source, r.fetcher);
    const std::string mpd = read_file("tests/data/dash/manifest.mpd");
    CHECK(client.start("http://example.com/manifest.mpd", ConstByteView(reinterpret_cast<const uint8_t*>(mpd.data()), mpd.size())));
    r.fetcher.fail_all = true;
    client.tick(0);
    CHECK(client.error() == 2);
    Run r2;
    r2.source = r2.hub.create_source();
    r2.hub.attach(r2.source, 1);
    DashClient bad(A, r2.hub, r2.source, r2.fetcher);
    CHECK(!bad.start("http://example.com/x.mpd", ConstByteView(reinterpret_cast<const uint8_t*>("<MPD/>"), 6)) && bad.error() == 4);
    // protected content cannot be played
    Run r3;
    r3.source = r3.hub.create_source();
    r3.hub.attach(r3.source, 1);
    DashClient prot(A, r3.hub, r3.source, r3.fetcher);
    const char* enc = "<MPD type='static' mediaPresentationDuration='PT4S'><Period><AdaptationSet contentType='video' mimeType='video/mp4' codecs='avc1.42c00d'>"
                      "<ContentProtection schemeIdUri='urn:uuid:EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED'/><SegmentTemplate timescale='1' duration='1' media='$Number$.m4s'/>"
                      "<Representation id='r' bandwidth='1'/></AdaptationSet></Period></MPD>";
    CHECK(!prot.start("http://example.com/e.mpd", ConstByteView(reinterpret_cast<const uint8_t*>(enc), strlen(enc))) && prot.error() == 4);
  }
  {   // a dynamic presentation: segments are published as time passes, and the manifest is fetched again
    Run r;
    r.fetcher.dir = "tests/data/dash";
    r.fetcher.wall = 1000;
    const char* live =
        "<MPD type='dynamic' availabilityStartTime='1970-01-01T00:00:00Z' minimumUpdatePeriod='PT1S' suggestedPresentationDelay='PT1S' minBufferTime='PT1S'>"
        "<Period start='PT0S'><AdaptationSet contentType='video' mimeType='video/mp4' codecs='avc1.f4000c'>"
        "<SegmentTemplate timescale='10240' duration='10240' startNumber='1' media='chunk-stream0-$Number%05d$.m4s' initialization='init-stream0.m4s'/>"
        "<Representation id='0' bandwidth='400000'/></AdaptationSet></Period></MPD>";
    r.fetcher.override_name = "live.mpd";
    r.fetcher.override_body = live;
    r.source = r.hub.create_source();
    r.hub.attach(r.source, 1);
    DashClient client(A, r.hub, r.source, r.fetcher);
    // availability is far beyond the four segments there are; the client asks for segment numbers around the edge, so only the first four exist
    CHECK(client.start("http://example.com/live.mpd", ConstByteView(reinterpret_cast<const uint8_t*>(live), strlen(live))));
    CHECK(client.manifest() && client.manifest()->dynamic);
    r.fetcher.wall = 3;   // the edge is at 3 s: segments 1 and 2 are out
    client.tick(0);
    CHECK(client.error() == 0);
    CHECK(!client.ended());
    r.fetcher.log.clear();
    r.fetcher.clock += 3000;
    client.tick(500000);
    bool manifest_again = false;
    for (const auto& l : r.fetcher.log) manifest_again = manifest_again || l == "http://example.com/live.mpd";
    CHECK(manifest_again);
  }
}

// Truncated and damaged manifests: nothing may crash.
static void test_damage() {
  const std::string mpd = read_file("tests/data/dash/manifest.mpd");
  for (size_t cut = 0; cut < mpd.size(); cut += 3) {
    DashManifest m(A);
    parse_mpd(A, StringView(mpd.data(), cut), "http://example.com/m.mpd", &m);
  }
  for (size_t i = 0; i < mpd.size(); i += 5) {
    for (int bit = 0; bit < 8; bit += 3) {
      std::string f = mpd;
      f[i] = static_cast<char>(f[i] ^ (1 << bit));
      DashManifest m(A);
      if (!parse_mpd(A, StringView(f.data(), f.size()), "http://example.com/m.mpd", &m)) continue;
      for (const auto& p : m.periods.view())
        for (const auto& s : p->sets.view())
          for (const auto& rep : s->representations.view()) {
            DashSegments segs(A);
            dash_segments(A, m, *p, m.duration_us, *rep, 10000000, nullptr, &segs);
          }
    }
  }
}

int main() {
  test_xml();
  test_values();
  test_mpd();
  test_template_variants();
  test_sidx();
  test_client();
  test_damage();
  CHECK(A.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("DashTest passed\n");
  return 0;
}
