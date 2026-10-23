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

// Host-side tests for grid layout (src/Layout/Grid.cpp). Text is measured with FixedWidthMeasurer: every character is 8px wide at the default
// 16px font and a line is 16px high, so the expected numbers can be worked out by hand from the CSS Grid rules.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "Browser/Renderer.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Layout/BoxBuilder.hpp"
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
  FakeObjects objects;
  bool ok = false;

  explicit Laid(const char* html, int width = 800) : doc(g_alloc), sheet(g_alloc), tree(g_alloc) {
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
    options.measurer = &fixed_measurer;
    options.sheet = &sheet;
    ok = build_box_tree(doc, sheet, build, &tree) && layout_tree(tree, options);
  }
  BoxId box(const char* id) const {
    const dom::Node* n = doc.element_by_id(id);
    if (!n) return kNoBox;
    for (uint64_t i = 0; i < tree.count(); ++i)
      if (tree.box(static_cast<BoxId>(i)).node == n->serial && tree.box(static_cast<BoxId>(i)).kind != BoxKind::Text) return static_cast<BoxId>(i);
    return kNoBox;
  }
  const Box& at(const char* id) const { return tree.box(box(id)); }
  int abs_x(const char* id) const { return abs(box(id), true); }
  int abs_y(const char* id) const { return abs(box(id), false); }
  int abs(BoxId b, bool x) const {
    int sum = 0;
    for (BoxId cur = b; cur != kNoBox; cur = tree.box(cur).parent) {
      const Box& bx = tree.box(cur);
      if (bx.kind == BoxKind::Inline) continue;
      sum += (x ? bx.x : bx.y).round();
    }
    return sum;
  }
  int w(const char* id) const { return at(id).width.round(); }
  int h(const char* id) const { return at(id).height.round(); }
};

static const char* kReset = "<style>html,body,p,div,ul,li,h1{margin:0;padding:0}</style>";
static std::string page(const char* body, const char* css = "") { return std::string(kReset) + "<style>" + css + "</style>" + body; }

// A grid container "g" (with the given CSS) holding the given items.
static std::string grid(const char* css, const char* items, int width = 0) {
  std::string c = std::string("display:grid;") + css;
  if (width) c += ";width:" + std::to_string(width) + "px";
  return page((std::string("<div id=g style='") + c + "'>" + items + "</div>").c_str());
}

static void test_fr_and_gaps() {
  // fr columns share what the gaps leave: 800 - 2*20 = 760 in the ratio 1:2:1
  Laid p(grid("grid-template-columns:1fr 2fr 1fr;column-gap:20px", "<div id=a>a</div><div id=b>b</div><div id=c>c</div>").c_str());
  CHECK(p.ok);
  CHECK(p.w("a") == 190 && p.w("b") == 380 && p.w("c") == 190);
  CHECK(p.abs_x("a") == 0 && p.abs_x("b") == 210 && p.abs_x("c") == 610);
  CHECK(p.h("a") == 16 && p.h("g") == 16);

  // gap shorthand with rows: two rows of 16px and a 10px gap
  Laid q(grid("grid-template-columns:100px 100px;gap:10px", "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div>").c_str());
  CHECK(q.abs_x("b") == 110 && q.abs_y("c") == 26 && q.abs_y("d") == 26 && q.h("g") == 42);

  // fixed, auto and fr: the auto column is as wide as its text, the fr column takes the rest
  Laid r(grid("grid-template-columns:100px auto 1fr", "<div id=a>a</div><div id=b>hello</div><div id=c>c</div>").c_str());
  CHECK(r.w("a") == 100 && r.w("b") == 40 && r.w("c") == 660);
  CHECK(r.abs_x("b") == 100 && r.abs_x("c") == 140);

  // auto columns share the free room equally (stretch): 48px of text, 752px left
  Laid s(grid("grid-template-columns:auto auto", "<div id=a>aaaa</div><div id=b>bb</div>").c_str());
  CHECK(s.w("a") == 32 + 376 && s.w("b") == 16 + 376);

  // 0.5fr + 0.5fr fill everything; fr smaller than the content grows to it (the minimum is the content)
  Laid t(grid("grid-template-columns:1fr 1fr", "<div id=a>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</div><div id=b>b</div>", 400).c_str());
  CHECK(t.w("a") > 400 && t.w("b") == 8);
  // minmax(0, 1fr) does not grow to the content
  Laid u(grid("grid-template-columns:minmax(0,1fr) minmax(0,1fr)", "<div id=a>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</div><div id=b>b</div>", 400).c_str());
  CHECK(u.w("a") == 200 && u.w("b") == 200);

  // percentages
  Laid v(grid("grid-template-columns:25% 75%", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(v.w("a") == 200 && v.w("b") == 600);
  Laid w(grid("grid-template-columns:calc(100% - 100px) 100px", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(w.w("a") == 700 && w.w("b") == 100);
  // repeat() with a count
  Laid x(grid("grid-template-columns:repeat(4, 1fr)", "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div>").c_str());
  CHECK(x.w("a") == 200 && x.abs_x("d") == 600 && x.abs_y("e") == 16 && x.abs_x("e") == 0);
}

static void test_rows_and_areas() {
  // rows: a fixed one and one from the content
  Laid p(grid("grid-template-columns:100px;grid-template-rows:50px auto", "<div id=a>a</div><div id=b style='height:30px'></div>").c_str());
  CHECK(p.h("a") == 50 && p.h("b") == 30 && p.abs_y("b") == 50 && p.h("g") == 80);

  // the implicit rows: grid-auto-rows
  Laid q(grid("grid-template-columns:100px;grid-auto-rows:40px", "<div id=a>a</div><div id=b>b</div><div id=c>c</div>").c_str());
  CHECK(q.h("a") == 40 && q.abs_y("c") == 80 && q.h("g") == 120);

  // the holy grail: header, nav / main / aside, footer, in a container 300px high
  Laid r(page(
      "<div id=g style='display:grid;height:300px;grid-template-columns:100px 1fr 100px;grid-template-rows:auto 1fr auto;"
      "grid-template-areas:\"head head head\" \"nav main side\" \"foot foot foot\"'>"
      "<div id=head style='grid-area:head;height:20px'></div><div id=nav style='grid-area:nav'>n</div>"
      "<div id=main style='grid-area:main'>m</div><div id=side style='grid-area:side'>s</div>"
      "<div id=foot style='grid-area:foot;height:30px'></div></div>").c_str());
  CHECK(r.ok);
  CHECK(r.w("head") == 800 && r.h("head") == 20 && r.abs_y("head") == 0);
  CHECK(r.abs_x("nav") == 0 && r.w("nav") == 100 && r.abs_y("nav") == 20 && r.h("nav") == 250);
  CHECK(r.abs_x("main") == 100 && r.w("main") == 600 && r.h("main") == 250);
  CHECK(r.abs_x("side") == 700 && r.w("side") == 100);
  CHECK(r.abs_y("foot") == 270 && r.h("foot") == 30 && r.w("foot") == 800);

  // the same with min-height: the fr row takes the room the minimum height leaves
  Laid s(page(
      "<div id=g style='display:grid;min-height:300px;grid-template-rows:auto 1fr auto'>"
      "<div id=head style='height:20px'></div><div id=main>m</div><div id=foot style='height:30px'></div></div>").c_str());
  CHECK(s.h("g") == 300 && s.h("main") == 250 && s.abs_y("foot") == 270);
  // without a height the 1fr row is the content
  Laid t(page(
      "<div id=g style='display:grid;grid-template-rows:auto 1fr auto'>"
      "<div id=head style='height:20px'></div><div id=main>m</div><div id=foot style='height:30px'></div></div>").c_str());
  CHECK(t.h("g") == 66 && t.h("main") == 16);

  // areas with some empty cells and an item in the gap
  Laid u(page(
      "<div id=g style='display:grid;grid-template-columns:100px 100px;grid-template-areas:\"a .\" \". b\"'>"
      "<div id=a style='grid-area:a'>a</div><div id=b style='grid-area:b'>b</div></div>").c_str());
  CHECK(u.abs_x("a") == 0 && u.abs_y("a") == 0 && u.abs_x("b") == 100 && u.abs_y("b") == 16);
}

static void test_placement() {
  // spans and line numbers
  Laid p(grid("grid-template-columns:repeat(4,100px)",
              "<div id=a style='grid-column:1 / span 2'>a</div><div id=b style='grid-column:3 / 5'>b</div>"
              "<div id=c style='grid-column:2 / -1'>c</div><div id=d style='grid-column:span 3'>d</div>").c_str());
  CHECK(p.w("a") == 200 && p.abs_x("a") == 0);
  CHECK(p.w("b") == 200 && p.abs_x("b") == 200);
  CHECK(p.w("c") == 300 && p.abs_x("c") == 100 && p.abs_y("c") == 16);
  CHECK(p.w("d") == 300 && p.abs_y("d") == 32);

  // rows
  Laid q(grid("grid-template-columns:100px 100px;grid-template-rows:50px 50px 50px",
              "<div id=a style='grid-row:1 / span 2'>a</div><div id=b>b</div><div id=c>c</div>").c_str());
  CHECK(q.h("a") == 100 && q.abs_x("b") == 100 && q.abs_y("b") == 0 && q.abs_x("c") == 100 && q.abs_y("c") == 50);

  // named lines
  Laid r(grid("grid-template-columns:[start] 100px [mid] 1fr [end]",
              "<div id=a style='grid-column:mid / end'>a</div><div id=b style='grid-column:start / mid'>b</div>").c_str());
  CHECK(r.abs_x("a") == 100 && r.w("a") == 700 && r.w("b") == 100 && r.abs_x("b") == 0);

  // sparse and dense packing: a and b take two columns of three, c is one column
  const char* items = "<div id=a style='grid-column:span 2'>a</div><div id=b style='grid-column:span 2'>b</div><div id=c>c</div>";
  Laid s(grid("grid-template-columns:repeat(3,100px)", items).c_str());
  CHECK(s.abs_y("b") == 16 && s.abs_x("c") == 200 && s.abs_y("c") == 16);   // sparse: c follows b
  Laid t(grid("grid-template-columns:repeat(3,100px);grid-auto-flow:dense", items).c_str());
  CHECK(t.abs_y("b") == 16 && t.abs_x("c") == 200 && t.abs_y("c") == 0);   // dense: c fills the hole after a

  // column flow
  Laid u(grid("grid-template-rows:repeat(2,20px);grid-auto-flow:column;grid-auto-columns:100px", "<div id=a>a</div><div id=b>b</div><div id=c>c</div>").c_str());
  CHECK(u.abs_x("a") == 0 && u.abs_y("b") == 20 && u.abs_x("c") == 100 && u.abs_y("c") == 0);

  // items with a definite row come first; an item on a negative line makes an implicit track before the explicit grid
  Laid v(grid("grid-template-columns:100px 100px", "<div id=a style='grid-column:-4 / span 1;grid-row:1'>a</div><div id=b style='grid-column:1'>b</div>").c_str());
  CHECK(v.abs_x("a") == 0 && v.abs_x("b") == 600);   // (the implicit column before the grid is auto: it takes the room left over)

  // order
  Laid w(grid("grid-template-columns:100px 100px", "<div id=a style='order:2'>a</div><div id=b style='order:1'>b</div>").c_str());
  CHECK(w.abs_x("b") == 0 && w.abs_x("a") == 100);

  // implicit columns: grid-auto-columns
  Laid x(grid("grid-template-columns:100px;grid-auto-columns:50px", "<div id=a style='grid-column:1'>a</div><div id=b style='grid-column:3'>b</div>").c_str());
  CHECK(x.abs_x("b") == 100 + 50 && x.w("b") == 50);
}

static void test_auto_repeat() {
  // repeat(auto-fill, minmax(100px, 1fr)): 7 columns of 105.7px fit in 800px with 10px gaps; the 8th item starts the second row
  Laid p(grid("grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:10px",
              "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div><div id=f>f</div><div id=g2>g</div><div id=h>h</div>").c_str());
  CHECK(p.w("a") == 106 && p.abs_x("g2") == 694);
  CHECK(p.abs_y("h") == 26 && p.abs_x("h") == 0);
  // 350px: three columns of 110px with 10px gaps
  Laid q(grid("grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:10px", "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div>", 350).c_str());
  CHECK(q.w("a") == 110 && q.abs_x("c") == 240 && q.abs_y("d") == 26);
  // auto-fill keeps the empty columns, auto-fit collapses them
  Laid r(grid("grid-template-columns:repeat(auto-fill,100px)", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(r.w("a") == 100 && r.abs_x("b") == 100);
  Laid s(grid("grid-template-columns:repeat(auto-fit,minmax(100px,1fr))", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(s.w("a") == 400 && s.w("b") == 400 && s.abs_x("b") == 400);
  Laid t(grid("grid-template-columns:repeat(auto-fill,minmax(100px,1fr))", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(t.w("a") == 100 && t.abs_x("b") == 100);
}

static void test_spanning_items() {
  // an item over two auto columns makes them wide enough; the single items come first
  Laid p(grid("grid-template-columns:auto auto 1fr",
              "<div id=a>a</div><div id=b>b</div><div id=c style='grid-column:1 / span 2'>cccccccccc</div>").c_str());
  CHECK(p.w("a") == 40 && p.w("b") == 40 && p.w("c") == 80);
  // the item over a flexible track
  Laid q(grid("grid-template-columns:100px 1fr", "<div id=a style='grid-column:1 / span 2'>aaaa</div>").c_str());
  CHECK(q.w("a") == 800);
  // rows: an item over two rows makes them high enough
  Laid r(grid("grid-template-columns:100px 100px;grid-template-rows:auto auto", "<div id=a style='grid-row:1 / span 2;height:100px'></div><div id=b>b</div><div id=c>c</div>").c_str());
  CHECK(r.h("a") == 100 && r.abs_y("c") >= 16 && r.h("g") == 100);
}

static void test_alignment() {
  const char* sized = "<div id=a style='width:20px;height:10px'></div>";
  // justify-items / align-items
  Laid p(grid("grid-template-columns:200px;grid-template-rows:100px;justify-items:center;align-items:end", sized).c_str());
  CHECK(p.abs_x("a") == 90 && p.abs_y("a") == 90);
  Laid q(grid("grid-template-columns:200px;grid-template-rows:100px;justify-items:end;align-items:center", sized).c_str());
  CHECK(q.abs_x("a") == 180 && q.abs_y("a") == 45);
  Laid r(grid("grid-template-columns:200px;grid-template-rows:100px", "<div id=a style='width:20px;height:10px;justify-self:end;align-self:start'></div>").c_str());
  CHECK(r.abs_x("a") == 180 && r.abs_y("a") == 0);
  // place-items
  Laid r2(grid("grid-template-columns:200px;grid-template-rows:100px;place-items:center", sized).c_str());
  CHECK(r2.abs_x("a") == 90 && r2.abs_y("a") == 45);
  // stretch is the default: the item fills the area; an item with a width does not
  Laid s(grid("grid-template-columns:200px;grid-template-rows:100px", "<div id=a>a</div>").c_str());
  CHECK(s.w("a") == 200 && s.h("a") == 100);
  // start alignment: shrink to the content
  Laid t(grid("grid-template-columns:200px;grid-template-rows:100px;justify-items:start;align-items:start", "<div id=a>aaa</div>").c_str());
  CHECK(t.w("a") == 24 && t.h("a") == 16);
  // auto margins
  Laid u(grid("grid-template-columns:200px;grid-template-rows:100px", "<div id=a style='width:20px;height:10px;margin:auto'></div>").c_str());
  CHECK(u.abs_x("a") == 90 && u.abs_y("a") == 45);
  Laid u2(grid("grid-template-columns:200px;grid-template-rows:100px", "<div id=a style='width:20px;height:10px;margin-left:auto'></div>").c_str());
  CHECK(u2.abs_x("a") == 180 && u2.abs_y("a") == 0);
  // margins reduce the stretched size
  Laid v(grid("grid-template-columns:200px;grid-template-rows:100px", "<div id=a style='margin:10px 20px'>a</div>").c_str());
  CHECK(v.w("a") == 160 && v.h("a") == 80 && v.abs_x("a") == 20 && v.abs_y("a") == 10);

  // justify-content / align-content
  const char* three = "<div id=a>a</div><div id=b>b</div><div id=c>c</div>";
  Laid w(grid("grid-template-columns:repeat(3,100px);justify-content:space-between", three).c_str());
  CHECK(w.abs_x("a") == 0 && w.abs_x("b") == 350 && w.abs_x("c") == 700);
  Laid w2(grid("grid-template-columns:repeat(3,100px);justify-content:center", three).c_str());
  CHECK(w2.abs_x("a") == 250 && w2.abs_x("c") == 450);
  Laid w3(grid("grid-template-columns:repeat(3,100px);justify-content:end", three).c_str());
  CHECK(w3.abs_x("a") == 500);
  Laid w4(grid("grid-template-columns:repeat(3,100px);justify-content:space-around", three).c_str());
  CHECK(w4.abs_x("a") == 83 && w4.abs_x("b") == 350);
  Laid w5(grid("grid-template-columns:repeat(3,100px);justify-content:space-evenly", three).c_str());
  CHECK(w5.abs_x("a") == 125 && w5.abs_x("b") == 350);
  Laid x(grid("height:200px;grid-template-columns:100px;grid-template-rows:50px 50px;align-content:center", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(x.abs_y("a") == 50 && x.abs_y("b") == 100);
  // align-content: stretch (the default) grows auto rows
  Laid y(grid("height:200px;grid-template-columns:100px;grid-template-rows:auto auto", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(y.h("a") == 100 && y.abs_y("b") == 100);

  // baseline alignment: the items' first lines meet
  Laid z(grid("grid-template-columns:100px 100px;align-items:baseline",
              "<div id=a style='padding-top:10px'>a</div><div id=b>b</div>").c_str());
  CHECK(z.abs_y("a") == 0 && z.abs_y("b") == 10 && z.h("g") == 26);
}

static void test_content_sizes() {
  // min-content and max-content columns
  Laid p(grid("grid-template-columns:min-content max-content", "<div id=a>aa bbbb</div><div id=b>aa bbbb</div>").c_str());
  CHECK(p.w("a") == 32 && p.w("b") == 56);
  // fit-content: the content up to the limit
  Laid q(grid("grid-template-columns:fit-content(30px) fit-content(200px)", "<div id=a>aaaa aaaa</div><div id=b>aaaa aaaa</div>").c_str());
  CHECK(q.w("a") == 32 && q.w("b") == 72);
  // minmax(100px, auto)
  Laid r(grid("grid-template-columns:minmax(100px,auto) 1fr", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(r.w("a") == 100 && r.w("b") == 700);
  // the width of an item is respected for auto columns
  Laid s(grid("grid-template-columns:auto 1fr", "<div id=a style='width:120px'>a</div><div id=b>b</div>").c_str());
  CHECK(s.w("a") == 120 && s.w("b") == 680);
  // padding and margin of items count
  Laid t(grid("grid-template-columns:auto 1fr", "<div id=a style='padding:0 10px;margin:0 5px'>aa</div><div id=b>b</div>").c_str());
  CHECK(t.w("a") == 16 + 20 && t.abs_x("b") == 36 + 10 + 0);
}

static void test_real_world() {
  // grid-area as four lines: row-start / column-start / row-end / column-end
  Laid p(grid("grid-template-columns:repeat(4,100px);grid-template-rows:repeat(3,20px)", "<div id=a style='grid-area:2 / 2 / 4 / 4'>a</div>").c_str());
  CHECK(p.abs_x("a") == 100 && p.abs_y("a") == 20 && p.w("a") == 200 && p.h("a") == 40);
  // a card grid: pictures and captions in rows of the same height
  Laid q(grid("grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px",
              "<div id=a><img src=x><p id=ta>short</p></div><div id=b><img src=x><p id=tb>aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa aaaa</p></div>"
              "<div id=c>c</div>", 500).c_str());
  CHECK(q.w("a") == 160 && q.w("b") == 160 && q.abs_x("c") == 340 && q.h("a") == q.h("b") && q.h("b") > 66);
  // a sidebar layout: fixed side column, content column that can shrink to 0
  Laid r(page("<div id=g style='display:grid;grid-template-columns:200px minmax(0,1fr);gap:16px'><div id=side>s</div><div id=main><div id=wide style='width:2000px;height:10px'></div></div></div>").c_str());
  CHECK(r.w("main") == 800 - 216 && r.abs_x("main") == 216);
  // the item's percentage width is of its area
  Laid s(grid("grid-template-columns:200px 200px", "<div id=a style='width:50%'>a</div>").c_str());
  CHECK(s.w("a") == 100);
  // height: 100% of the area
  Laid t(grid("grid-template-columns:200px;grid-template-rows:80px", "<div id=a style='height:100%;align-self:start'>a</div>").c_str());
  CHECK(t.h("a") == 80);
  // a stretched item passes its height on to a block child with a percentage height
  Laid u(grid("grid-template-columns:200px;grid-template-rows:80px", "<div id=a><div id=c style='height:50%'></div></div>").c_str());
  CHECK(u.h("a") == 80 && u.h("c") == 40);
  // a nested grid fills a row whose height is given
  Laid v(page("<div id=g style='display:grid;height:200px;grid-template-rows:100px 1fr'><div>x</div><div id=h style='display:grid;grid-template-rows:1fr 2fr'><div id=a>a</div><div id=b>b</div></div></div>").c_str());
  CHECK(v.h("h") == 100 && v.h("a") == 33 && v.h("b") == 67);
  // space-between with gaps and an odd count
  Laid w(grid("grid-template-columns:repeat(2,100px);column-gap:10px;justify-content:space-between", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(w.abs_x("a") == 0 && w.abs_x("b") == 700);
}

static void test_inline_grid_and_nesting() {
  // inline-grid is as wide as its columns
  Laid p(page("<div id=g style='display:inline-grid;grid-template-columns:auto auto;gap:10px'><div id=a>aaaa</div><div id=b>bb</div></div>").c_str());
  CHECK(p.w("g") == 32 + 16 + 10 && p.w("a") == 32);
  // a grid in a shrink-to-fit float
  Laid q(page("<div id=f style='float:left'><div id=g style='display:grid;grid-template-columns:auto auto'><div>aaaa</div><div>bb</div></div></div>").c_str());
  CHECK(q.w("f") == 48);
  // a grid inside a grid item
  Laid r(page("<div id=g style='display:grid;grid-template-columns:1fr 1fr'><div id=a><div id=h style='display:grid;grid-template-columns:1fr 1fr'><div id=x>x</div><div id=y>y</div></div></div><div id=b>b</div></div>").c_str());
  CHECK(r.w("a") == 400 && r.w("h") == 400 && r.w("x") == 200 && r.abs_x("y") == 200);
  // text directly in a grid is an anonymous item; inline children are items of their own
  Laid s(page("<div id=g style='display:grid;grid-template-columns:100px 100px'><span id=a>one</span><span id=b>two</span></div>").c_str());
  CHECK(s.ok && s.abs_x("b") == 100 && s.w("a") == 100);
  Laid t(page("<div id=g style='display:grid;grid-template-columns:100px 100px'>text<span id=b>two</span></div>").c_str());
  CHECK(t.ok && t.abs_x("b") == 100);
  // the baseline of the grid is that of its first row: inline-grids sit on it
  Laid u(page("<div id=o>ab<div id=g style='display:inline-grid;padding-top:8px'><div>x</div></div>cd</div>").c_str());
  CHECK(u.ok && u.h("o") >= 24);
}

static void test_positioned_and_replaced() {
  // an absolutely positioned child is out of the grid
  Laid p(page("<div id=g style='display:grid;position:relative;grid-template-columns:100px 100px'><div id=a>a</div><div id=abs style='position:absolute;right:0;bottom:0;width:10px;height:10px'></div><div id=b>b</div></div>").c_str());
  CHECK(p.abs_x("b") == 100 && p.w("abs") == 10 && p.abs_x("abs") == 790 && p.abs_y("abs") == 6);
  // a relatively positioned item is moved
  Laid q(grid("grid-template-columns:100px 100px", "<div id=a style='position:relative;left:5px'>a</div><div id=b>b</div>").c_str());
  CHECK(q.abs_x("a") == 5 && q.abs_x("b") == 100);
  // a picture keeps its size
  Laid r(grid("grid-template-columns:200px 1fr", "<img id=i src=x><div id=b>b</div>").c_str());
  CHECK(r.w("i") == 100 && r.h("i") == 50 && r.abs_x("b") == 200);
  // gaps with percentages and a grid with no items
  Laid s(grid("grid-template-columns:100px 100px;column-gap:10%", "<div id=a>a</div><div id=b>b</div>").c_str());
  CHECK(s.abs_x("b") == 180);
  Laid t(grid("grid-template-columns:100px 100px;grid-template-rows:30px", "").c_str());
  CHECK(t.ok && t.h("g") == 30);
}

static void test_robustness() {
  const char* values[] = {
      "repeat(", "repeat(auto-fill)", "repeat(0, 1fr)", "repeat(99999999, 1fr)", "minmax(", "minmax(1fr, 1fr)", "fit-content(", "[", "]]", "1fr 2fr [a", "span", "span span",
      "\"a a\" \"b\"", "-9999999999px", "calc(", "repeat(auto-fill, auto) repeat(auto-fit, auto)", "1e99fr", "0fr 0fr", "none", "subgrid", "masonry", "repeat(2, repeat(2, 1fr))"};
  for (const char* v : values) {
    std::string css = std::string("grid-template-columns:") + v + ";grid-template-rows:" + v + ";grid-template-areas:" + v + ";grid-auto-columns:" + v + ";grid-auto-rows:" + v;
    Laid p(grid(css.c_str(), "<div id=a style='grid-column:1/1'>a</div><div style='grid-row:span 99999;grid-column:-99999/99999'>b</div><div>c</div>").c_str());
    CHECK(p.ok);
    Laid q(grid("grid-template-columns:repeat(3,1fr)", (std::string("<div style='grid-column:") + v + ";grid-row:" + v + "'>a</div><div>b</div>").c_str()).c_str());
    CHECK(q.ok);
  }
  // bit flips in a template and truncation
  const std::string base = "grid-template-columns:repeat(auto-fill,minmax(100px,1fr)) [x] 2fr;grid-template-areas:\"a b\" \"c d\";grid-auto-flow:dense;gap:5px 10%";
  for (size_t i = 0; i < base.size(); ++i) {
    std::string cut = base.substr(0, i);
    Laid p(grid(cut.c_str(), "<div style='grid-area:a'>a</div><div>b</div><div style='grid-column:span 2'>c</div>").c_str());
    CHECK(p.ok);
    for (int bit = 0; bit < 7; ++bit) {
      std::string flipped = base;
      flipped[i] = static_cast<char>(flipped[i] ^ (1 << bit));
      if (flipped[i] == '\'' || flipped[i] == '<' || flipped[i] == '>' || flipped[i] == '&') continue;
      Laid q(grid(flipped.c_str(), "<div style='grid-area:a'>a</div><div>b</div><div style='grid-column:span 2'>c</div>").c_str());
      CHECK(q.ok);
    }
  }
  // many items
  std::string many;
  for (int i = 0; i < 300; ++i) many += "<div>x</div>";
  Laid m(grid("grid-template-columns:repeat(auto-fill,minmax(40px,1fr));gap:1px", many.c_str()).c_str());
  CHECK(m.ok);
  Laid m2(grid("grid-template-columns:repeat(7,1fr);grid-auto-flow:row dense", many.c_str()).c_str());
  CHECK(m2.ok);
}

int main() {
  test_fr_and_gaps();
  test_rows_and_areas();
  test_placement();
  test_auto_repeat();
  test_spanning_items();
  test_alignment();
  test_content_sizes();
  test_real_world();
  test_inline_grid_and_nesting();
  test_positioned_and_replaced();
  test_robustness();
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("grid tests passed\n");
  return 0;
}
