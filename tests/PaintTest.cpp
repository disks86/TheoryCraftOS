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

// Host-side tests for painting: a page is parsed, styled, laid out with the bundled fonts, turned into a display list and drawn with the software
// renderer; the tests look at pixels (colours of boxes, borders, clips, opacity, stacking order, gradients, text) and at golden images.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "Browser/Renderer.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Layout/BoxBuilder.hpp"
#include "Layout/FontMeasurer.hpp"
#include "Layout/Layout.hpp"
#include "Media/Image.hpp"
#include "Paint/DisplayPainter.hpp"
#include "Paint/HitTest.hpp"
#include "Paint/PaintBuilder.hpp"
#include "PixelGolden.hpp"

using namespace layout;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static TestAllocator g_alloc;
static font::FontSet* g_fonts = nullptr;

// A page drawn at `width` x `height`.
struct Shot {
  dom::Document doc;
  browser::StyleSheet sheet;
  BoxTree tree;
  paint::DisplayList list;
  media::Image image;
  uint32_t width, height;

  Shot(const std::string& html, uint32_t w = 200, uint32_t h = 100) : doc(g_alloc), sheet(g_alloc), tree(g_alloc), list(g_alloc), image(g_alloc), width(w), height(h) {
    dom::build_document(doc, html.c_str(), nullptr);
    browser::RenderOptions render;
    render.media.width = static_cast<int32_t>(w);
    render.media.height = static_cast<int32_t>(h);
    browser::collect_style_sheets(doc, render, &sheet);
    sheet.set_viewport(static_cast<int32_t>(w), static_cast<int32_t>(h));
    FontMeasurer measurer(*g_fonts, &sheet);
    BuildOptions build;
    build_box_tree(doc, sheet, build, &tree);
    LayoutOptions options;
    options.viewport_width = LayoutUnit::from_px(w);
    options.viewport_height = LayoutUnit::from_px(h);
    options.measurer = &measurer;
    options.sheet = &sheet;
    layout_tree(tree, options);
    paint::PaintOptions po;
    po.measurer = &measurer;
    po.sheet = &sheet;
    po.doc = &doc;
    paint::build_display_list(tree, po, &list);
    image.create(w, h);
    graphics::Surface surface = image.surface(0);
    graphics::SoftwareRenderer2D renderer(surface, nullptr, &g_alloc);
    const uint32_t paper = list.background() ? list.background() : 0xFFFFFFFFu;
    renderer.set_blend(graphics::BlendMode::Replace);
    renderer.fill_rect(graphics::Rect2D{0, 0, w, h}, paper);
    paint::DisplayPainter painter(g_alloc, *g_fonts, nullptr);
    graphics::Rect2D view{0, 0, w, h};
    painter.paint(renderer, list, 0, 0, view);
  }
  uint32_t px(uint32_t x, uint32_t y) const { return image.frame(0)[y * width + x]; }
  uint32_t rgb(uint32_t x, uint32_t y) const { return px(x, y) & 0x00FFFFFFu; }
  // how many pixels of the area are not the colour `background` (text and the like)
  int ink(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t background = 0xFFFFFFFFu) const {
    int n = 0;
    for (uint32_t y = y0; y < y1; ++y)
      for (uint32_t x = x0; x < x1; ++x)
        if (px(x, y) != background) ++n;
    return n;
  }
};

static std::string page(const char* body, const char* css = "") {
  return std::string("<style>html,body,div,p{margin:0;padding:0}") + css + "</style>" + body;
}

static bool near_color(uint32_t actual, uint32_t expected, int tolerance) {
  for (int shift = 0; shift < 24; shift += 8) {
    const int a = static_cast<int>((actual >> shift) & 0xFF), e = static_cast<int>((expected >> shift) & 0xFF);
    if (a - e > tolerance || e - a > tolerance) return false;
  }
  return true;
}

static void test_boxes() {
  Shot a(page("<div style='width:100px;height:50px;background:#ff0000'></div>"));
  CHECK(a.rgb(10, 10) == 0xFF0000u && a.rgb(99, 49) == 0xFF0000u && a.rgb(100, 10) == 0xFFFFFFu && a.rgb(10, 50) == 0xFFFFFFu);

  // a border takes the edge; the background lies under it
  Shot b(page("<div style='width:80px;height:40px;background:#00ff00;border:4px solid #0000ff'></div>"));
  CHECK(b.rgb(1, 20) == 0x0000FFu && b.rgb(40, 1) == 0x0000FFu && b.rgb(40, 20) == 0x00FF00u && b.rgb(87, 20) == 0x0000FFu && b.rgb(40, 47) == 0x0000FFu && b.rgb(40, 48) == 0xFFFFFFu);

  // different sides, different colours
  Shot c(page("<div style='width:40px;height:20px;border-style:solid;border-width:5px;border-color:#f00 #0f0 #00f #ff0'></div>"));
  CHECK(c.rgb(25, 2) == 0xFF0000u && c.rgb(47, 20) == 0x00FF00u && c.rgb(25, 27) == 0x0000FFu && c.rgb(2, 15) == 0xFFFF00u);

  // rounded corners leave the corner pixel alone; the middle is coloured
  Shot d(page("<div style='width:60px;height:60px;background:#ff0000;border-radius:30px'></div>"));
  CHECK(d.rgb(0, 0) == 0xFFFFFFu && d.rgb(30, 30) == 0xFF0000u && d.rgb(30, 1) == 0xFF0000u && d.rgb(59, 59) == 0xFFFFFFu);

  // the body's background is the page's canvas, all of it
  Shot e(page("<div style='width:10px;height:10px'></div>", "body{background:#123456}"));
  CHECK(e.rgb(150, 80) == 0x123456u && e.rgb(2, 2) == 0x123456u);

  // half-transparent colours blend
  Shot f(page("<div style='width:50px;height:50px;background:rgba(255,0,0,0.5)'></div>"));
  CHECK(near_color(f.rgb(10, 10), 0xFF7F7Fu, 2));
}

static void test_stacking() {
  // z-index: the higher box is on top even though it comes first
  Shot a(page("<div style='position:absolute;left:0;top:0;width:60px;height:60px;background:#f00;z-index:2'></div>"
              "<div style='position:absolute;left:30px;top:30px;width:60px;height:60px;background:#00f;z-index:1'></div>"));
  CHECK(a.rgb(40, 40) == 0xFF0000u && a.rgb(80, 80) == 0x0000FFu);
  // without z-index later boxes are on top
  Shot b(page("<div style='position:absolute;left:0;top:0;width:60px;height:60px;background:#f00'></div>"
              "<div style='position:absolute;left:30px;top:30px;width:60px;height:60px;background:#00f'></div>"));
  CHECK(b.rgb(40, 40) == 0x0000FFu);
  // a positioned box is above in-flow content, even a later block's background
  Shot c(page("<div style='position:relative;width:50px;height:50px;background:#f00'></div><div style='margin-top:-30px;width:80px;height:80px;background:#0f0'></div>"));
  CHECK(c.rgb(10, 40) == 0xFF0000u && c.rgb(60, 40) == 0x00FF00u);
  // a negative z-index goes under the parent's background
  Shot d(page("<div style='position:relative;width:80px;height:80px;background:#0f0'><div style='position:absolute;z-index:-1;left:20px;top:20px;width:80px;height:80px;background:#f00'></div></div>"));
  CHECK(d.rgb(40, 40) == 0x00FF00u && d.rgb(90, 90) == 0xFF0000u);
  // floats are above block backgrounds
  Shot e(page("<div style='width:100px;height:50px;background:#00f'><div style='float:left;width:30px;height:30px;background:#f00'></div></div>"));
  CHECK(e.rgb(10, 10) == 0xFF0000u && e.rgb(50, 10) == 0x0000FFu);
}

static void test_clip_and_opacity() {
  // overflow:hidden clips children to the padding box
  Shot a(page("<div style='overflow:hidden;width:50px;height:50px;background:#ccc'><div style='width:100px;height:100px;background:#f00'></div></div>"));
  CHECK(a.rgb(40, 40) == 0xFF0000u && a.rgb(60, 40) == 0xFFFFFFu && a.rgb(40, 60) == 0xFFFFFFu);
  // with a radius the clip is round
  Shot b(page("<div style='overflow:hidden;width:60px;height:60px;border-radius:30px'><div style='width:100px;height:100px;background:#f00'></div></div>"));
  CHECK(b.rgb(1, 1) == 0xFFFFFFu && b.rgb(30, 30) == 0xFF0000u);
  // opacity draws a box and its content as a group
  Shot c(page("<div style='opacity:0.5;width:50px;height:50px;background:#000'></div>"));
  CHECK(near_color(c.rgb(10, 10), 0x7F7F7Fu, 3) && c.rgb(60, 10) == 0xFFFFFFu);
  Shot d(page("<div style='opacity:0;width:50px;height:50px;background:#000'></div>"));
  CHECK(d.rgb(10, 10) == 0xFFFFFFu);
  // visibility:hidden keeps the space but draws nothing; a visible child shows
  Shot e(page("<div style='visibility:hidden;width:50px;height:50px;background:#000'><div style='visibility:visible;width:20px;height:20px;background:#f00'></div></div>"));
  CHECK(e.rgb(40, 40) == 0xFFFFFFu && e.rgb(10, 10) == 0xFF0000u);
}

static void test_gradients_and_shadows() {
  Shot a(page("<div style='width:100px;height:20px;background:linear-gradient(to right,#ff0000,#0000ff)'></div>"));
  CHECK(near_color(a.rgb(2, 10), 0xFA0005u, 12) && near_color(a.rgb(97, 10), 0x0500FAu, 12) && near_color(a.rgb(50, 10), 0x7F007Fu, 12));
  Shot b(page("<div style='width:20px;height:100px;background:linear-gradient(#fff,#000)'></div>"));
  CHECK((b.rgb(10, 2) & 0xFF) > 0xF0 && (b.rgb(10, 97) & 0xFF) < 0x10);
  Shot c(page("<div style='width:100px;height:100px;background:radial-gradient(circle,#f00,#00f)'></div>"));
  CHECK(near_color(c.rgb(50, 50), 0xFF0000u, 12) && (c.rgb(2, 2) & 0xFF) > 0xA0);
  // a shadow darkens what is next to the box
  Shot d(page("<div style='margin:20px;width:40px;height:40px;background:#fff;box-shadow:6px 6px 0 #000'></div>"));
  CHECK(d.rgb(65, 65) == 0x000000u && d.rgb(30, 30) == 0xFFFFFFu && d.rgb(70, 30) == 0xFFFFFFu);
}

static void test_text() {
  // text leaves marks inside its line box and nowhere else; bigger text covers more
  Shot a(page("<div style='font:16px sans-serif;color:#000'>Hello</div>", "div{width:200px}"));
  CHECK(a.ink(0, 0, 60, 20) > 30 && a.ink(0, 30, 200, 100) == 0 && a.ink(70, 0, 200, 30) == 0);
  Shot b(page("<div style='font:32px sans-serif;color:#000'>Hello</div>"));
  CHECK(b.ink(0, 0, 200, 40) > a.ink(0, 0, 200, 20) * 2);
  // colour
  Shot c(page("<div style='font:32px sans-serif;color:#ff0000'>HHH</div>"));
  bool found_red = false;
  for (uint32_t y = 0; y < 40; ++y)
    for (uint32_t x = 0; x < 100; ++x)
      if (c.rgb(x, y) == 0xFF0000u) found_red = true;
  CHECK(found_red);
  // links are blue and underlined
  Shot d(page("<a href='#' style='font:16px sans-serif'>link</a>"));
  bool blue = false, underline = false;
  for (uint32_t y = 0; y < 30; ++y)
    for (uint32_t x = 0; x < 40; ++x)
      if ((d.rgb(x, y) & 0xFF) > 0xA0 && ((d.rgb(x, y) >> 16) & 0xFF) < 0x40) blue = true;
  for (uint32_t y = 12; y < 24; ++y) {
    int run = 0;
    for (uint32_t x = 0; x < 40; ++x) run += (d.rgb(x, y) & 0xFF) > 0xA0 && ((d.rgb(x, y) >> 16) & 0xFF) < 0x40 ? 1 : 0;
    if (run >= 20) underline = true;
  }
  CHECK(blue && underline);
  // text is not drawn outside an overflow clip
  Shot e(page("<div style='overflow:hidden;width:30px;height:20px;font:16px sans-serif'>HHHHHHHHHHHH</div>"));
  CHECK(e.ink(0, 0, 30, 20) > 20 && e.ink(40, 0, 200, 100) == 0);
}

static void test_hit_testing() {
  Shot a(page("<div id=a style='width:100px;height:50px'><div id=b style='width:40px;height:20px;background:#eee'></div></div>"));
  paint::HitResult r;
  CHECK(paint::hit_test(a.list, &a.doc, 10, 10, &r) && r.node == a.doc.element_by_id("b")->serial);
  CHECK(paint::hit_test(a.list, &a.doc, 60, 30, &r) && r.node == a.doc.element_by_id("a")->serial);   // an element with no paint of its own is still found
  CHECK(paint::hit_test(a.list, &a.doc, 150, 10, &r) && r.node != a.doc.element_by_id("a")->serial);   // (outside: only the page's own boxes)
  // the topmost box wins, by z-index
  Shot b(page("<div id=p style='position:absolute;left:0;top:0;width:60px;height:60px;z-index:1'></div><div id=q style='position:absolute;left:30px;top:30px;width:60px;height:60px;z-index:2'></div>"));
  CHECK(paint::hit_test(b.list, &b.doc, 40, 40, &r) && r.node == b.doc.element_by_id("q")->serial);
  CHECK(paint::hit_test(b.list, &b.doc, 10, 10, &r) && r.node == b.doc.element_by_id("p")->serial);
  // what overflow hides cannot be clicked
  Shot c(page("<div style='overflow:hidden;width:50px;height:50px'><div id=k style='width:100px;height:100px'></div></div>"));
  CHECK(paint::hit_test(c.list, &c.doc, 20, 20, &r) && r.node == c.doc.element_by_id("k")->serial);
  CHECK(!(paint::hit_test(c.list, &c.doc, 80, 20, &r) && r.node == c.doc.element_by_id("k")->serial));
  // text finds its element and the link around it; the cursor comes from the style
  Shot d(page("<p><a id=l href='#' style='font:16px sans-serif;cursor:pointer'>link <b id=bb>bold</b></a></p>"));
  CHECK(paint::hit_test(d.list, &d.doc, 8, 8, &r) && r.text && r.link == d.doc.element_by_id("l")->serial && r.node == d.doc.element_by_id("l")->serial);
  CHECK(paint::hit_test(d.list, &d.doc, 45, 8, &r) && r.node == d.doc.element_by_id("bb")->serial && r.link == d.doc.element_by_id("l")->serial);
  // pointer-events:none lets the click through
  Shot e(page("<div id=a style='width:50px;height:50px'><div id=b style='width:50px;height:50px;pointer-events:none'></div></div>"));
  CHECK(paint::hit_test(e.list, &e.doc, 10, 10, &r) && r.node == e.doc.element_by_id("a")->serial);
}

static void test_golden() {
  // a small page that uses most of it, compared with the picture made when it was last checked by eye
  Shot a(std::string("<style>body{margin:0;background:#f4f4f4;font-family:sans-serif}"
                     ".card{margin:12px;padding:10px 14px;background:#fff;border:1px solid #c0c0c0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.25);width:240px}"
                     "h1{margin:0 0 6px;font-size:20px;color:#1a73e8}p{margin:0;font-size:13px;color:#333;line-height:1.4}"
                     ".btn{display:inline-block;margin-top:8px;padding:4px 12px;background:linear-gradient(#4a90e2,#2a62b2);color:#fff;border-radius:4px;font-size:13px}"
                     ".pill{float:right;background:#e8f0fe;color:#1a73e8;border-radius:10px;padding:0 8px;font-size:11px}</style>"
                     "<div class=card><span class=pill>new</span><h1>Pixel layout</h1><p>Text is set in real fonts, boxes have borders, radii, shadows and gradients.</p>"
                     "<span class=btn>Open page</span></div>"),
         280, 170);
  const golden::Report r = golden::check(g_alloc, "paint_card", ArrayView<const uint32_t>(a.image.frame(0).data(), a.image.frame(0).size()), a.width, a.height, 24);
  if (!r.ok) printf("%s\n", r.message.c_str());
  CHECK(r.ok);
}

int main() {
  {
    font::FontSet fonts(g_alloc);
    fonts.add_bundled_fonts();
    g_fonts = &fonts;
    test_boxes();
    test_stacking();
    test_clip_and_opacity();
    test_gradients_and_shadows();
    test_text();
    test_hit_testing();
    test_golden();
    g_fonts = nullptr;
  }
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("paint tests passed\n");
  return 0;
}
