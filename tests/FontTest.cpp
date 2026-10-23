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
// Host-side tests for the font stack (src/Font): the bounds-checked TrueType and WOFF readers (including
// truncated and bit-flipped files), the glyph rasteriser, the bounded glyph cache, CSS font matching with
// fallback, and the shaper. A small test font is built in memory so metrics and pixels are exact.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Font/FontSet.hpp"
#include "Font/GlyphCache.hpp"
#include "Font/GlyphRaster.hpp"
#include "Font/TrueType.hpp"
#include "Font/WoffDecode.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"
#include "Media/Zlib.hpp"

using namespace font;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// ---------------------------------------------------------------------------------------------------------
// A tiny font, upem 1000. Glyphs: 0 .notdef box, 1 space, 2 'A' (rectangle 100..600 x 0..700), 3 'B' (composite:
// A scaled by 0.5 and moved right by 200), 4 'C' (a closed curve of four off-curve points), 5 U+1F600 (1000 box),
// 6 'E' (composite: A, plus A joined by point matching so that its first point meets A's fourth).
// ---------------------------------------------------------------------------------------------------------
struct TestFontOptions {
  bool format4 = true;
  bool format12 = true;
  bool long_loca = false;
  bool use_typo = false;
  bool self_reference = false;  // glyph 3 refers to itself
};

static void be16(ByteBuffer& b, uint32_t v) { b.put8(static_cast<uint8_t>(v >> 8)); b.put8(static_cast<uint8_t>(v)); }
static void be32(ByteBuffer& b, uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
static void put_text(ByteBuffer& b, const char* t) { while (*t) b.put8(static_cast<uint8_t>(*t++)); }

struct Pt { int x, y; bool on; };

static void simple_glyph(ByteBuffer& g, const Pt* pts, int n) {
  int minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
  for (int i = 0; i < n; ++i) {
    if (pts[i].x < minx) minx = pts[i].x;
    if (pts[i].x > maxx) maxx = pts[i].x;
    if (pts[i].y < miny) miny = pts[i].y;
    if (pts[i].y > maxy) maxy = pts[i].y;
  }
  be16(g, 1); be16(g, static_cast<uint32_t>(minx)); be16(g, static_cast<uint32_t>(miny)); be16(g, static_cast<uint32_t>(maxx)); be16(g, static_cast<uint32_t>(maxy));
  be16(g, static_cast<uint32_t>(n - 1));
  be16(g, 0);  // no instructions
  for (int i = 0; i < n; ++i) g.put8(pts[i].on ? 1 : 0);  // 16-bit deltas
  int prev = 0;
  for (int i = 0; i < n; ++i) { be16(g, static_cast<uint32_t>(pts[i].x - prev) & 0xFFFF); prev = pts[i].x; }
  prev = 0;
  for (int i = 0; i < n; ++i) { be16(g, static_cast<uint32_t>(pts[i].y - prev) & 0xFFFF); prev = pts[i].y; }
  while (g.size() % 4) g.put8(0);
}

static void build_test_font(ByteBuffer& out, const TestFontOptions& o = TestFontOptions()) {
  TestAllocator tmp;
  ByteBuffer glyf(tmp), loca(tmp), head(tmp), hhea(tmp), maxp(tmp), hmtx(tmp), cmap(tmp), kern(tmp), os2(tmp), name(tmp);
  uint32_t offsets[8] = {};
  auto mark = [&](int i) { offsets[i] = static_cast<uint32_t>(glyf.size()); };
  const Pt notdef[4] = {{0, 0, true}, {0, 700, true}, {500, 700, true}, {500, 0, true}};
  const Pt rect_a[4] = {{100, 0, true}, {100, 700, true}, {600, 700, true}, {600, 0, true}};
  const Pt curve[4] = {{200, 200, false}, {200, 800, false}, {800, 800, false}, {800, 200, false}};
  const Pt box[4] = {{0, 0, true}, {0, 1000, true}, {1000, 1000, true}, {1000, 0, true}};
  mark(0); simple_glyph(glyf, notdef, 4);
  mark(1);  // space: empty
  mark(2); simple_glyph(glyf, rect_a, 4);
  mark(3);
  be16(glyf, 0xFFFF); for (int i = 0; i < 4; ++i) be16(glyf, 0);  // composite header, bbox ignored
  be16(glyf, 1 | 2 | 8);                  // words, xy values, scale
  be16(glyf, o.self_reference ? 3 : 2);
  be16(glyf, 200); be16(glyf, 0);
  be16(glyf, 0x2000);                     // 0.5 in F2Dot14
  while (glyf.size() % 4) glyf.put8(0);
  mark(4); simple_glyph(glyf, curve, 4);
  mark(5); simple_glyph(glyf, box, 4);
  mark(6);
  be16(glyf, 0xFFFF); for (int i = 0; i < 4; ++i) be16(glyf, 0);
  be16(glyf, 0x20 | 2); be16(glyf, 2); glyf.put8(0); glyf.put8(0);  // A at (0,0), bytes
  be16(glyf, 0);        be16(glyf, 2); glyf.put8(3); glyf.put8(0);  // A again: its point 0 on point 3 of what is there
  while (glyf.size() % 4) glyf.put8(0);
  offsets[7] = static_cast<uint32_t>(glyf.size());
  for (int i = 0; i < 8; ++i) { if (o.long_loca) be32(loca, offsets[i]); else be16(loca, offsets[i] / 2); }

  be32(head, 0x00010000); be32(head, 0); be32(head, 0); be32(head, 0x5F0F3CF5); be16(head, 0); be16(head, 1000);
  for (int i = 0; i < 4; ++i) be32(head, 0);  // created, modified
  be16(head, 0); be16(head, 0); be16(head, 1000); be16(head, 1000);
  be16(head, 1); be16(head, 8); be16(head, 2); be16(head, o.long_loca ? 1 : 0); be16(head, 0);

  be32(hhea, 0x00010000); be16(hhea, 800); be16(hhea, static_cast<uint32_t>(-200) & 0xFFFF); be16(hhea, 100);
  for (int i = 0; i < 12; ++i) be16(hhea, 0);
  be16(hhea, 6);  // numberOfHMetrics: glyph 6 shares glyph 5's advance
  be32(maxp, 0x00010000); be16(maxp, 7); for (int i = 0; i < 13; ++i) be16(maxp, 0);

  const uint32_t adv[6] = {600, 300, 700, 700, 800, 1000};
  for (int i = 0; i < 6; ++i) { be16(hmtx, adv[i]); be16(hmtx, 0); }
  be16(hmtx, 0);

  // cmap
  ByteBuffer f4(tmp), f12(tmp);
  {
    const uint32_t seg = 4;
    be16(f4, 4); be16(f4, 16 + 8 * seg + 6); be16(f4, 0); be16(f4, seg * 2); be16(f4, 8); be16(f4, 2); be16(f4, 0);
    const uint32_t ends[4] = {0x20, 0x43, 0x45, 0xFFFF}, starts[4] = {0x20, 0x41, 0x45, 0xFFFF};
    for (uint32_t e : ends) be16(f4, e);
    be16(f4, 0);
    for (uint32_t s : starts) be16(f4, s);
    be16(f4, (1 - 0x20) & 0xFFFF); be16(f4, 0); be16(f4, (6 - 0x45) & 0xFFFF); be16(f4, 1);
    // idRangeOffset: segment 1 reads the glyph id array that follows the last offset (2 entries after it: offset = 2*(seg-1) + 2 ... )
    be16(f4, 0);
    be16(f4, 2 * (seg - 1));  // from this entry (index 1) to the array that follows the last entry: seg - 1 entries on
    be16(f4, 0); be16(f4, 0);
    be16(f4, 2); be16(f4, 3); be16(f4, 4);  // glyph id array for 'A'..'C'
  }
  {
    be16(f12, 12); be16(f12, 0); be32(f12, 16 + 12 * 4); be32(f12, 0); be32(f12, 4);
    const uint32_t g[4][3] = {{0x20, 0x20, 1}, {0x41, 0x43, 2}, {0x45, 0x45, 6}, {0x1F600, 0x1F600, 5}};
    for (auto& r : g) { be32(f12, r[0]); be32(f12, r[1]); be32(f12, r[2]); }
  }
  const uint32_t tables_in_cmap = (o.format4 ? 1 : 0) + (o.format12 ? 1 : 0);
  be16(cmap, 0); be16(cmap, tables_in_cmap);
  uint32_t off = 4 + 8 * tables_in_cmap;
  if (o.format4) { be16(cmap, 3); be16(cmap, 1); be32(cmap, off); off += static_cast<uint32_t>(f4.size()); }
  if (o.format12) { be16(cmap, 3); be16(cmap, 10); be32(cmap, off); }
  if (o.format4) cmap.append(f4.view());
  if (o.format12) cmap.append(f12.view());

  // kern: (A,B) -50, (A,C) -20, (C,A) +30
  be16(kern, 0); be16(kern, 1);
  be16(kern, 0); be16(kern, 14 + 18); be16(kern, 1);
  be16(kern, 3); be16(kern, 12); be16(kern, 1); be16(kern, 0);
  be16(kern, 2); be16(kern, 3); be16(kern, static_cast<uint32_t>(-50) & 0xFFFF);
  be16(kern, 2); be16(kern, 4); be16(kern, static_cast<uint32_t>(-20) & 0xFFFF);
  be16(kern, 4); be16(kern, 2); be16(kern, 30);

  // OS/2 version 2
  be16(os2, 2); be16(os2, 600); be16(os2, 700); be16(os2, 5); be16(os2, 0);
  while (os2.size() < 62) os2.put8(0);
  be16(os2, 0x20 | (o.use_typo ? 0x80 : 0));  // fsSelection at 62
  be16(os2, 0); be16(os2, 0);
  be16(os2, 750); be16(os2, static_cast<uint32_t>(-250) & 0xFFFF); be16(os2, 50);
  be16(os2, 900); be16(os2, 300);
  be32(os2, 0); be32(os2, 0);
  be16(os2, 500); be16(os2, 700); be16(os2, 0); be16(os2, 0); be16(os2, 0);

  // name: family, subfamily, full, postscript (Windows, UTF-16BE)
  const char* names[4] = {"Test Sans", "Bold", "Test Sans Bold", "TestSans-Bold"};
  const uint32_t ids[4] = {1, 2, 4, 6};
  be16(name, 0); be16(name, 4); be16(name, 6 + 12 * 4);
  uint32_t so = 0;
  for (int i = 0; i < 4; ++i) { const uint32_t len = static_cast<uint32_t>(strlen(names[i])) * 2; be16(name, 3); be16(name, 1); be16(name, 0x409); be16(name, ids[i]); be16(name, len); be16(name, so); so += len; }
  for (int i = 0; i < 4; ++i) for (const char* c = names[i]; *c; ++c) { name.put8(0); name.put8(static_cast<uint8_t>(*c)); }

  struct T { const char* tag; ByteBuffer* data; } tables[] = {{"OS/2", &os2}, {"cmap", &cmap}, {"glyf", &glyf}, {"head", &head}, {"hhea", &hhea}, {"hmtx", &hmtx},
                                                         {"kern", &kern}, {"loca", &loca}, {"maxp", &maxp}, {"name", &name}};
  const uint32_t n = 10;
  be32(out, 0x00010000); be16(out, n); be16(out, 128); be16(out, 3); be16(out, n * 16 - 128);
  uint32_t pos = 12 + 16 * n;
  for (auto& t : tables) {
    put_text(out, t.tag); be32(out, 0); be32(out, pos); be32(out, static_cast<uint32_t>(t.data->size()));
    pos += (static_cast<uint32_t>(t.data->size()) + 3) & ~3u;
  }
  for (auto& t : tables) { out.append(t.data->view()); while (out.size() % 4) out.put8(0); }
}

// Wraps an sfnt as WOFF 1.0 (deflating each table that gets smaller).
static void build_woff(ConstByteView sfnt, ByteBuffer& out, TestAllocator& a) {
  ByteReader r(sfnt);
  const uint32_t flavor = r.be32();
  const uint32_t n = r.be16();
  r.skip(6);
  struct E { uint32_t tag, off, len; } entries[16];
  UniquePointer<ByteBuffer> bufs[16];
  for (uint32_t i = 0; i < n; ++i) { entries[i].tag = r.be32(); r.be32(); entries[i].off = r.be32(); entries[i].len = r.be32(); bufs[i] = UniquePointer<ByteBuffer>::make(a, a); }
  uint32_t pos = 44 + 20 * n;
  uint32_t comp_len[16], file_off[16];
  for (uint32_t i = 0; i < n; ++i) {
    ConstByteView data = sfnt.subview(entries[i].off, entries[i].len);
    media::deflate(data, bufs[i].get(), 6, a);
    if (bufs[i]->size() >= entries[i].len) { bufs[i]->clear(); bufs[i]->append(data); }
    comp_len[i] = static_cast<uint32_t>(bufs[i]->size());
    file_off[i] = pos;
    pos += (comp_len[i] + 3) & ~3u;
  }
  put_text(out, "wOFF"); be32(out, flavor); be32(out, pos); be16(out, n); be16(out, 0); be32(out, static_cast<uint32_t>(sfnt.size()));
  be16(out, 1); be16(out, 0); for (int i = 0; i < 5; ++i) be32(out, 0);
  for (uint32_t i = 0; i < n; ++i) { be32(out, entries[i].tag); be32(out, file_off[i]); be32(out, comp_len[i]); be32(out, entries[i].len); be32(out, 0); }
  for (uint32_t i = 0; i < n; ++i) { out.append(bufs[i]->view()); while (out.size() % 4) out.put8(0); }
}

static uint8_t px(const GlyphBitmap& b, uint32_t x, uint32_t y) { return b.pixels()[y * b.width + x]; }
static bool near(int a, int b, int tol = 2) { return a - b <= tol && b - a <= tol; }

// Calls everything that reads font data, on arbitrary (possibly corrupt) bytes. Must never crash or report a sanitizer error.
static void exercise(ConstByteView data, TestAllocator& a) {
  TrueTypeFont f(a);
  if (!f.parse(data)) return;
  (void)f.metrics();
  (void)f.scaled_metrics(12 * kFixedOne);
  const uint32_t cps[] = {0, 0x20, 0x41, 0x42, 0x43, 0x45, 0x7E, 0xFFFF, 0x1F600, 0x10FFFF, 0xFFFFFFFFu};
  for (uint32_t cp : cps) (void)f.glyph_for_codepoint(cp);
  uint32_t count = f.glyph_count() < 64 ? f.glyph_count() : 64;
  for (uint32_t g = 0; g < count + 2; ++g) {
    (void)f.advance(g, 12 * kFixedOne);
    (void)f.kerning(g, g + 1, 12 * kFixedOne);
    Outline o(a);
    f.load_outline(g, &o);
    GlyphBitmap bm(a);
    rasterize_glyph(f, g, 12 * kFixedOne, g & 3, a, &bm);
  }
  (void)f.family_name().size();
}

static void test_parse_and_metrics() {
  TestAllocator a;
  {
    ByteBuffer data(a);
    build_test_font(data);
    TrueTypeFont f(a);
    CHECK(f.parse(data.view()));
    CHECK(f.units_per_em() == 1000 && f.glyph_count() == 7);
    CHECK(f.family_name().equals("Test Sans") && f.style_name().equals("Bold") && f.full_name().equals("Test Sans Bold") && f.postscript_name().equals("TestSans-Bold"));
    const FaceMetrics& m = f.metrics();
    CHECK(m.ascent == 800 && m.descent == 200 && m.line_gap == 100);
    CHECK(m.weight == 700 && m.stretch == 100 && m.bold && !m.italic);
    CHECK(m.x_height == 500 && m.cap_height == 700);
    const ScaledMetrics sm = f.scaled_metrics(16 * kFixedOne);  // 1024 units of 1/64 px per em
    CHECK(sm.ascent == 819 && sm.descent == 205 && sm.line_gap == 102);
    CHECK(sm.x_height == 512 && sm.cap_height == 717);
    CHECK(sm.zero_advance == 614);  // no '0': .notdef, 600
    CHECK(sm.line_spacing() == 819 + 205 + 102);
    CHECK(f.glyph_for_codepoint(0x20) == 1 && f.glyph_for_codepoint('A') == 2 && f.glyph_for_codepoint('B') == 3);
    CHECK(f.glyph_for_codepoint('C') == 4 && f.glyph_for_codepoint('E') == 6);
    CHECK(f.glyph_for_codepoint('D') == 0 && f.glyph_for_codepoint('a') == 0 && f.glyph_for_codepoint(0x10FFFF) == 0);
    CHECK(f.glyph_for_codepoint(0x1F600) == 5);
    CHECK(f.advance_units(2) == 700 && f.advance_units(5) == 1000 && f.advance_units(6) == 1000 && f.advance_units(7) == 0 && f.advance_units(0xFFFFFFFF) == 0);
    CHECK(f.advance(2, 20 * kFixedOne) == 14 * kFixedOne);
    CHECK(f.kerning_units(2, 3) == -50 && f.kerning_units(2, 4) == -20 && f.kerning_units(4, 2) == 30 && f.kerning_units(3, 2) == 0);
    CHECK(f.kerning(2, 3, 20 * kFixedOne) == -kFixedOne);
    CHECK(f.has_kerning());
  }
  {  // cmap variants and the long loca format
    TestFontOptions only4; only4.format12 = false; only4.long_loca = true;
    ByteBuffer d4(a); build_test_font(d4, only4);
    TrueTypeFont f4(a);
    CHECK(f4.parse(d4.view()));
    CHECK(f4.glyph_for_codepoint('A') == 2 && f4.glyph_for_codepoint('C') == 4 && f4.glyph_for_codepoint('E') == 6 && f4.glyph_for_codepoint(0x20) == 1);
    CHECK(f4.glyph_for_codepoint('D') == 0 && f4.glyph_for_codepoint(0x1F600) == 0);
    Outline o(a);
    CHECK(f4.load_outline(2, &o) && o.points.count() == 4);
    TestFontOptions only12; only12.format4 = false;
    ByteBuffer d12(a); build_test_font(d12, only12);
    TrueTypeFont f12(a);
    CHECK(f12.parse(d12.view()) && f12.glyph_for_codepoint('B') == 3 && f12.glyph_for_codepoint(0x1F600) == 5);
    TestFontOptions none; none.format4 = none.format12 = false;
    ByteBuffer d0(a); build_test_font(d0, none);
    TrueTypeFont f0(a);
    CHECK(!f0.parse(d0.view()));  // no usable cmap
    TestFontOptions typo; typo.use_typo = true;
    ByteBuffer dt(a); build_test_font(dt, typo);
    TrueTypeFont ft(a);
    CHECK(ft.parse(dt.view()) && ft.metrics().ascent == 750 && ft.metrics().descent == 250 && ft.metrics().line_gap == 50);
  }
  {  // rejects things that are not TrueType
    TrueTypeFont f(a);
    const uint8_t junk[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    CHECK(!f.parse(ConstByteView(junk)));
    CHECK(!f.parse(ConstByteView()));
    const uint8_t otto[] = {'O', 'T', 'T', 'O', 0, 1, 0, 0, 0, 0, 0, 0};
    CHECK(!f.parse(ConstByteView(otto)));
    ByteBuffer data(a);
    build_test_font(data);
    CHECK(!f.parse(data.view(), 1));  // not a collection
    CHECK(f.glyph_for_codepoint('A') == 0 && f.advance_units(2) == 0 && !f.valid());
    Outline o(a);
    CHECK(!f.load_outline(2, &o));
  }
  CHECK(a.live == 0);
}

static void test_outlines() {
  TestAllocator a;
  ByteBuffer data(a);
  build_test_font(data);
  TrueTypeFont f(a);
  CHECK(f.parse(data.view()));
  Outline o(a);
  CHECK(f.load_outline(2, &o));
  CHECK(o.points.count() == 4 && o.contour_ends.count() == 1 && o.contour_ends[0] == 3);
  CHECK(o.points[0].x == 100 && o.points[0].y == 0 && o.points[2].x == 600 && o.points[2].y == 700 && o.points[1].on_curve);
  CHECK(f.load_outline(1, &o) && o.points.count() == 0);  // space
  CHECK(f.load_outline(3, &o) && o.points.count() == 4);  // 0.5 scaled, moved by 200
  CHECK(o.points[0].x == 250 && o.points[0].y == 0 && o.points[2].x == 500 && o.points[2].y == 350);
  CHECK(f.load_outline(4, &o) && o.points.count() == 4 && !o.points[0].on_curve);
  CHECK(f.load_outline(6, &o) && o.points.count() == 8 && o.contour_ends.count() == 2 && o.contour_ends[1] == 7);
  CHECK(o.points[4].x == 600 && o.points[4].y == 0 && o.points[6].x == 1100 && o.points[6].y == 700);  // point matching
  CHECK(!f.load_outline(7, &o) && o.points.count() == 0);
  CHECK(!f.load_outline(1000, &o));
  {  // a composite that includes itself ends at the depth limit instead of recursing forever
    TestFontOptions bad; bad.self_reference = true;
    ByteBuffer d(a); build_test_font(d, bad);
    TrueTypeFont g(a);
    CHECK(g.parse(d.view()));
    Outline oo(a);
    CHECK(!g.load_outline(3, &oo));
    GlyphBitmap bm(a);
    CHECK(!rasterize_glyph(g, 3, 12 * kFixedOne, 0, a, &bm));
  }
}

static void test_raster() {
  TestAllocator a;
  {
  ByteBuffer data(a);
  build_test_font(data);
  TrueTypeFont f(a);
  CHECK(f.parse(data.view()));
  const Fixed ten = 10 * kFixedOne;
  {
    GlyphBitmap b(a);
    CHECK(rasterize_glyph(f, 2, ten, 0, a, &b));
    CHECK(b.left == 1 && b.top == 7 && b.width == 5 && b.height == 7);  // x 100..600 and y 0..700 units at 0.01 px/unit
    bool all = true;
    for (uint32_t y = 0; y < b.height; ++y) for (uint32_t x = 0; x < b.width; ++x) all = all && px(b, x, y) == 255;
    CHECK(all);
    GlyphBitmap h(a);
    CHECK(rasterize_glyph(f, 2, ten, 2, a, &h));  // half a pixel to the right
    CHECK(h.left == 1 && h.width == 6 && h.height == 7);
    CHECK(near(px(h, 0, 3), 128) && near(px(h, 5, 3), 128) && px(h, 2, 3) == 255);
  }
  {  // composite with a fractional box: x 2.5..5.0, y 0..3.5
    GlyphBitmap b(a);
    CHECK(rasterize_glyph(f, 3, ten, 0, a, &b));
    CHECK(b.left == 2 && b.top == 4 && b.width == 3 && b.height == 4);
    CHECK(near(px(b, 0, 0), 64) && near(px(b, 1, 0), 128) && near(px(b, 2, 0), 128));
    CHECK(near(px(b, 0, 2), 128) && px(b, 1, 2) == 255 && px(b, 2, 3) == 255 && near(px(b, 0, 3), 128));
  }
  {  // curves: the middle is solid, the bounding box corners are not
    GlyphBitmap b(a);
    CHECK(rasterize_glyph(f, 4, ten, 0, a, &b));
    CHECK(b.width == 6 && b.height == 6 && b.left == 2 && b.top == 8);
    CHECK(px(b, 2, 2) == 255 && px(b, 3, 3) == 255 && px(b, 0, 0) < 100 && px(b, 5, 5) < 100 && px(b, 0, 5) < 100);
    int total = 0;
    for (uint32_t y = 0; y < 6; ++y) for (uint32_t x = 0; x < 6; ++x) total += px(b, x, y);
    // area of the quadratic closed shape: a rounded square inscribed in the 6x6 box, between half and all of it
    CHECK(total > 255 * 18 && total < 255 * 36);
  }
  {  // blank and degenerate cases
    GlyphBitmap b(a);
    CHECK(rasterize_glyph(f, 1, ten, 0, a, &b) && b.empty());
    CHECK(rasterize_glyph(f, 2, 0, 0, a, &b) && b.empty());
    CHECK(rasterize_glyph(f, 5, 4000 * kFixedOne, 0, a, &b) && b.empty());  // beyond the size limit: blank, not a crash
    CHECK(!rasterize_glyph(f, 99, ten, 0, a, &b));
    GlyphBitmap big(a);
    CHECK(rasterize_glyph(f, 5, 500 * kFixedOne, 0, a, &big) && big.width == 500 && big.height == 500);
    CHECK(big.width == 500 && px(big, 250, 250) == 255);
  }
  {  // scaling is linear
    GlyphBitmap b(a);
    CHECK(rasterize_glyph(f, 2, 20 * kFixedOne, 0, a, &b));
    CHECK(b.width == 10 && b.height == 14 && b.left == 2);
  }
  }
  CHECK(a.live == 0);
}

static void flip_and_truncate_font(const TestFontOptions& o) {
  TestAllocator a;
  {
  ByteBuffer data(a);
  build_test_font(data, o);
  const ConstByteView whole = data.view();
  for (uint64_t len = 0; len <= whole.size(); ++len) exercise(whole.first(len), a);  // every truncation
  ByteBuffer copy(a);
  copy.append(whole);
  ByteView w = copy.writable_view();
  for (uint64_t i = 0; i < w.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      w[i] ^= static_cast<uint8_t>(1 << bit);
      exercise(copy.view(), a);
      w[i] ^= static_cast<uint8_t>(1 << bit);
    }
  }
  }
  CHECK(a.live == 0);
}

static void test_corrupt_fonts() {
  TestFontOptions defaults;
  flip_and_truncate_font(defaults);
  TestFontOptions long_loca; long_loca.long_loca = true; long_loca.format12 = false;
  flip_and_truncate_font(long_loca);
  TestFontOptions only12; only12.format4 = false;
  flip_and_truncate_font(only12);
}

static void test_woff() {
  TestAllocator a;
  ByteBuffer sfnt(a);
  build_test_font(sfnt);
  ByteBuffer woff(a);
  build_woff(sfnt.view(), woff, a);
  CHECK(is_woff(woff.view()) && !is_woff2(woff.view()) && !is_woff(sfnt.view()));
  ByteBuffer back(a);
  CHECK(decode_woff(woff.view(), &back, 1 << 20));
  TrueTypeFont f(a);
  CHECK(f.parse(back.view()) && f.glyph_for_codepoint('B') == 3 && f.kerning_units(2, 3) == -50 && f.family_name().equals("Test Sans"));
  ByteBuffer any(a);
  CHECK(font_to_sfnt(woff.view(), &any, 1 << 20) && any.size() == back.size());
  ByteBuffer plain(a);
  CHECK(font_to_sfnt(sfnt.view(), &plain, 1 << 20) && plain.size() == sfnt.size());
  {  // limits
    ByteBuffer small(a);
    CHECK(!decode_woff(woff.view(), &small, 200));
    ByteBuffer none(a);
    CHECK(!font_to_sfnt(ConstByteView(), &none, 1 << 20));
    const uint8_t woff2[] = {'w', 'O', 'F', '2', 0, 1, 0, 0};
    CHECK(!font_to_sfnt(ConstByteView(woff2), &none, 1 << 20));
    const uint8_t text[] = {'h', 'e', 'l', 'l', 'o', '!', '!', '!'};
    CHECK(!font_to_sfnt(ConstByteView(text), &none, 1 << 20));
  }
  {  // a table that claims to inflate to 4 GiB
    ByteBuffer bomb(a);
    bomb.append(woff.view());
    ByteView w = bomb.writable_view();
    w[44 + 12] = 0xFF; w[44 + 13] = 0xFF; w[44 + 14] = 0xFF; w[44 + 15] = 0xF0;  // origLength of table 0
    ByteBuffer out(a);
    CHECK(!decode_woff(bomb.view(), &out, 1 << 20));
  }
  const ConstByteView whole = woff.view();
  for (uint64_t len = 0; len < whole.size(); ++len) {
    ByteBuffer out(a);
    if (decode_woff(whole.first(len), &out, 1 << 20)) exercise(out.view(), a);
  }
  ByteBuffer copy(a);
  copy.append(whole);
  ByteView w = copy.writable_view();
  for (uint64_t i = 0; i < w.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      w[i] ^= static_cast<uint8_t>(1 << bit);
      ByteBuffer out(a);
      if (decode_woff(copy.view(), &out, 1 << 20)) exercise(out.view(), a);
      w[i] ^= static_cast<uint8_t>(1 << bit);
    }
  }
}

static GlyphBitmap make_bitmap(TestAllocator& a, uint32_t w, uint32_t h) {
  GlyphBitmap b(a);
  b.allocate(w, h);
  return b;
}

static void test_cache() {
  TestAllocator a;
  {
    GlyphCacheLimits limits;
    limits.max_bytes = 3 * (GlyphCache::kEntryOverhead + 100) + 10;
    limits.max_entries = 100;
    GlyphCache cache(a, limits);
    GlyphKey k[6];
    for (uint32_t i = 0; i < 6; ++i) { k[i].face = 1; k[i].glyph = i; k[i].size = 16 * kFixedOne; }
    CHECK(cache.find(k[0]) == nullptr && cache.misses() == 1);
    for (uint32_t i = 0; i < 3; ++i) CHECK(cache.insert(k[i], make_bitmap(a, 10, 10)) != nullptr);
    CHECK(cache.entries() == 3 && cache.bytes() == 3 * (GlyphCache::kEntryOverhead + 100));
    CHECK(cache.find(k[0]) != nullptr && cache.hits() == 1);  // 0 is now the most recent, 1 the oldest
    cache.insert(k[3], make_bitmap(a, 10, 10));
    CHECK(cache.entries() == 3 && cache.find(k[1]) == nullptr && cache.find(k[0]) && cache.find(k[2]) && cache.find(k[3]));
    CHECK(cache.bytes() <= limits.max_bytes);
    // a bitmap bigger than the whole budget is handed back but not kept
    const GlyphBitmap* big = cache.insert(k[4], make_bitmap(a, 40, 40));
    CHECK(big && big->width == 40 && cache.entries() == 3 && cache.find(k[4]) == nullptr);
    // the same key replaces
    cache.insert(k[0], make_bitmap(a, 5, 5));
    CHECK(cache.find(k[0])->width == 5 && cache.entries() == 3);
    // size and subpixel are part of the key
    GlyphKey other = k[0]; other.size = 17 * kFixedOne;
    GlyphKey phase = k[0]; phase.subpixel = 1;
    CHECK(cache.find(other) == nullptr && cache.find(phase) == nullptr);
    cache.evict_face(1);
    CHECK(cache.entries() == 0 && cache.bytes() == 0);
    // many inserts never exceed the caps
    for (uint32_t i = 0; i < 500; ++i) {
      GlyphKey key; key.face = i % 7; key.glyph = i; key.size = 16 * kFixedOne;
      cache.insert(key, make_bitmap(a, 1 + i % 12, 1 + i % 9));
      CHECK(cache.bytes() <= limits.max_bytes);
    }
    cache.clear();
    CHECK(cache.entries() == 0);
  }
  {  // entry-count cap
    GlyphCacheLimits limits;
    limits.max_entries = 4;
    GlyphCache cache(a, limits);
    for (uint32_t i = 0; i < 50; ++i) {
      GlyphKey key; key.face = 2; key.glyph = i; key.size = 640;
      cache.insert(key, make_bitmap(a, 3, 3));
    }
    CHECK(cache.entries() == 4);
  }
  CHECK(a.live == 0);
}

static void test_unicode_range() {
  TestAllocator a;
  List<CodepointRange> r(a);
  CHECK(parse_unicode_range("U+0041-0043, u+4??, U+1F600", &r));
  CHECK(r.count() == 3 && r[0].first == 0x41 && r[0].last == 0x43 && r[1].first == 0x400 && r[1].last == 0x4FF && r[2].first == 0x1F600 && r[2].last == 0x1F600);
  List<CodepointRange> bad(a);
  CHECK(!parse_unicode_range("U+XYZ", &bad));
  CHECK(!parse_unicode_range("0041", &bad));
  List<CodepointRange> backwards(a);
  CHECK(parse_unicode_range("U+50-40, U+41", &backwards) && backwards.count() == 1);
}

static void test_fontset_bundled() {
  TestAllocator a;
  {
    FontSet fs(a);
    CHECK(fs.add_bundled_fonts() && fs.face_count() == 13);
    FontQuery q;
    q.families = "\"Helvetica Neue\", Arial, sans-serif";
    q.size = 16 * kFixedOne;
    const FontHandle sans = fs.match(q);
    const FontHandle again = fs.match(q);
    CHECK(sans.instance == again.instance && sans.size == 1024);
    GlyphRun run(a);
    CHECK(fs.shape(sans, "Hello", ShapeOptions(), &run) && run.glyphs.count() == 5);
    FaceInfo info;
    CHECK(fs.face_info(run.glyphs[0].face, &info) && info.family.equals("TCOS Sans") && info.weight == 400 && info.style == FontStyle::Normal && info.bundled);
    const TrueTypeFont* tt = fs.font_of(run.glyphs[0].face);
    CHECK(tt && tt->advance_units(run.glyphs[0].glyph) == 1479);  // 'H' of Arial / Liberation Sans
    CHECK(run.glyphs[0].cluster == 0 && run.glyphs[4].cluster == 4 && run.glyphs[4].codepoint == 'o');
    CHECK(near(run.width, 2334, 8) && run.width == fs.measure(sans, "Hello"));  // 4667/2048 em at 16 px
    const ScaledMetrics m = fs.metrics(sans);
    CHECK(m.ascent > 13 * 64 && m.ascent < 15 * 64 && m.descent > 3 * 64 && m.descent < 4 * 64 && m.line_spacing() > 17 * 64 && m.line_spacing() < 19 * 64);
    CHECK(m.zero_advance == tt->advance(tt->glyph_for_codepoint('0'), q.size) && m.x_height > 6 * 64 && m.x_height < 9 * 64);

    // weight and style selection
    FontQuery bold = q; bold.weight = 700;
    GlyphRun r2(a);
    fs.shape(fs.match(bold), "a", ShapeOptions(), &r2);
    CHECK(fs.face_info(r2.glyphs[0].face, &info) && info.weight == 700 && info.style == FontStyle::Normal && info.family.equals("TCOS Sans"));
    FontQuery italic = q; italic.style = FontStyle::Italic;
    fs.shape(fs.match(italic), "a", ShapeOptions(), &r2);
    CHECK(fs.face_info(r2.glyphs[0].face, &info) && info.weight == 400 && info.style == FontStyle::Italic);
    FontQuery oblique_bold = q; oblique_bold.style = FontStyle::Oblique; oblique_bold.weight = 900;
    fs.shape(fs.match(oblique_bold), "a", ShapeOptions(), &r2);
    CHECK(fs.face_info(r2.glyphs[0].face, &info) && info.weight == 700 && info.style == FontStyle::Italic);
    FontQuery light = q; light.weight = 300;
    fs.shape(fs.match(light), "a", ShapeOptions(), &r2);
    CHECK(fs.face_info(r2.glyphs[0].face, &info) && info.weight == 400);  // below 400: lighter first, then heavier

    // generic families and aliases
    FontQuery serif = q; serif.families = "serif";
    fs.shape(fs.match(serif), "a", ShapeOptions(), &r2);
    CHECK(fs.face_info(r2.glyphs[0].face, &info) && info.family.equals("TCOS Serif"));
    FontQuery times = q; times.families = "'Times New Roman', serif";
    fs.shape(fs.match(times), "a", ShapeOptions(), &r2);
    CHECK(fs.face_info(r2.glyphs[0].face, &info) && info.family.equals("TCOS Serif"));
    FontQuery mono = q; mono.families = "monospace";
    const FontHandle mh = fs.match(mono);
    CHECK(fs.measure(mh, "i") == fs.measure(mh, "m") && fs.measure(mh, "iiii") == 4 * fs.measure(mh, "i"));
    CHECK(fs.measure(mh, "i") == fs.measure(fs.match(FontQuery{"Courier New, monospace"}), "m"));
    FontQuery unknown = q; unknown.families = "NoSuchFont"; 
    fs.shape(fs.match(unknown), "a", ShapeOptions(), &r2);
    CHECK(fs.face_info(r2.glyphs[0].face, &info) && info.family.equals("TCOS Serif"));  // the UA's standard font
    unknown.default_generic = GenericFamily::SansSerif;
    fs.shape(fs.match(unknown), "a", ShapeOptions(), &r2);
    CHECK(fs.face_info(r2.glyphs[0].face, &info) && info.family.equals("TCOS Sans"));

    // per-codepoint fallback: Cyrillic is in the sans face, Vietnamese only in the fallback, Hiragana nowhere
    GlyphRun r3(a);
    const char* mixed = "A\xD0\x96\xE1\xBA\xA0\xE3\x81\x82!";  // A, Zhe, A with dot below, hiragana a, !
    CHECK(fs.shape(sans, mixed, ShapeOptions(), &r3) && r3.glyphs.count() == 5);
    CHECK(fs.face_info(r3.glyphs[1].face, &info) && info.family.equals("TCOS Sans"));
    CHECK(fs.face_info(r3.glyphs[2].face, &info) && info.family.equals("TCOS Fallback"));
    CHECK(r3.glyphs[3].face == FontSet::kFaceTofu && r3.glyphs[3].codepoint == 0x3042 && r3.glyphs[3].advance > 0);
    CHECK(r3.glyphs[4].face != FontSet::kFaceTofu && r3.glyphs[2].cluster == 3 && r3.glyphs[4].cluster == 9);
    const GlyphBitmap* tofu = fs.glyph_bitmap(sans, FontSet::kFaceTofu, 0x3042);
    CHECK(tofu && !tofu->empty() && tofu->pixels()[0] == 255);

    // controls, malformed UTF-8
    GlyphRun r4(a);
    fs.shape(sans, "a\xE2\x80\x8B" "b\x01\xC2\xAD" "c", ShapeOptions(), &r4);
    CHECK(r4.glyphs.count() == 3);
    fs.shape(sans, "x\xFF" "y\xC0\x80" "z\xED\xA0\x80", ShapeOptions(), &r4);
    CHECK(r4.glyphs.count() == 9);  // x, FFFD, y, FFFD, FFFD (C0 80), z, then the surrogate ED A0 80 as three FFFD
    CHECK(r4.glyphs[1].codepoint == 0xFFFD);
    fs.shape(sans, "", ShapeOptions(), &r4);
    CHECK(r4.glyphs.count() == 0 && r4.width == 0);
    fs.shape(sans, "a\tb\n", ShapeOptions(), &r4);
    CHECK(r4.glyphs.count() == 4 && r4.glyphs[1].codepoint == ' ');

    // spacing and kerning
    ShapeOptions sp; sp.letter_spacing = 64; sp.word_spacing = 128;
    const Fixed plain = fs.measure(sans, "a b");
    CHECK(fs.measure(sans, "a b", sp) == plain + 3 * 64 + 128);
    ShapeOptions nokern; nokern.kerning = false;
    CHECK(fs.measure(sans, "AV") < fs.measure(sans, "AV", nokern));
    CHECK(fs.measure(sans, "AV", nokern) == fs.measure(sans, "A") + fs.measure(sans, "V"));

    // bitmaps and the cache
    const GlyphBitmap* h = fs.glyph_bitmap(sans, run.glyphs[0].face, run.glyphs[0].glyph);
    CHECK(h && h->width >= 8 && h->width <= 11 && h->height >= 11 && h->height <= 13 && h->top >= 11 && h->top <= 12);
    const uint64_t hits = fs.glyph_cache().hits();
    const GlyphBitmap* h2 = fs.glyph_bitmap(sans, run.glyphs[0].face, run.glyphs[0].glyph);
    CHECK(h2 && fs.glyph_cache().hits() == hits + 1 && h2->width == h->width);
    uint64_t ink = 0;
    for (uint8_t v : h2->pixels()) ink += v;
    CHECK(ink > 255 * 20);
    const GlyphBitmap* shifted = fs.glyph_bitmap(sans, run.glyphs[0].face, run.glyphs[0].glyph, 2);
    CHECK(shifted && shifted->width >= h2->width);
    CHECK(fs.glyph_bitmap(sans, run.glyphs[0].face, 99999) == nullptr);
    FontHandle zero = sans; zero.size = 0;
    CHECK(fs.glyph_bitmap(zero, run.glyphs[0].face, run.glyphs[0].glyph) == nullptr && fs.measure(zero, "abc") == 0);

    // many different requests stay bounded
    for (uint32_t i = 0; i < FontSet::kMaxInstances + 50; ++i) {
      char family[24];
      snprintf(family, sizeof family, "Family%u", i);
      FontQuery qq = q; qq.families = family;
      const FontHandle fh = fs.match(qq);
      if (i == FontSet::kMaxInstances + 10) CHECK(fs.measure(fh, "Hello") > 0);
    }
  }
  CHECK(a.live == 0);
}

static void test_fontset_console_fallback() {
  TestAllocator a;
  {
    FontSet fs(a);  // nothing registered: console font, then tofu
    FontQuery q; q.families = "Arial"; q.size = 16 * kFixedOne;
    const FontHandle f = fs.match(q);
    GlyphRun run(a);
    CHECK(fs.shape(f, "Hi\xC3\xA9", ShapeOptions(), &run) && run.glyphs.count() == 3);
    CHECK(run.glyphs[0].face == FontSet::kFaceBitmap && run.glyphs[1].face == FontSet::kFaceBitmap && run.glyphs[2].face == FontSet::kFaceTofu);
    CHECK(run.glyphs[0].advance == 8 * 64 && fs.measure(f, "Hi") == 16 * 64);
    const ScaledMetrics m = fs.metrics(f);
    CHECK(m.ascent == 12 * 64 && m.descent == 4 * 64);
    const GlyphBitmap* b = fs.glyph_bitmap(f, FontSet::kFaceBitmap, 'H');
    CHECK(b && b->width == 8 && b->height == 16 && b->top == 12 && b->left == 0);
    const ConstByteView rows = hardware::Font8x16::glyph('H');
    bool same = true;
    for (uint32_t y = 0; y < 16; ++y) for (uint32_t x = 0; x < 8; ++x) same = same && ((px(*b, x, y) == 255) == ((rows[y] & (0x80 >> x)) != 0));
    CHECK(same);
    FontQuery big = q; big.size = 32 * kFixedOne;
    const GlyphBitmap* b2 = fs.glyph_bitmap(fs.match(big), FontSet::kFaceBitmap, 'H');
    CHECK(b2 && b2->width == 16 && b2->height == 32 && px(*b2, 2, 2) == px(*b2, 3, 3) );
  }
  CHECK(a.live == 0);
}

static void test_fontset_web_fonts() {
  TestAllocator a;
  {
    FontSet fs(a);
    fs.add_bundled_fonts();
    ByteBuffer sfnt(a);
    build_test_font(sfnt);
    ByteBuffer woff(a);
    build_woff(sfnt.view(), woff, a);

    CodepointRange only_a{0x41, 0x41}, rest{0x42, 0x10FFFF};
    WebFontDescriptor d1; d1.family = "Split"; d1.weight = 400; d1.ranges = ArrayView<const CodepointRange>(&only_a, 1);
    WebFontDescriptor d2; d2.family = "SPLIT"; d2.weight = 700; d2.ranges = ArrayView<const CodepointRange>(&rest, 1);
    uint32_t id1 = 0, id2 = 0;
    CHECK(fs.add_web_font(d1, sfnt.view(), &id1) && fs.add_web_font(d2, woff.view(), &id2) && id1 != id2);
    FontQuery q; q.families = "split, sans-serif"; q.size = 20 * kFixedOne;
    const FontHandle h = fs.match(q);
    GlyphRun run(a);
    fs.shape(h, "ABC", ShapeOptions(), &run);
    CHECK(run.glyphs.count() == 3);
    CHECK(run.glyphs[0].face == id1 && run.glyphs[1].face == id2 && run.glyphs[2].face == id2);  // unicode-range picks the face
    CHECK(run.glyphs[1].glyph == 3 && run.glyphs[2].glyph == 4);
    // 'B' then 'C' are in the same face (A,B across two faces is not kerned)
    CHECK(run.glyphs[0].advance == 14 * 64);
    // letters the web font lacks fall through to the next family
    fs.shape(h, "a", ShapeOptions(), &run);
    FaceInfo info;
    CHECK(fs.face_info(run.glyphs[0].face, &info) && info.family.equals("TCOS Sans"));
    // kerning inside one face: AB in a font where both are in range
    WebFontDescriptor d3; d3.family = "Kern";
    CHECK(fs.add_web_font(d3, sfnt.view()));
    q.families = "Kern";
    const FontHandle k = fs.match(q);
    CHECK(fs.measure(k, "AB") == 14 * 64 + 14 * 64 - 64);  // -50 units at 20 px = -1 px
    ShapeOptions nokern; nokern.kerning = false;
    CHECK(fs.measure(k, "AB", nokern) == 28 * 64);
    // the weight, taken from the font's OS/2 when the descriptor has none
    CHECK(fs.face_info(fs.face_count() - 1, &info) && info.weight == 700 && info.stretch == 100);
    // handles made before the font arrived still resolve
    FontQuery late; late.families = "Late"; late.size = 20 * kFixedOne;
    const FontHandle before = fs.match(late);
    CHECK(fs.measure(before, "A") != 14 * 64);
    WebFontDescriptor d4; d4.family = "Late";
    CHECK(fs.add_web_font(d4, sfnt.view()));
    CHECK(fs.measure(before, "A") == 14 * 64);
    // refusals
    const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(!fs.add_web_font(d4, ConstByteView(junk)));
    CHECK(!fs.add_web_font(d4, ConstByteView()));
    WebFontDescriptor noname;
    CHECK(!fs.add_web_font(noname, sfnt.view()));
    // the glyph bitmap of a web face
    const GlyphBitmap* bm = fs.glyph_bitmap(k, id1, 2);
    CHECK(bm && bm->width == 10 && bm->height == 14);
  }
  CHECK(a.live == 0);
}

// Random damage to the real bundled fonts (which exercise composite glyphs, kerning and the format 4 cmap for real).
static void test_bundled_corruption() {
  TestAllocator a;
  const ArrayView<const BundledFontInfo> sans = bundled_sans_faces();
  const ArrayView<const BundledFontInfo> fallback = bundled_fallback_faces();
  ByteBuffer originals[2] = {ByteBuffer(a), ByteBuffer(a)};
  CHECK(decode_woff(sans[0].woff, &originals[0], 4 << 20) && decode_woff(fallback[0].woff, &originals[1], 4 << 20));
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  for (ByteBuffer& original : originals) {
    TrueTypeFont clean(a);
    CHECK(clean.parse(original.view()) && clean.glyph_count() > 800);
    ByteBuffer copy(a);
    copy.append(original.view());
    ByteView w = copy.writable_view();
    for (int i = 0; i < 40; ++i) exercise(original.view().first(rnd() % original.size()), a);
    for (int i = 0; i < 400; ++i) {
      const uint64_t at = rnd() % w.size();
      const uint8_t bit = static_cast<uint8_t>(1 << (rnd() % 8));
      w[at] ^= bit;
      exercise(copy.view(), a);
      w[at] ^= bit;
    }
    // damage concentrated on the table directory and the start of each table, where the structure is
    for (uint64_t at = 0; at < 12 + 16 * 12 && at < w.size(); ++at) {
      for (int bit = 0; bit < 8; ++bit) {
        w[at] ^= static_cast<uint8_t>(1 << bit);
        exercise(copy.view(), a);
        w[at] ^= static_cast<uint8_t>(1 << bit);
      }
    }
  }
  // every bundled face decodes, parses and has the basic Latin letters
  const ArrayView<const BundledFontInfo> families[4] = {bundled_sans_faces(), bundled_serif_faces(), bundled_mono_faces(), bundled_fallback_faces()};
  for (const ArrayView<const BundledFontInfo>& family : families) {
    for (const BundledFontInfo& info : family) {
      ByteBuffer sfnt(a);
      TrueTypeFont f(a);
      CHECK(font_to_sfnt(info.woff, &sfnt, 4 << 20) && f.parse(sfnt.view()));
      CHECK(f.has_codepoint('A') && f.has_codepoint('z') && f.has_codepoint('0') && f.has_codepoint(0x20));
      Outline o(a);
      CHECK(f.load_outline(f.glyph_for_codepoint('g'), &o) && o.points.count() > 10);
      CHECK(f.metrics().ascent > 0 && f.metrics().descent > 0);
    }
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_parse_and_metrics();
  test_outlines();
  test_raster();
  test_corrupt_fonts();
  test_woff();
  test_cache();
  test_unicode_range();
  test_fontset_bundled();
  test_fontset_console_fallback();
  test_fontset_web_fonts();
  test_bundled_corruption();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("font tests passed\n");
  return 0;
}
