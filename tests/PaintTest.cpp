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
#include <vector>
#include "Browser/Renderer.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Graphics/Graphics2D/Device/DeviceRenderer2D.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Graphics/Graphics3D/Software/SoftwareDevice.hpp"
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

  Shot(const std::string& html, uint32_t w = 200, uint32_t h = 100, browser::IObjectPainter* pictures = nullptr, bool focus_input = false, bool caret_hidden = false) : doc(g_alloc), sheet(g_alloc), tree(g_alloc), list(g_alloc), image(g_alloc), width(w), height(h) {
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
    po.caret_hidden = caret_hidden;
    if (focus_input)
      for (uint32_t serial = 1; serial < 64; ++serial) {
        const dom::Node* n = doc.by_serial(serial);
        if (n && n->is_tag("input")) { po.focus = serial; break; }
      }
    paint::build_display_list(tree, po, &list);
    image.create(w, h);
    graphics::Surface surface = image.surface(0);
    graphics::SoftwareRenderer2D renderer(surface, nullptr, &g_alloc);
    const uint32_t paper = list.background() ? list.background() : 0xFFFFFFFFu;
    renderer.set_blend(graphics::BlendMode::Replace);
    renderer.fill_rect(graphics::Rect2D{0, 0, w, h}, paper);
    paint::DisplayPainter painter(g_alloc, *g_fonts, pictures);
    graphics::Rect2D view{0, 0, w, h};
    painter.paint(renderer, list, 0, 0, view);
  }
  uint32_t px(uint32_t x, uint32_t y) const { return image.frame(0)[y * width + x]; }
  // The same list drawn by the device renderer (on the software device): its offscreen groups move the transformed content. Returns the frame.
  std::vector<uint32_t> via_device(browser::IObjectPainter* pictures = nullptr) const {
    graphics::SoftwareDevice device(g_alloc, nullptr);
    graphics::DeviceRenderer2D renderer(device, g_alloc, {width, height});
    std::vector<uint32_t> frame(static_cast<size_t>(width) * height, 0);
    if (!renderer.valid()) return frame;
    const uint32_t paper = list.background() ? list.background() : 0xFFFFFFFFu;
    renderer.set_blend(graphics::BlendMode::Replace);
    renderer.fill_rect(graphics::Rect2D{0, 0, width, height}, paper);
    paint::DisplayPainter painter(g_alloc, *g_fonts, pictures);
    painter.paint(renderer, list, 0, 0, graphics::Rect2D{0, 0, width, height});
    renderer.read_pixels(ArrayView<uint32_t>(frame.data(), frame.size()));
    return frame;
  }
  // For looking at a picture: with TCOS_DUMP=<directory> the page is written there as <name>.png
  void dump(const char* name) const {
    const char* dir = getenv("TCOS_DUMP");
    if (!dir || !*dir) return;
    golden::detail::make_dirs(dir);
    golden::detail::write_png(g_alloc, std::string(dir) + "/" + name + ".png", ArrayView<const uint32_t>(image.frame(0).data(), image.frame(0).size()), width, height);
  }
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

// The caret of a focused text field: a bar after the typed text that is left out in the hidden half of its blink (and nothing else moves then).
static void test_caret() {
  const std::string html = page("<input type=text value=\"ab\" style=\"width:100px\">");
  Shot plain(html, 200, 60);
  Shot shown(html, 200, 60, nullptr, true, false);
  Shot hidden(html, 200, 60, nullptr, true, true);
  CHECK(shown.ink(0, 0, 200, 60) > plain.ink(0, 0, 200, 60));   // the bar is extra ink
  CHECK(hidden.ink(0, 0, 200, 60) == plain.ink(0, 0, 200, 60));
  int different = 0;
  for (uint32_t y = 0; y < 60; ++y)
    for (uint32_t x = 0; x < 200; ++x) different += hidden.px(x, y) != plain.px(x, y);
  CHECK(different == 0);
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

// ---- transforms, filters, shadows, gradients, clip-path, borders, pictures -------------------------------------------------------------------------------


static bool is_white(uint32_t rgb) { return rgb == 0xFFFFFFu; }
static int luma(uint32_t rgb) { return static_cast<int>(((rgb >> 16) & 0xFF) * 3 + ((rgb >> 8) & 0xFF) * 6 + (rgb & 0xFF)) / 10; }

// Transformed content drawn through the device renderer's offscreen groups agrees with the CPU path (edges and filtering differ a little).
static void test_transforms_on_device() {
  const char* pages[] = {
      "<div style='margin:10px;width:20px;height:20px;background:#f00;transform:scale(2);transform-origin:0 0'></div>",
      "<div id=a style='margin:40px 0 0 40px;width:40px;height:20px;background:#f00;transform:rotate(90deg)'></div>",
      "<div style='margin:30px;width:40px;height:40px;background:#f00;transform:rotate(45deg)'></div>",
      "<div style='margin:10px;width:60px;height:40px;background:#08f;border:3px solid #000;transform:rotate(-20deg);opacity:.5'><div style='width:20px;height:20px;background:#0c0'></div></div>",
      "<div style='margin:20px;width:60px;height:30px;transform:rotate(10deg);color:#000'>Transform</div>",
  };
  {
    graphics::SoftwareDevice device(g_alloc, nullptr);
    graphics::DeviceRenderer2D renderer(device, g_alloc, {16, 16});
    CHECK(renderer.valid() && renderer.begin_group(graphics::Rect2D{0, 0, 8, 8}) && renderer.end_group(graphics::Matrix2D(), 255, graphics::ImageFilter::Nearest));   // (the pages below do use groups)
  }
  for (const char* body : pages) {
    Shot cpu(page(body), 120, 100);
    const std::vector<uint32_t> gpu = cpu.via_device();
    uint32_t far = 0, inked = 0;
    for (uint32_t y = 0; y < cpu.height; ++y)
      for (uint32_t x = 0; x < cpu.width; ++x) {
        const uint32_t a = cpu.px(x, y), b = gpu[y * cpu.width + x];
        if (a != 0xFFFFFFFFu) ++inked;
        if (!near_color(a, b, 40)) ++far;
      }
    CHECK(inked > 100);
    CHECK(far * 5 < inked + 40);   // (only edge pixels of the pictures may differ: the quad's edges are not anti-aliased)
  }
}

// CSS filters drawn through the device renderer's group filters agree with the CPU filters.
static void test_filters_on_device() {
  const char* pages[] = {
      "<div style='margin:20px;width:40px;height:30px;background:#e22;filter:blur(3px)'></div>",
      "<div style='margin:20px;width:40px;height:30px;background:#e22;border:4px solid #139;filter:grayscale(1)'></div>",
      "<div style='margin:20px;width:40px;height:30px;background:#2a6;filter:brightness(1.5) contrast(.8)'></div>",
      "<div style='margin:20px;width:40px;height:30px;background:linear-gradient(90deg,#f00,#00f);filter:hue-rotate(90deg) saturate(2)'></div>",
      "<div style='margin:20px;width:40px;height:30px;background:#2a6;filter:sepia(1) invert(.5) opacity(.6)'></div>",
      "<div style='margin:30px;width:30px;height:20px;background:#0a0;filter:blur(2px) brightness(1.3)'></div>",
  };
  for (const char* body : pages) {
    Shot cpu(page(body), 120, 100);
    const std::vector<uint32_t> gpu = cpu.via_device();
    uint32_t far = 0, inked = 0;
    for (uint32_t y = 0; y < cpu.height; ++y)
      for (uint32_t x = 0; x < cpu.width; ++x) {
        const uint32_t a = cpu.px(x, y), b = gpu[y * cpu.width + x];
        if (a != 0xFFFFFFFFu) ++inked;
        if (!near_color(a, b, 24)) ++far;
      }
    CHECK(inked > 100);
    if (far * 10 >= inked + 40) printf("filter page differs: far %u inked %u: %s\n", far, inked, body);
    CHECK(far * 10 < inked + 40);
  }
}

// Rounded clips and clip-path shapes through the device renderer (a group laid over the target through the clip's coverage) agree with the CPU's clips.
static void test_clips_on_device() {
  const char* pages[] = {
      "<div style='margin:10px;width:70px;height:60px;border-radius:25px;overflow:hidden;background:#fff'><div style='width:100px;height:100px;background:#e22'></div></div>",
      "<div style='margin:10px;width:70px;height:60px;border-radius:12px 30px 5px 40px;overflow:hidden'><div style='width:100px;height:100px;background:linear-gradient(90deg,#00f,#0c0)'></div></div>",
      "<div style='margin:10px;width:80px;height:70px;background:#e22;clip-path:polygon(50% 0,100% 100%,0 100%)'></div>",
      "<div style='margin:10px;width:80px;height:70px;background:#139;clip-path:circle(35px at 40px 35px)'></div>",
      "<div style='margin:10px;width:90px;height:80px;border-radius:30px;overflow:hidden;background:#ccc'><div style='margin:5px;width:60px;height:60px;border-radius:20px;overflow:hidden;background:#0a0'><div style='width:100px;height:30px;background:#fa0'></div></div></div>",
  };
  for (const char* body : pages) {
    Shot cpu(page(body), 120, 100);
    const std::vector<uint32_t> gpu = cpu.via_device();
    uint32_t far = 0, inked = 0;
    for (uint32_t y = 0; y < cpu.height; ++y)
      for (uint32_t x = 0; x < cpu.width; ++x) {
        const uint32_t a = cpu.px(x, y), b = gpu[y * cpu.width + x];
        if (a != 0xFFFFFFFFu) ++inked;
        if (!near_color(a, b, 12)) ++far;
      }
    CHECK(inked > 500);
    if (far * 50 >= inked) printf("clip page differs: far %u inked %u: %s\n", far, inked, body);
    CHECK(far * 50 < inked);
  }
}

// Box shadows through the device renderer (blurred in a filter group, the box cut out of it) agree with the CPU's.
static void test_shadows_on_device() {
  const char* pages[] = {
      "<div style='margin:30px;width:50px;height:30px;background:#fff;box-shadow:6px 8px 10px #000'></div>",
      "<div style='margin:30px;width:50px;height:30px;background:#fff;border-radius:14px;box-shadow:0 0 12px 4px rgba(200,0,0,.8)'></div>",
      "<div style='margin:30px;width:50px;height:30px;background:#ffc;box-shadow:inset 0 0 10px #000'></div>",
      "<div style='margin:30px;width:50px;height:30px;background:#ffc;border-radius:12px;box-shadow:inset 4px 4px 8px 2px #139'></div>",
      "<div style='margin:30px;width:50px;height:30px;background:#fff;box-shadow:3px 3px 0 #000, 0 0 16px #06c'></div>",
  };
  for (const char* body : pages) {
    Shot cpu(page(body), 120, 100);
    const std::vector<uint32_t> gpu = cpu.via_device();
    uint32_t far = 0, inked = 0;
    for (uint32_t y = 0; y < cpu.height; ++y)
      for (uint32_t x = 0; x < cpu.width; ++x) {
        const uint32_t a = cpu.px(x, y), b = gpu[y * cpu.width + x];
        if (a != 0xFFFFFFFFu) ++inked;
        if (!near_color(a, b, 28)) ++far;
      }
    CHECK(inked > 300);
    if (far * 25 >= inked) printf("shadow page differs: far %u inked %u: %s\n", far, inked, body);
    CHECK(far * 25 < inked);
  }
}

// text-shadow blur and filter: drop-shadow() through the device renderer's group filters agree with the CPU's.
static void test_text_and_drop_shadows_on_device() {
  const char* pages[] = {
      "<div style='margin:20px;font:24px sans-serif;color:#fff;background:#222;width:90px;height:40px;text-shadow:2px 2px 4px #f00'>Shade</div>",
      "<div style='margin:20px;font:26px sans-serif;color:#036;text-shadow:0 0 6px #fa0, 1px 1px 0 #000'>Glow</div>",
      "<div style='margin:25px;width:40px;height:30px;background:#e22;filter:drop-shadow(6px 5px 4px #000)'></div>",
      "<div style='margin:20px;width:40px;height:30px;border-radius:15px;background:#2a6;filter:drop-shadow(0 0 6px rgba(0,0,200,.9)) brightness(1.2)'></div>",
      "<div style='margin:25px;width:30px;height:30px;background:#e22;filter:drop-shadow(4px 4px 0 #00f)'></div>",
  };
  for (const char* body : pages) {
    Shot cpu(page(body), 140, 100);
    const std::vector<uint32_t> gpu = cpu.via_device();
    uint32_t far = 0, inked = 0;
    for (uint32_t y = 0; y < cpu.height; ++y)
      for (uint32_t x = 0; x < cpu.width; ++x) {
        const uint32_t a = cpu.px(x, y), b = gpu[y * cpu.width + x];
        if (a != 0xFFFFFFFFu) ++inked;
        if (!near_color(a, b, 40)) ++far;
      }
    CHECK(inked > 200);
    if (far * 12 >= inked) printf("shadow page differs: far %u inked %u: %s\n", far, inked, body);
    CHECK(far * 12 < inked);
  }
}

// Conic and hinted gradients through the device renderer (wedges, extra stops) agree with the CPU's pixel by pixel colors, to a small error.
static void test_special_gradients_on_device() {
  const char* pages[] = {
      "<div style='width:100px;height:100px;background:conic-gradient(#f00 0%,#f00 25%,#0f0 25%,#0f0 50%,#00f 50%,#00f 75%,#ff0 75%,#ff0 100%)'></div>",
      "<div style='width:100px;height:100px;background:conic-gradient(from 40deg at 30% 60%,#000,#fff,#08f,#000)'></div>",
      "<div style='width:100px;height:100px;background:repeating-conic-gradient(#f00 0 10%,#00f 10% 20%)'></div>",
      "<div style='width:80px;height:80px;border-radius:40px;background:conic-gradient(#f00,#00f)'></div>",
      "<div style='width:100px;height:30px;background:linear-gradient(90deg,#f00,30%,#00f)'></div>",
      "<div style='width:100px;height:60px;background:radial-gradient(circle at 50% 50%,#fff,70%,#036)'></div>",
      "<div style='width:100px;height:30px;background:repeating-linear-gradient(90deg,#f00,20%,#00f 40px)'></div>",
  };
  for (const char* body : pages) {
    Shot cpu(page(body), 120, 120);
    const std::vector<uint32_t> gpu = cpu.via_device();
    uint32_t far = 0, inked = 0;
    for (uint32_t y = 0; y < cpu.height; ++y)
      for (uint32_t x = 0; x < cpu.width; ++x) {
        const uint32_t a = cpu.px(x, y), b = gpu[y * cpu.width + x];
        if (a != 0xFFFFFFFFu) ++inked;
        if (!near_color(a, b, 14)) ++far;
      }
    CHECK(inked > 2000);
    if (far * 40 > inked) printf("gradient page differs: far %u inked %u: %s\n", far, inked, body);
    CHECK(far * 40 <= inked);   // (hard edges and the rim of the shapes may differ by a pixel)
  }
}

// Border line styles: dashes leave gaps, dots are round or square, double has two lines, groove and ridge two shades. With TCOS_HASH set the frames are hashed to the
// output (to compare two builds of the painter).
static void test_border_styles() {
  const char* styles[] = {"dashed", "dotted", "double", "groove", "ridge", "inset", "outset", "solid"};
  for (const char* style : styles) {
    for (const int width : {1, 2, 3, 6}) {
      for (const char* extra : {"", "border-radius:12px;", "border-color:#c00 #0a0 #00c #aa0;", "border-top-width:0;"}) {
        char body[400];
        snprintf(body, sizeof body, "<div style='margin:10px;width:90px;height:50px;background:#fff;border:%dpx %s #036;%s'></div>", width, style, extra);
        Shot s(page(body), 130, 90);
        if (getenv("TCOS_HASH")) {
          uint64_t h = 1469598103934665603ull;
          for (uint32_t y = 0; y < s.height; ++y)
            for (uint32_t x = 0; x < s.width; ++x) h = (h ^ s.px(x, y)) * 1099511628211ull;
          printf("HASH %s %d [%s] %016llx\n", style, width, extra, static_cast<unsigned long long>(h));
        }
      }
    }
  }
  // the straight part of a dashed side has ink and gaps
  Shot d(page("<div style='margin:10px;width:90px;height:50px;border:3px dashed #000'></div>"), 130, 90);
  int inked = 0, gaps = 0;
  for (uint32_t x = 30; x < 90; ++x) (d.px(x, 11) != 0xFFFFFFFFu ? inked : gaps)++;
  CHECK(inked > 15 && gaps > 10);
  Shot o(page("<div style='margin:10px;width:90px;height:50px;border:6px double #000'></div>"), 130, 90);
  CHECK(o.px(50, 10) != 0xFFFFFFFFu && o.px(50, 12) == 0xFFFFFFFFu && o.px(50, 15) != 0xFFFFFFFFu);   // two lines with a gap between
}

static void test_transforms() {
  // translate
  Shot a(page("<div style='width:40px;height:40px;background:#f00;transform:translate(50px,20px)'></div>"));
  CHECK(is_white(a.rgb(10, 10)) && a.rgb(55, 25) == 0xFF0000u && a.rgb(89, 59) == 0xFF0000u && is_white(a.rgb(90, 30)) && is_white(a.rgb(49, 30)) && is_white(a.rgb(60, 19)) && is_white(a.rgb(60, 60)));
  // a scale about the top left lands on whole pixels: exact edges, no blur
  Shot b(page("<div style='margin:10px;width:20px;height:20px;background:#f00;transform:scale(2);transform-origin:0 0'></div>"));
  CHECK(b.rgb(10, 10) == 0xFF0000u && b.rgb(49, 49) == 0xFF0000u && is_white(b.rgb(50, 30)) && is_white(b.rgb(30, 50)) && is_white(b.rgb(9, 30)) && is_white(b.rgb(30, 9)));
  // a scale about the centre (the default origin), down: (20,20)-(60,60) becomes (30,30)-(50,50)
  Shot c(page("<div style='margin:20px;width:40px;height:40px;background:#f00;transform:scale(.5)'></div>"));
  CHECK(c.rgb(30, 30) == 0xFF0000u && c.rgb(49, 49) == 0xFF0000u && is_white(c.rgb(29, 40)) && is_white(c.rgb(50, 40)) && is_white(c.rgb(40, 29)) && is_white(c.rgb(40, 50)) && is_white(c.rgb(22, 22)));
  // transform-origin
  Shot d(page("<div style='margin:50px;width:20px;height:20px;background:#f00;transform:scale(2);transform-origin:100% 100%'></div>"));
  CHECK(d.rgb(30, 30) == 0xFF0000u && d.rgb(69, 69) == 0xFF0000u && is_white(d.rgb(29, 40)) && is_white(d.rgb(70, 70)));
  // rotate(90deg) about the centre: (40,40)-(80,60) becomes (50,30)-(70,70)
  Shot e(page("<div id=a style='margin:40px 0 0 40px;width:40px;height:20px;background:#f00;transform:rotate(90deg)'></div>"));
  CHECK(near_color(e.rgb(60, 35), 0xFF0000u, 6) && near_color(e.rgb(60, 69), 0xFF0000u, 6) && near_color(e.rgb(60, 71), 0xFFFFFFu, 6) && near_color(e.rgb(42, 50), 0xFFFFFFu, 6) &&
        near_color(e.rgb(72, 50), 0xFFFFFFu, 6) && near_color(e.rgb(50, 50), 0xFF0000u, 6) && near_color(e.rgb(49, 50), 0xFFFFFFu, 8));
  // rotate(45deg): the corners leave their places, the diamond's points reach out
  Shot f(page("<div style='margin:30px;width:40px;height:40px;background:#f00;transform:rotate(45deg)'></div>"));
  CHECK(f.rgb(50, 50) == 0xFF0000u && near_color(f.rgb(50, 24), 0xFF0000u, 8) && near_color(f.rgb(50, 76), 0xFF0000u, 8) && is_white(f.rgb(31, 31)) && is_white(f.rgb(69, 69)) && is_white(f.rgb(31, 69)));
  // the individual transform properties work the same
  Shot g(page("<div id=a style='margin:40px 0 0 40px;width:40px;height:20px;background:#f00;rotate:90deg;translate:10px 0'></div>"));
  CHECK(near_color(g.rgb(70, 35), 0xFF0000u, 6) && near_color(g.rgb(55, 50), 0xFFFFFFu, 8));
  // skew: the bottom row moves right by its height
  Shot h(page("<div style='margin:10px;width:20px;height:20px;background:#f00;transform:skewX(45deg);transform-origin:0 0'></div>"));
  CHECK(near_color(h.rgb(35, 29), 0xFF0000u, 8) && is_white(h.rgb(12, 29)) && near_color(h.rgb(12, 11), 0xFF0000u, 8) && is_white(h.rgb(45, 12)));
  // matrix()
  Shot i(page("<div style='width:20px;height:20px;background:#f00;transform:matrix(1,0,0,1,30,10)'></div>"));
  CHECK(i.rgb(40, 20) == 0xFF0000u && is_white(i.rgb(29, 20)) && is_white(i.rgb(40, 9)) && is_white(i.rgb(51, 20)));
  // half a pixel: the edges are halfway
  Shot j(page("<div style='margin:10px;width:40px;height:40px;background:#f00;transform:translate(.5px,0)'></div>"));
  CHECK(near_color(j.rgb(10, 30), 0xFF7F7Fu, 4) && j.rgb(11, 30) == 0xFF0000u && near_color(j.rgb(50, 30), 0xFF7F7Fu, 4) && is_white(j.rgb(51, 30)));
  // what is inside moves with the box, and an overflow clip outside still cuts it
  Shot k(page("<div style='width:60px;height:40px;transform:translate(30px,0)'><div style='width:20px;height:20px;background:#f00'></div></div>"));
  CHECK(k.rgb(40, 10) == 0xFF0000u && is_white(k.rgb(10, 10)) && is_white(k.rgb(55, 10)));
  Shot l(page("<div style='overflow:hidden;width:50px;height:50px'><div style='width:20px;height:20px;background:#f00;transform:translate(40px,0)'></div></div>"));
  CHECK(l.rgb(45, 10) == 0xFF0000u && is_white(l.rgb(55, 10)));
  // together with opacity: one group
  Shot m(page("<div style='width:40px;height:40px;background:#000;opacity:.5;transform:translate(30px,0)'></div>"));
  CHECK(near_color(m.rgb(50, 20), 0x7F7F7Fu, 4) && is_white(m.rgb(10, 20)));
  // nested transforms
  Shot n(page("<div style='width:100px;height:100px;transform:translate(20px,0) scale(.5);transform-origin:0 0'><div style='width:40px;height:40px;background:#f00;transform:translate(20px,0)'></div></div>"));
  CHECK(n.rgb(40, 10) == 0xFF0000u && n.rgb(49, 19) == 0xFF0000u && is_white(n.rgb(50, 10)) && is_white(n.rgb(29, 10)) && is_white(n.rgb(40, 20)) && is_white(n.rgb(60, 10)));
  // mouse: a rotated box is found where it is drawn, not where it was laid out
  paint::HitResult hr;
  CHECK(paint::hit_test(e.list, &e.doc, 60, 35, &hr) && hr.node == e.doc.element_by_id("a")->serial);
  CHECK(!(paint::hit_test(e.list, &e.doc, 45, 50, &hr) && hr.node == e.doc.element_by_id("a")->serial));
  CHECK(paint::hit_test(e.list, &e.doc, 60, 69, &hr) && hr.node == e.doc.element_by_id("a")->serial);
  // degenerate matrices and absurd values do not hurt
  Shot z1(page("<div style='width:40px;height:40px;background:#f00;transform:scale(0)'></div>"));
  CHECK(is_white(z1.rgb(10, 10)));
  Shot z2(page("<div style='width:40px;height:40px;background:#f00;transform:scale(1000);transform-origin:0 0'></div>"));
  CHECK(z2.rgb(10, 10) == 0xFF0000u);
  Shot z3(page("<div style='width:40px;height:40px;background:#f00;transform:translate(1000000000px,0) rotate(99999deg)'></div>"));
  CHECK(is_white(z3.rgb(10, 10)));
  Shot z4(page("<div style='width:40px;height:40px;background:#f00;transform:scale(0.0001, 1000)'></div>"));
  CHECK(is_white(z4.rgb(5, 5)));
}

static void test_filters() {
  auto box = [](const char* filter, const char* bg = "#f00", const char* page_bg = "") {
    return page((std::string("<div style='width:50px;height:50px;background:") + bg + ";filter:" + filter + "'></div>").c_str(), page_bg);
  };
  Shot a(box("grayscale(1)"));
  CHECK(near_color(a.rgb(10, 10), 0x363636u, 4) && is_white(a.rgb(60, 10)));
  Shot b(box("invert(1)"));
  CHECK(b.rgb(10, 10) == 0x00FFFFu);
  Shot c(box("brightness(.5)", "#ff8040"));
  CHECK(near_color(c.rgb(10, 10), 0x7F4020u, 3));
  Shot d(box("contrast(2)", "#404040"));
  CHECK((d.rgb(10, 10) & 0xFF) < 8);
  Shot e(box("sepia(1)", "#fff", "body{background:#000}"));
  CHECK(near_color(e.rgb(10, 10), 0xFFFFEFu, 4));
  Shot f(box("hue-rotate(180deg)"));
  CHECK(near_color(f.rgb(10, 10), 0x006D6Du, 5));
  Shot g(box("saturate(0)"));
  CHECK(near_color(g.rgb(10, 10), 0x363636u, 4));
  Shot h(box("opacity(.5)", "#000"));
  CHECK(near_color(h.rgb(10, 10), 0x7F7F7Fu, 3));
  // several functions, in order: invert, then gray
  Shot i(box("invert(1) grayscale(1)"));
  CHECK(near_color(i.rgb(10, 10), 0xC9C9C9u, 5));
  // a blur: the middle stays, the edge goes soft and is the same on both sides
  Shot j(page("<div style='margin:40px;width:20px;height:20px;background:#000;filter:blur(4px)'></div>"));
  CHECK((j.rgb(50, 50) & 0xFF) < 0x40 && is_white(j.rgb(10, 10)) && is_white(j.rgb(150, 90)));
  const int l35 = luma(j.rgb(35, 50)), l38 = luma(j.rgb(38, 50)), l44 = luma(j.rgb(44, 50)), l64 = luma(j.rgb(64, 50));
  CHECK(l35 > 0xA0 && l35 < 0xFF && l38 < l35 && l44 < l38 && (l35 - l64 < 6 && l64 - l35 < 6));
  // drop-shadow: a blue copy, shifted
  Shot k(page("<div style='margin:20px;width:20px;height:20px;background:#f00;filter:drop-shadow(10px 10px 0 #00f)'></div>"));
  CHECK(k.rgb(25, 25) == 0xFF0000u && k.rgb(45, 45) == 0x0000FFu && is_white(k.rgb(25, 45)) && is_white(k.rgb(45, 25)) && is_white(k.rgb(55, 55)));
  // the filter works on the box as a group: the children are in it
  Shot l(page("<div style='width:60px;height:60px;filter:grayscale(1)'><div style='width:20px;height:20px;background:#f00'></div><div style='width:20px;height:20px;background:#00f'></div></div>"));
  CHECK(near_color(l.rgb(10, 10), 0x363636u, 4) && near_color(l.rgb(10, 30), 0x0E0E0Eu, 4));
  // with a transform on the same box: the filter first, then the move
  Shot m(page("<div style='width:20px;height:20px;background:#f00;filter:grayscale(1);transform:translate(40px,30px)'></div>"));
  CHECK(near_color(m.rgb(50, 40), 0x363636u, 4) && is_white(m.rgb(10, 10)));
  // a blur reaches outside the layout box and is cut by an enclosing clip
  Shot n(page("<div style='overflow:hidden;width:50px;height:100px'><div style='margin:20px;width:20px;height:20px;background:#000;filter:blur(3px)'></div></div>"));
  CHECK(luma(n.rgb(45, 30)) < 0xFE && is_white(n.rgb(60, 30)));
  // filters that cannot be (url references) are left out
  Shot o(box("url(#nothing)"));
  CHECK(o.rgb(10, 10) == 0xFF0000u);
}

static void test_shadows() {
  // a blurred shadow: falls off with distance, is gone beyond the blur, and is cut out under the box
  Shot a(page("<div style='margin:50px;width:40px;height:40px;box-shadow:0 0 10px #000'></div>"), 140, 140);
  const int e0 = luma(a.rgb(49, 70)), e1 = luma(a.rgb(44, 70)), e2 = luma(a.rgb(39, 70)), e3 = luma(a.rgb(25, 70));
  CHECK(e0 > 0x50 && e0 < 0xC0 && e1 > e0 && e2 > e1 && e3 == 0xFF);
  CHECK(is_white(a.rgb(70, 70)) && is_white(a.rgb(60, 60)));   // (nothing under the box)
  const int s0 = luma(a.rgb(90, 70)), s1 = luma(a.rgb(70, 49));   // the same on the other sides
  CHECK(s0 - e0 < 6 && e0 - s0 < 6 && s1 - e0 < 6 && e0 - s1 < 6);
  // an offset and no blur: a hard copy
  Shot b(page("<div style='margin:20px;width:30px;height:30px;box-shadow:10px 10px 0 #00f'></div>"));
  CHECK(b.rgb(55, 55) == 0x0000FFu && is_white(b.rgb(35, 35)) && is_white(b.rgb(55, 15)) && b.rgb(45, 55) == 0x0000FFu);
  // spread
  Shot c(page("<div style='margin:30px;width:40px;height:40px;box-shadow:0 0 0 5px #00f'></div>"));
  CHECK(c.rgb(27, 50) == 0x0000FFu && is_white(c.rgb(24, 50)) && c.rgb(50, 27) == 0x0000FFu && c.rgb(72, 50) == 0x0000FFu && is_white(c.rgb(75, 50)) && c.rgb(26, 26) == 0x0000FFu);
  // several shadows: the first is on top
  Shot d(page("<div style='margin:30px;width:40px;height:40px;box-shadow:0 0 0 4px #f00, 0 0 0 8px #00f'></div>"));
  CHECK(d.rgb(27, 50) == 0xFF0000u && d.rgb(23, 50) == 0x0000FFu && is_white(d.rgb(21, 50)));
  // a shadow follows the radius
  Shot e(page("<div style='margin:30px;width:60px;height:60px;border-radius:30px;box-shadow:0 0 0 6px #00f'></div>"));
  CHECK(is_white(e.rgb(27, 27)) && e.rgb(27, 60) == 0x0000FFu && e.rgb(60, 27) == 0x0000FFu && is_white(e.rgb(60, 60)) && is_white(e.rgb(60, 21)));
  // inset: dark inside the edge, light in the middle, nothing outside
  Shot f(page("<div style='margin:20px;width:60px;height:60px;background:#fff;box-shadow:inset 0 0 10px #000'></div>"));
  CHECK(luma(f.rgb(21, 50)) < 0xB0 && luma(f.rgb(50, 50)) > 0xF0 && is_white(f.rgb(10, 50)) && luma(f.rgb(24, 50)) > luma(f.rgb(21, 50)) && luma(f.rgb(50, 21)) < 0xB0 && luma(f.rgb(78, 50)) < 0xB0 && is_white(f.rgb(82, 50)));
  // inset with an offset and no blur: a hard band on the top and left
  Shot g(page("<div style='margin:20px;width:60px;height:60px;background:#fff;box-shadow:inset 10px 10px 0 #00f'></div>"));
  CHECK(g.rgb(25, 50) == 0x0000FFu && g.rgb(50, 25) == 0x0000FFu && is_white(g.rgb(50, 50)) && is_white(g.rgb(75, 50)) && is_white(g.rgb(50, 75)) && g.rgb(20, 20) == 0x0000FFu);
  // inset with a border: inside the padding box only
  Shot h(page("<div style='margin:20px;width:50px;height:50px;border:5px solid #888;background:#fff;box-shadow:inset 0 0 0 6px #00f'></div>"));
  CHECK(h.rgb(22, 50) == 0x888888u && h.rgb(27, 50) == 0x0000FFu && h.rgb(34, 50) == 0xFFFFFFu);
  // text-shadow: a hard copy in red, a blurred one in blue
  Shot t1(page("<div style='font:32px sans-serif;color:#000;text-shadow:4px 4px 0 #f00'>HHH</div>"));
  const int reds = t1.ink(0, 0, 120, 50) - t1.ink(0, 0, 120, 50, 0xFFFFFFFFu) ;
  (void)reds;
  int red = 0, black = 0;
  for (uint32_t y = 0; y < 50; ++y)
    for (uint32_t x = 0; x < 120; ++x) {
      red += t1.rgb(x, y) == 0xFF0000u;
      black += t1.rgb(x, y) == 0x000000u;
    }
  CHECK(red > 40 && black > 40);
  Shot t2(page("<div style='font:32px sans-serif;color:#000;text-shadow:0 0 6px #00f'>HHH</div>"));
  int bluish = 0;
  for (uint32_t y = 0; y < 50; ++y)
    for (uint32_t x = 0; x < 120; ++x) {
      const uint32_t c = t2.rgb(x, y);
      bluish += (c & 0xFF) > ((c >> 16) & 0xFF) + 30 && c != 0xFFFFFFu;
    }
  CHECK(bluish > 100);
  // no shadow where there is no text
  CHECK(is_white(t2.rgb(150, 80)));
}

static void test_gradients_more() {
  // conic: four quadrants (clockwise from the top)
  Shot a(page("<div style='width:100px;height:100px;background:conic-gradient(#f00 0%,#f00 25%,#0f0 25%,#0f0 50%,#00f 50%,#00f 75%,#ff0 75%,#ff0 100%)'></div>"), 100, 100);
  CHECK(near_color(a.rgb(75, 25), 0xFF0000u, 2) && near_color(a.rgb(75, 75), 0x00FF00u, 2) && near_color(a.rgb(25, 75), 0x0000FFu, 2) && near_color(a.rgb(25, 25), 0xFFFF00u, 2));
  // from an angle
  Shot b(page("<div style='width:100px;height:100px;background:conic-gradient(from 90deg,#f00 0%,#f00 25%,#0f0 25%,#0f0 100%)'></div>"), 100, 100);
  CHECK(near_color(b.rgb(75, 75), 0xFF0000u, 2) && near_color(b.rgb(75, 25), 0x00FF00u, 2) && near_color(b.rgb(25, 75), 0x00FF00u, 2));
  // a smooth one: half way round (the bottom) is the middle colour
  Shot c(page("<div style='width:100px;height:100px;background:conic-gradient(#000,#fff)'></div>"), 100, 100);
  CHECK(luma(c.rgb(50, 90)) > 0x70 && luma(c.rgb(50, 90)) < 0x90 && luma(c.rgb(75, 50)) > 0x30 && luma(c.rgb(75, 50)) < 0x50 && luma(c.rgb(51, 10)) < 0x10 && luma(c.rgb(49, 10)) > 0xF0);
  // an off-centre one
  Shot d(page("<div style='width:100px;height:100px;background:conic-gradient(at 20% 20%,#f00 0%,#f00 25%,#00f 25%,#00f 100%)'></div>"), 100, 100);
  CHECK(near_color(d.rgb(60, 10), 0xFF0000u, 2) && near_color(d.rgb(10, 60), 0x0000FFu, 2));
  // repeating linear: stripes
  Shot e(page("<div style='width:100px;height:20px;background:repeating-linear-gradient(90deg,#f00 0,#f00 10px,#00f 10px,#00f 20px)'></div>"));
  CHECK(near_color(e.rgb(5, 10), 0xFF0000u, 2) && near_color(e.rgb(15, 10), 0x0000FFu, 2) && near_color(e.rgb(25, 10), 0xFF0000u, 2) && near_color(e.rgb(45, 10), 0xFF0000u, 2) && near_color(e.rgb(95, 10), 0x0000FFu, 2));
  // a repeating one that blends
  Shot f(page("<div style='width:100px;height:20px;background:repeating-linear-gradient(90deg,#000,#fff 20px)'></div>"));
  CHECK(luma(f.rgb(2, 10)) < 0x30 && luma(f.rgb(18, 10)) > 0xD0 && luma(f.rgb(22, 10)) < 0x30 && luma(f.rgb(58, 10)) > 0xD0);
  // repeating radial: rings
  Shot g(page("<div style='width:100px;height:100px;background:repeating-radial-gradient(circle at 0 0,#f00 0,#f00 10px,#00f 10px,#00f 20px)'></div>"), 100, 100);
  CHECK(near_color(g.rgb(5, 0), 0xFF0000u, 2) && near_color(g.rgb(15, 0), 0x0000FFu, 2) && near_color(g.rgb(25, 0), 0xFF0000u, 2) && near_color(g.rgb(0, 15), 0x0000FFu, 2));
  // a colour hint moves the middle of the transition: at the hint the colour is half way
  Shot h(page("<div style='width:100px;height:20px;background:linear-gradient(to right,#000,20%,#fff)'></div>"));
  CHECK(luma(h.rgb(20, 10)) > 0x70 && luma(h.rgb(20, 10)) < 0x90 && luma(h.rgb(50, 10)) > 0xAA && luma(h.rgb(50, 10)) < 0xD0 && luma(h.rgb(2, 10)) < 0x40);
  // without one the middle is in the middle
  Shot i(page("<div style='width:100px;height:20px;background:linear-gradient(to right,#000,#fff)'></div>"));
  CHECK(luma(i.rgb(50, 10)) > 0x70 && luma(i.rgb(50, 10)) < 0x90);
  // gradients tile with background-size and repeat
  Shot j(page("<div style='width:100px;height:40px;background:linear-gradient(90deg,#f00,#00f) 0 0/50px 40px repeat-x'></div>"));
  CHECK(near_color(j.rgb(2, 10), 0xFA0005u, 16) && near_color(j.rgb(47, 10), 0x0500FAu, 16) && near_color(j.rgb(52, 10), 0xFA0005u, 16));
  // a rounded box clips a conic gradient
  Shot k(page("<div style='width:80px;height:80px;border-radius:40px;background:conic-gradient(#f00,#00f)'></div>"), 80, 80);
  CHECK(is_white(k.rgb(1, 1)) && !is_white(k.rgb(40, 20)));
}

static void test_clip_path() {
  const char* red = "width:100px;height:100px;background:#f00;";
  Shot a(page((std::string("<div style='") + red + "clip-path:inset(10px)'></div>").c_str()), 120, 120);
  CHECK(is_white(a.rgb(5, 50)) && a.rgb(15, 50) == 0xFF0000u && is_white(a.rgb(95, 50)) && a.rgb(89, 50) == 0xFF0000u && is_white(a.rgb(50, 5)) && is_white(a.rgb(50, 95)));
  Shot b(page((std::string("<div style='") + red + "clip-path:inset(10px round 20px)'></div>").c_str()), 120, 120);
  CHECK(is_white(b.rgb(11, 11)) && b.rgb(50, 11) == 0xFF0000u && b.rgb(11, 50) == 0xFF0000u && b.rgb(40, 40) == 0xFF0000u && is_white(b.rgb(88, 11)));
  Shot c(page((std::string("<div style='") + red + "clip-path:circle(30px at 50% 50%)'></div>").c_str()), 120, 120);
  CHECK(c.rgb(50, 50) == 0xFF0000u && c.rgb(75, 50) == 0xFF0000u && is_white(c.rgb(85, 50)) && is_white(c.rgb(5, 5)) && is_white(c.rgb(50, 85)) && c.rgb(50, 25) == 0xFF0000u);
  // the default radius reaches the closest side; the edge of the circle is anti-aliased
  Shot d(page((std::string("<div style='") + red + "clip-path:circle()'></div>").c_str()), 120, 120);
  CHECK(d.rgb(50, 50) == 0xFF0000u && d.rgb(5, 50) == 0xFF0000u && is_white(d.rgb(3, 3)) && is_white(d.rgb(97, 3)));
  const uint32_t edge = d.rgb(50 + 35, 50 + 35);   // (distance 49.5: on the edge)
  CHECK(edge != 0xFF0000u);
  Shot e(page((std::string("<div style='") + red + "clip-path:ellipse(40px 20px)'></div>").c_str()), 120, 120);
  CHECK(e.rgb(50, 50) == 0xFF0000u && e.rgb(85, 50) == 0xFF0000u && is_white(e.rgb(50, 25)) && e.rgb(50, 35) == 0xFF0000u && is_white(e.rgb(95, 50)));
  // a triangle
  Shot f(page((std::string("<div style='") + red + "clip-path:polygon(50% 0,100% 100%,0 100%)'></div>").c_str()), 120, 120);
  CHECK(f.rgb(50, 50) == 0xFF0000u && f.rgb(50, 10) == 0xFF0000u && is_white(f.rgb(10, 10)) && is_white(f.rgb(90, 10)) && f.rgb(10, 95) == 0xFF0000u && f.rgb(90, 95) == 0xFF0000u && is_white(f.rgb(10, 50)) &&
        is_white(f.rgb(90, 50)));
  // the slanted edge is smooth, not jagged: some pixels along it are partly covered
  int partial = 0;
  for (uint32_t y = 20; y < 90; ++y)
    for (uint32_t x = 0; x < 100; ++x) {
      const uint32_t c2 = f.rgb(x, y);
      partial += c2 != 0xFF0000u && c2 != 0xFFFFFFu;
    }
  CHECK(partial > 40);
  // even-odd: a star made of one self-crossing path has a hole in the middle
  Shot g(page((std::string("<div style='") + red + "clip-path:polygon(evenodd,50% 0,80% 100%,0 38%,100% 38%,20% 100%)'></div>").c_str()), 120, 120);
  CHECK(is_white(g.rgb(50, 50)) && g.rgb(50, 15) == 0xFF0000u);
  Shot g2(page((std::string("<div style='") + red + "clip-path:polygon(nonzero,50% 0,80% 100%,0 38%,100% 38%,20% 100%)'></div>").c_str()), 120, 120);
  CHECK(g2.rgb(50, 50) == 0xFF0000u);
  // children are clipped with their parent, and cannot be clicked outside the shape
  Shot h(page("<div style='width:100px;height:100px;clip-path:circle(30px)'><div id=k style='width:100px;height:100px;background:#00f'></div></div>"), 120, 120);
  paint::HitResult hr;
  CHECK(h.rgb(50, 50) == 0x0000FFu && is_white(h.rgb(5, 5)));
  CHECK(paint::hit_test(h.list, &h.doc, 50, 50, &hr) && hr.node == h.doc.element_by_id("k")->serial);
  CHECK(!(paint::hit_test(h.list, &h.doc, 5, 5, &hr) && hr.node == h.doc.element_by_id("k")->serial));
  // the geometry box
  Shot i(page("<div style='width:60px;height:60px;padding:20px;background:#f00;clip-path:inset(0) content-box'></div>"), 120, 120);
  CHECK(is_white(i.rgb(10, 50)) && i.rgb(30, 50) == 0xFF0000u && is_white(i.rgb(85, 50)));
  // with a transform: the clip moves with the box
  Shot j(page((std::string("<div style='") + red + "clip-path:inset(10px);transform:translate(20px,0)'></div>").c_str()), 140, 120);
  CHECK(is_white(j.rgb(25, 50)) && j.rgb(35, 50) == 0xFF0000u && is_white(j.rgb(115, 50)));
}

// A display list drawn on its own (no page): for what the style does not reach yet.
struct ListShot {
  media::Image image;
  uint32_t width, height;
  ListShot(const paint::DisplayList& list, uint32_t w, uint32_t h) : image(g_alloc), width(w), height(h) {
    image.create(w, h);
    graphics::Surface surface = image.surface(0);
    graphics::SoftwareRenderer2D renderer(surface, nullptr, &g_alloc);
    renderer.set_blend(graphics::BlendMode::Replace);
    renderer.fill_rect(graphics::Rect2D{0, 0, w, h}, 0xFFFFFFFFu);
    paint::DisplayPainter painter(g_alloc, *g_fonts, nullptr);
    painter.paint(renderer, list, 0, 0, graphics::Rect2D{0, 0, w, h});
  }
  uint32_t rgb(uint32_t x, uint32_t y) const { return image.frame(0)[y * width + x] & 0x00FFFFFFu; }
  int rows_with_ink(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) const {
    int rows = 0;
    for (uint32_t y = y0; y < y1; ++y) {
      bool any = false;
      for (uint32_t x = x0; x < x1; ++x) any = any || rgb(x, y) != 0xFFFFFFu;
      rows += any;
    }
    return rows;
  }
  int runs_in_row(uint32_t y, uint32_t x0, uint32_t x1) const {   // how many separate inked stretches
    int runs = 0;
    bool in = false;
    for (uint32_t x = x0; x < x1; ++x) {
      const bool ink = rgb(x, y) != 0xFFFFFFu;
      runs += ink && !in;
      in = ink;
    }
    return runs;
  }
};

static int count_ink(const ListShot& s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
  int n = 0;
  for (uint32_t y = y0; y < y1; ++y)
    for (uint32_t x = x0; x < x1; ++x) n += s.rgb(x, y) != 0xFFFFFFu;
  return n;
}

static void test_borders() {
  // dashed: dashes of 3x the width with gaps, a dash in every corner
  Shot a(page("<div style='width:92px;height:40px;border:4px dashed #000'></div>"), 120, 60);
  CHECK(a.rgb(1, 1) == 0x000000u && a.rgb(10, 1) == 0x000000u && is_white(a.rgb(15, 1)) && a.rgb(25, 1) == 0x000000u && a.rgb(98, 1) == 0x000000u && is_white(a.rgb(50, 8)));
  int dash_runs = 0;
  for (uint32_t x = 0, in = 0; x < 100; ++x) {
    const bool ink = a.rgb(x, 1) != 0xFFFFFFu;
    dash_runs += ink && !in;
    in = ink;
  }
  CHECK(dash_runs >= 4 && dash_runs <= 7);
  // dotted: round dots, one in each corner
  Shot b(page("<div style='width:94px;height:40px;border:6px dotted #000'></div>"), 120, 60);
  CHECK(b.rgb(3, 3) == 0x000000u && is_white(b.rgb(0, 0)) && is_white(b.rgb(40, 20)));
  int dots = 0;
  for (uint32_t x = 0, in = 0; x < 106; ++x) {
    const bool ink = b.rgb(x, 3) != 0xFFFFFFu;
    dots += ink && !in;
    in = ink;
  }
  CHECK(dots >= 5);
  // double: line, gap, line
  Shot c(page("<div style='width:92px;height:40px;border:9px double #000'></div>"), 120, 70);
  CHECK(c.rgb(50, 1) == 0x000000u && is_white(c.rgb(50, 4)) && c.rgb(50, 7) == 0x000000u && is_white(c.rgb(50, 12)) && c.rgb(1, 25) == 0x000000u && is_white(c.rgb(4, 25)) && c.rgb(7, 25) == 0x000000u);
  // inset / outset: the top and left against the bottom and right
  Shot d(page("<div style='width:80px;height:30px;border:6px outset #888'></div>"), 120, 60);
  Shot e(page("<div style='width:80px;height:30px;border:6px inset #888'></div>"), 120, 60);
  CHECK(luma(d.rgb(50, 2)) > luma(d.rgb(50, 40)) && luma(d.rgb(2, 20)) > luma(d.rgb(90, 20)) && luma(e.rgb(50, 2)) < luma(e.rgb(50, 40)) && luma(e.rgb(2, 20)) < luma(e.rgb(90, 20)));
  // groove / ridge: each side is split in two shades, in opposite order
  Shot f(page("<div style='width:80px;height:30px;border:8px groove #888'></div>"), 120, 60);
  Shot g(page("<div style='width:80px;height:30px;border:8px ridge #888'></div>"), 120, 60);
  CHECK(luma(f.rgb(50, 1)) < luma(f.rgb(50, 6)) && luma(g.rgb(50, 1)) > luma(g.rgb(50, 6)) && luma(f.rgb(50, 44)) > luma(f.rgb(50, 40)) && luma(g.rgb(50, 44)) < luma(g.rgb(50, 40)) && is_white(f.rgb(50, 20)));
  // sides of different colours meet on a diagonal (a mitre)
  Shot h(page("<div style='width:60px;height:40px;border-style:solid;border-width:10px;border-color:#f00 #00f #f00 #00f'></div>"), 100, 80);
  CHECK(near_color(h.rgb(30, 3), 0xFF0000u, 2) && near_color(h.rgb(3, 25), 0x0000FFu, 2) && near_color(h.rgb(8, 1), 0xFF0000u, 2) && near_color(h.rgb(1, 8), 0x0000FFu, 2) && near_color(h.rgb(30, 55), 0xFF0000u, 2) &&
        near_color(h.rgb(75, 25), 0x0000FFu, 2) && near_color(h.rgb(78, 48), 0x0000FFu, 2) && near_color(h.rgb(62, 58), 0xFF0000u, 2));
  // ... and the diagonal follows the widths: a wide top and a narrow left
  Shot i(page("<div style='width:60px;height:40px;border-style:solid;border-width:20px 4px 4px 4px;border-color:#f00 #00f #00f #00f'></div>"), 100, 80);
  CHECK(near_color(i.rgb(3, 10), 0xFF0000u, 2) && near_color(i.rgb(1, 10), 0x0000FFu, 2) && near_color(i.rgb(2, 40), 0x0000FFu, 2));
  // rounded corners with sides of different colours
  Shot j(page("<div style='width:60px;height:60px;border:8px solid;border-color:#f00 #00f #f00 #00f;border-radius:20px'></div>"), 100, 100);
  CHECK(near_color(j.rgb(38, 3), 0xFF0000u, 2) && near_color(j.rgb(3, 38), 0x0000FFu, 2) && is_white(j.rgb(1, 1)) && is_white(j.rgb(38, 38)) && !is_white(j.rgb(8, 8)));
  // rounded dashed border: ink on the curve, nothing inside
  Shot k(page("<div style='width:60px;height:60px;border:4px dashed #000;border-radius:30px'></div>"), 100, 100);
  CHECK(!is_white(k.rgb(24, 2)) && is_white(k.rgb(34, 34)) && is_white(k.rgb(1, 1)));
  // widths differ: the sides keep their own widths
  Shot l(page("<div style='width:60px;height:30px;border-style:solid;border-color:#000;border-width:2px 8px 4px 12px'></div>"), 100, 60);
  CHECK(l.rgb(30, 1) == 0x000000u && is_white(l.rgb(30, 2)) && l.rgb(11, 20) == 0x000000u && is_white(l.rgb(12, 20)) && l.rgb(78, 20) == 0x000000u && is_white(l.rgb(71, 20)) && l.rgb(30, 35) == 0x000000u);
  // outline with an offset: outside the border box, with a gap; and a negative offset draws over the box
  Shot m(page("<div style='margin:20px;width:30px;height:30px;background:#0f0;outline:3px solid #f00;outline-offset:5px'></div>"), 100, 100);
  CHECK(m.rgb(13, 40) == 0xFF0000u && is_white(m.rgb(17, 40)) && m.rgb(30, 40) == 0x00FF00u && m.rgb(12, 40) == 0xFF0000u && is_white(m.rgb(10, 40)));
  Shot n(page("<div style='margin:20px;width:30px;height:30px;background:#0f0;outline:3px solid #f00;outline-offset:-3px'></div>"), 100, 100);
  CHECK(n.rgb(21, 40) == 0xFF0000u && n.rgb(25, 40) == 0x00FF00u && is_white(n.rgb(19, 40)));
}

static void add_text_op(paint::DisplayList* list, uint8_t decorations, paint::DecorationStyle style, int32_t thickness, uint32_t color = 0xFF000000u, uint32_t line_color = 0) {
  paint::Op op;
  op.kind = paint::OpKind::Text;
  op.rect = paint::IRect{10, 10, 80, 20};
  op.color = color;
  op.baseline = 26;
  op.decorations = decorations;
  op.decoration_style = static_cast<uint8_t>(style);
  op.decoration_thickness = thickness;
  op.decoration_offset = 4;
  op.decoration_color = line_color;
  op.strike_offset = 6;
  list->add(op);
}

static void test_text_decorations() {
  using paint::DecorationStyle;
  // underline, solid: thickness rows exactly; y = baseline + offset - thickness + 1
  paint::DisplayList solid(g_alloc);
  add_text_op(&solid, paint::kDecorationUnderline, DecorationStyle::Solid, 2);
  ListShot a(solid, 100, 50);
  CHECK(a.rows_with_ink(10, 90, 0, 50) == 2 && a.rgb(10, 29) == 0u && a.rgb(89, 30) == 0u && a.rgb(90, 29) == 0xFFFFFFu && a.rgb(9, 29) == 0xFFFFFFu);
  // double: two lines two thicknesses apart
  paint::DisplayList dbl(g_alloc);
  add_text_op(&dbl, paint::kDecorationUnderline, DecorationStyle::Double, 2);
  ListShot b(dbl, 100, 50);
  CHECK(b.rows_with_ink(10, 90, 0, 50) == 4 && b.rgb(40, 29) == 0u && b.rgb(40, 31) == 0xFFFFFFu && b.rgb(40, 33) == 0u);
  // dotted: separate dots
  paint::DisplayList dotted(g_alloc);
  add_text_op(&dotted, paint::kDecorationUnderline, DecorationStyle::Dotted, 3);
  ListShot c(dotted, 100, 50);
  CHECK(c.runs_in_row(28, 10, 90) >= 8 && c.rows_with_ink(10, 90, 0, 50) == 3);
  // dashed: dashes of three widths
  paint::DisplayList dashed(g_alloc);
  add_text_op(&dashed, paint::kDecorationUnderline, DecorationStyle::Dashed, 2);
  ListShot d(dashed, 100, 50);
  CHECK(d.runs_in_row(29, 10, 90) >= 6 && d.runs_in_row(29, 10, 90) <= 9 && d.rgb(11, 29) == 0u && d.rgb(17, 29) == 0xFFFFFFu);
  // wavy: the line rises and falls (more rows than its thickness), keeps going across the run
  paint::DisplayList wavy(g_alloc);
  add_text_op(&wavy, paint::kDecorationUnderline, DecorationStyle::Wavy, 2);
  ListShot e(wavy, 100, 50);
  CHECK(e.rows_with_ink(10, 90, 0, 50) >= 4 && e.rows_with_ink(10, 90, 0, 50) <= 9);
  int columns_with_ink = 0;
  for (uint32_t x = 10; x < 90; ++x) {
    bool any = false;
    for (uint32_t y = 20; y < 45; ++y) any = any || e.rgb(x, y) != 0xFFFFFFu;
    columns_with_ink += any;
  }
  CHECK(columns_with_ink == 80);
  // overline above the text and line-through across it, with their own colour
  paint::DisplayList both(g_alloc);
  add_text_op(&both, paint::kDecorationOverline | paint::kDecorationStrike, DecorationStyle::Solid, 1, 0xFF000000u, 0xFFFF0000u);
  ListShot f(both, 100, 50);
  CHECK(f.rgb(40, 10) == 0xFF0000u && f.rgb(40, 19) == 0xFF0000u && f.rgb(40, 20) == 0xFFFFFFu && f.rows_with_ink(10, 90, 0, 50) == 2);
}

// Stands in for the page's pictures: 8x8 for "tile.png", 100x50 for "wide.png"; draws them as a solid colour and notes every call.
struct TestPictures final : browser::IObjectPainter {
  struct Call {
    std::string url;
    int32_t x, y;
    uint32_t w, h;
    graphics::Rect2D clip;
  };
  std::vector<Call> calls;
  void paint_object(graphics::IRenderer2D&, uint32_t, browser::ObjectKind, int32_t, int32_t, uint32_t, uint32_t, const browser::Rect2D&) override {}
  static std::string text(StringView v) { return std::string(v.data(), static_cast<size_t>(v.size())); }
  bool background_image_size(StringView url, int32_t* w, int32_t* h) override {
    const std::string u = text(url);
    if (u == "tile.png") { *w = 8; *h = 8; return true; }
    if (u == "wide.png") { *w = 100; *h = 50; return true; }
    if (u == "pattern.png") { *w = 6; *h = 4; return true; }
    return false;
  }
  void paint_background_image(graphics::IRenderer2D& r, StringView url, int32_t x, int32_t y, uint32_t w, uint32_t h, const browser::Rect2D& clip) override {
    calls.push_back({text(url), x, y, w, h, clip});
    r.push_clip_rect(clip);
    if (text(url) == "pattern.png") {   // four colours, one in each quarter, and a translucent stripe
      const uint32_t hw = w / 2, hh = h / 2;
      r.fill_rect(graphics::Rect2D{x, y, hw, hh}, 0xFFFF0000u);
      r.fill_rect(graphics::Rect2D{x + static_cast<int32_t>(hw), y, w - hw, hh}, 0xFF00AA00u);
      r.fill_rect(graphics::Rect2D{x, y + static_cast<int32_t>(hh), hw, h - hh}, 0xFF0000FFu);
      r.fill_rect(graphics::Rect2D{x + static_cast<int32_t>(hw), y + static_cast<int32_t>(hh), w - hw, h - hh}, 0xFFFFFF00u);
      r.fill_rect(graphics::Rect2D{x, y + 1, w, 1}, 0x80000000u);
    } else {
      r.fill_rect(graphics::Rect2D{x, y, w, h}, text(url) == "tile.png" ? 0xFFFF0000u : 0xFF00AA00u);
    }
    r.pop_clip();
  }
  bool has(int32_t x, int32_t y, uint32_t w, uint32_t h) const {
    for (const Call& c : calls)
      if (c.x == x && c.y == y && c.w == w && c.h == h) return true;
    return false;
  }
};

// Patterns of a picture (many tiles) through the device renderer: one texture the sampler repeats, the same pixels as the CPU's copies.
static void test_tiled_backgrounds_on_device() {
  const char* pages[] = {
      "<div style='width:100px;height:60px;background:url(pattern.png)'></div>",
      "<div style='margin:7px;width:90px;height:50px;background:url(pattern.png) 3px 2px'></div>",
      "<div style='width:100px;height:60px;background:url(pattern.png) 0 0/12px 8px'></div>",
      "<div style='width:100px;height:40px;background:url(pattern.png) repeat-x 0 0/3px 4px'></div>",
      "<div style='width:96px;height:60px;border-radius:20px;background:url(pattern.png) 5px 5px'></div>",
      "<div style='width:100px;height:60px;background:url(pattern.png) round 0 0/9px 7px'></div>",
  };
  for (const char* body : pages) {
    TestPictures pictures;
    Shot cpu(page(body), 120, 80, &pictures);
    TestPictures again;
    const std::vector<uint32_t> gpu = cpu.via_device(&again);
    uint32_t far = 0, inked = 0;
    for (uint32_t y = 0; y < cpu.height; ++y)
      for (uint32_t x = 0; x < cpu.width; ++x) {
        const uint32_t a = cpu.px(x, y), b = gpu[y * cpu.width + x];
        if (a != 0xFFFFFFFFu) ++inked;
        if (!near_color(a, b, 2)) ++far;
      }
    CHECK(inked > 300);
    if (far * 100 > inked) printf("tiled page differs: far %u inked %u: %s\n", far, inked, body);
    CHECK(far * 100 <= inked);   // (the `round` page may differ by a texel here and there)
    CHECK(again.calls.size() <= 2);   // (the tile is painted once, not once per copy)
  }
}

static void test_background_images() {
  // repeated by default: the tiles that show, and no others
  TestPictures p1;
  Shot a(page("<div style='width:20px;height:20px;background:url(tile.png)'></div>"), 100, 100, &p1);
  CHECK(p1.calls.size() == 9 && p1.has(0, 0, 8, 8) && p1.has(16, 16, 8, 8) && !p1.has(24, 0, 8, 8) && a.rgb(19, 19) == 0xFF0000u && is_white(a.rgb(20, 10)));
  // no-repeat, at the bottom right
  TestPictures p2;
  Shot b(page("<div style='width:20px;height:20px;background:url(tile.png) no-repeat right bottom'></div>"), 100, 100, &p2);
  CHECK(p2.calls.size() == 1 && p2.has(12, 12, 8, 8) && b.rgb(15, 15) == 0xFF0000u && is_white(b.rgb(5, 5)));
  // position in pixels and percent
  TestPictures p3;
  Shot c(page("<div style='width:30px;height:30px;background:url(tile.png) no-repeat 25% 4px'></div>"), 100, 100, &p3);
  CHECK(p3.calls.size() == 1 && p3.has(5, 4, 8, 8));
  // cover: scaled up until it fills, centred; contain: until it fits
  TestPictures p4;
  Shot d(page("<div style='width:40px;height:20px;background:url(tile.png) no-repeat center/cover'></div>"), 100, 100, &p4);
  CHECK(p4.calls.size() == 1 && p4.has(0, -10, 40, 40) && d.rgb(20, 10) == 0xFF0000u);
  TestPictures p5;
  Shot e(page("<div style='width:40px;height:20px;background:url(tile.png) no-repeat center/contain'></div>"), 100, 100, &p5);
  CHECK(p5.calls.size() == 1 && p5.has(10, 0, 20, 20) && is_white(e.rgb(5, 10)) && e.rgb(20, 10) == 0xFF0000u);
  // an explicit size; one side auto keeps the shape of the picture
  TestPictures p6;
  Shot f(page("<div style='width:40px;height:40px;background:url(wide.png) no-repeat 0 0/20px auto'></div>"), 100, 100, &p6);
  CHECK(p6.calls.size() == 1 && p6.has(0, 0, 20, 10) && f.rgb(10, 5) == 0x00AA00u && is_white(f.rgb(10, 15)));
  TestPictures p7;
  Shot g(page("<div style='width:40px;height:30px;background:url(tile.png) repeat-x 0 0/16px 16px'></div>"), 100, 100, &p7);
  CHECK(p7.calls.size() == 3 && p7.has(0, 0, 16, 16) && p7.has(32, 0, 16, 16) && is_white(g.rgb(10, 20)));
  // space: whole tiles with the gaps spread evenly; round: the tiles stretch to fit
  TestPictures p8;
  Shot h(page("<div style='width:54px;height:10px;background:url(tile.png) space no-repeat 0 0/10px 10px'></div>"), 100, 100, &p8);
  CHECK(p8.calls.size() == 5 && p8.has(0, 0, 10, 10) && p8.has(11, 0, 10, 10) && p8.has(44, 0, 10, 10));
  TestPictures p9;
  Shot i(page("<div style='width:58px;height:10px;background:url(tile.png) round no-repeat 0 0/10px 10px'></div>"), 100, 100, &p9);
  CHECK(p9.calls.size() == 6 && p9.has(0, 0, 10, 10) && p9.has(50, 0, 10, 10));
  // origin and clip: the content box
  TestPictures p10;
  Shot j(page("<div style='width:30px;height:30px;padding:10px;background:url(tile.png);background-origin:content-box;background-clip:content-box'></div>"), 100, 100, &p10);
  CHECK(p10.calls.size() == 16 && p10.has(10, 10, 8, 8) && !p10.has(2, 10, 8, 8) && p10.has(34, 34, 8, 8) && is_white(j.rgb(5, 20)) && j.rgb(12, 12) == 0xFF0000u && is_white(j.rgb(41, 20)));
  for (const auto& c : p10.calls) CHECK(c.clip.x >= 10 && c.clip.x + static_cast<int32_t>(c.clip.width) <= 40 && c.clip.y >= 10 && c.clip.y + static_cast<int32_t>(c.clip.height) <= 40);
  TestPictures p11;
  Shot k(page("<div style='width:30px;height:30px;padding:10px;background:url(tile.png);background-clip:padding-box;border:5px solid #00f'></div>"), 100, 100, &p11);
  CHECK(k.rgb(2, 20) == 0x0000FFu && k.rgb(20, 20) == 0xFF0000u);
  // layers: the first is on top; a colour is under all of them
  TestPictures p12;
  Shot l(page("<div style='width:30px;height:30px;background:url(tile.png) no-repeat 0 0, url(wide.png) no-repeat 0 0/30px 30px #00f'></div>"), 100, 100, &p12);
  CHECK(l.rgb(4, 4) == 0xFF0000u && l.rgb(20, 20) == 0x00AA00u);
  // a rounded box clips the picture
  TestPictures p13;
  Shot m(page("<div style='width:40px;height:40px;border-radius:20px;background:url(wide.png) 0 0/40px 40px'></div>"), 100, 100, &p13);
  CHECK(is_white(m.rgb(1, 1)) && m.rgb(20, 20) == 0x00AA00u);
  // a picture that is not there (no size known) draws nothing; so does a page without an object painter
  TestPictures p14;
  Shot n(page("<div style='width:30px;height:30px;background:url(missing.png) #ff0'></div>"), 100, 100, &p14);
  CHECK(n.rgb(10, 10) == 0xFFFF00u && p14.calls.empty());
  Shot o(page("<div style='width:30px;height:30px;background:url(tile.png) #ff0'></div>"));
  CHECK(o.rgb(10, 10) == 0xFFFF00u);
  // list-style-image: the picture stands for the bullet
  TestPictures p15;
  Shot q(page("<ul style='margin:0;padding:0 0 0 30px;list-style-image:url(tile.png)'><li style='font:16px sans-serif'>item</li></ul>"), 100, 40, &p15);
  CHECK(p15.calls.size() == 1 && p15.calls[0].w == 8 && p15.calls[0].h == 8 && p15.calls[0].x < 30 && p15.calls[0].y >= 0 && p15.calls[0].y < 16);
  // a pattern of many tiny tiles: the picture is asked for once
  TestPictures p17;
  Shot t(page("<div style='width:60px;height:40px;background:url(tile.png) 0 0/2px 2px'></div>"), 100, 100, &p17);
  CHECK(p17.calls.size() == 1 && t.rgb(0, 0) == 0xFF0000u && t.rgb(59, 39) == 0xFF0000u && is_white(t.rgb(60, 10)) && is_white(t.rgb(10, 40)));
  TestPictures p18;
  Shot u(page("<div style='width:1000px;height:600px;background:url(tile.png) 0 0/1px 1px'></div>"), 200, 100, &p18);
  CHECK(p18.calls.size() == 1 && u.rgb(199, 99) == 0xFF0000u);
  // with a transform and a filter around it
  TestPictures p16;
  Shot r(page("<div style='width:20px;height:20px;background:url(tile.png);filter:grayscale(1);transform:translate(40px,0)'></div>"), 100, 100, &p16);
  CHECK(near_color(r.rgb(45, 10), 0x363636u, 4));
}

static void test_effects_golden() {
  Shot a(std::string("<style>body{margin:0;background:#f4f4f4;font-family:sans-serif}div{box-sizing:border-box}"
                     ".t{position:absolute;width:60px;height:40px;background:#e8453c;transform:rotate(-20deg) scale(1.2);left:20px;top:20px;border-radius:6px}"
                     ".f{position:absolute;left:110px;top:12px;width:50px;height:50px;background:linear-gradient(135deg,#ffcc00,#ff3366);filter:blur(2px) saturate(1.5)}"
                     ".s{position:absolute;left:190px;top:15px;width:50px;height:50px;background:#fff;border-radius:10px;box-shadow:0 6px 14px rgba(0,0,0,.45),inset 0 0 8px rgba(0,0,100,.5)}"
                     ".c{position:absolute;left:20px;top:100px;width:70px;height:70px;background:conic-gradient(from 20deg,#f00,#ff0,#0f0,#0ff,#00f,#f0f,#f00)}"
                     ".p{position:absolute;left:110px;top:100px;width:70px;height:70px;background:repeating-linear-gradient(45deg,#2a62b2 0 6px,#8ab4f8 6px 12px);clip-path:polygon(50% 0,100% 38%,82% 100%,18% 100%,0 38%)}"
                     ".b{position:absolute;left:200px;top:100px;width:80px;height:34px;border:6px double #333;border-radius:8px}"
                     ".d{position:absolute;left:200px;top:142px;width:80px;height:30px;border:5px groove #888;background:#fff}"
                     ".x{position:absolute;left:20px;top:182px;font-size:26px;color:#223;text-shadow:2px 2px 3px rgba(255,0,80,.6)}"
                     ".o{position:absolute;left:180px;top:186px;width:30px;height:30px;background:#9c6;outline:3px dashed #36c;outline-offset:4px}"
                     ".k{position:absolute;left:236px;top:186px;width:44px;height:30px;border:3px dotted #c36;box-shadow:0 0 0 3px #fc0}"
                     "</style>"
                     "<div class=t></div><div class=f></div><div class=s></div><div class=c></div><div class=p></div><div class=b></div><div class=d></div>"
                     "<div class=x>Effects</div><div class=o></div><div class=k></div>"),
         300, 230);
  const golden::Report r = golden::check(g_alloc, "paint_effects", ArrayView<const uint32_t>(a.image.frame(0).data(), a.image.frame(0).size()), a.width, a.height, 24);
  if (!r.ok) printf("%s\n", r.message.c_str());
  CHECK(r.ok);
}

int main() {
  {
    font::FontSet fonts(g_alloc);
    fonts.add_bundled_fonts();
    g_fonts = &fonts;
    test_caret();
    test_boxes();
    test_stacking();
    test_clip_and_opacity();
    test_gradients_and_shadows();
    test_text();
    test_hit_testing();
    test_golden();
    test_transforms();
    test_transforms_on_device();
    test_filters_on_device();
    test_clips_on_device();
    test_shadows_on_device();
    test_text_and_drop_shadows_on_device();
    test_tiled_backgrounds_on_device();
    test_special_gradients_on_device();
    test_border_styles();
    test_filters();
    test_shadows();
    test_gradients_more();
    test_clip_path();
    test_borders();
    test_text_decorations();
    test_background_images();
    test_effects_golden();
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
