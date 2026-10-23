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
  FixedWidthMeasurer measurer;
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
    options.measurer = &measurer;
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

static void test_block_stacking_and_sizes() {
  Laid p(page("<div id=a style='height:20px'></div><div id=b style='height:30px'></div>").c_str());
  CHECK(p.ok);
  CHECK(px(p.at("a").width) == 800 && px(p.at("a").height) == 20 && p.abs_y("a") == 0);
  CHECK(px(p.at("b").height) == 30 && p.abs_y("b") == 20);
  CHECK(px(p.tree.box(p.tree.root()).height) == 50);   // the root is as high as the page

  Laid q(page("<div id=a style='width:200px;margin:0 auto;height:10px'></div><div id=b style='width:200px;margin-left:30px;height:10px'></div>"
              "<div id=c style='width:100px;margin-left:auto;height:10px'></div>").c_str());
  CHECK(q.abs_x("a") == 300 && q.abs_x("b") == 30 && q.abs_x("c") == 700);   // centred, left margin, auto margin takes the rest

  // padding, border and box-sizing
  Laid r(page("<div id=a style='width:100px;padding:10px;border:2px solid #000;height:20px'></div>"
              "<div id=b style='box-sizing:border-box;width:100px;padding:10px;border:2px solid #000;height:50px'></div>").c_str());
  CHECK(px(r.at("a").width) == 124 && px(r.at("a").height) == 44);
  CHECK(px(r.at("b").width) == 100 && px(r.at("b").height) == 50 && px(r.at("b").content_width()) == 76);
  CHECK(r.abs_y("b") == 44);

  // min and max width/height; a block's width fills its container minus margins
  Laid s(page("<div id=a style='max-width:300px;height:5px'></div><div id=b style='min-width:900px;height:5px'></div>"
              "<div id=c style='margin:0 20px;height:5px'></div><div id=d style='min-height:40px'></div>").c_str());
  CHECK(px(s.at("a").width) == 300 && px(s.at("b").width) == 900);
  CHECK(px(s.at("c").width) == 760 && s.abs_x("c") == 20);
  CHECK(px(s.at("d").height) == 40);

  // nested blocks sit inside their parent's padding
  Laid t(page("<div id=a style='padding:10px 20px'><div id=b style='height:30px'></div></div>").c_str());
  CHECK(t.abs_x("b") == 20 && t.abs_y("b") == 10 && px(t.at("b").width) == 760 && px(t.at("a").height) == 50);
}

static void test_margin_collapsing() {
  // adjoining sibling margins collapse to the larger
  Laid a(page("<div id=a style='height:10px;margin-bottom:20px'></div><div id=b style='height:10px;margin-top:30px'></div>").c_str());
  CHECK(a.abs_y("b") == 40);   // 10 + max(20, 30)
  // a positive and a negative margin add
  Laid b(page("<div id=a style='height:10px;margin-bottom:20px'></div><div id=b style='height:10px;margin-top:-8px'></div>").c_str());
  CHECK(b.abs_y("b") == 22);   // 10 + (20 - 8)
  // a parent's top margin collapses with its first child's; with padding it does not
  Laid c(page("<div id=p style='margin-top:10px'><div id=k style='margin-top:25px;height:10px'></div></div>").c_str());
  CHECK(c.abs_y("p") == 25 && c.abs_y("k") == 25);
  Laid d(page("<div id=p style='margin-top:10px;padding-top:1px'><div id=k style='margin-top:25px;height:10px'></div></div>").c_str());
  CHECK(d.abs_y("p") == 10 && d.abs_y("k") == 36);
  // an empty block's margins collapse through it
  Laid e(page("<div id=a style='height:10px'></div><div style='margin-top:15px;margin-bottom:40px'></div><div id=b style='height:10px;margin-top:5px'></div>").c_str());
  CHECK(e.abs_y("b") == 50);   // 10 + max(15, 40, 5)
  // the last child's bottom margin collapses with the parent's
  Laid f(page("<div id=p style='margin-bottom:10px'><div style='height:10px;margin-bottom:30px'></div></div><div id=n style='height:5px'></div>").c_str());
  CHECK(px(f.at("p").height) == 10 && f.abs_y("n") == 40);
  // overflow:hidden starts a new formatting context: margins stay inside
  Laid g(page("<div id=p style='overflow:hidden'><div style='margin-top:25px;height:10px'></div></div>").c_str());
  CHECK(px(g.at("p").height) == 35);
}

static void test_inline_layout() {
  // text is 8px per character on 16px lines
  Laid a(page("<div id=a>hello world</div>").c_str());
  const Box& da = a.at("a");
  CHECK(da.lines.count() == 1 && px(da.lines[0].height) == 16 && px(da.height) == 16 && px(da.lines[0].width) == 88);
  CHECK(da.fragments.count() == 1 && da.fragments[0].text_length == 11);

  // a line breaks at a space when the next word does not fit
  Laid b(page("<div id=a style='width:80px'>aaa bbb ccc</div>").c_str());
  CHECK(b.at("a").lines.count() == 2 && px(b.at("a").height) == 32);
  CHECK(px(b.at("a").lines[0].width) == 56 && px(b.at("a").lines[1].width) == 24);   // "aaa bbb" and "ccc": spaces at the end hang

  // a word wider than the line overflows it instead of being cut; later words go to the next line
  Laid c(page("<div id=a style='width:40px'>abcdefghij kl</div>").c_str());
  CHECK(c.at("a").lines.count() == 2 && px(c.at("a").lines[0].width) == 80);

  // white space collapses: runs of spaces and newlines become one space, none at the ends
  Laid d(page("<div id=a>  a   b \n\n c  </div>").c_str());
  CHECK(d.at("a").lines.count() == 1 && px(d.at("a").lines[0].width) == 40);   // "a b c"

  // <br> ends a line, also an empty one
  Laid e(page("<div id=a>one<br>two<br><br>three</div>").c_str());
  CHECK(e.at("a").lines.count() == 4 && px(e.at("a").height) == 64);

  // white-space:pre keeps spaces and newlines and does not wrap
  Laid f(page("<div id=a style='white-space:pre;width:40px'>ab  cd\n  e</div>").c_str());
  CHECK(f.at("a").lines.count() == 2 && px(f.at("a").lines[0].width) == 48 && px(f.at("a").lines[1].width) == 24);

  // nowrap keeps the words on one line
  Laid g(page("<div id=a style='white-space:nowrap;width:40px'>aaa bbb ccc</div>").c_str());
  CHECK(g.at("a").lines.count() == 1);

  // text-align and text-indent
  Laid h(page("<div id=a style='text-align:center;width:200px'>abcd</div><div id=b style='text-align:right;width:200px'>abcd</div>"
              "<div id=c style='text-indent:16px'>ab</div>").c_str());
  CHECK(px(h.at("a").fragments[0].x) == 84 && px(h.at("b").fragments[0].x) == 168 && px(h.at("c").fragments[0].x) == 16);

  // a hyphen is a place to break
  Laid i(page("<div id=a style='width:48px'>ab-cdefg</div>").c_str());
  CHECK(i.at("a").lines.count() == 2 && px(i.at("a").lines[0].width) == 24);

  // text-transform
  Laid j(page("<div id=a style='text-transform:uppercase'>ab</div>").c_str());
  CHECK(j.at("a").children.count() == 1 && std::string(j.tree.box(j.at("a").children[0]).text.c_str()) == "AB");
}

static void test_inline_boxes_and_atomics() {
  // an inline box's padding and border take room in the line; its fragment covers them
  Laid a(page("<div id=a>x<span id=s style='padding:0 8px;border:1px solid #000'>yy</span>z</div>").c_str());
  CHECK(px(a.at("a").lines[0].width) == 8 + 9 + 16 + 9 + 8);   // x, [edge yy edge], z
  bool found = false;
  for (uint64_t i = 0; i < a.at("a").fragments.count(); ++i) {
    const Fragment& f = a.at("a").fragments[i];
    if (f.box == a.box("s")) {
      found = true;
      CHECK(px(f.x) == 8 && px(f.width) == 34 && f.starts_box && f.ends_box);
    }
  }
  CHECK(found);

  // an inline-block is as wide as its content and sits in the line; the line grows to fit it
  Laid b(page("<div id=a>ab<span id=ib style='display:inline-block;height:40px'>cdef</span>gh</div>").c_str());
  CHECK(px(b.at("ib").width) == 32 && px(b.at("ib").height) == 40);
  CHECK(b.at("a").lines.count() == 1 && b.abs_x("ib") == 16);
  CHECK(px(b.at("a").lines[0].width) == 16 + 32 + 16);
  CHECK(px(b.at("a").height) >= 40);

  // the baseline of an inline-block is that of its last line: its text lines up with the text beside it
  Laid c(page("<div id=a>x<span id=ib style='display:inline-block'>y</span></div>").c_str());
  CHECK(c.abs_y("ib") == 0 && px(c.at("a").height) == 16);

  // a picture takes its natural size, or keeps its shape when one side is given; it sits on the baseline
  Laid d(page("<div id=a><img id=i></div><div id=b><img id=j style='width:50px'></div><div id=c><img id=k style='height:10px;width:30px'></div>").c_str());
  CHECK(px(d.at("i").width) == 100 && px(d.at("i").height) == 50);
  CHECK(px(d.at("j").width) == 50 && px(d.at("j").height) == 25);
  CHECK(px(d.at("k").width) == 30 && px(d.at("k").height) == 10);
  CHECK(px(d.at("a").height) >= 50 && px(d.at("a").lines[0].baseline) == 50);   // the bottom of the picture is on the baseline

  // a block-level picture is centred by auto margins
  Laid e(page("<img id=i style='display:block;margin:0 auto'>").c_str());
  CHECK(e.abs_x("i") == 350);

  // two pictures can be split across lines
  Laid f(page("<div id=a style='width:150px'><img style='width:100px'><img style='width:100px'></div>").c_str());
  CHECK(f.at("a").lines.count() == 2);
}

static void test_box_generation() {
  // inline content beside blocks is wrapped in anonymous blocks; white space between blocks makes no box
  Laid a(page("<div id=a>one <p id=p>two</p> three</div>").c_str());
  const Box& da = a.at("a");
  CHECK(da.children.count() == 3);
  CHECK(a.tree.box(da.children[0]).kind == BoxKind::Anonymous && a.tree.box(da.children[1]).kind == BoxKind::Block && a.tree.box(da.children[2]).kind == BoxKind::Anonymous);
  CHECK(!da.inline_content && a.tree.box(da.children[0]).inline_content);
  Laid b(page("<div id=a>\n  <p>x</p>\n  <p>y</p>\n</div>").c_str());
  CHECK(b.at("a").children.count() == 2);
  // display:none and display:contents
  Laid c(page("<div id=a><span style='display:none'>hidden</span><span style='display:contents'><b>shown</b></span></div>").c_str());
  CHECK(c.at("a").lines.count() == 1 && px(c.at("a").lines[0].width) == 40);
  // an inline element with a block inside becomes a block
  Laid d(page("<a id=l href=#><div id=k style='height:10px'></div></a>").c_str());
  CHECK(d.at("l").kind == BoxKind::Block && px(d.at("k").height) == 10);
  // script, style and head make no boxes; hr is a box with a border
  Laid e(page("<div id=a>x<script>1</script></div><hr id=h>").c_str());
  CHECK(e.at("a").children.count() == 1 && px(e.at("h").height) >= 2);
}

static void test_lists() {
  Laid a(page("<ul id=u><li id=i1>one</li><li id=i2>two</li></ul><ol id=o><li id=j1>a</li><li id=j2>b</li></ol>").c_str());
  CHECK(px(a.at("i1").height) == 16 && px(a.at("i2").height) == 16 && a.abs_y("i2") == 16);
  bool marker = false;
  for (uint64_t i = 0; i < a.at("i1").children.count(); ++i) marker = marker || a.tree.box(a.at("i1").children[i]).kind == BoxKind::Marker;
  CHECK(marker);
  // ordered markers count
  for (uint64_t i = 0; i < a.at("j2").children.count(); ++i) {
    const Box& m = a.tree.box(a.at("j2").children[i]);
    if (m.kind == BoxKind::Marker) CHECK(std::string(m.text.c_str()) == "2. ");
  }
}

static void test_intrinsic_sizes() {
  // a float-like shrink-to-fit: an inline-block with a long text is as wide as the text when it fits, and as wide as the room when it does not
  Laid a(page("<div style='width:200px'><span id=s style='display:inline-block'>aaaa bbbb cccc dddd eeee ffff</span></div>").c_str());
  CHECK(px(a.at("s").width) == 200);   // 29 characters = 232px does not fit: as wide as the container
  Laid b(page("<div style='width:300px'><span id=s style='display:inline-block'>aaaa bbbb</span></div>").c_str());
  CHECK(px(b.at("s").width) == 72);
  // the narrowest a box can be is its longest word
  Laid c(page("<div style='width:10px'><span id=s style='display:inline-block'>aaaa bb</span></div>").c_str());
  CHECK(px(c.at("s").width) == 32);
  // blocks inside an inline-block: the widest child decides
  Laid d(page("<div style='width:500px'><span id=s style='display:inline-block'><div>aa</div><div>bbbbbb</div></span></div>").c_str());
  CHECK(px(d.at("s").width) == 48);
}

static void test_floats() {
  // a float takes its size from its content and sits at the left (or right) edge; text flows beside it and under it
  Laid a(page("<div id=c style='width:200px'><div id=f style='float:left;width:50px;height:32px'></div>"
              "<span id=t>aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk</span></div>").c_str());
  CHECK(a.abs_x("f") == 0 && a.abs_y("f") == 0 && px(a.at("f").width) == 50);
  const Box& c = a.at("c");
  // beside the float there are 150px: "aaaa bbbb cccc" (112px) fits, "dddd" (to 152px) does not; the second line is still beside the float (it ends at 32px)
  CHECK(c.lines.count() == 3);
  CHECK(px(c.lines[0].x) == 50 && px(c.lines[0].y) == 0 && px(c.lines[0].width) == 112);
  CHECK(px(c.lines[1].x) == 50 && px(c.lines[1].y) == 16);
  // below the float the lines use the whole width again
  CHECK(px(c.lines[2].x) == 0 && px(c.lines[2].y) == 32);

  Laid b(page("<div id=c style='width:200px'><div id=f style='float:right;width:50px;height:10px'></div>x</div>").c_str());
  CHECK(b.abs_x("f") == 150);

  // two left floats sit side by side; a third that does not fit goes below
  Laid d(page("<div style='width:100px'><div id=a style='float:left;width:40px;height:10px'></div><div id=b style='float:left;width:40px;height:10px'></div>"
              "<div id=c style='float:left;width:40px;height:10px'></div></div>").c_str());
  CHECK(d.abs_x("a") == 0 && d.abs_x("b") == 40 && d.abs_x("c") == 0 && d.abs_y("c") == 10);

  // clear puts a block below the floats; without it the block's text still flows around them
  Laid e(page("<div style='width:200px'><div style='float:left;width:50px;height:40px'></div><div id=n style='clear:left;height:5px'></div></div>").c_str());
  CHECK(e.abs_y("n") == 40);
  Laid f(page("<div style='width:200px'><div style='float:left;width:50px;height:40px'></div><div id=n style='height:5px'></div></div>").c_str());
  CHECK(f.abs_y("n") == 0);   // the block ignores the float; only its text would flow around it

  // a box that starts a formatting context contains its floats; one that does not lets them hang out
  Laid g(page("<div id=a style='overflow:hidden'><div style='float:left;width:10px;height:30px'></div></div><div id=b><div style='float:left;width:10px;height:30px'></div></div>").c_str());
  CHECK(px(g.at("a").height) == 30 && px(g.at("b").height) == 0);

  // a box that starts a formatting context sits beside a float instead of under it
  Laid h(page("<div style='width:200px'><div style='float:left;width:50px;height:40px'></div><div id=n style='overflow:hidden;height:20px'></div></div>").c_str());
  CHECK(h.abs_x("n") == 50 && px(h.at("n").width) == 150);

  // a float inside inline content after some text starts on the next line
  Laid i(page("<div id=c style='width:200px'>ab<span id=f style='float:left;width:30px;height:16px'></span> cd ef gh ij kl</div>").c_str());
  CHECK(i.abs_y("f") == 16);   // the line "ab cd ef gh ij kl" fits on one line: the float goes under it

  // floats are margin boxes: margins push neighbours away
  Laid j(page("<div style='width:200px'><div id=a style='float:left;width:20px;height:10px;margin:5px'></div><div id=b style='float:left;width:20px;height:10px'></div></div>").c_str());
  CHECK(j.abs_x("a") == 5 && j.abs_y("a") == 5 && j.abs_x("b") == 30);
}

static void test_positioning() {
  // a relatively positioned box is shifted after it is placed; its siblings do not notice
  Laid a(page("<div id=a style='position:relative;top:10px;left:5px;height:20px'></div><div id=b style='height:10px'></div>").c_str());
  CHECK(a.abs_x("a") == 5 && a.abs_y("a") == 10 && a.abs_y("b") == 20);
  Laid a2(page("<div id=a style='position:relative;bottom:4px;right:6px;height:20px'></div>").c_str());
  CHECK(a2.abs_x("a") == -6 && a2.abs_y("a") == -4);

  // an absolutely positioned box is placed in the padding box of its positioned ancestor
  Laid b(page("<div id=p style='position:relative;width:300px;height:200px;margin:10px;border:2px solid #000'>"
              "<div id=k style='position:absolute;right:10px;bottom:20px;width:50px;height:30px'></div>"
              "<div id=l style='position:absolute;left:7px;top:9px;width:5px;height:5px'></div></div>").c_str());
  CHECK(b.abs_x("k") == 10 + 2 + 300 - 10 - 50 && b.abs_y("k") == 10 + 2 + 200 - 20 - 30);
  CHECK(b.abs_x("l") == 10 + 2 + 7 && b.abs_y("l") == 10 + 2 + 9);
  CHECK(px(b.at("p").height) == 204);   // it takes no room in the flow

  // with no positioned ancestor the viewport is the containing block; fixed boxes use it too
  Laid c(page("<div style='height:50px'></div><div id=p style='position:relative;height:60px'><div id=k style='position:absolute;top:5px;left:7px;width:1px;height:1px'></div>"
              "<div id=f style='position:fixed;top:0;left:0;width:1px;height:1px'></div></div>").c_str());
  CHECK(c.abs_x("k") == 7 && c.abs_y("k") == 50 + 5 && c.abs_x("f") == 0 && c.abs_y("f") == 0);
  Laid c2(page("<div style='height:50px'></div><div id=k style='position:absolute;top:5px;left:7px;width:1px;height:1px'></div>").c_str());
  CHECK(c2.abs_x("k") == 7 && c2.abs_y("k") == 5);

  // auto offsets keep the place the box would have had in the flow
  Laid d(page("<div style='height:10px'></div><div id=k style='position:absolute;width:20px;height:5px'></div>").c_str());
  CHECK(d.abs_x("k") == 0 && d.abs_y("k") == 10);
  Laid d2(page("<div id=p>ab<span id=k style='position:absolute;width:3px;height:3px'></span></div>").c_str());
  CHECK(d2.abs_x("k") == 16 && d2.abs_y("k") == 0 && d2.at("p").lines.count() == 1 && px(d2.at("p").lines[0].width) == 16);

  // left and right together stretch an auto width; auto margins centre a fixed one
  Laid e(page("<div style='position:relative;width:300px;height:50px'><div id=k style='position:absolute;left:10px;right:20px;height:5px'></div>"
              "<div id=m style='position:absolute;left:0;right:0;width:100px;margin:0 auto;height:5px'></div></div>").c_str());
  CHECK(px(e.at("k").width) == 270 && e.abs_x("k") == 10 && e.abs_x("m") == 100);

  // top and bottom stretch an auto height
  Laid f(page("<div style='position:relative;height:100px'><div id=k style='position:absolute;top:10px;bottom:20px;width:5px'></div></div>").c_str());
  CHECK(px(f.at("k").height) == 70 && f.abs_y("k") == 10);

  // an auto width is shrink-to-fit
  Laid g(page("<div style='position:relative;width:300px'><div id=k style='position:absolute'>abcd</div></div>").c_str());
  CHECK(px(g.at("k").width) == 32 && px(g.at("k").height) == 16);

  // nested: the inner box is placed against the outer one
  Laid h(page("<div id=o style='position:absolute;left:100px;top:100px;width:200px;height:200px'><div id=i style='position:absolute;left:10px;top:20px;width:5px;height:5px'></div></div>").c_str());
  CHECK(h.abs_x("i") == 110 && h.abs_y("i") == 120);

  // a positioned box is not floated even if it says so
  Laid i(page("<div style='width:200px'><div id=k style='position:absolute;float:left;width:10px;height:10px'></div>x</div>").c_str());
  CHECK(i.abs_x("k") == 0 && i.at("k").out_of_flow && !i.at("k").floated);
}

static void test_robustness() {
  // deep nesting and empty pages do not crash; a page with no body still lays out
  std::string deep;
  for (int i = 0; i < 300; ++i) deep += "<div>";
  deep += "x";
  Laid a(page(deep.c_str()).c_str());
  CHECK(a.ok);
  Laid b("");
  CHECK(!b.ok || b.tree.count() >= 1);
  Laid c(page("<div id=a style='width:99999999px;margin-left:99999999px;height:99999999px'>x</div>").c_str());
  CHECK(c.ok && c.at("a").width.raw() > 0);
}

int main() {
  test_block_stacking_and_sizes();
  test_margin_collapsing();
  test_inline_layout();
  test_inline_boxes_and_atomics();
  test_box_generation();
  test_lists();
  test_intrinsic_sizes();
  test_floats();
  test_positioning();
  test_robustness();
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("layout tests passed\n");
  return 0;
}
