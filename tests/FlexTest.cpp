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

// Host-side tests for flexbox layout (src/Layout/Flex.cpp, CSS Flexbox Level 1 section 9). Text is measured with FixedWidthMeasurer: every
// character is 8px wide at the default 16px font and a line is 16px high, so the expected numbers can be worked out by hand.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "Browser/Renderer.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Layout/BoxBuilder.hpp"
#include "Layout/FontMeasurer.hpp"
#include "Layout/Layout.hpp"

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
static font::FontSet* g_fonts = nullptr;   // the bundled fonts (made in main)

// Every <img> is 100x50 and ready.
struct FakeObjects final : browser::IObjectSource {
  bool object_info(const dom::Node* element, browser::ObjectInfo* out) override {
    if (!element->is_tag("img")) return false;
    out->kind = browser::ObjectKind::Image;
    out->width = 100;
    out->height = 50;
    out->ready = true;
    return true;
  }
};

// A page laid out: `html` is the markup, `width` the viewport's width in pixels.
struct Laid {
  dom::Document doc;
  browser::StyleSheet sheet;
  BoxTree tree;
  FixedWidthMeasurer fixed_measurer;
  UniquePointer<FontMeasurer> font_measurer;
  FakeObjects objects;
  bool ok = false;

  explicit Laid(const char* html, int width = 800, bool real_fonts = false) : doc(g_alloc), sheet(g_alloc), tree(g_alloc) {
    dom::build_document(doc, html, nullptr);
    browser::RenderOptions render;
    render.media.width = width;
    render.media.height = 600;
    browser::collect_style_sheets(doc, render, &sheet);
    sheet.set_viewport(width, 600);
    BuildOptions build;
    build.objects = &objects;
    LayoutOptions options;
    options.viewport_width = LayoutUnit::from_px(width);
    options.viewport_height = LayoutUnit::from_px(600);
    if (real_fonts) font_measurer = UniquePointer<FontMeasurer>::make(g_alloc, *g_fonts, &sheet);
    options.measurer = real_fonts ? static_cast<ITextMeasurer*>(font_measurer.get()) : &fixed_measurer;
    options.sheet = &sheet;
    ok = build_box_tree(doc, sheet, build, &tree) && layout_tree(tree, options);
  }
  // The box of the element with this id (kNoBox if there is none).
  BoxId box(const char* id) const {
    const dom::Node* n = doc.element_by_id(id);
    if (!n) return kNoBox;
    for (uint64_t i = 0; i < tree.count(); ++i)
      if (tree.box(static_cast<BoxId>(i)).node == n->serial && tree.box(static_cast<BoxId>(i)).kind != BoxKind::Text) return static_cast<BoxId>(i);
    return kNoBox;
  }
  const Box& at(const char* id) const { return tree.box(box(id)); }
  // Absolute position of a box's border box: the offsets of the boxes that contain it added up.
  int abs_x(const char* id) const { return abs(box(id), true); }
  int abs_y(const char* id) const { return abs(box(id), false); }
  int abs(BoxId b, bool x) const {
    int sum = 0;
    for (BoxId cur = b; cur != kNoBox; cur = tree.box(cur).parent) {
      const Box& bx = tree.box(cur);
      if (bx.kind == BoxKind::Inline) continue;   // (an inline box has no place of its own)
      sum += (x ? bx.x : bx.y).round();
    }
    return sum;
  }
};

static const char* kReset = "<style>html,body,p,div,ul,li,h1{margin:0;padding:0}</style>";

static std::string page(const char* body, const char* css = "") { return std::string(kReset) + "<style>" + css + "</style>" + body; }

static int px(LayoutUnit u) { return u.round(); }

// A flex container (id f) with the given declarations and items.
static std::string flex(const char* style, const char* items) { return std::string("<div id=f style='display:flex;") + style + "'>" + items + "</div>"; }

static void test_row_basics() {
  // auto-sized items take their max-content width and sit side by side; they stretch to the tallest
  Laid p(page(flex("width:400px", "<div id=a>abc</div><div id=b style='width:100px;height:40px'></div><div id=c>de</div>").c_str()).c_str());
  CHECK(p.ok);
  CHECK(px(p.at("a").width) == 24 && p.abs_x("a") == 0);
  CHECK(px(p.at("b").width) == 100 && p.abs_x("b") == 24);
  CHECK(px(p.at("c").width) == 16 && p.abs_x("c") == 124);
  CHECK(px(p.at("f").height) == 40 && px(p.at("a").height) == 40 && px(p.at("c").height) == 40);   // (align-items: stretch)
  CHECK(px(p.at("f").width) == 400);

  // a flex container is block-level and as wide as its parent without a width; its height is that of the tallest item
  Laid q(page(flex("", "<div id=a style='height:30px;width:50px'></div><div id=b style='height:10px;width:50px'></div>").c_str()).c_str());
  CHECK(px(q.at("f").width) == 800 && px(q.at("f").height) == 30);
  CHECK(px(q.at("b").height) == 10);   // (a height of its own: no stretch)

  // padding, border and margins of the container and the items
  Laid r(page(flex("width:300px;padding:10px;border:2px solid;margin:5px", "<div id=a style='width:50px;height:20px;margin:3px'></div><div id=b style='width:50px;height:20px;padding:4px;border:1px solid'></div>").c_str()).c_str());
  CHECK(r.abs_x("f") == 5 && r.abs_y("f") == 5);
  CHECK(r.abs_x("a") == 5 + 2 + 10 + 3 && r.abs_y("a") == 5 + 2 + 10 + 3);
  CHECK(r.abs_x("b") == 5 + 2 + 10 + 3 + 50 + 3 && px(r.at("b").width) == 60);   // (content-box width: 50 + 2*4 + 2*1)
  CHECK(px(r.at("f").height) == 2 * 2 + 2 * 10 + 30);   // the second item is 30 high with its padding and border, the first 26 with margins; stretch
}

static void test_flex_grow_and_shrink() {
  // flex: 1 gives equal columns whatever is in them
  Laid p(page(flex("width:300px", "<div id=a style='flex:1'>x</div><div id=b style='flex:1'>a much longer text</div><div id=c style='flex:1'></div>").c_str()).c_str());
  CHECK(px(p.at("a").width) == 100 && px(p.at("b").width) == 100 && px(p.at("c").width) == 100);
  CHECK(p.abs_x("b") == 100 && p.abs_x("c") == 200);

  // grow factors share the room in proportion
  Laid q(page(flex("width:300px", "<div id=a style='flex:1'></div><div id=b style='flex:2'></div>").c_str()).c_str());
  CHECK(px(q.at("a").width) == 100 && px(q.at("b").width) == 200);

  // the free space is shared on top of the flex basis (width here): 500 - 300 = 200 -> 50, 50, 100
  Laid r(page(flex("width:500px", "<div id=a style='width:100px;flex-grow:1'></div><div id=b style='width:100px;flex-grow:1'></div><div id=c style='width:100px;flex-grow:2'></div>").c_str()).c_str());
  CHECK(px(r.at("a").width) == 150 && px(r.at("b").width) == 150 && px(r.at("c").width) == 200);
  CHECK(r.abs_x("c") == 300);

  // flex-basis wins over width
  Laid s(page(flex("width:500px", "<div id=a style='width:10px;flex:1 1 200px'></div><div id=b style='flex:1 1 100px'></div>").c_str()).c_str());
  CHECK(px(s.at("a").width) == 300 && px(s.at("b").width) == 200);   // 500 - 300 = 200 free: +100 each

  // shrinking: the shrink factor is weighted by the flex base size
  Laid t(page(flex("width:400px", "<div id=a style='width:400px'></div><div id=b style='width:200px'></div>").c_str()).c_str());
  CHECK(px(t.at("a").width) == 267 && px(t.at("b").width) == 133);
  Laid t2(page(flex("width:400px", "<div id=a style='width:300px;flex-shrink:0'></div><div id=b style='width:300px'></div>").c_str()).c_str());
  CHECK(px(t2.at("a").width) == 300 && px(t2.at("b").width) == 100);
  Laid t3(page(flex("width:400px", "<div id=a style='width:300px;flex:none'></div><div id=b style='width:300px;flex:none'></div>").c_str()).c_str());
  CHECK(px(t3.at("a").width) == 300 && px(t3.at("b").width) == 300 && t3.abs_x("b") == 300);   // overflows

  // flex: auto grows from the content width
  Laid u(page(flex("width:200px", "<div id=a style='flex:auto'>abcd</div><div id=b style='flex:auto'>ab</div>").c_str()).c_str());
  CHECK(px(u.at("a").width) == 108 && px(u.at("b").width) == 92);   // 32 and 16 + 76 each
}

static void test_min_max_clamping() {
  // max-width freezes an item and the others take what is left
  Laid p(page(flex("width:300px", "<div id=a style='flex:1;max-width:80px'></div><div id=b style='flex:1'></div>").c_str()).c_str());
  CHECK(px(p.at("a").width) == 80 && px(p.at("b").width) == 220);

  // min-width freezes an item when shrinking
  Laid q(page(flex("width:300px", "<div id=a style='width:200px;min-width:180px'></div><div id=b style='width:200px'></div>").c_str()).c_str());
  CHECK(px(q.at("a").width) == 180 && px(q.at("b").width) == 120);

  // min-width: auto keeps an item from shrinking below its longest word; min-width: 0 lets it
  Laid r(page(flex("width:100px", "<div id=a>abcdefghij</div><div id=b>abcdefghij</div>").c_str()).c_str());
  CHECK(px(r.at("a").width) == 80 && px(r.at("b").width) == 80 && r.abs_x("b") == 80);
  Laid s(page(flex("width:100px", "<div id=a style='min-width:0'>abcdefghij</div><div id=b style='min-width:0'>abcdefghij</div>").c_str()).c_str());
  CHECK(px(s.at("a").width) == 50 && px(s.at("b").width) == 50);
  // text that can wrap does shrink down to the longest word
  Laid t(page(flex("width:100px", "<div id=a>ab cd ef gh ij</div><div id=b>ab cd ef gh ij</div>").c_str()).c_str());
  CHECK(px(t.at("a").width) == 50 && px(t.at("b").width) == 50 && px(t.at("a").height) == 48);   // (5 words of 16px fit 6 per... 50px holds 3 chars: "ab", "cd", ... one per line = 5 lines?)
  // a specified width limits the automatic minimum
  Laid u(page(flex("width:100px", "<div id=a style='width:30px'>abcdefghij</div><div id=b style='flex:1;min-width:0'></div>").c_str()).c_str());
  CHECK(px(u.at("a").width) == 30);
  // overflow other than visible makes the minimum 0
  Laid v(page(flex("width:40px", "<div id=a style='overflow:hidden'>abcdefghij</div>").c_str()).c_str());
  CHECK(px(v.at("a").width) == 40);
}

static void test_justify_content() {
  const char* items = "<div id=a style='width:60px;height:10px'></div><div id=b style='width:60px;height:10px'></div><div id=c style='width:60px;height:10px'></div>";
  struct { const char* value; int a, b, c; } cases[] = {
      {"flex-start", 0, 60, 120}, {"flex-end", 120, 180, 240}, {"center", 60, 120, 180}, {"space-between", 0, 120, 240},
      {"space-around", 20, 120, 220}, {"space-evenly", 30, 120, 210}, {"normal", 0, 60, 120}, {"start", 0, 60, 120}, {"end", 120, 180, 240}};
  for (const auto& tc : cases) {
    const std::string css = std::string("width:300px;justify-content:") + tc.value;
    Laid p(page(flex(css.c_str(), items).c_str()).c_str());
    CHECK(p.abs_x("a") == tc.a && p.abs_x("b") == tc.b && p.abs_x("c") == tc.c);
  }
  // not enough room: space-between falls back to flex-start, center overflows on both sides
  Laid q(page(flex("width:100px;justify-content:space-between", "<div id=a style='width:60px;flex:none'></div><div id=b style='width:60px;flex:none'></div>").c_str()).c_str());
  CHECK(q.abs_x("a") == 0 && q.abs_x("b") == 60);
  Laid q2(page(flex("width:100px;justify-content:space-around", "<div id=a style='width:60px;flex:none'></div><div id=b style='width:60px;flex:none'></div>").c_str()).c_str());
  CHECK(q2.abs_x("a") == 0 && q2.abs_x("b") == 60);
  Laid r(page(flex("width:100px;justify-content:center;flex-wrap:nowrap", "<div id=a style='width:80px;flex:none'></div><div id=b style='width:80px;flex:none'></div>").c_str()).c_str());
  CHECK(r.abs_x("a") == -30 && r.abs_x("b") == 50);

  // a navbar: logo left, links on the right
  Laid n(page("<nav id=f style='display:flex;justify-content:space-between;width:400px'><a id=logo>Logo</a><div id=links style='display:flex'><a id=l1>One</a><a id=l2>Two</a></div></nav>").c_str());
  CHECK(n.ok);
  CHECK(n.abs_x("logo") == 0 && px(n.at("logo").width) == 32);
  CHECK(px(n.at("links").width) == 48 && n.abs_x("links") == 352 && n.abs_x("l2") == 376);
}

static void test_cross_axis() {
  const char* items = "<div id=a style='width:20px;height:20px'></div><div id=b style='width:20px;height:30px'></div><div id=c style='width:20px'></div>";
  struct { const char* value; int ay, by, ch; } cases[] = {{"flex-start", 0, 0, 0}, {"flex-end", 80, 70, 0}, {"center", 40, 35, 0}, {"stretch", 0, 0, 100}};
  for (const auto& tc : cases) {
    const std::string css = std::string("height:100px;align-items:") + tc.value;
    Laid p(page(flex(css.c_str(), items).c_str()).c_str());
    CHECK(p.abs_y("a") == tc.ay && p.abs_y("b") == tc.by);
    if (tc.ch) CHECK(px(p.at("c").height) == tc.ch && p.abs_y("c") == 0 && px(p.at("a").height) == 20);
  }
  // align-self overrides align-items
  Laid q(page(flex("height:100px;align-items:flex-end", "<div id=a style='height:20px;align-self:flex-start'></div><div id=b style='height:20px'></div><div id=c style='height:20px;align-self:center'></div>").c_str()).c_str());
  CHECK(q.abs_y("a") == 0 && q.abs_y("b") == 80 && q.abs_y("c") == 40);

  // centring a box in a box
  Laid r(page("<div id=f style='display:flex;justify-content:center;align-items:center;width:400px;height:200px'><div id=a style='width:100px;height:50px'></div></div>").c_str());
  CHECK(r.abs_x("a") == 150 && r.abs_y("a") == 75);

  // stretch does not apply to an item with a height, nor with auto margins
  Laid s(page(flex("height:100px", "<div id=a style='height:20px'></div><div id=b style='margin-top:auto;height:10px'></div>").c_str()).c_str());
  CHECK(px(s.at("a").height) == 20 && px(s.at("b").height) == 10 && s.abs_y("b") == 90);

  // baseline: the text of the items lines up whatever the sizes
  Laid t(page(flex("align-items:baseline", "<div id=a>aa</div><div id=b style='font-size:32px'>bb</div><div id=c style='margin-top:10px'>cc</div>").c_str()).c_str());
  CHECK(t.ok);
  // ascent of 32px text is 25.6, of 16px text 12.8, with the 10px margin 22.8: the largest decides
  CHECK(px(t.at("f").height) == 32);
  CHECK(t.abs_y("b") == 0 && px(t.at("a").y) == 13 && px(t.at("c").y) == 13);   // (its 10px margin counts: 10 + 2.8)
  // the container's baseline is that of the shared baseline of the first line
  CHECK(t.at("f").has_baseline && t.at("f").baseline.to_double() > 25.5 && t.at("f").baseline.to_double() < 25.7);
}

static void test_wrapping_and_align_content() {
  const char* items = "<div id=a style='width:100px;height:20px'></div><div id=b style='width:100px;height:20px'></div><div id=c style='width:100px;height:20px'></div><div id=d style='width:100px;height:20px'></div>";
  Laid p(page(flex("width:300px;flex-wrap:wrap", items).c_str()).c_str());
  CHECK(p.abs_x("a") == 0 && p.abs_x("b") == 100 && p.abs_x("c") == 200 && p.abs_x("d") == 0 && p.abs_y("d") == 20);
  CHECK(px(p.at("f").height) == 40);

  // wrapped items grow within their own line
  Laid q(page(flex("width:300px;flex-wrap:wrap", "<div id=a style='flex:1 1 200px;height:20px'></div><div id=b style='flex:1 1 200px;height:20px'></div><div id=c style='flex:1 1 100px;height:20px'></div>").c_str()).c_str());
  CHECK(px(q.at("a").width) == 300 && px(q.at("b").width) == 200 && px(q.at("c").width) == 100 && q.abs_y("b") == 20 && q.abs_x("c") == 200);

  // wrapping cards with a gap
  Laid r(page(flex("width:320px;flex-wrap:wrap;gap:10px", items).c_str()).c_str());
  CHECK(r.abs_x("b") == 110 && r.abs_x("c") == 220 && r.abs_x("d") == 0 && r.abs_y("d") == 30 && px(r.at("f").height) == 50);
  Laid r2(page(flex("width:320px;flex-wrap:wrap;gap:5px 10px;", items).c_str()).c_str());
  CHECK(r2.abs_y("d") == 25 && r2.abs_x("b") == 110);
  Laid r3(page(flex("width:300px;flex-wrap:wrap;column-gap:20px", "<div id=a style='width:140px;height:5px'></div><div id=b style='width:140px;height:5px'></div><div id=c style='width:140px;height:5px'></div>").c_str()).c_str());
  CHECK(r3.abs_x("b") == 160 && r3.abs_y("c") == 5);   // (140 + 20 + 140 = 300 fits)

  // align-content with room to spare in the cross axis (the lines are 20px high, the container 100px)
  struct { const char* value; int cy, dy; } cases[] = {{"flex-start", 0, 20}, {"flex-end", 60, 80}, {"center", 30, 50}, {"space-between", 0, 80}, {"space-around", 15, 65}, {"space-evenly", 20, 60}, {"stretch", 0, 50}, {"normal", 0, 50}};
  for (const auto& tc : cases) {
    const std::string css = std::string("width:200px;height:100px;flex-wrap:wrap;align-content:") + tc.value;
    Laid s(page(flex(css.c_str(), items).c_str()).c_str());
    CHECK(s.abs_y("a") == tc.cy);
    CHECK(s.abs_y("c") == tc.dy);
  }
  // wrap-reverse stacks the lines from the bottom
  Laid t(page(flex("width:200px;flex-wrap:wrap-reverse", items).c_str()).c_str());
  CHECK(t.abs_y("a") == 20 && t.abs_y("c") == 0 && t.abs_x("a") == 0 && t.abs_x("b") == 100);
}

static void test_directions() {
  const char* items = "<div id=a style='width:50px;height:10px'></div><div id=b style='width:60px;height:20px'></div>";
  Laid p(page(flex("width:300px;flex-direction:row-reverse", items).c_str()).c_str());
  CHECK(p.abs_x("a") == 250 && p.abs_x("b") == 190);
  Laid q(page(flex("width:300px;flex-direction:row-reverse;justify-content:flex-end", items).c_str()).c_str());
  CHECK(q.abs_x("a") == 0 + 60 && q.abs_x("b") == 0);
  Laid r(page(flex("flex-direction:column", items).c_str()).c_str());
  CHECK(r.abs_y("a") == 0 && r.abs_y("b") == 10 && px(r.at("f").height) == 30);
  Laid r2(page(flex("flex-direction:column", "<div id=a style='height:10px'></div><div id=b style='height:20px'></div>").c_str()).c_str());
  CHECK(px(r2.at("a").width) == 800 && px(r2.at("b").width) == 800);   // (stretch)
  Laid s(page(flex("flex-direction:column-reverse;height:100px", items).c_str()).c_str());
  CHECK(s.abs_y("a") == 90 && s.abs_y("b") == 70);
  Laid t(page(flex("flex-direction:column;align-items:center;width:200px", items).c_str()).c_str());
  CHECK(t.abs_x("a") == 75 && px(t.at("a").width) == 50 && t.abs_x("b") == 70);
  Laid u(page(flex("flex-direction:column;align-items:flex-end;width:200px", "<div id=a>abc</div>").c_str()).c_str());
  CHECK(u.abs_x("a") == 176 && px(u.at("a").width) == 24);
  Laid v(page(flex("flex-direction:column;justify-content:space-between;height:100px", items).c_str()).c_str());
  CHECK(v.abs_y("a") == 0 && v.abs_y("b") == 80);
  // rtl rows start at the right
  Laid w(page(flex("width:300px;direction:rtl", items).c_str()).c_str());
  CHECK(w.abs_x("a") == 250 && w.abs_x("b") == 190);
  // order
  Laid x(page(flex("width:300px", "<div id=a style='width:50px;order:2'></div><div id=b style='width:60px;order:-1'></div><div id=c style='width:70px'></div><div id=d style='width:10px;order:2'></div>").c_str()).c_str());
  CHECK(x.abs_x("b") == 0 && x.abs_x("c") == 60 && x.abs_x("a") == 130 && x.abs_x("d") == 180);
}

static void test_column_sizing() {
  // a column with a definite height: the middle item takes what the others leave
  Laid p(page(flex("flex-direction:column;height:300px", "<div id=h style='height:50px'></div><div id=m style='flex:1'></div><div id=ft style='height:40px'></div>").c_str()).c_str());
  CHECK(px(p.at("m").height) == 210 && p.abs_y("m") == 50 && p.abs_y("ft") == 260);
  // a sticky footer: the page is at least as high as its container asks
  Laid q(page(flex("flex-direction:column;min-height:300px", "<div id=h style='height:20px'></div><div id=m style='flex:1'>x</div><div id=ft style='height:20px'></div>").c_str()).c_str());
  CHECK(px(q.at("f").height) == 300 && px(q.at("m").height) == 260 && q.abs_y("ft") == 280);
  // without a height or min-height the column is as high as its items, flex or not
  Laid r(page(flex("flex-direction:column", "<div id=a style='flex:1'>x</div><div id=b style='flex:1'>y</div>").c_str()).c_str());
  CHECK(px(r.at("f").height) == 32 && px(r.at("a").height) == 16);
  // the content basis of a column item is the height of its content at the container's width
  Laid s(page(flex("flex-direction:column;width:40px;height:200px", "<div id=a>aaaa bbbb cccc</div><div id=b style='flex:1'></div>").c_str()).c_str());
  CHECK(px(s.at("a").height) == 48 && px(s.at("b").height) == 152);
  // shrinking in a column; a content-sized item does not shrink below its content
  Laid t(page(flex("flex-direction:column;height:50px", "<div id=a style='height:40px'></div><div id=b style='height:40px'></div>").c_str()).c_str());
  CHECK(px(t.at("a").height) == 25 && px(t.at("b").height) == 25);
  Laid u(page(flex("flex-direction:column;height:30px", "<div id=a>x</div><div id=b>y</div>").c_str()).c_str());
  CHECK(px(u.at("a").height) == 16 && px(u.at("b").height) == 16);
  // percentage heights of the children of a stretched or flexed item
  Laid v(page(flex("flex-direction:column;height:200px", "<div id=a style='flex:1'><div id=in style='height:50%'></div></div>").c_str()).c_str());
  CHECK(px(v.at("in").height) == 100);
}

static void test_auto_margins_and_gaps() {
  Laid p(page(flex("width:300px", "<div id=a style='width:50px;height:10px'></div><div id=b style='width:50px;height:10px;margin-left:auto'></div>").c_str()).c_str());
  CHECK(p.abs_x("a") == 0 && p.abs_x("b") == 250);
  Laid q(page(flex("width:300px;height:100px", "<div id=a style='width:50px;height:10px;margin:auto'></div>").c_str()).c_str());
  CHECK(q.abs_x("a") == 125 && q.abs_y("a") == 45);
  Laid r(page(flex("width:300px", "<div id=a style='width:50px;height:10px;margin:0 auto 0 0'></div><div id=b style='width:50px;height:10px;margin:0 auto'></div><div id=c style='width:50px;height:10px'></div>").c_str()).c_str());
  CHECK(r.abs_x("a") == 0 && r.abs_x("b") == 150 && r.abs_x("c") == 250);   // (150 free: three auto margins of 50)
  // auto margins swallow the free space so justify-content has nothing left to share
  Laid s(page(flex("width:300px;justify-content:center", "<div id=a style='width:50px;margin-right:auto;height:10px'></div><div id=b style='width:50px;height:10px'></div>").c_str()).c_str());
  CHECK(s.abs_x("a") == 0 && s.abs_x("b") == 250);
  Laid t(page(flex("width:300px;column-gap:10px", "<div id=a style='width:50px;height:10px'></div><div id=b style='width:50px;height:10px'></div><div id=c style='width:50px;height:10px'></div>").c_str()).c_str());
  CHECK(t.abs_x("b") == 60 && t.abs_x("c") == 120);
  Laid u(page(flex("width:320px;gap:10px", "<div id=a style='flex:1'></div><div id=b style='flex:1'></div><div id=c style='flex:1'></div>").c_str()).c_str());
  CHECK(px(u.at("a").width) == 100 && u.abs_x("b") == 110 && u.abs_x("c") == 220);
  Laid v(page(flex("flex-direction:column;height:100px;gap:10px", "<div id=a style='height:20px'></div><div id=b style='height:20px'></div>").c_str()).c_str());
  CHECK(v.abs_y("b") == 30);
}

static void test_percentages_and_boxes() {
  Laid p(page(flex("width:400px", "<div id=a style='width:50%;height:10px'></div><div id=b style='flex-basis:25%;height:10px'></div>").c_str()).c_str());
  CHECK(px(p.at("a").width) == 200 && px(p.at("b").width) == 100);
  // box-sizing and padding count in the flex basis
  Laid q(page(flex("width:400px", "<div id=a style='width:100px;padding:10px;box-sizing:border-box;flex:none;height:10px'></div><div id=b style='width:100px;padding:10px;flex:none;height:10px'></div>").c_str()).c_str());
  CHECK(px(q.at("a").width) == 100 && px(q.at("b").width) == 120 && q.abs_x("b") == 100);
  Laid r(page(flex("width:400px", "<div id=a style='flex:1;padding:0 20px;height:10px'></div><div id=b style='flex:1;height:10px'></div>").c_str()).c_str());
  CHECK(px(r.at("a").width) == 220 && px(r.at("b").width) == 180);   // 400 = 40 + 2 * 180 free
  // images are items too
  Laid s(page(flex("width:400px", "<img id=i><div id=a style='flex:1'></div>").c_str()).c_str());
  CHECK(px(s.at("i").width) == 100 && px(s.at("i").height) == 50 && px(s.at("a").width) == 300 && px(s.at("f").height) == 50);
  // a percentage height needs a definite height on the container
  Laid t(page(flex("height:200px", "<div id=a style='height:50%;width:10px'></div>").c_str()).c_str());
  CHECK(px(t.at("a").height) == 100);
  // relatively positioned items move after they are placed
  Laid u(page(flex("width:300px", "<div id=a style='width:50px;height:10px;position:relative;left:7px;top:3px'></div><div id=b style='width:50px;height:10px'></div>").c_str()).c_str());
  CHECK(u.abs_x("a") == 7 && u.abs_y("a") == 3 && u.abs_x("b") == 50);
}

static void test_items_from_content() {
  // text and elements of any display become items; blank text between them is nothing
  Laid p(page("<div id=f style='display:flex;width:400px'>\n  <span id=a>abc</span>\n  <a id=b>de</a>\n  <b id=c style='display:inline-block'>f</b> loose text\n</div>").c_str());
  CHECK(p.ok);
  CHECK(px(p.at("a").width) == 24 && p.abs_x("a") == 0);
  CHECK(px(p.at("b").width) == 16 && p.abs_x("b") == 24);
  CHECK(px(p.at("c").width) == 8 && p.abs_x("c") == 40);
  // a container that holds only text
  Laid q(page(flex("width:400px;justify-content:center", "abc").c_str()).c_str());
  CHECK(q.ok && px(q.at("f").height) == 16);
  // floats do not apply to items
  Laid r(page(flex("width:300px", "<div id=a style='float:right;width:50px;height:10px'></div><div id=b style='width:50px;height:10px'></div>").c_str()).c_str());
  CHECK(r.abs_x("a") == 0 && r.abs_x("b") == 50);
  // an empty container
  Laid s(page(flex("width:300px", "").c_str()).c_str());
  CHECK(s.ok && px(s.at("f").height) == 0);
  Laid s2(page(flex("width:300px;min-height:40px", "").c_str()).c_str());
  CHECK(px(s2.at("f").height) == 40);
  // display: none items make no room; display: contents children are the items
  Laid t(page(flex("width:300px", "<div style='display:none;width:50px'></div><div style='display:contents'><div id=a style='width:20px;height:5px'></div><div id=b style='width:30px;height:5px'></div></div>").c_str()).c_str());
  CHECK(t.abs_x("a") == 0 && t.abs_x("b") == 20);
  // nested containers and an item that is itself a column
  Laid u(page("<div id=f style='display:flex;width:300px;height:100px'><div id=side style='width:80px'></div><div id=main style='flex:1;display:flex;flex-direction:column'><div id=top style='height:20px'></div><div id=fill style='flex:1'></div></div></div>").c_str());
  CHECK(px(u.at("main").width) == 220 && px(u.at("main").height) == 100 && px(u.at("fill").height) == 80 && u.abs_x("fill") == 80 && u.abs_y("fill") == 20);
}

static void test_intrinsic_and_inline_flex() {
  // a floated container is as wide as its items want
  Laid p(page("<div id=f style='display:flex;float:left'><div style='width:100px;height:5px'></div><div style='width:50px;height:5px'></div></div>").c_str());
  CHECK(px(p.at("f").width) == 150);
  Laid q(page("<div id=f style='display:flex;flex-direction:column;float:left'><div style='width:100px;height:5px'></div><div style='width:50px;height:5px'></div></div>").c_str());
  CHECK(px(q.at("f").width) == 100 && px(q.at("f").height) == 10);
  Laid r(page("<div id=f style='display:flex;float:left;gap:10px'><div>abc</div><div>de</div></div>").c_str());
  CHECK(px(r.at("f").width) == 24 + 16 + 10);
  // inline-flex sits in a line like a word and is as wide as its items
  Laid s(page("<div id=o>ab <span id=f style='display:inline-flex'><i id=x>cd</i><i id=y>ef</i></span> gh</div>").c_str());
  CHECK(s.ok && px(s.at("f").width) == 32 && px(s.at("x").width) == 16 && s.abs_x("y") == 24 + 16);
  CHECK(px(s.at("o").height) == 16);
  // a flex container shrinks to fit an available width smaller than its content
  Laid t(page("<div style='width:30px;float:left'><div id=f style='display:flex;float:left'><div id=a>abcdef</div><div id=b>abcdef</div></div></div>").c_str());
  CHECK(px(t.at("f").width) == 96 && px(t.at("a").width) == 48);
  // the width of a flex item that is a flex container: the sum of its items
  Laid u(page(flex("width:400px", "<div id=a style='display:flex'><div style='width:70px;height:5px'></div><div style='width:30px;height:5px'></div></div><div id=b style='flex:1'></div>").c_str()).c_str());
  CHECK(px(u.at("a").width) == 100 && px(u.at("b").width) == 300);
}

static void test_absolute_children() {
  Laid p(page("<div id=f style='display:flex;position:relative;width:300px;height:100px;padding:10px'><div id=a style='width:50px;height:10px'></div><div id=ab style='position:absolute;right:0;top:0;width:20px;height:20px'></div><div id=b style='width:50px;height:10px'></div></div>").c_str());
  CHECK(p.abs_x("b") == 60 && p.abs_x("a") == 10);   // (the positioned child takes no room)
  CHECK(p.abs_x("ab") == 300 + 20 - 20 && p.abs_y("ab") == 0);
}

static void test_robustness() {
  // absurd sizes, zero widths and deep nesting must not crash or loop
  Laid p(page(flex("width:100px;flex-wrap:wrap", "<div style='width:99999999px;height:5px'></div><div style='flex:1000000 1 0;height:5px'></div><div style='flex-shrink:99999999;width:50px'></div>").c_str()).c_str());
  CHECK(p.ok);
  Laid q(page(flex("width:0", "<div style='flex:1'>text</div><div style='flex:1'>text</div>").c_str()).c_str());
  CHECK(q.ok);
  Laid r(page(flex("width:100px;flex-direction:column;height:0", "<div style='flex:1'>text</div>").c_str()).c_str());
  CHECK(r.ok);
  std::string deep;
  for (int i = 0; i < 12; ++i) deep += (i % 2 ? "<div style='display:flex;flex-direction:column'>" : "<div style='display:flex'>");
  deep += "<div id=leaf style='width:10px;height:10px'>x</div>";
  for (int i = 0; i < 12; ++i) deep += "</div>";
  Laid s(page(deep.c_str()).c_str());
  CHECK(s.ok && px(s.at("leaf").width) == 10);
  // a flex container with items whose text is wide
  Laid t(page(flex("width:50px", "<div>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</div>").c_str()).c_str());
  CHECK(t.ok);
  // negative and huge gaps and margins
  Laid u(page(flex("width:100px;gap:99999999px", "<div style='width:10px;margin:-99999999px'></div><div style='width:10px'></div>").c_str()).c_str());
  CHECK(u.ok);
  // real fonts
  Laid w(page(flex("width:300px;justify-content:space-between", "<div id=a>Home</div><div id=b>About us</div>").c_str()).c_str(), 800, true);
  CHECK(w.ok && w.abs_x("a") == 0 && w.abs_x("b") + px(w.at("b").width) == 300);
}

int main() {
  {
  font::FontSet fonts(g_alloc);
  fonts.add_bundled_fonts();
  g_fonts = &fonts;
  test_row_basics();
  test_flex_grow_and_shrink();
  test_min_max_clamping();
  test_justify_content();
  test_cross_axis();
  test_wrapping_and_align_content();
  test_directions();
  test_column_sizing();
  test_auto_margins_and_gaps();
  test_percentages_and_boxes();
  test_items_from_content();
  test_intrinsic_and_inline_flex();
  test_absolute_children();
  test_robustness();
  g_fonts = nullptr;
  }
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("flex tests passed\n");
  return 0;
}
