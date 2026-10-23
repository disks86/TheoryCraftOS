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

// Host-side tests for the box tree and the layout engine (src/Layout). Text is measured with FixedWidthMeasurer: every character is 8px wide
// at the default 16px font and a line is 16px high, so the expected numbers can be worked out by hand from the CSS 2.1 rules.

// Table layout (CSS 2.1 chapter 17): the automatic and fixed algorithms, spacing, spans, collapsed borders, captions, anonymous boxes, the
// attributes old pages use. FixedWidthMeasurer makes every character 8px wide on 16px lines, so the numbers can be worked out by hand.
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

// A page laid out: `html` is the markup, `width` the viewport's width in pixels.
struct Laid {
  dom::Document doc;
  browser::StyleSheet sheet;
  BoxTree tree;
  FixedWidthMeasurer measurer;
  bool ok = false;

  explicit Laid(const char* html, int width = 800) : doc(g_alloc), sheet(g_alloc), tree(g_alloc) {
    dom::build_document(doc, html, nullptr);
    browser::RenderOptions render;
    render.media.width = width;
    render.media.height = 600;
    browser::collect_style_sheets(doc, render, &sheet);
    sheet.set_viewport(width, 600);
    BuildOptions build;
    LayoutOptions options;
    options.viewport_width = LayoutUnit::from_px(width);
    options.viewport_height = LayoutUnit::from_px(600);
    options.measurer = &measurer;
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
};

static int px(LayoutUnit u) { return u.round(); }

static std::string page(const char* body, const char* css = "") {
  return std::string("<style>html,body,p,div,td,th{margin:0}</style><style>") + css + "</style>" + body;
}

static void test_basic_grid() {
  // columns take the widest cell; rows the tallest
  Laid p(page("<table id=t cellspacing=0 cellpadding=0><tr><td id=a>aaaa</td><td id=b>bb</td></tr><tr><td id=c>c</td><td id=d>dddddd</td></tr></table>").c_str());
  CHECK(p.ok);
  CHECK(px(p.at("t").width) == 80 && px(p.at("t").height) == 32);
  CHECK(px(p.at("a").width) == 32 && px(p.at("b").width) == 48 && px(p.at("c").width) == 32 && px(p.at("d").width) == 48);
  CHECK(p.abs_x("a") == 0 && p.abs_x("b") == 32 && p.abs_y("a") == 0 && p.abs_y("c") == 16 && p.abs_x("d") == 32);
  CHECK(px(p.at("a").height) == 16 && px(p.at("d").height) == 16);

  // the browser's own style: 2px between the cells, 1px padding in them
  Laid q(page("<table id=t><tr><td id=a>aaaa</td><td id=b>bb</td></tr></table>").c_str());
  CHECK(px(q.at("a").width) == 34 && px(q.at("b").width) == 18);
  CHECK(px(q.at("t").width) == 58 && px(q.at("t").height) == 22);
  CHECK(q.abs_x("a") == 2 && q.abs_x("b") == 38 && q.abs_y("a") == 2);

  // cellspacing and cellpadding
  Laid r(page("<table id=t cellspacing=5 cellpadding=3><tr><td id=a>aa</td></tr></table>").c_str());
  CHECK(px(r.at("a").width) == 22 && px(r.at("a").height) == 22 && px(r.at("t").width) == 32 && px(r.at("t").height) == 32);
  CHECK(r.abs_x("a") == 5 && r.abs_y("a") == 5);

  // a table is not full width and has no margin of the old style sheet's
  Laid s(page("<div style='height:10px'></div><table id=t cellspacing=0 cellpadding=0><tr><td>x</td></tr></table>").c_str());
  CHECK(s.abs_y("t") == 10 && px(s.at("t").width) == 8);

  // thead and tfoot are laid out first and last wherever they are written
  Laid u(page("<table cellspacing=0 cellpadding=0><tbody><tr><td id=b>b</td></tr></tbody><tfoot><tr><td id=f>f</td></tr></tfoot><thead><tr><td id=h>h</td></tr></thead></table>").c_str());
  CHECK(u.abs_y("h") == 0 && u.abs_y("b") == 16 && u.abs_y("f") == 32);
}

static void test_widths() {
  // width 100%: the extra goes to the columns in proportion to their widths
  Laid p(page("<table id=t width=100% cellspacing=0 cellpadding=0><tr><td id=a>aa</td><td id=b>bbbb</td></tr></table>").c_str());
  CHECK(px(p.at("t").width) == 800 && px(p.at("a").width) == 267 && px(p.at("b").width) == 533);

  // percentage column
  Laid q(page("<table id=t width=400 cellspacing=0 cellpadding=0><tr><td id=a width=25%>x</td><td id=b>y</td></tr></table>").c_str());
  CHECK(px(q.at("t").width) == 400 && px(q.at("a").width) == 100 && px(q.at("b").width) == 300);

  // a column with a width in pixels keeps it, the one without takes the rest
  Laid r(page("<table id=t width=500 cellspacing=0 cellpadding=0><tr><td id=a width=100>x</td><td id=b>y</td></tr></table>").c_str());
  CHECK(px(r.at("a").width) == 100 && px(r.at("b").width) == 400);

  // padding is added to a width given in pixels (content-box cells)
  Laid r2(page("<table id=t cellspacing=0 cellpadding=5><tr><td id=a width=100>x</td></tr></table>").c_str());
  CHECK(px(r2.at("a").width) == 110 && px(r2.at("t").width) == 110);

  // css widths work like the attributes
  Laid s(page("<table id=t style='width:300px;border-spacing:0'><tr><td id=a style='width:60%;padding:0'>x</td><td id=b style='padding:0'>y</td></tr></table>").c_str());
  CHECK(px(s.at("t").width) == 300 && px(s.at("a").width) == 180 && px(s.at("b").width) == 120);

  // table-layout: fixed ignores the content
  Laid f(page("<table id=t style='table-layout:fixed;width:300px' cellspacing=0 cellpadding=0><tr><td id=a style='width:100px'>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</td><td id=b>b</td></tr></table>").c_str());
  CHECK(px(f.at("a").width) == 100 && px(f.at("b").width) == 200 && px(f.at("t").width) == 300);
  Laid f2(page("<table id=t style='table-layout:fixed;width:300px' cellspacing=0 cellpadding=0><tr><td id=a>a</td><td id=b>b</td><td id=c>c</td></tr></table>").c_str());
  CHECK(px(f2.at("a").width) == 100 && px(f2.at("b").width) == 100 && px(f2.at("c").width) == 100);

  // a table with a narrow width is as wide as its columns need
  Laid n(page("<table id=t width=10 cellspacing=0 cellpadding=0><tr><td id=a>aaaaaaaaaa</td></tr></table>").c_str());
  CHECK(px(n.at("a").width) == 80 && px(n.at("t").width) == 80);   // (one unbreakable word)

  // in a narrow window the text wraps and the table shrinks to what there is
  Laid w(page("<table id=t cellspacing=0 cellpadding=0><tr><td id=a>aaaa bbbb</td></tr></table>").c_str(), 50);
  CHECK(px(w.at("t").width) == 50 && px(w.at("a").height) == 32);
  Laid w2(page("<table id=t cellspacing=0 cellpadding=0><tr><td id=a>aaaa bbbb</td></tr></table>").c_str(), 400);
  CHECK(px(w2.at("t").width) == 72 && px(w2.at("a").height) == 16);

  // box-sizing: a table's width is that of its border box
  Laid bb(page("<table id=t border=2 width=200 cellspacing=0 cellpadding=0><tr><td id=a>x</td></tr></table>").c_str());
  CHECK(px(bb.at("t").width) == 200 && px(bb.at("a").width) == 196);
}

static void test_spans() {
  // colspan: a wide cell makes the columns it covers wider
  Laid p(page("<table id=t cellspacing=0 cellpadding=0><tr><td id=a colspan=2>aaaaaaaaaaaa</td></tr><tr><td id=b>b</td><td id=c>c</td></tr></table>").c_str());
  CHECK(px(p.at("a").width) == 96 && px(p.at("b").width) == 48 && p.abs_x("c") == 48 && px(p.at("t").width) == 96);

  // colspan with spacing in between
  Laid p2(page("<table id=t cellspacing=4 cellpadding=0><tr><td id=a colspan=2>x</td></tr><tr><td id=b>b</td><td id=c>c</td></tr></table>").c_str());
  CHECK(px(p2.at("a").width) == 20 && px(p2.at("t").width) == 28);

  // rowspan
  Laid q(page("<table id=t cellspacing=0 cellpadding=0><tr><td rowspan=2 id=a>a</td><td id=b>b</td></tr><tr><td id=c>c</td></tr></table>").c_str());
  CHECK(px(q.at("a").height) == 32 && p2.ok && q.abs_x("c") == 8 && q.abs_y("c") == 16 && px(q.at("t").width) == 16);

  // a rowspan cell taller than its rows makes them taller
  Laid q2(page("<table id=t cellspacing=0 cellpadding=0><tr><td rowspan=2 id=a>a<br>b<br>c<br>d</td><td id=b>b</td></tr><tr><td id=c>c</td></tr></table>").c_str());
  CHECK(px(q2.at("a").height) == 64 && px(q2.at("t").height) == 64 && q2.abs_y("c") >= 16);

  // cells of a later row skip the columns a rowspan holds
  Laid q3(page("<table cellspacing=0 cellpadding=0><tr><td rowspan=2 id=a>aaaa</td><td id=b>b</td></tr><tr><td id=c>c</td><td id=d>d</td></tr></table>").c_str());
  CHECK(q3.abs_x("c") == 32 && q3.abs_x("d") == 40);

  // short rows
  Laid s(page("<table id=t cellspacing=0 cellpadding=0><tr><td id=a>a</td><td id=b>b</td><td>c</td></tr><tr><td id=d>d</td></tr></table>").c_str());
  CHECK(px(s.at("t").width) == 24 && px(s.at("d").width) == 8);

  // absurd spans do not break anything
  Laid z(page("<table><tr><td colspan=100000 rowspan=70000>a</td><td colspan=0 rowspan=-3>b</td></tr></table>").c_str());
  CHECK(z.ok);
}

static void test_vertical_alignment() {
  // cells are middle aligned by default
  Laid p(page("<table id=t cellspacing=0 cellpadding=0><tr><td id=a>one<br>two<br>three</td><td id=b>x</td></tr></table>").c_str());
  CHECK(px(p.at("b").height) == 48 && px(p.at("a").height) == 48);
  CHECK(px(p.at("b").lines[0].y) == 16 && px(p.at("a").lines[0].y) == 0);
  Laid q(page("<table cellspacing=0 cellpadding=0><tr><td id=a>one<br>two<br>three</td><td id=b valign=top>x</td><td id=c valign=bottom>x</td><td id=d style='vertical-align:top'>x</td></tr></table>").c_str());
  CHECK(px(q.at("b").lines[0].y) == 0 && px(q.at("c").lines[0].y) == 32 && px(q.at("d").lines[0].y) == 0);
  // valign on the row
  Laid r(page("<table cellspacing=0 cellpadding=0><tr valign=top><td id=a>one<br>two</td><td id=b>x</td></tr></table>").c_str());
  CHECK(px(r.at("b").lines[0].y) == 0);
  Laid r2(page("<table cellspacing=0 cellpadding=0><tbody style='vertical-align:bottom'><tr><td id=a>one<br>two</td><td id=b>x</td></tr></tbody></table>").c_str());
  CHECK(px(r2.at("b").lines[0].y) == 16);
  // baseline: the first lines line up
  Laid s(page("<table cellspacing=0 cellpadding=0><tr><td id=a valign=baseline style='font-size:32px'>big</td><td id=b valign=baseline>small</td></tr></table>").c_str());
  CHECK(px(s.at("a").height) == 32);
  CHECK(s.at("a").lines[0].y == LayoutUnit() && px(s.at("b").lines[0].y + s.at("b").lines[0].baseline) == px(s.at("a").lines[0].baseline));
  // padding moves the content in the cell
  Laid t(page("<table cellspacing=0 cellpadding=0><tr><td id=a style='padding:4px 0 10px'>a<br>b</td></tr></table>").c_str());
  CHECK(px(t.at("a").height) == 46 && px(t.at("a").lines[0].y) == 4 && px(t.at("a").content_top()) == 4);
  // a height on the cell or the row, or the table, is a least height
  Laid h(page("<table id=t cellspacing=0 cellpadding=0><tr><td id=a style='height:40px'>a</td></tr><tr style='height:30px'><td id=b>b</td></tr></table>").c_str());
  CHECK(px(h.at("a").height) == 40 && px(h.at("b").height) == 30 && px(h.at("t").height) == 70);
  Laid h2(page("<table id=t height=100 cellspacing=0 cellpadding=0><tr><td id=a>a</td></tr></table>").c_str());
  CHECK(px(h2.at("t").height) == 100 && px(h2.at("a").height) == 100 && px(h2.at("a").lines[0].y) == 42);
  Laid h3(page("<table id=t height=10 cellspacing=0 cellpadding=0><tr><td id=a>a<br>b</td></tr></table>").c_str());
  CHECK(px(h3.at("t").height) == 32);
  Laid h4(page("<table id=t height=100 cellspacing=0 cellpadding=0><tr><td id=a>a</td></tr><tr><td id=b>b</td></tr></table>").c_str());
  CHECK(px(h4.at("a").height) == 50 && px(h4.at("b").height) == 50 && h4.abs_y("b") == 50);
}

static void test_collapsed_borders() {
  const char* css = "table{border-collapse:collapse} td{padding:0;border:2px solid #000}";
  Laid p(page("<table id=t><tr><td id=a>a</td><td id=b>b</td></tr></table>", css).c_str());
  CHECK(px(p.at("t").border.left) == 1 && px(p.at("t").border.top) == 1 && px(p.at("t").border.right) == 1);
  CHECK(px(p.at("a").border.left) == 1 && px(p.at("a").border.right) == 1 && px(p.at("b").border.left) == 1);
  CHECK(px(p.at("a").width) == 10 && px(p.at("b").width) == 10 && px(p.at("t").width) == 22 && px(p.at("t").height) == 20);
  CHECK(p.abs_x("a") == 1 && p.abs_x("b") == 11);

  // the wider border wins at an edge it is shared
  Laid q(page("<table id=t><tr><td id=a style='border:1px solid red'>a</td><td id=b style='border:3px solid blue'>b</td></tr></table>", css).c_str());
  CHECK(px(q.at("a").border.right) == 2 && px(q.at("b").border.left) == 1 && q.at("a").style.border_color[1] == q.at("b").style.border_color[3]);
  CHECK(px(q.at("a").border.left) == 0 || px(q.at("a").border.left) == 1);

  // none loses, hidden wins
  Laid r(page("<table><tr><td id=a style='border:none'>a</td><td id=b style='border:2px solid'>b</td></tr></table>", css).c_str());
  CHECK(px(r.at("a").border.right) == 1 && r.at("a").style.border_style[1] != browser::BorderStyle::None);
  Laid r2(page("<table><tr><td id=a style='border-right:hidden'>a</td><td id=b>b</td></tr></table>", css).c_str());
  CHECK(px(r2.at("a").border.right) == 0 && px(r2.at("b").border.left) == 0);

  // the table's own border takes part: 4px beats the cells' 2px
  Laid s(page("<table id=t style='border:4px solid green'><tr><td id=a>a</td></tr></table>", css).c_str());
  CHECK(px(s.at("t").border.left) == 2 && px(s.at("a").border.left) == 2 && s.at("a").style.border_color[3] == s.at("t").style.border_color[3]);
  CHECK(px(s.at("t").width) == 8 + 2 + 2 + 2 + 2);

  // no spacing, whatever border-spacing says
  Laid u(page("<table id=t cellspacing=10 style='border-collapse:collapse'><tr><td id=a>a</td><td id=b>b</td></tr></table>").c_str());
  CHECK(u.abs_x("b") - u.abs_x("a") == px(u.at("a").width));

  // the usual grid: a 1px line round every cell
  Laid g(page("<table id=t cellpadding=0><tr><td id=a>a</td><td id=b>b</td></tr><tr><td id=c>c</td><td id=d>d</td></tr></table>",
              "table{border-collapse:collapse} td{border:1px solid #888}").c_str());
  CHECK(px(g.at("t").width) == 8 + 8 + 3 && px(g.at("t").height) == 16 + 16 + 3);
  CHECK(g.abs_x("b") == 1 + 9 && g.abs_y("c") == 1 + 17);
}

static void test_captions() {
  Laid p(page("<table id=t cellspacing=0 cellpadding=0><caption id=c>cap</caption><tr><td id=a>aaaaaaaaaa</td></tr></table>").c_str());
  CHECK(p.ok && p.abs_y("c") == 0 && p.abs_y("t") == 16 && px(p.at("t").width) == 80 && px(p.at("c").width) == 80);
  CHECK(px(p.at("c").lines[0].x) > 0);   // (the caption's text is centred over the table)
  const Box& wrapper = p.tree.box(p.at("t").parent);
  CHECK(wrapper.kind == BoxKind::Anonymous && px(wrapper.height) == 32);

  Laid q(page("<table id=t cellspacing=0 cellpadding=0 style='caption-side:bottom'><caption id=c style='caption-side:bottom'>cap</caption><tr><td id=a>x</td></tr></table>").c_str());
  CHECK(q.abs_y("t") == 0 && q.abs_y("c") == 16);

  // the border and background belong to the table, not to the caption
  Laid r(page("<table id=t border=1 cellspacing=0 cellpadding=0><caption id=c>cap</caption><tr><td>x</td></tr></table>").c_str());
  CHECK(px(r.at("t").height) == 20 && r.abs_y("t") == 16 && px(r.at("c").width) == 24);

  // a caption wider than the table wraps (its widest word is as narrow as it gets)
  Laid s(page("<table id=t cellspacing=0 cellpadding=0><caption id=c>a long caption</caption><tr><td>x</td></tr></table>").c_str());
  CHECK(px(s.at("c").width) == 56 && px(s.at("c").height) == 32);

  // margin and centring act on the whole
  Laid u(page("<table id=t align=center width=200 cellspacing=0 cellpadding=0><caption>c</caption><tr><td>x</td></tr></table>").c_str());
  CHECK(u.abs_x("t") == 300 && px(u.at("t").width) == 200);
}

static void test_anonymous_boxes() {
  // css tables built from divs
  Laid p(page("<div style='display:table' id=t><div style='display:table-row'><div style='display:table-cell' id=a>aa</div><div style='display:table-cell' id=b>b</div></div></div>").c_str());
  CHECK(px(p.at("a").width) == 16 && p.abs_x("b") == 16 && px(p.at("t").width) == 24);

  // a cell on its own gets a row and a table
  Laid q(page("<div><div style='display:table-cell' id=a>aa</div><div style='display:table-cell' id=b>bbb</div></div>").c_str());
  CHECK(q.ok && px(q.at("a").width) == 16 && q.abs_x("b") == 16 && px(q.at("b").width) == 24);
  const Box& row = q.tree.box(q.at("a").parent);
  CHECK(row.style.layout == browser::Layout::TableRow && q.tree.box(row.parent).style.layout == browser::Layout::Table);

  // a row on its own, text in a table, a table row in a row
  Laid r(page("<table cellspacing=0 cellpadding=0 id=t>loose<tr><td id=a>x</td></tr></table>").c_str());
  CHECK(r.ok && px(r.at("t").height) >= 32);
  Laid r2(page("<div style='display:table-row'><div style='display:table-cell' id=a>x</div></div>").c_str());
  CHECK(r2.ok && px(r2.at("a").width) == 8);
  Laid r3(page("<table><tr><div id=a>text</div><td id=b>b</td></tr></table>").c_str());
  CHECK(r3.ok && r3.abs_x("b") > r3.abs_x("a"));
  Laid r4(page("<table><tbody><td id=a>x</td><td id=b>y</td></tbody></table>").c_str());
  CHECK(r4.ok && r4.abs_y("a") == r4.abs_y("b") && r4.abs_x("b") > r4.abs_x("a"));
  Laid r5(page("<table cellspacing=0 cellpadding=0><tr><td id=a><div style='display:table-row'><div style='display:table-cell' id=b>bb</div></div></td></tr></table>").c_str());
  CHECK(r5.ok && px(r5.at("b").width) == 16 && px(r5.at("a").width) == 16);

  // display: block on cells (the responsive trick) turns the table into blocks
  Laid b(page("<table><tr><td id=a style='display:block'>a</td><td id=b style='display:block'>b</td></tr></table>").c_str());
  CHECK(b.ok && b.abs_y("b") > b.abs_y("a") - 1);
}

static void test_nested_and_layout_tables() {
  // the layout tables of old pages: 100% wide, no spacing, a narrow column beside a wide one
  Laid p(page("<table id=t width=100% cellpadding=0 cellspacing=0 border=0><tr><td id=a width=150 valign=top>nav</td><td id=b>content here</td></tr></table>").c_str());
  CHECK(px(p.at("a").width) == 150 && px(p.at("b").width) == 650 && p.abs_x("b") == 150);

  // a table in a cell
  Laid q(page("<table id=o width=400 cellpadding=0 cellspacing=0><tr><td id=a><table id=i width=100% cellpadding=0 cellspacing=0><tr><td id=x>one</td><td id=y>two</td></tr></table></td></tr></table>").c_str());
  CHECK(px(q.at("o").width) == 400 && px(q.at("i").width) == 400 && px(q.at("x").width) == 200 && q.abs_x("y") == 200);

  // an inner table with its own size makes the outer cell as big
  Laid r(page("<table id=o cellpadding=0 cellspacing=0><tr><td id=a><table id=i cellpadding=0 cellspacing=0><tr><td>1234567890</td></tr></table></td><td id=b>x</td></tr></table>").c_str());
  CHECK(px(r.at("a").width) == 80 && px(r.at("o").width) == 88 && r.abs_x("b") == 80);

  // Hacker News style: a centred table in a table
  Laid h(page("<center><table id=o width=85% bgcolor=#f6f6ef cellpadding=0 cellspacing=0><tr><td id=a><table width=100% cellpadding=0 cellspacing=0 id=i><tr><td id=x>a</td><td id=y align=right>b</td></tr></table></td></tr></table></center>").c_str());
  CHECK(px(h.at("o").width) == 680 && h.abs_x("o") == 60 && px(h.at("i").width) == 680);

  // an image-and-text row, a form in a cell
  Laid f(page("<table cellpadding=0 cellspacing=0><tr><td id=a><div style='width:100px;height:30px'></div></td><td id=b>text</td></tr></table>").c_str());
  CHECK(px(f.at("a").width) == 100 && px(f.at("a").height) == 30 && px(f.at("b").lines[0].y) == 7);
}

static void test_inline_and_misc() {
  // inline-table sits in the line on its first row's baseline
  Laid p(page("<div id=d>x<table id=t style='display:inline-table' cellspacing=0 cellpadding=0><tr><td>y<br>z</td></tr></table>w</div>").c_str());
  CHECK(p.ok && p.abs_y("t") == 0 && px(p.at("d").height) == 32 && p.abs_x("t") == 8);
  CHECK(p.at("t").has_baseline);

  // empty cells keep their row's height, empty rows have none
  Laid q(page("<table id=t cellspacing=0 cellpadding=0><tr><td id=a></td><td id=b>x</td></tr><tr></tr></table>").c_str());
  CHECK(px(q.at("a").width) == 0 && px(q.at("a").height) == 16 && px(q.at("t").height) == 16);

  // an empty table
  Laid e(page("<table id=t></table><p id=n>after</p>").c_str());
  CHECK(e.ok && px(e.at("t").height) <= 4);

  // the border attribute: outset on the table, inset on the cells
  Laid b(page("<table id=t border=1 cellspacing=0 cellpadding=0><tr><td id=a>x</td></tr></table>").c_str());
  CHECK(px(b.at("t").border.left) == 1 && px(b.at("a").border.left) == 1 && px(b.at("a").width) == 10 && px(b.at("t").width) == 12 && px(b.at("t").height) == 20);
  CHECK(b.at("t").style.border_style[0] == browser::BorderStyle::Outset && b.at("a").style.border_style[0] == browser::BorderStyle::Inset);
  Laid b0(page("<table id=t border=0 cellspacing=0 cellpadding=0><tr><td id=a>x</td></tr></table>").c_str());
  CHECK(px(b0.at("t").border.left) == 0 && px(b0.at("a").width) == 8);

  // align=right floats the table
  Laid f(page("<table id=t align=right width=100 cellspacing=0 cellpadding=0><tr><td>x</td></tr></table><div id=d>text</div>").c_str());
  CHECK(f.abs_x("t") == 700 && f.abs_x("d") == 0);

  // nowrap keeps the text on one line
  Laid n(page("<table id=t cellspacing=0 cellpadding=0 width=40><tr><td id=a nowrap>aa bb cc</td></tr></table>").c_str());
  CHECK(px(n.at("a").height) == 16 && px(n.at("a").width) == 64);

  // padding set by css wins over cellpadding; padding:0 is kept
  Laid c(page("<table cellpadding=9 cellspacing=0><tr><td id=a style='padding:0'>x</td><td id=b>y</td></tr></table>").c_str());
  CHECK(px(c.at("a").width) == 8 && px(c.at("b").width) == 26);
  Laid c2(page("<table cellspacing=0><tr><td id=a>x</td></tr></table>", "td{padding:0}").c_str());
  CHECK(px(c2.at("a").width) == 8);

  // th is bold and centred, td margins do not apply
  Laid th(page("<table cellspacing=0 cellpadding=0 width=100><tr><th id=a>a</th></tr></table>").c_str());
  CHECK(th.at("a").style.bold && th.at("a").lines.count() == 1 && px(th.at("a").lines[0].x) > 0);
  Laid m(page("<table cellspacing=0 cellpadding=0><tr><td id=a style='margin:20px'>x</td></tr></table>").c_str());
  CHECK(px(m.at("a").width) == 8 && m.abs_x("a") == 0);

  // a block-level table in a flow: following content comes below it
  Laid fl(page("<table id=t cellspacing=0 cellpadding=0><tr><td>x</td></tr></table><div id=d>after</div>").c_str());
  CHECK(fl.abs_y("d") == 16);

  // a table is a block formatting context: it does not overlap a float
  Laid fb(page("<div style='float:left;width:100px;height:50px'></div><table id=t cellspacing=0 cellpadding=0><tr><td>x</td></tr></table>").c_str());
  CHECK(fb.ok && fb.abs_x("t") >= 100);

  // position: relative and display: none on parts
  Laid hid(page("<table id=t cellspacing=0 cellpadding=0><tr><td id=a>a</td><td id=b style='display:none'>b</td></tr></table>").c_str());
  CHECK(px(hid.at("t").width) == 8);
}

// Truncated and corrupted markup lays out without a sanitizer report.
static void test_robustness() {
  static const char kDoc[] =
      "<table border=1 cellpadding=2 width=90%><caption>c</caption><thead><tr><th>h1<th colspan=2>h2</thead><tbody><tr><td rowspan=2>a<td>b<td>c<tr><td colspan=3>d"
      "<td><table><tr><td>n</table></tbody><tfoot><tr><td>f</tfoot></table><div style='display:table-cell'>x</div><span style='display:table-row'>y</span>"
      "<table style='border-collapse:collapse;table-layout:fixed;width:100px'><tr><td style='border:3px dashed'>1<td style='border:hidden'>2</table>";
  const uint64_t n = sizeof(kDoc) - 1;
  for (uint64_t cut = 0; cut <= n; cut += 3) {
    std::string html = page(std::string(kDoc, cut).c_str());
    Laid l(html.c_str(), 300);
    CHECK(l.ok);
  }
  uint32_t seed = 12345;
  for (int round = 0; round < 60; ++round) {
    std::string doc(kDoc, n);
    for (int k = 0; k < 3; ++k) {
      seed = seed * 1664525u + 1013904223u;
      doc[(seed >> 8) % doc.size()] ^= static_cast<char>(1u << ((seed >> 4) % 7));
    }
    std::string html = page(doc.c_str());
    Laid l(html.c_str(), 200 + round * 7);
    CHECK(l.ok);
  }
  // a wide table of many cells
  std::string big = "<table>";
  for (int r = 0; r < 40; ++r) {
    big += "<tr>";
    for (int c = 0; c < 30; ++c) big += "<td>x</td>";
    big += "</tr>";
  }
  big += "</table>";
  Laid l(page(big.c_str()).c_str());
  CHECK(l.ok);
}

int main() {
  test_basic_grid();
  test_widths();
  test_spans();
  test_vertical_alignment();
  test_collapsed_borders();
  test_captions();
  test_anonymous_boxes();
  test_nested_and_layout_tables();
  test_inline_and_misc();
  test_robustness();
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("table tests passed\n");
  return 0;
}
