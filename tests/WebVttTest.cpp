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
#include <string>
#include "Media/WebVtt.hpp"

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

static std::string html(const char* text) {
  String out(A);
  CHECK(vtt_cue_text_to_html(text, &out));
  return std::string(out.c_str());
}
static std::string plain(const char* text) {
  String out(A);
  CHECK(vtt_cue_text_to_plain(text, &out));
  return std::string(out.c_str());
}
static bool near(double a, double b) { return a - b < 0.0005 && b - a < 0.0005; }

static void test_timestamps() {
  double s;
  CHECK(parse_vtt_timestamp("00:01.500", &s) && near(s, 1.5));
  CHECK(parse_vtt_timestamp("01:02:03.004", &s) && near(s, 3723.004));
  CHECK(parse_vtt_timestamp("100:00:00.000", &s) && near(s, 360000));
  CHECK(!parse_vtt_timestamp("1:02.000", &s));
  CHECK(!parse_vtt_timestamp("00:60.000", &s));
  CHECK(!parse_vtt_timestamp("00:00.00", &s));
  CHECK(!parse_vtt_timestamp("00:00:00", &s));
  CHECK(!parse_vtt_timestamp("00:00.000x", &s));
  CHECK(!parse_vtt_timestamp("", &s));
  String f(A);
  format_vtt_timestamp(3723.004, &f);
  CHECK(std::string(f.c_str()) == "01:02:03.004");
  f.clear();
  format_vtt_timestamp(61.5, &f);
  CHECK(std::string(f.c_str()) == "01:01.500");
}

static void test_parse() {
  const char* file =
      "\xEF\xBB\xBFWEBVTT - a title\r\nKind: captions\r\n\r\n"
      "STYLE\r\n::cue { color: red }\r\n\r\n"
      "REGION\r\nid:r1\r\nwidth:40%\r\nlines:4\r\nregionanchor:10%,90%\r\nviewportanchor:20%,80%\r\nscroll:up\r\n\r\n"
      "NOTE this is a comment\r\nwith two lines\r\n\r\n"
      "intro\r\n00:00:01.000 --> 00:00:04.000 line:10% position:20%,line-left size:60% align:left vertical:rl region:r1\r\nHello <b>world</b>\r\nsecond\r\n\r\n"
      "00:05.000 --> 00:06.000\r\nno id\r\n\r\n"
      "bad\r\n00:07.000 -> 00:08.000\r\nnot a cue\r\n\r\n"
      "00:09.000 --> 00:10.000 line:-2,end vertical:lr\r\nglued\r\n00:11.000 --> 00:12.000\r\nnext\r\n";
  VttFile v(A);
  CHECK(parse_webvtt(A, file, &v));
  CHECK(v.styles.count() == 1 && std::string(v.styles[0].c_str()) == "::cue { color: red }");
  CHECK(v.regions.count() == 1);
  if (v.regions.count() == 1) {
    const VttRegion& r = *v.regions[0];
    CHECK(std::string(r.id.c_str()) == "r1" && r.width == 40 && r.lines == 4 && r.anchor_x == 10 && r.anchor_y == 90 && r.viewport_x == 20 && r.viewport_y == 80 && r.scroll_up);
  }
  CHECK(v.cues.count() == 4);
  if (v.cues.count() == 4) {
    const VttCue& c = *v.cues[0];
    CHECK(std::string(c.id.c_str()) == "intro" && near(c.start, 1) && near(c.end, 4));
    CHECK(std::string(c.text.c_str()) == "Hello <b>world</b>\nsecond");
    CHECK(!c.snap_to_lines && !c.line_auto && c.line == 10);
    CHECK(!c.position_auto && c.position == 20 && c.position_align == VttPositionAlign::LineLeft);
    CHECK(c.size == 60 && c.align == VttAlign::Left && c.vertical == VttVertical::GrowingLeft && std::string(c.region_id.c_str()) == "r1");
    CHECK(v.cues[1]->id.empty() && v.cues[1]->line_auto && v.cues[1]->snap_to_lines && v.cues[1]->position_auto && v.cues[1]->size == 100);
    const VttCue& g = *v.cues[2];
    CHECK(near(g.start, 9) && g.snap_to_lines && g.line == -2 && g.line_align == VttLineAlign::End && g.vertical == VttVertical::GrowingRight);
    CHECK(std::string(g.text.c_str()) == "glued");
    CHECK(near(v.cues[3]->start, 11) && std::string(v.cues[3]->text.c_str()) == "next");
  }
  // not WebVTT
  VttFile w(A);
  CHECK(!parse_webvtt(A, "WEBVTTX\n\n", &w));
  CHECK(!parse_webvtt(A, "", &w));
  CHECK(!parse_webvtt(A, "00:00.000 --> 00:01.000\nhi\n", &w));
  CHECK(parse_webvtt(A, "WEBVTT", &w) && w.cues.count() == 0);
  // a cue right after the signature without a blank line is still read
  VttFile x(A);
  CHECK(parse_webvtt(A, "WEBVTT\n00:00.000 --> 00:01.000\nhi", &x) && x.cues.count() == 1);
  // invalid settings are ignored, valid ones stay
  VttFile y(A);
  CHECK(parse_webvtt(A, "WEBVTT\n\n00:00.000 --> 00:01.000 line:abc size:101% position:5% align:bogus vertical:xx foo bar:\nhi\n", &y) && y.cues.count() == 1);
  if (y.cues.count() == 1) CHECK(y.cues[0]->line_auto && y.cues[0]->size == 100 && y.cues[0]->position == 5 && y.cues[0]->align == VttAlign::Center && y.cues[0]->vertical == VttVertical::Horizontal);
  // a region of the same id replaces the earlier one; STYLE after a cue is not a style
  VttFile z(A);
  CHECK(parse_webvtt(A, "WEBVTT\n\nREGION\nid:a\nlines:2\n\nREGION\nid:a\nlines:5\n\n00:00.000 --> 00:01.000\nhi\n\nSTYLE\nx\n", &z));
  CHECK(z.regions.count() == 1 && z.regions[0]->lines == 5 && z.styles.count() == 0 && z.cues.count() == 1);
}

static void test_cue_text() {
  CHECK(html("plain") == "plain");
  CHECK(html("a <b>bold</b> <i>it</i> <u>un</u>") == "a <b>bold</b> <i>it</i> <u>un</u>");
  CHECK(html("<c.red.big>x</c>") == "<span class=\"red big\">x</span>");
  CHECK(html("<v Esme Smith>Hi</v>") == "<span title=\"Esme Smith\">Hi</span>");
  CHECK(html("<v.loud   Fred  >Hi") == "<span title=\"Fred\" class=\"loud\">Hi</span>");
  CHECK(html("<lang en-GB>Colour</lang>") == "<span lang=\"en-GB\">Colour</span>");
  CHECK(html("<ruby>base<rt>top</rt></ruby>") == "<ruby>base<rt>top</rt></ruby>");
  CHECK(html("<ruby>base<rt>top</ruby>!") == "<ruby>base<rt>top</rt></ruby>!");
  CHECK(html("<rt>no</rt>") == "no");
  CHECK(html("a<00:00:01.500>b") == "a<span data-timestamp=\"1.500\"></span>b");
  CHECK(html("a<00:1.5>b") == "ab");
  CHECK(html("&amp; &lt;b&gt; &nbsp;&#65;&#x42;&bogus; &") == "&amp; &lt;b&gt; \xC2\xA0" "AB&amp;bogus; &amp;");
  CHECK(html("<b>unclosed <i>nest") == "<b>unclosed <i>nest</i></b>");
  CHECK(html("</b>stray</i>") == "stray");
  CHECK(html("<unknown>x</unknown>") == "x");
  CHECK(html("a < b") == "a ");
  CHECK(plain("<v Bob>Hi &amp; <i>bye</i><00:01.000>!") == "Hi & bye!");
  // nesting is bounded
  std::string deep;
  for (int i = 0; i < 500; ++i) deep += "<b>";
  deep += "x";
  CHECK(html(deep.c_str()).find("x") != std::string::npos);
}


static void test_layout() {
  VttCue a(A), b(A);
  a.text.assign("Hello");
  b.text.assign("World");
  const VttCue* one[] = {&a};
  List<UniquePointer<VttDrawOp>> ops(A);
  // default: centred at the bottom of a 640x360 video
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(one, 1), 640, 360, 0, &ops));
  CHECK(ops.count() == 1);
  if (ops.count() == 1) {
    CHECK(ops[0]->width == 40 && ops[0]->height == 16 && ops[0]->x == 300 && ops[0]->y == 344);
    CHECK(ops[0]->background == 0xCC000000u && ops[0]->color == 0xFFFFFFFFu);
  }
  // the control bar keeps the bottom
  ops.clear();
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(one, 1), 640, 360, 24, &ops) && ops.count() == 1 && ops[0]->y == 320);
  // two cues at the default line stack upwards
  const VttCue* two[] = {&a, &b};
  ops.clear();
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(two, 2), 640, 360, 0, &ops) && ops.count() == 2);
  if (ops.count() == 2) CHECK(ops[0]->y == 344 && ops[1]->y == 328);
  // align:start, position auto: at the left; line 0: at the top
  a.align = VttAlign::Start;
  a.line_auto = false;
  a.line = 0;
  ops.clear();
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(one, 1), 640, 360, 0, &ops) && ops.count() == 1 && ops[0]->x == 0 && ops[0]->y == 0);
  // a percent line, a size that wraps the text, position at 100% line-right
  VttCue c(A);
  c.text.assign("one two three four");
  c.size = 20;   // 128 px = 16 cells
  c.snap_to_lines = false;
  c.line_auto = false;
  c.line = 50;
  c.position_auto = false;
  c.position = 100;
  c.position_align = VttPositionAlign::LineRight;
  c.align = VttAlign::Right;
  const VttCue* cc[] = {&c};
  ops.clear();
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(cc, 1), 640, 360, 0, &ops));
  CHECK(ops.count() == 2);
  if (ops.count() == 2) {
    CHECK(std::string(ops[0]->text.c_str()) == "one two three" && std::string(ops[1]->text.c_str()) == "four");
    CHECK(ops[0]->y == 180 && ops[1]->y == 196);
    CHECK(ops[1]->x + static_cast<int32_t>(ops[1]->width) == 640 && ops[0]->x + static_cast<int32_t>(ops[0]->width) == 640);
  }
  // styled runs
  VttCue d(A);
  d.text.assign("<c.yellow>Y</c><b>B</b><c.bg_blue>Z</c>");
  const VttCue* dd[] = {&d};
  ops.clear();
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(dd, 1), 640, 360, 0, &ops) && ops.count() == 3);
  if (ops.count() == 3) {
    CHECK(ops[0]->color == 0xFFFFFF00u && (ops[1]->flags & VttDrawOp::Bold) && ops[2]->background == 0xCC0000FFu);
    CHECK(ops[1]->x == ops[0]->x + 8);
  }
  // a region rolls its lines up from the bottom and keeps the newest
  VttCue r1(A), r2(A), r3(A);
  for (VttCue* r : {&r1, &r2, &r3}) {
    r->has_region = true;
    r->region.id.assign("r");
    r->region.lines = 2;
    r->region.width = 50;
    r->region.anchor_x = 50;
    r->region.anchor_y = 100;
    r->region.viewport_x = 50;
    r->region.viewport_y = 100;
  }
  r1.text.assign("first");
  r2.text.assign("second");
  r3.text.assign("third");
  const VttCue* rr[] = {&r1, &r2, &r3};
  ops.clear();
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(rr, 3), 640, 360, 0, &ops) && ops.count() == 2);
  if (ops.count() == 2) CHECK(std::string(ops[0]->text.c_str()) == "second" && std::string(ops[1]->text.c_str()) == "third" && ops[1]->y == 344);
  // vertical: characters stack in a column
  VttCue v(A);
  v.text.assign("ab");
  v.vertical = VttVertical::GrowingLeft;
  const VttCue* vv[] = {&v};
  ops.clear();
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(vv, 1), 640, 360, 0, &ops) && ops.count() == 2);
  if (ops.count() == 2) CHECK(ops[1]->y == ops[0]->y + 16 && ops[0]->x == ops[1]->x);
  // too small, or blank: nothing
  ops.clear();
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(one, 1), 16, 16, 0, &ops) && ops.count() == 0);
  VttCue blank(A);
  blank.text.assign("   \n  ");
  const VttCue* bl[] = {&blank};
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(bl, 1), 640, 360, 0, &ops) && ops.count() == 0);
  // an enormous word is cut to the box
  VttCue big(A);
  big.text.assign(std::string(5000, 'x').c_str());
  const VttCue* bg[] = {&big};
  ops.clear();
  CHECK(layout_vtt_cues(A, ArrayView<const VttCue* const>(bg, 1), 640, 360, 0, &ops));
  for (const auto& o : ops.view()) CHECK(o->x >= 0 && o->x + static_cast<int32_t>(o->width) <= 640 && o->y >= 0 && o->y + 16 <= 360);
}

// Truncated and bit-flipped files: nothing may crash, and what is parsed must still lay out.
static void test_damage() {
  const std::string file =
      "WEBVTT\n\nSTYLE\n::cue{}\n\nREGION\nid:r\nwidth:30%\nlines:2\n\n1\n00:00:01.000 --> 00:00:04.000 line:10% position:20%,line-left size:60% align:left region:r\n"
      "Hello <v Bob><c.red.bg_blue>wor&amp;ld</c></v>\nsecond <00:00:02.000>line <ruby>a<rt>b</rt></ruby>\n\n00:05.000 --> 00:06.000 vertical:lr\nvertical text\n";
  for (size_t cut = 0; cut <= file.size(); ++cut) {
    VttFile v(A);
    if (!parse_webvtt(A, StringView(file.data(), cut), &v)) continue;
    List<UniquePointer<VttCue>>& cues = v.cues;
    List<const VttCue*> shown(A);
    for (auto& c : cues.view()) {
      String h(A);
      vtt_cue_text_to_html(c->text.view(), &h);
      shown.push_back(c.get());
    }
    List<UniquePointer<VttDrawOp>> ops(A);
    layout_vtt_cues(A, shown.view(), 320, 180, 0, &ops);
  }
  for (size_t i = 0; i < file.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string f = file;
      f[i] = static_cast<char>(f[i] ^ (1 << bit));
      VttFile v(A);
      if (!parse_webvtt(A, f.c_str(), &v)) continue;
      List<const VttCue*> shown(A);
      for (auto& c : v.cues.view()) shown.push_back(c.get());
      List<UniquePointer<VttDrawOp>> ops(A);
      layout_vtt_cues(A, shown.view(), 320, 180, 24, &ops);
      for (const auto& o : ops.view()) CHECK(o->x > -100000 && o->x < 100000);
    }
  }
}

int main() {
  test_timestamps();
  test_parse();
  test_cue_text();
  test_layout();
  test_damage();
  CHECK(A.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("WebVttTest passed\n");
  return 0;
}
