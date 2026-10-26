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

// Host-side layout conformance tests derived from web-platform-tests css/CSS2 (normal flow, margin collapsing, floats, positioning), css-flexbox,
// css-grid, css-tables, css-transforms and css-sizing: each case is a style sheet and some markup, and the border box of one element (x, y, width, height
// relative to the container #h, which is 300px wide) is compared. Text is measured with FixedWidthMeasurer (8px per character, 16px lines).
// A case in kKnownGaps is a gap in the layout engine that must still fail; when it starts to pass the test fails so the entry gets removed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "Browser/Renderer.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Layout/BoxBuilder.hpp"
#include "Layout/FontMeasurer.hpp"
#include "Layout/Layout.hpp"

using namespace layout;

static int g_failed = 0;

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

struct Laid {
  dom::Document doc;
  browser::StyleSheet sheet;
  BoxTree tree;
  FixedWidthMeasurer measurer;
  FakeObjects objects;
  bool ok = false;

  explicit Laid(const char* html) : doc(g_alloc), sheet(g_alloc), tree(g_alloc) {
    const int width = 800;
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
  // Absolute position of a box's border box: the offsets of the boxes that contain it added up.
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

struct Case {
  const char* name;
  const char* css;
  const char* body;
  const char* id;
  const char* expected;   // "x,y,width,height" of the element, relative to #h
};

static const Case kCases[] = {
    {"block fills width", "#h{width:300px;margin:0;padding:0} #h div{height:10px}", "<div id=t style=\"height:10px\"></div>", "t", "0,0,300,10"},
    {"margin offsets", "#t{margin:5px 7px;height:10px;width:20px}", "<div id=t></div>", "t", "7,5,20,10"},
    {"padding grows box", "#t{padding:3px;width:10px;height:10px}", "<div id=t></div>", "t", "0,0,16,16"},
    {"border-box sizing", "#t{box-sizing:border-box;padding:3px;border:2px solid;width:20px;height:20px}", "<div id=t></div>", "t", "0,0,20,20"},
    {"border grows box", "#t{border:2px solid;width:10px;height:10px}", "<div id=t></div>", "t", "0,0,14,14"},
    {"auto margins centre", "#h{width:100px} #t{width:40px;height:10px;margin:0 auto}", "<div id=t></div>", "t", "30,0,40,10"},
    {"margin collapse siblings", "#a{height:10px;margin-bottom:20px} #b{height:10px;margin-top:30px}", "<div id=a></div><div id=b></div>", "b", "0,40,300,10"},
    {"margin collapse parent child", "#p{margin-top:0} #c{margin-top:20px;height:10px}", "<div id=p><div id=c></div></div>", "c", "0,20,300,10"},
    {"no margin collapse with padding", "#p{padding-top:1px} #c{margin-top:20px;height:10px}", "<div id=p><div id=c></div></div>", "c", "0,21,300,10"},
    {"no margin collapse with flow-root", "#p{display:flow-root} #c{margin-top:20px;height:10px}", "<div id=p><div id=c></div></div>", "c", "0,20,300,10"},
    {"negative margins collapse", "#a{height:10px;margin-bottom:-5px} #b{height:10px}", "<div id=a></div><div id=b></div>", "b", "0,5,300,10"},
    {"percent width", "#h{width:200px} #t{width:50%;height:10px}", "<div id=t></div>", "t", "0,0,100,10"},
    {"min-width", "#t{width:10px;min-width:50px;height:10px}", "<div id=t></div>", "t", "0,0,50,10"},
    {"max-width", "#t{width:500px;max-width:50px;height:10px}", "<div id=t></div>", "t", "0,0,50,10"},
    {"min-height wins over max", "#t{height:10px;min-height:20px;max-height:5px}", "<div id=t></div>", "t", "0,0,300,20"},
    {"float left", "#t{float:left;width:20px;height:10px}", "<div id=t></div>", "t", "0,0,20,10"},
    {"float right", "#t{float:right;width:20px;height:10px}", "<div id=t></div>", "t", "280,0,20,10"},
    {"floats stack", "#a{float:left;width:20px;height:10px} #t{float:left;width:20px;height:10px}", "<div id=a></div><div id=t></div>", "t", "20,0,20,10"},
    {"float wraps when no room", "#a{float:left;width:200px;height:10px} #t{float:left;width:200px;height:10px}", "<div id=a></div><div id=t></div>", "t", "0,10,200,10"},
    {"clear", "#a{float:left;width:20px;height:30px} #t{clear:left;height:10px}", "<div id=a></div><div id=t></div>", "t", "0,30,300,10"},
    {"float does not grow parent", "#p{} #a{float:left;width:20px;height:30px}", "<div id=p><div id=a></div></div>", "p", "0,0,300,0"},
    {"flow-root contains float", "#p{display:flow-root} #a{float:left;width:20px;height:30px}", "<div id=p><div id=a></div></div>", "p", "0,0,300,30"},
    {"relative position offsets", "#t{position:relative;left:5px;top:6px;width:10px;height:10px}", "<div id=t></div>", "t", "5,6,10,10"},
    {"absolute position", "#h{position:relative;height:100px} #t{position:absolute;left:10px;top:20px;width:30px;height:40px}", "<div id=t></div>", "t", "10,20,30,40"},
    {"absolute right bottom", "#h{position:relative;height:100px} #t{position:absolute;right:10px;bottom:20px;width:30px;height:40px}", "<div id=t></div>", "t", "260,40,30,40"},
    {"absolute stretches", "#h{position:relative;height:100px} #t{position:absolute;left:10px;right:20px;top:5px;bottom:5px}", "<div id=t></div>", "t", "10,5,270,90"},
    {"absolute auto width shrink", "#h{position:relative} #t{position:absolute;left:0;top:0;height:10px}", "<div id=t style=\"width:40px\"></div>", "t", "0,0,40,10"},
    {"fixed position", "#t{position:fixed;left:3px;top:4px;width:10px;height:10px}", "<div id=t></div>", "t", "3,4,10,10"},
    {"inline-block width", "#t{display:inline-block;width:30px;height:10px}", "<div id=t></div>", "t", "0,3,30,10"},
    {"inline-block baseline", "#t{display:inline-block;width:30px;height:10px}", "<span>x</span><div id=t></div>", "t", "8,3,30,10"},
    {"display none takes no space", "#a{display:none} #t{height:10px}", "<div id=a></div><div id=t></div>", "t", "0,0,300,10"},
    {"visibility hidden keeps space", "#a{visibility:hidden;height:10px} #t{height:10px}", "<div id=a></div><div id=t></div>", "t", "0,10,300,10"},
    {"display contents has no box", "#a{display:contents} #t{height:10px}", "<div id=a><div id=t></div></div>", "t", "0,0,300,10"},
    {"overflow hidden clips height auto", "#t{overflow:hidden;height:20px}", "<div id=t><div style=\"height:100px\"></div></div>", "t", "0,0,300,20"},
    {"height auto grows with content", "", "<div id=t><div style=\"height:10px\"></div><div style=\"height:15px\"></div></div>", "t", "0,0,300,25"},
    {"line box height", "#t{font-size:16px;line-height:20px}", "<div id=t>x</div>", "t", "0,0,300,20"},
    {"two lines wrap", "#t{font-size:16px;line-height:20px;width:20px}", "<div id=t>aa aa</div>", "t", "0,0,20,40"},
    {"nowrap stays one line", "#t{font-size:16px;line-height:20px;width:20px;white-space:nowrap}", "<div id=t>aa aa</div>", "t", "0,0,20,20"},
    {"br makes a new line", "#t{font-size:16px;line-height:20px}", "<div id=t>a<br>b</div>", "t", "0,0,300,40"},
    {"pre keeps newlines", "#t{font-size:16px;line-height:20px;margin:0}", "<pre id=t>a\nb\nc</pre>", "t", "0,0,300,60"},
    {"text-align center inline-block", "#t{display:inline-block;width:20px;height:10px} #h{text-align:center}", "<div id=t></div>", "t", "140,3,20,10"},
    {"text-align right inline-block", "#t{display:inline-block;width:20px;height:10px} #h{text-align:right}", "<div id=t></div>", "t", "280,3,20,10"},
    {"vertical-align middle", "#t{display:inline-block;width:10px;height:10px;vertical-align:middle}", "<div id=t></div>", "t", "0,4,10,10"},
    {"flex row", "#h{display:flex} #t{width:40px;height:10px}", "<div id=t></div>", "t", "0,0,40,10"},
    {"flex grow", "#h{display:flex} #t{flex:1;height:10px}", "<div id=t></div>", "t", "0,0,300,10"},
    {"flex grow share", "#h{display:flex} #a{flex:1;height:10px} #t{flex:2;height:10px}", "<div id=a></div><div id=t></div>", "t", "100,0,200,10"},
    {"flex shrink", "#h{display:flex;width:100px} #a{width:100px;height:10px} #t{width:100px;height:10px}", "<div id=a></div><div id=t></div>", "t", "50,0,50,10"},
    {"flex wrap", "#h{display:flex;flex-wrap:wrap;width:100px} #a{width:60px;height:10px} #t{width:60px;height:10px}", "<div id=a></div><div id=t></div>", "t", "0,10,60,10"},
    {"flex column", "#h{display:flex;flex-direction:column} #a{height:10px} #t{height:15px}", "<div id=a></div><div id=t></div>", "t", "0,10,300,15"},
    {"flex justify center", "#h{display:flex;justify-content:center} #t{width:40px;height:10px}", "<div id=t></div>", "t", "130,0,40,10"},
    {"flex justify space-between", "#h{display:flex;justify-content:space-between} #a{width:40px;height:10px} #t{width:40px;height:10px}", "<div id=a></div><div id=t></div>", "t", "260,0,40,10"},
    {"flex align-items center", "#h{display:flex;align-items:center;height:100px} #t{width:40px;height:10px}", "<div id=t></div>", "t", "0,45,40,10"},
    {"flex align-items stretch", "#h{display:flex;height:100px} #t{width:40px}", "<div id=t></div>", "t", "0,0,40,100"},
    {"flex order", "#h{display:flex} #a{width:10px;height:10px} #t{width:10px;height:10px;order:-1}", "<div id=a></div><div id=t></div>", "t", "0,0,10,10"},
    {"flex gap", "#h{display:flex;gap:7px} #a{width:10px;height:10px} #t{width:10px;height:10px}", "<div id=a></div><div id=t></div>", "t", "17,0,10,10"},
    {"flex row-reverse", "#h{display:flex;flex-direction:row-reverse} #t{width:40px;height:10px}", "<div id=t></div>", "t", "260,0,40,10"},
    {"flex margin auto", "#h{display:flex} #t{width:40px;height:10px;margin-left:auto}", "<div id=t></div>", "t", "260,0,40,10"},
    {"flex basis", "#h{display:flex} #t{flex:0 0 70px;height:10px}", "<div id=t></div>", "t", "0,0,70,10"},
    {"flex min-width auto prevents shrink below content", "#h{display:flex;width:50px} #t{height:10px}", "<div id=t><div style=\"width:80px;height:10px\"></div></div>", "t", "0,0,80,10"},
    {"inline-flex", "#t{display:inline-flex;width:40px;height:10px}", "<div id=t></div>", "t", "0,3,40,10"},
    {"grid fixed tracks", "#h{display:grid;grid-template-columns:100px 100px} #a{height:10px} #t{height:10px}", "<div id=a></div><div id=t></div>", "t", "100,0,100,10"},
    {"grid fr tracks", "#h{display:grid;grid-template-columns:1fr 2fr} #a{height:10px} #t{height:10px}", "<div id=a></div><div id=t></div>", "t", "100,0,200,10"},
    {"grid gap", "#h{display:grid;grid-template-columns:100px 100px;gap:10px 20px} #a{height:10px} #b{height:10px} #t{height:10px}", "<div id=a></div><div id=b></div><div id=t></div>", "t", "0,20,100,10"},
    {"grid column span", "#h{display:grid;grid-template-columns:100px 100px} #t{grid-column:1 / span 2;height:10px}", "<div id=t></div>", "t", "0,0,200,10"},
    {"grid placement", "#h{display:grid;grid-template-columns:100px 100px;grid-template-rows:20px 30px} #t{grid-row:2;grid-column:2}", "<div id=t></div>", "t", "100,20,100,30"},
    {"grid areas", "#h{display:grid;grid-template-columns:50px 50px;grid-template-areas:\"a b\" \"c d\"} #t{grid-area:d;height:10px}", "<div id=t></div>", "t", "50,0,50,10"},
    {"grid auto rows", "#h{display:grid;grid-auto-rows:25px;grid-template-columns:100px} #a{} #t{}", "<div id=a></div><div id=t></div>", "t", "0,25,100,25"},
    {"grid repeat auto-fill", "#h{display:grid;grid-template-columns:repeat(auto-fill, 100px)} #a{height:10px} #b{height:10px} #c{height:10px} #t{height:10px}", "<div id=a></div><div id=b></div><div id=c></div><div id=t></div>", "t", "0,10,100,10"},
    {"grid minmax", "#h{display:grid;grid-template-columns:minmax(50px, 1fr) 100px} #t{height:10px}", "<div id=t></div>", "t", "0,0,200,10"},
    {"grid justify-self", "#h{display:grid;grid-template-columns:100px} #t{width:20px;height:10px;justify-self:end}", "<div id=t></div>", "t", "80,0,20,10"},
    {"grid align-items center", "#h{display:grid;grid-template-columns:100px;grid-template-rows:50px;align-items:center} #t{height:10px}", "<div id=t></div>", "t", "0,20,100,10"},
    {"grid dense", "#h{display:grid;grid-template-columns:50px 50px;grid-auto-flow:dense} #a{grid-column:span 2;height:10px} #b{height:10px;grid-column:span 2} #t{height:10px}", "<div id=a></div><div id=b></div><div id=t></div>", "t", "0,20,50,10"},
    {"table cell widths", "#h{} table{border-collapse:collapse;width:200px} td{padding:0}", "<table><tr><td id=t style=\"height:10px\">a</td><td>b</td></tr></table>", "t", "0,0,100,16"},
    {"table border spacing", "table{border-spacing:5px} td{padding:0;height:10px;width:10px}", "<table><tr><td id=t></td></tr></table>", "t", "5,5,10,10"},
    {"table auto layout uses content", "table{border-spacing:0} td{padding:0}", "<table><tr><td id=t><div style=\"width:50px;height:10px\"></div></td></tr></table>", "t", "0,0,50,10"},
    {"table colspan", "table{border-spacing:0;width:100px} td{padding:0;height:10px}", "<table><tr><td>a</td><td>b</td></tr><tr><td id=t colspan=2></td></tr></table>", "t", "0,16,100,10"},
    {"table caption", "table{border-spacing:0} caption{height:20px} td{padding:0;height:10px;width:10px}", "<table><caption></caption><tr><td id=t></td></tr></table>", "t", "0,20,10,10"},
    {"transform does not affect layout", "#a{transform:translate(50px,50px);height:10px} #t{height:10px}", "<div id=a></div><div id=t></div>", "t", "0,10,300,10"},
    {"multicol", "#t{column-count:2;column-gap:0;height:100px}", "<div id=t><div style=\"height:10px\"></div></div>", "t", "0,0,300,100"},
    {"sticky stays in flow", "#t{position:sticky;top:0;height:10px}", "<div id=t></div>", "t", "0,0,300,10"},
    {"aspect-ratio box", "#t{width:160px;aspect-ratio:16/9}", "<div id=t></div>", "t", "0,0,160,90"},
    {"img attributes aspect", "", "<img id=t width=40 height=20>", "t", "0,0,40,20"},
    {"empty inline has no height", "", "<div id=t><span></span></div>", "t", "0,0,300,0"},
    {"list item marker outside", "#t{height:10px}", "<ul style=\"margin:0;padding:0\"><li id=t></li></ul>", "t", "0,0,300,10"},
    {"calc width", "#t{width:calc(100% - 50px);height:10px}", "<div id=t></div>", "t", "0,0,250,10"},
    {"fit-content", "#t{width:fit-content;height:10px}", "<div id=t><div style=\"width:60px\"></div></div>", "t", "0,0,60,10"},
};

// Cases the layout engine gets wrong today.
static const char* const kKnownGaps[] = {
  "flow-root contains float",   // (display: flow-root is laid out as a plain block, which does not contain its floats)
  "aspect-ratio box",           // (aspect-ratio does not size the box)
};

static bool is_known_gap(const char* name) {
  for (const char* gap : kKnownGaps)
    if (strcmp(gap, name) == 0) return true;
  return false;
}

static std::string measure(const Case& c) {
  const std::string html = std::string("<style>html,body{margin:0;padding:0} #h{margin:0;padding:0;width:300px;overflow:hidden} #h *{margin:0;padding:0;box-sizing:content-box}") + c.css +
                           "</style><div id=h>" + c.body + "</div>";
  Laid laid(html.c_str());
  if (!laid.ok) return "layout failed";
  const BoxId b = laid.box(c.id);
  const BoxId h = laid.box("h");
  if (b == kNoBox || h == kNoBox) return "no box";
  const Box& bx = laid.tree.box(b);
  char text[96];
  snprintf(text, sizeof(text), "%d,%d,%d,%d", laid.abs(b, true) - laid.abs(h, true), laid.abs(b, false) - laid.abs(h, false), bx.width.round(), bx.height.round());
  return text;
}

int main() {
  for (const Case& c : kCases) {
    const std::string got = measure(c);
    const bool pass = got == c.expected;
    const bool gap = is_known_gap(c.name);
    if (!pass && !gap) {
      printf("FAIL %s: expected %s got %s\n", c.name, c.expected, got.c_str());
      ++g_failed;
    }
    if (pass && gap) {
      printf("FIXED a known gap (remove it from kKnownGaps): %s\n", c.name);
      ++g_failed;
    }
  }
  if (g_alloc.live != 0) {
    printf("leak: %d allocations\n", g_alloc.live);
    ++g_failed;
  }
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("layout conformance tests passed\n");
  return 0;
}
